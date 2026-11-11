/**
 * @file    test_foc_core.c
 * @brief   Host contract tests for transactional voltage-open-loop stepping.
 * @version v0.1.0
 * @date    2026.11.09
 *
 * Test role
 * ---------
 * This executable is the host acceptance owner for foc_step transactions.
 *
 * This test checks lifecycle, timing, state commit, deterministic failure
 * output, and repeatability of the G2 Core entry point.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_core.h"

#include <math.h>
#include <stdio.h>

#define TEST_ASSERT(condition)                                                                  \
    do {                                                                                        \
        if (!(condition)) {                                                                     \
            fprintf(stderr, "assertion failed: %s at %s:%d\n", #condition, __FILE__, __LINE__); \
            return 1;                                                                           \
        }                                                                                       \
    } while (0)

/* Private Helpers -------------------------------------------------------------- */

static int nearly_equal(float lhs, float rhs, float tolerance)
{
    return fabsf(lhs - rhs) <= tolerance;
}

static int safe_output_is_complete(const struct foc_output *output)
{
    return output->output_enable == 0U && output->voltage_alpha_beta_v.alpha == 0.0f &&
        output->voltage_alpha_beta_v.beta == 0.0f && output->svpwm.duty_a == 0.5f &&
        output->svpwm.duty_b == 0.5f && output->svpwm.duty_c == 0.5f &&
        output->svpwm.sector == 0U && output->svpwm.saturated != 0U &&
        isfinite(output->theta_elec_rad) != 0;
}

/* Test Entry ------------------------------------------------------------------ */

