/**
 * @file    foc_board_calibrate.c
 * @brief   Board1 on-MCU calibration manager: CAL-1 ADC zero and CAL-2 rotor mapping.
 * @version v0.1.0
 * @date    2026.11.22
 *
 * Implementation role
 * -------------------
 * CAL-1: with the output inhibited and the shaft still, H0 feeds every valid ADC
 * frame to the current-zero estimator until enough samples are collected.
 *
 * CAL-2: SET_MODE CALIBRATION, ARM, START. A d-axis voltage at electrical angle 0
 * ramps until the decoded current reaches the alignment current, then holds. The
 * angle then sweeps forward and back by a whole number of electrical turns at a
 * slow rate; at each sample point the commanded angle and the encoder position
 * (in the raw source frame) are recorded. Forward and backward lag cancel in the
 * fit. The rotor estimator finds direction and pole pairs, the offset estimator
 * the electrical offset, and the existing admission builds the snapshot.
 *
 * Apply: with the drive stopped and DISABLED, producers are held, the instance is
 * re-initialized from the snapshot, and the previous controller tuning re-applied.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_board_calibrate.h"
#include "foc_board_boot.h"

#include <math.h>

/* Private Macros --------------------------------------------------------------- */

/* Estimator success code (0), private to foc_calibration.c. */
#define CAL_OK                   0
#define CAL_TWO_PI               6.2831853f
#define CAL_ADC_SAMPLES          256U
#define CAL_ADC_TIMEOUT_US       500000U
#define CAL_STILL_RAD_S          0.5f
#define CAL_LIFECYCLE_TIMEOUT_US 20000U
#define CAL_LIFECYCLE_ATTEMPTS   50U
/* H0 rejects ARM/START until fresh sensing follows a STOP: space the retries. */
#define CAL_RETRY_US             2000U
#define CAL_ALIGN_CURRENT_A      0.5f
#define CAL_MIN_CURRENT_A        0.1f
#define CAL_RAMP_V_PER_S         2.0f
#define CAL_SETTLE_US            300000U
#define CAL_SWEEP_TURNS          2.0f
#define CAL_SWEEP_RAD_PER_S      CAL_TWO_PI
#define CAL_SAMPLES_PER_TURN     12U
#define CAL_SENSOR_AGE_US        2000U
#define CAL_STOP_TIMEOUT_US      20000U

/* Private Helpers -------------------------------------------------------------- */

static struct foc_calibration_identity cal_identity(uint16_t pole_pairs, int32_t direction)
{
    struct foc_calibration_identity identity;
    foc_board_calibration_identity(pole_pairs, &identity);
    if (direction != 0) {
        identity.encoder_direction = direction;
    }
    return identity;
}

static void
cal_enter(struct foc_board_calibration *cal, enum foc_board_cal_state state, uint32_t now_us)
{
    cal->state = state;
    cal->state_us = now_us;
    cal->attempts = 0U;
}

static void
cal_fail(struct foc_board_calibration *cal, enum foc_board_cal_failure failure, uint32_t now_us)
{
    atomic_store(&cal->adc_active, 0U);
    if (cal->state >= FOC_BOARD_CAL_DRIVE_MODE && cal->state <= FOC_BOARD_CAL_SWEEP) {
        foc_runtime_revoke_command(cal->runtime);
    }
    cal->failed_state = cal->state;
    cal->failure = failure;
    cal_enter(cal, FOC_BOARD_CAL_FAILED, now_us);
}

static bool cal_quiet(const struct foc_runtime_snapshot *view)
{
    return view->state == FOC_SAFE_STATE_DISABLED && !view->inhibit_pending &&
        view->output_confirmed && view->action == FOC_DEVICE_OUTPUT_INHIBIT;
}

