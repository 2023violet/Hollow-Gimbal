/**
 * @file    test_foc_runtime_impedance_virtual.c
 * @brief   Runtime H1 impedance mode under the existing Virtual Motor.
 * @version v0.1.0
 * @date    2026.11.18
 *
 * Test role
 * ---------
 * Run frozen virtual motor holdouts through Foreground, H1, H0 and the
 * ordinary output adapter. Report shaft and transaction measurements.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_impedance_feasibility.h"
#include "foc_virtual_current.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* Private Macros --------------------------------------------------------------- */

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return 1;                                                       \
        }                                                                   \
    } while (0)

/* Private Constants ------------------------------------------------------------ */

static const uint32_t epoch_us = 1000U;
static const uint32_t step_us = 500U;
static const uint32_t dwell_us = 100000U;
static const uint32_t frame_id = 71U;

/* Private Types --------------------------------------------------------------- */

struct study_case {
    const char *name;
    uint32_t end_us;
    uint32_t release_or_reverse_us;
    float position_rad;
    float damping;
    float cap_a;
    bool load;
};

struct metrics {
    double error_sq;
    double peak_error;
    double overshoot;
    double peak_speed;
    double peak_actual_iq;
    double min_target;
    double max_target;
    double pre_error;
    double load_error_sum;
    double final_error;
    double final_pos;
    uint32_t load_samples;
    uint32_t settle_us;
    uint32_t capped_us;
    uint32_t max_capped_run_us;
};

/* Private Helpers -------------------------------------------------------------- */

static int send_reference(
    struct foc_virtual_current *vm,
    float position_rad,
    float stiffness,
    float damping,
    float feedforward
)
{
    struct foc_runtime_snapshot snapshot;
    CHECK(foc_virtual_current_snapshot(vm, &snapshot) == FOC_RUNTIME_OK);
    const struct foc_runtime_command command = {
        .mode = FOC_RUNTIME_MODE_IMPEDANCE,
        .seq = vm->command_seq + 1U,
        .produced_us = vm->virtual_time_us,
        .target.impedance = {
            .position_rad = position_rad,
            .stiffness_nm_per_rad = stiffness,
            .damping_nm_s_per_rad = damping,
            .torque_feedforward_nm = feedforward,
            .mechanical_frame_id = frame_id,
            .source_session = snapshot.motion_session,
        },
    };
    const int status =
        foc_runtime_publish_impedance_command(&vm->runtime, &command, vm->virtual_time_us);
    if (status == FOC_RUNTIME_OK) {
        vm->command_seq = command.seq;
    }
    return status;
}

static int lifecycle(
    struct foc_virtual_current *vm,
    enum foc_runtime_action action,
    uint32_t at,
    enum foc_safe_state state
)
{
    CHECK(
        foc_virtual_current_publish_lifecycle(vm, action, FOC_RUNTIME_MODE_IMPEDANCE) ==
        FOC_RUNTIME_OK
    );
    struct foc_runtime_lifecycle_result response;
    CHECK(foc_virtual_current_read_lifecycle(vm, &response) == FOC_RUNTIME_OK);
    CHECK(
        response.result == FOC_RUNTIME_REQUEST_PENDING &&
        response.request_sequence == vm->lifecycle_sequence
    );
    CHECK(foc_virtual_current_run_until(vm, at) == 0);
    CHECK(foc_virtual_current_read_lifecycle(vm, &response) == FOC_RUNTIME_OK);
    CHECK(
        response.result == FOC_RUNTIME_REQUEST_APPLIED && response.h0_us == at &&
        response.action == action && response.requested_mode == FOC_RUNTIME_MODE_IMPEDANCE &&
        response.state == state && response.applied_session == vm->runtime.motion_session
    );
    CHECK(vm->runtime.mode == FOC_RUNTIME_MODE_IMPEDANCE && vm->output.accepted_updates == 0U);
    CHECK(vm->output.request.action == FOC_DEVICE_OUTPUT_INHIBIT);
    return 0;
}

