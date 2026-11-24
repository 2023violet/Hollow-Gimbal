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
 *
 * CAL-7 (stage mask 8): q voltage of fixed magnitude on the rotor angle, reversed
 * when the rotor passes half the ID 109 travel from where it started or nears the
 * speed limit, so it accelerates and brakes in turn within the travel. Each H0
 * record's speed and Kt x iq feed the inertia/damping fit; TUNE writes J and B,
 * and the unified refresh derives the outer gains at the velocity bandwidth.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_board_calibrate.h"
#include "foc_board_boot.h"
#include "foc_board_profile.h"

#include <math.h>

/* Private Macros --------------------------------------------------------------- */

/* Estimator success code (0), private to foc_calibration.c. */
#define CAL_OK                      0
#define CAL_TWO_PI                  6.2831853f
#define CAL_ADC_SAMPLES             256U
#define CAL_ADC_TIMEOUT_US          500000U
#define CAL_ADC_MAX_NOISE_COUNTS    4.0f
#define CAL_STILL_RAD_S             0.5f
#define CAL_LIFECYCLE_TIMEOUT_US    20000U
#define CAL_LIFECYCLE_ATTEMPTS      50U
/* H0 rejects ARM/START until fresh sensing follows a STOP: space the retries. */
#define CAL_RETRY_US                2000U
#define CAL_ALIGN_CURRENT_A         0.5f
#define CAL_MIN_CURRENT_A           0.1f
#define CAL_RAMP_V_PER_S            2.0f
#define CAL_SETTLE_US               300000U
#define CAL_SWEEP_TURNS             2.0f
/* Half an electrical turn per second: under voltage drive the only damping is the back-EMF
 * current (~ flux^2 / Rs), so a faster sweep rings at the reversal on high-Rs motors
 * (SIL: max residual 0.22 rad at 1.3 Ohm at one turn/s, <= 0.07 rad at half). */
#define CAL_SWEEP_RAD_PER_S         (0.5f * CAL_TWO_PI)
#define CAL_SAMPLES_PER_TURN        12U
#define CAL_SENSOR_AGE_US           2000U
#define CAL_STOP_TIMEOUT_US         20000U
#define CAL_FAIL_QUIET_US           1000000U
/* A refused automatic ADC zero is retried after this interval. */
#define CAL_ZERO_RETRY_US           1000000U
/* Settling time of the encoder speed after a re-initialization, before a zero run. */
#define CAL_ZERO_SETTLE_US          100000U
/* M3b: H0 slot clock and preload. A command computed in slot k takes effect at the
 * next update, so an aperture interval sees 80 % of V(k-2) and 20 % of V(k-1). */
#define CAL_SLOT_US                 ((uint32_t)FOC_BOARD_H0_US)
#define CAL_PRELOAD_NEW             ((float)FOC_BOARD_ADC_COMPARE_US / (float)FOC_BOARD_H0_US)
#define CAL_RL_LEVEL_SLOTS          24U
#define CAL_RL_INTERVALS            768U
#define CAL_RL_STILL_RAD_S          1.0f
/* Below this window-mean speed RMS the q shaft is still: the two-term fit is published. */
#define CAL_RL_MOTION_RAD_S         0.05f
#define CAL_FLUX_SETTLE_US          400000U
/* A level is captured once its speed has the level's sign and is within 5 % of steady: a
 * heavy rotor (tau = J Rs / (ke Kt)) takes seconds, and after a reversal it still turns the
 * old way. Every 100 ms the mean speed of the interval is taken from the encoder angle and
 * its capture times (no filter lag or staircase at a crawl). Two successive changes d1, d2
 * of an exponential approach leave d2 r / (1 - r) to go, r = d2 / d1, whatever tau; a
 * change within 0.3 % of the speed is steady. The speed must also reach twice the
 * estimator's 1 rad/s electrical window floor. Each level waits at most 10 s (TIMEOUT). */
#define CAL_FLUX_STEADY_US          100000U
#define CAL_FLUX_STEADY_REMAINING   0.05f
#define CAL_FLUX_STEADY_FLOOR       0.003f
#define CAL_FLUX_MIN_SPEED_E_RAD_S  2.0f
#define CAL_FLUX_SETTLE_MAX_US      10000000U
#define CAL_FLUX_WINDOW             32U
#define CAL_FLUX_WINDOWS            4U
#define CAL_TUNE_TIMEOUT_US         50000U
/* CAL-7 windows adapt to the motion: a probe measures the time between reversals and a
 * window is a tenth of it, so each window sees a speed change well above the speed noise
 * on a heavy rotor and a switching transient stays short on a light one. The capture then
 * spans at least four more reversals and 40 windows, at most 60000 samples (12 s). */
#define CAL_INERTIA_PROBE_REVERSALS 3U
#define CAL_INERTIA_PROBE_US        10000000U
#define CAL_INERTIA_MIN_WINDOW      5U
#define CAL_INERTIA_MAX_WINDOW      500U
#define CAL_INERTIA_WINDOWS         40U
#define CAL_INERTIA_MAX_SAMPLES     60000U
#define CAL_INERTIA_REVERSALS       4U

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
    atomic_store(&cal->record_active, 0U);
    if ((cal->state >= FOC_BOARD_CAL_DRIVE_MODE && cal->state <= FOC_BOARD_CAL_SWEEP) ||
        (cal->state >= FOC_BOARD_CAL_RL_D && cal->state <= FOC_BOARD_CAL_FLUX) ||
        cal->state == FOC_BOARD_CAL_INERTIA) {
        foc_runtime_revoke_command(cal->runtime);
    }
    if (cal->failing) {
        /* Failed while returning the mode: stop here and keep the first reason. */
        cal_enter(cal, FOC_BOARD_CAL_FAILED, now_us);
        return;
    }
    cal->failed_state = cal->state;
    cal->failure = failure;
    if (cal->mode_claimed) {
        cal->failing = true;
        cal->lifecycle_sequence = 0U;
        cal_enter(cal, FOC_BOARD_CAL_RESTORE, now_us);
        return;
    }
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

static void cal_drive_dq(
    struct foc_board_calibration *cal,
    float vd,
    float vq,
    float theta,
    bool follow_rotor,
    uint32_t now_us
)
{
    const struct foc_runtime_command command = {
        .mode = FOC_RUNTIME_MODE_CALIBRATION,
        /* One publication sequence with the UART adapter: the Runtime admits only newer
         * commands, so a private counter would lock out the Host after a run. */
        .seq = ++cal->serial->publication_seq,
        .produced_us = now_us,
        .target.calibration =
            {.voltage_v = {vd, vq}, .theta_elec_rad = theta, .follow_rotor = follow_rotor},
    };
    (void)foc_runtime_publish_calibration_command(cal->runtime, &command, now_us);
}

static void cal_drive(struct foc_board_calibration *cal, uint32_t now_us)
{
    cal_drive_dq(cal, cal->voltage_v, 0.0f, cal->theta_rad, false, now_us);
}

/* H0, after each Runtime step of a valid frame, while an electrical stage records. */
static void
cal_h0_observer(void *argument, const struct foc_runtime *runtime, uint32_t slot_id, int h0_result)
{
    struct foc_board_calibration *cal = argument;
    if (atomic_load_explicit(&cal->record_active, memory_order_acquire) == 0U) {
        return;
    }
    const unsigned int head = atomic_load_explicit(&cal->ring_head, memory_order_relaxed);
    const unsigned int tail = atomic_load_explicit(&cal->ring_tail, memory_order_acquire);
    if (head - tail >= FOC_BOARD_CAL_RING) {
        atomic_store_explicit(&cal->ring_overflow, 1U, memory_order_relaxed);
        return;
    }
    cal->ring[head % FOC_BOARD_CAL_RING] = (struct foc_board_cal_record){
        .slot = slot_id,
        .current_a = runtime->pipeline.measured_dq_a,
        .voltage_v = runtime->pipeline.voltage_dq_v,
        .mechanical_rad_s = runtime->observed_sensor.mechanical_rad_s,
        .mechanical_rad = runtime->observed_sensor.mechanical_rad,
        .aperture_us = runtime->adc.produced_us,
        .sensor_us = runtime->observed_sensor.captured_us,
        .accepted = h0_result == FOC_RUNTIME_OK &&
            runtime->pipeline.safety.state == FOC_SAFE_STATE_RUNNING &&
            runtime->pipeline.last_reason == FOC_PIPELINE_OK && runtime->observed_sensor.valid,
    };
    atomic_store_explicit(&cal->ring_head, head + 1U, memory_order_release);
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
            .maximum_noise_rms_counts = CAL_ADC_MAX_NOISE_COUNTS,
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
    cal->committed |= FOC_BOARD_CAL_STAGE_ROTOR;
    cal->active_source = FOC_BOARD_CAL_SOURCE_RUN;
    cal->mode_claimed = false;
    cal->zero_ready = true;
    cal->zero_refused = false;
    if ((cal->stages & (FOC_BOARD_CAL_STAGE_ELECTRICAL | FOC_BOARD_CAL_STAGE_MECHANICAL)) != 0U) {
        cal->electrical_phase = true;
        cal_enter(cal, FOC_BOARD_CAL_DRIVE_MODE, now_us);
        return;
    }
    cal_enter(cal, FOC_BOARD_CAL_DONE, now_us);
}