/* H0: one valid ADC frame while CAL-1 collects. Ranks are VBUS, A, B, C. */
static void
cal_adc_tap(void *argument, const uint16_t *ranks, uint32_t generation, uint32_t sample_us)
{
    struct foc_board_calibration *cal = argument;
    if (atomic_load_explicit(&cal->adc_active, memory_order_acquire) == 0U) {
        return;
    }
    const struct foc_calibration_adc_sample sample = {
        .phase_a = ranks[1],
        .phase_b = ranks[2],
        .phase_c = ranks[3],
        .valid_mask = 7U,
        .sequence = generation,
        .captured_us = sample_us,
        /* Foreground confirmed DISABLED, INHIBIT acknowledged and a still shaft before
         * enabling collection, and keeps the Runtime until collection ends. */
        .zero_current_confirmed = true,
    };
    if (foc_calibration_adc_collect(&cal->adc_estimator, &sample) == CAL_OK) {
        atomic_fetch_add_explicit(&cal->adc_samples, 1U, memory_order_release);
    }
}

/* Publish ARM, START or SET_MODE and report its receipt: 0 pending, 1 applied, -1 not applied. */
static int cal_lifecycle(
    struct foc_board_calibration *cal,
    enum foc_runtime_action action,
    enum foc_runtime_mode mode,
    uint32_t now_us
)
{
    struct foc_runtime_lifecycle_result result;
    if (cal->lifecycle_sequence != 0U &&
        foc_runtime_read_lifecycle_result(cal->runtime, &result) == FOC_RUNTIME_OK &&
        result.request_sequence == cal->lifecycle_sequence) {
        if (result.result == FOC_RUNTIME_REQUEST_PENDING) {
            return 0;
        }
        const bool applied = result.result == FOC_RUNTIME_REQUEST_APPLIED;
        cal->lifecycle_sequence = 0U;
        return applied ? 1 : -1;
    }
    struct foc_runtime_snapshot view;
    if (foc_runtime_read_snapshot(cal->runtime, &view) != FOC_RUNTIME_OK) {
        return 0;
    }
    const struct foc_runtime_lifecycle_request request = {
        .action = action,
        .requested_mode = mode,
        .request_sequence = cal->serial->lifecycle_sequence + 1U,
        .source_session = view.motion_session,
        .produced_us = now_us,
    };
    if (foc_runtime_publish_lifecycle_request(cal->runtime, &request, now_us) == FOC_RUNTIME_OK) {
        cal->serial->lifecycle_sequence = request.request_sequence;
        cal->lifecycle_sequence = request.request_sequence;
    }
    return 0;
}

static void cal_drive(struct foc_board_calibration *cal, uint32_t now_us)
{
    const struct foc_runtime_command command = {
        .mode = FOC_RUNTIME_MODE_CALIBRATION,
        .seq = ++cal->command_sequence,
        .produced_us = now_us,
        .target
            .calibration = {.voltage_v = {cal->voltage_v, 0.0f}, .theta_elec_rad = cal->theta_rad},
    };
    (void)foc_runtime_publish_calibration_command(cal->runtime, &command, now_us);
}

/* Lifecycle step with retries: H0 rejects ARM/START until fresh sensing arrives. */
static void cal_step_lifecycle(
    struct foc_board_calibration *cal,
    enum foc_runtime_action action,
    enum foc_runtime_mode mode,
    enum foc_board_cal_state next,
    uint32_t now_us
)
{
    if (cal->lifecycle_sequence == 0U && now_us - cal->retry_us < CAL_RETRY_US &&
        cal->attempts != 0U) {
        return;
    }
    const int receipt = cal_lifecycle(cal, action, mode, now_us);
    if (receipt > 0) {
        cal_enter(cal, next, now_us);
    } else if (receipt < 0) {
        cal->retry_us = now_us;
        if (++cal->attempts >= CAL_LIFECYCLE_ATTEMPTS) {
            cal_fail(cal, FOC_BOARD_CAL_FAILURE_LIFECYCLE, now_us);
        }
    } else if (now_us - cal->state_us > CAL_LIFECYCLE_TIMEOUT_US * CAL_LIFECYCLE_ATTEMPTS) {
        cal_fail(cal, FOC_BOARD_CAL_FAILURE_TIMEOUT, now_us);
    }
}

