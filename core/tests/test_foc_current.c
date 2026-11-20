/**
 * @file    test_foc_current.c
 * @brief   Verify the d/q current controller and its fixed-rotor feedback loop.
 * @version v0.1.0
 * @date    2026.11.10
 *
 * Test role
 * ---------
 * Check numeric contracts and drive an independent RL model from the
 * controller's final duties. The model is confined to this test executable.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_current.h"
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define TEST_ASSERT(condition)                                                                  \
    do {                                                                                        \
        if (!(condition)) {                                                                     \
            fprintf(stderr, "assertion failed: %s at %s:%d\n", #condition, __FILE__, __LINE__); \
            return 1;                                                                           \
        }                                                                                       \
    } while (0)

/* Private Helpers -------------------------------------------------------------- */

static int near(float actual, float expected)
{
    return fabsf(actual - expected) <= 2.0e-5f;
}

static struct foc_current_config test_config(void)
{
    return (struct foc_current_config){
        .pwm =
            {.vbus_min_v = 1.0f,
             .voltage_limit_v = 10.0f,
             .duty_min = 0.0f,
             .duty_max = 1.0f,
             .max_dt_s = 0.01f},
        .kp_v_per_a = {2.0f, 2.0f},
        .ki_v_per_a_s = {0.0f, 0.0f},
        .current_limit_a = 50.0f,
    };
}

static int test_proportional_and_coordinates(void)
{
    const struct foc_current_config config = test_config();
    const float half_pi = 1.57079632679489661923f;
    const float two_pi = 6.28318530717958647692f;
    const struct foc_vector_dq targets[] = {
        {1.0f, 0.0f},
        {-1.0f, 0.0f},
        {0.0f, 1.0f},
        {0.0f, -1.0f},
        {1.0f, -1.0f},
    };
    struct foc_current_context context = {0};
    struct foc_current_input input = {.vbus_v = 12.0f, .dt_s = 0.001f};
    struct foc_current_output output;

    TEST_ASSERT(foc_current_init(&context, &config) == FOC_STATUS_OK);
    for (unsigned int index = 0U; index < sizeof(targets) / sizeof(targets[0]); ++index) {
        input.target_a = targets[index];
        TEST_ASSERT(foc_current_step(&context, &input, &output) == FOC_STATUS_OK);
        TEST_ASSERT(output.output_enable == 1U && output.svpwm.saturated == 0U);
        TEST_ASSERT(near(output.voltage_dq_v.d, 2.0f * input.target_a.d));
        TEST_ASSERT(near(output.voltage_dq_v.q, 2.0f * input.target_a.q));
        TEST_ASSERT(context.integral_v.d == 0.0f && context.integral_v.q == 0.0f);
    }

    input.phase_current_a = (struct foc_vector_abc){1.0f, -0.5f, -0.5f};
    input.target_a = (struct foc_vector_dq){0.0f, 0.0f};
    input.theta_elec_rad = half_pi + two_pi;
    TEST_ASSERT(foc_current_step(&context, &input, &output) == FOC_STATUS_OK);
    TEST_ASSERT(near(output.measured_dq_a.d, 0.0f) && near(output.measured_dq_a.q, -1.0f));
    TEST_ASSERT(near(output.voltage_dq_v.q, 2.0f));
    input.theta_elec_rad = -half_pi;
    TEST_ASSERT(foc_current_step(&context, &input, &output) == FOC_STATUS_OK);
    TEST_ASSERT(near(output.measured_dq_a.q, 1.0f) && near(output.voltage_dq_v.q, -2.0f));
    input.target_a.q = 1.0f;
    TEST_ASSERT(foc_current_step(&context, &input, &output) == FOC_STATUS_OK);
    TEST_ASSERT(near(output.voltage_dq_v.q, 0.0f) && near(output.svpwm.duty_a, 0.5f));
    return 0;
}

