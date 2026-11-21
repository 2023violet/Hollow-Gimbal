/**
 * @file    foc_board_boot.h
 * @brief   Locked Board1 startup selection of nominal or reviewed calibrated configuration.
 * @version v0.1.0
 * @date    2026.11.21
 *
 * Design overview
 * ---------------
 * A build-time, read-only supply may carry the existing calibration admission
 * candidates and their accepted snapshot. Startup re-runs the admission with
 * Board-owned identity and independently configured expectations, applies the
 * Board qualification window, and only then calls the explicit calibrated Board
 * initialization. Any rejection discards the whole candidate and initializes the
 * locked nominal configuration from scratch. The effective configuration and
 * the calibration input disposition are reported separately.
 *
 * Usage example
 * -------------
 * Zero the Runtime, output journal and encoder, call foc_board_boot_select()
 * after the output lock and before any producer or IRQ starts, and start the
 * producers only when it returns FOC_RUNTIME_OK.
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * Startup context only. The report is written once before producers start and
 * is immutable afterwards, so Foreground readers need no synchronization.
 *
 * Failure / recovery
 * ------------------
 * When neither configuration initializes, the report has no effective
 * configuration, runtime_ready stays false and the caller must stay locked.
 * Accepting a parameter set never qualifies physical current measurement.
 */
#pragma once

#include "foc_board.h"
#include "foc_board_capture.h"
#include "foc_serial.h"

/* Public Types ---------------------------------------------------------------- */

/** Supply record layout identifier ("FBS1"). */
#define FOC_BOARD_CALIBRATION_SUPPLY_FORMAT 0x31534246U

/** Encoder coordinate in which the supplied rotor and offset candidates were fitted. */
enum foc_board_encoder_coordinate {
    /** Provenance did not state the coordinate. Always rejected. */
    FOC_BOARD_COORDINATE_UNKNOWN = 0,
    /** Integer raw encoder code with no linearization, as used by Board1 today. */
    FOC_BOARD_COORDINATE_RAW_NO_LUT = 1,
    /** LUT-corrected encoder coordinate. Board1 has no LUT, so always rejected. */
    FOC_BOARD_COORDINATE_LUT_CORRECTED = 2,
};

/**
 * Build-time read-only calibration supply.
 *
 * The record carries the reviewed admission inputs (candidates and limits) and
 * the admission output. Neither accepted=true nor the snapshot alone is trusted:
 * Board startup recomputes the snapshot from the candidates.
 */
struct foc_board_calibration_supply {
    /** Must equal FOC_BOARD_CALIBRATION_SUPPLY_FORMAT. */
    uint32_t format;
    /** Declared supply identity, compared with an independent build expectation. */
    uint32_t supply_id;
    /** Reference to the review material that produced the candidates. */
    uint32_t review_id;
    /** Encoder coordinate of the rotor and offset fit. */
    enum foc_board_encoder_coordinate coordinate;
    /** Reviewed three-phase current-zero candidate. */
    struct foc_calibration_adc_result adc;
    /** Reviewed direction and pole-pair candidate. */
    struct foc_calibration_rotor_result rotor;
    /** Reviewed electrical-offset candidate. */
    struct foc_calibration_offset_result offset;
    /** Admission limits used for the review. Board qualification is applied separately. */
    struct foc_calibration_admission_limits limits;
    /** Admission output that the review recorded. */
    struct foc_calibration_snapshot snapshot;
};

/** Expectations configured independently of the supply payload. */
struct foc_board_calibration_expectation {
    /** Supply identity this build accepts; zero accepts none. */
    uint32_t supply_id;
    /** Expected physical pole-pair count of the connected motor. */
    uint16_t pole_pairs;
};

/** Configuration actually initialized for this boot. */
enum foc_board_effective_configuration {
    /** No configuration initialized; producers must not start. */
    FOC_BOARD_CONFIGURATION_NONE = 0,
    /** Complete Board1 nominal configuration. */
    FOC_BOARD_NOMINAL_CONFIGURATION = 1,
    /** Board1 configuration with an accepted calibration snapshot. */
    FOC_BOARD_CALIBRATED_CONFIGURATION = 2,
};

/** Disposition of the calibration input, independent of the effective configuration. */
enum foc_board_calibration_input {
    /** Selection has not run. */
    FOC_BOARD_CALIBRATION_UNSELECTED = 0,
    /** No supply was present in this build. */
    FOC_BOARD_CALIBRATION_NOT_AVAILABLE = 1,
    /** The supply was accepted and initialized. */
    FOC_BOARD_CALIBRATION_ACCEPTED = 2,
    /** The supply was rejected and discarded. */
    FOC_BOARD_CALIBRATION_REJECTED = 3,
};

/** First failed check of a rejected supply. */
enum foc_board_calibration_rejection {
    FOC_BOARD_REJECTION_NONE = 0,
    /** Unknown record format. */
    FOC_BOARD_REJECTION_FORMAT = 1,
    /** Supply identity was zero or differed from the build expectation. */
    FOC_BOARD_REJECTION_SUPPLY_IDENTITY = 2,
    /** Rotor/offset coordinate unknown or LUT dependent. */
    FOC_BOARD_REJECTION_COORDINATE = 3,
    /** The existing admission rejected the candidates; see admission_failure. */
    FOC_BOARD_REJECTION_ADMISSION = 4,
    /** The supplied snapshot differs from the recomputed admission output. */
    FOC_BOARD_REJECTION_SNAPSHOT_MISMATCH = 5,
    /** Snapshot source identity differs from Board facts or expected pole pairs. */
    FOC_BOARD_REJECTION_BOARD_IDENTITY = 6,
    /** A value is outside the Board qualification window. */
    FOC_BOARD_REJECTION_BOARD_RANGE = 7,
    /** Runtime direction is not source direction times calibration direction. */
    FOC_BOARD_REJECTION_DIRECTION_PRODUCT = 8,
    /** Calibrated initialization failed after qualification. */
    FOC_BOARD_REJECTION_INITIALIZATION = 9,
};

