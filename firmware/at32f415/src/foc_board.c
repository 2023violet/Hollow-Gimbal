/**
 * @file    foc_board.c
 * @brief   Reference Board1 configuration and portable peripheral boundary.
 * @version v0.1.0
 * @date    2026.11.10
 *
 * Implementation role
 * -------------------
 * Keep nominal scaling, timing and phase conventions local to Board1.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_board.h"
#if defined(FALCONFOC_BOARD_CAPTURE)
#include "foc_board_capture.h"
#endif

#include <stddef.h>

/* Private Macros --------------------------------------------------------------- */

#define FOC_BOARD_ADC_MAX          4095U
#define FOC_BOARD_CLOCK_HALF       0x80000000U
#define FOC_BOARD_TICKS_PER_US     (FOC_BOARD_TIMER_HZ / FOC_BOARD_US_HZ)
#define FOC_BOARD_ENCODER_FRAME_US 100U
#define FOC_BOARD_ENCODER_GAP_US   2500U

#if defined(FALCONFOC_RT02_TIMING_TRACE)
static bool foc_board_trace_submit(void *argument, const struct foc_device_pwm_request *request)
{
    struct foc_board_output *output = argument;
    if (output == NULL || output->trace_owner == NULL) {
        return foc_board_output_submit(argument, request);
    }
    struct foc_board_adc *adc = output->trace_owner;
    struct foc_board_adc_trace *trace = &adc->live.transaction.trace;
    const uint32_t started = adc->trace_cycles();
    const bool accepted = foc_board_output_submit(argument, request);
    const uint32_t returned = adc->trace_cycles();
    trace->adapter_start_cycles = started;
    trace->adapter_return_cycles = returned;
    trace->adapter_result = output->result;
    trace->adapter_accepted = accepted;
    trace->points |= FOC_BOARD_TRACE_ADAPTER;
    if (request != NULL && request->action == FOC_DEVICE_OUTPUT_UPDATE) {
        trace->update_start_cycles = started;
        trace->update_return_cycles = returned;
        trace->update_result = output->result;
        trace->update_accepted = accepted;
        trace->points |= FOC_BOARD_TRACE_UPDATE;
    }
    return accepted;
}
#endif

/* Private Constants ------------------------------------------------------------ */

/*
 * Board1 nominal control configuration. Physical conversion factors and loop
 * gains are kept here as the current firmware defaults; they are not a claim
 * that the connected motor has been identified. Units are documented by the
 * owning public structures in core/, g3/, and runtime/.
 */
static const struct foc_pipeline_config pipeline_config = {
    .current =
        {
            .pwm =
                {
                    .vbus_min_v = 1.0f,
                    .voltage_limit_v = 2.4f,
                    .duty_min = 0.0f,
                    .duty_max = 1.0f,
                    .max_dt_s = 0.0006f,
                },
            .kp_v_per_a = {.d = 1.08f, .q = 1.35f},
            .ki_v_per_a_s = {.d = 540.0f, .q = 540.0f},
            .current_limit_a = 2.5f,
        },
    .adc =
        {
            /* 3.3 V ADC full scale and an 11:1 VBUS divider, in V/count. */
            .vbus_v_per_count = 3.3f * 11.0f / (float)FOC_BOARD_ADC_MAX,
            /* 0.5 V/A current amplifier scale, in A/count. */
            .current_a_per_count = 3.3f / ((float)FOC_BOARD_ADC_MAX * 0.5f),
            .vbus_min_v = 1.0f,
            .phase_a_offset_count = 2048.0f,
            .phase_b_offset_count = 2048.0f,
            .phase_c_offset_count = 2048.0f,
            .phase_order = FOC_DEVICE_PHASE_ORDER_ABC,
        },
    .pwm =
        {
            .period_counts = FOC_BOARD_COMPARE_MAX,
            .phase_order = FOC_DEVICE_PHASE_ORDER_ABC,
        },
    /* The first usable velocity follows the second encoder frame. */
    .safety =
        {
            .command_timeout_us = 2500U,
            .sample_timeout_us = 500U,
            .fast_heartbeat_timeout_us = 700U,
            .foreground_heartbeat_timeout_us = 1000U,
            .invalid_sample_latch_after = 5U,
        },
#if defined(FALCONFOC_RT02_TIMING_TRACE)
    .output_submit = foc_board_trace_submit,
#else
    .output_submit = foc_board_output_submit,
#endif
};