/* Electrical stages ------------------------------------------------------------- */

static float cal_voltage_limit(const struct foc_board_calibration *cal)
{
    return cal->runtime->pipeline.current.config.pwm.voltage_limit_v;
}

/* A commutation group other than the boot placeholder is in effect. */
static bool cal_commutation_valid(const struct foc_board_calibration *cal)
{
    return cal->snapshot.accepted && cal->active_source != FOC_BOARD_CAL_SOURCE_BOOT;
}

/* Re-initialize under quiescence (NULL snapshot: nominal) and keep the given tuning. */
static int cal_reconfigure(
    struct foc_board_calibration *cal,
    const struct foc_calibration_snapshot *snapshot,
    const struct foc_runtime_tuning *tuning,
    uint32_t now_us
)
{
    if (cal->quiesce != NULL) {
        cal->quiesce(cal->quiesce_argument, true);
    }
    const int status = snapshot != NULL
        ? foc_board_reconfigure(cal->runtime, cal->output, cal->encoder, snapshot, now_us)
        : foc_board_reconfigure_nominal(cal->runtime, cal->output, cal->encoder, now_us);
    if (status == FOC_RUNTIME_OK) {
        /* Tuning outlives a re-initialization; a refused value keeps the new defaults. */
        (void)foc_runtime_apply_tuning(cal->runtime, tuning);
    }
    if (cal->quiesce != NULL) {
        cal->quiesce(cal->quiesce_argument, false);
    }
    return status;
}

/* Zero-only run: the measured ADC zero replaces the valid group's, without a drive. */
static void cal_zero_apply(struct foc_board_calibration *cal, uint32_t now_us)
{
    struct foc_calibration_snapshot snapshot = cal->snapshot;
    snapshot.phase_a_offset_count = cal->adc_result.offset_a_counts;
    snapshot.phase_b_offset_count = cal->adc_result.offset_b_counts;
    snapshot.phase_c_offset_count = cal->adc_result.offset_c_counts;
    if (!foc_calibration_snapshot_valid(&snapshot) ||
        cal_reconfigure(cal, &snapshot, &cal->tuning, now_us) != FOC_RUNTIME_OK) {
        cal_fail(cal, FOC_BOARD_CAL_FAILURE_APPLY, now_us);
        return;
    }
    cal->snapshot = snapshot;
    cal->zero_ready = true;
    cal->zero_refused = false;
    cal_enter(cal, FOC_BOARD_CAL_DONE, now_us);
}

/* Foreground, while idle: measure this boot's ADC zero once a valid group exists. */
static void cal_auto_zero(struct foc_board_calibration *cal, uint32_t now_us)
{
    if (cal->zero_pending) {
        cal->zero_pending = false;
        /* Only a refused measurement waits; an interrupted one (shaft moved, state
         * changed) retries after the settling time. */
        cal->zero_refused = !cal->zero_ready && cal->failure == FOC_BOARD_CAL_FAILURE_ADC;
        cal->zero_attempted = cal->zero_refused;
        cal->zero_attempt_us = now_us;
        if (!cal->zero_ready && !cal->zero_refused) {
            cal->zero_settling = true;
            cal->zero_settle_us = now_us;
        }
    }
    if (cal->zero_settling && now_us - cal->zero_settle_us >= CAL_ZERO_SETTLE_US) {
        cal->zero_settling = false;
    }
    if (cal->zero_ready || !cal_commutation_valid(cal) || cal->zero_settling ||
        (cal->zero_attempted && now_us - cal->zero_attempt_us < CAL_ZERO_RETRY_US)) {
        return;
    }
    /* A transient refusal (moving shaft, pending tuning, not yet inhibited) retries on the
     * next pass; only a measurement that ran is rate-limited. */
    if (foc_board_calibration_start(cal, now_us, FOC_BOARD_CAL_STAGE_ZERO) == FOC_PROTO_ACCEPTED) {
        cal->zero_pending = true;
        cal->zero_attempted = true;
        cal->zero_attempt_us = now_us;
    }
}

static float cal_mean(float older, float newer)
{
    return (1.0f - CAL_PRELOAD_NEW) * older + CAL_PRELOAD_NEW * newer;
}

/* Begin a capture: clear the recorder history; the ring keeps running. */
static void cal_capture_begin(struct foc_board_calibration *cal)
{
    cal->history_count = 0U;
    cal->rl_pending_count = 0U;
    cal->rl_points = 0U;
    cal->first_pushed = false;
    cal->pushed = 0U;
    cal->capturing = true;
}

static bool cal_rl_begin(struct foc_board_calibration *cal, enum foc_motor_id_axis axis)
{
    const float limit = cal_voltage_limit(cal);
    const struct foc_motor_id_config config = {
        .axis = axis,
        .source = FOC_MOTOR_ID_MEASURED,
        .voltage_source = FOC_MOTOR_ID_VOLTAGE_COMMANDED,
        .setup_id = 1U,
        .min_interval_us = CAL_SLOT_US,
        .max_interval_us = CAL_SLOT_US,
        .intervals_per_window = 8U,
        .min_windows = 32U,
        .max_abs_current_a = cal->runtime->pipeline.current.config.current_limit_a,
        .max_abs_voltage_v = limit,
        .min_current_rms_a = 0.05f,
        .min_slew_rms_a_per_s = 5.0f,
        .min_voltage_rms_v = 0.05f,
        .max_residual_rms_v = 0.05f,
        /* Wide bounds: the fit itself refuses a motor whose pole the 200 us interval
         * does not resolve (interval x Rs / L above 0.3, i.e. Rs / L above 1500 /s). */
        .min_rs_ohm = 0.03f,
        .max_rs_ohm = 10.0f,
        .min_inductance_h = 0.00005f,
        .max_inductance_h = 0.05f,
        /* q: the pulses turn a light shaft (speed ~ Kt i t / J); the back-EMF is fitted
         * from the position change, so no inertia is too small. Up to the speed limit. */
        .fit_back_emf = axis == FOC_MOTOR_ID_Q,
        .max_abs_speed_rad_s = cal->runtime->config.motion.position.velocity_limit_rad_s,
        .min_speed_rms_rad_s = CAL_RL_MOTION_RAD_S,
        /* The d-axis Rs: a turning q fit takes R from the still axis, fits L and ke. */
        .known_rs_ohm = axis == FOC_MOTOR_ID_Q ? cal->rl_d.rs_ohm : 0.0f,
    };
    cal_capture_begin(cal);
    return foc_motor_id_init(&cal->motor_id, &config, ++cal->capture_serial) == FOC_MOTOR_ID_OK;
}

static bool cal_flux_begin(struct foc_board_calibration *cal)
{
    const struct foc_flux_id_config config = {
        .identity =
            {
                .session = 1U,
                .capture = ++cal->capture_serial,
                .motor = 1U,
                .dq_frame = 1U,
                .current_chain = 1U,
                .voltage_chain = 1U,
                .speed_chain = 1U,
                .clock = 1U,
                .rl_revision = cal->capture_serial,
                /* Rotor mapping revision: 1 is the boot one (restored or nominal),
                 * each applied CAL-1/2 run in this boot adds one; never 0. */
                .poles_revision = cal->applied_runs + 1U,
                .phase_order = 0U,
                .pole_pairs = cal->runtime->config.motion.rotor.pole_pairs,
            },
        .rl = cal->rl_model,
        .rl_qualified = true,
        .source = FOC_MOTOR_ID_MEASURED,
        .voltage_source = FOC_MOTOR_ID_VOLTAGE_COMMANDED,
        .interval_us = CAL_SLOT_US,
        .intervals_per_window = CAL_FLUX_WINDOW,
        .min_windows_per_direction = 2U * CAL_FLUX_WINDOWS,
        .max_abs_current_a = cal->runtime->pipeline.current.config.current_limit_a,
        .max_abs_vq_v = cal_voltage_limit(cal),
        .max_abs_speed_rad_s = 2000.0,
        .min_abs_window_speed_rad_s = 1.0,
        .min_speed_span_rad_s = 1.0,
        .min_speed_rms_rad_s = 5.0,
        .min_flux_wb = 0.0005,
        .max_flux_wb = 0.5,
        .max_residual_rms_v = 0.05,
        .max_direction_difference_relative = 0.1,
    };
    cal->capturing = false;
    cal->pushed = 0U;
    cal->level = 0U;
    cal->flux_checks = 0U;
    cal->flux_samples = 0U;
    return foc_flux_id_init(&cal->flux_id, &config) == FOC_CALIBRATION_FAILURE_NONE;
}