/** Board qualification window applied after admission, in raw counts and pole pairs. */
enum foc_board_calibration_window {
    FOC_BOARD_OFFSET_MIN_COUNTS = 1536,
    FOC_BOARD_OFFSET_MAX_COUNTS = 2560,
    FOC_BOARD_POLE_PAIRS_MAX = 64,
};

/** Write-once startup report. */
struct foc_board_boot {
    enum foc_board_effective_configuration configuration;
    enum foc_board_calibration_input calibration;
    enum foc_board_calibration_rejection rejection;
    /** Detail of an admission rejection. */
    enum foc_calibration_admission_failure admission_failure;
    /** Status of the calibrated and nominal initialization attempts. */
    int calibrated_status;
    int nominal_status;
    /** Identities of the examined supply, zero when absent. */
    uint32_t supply_id;
    uint32_t review_id;
    /** Accepted snapshot, valid only for CALIBRATED_CONFIGURATION. */
    struct foc_calibration_snapshot snapshot;
    /** Bound live instances after a successful initialization. */
    const struct foc_runtime *runtime;
    const struct foc_board_encoder *encoder;
    /** True only after a complete initialization committed. */
    bool runtime_ready;
};

/** Startup initialization steps; replaced only by Host fault-injection tests. */
struct foc_board_boot_ops {
    int (*calibrated)(struct foc_runtime *runtime,
                      struct foc_board_output *output,
                      struct foc_board_encoder *encoder,
                      const struct foc_calibration_snapshot *snapshot,
                      uint32_t now_us);
    int (*nominal)(struct foc_runtime *runtime, struct foc_board_output *output, uint32_t now_us);
};

/** Effective values read back from the live instances. */
struct foc_board_boot_view {
    enum foc_board_effective_configuration configuration;
    enum foc_board_calibration_input calibration;
    enum foc_board_calibration_rejection rejection;
    enum foc_calibration_admission_failure admission_failure;
    uint32_t supply_id;
    uint32_t review_id;
    float phase_a_offset_count;
    float phase_b_offset_count;
    float phase_c_offset_count;
    uint16_t pole_pairs;
    float electrical_offset_rad;
    uint32_t encoder_counts_per_revolution;
    /** Raw-code to Runtime mechanical direction actually selected, +1 or -1. */
    int32_t runtime_encoder_direction;
    /** Board raw source direction, +1 or -1. */
    int32_t source_encoder_direction;
    /** Calibration direction for CALIBRATED_CONFIGURATION, otherwise 0. */
    int32_t calibration_direction;
    /** Physical current measurement qualification. Always false in this image. */
    bool current_measurement_qualified;
};

/* Public Functions ------------------------------------------------------------- */

/** Build-time supply. NULL when the build carries no calibration input. */
const struct foc_board_calibration_supply *foc_board_calibration_supply(void);

/** Build-time expectations, independent of the supply payload. */
struct foc_board_calibration_expectation foc_board_calibration_expectation(void);

/**
 * Select and initialize the boot configuration before any producer starts.
 *
 * @param boot Report, overwritten.
 * @param runtime Zero or uninitialized Runtime, written only by a successful init.
 * @param output Output journal bound to the Runtime.
 * @param encoder Fresh encoder state; a previously initialized one is refused.
 * @param supply Optional read-only supply.
 * @param expected Independent expectations, required when supply is non-NULL.
 * @param now_us Common epoch passed to the Runtime.
 * @return FOC_RUNTIME_OK when a configuration is ready, otherwise an error.
 */
int foc_board_boot_select(struct foc_board_boot *boot,
                          struct foc_runtime *runtime,
                          struct foc_board_output *output,
                          struct foc_board_encoder *encoder,
                          const struct foc_board_calibration_supply *supply,
                          const struct foc_board_calibration_expectation *expected,
                          uint32_t now_us);

/** As foc_board_boot_select() with explicit initialization steps. */
int foc_board_boot_select_with(struct foc_board_boot *boot,
                               struct foc_runtime *runtime,
                               struct foc_board_output *output,
                               struct foc_board_encoder *encoder,
                               const struct foc_board_calibration_supply *supply,
                               const struct foc_board_calibration_expectation *expected,
                               uint32_t now_us,
                               const struct foc_board_boot_ops *ops);

/** Check a supply without initializing anything. Returns the rejection code. */
enum foc_board_calibration_rejection foc_board_boot_qualify(
    const struct foc_board_calibration_supply *supply,
    const struct foc_board_calibration_expectation *expected,
    struct foc_calibration_snapshot *snapshot,
    enum foc_calibration_admission_failure *admission_failure);

/** Read the effective values from the bound live instances. False before ready. */
bool foc_board_boot_read(const struct foc_board_boot *boot, struct foc_board_boot_view *view);

/** Acquisition metadata of the effective configuration. False before ready. */
bool foc_board_boot_capture_profile(const struct foc_board_boot *boot,
                                    uint32_t build_tag,
                                    struct foc_capture_profile *profile);

/** foc_serial_boot_reader adapter; context is a const struct foc_board_boot. */
bool foc_board_boot_serial_readback(const void *boot, struct foc_serial_boot_readback *readback);

/* ---------------------------------------- end of file ----------------------------------------- */