/* H0/H1 and foreground values are nominal Board1 scheduling budgets in us. */
static const struct foc_runtime_config runtime_config = {
    .h0 = {.period_us = FOC_BOARD_H0_US, .max_delay_us = 150U, .budget_us = 100U},
    .h1 = {.period_us = FOC_BOARD_H1_US, .max_delay_us = 150U, .budget_us = 150U},
    .supervision = {.period_us = 200U, .max_delay_us = 150U, .budget_us = 100U},
    .telemetry = {.period_us = 10000U, .max_delay_us = 500U, .budget_us = 100U},
    .foreground_budget_us = 150U,
    .angle_timeout_us = 650U,
    .motion =
        {
            .enabled = true,
            .rotor = {.pole_pairs = 7U, .electrical_offset_rad = 0.0f},
            .prediction =
                {
                    .max_prediction_us = 650U,
                    .max_accel_rad_s2 = 2000.0f,
                    .max_error_rad = 0.00065f,
                },
            .velocity = {.kp_a_per_rad_s = 0.18f, .ki_a_per_rad = 1.4f, .max_dt_s = 0.002f},
            .position = {.kp_per_s = 4.0f, .velocity_limit_rad_s = 12.0f},
            .iq_limit_a = 2.5f,
            .trajectory = {.enabled = false},
        },
    /*
     * Motor protection (contract section 12.4). 3.0 A lies between the 2.5 A control
     * limit and the ±3.3 A current-sense span; 22 V lies between the 20 V VM input
     * rating of the schematic and the 24 V DRV8311 absolute maximum; 5.0 V is just
     * below the 5.25 V input minimum. Over-speed 0 follows 1.25 x the speed limit.
     * All values await confirmation on the target board.
     */
    .trip_enabled = true,
    .trips =
        {
            .overcurrent_a = 3.0f,
            .overvoltage_v = 22.0f,
            .undervoltage_v = 5.0f,
            .overspeed_rad_s = 0.0f,
            .sustained_ms = 1000U,
            .stall_ms = 200U,
            .divergence_ms = 300U,
        },
    /* No motor is identified at boot: the model and the user profile are unset. The
     * 150 Hz bandwidth is the value CAL-5 has designed with since M3b. */
    .profile = {.gear_ratio = 1.0f, .current_bandwidth_hz = 150.0f},
};

static const struct foc_encoder_config encoder_config = {
    .counts_per_revolution = 65536U,
    .direction = FOC_ENCODER_DIRECTION_REVERSED,
    .max_speed_rad_s = 1000.0f,
    .min_dt_us = 1U,
    .max_dt_us = FOC_BOARD_ENCODER_GAP_US,
    .speed_lpf_cutoff_hz = 80.0f,
    .tracker_alpha = 0.85f,
    .tracker_beta = 0.2f,
};

/* Private Helpers -------------------------------------------------------------- */

static int foc_board_encoder_init_sensor(struct foc_board_encoder *encoder)
{
    if (encoder->sensor.initialized) {
        return FOC_ENCODER_STATUS_OK;
    }
    const struct foc_encoder_config *config =
        encoder->config_valid ? &encoder->config : &encoder_config;
    return foc_encoder_init(&encoder->sensor, config);
}

static enum foc_encoder_transport foc_board_encoder_transport(enum foc_board_encoder_result result)
{
    if (result == FOC_BOARD_ENCODER_TIMEOUT) {
        return FOC_ENCODER_TRANSPORT_TIMEOUT;
    }
    if (result != FOC_BOARD_ENCODER_OK) {
        return FOC_ENCODER_TRANSPORT_FRAME_ERROR;
    }
    return FOC_ENCODER_TRANSPORT_OK;
}

static enum foc_board_encoder_result foc_board_encoder_quality(enum foc_encoder_quality quality)
{
    switch (quality) {
    case FOC_ENCODER_QUALITY_TIMEOUT:
        return FOC_BOARD_ENCODER_TIMEOUT;
    case FOC_ENCODER_QUALITY_TRANSPORT_ERROR:
    case FOC_ENCODER_QUALITY_INVALID_SAMPLE:
    case FOC_ENCODER_QUALITY_NUMERIC_ERROR:
        return FOC_BOARD_ENCODER_FRAME;
    case FOC_ENCODER_QUALITY_DISCONTINUITY:
    case FOC_ENCODER_QUALITY_SPEED_OUT_OF_RANGE:
    case FOC_ENCODER_QUALITY_DUPLICATE:
    case FOC_ENCODER_QUALITY_TIME_ORDER_ERROR:
    case FOC_ENCODER_QUALITY_INVALID_INTERVAL:
        return FOC_BOARD_ENCODER_DISCONTINUITY;
    default:
        return FOC_BOARD_ENCODER_OK;
    }
}

static bool foc_board_compares_zero(const struct foc_device_pwm_request *request)
{
    return request->duty_a == 0.0f && request->duty_b == 0.0f && request->duty_c == 0.0f &&
        request->channel1_compare == 0U && request->channel2_compare == 0U &&
        request->channel3_compare == 0U;
}

/* Public Functions ------------------------------------------------------------- */

#if defined(FALCONFOC_BOARD_CAPTURE)
_Static_assert(FOC_CAPTURE_BUILD_TAG <= UINT32_MAX, "recorder build tag is uint32");

