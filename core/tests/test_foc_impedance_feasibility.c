/**
 * @file    test_foc_impedance_feasibility.c
 * @brief   Numerical and provenance contracts of static impedance diagnosis.
 * @version v0.1.0
 * @date    2026.11.18
 *
 * Test role
 * ---------
 * Check numeric boundaries and source admission with fixed motor settings.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_impedance_feasibility.h"

#include <float.h>
#include <math.h>
#include <stdio.h>

/* Private Macros --------------------------------------------------------------- */

#define CHECK(expression)                                                             \
    do {                                                                              \
        if (!(expression)) {                                                          \
            fprintf(stderr, "impedance feasibility:%d: %s\n", __LINE__, #expression); \
            return 1;                                                                 \
        }                                                                             \
    } while (0)

/* Private Constants ------------------------------------------------------------ */

static const struct foc_impedance_config settings = {0.5f, 0.048f, 0.15f, 0.315f};
static const struct foc_impedance_feasibility_input qualified = {
    .live_iq_limit_a = 0.5f,
    .external_load_nm = 0.039f,
    .position_tolerance_rad = 0.08f,
    .torque_model_id = 2U,
    .load_model_id = 3U,
    .stationary_target = true,
    .support_torque_accounted = true,
    .current_tracking_qualified = true,
};

/* Private Helpers -------------------------------------------------------------- */

static int test_load_and_limits(void)
{
    struct foc_impedance_feasibility_input input = qualified;
    struct foc_impedance_config config = settings;
    struct foc_impedance_feasibility_result r;
    CHECK(foc_impedance_feasibility_evaluate(&config, &input, &r) == FOC_STATUS_OK);
    CHECK(
        r.verdict == FOC_IMPEDANCE_FEASIBILITY_REACHABLE && r.position_constrained &&
        r.has_equilibrium && !r.at_torque_limit
    );
    CHECK(fabsf(r.equilibrium_error_rad - 0.078f) < 1e-7f && r.torque_margin_nm > 0.035f);
    input.position_tolerance_rad = 0.04f;
    input.external_load_nm = -0.039f;
    CHECK(foc_impedance_feasibility_evaluate(&config, &input, &r) == FOC_STATUS_OK);
    CHECK(
        r.verdict == FOC_IMPEDANCE_FEASIBILITY_POSITION_TOLERANCE_EXCEEDED && r.has_equilibrium &&
        fabsf(r.equilibrium_error_rad + 0.078f) < 1e-7f
    );
    input.torque_feedforward_nm = -0.039f;
    CHECK(foc_impedance_feasibility_evaluate(&config, &input, &r) == FOC_STATUS_OK);
    CHECK(r.verdict == FOC_IMPEDANCE_FEASIBILITY_REACHABLE && r.equilibrium_error_rad == 0.0f);
    input.live_iq_limit_a = 0.25f;
    CHECK(foc_impedance_feasibility_evaluate(&config, &input, &r) == FOC_STATUS_OK);
    CHECK(
        r.verdict == FOC_IMPEDANCE_FEASIBILITY_TORQUE_INSUFFICIENT && !r.has_equilibrium &&
        r.torque_capacity_nm == 0.0375f && r.torque_margin_nm < 0.0f
    );
    /* A rounded-up Float32 product cannot fund that same Float32 load. */
    config.torque_constant_nm_per_a = 0.112f;
    input.live_iq_limit_a = 1.3f;
    input.external_load_nm = 0.14560000598430634f;
    CHECK(foc_impedance_feasibility_evaluate(&config, &input, &r) == FOC_STATUS_OK);
    CHECK(
        r.verdict == FOC_IMPEDANCE_FEASIBILITY_TORQUE_INSUFFICIENT && !r.at_torque_limit &&
        r.torque_margin_nm < 0.0
    );
    config = settings;
    input.live_iq_limit_a = 0.5f;
    input.external_load_nm = -0.039f;
    config.torque_limit_nm = 0.03f;
    CHECK(foc_impedance_feasibility_evaluate(&config, &input, &r) == FOC_STATUS_OK);
    CHECK(
        r.verdict == FOC_IMPEDANCE_FEASIBILITY_TORQUE_INSUFFICIENT &&
        r.torque_capacity_nm == config.torque_limit_nm
    );
    return 0;
}

static int test_equilibrium_boundaries(void)
{
    struct foc_impedance_feasibility_input input = qualified;
    struct foc_impedance_config config = settings;
    struct foc_impedance_feasibility_result r;
    input.live_iq_limit_a = 0.25f;
    input.external_load_nm = 0.0375f;
    CHECK(foc_impedance_feasibility_evaluate(&config, &input, &r) == 0);
    CHECK(
        r.verdict == FOC_IMPEDANCE_FEASIBILITY_REACHABLE && !r.position_constrained &&
        r.at_torque_limit && r.torque_margin_nm == 0.0f && r.equilibrium_error_rad == 0.075f
    );
    input.torque_feedforward_nm = 0.05f;
    CHECK(foc_impedance_feasibility_evaluate(&config, &input, &r) == 0);
    CHECK(r.at_torque_limit && r.equilibrium_error_rad == 0.0f);
    input.external_load_nm = -0.0375f;
    input.torque_feedforward_nm = -0.05f;
    CHECK(foc_impedance_feasibility_evaluate(&config, &input, &r) == 0);
    CHECK(r.at_torque_limit && r.equilibrium_error_rad == 0.0f);
    config.stiffness_nm_per_rad = 0.0f;
    CHECK(foc_impedance_feasibility_evaluate(&config, &input, &r) == 0);
    CHECK(r.verdict == FOC_IMPEDANCE_FEASIBILITY_REACHABLE && !r.position_constrained);
    input.external_load_nm = -0.03f;
    CHECK(foc_impedance_feasibility_evaluate(&config, &input, &r) == 0);
    CHECK(r.verdict == FOC_IMPEDANCE_FEASIBILITY_NO_EQUILIBRIUM && !r.has_equilibrium);
    input.torque_feedforward_nm = -0.03f;
    CHECK(foc_impedance_feasibility_evaluate(&config, &input, &r) == 0);
    CHECK(r.has_equilibrium && !r.position_constrained && !r.at_torque_limit);
    input.live_iq_limit_a = 0.0f;
    input.external_load_nm = 0.0f;
    CHECK(foc_impedance_feasibility_evaluate(&config, &input, &r) == 0);
    CHECK(r.has_equilibrium && !r.position_constrained && r.at_torque_limit);
    return 0;
}

static int test_source_and_invalid(void)
{
    struct foc_impedance_feasibility_input input = qualified;
    struct foc_impedance_config config = settings;
    struct foc_impedance_feasibility_result r;
    input.load_model_id = 0U;
    CHECK(foc_impedance_feasibility_evaluate(&config, &input, &r) == 0);
    CHECK(
        r.verdict == FOC_IMPEDANCE_FEASIBILITY_UNKNOWN && !r.has_equilibrium &&
        r.torque_capacity_nm == 0.0f
    );
    input = qualified;
    input.torque_model_id = 0U;
    CHECK(foc_impedance_feasibility_evaluate(&config, &input, &r) == 0);
    CHECK(r.verdict == FOC_IMPEDANCE_FEASIBILITY_UNKNOWN);
    input = qualified;
    input.support_torque_accounted = false;
    CHECK(foc_impedance_feasibility_evaluate(&config, &input, &r) == 0);
    CHECK(r.verdict == FOC_IMPEDANCE_FEASIBILITY_UNKNOWN);
    input.support_torque_accounted = true;
    input.current_tracking_qualified = false;
    CHECK(foc_impedance_feasibility_evaluate(&config, &input, &r) == 0);
    CHECK(r.verdict == FOC_IMPEDANCE_FEASIBILITY_UNKNOWN);
    input.stationary_target = false;
    CHECK(foc_impedance_feasibility_evaluate(&config, &input, &r) == 0);
    CHECK(r.verdict == FOC_IMPEDANCE_FEASIBILITY_MOVING_TARGET && !r.has_equilibrium);

    input = qualified;
    input.torque_feedforward_nm = NAN;
    CHECK(foc_impedance_feasibility_evaluate(&config, &input, &r) == FOC_STATUS_ERR_NONFINITE);
    CHECK(r.verdict == FOC_IMPEDANCE_FEASIBILITY_INVALID && !r.has_equilibrium);
    input = qualified;
    config.torque_constant_nm_per_a = 0.0f;
    CHECK(foc_impedance_feasibility_evaluate(&config, &input, &r) == FOC_STATUS_ERR_INVALID_CONFIG);
    config = settings;
    config.damping_nm_s_per_rad = INFINITY;
    CHECK(foc_impedance_feasibility_evaluate(&config, &input, &r) == FOC_STATUS_ERR_NONFINITE);
    config = settings;
    config.stiffness_nm_per_rad = FLT_MIN;
    input.torque_feedforward_nm = -FLT_MAX;
    CHECK(foc_impedance_feasibility_evaluate(&config, &input, &r) == FOC_STATUS_ERR_NONFINITE);
    CHECK(r.verdict == FOC_IMPEDANCE_FEASIBILITY_INVALID && r.torque_capacity_nm == 0.0f);
    config = settings;
    config.torque_constant_nm_per_a = FLT_MAX;
    input = qualified;
    input.live_iq_limit_a = 2.0f;
    CHECK(foc_impedance_feasibility_evaluate(&config, &input, &r) == FOC_STATUS_ERR_NONFINITE);
    config.torque_constant_nm_per_a = FLT_TRUE_MIN;
    input.live_iq_limit_a = FLT_TRUE_MIN;
    CHECK(foc_impedance_feasibility_evaluate(&config, &input, &r) == FOC_STATUS_ERR_NONFINITE);
    CHECK(foc_impedance_feasibility_evaluate(NULL, &input, &r) == FOC_STATUS_ERR_NULL);
    CHECK(r.verdict == FOC_IMPEDANCE_FEASIBILITY_INVALID && !r.has_equilibrium);
    CHECK(foc_impedance_feasibility_evaluate(&config, &input, NULL) == FOC_STATUS_ERR_NULL);
    return 0;
}

/* Test Entry ------------------------------------------------------------------- */

int main(void)
{
    return test_load_and_limits() || test_equilibrium_boundaries() || test_source_and_invalid();
}

/* ---------------------------------------- end of file ----------------------------------------- */