/* Feed one contiguous record to the stage's estimator. False on a capture failure. */
static bool
cal_record_estimate(struct foc_board_calibration *cal, const struct foc_board_cal_record *record)
{
    /* A flux level ends exactly on a window boundary: no open partial window. */
    if (cal->state == FOC_BOARD_CAL_FLUX && cal->pushed >= CAL_FLUX_WINDOW * CAL_FLUX_WINDOWS) {
        cal->capturing = false;
        return true;
    }
    if (cal->history_count != 0U &&
        record->slot != cal->history[cal->history_count - 1U].slot + 1U) {
        return false;
    }
    if (cal->state == FOC_BOARD_CAL_INERTIA) {
        /* One endpoint per H0 slot: speed and Kt x iq of the same aperture. */
        const uint32_t begin =
            cal->history_count == 0U ? record->slot : cal->history[cal->history_count - 1U].slot;
        const struct foc_mechanical_id_sample sample = {
            .setup_id = 1U,
            .torque_model_id = 1U,
            .capture_id = cal->capture_serial,
            .torque_source = FOC_MECHANICAL_ID_TORQUE_OBSERVED_DQ,
            .load_source = FOC_MECHANICAL_ID_LOAD_NEGLIGIBLE,
            .sequence = record->slot,
            .speed_us = record->slot * CAL_SLOT_US,
            .torque_us = record->slot * CAL_SLOT_US,
            .interval_begin_us = begin * CAL_SLOT_US,
            .speed_rad_s = record->mechanical_rad_s,
            .id_a = record->current_a.d,
            .iq_a = record->current_a.q,
            .speed_valid = true,
            .torque_valid = true,
            /* follow_rotor keeps the frame on the calibrated rotor angle. */
            .frame_confirmed = true,
            .interval_confirmed = record->accepted,
        };
        cal->history[0] = *record;
        cal->history_count = 1U;
        cal->pushed++;
        if (foc_mechanical_id_push(&cal->mech_id, &sample) != FOC_MECHANICAL_ID_OK) {
            return false;
        }
        /* Stop only on a window boundary, once the run is long and varied enough. */
        const uint32_t windows = (cal->pushed - 1U) / cal->inertia_window;
        if ((cal->pushed - 1U) % cal->inertia_window == 0U &&
            ((windows >= CAL_INERTIA_WINDOWS && cal->inertia_reversals >= CAL_INERTIA_REVERSALS) ||
             cal->pushed + cal->inertia_window > CAL_INERTIA_MAX_SAMPLES)) {
            cal->capturing = false;
        }
        return true;
    }
    if (cal->history_count < 2U) {
        cal->history[cal->history_count++] = *record;
        return true;
    }
    const struct foc_board_cal_record *older = &cal->history[0];
    const struct foc_board_cal_record *begin = &cal->history[1];
    const bool confirmed = older->accepted && begin->accepted && record->accepted;
    const float limit = cal_voltage_limit(cal);
    const float vd = cal_mean(older->voltage_v.d, begin->voltage_v.d);
    const float vq = cal_mean(older->voltage_v.q, begin->voltage_v.q);
    const bool saturated = hypotf(begin->voltage_v.d, begin->voltage_v.q) >= 0.98f * limit;
    bool ok = true;
    if (cal->state == FOC_BOARD_CAL_RL_D || cal->state == FOC_BOARD_CAL_RL_Q) {
        const bool d_axis = cal->state == FOC_BOARD_CAL_RL_D;
        const bool still = fabsf(begin->mechanical_rad_s) < CAL_RL_STILL_RAD_S &&
            fabsf(record->mechanical_rad_s) < CAL_RL_STILL_RAD_S;
        struct foc_motor_id_sample sample = {
            .setup_id = 1U,
            .capture_id = cal->capture_serial,
            .source = FOC_MOTOR_ID_MEASURED,
            .sequence = record->slot,
            .captured_us = record->slot * CAL_SLOT_US,
            .voltage_begin_us = begin->slot * CAL_SLOT_US,
            .voltage_end_us = record->slot * CAL_SLOT_US,
            .voltage_source = FOC_MOTOR_ID_VOLTAGE_COMMANDED,
            .current_a = d_axis ? record->current_a.d : record->current_a.q,
            .mean_voltage_v = d_axis ? vd : vq,
            .omega_e_rad_s = 0.0f,
            .rotor_angle_rad = record->mechanical_rad,
            .current_valid = true,
            .voltage_valid = true,
            .output_confirmed = confirmed,
            /* follow_rotor keeps the frame on the calibrated rotor angle. */
            .frame_confirmed = true,
            .stationary_confirmed = still,
            .saturated = saturated,
        };
        if (!cal->first_pushed) {
            struct foc_motor_id_sample first = sample;
            first.sequence = begin->slot;
            first.captured_us = begin->slot * CAL_SLOT_US;
            first.current_a = d_axis ? begin->current_a.d : begin->current_a.q;
            first.rotor_angle_rad = begin->mechanical_rad;
            ok = foc_motor_id_push(&cal->motor_id, &first) == FOC_MOTOR_ID_OK;
            cal->first_pushed = true;
        }
        ok = ok && foc_motor_id_push(&cal->motor_id, &sample) == FOC_MOTOR_ID_OK;
    } else if (cal->state == FOC_BOARD_CAL_FLUX) {
        const float pole_pairs = (float)cal->runtime->config.motion.rotor.pole_pairs;
        const struct foc_flux_id_interval interval = {
            .identity = cal->flux_id.config.identity,
            .source = FOC_MOTOR_ID_MEASURED,
            .voltage_source = FOC_MOTOR_ID_VOLTAGE_COMMANDED,
            .sequence = record->slot,
            .begin_us = begin->slot * CAL_SLOT_US,
            .end_us = record->slot * CAL_SLOT_US,
            .voltage_begin_us = begin->slot * CAL_SLOT_US,
            .voltage_end_us = record->slot * CAL_SLOT_US,
            .speed_begin_us = begin->slot * CAL_SLOT_US,
            .speed_end_us = record->slot * CAL_SLOT_US,
            .id_begin_a = begin->current_a.d,
            .iq_begin_a = begin->current_a.q,
            .id_end_a = record->current_a.d,
            .iq_end_a = record->current_a.q,
            .mean_omega_e_rad_s =
                0.5 * pole_pairs * (begin->mechanical_rad_s + record->mechanical_rad_s),
            .mean_vq_v = vq,
            .current_valid = true,
            .voltage_valid = true,
            .speed_valid = true,
            .frame_confirmed = true,
            .output_confirmed = confirmed,
            .terminal_voltage_witnessed = false,
            .saturated = saturated,
        };
        ok = foc_flux_id_push(&cal->flux_id, &interval) == FOC_CALIBRATION_FAILURE_NONE;
    }
    cal->history[0] = cal->history[1];
    cal->history[1] = *record;
    cal->pushed++;
    return ok;
}

/* CAL-3: the encoder samples at the H1 rate, so a record's angle is up to one H1 period
 * older than its current. Hold records until the encoder sample after their aperture
 * arrives, then interpolate the angle to the aperture: a stale angle would put a
 * Kt x delay / J term into the q-axis Rs. False on a capture failure. */
