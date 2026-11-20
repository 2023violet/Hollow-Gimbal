/**
 * @file foc_parameter.c
 * @brief Foreground facade over immutable settings, Runtime requests and snapshots.
 * @version 0.1
 * @date 2026-11-18
 *
 * Implementation role
 * -------------------
 * Keep discovery and staged configuration in Foreground and use Runtime
 * publication and observation for command, lifecycle and output decisions.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_parameter.h"

#include <float.h>
#include <math.h>
#include <string.h>

/* Private Constants ------------------------------------------------------------ */

#define FLOAT_RANGE(lo, hi, positive) {{.f32 = (lo)}, {.f32 = (hi)}, (positive)}
#define UNSIGNED_RANGE(lo, hi)        {{.u32 = (lo)}, {.u32 = (hi)}, false}
#define SIGNED_RANGE(lo, hi)          {{.i32 = (lo)}, {.i32 = (hi)}, false}
#define DESCRIPTOR(key, label, scalar, unit, group, access, bounds, dynamic, text) \
    {FOC_PARAMETER_##key,                                                          \
     label,                                                                        \
     FOC_PARAMETER_##scalar,                                                       \
     unit,                                                                         \
     FOC_PARAMETER_##group,                                                        \
     FOC_PARAMETER_##access,                                                       \
     bounds,                                                                       \
     dynamic,                                                                      \
     FOC_PARAMETER_##access == FOC_PARAMETER_INITIAL_CONFIG,                       \
     FOC_PARAMETER_##access == FOC_PARAMETER_INITIAL_CONFIG,                       \
     text}