static int start_impedance(struct foc_virtual_current *vm, float cap)
{
    struct foc_virtual_current_profile profile = foc_virtual_current_default_profile();
    profile.current.current_limit_a = cap;
    profile.runtime.motion.iq_limit_a = cap;
    profile.runtime.motion.impedance = (struct foc_runtime_impedance_config){
        .enabled = true,
        .mechanical_frame_id = frame_id,
        .torque_constant_nm_per_a =
            (float)(1.5 * profile.motor.pole_pairs * profile.motor.psi_f_wb),
        .torque_limit_nm = 0.315f,
        .max_stiffness_nm_per_rad = 1.0f,
        .max_damping_nm_s_per_rad = 0.1f,
        .max_feedforward_nm = 0.3f,
    };
    CHECK(profile.runtime.motion.impedance.torque_constant_nm_per_a == 0.15f);
    CHECK(foc_virtual_current_init(vm, &profile) == 0);
    CHECK(foc_virtual_current_run_until(vm, 0U) == 0);
    CHECK(
        vm->runtime.pipeline.mock.output_confirmed &&
        vm->runtime.pipeline.safety.state == FOC_SAFE_STATE_DISABLED
    );
    CHECK(lifecycle(vm, FOC_RUNTIME_ACTION_SET_MODE, 200U, FOC_SAFE_STATE_DISABLED) == 0);
    CHECK(vm->runtime.motion_session == 1U);
    CHECK(foc_virtual_current_run_until(vm, 201U) == 0);
    CHECK(send_reference(vm, 0.0f, 0.65f, 0.048f, 0.0f) == FOC_RUNTIME_OK);
    CHECK(foc_virtual_current_run_until(vm, 500U) == 0);
    CHECK(lifecycle(vm, FOC_RUNTIME_ACTION_ARM, 600U, FOC_SAFE_STATE_READY) == 0);
    CHECK(lifecycle(vm, FOC_RUNTIME_ACTION_START, 800U, FOC_SAFE_STATE_RUNNING) == 0);
    return 0;
}

