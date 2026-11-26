/**
 * @file    foc_board_cogging.h
 * @brief   CAL-8 cogging acquisition: slow sweep reference, angle bins and the table fit.
 * @version v0.1.0
 * @date    2026.11.25
 *
 * Design overview
 * ---------------
 * The calibration manager holds POSITION mode and moves the reference at a constant slow
 * speed: a lead-in, one captured forward turn (plus a margin for the loop lag), a turn
 * around and one captured backward turn. Every new sensor sample adds the outer loop's q
 * current to the bin of its angle modulo one turn, separately per direction. The fit
 * averages both directions (constant friction cancels), removes the mean (constant load)
 * and by default the once-per-turn component (imbalance and eccentricity), and quantizes
 * the result into the Runtime's foc_runtime_cogging_table.
 *
 * Usage example
 * -------------
 * Begin with the start position and speed, call foc_board_cogging_step() every Foreground
 * pass to get the reference and the capture direction, feed new samples to
 * foc_board_cogging_sample(), and call foc_board_cogging_finish() once the step reports
 * the end, with compensation off and the drive stopped.
 *
  * Ownership / lifecycle / context
 * -------------------------------
 * Foreground only. The table is read by Runtime H1 while compensation is on; finish writes
 * it only while compensation is off. The bins are caller storage (`bins`, 5 KiB) that the
 * owner may reuse between runs: Board1 lends the calibration manager's work area, which the
 * other stages use while no sweep runs. Init keeps the pointer and does not touch the bins;
 * begin clears them.
 *
 * Failure / recovery
 * ------------------
 * A failed fit keeps the previous table; the status says why.
 */
#pragma once

#include "foc_runtime.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Public Macros ---------------------------------------------------------------- */

/** Uncaptured travel before the forward turn and around the turn-around, rotor rad. */
#define FOC_BOARD_COGGING_LEAD_RAD        0.5f
/** Captured travel per direction beyond one turn, covering the position loop's lag. */
#define FOC_BOARD_COGGING_MARGIN_RAD      0.3f
/** Largest share of bins without samples (filled by interpolation) per direction. */
#define FOC_BOARD_COGGING_MAX_EMPTY       0.05f
/** Smallest correlation of the forward and backward shapes. */
#define FOC_BOARD_COGGING_MIN_CONSISTENCY 0.5f

/* Public Types ----------------------------------------------------------------- */

/** Outcome of the latest CAL-8 run (PARAM_READ ID 152). */
enum foc_board_cogging_status {
    FOC_BOARD_COGGING_NONE = 0,
    /** Table fitted and compensation switched on. */
    FOC_BOARD_COGGING_VALID = 1,
    /** Peak below the floor: no compensation, the table is cleared. */
    FOC_BOARD_COGGING_NEGLIGIBLE = 2,
    /** Forward and backward shapes disagree (noise, slip or a moving load). */
    FOC_BOARD_COGGING_INCONSISTENT = 3,
    /** Too many bins without samples (the rotor did not follow the sweep). */
    FOC_BOARD_COGGING_SAMPLES = 4,
    /** The drive or a lifecycle step failed, or the run was aborted. */
    FOC_BOARD_COGGING_FAILED = 5,
    /** Peak above half the outer q-current limit. */
    FOC_BOARD_COGGING_RANGE = 6,
};

/** Sweep segments. */
enum foc_board_cogging_phase {
    FOC_BOARD_COGGING_IDLE = 0,
    FOC_BOARD_COGGING_LEAD = 1,
    FOC_BOARD_COGGING_FORWARD = 2,
    FOC_BOARD_COGGING_TURN = 3,
    FOC_BOARD_COGGING_BACKWARD = 4,
    FOC_BOARD_COGGING_END = 5,
};

/** Sweep bins shared by CAL-8 and CAL-9: valid from begin until the fit has run. */
struct foc_board_cal_bins {
    /** Per-direction running mean per bin in 0.1 mA (INT16_MIN: no sample). */
    int16_t mean[2][FOC_RUNTIME_COGGING_BINS];
    /** Sample count of the direction being captured (saturating). */
    uint8_t count[FOC_RUNTIME_COGGING_BINS];
};

struct foc_board_cogging {
    /** The live table the Runtime points at. */
    struct foc_runtime_cogging_table table;
    /** Caller-owned bins; begin, sample and finish do nothing useful while NULL. */
    struct foc_board_cal_bins *bins;
    uint32_t empty[2];
    /* Sweep. */
    enum foc_board_cogging_phase phase;
    float origin_rad;
    float speed_rad_s;
    float travel_rad;
    uint32_t last_sensor_us;
    bool sensor_seen;
    /* Result of the latest run. */
    enum foc_board_cogging_status status;
    float peak_a;
    float rms_a;
    float friction_a;
    float consistency;
    /** The table changed since it was last saved or restored. */
    bool dirty;
};

/** Board1's instance: every Board1 Runtime configuration points at its table. */
extern struct foc_board_cogging foc_board_cogging_state;

/* Public Functions ------------------------------------------------------------- */

/** Clear the table and the result; keeps the bins pointer and leaves the bins alone. */
void foc_board_cogging_init(struct foc_board_cogging *cogging);

/** Start a sweep at origin_rad (rotor) with speed_rad_s > 0 and clear the bins; keeps the
 * table. */
void foc_board_cogging_begin(struct foc_board_cogging *cogging,
                             float origin_rad,
                             float speed_rad_s);

/**
 * Place the sweep at elapsed_us since foc_board_cogging_begin() (travel = speed x time, so
 * the reference does not depend on how often the caller polls).
 *
 * @param reference_rad Rotor position reference for this pass.
 * @return Capture direction: 0 forward, 1 backward, -1 none; the phase becomes END after
 *         the backward turn.
 */
int foc_board_cogging_step(struct foc_board_cogging *cogging,
                           uint32_t elapsed_us,
                           float *reference_rad);

/** Add one sample for the direction step returned; a repeated sensor_us is ignored. */
void foc_board_cogging_sample(struct foc_board_cogging *cogging,
                              int direction,
                              uint32_t sensor_us,
                              float mechanical_rad,
                              float iq_a);

/**
 * Fit the table from both directions and set the result.
 *
 * @param keep_first_harmonic Keep the once-per-turn component.
 * @param iq_limit_a Outer q-current limit for the range check.
  * @return The status (FAILED without bins); the table changes only for VALID (new table) and
 *         NEGLIGIBLE (cleared).
 */
enum foc_board_cogging_status foc_board_cogging_finish(struct foc_board_cogging *cogging,
                                                       bool keep_first_harmonic,
                                                       float iq_limit_a);

/** Bins without samples (INT16_MIN) in one direction's array. */
uint32_t foc_board_cogging_empty_bins(const int16_t *bins);

/** Fill empty bins by circular linear interpolation; false when all are empty. */
bool foc_board_cogging_fill_bins(int16_t *bins);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