static const struct foc_parameter_descriptor catalog[] = {
    DESCRIPTOR(
        POLE_PAIRS,
        "motor.pole_pairs",
        U32,
        "1",
        CONFIG,
        READ_ONLY,
        UNSIGNED_RANGE(1U, UINT32_MAX),
        false,
        "Initial rotor mapping. Requires qualified calibration."
    ),
    DESCRIPTOR(
        ELECTRICAL_OFFSET,
        "motor.electrical_offset",
        F32,
        "rad",
        CONFIG,
        READ_ONLY,
        FLOAT_RANGE(-FLT_MAX, FLT_MAX, false),
        false,
        "Initial electrical angle offset."
    ),
    DESCRIPTOR(
        KT,
        "motor.kt",
        F32,
        "N*m/A",
        CONFIG,
        READ_ONLY,
        FLOAT_RANGE(0.0f, FLT_MAX, true),
        false,
        "Caller-qualified torque constant for IMPEDANCE."
    ),
    DESCRIPTOR(
        IMPEDANCE_FRAME,
        "servo.mechanical_frame",
        U32,
        "1",
        CONFIG,
        READ_ONLY,
        UNSIGNED_RANGE(1U, UINT32_MAX),
        false,
        "Qualified nonzero IMPEDANCE shaft frame identity."
    ),
    DESCRIPTOR(
        MOTION_IQ_LIMIT,
        "motion.iq_limit",
        F32,
        "A",
        CONFIG,
        INITIAL_CONFIG,
        FLOAT_RANGE(0.0f, FLT_MAX, true),
        true,
        "Detached live candidate may only lower this cap; initial candidates use the Pipeline "
        "limit."
    ),
    DESCRIPTOR(
        TORQUE_LIMIT,
        "servo.torque_limit",
        F32,
        "N*m",
        CONFIG,
        INITIAL_CONFIG,
        FLOAT_RANGE(0.0f, FLT_MAX, true),
        false,
        "Symmetric IMPEDANCE torque cap."
    ),
    DESCRIPTOR(
        MAX_STIFFNESS,
        "servo.max_stiffness",
        F32,
        "N*m/rad",
        CONFIG,
        INITIAL_CONFIG,
        FLOAT_RANGE(0.0f, FLT_MAX, false),
        false,
        "IMPEDANCE command stiffness ceiling."
    ),
    DESCRIPTOR(
        MAX_DAMPING,
        "servo.max_damping",
        F32,
        "N*m*s/rad",
        CONFIG,
        INITIAL_CONFIG,
        FLOAT_RANGE(0.0f, FLT_MAX, false),
        false,
        "IMPEDANCE command damping ceiling."
    ),
    DESCRIPTOR(
        MAX_FEEDFORWARD,
        "servo.max_feedforward",
        F32,
        "N*m",
        CONFIG,
        INITIAL_CONFIG,
        FLOAT_RANGE(0.0f, FLT_MAX, false),
        false,
        "Absolute IMPEDANCE feedforward ceiling."
    ),
    DESCRIPTOR(
        POSITION_SPEED_LIMIT,
        "position.speed_limit",
        F32,
        "rad/s",
        CONFIG,
        READ_ONLY,
        FLOAT_RANGE(0.0f, FLT_MAX, true),
        false,
        "Velocity cap for the Position controller."
    ),

    DESCRIPTOR(
        LIMITS_ENABLED,
        "motion.limits_enabled",
        U32,
        "1",
        CONFIG,
        INITIAL_CONFIG,
        UNSIGNED_RANGE(0U, 1U),
        false,
        "Enable software position limits at initialization."
    ),
    DESCRIPTOR(
        LIMIT_POSITION_MIN,
        "motion.position_min",
        F32,
        "rad",
        CONFIG,
        INITIAL_CONFIG,
        FLOAT_RANGE(-FLT_MAX, FLT_MAX, false),
        false,
        "Minimum continuous mechanical position."
    ),
    DESCRIPTOR(
        LIMIT_POSITION_MAX,
        "motion.position_max",
        F32,
        "rad",
        CONFIG,
        INITIAL_CONFIG,
        FLOAT_RANGE(-FLT_MAX, FLT_MAX, false),
        false,
        "Maximum continuous mechanical position."
    ),
    DESCRIPTOR(
        LIMIT_TOLERANCE,
        "motion.limit_tolerance",
        F32,
        "rad",
        CONFIG,
        INITIAL_CONFIG,
        FLOAT_RANGE(0.0f, FLT_MAX, false),
        false,
        "Inward buffer for outward request rejection."
    ),
    DESCRIPTOR(
        LIMIT_MECHANICAL_FRAME,
        "motion.protection_frame",
        U32,
        "1",
        CONFIG,
        INITIAL_CONFIG,
        UNSIGNED_RANGE(1U, UINT32_MAX),
        false,
        "Qualified mechanical frame identity for motion protection."
    ),
    DESCRIPTOR(
        FOLLOWING_ENABLED,
        "motion.following_enabled",
        U32,
        "1",
        CONFIG,
        INITIAL_CONFIG,
        UNSIGNED_RANGE(0U, 1U),
        false,
        "Enable sustained position-error monitoring at initialization."
    ),
    DESCRIPTOR(
        FOLLOWING_THRESHOLD,
        "motion.following_threshold",
        F32,
        "rad",
        CONFIG,
        INITIAL_CONFIG,
        FLOAT_RANGE(0.0f, FLT_MAX, true),
        false,
        "Absolute effective position-reference error threshold."
    ),
    DESCRIPTOR(
        FOLLOWING_DURATION_US,
        "motion.following_duration",
        U32,
        "us",
        CONFIG,
        INITIAL_CONFIG,
        UNSIGNED_RANGE(1U, INT32_MAX),
        false,
        "Required duration of the sustained tracking error."
    ),
    DESCRIPTOR(
        FOLLOWING_MAX_GAP_US,
        "motion.following_max_gap",
        U32,
        "us",
        CONFIG,
        INITIAL_CONFIG,
        UNSIGNED_RANGE(1U, INT32_MAX),
        false,
        "Maximum observation gap within an error interval."
    ),

    DESCRIPTOR(
        TARGET_POSITION,
        "servo.target_position",
        F32,
        "rad",
        COMMAND,
        COMMAND_ONLY,
        FLOAT_RANGE(
            -FOC_RUNTIME_IMPEDANCE_POSITION_LIMIT_RAD,
            FOC_RUNTIME_IMPEDANCE_POSITION_LIMIT_RAD,
            false
        ),
        false,
        "Continuous motor-shaft reference in the configured frame. Submit a complete command."
    ),
    DESCRIPTOR(
        TARGET_VELOCITY,
        "servo.target_velocity",
        F32,
        "rad/s",
        COMMAND,
        COMMAND_ONLY,
        FLOAT_RANGE(-FLT_MAX, FLT_MAX, false),
        false,
        "Motor-shaft velocity reference. Submit a complete command."
    ),
    DESCRIPTOR(
        STIFFNESS,
        "servo.stiffness",
        F32,
        "N*m/rad",
        COMMAND,
        COMMAND_ONLY,
        FLOAT_RANGE(0.0f, FLT_MAX, false),
        true,
        "Command stiffness, bounded by servo.max_stiffness."
    ),
    DESCRIPTOR(
        DAMPING,
        "servo.damping",
        F32,
        "N*m*s/rad",
        COMMAND,
        COMMAND_ONLY,
        FLOAT_RANGE(0.0f, FLT_MAX, false),
        true,
        "Command damping, bounded by servo.max_damping."
    ),
    DESCRIPTOR(
        FEEDFORWARD,
        "servo.feedforward",
        F32,
        "N*m",
        COMMAND,
        COMMAND_ONLY,
        FLOAT_RANGE(-FLT_MAX, FLT_MAX, false),
        true,
        "Signed explicit feedforward, bounded by servo.max_feedforward."
    ),

    DESCRIPTOR(
        MODE,
        "runtime.mode",
        ENUM,
        "1",
        STATUS,
        READ_ONLY,
        UNSIGNED_RANGE(FOC_RUNTIME_MODE_CURRENT, FOC_RUNTIME_MODE_IMPEDANCE),
        false,
        "enum foc_runtime_mode."
    ),
    DESCRIPTOR(
        STATE,
        "safety.state",
        ENUM,
        "1",
        STATUS,
        READ_ONLY,
        UNSIGNED_RANGE(FOC_SAFE_STATE_BOOT, FOC_SAFE_STATE_FAULT_LATCHED),
        false,
        "enum foc_safe_state."
    ),
    DESCRIPTOR(
        FAULTS,
        "safety.faults",
        U32,
        "bits",
        STATUS,
        READ_ONLY,
        UNSIGNED_RANGE(0U, UINT32_MAX),
        false,
        "Latched and pending Runtime fault causes."
    ),
    DESCRIPTOR(
        SESSION,
        "runtime.session",
        U32,
        "1",
        STATUS,
        READ_ONLY,
        UNSIGNED_RANGE(0U, UINT32_MAX),
        false,
        "Current motion session."
    ),
    DESCRIPTOR(
        COMMAND_SEQUENCE,
        "command.observed_sequence",
        U32,
        "1",
        STATUS,
        READ_ONLY,
        UNSIGNED_RANGE(0U, UINT32_MAX),
        false,
        "Last observed command sequence. This alone does not prove UPDATE."
    ),
    DESCRIPTOR(
        COMMAND_US,
        "command.observed_source_time",
        U32,
        "us",
        STATUS,
        READ_ONLY,
        UNSIGNED_RANGE(0U, UINT32_MAX),
        false,
        "Original source time of the last observed command."
    ),
    DESCRIPTOR(
        SENSOR_US,
        "sensor.source_time",
        U32,
        "us",
        TELEMETRY,
        READ_ONLY,
        UNSIGNED_RANGE(0U, UINT32_MAX),
        false,
        "Source time of the last valid H0-consumed observation."
    ),
    DESCRIPTOR(
        OUTPUT_ACTION,
        "output.action",
        ENUM,
        "1",
        STATUS,
        READ_ONLY,
        UNSIGNED_RANGE(FOC_DEVICE_OUTPUT_INHIBIT, FOC_DEVICE_OUTPUT_ACTIVE_BRAKE),
        false,
        "enum foc_device_output_action."
    ),
    DESCRIPTOR(
        OUTPUT_CONFIRMED,
        "output.confirmed",
        U32,
        "1",
        STATUS,
        READ_ONLY,
        UNSIGNED_RANGE(0U, 1U),
        false,
        "Confirmation at the adapter's declared scope."
    ),
    DESCRIPTOR(
        UPDATES,
        "output.updates",
        U32,
        "1",
        TELEMETRY,
        READ_ONLY,
        UNSIGNED_RANGE(0U, UINT32_MAX),
        false,
        "Total accepted software UPDATEs."
    ),
    DESCRIPTOR(
        INHIBIT_PENDING,
        "output.inhibit_pending",
        U32,
        "1",
        STATUS,
        READ_ONLY,
        UNSIGNED_RANGE(0U, 1U),
        false,
        "Independent inhibit awaits Runtime acknowledgement."
    ),
    DESCRIPTOR(
        SENSOR_VALID,
        "sensor.valid",
        U32,
        "1",
        STATUS,
        READ_ONLY,
        UNSIGNED_RANGE(0U, 1U),
        false,
        "Mechanical observation valid at snapshot publication. Check source age for use."
    ),
    DESCRIPTOR(
        MECHANICAL_POSITION,
        "sensor.position",
        F32,
        "rad",
        TELEMETRY,
        READ_ONLY,
        FLOAT_RANGE(-FLT_MAX, FLT_MAX, false),
        false,
        "Continuous H0-consumed mechanical position."
    ),
    DESCRIPTOR(
        MECHANICAL_VELOCITY,
        "sensor.velocity",
        F32,
        "rad/s",
        TELEMETRY,
        READ_ONLY,
        FLOAT_RANGE(-FLT_MAX, FLT_MAX, false),
        false,
        "Velocity from the same H0-consumed Sensor observation."
    ),
    DESCRIPTOR(
        PIPELINE_STATUS,
        "runtime.pipeline_status",
        I32,
        "1",
        STATUS,
        READ_ONLY,
        SIGNED_RANGE(INT32_MIN, INT32_MAX),
        false,
        "Original pipeline or lifecycle status from Runtime."
    ),

    DESCRIPTOR(
        FOLLOWING_STATE,
        "motion.following_state",
        ENUM,
        "1",
        STATUS,
        READ_ONLY,
        UNSIGNED_RANGE(FOC_FOLLOWING_INACTIVE, FOC_FOLLOWING_TIME_ORDER),
        false,
        "H0-observed following-error monitor state."
    ),
    DESCRIPTOR(
        MOTION_REJECTION,
        "motion.rejection",
        ENUM,
        "1",
        STATUS,
        READ_ONLY,
        UNSIGNED_RANGE(FOC_MOTION_REJECT_NONE, FOC_MOTION_REJECT_FOLLOWING),
        false,
        "Latest motion protection rejection published by H0."
    ),

    DESCRIPTOR(
        STOP,
        "action.stop",
        NONE,
        "1",
        ACTION,
        ACTION_ONLY,
        UNSIGNED_RANGE(0U, 0U),
        false,
        "Request independent STOP and revoke the command."
    ),
    DESCRIPTOR(
        CLEAR_TIMEOUT,
        "action.clear_command_timeout",
        NONE,
        "1",
        ACTION,
        ACTION_ONLY,
        UNSIGNED_RANGE(0U, 0U),
        false,
        "Request existing isolated command-timeout recovery."
    ),
    DESCRIPTOR(
        SET_MODE,
        "action.set_mode",
        ENUM,
        "1",
        ACTION,
        ACTION_ONLY,
        UNSIGNED_RANGE(FOC_RUNTIME_MODE_CURRENT, FOC_RUNTIME_MODE_IMPEDANCE),
        false,
        "Publish Runtime SET_MODE with its identity."
    ),
    DESCRIPTOR(
        ARM,
        "action.arm",
        NONE,
        "1",
        ACTION,
        ACTION_ONLY,
        UNSIGNED_RANGE(0U, 0U),
        false,
        "Publish Runtime ARM with its identity."
    ),
    DESCRIPTOR(
        START,
        "action.start",
        NONE,
        "1",
        ACTION,
        ACTION_ONLY,
        UNSIGNED_RANGE(0U, 0U),
        false,
        "Publish Runtime START with its identity."
    ),
};

