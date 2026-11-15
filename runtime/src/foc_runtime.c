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

#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

/* Private Constants ------------------------------------------------------------ */

#define FOC_RUNTIME_HALF_RANGE   UINT32_C(0x80000000)
#define FOC_RUNTIME_STOP_REQUEST FOC_RUNTIME_HALF_RANGE

enum {
    FOC_RUNTIME_UNPUBLISHED = 2,
    SNAPSHOT_CONFIRMED = 1U << 0,
    SNAPSHOT_FAST_SEEN = 1U << 1,
    SNAPSHOT_COMMAND_SEEN = 1U << 2,
    SNAPSHOT_SAMPLE_SEEN = 1U << 3,
    SNAPSHOT_OUTER_SEEN = 1U << 4,
    SNAPSHOT_SENSOR_VALID = 1U << 5,
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
            (runtime->observed_sensor.valid ? SNAPSHOT_SENSOR_VALID : 0U),
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
    bool inject
)
{
    const unsigned int before = atomic_load(&runtime->telemetry_sequence);
    if ((before & 1U) != 0U) {
        return FOC_RUNTIME_ERR_BUSY;
    }
    unsigned int words[WORD_COUNT];
    words[WORD_FLAGS] = atomic_load(&runtime->telemetry_words[WORD_FLAGS]);
    if (inject) {
        foc_runtime_hook(runtime, FOC_RUNTIME_HOOK_TELEMETRY_READING);
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

static bool foc_runtime_outer_mode(enum foc_runtime_mode mode)
{
    return mode == FOC_RUNTIME_MODE_VELOCITY || mode == FOC_RUNTIME_MODE_POSITION;
}

static int
foc_runtime_set_mode(struct foc_runtime *runtime, enum foc_runtime_mode mode, uint32_t now_us)
{
    const struct foc_pipeline *pipeline = &runtime->pipeline;
    if (pipeline->safety.state != FOC_SAFE_STATE_DISABLED || !pipeline->mock.output_confirmed ||
        pipeline->mock.last_request.action != FOC_DEVICE_OUTPUT_INHIBIT ||
        (mode != FOC_RUNTIME_MODE_CURRENT &&
         (!runtime->config.motion.enabled || !foc_runtime_outer_mode(mode)))) {
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

static bool foc_runtime_admit_update(void *argument)
{
    struct foc_runtime *runtime = argument;

    foc_runtime_hook(runtime, FOC_RUNTIME_HOOK_BEFORE_SUBMIT);
    /* Linearize serial admission against the independent software stop. */
    return atomic_load(&runtime->stop_requests) == 0U;
}

static bool foc_runtime_timeout_clear_ready(
    struct foc_runtime *runtime,
    const struct foc_runtime_fast_input *input
)
{
    const struct foc_pipeline *pipeline = &runtime->pipeline;
    const unsigned int sensor = atomic_load_explicit(&runtime->sensor_slot, memory_order_acquire);
    struct foc_device_measurement measurement;
    return pipeline->safety.state == FOC_SAFE_STATE_FAULT_LATCHED &&
        pipeline->safety.latched_faults == FOC_SAFE_FAULT_COMMAND_TIMEOUT &&
        pipeline->mock.output_confirmed &&
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

static int
foc_runtime_control(struct foc_runtime *runtime, const struct foc_runtime_fast_input *input)
{
    struct foc_pipeline *pipeline = &runtime->pipeline;
    const struct foc_device_adc_raw *adc = &input->adc;
    const uint32_t now_us = input->now_us;
    if (input->action == FOC_RUNTIME_ACTION_SET_MODE) {
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
    const bool outer = foc_runtime_outer_mode(runtime->mode);
    const bool target_valid = outer && isfinite(aligned) && sensor.target_valid &&
        sensor.mode == runtime->mode && sensor.session == runtime->motion_session &&
        foc_safe_timestamp_fresh(now_us,
                                 sensor.target.produced_us,
                                 pipeline->safety.config.command_timeout_us);
    const struct foc_pipeline_input step = {
        .adc = *adc,
        .theta_elec_rad = aligned,
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
    } else {
        const unsigned int command_index =
            atomic_load_explicit(&runtime->command_slot, memory_order_acquire);
        if (command_index != FOC_RUNTIME_UNPUBLISHED &&
            runtime->command_slots[command_index].mode == FOC_RUNTIME_MODE_CURRENT) {
            const struct foc_runtime_command command = runtime->command_slots[command_index];
            const struct foc_pipeline_command source =
                {command.target.current_a, command.seq, command.produced_us};
            foc_runtime_import_command(runtime, &source, now_us);
        }
    }
    foc_runtime_hook(runtime, FOC_RUNTIME_HOOK_H0_INPUTS);
    if (foc_runtime_drain_stop(runtime, now_us)) {
        return FOC_RUNTIME_ERR_STOP;
    }
    if (!foc_runtime_action_valid(input->action, pipeline->safety.state)) {
        runtime->pipeline_status = FOC_PIPELINE_ERR_STATE;
        return FOC_RUNTIME_ERR_CONTROL;
    }

    const int status =
        foc_pipeline_step_guarded(pipeline, &step, foc_runtime_admit_update, runtime);
    runtime->pipeline_status = status;
    foc_runtime_hook(runtime, FOC_RUNTIME_HOOK_AFTER_CONTROL);
    const bool stopped = foc_runtime_drain_stop(runtime, now_us);
    if (pipeline->mock.output_confirmed) {
        (void)foc_safe_heartbeat_fast(&pipeline->safety, now_us);
    }
    if (status == FOC_PIPELINE_OK) {
        if (target_valid && !stopped) {
            runtime->accepted_outer_seq = sensor.sample.seq;
            runtime->accepted_outer_seen = true;
        }
        return FOC_RUNTIME_OK;
    }
    if (stopped || status == FOC_PIPELINE_ERR_STOP) {
        runtime->pipeline_status = status;
        return FOC_RUNTIME_ERR_STOP;
    }
    if (input->action == FOC_RUNTIME_ACTION_CLEAR_FAULT ||
        (input->action != FOC_RUNTIME_ACTION_NONE && status == FOC_PIPELINE_ERR_STATE)) {
        if (input->action != FOC_RUNTIME_ACTION_CLEAR_FAULT) {
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
    struct foc_runtime_snapshot snapshot;
    const int status = foc_runtime_copy_snapshot(runtime, &snapshot, false);
    if (status != FOC_RUNTIME_OK) {
        foc_runtime_request_inhibit(runtime, FOC_SAFE_FAULT_FOREGROUND_HEARTBEAT);
        return status;
    }

    const struct foc_safe_config config = runtime->pipeline.safety.config;
    const bool previous_seen =
        atomic_load_explicit(&runtime->foreground_seen, memory_order_relaxed) != 0U;
    const uint32_t previous = atomic_load_explicit(&runtime->foreground_us, memory_order_relaxed);
    if (previous_seen &&
        !foc_safe_timestamp_fresh(now_us, previous, config.foreground_heartbeat_timeout_us)) {
        foc_runtime_request_inhibit(runtime, FOC_SAFE_FAULT_FOREGROUND_HEARTBEAT);
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
    const struct foc_runtime_command command = runtime->command_slots[index];
    if (command.mode != view->mode ||
        !foc_safe_timestamp_fresh(
            now_us,
            command.produced_us,
            runtime->pipeline.safety.config.command_timeout_us
        )) {
        return;
    }

    float iq = 0.0f;
    if (running) {
        if (!same_session) {
            return;
        }
        const struct foc_runtime_motion_config *policy = &runtime->config.motion;
        const float speed_limit = policy->position.velocity_limit_rad_s;
        float speed = fmaxf(-speed_limit, fminf(command.target.motion_ref, speed_limit));
        const float dt_s =
            (float)(candidate->sample.captured_us - previous->sample.captured_us) * 1.0e-6f;
        if (trajectory_enabled) {
            struct foc_trajectory_output trajectory_output;
            struct foc_trajectory_context trajectory_trial = runtime->trajectory;
            const int trajectory_status = command.mode == FOC_RUNTIME_MODE_POSITION
                ? foc_trajectory_position_step(
                      &trajectory_trial,
                      command.target.motion_ref,
                      candidate->sample.mechanical_rad,
                      dt_s,
                      &trajectory_output
                  )
                : foc_trajectory_velocity_step(
                      &trajectory_trial,
                      command.target.motion_ref,
                      dt_s,
                      &trajectory_output
                  );
            if (trajectory_status != FOC_TRAJECTORY_STATUS_OK) {
                return;
            }
            runtime->trajectory_trial = trajectory_trial;
            speed = trajectory_output.velocity_ref_rad_s;
        } else if (command.mode == FOC_RUNTIME_MODE_POSITION) {
            struct foc_position_output output;
            if (foc_position_step(
                    &policy->position,
                    command.target.motion_ref,
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
    }
    candidate->target = (struct foc_pipeline_command){{0.0f, iq}, command.seq, command.produced_us};
    candidate->target_valid = true;
}

static int foc_runtime_publish_reference(
    struct foc_runtime *runtime,
    const struct foc_runtime_command *command,
    uint32_t now_us
)
{
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
    atomic_store_explicit(&runtime->command_slot, next, memory_order_release);
    return FOC_RUNTIME_OK;
}

/* Public Functions ------------------------------------------------------------- */

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
        config->angle_timeout_us >= FOC_RUNTIME_HALF_RANGE) {
        return FOC_RUNTIME_ERR_CONFIG;
    }
    struct foc_pipeline pipeline;
    if (foc_pipeline_init(&pipeline, pipeline_config) != FOC_PIPELINE_OK) {
        return FOC_RUNTIME_ERR_CONFIG;
    }
    struct foc_velocity_context velocity = {0};
    struct foc_trajectory_context trajectory = {0};
    if (config->motion.trajectory.enabled && !config->motion.enabled) {
        return FOC_RUNTIME_ERR_CONFIG;
    }
    if (config->motion.enabled) {
        const struct foc_runtime_motion_config *motion = &config->motion;
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
        .h0 = {.next_us = now_us, .last_call_us = now_us},
        .h1 = {.next_us = now_us, .last_call_us = now_us},
        .supervision = {.next_us = now_us, .last_call_us = now_us},
        .telemetry = {.next_us = now_us, .last_call_us = now_us},
    };
    atomic_init(&runtime->sensor_slot, FOC_RUNTIME_UNPUBLISHED);
    atomic_init(&runtime->command_slot, FOC_RUNTIME_UNPUBLISHED);
    atomic_init(&runtime->foreground_us, 0U);
    atomic_init(&runtime->foreground_seen, 0U);
    atomic_init(&runtime->stop_requests, 0U);
    atomic_init(&runtime->telemetry_sequence, 1U);
    atomic_init(&runtime->telemetry_pipeline_status, FOC_PIPELINE_OK);
    atomic_init(&runtime->clear_result, FOC_RUNTIME_CLEAR_NONE);
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
    const struct foc_runtime_command source = {
        .mode = FOC_RUNTIME_MODE_CURRENT,
        .seq = command->seq,
        .produced_us = command->produced_us,
        .target.current_a = command->target_a,
    };
    return foc_runtime_publish_reference(runtime, &source, now_us);
}

int foc_runtime_publish_motion_command(
    struct foc_runtime *runtime,
    const struct foc_runtime_command *command,
    uint32_t now_us
)
{
    if (runtime == NULL || !runtime->initialized || !runtime->config.motion.enabled ||
        command == NULL || !foc_runtime_outer_mode(command->mode) ||
        !isfinite(command->target.motion_ref)) {
        return FOC_RUNTIME_ERR_INPUT;
    }
    return foc_runtime_publish_reference(runtime, command, now_us);
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
        const int snapshot_status = foc_runtime_copy_snapshot(runtime, &view, false);
        if (snapshot_status != FOC_RUNTIME_OK) {
            return snapshot_status;
        }
        candidate.sample.valid = sample->valid &&
            foc_rotor_electrical(
                &runtime->config.motion.rotor,
                sample->mechanical_rad,
                sample->mechanical_rad_s,
                &candidate.electrical
            ) == FOC_STATUS_OK;
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
    int status = schedule;
    if (stopped && schedule >= 0) {
        status = FOC_RUNTIME_ERR_STOP;
    } else if (schedule == FOC_RUNTIME_OK) {
        const bool clearing = atomic_load(&runtime->clear_result) == FOC_RUNTIME_CLEAR_PENDING;
        if (clearing) {
            struct foc_runtime_fast_input recovery = *input;
            if (input->action == FOC_RUNTIME_ACTION_NONE &&
                foc_runtime_timeout_clear_ready(runtime, input)) {
                recovery.action = FOC_RUNTIME_ACTION_CLEAR_FAULT;
            }
            status = foc_runtime_control(runtime, &recovery);
            atomic_store(
                &runtime->clear_result,
                recovery.action == FOC_RUNTIME_ACTION_CLEAR_FAULT &&
                        runtime->pipeline.safety.state == FOC_SAFE_STATE_DISABLED
                    ? FOC_RUNTIME_CLEAR_APPLIED
                    : FOC_RUNTIME_CLEAR_REJECTED
            );
        } else {
            status = foc_runtime_control(runtime, input);
        }
    }
    if (runtime->mode != previous_mode ||
        (runtime->pipeline.rearm_boundary_seen &&
         (!had_boundary || previous_boundary != runtime->pipeline.rearm_after_us)) ||
        (runtime->pipeline.safety.state == FOC_SAFE_STATE_FAULT_LATCHED &&
         previous_state != FOC_SAFE_STATE_FAULT_LATCHED)) {
        runtime->motion_session++;
        runtime->accepted_outer_seen = false;
    }
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
        view.state != FOC_SAFE_STATE_FAULT_LATCHED ||
        view.faults != FOC_SAFE_FAULT_COMMAND_TIMEOUT || view.inhibit_pending ||
        !view.output_confirmed || view.action != FOC_DEVICE_OUTPUT_INHIBIT) {
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
    return foc_runtime_copy_snapshot(runtime, snapshot, true);
}

/* ---------------------------------------- end of file ----------------------------------------- */