struct foc_capture_profile foc_board_capture_default_profile(void)
{
    _Static_assert((uint64_t)FOC_CAPTURE_BUILD_TAG <= UINT32_MAX, "capture build tag is uint32");
    return (struct foc_capture_profile){
        .build_tag = FOC_CAPTURE_BUILD_TAG,
        .encoder_cpr = encoder_config.counts_per_revolution,
        .encoder_direction = encoder_config.direction,
        .pole_pairs = runtime_config.motion.rotor.pole_pairs,
        .phase_order = pipeline_config.adc.phase_order,
        .configuration = FOC_CAPTURE_CONFIGURATION_NOMINAL,
    };
}
#endif

void foc_board_calibration_identity(uint16_t pole_pairs, struct foc_calibration_identity *identity)
{
    if (identity == NULL) {
        return;
    }
    *identity = (struct foc_calibration_identity){
        .adc_full_scale_counts = FOC_BOARD_ADC_MAX,
        .current_a_per_count = pipeline_config.adc.current_a_per_count,
        .encoder_counts_per_revolution = encoder_config.counts_per_revolution,
        .encoder_direction = encoder_config.direction,
        .phase_order = pipeline_config.adc.phase_order,
        .pole_pairs = pole_pairs,
    };
}

void foc_board_encoder_policy(
    const struct foc_board_encoder *encoder,
    struct foc_encoder_config *config
)
{
    if (config == NULL) {
        return;
    }
    *config = encoder != NULL && encoder->config_valid ? encoder->config : encoder_config;
}

int foc_board_init(struct foc_runtime *runtime, struct foc_board_output *output, uint32_t now_us)
{
    if (output == NULL) {
        return FOC_RUNTIME_ERR_INPUT;
    }
    *output = (struct foc_board_output){0};
    struct foc_pipeline_config config = pipeline_config;
    config.output_arg = output;
    return foc_runtime_init(runtime, &config, &runtime_config, now_us);
}

int foc_board_init_with_calibration(
    struct foc_runtime *runtime,
    struct foc_board_output *output,
    struct foc_board_encoder *encoder,
    const struct foc_calibration_snapshot *snapshot,
    uint32_t now_us
)
{
    /* Resolve the Sensor direction once: raw source frame times CAL-01 rotor sign. */
    if (runtime == NULL || output == NULL || encoder == NULL ||
        !foc_calibration_snapshot_valid(snapshot) || encoder->sensor.initialized ||
        encoder->config_valid ||
        snapshot->runtime_encoder_direction !=
            snapshot->source_identity.encoder_direction * snapshot->calibration_direction ||
        !foc_calibration_identity_matches(
            &snapshot->source_identity,
            &(struct foc_calibration_identity){
                .adc_full_scale_counts = FOC_BOARD_ADC_MAX,
                .current_a_per_count = pipeline_config.adc.current_a_per_count,
                .encoder_counts_per_revolution = encoder_config.counts_per_revolution,
                .encoder_direction = encoder_config.direction,
                .phase_order = pipeline_config.adc.phase_order,
                .pole_pairs = snapshot->pole_pairs,
            }
        )) {
        return FOC_RUNTIME_ERR_CONFIG;
    }

    struct foc_pipeline_config candidate_pipeline = pipeline_config;
    candidate_pipeline.adc.phase_a_offset_count = snapshot->phase_a_offset_count;
    candidate_pipeline.adc.phase_b_offset_count = snapshot->phase_b_offset_count;
    candidate_pipeline.adc.phase_c_offset_count = snapshot->phase_c_offset_count;
    struct foc_runtime_config candidate_runtime_config = runtime_config;
    candidate_runtime_config.motion.rotor.pole_pairs = snapshot->pole_pairs;
    candidate_runtime_config.motion.rotor.electrical_offset_rad = snapshot->electrical_offset_rad;

    struct foc_encoder_config candidate_encoder = encoder_config;
    candidate_encoder.counts_per_revolution =
        snapshot->source_identity.encoder_counts_per_revolution;
    candidate_encoder.direction = snapshot->runtime_encoder_direction == 1
        ? FOC_ENCODER_DIRECTION_NORMAL
        : FOC_ENCODER_DIRECTION_REVERSED;

    struct foc_board_output candidate_output = {0};
    candidate_pipeline.output_arg = &candidate_output;
    const int status =
        foc_runtime_init(runtime, &candidate_pipeline, &candidate_runtime_config, now_us);
    if (status != FOC_RUNTIME_OK) {
        return status;
    }

    runtime->pipeline.output_arg = output;
    *output = candidate_output;
    *encoder = (struct foc_board_encoder){.config = candidate_encoder, .config_valid = true};
    return FOC_RUNTIME_OK;
}

