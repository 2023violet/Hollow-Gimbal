/**
 * @file    foc_runtime.c
 * @brief   Fixed H0, H1, and Foreground services around the Host Pipeline.
 * @version v0.1.0
 * @date    2026.11.10
 *
 * Implementation role
 * -------------------
 * Owns cross-domain publication and bounded scheduling. Only H0 executes
 * Pipeline and acknowledges independent stop requests. Foreground evaluates
 * the existing Safety rules on a coherent copy of H0's observations.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_runtime.h"

#include "foc_impedance.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

/* Private Constants ------------------------------------------------------------ */

#define FOC_RUNTIME_HALF_RANGE   UINT32_C(0x80000000)
#define FOC_RUNTIME_STOP_REQUEST FOC_RUNTIME_HALF_RANGE

enum {
    FOC_RUNTIME_UNPUBLISHED = 2,
    FOC_RUNTIME_REQUEST_IDLE = 0,
    FOC_RUNTIME_REQUEST_WRITING = 1,
    FOC_RUNTIME_REQUEST_PUBLISHED = 2,
    FOC_RUNTIME_REQUEST_CLAIMED = 3,
    SNAPSHOT_CONFIRMED = 1U << 0,
    SNAPSHOT_FAST_SEEN = 1U << 1,
    SNAPSHOT_COMMAND_SEEN = 1U << 2,
    SNAPSHOT_SAMPLE_SEEN = 1U << 3,
    SNAPSHOT_OUTER_SEEN = 1U << 4,
    SNAPSHOT_SENSOR_VALID = 1U << 5,
    SNAPSHOT_CURRENT_VALID = 1U << 6,
};

enum {
    WORD_FLAGS,
    WORD_STATE,
    WORD_FAULTS,
    WORD_ACTION,
    WORD_COMPARE1,
    WORD_COMPARE2,
    WORD_COMPARE3,
    WORD_UPDATES,
    WORD_INTERVAL,
    WORD_TRIGGER,
    WORD_DELAY,
    WORD_MISSED,
    WORD_FAST,
    WORD_COMMAND,
    WORD_SAMPLE,
    WORD_MODE,
    WORD_SESSION,
    WORD_OUTER_SEQ,
    WORD_PREDICTION,
    WORD_POSITION,
    WORD_VELOCITY,
    WORD_SENSOR_TIME,
    WORD_COMMAND_SEQ,
    WORD_FOLLOWING_STATE,
    WORD_MOTION_REJECTION,
    WORD_MEASURED_ID,
    WORD_MEASURED_IQ,
    WORD_TARGET_ID,
    WORD_TARGET_IQ,
    WORD_COUNT,
};

_Static_assert(UINT_MAX == UINT32_MAX, "Runtime requires 32-bit unsigned int");
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "ISR handoffs require lock-free word atomics");
_Static_assert((int)WORD_COUNT == (int)FOC_RUNTIME_TELEMETRY_WORDS, "Telemetry size mismatch");

/* Private Helpers -------------------------------------------------------------- */

static unsigned int foc_runtime_float_word(float value)
{
    unsigned int word;
    _Static_assert(sizeof word == sizeof value, "Telemetry requires 32-bit float");
    memcpy(&word, &value, sizeof word);
    return word;
}

static float foc_runtime_word_float(unsigned int word)
{
    float value;
    memcpy(&value, &word, sizeof value);
    return value;
}

static bool foc_runtime_lane_valid(const struct foc_runtime_lane_config *lane)
{
    return lane->period_us > 0U && lane->period_us < FOC_RUNTIME_HALF_RANGE &&
        lane->max_delay_us < FOC_RUNTIME_HALF_RANGE && lane->budget_us > 0U &&
        lane->budget_us < FOC_RUNTIME_HALF_RANGE;
}

static int foc_runtime_schedule(
    struct foc_runtime_lane *lane,
    const struct foc_runtime_lane_config *config,
    uint32_t now_us,
    uint32_t cost_us
)
{
    if (!foc_safe_timestamp_fresh(now_us, lane->last_call_us, UINT32_MAX)) {
        return FOC_RUNTIME_ERR_ORDER;
    }
    lane->last_call_us = now_us;
    const uint32_t delay_us = now_us - lane->next_us;
    if (delay_us >= FOC_RUNTIME_HALF_RANGE) {
        return FOC_RUNTIME_IDLE;
    }

    const uint32_t skipped = delay_us / config->period_us;
    const bool late = delay_us > config->max_delay_us;
    const bool over_budget = cost_us > config->budget_us;
    lane->deadline_us = lane->next_us;
    lane->trigger_us = now_us;
    lane->delay_us = delay_us;
    lane->next_us += (skipped + 1U) * config->period_us;
    lane->missed += skipped;
    lane->due_count++;
    lane->late_count += late;
    lane->budget_count += over_budget;
    return late || over_budget ? FOC_RUNTIME_ERR_TIMING : FOC_RUNTIME_OK;
}

static void foc_runtime_hook(struct foc_runtime *runtime, enum foc_runtime_hook_point point)
{
    if (runtime->config.hook != NULL) {
        runtime->config.hook(runtime, point, runtime->config.hook_arg);
    }
}

/* All words and sequence updates are sequentially consistent. */
static void foc_runtime_publish_snapshot(struct foc_runtime *runtime)
{
    const struct foc_safe_context *safe = &runtime->pipeline.safety;
    const struct foc_pipeline_mock *mock = &runtime->pipeline.mock;
    const unsigned int words[WORD_COUNT] = {
        [WORD_FLAGS] = (mock->output_confirmed ? SNAPSHOT_CONFIRMED : 0U) |
            (safe->fast_heartbeat_seen ? SNAPSHOT_FAST_SEEN : 0U) |
            (safe->command_seen ? SNAPSHOT_COMMAND_SEEN : 0U) |
            (safe->sample_seen ? SNAPSHOT_SAMPLE_SEEN : 0U) |
            (runtime->accepted_outer_seen ? SNAPSHOT_OUTER_SEEN : 0U) |
            (runtime->observed_sensor.valid ? SNAPSHOT_SENSOR_VALID : 0U) |
            (mock->output_confirmed && mock->last_request.action == FOC_DEVICE_OUTPUT_UPDATE &&
                     runtime->pipeline.last_reason == FOC_PIPELINE_OK
                 ? SNAPSHOT_CURRENT_VALID
                 : 0U),
        [WORD_STATE] = safe->state,
        [WORD_FAULTS] = safe->latched_faults,
        [WORD_ACTION] = mock->last_request.action,
        [WORD_COMPARE1] = mock->output_confirmed ? mock->registers.channel1_compare : 0U,
        [WORD_COMPARE2] = mock->output_confirmed ? mock->registers.channel2_compare : 0U,
        [WORD_COMPARE3] = mock->output_confirmed ? mock->registers.channel3_compare : 0U,
        [WORD_UPDATES] = mock->updates,
        [WORD_INTERVAL] = runtime->interval_us,
        [WORD_TRIGGER] = runtime->h0.trigger_us,
        [WORD_DELAY] = runtime->h0.delay_us,
        [WORD_MISSED] = runtime->h0.missed,
        [WORD_FAST] = safe->last_fast_heartbeat_us,
        [WORD_COMMAND] = safe->last_command_us,
        [WORD_SAMPLE] = safe->last_sample_us,
        [WORD_MODE] = runtime->mode,
        [WORD_SESSION] = runtime->motion_session,
        [WORD_OUTER_SEQ] = runtime->accepted_outer_seq,
        [WORD_PREDICTION] = runtime->angle_prediction_us,
        [WORD_POSITION] = foc_runtime_float_word(runtime->observed_sensor.mechanical_rad),
        [WORD_VELOCITY] = foc_runtime_float_word(runtime->observed_sensor.mechanical_rad_s),
        [WORD_SENSOR_TIME] = runtime->observed_sensor.captured_us,
        [WORD_COMMAND_SEQ] = safe->command_seq,
        [WORD_FOLLOWING_STATE] = (unsigned int)runtime->following.status,
        [WORD_MOTION_REJECTION] = (unsigned int)runtime->motion_rejection,
        [WORD_MEASURED_ID] = foc_runtime_float_word(runtime->pipeline.measured_dq_a.d),
        [WORD_MEASURED_IQ] = foc_runtime_float_word(runtime->pipeline.measured_dq_a.q),
        [WORD_TARGET_ID] = foc_runtime_float_word(runtime->pipeline.target_dq_a.d),
        [WORD_TARGET_IQ] = foc_runtime_float_word(runtime->pipeline.target_dq_a.q),
    };
    for (unsigned int index = 0U; index < WORD_COUNT; ++index) {
        atomic_store(&runtime->telemetry_words[index], words[index]);
    }
    atomic_store(&runtime->telemetry_pipeline_status, runtime->pipeline_status);
    atomic_fetch_add(&runtime->telemetry_sequence, 1U);
}

static int foc_runtime_copy_snapshot(
    struct foc_runtime *runtime,
    struct foc_runtime_snapshot *snapshot,
    enum foc_runtime_hook_point hook
)
{
    const unsigned int before = atomic_load(&runtime->telemetry_sequence);
    if ((before & 1U) != 0U) {
        return FOC_RUNTIME_ERR_BUSY;
    }
    unsigned int words[WORD_COUNT];
    words[WORD_FLAGS] = atomic_load(&runtime->telemetry_words[WORD_FLAGS]);
    if (hook != FOC_RUNTIME_HOOK_NONE) {
        foc_runtime_hook(runtime, hook);
    }
    for (unsigned int index = WORD_STATE; index < WORD_COUNT; ++index) {
        words[index] = atomic_load(&runtime->telemetry_words[index]);
    }
    const int pipeline_status = atomic_load(&runtime->telemetry_pipeline_status);
    /* Include pending stop in the sequence window so acknowledgement cannot unmask old data. */
    const unsigned int pending = atomic_load(&runtime->stop_requests);
    if (before != atomic_load(&runtime->telemetry_sequence)) {
        return FOC_RUNTIME_ERR_BUSY;
    }

    *snapshot = (struct foc_runtime_snapshot){
        .state = (enum foc_safe_state)words[WORD_STATE],
        .faults = words[WORD_FAULTS] | (pending & ~FOC_RUNTIME_STOP_REQUEST),
        .action = pending != 0U ? FOC_DEVICE_OUTPUT_INHIBIT
                                : (enum foc_device_output_action)words[WORD_ACTION],
        .compare =
            {pending == 0U ? words[WORD_COMPARE1] : 0U,
             pending == 0U ? words[WORD_COMPARE2] : 0U,
             pending == 0U ? words[WORD_COMPARE3] : 0U},
        .updates = words[WORD_UPDATES],
        .interval_us = words[WORD_INTERVAL],
        .trigger_us = words[WORD_TRIGGER],
        .delay_us = words[WORD_DELAY],
        .missed = words[WORD_MISSED],
        .pipeline_status = pipeline_status,
        .fast_us = words[WORD_FAST],
        .command_us = words[WORD_COMMAND],
        .command_seq = words[WORD_COMMAND_SEQ],
        .sample_us = words[WORD_SAMPLE],
        .output_confirmed = pending == 0U && (words[WORD_FLAGS] & SNAPSHOT_CONFIRMED) != 0U,
        .inhibit_pending = pending != 0U,
        .fast_seen = (words[WORD_FLAGS] & SNAPSHOT_FAST_SEEN) != 0U,
        .command_seen = (words[WORD_FLAGS] & SNAPSHOT_COMMAND_SEEN) != 0U,
        .sample_seen = (words[WORD_FLAGS] & SNAPSHOT_SAMPLE_SEEN) != 0U,
        .mode = (enum foc_runtime_mode)words[WORD_MODE],
        .motion_session = words[WORD_SESSION],
        .accepted_outer_seq = words[WORD_OUTER_SEQ],
        .accepted_outer_seen = (words[WORD_FLAGS] & SNAPSHOT_OUTER_SEEN) != 0U,
        .angle_prediction_us = words[WORD_PREDICTION],
        .clear_result = (enum foc_runtime_clear_result)atomic_load(&runtime->clear_result),
        .mechanical_rad = foc_runtime_word_float(words[WORD_POSITION]),
        .mechanical_rad_s = foc_runtime_word_float(words[WORD_VELOCITY]),
        .sensor_us = words[WORD_SENSOR_TIME],
        .sensor_valid = (words[WORD_FLAGS] & SNAPSHOT_SENSOR_VALID) != 0U,
        .following_state = (enum foc_following_state)words[WORD_FOLLOWING_STATE],
        .motion_rejection = (enum foc_motion_rejection)words[WORD_MOTION_REJECTION],
        .measured_id_a = foc_runtime_word_float(words[WORD_MEASURED_ID]),
        .measured_iq_a = foc_runtime_word_float(words[WORD_MEASURED_IQ]),
        .current_valid = (words[WORD_FLAGS] & SNAPSHOT_CURRENT_VALID) != 0U,
        .target_id_a = foc_runtime_word_float(words[WORD_TARGET_ID]),
        .target_iq_a = foc_runtime_word_float(words[WORD_TARGET_IQ]),
    };
    return FOC_RUNTIME_OK;
}

static bool foc_runtime_drain_stop(struct foc_runtime *runtime, uint32_t now_us)
{
    const unsigned int pending = atomic_exchange(&runtime->stop_requests, 0U);
    if (pending == 0U) {
        return false;
    }
    runtime->pipeline_status = foc_pipeline_request_inhibit(
        &runtime->pipeline,
        now_us,
        pending & ~FOC_RUNTIME_STOP_REQUEST
    );
    return true;
}

static void foc_runtime_import_foreground(struct foc_runtime *runtime, uint32_t now_us)
{
    if (atomic_load_explicit(&runtime->foreground_seen, memory_order_acquire) == 0U) {
        return;
    }
    const uint32_t stamp = atomic_load_explicit(&runtime->foreground_us, memory_order_acquire);
    if (foc_safe_sequence_newer(
            stamp,
            runtime->consumed_foreground_us,
            runtime->consumed_foreground_seen
        ) &&
        foc_safe_timestamp_fresh(now_us, stamp, UINT32_MAX) &&
        foc_safe_heartbeat_foreground(&runtime->pipeline.safety, stamp) == FOC_SAFE_STATUS_OK) {
        runtime->consumed_foreground_us = stamp;
        runtime->consumed_foreground_seen = true;
    }
}