static void cal_sample(
    struct foc_board_calibration *cal,
    const struct foc_runtime_snapshot *view,
    uint32_t now_us
)
{
    struct foc_encoder_config policy;
    foc_board_encoder_policy(cal->encoder, &policy);
    if (!view->sensor_valid || now_us - view->sensor_us > CAL_SENSOR_AGE_US) {
        cal_fail(cal, FOC_BOARD_CAL_FAILURE_SENSOR, now_us);
        return;
    }
    /* The estimators work in the source sensor frame (raw code decoded with Board1's
     * source direction): admission maps it to the Runtime as source x fitted direction.
     * The Runtime position carries the active policy direction, so convert it back. */
    const int32_t source = cal_identity(0U, 0).encoder_direction;
    const struct foc_calibration_angle_sample sample = {
        .mechanical_rad = (float)(source * (int32_t)policy.direction) * view->mechanical_rad,
        .electrical_rad = cal->theta_rad,
        .captured_us = now_us,
        .sequence = cal->sample_index++,
        .valid = true,
    };
    if (cal->sweep_count >= FOC_BOARD_CAL_SWEEP_SAMPLES) {
        cal_fail(cal, FOC_BOARD_CAL_FAILURE_ROTOR, now_us);
        return;
    }
    cal->sweep[cal->sweep_count++] = sample;
}

/* Average each forward sample with the backward sample at the same commanded angle:
 * the drive lag reverses with the sweep and cancels. The turnaround points have no
 * opposite partner and are left out. */
static bool cal_collect_pairs(struct foc_board_calibration *cal)
{
    const uint32_t count = cal->sweep_count;
    if (count < 5U || count % 2U == 0U) {
        return false;
    }
    for (uint32_t index = 1U; index < count / 2U; ++index) {
        const struct foc_calibration_angle_sample *forward = &cal->sweep[index];
        const struct foc_calibration_angle_sample *backward = &cal->sweep[count - 1U - index];
        const struct foc_calibration_angle_sample pair = {
            .mechanical_rad = 0.5f * (forward->mechanical_rad + backward->mechanical_rad),
            .electrical_rad = forward->electrical_rad,
            .captured_us = forward->captured_us,
            .sequence = forward->sequence,
            .valid = true,
        };
        if (foc_calibration_rotor_collect(&cal->rotor_estimator, &pair) != CAL_OK) {
            return false;
        }
    }
    return true;
}