int foc_board_reconfigure(
    struct foc_runtime *runtime,
    struct foc_board_output *output,
    struct foc_board_encoder *encoder,
    const struct foc_calibration_snapshot *snapshot,
    uint32_t now_us
)
{
    if (runtime == NULL || output == NULL || encoder == NULL) {
        return FOC_RUNTIME_ERR_INPUT;
    }
    const struct foc_board_encoder previous_encoder = *encoder;
    /* The lanes follow the running TMR1/TMR5 producers, not the configuration: a fresh
     * schedule anchored at an arbitrary instant would make the next H0/H1 look late. */
    const struct foc_runtime_lane lanes[] =
        {runtime->h0, runtime->h1, runtime->supervision, runtime->telemetry};
    const foc_board_output_backend backend = output->backend;
    void *const backend_argument = output->backend_argument;
    *encoder = (struct foc_board_encoder){0};
    const int status = foc_board_init_with_calibration(runtime, output, encoder, snapshot, now_us);
    if (status != FOC_RUNTIME_OK) {
        *encoder = previous_encoder;
        return status;
    }
    runtime->h0 = lanes[0];
    runtime->h1 = lanes[1];
    runtime->supervision = lanes[2];
    runtime->telemetry = lanes[3];
    output->backend = backend;
    output->backend_argument = backend_argument;
    return FOC_RUNTIME_OK;
}

int foc_board_reconfigure_nominal(
    struct foc_runtime *runtime,
    struct foc_board_output *output,
    struct foc_board_encoder *encoder,
    uint32_t now_us
)
{
    if (runtime == NULL || output == NULL || encoder == NULL) {
        return FOC_RUNTIME_ERR_INPUT;
    }
    /* Keep the running lane schedules and the output backend, as foc_board_reconfigure. */
    const struct foc_runtime_lane lanes[] =
        {runtime->h0, runtime->h1, runtime->supervision, runtime->telemetry};
    const foc_board_output_backend backend = output->backend;
    void *const backend_argument = output->backend_argument;
    /* foc_runtime_init() validates before it writes, so a refusal keeps the Runtime; the
     * nominal configuration is a compile-time constant that always validates. */
    const int status = foc_board_init(runtime, output, now_us);
    output->backend = backend;
    output->backend_argument = backend_argument;
    if (status != FOC_RUNTIME_OK) {
        return status;
    }
    *encoder = (struct foc_board_encoder){0};
    runtime->h0 = lanes[0];
    runtime->h1 = lanes[1];
    runtime->supervision = lanes[2];
    runtime->telemetry = lanes[3];
    return FOC_RUNTIME_OK;
}

bool foc_board_elapsed_s(uint32_t now_us, uint32_t previous_us, uint32_t limit_us, float *seconds)
{
    if (seconds == NULL || !foc_safe_timestamp_fresh(now_us, previous_us, limit_us)) {
        return false;
    }
    *seconds = (float)(now_us - previous_us) * 1.0e-6f;
    return true;
}

/* ADC timing helpers ----------------------------------------------------------- */
enum foc_board_adc_snapshot_state {
    FOC_BOARD_ADC_SNAPSHOT_IDLE,
    FOC_BOARD_ADC_SNAPSHOT_REQUESTED,
    FOC_BOARD_ADC_SNAPSHOT_READY,
};

static void foc_board_adc_add(uint32_t *counter, uint32_t n)
{
    *counter += n <= UINT32_MAX - *counter ? n : UINT32_MAX - *counter;
}

static bool foc_board_adc_pending(const struct foc_board_adc_transaction *token)
{
    return token->state == FOC_BOARD_ADC_PENDING || token->state == FOC_BOARD_ADC_DRAIN;
}

static bool
foc_board_adc_observe(struct foc_board_adc *adc, const struct foc_board_adc_event *event)
{
    struct foc_board_adc_diagnostics *view = &adc->live;
    const uint32_t span = event->now_us - event->before_us;
    unsigned int expected = 0U;
    if (event->now_us < adc->last_clock_us) {
        expected |= FOC_BOARD_TIME_WRAP;
    }
    if ((adc->last_clock_us & FOC_BOARD_CLOCK_HALF) == 0U &&
        (event->now_us & FOC_BOARD_CLOCK_HALF) != 0U) {
        expected |= FOC_BOARD_TIME_HALF;
    }
    if (!view->clock_valid || !event->coherent || span > FOC_BOARD_ADC_BRACKET_US ||
        event->before_us - adc->last_clock_us >= FOC_BOARD_CLOCK_HALF ||
        event->now_us - adc->last_clock_us >= FOC_BOARD_CLOCK_HALF ||
        (event->flags & (FOC_BOARD_TIME_WRAP | FOC_BOARD_TIME_HALF)) != expected ||
        event->phase_ticks > FOC_BOARD_COMPARE_MAX) {
        return false;
    }

    /* now - floor(phase) is the latest possible slot boundary. Intersect
     * its read bracket (plus one tick of floor error) with the startup
     * bracket translated by k periods. The brackets are much shorter
     * than one slot, so an intersection identifies exactly one k. */
    const uint32_t elapsed =
        event->now_us - event->phase_ticks / FOC_BOARD_TICKS_PER_US - adc->slot_start_us;
    if (elapsed >= FOC_BOARD_CLOCK_HALF) {
        return false;
    }
    const uint32_t tolerance = adc->epoch_width_us + span + 1U;
    const bool rolled = (event->flags & FOC_BOARD_SLOT_WRAP) != 0U;
    if (elapsed < FOC_BOARD_H0_US) {
        if (elapsed > tolerance || rolled) {
            return false;
        }
        adc->last_clock_us = event->now_us;
        if (view->observed_slots == 0U) {
            view->observed_slots = 1U;
        }
        return true;
    }
    if (!rolled) {
        return false;
    }
    const uint32_t advance = elapsed < 2U * FOC_BOARD_H0_US ? 1U : elapsed / FOC_BOARD_H0_US;
    const uint32_t translation = advance * FOC_BOARD_H0_US;
    if (elapsed - translation > tolerance) {
        return false;
    }
    adc->last_clock_us = event->now_us;
    foc_board_adc_add(&view->missing_slots, advance - (adc->slot_served ? 1U : 0U));
    foc_board_adc_add(&view->observed_slots, 1U);
    view->slot_id += advance;
    adc->slot_start_us += translation;
    adc->slot_served = false;
    adc->trigger_seen = false;
    return true;
}