static void foc_runtime_import_command(
    struct foc_runtime *runtime,
    const struct foc_pipeline_command *command,
    uint32_t now_us
)
{
    if (command != NULL &&
        foc_safe_sequence_newer(
            command->seq,
            runtime->consumed_command.seq,
            runtime->consumed_command.seen
        )) {
        runtime->consumed_command =
            (struct foc_pipeline_watermark){command->seq, command->produced_us, true};
        (void)foc_pipeline_set_command(&runtime->pipeline, command, now_us);
    }
}

static bool foc_runtime_cascade_mode(enum foc_runtime_mode mode)
{
    return mode == FOC_RUNTIME_MODE_VELOCITY || mode == FOC_RUNTIME_MODE_POSITION;
}

static bool foc_runtime_outer_mode(enum foc_runtime_mode mode)
{
    return foc_runtime_cascade_mode(mode) || mode == FOC_RUNTIME_MODE_IMPEDANCE;
}

static bool foc_runtime_mode_enabled(const struct foc_runtime *runtime, enum foc_runtime_mode mode)
{
    return ((mode == FOC_RUNTIME_MODE_CURRENT || mode == FOC_RUNTIME_MODE_CALIBRATION) &&
            !runtime->config.motion.limits.enabled) ||
        (runtime->config.motion.enabled &&
         (foc_runtime_cascade_mode(mode) ||
          (mode == FOC_RUNTIME_MODE_IMPEDANCE && runtime->config.motion.impedance.enabled)));
}

static bool foc_runtime_protection_enabled(const struct foc_runtime_motion_config *motion)
{
    return motion->limits.enabled || motion->following_error.enabled;
}

static int
foc_runtime_set_mode(struct foc_runtime *runtime, enum foc_runtime_mode mode, uint32_t now_us)
{
    const struct foc_pipeline *pipeline = &runtime->pipeline;
    if (pipeline->safety.state != FOC_SAFE_STATE_DISABLED || !pipeline->mock.output_confirmed ||
        pipeline->mock.last_request.action != FOC_DEVICE_OUTPUT_INHIBIT ||
        !foc_runtime_mode_enabled(runtime, mode)) {
        runtime->pipeline_status = FOC_PIPELINE_ERR_STATE;
        return FOC_RUNTIME_ERR_CONTROL;
    }
    if (mode != runtime->mode) {
        runtime->pipeline_status = foc_pipeline_request_inhibit(&runtime->pipeline, now_us, 0U);
        if (runtime->pipeline_status != FOC_PIPELINE_OK) {
            return FOC_RUNTIME_ERR_CONTROL;
        }
        runtime->mode = mode;
    }
    return FOC_RUNTIME_IDLE;
}

static float foc_runtime_align_angle(
    struct foc_runtime *runtime,
    const struct foc_runtime_sensor_slot *slot,
    const struct foc_runtime_fast_input *input
)
{
    const uint32_t captured = slot->sample.captured_us;
    if (!slot->sample.valid ||
        !foc_safe_timestamp_fresh(input->now_us, captured, runtime->config.angle_timeout_us)) {
        return NAN;
    }
    if (!runtime->config.motion.enabled) {
        return captured == input->adc.sample_tick ? slot->sample.theta_elec_rad : NAN;
    }
    if (!foc_safe_timestamp_fresh(input->adc.sample_tick, captured, UINT32_MAX)) {
        return NAN;
    }
    const uint32_t elapsed = input->adc.sample_tick - captured;
    float angle;
    if (foc_rotor_predict(&runtime->config.motion.prediction, &slot->electrical, elapsed, &angle) !=
        FOC_STATUS_OK) {
        return NAN;
    }
    runtime->angle_prediction_us = elapsed;
    return angle;
}

struct foc_runtime_output_admission {
    struct foc_runtime *runtime;
    bool following_stop;
    bool following_denied;
};

static bool foc_runtime_admit_update(void *argument)
{
    struct foc_runtime_output_admission *admission = argument;
    struct foc_runtime *runtime = admission->runtime;

    foc_runtime_hook(runtime, FOC_RUNTIME_HOOK_BEFORE_SUBMIT);
    /* Retain independent STOP priority at the original linearization point. */
    if (atomic_load(&runtime->stop_requests) != 0U) {
        return false;
    }
    /* Pipeline has accepted the current sample and Safety gate here. Its
     * existing denial path emits INHIBIT before the trial can commit. */
    admission->following_denied = admission->following_stop;
    return !admission->following_denied;
}

/* FAULT_CLEAR recovers command timeout and motor protection only, never other causes. */
static bool foc_runtime_clearable_faults(uint32_t faults)
{
    return faults != FOC_SAFE_FAULT_NONE &&
        (faults & ~(FOC_SAFE_FAULT_COMMAND_TIMEOUT | FOC_SAFE_FAULT_PROTECTION_MASK)) == 0U;
}

static bool foc_runtime_timeout_clear_ready(
    struct foc_runtime *runtime,
    const struct foc_runtime_fast_input *input
)
{
    const struct foc_pipeline *pipeline = &runtime->pipeline;
    const unsigned int sensor = atomic_load_explicit(&runtime->sensor_slot, memory_order_acquire);
    struct foc_device_measurement measurement;
    const uint32_t latched = pipeline->safety.latched_faults;
    const bool voltage =
        (latched & (FOC_SAFE_FAULT_OVER_VOLTAGE | FOC_SAFE_FAULT_UNDER_VOLTAGE)) != 0U;
    return pipeline->safety.state == FOC_SAFE_STATE_FAULT_LATCHED &&
        foc_runtime_clearable_faults(latched) && pipeline->mock.output_confirmed &&
        pipeline->mock.last_request.action == FOC_DEVICE_OUTPUT_INHIBIT &&
        atomic_load(&runtime->command_slot) == FOC_RUNTIME_UNPUBLISHED &&
        atomic_load(&runtime->foreground_seen) != 0U &&
        foc_safe_timestamp_fresh(
               input->now_us,
               atomic_load(&runtime->foreground_us),
               pipeline->safety.config.foreground_heartbeat_timeout_us
        ) &&
        foc_safe_timestamp_fresh(
               input->now_us,
               runtime->clear_requested_us,
               pipeline->safety.config.command_timeout_us
        ) &&
        foc_safe_timestamp_fresh(
               input->now_us,
               input->adc.sample_tick,
               pipeline->safety.config.sample_timeout_us
        ) &&
        foc_device_decode_sample(&input->adc, &pipeline->adc_config, &measurement) ==
        FOC_DEVICE_STATUS_OK &&
        (!voltage ||
         (measurement.core.vbus_v >= runtime->protect.undervoltage_v &&
          measurement.core.vbus_v <= runtime->protect.overvoltage_v)) &&
        sensor != FOC_RUNTIME_UNPUBLISHED &&
        isfinite(foc_runtime_align_angle(runtime, &runtime->sensor_slots[sensor], input));
}

static bool foc_runtime_action_valid(enum foc_runtime_action action, enum foc_safe_state state)
{
    return action == FOC_RUNTIME_ACTION_NONE ||
        (action == FOC_RUNTIME_ACTION_ARM && state == FOC_SAFE_STATE_DISABLED) ||
        (action == FOC_RUNTIME_ACTION_START && state == FOC_SAFE_STATE_READY) ||
        (action == FOC_RUNTIME_ACTION_CLEAR_FAULT && state == FOC_SAFE_STATE_FAULT_LATCHED);
}

static int
foc_runtime_lifecycle(struct foc_runtime *runtime, enum foc_runtime_action action, uint32_t now_us)
{
    switch (action) {
    case FOC_RUNTIME_ACTION_ARM:
        return foc_pipeline_arm(&runtime->pipeline, now_us);
    case FOC_RUNTIME_ACTION_START:
        return foc_pipeline_start(&runtime->pipeline, now_us);
    case FOC_RUNTIME_ACTION_CLEAR_FAULT:
        return foc_pipeline_clear_fault(&runtime->pipeline, now_us);
    default:
        return FOC_PIPELINE_ERR_STATE;
    }
}

static bool foc_runtime_lifecycle_shape_valid(const struct foc_runtime_lifecycle_request *request)
{
    if (request == NULL) {
        return false;
    }
    if (request->action == FOC_RUNTIME_ACTION_SET_MODE) {
        return request->requested_mode == FOC_RUNTIME_MODE_CURRENT ||
            request->requested_mode == FOC_RUNTIME_MODE_CALIBRATION ||
            foc_runtime_outer_mode(request->requested_mode);
    }
    return request->action == FOC_RUNTIME_ACTION_ARM || request->action == FOC_RUNTIME_ACTION_START;
}

static void foc_runtime_lifecycle_write_result(
    struct foc_runtime *runtime,
    const struct foc_runtime_lifecycle_request *request,
    enum foc_runtime_request_result result,
    uint32_t now_us,
    int pipeline_status
)
{
    const unsigned int current =
        atomic_load_explicit(&runtime->lifecycle_result_slot, memory_order_relaxed);
    const unsigned int next = current == 0U ? 1U : 0U;
    runtime->lifecycle_results[next] = (struct foc_runtime_lifecycle_result){
        .result = result,
        .action = request == NULL ? FOC_RUNTIME_ACTION_NONE : request->action,
        .requested_mode = request == NULL ? runtime->mode : request->requested_mode,
        .request_sequence = request == NULL ? 0U : request->request_sequence,
        .applied_session = runtime->motion_session,
        .h0_us = now_us,
        .state = runtime->pipeline.safety.state,
        .faults = runtime->pipeline.safety.latched_faults,
        .pipeline_status = pipeline_status,
    };
    atomic_store_explicit(&runtime->lifecycle_result_slot, next, memory_order_release);
}

static bool foc_runtime_lifecycle_claim(
    struct foc_runtime *runtime,
    struct foc_runtime_lifecycle_request *request
)
{
    unsigned int expected = FOC_RUNTIME_REQUEST_PUBLISHED;
    if (!atomic_compare_exchange_strong_explicit(
            &runtime->lifecycle_request_pending,
            &expected,
            FOC_RUNTIME_REQUEST_CLAIMED,
            memory_order_acquire,
            memory_order_relaxed
        )) {
        return false;
    }
    const unsigned int slot =
        atomic_load_explicit(&runtime->lifecycle_request_slot, memory_order_acquire);
    *request = runtime->lifecycle_requests[slot];
    return true;
}

static bool foc_runtime_lifecycle_applied(
    const struct foc_runtime *runtime,
    const struct foc_runtime_lifecycle_request *request
)
{
    switch (request->action) {
    case FOC_RUNTIME_ACTION_ARM:
        return runtime->pipeline.safety.state == FOC_SAFE_STATE_READY;
    case FOC_RUNTIME_ACTION_START:
        return runtime->pipeline.safety.state == FOC_SAFE_STATE_RUNNING;
    case FOC_RUNTIME_ACTION_SET_MODE:
        return runtime->pipeline.safety.state == FOC_SAFE_STATE_DISABLED &&
            runtime->mode == request->requested_mode;
    default:
        return false;
    }
}

static bool foc_runtime_lifecycle_request_current(
    const struct foc_runtime *runtime,
    const struct foc_runtime_lifecycle_request *request,
    uint32_t now_us
)
{
    return request->source_session == runtime->motion_session &&
        foc_safe_timestamp_fresh(
               now_us,
               request->produced_us,
               runtime->pipeline.safety.config.command_timeout_us
        ) &&
        runtime->pipeline.safety.state != FOC_SAFE_STATE_FAULT_LATCHED;
}

static void foc_runtime_lifecycle_finish(
    struct foc_runtime *runtime,
    const struct foc_runtime_lifecycle_request *request,
    enum foc_runtime_request_result result,
    uint32_t now_us,
    int pipeline_status
)
{
    foc_runtime_lifecycle_write_result(runtime, request, result, now_us, pipeline_status);
    atomic_store_explicit(
        &runtime->lifecycle_request_pending,
        FOC_RUNTIME_REQUEST_IDLE,
        memory_order_release
    );
}

/* H0 reads the same complete publication that supplies the actual Pipeline target. */
static enum foc_motion_rejection foc_runtime_motion_admission(
    const struct foc_runtime *runtime,
    const struct foc_runtime_sensor_slot *sensor,
    bool aligned,
    bool target_valid
)
{
    if (!aligned) {
        return FOC_MOTION_REJECT_FEEDBACK;
    }
    if (sensor->mode != runtime->mode || sensor->session != runtime->motion_session) {
        return FOC_MOTION_REJECT_NONE;
    }
    if (sensor->motion_rejection != FOC_MOTION_REJECT_NONE) {
        return sensor->motion_rejection;
    }
    if (!target_valid) {
        return FOC_MOTION_REJECT_NONE;
    }
    const struct foc_motion_limits_config *limits = &runtime->config.motion.limits;
    if (sensor->sample.mechanical_frame_id != limits->mechanical_frame_id) {
        return FOC_MOTION_REJECT_FRAME;
    }
    struct foc_motion_limits_request request = sensor->motion_request;
    request.iq_ref_a = sensor->target.target_a.q;
    return foc_motion_limits_check(limits, sensor->sample.mechanical_rad, &request);
}

static enum foc_following_state foc_runtime_following_trial(
    const struct foc_runtime *runtime,
    const struct foc_runtime_sensor_slot *sensor,
    bool target_valid,
    bool sensor_valid,
    struct foc_following_error_state *trial
)
{
    *trial = runtime->following;
    const struct foc_following_error_input sample = {
        .reference_rad = sensor->motion_request.position_ref_rad,
        .measured_rad = sensor->sample.mechanical_rad,
        .captured_us = sensor->sample.captured_us,
        .applicable = runtime->mode == FOC_RUNTIME_MODE_POSITION ||
            runtime->mode == FOC_RUNTIME_MODE_IMPEDANCE,
        .reference_valid = target_valid && sensor->motion_request.has_position,
        .sensor_valid = sensor_valid,
    };
    return foc_following_error_step(&runtime->config.motion.following_error, trial, &sample);
}

/* Protection ------------------------------------------------------------------ */

static bool foc_runtime_unset_or(float value, float minimum, float maximum)
{
    return isfinite(value) && (value == 0.0f || (value >= minimum && value <= maximum));
}