enum {
    MAX_NAME_BYTES = 64,
    STATIC_EDITS = 14
};

/* Private Helpers ------------------------------------------------------------ */

static int runtime_status(int status)
{
    switch (status) {
    case FOC_RUNTIME_OK:
        return FOC_PARAMETER_OK;
    case FOC_RUNTIME_ERR_ORDER:
        return FOC_PARAMETER_ERR_ORDER;
    case FOC_RUNTIME_ERR_BUSY:
        return FOC_PARAMETER_ERR_BUSY;
    case FOC_RUNTIME_ERR_STOP:
        return FOC_PARAMETER_ERR_INHIBIT;
    case FOC_RUNTIME_ERR_CONFIG:
        return FOC_PARAMETER_ERR_CONFIG;
    case FOC_RUNTIME_ERR_INPUT:
        return FOC_PARAMETER_ERR_INPUT;
    case FOC_RUNTIME_ERR_MOTION_LIMIT:
        return FOC_PARAMETER_ERR_MOTION_LIMIT;
    case FOC_RUNTIME_ERR_MOTION_MODE:
        return FOC_PARAMETER_ERR_MOTION_MODE;
    default:
        return FOC_PARAMETER_ERR_STATE;
    }
}

static bool available(const struct foc_runtime_motion_config *motion, enum foc_parameter_id id)
{
    switch (id) {
    case FOC_PARAMETER_POLE_PAIRS:
    case FOC_PARAMETER_ELECTRICAL_OFFSET:
    case FOC_PARAMETER_MOTION_IQ_LIMIT:
    case FOC_PARAMETER_POSITION_SPEED_LIMIT:
        return motion->enabled;
    case FOC_PARAMETER_IMPEDANCE_FRAME:
    case FOC_PARAMETER_KT:
    case FOC_PARAMETER_TORQUE_LIMIT:
    case FOC_PARAMETER_MAX_STIFFNESS:
    case FOC_PARAMETER_MAX_DAMPING:
    case FOC_PARAMETER_MAX_FEEDFORWARD:
    case FOC_PARAMETER_TARGET_POSITION:
    case FOC_PARAMETER_TARGET_VELOCITY:
    case FOC_PARAMETER_STIFFNESS:
    case FOC_PARAMETER_DAMPING:
    case FOC_PARAMETER_FEEDFORWARD:
        return motion->enabled && motion->impedance.enabled;
    default:
        return true;
    }
}