static void foc_board_adc_reject(
    struct foc_runtime *runtime,
    struct foc_board_adc_diagnostics *view,
    enum foc_board_adc_rejection reason,
    uint32_t faults
)
{
    view->last_rejection = reason;
    if (reason != FOC_BOARD_ADC_BAD_RANKS) {
        foc_board_adc_add(&view->timing_rejections, 1U);
    }
    if (reason == FOC_BOARD_ADC_AMBIGUOUS_CLOCK) {
        view->clock_valid = false;
        foc_board_adc_add(&view->unattributable_events, 1U);
    }
    if (reason == FOC_BOARD_ADC_OVERLAP) {
        foc_board_adc_add(&view->overlap_rejections, 1U);
    }
    if (reason == FOC_BOARD_ADC_UNMATCHED_PCCE) {
        foc_board_adc_add(&view->unmatched_completions, 1U);
    }
    if (reason == FOC_BOARD_ADC_LATE) {
        foc_board_adc_add(&view->late_completions, 1U);
    }
    foc_runtime_request_inhibit(runtime, faults);
}

/* ADC IRQ and foreground entry points -------------------------------------------- */
bool foc_board_adc_init_timing(
    struct foc_board_adc *adc,
    uint32_t before_us,
    uint32_t after_us,
    uint32_t initial_phase_ticks
)
{
    if (adc->timing_initialized) {
        return false;
    }
    adc->timing_initialized = true;
    adc->slot_start_us = before_us;
    adc->last_clock_us = after_us;
    adc->epoch_width_us = after_us - before_us;
    atomic_init(&adc->snapshot_state, FOC_BOARD_ADC_SNAPSHOT_IDLE);
    adc->live.clock_valid =
        adc->epoch_width_us <= FOC_BOARD_ADC_BRACKET_US && initial_phase_ticks == 0U;
    return adc->live.clock_valid;
}

bool foc_board_adc_begin(
    struct foc_runtime *runtime,
    struct foc_board_adc *adc,
    const struct foc_board_adc_event *event,
    bool pcce_pending
)
{
    struct foc_board_adc_diagnostics *view = &adc->live;
    struct foc_board_adc_transaction *token = &view->transaction;
    enum foc_board_adc_rejection reason = FOC_BOARD_ADC_NO_REJECTION;
    uint32_t faults = FOC_SAFE_FAULT_FAST_HEARTBEAT;
    if (!foc_board_adc_observe(adc, event)) {
        reason = FOC_BOARD_ADC_AMBIGUOUS_CLOCK;
    } else if (
        event->phase_ticks < FOC_BOARD_ADC_COMPARE_US * FOC_BOARD_TICKS_PER_US ||
        event->phase_ticks > FOC_BOARD_ADC_LATEST_US * FOC_BOARD_TICKS_PER_US
    ) {
        reason = FOC_BOARD_ADC_TRIGGER_WINDOW;
    } else if (adc->trigger_seen) {
        reason = FOC_BOARD_ADC_DUPLICATE_TRIGGER;
    } else {
        adc->trigger_seen = true;
        if (foc_board_adc_pending(token)) {
            token->state = FOC_BOARD_ADC_DRAIN;
            reason = FOC_BOARD_ADC_OVERLAP;
            faults = FOC_SAFE_FAULT_SAMPLE_TIMEOUT;
        } else if (pcce_pending) {
            reason = FOC_BOARD_ADC_UNDRAINED_PCCE;
            faults = FOC_SAFE_FAULT_SAMPLE_INVALID;
        }
    }
    if (reason != FOC_BOARD_ADC_NO_REJECTION) {
        foc_board_adc_reject(runtime, view, reason, faults);
        return false;
    }

    *token = (struct foc_board_adc_transaction){
        .slot_id = view->slot_id,
        .generation = adc->next_generation++,
        .requested_us = event->now_us,
        .diagnostic_load_us = adc->slot_start_us + FOC_BOARD_H0_US,
        .state = FOC_BOARD_ADC_PENDING,
    };
#if defined(FALCONFOC_RT02_TIMING_TRACE)
    if (adc->trace_cycles != NULL) {
        token->trace.compare_irq_cycles = event->irq_entry_cycles;
        token->trace.compare_irq_phase_ticks = event->irq_entry_phase_ticks;
    }
#endif
    foc_board_adc_add(&view->trigger_requests, 1U);
    return true;
}