/* Profile ranges and the ordering against the limits it bounds. */
static bool foc_runtime_profile_valid(
    const struct foc_runtime_profile *profile,
    float current_limit_a,
    float velocity_limit_rad_s
)
{
    return foc_runtime_unset_or(profile->rs_ohm, 1.0e-3f, 100.0f) &&
        foc_runtime_unset_or(profile->ld_h, 1.0e-6f, 1.0f) &&
        foc_runtime_unset_or(profile->lq_h, 1.0e-6f, 1.0f) &&
        foc_runtime_unset_or(profile->flux_wb, 1.0e-5f, 1.0f) &&
        foc_runtime_unset_or(profile->inertia_kg_m2, 0.0f, 1.0f) &&
        foc_runtime_unset_or(profile->damping_nm_s_per_rad, 0.0f, 10.0f) &&
        foc_runtime_unset_or(profile->rated_current_a, 0.05f, 1000.0f) &&
        foc_runtime_unset_or(profile->peak_current_a, 0.05f, 1000.0f) &&
        foc_runtime_unset_or(profile->max_speed_rad_s, 0.1f, 100000.0f) &&
        foc_runtime_unset_or(profile->gear_ratio, 1.0f, 1000.0f) &&
        foc_runtime_unset_or(profile->current_bandwidth_hz, 10.0f, 1000.0f) &&
        (profile->rated_current_a == 0.0f || profile->peak_current_a == 0.0f ||
         profile->rated_current_a <= profile->peak_current_a) &&
        (profile->peak_current_a == 0.0f || current_limit_a <= profile->peak_current_a) &&
        (profile->max_speed_rad_s == 0.0f || velocity_limit_rad_s <= profile->max_speed_rad_s);
}

/* Derive effective thresholds; a disabled instance yields a disabled configuration. */
static bool foc_runtime_trip_resolve(
    bool enabled,
    const struct foc_runtime_trip_limits *trips,
    const struct foc_runtime_profile *profile,
    float current_limit_a,
    float iq_limit_a,
    float velocity_limit_rad_s,
    struct foc_protect_config *protect
)
{
    *protect = (struct foc_protect_config){0};
    if (!enabled) {
        return true;
    }
    const float iq_basis = iq_limit_a > 0.0f ? iq_limit_a : current_limit_a;
    const float speed_basis =
        profile->max_speed_rad_s > 0.0f ? profile->max_speed_rad_s : velocity_limit_rad_s;
    if (!isfinite(trips->overcurrent_a) || !(trips->overcurrent_a > current_limit_a) ||
        !isfinite(trips->overspeed_rad_s) ||
        !(trips->overspeed_rad_s == 0.0f || trips->overspeed_rad_s >= velocity_limit_rad_s) ||
        !isfinite(velocity_limit_rad_s) || !(velocity_limit_rad_s > 0.0f) ||
        trips->sustained_ms > FOC_RUNTIME_TRIP_WINDOW_MAX_MS ||
        trips->stall_ms > FOC_RUNTIME_TRIP_WINDOW_MAX_MS ||
        trips->divergence_ms > FOC_RUNTIME_TRIP_WINDOW_MAX_MS) {
        return false;
    }
    *protect = (struct foc_protect_config){
        .enabled = true,
        .overcurrent_a = trips->overcurrent_a,
        .overvoltage_v = trips->overvoltage_v,
        .undervoltage_v = trips->undervoltage_v,
        .voltage_samples = 3U,
        .overspeed_rad_s =
            trips->overspeed_rad_s > 0.0f ? trips->overspeed_rad_s : 1.25f * velocity_limit_rad_s,
        .sustained_current_a =
            profile->rated_current_a > 0.0f ? profile->rated_current_a : 0.95f * current_limit_a,
        .sustained_us = trips->sustained_ms * 1000U,
        .stall_current_a = 0.95f * iq_basis,
        .stall_speed_rad_s = 0.05f * speed_basis,
        .stall_us = trips->stall_ms * 1000U,
        .divergence_current_a = 0.95f * iq_basis,
        .divergence_us = trips->divergence_ms * 1000U,
    };
    return foc_protect_validate(protect);
}

static void foc_runtime_publish_fault_snapshot(
    struct foc_runtime *runtime,
    const struct foc_runtime_fault_snapshot *snapshot
)
{
    const unsigned int current =
        atomic_load_explicit(&runtime->fault_snapshot_slot, memory_order_relaxed);
    const unsigned int next = current == 0U ? 1U : 0U;
    runtime->fault_snapshots[next] = *snapshot;
    atomic_store_explicit(&runtime->fault_snapshot_slot, next, memory_order_release);
}

/* H0, after an accepted running cycle: evaluate the rules and request a latching stop. */
static void foc_runtime_protect(
    struct foc_runtime *runtime,
    const struct foc_runtime_sensor_slot *sensor,
    bool target_valid,
    bool sensor_fresh,
    uint32_t now_us
)
{
    if (!runtime->protect.enabled) {
        return;
    }
    const struct foc_pipeline *pipeline = &runtime->pipeline;
    const struct foc_vector_dq measured = pipeline->measured_dq_a;
    const bool reference = target_valid && sensor->velocity_ref_valid;
    const struct foc_protect_input input = {
        .now_us = now_us,
        .scope = runtime->mode == FOC_RUNTIME_MODE_CALIBRATION ? FOC_PROTECT_SCOPE_ELECTRICAL
            : foc_runtime_cascade_mode(runtime->mode)          ? FOC_PROTECT_SCOPE_MOTION
                                                               : FOC_PROTECT_SCOPE_TORQUE,
        .phase_current_a = pipeline->measurement.phase_current_a,
        .vbus_v = pipeline->measurement.core.vbus_v,
        .current_magnitude_a = sqrtf(measured.d * measured.d + measured.q * measured.q),
        .speed_rad_s = sensor->sample.mechanical_rad_s,
        .speed_ref_rad_s = reference ? sensor->velocity_ref_rad_s : 0.0f,
        .iq_ref_a = reference ? sensor->target.target_a.q : 0.0f,
        .speed_valid = runtime->config.motion.enabled && sensor->sample.valid && sensor_fresh,
        .reference_valid = reference,
    };
    const uint32_t trips = foc_protect_step(&runtime->protect, &runtime->protect_state, &input);
    if (trips == FOC_SAFE_FAULT_NONE) {
        return;
    }
    const unsigned int slot =
        atomic_load_explicit(&runtime->fault_snapshot_slot, memory_order_relaxed);
    if (!runtime->fault_snapshots[slot].valid) {
        const struct foc_vector_abc *phase = &input.phase_current_a;
        const struct foc_runtime_fault_snapshot snapshot = {
            .faults = trips,
            .h0_us = now_us,
            .mode = runtime->mode,
            .speed_rad_s = input.speed_valid ? input.speed_rad_s : 0.0f,
            .speed_ref_rad_s = input.speed_ref_rad_s,
            .measured_id_a = measured.d,
            .measured_iq_a = measured.q,
            .iq_ref_a = input.iq_ref_a,
            .phase_peak_a = fmaxf(fabsf(phase->a), fmaxf(fabsf(phase->b), fabsf(phase->c))),
            .vbus_v = input.vbus_v,
            .valid = true,
        };
        foc_runtime_publish_fault_snapshot(runtime, &snapshot);
    }
    foc_runtime_request_inhibit(runtime, trips);
}

static int
foc_runtime_control(struct foc_runtime *runtime, const struct foc_runtime_fast_input *input)
{
    struct foc_pipeline *pipeline = &runtime->pipeline;
    const struct foc_device_adc_raw *adc = &input->adc;
    const uint32_t now_us = input->now_us;
    /* A valid disabled mode change retains its existing control-free boundary.
     * Rejected actions must still observe this cycle's sample and Safety. */
    if (input->action == FOC_RUNTIME_ACTION_SET_MODE &&
        pipeline->safety.state == FOC_SAFE_STATE_DISABLED && pipeline->mock.output_confirmed &&
        pipeline->mock.last_request.action == FOC_DEVICE_OUTPUT_INHIBIT &&
        foc_runtime_mode_enabled(runtime, input->requested_mode)) {
        return foc_runtime_set_mode(runtime, input->requested_mode, now_us);
    }
    if (!foc_safe_sequence_newer(adc->sample_seq, runtime->adc.seq, runtime->adc.seen) ||
        !foc_safe_sequence_newer(adc->sample_tick, runtime->adc.produced_us, runtime->adc.seen) ||
        !foc_safe_timestamp_fresh(now_us, adc->sample_tick, UINT32_MAX)) {
        runtime->pipeline_status = foc_pipeline_request_inhibit(pipeline, now_us, 0U);
        return FOC_RUNTIME_ERR_ORDER;
    }
    runtime->interval_us = runtime->adc.seen ? adc->sample_tick - runtime->adc.produced_us : 0U;
    runtime->adc = (struct foc_pipeline_watermark){adc->sample_seq, adc->sample_tick, true};

    const unsigned int index = atomic_load_explicit(&runtime->sensor_slot, memory_order_acquire);
    const struct foc_runtime_sensor_slot sensor = index == FOC_RUNTIME_UNPUBLISHED
        ? (struct foc_runtime_sensor_slot){0}
        : runtime->sensor_slots[index];
    const float aligned = foc_runtime_align_angle(runtime, &sensor, input);
    runtime->observed_sensor = sensor.sample;
    runtime->observed_sensor.valid = isfinite(aligned) && runtime->config.motion.enabled;
    if (index == FOC_RUNTIME_UNPUBLISHED && runtime->config.motion.enabled &&
        pipeline->safety.state == FOC_SAFE_STATE_DISABLED &&
        input->action == FOC_RUNTIME_ACTION_NONE) {
        /* Sensor warm-up: after initialization (boot or a re-initialization that installs a
         * new commutation group) H1 needs its first usable encoder frames before any angle
         * exists. A DISABLED instance keeps INHIBIT and its heartbeats without counting
         * these angle-less samples toward the invalid-sample latch; ARM still requires a
         * valid angle and the latch applies again once a sensor is published. */
        foc_runtime_import_foreground(runtime, now_us);
        runtime->pipeline_status = foc_pipeline_request_inhibit(pipeline, now_us, 0U);
        if (pipeline->mock.output_confirmed) {
            (void)foc_safe_heartbeat_fast(&pipeline->safety, now_us);
        }
        return FOC_RUNTIME_ERR_ALIGNMENT;
    }
    const bool outer = foc_runtime_outer_mode(runtime->mode);
    const bool protection = outer && foc_runtime_protection_enabled(&runtime->config.motion);
    bool target_valid = outer && isfinite(aligned) && sensor.target_valid &&
        sensor.mode == runtime->mode && sensor.session == runtime->motion_session &&
        foc_safe_timestamp_fresh(
                            now_us,
                            sensor.target.produced_us,
                            pipeline->safety.config.command_timeout_us
        ) &&
        ((!protection && runtime->mode != FOC_RUNTIME_MODE_IMPEDANCE) ||
         !pipeline->rearm_boundary_seen ||
         (foc_safe_sequence_newer(sensor.target.produced_us, pipeline->rearm_after_us, true) &&
          foc_safe_sequence_newer(sensor.sample.captured_us, pipeline->rearm_after_us, true)));
    const bool running = pipeline->safety.state == FOC_SAFE_STATE_RUNNING;
    const bool monitor = protection && running && runtime->config.motion.following_error.enabled;
    enum foc_motion_rejection motion_refusal = FOC_MOTION_REJECT_NONE;
    if (protection) {
        motion_refusal =
            foc_runtime_motion_admission(runtime, &sensor, isfinite(aligned), target_valid);
        if (motion_refusal != FOC_MOTION_REJECT_NONE) {
            target_valid = false;
        }
    }
    /* CALIBRATION takes its angle and voltage from the latest drive command. */
    const bool calibration = runtime->mode == FOC_RUNTIME_MODE_CALIBRATION;
    const unsigned int drive_index =
        atomic_load_explicit(&runtime->command_slot, memory_order_acquire);
    const struct foc_runtime_command *drive = calibration &&
            drive_index != FOC_RUNTIME_UNPUBLISHED &&
            runtime->command_slots[drive_index].mode == FOC_RUNTIME_MODE_CALIBRATION
        ? &runtime->command_slots[drive_index]
        : NULL;
    const struct foc_pipeline_input step = {
        .adc = *adc,
        /* Without a drive command (e.g. after STOP) the cycle observes like CURRENT does. */
        .theta_elec_rad = drive == NULL ? aligned
            : drive->target.calibration.follow_rotor
            ? aligned + drive->target.calibration.theta_elec_rad
            : drive->target.calibration.theta_elec_rad,
        .voltage_drive = calibration,
        .angle_sample_us = adc->sample_tick,
        .now_us = now_us,
        .dt_s = (float)runtime->interval_us * 1.0e-6f,
        .electrical_feedback =
            {
                .state = sensor.electrical,
                .source_us = sensor.sample.captured_us,
                .source_seq = sensor.sample.seq,
                .valid = runtime->config.motion.enabled && sensor.sample.valid,
            },
        .cycle_target = target_valid ? &sensor.target : NULL,
        .require_cycle_target = outer,
    };
    foc_runtime_import_foreground(runtime, now_us);
    if (outer) {
        foc_runtime_import_command(runtime, step.cycle_target, now_us);
    } else if (calibration) {
        if (drive != NULL) {
            const struct foc_pipeline_command source =
                {drive->target.calibration.voltage_v, drive->seq, drive->produced_us};
            foc_runtime_import_command(runtime, &source, now_us);
        }
    } else {
        const unsigned int command_index =
            atomic_load_explicit(&runtime->command_slot, memory_order_acquire);
        if (command_index != FOC_RUNTIME_UNPUBLISHED &&
            runtime->command_slots[command_index].mode == FOC_RUNTIME_MODE_CURRENT) {
            const struct foc_runtime_command *command = &runtime->command_slots[command_index];
            const struct foc_pipeline_command source =
                {command->target.current_a, command->seq, command->produced_us};
            foc_runtime_import_command(runtime, &source, now_us);
        }
    }
    foc_runtime_hook(runtime, FOC_RUNTIME_HOOK_H0_INPUTS);
    if (foc_runtime_drain_stop(runtime, now_us)) {
        return FOC_RUNTIME_ERR_STOP;
    }
    if (motion_refusal != FOC_MOTION_REJECT_NONE &&
        (running || input->action == FOC_RUNTIME_ACTION_ARM ||
         input->action == FOC_RUNTIME_ACTION_START)) {
        runtime->motion_rejection = motion_refusal;
    }
    struct foc_runtime_output_admission admission = {.runtime = runtime};
    struct foc_following_error_state following_trial = {0};
    if (monitor) {
        const enum foc_following_state following = foc_runtime_following_trial(
            runtime,
            &sensor,
            target_valid,
            isfinite(aligned) &&
                sensor.sample.mechanical_frame_id ==
                    runtime->config.motion.limits.mechanical_frame_id,
            &following_trial
        );
        admission.following_stop = following == FOC_FOLLOWING_TRIGGERED && target_valid;
        if (!target_valid) {
            /* Record missing evidence, but do not accrue time for a rejected proposal. */
            runtime->following = following_trial;
        }
    }
    /* Decide before observation can disable/fault the controller. Never reinterpret
     * a rejected ARM as eligible after an invalid sample changes RUNNING to DISABLED. */
    const bool action_valid = foc_runtime_action_valid(input->action, pipeline->safety.state);

    const int status =
        foc_pipeline_step_guarded(pipeline, &step, foc_runtime_admit_update, &admission);
    runtime->pipeline_status = status;
    if (status == FOC_PIPELINE_OK && running) {
        /* A trip joins the stop drained below, which latches it and inhibits. */
        foc_runtime_protect(runtime, &sensor, target_valid, isfinite(aligned), now_us);
    }
    foc_runtime_hook(runtime, FOC_RUNTIME_HOOK_AFTER_CONTROL);
    const bool stopped = foc_runtime_drain_stop(runtime, now_us);
    if (pipeline->mock.output_confirmed) {
        (void)foc_safe_heartbeat_fast(&pipeline->safety, now_us);
    }
    if (status == FOC_PIPELINE_OK) {
        if (target_valid && !stopped) {
            runtime->accepted_outer_seq = sensor.sample.seq;
            runtime->accepted_outer_seen = true;
            if (monitor) {
                runtime->following = following_trial;
            }
        }
        return action_valid ? FOC_RUNTIME_OK : FOC_RUNTIME_ERR_CONTROL;
    }
    if (admission.following_denied) {
        runtime->following = following_trial;
        runtime->motion_rejection = FOC_MOTION_REJECT_FOLLOWING;
    }
    if (stopped || admission.following_denied || status == FOC_PIPELINE_ERR_STOP) {
        runtime->pipeline_status = status;
        return FOC_RUNTIME_ERR_STOP;
    }
    if (!action_valid) {
        return FOC_RUNTIME_ERR_CONTROL;
    }
    if (input->action == FOC_RUNTIME_ACTION_CLEAR_FAULT ||
        (input->action != FOC_RUNTIME_ACTION_NONE && status == FOC_PIPELINE_ERR_STATE)) {
        if (input->action != FOC_RUNTIME_ACTION_CLEAR_FAULT) {
            if (!foc_runtime_mode_enabled(runtime, runtime->mode)) {
                runtime->motion_rejection = FOC_MOTION_REJECT_UNSUPPORTED_MODE;
                return FOC_RUNTIME_ERR_MOTION_MODE;
            }
            /* A valid projection of a pre-stop angle still belongs to the old session. */
            if (runtime->config.motion.enabled && pipeline->rearm_boundary_seen &&
                !foc_safe_sequence_newer(
                    sensor.sample.captured_us,
                    pipeline->rearm_after_us,
                    true
                )) {
                runtime->pipeline_status = FOC_PIPELINE_ERR_SAMPLE_ORDER;
                return FOC_RUNTIME_ERR_CONTROL;
            }
            if (outer && !target_valid) {
                runtime->pipeline_status = FOC_PIPELINE_ERR_COMMAND;
                return FOC_RUNTIME_ERR_CONTROL;
            }
        }
        runtime->pipeline_status = foc_runtime_lifecycle(runtime, input->action, now_us);
        if (runtime->pipeline_status == FOC_PIPELINE_OK &&
            input->action == FOC_RUNTIME_ACTION_CLEAR_FAULT) {
            (void)foc_safe_heartbeat_fast(&pipeline->safety, now_us);
        }
        return runtime->pipeline_status == FOC_PIPELINE_OK ? FOC_RUNTIME_IDLE
                                                           : FOC_RUNTIME_ERR_CONTROL;
    }
    if (runtime->config.motion.enabled && !isfinite(aligned) && status == FOC_PIPELINE_ERR_SAMPLE) {
        return FOC_RUNTIME_ERR_ALIGNMENT;
    }
    return status == FOC_PIPELINE_ERR_STATE ? FOC_RUNTIME_IDLE : FOC_RUNTIME_ERR_CONTROL;
}