static int test_integral_and_instances(void)
{
    const float periods[] = {0.00005f, 0.0001f, 0.0002f, 0.001f};
    const unsigned int steps[] = {20U, 10U, 5U, 1U};
    struct foc_current_config config = test_config();
    struct foc_current_context first = {0};
    struct foc_current_context second = {0};
    struct foc_current_input input = {.target_a = {1.0f, -2.0f}, .vbus_v = 12.0f};
    struct foc_current_output output;

    config.kp_v_per_a = (struct foc_vector_dq){0.0f, 0.0f};
    config.ki_v_per_a_s = (struct foc_vector_dq){100.0f, 200.0f};
    TEST_ASSERT(foc_current_init(&first, &config) == FOC_STATUS_OK);
    config.kp_v_per_a.d = 3.0f;
    TEST_ASSERT(foc_current_init(&second, &config) == FOC_STATUS_OK);

    for (unsigned int period = 0U; period < sizeof(periods) / sizeof(periods[0]); ++period) {
        input.dt_s = periods[period];
        TEST_ASSERT(foc_current_reset(&first) == FOC_STATUS_OK);
        TEST_ASSERT(first.integral_v.d == 0.0f && first.integral_v.q == 0.0f);
        for (unsigned int step = 0U; step < steps[period]; ++step) {
            TEST_ASSERT(foc_current_step(&first, &input, &output) == FOC_STATUS_OK);
        }
        TEST_ASSERT(near(first.integral_v.d, 0.1f) && near(first.integral_v.q, -0.4f));
        TEST_ASSERT(near(output.voltage_dq_v.d, 0.1f) && near(output.voltage_dq_v.q, -0.4f));
    }
    TEST_ASSERT(second.integral_v.d == 0.0f && second.integral_v.q == 0.0f);
    TEST_ASSERT(foc_current_step(&second, &input, &output) == FOC_STATUS_OK);
    TEST_ASSERT(near(output.voltage_dq_v.d, 3.1f));
    TEST_ASSERT(near(first.integral_v.d, 0.1f));
    return 0;
}

static int test_voltage_and_anti_windup(void)
{
    struct foc_current_config config = test_config();
    struct foc_current_context context = {0};
    struct foc_current_input input = {.target_a = {3.0f, 4.0f}, .vbus_v = 12.0f, .dt_s = 0.001f};
    struct foc_current_output output;

    config.pwm.voltage_limit_v = 5.0f;
    TEST_ASSERT(foc_current_init(&context, &config) == FOC_STATUS_OK);
    TEST_ASSERT(foc_current_step(&context, &input, &output) == FOC_STATUS_OK);
    TEST_ASSERT(output.svpwm.saturated != 0U);
    TEST_ASSERT(near(output.voltage_dq_v.d, 3.0f) && near(output.voltage_dq_v.q, 4.0f));

    config.pwm.voltage_limit_v = 20.0f;
    TEST_ASSERT(foc_current_init(&context, &config) == FOC_STATUS_OK);
    TEST_ASSERT(foc_current_step(&context, &input, &output) == FOC_STATUS_OK);
    TEST_ASSERT(near(hypotf(output.voltage_dq_v.d, output.voltage_dq_v.q), 12.0f / sqrtf(3.0f)));
    config.current_limit_a = 2.5f;
    TEST_ASSERT(foc_current_init(&context, &config) == FOC_STATUS_OK);
    TEST_ASSERT(foc_current_step(&context, &input, &output) == FOC_STATUS_OK);
    TEST_ASSERT(output.target_limited != 0U && output.svpwm.saturated == 0U);
    TEST_ASSERT(near(output.voltage_dq_v.d, 3.0f) && near(output.voltage_dq_v.q, 4.0f));

    /* Restrictive duty limits saturate before the shared voltage envelope. */
    config.current_limit_a = 50.0f;
    config.pwm.duty_min = 0.4f;
    config.pwm.duty_max = 0.6f;
    config.kp_v_per_a = (struct foc_vector_dq){0.0f, 0.0f};
    config.ki_v_per_a_s = (struct foc_vector_dq){1000.0f, 1000.0f};
    input.target_a = (struct foc_vector_dq){2.0f, 0.0f};
    TEST_ASSERT(foc_current_init(&context, &config) == FOC_STATUS_OK);
    for (unsigned int step = 0U; step < 3000U; ++step) {
        TEST_ASSERT(foc_current_step(&context, &input, &output) == FOC_STATUS_OK);
    }
    TEST_ASSERT(output.svpwm.saturated != 0U);
    TEST_ASSERT(near(context.integral_v.d, 1.6f) && near(output.voltage_dq_v.d, 1.6f));
    TEST_ASSERT(near(output.svpwm.duty_a, 0.6f) && near(output.svpwm.duty_b, 0.4f));
    input.target_a.d = -0.5f;
    TEST_ASSERT(foc_current_step(&context, &input, &output) == FOC_STATUS_OK);
    TEST_ASSERT(near(context.integral_v.d, 1.1f));
    input.target_a.d = -3.0f;
    TEST_ASSERT(foc_current_step(&context, &input, &output) == FOC_STATUS_OK);
    TEST_ASSERT(near(context.integral_v.d, -1.6f) && near(output.voltage_dq_v.d, -1.6f));
    return 0;
}

