/**
 * @file    foc_board_calibrate.h
 * @brief   Board1 on-MCU calibration manager: CAL-1 ADC zero and CAL-2 rotor mapping.
 * @version v0.1.0
 * @date    2026.11.22
 *
 * Design overview
 * ---------------
 * A Foreground state machine runs the existing estimators on live firmware data:
 * CAL-1 collects ADC frames in H0 (through the Board ADC tap) while the output is
 * inhibited and the shaft is still; CAL-2 selects the Runtime CALIBRATION mode,
 * drives a d-axis voltage vector at a commanded electrical angle that sweeps
 * forward and back, and pairs each angle with the encoder position. The existing
 * admission turns the three candidates into one snapshot, and the instance is
 * re-initialized with it. Nothing reads the plant's or a fixture's true values.
 *
 * Usage example
 * -------------
 * foc_board_calibration_bind() after boot; foc_board_calibration_poll() every
 * Foreground pass after foc_serial_poll(); attach foc_board_calibration_serial()
 * to the UART adapter.
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * Foreground owns the manager. The ADC estimator is H0-owned while collecting.
 * The reconfigure hook must hold H0/H1 producers off while it runs.
 *
 * Failure / recovery
 * ------------------
 * Any failure or STOP ends the drive, leaves the previous configuration in place
 * and reports the stage and reason. A completed run applies all results or none.
 */
#pragma once

#include "foc_board.h"
#include "foc_calibration.h"
#include "foc_serial.h"

#include <stdatomic.h>

/* Public Types ---------------------------------------------------------------- */

/** Wire values of CALIBRATE_READ byte 1. */
enum foc_board_cal_state {
    FOC_BOARD_CAL_IDLE = 0,
    FOC_BOARD_CAL_ADC_ZERO = 1,
    FOC_BOARD_CAL_DRIVE_MODE = 2,
    FOC_BOARD_CAL_DRIVE_ARM = 3,
    FOC_BOARD_CAL_DRIVE_START = 4,
    FOC_BOARD_CAL_ALIGN = 5,
    FOC_BOARD_CAL_SWEEP = 6,
    FOC_BOARD_CAL_DRIVE_STOP = 7,
    FOC_BOARD_CAL_APPLY = 8,
    FOC_BOARD_CAL_DONE = 9,
    FOC_BOARD_CAL_FAILED = 10,
};

/** Wire values of CALIBRATE_READ byte 2. */
enum foc_board_cal_failure {
    FOC_BOARD_CAL_FAILURE_NONE = 0,
    /** Not DISABLED with INHIBIT confirmed, or the shaft was moving. */
    FOC_BOARD_CAL_FAILURE_STATE = 1,
    /** CAL-1 samples failed the estimator quality checks. */
    FOC_BOARD_CAL_FAILURE_ADC = 2,
    /** A lifecycle request (SET_MODE/ARM/START) was not applied. */
    FOC_BOARD_CAL_FAILURE_LIFECYCLE = 3,
    /** The drive left RUNNING (fault, lease, output refused). */
    FOC_BOARD_CAL_FAILURE_DRIVE = 4,
    /** The alignment current could not be reached within the voltage limit. */
    FOC_BOARD_CAL_FAILURE_CURRENT = 5,
    /** The encoder did not deliver fresh positions. */
    FOC_BOARD_CAL_FAILURE_SENSOR = 6,
    /** Direction/pole pairs were not identifiable. */
    FOC_BOARD_CAL_FAILURE_ROTOR = 7,
    /** The electrical offset fit failed. */
    FOC_BOARD_CAL_FAILURE_OFFSET = 8,
    /** The admission rejected the candidates; see admission_failure. */
    FOC_BOARD_CAL_FAILURE_ADMISSION = 9,
    /** Re-initialization with the snapshot failed. */
    FOC_BOARD_CAL_FAILURE_APPLY = 10,
    /** STOP or an abort request ended the run. */
    FOC_BOARD_CAL_FAILURE_ABORTED = 11,
    /** A stage exceeded its time budget. */
    FOC_BOARD_CAL_FAILURE_TIMEOUT = 12,
};

/** Raw sweep samples: forward and backward points of CAL-2. */
#define FOC_BOARD_CAL_SWEEP_SAMPLES 64U

/** Producer hold for reconfiguration. hold true masks H0/H1, false releases them. */
typedef void (*foc_board_cal_quiesce)(void *argument, bool hold);

struct foc_board_calibration {
    /* Bindings. */
    struct foc_runtime *runtime;
    struct foc_board_output *output;
    struct foc_board_encoder *encoder;
    struct foc_board_adc *adc;
    struct foc_serial *serial;
    foc_board_cal_quiesce quiesce;
    void *quiesce_argument;
    /* Progress. */
    enum foc_board_cal_state state;
    enum foc_board_cal_failure failure;
    enum foc_board_cal_state failed_state;
    enum foc_calibration_admission_failure admission_failure;
    uint32_t state_us;
    uint32_t last_us;
    uint32_t lifecycle_sequence;
    uint32_t command_sequence;
    uint32_t attempts;
    uint32_t retry_us;
    uint32_t sample_index;
    float voltage_v;
    float theta_rad;
    float sweep_rad;
    struct foc_runtime_tuning tuning;
    /* CAL-1: H0 collects while adc_active is set. */
    atomic_uint adc_active;
    atomic_uint adc_samples;
    struct foc_calibration_adc_context adc_estimator;
    /* CAL-2: raw sweep, then forward/backward pairs feed the estimators. */
    struct foc_calibration_angle_sample sweep[FOC_BOARD_CAL_SWEEP_SAMPLES];
    uint32_t sweep_count;
    struct foc_calibration_rotor_context rotor_estimator;
    struct foc_calibration_offset_context offset_estimator;
    /* Results of the latest run and the applied snapshot. */
    struct foc_calibration_adc_result adc_result;
    struct foc_calibration_rotor_result rotor_result;
    struct foc_calibration_offset_result offset_result;
    struct foc_calibration_snapshot snapshot;
    uint32_t applied_runs;
};

/* Public Functions ------------------------------------------------------------- */

/** Bind the live instances and install the ADC tap. Call after boot, before producers. */
void foc_board_calibration_bind(struct foc_board_calibration *calibration,
                                struct foc_runtime *runtime,
                                struct foc_board_output *output,
                                struct foc_board_encoder *encoder,
                                struct foc_board_adc *adc,
                                struct foc_serial *serial,
                                foc_board_cal_quiesce quiesce,
                                void *quiesce_argument);

/** Start CAL-1 and CAL-2. Requires DISABLED, INHIBIT confirmed and a still shaft. */
int foc_board_calibration_start(struct foc_board_calibration *calibration, uint32_t now_us);

/** Advance the state machine. Foreground only, every pass. */
void foc_board_calibration_poll(struct foc_board_calibration *calibration, uint32_t now_us);

/** Request an abort; the drive stops at the next poll. */
void foc_board_calibration_abort(struct foc_board_calibration *calibration);

/** True while a run owns the Runtime (between start and DONE/FAILED). */
bool foc_board_calibration_busy(const struct foc_board_calibration *calibration);

/** UART adapter callbacks bound to this manager. */
struct foc_serial_calibration foc_board_calibration_serial(
    struct foc_board_calibration *calibration);

/* ---------------------------------------- end of file ----------------------------------------- */