static int range_value(
    const struct foc_parameter_descriptor *descriptor,
    const struct foc_parameter_range *range,
    const struct foc_parameter_value *value
)
{
    if (value->type != descriptor->type) {
        return FOC_PARAMETER_ERR_TYPE;
    }
    switch (value->type) {
    case FOC_PARAMETER_F32: {
        const float v = value->data.f32;
        const float minimum = range->minimum.f32;
        return isfinite(v) && (range->minimum_exclusive ? v > minimum : v >= minimum) &&
                v <= range->maximum.f32
            ? FOC_PARAMETER_OK
            : FOC_PARAMETER_ERR_RANGE;
    }
    case FOC_PARAMETER_U32:
    case FOC_PARAMETER_ENUM:
        return value->data.u32 >= range->minimum.u32 && value->data.u32 <= range->maximum.u32
            ? FOC_PARAMETER_OK
            : FOC_PARAMETER_ERR_RANGE;
    case FOC_PARAMETER_I32:
        return value->data.i32 >= range->minimum.i32 && value->data.i32 <= range->maximum.i32
            ? FOC_PARAMETER_OK
            : FOC_PARAMETER_ERR_RANGE;
    default:
        return FOC_PARAMETER_ERR_TYPE;
    }
}

static int static_config_value(
    const struct foc_runtime_config *config,
    enum foc_parameter_id id,
    union foc_parameter_scalar *value
)
{
    const struct foc_runtime_motion_config *motion = &config->motion;
    switch (id) {
    case FOC_PARAMETER_POLE_PAIRS:
        value->u32 = motion->rotor.pole_pairs;
        break;
    case FOC_PARAMETER_ELECTRICAL_OFFSET:
        value->f32 = motion->rotor.electrical_offset_rad;
        break;
    case FOC_PARAMETER_KT:
        value->f32 = motion->impedance.torque_constant_nm_per_a;
        break;
    case FOC_PARAMETER_IMPEDANCE_FRAME:
        value->u32 = motion->impedance.mechanical_frame_id;
        break;
    case FOC_PARAMETER_MOTION_IQ_LIMIT:
        value->f32 = motion->iq_limit_a;
        break;
    case FOC_PARAMETER_TORQUE_LIMIT:
        value->f32 = motion->impedance.torque_limit_nm;
        break;
    case FOC_PARAMETER_MAX_STIFFNESS:
        value->f32 = motion->impedance.max_stiffness_nm_per_rad;
        break;
    case FOC_PARAMETER_MAX_DAMPING:
        value->f32 = motion->impedance.max_damping_nm_s_per_rad;
        break;
    case FOC_PARAMETER_MAX_FEEDFORWARD:
        value->f32 = motion->impedance.max_feedforward_nm;
        break;
    case FOC_PARAMETER_POSITION_SPEED_LIMIT:
        value->f32 = motion->position.velocity_limit_rad_s;
        break;
    case FOC_PARAMETER_LIMITS_ENABLED:
        value->u32 = motion->limits.enabled;
        break;
    case FOC_PARAMETER_LIMIT_POSITION_MIN:
        value->f32 = motion->limits.position_min_rad;
        break;
    case FOC_PARAMETER_LIMIT_POSITION_MAX:
        value->f32 = motion->limits.position_max_rad;
        break;
    case FOC_PARAMETER_LIMIT_TOLERANCE:
        value->f32 = motion->limits.tolerance_rad;
        break;
    case FOC_PARAMETER_LIMIT_MECHANICAL_FRAME:
        value->u32 = motion->limits.mechanical_frame_id;
        break;
    case FOC_PARAMETER_FOLLOWING_ENABLED:
        value->u32 = motion->following_error.enabled;
        break;
    case FOC_PARAMETER_FOLLOWING_THRESHOLD:
        value->f32 = motion->following_error.threshold_rad;
        break;
    case FOC_PARAMETER_FOLLOWING_DURATION_US:
        value->u32 = motion->following_error.duration_us;
        break;
    case FOC_PARAMETER_FOLLOWING_MAX_GAP_US:
        value->u32 = motion->following_error.max_gap_us;
        break;
    default:
        return FOC_PARAMETER_ERR_ID;
    }
    return FOC_PARAMETER_OK;
}

