/**
 * @file    test_foc_impedance.c
 * @brief   Portable impedance SI, limiting, and failure contract tests.
 * @version v0.1.0
 * @date    2026.11.18
 *
 * Test role
 * ---------
 * Check signed motor-shaft requests against hand-derived cases, and poison
 * output storage to verify that invalid operations publish only zeros.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_impedance.h"

#include <float.h>
#include <math.h>
#include <stdio.h>

/* Private Macros --------------------------------------------------------------- */

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return 1;                                                       \
        }                                                                   \
    } while (0)

/* Private Helpers -------------------------------------------------------------- */

static int test_request(void)
{
    static const struct {
        struct foc_impedance_config config;
        struct foc_impedance_input input;
        double torque;
        double limited_torque;
        double iq;
        bool limited;
    } cases[] = {
        {{2.0f, 0.0f, 0.5f, 9.0f}, {3.0f, 0, 0, 0, 18.0f}, 6, 6, 12, false},
        {{2.0f, 0.0f, 0.5f, 9.0f}, {-3.0f, 0, 0, 0, 18.0f}, -6, -6, -12, false},
        {{0.0f, 0.25f, 0.5f, 3.0f}, {0, 3.0f, 1.0f, 0, 6.0f}, 0.5, 0.5, 1, false},
        {{0.0f, 0.25f, 0.5f, 3.0f}, {0, -2.0f, 2.0f, 0, 6.0f}, -1, -1, -2, false},
        {{0.0f, 0.0f, 0.5f, 3.0f}, {0, 0, 0, -0.75f, 6.0f}, -0.75, -0.75, -1.5, false},
        {{2.0f, 0.25f, 0.5f, 3.0f}, {0.75f, 1.0f, 3.0f, -0.25f, 6.0f}, 0.75, 0.75, 1.5, false},
        {{2.0f, 0.25f, 0.5f, 3.0f}, {0, 3.0f, 3.0f, 0, 6.0f}, 0, 0, 0, false},
        {{2.0f, 0.0f, 0.5f, 3.0f}, {3.0f, 0, 0, 0, 10.0f}, 6, 3, 6, true},
        {{2.0f, 0.0f, 0.5f, 3.0f}, {-3.0f, 0, 0, 0, 2.0f}, -6, -1, -2, true},
        {{2.0f, 0.0f, 0.5f, 3.0f}, {3.0f, 0, 0, 0, 6.0f}, 6, 3, 6, true},
        {{2.0f, 0.0f, 0.5f, 3.0f}, {1.5f, 0, 0, 0, 6.0f}, 3, 3, 6, false},
        {{2.0f, 0.0f, 0.5f, 3.0f}, {3.0f, 0, 0, 0, 0.0f}, 6, 0, 0, true},
        {{2.0f, 0.0f, 0.5f, 0.0f}, {-3.0f, 0, 0, 0, 6.0f}, -6, 0, 0, true},
        {{0.25f, 0.0f, 0.5f, 9.0f}, {16.0f, 0, 0, 0, 18.0f}, 4, 4, 8, false},
        {{1.0e20f, 0.0f, 0.5f, 3.0f}, {1.0e10f, 0, 0, 0, 2.0f}, 1.0e30, 1, 2, true},
    };
    for (unsigned i = 0U; i < sizeof cases / sizeof cases[0]; ++i) {
        struct foc_impedance_config config = cases[i].config;
        struct foc_impedance_input input = cases[i].input;
        struct foc_impedance_output out;
        CHECK(foc_impedance_step(&config, &input, &out) == FOC_STATUS_OK);
        CHECK(out.valid && out.limited == cases[i].limited);
        CHECK(
            fabs(out.requested_torque_nm - cases[i].torque) <=
            1.0e-6 * fmax(1.0, fabs(cases[i].torque))
        );
        CHECK(fabs(out.limited_torque_nm - cases[i].limited_torque) <= 1.0e-6);
        CHECK(fabs(out.iq_ref_a - cases[i].iq) <= 1.0e-6);
        CHECK(fabsf(out.iq_ref_a) <= input.live_iq_limit_a);
        CHECK(
            config.stiffness_nm_per_rad == cases[i].config.stiffness_nm_per_rad &&
            config.damping_nm_s_per_rad == cases[i].config.damping_nm_s_per_rad &&
            config.torque_constant_nm_per_a == cases[i].config.torque_constant_nm_per_a &&
            config.torque_limit_nm == cases[i].config.torque_limit_nm
        );
        CHECK(
            input.position_error_rad == cases[i].input.position_error_rad &&
            input.reference_velocity_rad_s == cases[i].input.reference_velocity_rad_s &&
            input.measured_velocity_rad_s == cases[i].input.measured_velocity_rad_s &&
            input.torque_feedforward_nm == cases[i].input.torque_feedforward_nm &&
            input.live_iq_limit_a == cases[i].input.live_iq_limit_a
        );
        struct foc_impedance_output repeat;
        CHECK(foc_impedance_step(&cases[0].config, &cases[0].input, &repeat) == FOC_STATUS_OK);
        CHECK(repeat.valid && repeat.iq_ref_a == 12.0f);
        CHECK(foc_impedance_step(&config, &input, &repeat) == FOC_STATUS_OK);
        CHECK(
            repeat.valid == out.valid && repeat.limited == out.limited &&
            repeat.requested_torque_nm == out.requested_torque_nm &&
            repeat.limited_torque_nm == out.limited_torque_nm && repeat.iq_ref_a == out.iq_ref_a
        );
    }
    return 0;
}