static bool
cal_record_align(struct foc_board_calibration *cal, const struct foc_board_cal_record *record)
{
    if (cal->rl_pending_count != 0U && record->slot != cal->rl_last_slot + 1U) {
        return false;
    }
    cal->rl_last_slot = record->slot;
    if (cal->rl_points == 0U || record->sensor_us != cal->rl_point_us[1]) {
        cal->rl_point_us[0] = cal->rl_point_us[1];
        cal->rl_point_rad[0] = cal->rl_point_rad[1];
        cal->rl_point_us[1] = record->sensor_us;
        cal->rl_point_rad[1] = record->mechanical_rad;
        cal->rl_points++;
    }
    if (cal->rl_pending_count == FOC_BOARD_CAL_RL_PENDING) {
        return false;
    }
    cal->rl_pending[cal->rl_pending_count++] = *record;
    uint32_t resolved = 0U;
    /* One call may resolve several records: stop at the capture length (whole windows). */
    while (cal->rl_points >= 2U && resolved < cal->rl_pending_count &&
           cal->pushed < CAL_RL_INTERVALS) {
        struct foc_board_cal_record aligned = cal->rl_pending[resolved];
        const int32_t after_first = (int32_t)(aligned.aperture_us - cal->rl_point_us[0]);
        if ((int32_t)(aligned.aperture_us - cal->rl_point_us[1]) > 0) {
            break;
        }
        ++resolved;
        if (after_first < 0) {
            /* Only before the first push: the record precedes the first encoder pair. */
            if (cal->history_count != 0U) {
                return false;
            }
            continue;
        }
        const float span = (float)(cal->rl_point_us[1] - cal->rl_point_us[0]);
        aligned.mechanical_rad = cal->rl_point_rad[0] +
            (cal->rl_point_rad[1] - cal->rl_point_rad[0]) * ((float)after_first / span);
        if (!cal_record_estimate(cal, &aligned)) {
            return false;
        }
    }
    for (uint32_t index = resolved; index < cal->rl_pending_count; ++index) {
        cal->rl_pending[index - resolved] = cal->rl_pending[index];
    }
    cal->rl_pending_count -= resolved;
    return true;
}

static bool cal_record(struct foc_board_calibration *cal, const struct foc_board_cal_record *record)
{
    if (!cal->capturing) {
        return true;
    }
    return cal->state == FOC_BOARD_CAL_RL_D || cal->state == FOC_BOARD_CAL_RL_Q
        ? cal_record_align(cal, record)
        : cal_record_estimate(cal, record);
}

static bool cal_drain(struct foc_board_calibration *cal)
{
    if (atomic_load_explicit(&cal->ring_overflow, memory_order_relaxed) != 0U) {
        return false;
    }
    unsigned int tail = atomic_load_explicit(&cal->ring_tail, memory_order_relaxed);
    const unsigned int head = atomic_load_explicit(&cal->ring_head, memory_order_acquire);
    if (head - tail > cal->ring_high_water) {
        cal->ring_high_water = (uint8_t)(head - tail);
    }
    bool ok = true;
    while (tail != head && ok) {
        const struct foc_board_cal_record *record = &cal->ring[tail % FOC_BOARD_CAL_RING];
        cal->probe_current_a = hypotf(record->current_a.d, record->current_a.q);
        cal->flux_angle = record->mechanical_rad;
        cal->flux_angle_us = record->sensor_us;
        ok = cal_record(cal, record);
        ++tail;
    }
    atomic_store_explicit(&cal->ring_tail, tail, memory_order_release);
    return ok;
}

/* Kt for CAL-7: this run's flux when CAL-4 ran, otherwise the profile's. */
static float cal_inertia_kt(const struct foc_board_calibration *cal)
{
    const float flux = cal->flux_result.state == FOC_CALIBRATION_STATE_VALID
        ? (float)cal->flux_result.flux_wb
        : cal->tuning.profile.flux_wb;
    return 1.5f * (float)cal->runtime->config.motion.rotor.pole_pairs * flux;
}

/* Rs for the CAL-7 voltage: this run's CAL-3 model, otherwise the profile's. */
static float cal_inertia_rs(const struct foc_board_calibration *cal)
{
    return cal->rl_model.rs_ohm > 0.0f ? cal->rl_model.rs_ohm : cal->tuning.profile.rs_ohm;
}

/* After the probe: the fit with the window the reversal period selects. */
static bool cal_inertia_capture(struct foc_board_calibration *cal, uint32_t window)
{
    const float kt = cal_inertia_kt(cal);
    const float current_limit = cal->runtime->pipeline.current.config.current_limit_a;
    const float speed_limit = cal->runtime->config.motion.position.velocity_limit_rad_s;
    const struct foc_mechanical_id_config config = {
        .setup_id = 1U,
        .torque_model_id = 1U,
        .torque_source = FOC_MECHANICAL_ID_TORQUE_OBSERVED_DQ,
        .load_source = FOC_MECHANICAL_ID_LOAD_NEGLIGIBLE,
        .min_interval_us = CAL_SLOT_US,
        .max_interval_us = CAL_SLOT_US,
        .intervals_per_window = window,
        .min_windows = CAL_INERTIA_WINDOWS,
        .torque_constant_nm_per_a = kt,
        /* Voltage drive at speed leaves some d current; the surface-magnet Kt x iq
         * approximation holds while it stays within the current limit. */
        .max_abs_id_a = current_limit,
        /* Filtered encoder speed: a window counts as accelerating or braking above
         * 0.01 rad/s of change, i.e. 1 rad/s² over 10 ms. */
        .speed_resolution_rad_s = 0.005f,
        .max_abs_speed_rad_s = 2.0f * speed_limit,
        .max_abs_torque_nm = 2.0f * kt * current_limit,
        .min_accel_rms_rad_s2 = 0.2f,
        .min_speed_rms_rad_s = 0.5f,
        .min_excitation = 0.2f,
        .max_residual_rms_nm = 0.1f * kt * current_limit,
        .min_inertia_kg_m2 = 1.0e-7f,
        .max_inertia_kg_m2 = 1.0f,
        .max_damping_nm_s = 10.0f,
    };
    cal->inertia_window = window;
    cal->inertia_reversals = 0U;
    cal_capture_begin(cal);
    return foc_mechanical_id_init(&cal->mech_id, &config, ++cal->capture_serial) ==
        FOC_MECHANICAL_ID_OK;
}

/* Start the drive and the probe around the present position. */
static bool cal_inertia_begin(struct foc_board_calibration *cal)
{
    const float current_limit = cal->runtime->pipeline.current.config.current_limit_a;
    /* The CAL-4 level: at most 60 % of the current limit at a reversal. */
    cal->inertia_voltage_v =
        fminf(0.15f * cal_voltage_limit(cal), 0.2f * current_limit * cal_inertia_rs(cal));
    struct foc_runtime_snapshot view;
    if (!(cal_inertia_kt(cal) > 0.0f) || !(cal->inertia_voltage_v > 0.0f) ||
        !(cal->tuning.profile.inertia_travel_rad > 0.0f) ||
        foc_runtime_read_snapshot(cal->runtime, &view) != FOC_RUNTIME_OK) {
        return false;
    }
    cal->inertia_origin_rad = view.mechanical_rad;
    cal->inertia_direction = 1.0f;
    cal->inertia_reversals = 0U;
    cal->inertia_probing = true;
    cal->inertia_reversal_us = 0U;
    cal->mech_result = (struct foc_mechanical_id_result){0};
    cal->capturing = false;
    return true;
}