static void foc_runtime_publish_foreground(struct foc_runtime *runtime, uint32_t now_us)
{
    atomic_store_explicit(&runtime->foreground_us, now_us, memory_order_release);
    atomic_store_explicit(&runtime->foreground_seen, 1U, memory_order_release);
}

static int foc_runtime_supervise(
    struct foc_runtime *runtime,
    uint32_t now_us,
    struct foc_runtime_foreground_result *result
)
{
    const struct foc_safe_config config = runtime->pipeline.safety.config;
    const bool previous_seen =
        atomic_load_explicit(&runtime->foreground_seen, memory_order_relaxed) != 0U;
    const uint32_t previous = previous_seen
        ? atomic_load_explicit(&runtime->foreground_us, memory_order_relaxed)
        : runtime->boot_us;
    if (!foc_safe_timestamp_fresh(now_us, previous, config.foreground_heartbeat_timeout_us)) {
        foc_runtime_request_inhibit(runtime, FOC_SAFE_FAULT_FOREGROUND_HEARTBEAT);
    }

    struct foc_runtime_snapshot snapshot;
    const int status =
        foc_runtime_copy_snapshot(runtime, &snapshot, FOC_RUNTIME_HOOK_CONTROL_SNAPSHOT_READING);
    if (status != FOC_RUNTIME_OK) {
        return status;
    }
    if (snapshot.fast_seen && !foc_safe_timestamp_fresh(now_us, snapshot.fast_us, UINT32_MAX)) {
        /* The caller's time precedes a newer H0 publication. Try on the next
         * scheduled poll and withhold heartbeat and watchdog permission here. */
        return FOC_RUNTIME_ERR_BUSY;
    }

    if (snapshot.fast_seen ||
        !foc_safe_timestamp_fresh(now_us, runtime->boot_us, config.fast_heartbeat_timeout_us)) {
        struct foc_safe_context view = {
            .config = config,
            .state = snapshot.state,
            .latched_faults = snapshot.faults,
            .last_fast_heartbeat_us = snapshot.fast_us,
            .last_command_us = snapshot.command_us,
            .last_sample_us = snapshot.sample_us,
            .fast_heartbeat_seen = snapshot.fast_seen,
            .command_seen = snapshot.command_seen,
            .sample_seen = snapshot.sample_seen,
        };
        (void)foc_safe_heartbeat_foreground(&view, now_us);
        const int permission = foc_safe_watchdog_permitted(&view, now_us);
        const uint32_t new_faults = view.latched_faults & ~snapshot.faults;
        if (new_faults != FOC_SAFE_FAULT_NONE) {
            foc_runtime_request_inhibit(runtime, new_faults);
        }
        result->watchdog_permitted = permission == FOC_SAFE_STATUS_OK &&
            snapshot.output_confirmed &&
            (snapshot.state != FOC_SAFE_STATE_FAULT_LATCHED ||
             snapshot.action == FOC_DEVICE_OUTPUT_INHIBIT) &&
            atomic_load(&runtime->stop_requests) == 0U;
    }

    result->supervised = true;
    foc_runtime_publish_foreground(runtime, now_us);
    return FOC_RUNTIME_OK;
}

static float foc_runtime_impedance_damping_error(
    struct foc_runtime *runtime,
    const struct foc_runtime_sensor *sample,
    const struct foc_runtime_sensor *previous,
    float error
)
{
    if (previous != NULL && sample->seq - previous->seq == 1U) {
        const uint32_t dt_us = sample->captured_us - previous->captured_us;
        const uint32_t max_gap_us = runtime->config.h1.period_us + runtime->config.h1.max_delay_us;
        if (dt_us <= max_gap_us) {
            /* H1 has already admitted the sample and ordered source time across wrap. */
            const float omega_dt = (6.283185307179586f * 1.0e-6f) *
                runtime->config.motion.impedance.damping_filter_hz * (float)dt_us;
            const float alpha = omega_dt / (1.0f + omega_dt);
            error = (1.0f - alpha) * runtime->damping_speed_error_rad_s + alpha * error;
        }
    }
    runtime->damping_speed_error_rad_s = error;
    runtime->damping_filter_seen = isfinite(error);
    return error;
}

static bool foc_runtime_impedance_target(
    struct foc_runtime *runtime,
    const struct foc_runtime_impedance_command *reference,
    const struct foc_runtime_sensor *sample,
    const struct foc_runtime_sensor *damping_previous,
    uint32_t session,
    bool running,
    float *iq
)
{
    const struct foc_runtime_motion_config *motion = &runtime->config.motion;
    const struct foc_runtime_impedance_config *policy = &motion->impedance;
    if (reference->source_session != session ||
        sample->mechanical_frame_id != policy->mechanical_frame_id ||
        fabsf(sample->mechanical_rad) > FOC_RUNTIME_IMPEDANCE_POSITION_LIMIT_RAD) {
        return false;
    }
    const struct foc_impedance_config config = {
        .stiffness_nm_per_rad = reference->stiffness_nm_per_rad,
        .damping_nm_s_per_rad = reference->damping_nm_s_per_rad,
        .torque_constant_nm_per_a = policy->torque_constant_nm_per_a,
        .torque_limit_nm = policy->torque_limit_nm,
    };
    struct foc_impedance_input input = {
        .position_error_rad = reference->position_rad - sample->mechanical_rad,
        .reference_velocity_rad_s = reference->velocity_rad_s,
        .measured_velocity_rad_s = sample->mechanical_rad_s,
        .torque_feedforward_nm = reference->torque_feedforward_nm,
        .live_iq_limit_a = motion->iq_limit_a,
    };
    if (running && policy->damping_filter_hz > 0.0f) {
        const float damping_error = foc_runtime_impedance_damping_error(
            runtime,
            sample,
            damping_previous,
            reference->velocity_rad_s - sample->mechanical_rad_s
        );
        /* Core uses the speed difference. Supply it in a zero-reference frame. */
        input.reference_velocity_rad_s = 0.0f;
        input.measured_velocity_rad_s = -damping_error;
    }
    struct foc_impedance_output output;
    if (foc_impedance_step(&config, &input, &output) != FOC_STATUS_OK || !output.valid) {
        runtime->damping_filter_seen = false;
        return false;
    }
    *iq = output.iq_ref_a;
    return true;
}

#if defined(FOC_RUNTIME_HOST_COGGING)
static bool foc_runtime_cogging_valid(const struct foc_runtime_motion_config *motion)
{
    const struct foc_runtime_cogging_config *config = &motion->cogging;
    const struct foc_runtime_cogging_lut *lut = &config->lut;
    if (!motion->enabled || !config->enabled || !lut->qualified || config->motor_id == 0U ||
        config->motor_id != lut->motor_id || config->mechanical_frame_id == 0U ||
        config->mechanical_frame_id != lut->mechanical_frame_id || lut->training_capture_id == 0U ||
        lut->holdout_capture_id == 0U || lut->training_capture_id == lut->holdout_capture_id ||
        !isfinite(config->torque_constant_nm_per_a) || config->torque_constant_nm_per_a <= 0.0f ||
        !isfinite(config->iq_limit_a) || config->iq_limit_a <= 0.0f ||
        config->iq_limit_a > motion->iq_limit_a) {
        return false;
    }
    for (unsigned int i = 0U; i < FOC_RUNTIME_COGGING_POINTS; ++i) {
        const float iq = lut->torque_nm[i] / config->torque_constant_nm_per_a;
        if (!isfinite(iq) || fabsf(iq) > config->iq_limit_a) {
            return false;
        }
    }
    return true;
}

static float foc_runtime_add_cogging(
    const struct foc_runtime_motion_config *motion,
    const struct foc_runtime_sensor *sample,
    const struct foc_runtime_command *command,
    float iq
)
{
    const struct foc_runtime_cogging_config *config = &motion->cogging;
    /* Continuous Float32 position loses phase resolution over long travel. */
    if (!config->enabled || sample->mechanical_frame_id != config->mechanical_frame_id ||
        !isfinite(sample->mechanical_rad) || fabsf(sample->mechanical_rad) > 4096.0f) {
        return iq;
    }
    const float two_pi = 6.28318530717958647692f;
    float phase = fmodf(sample->mechanical_rad, two_pi);
    if (phase < 0.0f) {
        phase += two_pi;
    }
    const float knot = phase * ((float)FOC_RUNTIME_COGGING_POINTS / two_pi);
    const unsigned int lower = (unsigned int)knot;
    const unsigned int index = lower % FOC_RUNTIME_COGGING_POINTS;
    const unsigned int next = (index + 1U) % FOC_RUNTIME_COGGING_POINTS;
    const float blend = knot - (float)lower;
    const float torque =
        (1.0f - blend) * config->lut.torque_nm[index] + blend * config->lut.torque_nm[next];
    float correction = torque / config->torque_constant_nm_per_a;
    float cap = motion->iq_limit_a;
    if (command->mode == FOC_RUNTIME_MODE_IMPEDANCE) {
        const struct foc_runtime_impedance_config *policy = &motion->impedance;
        const float commanded = command->target.impedance.torque_feedforward_nm;
        const float kt = policy->torque_constant_nm_per_a;
        const float lower_headroom = (-policy->max_feedforward_nm - commanded) / kt;
        const float upper_headroom = (policy->max_feedforward_nm - commanded) / kt;
        correction = fmaxf(lower_headroom, fminf(upper_headroom, correction));
        cap = fminf(cap, policy->torque_limit_nm / kt);
    }
    return fmaxf(-cap, fminf(cap, iq + correction));
}
#endif

