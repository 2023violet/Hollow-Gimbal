/**
 * @file    test_foc_impedance_tuning.c
 * @brief   Model-based impedance design and failure atomicity tests.
 * @version v0.1.0
 * @date    2026.11.18
 *
 * Test role
 * ---------
 * Use analytic gains and physical edge cases to check cold-path design.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_impedance_tuning.h"

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

/* Private Constants ------------------------------------------------------------ */

static const struct foc_impedance_tuning_input nominal = {
    .model_id = 8U,
    .damping_source = FOC_IMPEDANCE_TUNING_DAMPING_PROVIDED,
    .inertia_kg_m2 = 0.002f,
    .viscous_damping_nm_s_per_rad = 0.006f,
    .target_natural_frequency_rad_s = 18.0f,
    .target_damping_ratio = 0.75f,
    .torque_constant_nm_per_a = 0.15f,
    .max_stiffness_nm_per_rad = 3.0f,
    .max_damping_nm_s_per_rad = 0.3f,
    .iq_limit_a = 2.2f,
    .sample_period_s = 0.0005f,
    .extra_delay_s = 0.0006f,
};

/* Private Helpers -------------------------------------------------------------- */

static int test_analytic(void)
{
    struct foc_impedance_tuning_candidate actual;
    CHECK(foc_impedance_tuning_design(&nominal, &actual) == FOC_IMPEDANCE_TUNING_OK);
    CHECK(
        actual.valid && actual.model_id == 8U &&
        actual.damping_source == FOC_IMPEDANCE_TUNING_DAMPING_PROVIDED
    );
    CHECK(fabsf(actual.stiffness_nm_per_rad - 0.648f) < 1e-6f);
    CHECK(fabsf(actual.damping_nm_s_per_rad - 0.048f) < 1e-7f);
    CHECK(fabsf(actual.ideal_natural_frequency_rad_s - 18.0f) < 2e-6f);
    CHECK(fabsf(actual.ideal_damping_ratio - 0.75f) < 1e-7f);
    CHECK(fabsf(actual.current_limited_torque_nm - 0.33f) < 1e-7f);

    struct foc_impedance_tuning_input second = nominal;
    second.damping_source = FOC_IMPEDANCE_TUNING_DAMPING_ASSUMED_ZERO;
    second.viscous_damping_nm_s_per_rad = 0.0f;
    second.model_id = 9U;
    struct foc_impedance_tuning_candidate other;
    CHECK(foc_impedance_tuning_design(&second, &other) == FOC_IMPEDANCE_TUNING_OK);
    CHECK(
        other.valid && other.model_id == 9U &&
        other.damping_source == FOC_IMPEDANCE_TUNING_DAMPING_ASSUMED_ZERO
    );
    CHECK(fabsf(other.damping_nm_s_per_rad - 0.054f) < 1e-7f);
    CHECK(foc_impedance_tuning_design(&nominal, &other) == FOC_IMPEDANCE_TUNING_OK);
    CHECK(
        other.damping_nm_s_per_rad == actual.damping_nm_s_per_rad &&
        other.stiffness_nm_per_rad == actual.stiffness_nm_per_rad &&
        other.model_id == actual.model_id
    );

    struct foc_impedance_tuning_input exact = nominal;
    exact.inertia_kg_m2 = 0.25f;
    exact.target_natural_frequency_rad_s = 2.0f;
    exact.target_damping_ratio = 0.5f;
    exact.viscous_damping_nm_s_per_rad = 0.5f;
    exact.max_stiffness_nm_per_rad = 1.0f;
    exact.max_damping_nm_s_per_rad = 0.0f;
    exact.sample_period_s = 0.125f;
    exact.extra_delay_s = 0.0f;
    CHECK(foc_impedance_tuning_design(&exact, &actual) == FOC_IMPEDANCE_TUNING_OK);
    CHECK(
        actual.valid && actual.stiffness_nm_per_rad == 1.0f && actual.damping_nm_s_per_rad == 0.0f
    );
    return 0;
}

static int rejected(const struct foc_impedance_tuning_input *input, int reason)
{
    struct foc_impedance_tuning_candidate result =
        {1U, FOC_IMPEDANCE_TUNING_DAMPING_ASSUMED_ZERO, 1, 2, 3, 4, 5, true};
    CHECK(foc_impedance_tuning_design(input, &result) == reason);
    CHECK(
        !result.valid && result.model_id == 0U &&
        result.damping_source == FOC_IMPEDANCE_TUNING_DAMPING_PROVIDED &&
        result.stiffness_nm_per_rad == 0.0f && result.damping_nm_s_per_rad == 0.0f &&
        result.ideal_natural_frequency_rad_s == 0.0f && result.ideal_damping_ratio == 0.0f &&
        result.current_limited_torque_nm == 0.0f
    );
    return 0;
}

