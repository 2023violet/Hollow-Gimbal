/**
 * @file    core_consumer.c
 * @brief   Host caller of the installed C11 Core and a software-only output gate.
 * @version v0.1.0
 * @date    2026.11.20
 *
 * A synthetic encoder and ADC capture share one instant. The caller owns
 * sample admission, staged state, RUN permission, STOP and the fault latch.
 */

#include "foc_current.h"
#include "foc_motion.h"

#include <stdio.h>

#define CHECK(condition)                                                                          \
    do {                                                                                          \
        if (!(condition)) {                                                                       \
            fprintf(stderr, "Core consumer check failed at line %d: %s\n", __LINE__, #condition); \
            return 1;                                                                             \
        }                                                                                         \
    } while (0)

struct mock_output {
    bool run_permitted;
    bool fault_latched;
    bool update_applied;
    unsigned int updates;
    struct foc_svpwm_output request;
};

static bool control_cycle(
    struct foc_current_context *current,
    struct foc_velocity_context *velocity,
    struct foc_current_input sample,
    float velocity_ref_rad_s,
    bool capture_valid,
    struct mock_output *mock
)
{
    mock->update_applied = false;
    mock->request = (struct foc_svpwm_output){0};
    if (!capture_valid || !mock->run_permitted || mock->fault_latched) {
        return false;
    }

    const struct foc_velocity_input velocity_input = {
        .ref_rad_s = velocity_ref_rad_s,
        .measured_rad_s = 0.0f,
        .dt_s = sample.dt_s,
        .iq_limit_a = 1.0f,
        .enabled = true,
    };
    struct foc_velocity_context velocity_trial = *velocity;
    struct foc_velocity_output target;
    if (foc_velocity_step(&velocity_trial, &velocity_input, &target) != FOC_STATUS_OK ||
        !target.enabled) {
        return false;
    }

    sample.target_a.q = target.iq_ref_a;
    struct foc_current_context current_trial = *current;
    struct foc_current_output request;
    if (foc_current_step(&current_trial, &sample, &request) != FOC_STATUS_OK ||
        request.output_enable == 0U) {
        return false;
    }

    /* Synchronous Host sink. Only this serial caller owns permission and output. */
    mock->request = request.svpwm;
    ++mock->updates;
    mock->update_applied = true;
    *current = current_trial;
    *velocity = velocity_trial;
    return true;
}

int main(void)
{
    const struct foc_rotor_config rotor = {.pole_pairs = 4U, .electrical_offset_rad = 0.1f};
    const struct foc_current_config current_config = {
        .pwm = {1.0f, 2.4f, 0.05f, 0.95f, 0.0003f},
        .kp_v_per_a = {1.08f, 1.35f},
        .ki_v_per_a_s = {540.0f, 540.0f},
        .current_limit_a = 2.5f,
    };
    const struct foc_velocity_config velocity_config = {0.18f, 1.4f, 0.002f};
    struct foc_current_context current = {0};
    struct foc_velocity_context velocity = {0};
    struct mock_output output = {.run_permitted = true};
    struct foc_current_input sample = {
        .phase_current_a = {0.01f, -0.02f, 0.01f},
        .vbus_v = 12.0f,
        .dt_s = 0.0001f,
    };

    CHECK(foc_current_init(&current, &current_config) == FOC_STATUS_OK);
    CHECK(foc_velocity_init(&velocity, &velocity_config) == FOC_STATUS_OK);
    CHECK(foc_rotor_phase_from_counts(&rotor, 16U, 4096U, &sample.theta_elec_rad) == FOC_STATUS_OK);
    CHECK(control_cycle(&current, &velocity, sample, 4.0f, true, &output));
    CHECK(output.update_applied && output.request.sector >= 1U && output.request.sector <= 6U);
    CHECK(output.request.duty_b > output.request.duty_c);
    CHECK(control_cycle(&current, &velocity, sample, -4.0f, true, &output));
    CHECK(output.update_applied && output.request.duty_b < output.request.duty_c);

    CHECK(!control_cycle(&current, &velocity, sample, 4.0f, false, &output));
    CHECK(!output.update_applied && output.updates == 2U);

    output.run_permitted = false;
    CHECK(!control_cycle(&current, &velocity, sample, 4.0f, true, &output));
    CHECK(!output.update_applied && output.updates == 2U);
    CHECK(foc_current_reset(&current) == FOC_STATUS_OK);
    CHECK(foc_velocity_reset(&velocity) == FOC_STATUS_OK);

    /* A latched fault overrides a renewed application RUN request. */
    output.run_permitted = true;
    output.fault_latched = true;
    CHECK(!control_cycle(&current, &velocity, sample, 4.0f, true, &output));
    CHECK(!output.update_applied && output.updates == 2U);

    puts("external Core: signed velocity and SVPWM, invalid capture, STOP, and FAULT passed");
    return 0;
}
