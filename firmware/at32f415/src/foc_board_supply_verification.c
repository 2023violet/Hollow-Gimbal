/**
 * @file    foc_board_supply_verification.c
 * @brief   Synthetic calibration supply for the locked software-verification image only.
 * @version v0.1.0
 * @date    2026.11.21
 *
 * Implementation role
 * -------------------
 * SYNTHETIC TEST DATA. Not a measurement of any board or motor. The values are
 * deliberately non-nominal and distinguishable (A/B/C offsets, calibration
 * direction -1 giving runtime direction +1, 11 pole pairs, 1.25 rad offset) so
 * the locked verification image can prove that the formal main() consumes a
 * supply. They are reachable by the existing CAL-01 estimators (see
 * test_foc_board_boot.c) and pass the existing admission. Only the
 * verification build links this file; the default build links
 * foc_board_supply_none.c.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_board_boot.h"

/* Private Macros --------------------------------------------------------------- */

#define FOC_VERIFY_POLE_PAIRS  11U
#define FOC_VERIFY_A_PER_COUNT (3.3f / (4095.0f * 0.5f))
#define FOC_VERIFY_OFFSET_RAD  1.2509766f

/* Private Constants ------------------------------------------------------------ */

/* Board1 raw-code identity: 4095 full scale, 0.5 V/A CSA, 65536 CPR, raw -1, ABC. */
#define FOC_VERIFY_IDENTITY(direction)                 \
    {                                                  \
        .adc_full_scale_counts = 4095U,                \
        .current_a_per_count = FOC_VERIFY_A_PER_COUNT, \
        .encoder_counts_per_revolution = 65536U,       \
        .encoder_direction = (direction),              \
        .phase_order = 0U,                             \
        .pole_pairs = FOC_VERIFY_POLE_PAIRS,           \
    }

static const struct foc_board_calibration_supply foc_board_verification_supply = {
    .format = FOC_BOARD_CALIBRATION_SUPPLY_FORMAT,
    .supply_id = 0x56464331U, /* "1CFV" synthetic verification supply */
    .review_id = 0x01210001U, /* NEXT-FW-INTEG-01 synthetic review record */
    .coordinate = FOC_BOARD_COORDINATE_RAW_NO_LUT,
    .adc =
        {
            .identity = FOC_VERIFY_IDENTITY(-1),
            .offset_a_counts = 2031.5f,
            .offset_b_counts = 2063.25f,
            .offset_c_counts = 2017.75f,
            .noise_rms_a_counts = 0.5f,
            .noise_rms_b_counts = 0.4330127f,
            .noise_rms_c_counts = 0.4330127f,
            .samples = 64U,
            .state = FOC_CALIBRATION_STATE_VALID,
            .failure = FOC_CALIBRATION_FAILURE_NONE,
            .usable = true,
        },
    .rotor =
        {
            .identity = FOC_VERIFY_IDENTITY(-1),
            .direction = -1,
            .pole_pairs = FOC_VERIFY_POLE_PAIRS,
            .electrical_offset_rad = 1.25f,
            .residual_rms_rad = 0.004f,
            .residual_max_rad = 0.009f,
            .candidate_separation_rad = 0.31f,
            .mechanical_span_rad = 28.47f,
            .samples = 40U,
            .state = FOC_CALIBRATION_STATE_VALID,
            .failure = FOC_CALIBRATION_FAILURE_NONE,
            .identifiable = true,
        },
    .offset =
        {
            .identity = FOC_VERIFY_IDENTITY(-1),
            .electrical_offset_rad = FOC_VERIFY_OFFSET_RAD,
            .residual_rms_rad = 0.0021f,
            .residual_max_rad = 0.0047f,
            .samples = 20U,
            .state = FOC_CALIBRATION_STATE_VALID,
            .failure = FOC_CALIBRATION_FAILURE_NONE,
            .usable = true,
        },
    .limits =
        {
            .minimum_adc_samples = 32U,
            .minimum_offset_counts = 1800.0f,
            .maximum_offset_counts = 2300.0f,
            .maximum_noise_rms_counts = 2.0f,
            .maximum_rotor_residual_rad = 0.02f,
            .maximum_offset_residual_rad = 0.02f,
        },
    .snapshot = {
        .source_identity = FOC_VERIFY_IDENTITY(-1),
        .phase_a_offset_count = 2031.5f,
        .phase_b_offset_count = 2063.25f,
        .phase_c_offset_count = 2017.75f,
        .calibration_direction = -1,
        .runtime_encoder_direction = 1,
        .pole_pairs = FOC_VERIFY_POLE_PAIRS,
        .electrical_offset_rad = FOC_VERIFY_OFFSET_RAD,
        .accepted = true,
    },
};

/* Private Helpers -------------------------------------------------------------- */

/* None: the record is immutable .rodata. */

/* Public Functions ------------------------------------------------------------- */

const struct foc_board_calibration_supply *foc_board_calibration_supply(void)
{
    return &foc_board_verification_supply;
}

/* ---------------------------------------- end of file ----------------------------------------- */