int foc_board_adc_complete(
    struct foc_runtime *runtime,
    struct foc_board_adc *adc,
    struct foc_board_output *output,
    const uint16_t *ranks,
    unsigned int count,
    const struct foc_board_adc_event *event,
    bool pcce_drained
)
{
    struct foc_board_adc_diagnostics *view = &adc->live;
    struct foc_board_adc_transaction *token = &view->transaction;
    const bool clock_valid = foc_board_adc_observe(adc, event);
    const bool pending = foc_board_adc_pending(token);
    const bool complete = ranks != NULL && count == 4U;
    const bool valid = complete && ranks[0] <= FOC_BOARD_ADC_MAX && ranks[1] <= FOC_BOARD_ADC_MAX &&
        ranks[2] <= FOC_BOARD_ADC_MAX && ranks[3] <= FOC_BOARD_ADC_MAX;
    uint32_t faults = valid ? 0U : FOC_SAFE_FAULT_SAMPLE_INVALID;
    enum foc_board_adc_rejection reason = FOC_BOARD_ADC_NO_REJECTION;
    if (!clock_valid) {
        reason = FOC_BOARD_ADC_AMBIGUOUS_CLOCK;
        faults |= FOC_SAFE_FAULT_FAST_HEARTBEAT;
    } else if (!pending) {
        reason = FOC_BOARD_ADC_UNMATCHED_PCCE;
        faults |= FOC_SAFE_FAULT_SAMPLE_INVALID;
    } else if (!pcce_drained) {
        reason = FOC_BOARD_ADC_UNDRAINED_PCCE;
        faults |= FOC_SAFE_FAULT_SAMPLE_INVALID;
    } else if (
        token->state == FOC_BOARD_ADC_DRAIN || token->slot_id != view->slot_id ||
        event->now_us - token->requested_us > FOC_BOARD_ADC_FRAME_US
    ) {
        reason = FOC_BOARD_ADC_LATE;
        faults |= FOC_SAFE_FAULT_SAMPLE_TIMEOUT;
    }

    if (reason == FOC_BOARD_ADC_NO_REJECTION) {
        foc_board_adc_add(&view->adc_completions, 1U);
        if (!valid) {
            reason = FOC_BOARD_ADC_BAD_RANKS;
        }
    }
    if (pending) {
#if defined(FALCONFOC_RT02_TIMING_TRACE)
        if (adc->trace_cycles != NULL) {
            token->trace.pcce_irq_cycles = event->irq_entry_cycles;
            token->trace.ranks_ready_cycles = event->ranks_ready_cycles;
            token->trace.points |= FOC_BOARD_TRACE_PCCE_ENTRY;
        }
#endif
        token->completed_us = event->now_us;
        token->adc_valid = reason == FOC_BOARD_ADC_NO_REJECTION;
        token->rejection = reason;
        token->state = !pcce_drained               ? FOC_BOARD_ADC_DRAIN
            : reason == FOC_BOARD_ADC_NO_REJECTION ? FOC_BOARD_ADC_COMPLETED
                                                   : FOC_BOARD_ADC_REJECTED;
    }
    if (reason != FOC_BOARD_ADC_NO_REJECTION) {
        foc_board_adc_reject(runtime, view, reason, faults);
        if (reason != FOC_BOARD_ADC_BAD_RANKS || !complete) {
            return reason == FOC_BOARD_ADC_BAD_RANKS ? FOC_RUNTIME_ERR_INPUT
                                                     : FOC_RUNTIME_ERR_ORDER;
        }
        /* Preserve the existing timely invalid-frame STOP service, without
         * granting the frame validity or offering it to the control path. */
    }

    adc->slot_served = valid;
    if (valid && adc->tap != NULL) {
        adc->tap(adc->tap_argument, ranks, token->generation, token->requested_us);
    }
    const struct foc_runtime_fast_input input = {
        .adc =
            {ranks[0],
             ranks[1],
             ranks[2],
             ranks[3],
             valid ? FOC_DEVICE_SAMPLE_ALL_VALID : 0U,
             token->generation,
             token->requested_us},
        .now_us = event->now_us,
    };
    const uint32_t updates = runtime->pipeline.mock.updates;
    const uint32_t declined = output->declined_updates;
    const bool already_observed = runtime->adc.seen && runtime->adc.seq == token->generation &&
        runtime->adc.produced_us == token->requested_us;
#if defined(FALCONFOC_RT02_TIMING_TRACE)
    if (adc->trace_cycles != NULL) {
        output->trace_owner = adc;
        token->trace.h0_start_cycles = adc->trace_cycles();
    }
#endif
    const int result = foc_runtime_h0_step(runtime, &input);
#if defined(FALCONFOC_RT02_TIMING_TRACE)
    if (adc->trace_cycles != NULL) {
        token->trace.h0_finish_cycles = adc->trace_cycles();
        token->trace.points |= FOC_BOARD_TRACE_H0;
        if (runtime->observed_sensor.valid) {
            token->trace.sensor_used = true;
            token->trace.sensor_capture_us = runtime->observed_sensor.captured_us;
            token->trace.sensor_seq = runtime->observed_sensor.seq;
            token->trace.prediction_us = runtime->angle_prediction_us;
        }
        token->trace.motion_session = runtime->motion_session;
        token->trace.outer_accepted = runtime->accepted_outer_seen;
        token->trace.accepted_outer_seq = runtime->accepted_outer_seq;
        output->trace_owner = NULL;
    }
#endif
    token->h0_called = true;
    token->h0_result = result;
    if (valid && adc->observer != NULL) {
        adc->observer(adc->observer_argument, runtime, token->slot_id, result);
    }
    token->h0_consumed = valid && !already_observed && runtime->adc.seen &&
        runtime->adc.seq == token->generation && runtime->adc.produced_us == token->requested_us;
    token->update_accepted = runtime->pipeline.mock.updates != updates;
    token->update_declined = output->declined_updates != declined;
    token->final_output = output->result;
    if (token->h0_consumed) {
        foc_board_adc_add(&view->h0_consumed, 1U);
    }
    return result;
}