static int test_conversion_rounding(void)
{
    const struct foc_impedance_config config = {0, 0, 0.112f, 2.0f};
    for (int direction = -1; direction <= 1; direction += 2) {
        const struct foc_impedance_input input = {0, 0, 0, (float)direction, 1.3f};
        struct foc_impedance_output out;
        CHECK(foc_impedance_step(&config, &input, &out) == FOC_STATUS_OK);
        CHECK(out.valid && out.limited && out.iq_ref_a == direction * input.live_iq_limit_a);
    }
    return 0;
}

static int check_rejected(
    const struct foc_impedance_config *config,
    const struct foc_impedance_input *input,
    int status
)
{
    struct foc_impedance_output out = {1.0f, 2.0f, 3.0f, true, true};
    CHECK(foc_impedance_step(config, input, &out) == status);
    CHECK(
        !out.valid && !out.limited && out.iq_ref_a == 0.0f && out.requested_torque_nm == 0.0f &&
        out.limited_torque_nm == 0.0f
    );
    return 0;
}

static int test_invalid_inputs(void)
{
    struct foc_impedance_config config = {2.0f, 0.5f, 0.5f, 2.0f};
    struct foc_impedance_input input = {0.5f, 2.0f, 1.0f, 1.0f, 3.0f};
    CHECK(foc_impedance_step(&config, &input, NULL) == FOC_STATUS_ERR_NULL);
    CHECK(check_rejected(NULL, &input, FOC_STATUS_ERR_NULL) == 0);
    CHECK(check_rejected(&config, NULL, FOC_STATUS_ERR_NULL) == 0);

    float *fields[] = {
        &config.stiffness_nm_per_rad,
        &config.damping_nm_s_per_rad,
        &config.torque_constant_nm_per_a,
        &config.torque_limit_nm,
        &input.live_iq_limit_a,
        &input.position_error_rad,
        &input.reference_velocity_rad_s,
        &input.measured_velocity_rad_s,
        &input.torque_feedforward_nm,
    };
    for (unsigned i = 0U; i < sizeof fields / sizeof fields[0]; ++i) {
        const float valid = *fields[i];
        *fields[i] = NAN;
        CHECK(check_rejected(&config, &input, FOC_STATUS_ERR_NONFINITE) == 0);
        *fields[i] = i % 2U == 0U ? INFINITY : -INFINITY;
        CHECK(check_rejected(&config, &input, FOC_STATUS_ERR_NONFINITE) == 0);
        if (i < 5U) {
            *fields[i] = -1.0f;
            CHECK(check_rejected(&config, &input, FOC_STATUS_ERR_INVALID_CONFIG) == 0);
        }
        *fields[i] = valid;
    }
    config.torque_constant_nm_per_a = 0.0f;
    CHECK(check_rejected(&config, &input, FOC_STATUS_ERR_INVALID_CONFIG) == 0);
    return 0;
}

static int test_overflow(void)
{
    static const struct {
        struct foc_impedance_config config;
        struct foc_impedance_input input;
    } cases[] = {
        {{0, 0, 1.0f, 1.0f}, {0, FLT_MAX, -FLT_MAX, 0, 1.0f}},
        {{2.0f, 0, 1.0f, 1.0f}, {FLT_MAX, 0, 0, 0, 1.0f}},
        {{0, 2.0f, 1.0f, 1.0f}, {0, -FLT_MAX, 0, 0, 1.0f}},
        {{1.0f, 1.0f, 1.0f, 1.0f}, {FLT_MAX, FLT_MAX, 0, -FLT_MAX, 1.0f}},
        {{1.0f, 0, 1.0f, 1.0f}, {FLT_MAX, 0, 0, FLT_MAX, 1.0f}},
        {{0, 0, FLT_MAX, 1.0f}, {0, 0, 0, 0, 2.0f}},
        {{0, 0, 0.75f, FLT_MAX}, {0, 0, 0, FLT_MAX, FLT_MAX}},
    };
    for (unsigned i = 0U; i < sizeof cases / sizeof cases[0] - 1U; ++i) {
        CHECK(check_rejected(&cases[i].config, &cases[i].input, FOC_STATUS_ERR_NONFINITE) == 0);
    }
    const unsigned edge = sizeof cases / sizeof cases[0] - 1U;
    struct foc_impedance_output out;
    CHECK(foc_impedance_step(&cases[edge].config, &cases[edge].input, &out) == FOC_STATUS_OK);
    CHECK(out.valid && out.limited && isfinite(out.iq_ref_a));
    return 0;
}

/* Test Entry ------------------------------------------------------------------- */

int main(void)
{
    return test_request() || test_conversion_rounding() || test_invalid_inputs() || test_overflow();
}

/* ---------------------------------------- end of file ----------------------------------------- */