static int snapshot_value(
    struct foc_runtime *runtime,
    enum foc_parameter_id id,
    union foc_parameter_scalar *value
)
{
    struct foc_runtime_snapshot view;
    const int status = runtime_status(foc_runtime_read_snapshot(runtime, &view));
    if (status != FOC_PARAMETER_OK) {
        return status;
    }
    if ((id == FOC_PARAMETER_COMMAND_SEQUENCE || id == FOC_PARAMETER_COMMAND_US) &&
        !view.command_seen) {
        return FOC_PARAMETER_ERR_UNAVAILABLE;
    }
    if ((id == FOC_PARAMETER_SENSOR_US || id == FOC_PARAMETER_MECHANICAL_POSITION ||
         id == FOC_PARAMETER_MECHANICAL_VELOCITY) &&
        !view.sensor_valid) {
        return FOC_PARAMETER_ERR_SENSOR;
    }
    switch (id) {
    case FOC_PARAMETER_MODE:
        value->u32 = (uint32_t)view.mode;
        break;
    case FOC_PARAMETER_STATE:
        value->u32 = (uint32_t)view.state;
        break;
    case FOC_PARAMETER_FAULTS:
        value->u32 = view.faults;
        break;
    case FOC_PARAMETER_SESSION:
        value->u32 = view.motion_session;
        break;
    case FOC_PARAMETER_COMMAND_SEQUENCE:
        value->u32 = view.command_seq;
        break;
    case FOC_PARAMETER_COMMAND_US:
        value->u32 = view.command_us;
        break;
    case FOC_PARAMETER_SENSOR_US:
        value->u32 = view.sensor_us;
        break;
    case FOC_PARAMETER_OUTPUT_ACTION:
        value->u32 = (uint32_t)view.action;
        break;
    case FOC_PARAMETER_OUTPUT_CONFIRMED:
        value->u32 = view.output_confirmed;
        break;
    case FOC_PARAMETER_UPDATES:
        value->u32 = view.updates;
        break;
    case FOC_PARAMETER_INHIBIT_PENDING:
        value->u32 = view.inhibit_pending;
        break;
    case FOC_PARAMETER_SENSOR_VALID:
        value->u32 = view.sensor_valid;
        break;
    case FOC_PARAMETER_MECHANICAL_POSITION:
        value->f32 = view.mechanical_rad;
        break;
    case FOC_PARAMETER_MECHANICAL_VELOCITY:
        value->f32 = view.mechanical_rad_s;
        break;
    case FOC_PARAMETER_PIPELINE_STATUS:
        value->i32 = view.pipeline_status;
        break;
    case FOC_PARAMETER_FOLLOWING_STATE:
        value->u32 = (uint32_t)view.following_state;
        break;
    case FOC_PARAMETER_MOTION_REJECTION:
        value->u32 = (uint32_t)view.motion_rejection;
        break;
    default:
        return FOC_PARAMETER_ERR_ID;
    }
    return FOC_PARAMETER_OK;
}

static int write_gate(struct foc_runtime *runtime, uint32_t session, bool detached)
{
    struct foc_runtime_snapshot view;
    const int status = runtime_status(foc_runtime_read_snapshot(runtime, &view));
    if (status != FOC_PARAMETER_OK) {
        return status;
    }
    if (view.faults != 0U || view.state == FOC_SAFE_STATE_FAULT_LATCHED) {
        return FOC_PARAMETER_ERR_FAULT;
    }
    if (view.motion_session != session) {
        return FOC_PARAMETER_ERR_SESSION;
    }
    if (detached && view.state != FOC_SAFE_STATE_DISABLED) {
        return FOC_PARAMETER_ERR_STATE;
    }
    if (view.inhibit_pending ||
        (detached && (!view.output_confirmed || view.action != FOC_DEVICE_OUTPUT_INHIBIT))) {
        return FOC_PARAMETER_ERR_INHIBIT;
    }
    return detached || view.mode == FOC_RUNTIME_MODE_IMPEDANCE ? FOC_PARAMETER_OK
                                                               : FOC_PARAMETER_ERR_STATE;
}