/* CAL-7 drive and capture. */
static void cal_inertia(
    struct foc_board_calibration *cal,
    const struct foc_runtime_snapshot *view,
    uint32_t now_us
)
{
    if (!cal_drain(cal)) {
        cal->admission_failure =
            (enum foc_calibration_admission_failure)cal->mech_id.result.failure;
        cal_fail(
            cal,
            atomic_load(&cal->ring_overflow) != 0U ? FOC_BOARD_CAL_FAILURE_CAPTURE
                                                   : FOC_BOARD_CAL_FAILURE_INERTIA,
            now_us
        );
        return;
    }
    /* Reverse past half the travel, or near the speed limit: the reversal brakes at least
     * as hard as the start accelerated (back-EMF helps), so the rotor stops within it. */
    const float half_travel = 0.5f * cal->tuning.profile.inertia_travel_rad;
    const float offset = view->mechanical_rad - cal->inertia_origin_rad;
    const float speed_limit = 0.8f * cal->runtime->config.motion.position.velocity_limit_rad_s;
    if ((cal->inertia_direction > 0.0f &&
         (offset > half_travel || view->mechanical_rad_s > speed_limit)) ||
        (cal->inertia_direction < 0.0f &&
         (offset < -half_travel || view->mechanical_rad_s < -speed_limit))) {
        cal->inertia_direction = -cal->inertia_direction;
        cal->inertia_reversals++;
        if (cal->inertia_probing && cal->inertia_reversals == 1U) {
            cal->inertia_reversal_us = now_us;
        } else if (cal->inertia_probing && cal->inertia_reversals == CAL_INERTIA_PROBE_REVERSALS) {
            /* Two steady half periods after the first reversal; a tenth of one per window. */
            const uint32_t half_slots = (now_us - cal->inertia_reversal_us) /
                ((CAL_INERTIA_PROBE_REVERSALS - 1U) * CAL_SLOT_US);
            uint32_t window = half_slots / 10U;
            window = window < CAL_INERTIA_MIN_WINDOW ? CAL_INERTIA_MIN_WINDOW
                : window > CAL_INERTIA_MAX_WINDOW    ? CAL_INERTIA_MAX_WINDOW
                                                     : window;
            cal->inertia_probing = false;
            if (!cal_inertia_capture(cal, window)) {
                cal_fail(cal, FOC_BOARD_CAL_FAILURE_INERTIA, now_us);
                return;
            }
        }
    }
    cal_drive_dq(cal, 0.0f, cal->inertia_direction * cal->inertia_voltage_v, 0.0f, true, now_us);
    if (cal->inertia_probing) {
        if (now_us - cal->state_us > CAL_INERTIA_PROBE_US) {
            cal_fail(cal, FOC_BOARD_CAL_FAILURE_TIMEOUT, now_us);
        }
        return;
    }
    if (cal->capturing) {
        return;
    }
    atomic_store(&cal->record_active, 0U);
    foc_runtime_revoke_command(cal->runtime);
    if (foc_mechanical_id_finish(&cal->mech_id, &cal->mech_result) != FOC_MECHANICAL_ID_OK ||
        !cal->mech_result.usable) {
        cal->admission_failure = (enum foc_calibration_admission_failure)cal->mech_result.failure;
        cal_fail(cal, FOC_BOARD_CAL_FAILURE_INERTIA, now_us);
        return;
    }
    cal_enter(cal, FOC_BOARD_CAL_DRIVE_STOP, now_us);
}

/* CAL-3/CAL-4 drive and capture, one state per call. */
static void cal_electrical(struct foc_board_calibration *cal, uint32_t now_us)
{
    if (!cal_drain(cal)) {
        /* Detail byte: the estimator's own failure reason. */
        cal->admission_failure = (enum foc_calibration_admission_failure)(
            cal->state == FOC_BOARD_CAL_FLUX ? cal->flux_id.result.failure
                                             : cal->motor_id.result.failure
        );
        cal_fail(
            cal,
            atomic_load(&cal->ring_overflow) != 0U ? FOC_BOARD_CAL_FAILURE_CAPTURE
                : cal->state == FOC_BOARD_CAL_FLUX ? FOC_BOARD_CAL_FAILURE_FLUX
                                                   : FOC_BOARD_CAL_FAILURE_RL,
            now_us
        );
        return;
    }
    const uint32_t elapsed = now_us - cal->state_us;
    if (cal->probing) {
        /* No alignment in this boot: ramp the d axis on the rotor angle (no torque) to
         * the alignment current, then start CAL-3 with that voltage as its scale. */
        const float limit = cal_voltage_limit(cal);
        const float voltage = fminf(CAL_RAMP_V_PER_S * (float)elapsed * 1.0e-6f, limit);
        if (cal->probe_current_a >= CAL_ALIGN_CURRENT_A ||
            (voltage >= limit && cal->probe_current_a >= CAL_MIN_CURRENT_A)) {
            cal->align_voltage_v = voltage;
            cal->probing = false;
            if (!cal_rl_begin(cal, FOC_MOTOR_ID_D)) {
                cal_fail(cal, FOC_BOARD_CAL_FAILURE_RL, now_us);
                return;
            }
            cal->state_us = now_us;
        } else if (voltage >= limit) {
            cal_fail(cal, FOC_BOARD_CAL_FAILURE_CURRENT, now_us);
            return;
        }
        cal_drive_dq(cal, voltage, 0.0f, 0.0f, true, now_us);
        return;
    }
    if (cal->state == FOC_BOARD_CAL_RL_D || cal->state == FOC_BOARD_CAL_RL_Q) {
        /* d: a varied pulse train (no torque on the d axis). q: a zero-mean train, so the
         * free shaft only dithers about its start; its back-EMF is fitted, not assumed away. */
        static const float d_levels[8] = {1.0f, 1.0f, -0.75f, -0.75f, 0.33f, -1.0f, 0.65f, -0.26f};
        static const float q_levels[8] = {1.0f, -1.0f, -1.0f, 1.0f, 0.5f, -0.5f, -0.5f, 0.5f};
        const uint32_t level = (elapsed / (CAL_RL_LEVEL_SLOTS * CAL_SLOT_US)) % 8U;
        const float amplitude =
            fminf(fmaxf(1.6f * cal->align_voltage_v, 0.1f), 0.5f * cal_voltage_limit(cal));
        const bool d_axis = cal->state == FOC_BOARD_CAL_RL_D;
        const float voltage = amplitude * (d_axis ? d_levels[level] : q_levels[level]);
        cal_drive_dq(cal, d_axis ? voltage : 0.0f, d_axis ? 0.0f : voltage, 0.0f, true, now_us);
        if (cal->pushed >= CAL_RL_INTERVALS) {
            struct foc_motor_id_result *result = d_axis ? &cal->rl_d : &cal->rl_q;
            if (foc_motor_id_finish(&cal->motor_id, result) != FOC_MOTOR_ID_OK || !result->usable) {
                cal->admission_failure = (enum foc_calibration_admission_failure)result->failure;
                cal_fail(cal, FOC_BOARD_CAL_FAILURE_RL, now_us);
            } else if (d_axis) {
                if (cal_rl_begin(cal, FOC_MOTOR_ID_Q)) {
                    cal_enter(cal, FOC_BOARD_CAL_RL_Q, now_us);
                } else {
                    cal_fail(cal, FOC_BOARD_CAL_FAILURE_RL, now_us);
                }
            } else if (
                foc_motor_id_make_model(&cal->rl_d, &cal->rl_q, 0.25f, &cal->rl_model) !=
                    FOC_MOTOR_ID_OK ||
                !cal_flux_begin(cal)
            ) {
                cal_fail(cal, FOC_BOARD_CAL_FAILURE_RL, now_us);
            } else {
                cal_enter(cal, FOC_BOARD_CAL_FLUX, now_us);
            }
        }
        return;
    }
    /* CAL-4: four speed levels, two in each direction, voltage-mode FOC on the rotor angle.
     * Each level settles first; only its steady part is captured, as one segment.
     * Levels a, 2a, -a, -2a: the reversal from 2a to -a meets a back-EMF of about 2a, so the
     * current steps by about 3a / Rs. With the CAL-3 Rs, a keeps that step within 60% of
     * the current limit (a <= 0.2 x limit x Rs), and never above 0.15 x the voltage limit. */
    const float limit = cal_voltage_limit(cal);
    const float base = fminf(
        0.15f * limit,
        0.2f * cal->runtime->pipeline.current.config.current_limit_a * cal->rl_model.rs_ohm
    );
    const float levels[4] = {base, 2.0f * base, -base, -2.0f * base};
    cal_drive_dq(cal, 0.0f, levels[cal->level], 0.0f, true, now_us);
    if (!cal->capturing && cal->pushed >= CAL_FLUX_WINDOW * CAL_FLUX_WINDOWS) {
        /* Level complete: move on, or finish after the fourth. */
        cal->pushed = 0U;
        if (++cal->level < 4U) {
            cal->state_us = now_us;
            cal->flux_checks = 0U;
            cal->flux_samples = 0U;
            return;
        }
        if (foc_flux_id_finish(&cal->flux_id, &cal->flux_result) != FOC_CALIBRATION_FAILURE_NONE ||
            cal->flux_result.state != FOC_CALIBRATION_STATE_VALID) {
            cal->admission_failure =
                (enum foc_calibration_admission_failure)cal->flux_result.failure;
            cal_fail(cal, FOC_BOARD_CAL_FAILURE_FLUX, now_us);
            return;
        }
        if ((cal->stages & FOC_BOARD_CAL_STAGE_MECHANICAL) != 0U) {
            /* The drive keeps running: CAL-7 starts from this level's speed. */
            if (!cal_inertia_begin(cal)) {
                cal_fail(cal, FOC_BOARD_CAL_FAILURE_INERTIA, now_us);
                return;
            }
            cal_enter(cal, FOC_BOARD_CAL_INERTIA, now_us);
            return;
        }
        atomic_store(&cal->record_active, 0U);
        foc_runtime_revoke_command(cal->runtime);
        cal_enter(cal, FOC_BOARD_CAL_DRIVE_STOP, now_us);
    } else if (!cal->capturing && cal->pushed == 0U) {
        /* Settle: one check per interval; the first three only set the references. */
        if (elapsed > CAL_FLUX_SETTLE_MAX_US) {
            cal_fail(cal, FOC_BOARD_CAL_FAILURE_TIMEOUT, now_us);
            return;
        }
        const uint32_t check = elapsed / CAL_FLUX_STEADY_US;
        if (check <= cal->flux_checks) {
            return;
        }
        const uint32_t span_us = cal->flux_angle_us - cal->flux_check_us;
        const float speed = cal->flux_samples == 0U || span_us == 0U
            ? 0.0f
            : (cal->flux_angle - cal->flux_check_angle) * (1.0e6f / (float)span_us);
        const float d1 = cal->flux_check_speed[1] - cal->flux_check_speed[0];
        const float d2 = speed - cal->flux_check_speed[1];
        bool steady = false;
        const float pole_pairs = (float)cal->runtime->config.motion.rotor.pole_pairs;
        if (cal->flux_samples >= 3U && elapsed >= CAL_FLUX_SETTLE_US &&
            speed * levels[cal->level] > 0.0f &&
            fabsf(speed) * pole_pairs >= CAL_FLUX_MIN_SPEED_E_RAD_S) {
            if (fabsf(d2) <= CAL_FLUX_STEADY_FLOOR * fabsf(speed)) {
                steady = true;
            } else if (d1 * d2 > 0.0f && fabsf(d2) < fabsf(d1)) {
                const float ratio = d2 / d1;
                steady =
                    fabsf(d2 * ratio / (1.0f - ratio)) <= CAL_FLUX_STEADY_REMAINING * fabsf(speed);
            }
        }
        cal->flux_check_speed[0] = cal->flux_check_speed[1];
        cal->flux_check_speed[1] = speed;
        cal->flux_check_angle = cal->flux_angle;
        cal->flux_check_us = cal->flux_angle_us;
        cal->flux_checks = check;
        cal->flux_samples++;
        if (steady) {
            if (cal->level != 0U) {
                foc_flux_id_break(&cal->flux_id);
            }
            cal_capture_begin(cal);
        }
    }
}