static int run_case(const struct study_case *test, struct metrics *out, bool trace)
{
    struct foc_virtual_current vm;
    CHECK(start_impedance(&vm, test->cap_a) == 0);
    *out = (struct metrics){0};
    uint32_t cap_run = 0U;
    uint32_t samples = 0U;
    for (uint32_t t = 0U; t < test->end_us; t += step_us) {
        const uint32_t capture = epoch_us + t;
        const bool reversing =
            !test->load && test->release_or_reverse_us != 0U && t >= test->release_or_reverse_us;
        const float reference = reversing ? -0.20f : test->position_rad;
        CHECK(foc_virtual_current_run_until(&vm, capture - 1U) == 0);
        CHECK(send_reference(&vm, reference, 0.65f, test->damping, 0.0f) == FOC_RUNTIME_OK);
        CHECK(foc_virtual_current_run_until(&vm, capture) == 0);
        if (test->load && (t == 800000U || t == 1600000U)) {
            CHECK(foc_virtual_motor_set_load(&vm.motor, t == 800000U ? 0.039 : 0.0) == 0);
        }
        const struct foc_virtual_motor_state measured = vm.motor.state;
        const unsigned index = atomic_load_explicit(&vm.runtime.sensor_slot, memory_order_acquire);
        CHECK(index < sizeof vm.runtime.sensor_slots / sizeof vm.runtime.sensor_slots[0]);
        const struct foc_runtime_sensor_slot *slot = &vm.runtime.sensor_slots[index];
        CHECK(
            slot->target_valid && slot->mode == FOC_RUNTIME_MODE_IMPEDANCE && slot->session == 1U
        );
        CHECK(slot->sample.captured_us == capture && slot->sample.seq == vm.encoder_seq);
        CHECK(
            slot->sample.mechanical_frame_id == frame_id && vm.encoder.velocity_seen &&
            slot->sample.mechanical_rad_s == vm.encoder.lpf_velocity_rad_s &&
            vm.encoder.previous_us == capture
        );
        CHECK(slot->target.seq == vm.command_seq && slot->target.produced_us == capture - 1U);
        CHECK(!vm.runtime.outer_trial_seen);
        const float iq_target = slot->target.target_a.q;
        CHECK(slot->target.target_a.d == 0.0f && fabsf(iq_target) <= test->cap_a);
        const uint32_t consume = capture + (capture % 200U == 0U ? 0U : 100U);
        CHECK(foc_virtual_current_run_until(&vm, consume) == 0);
        struct foc_runtime_snapshot snapshot;
        CHECK(foc_virtual_current_snapshot(&vm, &snapshot) == FOC_RUNTIME_OK);
        CHECK(
            snapshot.mode == FOC_RUNTIME_MODE_IMPEDANCE && snapshot.motion_session == 1U &&
            snapshot.state == FOC_SAFE_STATE_RUNNING && snapshot.faults == 0U
        );
        CHECK(
            snapshot.accepted_outer_seen && snapshot.accepted_outer_seq == slot->sample.seq &&
            snapshot.sensor_us == capture && snapshot.angle_prediction_us == consume - capture
        );
        CHECK(snapshot.command_seq == vm.command_seq && snapshot.command_us == capture - 1U);
        CHECK(
            vm.runtime.pipeline.target_a.q == iq_target &&
            vm.runtime.pipeline.target_a.d == slot->target.target_a.d
        );
        CHECK(
            snapshot.action == FOC_DEVICE_OUTPUT_UPDATE && snapshot.output_confirmed &&
            vm.output.accepted_updates == (consume - epoch_us) / 200U + 1U &&
            vm.output.rejected_updates == 0U
        );

        const double error = (double)reference - measured.mechanical_position_rad;
        CHECK(
            isfinite(error) && isfinite(measured.mechanical_velocity_rad_s) &&
            isfinite(measured.iq_a)
        );
        out->error_sq += error * error;
        out->peak_error = fmax(out->peak_error, fabs(error));
        out->overshoot = fmax(out->overshoot, reversing ? error : -error);
        out->peak_speed = fmax(out->peak_speed, fabs(measured.mechanical_velocity_rad_s));
        out->peak_actual_iq = fmax(out->peak_actual_iq, fabs(measured.iq_a));
        out->min_target = fmin(out->min_target, iq_target);
        out->max_target = fmax(out->max_target, iq_target);
        const bool capped = fabsf(iq_target) >= 0.999999f * test->cap_a;
        if (capped) {
            cap_run += step_us;
            out->capped_us += step_us;
            if (cap_run > out->max_capped_run_us) {
                out->max_capped_run_us = cap_run;
            }
        } else {
            cap_run = 0U;
        }
        const uint32_t change = test->load ? 800000U : test->release_or_reverse_us;
        if (change != 0U && t < change && t >= change - dwell_us) {
            out->pre_error = fmax(out->pre_error, fabs(error));
        }
        if (test->load && t >= 1500000U && t < 1600000U) {
            out->load_error_sum += error;
            out->load_samples++;
        }
        if (t >= test->end_us - dwell_us) {
            out->final_error = fmax(out->final_error, fabs(error));
        }
        out->final_pos = measured.mechanical_position_rad;
        if (t >= test->release_or_reverse_us && fabs(error) > 0.008) {
            out->settle_us = t - test->release_or_reverse_us + step_us;
        }
        samples++;
        if (trace) {
            printf(
                "TRACE,%s,%u,%u,%u,%u,%u,%u,%u,%u,%u,%.9g,%.12g,%.12g,%.12g,%.9g,%.9g,%.9g,%u\n",
                test->name,
                t,
                capture,
                slot->sample.seq,
                snapshot.command_seq,
                snapshot.command_us,
                snapshot.motion_session,
                consume,
                snapshot.angle_prediction_us,
                snapshot.updates,
                reference,
                measured.mechanical_position_rad,
                measured.mechanical_velocity_rad_s,
                measured.iq_a,
                iq_target,
                test->cap_a,
                vm.motor.load_torque_nm,
                (unsigned)capped
            );
        }
    }
    printf(
        "SUMMARY,%s,%u,%.10g,%.10g,%.10g,%u,%.10g,%.10g,%.10g,%.10g,%.10g,%.10g,%.10g,"
        "%.10g,%u,%u,%u\n",
        test->name,
        samples,
        sqrt(out->error_sq / samples),
        out->peak_error,
        out->overshoot,
        out->settle_us,
        out->peak_speed,
        out->peak_actual_iq,
        out->min_target,
        out->max_target,
        out->pre_error,
        out->load_samples ? out->load_error_sum / out->load_samples : 0.0,
        out->final_error,
        out->final_pos,
        out->capped_us,
        out->max_capped_run_us,
        vm.output.accepted_updates
    );
    return 0;
}