static int prepare(
    float inner_limit,
    const struct foc_runtime_config *base,
    const struct foc_parameter_edit *edits,
    size_t count,
    struct foc_parameter_candidate *candidate
)
{
    if (base == NULL || edits == NULL || candidate == NULL || count == 0U || count > STATIC_EDITS) {
        return FOC_PARAMETER_ERR_INPUT;
    }
    struct foc_parameter_candidate trial = {.config = *base};
    struct foc_runtime_motion_config *motion = &trial.config.motion;
    for (size_t i = 0U; i < count; ++i) {
        const struct foc_parameter_descriptor *descriptor = foc_parameter_find(edits[i].id);
        if (descriptor == NULL) {
            return FOC_PARAMETER_ERR_ID;
        }
        if (descriptor->access != FOC_PARAMETER_INITIAL_CONFIG) {
            return FOC_PARAMETER_ERR_READ_ONLY;
        }
        struct foc_parameter_range range = descriptor->range;
        if (descriptor->id == FOC_PARAMETER_MOTION_IQ_LIMIT) {
            range.maximum.f32 = inner_limit;
        }
        const int status = range_value(descriptor, &range, &edits[i].value);
        if (status != FOC_PARAMETER_OK) {
            return status;
        }
        if (!available(motion, descriptor->id)) {
            return FOC_PARAMETER_ERR_UNAVAILABLE;
        }
        for (size_t previous = 0U; previous < i; ++previous) {
            if (edits[previous].id == edits[i].id) {
                return FOC_PARAMETER_ERR_DUPLICATE;
            }
        }

        const union foc_parameter_scalar v = edits[i].value.data;
        switch (descriptor->id) {
        case FOC_PARAMETER_MOTION_IQ_LIMIT:
            motion->iq_limit_a = v.f32;
            break;
        case FOC_PARAMETER_TORQUE_LIMIT:
            motion->impedance.torque_limit_nm = v.f32;
            break;
        case FOC_PARAMETER_MAX_STIFFNESS:
            motion->impedance.max_stiffness_nm_per_rad = v.f32;
            break;
        case FOC_PARAMETER_MAX_DAMPING:
            motion->impedance.max_damping_nm_s_per_rad = v.f32;
            break;
        case FOC_PARAMETER_MAX_FEEDFORWARD:
            motion->impedance.max_feedforward_nm = v.f32;
            break;
        case FOC_PARAMETER_LIMITS_ENABLED:
            motion->limits.enabled = v.u32 != 0U;
            break;
        case FOC_PARAMETER_LIMIT_POSITION_MIN:
            motion->limits.position_min_rad = v.f32;
            break;
        case FOC_PARAMETER_LIMIT_POSITION_MAX:
            motion->limits.position_max_rad = v.f32;
            break;
        case FOC_PARAMETER_LIMIT_TOLERANCE:
            motion->limits.tolerance_rad = v.f32;
            break;
        case FOC_PARAMETER_LIMIT_MECHANICAL_FRAME:
            motion->limits.mechanical_frame_id = v.u32;
            break;
        case FOC_PARAMETER_FOLLOWING_ENABLED:
            motion->following_error.enabled = v.u32 != 0U;
            break;
        case FOC_PARAMETER_FOLLOWING_THRESHOLD:
            motion->following_error.threshold_rad = v.f32;
            break;
        case FOC_PARAMETER_FOLLOWING_DURATION_US:
            motion->following_error.duration_us = v.u32;
            break;
        case FOC_PARAMETER_FOLLOWING_MAX_GAP_US:
            motion->following_error.max_gap_us = v.u32;
            break;
        default:
            return FOC_PARAMETER_ERR_READ_ONLY;
        }
    }

    const struct foc_runtime_impedance_config *policy = &motion->impedance;
    const bool protection_enabled = motion->limits.enabled || motion->following_error.enabled;
    if (!foc_motion_limits_validate(&motion->limits) ||
        !foc_following_error_validate(&motion->following_error) ||
        (motion->following_error.enabled &&
         (motion->following_error.max_gap_us < trial.config.h0.period_us ||
          motion->following_error.max_gap_us < trial.config.h1.period_us)) ||
        (protection_enabled &&
         (!motion->enabled || motion->limits.mechanical_frame_id == 0U ||
          (policy->enabled &&
           policy->mechanical_frame_id != motion->limits.mechanical_frame_id))) ||
        !isfinite(inner_limit) ||
        (motion->enabled &&
         (!isfinite(motion->iq_limit_a) || motion->iq_limit_a <= 0.0f ||
          motion->iq_limit_a > inner_limit)) ||
        (policy->enabled &&
         (policy->mechanical_frame_id == 0U || !isfinite(policy->torque_constant_nm_per_a) ||
          policy->torque_constant_nm_per_a <= 0.0f ||
          !isfinite(policy->torque_constant_nm_per_a * motion->iq_limit_a) ||
          !isfinite(policy->torque_limit_nm) || policy->torque_limit_nm <= 0.0f ||
          !isfinite(policy->max_stiffness_nm_per_rad) || policy->max_stiffness_nm_per_rad < 0.0f ||
          !isfinite(policy->max_damping_nm_s_per_rad) || policy->max_damping_nm_s_per_rad < 0.0f ||
          !isfinite(policy->max_feedforward_nm) || policy->max_feedforward_nm < 0.0f ||
          !(policy->damping_filter_hz == 0.0f ||
            (policy->damping_filter_hz >= 1.0f && policy->damping_filter_hz <= 200.0f))))) {
        return FOC_PARAMETER_ERR_CONFIG;
    }
    *candidate = trial;
    return FOC_PARAMETER_VALIDATED;
}

/* Public Functions ------------------------------------------------------------- */

size_t foc_parameter_count(void)
{
    return sizeof catalog / sizeof catalog[0];
}

const struct foc_parameter_descriptor *foc_parameter_at(size_t index)
{
    return index < foc_parameter_count() ? &catalog[index] : NULL;
}

const struct foc_parameter_descriptor *foc_parameter_find(enum foc_parameter_id id)
{
    for (size_t i = 0U; i < foc_parameter_count(); ++i) {
        if (catalog[i].id == id) {
            return &catalog[i];
        }
    }
    return NULL;
}

int foc_parameter_check_descriptors(
    const struct foc_parameter_descriptor *descriptors,
    size_t count
)
{
    if (descriptors == NULL || count == 0U || count > foc_parameter_count()) {
        return FOC_PARAMETER_ERR_INPUT;
    }
    for (size_t i = 0U; i < count; ++i) {
        if (descriptors[i].name == NULL || descriptors[i].name[0] == '\0' ||
            descriptors[i].id == 0) {
            return FOC_PARAMETER_ERR_INPUT;
        }
        for (size_t j = 0U; j < i; ++j) {
            if (descriptors[i].id == descriptors[j].id ||
                strncmp(descriptors[i].name, descriptors[j].name, MAX_NAME_BYTES) == 0) {
                return FOC_PARAMETER_ERR_DUPLICATE;
            }
        }
    }
    return FOC_PARAMETER_OK;
}