/* CAL-5: design the current PI from the identified model and apply it through H0. */
static void cal_tune(struct foc_board_calibration *cal, uint32_t now_us)
{
    struct foc_runtime_tuning_result result;
    if (cal->tuning_sequence != 0U) {
        if (foc_runtime_read_tuning_result(cal->runtime, &result) != FOC_RUNTIME_OK ||
            result.request_sequence != cal->tuning_sequence ||
            result.result == FOC_RUNTIME_REQUEST_PENDING) {
            if (now_us - cal->state_us > CAL_TUNE_TIMEOUT_US) {
                cal_fail(cal, FOC_BOARD_CAL_FAILURE_TUNE, now_us);
            }
            return;
        }
        if (result.result != FOC_RUNTIME_REQUEST_APPLIED) {
            cal_fail(cal, FOC_BOARD_CAL_FAILURE_TUNE, now_us);
            return;
        }
        if (foc_runtime_read_tuning(cal->runtime, &cal->tuning) != FOC_RUNTIME_OK) {
            cal_fail(cal, FOC_BOARD_CAL_FAILURE_TUNE, now_us);
            return;
        }
        cal->tuning_sequence = 0U;
        cal->committed |=
            (uint8_t)(cal->stages &
                      (FOC_BOARD_CAL_STAGE_ELECTRICAL |
                       (cal->mech_result.usable ? FOC_BOARD_CAL_STAGE_MECHANICAL : 0U)));
        cal_enter(cal, FOC_BOARD_CAL_RESTORE, now_us);
        return;
    }
    /* The identified model joins the profile; the unified refresh designs the PI at
     * the profile bandwidth (ID 104), and H0 applies model and gains together. */
    struct foc_runtime_tuning tuning;
    if (foc_runtime_read_tuning(cal->runtime, &tuning) != FOC_RUNTIME_OK) {
        cal_fail(cal, FOC_BOARD_CAL_FAILURE_TUNE, now_us);
        return;
    }
    const bool electrical = (cal->stages & FOC_BOARD_CAL_STAGE_ELECTRICAL) != 0U;
    if (electrical) {
        tuning.profile.rs_ohm = cal->rl_model.rs_ohm;
        tuning.profile.ld_h = cal->rl_model.ld_h;
        tuning.profile.lq_h = cal->rl_model.lq_h;
        if (cal->flux_result.state == FOC_CALIBRATION_STATE_VALID) {
            tuning.profile.flux_wb = cal->flux_result.flux_wb;
        }
    }
    if (cal->mech_result.usable) {
        tuning.profile.inertia_kg_m2 = cal->mech_result.inertia_kg_m2;
        tuning.profile.damping_nm_s_per_rad = cal->mech_result.damping_nm_s;
    }
    if (!foc_board_profile_refresh(
            cal->runtime->config.h0.period_us,
            cal_voltage_limit(cal),
            cal->runtime->config.motion.rotor.pole_pairs,
            &tuning,
            electrical,
            true,
            electrical ? &cal->pi : NULL
        ) ||
        !foc_board_profile_model_complete(&tuning.profile)) {
        cal_fail(cal, FOC_BOARD_CAL_FAILURE_TUNE, now_us);
        return;
    }
    const uint32_t sequence = cal->serial->tuning_sequence + 1U;
    if (foc_runtime_publish_tuning(cal->runtime, &tuning, sequence) != FOC_RUNTIME_OK) {
        cal_fail(cal, FOC_BOARD_CAL_FAILURE_TUNE, now_us);
        return;
    }
    cal->serial->tuning_sequence = sequence;
    cal->tuning_sequence = sequence;
    cal->state_us = now_us;
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
    atomic_init(&calibration->record_active, 0U);
    atomic_init(&calibration->ring_head, 0U);
    atomic_init(&calibration->ring_tail, 0U);
    atomic_init(&calibration->ring_overflow, 0U);
    adc->tap_argument = calibration;
    adc->tap = cal_adc_tap;
    adc->observer_argument = calibration;
    adc->observer = cal_h0_observer;
}

bool foc_board_calibration_busy(const struct foc_board_calibration *calibration)
{
    return calibration->state != FOC_BOARD_CAL_IDLE && calibration->state != FOC_BOARD_CAL_DONE &&
        calibration->state != FOC_BOARD_CAL_FAILED;
}