static int test_runtime_boundary(unsigned kind)
{
    struct foc_virtual_current vm;
    CHECK(start_impedance(&vm, 2.2f) == 0);
    CHECK(foc_virtual_current_run_until(&vm, 999U) == 0);
    CHECK(send_reference(&vm, 0.0f, 0.0f, 0.0f, 0.075f) == FOC_RUNTIME_OK);
    CHECK(foc_virtual_current_run_until(&vm, 1000U) == 0);
    CHECK(vm.runtime.pipeline.target_a.q == 0.5f && vm.output.accepted_updates == 1U);
    CHECK(foc_virtual_current_run_until(&vm, 1499U) == 0);
    CHECK(send_reference(&vm, 0.0f, 0.0f, 0.0f, -0.075f) == FOC_RUNTIME_OK);
    CHECK(foc_virtual_current_run_until(&vm, 1500U) == 0);
    const struct foc_runtime_sensor_slot *slot =
        &vm.runtime.sensor_slots[atomic_load(&vm.runtime.sensor_slot)];
    CHECK(
        slot->target_valid && slot->target.target_a.q == -0.5f && slot->target.seq == vm.command_seq
    );
    CHECK(
        vm.runtime.pipeline.command.seq == vm.command_seq - 1U && vm.output.accepted_updates == 3U
    );

    const struct foc_vector_dq integral = vm.runtime.pipeline.current.integral_v;
    const uint32_t session = vm.runtime.motion_session;
    uint32_t expected_updates = vm.output.accepted_updates;
    if (kind == 0U) {
        CHECK(foc_virtual_current_request_stop(&vm) == FOC_RUNTIME_OK);
        CHECK(foc_virtual_current_run_until(&vm, 1600U) == 0);
    } else if (kind == 1U) {
        CHECK(foc_virtual_current_reject_next_update(&vm) == 0);
        CHECK(foc_virtual_current_run_until(&vm, 1600U) == 0);
        CHECK(vm.output.rejected_updates == 1U);
        CHECK(
            vm.runtime.pipeline.current.integral_v.d == integral.d &&
            vm.runtime.pipeline.current.integral_v.q == integral.q
        );
    } else {
        CHECK(foc_virtual_current_run_until(&vm, 1600U) == 0);
        CHECK(vm.runtime.pipeline.target_a.q == -0.5f && vm.output.accepted_updates == 4U);
        if (kind == 2U) {
            CHECK(foc_virtual_current_run_until(&vm, 1800U) == 0);
            CHECK(foc_virtual_current_mark_sensor_fault(&vm, 2000U) == 0);
            expected_updates = vm.output.accepted_updates;
            for (uint32_t t = 2000U; t <= 3500U; t += step_us) {
                CHECK(foc_virtual_current_fail_next_sensor(&vm) == 0);
                CHECK(foc_virtual_current_run_until(&vm, t) == 0);
            }
            CHECK(vm.runtime.pipeline.safety.latched_faults & FOC_SAFE_FAULT_SAMPLE_INVALID);
        } else {
            CHECK(foc_virtual_current_run_until(&vm, 3800U) == 0);
            expected_updates = vm.output.accepted_updates;
            CHECK(foc_virtual_current_run_until(&vm, 4600U) == 0);
            CHECK(vm.runtime.pipeline.safety.latched_faults == FOC_SAFE_FAULT_COMMAND_TIMEOUT);
        }
    }
    CHECK(
        vm.output.accepted_updates == expected_updates &&
        vm.output.request.action == FOC_DEVICE_OUTPUT_INHIBIT &&
        vm.runtime.motion_session != session && !vm.runtime.accepted_outer_seen
    );
    CHECK(
        vm.runtime.pipeline.safety.state ==
        (kind <= 1U ? FOC_SAFE_STATE_DISABLED : FOC_SAFE_STATE_FAULT_LATCHED)
    );
    printf(
        "BOUNDARY,%u,%u,%u,%u,%u,%u,%u\n",
        kind,
        expected_updates,
        vm.output.accepted_updates,
        vm.output.rejected_updates,
        session,
        vm.runtime.motion_session,
        (unsigned)vm.runtime.pipeline.safety.latched_faults
    );
    if (kind == 3U) {
        const struct foc_runtime_command stale =
            vm.runtime.command_slots[atomic_load(&vm.runtime.command_slot)];
        CHECK(foc_runtime_request_timeout_clear(&vm.runtime, vm.virtual_time_us) == FOC_RUNTIME_OK);
        CHECK(foc_virtual_current_run_until(&vm, 5200U) == 0);
        struct foc_runtime_snapshot snapshot;
        CHECK(foc_virtual_current_snapshot(&vm, &snapshot) == FOC_RUNTIME_OK);
        CHECK(
            snapshot.clear_result == FOC_RUNTIME_CLEAR_APPLIED && snapshot.faults == 0U &&
            snapshot.state == FOC_SAFE_STATE_DISABLED && snapshot.output_confirmed &&
            snapshot.action == FOC_DEVICE_OUTPUT_INHIBIT && snapshot.updates == expected_updates
        );
        CHECK(
            foc_runtime_publish_impedance_command(&vm.runtime, &stale, 5200U) ==
            FOC_RUNTIME_ERR_ORDER
        );
        printf(
            "RECOVERY,timeout-clear,%u,%u,%u\n",
            snapshot.motion_session,
            snapshot.updates,
            (unsigned)snapshot.clear_result
        );
    }
    return 0;
}