static int output_disabled(const struct foc_current_output *output)
{
    return output->output_enable == 0U && output->target_limited == 0U &&
        output->measured_dq_a.d == 0.0f && output->measured_dq_a.q == 0.0f &&
        output->voltage_dq_v.d == 0.0f && output->voltage_dq_v.q == 0.0f &&
        output->svpwm.duty_a == 0.0f && output->svpwm.duty_b == 0.0f &&
        output->svpwm.duty_c == 0.0f && output->svpwm.sector == 0U && output->svpwm.saturated == 0U;
}

static int test_rejected_steps(void)
{
    const struct foc_current_input valid = {
        .target_a = {1.0f, -1.0f},
        .vbus_v = 12.0f,
        .dt_s = 0.001f,
    };
    const struct {
        struct foc_current_input input;
        int status;
    } cases[] = {
        {{.phase_current_a = {NAN, 0.0f, 0.0f}, .vbus_v = 12.0f, .dt_s = 0.001f},
         FOC_STATUS_ERR_NONFINITE},
        {{.phase_current_a = {0.0f, FLT_MAX, -FLT_MAX}, .vbus_v = 12.0f, .dt_s = 0.001f},
         FOC_STATUS_ERR_NONFINITE},
        {{.theta_elec_rad = INFINITY, .vbus_v = 12.0f, .dt_s = 0.001f}, FOC_STATUS_ERR_NONFINITE},
        {{.target_a = {0.0f, INFINITY}, .vbus_v = 12.0f, .dt_s = 0.001f}, FOC_STATUS_ERR_NONFINITE},
        {{.vbus_v = 0.0f, .dt_s = 0.001f}, FOC_STATUS_ERR_INVALID_VBUS},
        {{.vbus_v = 0.5f, .dt_s = 0.001f}, FOC_STATUS_ERR_INVALID_VBUS},
        {{.vbus_v = NAN, .dt_s = 0.001f}, FOC_STATUS_ERR_INVALID_VBUS},
        {{.vbus_v = 12.0f, .dt_s = 0.0f}, FOC_STATUS_ERR_INVALID_DT},
        {{.vbus_v = 12.0f, .dt_s = -0.001f}, FOC_STATUS_ERR_INVALID_DT},
        {{.vbus_v = 12.0f, .dt_s = 0.1f}, FOC_STATUS_ERR_INVALID_DT},
        {{.vbus_v = 12.0f, .dt_s = NAN}, FOC_STATUS_ERR_INVALID_DT},
        {{.vbus_v = 12.0f, .dt_s = INFINITY}, FOC_STATUS_ERR_INVALID_DT},
    };
    struct foc_current_config config = test_config();
    struct foc_current_context context = {0};
    struct foc_current_context replay;
    struct foc_current_output output;
    struct foc_current_output replay_output;

    memset(&output, 0xa5, sizeof(output));
    TEST_ASSERT(foc_current_step(&context, &valid, &output) == FOC_STATUS_ERR_NOT_INITIALIZED);
    TEST_ASSERT(output_disabled(&output));
    TEST_ASSERT(foc_current_reset(&context) == FOC_STATUS_ERR_NOT_INITIALIZED);
    TEST_ASSERT(foc_current_reset(NULL) == FOC_STATUS_ERR_NULL);
    TEST_ASSERT(foc_current_init(NULL, &config) == FOC_STATUS_ERR_NULL);
    TEST_ASSERT(foc_current_init(&context, NULL) == FOC_STATUS_ERR_NULL);
    config.pwm.duty_min = 0.5f;
    TEST_ASSERT(foc_current_init(&context, &config) == FOC_STATUS_ERR_INVALID_CONFIG);
    TEST_ASSERT(context.initialized == 0U);
    config.pwm.duty_min = 0.0f;
    config.ki_v_per_a_s = (struct foc_vector_dq){100.0f, 100.0f};
    TEST_ASSERT(foc_current_init(&context, &config) == FOC_STATUS_OK);
    TEST_ASSERT(foc_current_step(&context, &valid, &output) == FOC_STATUS_OK);
    replay = context;
    config.kp_v_per_a.d = -1.0f;
    TEST_ASSERT(foc_current_init(&context, &config) == FOC_STATUS_ERR_INVALID_CONFIG);
    config.kp_v_per_a.d = NAN;
    TEST_ASSERT(foc_current_init(&context, &config) == FOC_STATUS_ERR_NONFINITE);
    config = replay.config;
    config.ki_v_per_a_s.q = -1.0f;
    TEST_ASSERT(foc_current_init(&context, &config) == FOC_STATUS_ERR_INVALID_CONFIG);
    config = replay.config;
    config.pwm.voltage_limit_v = 0.0f;
    TEST_ASSERT(foc_current_init(&context, &config) == FOC_STATUS_ERR_INVALID_CONFIG);
    TEST_ASSERT(context.config.pwm.voltage_limit_v == replay.config.pwm.voltage_limit_v);
    TEST_ASSERT(context.config.kp_v_per_a.d == replay.config.kp_v_per_a.d);

    TEST_ASSERT(foc_current_step(NULL, &valid, &output) == FOC_STATUS_ERR_NULL);
    TEST_ASSERT(output_disabled(&output));
    TEST_ASSERT(foc_current_step(&context, NULL, &output) == FOC_STATUS_ERR_NULL);
    TEST_ASSERT(output_disabled(&output));
    TEST_ASSERT(foc_current_step(&context, &valid, NULL) == FOC_STATUS_ERR_NULL);
    for (unsigned int index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        struct foc_current_input input;

        memcpy(&input, &cases[index].input, sizeof(input));
        memset(&output, 0xa5, sizeof(output));
        TEST_ASSERT(foc_current_step(&context, &input, &output) == cases[index].status);
        TEST_ASSERT(output_disabled(&output));
        TEST_ASSERT(context.integral_v.d == replay.integral_v.d);
        TEST_ASSERT(context.integral_v.q == replay.integral_v.q);
        TEST_ASSERT(memcmp(&input, &cases[index].input, sizeof(input)) == 0);
    }
    TEST_ASSERT(foc_current_step(&context, &valid, &output) == FOC_STATUS_OK);
    TEST_ASSERT(foc_current_step(&replay, &valid, &replay_output) == FOC_STATUS_OK);
    TEST_ASSERT(output.voltage_dq_v.d == replay_output.voltage_dq_v.d);
    TEST_ASSERT(output.voltage_dq_v.q == replay_output.voltage_dq_v.q);
    TEST_ASSERT(output.svpwm.duty_a == replay_output.svpwm.duty_a);
    TEST_ASSERT(output.svpwm.duty_b == replay_output.svpwm.duty_b);
    TEST_ASSERT(output.svpwm.duty_c == replay_output.svpwm.duty_c);

    config = test_config();
    config.pwm.voltage_limit_v = 5.0f;
    config.ki_v_per_a_s.d = 100.0f;
    TEST_ASSERT(foc_current_init(&context, &config) == FOC_STATUS_OK);
    TEST_ASSERT(foc_current_step(&context, &valid, &output) == FOC_STATUS_OK);
    replay = context;
    {
        /* Duty resolution at this bus can round a 5 V request up to about 6 V. */
        struct foc_current_input coarse_duty = valid;
        struct foc_current_input original;

        coarse_duty.target_a = (struct foc_vector_dq){3.0f, 0.0f};
        coarse_duty.vbus_v = 1.0e8f;
        memcpy(&original, &coarse_duty, sizeof(original));
        TEST_ASSERT(
            foc_current_step(&context, &coarse_duty, &output) == FOC_STATUS_ERR_VOLTAGE_LIMIT
        );
        TEST_ASSERT(output_disabled(&output));
        TEST_ASSERT(context.integral_v.d == replay.integral_v.d);
        TEST_ASSERT(context.integral_v.q == replay.integral_v.q);
        TEST_ASSERT(memcmp(&original, &coarse_duty, sizeof(original)) == 0);
    }

    config = test_config();
    config.kp_v_per_a.q = 0.0f;
    config.ki_v_per_a_s.q = FLT_MAX;
    TEST_ASSERT(foc_current_init(&context, &config) == FOC_STATUS_OK);
    {
        struct foc_current_input overflow = valid;
        struct foc_current_input original;

        overflow.phase_current_a.b = -1.0e30f;
        overflow.phase_current_a.c = 1.0e30f;
        memcpy(&original, &overflow, sizeof(original));
        TEST_ASSERT(foc_current_step(&context, &overflow, &output) == FOC_STATUS_ERR_NONFINITE);
        TEST_ASSERT(output_disabled(&output));
        TEST_ASSERT(context.integral_v.d == 0.0f && context.integral_v.q == 0.0f);
        TEST_ASSERT(memcmp(&original, &overflow, sizeof(original)) == 0);
    }
    return 0;
}