/* Fit, admit and apply. Producers are held only around the re-initialization. */
static void cal_apply(struct foc_board_calibration *cal, uint32_t now_us)
{
    if (!cal_collect_pairs(cal) ||
        foc_calibration_rotor_finalize(&cal->rotor_estimator, &cal->rotor_result) != CAL_OK ||
        !cal->rotor_result.identifiable) {
        cal_fail(cal, FOC_BOARD_CAL_FAILURE_ROTOR, now_us);
        return;
    }
    const uint16_t pole_pairs = cal->rotor_result.pole_pairs;
    const int32_t direction = cal->rotor_result.direction;
    const struct foc_calibration_offset_config offset_config = {
        .identity = cal_identity(pole_pairs, direction),
        .maximum_residual_rad = 0.2f,
        .maximum_interval_us = 1000000U,
        .maximum_samples = FOC_CALIBRATION_MAX_REFERENCE_SAMPLES,
    };
    bool offset_ok = foc_calibration_offset_init(&cal->offset_estimator, &offset_config) == CAL_OK;
    for (uint32_t index = 0U; offset_ok && index < cal->rotor_estimator.sample_count; ++index) {
        offset_ok = foc_calibration_offset_collect(
                        &cal->offset_estimator,
                        &cal->rotor_estimator.samples[index]
                    ) == CAL_OK;
    }
    if (!offset_ok ||
        foc_calibration_offset_finalize(&cal->offset_estimator, &cal->offset_result) != CAL_OK ||
        !cal->offset_result.usable) {
        cal_fail(cal, FOC_BOARD_CAL_FAILURE_OFFSET, now_us);
        return;
    }
    /* CAL-1 ran before the pole pairs were known; its identity takes the fitted value. */
    cal->adc_result.identity.pole_pairs = pole_pairs;
    const struct foc_calibration_identity expected = cal_identity(pole_pairs, 0);
    const struct foc_calibration_admission_input input = {
        .adc = &cal->adc_result,
        .rotor = &cal->rotor_result,
        .offset = &cal->offset_result,
        .expected_identity = &expected,
        .limits = {
            .minimum_adc_samples = CAL_ADC_SAMPLES,
            .minimum_offset_counts = (float)FOC_BOARD_OFFSET_MIN_COUNTS,
            .maximum_offset_counts = (float)FOC_BOARD_OFFSET_MAX_COUNTS,
            .maximum_noise_rms_counts = 4.0f,
            /* Applies to the rms and to the largest single residual. */
            .maximum_rotor_residual_rad = 0.2f,
            .maximum_offset_residual_rad = 0.2f,
        },
    };
    struct foc_calibration_snapshot snapshot;
    if (foc_calibration_admit(&input, &snapshot, &cal->admission_failure) !=
        FOC_CALIBRATION_ADMISSION_OK) {
        cal_fail(cal, FOC_BOARD_CAL_FAILURE_ADMISSION, now_us);
        return;
    }
    if (cal->quiesce != NULL) {
        cal->quiesce(cal->quiesce_argument, true);
    }
    int status = foc_board_reconfigure(cal->runtime, cal->output, cal->encoder, &snapshot, now_us);
    if (status == FOC_RUNTIME_OK) {
        /* Tuning outlives a re-initialization; a refused value keeps the new defaults. */
        (void)foc_runtime_apply_tuning(cal->runtime, &cal->tuning);
    }
    if (cal->quiesce != NULL) {
        cal->quiesce(cal->quiesce_argument, false);
    }
    if (status != FOC_RUNTIME_OK) {
        cal_fail(cal, FOC_BOARD_CAL_FAILURE_APPLY, now_us);
        return;
    }
    cal->snapshot = snapshot;
    cal->lifecycle_sequence = 0U;
    cal->applied_runs++;
    cal_enter(cal, FOC_BOARD_CAL_DONE, now_us);
}

/* Public Functions ------------------------------------------------------------- */

void foc_board_calibration_bind(
    struct foc_board_calibration *calibration,
    struct foc_runtime *runtime,
    struct foc_board_output *output,
    struct foc_board_encoder *encoder,
    struct foc_board_adc *adc,
    struct foc_serial *serial,
    foc_board_cal_quiesce quiesce,
    void *quiesce_argument
)
{
    *calibration = (struct foc_board_calibration){
        .runtime = runtime,
        .output = output,
        .encoder = encoder,
        .adc = adc,
        .serial = serial,
        .quiesce = quiesce,
        .quiesce_argument = quiesce_argument,
    };
    atomic_init(&calibration->adc_active, 0U);
    atomic_init(&calibration->adc_samples, 0U);
    adc->tap_argument = calibration;
    adc->tap = cal_adc_tap;
}

bool foc_board_calibration_busy(const struct foc_board_calibration *calibration)
{
    return calibration->state != FOC_BOARD_CAL_IDLE && calibration->state != FOC_BOARD_CAL_DONE &&
        calibration->state != FOC_BOARD_CAL_FAILED;
}