struct static_case {
    const char *name;
    float cap_a;
    float load_nm;
    float feedforward_nm;
    float tolerance_rad;
    bool load_qualified;
    enum foc_impedance_feasibility_verdict expected;
};

static int test_static_hold(const struct static_case *test, bool trace)
{
    static const uint32_t load_on_us = 100000U;
    static const uint32_t load_off_us = 2000000U;
    static const uint32_t test_end_us = 3000000U;
    static const uint32_t tail_us = 300000U;
    static const float reference = 0.05f;
    struct foc_virtual_current vm;
    CHECK(start_impedance(&vm, test->cap_a) == 0);

    const struct foc_impedance_config config = {
        .stiffness_nm_per_rad = 0.65f,
        .damping_nm_s_per_rad = 0.048f,
        .torque_constant_nm_per_a = vm.runtime.config.motion.impedance.torque_constant_nm_per_a,
        .torque_limit_nm = vm.runtime.config.motion.impedance.torque_limit_nm,
    };
    const struct foc_impedance_feasibility_input input = {
        .live_iq_limit_a = test->cap_a,
        .external_load_nm = test->load_qualified ? test->load_nm : 0.0f,
        .torque_feedforward_nm = test->feedforward_nm,
        .position_tolerance_rad = test->tolerance_rad,
        .torque_model_id = 71U,
        .load_model_id = test->load_qualified ? 72U : 0U,
        .stationary_target = true,
        .support_torque_accounted = true,
        .current_tracking_qualified = true,
    };
    struct foc_impedance_feasibility_result verdict;
    CHECK(foc_impedance_feasibility_evaluate(&config, &input, &verdict) == FOC_STATUS_OK);
    CHECK(verdict.verdict == test->expected);

    double error_sum = 0.0, speed_sum = 0.0, iq_sum = 0.0;
    double load_tail_max_error = 0.0, final_max_error = 0.0;
    uint32_t limited_us = 0U, run_us = 0U, max_run_us = 0U, recover_us = 0U;
    uint32_t count = 0U;
    const uint32_t session = vm.runtime.motion_session;
    for (uint32_t t = 0U; t < test_end_us; t += step_us) {
        const uint32_t capture = epoch_us + t;
        const bool loaded = t >= load_on_us && t < load_off_us;
        const float feedforward = loaded ? test->feedforward_nm : 0.0f;
        CHECK(foc_virtual_current_run_until(&vm, capture - 1U) == 0);
        if (t == load_on_us || t == load_off_us) {
            CHECK(foc_virtual_motor_set_load(&vm.motor, loaded ? test->load_nm : 0.0) == 0);
        }
        CHECK(
            send_reference(
                &vm,
                reference,
                config.stiffness_nm_per_rad,
                config.damping_nm_s_per_rad,
                feedforward
            ) == FOC_RUNTIME_OK
        );
        CHECK(foc_virtual_current_run_until(&vm, capture) == 0);
        const struct foc_runtime_sensor_slot *slot =
            &vm.runtime
                 .sensor_slots[atomic_load_explicit(&vm.runtime.sensor_slot, memory_order_acquire)];
        CHECK(
            slot->target_valid && slot->mode == FOC_RUNTIME_MODE_IMPEDANCE &&
            slot->session == session && slot->sample.captured_us == capture &&
            slot->target.seq == vm.command_seq && !vm.runtime.outer_trial_seen
        );
        const float iq_request = slot->target.target_a.q;
        CHECK(slot->target.target_a.d == 0.0f && fabsf(iq_request) <= test->cap_a);
        const uint32_t consume = capture + (capture % 200U == 0U ? 0U : 100U);
        CHECK(foc_virtual_current_run_until(&vm, consume) == 0);
        const struct foc_virtual_motor_state *plant = &vm.motor.state;
        CHECK(
            vm.runtime.pipeline.target_a.q == iq_request &&
            vm.runtime.pipeline.safety.state == FOC_SAFE_STATE_RUNNING &&
            vm.runtime.motion_session == session && vm.output.rejected_updates == 0U &&
            vm.output.request.action == FOC_DEVICE_OUTPUT_UPDATE
        );
        const double error = (double)reference - plant->mechanical_position_rad;
        const bool at_iq_limit = fabsf(iq_request) >= 0.999999f * test->cap_a;
        if (loaded && at_iq_limit) {
            limited_us += step_us;
            run_us += step_us;
            if (run_us > max_run_us) {
                max_run_us = run_us;
            }
        } else {
            run_us = 0U;
        }
        if (t >= load_off_us - tail_us && t < load_off_us) {
            error_sum += error;
            speed_sum += plant->mechanical_velocity_rad_s;
            iq_sum += plant->iq_a;
            load_tail_max_error = fmax(load_tail_max_error, fabs(error));
            count++;
        }
        if (t >= load_off_us && fabs(error) > 0.008) {
            recover_us = t - load_off_us + step_us;
        }
        if (t >= test_end_us - tail_us) {
            final_max_error = fmax(final_max_error, fabs(error));
        }
        if (trace) {
            printf(
                "STATIC_TRACE,%s,%u,%u,%u,%u,%.9g,%.10g,%.10g,%.10g,%.10g,%.9g,%.9g,%u,%u,%u\n",
                test->name,
                t,
                capture,
                consume,
                slot->target.seq,
                reference,
                plant->mechanical_position_rad,
                error,
                plant->mechanical_velocity_rad_s,
                plant->iq_a,
                iq_request,
                feedforward,
                (unsigned)at_iq_limit,
                vm.output.accepted_updates,
                session
            );
        }
    }
    CHECK(count == tail_us / step_us);
    const double mean_error = error_sum / count;
    const double mean_speed = speed_sum / count;
    const double mean_iq = iq_sum / count;
    if (verdict.position_constrained) {
        CHECK(fabs(mean_error - verdict.equilibrium_error_rad) <= 0.003);
        CHECK(fabs(mean_speed) <= 0.010);
        CHECK(
            (load_tail_max_error <= test->tolerance_rad) ==
            (verdict.verdict == FOC_IMPEDANCE_FEASIBILITY_REACHABLE)
        );
    }
    if (verdict.at_torque_limit) {
        CHECK(verdict.torque_margin_nm == 0.0f && !verdict.position_constrained);
        CHECK(fabs(mean_iq - test->cap_a) < 0.005);
    }
    if (verdict.verdict == FOC_IMPEDANCE_FEASIBILITY_TORQUE_INSUFFICIENT) {
        CHECK(mean_speed < -0.05 && max_run_us >= 1000000U);
        CHECK(fabs(mean_iq - test->cap_a) < 0.005 && load_tail_max_error > test->tolerance_rad);
    }
    CHECK(final_max_error <= 0.008 && recover_us <= 650000U);
    const uint32_t updates = vm.output.accepted_updates;
    CHECK(foc_virtual_current_request_stop(&vm) == FOC_RUNTIME_OK);
    CHECK(foc_virtual_current_run_until(&vm, vm.virtual_time_us + 400U) == 0);
    CHECK(
        vm.output.accepted_updates == updates &&
        vm.output.request.action == FOC_DEVICE_OUTPUT_INHIBIT &&
        vm.runtime.motion_session != session && !vm.runtime.accepted_outer_seen
    );
    printf(
        "STATIC,%s,%u,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%u,%u,%u,%.10g,%.10g,%.10g,%.10g,"
        "%u,%u,%u,%.10g,%u,%u,%u,%u\n",
        test->name,
        (unsigned)verdict.verdict,
        test->cap_a,
        test->load_nm,
        test->feedforward_nm,
        test->tolerance_rad,
        verdict.torque_capacity_nm,
        verdict.torque_margin_nm,
        (unsigned)verdict.has_equilibrium,
        (unsigned)verdict.at_torque_limit,
        (unsigned)verdict.position_constrained,
        (double)verdict.equilibrium_error_rad,
        mean_error,
        mean_speed,
        mean_iq,
        limited_us,
        max_run_us,
        recover_us,
        final_max_error,
        updates,
        vm.output.accepted_updates,
        session,
        vm.runtime.motion_session
    );
    return 0;
}

