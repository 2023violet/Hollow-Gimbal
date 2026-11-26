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
#include "foc_board_boot.h"
#include "foc_calibration.h"
#include "foc_flux_id.h"
#include "foc_mechanical_id.h"
#include "foc_board_cogging.h"
#include "foc_board_encoder_lut.h"
#include "foc_motor_id.h"
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
    /* M3b electrical stages (appended). */
    FOC_BOARD_CAL_RL_D = 11,
    FOC_BOARD_CAL_RL_Q = 12,
    FOC_BOARD_CAL_FLUX = 13,
    FOC_BOARD_CAL_TUNE = 14,
    /** Hand the Runtime back in CURRENT mode after the electrical stages. */
    FOC_BOARD_CAL_RESTORE = 15,
    /** CAL-7 inertia and damping identification (P1, appended). */
    FOC_BOARD_CAL_INERTIA = 16,
    /** Bridge off after the electrical stages: flux and angle error from the coasting EMF. */
    FOC_BOARD_CAL_COAST = 17,
    /** CAL-8 cogging (appended): POSITION mode, ARM and START at a hold command, the slow
     * sweep, then the fit once the drive is stopped. */
    FOC_BOARD_CAL_COGGING_MODE = 18,
    FOC_BOARD_CAL_COGGING_ARM = 19,
    FOC_BOARD_CAL_COGGING_START = 20,
    FOC_BOARD_CAL_COGGING = 21,
    FOC_BOARD_CAL_COGGING_STOP = 22,
    /** CAL-9 encoder linearization (appended): CALIBRATION mode, ARM, START, the hold
     * current ramp, the field sweep, then the fit once the drive is stopped. */
    FOC_BOARD_CAL_ENCODER_MODE = 23,
    FOC_BOARD_CAL_ENCODER_ARM = 24,
    FOC_BOARD_CAL_ENCODER_START = 25,
    FOC_BOARD_CAL_ENCODER_ALIGN = 26,
    FOC_BOARD_CAL_ENCODER = 27,
    FOC_BOARD_CAL_ENCODER_STOP = 28,
};

/** One coast sample: encoder position and its time, phase-voltage time and pair voltage. */
struct foc_board_cal_coast_point {
    float mechanical_rad;
    uint32_t sensor_us;
    uint32_t sample_us;
    float pair_v;
    uint8_t high;
    uint8_t low;
    bool usable;
};

/** Coast check outcome (PARAM_READ ID 91). */
enum foc_board_cal_coast {
    /** Not run: no electrical stage, or no phase-voltage reader attached. */
    FOC_BOARD_CAL_COAST_NOT_RUN = 0,
    /** Measured; its flux replaced the driven CAL-4 flux. */
    FOC_BOARD_CAL_COAST_USED = 1,
    /** Too few usable samples (the rotor was too slow or stopped too soon). */
    FOC_BOARD_CAL_COAST_TOO_FEW = 2,
    /** The fit did not describe the samples (residual, or flux out of range). */
    FOC_BOARD_CAL_COAST_INCONSISTENT = 3,
};

/**
 * Optional phase-voltage reader for the coast check: three terminal voltages in ADC counts
 * in logical phase order A, B, C (the order of the currents and PWM channels) and their
 * sample time in monotonic microseconds. Foreground only, while the bridge is off.
 */
typedef bool (*foc_board_phase_voltage_read)(void *argument,
                                             uint16_t counts[3],
                                             uint32_t *sample_us);