static int test_rejection(void)
{
    CHECK(rejected(NULL, FOC_IMPEDANCE_TUNING_ERR_NULL) == 0);
    CHECK(foc_impedance_tuning_design(&nominal, NULL) == FOC_IMPEDANCE_TUNING_ERR_NULL);
    struct foc_impedance_tuning_input input = nominal;
    float *fields[] = {
        &input.inertia_kg_m2,
        &input.viscous_damping_nm_s_per_rad,
        &input.target_natural_frequency_rad_s,
        &input.target_damping_ratio,
        &input.torque_constant_nm_per_a,
        &input.max_stiffness_nm_per_rad,
        &input.max_damping_nm_s_per_rad,
        &input.iq_limit_a,
        &input.sample_period_s,
        &input.extra_delay_s
    };
    for (unsigned i = 0U; i < sizeof fields / sizeof fields[0]; ++i) {
        const float saved = *fields[i];
        *fields[i] = i % 2U ? INFINITY : NAN;
        CHECK(rejected(&input, FOC_IMPEDANCE_TUNING_ERR_NUMERIC) == 0);
        *fields[i] = -1.0f;
        CHECK(rejected(&input, FOC_IMPEDANCE_TUNING_ERR_CONFIG) == 0);
        *fields[i] = saved;
    }
    input.inertia_kg_m2 = 0.0f;
    CHECK(rejected(&input, FOC_IMPEDANCE_TUNING_ERR_CONFIG) == 0);
    input = nominal;
    input.torque_constant_nm_per_a = 0.0f;
    CHECK(rejected(&input, FOC_IMPEDANCE_TUNING_ERR_CONFIG) == 0);
    input = nominal;
    input.model_id = 0U;
    CHECK(rejected(&input, FOC_IMPEDANCE_TUNING_ERR_CONFIG) == 0);
    input = nominal;
    input.damping_source = FOC_IMPEDANCE_TUNING_DAMPING_ASSUMED_ZERO;
    CHECK(rejected(&input, FOC_IMPEDANCE_TUNING_ERR_CONFIG) == 0);
    input = nominal;
    input.viscous_damping_nm_s_per_rad = 0.06f;
    CHECK(rejected(&input, FOC_IMPEDANCE_TUNING_ERR_NEGATIVE_DAMPING) == 0);
    input = nominal;
    input.target_natural_frequency_rad_s = 250.0f;
    CHECK(rejected(&input, FOC_IMPEDANCE_TUNING_ERR_TIMING) == 0);
    input = nominal;
    input.extra_delay_s = 0.020f;
    CHECK(rejected(&input, FOC_IMPEDANCE_TUNING_ERR_TIMING) == 0);
    input = nominal;
    input.inertia_kg_m2 = 1.0e-30f;
    input.target_natural_frequency_rad_s = 3.0e-8f;
    input.viscous_damping_nm_s_per_rad = 0.0f;
    input.sample_period_s = 8.0e6f;
    input.extra_delay_s = 0.0f;
    CHECK(rejected(&input, FOC_IMPEDANCE_TUNING_ERR_TIMING) == 0);
    input = nominal;
    input.max_stiffness_nm_per_rad = 0.64f;
    CHECK(rejected(&input, FOC_IMPEDANCE_TUNING_ERR_GAIN_LIMIT) == 0);
    input = nominal;
    input.max_damping_nm_s_per_rad = 0.04f;
    CHECK(rejected(&input, FOC_IMPEDANCE_TUNING_ERR_GAIN_LIMIT) == 0);
    input = nominal;
    input.inertia_kg_m2 = FLT_MAX;
    CHECK(rejected(&input, FOC_IMPEDANCE_TUNING_ERR_NUMERIC) == 0);
    input = nominal;
    input.torque_constant_nm_per_a = FLT_MAX;
    CHECK(rejected(&input, FOC_IMPEDANCE_TUNING_ERR_NUMERIC) == 0);
    input = nominal;
    input.target_natural_frequency_rad_s = FLT_MIN;
    input.viscous_damping_nm_s_per_rad = 0.0f;
    CHECK(rejected(&input, FOC_IMPEDANCE_TUNING_ERR_NUMERIC) == 0);
    return 0;
}

/* Test Entry ------------------------------------------------------------------- */

int main(void)
{
    return test_analytic() || test_rejection();
}

/* ---------------------------------------- end of file ----------------------------------------- */