static int run_static_hold_suite(bool trace)
{
    static const struct static_case cases[] = {
        {"reserve", 0.70f, 0.039f, 0.0f, 0.075f, true, FOC_IMPEDANCE_FEASIBILITY_REACHABLE},
        {"equal-capacity", 0.25f, 0.0375f, 0.0f, 0.075f, true, FOC_IMPEDANCE_FEASIBILITY_REACHABLE},
        {"beyond-capacity",
         0.25f,
         0.039f,
         0.0f,
         0.075f,
         true,
         FOC_IMPEDANCE_FEASIBILITY_TORQUE_INSUFFICIENT},
        {"tight-tolerance",
         0.70f,
         0.039f,
         0.0f,
         0.025f,
         true,
         FOC_IMPEDANCE_FEASIBILITY_POSITION_TOLERANCE_EXCEEDED},
        {"correct-feedforward",
         0.70f,
         0.039f,
         0.039f,
         0.025f,
         true,
         FOC_IMPEDANCE_FEASIBILITY_REACHABLE},
        {"wrong-feedforward",
         0.70f,
         0.039f,
         -0.039f,
         0.025f,
         true,
         FOC_IMPEDANCE_FEASIBILITY_POSITION_TOLERANCE_EXCEEDED},
        {"unknown-load", 0.70f, 0.039f, 0.0f, 0.075f, false, FOC_IMPEDANCE_FEASIBILITY_UNKNOWN},
        {"reverse-load", 0.70f, -0.039f, 0.0f, 0.075f, true, FOC_IMPEDANCE_FEASIBILITY_REACHABLE},
    };
    for (unsigned i = 0U; i < sizeof cases / sizeof cases[0]; ++i) {
        CHECK(test_static_hold(&cases[i], trace) == 0);
    }
    return 0;
}