static int test_low_voltage_duty_roundoff(void)
{
    struct foc_current_config config = test_config();
    struct foc_current_context context = {0};
    const struct foc_current_input input = {
        .phase_current_a = {-0.0240000002f, 0.0659999996f, -0.0420000032f},
        .theta_elec_rad = 0.37f,
        .target_a = {0.0f, 0.4f},
        .vbus_v = 12.0f,
        .dt_s = 0.0002f,
    };
    struct foc_current_output output;

    config.pwm.voltage_limit_v = 0.12f;
    config.kp_v_per_a = (struct foc_vector_dq){1.08f, 1.35f};
    config.ki_v_per_a_s = (struct foc_vector_dq){540.0f, 540.0f};
    TEST_ASSERT(foc_current_init(&context, &config) == FOC_STATUS_OK);
    TEST_ASSERT(foc_current_step(&context, &input, &output) == FOC_STATUS_OK);
    TEST_ASSERT(output.output_enable == 1U && output.svpwm.saturated == 1U);
    const double ab = (double)output.svpwm.duty_a - output.svpwm.duty_b;
    const double ac = (double)output.svpwm.duty_a - output.svpwm.duty_c;
    const double bc = (double)output.svpwm.duty_b - output.svpwm.duty_c;
    const double voltage = input.vbus_v * sqrt(2.0 * (ab * ab + ac * ac + bc * bc)) / 3.0;
    TEST_ASSERT(voltage <= config.pwm.voltage_limit_v && voltage > 0.1199);
    TEST_ASSERT(output.voltage_dq_v.q > 0.1199f && context.integral_v.q == 0.0f);
    return 0;
}