/** CALIBRATE_START stage mask. */
enum foc_board_cal_stages {
    /** CAL-1 ADC zero and CAL-2 rotor mapping, applied by re-initialization. */
    FOC_BOARD_CAL_STAGE_ROTOR = 1,
    /** CAL-3 R/L, CAL-4 flux/Kt and CAL-5 current PI design. Needs an applied rotor result. */
    FOC_BOARD_CAL_STAGE_ELECTRICAL = 2,
    FOC_BOARD_CAL_STAGE_ALL = 3,
    /** CAL-1 only: measure the ADC zero and apply it to the valid group (no drive). */
    FOC_BOARD_CAL_STAGE_ZERO = 4,
    /** CAL-7 inertia and damping; combines with ROTOR and ELECTRICAL. Without
     * ELECTRICAL it needs Rs and flux in the profile. The shaft turns within ID 109. */
    FOC_BOARD_CAL_STAGE_MECHANICAL = 8,
    /** CAL-8 cogging table, alone. Needs a valid group, this boot's ADC zero and working
     * position control; the rotor turns about 2.3 turns forward and back (IDs 150, 151). */
    FOC_BOARD_CAL_STAGE_COGGING = 16,
    /** CAL-9 encoder correction, alone, with the same preconditions; the field drags the
     * rotor about 2.3 turns forward and back at 1 rad/s (ID 157 switches the result). */
    FOC_BOARD_CAL_STAGE_ENCODER = 32,
};

/** CALIBRATE_READ byte 73: where the active rotor calibration came from. */
enum foc_board_cal_source {
    FOC_BOARD_CAL_SOURCE_BOOT = 0,
    FOC_BOARD_CAL_SOURCE_RUN = 1,
    FOC_BOARD_CAL_SOURCE_STORAGE = 2,
    /** COMMUTATION_WRITE from the Host. */
    FOC_BOARD_CAL_SOURCE_HOST = 3,
    /** The build's accepted calibration supply. */
    FOC_BOARD_CAL_SOURCE_SUPPLY = 4,
};

/** PARAM_READ ID 90: the ADC zero of this boot. */
enum foc_board_cal_zero_state {
    FOC_BOARD_CAL_ZERO_PENDING = 0,
    FOC_BOARD_CAL_ZERO_READY = 1,
    FOC_BOARD_CAL_ZERO_REFUSED = 2,
};

/** CALIBRATE_READ byte 72: results the latest run produced (present, not applied). */
enum foc_board_cal_result_bits {
    FOC_BOARD_CAL_RESULT_ROTOR = 1,
    FOC_BOARD_CAL_RESULT_RL = 2,
    FOC_BOARD_CAL_RESULT_FLUX = 4,
    FOC_BOARD_CAL_RESULT_PI = 8,
    FOC_BOARD_CAL_RESULT_INERTIA = 16,
    /** CAL-8 fitted a table (ID 152 VALID). */
    FOC_BOARD_CAL_RESULT_COGGING = 32,
    /** CAL-9 fitted an encoder correction (ID 158 VALID). */
    FOC_BOARD_CAL_RESULT_ENCODER = 64,
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
    /** CAL-3 R/L fit failed or the d/q models disagree (appended). */
    FOC_BOARD_CAL_FAILURE_RL = 13,
    /** CAL-4 flux fit failed. */
    FOC_BOARD_CAL_FAILURE_FLUX = 14,
    /** CAL-5 PI design was refused or its tuning was not applied. */
    FOC_BOARD_CAL_FAILURE_TUNE = 15,
    /** H0 records were lost or not contiguous. */
    FOC_BOARD_CAL_FAILURE_CAPTURE = 16,
    /** CAL-7 inertia/damping fit failed; see admission_failure (appended). */
    FOC_BOARD_CAL_FAILURE_INERTIA = 17,
    /** CAL-8 fit refused; ID 152 says why (appended). */
    FOC_BOARD_CAL_FAILURE_COGGING = 18,
    /** CAL-9 fit refused; ID 158 says why (appended). */
    FOC_BOARD_CAL_FAILURE_ENCODER = 19,
};

/** One H0 cycle as seen by the electrical stages (logical slot clock). */
struct foc_board_cal_record {
    uint32_t slot;
    struct foc_vector_dq current_a;
    struct foc_vector_dq voltage_v;
    float mechanical_rad_s;
    /** Continuous rotor position: the CAL-3 q-axis back-EMF regressor. */
    float mechanical_rad;
    /** ADC aperture time of the current, monotonic microseconds. */
    uint32_t aperture_us;
    /** Capture time of mechanical_rad (the encoder runs at the H1 rate). */
    uint32_t sensor_us;
    bool accepted;
};

#define FOC_BOARD_CAL_RING          64U

