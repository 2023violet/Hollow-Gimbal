/**
 * @file    foc_virtual_calibration_reuse.h
 * @brief   Host-only identities and validity checks for reviewed calibration results.
 * @version v0.1.0
 * @date    2026.11.20
 *
 * Design overview
 * ---------------
 * Store a reviewed origin and dependency revisions for each Host parameter.
 *
 * Usage example
 * -------------
 * Bind new observations in dependency order, then evaluate the history against
 * the proposed device and operating point before copying retained records.
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * The Host Foreground owner serializes history operations and owns the values.
 *
 * Failure / recovery
 * ------------------
 * A missing or invalid record is ineligible for reuse. Capture and review a
 * replacement under the current prerequisites.
 */

#pragma once

/* Includes --------------------------------------------------------------------- */

#include <stdbool.h>
#include <stdint.h>

/* Public Types ---------------------------------------------------------------- */

enum foc_cal_reuse_parameter {
    FOC_CAL_REUSE_ADC,
    FOC_CAL_REUSE_RS,
    FOC_CAL_REUSE_RL,
    FOC_CAL_REUSE_LUT,
    FOC_CAL_REUSE_POLES,
    FOC_CAL_REUSE_OFFSET,
    FOC_CAL_REUSE_HOME,
    FOC_CAL_REUSE_FLUX,
    FOC_CAL_REUSE_JB,
    FOC_CAL_REUSE_CURRENT_PI,
    FOC_CAL_REUSE_SERVO,
    FOC_CAL_REUSE_COGGING,
    FOC_CAL_REUSE_LIMITS,
    FOC_CAL_REUSE_COUNT,
};

#define FOC_CAL_REUSE_BIT(parameter) (UINT32_C(1) << (parameter))

enum foc_cal_reuse_identity {
    FOC_CAL_REUSE_ID_MOTOR,
    FOC_CAL_REUSE_ID_ENCODER,
    FOC_CAL_REUSE_ID_ADC_CHAIN,
    FOC_CAL_REUSE_ID_PHASE,
    FOC_CAL_REUSE_ID_ASSEMBLY,
    FOC_CAL_REUSE_ID_AXIS_LOAD,
    FOC_CAL_REUSE_ID_MECH_FRAME,
    FOC_CAL_REUSE_ID_VOLTAGE_SOURCE,
    FOC_CAL_REUSE_ID_ANGLE_REFERENCE,
    FOC_CAL_REUSE_ID_TORQUE_SOURCE,
    FOC_CAL_REUSE_ID_TIMING,
    FOC_CAL_REUSE_ID_CONTROL_LIMITS,
    FOC_CAL_REUSE_ID_OFFSET_REFERENCE,
    FOC_CAL_REUSE_ID_LUT_REFERENCE,
    FOC_CAL_REUSE_ID_KT_REFERENCE,
    FOC_CAL_REUSE_ID_COUNT,
};

enum foc_cal_reuse_source {
    FOC_CAL_REUSE_SOURCE_UNKNOWN,
    FOC_CAL_REUSE_SOURCE_HOST_VIRTUAL,
    FOC_CAL_REUSE_SOURCE_EXTERNAL_RECORDED,
    FOC_CAL_REUSE_SOURCE_INSTRUMENT,
    FOC_CAL_REUSE_SOURCE_MANUFACTURER,
    FOC_CAL_REUSE_SOURCE_DERIVED,
};

enum foc_cal_reuse_dimension {
    FOC_CAL_REUSE_RANGE_TEMP,
    FOC_CAL_REUSE_RANGE_SPEED,
    FOC_CAL_REUSE_RANGE_CURRENT,
    FOC_CAL_REUSE_RANGE_FREQUENCY,
    FOC_CAL_REUSE_RANGE_LOAD,
    FOC_CAL_REUSE_RANGE_COUNT,
};

enum foc_cal_reuse_reason {
    FOC_CAL_REUSE_REASON_VALID,
    FOC_CAL_REUSE_REASON_MISSING,
    FOC_CAL_REUSE_REASON_UNREVIEWED,
    FOC_CAL_REUSE_REASON_PROVENANCE,
    FOC_CAL_REUSE_REASON_RANGE_INVALID,
    FOC_CAL_REUSE_REASON_OUTSIDE_RANGE,
    FOC_CAL_REUSE_REASON_DEPENDENCY_MISSING,
    FOC_CAL_REUSE_REASON_DEPENDENCY_CHANGED,
    FOC_CAL_REUSE_REASON_DEPENDENCY_INVALID,
    FOC_CAL_REUSE_REASON_IDENTITY_FIRST,
    FOC_CAL_REUSE_REASON_IDENTITY_LAST =
        FOC_CAL_REUSE_REASON_IDENTITY_FIRST + FOC_CAL_REUSE_ID_COUNT - 1,
};

/** Nonzero identities name the actual assemblies, configurations and sources. */
struct foc_cal_reuse_setup {
    uint32_t id[FOC_CAL_REUSE_ID_COUNT];
    float temperature_c;
    float speed_rad_s;
    float current_a;
    float frequency_hz;
    float load_nm;
};

struct foc_cal_reuse_range {
    float min;
    float max;
};

/** A reviewed measurement or derived value already owned by the caller.
 * `uses_mapped_angle` means this observation used the installed Encoder LUT.
 * `uses_kt` identifies a J/B observation using Kt*Iq; other J/B observations use
 * equivalent measured torque. `uses_rl` identifies R/L-compensated flux.
 */
struct foc_cal_reuse_record {
    uint32_t session;
    uint32_t capture;
    uint32_t revision;
    enum foc_cal_reuse_source source;
    bool reviewed;
    bool uses_adc;
    bool uses_mapped_angle;
    bool uses_kt;
    bool uses_rl;
    uint32_t range_mask;
    struct foc_cal_reuse_range range[FOC_CAL_REUSE_RANGE_COUNT];
    uint32_t dependency[FOC_CAL_REUSE_COUNT];
};

struct foc_cal_reuse_history {
    struct foc_cal_reuse_setup setup;
    struct foc_cal_reuse_record records[FOC_CAL_REUSE_COUNT];
};

/* Public Functions ------------------------------------------------------------- */

/** Bind fresh reviewed evidence to valid dependencies in this setup.
 * A replacement requires an increasing revision for that parameter. Input
 * dependency slots must be zero. Check an existing dependency record with
 * evaluate when reusing it. A rebind would discard its original dependencies.
 * A selected range is inclusive, including a range with equal endpoints.
 * For a relevant dimension without a range, reuse is limited to the original
 * operating point. The caller must provide the measured or authorized point.
 */
bool foc_cal_reuse_bind_record(struct foc_cal_reuse_history *history,
                               enum foc_cal_reuse_parameter parameter,
                               const struct foc_cal_reuse_record *record);

/** Return the reusable parameter mask and optionally a reason for every entry. */
uint32_t foc_cal_reuse_evaluate(const struct foc_cal_reuse_history *history,
                                const struct foc_cal_reuse_setup *next,
                                enum foc_cal_reuse_reason reasons[FOC_CAL_REUSE_COUNT]);

const char *foc_cal_reuse_parameter_name(enum foc_cal_reuse_parameter parameter);

const char *foc_cal_reuse_reason_name(enum foc_cal_reuse_reason reason);

/* ---------------------------------------- end of file ----------------------------------------- */