static void foc_runtime_prepare_outer(
    struct foc_runtime *runtime,
    struct foc_runtime_sensor_slot *candidate,
    const struct foc_runtime_sensor_slot *previous,
    const struct foc_runtime_snapshot *view,
    uint32_t now_us
)
{
    const bool same_session = previous != NULL && previous->session == view->motion_session;
    const bool running = view->state == FOC_SAFE_STATE_RUNNING && !view->inhibit_pending;
    const bool trajectory_enabled = runtime->config.motion.trajectory.enabled;
    const struct foc_runtime_sensor *damping_previous =
        same_session && runtime->damping_filter_seen ? &previous->sample : NULL;
    runtime->damping_filter_seen = false;
    if (!same_session || !running) {
        (void)foc_velocity_reset(&runtime->outer_velocity);
        if (trajectory_enabled && runtime->trajectory.initialized) {
            (void)foc_trajectory_reset(&runtime->trajectory, 0.0f);
        }
        runtime->outer_trial_seen = false;
    }
    candidate->mode = view->mode;
    candidate->session = view->motion_session;
    if (candidate->sample.valid && running && runtime->outer_trial_seen &&
        view->accepted_outer_seen && previous->sample.seq == view->accepted_outer_seq) {
        runtime->outer_velocity.integral_a = runtime->outer_trial_a;
        if (trajectory_enabled) {
            runtime->trajectory = runtime->trajectory_trial;
        }
    }
    runtime->outer_trial_seen = false;

    const unsigned int index = atomic_load_explicit(&runtime->command_slot, memory_order_acquire);
    if (!candidate->sample.valid || !foc_runtime_outer_mode(view->mode) ||
        index == FOC_RUNTIME_UNPUBLISHED || view->inhibit_pending ||
        view->state == FOC_SAFE_STATE_FAULT_LATCHED) {
        return;
    }
    /* Foreground cannot overwrite this slot while H1 is running. */
    const struct foc_runtime_command *command = &runtime->command_slots[index];
    if (command->mode != view->mode ||
        !foc_safe_timestamp_fresh(
            now_us,
            command->produced_us,
            runtime->pipeline.safety.config.command_timeout_us
        )) {
        return;
    }

    const struct foc_runtime_motion_config *policy = &runtime->config.motion;
    const bool protection = foc_runtime_protection_enabled(policy);
    if (protection) {
        if (runtime->command_sessions[index] != view->motion_session) {
            return;
        }
        if (candidate->sample.mechanical_frame_id != policy->limits.mechanical_frame_id) {
            candidate->motion_rejection = FOC_MOTION_REJECT_FRAME;
            return;
        }
        candidate->motion_request.has_position = command->mode != FOC_RUNTIME_MODE_VELOCITY;
        if (command->mode == FOC_RUNTIME_MODE_IMPEDANCE) {
            candidate->motion_request.position_ref_rad = command->target.impedance.position_rad;
            candidate->motion_request.velocity_ref_rad_s = command->target.impedance.velocity_rad_s;
        } else if (command->mode == FOC_RUNTIME_MODE_POSITION) {
            candidate->motion_request.position_ref_rad = command->target.motion_ref;
        } else {
            candidate->motion_request.requested_velocity_rad_s = command->target.motion_ref;
        }
    }
    float iq = 0.0f;
    if (running && !same_session) {
        return;
    }
    if (view->mode == FOC_RUNTIME_MODE_IMPEDANCE) {
        if (!foc_runtime_impedance_target(
                runtime,
                &command->target.impedance,
                &candidate->sample,
                damping_previous,
                view->motion_session,
                running,
                &iq
            )) {
            return;
        }
        /* A protected inactive proposal must first pass the direction check. */
        if (!running && !protection) {
            iq = 0.0f;
        }
    } else if (running) {
        const float speed_limit = policy->position.velocity_limit_rad_s;
        float speed = fmaxf(-speed_limit, fminf(command->target.motion_ref, speed_limit));
        const float dt_s =
            (float)(candidate->sample.captured_us - previous->sample.captured_us) * 1.0e-6f;
        if (trajectory_enabled) {
            struct foc_trajectory_output trajectory_output;
            struct foc_trajectory_context trajectory_trial = runtime->trajectory;
            const int trajectory_status = command->mode == FOC_RUNTIME_MODE_POSITION
                ? foc_trajectory_track_step(
                      &trajectory_trial,
                      command->target.motion_ref,
                      candidate->sample.mechanical_rad,
                      dt_s,
                      &trajectory_output
                  )
                : foc_trajectory_velocity_step(
                      &trajectory_trial,
                      command->target.motion_ref,
                      dt_s,
                      &trajectory_output
                  );
            if (trajectory_status != FOC_TRAJECTORY_STATUS_OK) {
                return;
            }
            runtime->trajectory_trial = trajectory_trial;
            speed = trajectory_output.velocity_ref_rad_s;
            if (trajectory_output.position_valid) {
                /* Position tracks the planned reference with its velocity as feedforward. */
                speed += policy->position.kp_per_s *
                    (trajectory_output.position_ref_rad - candidate->sample.mechanical_rad);
                speed = fmaxf(-speed_limit, fminf(speed, speed_limit));
            }
        } else if (command->mode == FOC_RUNTIME_MODE_POSITION) {
            struct foc_position_output output;
            if (foc_position_step(
                    &policy->position,
                    command->target.motion_ref,
                    candidate->sample.mechanical_rad,
                    &output
                ) != FOC_STATUS_OK) {
                return;
            }
            speed = output.velocity_ref_rad_s;
        }
        struct foc_velocity_context trial = runtime->outer_velocity;
        struct foc_velocity_output output;
        const struct foc_velocity_input input = {
            .ref_rad_s = speed,
            .measured_rad_s = candidate->sample.mechanical_rad_s,
            .dt_s = dt_s,
            .iq_limit_a = policy->iq_limit_a,
            .enabled = true,
        };
        if (foc_velocity_step(&trial, &input, &output) != FOC_STATUS_OK) {
            return;
        }
        runtime->outer_trial_a = trial.integral_a;
        runtime->outer_trial_seen = true;
        iq = output.iq_ref_a;
        candidate->velocity_ref_rad_s = speed;
        candidate->velocity_ref_valid = true;
        if (protection) {
            candidate->motion_request.velocity_ref_rad_s = speed;
        }
    }
    if (protection) {
        candidate->motion_request.iq_ref_a = iq;
        candidate->motion_rejection = foc_motion_limits_check(
            &policy->limits,
            candidate->sample.mechanical_rad,
            &candidate->motion_request
        );
        if (candidate->motion_rejection != FOC_MOTION_REJECT_NONE) {
            runtime->outer_trial_seen = false;
            return;
        }
        if (!running) {
            iq = 0.0f;
        }
    }
#if defined(FOC_RUNTIME_HOST_COGGING)
    if (running) {
        const float compensated = foc_runtime_add_cogging(policy, &candidate->sample, command, iq);
        if (protection && compensated != iq) {
            /* A feedforward proposal must preserve the base command's admission. */
            struct foc_motion_limits_request trial = candidate->motion_request;
            trial.iq_ref_a = compensated;
            if (foc_motion_limits_check(
                    &policy->limits,
                    candidate->sample.mechanical_rad,
                    &trial
                ) == FOC_MOTION_REJECT_NONE) {
                candidate->motion_request = trial;
                iq = compensated;
            }
        } else {
            iq = compensated;
        }
    }
#endif
    candidate->target =
        (struct foc_pipeline_command){{0.0f, iq}, command->seq, command->produced_us};
    candidate->target_valid = true;
}

static int foc_runtime_publish_reference(
    struct foc_runtime *runtime,
    const struct foc_runtime_command *command,
    uint32_t now_us
)
{
    const struct foc_runtime_motion_config *motion = &runtime->config.motion;
    const bool protection =
        foc_runtime_outer_mode(command->mode) && foc_runtime_protection_enabled(motion);
    uint32_t source_session = 0U;
    if (protection || (motion->impedance.enabled && command->mode != FOC_RUNTIME_MODE_IMPEDANCE)) {
        struct foc_runtime_snapshot snapshot;
        const int status = foc_runtime_copy_snapshot(runtime, &snapshot, FOC_RUNTIME_HOOK_NONE);
        if (status != FOC_RUNTIME_OK) {
            return status;
        }
        if (protection) {
            if (command->mode != snapshot.mode || snapshot.inhibit_pending ||
                snapshot.state == FOC_SAFE_STATE_FAULT_LATCHED) {
                return FOC_RUNTIME_ERR_CONTROL;
            }
            source_session = snapshot.motion_session;
            if (command->mode == FOC_RUNTIME_MODE_IMPEDANCE &&
                command->target.impedance.source_session != source_session) {
                return FOC_RUNTIME_ERR_ORDER;
            }
        } else if (snapshot.mode == FOC_RUNTIME_MODE_IMPEDANCE) {
            return FOC_RUNTIME_ERR_CONTROL;
        }
    }
    if (motion->impedance.enabled || protection) {
        /* Opt-in instances keep source order across modes and slot revocation. */
        const struct foc_pipeline_watermark *published = &runtime->published_command;
        if (published->seen &&
            (!foc_safe_sequence_newer(command->seq, published->seq, true) ||
             !foc_safe_timestamp_fresh(command->produced_us, published->produced_us, UINT32_MAX))) {
            return FOC_RUNTIME_ERR_ORDER;
        }
    }
    const unsigned int index = atomic_load_explicit(&runtime->command_slot, memory_order_relaxed);
    if (!foc_safe_timestamp_fresh(
            now_us,
            command->produced_us,
            runtime->pipeline.safety.config.command_timeout_us
        ) ||
        (index != FOC_RUNTIME_UNPUBLISHED &&
         (!foc_safe_sequence_newer(command->seq, runtime->command_slots[index].seq, true) ||
          !foc_safe_timestamp_fresh(
              command->produced_us,
              runtime->command_slots[index].produced_us,
              UINT32_MAX
          )))) {
        return FOC_RUNTIME_ERR_ORDER;
    }
    const unsigned int next = index == 0U ? 1U : 0U;
    runtime->command_slots[next].target = command->target;
    foc_runtime_hook(runtime, FOC_RUNTIME_HOOK_COMMAND_WRITING);
    runtime->command_slots[next] = *command;
    if (protection) {
        runtime->command_sessions[next] = source_session;
    }
    runtime->published_command =
        (struct foc_pipeline_watermark){command->seq, command->produced_us, true};
    atomic_store_explicit(&runtime->command_slot, next, memory_order_release);
    return FOC_RUNTIME_OK;
}

static bool foc_runtime_impedance_policy_valid(const struct foc_runtime_motion_config *motion)
{
    const struct foc_runtime_impedance_config *policy = &motion->impedance;
    return !policy->enabled ||
        (motion->enabled && policy->mechanical_frame_id != 0U &&
         isfinite(policy->torque_constant_nm_per_a) && policy->torque_constant_nm_per_a > 0.0f &&
         isfinite(policy->torque_limit_nm) && policy->torque_limit_nm > 0.0f &&
         isfinite(policy->max_stiffness_nm_per_rad) && policy->max_stiffness_nm_per_rad >= 0.0f &&
         isfinite(policy->max_damping_nm_s_per_rad) && policy->max_damping_nm_s_per_rad >= 0.0f &&
         isfinite(policy->max_feedforward_nm) && policy->max_feedforward_nm >= 0.0f &&
         (policy->damping_filter_hz == 0.0f ||
          (policy->damping_filter_hz >= 1.0f && policy->damping_filter_hz <= 200.0f)) &&
         isfinite(policy->torque_constant_nm_per_a * motion->iq_limit_a));
}

static bool foc_runtime_protection_valid(const struct foc_runtime_motion_config *motion)
{
    if (!foc_runtime_protection_enabled(motion)) {
        return true;
    }
    return motion->enabled && motion->limits.mechanical_frame_id != 0U &&
        foc_motion_limits_validate(&motion->limits) &&
        foc_following_error_validate(&motion->following_error) &&
        (!motion->impedance.enabled ||
         motion->impedance.mechanical_frame_id == motion->limits.mechanical_frame_id);
}

/* Tuning ---------------------------------------------------------------------- */

static bool foc_runtime_ramp_value_valid(float value)
{
    return isfinite(value) &&
        (value == 0.0f ||
         (value >= FOC_RUNTIME_RAMP_MIN_RAD_S2 && value <= FOC_RUNTIME_RAMP_MAX_RAD_S2));
}

/* The trajectory policy IDs 106/107 select: 0 is unlimited, both 0 disable shaping. */
static bool foc_runtime_ramp_trajectory(
    float accel_rad_s2,
    float decel_rad_s2,
    float velocity_limit_rad_s,
    float max_dt_s,
    struct foc_trajectory_config *trajectory
)
{
    if (!foc_runtime_ramp_value_valid(accel_rad_s2) ||
        !foc_runtime_ramp_value_valid(decel_rad_s2)) {
        return false;
    }
    *trajectory = (struct foc_trajectory_config){
        .enabled = accel_rad_s2 > 0.0f || decel_rad_s2 > 0.0f,
        .max_velocity_rad_s = velocity_limit_rad_s,
        .max_accel_rad_s2 = accel_rad_s2 > 0.0f ? accel_rad_s2 : FOC_RUNTIME_RAMP_UNLIMITED_RAD_S2,
        .max_decel_rad_s2 = decel_rad_s2 > 0.0f ? decel_rad_s2 : FOC_RUNTIME_RAMP_UNLIMITED_RAD_S2,
        .max_dt_s = max_dt_s,
    };
    struct foc_trajectory_context check;
    return !trajectory->enabled ||
        foc_trajectory_init(&check, trajectory) == FOC_TRAJECTORY_STATUS_OK;
}