void foc_board_adc_publish(struct foc_board_adc *adc)
{
    if (atomic_load_explicit(&adc->snapshot_state, memory_order_relaxed) ==
        FOC_BOARD_ADC_SNAPSHOT_REQUESTED) {
        /* Pair with the reader's release only when it requests a copy. */
        atomic_thread_fence(memory_order_acquire);
        adc->published = adc->live;
        atomic_store_explicit(
            &adc->snapshot_state,
            FOC_BOARD_ADC_SNAPSHOT_READY,
            memory_order_release
        );
    }
}

int foc_board_adc_read(struct foc_board_adc *adc, struct foc_board_adc_diagnostics *diagnostics)
{
    const unsigned int state = atomic_load_explicit(&adc->snapshot_state, memory_order_acquire);
    if (state == FOC_BOARD_ADC_SNAPSHOT_READY) {
        *diagnostics = adc->published;
        atomic_store_explicit(
            &adc->snapshot_state,
            FOC_BOARD_ADC_SNAPSHOT_IDLE,
            memory_order_release
        );
        return FOC_RUNTIME_OK;
    }
    if (state == FOC_BOARD_ADC_SNAPSHOT_IDLE) {
        atomic_store_explicit(
            &adc->snapshot_state,
            FOC_BOARD_ADC_SNAPSHOT_REQUESTED,
            memory_order_release
        );
    }
    return FOC_RUNTIME_ERR_BUSY;
}

bool foc_board_encoder_begin(struct foc_board_encoder *encoder, uint32_t now_us)
{
    if (encoder == NULL || foc_board_encoder_init_sensor(encoder) != FOC_ENCODER_STATUS_OK) {
        return false;
    }
    if (encoder->pending) {
        encoder->result = FOC_BOARD_ENCODER_OVERLAP;
        return false;
    }
    encoder->captured_us = now_us;
    encoder->pending = true;
    encoder->result = FOC_BOARD_ENCODER_OK;
    return true;
}