static int test_fixed_rotor_model(void)
{
    const double resistance = 0.5;
    const double inductance_d = 0.001;
    const double inductance_q = 0.0015;
    const double sqrt3 = 1.73205080756887729353;
    const double angle = 0.37;
    const double cosine = cos(angle);
    const double sine = sin(angle);
    const float periods[] = {0.00005f, 0.0001f, 0.0002f, 0.001f};
    const struct foc_vector_dq targets[] =
        {{0.0f, 1.0f}, {0.5f, -1.0f}, {5.0f, 8.0f}, {-0.5f, -0.75f}};
    struct foc_current_config config = test_config();

    config.pwm.voltage_limit_v = 2.0f;
    config.kp_v_per_a = (struct foc_vector_dq){1.0f, 1.5f};
    config.ki_v_per_a_s = (struct foc_vector_dq){500.0f, 500.0f};
    for (unsigned int period = 0U; period < sizeof(periods) / sizeof(periods[0]); ++period) {
        struct foc_current_context context = {0};
        struct foc_current_input input =
            {.theta_elec_rad = (float)angle, .vbus_v = 12.0f, .dt_s = periods[period]};
        const double dt = input.dt_s;
        const double decay_d = exp(-resistance * dt / inductance_d);
        const double decay_q = exp(-resistance * dt / inductance_q);
        const unsigned int steps = (unsigned int)lround(0.060 / dt);
        double id = 0.0;
        double iq = 0.0;

        TEST_ASSERT(foc_current_init(&context, &config) == FOC_STATUS_OK);
        for (unsigned int stage = 0U; stage < sizeof(targets) / sizeof(targets[0]); ++stage) {
            unsigned int limited = 0U;
            double final_window_error = 0.0;
            input.target_a = targets[stage];
            for (unsigned int step = 0U; step < steps; ++step) {
                const double alpha_a = cosine * id - sine * iq;
                const double beta_a = sine * id + cosine * iq;
                struct foc_current_output output;

                input.phase_current_a = (struct foc_vector_abc){
                    (float)alpha_a,
                    (float)(-0.5 * alpha_a + 0.5 * sqrt3 * beta_a),
                    (float)(-0.5 * alpha_a - 0.5 * sqrt3 * beta_a),
                };
                TEST_ASSERT(foc_current_step(&context, &input, &output) == FOC_STATUS_OK);
                TEST_ASSERT(output.output_enable == 1U);
                limited += output.svpwm.saturated != 0U;

                /* Subtract the floating neutral and rotate the ideal duty-derived voltage. */
                const double a = output.svpwm.duty_a;
                const double b = output.svpwm.duty_b;
                const double c = output.svpwm.duty_c;
                TEST_ASSERT(a >= 0.0 && a <= 1.0 && b >= 0.0 && b <= 1.0 && c >= 0.0 && c <= 1.0);
                const double alpha_v = (a - (a + b + c) / 3.0) * input.vbus_v;
                const double beta_v = (b - c) * input.vbus_v / sqrt3;
                const double vd = cosine * alpha_v + sine * beta_v;
                const double vq = cosine * beta_v - sine * alpha_v;
                TEST_ASSERT(hypot(vd, vq) <= 2.00002);
                TEST_ASSERT(fabs(vd - output.voltage_dq_v.d) < 2.0e-5);
                TEST_ASSERT(fabs(vq - output.voltage_dq_v.q) < 2.0e-5);
                id = decay_d * id + (1.0 - decay_d) * vd / resistance;
                iq = decay_q * iq + (1.0 - decay_q) * vq / resistance;
                if (step >= steps * 5U / 6U) {
                    const double error =
                        fmax(fabs(id - input.target_a.d), fabs(iq - input.target_a.q));

                    final_window_error = fmax(final_window_error, error);
                }
            }
            if (stage == 2U) {
                TEST_ASSERT(limited == steps);
                TEST_ASSERT(hypot(id, iq) <= 4.001);
            } else {
                TEST_ASSERT(final_window_error <= 0.01);
            }
            printf(
                "dt=%4.0f us stage=%u final=(%.6f, %.6f) A max_window_error=%.6f A limited=%u/%u\n",
                dt * 1.0e6,
                stage,
                id,
                iq,
                final_window_error,
                limited,
                steps
            );
        }
    }
    return 0;
}

/* Test Entry ------------------------------------------------------------------ */

int main(void)
{
    return test_proportional_and_coordinates() || test_integral_and_instances() ||
        test_voltage_and_anti_windup() || test_rejected_steps() ||
        test_low_voltage_duty_roundoff() || test_fixed_rotor_model();
}

/* ---------------------------------------- end of file ----------------------------------------- */