/* The same checks foc_runtime_init() applies, on candidate copies only. */
static int foc_runtime_tuning_check(
    const struct foc_runtime *runtime,
    const struct foc_runtime_tuning *tuning,
    struct foc_current_context *current,
    struct foc_velocity_context *velocity,
    struct foc_protect_config *protect
)
{
    if (tuning != NULL &&
        (!foc_runtime_profile_valid(
             &tuning->profile,
             tuning->current_limit_a,
             tuning->velocity_limit_rad_s
         ) ||
         !foc_runtime_trip_resolve(
             runtime->config.trip_enabled,
             &tuning->trips,
             &tuning->profile,
             tuning->current_limit_a,
             runtime->config.motion.enabled ? tuning->iq_limit_a : 0.0f,
             tuning->velocity_limit_rad_s,
             protect
         ))) {
        return FOC_RUNTIME_ERR_CONFIG;
    }
    if (tuning == NULL || runtime->pipeline.feedforward.enabled ||
        runtime->config.motion.impedance.enabled || !isfinite(tuning->current_limit_a) ||
        (!runtime->config.motion.enabled &&
         (tuning->accel_rad_s2 != 0.0f || tuning->decel_rad_s2 != 0.0f)) ||
        tuning->command_timeout_us < FOC_RUNTIME_LEASE_MIN_US ||
        tuning->command_timeout_us > FOC_RUNTIME_LEASE_MAX_US ||
        tuning->command_timeout_us < runtime->pipeline.safety.config.sample_timeout_us) {
        return FOC_RUNTIME_ERR_CONFIG;
    }
    struct foc_current_config inner = runtime->pipeline.current.config;
    inner.kp_v_per_a = tuning->current_kp_v_per_a;
    inner.ki_v_per_a_s = tuning->current_ki_v_per_a_s;
    inner.current_limit_a = tuning->current_limit_a;
    if (foc_current_init(current, &inner) != FOC_STATUS_OK) {
        return FOC_RUNTIME_ERR_CONFIG;
    }
    *velocity = runtime->outer_velocity;
    if (!runtime->config.motion.enabled) {
        return FOC_RUNTIME_OK;
    }
    struct foc_velocity_config outer = runtime->config.motion.velocity;
    outer.kp_a_per_rad_s = tuning->velocity_kp_a_per_rad_s;
    outer.ki_a_per_rad = tuning->velocity_ki_a_per_rad;
    const struct foc_position_config position = {
        .kp_per_s = tuning->position_kp_per_s,
        .velocity_limit_rad_s = tuning->velocity_limit_rad_s,
    };
    struct foc_position_output check;
    struct foc_trajectory_config ramp;
    if (!isfinite(tuning->iq_limit_a) || tuning->iq_limit_a <= 0.0f ||
        tuning->iq_limit_a > tuning->current_limit_a ||
        foc_position_step(&position, 0.0f, 0.0f, &check) != FOC_STATUS_OK ||
        foc_velocity_init(velocity, &outer) != FOC_STATUS_OK ||
        !foc_runtime_ramp_trajectory(
            tuning->accel_rad_s2,
            tuning->decel_rad_s2,
            tuning->velocity_limit_rad_s,
            outer.max_dt_s,
            &ramp
        )) {
        return FOC_RUNTIME_ERR_CONFIG;
    }
    return FOC_RUNTIME_OK;
}

/* Commit a checked candidate. Callers own the instance: H0, or boot before any IRQ. */
static void foc_runtime_tuning_commit(
    struct foc_runtime *runtime,
    const struct foc_runtime_tuning *tuning,
    const struct foc_current_context *current,
    const struct foc_velocity_context *velocity,
    const struct foc_protect_config *protect
)
{
    runtime->pipeline.current = *current;
    runtime->pipeline.safety.config.command_timeout_us = tuning->command_timeout_us;
    if (runtime->config.trip_enabled) {
        runtime->config.trips = tuning->trips;
        runtime->protect = *protect;
    }
    runtime->config.profile = tuning->profile;
    if (runtime->config.motion.enabled) {
        runtime->outer_velocity = *velocity;
        runtime->config.motion.velocity = velocity->config;
        runtime->config.motion.position.kp_per_s = tuning->position_kp_per_s;
        runtime->config.motion.position.velocity_limit_rad_s = tuning->velocity_limit_rad_s;
        runtime->config.motion.iq_limit_a = tuning->iq_limit_a;
        /* A ramp, or leaving one, replaces the trajectory policy; both zero before and
         * after keep an explicit policy. A new session resets the planner anyway. */
        struct foc_runtime_motion_config *motion = &runtime->config.motion;
        struct foc_trajectory_config ramp;
        if ((tuning->accel_rad_s2 != 0.0f || tuning->decel_rad_s2 != 0.0f ||
             motion->accel_rad_s2 != 0.0f || motion->decel_rad_s2 != 0.0f) &&
            foc_runtime_ramp_trajectory(
                tuning->accel_rad_s2,
                tuning->decel_rad_s2,
                tuning->velocity_limit_rad_s,
                motion->velocity.max_dt_s,
                &ramp
            )) {
            motion->trajectory = ramp;
            runtime->trajectory = (struct foc_trajectory_context){0};
            if (ramp.enabled) {
                (void)foc_trajectory_init(&runtime->trajectory, &ramp);
                (void)foc_trajectory_reset(&runtime->trajectory, 0.0f);
            }
            runtime->trajectory_trial = runtime->trajectory;
        }
        motion->accel_rad_s2 = tuning->accel_rad_s2;
        motion->decel_rad_s2 = tuning->decel_rad_s2;
    }
    runtime->tuning_revision++;
}

static void foc_runtime_tuning_write_result(
    struct foc_runtime *runtime,
    enum foc_runtime_request_result result,
    uint32_t now_us,
    int status
)
{
    const unsigned int current =
        atomic_load_explicit(&runtime->tuning_result_slot, memory_order_relaxed);
    const unsigned int next = current == 0U ? 1U : 0U;
    runtime->tuning_results[next] = (struct foc_runtime_tuning_result){
        .result = result,
        .request_sequence = runtime->tuning_request_sequence,
        .revision = runtime->tuning_revision,
        .h0_us = now_us,
        .status = status,
    };
    atomic_store_explicit(&runtime->tuning_result_slot, next, memory_order_release);
}

/* H0 only, after the cycle. A candidate takes effect from the next cycle. H1 reads the
 * outer gains only while RUNNING, which cannot begin in the cycle that applies them. */
static void foc_runtime_tuning_service(struct foc_runtime *runtime, uint32_t now_us, bool cancel)
{
    unsigned int expected = FOC_RUNTIME_REQUEST_PUBLISHED;
    if (!atomic_compare_exchange_strong_explicit(
            &runtime->tuning_pending,
            &expected,
            FOC_RUNTIME_REQUEST_CLAIMED,
            memory_order_acquire,
            memory_order_relaxed
        )) {
        return;
    }
    const struct foc_pipeline *pipeline = &runtime->pipeline;
    const struct foc_runtime_tuning tuning = runtime->tuning_request;
    enum foc_runtime_request_result result = FOC_RUNTIME_REQUEST_REJECTED;
    int status = FOC_RUNTIME_ERR_CONTROL;
    struct foc_current_context current;
    struct foc_velocity_context velocity;
    struct foc_protect_config protect;
    if (cancel || atomic_load(&runtime->stop_requests) != 0U) {
        result = FOC_RUNTIME_REQUEST_CANCELLED;
    } else if (
        pipeline->safety.state == FOC_SAFE_STATE_DISABLED && pipeline->mock.output_confirmed &&
        pipeline->mock.last_request.action == FOC_DEVICE_OUTPUT_INHIBIT
    ) {
        status = foc_runtime_tuning_check(runtime, &tuning, &current, &velocity, &protect);
        if (status == FOC_RUNTIME_OK) {
            foc_runtime_tuning_commit(runtime, &tuning, &current, &velocity, &protect);
            result = FOC_RUNTIME_REQUEST_APPLIED;
        }
    }
    foc_runtime_tuning_write_result(runtime, result, now_us, status);
    atomic_store_explicit(&runtime->tuning_pending, FOC_RUNTIME_REQUEST_IDLE, memory_order_release);
}

/* Public Functions ------------------------------------------------------------- */

int foc_runtime_read_tuning(const struct foc_runtime *runtime, struct foc_runtime_tuning *tuning)
{
    if (runtime == NULL || !runtime->initialized || tuning == NULL) {
        return FOC_RUNTIME_ERR_INPUT;
    }
    const struct foc_current_config *inner = &runtime->pipeline.current.config;
    const struct foc_runtime_motion_config *motion = &runtime->config.motion;
    *tuning = (struct foc_runtime_tuning){
        .current_kp_v_per_a = inner->kp_v_per_a,
        .current_ki_v_per_a_s = inner->ki_v_per_a_s,
        .current_limit_a = inner->current_limit_a,
        .velocity_kp_a_per_rad_s = motion->velocity.kp_a_per_rad_s,
        .velocity_ki_a_per_rad = motion->velocity.ki_a_per_rad,
        .position_kp_per_s = motion->position.kp_per_s,
        .velocity_limit_rad_s = motion->position.velocity_limit_rad_s,
        .iq_limit_a = motion->iq_limit_a,
        .accel_rad_s2 = motion->accel_rad_s2,
        .decel_rad_s2 = motion->decel_rad_s2,
        .command_timeout_us = runtime->pipeline.safety.config.command_timeout_us,
        .trips = runtime->config.trips,
        .profile = runtime->config.profile,
    };
    return FOC_RUNTIME_OK;
}

int foc_runtime_apply_tuning(struct foc_runtime *runtime, const struct foc_runtime_tuning *tuning)
{
    if (runtime == NULL || !runtime->initialized || tuning == NULL ||
        atomic_load_explicit(&runtime->tuning_pending, memory_order_acquire) !=
            FOC_RUNTIME_REQUEST_IDLE) {
        return FOC_RUNTIME_ERR_INPUT;
    }
    struct foc_current_context current;
    struct foc_velocity_context velocity;
    struct foc_protect_config protect;
    if (foc_runtime_tuning_check(runtime, tuning, &current, &velocity, &protect) !=
        FOC_RUNTIME_OK) {
        return FOC_RUNTIME_ERR_CONFIG;
    }
    foc_runtime_tuning_commit(runtime, tuning, &current, &velocity, &protect);
    runtime->tuning_request_sequence = 0U;
    foc_runtime_tuning_write_result(
        runtime,
        FOC_RUNTIME_REQUEST_APPLIED,
        runtime->boot_us,
        FOC_RUNTIME_OK
    );
    return FOC_RUNTIME_OK;
}

int foc_runtime_publish_tuning(
    struct foc_runtime *runtime,
    const struct foc_runtime_tuning *tuning,
    uint32_t request_sequence
)
{
    if (runtime == NULL || !runtime->initialized || tuning == NULL) {
        return FOC_RUNTIME_ERR_INPUT;
    }
    struct foc_current_context current;
    struct foc_velocity_context velocity;
    struct foc_protect_config protect;
    if (foc_runtime_tuning_check(runtime, tuning, &current, &velocity, &protect) !=
        FOC_RUNTIME_OK) {
        return FOC_RUNTIME_ERR_CONFIG;
    }
    if (atomic_load_explicit(&runtime->tuning_pending, memory_order_acquire) !=
        FOC_RUNTIME_REQUEST_IDLE) {
        return FOC_RUNTIME_ERR_BUSY;
    }
    runtime->tuning_request = *tuning;
    runtime->tuning_request_sequence = request_sequence;
    atomic_store_explicit(
        &runtime->tuning_pending,
        FOC_RUNTIME_REQUEST_PUBLISHED,
        memory_order_release
    );
    return FOC_RUNTIME_OK;
}

int foc_runtime_read_tuning_result(
    struct foc_runtime *runtime,
    struct foc_runtime_tuning_result *result
)
{
    if (runtime == NULL || !runtime->initialized || result == NULL) {
        return FOC_RUNTIME_ERR_INPUT;
    }
    const unsigned int pending =
        atomic_load_explicit(&runtime->tuning_pending, memory_order_acquire);
    const unsigned int slot =
        atomic_load_explicit(&runtime->tuning_result_slot, memory_order_acquire);
    *result = runtime->tuning_results[slot];
    if (pending != FOC_RUNTIME_REQUEST_IDLE) {
        result->result = FOC_RUNTIME_REQUEST_PENDING;
        result->request_sequence = runtime->tuning_request_sequence;
    }
    return FOC_RUNTIME_OK;
}