int foc_board_calibration_start(struct foc_board_calibration *cal, uint32_t now_us)
{
    if (cal->serial->target_policy != FOC_SERIAL_TARGET_VIRTUAL_ONLY &&
        cal->output->backend == NULL) {
        return FOC_PROTO_OUTPUT_LOCKED; /* CAL-2 must drive the motor. */
    }
    if (foc_board_calibration_busy(cal)) {
        return FOC_PROTO_BUSY;
    }
    struct foc_runtime_snapshot view;
    if (foc_runtime_read_snapshot(cal->runtime, &view) != FOC_RUNTIME_OK) {
        return FOC_PROTO_BUSY;
    }
    struct foc_runtime_tuning_result tuning_result;
    if (!cal_quiet(&view) || fabsf(view.mechanical_rad_s) > CAL_STILL_RAD_S ||
        foc_runtime_read_tuning_result(cal->runtime, &tuning_result) != FOC_RUNTIME_OK ||
        tuning_result.result == FOC_RUNTIME_REQUEST_PENDING ||
        foc_runtime_read_tuning(cal->runtime, &cal->tuning) != FOC_RUNTIME_OK) {
        return FOC_PROTO_BAD_STATE;
    }
    const struct foc_calibration_adc_config adc_config = {
        .identity = cal_identity(0U, 0),
        .required_valid_mask = 7U,
        .minimum_samples = CAL_ADC_SAMPLES,
        .maximum_samples = CAL_ADC_SAMPLES,
        .maximum_sample_step_counts = 40.0f,
        .maximum_noise_rms_counts = 4.0f,
        .minimum_offset_counts = (float)FOC_BOARD_OFFSET_MIN_COUNTS,
        .maximum_offset_counts = (float)FOC_BOARD_OFFSET_MAX_COUNTS,
        .maximum_interval_us = 1000U,
    };
    const struct foc_calibration_rotor_config rotor_config = {
        .identity = cal_identity(0U, 0),
        .minimum_pole_pairs = 1U,
        .maximum_pole_pairs = 30U,
        .minimum_mechanical_span_rad = 0.3f,
        .maximum_residual_rad = 0.1f,
        .minimum_candidate_separation_rad = 0.05f,
        .maximum_interval_us = 1000000U,
        .maximum_samples = FOC_CALIBRATION_MAX_REFERENCE_SAMPLES,
    };
    if (foc_calibration_adc_init(&cal->adc_estimator, &adc_config) != CAL_OK ||
        foc_calibration_rotor_init(&cal->rotor_estimator, &rotor_config) != CAL_OK) {
        return FOC_PROTO_INTERNAL_ERROR;
    }
    cal->failure = FOC_BOARD_CAL_FAILURE_NONE;
    cal->failed_state = FOC_BOARD_CAL_IDLE;
    cal->admission_failure = FOC_CALIBRATION_ADMISSION_NONE;
    cal->adc_result = (struct foc_calibration_adc_result){0};
    cal->rotor_result = (struct foc_calibration_rotor_result){0};
    cal->offset_result = (struct foc_calibration_offset_result){0};
    cal->lifecycle_sequence = 0U;
    cal->sample_index = 0U;
    cal->sweep_count = 0U;
    cal->voltage_v = 0.0f;
    cal->theta_rad = 0.0f;
    cal->sweep_rad = 0.0f;
    cal->last_us = now_us;
    atomic_store(&cal->adc_samples, 0U);
    cal_enter(cal, FOC_BOARD_CAL_ADC_ZERO, now_us);
    atomic_store_explicit(&cal->adc_active, 1U, memory_order_release);
    return FOC_PROTO_ACCEPTED;
}

void foc_board_calibration_abort(struct foc_board_calibration *calibration)
{
    if (foc_board_calibration_busy(calibration)) {
        calibration->failure = FOC_BOARD_CAL_FAILURE_ABORTED;
    }
}