int foc_parameter_read_config(
    const struct foc_runtime_config *config,
    enum foc_parameter_id id,
    struct foc_parameter_value *value
)
{
    if (config == NULL || value == NULL) {
        return FOC_PARAMETER_ERR_INPUT;
    }
    const struct foc_parameter_descriptor *descriptor = foc_parameter_find(id);
    if (descriptor == NULL || descriptor->category != FOC_PARAMETER_CONFIG) {
        return FOC_PARAMETER_ERR_ID;
    }
    if (!available(&config->motion, id)) {
        return FOC_PARAMETER_ERR_UNAVAILABLE;
    }
    struct foc_parameter_value result = {.type = descriptor->type};
    const int status = static_config_value(config, id, &result.data);
    if (status == FOC_PARAMETER_OK) {
        *value = result;
    }
    return status;
}

int foc_parameter_get_range(
    struct foc_runtime *runtime,
    enum foc_parameter_id id,
    struct foc_parameter_range *range
)
{
    if (range == NULL) {
        return FOC_PARAMETER_ERR_INPUT;
    }
    const struct foc_parameter_descriptor *descriptor = foc_parameter_find(id);
    if (descriptor == NULL) {
        return FOC_PARAMETER_ERR_ID;
    }
    struct foc_parameter_range limits = descriptor->range;
    if (descriptor->instance_range) {
        if (runtime == NULL || !runtime->initialized) {
            return FOC_PARAMETER_ERR_INPUT;
        }
        const struct foc_runtime_motion_config *motion = &runtime->config.motion;
        if (!available(motion, id)) {
            return FOC_PARAMETER_ERR_UNAVAILABLE;
        }
        switch (id) {
        case FOC_PARAMETER_MOTION_IQ_LIMIT:
            limits.maximum.f32 = motion->iq_limit_a;
            break;
        case FOC_PARAMETER_STIFFNESS:
            limits.maximum.f32 = motion->impedance.max_stiffness_nm_per_rad;
            break;
        case FOC_PARAMETER_DAMPING:
            limits.maximum.f32 = motion->impedance.max_damping_nm_s_per_rad;
            break;
        case FOC_PARAMETER_FEEDFORWARD:
            limits.maximum.f32 = motion->impedance.max_feedforward_nm;
            limits.minimum.f32 = -limits.maximum.f32;
            break;
        default:
            return FOC_PARAMETER_ERR_ID;
        }
    }
    *range = limits;
    return FOC_PARAMETER_OK;
}

int foc_parameter_read(
    struct foc_runtime *runtime,
    enum foc_parameter_id id,
    struct foc_parameter_value *value
)
{
    if (runtime == NULL || !runtime->initialized || value == NULL) {
        return FOC_PARAMETER_ERR_INPUT;
    }
    const struct foc_parameter_descriptor *descriptor = foc_parameter_find(id);
    if (descriptor == NULL) {
        return FOC_PARAMETER_ERR_ID;
    }
    if (!available(&runtime->config.motion, id) || descriptor->category == FOC_PARAMETER_COMMAND ||
        descriptor->category == FOC_PARAMETER_ACTION) {
        return FOC_PARAMETER_ERR_UNAVAILABLE;
    }

    struct foc_parameter_value result = {.type = descriptor->type};
    const int status = descriptor->category == FOC_PARAMETER_CONFIG
        ? static_config_value(&runtime->config, id, &result.data)
        : snapshot_value(runtime, id, &result.data);
    if (status == FOC_PARAMETER_OK) {
        *value = result;
    }
    return status;
}

int foc_parameter_validate(
    struct foc_runtime *runtime,
    enum foc_parameter_id id,
    const struct foc_parameter_value *value
)
{
    if (value == NULL) {
        return FOC_PARAMETER_ERR_INPUT;
    }
    const struct foc_parameter_descriptor *descriptor = foc_parameter_find(id);
    if (descriptor == NULL) {
        return FOC_PARAMETER_ERR_ID;
    }
    if (value->type != descriptor->type) {
        return FOC_PARAMETER_ERR_TYPE;
    }
    struct foc_parameter_range range;
    const int status = foc_parameter_get_range(runtime, id, &range);
    return status == FOC_PARAMETER_OK ? range_value(descriptor, &range, value) : status;
}

int foc_parameter_write_access(
    struct foc_runtime *runtime,
    enum foc_parameter_id id,
    uint32_t session
)
{
    if (runtime == NULL || !runtime->initialized) {
        return FOC_PARAMETER_ERR_INPUT;
    }
    const struct foc_parameter_descriptor *descriptor = foc_parameter_find(id);
    if (descriptor == NULL) {
        return FOC_PARAMETER_ERR_ID;
    }
    if (descriptor->access != FOC_PARAMETER_INITIAL_CONFIG &&
        descriptor->access != FOC_PARAMETER_COMMAND_ONLY) {
        return FOC_PARAMETER_ERR_READ_ONLY;
    }
    if (!available(&runtime->config.motion, id)) {
        return FOC_PARAMETER_ERR_UNAVAILABLE;
    }
    return write_gate(runtime, session, descriptor->access == FOC_PARAMETER_INITIAL_CONFIG);
}

int foc_parameter_prepare_initial(
    const struct foc_pipeline_config *pipeline,
    const struct foc_runtime_config *base,
    const struct foc_parameter_edit *edits,
    size_t count,
    struct foc_parameter_candidate *candidate
)
{
    if (pipeline == NULL) {
        return FOC_PARAMETER_ERR_INPUT;
    }
    return prepare(pipeline->current.current_limit_a, base, edits, count, candidate);
}

int foc_parameter_stage(
    struct foc_runtime *runtime,
    uint32_t session,
    const struct foc_parameter_edit *edits,
    size_t count,
    struct foc_parameter_candidate *candidate
)
{
    if (runtime == NULL || !runtime->initialized || candidate == NULL) {
        return FOC_PARAMETER_ERR_INPUT;
    }
    struct foc_parameter_candidate trial;
    const int status =
        prepare(runtime->config.motion.iq_limit_a, &runtime->config, edits, count, &trial);
    if (status != FOC_PARAMETER_VALIDATED) {
        return status;
    }
    const int gate = write_gate(runtime, session, true);
    if (gate != FOC_PARAMETER_OK) {
        return gate;
    }
    *candidate = trial;
    return FOC_PARAMETER_VALIDATED;
}

