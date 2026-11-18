/**
 * @file    test_foc_impedance_feedforward.c
 * @brief   Mechanical motion feedforward numerical and provenance contract.
 * @version v0.1.0
 * @date    2026.11.18
 *
 * Test role
 * ---------
 * Check a fixed axis, exact budget boundaries, and complete rejection outputs.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_impedance_feedforward.h"

#include <float.h>
#include <math.h>
#include <stdio.h>

/* Private Macros --------------------------------------------------------------- */

#define CHECK(condition)                                                             \
    do {                                                                             \
        if (!(condition)) {                                                          \
            fprintf(stderr, "impedance feedforward:%d: %s\n", __LINE__, #condition); \
            return 1;                                                                \
        }                                                                            \
    } while (0)

/* Private Constants ------------------------------------------------------------ */

static const struct foc_impedance_feedforward_input qualified = {
    .inertia_kg_m2 = 0.002f,
    .viscous_damping_nm_s_per_rad = 0.006f,
    .reference_velocity_rad_s = -1.0f,
    .reference_acceleration_rad_s2 = -10.0f,
    .limit_nm = 0.06f,
    .model_id = 100U,
    .model_frame_id = 7U,
    .reference_frame_id = 7U,
    .reference_qualified = true,
};

/* Private Helpers -------------------------------------------------------------- */

static int test_motion_torque(void)
{
    struct foc_impedance_feedforward_input input = qualified;
    struct foc_impedance_feedforward_result output;
    CHECK(foc_impedance_feedforward_evaluate(&input, &output) == 0);
    CHECK(output.valid && output.model_id == 100U && output.mechanical_frame_id == 7U);
    CHECK(fabsf(output.torque_nm + 0.026f) < 1.0e-8f);
    input.reference_acceleration_rad_s2 = 10.0f; /* Negative motion braking. */
    CHECK(foc_impedance_feedforward_evaluate(&input, &output) == 0);
    CHECK(fabsf(output.torque_nm - 0.014f) < 1.0e-8f);
    input.reference_velocity_rad_s = 0.0f;
    input.reference_acceleration_rad_s2 = 0.0f;
    input.limit_nm = 0.0f;
    CHECK(foc_impedance_feedforward_evaluate(&input, &output) == 0);
    CHECK(output.valid && output.torque_nm == 0.0f);
    input.inertia_kg_m2 = 0.125f;
    input.viscous_damping_nm_s_per_rad = 0.0f;
    input.reference_acceleration_rad_s2 = 2.0f;
    input.limit_nm = 0.25f;
    CHECK(foc_impedance_feedforward_evaluate(&input, &output) == 0);
    CHECK(output.valid && output.torque_nm == input.limit_nm);
    input.reference_acceleration_rad_s2 = -2.0f;
    CHECK(foc_impedance_feedforward_evaluate(&input, &output) == 0);
    CHECK(output.valid && output.torque_nm == -input.limit_nm);
    return 0;
}

static int test_limits_and_metadata(void)
{
    struct foc_impedance_feedforward_input input = qualified;
    struct foc_impedance_feedforward_result output;
    for (unsigned i = 0U; i < 18U; ++i) {
        input = qualified;
        int expected = FOC_IMPEDANCE_FEEDFORWARD_ERR_CONFIG;
        switch (i) {
        case 0:
            input.inertia_kg_m2 = 0.0f;
            break;
        case 1:
            input.inertia_kg_m2 = -1.0f;
            break;
        case 2:
            input.viscous_damping_nm_s_per_rad = -1.0f;
            break;
        case 3:
            input.limit_nm = -0.01f;
            break;
        case 4:
            input.model_id = 0U;
            break;
        case 5:
            input.model_frame_id = input.reference_frame_id = 0U;
            break;
        case 6:
            input.reference_frame_id++;
            expected = FOC_IMPEDANCE_FEEDFORWARD_ERR_REFERENCE;
            break;
        case 7:
            input.reference_qualified = false;
            expected = FOC_IMPEDANCE_FEEDFORWARD_ERR_REFERENCE;
            break;
        case 8:
            input.reference_velocity_rad_s = INFINITY;
            expected = FOC_IMPEDANCE_FEEDFORWARD_ERR_NUMERIC;
            break;
        case 9:
            input.reference_acceleration_rad_s2 = NAN;
            expected = FOC_IMPEDANCE_FEEDFORWARD_ERR_NUMERIC;
            break;
        case 10:
            input.inertia_kg_m2 = INFINITY;
            expected = FOC_IMPEDANCE_FEEDFORWARD_ERR_NUMERIC;
            break;
        case 11:
            input.viscous_damping_nm_s_per_rad = NAN;
            expected = FOC_IMPEDANCE_FEEDFORWARD_ERR_NUMERIC;
            break;
        case 12:
            input.limit_nm = INFINITY;
            expected = FOC_IMPEDANCE_FEEDFORWARD_ERR_NUMERIC;
            break;
        case 13:
            input.inertia_kg_m2 = FLT_MAX;
            input.reference_acceleration_rad_s2 = 2.0f;
            expected = FOC_IMPEDANCE_FEEDFORWARD_ERR_NUMERIC;
            break;
        case 14:
            input.inertia_kg_m2 = FLT_TRUE_MIN;
            input.reference_acceleration_rad_s2 = 0.01f;
            input.viscous_damping_nm_s_per_rad = 0.0f;
            expected = FOC_IMPEDANCE_FEEDFORWARD_ERR_NUMERIC;
            break;
        case 15:
            input.limit_nm = 0.025f;
            expected = FOC_IMPEDANCE_FEEDFORWARD_ERR_LIMIT;
            break;
        case 16:
        case 17:
            input.inertia_kg_m2 = 0.25f;
            input.reference_acceleration_rad_s2 = i == 16U ? 1.0f : -1.0f;
            input.viscous_damping_nm_s_per_rad = 0.25f;
            input.reference_velocity_rad_s = i == 16U ? 0x1p-60f : -0x1p-60f;
            input.limit_nm = 0.25f;
            expected = FOC_IMPEDANCE_FEEDFORWARD_ERR_LIMIT;
            break;
        }
        output = (struct foc_impedance_feedforward_result){1.0f, 21U, 8U, true};
        CHECK(foc_impedance_feedforward_evaluate(&input, &output) == expected);
        CHECK(
            !output.valid && output.torque_nm == 0.0f && output.model_id == 0U &&
            output.mechanical_frame_id == 0U
        );
    }
    CHECK(foc_impedance_feedforward_evaluate(NULL, &output) == FOC_IMPEDANCE_FEEDFORWARD_ERR_NULL);
    CHECK(!output.valid);
    CHECK(foc_impedance_feedforward_evaluate(&input, NULL) == FOC_IMPEDANCE_FEEDFORWARD_ERR_NULL);
    return 0;
}

/* Test Entry ------------------------------------------------------------------- */

int main(void)
{
    return test_motion_torque() || test_limits_and_metadata();
}

/* ---------------------------------------- end of file ----------------------------------------- */