/* Test Entry ------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--static-hold") == 0) {
        const bool static_trace = argc == 3 && strcmp(argv[2], "--trace") == 0;
        return (argc == 2 || static_trace) ? run_static_hold_suite(static_trace) : 2;
    }
    static const struct study_case cases[] = {
        {"hold", 1600000U, 0U, 0.30f, 0.048f, 2.2f, false},
        {"soft-damping", 1600000U, 0U, 0.30f, 0.006f, 2.2f, false},
        {"load", 2400000U, 1600000U, 0.30f, 0.048f, 2.2f, true},
        {"reversal", 1500000U, 700000U, 0.25f, 0.048f, 2.2f, false},
        {"limit-045", 1500000U, 0U, 0.65f, 0.048f, 0.45f, false},
        {"limit-025", 1500000U, 0U, 0.65f, 0.048f, 0.25f, false},
    };
    struct metrics result[sizeof cases / sizeof cases[0]];
    const bool trace = argc == 2 && strcmp(argv[1], "--trace") == 0;
    for (unsigned i = 0U; i < sizeof cases / sizeof cases[0]; ++i) {
        CHECK(run_case(&cases[i], &result[i], trace) == 0);
    }
    for (unsigned kind = 0U; kind < 4U; ++kind) {
        CHECK(test_runtime_boundary(kind) == 0);
    }
    const bool quality[] = {
        result[0].final_error <= 0.008 && result[0].settle_us <= 650000U &&
            result[0].overshoot <= 0.075,
        result[1].final_error <= 0.015 && result[1].settle_us > result[0].settle_us &&
            result[1].overshoot >= result[0].overshoot + 0.06,
        result[2].pre_error <= 0.008 && result[2].final_error <= 0.008 &&
            fabs(result[2].load_error_sum / result[2].load_samples - 0.060) <= 0.009 &&
            result[2].settle_us <= 650000U,
        result[3].pre_error <= 0.008 && result[3].final_error <= 0.008 &&
            result[3].min_target < 0.0 && result[3].max_target > 0.0 &&
            result[3].settle_us <= 650000U,
        result[4].max_capped_run_us >= 100000U && result[4].capped_us < cases[4].end_us &&
            result[4].final_error <= 0.012,
        result[5].max_capped_run_us >= 100000U && result[5].capped_us < cases[5].end_us &&
            result[5].final_error <= 0.012,
    };
    bool passed = true;
    for (unsigned i = 0U; i < sizeof cases / sizeof cases[0]; ++i) {
        printf("QUALITY,%s,%s\n", cases[i].name, quality[i] ? "PASS" : "QUALITY_FAIL");
        passed = passed && quality[i];
    }
    return passed ? 0 : 1;
}

/* ---------------------------------------- end of file ----------------------------------------- */
