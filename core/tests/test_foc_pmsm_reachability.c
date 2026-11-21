/**
 * @file    test_foc_pmsm_reachability.c
 * @brief   Independent steady-state PMSM and modulation boundary checks.
 * @version v0.1.0
 * @date    2026.11.20
 *
 * Test role
 * ---------
 * Compare signed operating points with independent limits and production SVPWM.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_pmsm_reachability.h"

#include <math.h>
#include <stdio.h>

/* Private Macros --------------------------------------------------------------- */

#define CHECK(condition)                                                          \
    do {                                                                          \
        if (!(condition)) {                                                       \
            (void)fprintf(stderr, "reachability:%d: %s\n", __LINE__, #condition); \
            return 1;                                                             \
        }                                                                         \
    } while (0)

/* Private Constants ------------------------------------------------------------ */

static const struct foc_pmsm_qualified_model model = {
    .rs_ohm = 0.6f,
    .ld_h = 0.0012f,
    .lq_h = 0.0015f,
    .flux_wb = 0.025f,
    .pole_pairs = 4U,
    .motor_id = 101U,
    .review_id = 0x1234U,
    .qualified = true,
};
static const struct foc_pmsm_reachability_limits limits = {
    .pwm = {6.0f, 2.4f, 0.0f, 1.0f, 0.0006f},
    .current_limit_a = 2.5f,
};

/* Private Helpers -------------------------------------------------------------- */

static int signed_points(void)
{
    struct foc_pmsm_reachability result;
    for (int sign = -1; sign <= 1; sign += 2) {
        const struct foc_pmsm_operating_point point =
            {FOC_PMSM_TARGET_CURRENT, 10.0f * sign, 0.4f, -0.6f * sign, 12.0f};
        CHECK(
            foc_pmsm_reachability_evaluate(&model, &limits, &point, &result) ==
            FOC_PMSM_MODEL_FEASIBLE
        );
        CHECK(result.motor_id == 101U && result.review_id == 0x1234U && result.range_valid);
        CHECK(fabs(result.electrical_rad_s - 40.0 * sign) < 1e-8);
        CHECK(fabs(result.vd_v - 0.276) < 1e-6);
        CHECK(fabs(result.vq_v - 0.6592 * sign) < 1e-6);
        CHECK(fabs(result.torque_nm + 0.089568 * sign) < 1e-6);
        CHECK(result.iq_min_a < point.target && result.iq_max_a > point.target);
    }
    return 0;
}

static int caps_and_provenance(void)
{
    struct foc_pmsm_reachability result;
    struct foc_pmsm_operating_point point = {FOC_PMSM_TARGET_TORQUE, 0, 0, 0.04f, 12};
    struct foc_pmsm_reachability_limits capped = limits;
    capped.torque_limit_nm = 0.06f;
    capped.pwm.voltage_limit_v = 0.72f;
    CHECK(
        foc_pmsm_reachability_evaluate(&model, &capped, &point, &result) == FOC_PMSM_MODEL_FEASIBLE
    );
    CHECK(fabs(result.iq_a - 0.266666667) < 1e-6);
    CHECK(fabs(result.iq_max_a - 0.4) < 1e-6 && fabs(result.iq_min_a + 0.4) < 1e-6);
    point.target = 0.08f;
    CHECK(
        foc_pmsm_reachability_evaluate(&model, &capped, &point, &result) ==
        FOC_PMSM_MODEL_INFEASIBLE
    );
    CHECK(result.reasons == FOC_PMSM_REASON_TORQUE_LIMIT);
    point = (struct foc_pmsm_operating_point){FOC_PMSM_TARGET_CURRENT, 0, 0, -2.7f, 6.1f};
    CHECK(
        foc_pmsm_reachability_evaluate(&model, &capped, &point, &result) ==
        FOC_PMSM_MODEL_INFEASIBLE
    );
    CHECK(
        result.reasons ==
        (FOC_PMSM_REASON_CURRENT_LIMIT | FOC_PMSM_REASON_TORQUE_LIMIT |
         FOC_PMSM_REASON_VOLTAGE_LIMIT)
    );
    point.vbus_v = 5.9f;
    CHECK(
        foc_pmsm_reachability_evaluate(&model, &capped, &point, &result) ==
        FOC_PMSM_MODEL_INFEASIBLE
    );
    CHECK((result.reasons & FOC_PMSM_REASON_BUS_MINIMUM) && !result.range_valid);
    struct foc_pmsm_qualified_model unqualified = model;
    unqualified.qualified = false;
    CHECK(
        foc_pmsm_reachability_evaluate(&unqualified, &capped, &point, &result) ==
        FOC_PMSM_INDETERMINATE
    );
    CHECK(result.reasons == FOC_PMSM_REASON_MODEL_UNQUALIFIED);
    point.vbus_v = NAN;
    CHECK(
        foc_pmsm_reachability_evaluate(&model, &capped, &point, &result) == FOC_PMSM_INDETERMINATE
    );
    CHECK(result.reasons == FOC_PMSM_REASON_INVALID_INPUT);
    return 0;
}

static int asymmetric_duty(void)
{
    struct foc_pmsm_reachability result;
    struct foc_pmsm_reachability_limits duty = {
        .pwm = {6.0f, 8.0f, 0.1f, 0.75f, 0.001f},
        .current_limit_a = 10.0f,
    };
    struct foc_pmsm_operating_point point = {FOC_PMSM_TARGET_CURRENT, 0, 0, 6.1f, 12};
    CHECK(foc_pmsm_reachability_evaluate(&model, &duty, &point, &result) == FOC_PMSM_INDETERMINATE);
    CHECK(result.reasons == FOC_PMSM_REASON_ANGLE_DEPENDENT && result.range_valid);
    CHECK(fabs(result.all_angle_limit_v - 3.4641016) < 1e-6);
    CHECK(fabs(result.best_angle_limit_v - 4.0) < 1e-6);
    CHECK(fabs(result.iq_max_a - 5.7735027) < 1e-6);
    struct foc_svpwm_output output;
    const float volts = (float)result.demanded_v;
    const struct foc_vector_alpha_beta alpha = {volts, 0.0f}, beta = {0.0f, volts};
    CHECK(
        foc_svpwm(&alpha, point.vbus_v, &duty.pwm, &output) == FOC_STATUS_OK &&
        output.saturated == 0U
    );
    CHECK(
        foc_svpwm(&beta, point.vbus_v, &duty.pwm, &output) == FOC_STATUS_OK &&
        output.saturated != 0U
    );
    point.target = 7.0f;
    CHECK(
        foc_pmsm_reachability_evaluate(&model, &duty, &point, &result) == FOC_PMSM_MODEL_INFEASIBLE
    );
    CHECK(result.reasons == FOC_PMSM_REASON_VOLTAGE_LIMIT);
    point.target = 5.7f;
    CHECK(
        foc_pmsm_reachability_evaluate(&model, &duty, &point, &result) == FOC_PMSM_MODEL_FEASIBLE
    );
    CHECK(result.near_voltage_limit);
    return 0;
}

/* Test Entry ------------------------------------------------------------------- */

int main(void)
{
    return signed_points() || caps_and_provenance() || asymmetric_duty();
}

/* ---------------------------------------- end of file ----------------------------------------- */