int foc_runtime_init(
    struct foc_runtime *runtime,
    const struct foc_pipeline_config *pipeline_config,
    const struct foc_runtime_config *config,
    uint32_t now_us
)
{
    if (runtime == NULL || config == NULL || pipeline_config == NULL) {
        return FOC_RUNTIME_ERR_INPUT;
    }
    if (!foc_runtime_lane_valid(&config->h0) || !foc_runtime_lane_valid(&config->h1) ||
        !foc_runtime_lane_valid(&config->supervision) ||
        !foc_runtime_lane_valid(&config->telemetry) ||
        config->foreground_budget_us < config->supervision.budget_us ||
        config->foreground_budget_us >= FOC_RUNTIME_HALF_RANGE || config->angle_timeout_us == 0U ||
        config->angle_timeout_us >= FOC_RUNTIME_HALF_RANGE ||
        !foc_runtime_impedance_policy_valid(&config->motion) ||
        !foc_runtime_protection_valid(&config->motion) ||
        (config->motion.following_error.enabled &&
         (config->motion.following_error.max_gap_us < config->h0.period_us ||
          config->motion.following_error.max_gap_us < config->h1.period_us))) {
        return FOC_RUNTIME_ERR_CONFIG;
    }
    struct foc_pipeline pipeline;
    struct foc_protect_config protect;
    if (foc_pipeline_init(&pipeline, pipeline_config) != FOC_PIPELINE_OK ||
        !foc_runtime_profile_valid(
            &config->profile,
            pipeline_config->current.current_limit_a,
            config->motion.position.velocity_limit_rad_s
        ) ||
        !foc_runtime_trip_resolve(
            config->trip_enabled,
            &config->trips,
            &config->profile,
            pipeline_config->current.current_limit_a,
            config->motion.enabled ? config->motion.iq_limit_a : 0.0f,
            config->motion.position.velocity_limit_rad_s,
            &protect
        )) {
        return FOC_RUNTIME_ERR_CONFIG;
    }
    struct foc_velocity_context velocity = {0};
    struct foc_trajectory_context trajectory = {0};
    struct foc_runtime_motion_config motion_config = config->motion;
    const bool ramp = motion_config.accel_rad_s2 != 0.0f || motion_config.decel_rad_s2 != 0.0f;
    if ((motion_config.trajectory.enabled || ramp) && !motion_config.enabled) {
        return FOC_RUNTIME_ERR_CONFIG;
    }
    if (ramp &&
        !foc_runtime_ramp_trajectory(
            motion_config.accel_rad_s2,
            motion_config.decel_rad_s2,
            motion_config.position.velocity_limit_rad_s,
            motion_config.velocity.max_dt_s,
            &motion_config.trajectory
        )) {
        return FOC_RUNTIME_ERR_CONFIG;
    }
    if (config->motion.enabled) {
        const struct foc_runtime_motion_config *motion = &motion_config;
        struct foc_electrical_state electrical;
        struct foc_position_output position;
        float predicted;
        if (!isfinite(motion->iq_limit_a) || motion->iq_limit_a <= 0.0f ||
            motion->iq_limit_a > pipeline_config->current.current_limit_a ||
            foc_rotor_electrical(&motion->rotor, 0.0f, 0.0f, &electrical) != FOC_STATUS_OK ||
            foc_rotor_predict(&motion->prediction, &electrical, 0U, &predicted) != FOC_STATUS_OK ||
            foc_position_step(&motion->position, 0.0f, 0.0f, &position) != FOC_STATUS_OK ||
            foc_velocity_init(&velocity, &motion->velocity) != FOC_STATUS_OK) {
            return FOC_RUNTIME_ERR_CONFIG;
        }
        if (motion->trajectory.enabled &&
            (foc_trajectory_init(&trajectory, &motion->trajectory) != FOC_TRAJECTORY_STATUS_OK ||
             foc_trajectory_reset(&trajectory, 0.0f) != FOC_TRAJECTORY_STATUS_OK)) {
            return FOC_RUNTIME_ERR_CONFIG;
        }
    }
    *runtime = (struct foc_runtime){
        .config = *config,
        .boot_us = now_us,
        .pipeline = pipeline,
        .outer_velocity = velocity,
        .trajectory = trajectory,
        .protect = protect,
        .h0 = {.next_us = now_us, .last_call_us = now_us},
        .h1 = {.next_us = now_us, .last_call_us = now_us},
        .supervision = {.next_us = now_us, .last_call_us = now_us},
        .telemetry = {.next_us = now_us, .last_call_us = now_us},
    };
#if defined(FOC_RUNTIME_HOST_COGGING)
    runtime->config.motion.cogging.enabled = foc_runtime_cogging_valid(&config->motion);
#endif
    runtime->config.motion.trajectory = motion_config.trajectory;
    runtime->trajectory_trial = trajectory;
    atomic_init(&runtime->sensor_slot, FOC_RUNTIME_UNPUBLISHED);
    atomic_init(&runtime->command_slot, FOC_RUNTIME_UNPUBLISHED);
    atomic_init(&runtime->foreground_us, 0U);
    atomic_init(&runtime->foreground_seen, 0U);
    atomic_init(&runtime->stop_requests, 0U);
    atomic_init(&runtime->telemetry_sequence, 1U);
    atomic_init(&runtime->telemetry_pipeline_status, FOC_PIPELINE_OK);
    atomic_init(&runtime->clear_result, FOC_RUNTIME_CLEAR_NONE);
    atomic_init(&runtime->lifecycle_request_slot, 0U);
    atomic_init(&runtime->lifecycle_request_pending, FOC_RUNTIME_REQUEST_IDLE);
    atomic_init(&runtime->lifecycle_result_slot, 0U);
    atomic_init(&runtime->lifecycle_last_sequence, 0U);
    atomic_init(&runtime->lifecycle_sequence_seen, 0U);
    atomic_init(&runtime->tuning_pending, FOC_RUNTIME_REQUEST_IDLE);
    atomic_init(&runtime->tuning_result_slot, 0U);
    atomic_init(&runtime->fault_snapshot_slot, 0U);
    runtime->lifecycle_results[0] = (struct foc_runtime_lifecycle_result){
        .result = FOC_RUNTIME_REQUEST_NONE,
        .state = FOC_SAFE_STATE_BOOT,
    };
    runtime->lifecycle_results[1] = runtime->lifecycle_results[0];
    for (unsigned int index = 0U; index < WORD_COUNT; ++index) {
        atomic_init(&runtime->telemetry_words[index], 0U);
    }
    foc_runtime_publish_snapshot(runtime);
    runtime->initialized = true;
    return FOC_RUNTIME_OK;
}

int foc_runtime_publish_command(
    struct foc_runtime *runtime,
    const struct foc_pipeline_command *command,
    uint32_t now_us
)
{
    if (runtime == NULL || !runtime->initialized || command == NULL ||
        !isfinite(command->target_a.d) || !isfinite(command->target_a.q)) {
        return FOC_RUNTIME_ERR_INPUT;
    }
    if (runtime->config.motion.limits.enabled) {
        return FOC_RUNTIME_ERR_MOTION_MODE;
    }
    const struct foc_runtime_command source = {
        .mode = FOC_RUNTIME_MODE_CURRENT,
        .seq = command->seq,
        .produced_us = command->produced_us,
        .target.current_a = command->target_a,
    };
    return foc_runtime_publish_reference(runtime, &source, now_us);
}

int foc_runtime_publish_calibration_command(
    struct foc_runtime *runtime,
    const struct foc_runtime_command *command,
    uint32_t now_us
)
{
    if (runtime == NULL || !runtime->initialized || command == NULL ||
        command->mode != FOC_RUNTIME_MODE_CALIBRATION ||
        !isfinite(command->target.calibration.voltage_v.d) ||
        !isfinite(command->target.calibration.voltage_v.q) ||
        !isfinite(command->target.calibration.theta_elec_rad)) {
        return FOC_RUNTIME_ERR_INPUT;
    }
    if (runtime->config.motion.limits.enabled) {
        return FOC_RUNTIME_ERR_MOTION_MODE;
    }
    return foc_runtime_publish_reference(runtime, command, now_us);
}

int foc_runtime_publish_motion_command(
    struct foc_runtime *runtime,
    const struct foc_runtime_command *command,
    uint32_t now_us
)
{
    if (runtime == NULL || !runtime->initialized || !runtime->config.motion.enabled ||
        command == NULL || !foc_runtime_cascade_mode(command->mode) ||
        !isfinite(command->target.motion_ref)) {
        return FOC_RUNTIME_ERR_INPUT;
    }
    if (command->mode == FOC_RUNTIME_MODE_POSITION && runtime->config.motion.limits.enabled &&
        foc_motion_limits_reference(&runtime->config.motion.limits, command->target.motion_ref) !=
            FOC_MOTION_REJECT_NONE) {
        return FOC_RUNTIME_ERR_MOTION_LIMIT;
    }
    return foc_runtime_publish_reference(runtime, command, now_us);
}

int foc_runtime_publish_impedance_command(
    struct foc_runtime *runtime,
    const struct foc_runtime_command *command,
    uint32_t now_us
)
{
    if (runtime == NULL || !runtime->initialized || command == NULL ||
        command->mode != FOC_RUNTIME_MODE_IMPEDANCE || !runtime->config.motion.impedance.enabled) {
        return FOC_RUNTIME_ERR_INPUT;
    }
    const struct foc_runtime_impedance_config *policy = &runtime->config.motion.impedance;
    const struct foc_runtime_impedance_command *reference = &command->target.impedance;
    if (reference->mechanical_frame_id != policy->mechanical_frame_id ||
        !isfinite(reference->position_rad) ||
        fabsf(reference->position_rad) > FOC_RUNTIME_IMPEDANCE_POSITION_LIMIT_RAD ||
        !isfinite(reference->velocity_rad_s) || !isfinite(reference->stiffness_nm_per_rad) ||
        reference->stiffness_nm_per_rad < 0.0f ||
        reference->stiffness_nm_per_rad > policy->max_stiffness_nm_per_rad ||
        !isfinite(reference->damping_nm_s_per_rad) || reference->damping_nm_s_per_rad < 0.0f ||
        reference->damping_nm_s_per_rad > policy->max_damping_nm_s_per_rad ||
        !isfinite(reference->torque_feedforward_nm) ||
        fabsf(reference->torque_feedforward_nm) > policy->max_feedforward_nm) {
        return FOC_RUNTIME_ERR_INPUT;
    }
    if (runtime->config.motion.limits.enabled &&
        foc_motion_limits_reference(&runtime->config.motion.limits, reference->position_rad) !=
            FOC_MOTION_REJECT_NONE) {
        return FOC_RUNTIME_ERR_MOTION_LIMIT;
    }
    struct foc_runtime_snapshot snapshot;
    const int status = foc_runtime_copy_snapshot(runtime, &snapshot, FOC_RUNTIME_HOOK_NONE);
    if (status != FOC_RUNTIME_OK) {
        return status;
    }
    if (snapshot.mode != FOC_RUNTIME_MODE_IMPEDANCE || snapshot.inhibit_pending ||
        snapshot.state == FOC_SAFE_STATE_FAULT_LATCHED) {
        return FOC_RUNTIME_ERR_CONTROL;
    }
    if (reference->source_session != snapshot.motion_session ||
        (snapshot.command_seen &&
         !foc_safe_sequence_newer(command->seq, snapshot.command_seq, true))) {
        return FOC_RUNTIME_ERR_ORDER;
    }
    return foc_runtime_publish_reference(runtime, command, now_us);
}

int foc_runtime_publish_lifecycle_request(
    struct foc_runtime *runtime,
    const struct foc_runtime_lifecycle_request *request,
    uint32_t now_us
)
{
    if (runtime == NULL || !runtime->initialized || !foc_runtime_lifecycle_shape_valid(request)) {
        return FOC_RUNTIME_ERR_INPUT;
    }
    if (request->action == FOC_RUNTIME_ACTION_SET_MODE &&
        request->requested_mode == FOC_RUNTIME_MODE_CURRENT &&
        runtime->config.motion.limits.enabled) {
        return FOC_RUNTIME_ERR_MOTION_MODE;
    }
    if (!foc_safe_timestamp_fresh(
            now_us,
            request->produced_us,
            runtime->pipeline.safety.config.command_timeout_us
        )) {
        return FOC_RUNTIME_ERR_ORDER;
    }
    struct foc_runtime_snapshot snapshot;
    if (foc_runtime_read_snapshot(runtime, &snapshot) != FOC_RUNTIME_OK) {
        return FOC_RUNTIME_ERR_BUSY;
    }
    if (request->source_session != snapshot.motion_session) {
        return FOC_RUNTIME_ERR_ORDER;
    }
    if (atomic_load_explicit(&runtime->lifecycle_request_pending, memory_order_acquire) !=
        FOC_RUNTIME_REQUEST_IDLE) {
        return FOC_RUNTIME_ERR_BUSY;
    }
    const bool sequence_seen =
        atomic_load_explicit(&runtime->lifecycle_sequence_seen, memory_order_relaxed) != 0U;
    const uint32_t previous_sequence =
        atomic_load_explicit(&runtime->lifecycle_last_sequence, memory_order_relaxed);
    if (!foc_safe_sequence_newer(request->request_sequence, previous_sequence, sequence_seen)) {
        return FOC_RUNTIME_ERR_ORDER;
    }
    unsigned int expected = FOC_RUNTIME_REQUEST_IDLE;
    if (!atomic_compare_exchange_strong_explicit(
            &runtime->lifecycle_request_pending,
            &expected,
            FOC_RUNTIME_REQUEST_WRITING,
            memory_order_acquire,
            memory_order_relaxed
        )) {
        return FOC_RUNTIME_ERR_BUSY;
    }
    const unsigned int current =
        atomic_load_explicit(&runtime->lifecycle_request_slot, memory_order_relaxed);
    const unsigned int next = current == 0U ? 1U : 0U;
    runtime->lifecycle_requests[next] = *request;
    atomic_store_explicit(&runtime->lifecycle_request_slot, next, memory_order_relaxed);
    atomic_store_explicit(
        &runtime->lifecycle_last_sequence,
        request->request_sequence,
        memory_order_relaxed
    );
    atomic_store_explicit(&runtime->lifecycle_sequence_seen, 1U, memory_order_relaxed);
    atomic_store_explicit(
        &runtime->lifecycle_request_pending,
        FOC_RUNTIME_REQUEST_PUBLISHED,
        memory_order_release
    );
    return FOC_RUNTIME_OK;
}

int foc_runtime_read_lifecycle_result(
    struct foc_runtime *runtime,
    struct foc_runtime_lifecycle_result *result
)
{
    if (runtime == NULL || !runtime->initialized || result == NULL) {
        return FOC_RUNTIME_ERR_INPUT;
    }
    const unsigned int pending =
        atomic_load_explicit(&runtime->lifecycle_request_pending, memory_order_acquire);
    if (pending == FOC_RUNTIME_REQUEST_WRITING) {
        return FOC_RUNTIME_ERR_BUSY;
    }
    if (pending == FOC_RUNTIME_REQUEST_PUBLISHED || pending == FOC_RUNTIME_REQUEST_CLAIMED) {
        const unsigned int slot =
            atomic_load_explicit(&runtime->lifecycle_request_slot, memory_order_acquire);
        const struct foc_runtime_lifecycle_request request = runtime->lifecycle_requests[slot];
        struct foc_runtime_snapshot snapshot;
        if (foc_runtime_read_snapshot(runtime, &snapshot) != FOC_RUNTIME_OK) {
            return FOC_RUNTIME_ERR_BUSY;
        }
        *result = (struct foc_runtime_lifecycle_result){
            .result = FOC_RUNTIME_REQUEST_PENDING,
            .action = request.action,
            .requested_mode = request.requested_mode,
            .request_sequence = request.request_sequence,
            .applied_session = snapshot.motion_session,
            .h0_us = snapshot.fast_us,
            .state = snapshot.state,
            .faults = snapshot.faults,
            .pipeline_status = snapshot.pipeline_status,
        };
        return FOC_RUNTIME_OK;
    }
    const unsigned int slot =
        atomic_load_explicit(&runtime->lifecycle_result_slot, memory_order_acquire);
    *result = runtime->lifecycle_results[slot];
    return FOC_RUNTIME_OK;
}