void foc_board_calibration_poll(struct foc_board_calibration *cal, uint32_t now_us)
{
    if (!foc_board_calibration_busy(cal)) {
        return;
    }
    const float dt_s = (float)(now_us - cal->last_us) * 1.0e-6f;
    cal->last_us = now_us;
    struct foc_runtime_snapshot view;
    if (foc_runtime_read_snapshot(cal->runtime, &view) != FOC_RUNTIME_OK) {
        return;
    }
    if (cal->failure == FOC_BOARD_CAL_FAILURE_ABORTED) {
        cal_fail(cal, FOC_BOARD_CAL_FAILURE_ABORTED, now_us);
        return;
    }
    const bool driving = cal->state >= FOC_BOARD_CAL_ALIGN && cal->state <= FOC_BOARD_CAL_SWEEP;
    if (driving && view.state != FOC_SAFE_STATE_RUNNING) {
        cal_fail(cal, FOC_BOARD_CAL_FAILURE_DRIVE, now_us);
        return;
    }
    switch (cal->state) {
    case FOC_BOARD_CAL_ADC_ZERO:
        if (!cal_quiet(&view) || fabsf(view.mechanical_rad_s) > CAL_STILL_RAD_S) {
            cal_fail(cal, FOC_BOARD_CAL_FAILURE_STATE, now_us);
        } else if (
            atomic_load_explicit(&cal->adc_samples, memory_order_acquire) >= CAL_ADC_SAMPLES
        ) {
            atomic_store_explicit(&cal->adc_active, 0U, memory_order_release);
            if (foc_calibration_adc_finalize(&cal->adc_estimator, &cal->adc_result) != CAL_OK ||
                !cal->adc_result.usable) {
                cal_fail(cal, FOC_BOARD_CAL_FAILURE_ADC, now_us);
            } else {
                cal_enter(cal, FOC_BOARD_CAL_DRIVE_MODE, now_us);
            }
        } else if (now_us - cal->state_us > CAL_ADC_TIMEOUT_US) {
            cal_fail(cal, FOC_BOARD_CAL_FAILURE_ADC, now_us);
        }
        break;
    case FOC_BOARD_CAL_DRIVE_MODE:
        cal_step_lifecycle(
            cal,
            FOC_RUNTIME_ACTION_SET_MODE,
            FOC_RUNTIME_MODE_CALIBRATION,
            FOC_BOARD_CAL_DRIVE_ARM,
            now_us
        );
        break;
    case FOC_BOARD_CAL_DRIVE_ARM:
        cal_drive(cal, now_us);
        cal_step_lifecycle(
            cal,
            FOC_RUNTIME_ACTION_ARM,
            FOC_RUNTIME_MODE_CALIBRATION,
            FOC_BOARD_CAL_DRIVE_START,
            now_us
        );
        break;
    case FOC_BOARD_CAL_DRIVE_START:
        cal_drive(cal, now_us);
        cal_step_lifecycle(
            cal,
            FOC_RUNTIME_ACTION_START,
            FOC_RUNTIME_MODE_CALIBRATION,
            FOC_BOARD_CAL_ALIGN,
            now_us
        );
        break;
    case FOC_BOARD_CAL_ALIGN: {
        const float current = view.current_valid
            ? sqrtf(
                  view.measured_id_a * view.measured_id_a + view.measured_iq_a * view.measured_iq_a
              )
            : 0.0f;
        const float limit = cal->runtime->pipeline.current.config.pwm.voltage_limit_v;
        if (current < CAL_ALIGN_CURRENT_A && cal->voltage_v < limit) {
            /* Slow ramp: the rotor follows quasi-statically instead of ringing. */
            cal->voltage_v = fminf(cal->voltage_v + CAL_RAMP_V_PER_S * dt_s, limit);
            cal->state_us = now_us;
        } else if (current < CAL_MIN_CURRENT_A) {
            cal_fail(cal, FOC_BOARD_CAL_FAILURE_CURRENT, now_us);
            break;
        } else if (now_us - cal->state_us >= CAL_SETTLE_US) {
            cal_sample(cal, &view, now_us);
            cal->sweep_rad = 0.0f;
            cal_enter(cal, FOC_BOARD_CAL_SWEEP, now_us);
        }
        cal_drive(cal, now_us);
        break;
    }
    case FOC_BOARD_CAL_SWEEP: {
        /* Forward CAL_SWEEP_TURNS electrical turns, then back to zero. */
        const float span = CAL_SWEEP_TURNS * CAL_TWO_PI;
        const float spacing = CAL_TWO_PI / (float)CAL_SAMPLES_PER_TURN;
        const float previous = cal->sweep_rad;
        cal->sweep_rad = fminf(previous + CAL_SWEEP_RAD_PER_S * dt_s, 2.0f * span);
        cal->theta_rad = cal->sweep_rad <= span ? cal->sweep_rad : 2.0f * span - cal->sweep_rad;
        if (floorf(cal->sweep_rad / spacing) > floorf(previous / spacing)) {
            cal_sample(cal, &view, now_us);
            if (cal->state != FOC_BOARD_CAL_SWEEP) {
                break;
            }
        }
        cal_drive(cal, now_us);
        if (cal->sweep_rad >= 2.0f * span) {
            foc_runtime_revoke_command(cal->runtime);
            cal_enter(cal, FOC_BOARD_CAL_DRIVE_STOP, now_us);
        }
        break;
    }
    case FOC_BOARD_CAL_DRIVE_STOP:
        if (cal_quiet(&view)) {
            cal_enter(cal, FOC_BOARD_CAL_APPLY, now_us);
        } else if (now_us - cal->state_us > CAL_STOP_TIMEOUT_US) {
            cal_fail(cal, FOC_BOARD_CAL_FAILURE_TIMEOUT, now_us);
        }
        break;
    case FOC_BOARD_CAL_APPLY:
        cal_apply(cal, now_us);
        break;
    default:
        break;
    }
}