/** CAL-3 records waiting for the encoder sample after their aperture. */
#define FOC_BOARD_CAL_RL_PENDING    8U

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
    /* CAL-2: raw sweep count (the samples and estimators are in the work area below). */
    uint32_t sweep_count;
    /* Results of the latest run and the applied snapshot. */
    struct foc_calibration_adc_result adc_result;
    struct foc_calibration_rotor_result rotor_result;
    struct foc_calibration_offset_result offset_result;
    struct foc_calibration_snapshot snapshot;
    uint32_t applied_runs;
    /* M3b. */
    unsigned int stages;
    bool electrical_phase;
    float align_voltage_v;
    atomic_uint record_active;
    atomic_uint ring_head;
    atomic_uint ring_tail;
    atomic_uint ring_overflow;
    uint32_t history_count;
    /* CAL-3: the rotor angle interpolated to each current aperture between the two
     * latest encoder samples; records wait for the sample after their aperture. */
    uint32_t rl_pending_count;
    uint32_t rl_points;
    uint32_t rl_point_us[2];
    float rl_point_rad[2];
    uint32_t rl_last_slot;
    bool first_pushed;
    bool capturing;
    uint32_t pushed;
    uint32_t level;
    uint32_t capture_serial;
    uint32_t tuning_sequence;
    struct foc_motor_id_result rl_d;
    struct foc_motor_id_result rl_q;
    struct foc_motor_rl_model rl_model;
    struct foc_flux_id_result flux_result;
    struct foc_motor_pi_candidate pi;
    /* Coast check: phase-voltage reader, least-squares sums and result. */
    foc_board_phase_voltage_read phase_read;
    void *phase_argument;
    double coast_qq;
    double coast_qd;
    double coast_dd;
    double coast_qv;
    double coast_dv;
    double coast_vv;
    uint32_t coast_samples;
    uint32_t coast_last_us;
    struct foc_board_cal_coast_point coast_points[2];
    uint32_t coast_points_count;
    float coast_start_speed;
    uint8_t coast_status;
    float coast_flux_wb;
    float coast_angle_error_rad;
    float driven_flux_wb;
    /* M3c: CAL-3 probes the drive voltage when no alignment ran in this boot. */
    bool probing;
    float probe_current_a;
    /* CAL-4: latest rotor angle and its capture time, and the steady check (mean speeds of
     * the last two check intervals) that starts each level's capture. */
    float flux_angle;
    uint32_t flux_angle_us;
    float flux_check_angle;
    uint32_t flux_check_us;
    float flux_check_speed[2];
    uint32_t flux_checks;
    uint32_t flux_samples;
    /* M3-R transaction report. mode_claimed: this run put the Runtime in CALIBRATION
     * mode; failing: RESTORE returns it to CURRENT on the way to FAILED. */
    bool mode_claimed;
    bool failing;
    uint8_t committed;
    uint8_t active_source;
    uint8_t ring_high_water;
    /* Step 3: ADC zero of this boot, the automatic zero run and the boot configuration
     * that CONFIG_ERASE returns to. */
    bool zero_ready;
    bool zero_refused;
    bool zero_pending;
    bool zero_attempted;
    uint32_t zero_attempt_us;
    /* After a re-initialization the encoder speed settles before the zero is measured. */
    bool zero_settling;
    uint32_t zero_settle_us;
    bool boot_calibrated;
    struct foc_calibration_snapshot boot_snapshot;
    struct foc_runtime_tuning boot_tuning;
    /* P1 CAL-7: bang-bang q voltage around the start position. */
    struct foc_mechanical_id_result mech_result;
    float inertia_origin_rad;
    float inertia_voltage_v;
    float inertia_direction;
    uint32_t inertia_reversals;
    /* Probe: the drive runs uncaptured until the reversal period sets the window. */
    bool inertia_probing;
    uint32_t inertia_reversal_us;
    uint32_t inertia_window;
    /* CAL-8: the acquisition the Runtime's cogging table lives in (NULL: not supported),
     * the hold position and the sweep start. */
    struct foc_board_cogging *cogging;
    float cogging_origin_rad;
    uint32_t cogging_start_us;
    /* CAL-9: the correction table (NULL: not supported); the sweep borrows the CAL-8 bins. */
    struct foc_board_encoder_lut *encoder_lut;
    /*
     * Work area: the stages of one run use it one after another, never together, so their
     * scratch shares storage. Each stage initializes its part when it begins and leaves
     * nothing in it that a later stage or a readback needs (results live above).
     * - CAL-2 (ALIGN to APPLY): the raw sweep and the rotor and offset estimators.
     * - CAL-3/4/7 (RL_D to INERTIA, COAST): the H0 record ring, written by H0 only while
     *   record_active is set, and the R/L, flux and inertia contexts.
     * - CAL-8/9 (alone): the sweep bins, lent to the attached acquisition (cogging->bins).
     * The ADC estimator stays outside: the idle automatic zero uses it from H0.
     */
    union {
        struct {
            struct foc_calibration_angle_sample sweep[FOC_BOARD_CAL_SWEEP_SAMPLES];
            struct foc_calibration_rotor_context rotor_estimator;
            struct foc_calibration_offset_context offset_estimator;
        };
        struct {
            struct foc_board_cal_record ring[FOC_BOARD_CAL_RING];
            struct foc_board_cal_record history[2];
            struct foc_board_cal_record rl_pending[FOC_BOARD_CAL_RL_PENDING];
            struct foc_motor_id_context motor_id;
            struct foc_flux_id flux_id;
            struct foc_mechanical_id_context mech_id;
        };
        struct foc_board_cal_bins bins;
    };
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