int foc_runtime_h1_step(
    struct foc_runtime *runtime,
    const struct foc_runtime_sensor *sample,
    uint32_t now_us,
    uint32_t cost_us
)
{
    if (runtime == NULL || !runtime->initialized || sample == NULL) {
        return FOC_RUNTIME_ERR_INPUT;
    }
    const int status = foc_runtime_schedule(&runtime->h1, &runtime->config.h1, now_us, cost_us);
    if (status != FOC_RUNTIME_OK) {
        return status;
    }
    const unsigned int index = atomic_load_explicit(&runtime->sensor_slot, memory_order_relaxed);
    const struct foc_runtime_sensor_slot *previous =
        index == FOC_RUNTIME_UNPUBLISHED ? NULL : &runtime->sensor_slots[index];
    if (!foc_safe_timestamp_fresh(now_us, sample->captured_us, runtime->config.angle_timeout_us) ||
        (previous != NULL &&
         (!foc_safe_sequence_newer(sample->seq, previous->sample.seq, true) ||
          !foc_safe_sequence_newer(sample->captured_us, previous->sample.captured_us, true)))) {
        return FOC_RUNTIME_ERR_ORDER;
    }
    const unsigned int next = index == 0U ? 1U : 0U;
    struct foc_runtime_sensor_slot candidate = {.sample = *sample};
    if (runtime->config.motion.enabled) {
        struct foc_runtime_snapshot view;
        int snapshot_status =
            foc_runtime_copy_snapshot(runtime, &view, FOC_RUNTIME_HOOK_CONTROL_SNAPSHOT_READING);
        if (snapshot_status == FOC_RUNTIME_ERR_BUSY) {
            /* A completed H0 publication may have interrupted the first copy. */
            snapshot_status = foc_runtime_copy_snapshot(
                runtime,
                &view,
                FOC_RUNTIME_HOOK_CONTROL_SNAPSHOT_READING
            );
        }
        if (snapshot_status != FOC_RUNTIME_OK) {
            return snapshot_status;
        }
        const float electrical_speed =
            (float)runtime->config.motion.rotor.pole_pairs * sample->mechanical_rad_s;
        candidate.sample.valid = sample->valid && sample->electrical_phase_valid &&
            isfinite(sample->mechanical_rad) && isfinite(sample->mechanical_rad_s) &&
            isfinite(sample->theta_elec_rad) && sample->theta_elec_rad >= 0.0f &&
            sample->theta_elec_rad < 6.28318530717958647692f && isfinite(electrical_speed);
        if (candidate.sample.valid) {
            candidate.electrical =
                (struct foc_electrical_state){sample->theta_elec_rad, electrical_speed};
        }
        foc_runtime_prepare_outer(runtime, &candidate, previous, &view, now_us);
    } else {
        candidate.sample.valid = sample->valid && isfinite(sample->theta_elec_rad);
    }
    runtime->sensor_slots[next].sample.captured_us = candidate.sample.captured_us;
    foc_runtime_hook(runtime, FOC_RUNTIME_HOOK_SENSOR_WRITING);
    runtime->sensor_slots[next] = candidate;
    atomic_store_explicit(&runtime->sensor_slot, next, memory_order_release);
    return candidate.sample.valid ? FOC_RUNTIME_OK : FOC_RUNTIME_ERR_INPUT;
}

int foc_runtime_h0_step(struct foc_runtime *runtime, const struct foc_runtime_fast_input *input)
{
    if (runtime == NULL || !runtime->initialized) {
        return FOC_RUNTIME_ERR_INPUT;
    }
    if (input == NULL) {
        foc_runtime_request_inhibit(runtime, 0U);
        return FOC_RUNTIME_ERR_INPUT;
    }
    const int schedule =
        foc_runtime_schedule(&runtime->h0, &runtime->config.h0, input->now_us, input->cost_us);
    if (schedule == FOC_RUNTIME_IDLE && atomic_load(&runtime->stop_requests) == 0U) {
        return FOC_RUNTIME_IDLE;
    }
    atomic_fetch_add(&runtime->telemetry_sequence, 1U);
    const enum foc_runtime_mode previous_mode = runtime->mode;
    const enum foc_safe_state previous_state = runtime->pipeline.safety.state;
    const bool had_boundary = runtime->pipeline.rearm_boundary_seen;
    const uint32_t previous_boundary = runtime->pipeline.rearm_after_us;
    runtime->angle_prediction_us = 0U;
    runtime->observed_sensor.valid = false;
    if (schedule < 0) {
        foc_runtime_request_inhibit(runtime, FOC_SAFE_FAULT_FAST_HEARTBEAT);
    }
    const bool stopped = foc_runtime_drain_stop(runtime, input->now_us);
    const bool clearing = atomic_load(&runtime->clear_result) == FOC_RUNTIME_CLEAR_PENDING;
    struct foc_runtime_lifecycle_request lifecycle_request = {0};
    bool lifecycle_claimed = false;
    enum foc_runtime_request_result lifecycle_result = FOC_RUNTIME_REQUEST_NONE;
    int lifecycle_status = FOC_RUNTIME_OK;
    int status = schedule;
    if (stopped && schedule >= 0) {
        status = FOC_RUNTIME_ERR_STOP;
        lifecycle_claimed = foc_runtime_lifecycle_claim(runtime, &lifecycle_request);
        if (lifecycle_claimed) {
            lifecycle_result = FOC_RUNTIME_REQUEST_CANCELLED;
            lifecycle_status = status;
        }
    } else if (schedule < 0) {
        lifecycle_claimed = foc_runtime_lifecycle_claim(runtime, &lifecycle_request);
        if (lifecycle_claimed) {
            lifecycle_result = FOC_RUNTIME_REQUEST_CANCELLED;
            lifecycle_status = status;
        }
    } else if (schedule == FOC_RUNTIME_OK) {
        if (input->action == FOC_RUNTIME_ACTION_NONE) {
            lifecycle_claimed = foc_runtime_lifecycle_claim(runtime, &lifecycle_request);
            if (lifecycle_claimed &&
                (clearing ||
                 !foc_runtime_lifecycle_request_current(
                     runtime,
                     &lifecycle_request,
                     input->now_us
                 ))) {
                lifecycle_result =
                    clearing ? FOC_RUNTIME_REQUEST_CANCELLED : FOC_RUNTIME_REQUEST_REJECTED;
                lifecycle_status = FOC_RUNTIME_ERR_CONTROL;
            }
        }
        struct foc_runtime_fast_input control_input = *input;
        if (lifecycle_claimed && lifecycle_result == FOC_RUNTIME_REQUEST_NONE) {
            control_input.action = lifecycle_request.action;
            control_input.requested_mode = lifecycle_request.requested_mode;
        }
        if (clearing) {
            struct foc_runtime_fast_input recovery =
                lifecycle_result == FOC_RUNTIME_REQUEST_NONE ? control_input : *input;
            if (recovery.action == FOC_RUNTIME_ACTION_NONE &&
                foc_runtime_timeout_clear_ready(runtime, input)) {
                recovery.action = FOC_RUNTIME_ACTION_CLEAR_FAULT;
            }
            status = foc_runtime_control(runtime, &recovery);
            const bool cleared = recovery.action == FOC_RUNTIME_ACTION_CLEAR_FAULT &&
                runtime->pipeline.safety.state == FOC_SAFE_STATE_DISABLED;
            if (cleared) {
                const struct foc_runtime_fault_snapshot none = {0};
                foc_runtime_publish_fault_snapshot(runtime, &none);
            }
            atomic_store(
                &runtime->clear_result,
                cleared ? FOC_RUNTIME_CLEAR_APPLIED : FOC_RUNTIME_CLEAR_REJECTED
            );
        } else {
            /* A rejected mailbox stays separate from the ordinary H0 cycle.
             * Its action was not copied into control_input above. */
            status = foc_runtime_control(runtime, &control_input);
        }
        if (lifecycle_claimed && lifecycle_result != FOC_RUNTIME_REQUEST_NONE &&
            (status == FOC_RUNTIME_ERR_STOP ||
             runtime->pipeline.safety.state == FOC_SAFE_STATE_FAULT_LATCHED)) {
            lifecycle_result = FOC_RUNTIME_REQUEST_CANCELLED;
            lifecycle_status = status;
        }
        if (lifecycle_claimed && lifecycle_result == FOC_RUNTIME_REQUEST_NONE) {
            lifecycle_status = status;
            lifecycle_result = status == FOC_RUNTIME_ERR_STOP ||
                    runtime->pipeline.safety.state == FOC_SAFE_STATE_FAULT_LATCHED
                ? FOC_RUNTIME_REQUEST_CANCELLED
                : (status >= FOC_RUNTIME_IDLE &&
                           foc_runtime_lifecycle_applied(runtime, &lifecycle_request)
                       ? FOC_RUNTIME_REQUEST_APPLIED
                       : FOC_RUNTIME_REQUEST_REJECTED);
        }
    }
    if (runtime->mode != previous_mode ||
        (runtime->pipeline.rearm_boundary_seen &&
         (!had_boundary || previous_boundary != runtime->pipeline.rearm_after_us)) ||
        (runtime->pipeline.safety.state == FOC_SAFE_STATE_FAULT_LATCHED &&
         previous_state != FOC_SAFE_STATE_FAULT_LATCHED)) {
        runtime->motion_session++;
        runtime->accepted_outer_seen = false;
        foc_protect_reset(&runtime->protect_state);
        if (runtime->config.motion.following_error.enabled) {
            const enum foc_following_state observed = runtime->following.status;
            const enum foc_following_state retained =
                (observed == FOC_FOLLOWING_TRACKING || observed == FOC_FOLLOWING_PENDING)
                ? FOC_FOLLOWING_INACTIVE
                : observed;
            runtime->following = (struct foc_following_error_state){.status = retained};
        }
    }
    if (previous_state != FOC_SAFE_STATE_RUNNING &&
        runtime->pipeline.safety.state == FOC_SAFE_STATE_RUNNING) {
        foc_protect_reset(&runtime->protect_state);
        if (runtime->config.motion.following_error.enabled) {
            runtime->following = (struct foc_following_error_state){0};
        }
    }
    if (lifecycle_claimed) {
        foc_runtime_lifecycle_finish(
            runtime,
            &lifecycle_request,
            lifecycle_result,
            input->now_us,
            lifecycle_status
        );
    }
    foc_runtime_tuning_service(runtime, input->now_us, stopped || schedule < 0 || clearing);
    foc_runtime_publish_snapshot(runtime);
    return status;
}

int foc_runtime_foreground_poll(
    struct foc_runtime *runtime,
    uint32_t now_us,
    uint32_t supervision_cost_us,
    uint32_t telemetry_cost_us,
    struct foc_runtime_foreground_result *result
)
{
    if (runtime == NULL || !runtime->initialized || result == NULL) {
        return FOC_RUNTIME_ERR_INPUT;
    }
    *result = (struct foc_runtime_foreground_result){0};
    const int supervision = foc_runtime_schedule(
        &runtime->supervision,
        &runtime->config.supervision,
        now_us,
        supervision_cost_us
    );
    uint32_t remaining = runtime->config.foreground_budget_us;
    int status = supervision;
    const unsigned int supervision_generation = atomic_load(&runtime->telemetry_sequence);
    if (supervision < 0) {
        foc_runtime_request_inhibit(runtime, FOC_SAFE_FAULT_FOREGROUND_HEARTBEAT);
        remaining = 0U;
    } else if (supervision == FOC_RUNTIME_OK) {
        remaining -= supervision_cost_us;
        status = foc_runtime_supervise(runtime, now_us, result);
    }

    const int telemetry = foc_runtime_schedule(
        &runtime->telemetry,
        &runtime->config.telemetry,
        now_us,
        telemetry_cost_us
    );
    if (telemetry != FOC_RUNTIME_IDLE) {
        struct foc_runtime_snapshot snapshot;
        result->telemetry_ready = status >= 0 && telemetry == FOC_RUNTIME_OK &&
            telemetry_cost_us <= remaining &&
            foc_runtime_read_snapshot(runtime, &snapshot) == FOC_RUNTIME_OK;
        runtime->telemetry_dropped += !result->telemetry_ready;
    }
    /* H0 may have acknowledged a new fault while Foreground copied telemetry. */
    if (result->watchdog_permitted &&
        (atomic_load(&runtime->stop_requests) != 0U ||
         supervision_generation != atomic_load(&runtime->telemetry_sequence))) {
        result->watchdog_permitted = false;
    }
    return result->telemetry_ready && status == FOC_RUNTIME_IDLE ? FOC_RUNTIME_OK : status;
}

void foc_runtime_request_inhibit(struct foc_runtime *runtime, uint32_t fault_bits)
{
    if (runtime != NULL && runtime->initialized) {
        atomic_fetch_or(&runtime->stop_requests, FOC_RUNTIME_STOP_REQUEST | fault_bits);
    }
}

void foc_runtime_revoke_command(struct foc_runtime *runtime)
{
    foc_runtime_request_inhibit(runtime, 0U);
    atomic_store_explicit(&runtime->command_slot, FOC_RUNTIME_UNPUBLISHED, memory_order_release);
    if (atomic_load(&runtime->clear_result) == FOC_RUNTIME_CLEAR_PENDING) {
        atomic_store(&runtime->clear_result, FOC_RUNTIME_CLEAR_REJECTED);
    }
}

int foc_runtime_request_timeout_clear(struct foc_runtime *runtime, uint32_t now_us)
{
    struct foc_runtime_snapshot view;
    if (foc_runtime_read_snapshot(runtime, &view) != FOC_RUNTIME_OK ||
        view.state != FOC_SAFE_STATE_FAULT_LATCHED || !foc_runtime_clearable_faults(view.faults) ||
        view.inhibit_pending || !view.output_confirmed ||
        view.action != FOC_DEVICE_OUTPUT_INHIBIT) {
        return FOC_RUNTIME_ERR_CONTROL;
    }
    if (view.clear_result == FOC_RUNTIME_CLEAR_PENDING) {
        return FOC_RUNTIME_ERR_BUSY;
    }
    foc_runtime_revoke_command(runtime);
    runtime->clear_requested_us = now_us;
    atomic_store_explicit(&runtime->clear_result, FOC_RUNTIME_CLEAR_PENDING, memory_order_release);
    return FOC_RUNTIME_OK;
}

int foc_runtime_read_snapshot(struct foc_runtime *runtime, struct foc_runtime_snapshot *snapshot)
{
    if (runtime == NULL || !runtime->initialized || snapshot == NULL) {
        return FOC_RUNTIME_ERR_INPUT;
    }
    return foc_runtime_copy_snapshot(runtime, snapshot, FOC_RUNTIME_HOOK_TELEMETRY_READING);
}

int foc_runtime_read_fault_snapshot(
    struct foc_runtime *runtime,
    struct foc_runtime_fault_snapshot *snapshot
)
{
    if (runtime == NULL || !runtime->initialized || snapshot == NULL) {
        return FOC_RUNTIME_ERR_INPUT;
    }
    const unsigned int slot =
        atomic_load_explicit(&runtime->fault_snapshot_slot, memory_order_acquire);
    *snapshot = runtime->fault_snapshots[slot];
    return FOC_RUNTIME_OK;
}

/* ---------------------------------------- end of file ----------------------------------------- */