/* UART adapter callbacks -------------------------------------------------------- */

static int cal_serial_start(void *context, uint32_t now_us)
{
    return foc_board_calibration_start(context, now_us);
}

static void cal_serial_abort(void *context)
{
    foc_board_calibration_abort(context);
}

static bool cal_serial_busy(const void *context)
{
    return foc_board_calibration_busy(context);
}

static bool cal_serial_read(const void *context, struct foc_serial_calibration_view *view)
{
    const struct foc_board_calibration *cal = context;
    const bool applied = cal->snapshot.accepted;
    float noise = fmaxf(cal->adc_result.noise_rms_a_counts, cal->adc_result.noise_rms_b_counts);
    noise = fmaxf(noise, cal->adc_result.noise_rms_c_counts);
    *view = (struct foc_serial_calibration_view){
        .state = (uint8_t)cal->state,
        .failure = (uint8_t)cal->failure,
        .failed_state = (uint8_t)cal->failed_state,
        .admission_failure = (uint8_t)cal->admission_failure,
        .samples = (uint16_t)cal->sweep_count,
        .pole_pairs = cal->rotor_result.pole_pairs,
        .calibration_direction = (int8_t)cal->rotor_result.direction,
        .runtime_direction = (int8_t)(applied ? cal->snapshot.runtime_encoder_direction : 0),
        .electrical_offset_rad = cal->offset_result.electrical_offset_rad,
        .rotor_residual_rad = cal->rotor_result.residual_rms_rad,
        .adc_offset_count =
            {cal->adc_result.offset_a_counts,
             cal->adc_result.offset_b_counts,
             cal->adc_result.offset_c_counts},
        .adc_noise_rms_count = noise,
        .applied_runs = cal->applied_runs,
    };
    return true;
}

struct foc_serial_calibration foc_board_calibration_serial(
    struct foc_board_calibration *calibration
)
{
    return (struct foc_serial_calibration){
        .context = calibration,
        .start = cal_serial_start,
        .abort = cal_serial_abort,
        .busy = cal_serial_busy,
        .read = cal_serial_read,
    };
}

/* ---------------------------------------- end of file ----------------------------------------- */