/** Start the stages in the mask (FOC_BOARD_CAL_STAGE_*). Requires DISABLED, INHIBIT
 * confirmed and a still shaft; the electrical stages alone need a rotor result applied
 * in this boot, by a run or by boot restore. */
int foc_board_calibration_start(struct foc_board_calibration *calibration,
                                uint32_t now_us,
                                unsigned int stages);

/** Adopt a calibration applied at boot from storage (electrical stages, later saves). */
void foc_board_calibration_adopt(struct foc_board_calibration *calibration,
                                 const struct foc_calibration_snapshot *snapshot);

/** Record the boot configuration and tuning CONFIG_ERASE returns to. Call after bind and
 * before a stored record is restored; an accepted supply becomes the SUPPLY group. */
/** Attach the coast check's phase-voltage reader, before producers start. NULL detaches. */
void foc_board_calibration_attach_phase_voltage(struct foc_board_calibration *calibration,
                                                foc_board_phase_voltage_read read,
                                                void *argument);

void foc_board_calibration_capture_boot(struct foc_board_calibration *calibration,
                                        const struct foc_board_boot *boot);

/** Attach the CAL-8 acquisition whose table the Runtime uses, before producers start. */
void foc_board_calibration_attach_cogging(struct foc_board_calibration *calibration,
                                          struct foc_board_cogging *cogging);

/** Attach the CAL-9 table the board encoder uses (needs the CAL-8 bins), before producers
 * start. While idle the manager applies ID 157 changes by re-initialization. */
void foc_board_calibration_attach_encoder_lut(struct foc_board_calibration *calibration,
                                              struct foc_board_encoder_lut *encoder_lut);

/** Write a commutation group (pole pairs 1-64, direction +1/-1, offset in [0, 2π)) with
 * the active ADC zero, by re-initialization. Returns a FOC_PROTO_* result. */
int foc_board_calibration_commutation(struct foc_board_calibration *calibration,
                                      uint32_t now_us,
                                      uint16_t pole_pairs,
                                      int8_t direction,
                                      float offset_rad);

/** Return the instance to the boot configuration and tuning (after CONFIG_ERASE). */
int foc_board_calibration_reset(struct foc_board_calibration *calibration, uint32_t now_us);

/** ARM/START admission: OK with a valid group and this boot's ADC zero, NOT_CALIBRATED
 * without a valid group, BUSY while the zero is pending or a run owns the Runtime. */
int foc_board_calibration_gate(const struct foc_board_calibration *calibration);

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