int foc_board_encoder_complete(
    struct foc_runtime *runtime,
    struct foc_board_encoder *encoder,
    uint32_t now_us,
    enum foc_board_encoder_result transport,
    uint16_t code
)
{
    if (!encoder->pending) {
        return FOC_RUNTIME_ERR_ORDER;
    }
    encoder->pending = false;
    struct foc_runtime_sensor sample = {.captured_us = encoder->captured_us, .seq = encoder->seq++};
#if defined(FALCONFOC_RT02_TIMING_TRACE)
    if (encoder->trace_cycles != NULL) {
        encoder->trace = (struct foc_board_encoder_trace){
            .capture_us = sample.captured_us,
            .close_us = now_us,
            .seq = sample.seq,
            .decode_start_cycles = encoder->trace_cycles(),
        };
    }
#endif
    if (encoder->result != FOC_BOARD_ENCODER_OVERLAP) {
        encoder->result = transport;
    }
    if (encoder->result != FOC_BOARD_ENCODER_OVERLAP) {
        if (encoder->result == FOC_BOARD_ENCODER_OK &&
            !foc_safe_timestamp_fresh(now_us, sample.captured_us, FOC_BOARD_ENCODER_FRAME_US)) {
            encoder->result = FOC_BOARD_ENCODER_TIMEOUT;
        }
        const enum foc_encoder_transport frame_transport =
            foc_board_encoder_transport(encoder->result);
        const int sensor_status = foc_encoder_update(
            &encoder->sensor,
            sample.captured_us,
            code,
            frame_transport,
            &encoder->feedback
        );
        sample.valid = encoder->feedback.valid;
        sample.mechanical_rad = encoder->feedback.mechanical_rad;
        sample.mechanical_rad_s = encoder->feedback.filtered_velocity_rad_s;
        sample.electrical_phase_valid = sample.valid &&
            encoder->feedback.captured_us == sample.captured_us &&
            foc_rotor_phase_from_counts(
                &runtime->config.motion.rotor,
                encoder->feedback.mechanical_phase_count,
                encoder->sensor.config.counts_per_revolution,
                &sample.theta_elec_rad
            ) == FOC_STATUS_OK;
        if (sensor_status != FOC_ENCODER_STATUS_OK && encoder->result == FOC_BOARD_ENCODER_OK) {
            encoder->result = foc_board_encoder_quality(encoder->feedback.quality);
        }
        encoder->continuity_lost = encoder->feedback.continuity_lost;
    }
    if (encoder->result == FOC_BOARD_ENCODER_OK) {
        encoder->losses = 0U;
    } else if (encoder->losses < UINT32_MAX) {
        encoder->losses++;
    }
#if defined(FALCONFOC_RT02_TIMING_TRACE)
    if (encoder->trace_cycles != NULL) {
        encoder->trace.decode_done_cycles = encoder->trace_cycles();
    }
#endif
    if (encoder->result != FOC_BOARD_ENCODER_OK) {
        /* H0 may preempt H1 before the invalid snapshot is published. */
        foc_runtime_request_inhibit(runtime, 0U);
    }
#if defined(FALCONFOC_RT02_TIMING_TRACE)
    if (encoder->trace_cycles != NULL) {
        encoder->trace.h1_start_cycles = encoder->trace_cycles();
    }
#endif
    const int status = foc_runtime_h1_step(runtime, &sample, now_us, 0U);
#if defined(FALCONFOC_RT02_TIMING_TRACE)
    if (encoder->trace_cycles != NULL) {
        encoder->trace.h1_finish_cycles = encoder->trace_cycles();
        encoder->trace.h1_status = status;
    }
#endif
    if (sample.valid && status != FOC_RUNTIME_OK) {
        foc_runtime_request_inhibit(runtime, 0U);
    }
    return status;
}

bool foc_board_output_submit(void *argument, const struct foc_device_pwm_request *request)
{
    struct foc_board_output *output = argument;
    if (output == NULL) {
        return false;
    }
    output->result = FOC_BOARD_OUTPUT_INVALID;
    if (request == NULL) {
        return false;
    }
    output->request = *request;
    if (request->action == FOC_DEVICE_OUTPUT_UPDATE) {
        const struct foc_output duties = {
            .output_enable = 1U,
            .svpwm =
                {.duty_a = request->duty_a, .duty_b = request->duty_b, .duty_c = request->duty_c},
        };
        struct foc_device_pwm_request mapped;
        if (request->output_enable_request == 1U &&
            foc_device_map_output(&duties, &pipeline_config.pwm, &mapped) == FOC_DEVICE_STATUS_OK &&
            mapped.channel1_compare == request->channel1_compare &&
            mapped.channel2_compare == request->channel2_compare &&
            mapped.channel3_compare == request->channel3_compare) {
            if (output->backend != NULL && output->backend(output->backend_argument, request)) {
                output->result = FOC_BOARD_OUTPUT_APPLIED;
                output->applied_updates++;
                return true;
            }
            output->result = FOC_BOARD_UPDATE_NOT_APPLIED;
            output->declined_updates++;
        }
    } else if (foc_board_compares_zero(request)) {
        if (request->action == FOC_DEVICE_OUTPUT_INHIBIT && request->output_enable_request == 0U) {
            /* INHIBIT is always accepted; a backend also removes its drive. */
            if (output->backend != NULL) {
                (void)output->backend(output->backend_argument, request);
            }
            output->result = FOC_BOARD_INHIBIT_SOFTWARE;
            return true;
        }
        if (request->output_enable_request == 1U) {
            if (request->action == FOC_DEVICE_OUTPUT_ZERO_DUTY) {
                output->result = FOC_BOARD_ZERO_DUTY_LOCKED;
            } else if (request->action == FOC_DEVICE_OUTPUT_ACTIVE_BRAKE) {
                output->result = FOC_BOARD_ACTIVE_BRAKE_LOCKED;
            }
        }
    }
    return false;
}

bool foc_board_output_attach(
    struct foc_board_output *output,
    foc_board_output_backend backend,
    void *argument
)
{
    if (output == NULL || backend == NULL || output->backend != NULL) {
        return false;
    }
    output->backend_argument = argument;
    output->backend = backend;
    return true;
}

/* ---------------------------------------- end of file ----------------------------------------- */