int foc_board_calibration_start(
    struct foc_board_calibration *cal,
    uint32_t now_us,
    unsigned int stages
)
{
    const bool zero_only = stages == FOC_BOARD_CAL_STAGE_ZERO;
    const unsigned int drive_stages = stages & ~(unsigned int)FOC_BOARD_CAL_STAGE_MECHANICAL;
    const bool mechanical = (stages & FOC_BOARD_CAL_STAGE_MECHANICAL) != 0U;
    if (!zero_only &&
        (drive_stages > FOC_BOARD_CAL_STAGE_ALL || (drive_stages == 0U && !mechanical))) {
        return FOC_PROTO_INVALID_ARGUMENT;
    }
    if (!zero_only && cal->serial->target_policy != FOC_SERIAL_TARGET_VIRTUAL_ONLY &&
        cal->output->backend == NULL) {
        return FOC_PROTO_OUTPUT_LOCKED; /* CAL-2 must drive the motor. */
    }
    if (zero_only && !cal_commutation_valid(cal)) {
        return FOC_PROTO_BAD_STATE; /* The zero applies to a valid group. */
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
    cal->stages = stages;
    cal->tuning_sequence = 0U;
    cal->rl_d = (struct foc_motor_id_result){0};
    cal->rl_q = (struct foc_motor_id_result){0};
    cal->rl_model = (struct foc_motor_rl_model){0};
    cal->flux_result = (struct foc_flux_id_result){0};
    cal->pi = (struct foc_motor_pi_candidate){0};
    cal->mech_result = (struct foc_mechanical_id_result){0};
    /* CAL-7 without CAL-3/4 takes Rs and Kt from the profile; TUNE needs the full model. */
    if (mechanical && (stages & FOC_BOARD_CAL_STAGE_ELECTRICAL) == 0U &&
        (!foc_board_profile_model_complete(&cal->tuning.profile) ||
         !(cal->tuning.profile.flux_wb > 0.0f))) {
        return FOC_PROTO_BAD_STATE;
    }
    atomic_store(&cal->ring_overflow, 0U);
    atomic_store(&cal->ring_tail, atomic_load(&cal->ring_head));
    cal->committed = 0U;
    cal->ring_high_water = 0U;
    cal->mode_claimed = false;
    cal->failing = false;
    if ((stages & FOC_BOARD_CAL_STAGE_ROTOR) == 0U && !zero_only) {
        /* The electrical stages commutate with the rotor mapping applied in this boot. */
        if (!cal->snapshot.accepted) {
            return FOC_PROTO_BAD_STATE;
        }
        cal->electrical_phase = true;
        cal_enter(cal, FOC_BOARD_CAL_DRIVE_MODE, now_us);
        return FOC_PROTO_ACCEPTED;
    }
    cal->electrical_phase = false;
    atomic_store(&cal->adc_samples, 0U);
    cal_enter(cal, FOC_BOARD_CAL_ADC_ZERO, now_us);
    atomic_store_explicit(&cal->adc_active, 1U, memory_order_release);
    return FOC_PROTO_ACCEPTED;
}

void foc_board_calibration_adopt(
    struct foc_board_calibration *calibration,
    const struct foc_calibration_snapshot *snapshot
)
{
    if (calibration != NULL && snapshot != NULL && snapshot->accepted &&
        !foc_board_calibration_busy(calibration)) {
        calibration->snapshot = *snapshot;
        calibration->align_voltage_v = 0.0f;
        calibration->active_source = FOC_BOARD_CAL_SOURCE_STORAGE;
    }
}

void foc_board_calibration_capture_boot(
    struct foc_board_calibration *calibration,
    const struct foc_board_boot *boot
)
{
    if (calibration == NULL || boot == NULL ||
        foc_runtime_read_tuning(calibration->runtime, &calibration->boot_tuning) !=
            FOC_RUNTIME_OK) {
        return;
    }
    calibration->boot_calibrated =
        boot->configuration == FOC_BOARD_CALIBRATED_CONFIGURATION && boot->snapshot.accepted;
    if (calibration->boot_calibrated) {
        calibration->boot_snapshot = boot->snapshot;
        calibration->snapshot = boot->snapshot;
        calibration->active_source = FOC_BOARD_CAL_SOURCE_SUPPLY;
    }
}

int foc_board_calibration_commutation(
    struct foc_board_calibration *cal,
    uint32_t now_us,
    uint16_t pole_pairs,
    int8_t direction,
    float offset_rad
)
{
    if (foc_board_calibration_busy(cal)) {
        return FOC_PROTO_BUSY;
    }
    struct foc_runtime_snapshot view;
    struct foc_runtime_tuning_result tuning_result;
    struct foc_runtime_tuning tuning;
    if (foc_runtime_read_snapshot(cal->runtime, &view) != FOC_RUNTIME_OK ||
        foc_runtime_read_tuning_result(cal->runtime, &tuning_result) != FOC_RUNTIME_OK ||
        tuning_result.result == FOC_RUNTIME_REQUEST_PENDING) {
        return FOC_PROTO_BUSY;
    }
    if (!cal_quiet(&view)) {
        return FOC_PROTO_BAD_STATE;
    }
    if (pole_pairs == 0U || pole_pairs > FOC_BOARD_POLE_PAIRS_MAX ||
        (direction != 1 && direction != -1) || !isfinite(offset_rad) || offset_rad < 0.0f ||
        offset_rad >= CAL_TWO_PI ||
        foc_runtime_read_tuning(cal->runtime, &tuning) != FOC_RUNTIME_OK) {
        return FOC_PROTO_INVALID_ARGUMENT;
    }
    /* The group joins the ADC zero in effect; the Runtime direction is the raw source
     * direction times the calibration direction. */
    const struct foc_device_adc_config *adc = &cal->runtime->pipeline.adc_config;
    struct foc_calibration_snapshot snapshot = {
        .phase_a_offset_count = adc->phase_a_offset_count,
        .phase_b_offset_count = adc->phase_b_offset_count,
        .phase_c_offset_count = adc->phase_c_offset_count,
        .runtime_encoder_direction = direction,
        .pole_pairs = pole_pairs,
        .electrical_offset_rad = offset_rad,
        .accepted = true,
    };
    foc_board_calibration_identity(pole_pairs, &snapshot.source_identity);
    snapshot.calibration_direction = direction * snapshot.source_identity.encoder_direction;
    if (!foc_calibration_snapshot_valid(&snapshot)) {
        return FOC_PROTO_INVALID_ARGUMENT;
    }
    if (cal_reconfigure(cal, &snapshot, &tuning, now_us) != FOC_RUNTIME_OK) {
        return FOC_PROTO_INVALID_ARGUMENT;
    }
    cal->snapshot = snapshot;
    cal->align_voltage_v = 0.0f;
    cal->active_source = FOC_BOARD_CAL_SOURCE_HOST;
    cal->zero_settling = true;
    cal->zero_settle_us = now_us;
    return FOC_PROTO_APPLIED;
}

int foc_board_calibration_reset(struct foc_board_calibration *cal, uint32_t now_us)
{
    if (foc_board_calibration_busy(cal)) {
        return FOC_PROTO_BUSY;
    }
    struct foc_runtime_snapshot view;
    if (foc_runtime_read_snapshot(cal->runtime, &view) != FOC_RUNTIME_OK) {
        return FOC_PROTO_BUSY;
    }
    if (!cal_quiet(&view)) {
        return FOC_PROTO_BAD_STATE;
    }
    const struct foc_calibration_snapshot *boot = cal->boot_calibrated ? &cal->boot_snapshot : NULL;
    if (cal_reconfigure(cal, boot, &cal->boot_tuning, now_us) != FOC_RUNTIME_OK) {
        return FOC_PROTO_INTERNAL_ERROR;
    }
    cal->serial->rx_budget = FOC_SERIAL_RX_DEFAULT;
    cal->serial->current_limit_a = cal->boot_tuning.iq_limit_a;
    cal->serial->position_limit_rad = 100.0f;
    cal->serial->output_zero_rad = 0.0f;
    cal->snapshot = boot != NULL ? *boot : (struct foc_calibration_snapshot){0};
    cal->active_source = boot != NULL ? FOC_BOARD_CAL_SOURCE_SUPPLY : FOC_BOARD_CAL_SOURCE_BOOT;
    cal->align_voltage_v = 0.0f;
    cal->zero_ready = false;
    cal->zero_refused = false;
    cal->zero_attempted = false;
    cal->zero_settling = true;
    cal->zero_settle_us = now_us;
    return FOC_PROTO_APPLIED;
}

int foc_board_calibration_gate(const struct foc_board_calibration *cal)
{
    if (!cal_commutation_valid(cal)) {
        return FOC_PROTO_NOT_CALIBRATED;
    }
    if (!cal->zero_ready || foc_board_calibration_busy(cal)) {
        return FOC_PROTO_BUSY;
    }
    return FOC_PROTO_OK;
}

void foc_board_calibration_abort(struct foc_board_calibration *calibration)
{
    if (foc_board_calibration_busy(calibration) && !calibration->failing) {
        calibration->failure = FOC_BOARD_CAL_FAILURE_ABORTED;
    }
}

void foc_board_calibration_poll(struct foc_board_calibration *cal, uint32_t now_us)
{
    if (!foc_board_calibration_busy(cal)) {
        cal_auto_zero(cal, now_us);
        return;
    }
    const float dt_s = (float)(now_us - cal->last_us) * 1.0e-6f;
    cal->last_us = now_us;
    struct foc_runtime_snapshot view;
    if (foc_runtime_read_snapshot(cal->runtime, &view) != FOC_RUNTIME_OK) {
        return;
    }
    if (cal->failure == FOC_BOARD_CAL_FAILURE_ABORTED && !cal->failing) {
        cal_fail(cal, FOC_BOARD_CAL_FAILURE_ABORTED, now_us);
        return;
    }
    const bool driving = (cal->state >= FOC_BOARD_CAL_ALIGN && cal->state <= FOC_BOARD_CAL_SWEEP) ||
        (cal->state >= FOC_BOARD_CAL_RL_D && cal->state <= FOC_BOARD_CAL_FLUX) ||
        cal->state == FOC_BOARD_CAL_INERTIA;
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
            /* Refuse a noisy zero before driving the motor; admission checks it again. */
            if (foc_calibration_adc_finalize(&cal->adc_estimator, &cal->adc_result) != CAL_OK ||
                !cal->adc_result.usable ||
                !(fmaxf(
                      fmaxf(cal->adc_result.noise_rms_a_counts, cal->adc_result.noise_rms_b_counts),
                      cal->adc_result.noise_rms_c_counts
                  ) <= CAL_ADC_MAX_NOISE_COUNTS)) {
                cal_fail(cal, FOC_BOARD_CAL_FAILURE_ADC, now_us);
            } else if (cal->stages == FOC_BOARD_CAL_STAGE_ZERO) {
                cal_zero_apply(cal, now_us);
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
        if (cal->state == FOC_BOARD_CAL_DRIVE_ARM) {
            cal->mode_claimed = true;
        }
        break;
    case FOC_BOARD_CAL_DRIVE_ARM:
        if (cal->electrical_phase) {
            cal_drive_dq(cal, 0.0f, 0.0f, 0.0f, true, now_us);
        } else {
            cal_drive(cal, now_us);
        }
        cal_step_lifecycle(
            cal,
            FOC_RUNTIME_ACTION_ARM,
            FOC_RUNTIME_MODE_CALIBRATION,
            FOC_BOARD_CAL_DRIVE_START,
            now_us
        );
        break;
    case FOC_BOARD_CAL_DRIVE_START:
        if (!cal->electrical_phase) {
            cal_drive(cal, now_us);
        }
        if (cal->electrical_phase) {
            cal_drive_dq(cal, 0.0f, 0.0f, 0.0f, true, now_us);
        }
        cal_step_lifecycle(
            cal,
            FOC_RUNTIME_ACTION_START,
            FOC_RUNTIME_MODE_CALIBRATION,
            !cal->electrical_phase                               ? FOC_BOARD_CAL_ALIGN
                : (cal->stages & FOC_BOARD_CAL_STAGE_ELECTRICAL) ? FOC_BOARD_CAL_RL_D
                                                                 : FOC_BOARD_CAL_INERTIA,
            now_us
        );
        if (cal->state == FOC_BOARD_CAL_INERTIA) {
            atomic_store(&cal->ring_tail, atomic_load(&cal->ring_head));
            atomic_store(&cal->record_active, 1U);
            if (!cal_inertia_begin(cal)) {
                cal_fail(cal, FOC_BOARD_CAL_FAILURE_INERTIA, now_us);
            }
        }
        if (cal->state == FOC_BOARD_CAL_RL_D) {
            atomic_store(&cal->ring_tail, atomic_load(&cal->ring_head));
            atomic_store(&cal->record_active, 1U);
            cal->probe_current_a = 0.0f;
            cal->probing = !(cal->align_voltage_v > 0.0f);
            if (cal->probing) {
                cal->capturing = false;
            } else if (!cal_rl_begin(cal, FOC_MOTOR_ID_D)) {
                cal_fail(cal, FOC_BOARD_CAL_FAILURE_RL, now_us);
            }
        }
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
            cal->align_voltage_v = cal->voltage_v;
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
            cal_enter(
                cal,
                cal->electrical_phase ? FOC_BOARD_CAL_TUNE : FOC_BOARD_CAL_APPLY,
                now_us
            );
        } else if (now_us - cal->state_us > CAL_STOP_TIMEOUT_US) {
            cal_fail(cal, FOC_BOARD_CAL_FAILURE_TIMEOUT, now_us);
        }
        break;
    case FOC_BOARD_CAL_APPLY:
        cal_apply(cal, now_us);
        break;
    case FOC_BOARD_CAL_RL_D:
    case FOC_BOARD_CAL_RL_Q:
    case FOC_BOARD_CAL_FLUX:
        cal_electrical(cal, now_us);
        break;
    case FOC_BOARD_CAL_TUNE:
        cal_tune(cal, now_us);
        break;
    case FOC_BOARD_CAL_INERTIA:
        cal_inertia(cal, &view, now_us);
        break;
    case FOC_BOARD_CAL_RESTORE:
        if (cal->failing && !cal_quiet(&view)) {
            if (now_us - cal->state_us > CAL_FAIL_QUIET_US) {
                cal_enter(cal, FOC_BOARD_CAL_FAILED, now_us);
            }
            break;
        }
        cal_step_lifecycle(
            cal,
            FOC_RUNTIME_ACTION_SET_MODE,
            FOC_RUNTIME_MODE_CURRENT,
            cal->failing ? FOC_BOARD_CAL_FAILED : FOC_BOARD_CAL_DONE,
            now_us
        );
        if (cal->state != FOC_BOARD_CAL_RESTORE) {
            cal->mode_claimed = false;
            cal->failing = false;
        }
        break;
    default:
        break;
    }
}

