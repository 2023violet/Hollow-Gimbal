/**
 * @file    foc_board_encoder_lut.h
 * @brief   CAL-9 encoder linearization: the fit and the board's correction table.
 * @version v0.1.0
 * @date    2026.11.25
 *
 * Design overview
 * ---------------
 * The calibration manager drags the rotor with a voltage vector forward and back over one
 * turn and records, per raw encoder angle, the field reference minus the encoder reading
 * (in the raw encoder frame) into the CAL-8 bins. The fit averages both directions
 * (friction lag cancels), removes the mean (alignment), and keeps the harmonics 1–16 per
 * turn that are not multiples of the pole pairs: the rotor's lag from cogging and pole
 * torque ripple repeats at multiples of the pole pairs, the encoder's error mostly at low
 * orders. The result is a 128-knot foc_encoder_lut in whole raw counts.
 *
 * Usage example
 * -------------
 * Sample with foc_board_cogging_sample() (value: error in rad, angle: raw angle), then call
 * foc_board_encoder_lut_fit(). The board encoder initializes with the table while active.
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * Foreground only; the board encoder reads the table when its sensor initializes, which
 * happens under the calibration manager's re-initialization.
 *
 * Failure / recovery
 * ------------------
 * A refused fit keeps the previous table; the status says why.
 */
#pragma once

#include "foc_board_cogging.h"
#include "foc_encoder.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Public Macros ---------------------------------------------------------------- */

/** Knots of the correction over one turn. */
#define FOC_BOARD_ENCODER_LUT_KNOTS     128U
/** Highest harmonic per turn the fit keeps. */
#define FOC_BOARD_ENCODER_LUT_HARMONICS 16U
/** foc_encoder_lut identity of Board1's encoder correction. */
#define FOC_BOARD_ENCODER_LUT_IDENTITY  UINT32_C(0x454E4331) /* "ENC1" */

/* Public Types ----------------------------------------------------------------- */

/** Outcome of the latest CAL-9 run (PARAM_READ ID 158); values as foc_board_cogging_status. */
enum foc_board_encoder_lut_status {
    FOC_BOARD_ENCODER_LUT_NONE = 0,
    FOC_BOARD_ENCODER_LUT_VALID = 1,
    FOC_BOARD_ENCODER_LUT_NEGLIGIBLE = 2,
    FOC_BOARD_ENCODER_LUT_INCONSISTENT = 3,
    FOC_BOARD_ENCODER_LUT_SAMPLES = 4,
    FOC_BOARD_ENCODER_LUT_FAILED = 5,
    FOC_BOARD_ENCODER_LUT_RANGE = 6,
};

struct foc_board_encoder_lut {
    /** Correction in raw counts at knot k * CPR / KNOTS; empty while counts_per_revolution is 0. */
    int16_t offsets[FOC_BOARD_ENCODER_LUT_KNOTS];
    uint32_t counts_per_revolution;
    /** The board encoder initializes with the table (set before a re-initialization). */
    bool active;
    /* Result of the latest run. */
    enum foc_board_encoder_lut_status status;
    float peak_rad;
    float rms_rad;
    float consistency;
    /** The table changed since it was last saved or restored. */
    bool dirty;
};

/** Board1's table, read by its encoder at initialization. */
extern struct foc_board_encoder_lut foc_board_encoder_lut_state;

/* Public Functions ------------------------------------------------------------- */

/** True and the descriptor for a non-empty table of this CPR. */
bool foc_board_encoder_lut_descriptor(const struct foc_board_encoder_lut *table,
                                      uint32_t counts_per_revolution,
                                      struct foc_encoder_lut *descriptor);

/**
 * Fit the correction from the bins (values: error in rad, by raw angle).
 *
 * @param table Table and result; the table changes only for VALID and NEGLIGIBLE (cleared).
 * @param bins CAL-8 bins after a forward and a backward sweep.
 * @param pole_pairs Pole pairs; their multiples are not fitted (2 or more).
 * @param counts_per_revolution Raw counts per turn.
 */
enum foc_board_encoder_lut_status foc_board_encoder_lut_fit(struct foc_board_encoder_lut *table,
                                                            struct foc_board_cogging *bins,
                                                            uint32_t pole_pairs,
                                                            uint32_t counts_per_revolution);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
