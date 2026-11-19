#include "foc_impedance_feedforward.h"
#include "foc_impedance_tuning.h"
#include "foc_impedance_feasibility.h"
#include "foc_virtual_current.h"

#include <math.h>
#include <stdio.h>

#define EXPECT(expression)                                                        \
    do {                                                                          \
        if (!(expression)) {                                                      \
            fprintf(stderr, "external consumer:%d: %s\n", __LINE__, #expression); \
            return 1;                                                             \
        }                                                                         \
    } while (0)

static int action(
    struct foc_virtual_current *vm,
    enum foc_runtime_action op,
    uint32_t h0,
    enum foc_safe_state state
)
{
    EXPECT(foc_virtual_current_run_until(vm, h0 - 1U) == 0);
    EXPECT(
        foc_virtual_current_publish_lifecycle(vm, op, FOC_RUNTIME_MODE_IMPEDANCE) == FOC_RUNTIME_OK
    );
    EXPECT(foc_virtual_current_run_until(vm, h0) == 0);
    struct foc_runtime_lifecycle_result result;
    EXPECT(foc_virtual_current_read_lifecycle(vm, &result) == FOC_RUNTIME_OK);
    EXPECT(
        result.result == FOC_RUNTIME_REQUEST_APPLIED && result.action == op &&
        result.state == state && result.h0_us == h0
    );
    printf(
        "external lifecycle action=%u H0_us=%u session=%u state=%u\n",
        (unsigned)op,
        (unsigned)result.h0_us,
        (unsigned)result.applied_session,
        (unsigned)result.state
    );
    return 0;
}

static int hold(
    struct foc_virtual_current *vm,
    const struct foc_impedance_tuning_candidate *gains,
    uint32_t seq,
    uint32_t producer_us,
    float position
)
{
    struct foc_runtime_snapshot status;
    EXPECT(foc_virtual_current_run_until(vm, producer_us) == 0);
    EXPECT(foc_virtual_current_snapshot(vm, &status) == FOC_RUNTIME_OK);
    EXPECT(status.mode == FOC_RUNTIME_MODE_IMPEDANCE);
    const struct foc_runtime_command command = {
        .mode = FOC_RUNTIME_MODE_IMPEDANCE,
        .seq = seq,
        .produced_us = producer_us,
        .target.impedance = {
            .position_rad = position,
            .stiffness_nm_per_rad = gains->stiffness_nm_per_rad,
            .damping_nm_s_per_rad = gains->damping_nm_s_per_rad,
            .mechanical_frame_id = 81U,
            .source_session = status.motion_session
        }
    };
    EXPECT(
        foc_runtime_publish_impedance_command(&vm->runtime, &command, producer_us) == FOC_RUNTIME_OK
    );
    return 0;
}

int main(void)
{
    struct foc_virtual_current_profile profile = foc_virtual_current_default_profile();
    profile.runtime.motion.impedance = (struct foc_runtime_impedance_config){
        .enabled = true,
        .mechanical_frame_id = 81U,
        .torque_constant_nm_per_a = 0.15f,
        .torque_limit_nm = 0.315f,
        .max_stiffness_nm_per_rad = 1.0f,
        .max_damping_nm_s_per_rad = 0.1f,
        .max_feedforward_nm = 0.1f
    };
    const struct foc_impedance_tuning_input design = {
        .model_id = 2U,
        .damping_source = FOC_IMPEDANCE_TUNING_DAMPING_PROVIDED,
        .inertia_kg_m2 = 0.002f,
        .viscous_damping_nm_s_per_rad = 0.006f,
        .target_natural_frequency_rad_s = 18.0f,
        .target_damping_ratio = 0.75f,
        .torque_constant_nm_per_a = 0.15f,
        .max_stiffness_nm_per_rad = 1.0f,
        .max_damping_nm_s_per_rad = 0.1f,
        .iq_limit_a = 2.2f,
        .sample_period_s = 0.0005f,
        .extra_delay_s = 0.0004f
    };
    struct foc_impedance_tuning_candidate gains;
    EXPECT(foc_impedance_tuning_design(&design, &gains) == FOC_IMPEDANCE_TUNING_OK && gains.valid);
    const struct foc_impedance_feedforward_input input = {
        .inertia_kg_m2 = design.inertia_kg_m2,
        .viscous_damping_nm_s_per_rad = design.viscous_damping_nm_s_per_rad,
        .reference_velocity_rad_s = 0.5f,
        .reference_acceleration_rad_s2 = 1.0f,
        .limit_nm = 0.1f,
        .model_id = design.model_id,
        .model_frame_id = 81U,
        .reference_frame_id = 81U,
        .reference_qualified = true
    };
    struct foc_impedance_feedforward_result ff;
    EXPECT(
        foc_impedance_feedforward_evaluate(&input, &ff) == FOC_IMPEDANCE_FEEDFORWARD_OK &&
        ff.valid && fabsf(ff.torque_nm - 0.005f) < 1e-7f
    );
    const struct foc_impedance_config impedance = {
        .stiffness_nm_per_rad = gains.stiffness_nm_per_rad,
        .damping_nm_s_per_rad = gains.damping_nm_s_per_rad,
        .torque_constant_nm_per_a = 0.15f,
        .torque_limit_nm = 0.315f
    };
    const struct foc_impedance_feasibility_input budget = {
        .live_iq_limit_a = 0.25f,
        .external_load_nm = 0.039f,
        .position_tolerance_rad = 0.08f,
        .torque_model_id = 2U,
        .load_model_id = 3U,
        .stationary_target = true,
        .support_torque_accounted = true,
        .current_tracking_qualified = true
    };
    struct foc_impedance_feasibility_result feasibility;
    EXPECT(
        foc_impedance_feasibility_evaluate(&impedance, &budget, &feasibility) == FOC_STATUS_OK &&
        feasibility.verdict == FOC_IMPEDANCE_FEASIBILITY_TORQUE_INSUFFICIENT
    );

    struct foc_virtual_current vm;
    EXPECT(foc_virtual_current_init(&vm, &profile) == 0);
    EXPECT(action(&vm, FOC_RUNTIME_ACTION_SET_MODE, 200U, FOC_SAFE_STATE_DISABLED) == 0);
    EXPECT(hold(&vm, &gains, 1U, 201U, 0.0f) == 0);
    EXPECT(action(&vm, FOC_RUNTIME_ACTION_ARM, 600U, FOC_SAFE_STATE_READY) == 0);
    EXPECT(action(&vm, FOC_RUNTIME_ACTION_START, 800U, FOC_SAFE_STATE_RUNNING) == 0);
    uint32_t seq = 2U;
    for (uint32_t h1 = 1000U; h1 <= 600000U; h1 += 500U) {
        EXPECT(hold(&vm, &gains, seq++, h1 - 1U, 0.10f) == 0);
        EXPECT(foc_virtual_current_run_until(&vm, h1) == 0);
    }
    struct foc_virtual_current_observation read;
    EXPECT(
        foc_virtual_current_observe(&vm, &read) == 0 && read.accepted_updates > 1000U &&
        fabs(read.mechanical_position_rad - 0.1) < 0.008
    );
    struct foc_runtime_snapshot running;
    EXPECT(foc_virtual_current_snapshot(&vm, &running) == FOC_RUNTIME_OK);
    EXPECT(
        running.mode == FOC_RUNTIME_MODE_IMPEDANCE && running.state == FOC_SAFE_STATE_RUNNING &&
        running.motion_session == read.motion_session && running.command_seen &&
        running.command_seq == seq - 1U && running.command_us == 599999U &&
        running.accepted_outer_seen && running.sensor_valid && running.sensor_us == 600000U &&
        running.output_confirmed && running.action == FOC_DEVICE_OUTPUT_UPDATE
    );
    printf(
        "external identity session=%u command_seq=%u producer_us=%u sensor_seq=%u sensor_us=%u\n",
        (unsigned)running.motion_session,
        (unsigned)running.command_seq,
        (unsigned)running.command_us,
        (unsigned)running.accepted_outer_seq,
        (unsigned)running.sensor_us
    );
    const uint32_t before = read.accepted_updates;
    const uint32_t session = read.motion_session;
    EXPECT(foc_virtual_current_request_stop(&vm) == FOC_RUNTIME_OK);
    EXPECT(foc_virtual_current_run_until(&vm, 600400U) == 0);
    struct foc_runtime_snapshot stopped;
    EXPECT(
        foc_virtual_current_snapshot(&vm, &stopped) == FOC_RUNTIME_OK &&
        stopped.updates == before && stopped.motion_session != session &&
        stopped.action == FOC_DEVICE_OUTPUT_INHIBIT && stopped.output_confirmed
    );
    printf(
        "external servo PASS position_rad=%.6f updates=%u->%u INHIBIT physical_applied=false\n",
        read.mechanical_position_rad,
        (unsigned)before,
        (unsigned)stopped.updates
    );
    return 0;
}