/* UART adapter callbacks -------------------------------------------------------- */

static int cal_serial_start(void *context, uint32_t now_us, unsigned int stages)
{
    return foc_board_calibration_start(context, now_us, stages);
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
        .rs_ohm = cal->rl_model.rs_ohm,
        .ld_h = cal->rl_model.ld_h,
        .lq_h = cal->rl_model.lq_h,
        .flux_wb = cal->flux_result.flux_wb,
        .kt_nm_per_a = cal->flux_result.kt_nm_per_peak_q_amp,
        .kp_d_v_per_a = cal->pi.kp_d_v_per_a,
        .kp_q_v_per_a = cal->pi.kp_q_v_per_a,
        .ki_v_per_a_s = cal->pi.ki_v_per_a_s,
        .committed_stages = cal->committed,
        .result_bits = (uint8_t)((cal->offset_result.usable ? FOC_BOARD_CAL_RESULT_ROTOR : 0U) |
                                 (cal->rl_model.rs_ohm > 0.0f ? FOC_BOARD_CAL_RESULT_RL : 0U) |
                                 (cal->flux_result.state == FOC_CALIBRATION_STATE_VALID
                                      ? FOC_BOARD_CAL_RESULT_FLUX
                                      : 0U) |
                                 ((cal->committed & FOC_BOARD_CAL_STAGE_ELECTRICAL) != 0U
                                      ? FOC_BOARD_CAL_RESULT_PI
                                      : 0U) |
                                 (cal->mech_result.usable ? FOC_BOARD_CAL_RESULT_INERTIA : 0U)),
        .active_source = cal->active_source,
        .ring_high_water = cal->ring_high_water,
        .active_pole_pairs = (uint16_t)cal->runtime->config.motion.rotor.pole_pairs,
        .active_electrical_offset_rad = cal->runtime->config.motion.rotor.electrical_offset_rad,
        .active_adc_offset_count = {
            cal->runtime->pipeline.adc_config.phase_a_offset_count,
            cal->runtime->pipeline.adc_config.phase_b_offset_count,
            cal->runtime->pipeline.adc_config.phase_c_offset_count
        },
    };
    return true;
}

static int cal_serial_commutation(
    void *context,
    uint32_t now_us,
    uint16_t pole_pairs,
    int8_t direction,
    float offset_rad
)
{
    return foc_board_calibration_commutation(context, now_us, pole_pairs, direction, offset_rad);
}

static int cal_serial_trim(void *context, uint32_t now_us, float offset_rad)
{
    struct foc_board_calibration *cal = context;
    if (!cal_commutation_valid(cal)) {
        return FOC_PROTO_BAD_STATE;
    }
    return foc_board_calibration_commutation(
        cal,
        now_us,
        cal->snapshot.pole_pairs,
        (int8_t)cal->snapshot.runtime_encoder_direction,
        offset_rad
    );
}

static int cal_serial_gate(const void *context)
{
    return foc_board_calibration_gate(context);
}

static bool cal_serial_commission(const void *context, struct foc_serial_commission_view *view)
{
    const struct foc_board_calibration *cal = context;
    *view = (struct foc_serial_commission_view){
        .valid = cal_commutation_valid(cal) ? 1U : 0U,
        .source = cal->active_source,
        .zero_state = cal->zero_ready ? FOC_BOARD_CAL_ZERO_READY
            : cal->zero_refused       ? FOC_BOARD_CAL_ZERO_REFUSED
                                      : FOC_BOARD_CAL_ZERO_PENDING,
    };
    return true;
}

static int cal_serial_reset(void *context, uint32_t now_us)
{
    return foc_board_calibration_reset(context, now_us);
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
        .commutation = cal_serial_commutation,
        .trim = cal_serial_trim,
        .gate = cal_serial_gate,
        .commission = cal_serial_commission,
        .reset = cal_serial_reset,
    };
}

/* ---------------------------------------- end of file ----------------------------------------- */