int foc_parameter_publish_impedance(
    struct foc_runtime *runtime,
    const struct foc_runtime_command *command,
    uint32_t now_us,
    struct foc_parameter_receipt *receipt
)
{
    if (runtime == NULL || !runtime->initialized || command == NULL || receipt == NULL ||
        command->mode != FOC_RUNTIME_MODE_IMPEDANCE) {
        return FOC_PARAMETER_ERR_INPUT;
    }
    const struct foc_runtime_impedance_command *target = &command->target.impedance;
    const struct foc_parameter_edit fields[] = {
        {FOC_PARAMETER_TARGET_POSITION, {FOC_PARAMETER_F32, {.f32 = target->position_rad}}},
        {FOC_PARAMETER_TARGET_VELOCITY, {FOC_PARAMETER_F32, {.f32 = target->velocity_rad_s}}},
        {FOC_PARAMETER_STIFFNESS, {FOC_PARAMETER_F32, {.f32 = target->stiffness_nm_per_rad}}},
        {FOC_PARAMETER_DAMPING, {FOC_PARAMETER_F32, {.f32 = target->damping_nm_s_per_rad}}},
        {FOC_PARAMETER_FEEDFORWARD, {FOC_PARAMETER_F32, {.f32 = target->torque_feedforward_nm}}},
    };
    for (size_t i = 0U; i < sizeof fields / sizeof fields[0]; ++i) {
        const int status = foc_parameter_validate(runtime, fields[i].id, &fields[i].value);
        if (status != FOC_PARAMETER_OK) {
            return status;
        }
    }
    if (target->mechanical_frame_id != runtime->config.motion.impedance.mechanical_frame_id) {
        return FOC_PARAMETER_ERR_FRAME;
    }
    const int gate = write_gate(runtime, target->source_session, false);
    if (gate != FOC_PARAMETER_OK) {
        return gate;
    }
    const int publication =
        runtime_status(foc_runtime_publish_impedance_command(runtime, command, now_us));
    if (publication != FOC_PARAMETER_OK) {
        return publication;
    }
    *receipt = (struct foc_parameter_receipt){
        .owner = runtime,
        .session = target->source_session,
        .sequence = command->seq,
        .produced_us = command->produced_us,
    };
    return FOC_PARAMETER_ACCEPTED;
}

int foc_parameter_command_status(
    struct foc_runtime *runtime,
    const struct foc_parameter_receipt *receipt
)
{
    if (runtime == NULL || receipt == NULL || receipt->owner != runtime) {
        return FOC_PARAMETER_ERR_INPUT;
    }
    struct foc_runtime_snapshot view;
    const int status = runtime_status(foc_runtime_read_snapshot(runtime, &view));
    if (status != FOC_PARAMETER_OK) {
        return status;
    }
    if (view.faults != 0U || view.state == FOC_SAFE_STATE_FAULT_LATCHED) {
        return FOC_PARAMETER_ERR_FAULT;
    }
    if (view.motion_session != receipt->session || view.mode != FOC_RUNTIME_MODE_IMPEDANCE) {
        return FOC_PARAMETER_ERR_SESSION;
    }
    if (view.inhibit_pending) {
        return FOC_PARAMETER_ERR_INHIBIT;
    }
    if (view.command_seen && view.command_seq == receipt->sequence &&
        view.command_us == receipt->produced_us && view.state == FOC_SAFE_STATE_RUNNING &&
        view.output_confirmed && view.action == FOC_DEVICE_OUTPUT_UPDATE && view.fast_seen &&
        view.sensor_valid && view.accepted_outer_seen && view.pipeline_status == FOC_PIPELINE_OK) {
        return FOC_PARAMETER_APPLIED;
    }
    if (view.command_seen && foc_safe_sequence_newer(view.command_seq, receipt->sequence, true)) {
        return FOC_PARAMETER_ERR_ORDER;
    }
    return FOC_PARAMETER_ACCEPTED;
}

int foc_parameter_request_action(
    struct foc_runtime *runtime,
    enum foc_parameter_id id,
    const struct foc_runtime_lifecycle_request *request,
    uint32_t now_us
)
{
    if (runtime == NULL || !runtime->initialized) {
        return FOC_PARAMETER_ERR_INPUT;
    }
    enum foc_runtime_action action;
    switch (id) {
    case FOC_PARAMETER_STOP:
        foc_runtime_revoke_command(runtime);
        return FOC_PARAMETER_ACCEPTED;
    case FOC_PARAMETER_CLEAR_TIMEOUT: {
        const int status = runtime_status(foc_runtime_request_timeout_clear(runtime, now_us));
        return status == FOC_PARAMETER_OK ? FOC_PARAMETER_ACCEPTED : status;
    }
    case FOC_PARAMETER_SET_MODE:
        action = FOC_RUNTIME_ACTION_SET_MODE;
        break;
    case FOC_PARAMETER_ARM:
        action = FOC_RUNTIME_ACTION_ARM;
        break;
    case FOC_PARAMETER_START:
        action = FOC_RUNTIME_ACTION_START;
        break;
    default:
        return FOC_PARAMETER_ERR_ID;
    }
    if (request == NULL || request->action != action) {
        return FOC_PARAMETER_ERR_INPUT;
    }
    const int status =
        runtime_status(foc_runtime_publish_lifecycle_request(runtime, request, now_us));
    return status == FOC_PARAMETER_OK ? FOC_PARAMETER_ACCEPTED : status;
}

/* ---------------------------------------- end of file ----------------------------------------- */
