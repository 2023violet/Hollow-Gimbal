/**
 * @file    test_foc_feedforward.c
 * @brief   Validate optional d/q PMSM feedforward and a coupled current model.
 * @version v0.1.0
 * @date    2026.11.13
 *
 * Test role
 * ---------
 * Check the independent feedforward equations, disabled-path equivalence,
 * transactional anti-windup composition, and a clearly labeled synthetic PMSM
 * model containing cross-coupling and permanent-magnet back EMF.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_current.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* Private Macros --------------------------------------------------------------- */

#define TEST_ASSERT(condition)                                                                  \
    do {                                                                                        \
        if (!(condition)) {                                                                     \
            fprintf(stderr, "assertion failed: %s at %s:%d\n", #condition, __FILE__, __LINE__); \
            return 1;                                                                           \
        }                                                                                       \
    } while (0)

/* Private Types ---------------------------------------------------------------- */

enum {
    MODEL_STEPS = 5000U,
    MODEL_MEASUREMENT_START = 1000U
};

struct model_metrics {
    bool completed;
    int status;
    unsigned int completed_steps;
    unsigned int measured_samples;
    float rmse_d;
    float rmse_q;
    float max_error;
    unsigned int saturated_steps;
};

/* Private Helpers -------------------------------------------------------------- */

static int near(float actual, float expected, float tolerance)
{
    return fabsf(actual - expected) <= tolerance;
}

static struct foc_current_config controller_config(void)
{
    return (struct foc_current_config){
        .pwm =
            {.vbus_min_v = 6.0f,
             .voltage_limit_v = 12.0f,
             .duty_min = 0.05f,
             .duty_max = 0.95f,
             .max_dt_s = 0.001f},
        .kp_v_per_a = {2.0f, 2.0f},
        .ki_v_per_a_s = {900.0f, 900.0f},
        .current_limit_a = 8.0f,
    };
}

static struct foc_current_feedforward_model motor_model(void)
{
    return (
        struct foc_current_feedforward_model
    ){.ld_h = 0.001f, .lq_h = 0.0015f, .psi_f_wb = 0.05f};
}

static struct foc_vector_abc phase_from_dq(struct foc_vector_dq dq)
{
    const float sqrt3_over_two = 0.86602540378443864676f;

    return (struct foc_vector_abc){
        dq.d,
        -0.5f * dq.d + sqrt3_over_two * dq.q,
        -0.5f * dq.d - sqrt3_over_two * dq.q,
    };
}

static struct model_metrics
run_model(bool enabled, float omega_e_rad_s, struct foc_current_feedforward_model feedforward_model)
{
    const struct foc_current_config config = controller_config();
    const struct foc_current_input empty_input = {
        .theta_elec_rad = 0.0f,
        .vbus_v = 24.0f,
        .dt_s = 0.0001f,
    };
    struct foc_current_context controller = {0};
    struct foc_current_feedforward_context feedforward = {0};
    struct foc_current_output output;
    struct foc_vector_dq current = {0};
    const float dt_s = empty_input.dt_s;
    const float resistance = 0.3f;
    const float ld = 0.001f;
    const float lq = 0.0015f;
    const float psi_f = 0.05f;
    struct model_metrics metrics = {.rmse_d = NAN, .rmse_q = NAN};
    float sum_d = 0.0f;
    float sum_q = 0.0f;

    metrics.status = foc_current_init(&controller, &config);
    if (metrics.status != FOC_STATUS_OK) {
        fprintf(stderr, "pmsm current_init status=%d\n", metrics.status);
        return metrics;
    }
    metrics.status = foc_current_feedforward_init(&feedforward, &feedforward_model, enabled);
    if (metrics.status != FOC_STATUS_OK) {
        fprintf(stderr, "pmsm feedforward_init status=%d\n", metrics.status);
        return metrics;
    }
    for (unsigned int step = 0U; step < MODEL_STEPS; ++step) {
        struct foc_current_input input = empty_input;
        input.phase_current_a = phase_from_dq(current);
        input.target_a = (struct foc_vector_dq){0.0f, step < 500U ? 0.0f : 3.0f};
        metrics.status = enabled ? foc_current_step_with_feedforward(
                                       &controller,
                                       &input,
                                       &feedforward,
                                       omega_e_rad_s,
                                       &output
                                   )
                                 : foc_current_step(&controller, &input, &output);
        if (metrics.status != FOC_STATUS_OK) {
            return metrics;
        }
        if (output.svpwm.saturated != 0U) {
            ++metrics.saturated_steps;
        }
        const float error_d = input.target_a.d - current.d;
        const float error_q = input.target_a.q - current.q;
        if (step >= MODEL_MEASUREMENT_START) {
            sum_d += error_d * error_d;
            sum_q += error_q * error_q;
            ++metrics.measured_samples;
        }
        const float magnitude = sqrtf(error_d * error_d + error_q * error_q);
        if (magnitude > metrics.max_error) {
            metrics.max_error = magnitude;
        }

        const float vd = output.voltage_dq_v.d;
        const float vq = output.voltage_dq_v.q;
        const float did = (vd - resistance * current.d + omega_e_rad_s * lq * current.q) / ld;
        const float diq =
            (vq - resistance * current.q - omega_e_rad_s * (ld * current.d + psi_f)) / lq;
        current.d += dt_s * did;
        current.q += dt_s * diq;
        if (!isfinite(current.d) || !isfinite(current.q)) {
            metrics.status = FOC_STATUS_ERR_NONFINITE;
            return metrics;
        }
        ++metrics.completed_steps;
    }
    if (metrics.completed_steps != MODEL_STEPS ||
        metrics.measured_samples != MODEL_STEPS - MODEL_MEASUREMENT_START ||
        metrics.measured_samples == 0U) {
        return metrics;
    }
    metrics.rmse_d = sqrtf(sum_d / (float)metrics.measured_samples);
    metrics.rmse_q = sqrtf(sum_q / (float)metrics.measured_samples);
    if (!isfinite(metrics.rmse_d) || !isfinite(metrics.rmse_q) || !isfinite(metrics.max_error)) {
        metrics.status = FOC_STATUS_ERR_NONFINITE;
        return metrics;
    }
    metrics.completed = true;
    return metrics;
}

static bool model_is_valid(const char *name, struct model_metrics metrics)
{
    const bool valid = metrics.completed && metrics.status == FOC_STATUS_OK &&
        metrics.completed_steps == MODEL_STEPS &&
        metrics.measured_samples == MODEL_STEPS - MODEL_MEASUREMENT_START &&
        metrics.measured_samples > 0U && isfinite(metrics.rmse_d) && isfinite(metrics.rmse_q) &&
        isfinite(metrics.max_error);

    fprintf(
        valid ? stdout : stderr,
        "pmsm trial=%s completed=%u status=%d steps=%u/%u samples=%u/%u "
        "rmse=(%.7g,%.7g) max=%.7g\n",
        name,
        (unsigned int)metrics.completed,
        metrics.status,
        metrics.completed_steps,
        (unsigned int)MODEL_STEPS,
        metrics.measured_samples,
        (unsigned int)(MODEL_STEPS - MODEL_MEASUREMENT_START),
        metrics.rmse_d,
        metrics.rmse_q,
        metrics.max_error
    );
    return valid;
}

/* Public Functions ------------------------------------------------------------- */

static int test_equations_and_validation(void)
{
    struct foc_current_feedforward_context context = {0};
    struct foc_current_feedforward_output output;
    struct foc_current_feedforward_model model = motor_model();

    TEST_ASSERT(foc_current_feedforward_init(&context, &model, true) == FOC_STATUS_OK);
    TEST_ASSERT(
        foc_current_feedforward_compute(
            &context,
            100.0f,
            (struct foc_vector_dq){1.0f, 2.0f},
            &output
        ) == FOC_STATUS_OK
    );
    TEST_ASSERT(near(output.voltage_d_v, -0.3f, 1.0e-6f));
    TEST_ASSERT(near(output.voltage_q_v, 5.1f, 1.0e-6f));
    TEST_ASSERT(
        foc_current_feedforward_compute(
            &context,
            -100.0f,
            (struct foc_vector_dq){1.0f, 2.0f},
            &output
        ) == FOC_STATUS_OK
    );
    TEST_ASSERT(near(output.voltage_d_v, 0.3f, 1.0e-6f));
    TEST_ASSERT(near(output.voltage_q_v, -5.1f, 1.0e-6f));
    TEST_ASSERT(
        foc_current_feedforward_compute(
            &context,
            NAN,
            (struct foc_vector_dq){1.0f, 2.0f},
            &output
        ) == FOC_STATUS_ERR_NONFINITE
    );
    TEST_ASSERT(
        foc_current_feedforward_compute(
            &context,
            0.0f,
            (struct foc_vector_dq){-3.0f, 2.0f},
            &output
        ) == FOC_STATUS_OK
    );
    TEST_ASSERT(output.voltage_d_v == 0.0f && output.voltage_q_v == 0.0f);

    struct foc_current_feedforward_model invalid = model;
    invalid.ld_h = 0.0f;
    TEST_ASSERT(
        foc_current_feedforward_init(&context, &invalid, true) == FOC_STATUS_ERR_INVALID_CONFIG
    );
    TEST_ASSERT(foc_current_feedforward_init(&context, NULL, false) == FOC_STATUS_OK);
    TEST_ASSERT(
        foc_current_feedforward_compute(
            &context,
            NAN,
            (struct foc_vector_dq){NAN, 0.0f},
            &output
        ) == FOC_STATUS_OK
    );
    TEST_ASSERT(!output.enabled && output.voltage_d_v == 0.0f && output.voltage_q_v == 0.0f);
    return 0;
}

static int test_disabled_equivalence_and_state(void)
{
    const struct foc_current_config config = controller_config();
    const struct foc_current_feedforward_model model = motor_model();
    struct foc_current_feedforward_context disabled = {0};
    struct foc_current_context original = {0};
    struct foc_current_context optional = {0};
    struct foc_current_output first;
    struct foc_current_output second;
    struct foc_current_input input = {
        .phase_current_a = {0.2f, -0.1f, -0.1f},
        .theta_elec_rad = 0.37f,
        .target_a = {0.1f, 1.2f},
        .vbus_v = 24.0f,
        .dt_s = 0.0001f,
    };

    TEST_ASSERT(foc_current_init(&original, &config) == FOC_STATUS_OK);
    TEST_ASSERT(foc_current_init(&optional, &config) == FOC_STATUS_OK);
    TEST_ASSERT(foc_current_feedforward_init(&disabled, &model, false) == FOC_STATUS_OK);
    for (unsigned int index = 0U; index < 50U; ++index) {
        TEST_ASSERT(foc_current_step(&original, &input, &first) == FOC_STATUS_OK);
        TEST_ASSERT(
            foc_current_step_with_feedforward(&optional, &input, &disabled, 123.0f, &second) ==
            FOC_STATUS_OK
        );
        TEST_ASSERT(first.output_enable == second.output_enable);
        TEST_ASSERT(first.svpwm.duty_a == second.svpwm.duty_a);
        TEST_ASSERT(first.svpwm.duty_b == second.svpwm.duty_b);
        TEST_ASSERT(first.svpwm.duty_c == second.svpwm.duty_c);
        TEST_ASSERT(original.integral_v.d == optional.integral_v.d);
        TEST_ASSERT(original.integral_v.q == optional.integral_v.q);
    }
    return 0;
}

static int test_model_comparison(void)
{
    const struct foc_current_feedforward_model exact = motor_model();
    const struct model_metrics base_forward = run_model(false, 100.0f, exact);
    const struct model_metrics ff_forward = run_model(true, 100.0f, exact);
    const struct model_metrics base_reverse = run_model(false, -100.0f, exact);
    const struct model_metrics ff_reverse = run_model(true, -100.0f, exact);
    struct foc_current_feedforward_model mismatch = exact;
    mismatch.ld_h *= 1.2f;
    mismatch.lq_h *= 0.8f;
    mismatch.psi_f_wb *= 0.8f;
    const struct model_metrics ff_mismatch = run_model(true, 100.0f, mismatch);

    TEST_ASSERT(model_is_valid("forward-baseline", base_forward));
    TEST_ASSERT(model_is_valid("forward-feedforward", ff_forward));
    TEST_ASSERT(model_is_valid("reverse-baseline", base_reverse));
    TEST_ASSERT(model_is_valid("reverse-feedforward", ff_reverse));
    TEST_ASSERT(model_is_valid("model-mismatch", ff_mismatch));
    TEST_ASSERT(ff_forward.rmse_q < base_forward.rmse_q);
    TEST_ASSERT(ff_reverse.rmse_q < base_reverse.rmse_q);
    TEST_ASSERT(ff_forward.saturated_steps <= base_forward.saturated_steps + 500U);
    printf(
        "pmsm omega=+100 base_rmse=(%.7g,%.7g) ff_rmse=(%.7g,%.7g) "
        "sat=(%u,%u) max=(%.7g,%.7g)\n",
        base_forward.rmse_d,
        base_forward.rmse_q,
        ff_forward.rmse_d,
        ff_forward.rmse_q,
        base_forward.saturated_steps,
        ff_forward.saturated_steps,
        base_forward.max_error,
        ff_forward.max_error
    );
    printf(
        "pmsm omega=-100 base_rmse=(%.7g,%.7g) ff_rmse=(%.7g,%.7g) "
        "sat=(%u,%u) max=(%.7g,%.7g)\n",
        base_reverse.rmse_d,
        base_reverse.rmse_q,
        ff_reverse.rmse_d,
        ff_reverse.rmse_q,
        base_reverse.saturated_steps,
        ff_reverse.saturated_steps,
        base_reverse.max_error,
        ff_reverse.max_error
    );
    printf(
        "pmsm mismatch omega=+100 rmse=(%.7g,%.7g) sat=%u max=%.7g\n",
        ff_mismatch.rmse_d,
        ff_mismatch.rmse_q,
        ff_mismatch.saturated_steps,
        ff_mismatch.max_error
    );
    return 0;
}

static int test_feedforward_saturation_transaction(void)
{
    struct foc_current_config config = controller_config();
    struct foc_current_context controller = {0};
    struct foc_current_feedforward_context feedforward = {0};
    struct foc_current_output output;
    struct foc_current_input input = {
        .target_a = {0.0f, 3.0f},
        .vbus_v = 24.0f,
        .dt_s = 0.0001f,
    };
    const struct foc_current_feedforward_model model = motor_model();
    unsigned int saturated = 0U;

    config.pwm.voltage_limit_v = 4.0f;
    TEST_ASSERT(foc_current_init(&controller, &config) == FOC_STATUS_OK);
    TEST_ASSERT(foc_current_feedforward_init(&feedforward, &model, true) == FOC_STATUS_OK);
    for (unsigned int index = 0U; index < 500U; ++index) {
        TEST_ASSERT(
            foc_current_step_with_feedforward(&controller, &input, &feedforward, 500.0f, &output) ==
            FOC_STATUS_OK
        );
        saturated += output.svpwm.saturated != 0U;
        TEST_ASSERT(isfinite(controller.integral_v.d) && isfinite(controller.integral_v.q));
    }
    TEST_ASSERT(saturated > 0U);
    TEST_ASSERT(fabsf(controller.integral_v.q) < 10.0f);
    input.target_a = (struct foc_vector_dq){0.0f, 0.0f};
    for (unsigned int index = 0U; index < 500U; ++index) {
        TEST_ASSERT(
            foc_current_step_with_feedforward(&controller, &input, &feedforward, 0.0f, &output) ==
            FOC_STATUS_OK
        );
        TEST_ASSERT(isfinite(controller.integral_v.d) && isfinite(controller.integral_v.q));
    }
    TEST_ASSERT(fabsf(controller.integral_v.q) < 1.0f);
    printf(
        "feedforward saturation steps=%u final_integral_q=%.7g\n",
        saturated,
        controller.integral_v.q
    );
    return 0;
}

static int test_invalid_speed_is_transactional(void)
{
    const struct foc_current_config config = controller_config();
    const struct foc_current_feedforward_model model = motor_model();
    struct foc_current_context controller = {0};
    struct foc_current_feedforward_context feedforward = {0};
    struct foc_current_output output = {.output_enable = 1U};
    struct foc_current_input input = {
        .phase_current_a = {0.1f, -0.05f, -0.05f},
        .theta_elec_rad = 0.41f,
        .target_a = {0.0f, 1.5f},
        .vbus_v = 24.0f,
        .dt_s = 0.0001f,
    };
    struct foc_current_context before;

    TEST_ASSERT(foc_current_init(&controller, &config) == FOC_STATUS_OK);
    TEST_ASSERT(foc_current_feedforward_init(&feedforward, &model, true) == FOC_STATUS_OK);
    TEST_ASSERT(
        foc_current_step_with_feedforward(&controller, &input, &feedforward, 120.0f, &output) ==
        FOC_STATUS_OK
    );
    before = controller;
    output.output_enable = 1U;
    output.svpwm.duty_a = 0.5f;
    TEST_ASSERT(
        foc_current_step_with_feedforward(&controller, &input, &feedforward, NAN, &output) ==
        FOC_STATUS_ERR_NONFINITE
    );
    TEST_ASSERT(memcmp(&controller, &before, sizeof(controller)) == 0);
    TEST_ASSERT(output.output_enable == 0U);
    TEST_ASSERT(output.svpwm.duty_a == 0.0f);
    TEST_ASSERT(output.svpwm.duty_b == 0.0f);
    TEST_ASSERT(output.svpwm.duty_c == 0.0f);
    return 0;
}

static int benchmark(void)
{
    const struct foc_current_config config = controller_config();
    const struct foc_current_feedforward_model model = motor_model();
    struct foc_current_feedforward_context disabled = {0};
    struct foc_current_feedforward_context enabled = {0};
    struct foc_current_context off_context = {0};
    struct foc_current_context on_context = {0};
    struct foc_current_input input = {
        .phase_current_a = {0.1f, -0.05f, -0.05f},
        .theta_elec_rad = 0.41f,
        .target_a = {0.0f, 1.5f},
        .vbus_v = 24.0f,
        .dt_s = 0.0001f,
    };
    struct foc_current_output output;
    volatile float sink = 0.0f;
    const unsigned int iterations = 200000U;
    clock_t begin;
    clock_t end;

    TEST_ASSERT(foc_current_init(&off_context, &config) == FOC_STATUS_OK);
    TEST_ASSERT(foc_current_init(&on_context, &config) == FOC_STATUS_OK);
    TEST_ASSERT(foc_current_feedforward_init(&disabled, &model, false) == FOC_STATUS_OK);
    TEST_ASSERT(foc_current_feedforward_init(&enabled, &model, true) == FOC_STATUS_OK);
    begin = clock();
    for (unsigned int index = 0U; index < iterations; ++index) {
        TEST_ASSERT(
            foc_current_step_with_feedforward(&off_context, &input, &disabled, 120.0f, &output) ==
            FOC_STATUS_OK
        );
        sink += output.voltage_dq_v.q;
    }
    end = clock();
    const double off_us =
        1.0e6 * (double)(end - begin) / (double)CLOCKS_PER_SEC / (double)iterations;
    begin = clock();
    for (unsigned int index = 0U; index < iterations; ++index) {
        TEST_ASSERT(
            foc_current_step_with_feedforward(&on_context, &input, &enabled, 120.0f, &output) ==
            FOC_STATUS_OK
        );
        sink += output.voltage_dq_v.q;
    }
    end = clock();
    const double on_us =
        1.0e6 * (double)(end - begin) / (double)CLOCKS_PER_SEC / (double)iterations;
    printf(
        "feedforward benchmark iterations=%u off_us=%.6g on_us=%.6g sink=%.7g\n",
        iterations,
        off_us,
        on_us,
        sink
    );
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--bench") == 0) {
        return benchmark();
    }
    return test_equations_and_validation() || test_disabled_equivalence_and_state() ||
        test_model_comparison() || test_feedforward_saturation_transaction() ||
        test_invalid_speed_is_transactional();
}

/* ---------------------------------------- end of file ----------------------------------------- */