int main(void)
{
    const struct foc_config config = {
        .vbus_min_v = 6.0f,
        .voltage_limit_v = 4.0f,
        .duty_min = 0.28f,
        .duty_max = 0.72f,
        .max_dt_s = 0.01f,
    };
    const struct foc_validated_motion_command command = {
        .mode = FOC_CONTROL_VOLTAGE_OPEN_LOOP,
        .enable = 1U,
        .vd_v = 0.0f,
        .vq_v = 1.0f,
        .electrical_speed_rad_s = 100.0f,
    };
    const struct foc_measurement measurement = {.vbus_v = 12.0f};
    const struct foc_timing timing = {.dt_s = 0.001f};
    struct foc_context context = {0};
    struct foc_output output = {0};

    TEST_ASSERT(foc_config_validate(&config) == FOC_STATUS_OK);
    {
        struct foc_config invalid_config = config;

        invalid_config.duty_min = 0.8f;
        TEST_ASSERT(foc_config_validate(&invalid_config) == FOC_STATUS_ERR_INVALID_CONFIG);
        invalid_config = config;
        invalid_config.vbus_min_v = NAN;
        TEST_ASSERT(foc_config_validate(&invalid_config) == FOC_STATUS_ERR_NONFINITE);
    }
    TEST_ASSERT(foc_init(&context, &config) == FOC_STATUS_OK);
    TEST_ASSERT(foc_step(&context, &command, &measurement, &timing, &output) == FOC_STATUS_OK);
    TEST_ASSERT(output.output_enable != 0U);
    TEST_ASSERT(nearly_equal(output.theta_elec_rad, 0.1f, 1.0e-6f));
    TEST_ASSERT(output.svpwm.duty_a >= config.duty_min);
    TEST_ASSERT(output.svpwm.duty_a <= config.duty_max);

    {
        struct foc_validated_motion_command limited_command = command;

        limited_command.vq_v = 10.0f;
        TEST_ASSERT(
            foc_step(&context, &limited_command, &measurement, &timing, &output) == FOC_STATUS_OK
        );
        TEST_ASSERT(output.output_enable != 0U);
        TEST_ASSERT(output.svpwm.saturated != 0U);
    }

    {
        const float previous_theta = context.theta_elec_rad;
        const struct foc_timing invalid_timing = {.dt_s = 0.0f};

        TEST_ASSERT(
            foc_step(&context, &command, &measurement, &invalid_timing, &output) ==
            FOC_STATUS_ERR_INVALID_DT
        );
        TEST_ASSERT(safe_output_is_complete(&output));
        TEST_ASSERT(context.theta_elec_rad == previous_theta);
    }

    {
        const float previous_theta = context.theta_elec_rad;
        const struct foc_timing invalid_timing = {.dt_s = 0.02f};

        TEST_ASSERT(
            foc_step(&context, &command, &measurement, &invalid_timing, &output) ==
            FOC_STATUS_ERR_INVALID_DT
        );
        TEST_ASSERT(safe_output_is_complete(&output));
        TEST_ASSERT(context.theta_elec_rad == previous_theta);
    }

    {
        struct foc_validated_motion_command disabled = command;
        disabled.enable = 0U;
        TEST_ASSERT(foc_step(&context, &disabled, &measurement, &timing, &output) == FOC_STATUS_OK);
        TEST_ASSERT(safe_output_is_complete(&output));
        TEST_ASSERT(nearly_equal(context.theta_elec_rad, 0.2f, 1.0e-6f));
    }

    {
        const struct foc_measurement invalid_measurement = {.vbus_v = NAN};
        TEST_ASSERT(
            foc_step(&context, &command, &invalid_measurement, &timing, &output) ==
            FOC_STATUS_ERR_INVALID_VBUS
        );
        TEST_ASSERT(safe_output_is_complete(&output));
    }

    {
        const struct foc_measurement invalid_measurement = {.vbus_v = 5.0f};

        TEST_ASSERT(
            foc_step(&context, &command, &invalid_measurement, &timing, &output) ==
            FOC_STATUS_ERR_INVALID_VBUS
        );
        TEST_ASSERT(safe_output_is_complete(&output));
    }

    {
        struct foc_validated_motion_command invalid_command = command;
        const float previous_theta = context.theta_elec_rad;

        invalid_command.vd_v = NAN;
        TEST_ASSERT(
            foc_step(&context, &invalid_command, &measurement, &timing, &output) ==
            FOC_STATUS_ERR_NONFINITE
        );
        TEST_ASSERT(safe_output_is_complete(&output));
        TEST_ASSERT(context.theta_elec_rad == previous_theta);
    }

    {
        struct foc_validated_motion_command wrapped_command = command;
        struct foc_context first_context = {0};
        struct foc_context second_context = {0};
        struct foc_output first_output = {0};
        struct foc_output second_output = {0};
        const struct foc_timing fast_timing = {.dt_s = 0.001f};

        wrapped_command.electrical_speed_rad_s = 7000.0f;
        TEST_ASSERT(foc_init(&first_context, &config) == FOC_STATUS_OK);
        TEST_ASSERT(foc_init(&second_context, &config) == FOC_STATUS_OK);
        TEST_ASSERT(
            foc_step(&first_context, &wrapped_command, &measurement, &fast_timing, &first_output) ==
            FOC_STATUS_OK
        );
        TEST_ASSERT(
            foc_step(
                &second_context,
                &wrapped_command,
                &measurement,
                &fast_timing,
                &second_output
            ) == FOC_STATUS_OK
        );
        TEST_ASSERT(nearly_equal(
            first_context.theta_elec_rad,
            7000.0f * 0.001f - 6.28318530717958647692f,
            1.0e-5f
        ));
        TEST_ASSERT(
            nearly_equal(first_output.theta_elec_rad, second_output.theta_elec_rad, 1.0e-6f)
        );
        TEST_ASSERT(nearly_equal(first_output.svpwm.duty_a, second_output.svpwm.duty_a, 1.0e-6f));
        TEST_ASSERT(foc_reset(&first_context) == FOC_STATUS_OK);
        TEST_ASSERT(first_context.theta_elec_rad == 0.0f);
    }

    {
        struct foc_validated_motion_command invalid_mode = command;

        invalid_mode.mode = (enum foc_control_mode)99;
        TEST_ASSERT(
            foc_step(&context, &invalid_mode, &measurement, &timing, &output) ==
            FOC_STATUS_ERR_INVALID_CONFIG
        );
        TEST_ASSERT(safe_output_is_complete(&output));
    }

    {
        struct foc_validated_motion_command integration_command = command;
        struct foc_context coarse_context = {0};
        struct foc_context fine_context = {0};
        struct foc_output coarse_output = {0};
        struct foc_output fine_output = {0};
        const struct foc_timing coarse_timing = {.dt_s = 0.001f};
        const struct foc_timing fine_timing = {.dt_s = 0.0005f};
        unsigned int step;

        integration_command.electrical_speed_rad_s = 123.4f;
        TEST_ASSERT(foc_init(&coarse_context, &config) == FOC_STATUS_OK);
        TEST_ASSERT(foc_init(&fine_context, &config) == FOC_STATUS_OK);
        for (step = 0U; step < 100U; ++step) {
            TEST_ASSERT(
                foc_step(
                    &coarse_context,
                    &integration_command,
                    &measurement,
                    &coarse_timing,
                    &coarse_output
                ) == FOC_STATUS_OK
            );
        }
        for (step = 0U; step < 200U; ++step) {
            TEST_ASSERT(
                foc_step(
                    &fine_context,
                    &integration_command,
                    &measurement,
                    &fine_timing,
                    &fine_output
                ) == FOC_STATUS_OK
            );
        }
        TEST_ASSERT(
            nearly_equal(coarse_context.theta_elec_rad, fine_context.theta_elec_rad, 1.0e-4f)
        );
        TEST_ASSERT(nearly_equal(coarse_output.svpwm.duty_a, fine_output.svpwm.duty_a, 1.0e-4f));
        TEST_ASSERT(nearly_equal(coarse_output.svpwm.duty_b, fine_output.svpwm.duty_b, 1.0e-4f));
        TEST_ASSERT(nearly_equal(coarse_output.svpwm.duty_c, fine_output.svpwm.duty_c, 1.0e-4f));
    }

    {
        context.theta_elec_rad = NAN;
        TEST_ASSERT(
            foc_step(&context, &command, &measurement, &timing, &output) == FOC_STATUS_ERR_NONFINITE
        );
        TEST_ASSERT(safe_output_is_complete(&output));
    }

    TEST_ASSERT(foc_step(NULL, &command, &measurement, &timing, &output) == FOC_STATUS_ERR_NULL);
    TEST_ASSERT(safe_output_is_complete(&output));

    puts("test_foc_core passed");
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
