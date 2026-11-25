/**
 * @file    foc_serial.c
 * @brief   V1 session, Runtime command admission and RAM parameter policy.
 * @version v0.1.0
 * @date    2026.11.10
 *
 * Implementation role
 * -------------------
 * Parse in Foreground after supervision and report scoped operation outcomes.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_serial.h"

#include "foc_board_profile.h"
#include "foc_parameter_table.h"

#include <float.h>
#include <math.h>
#include <string.h>

/* Private Constants ------------------------------------------------------------ */

enum {
    PARAM_U32 = 1,
    PARAM_F32 = 2,
    UNIT_NONE = 0,
    UNIT_AMPERE = 1,
    UNIT_RADIAN = 2,
    UNIT_RAD_S = 3,
    UNIT_COUNT = 4,
    UNIT_MICROSECOND = 5,
    UNIT_V_PER_A = 6,
    UNIT_V_PER_A_S = 7,
    UNIT_A_PER_RAD_S = 8,
    UNIT_A_PER_RAD = 9,
    UNIT_PER_SECOND = 10,
    UNIT_OHM = 11,
    UNIT_HENRY = 12,
    UNIT_WEBER = 13,
    UNIT_NM_PER_A = 14,
    UNIT_KG_M2 = 15,
    UNIT_NM_S_PER_RAD = 16,
    UNIT_HERTZ = 17,
    UNIT_VOLT = 18,
    UNIT_MILLISECOND = 20,
    UNIT_RAD_S2 = 21,
    /* Direction codes on the wire: none, +1 and -1. */
    DIRECTION_NONE = 0,
    DIRECTION_POSITIVE = 1,
    DIRECTION_NEGATIVE = 2,
    FLAG_LOCKED = 1,
    FLAG_SOFTWARE_ACK = 2,
    FLAG_SENSOR_VALID = 4,
    FLAG_COMMAND_SEEN = 8,
    FLAG_INHIBIT_PENDING = 16,
    FLAG_DIRTY = 32,
    FLAG_FAST_SEEN = 64,
    FLAG_SAMPLE_SEEN = 128,
    /* TELEMETRY_READ byte 62: bit 1 marks the firmware's decoded current. */
    TELEMETRY_CURRENT_VALID = 2,
    /* Capability bits: lock, RAM policy, polling telemetry, UART, mode switch, tuning;
     * bit 6 persistent settings and bit 7 on-MCU calibration are added when attached. */
    CAPABILITY_BITS = 63,
    CAPABILITY_PERSISTENCE = 64,
    CAPABILITY_CALIBRATION = 128,
    /* Lifecycle result codes on the wire. */
    LIFECYCLE_WIRE_NONE = 0,
};

/* Private Types ---------------------------------------------------------------- */

struct foc_serial_parameter {
    uint32_t value, minimum, maximum;
    uint8_t type, unit;
    bool writable;
    bool calibration_required;
    /* Written through the H0 tuning mailbox rather than Foreground RAM. */
    bool tuning;
};

/* Private Helpers -------------------------------------------------------------- */

_Static_assert(
    sizeof(float) == 4 && FLT_RADIX == 2 && FLT_MANT_DIG == 24 && FLT_MAX_EXP == 128,
    "Wire float requires IEEE-754 binary32"
);

static uint32_t foc_serial_float_bits(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

static float foc_serial_bits_float(uint32_t bits)
{
    float value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

static void
foc_serial_capability(const struct foc_serial *serial, struct foc_protocol_frame *response)
{
    const uint8_t bits =
        (uint8_t)(CAPABILITY_BITS |
                  (serial->persistence.save != NULL ? CAPABILITY_PERSISTENCE : 0) |
                  (serial->calibration.start != NULL ? CAPABILITY_CALIBRATION : 0));
    /* Algorithm modes: CURRENT, VELOCITY, POSITION and IMPEDANCE (bits 0-3). */
    const uint8_t capability[] = {FOC_PROTO_OK, 1, 0, 1, 0, 15, 0, bits, 0};
    memcpy(response->payload, capability, sizeof capability);
    response->length = sizeof capability;
}

/* Rotor turns per output turn; the Runtime keeps a validated value, 1 without one. */
static float foc_serial_gear_ratio(const struct foc_runtime *runtime)
{
    const float gear = runtime->config.profile.gear_ratio;
    return isfinite(gear) && gear >= 1.0f ? gear : 1.0f;
}

static int foc_serial_status(
    struct foc_serial *serial,
    struct foc_uart_buffer *uart,
    struct foc_runtime *runtime,
    uint32_t now_us,
    struct foc_protocol_frame *response
)
{
    struct foc_runtime_snapshot view;
    if (foc_runtime_read_snapshot(runtime, &view) != FOC_RUNTIME_OK) {
        return FOC_PROTO_BUSY;
    }
    const bool sensed = view.sensor_valid &&
        foc_safe_timestamp_fresh(now_us, view.sensor_us, runtime->config.angle_timeout_us);
    uint8_t *body = response->payload;
    body[1] = (uint8_t)view.state;
    body[2] = (uint8_t)view.mode;
    body[3] = FLAG_LOCKED | (view.output_confirmed ? FLAG_SOFTWARE_ACK : 0) |
        (sensed ? FLAG_SENSOR_VALID : 0) | (view.inhibit_pending ? FLAG_INHIBIT_PENDING : 0) |
        (serial->dirty ? FLAG_DIRTY : 0) | (view.command_seen ? FLAG_COMMAND_SEEN : 0) |
        (view.fast_seen ? FLAG_FAST_SEEN : 0) | (view.sample_seen ? FLAG_SAMPLE_SEEN : 0);
    body[4] = (uint8_t)view.action;
    foc_protocol_write_u32(body + 5, view.faults);
    foc_protocol_write_u32(body + 9, now_us);
    foc_protocol_write_u32(body + 13, (uint32_t)(int32_t)view.pipeline_status);
    body[17] = (uint8_t)view.clear_result;
    foc_protocol_write_u32(body + 18, view.sample_us);
    foc_protocol_write_u32(body + 22, view.command_us);
    foc_protocol_write_u32(body + 26, view.command_seq);
    response->length = 30;
    if (response->type == FOC_MSG_TELEMETRY_READ) {
        /* The firmware's own d/q decode of the latest accepted cycle. It is
         * zero in the locked image, which never accepts an UPDATE, and it
         * never implies physical qualification (PARAM_READ ID 45). */
        foc_protocol_write_u32(
            body + 30,
            foc_serial_float_bits(view.current_valid ? view.measured_id_a : 0.0f)
        );
        foc_protocol_write_u32(
            body + 34,
            foc_serial_float_bits(view.current_valid ? view.measured_iq_a : 0.0f)
        );
        /* Output frame: rotor position over the gear ratio, less the zero offset. */
        const float gear = foc_serial_gear_ratio(runtime);
        foc_protocol_write_u32(
            body + 38,
            foc_serial_float_bits(view.mechanical_rad / gear - serial->output_zero_rad)
        );
        foc_protocol_write_u32(body + 42, foc_serial_float_bits(view.mechanical_rad_s / gear));
        foc_protocol_write_u32(body + 46, view.sensor_us);
        foc_protocol_write_u32(body + 50, serial->crc_errors);
        foc_protocol_write_u32(body + 54, serial->dropped);
        foc_protocol_write_u32(body + 58, uart != NULL ? atomic_load(&uart->rx_lost) : 0U);
        /* Bit 0 physical applied and bit 2 sensor calibrated stay false here. */
        body[62] = view.current_valid ? TELEMETRY_CURRENT_VALID : 0U;
        /* M1 append: the d/q target of that cycle and the controller tuning revision. */
        struct foc_runtime_tuning_result tuning = {0};
        (void)foc_runtime_read_tuning_result(runtime, &tuning);
        foc_protocol_write_u32(
            body + 63,
            foc_serial_float_bits(view.current_valid ? view.target_id_a : 0.0f)
        );
        foc_protocol_write_u32(
            body + 67,
            foc_serial_float_bits(view.current_valid ? view.target_iq_a : 0.0f)
        );
        foc_protocol_write_u32(body + 71, tuning.revision);
        /* P1 append: the rotor-side position and speed the output frame derives from. */
        foc_protocol_write_u32(body + 75, foc_serial_float_bits(view.mechanical_rad));
        foc_protocol_write_u32(body + 79, foc_serial_float_bits(view.mechanical_rad_s));
        response->length = 83;
    }
    return FOC_PROTO_OK;
}

static uint32_t foc_serial_direction_code(int32_t direction)
{
    return direction == 1 ? DIRECTION_POSITIVE
        : direction == -1 ? DIRECTION_NEGATIVE
                          : DIRECTION_NONE;
}

static uint32_t foc_serial_request_code(enum foc_runtime_request_result result)
{
    return (uint32_t)result; /* NONE 0, PENDING 1, APPLIED 2, REJECTED 3, CANCELLED 4 */
}

/* A 4-byte field of a serial or tuning struct, by the table's byte offset. */
static uint32_t foc_serial_field_get(const void *base, uint16_t offset)
{
    uint32_t value;
    memcpy(&value, (const uint8_t *)base + offset, sizeof value);
    return value;
}

static void foc_serial_field_set(void *base, uint16_t offset, uint32_t value)
{
    memcpy((uint8_t *)base + offset, &value, sizeof value);
}

/* A bound: the table constant or the live value it names. */
static uint32_t foc_serial_bound(
    const struct foc_serial *serial,
    const struct foc_runtime *runtime,
    const struct foc_runtime_tuning *tuning,
    uint8_t bound,
    union foc_parameter_value constant
)
{
    switch (bound) {
    case FOC_PARAM_BOUND_RUNTIME_IQ_LIMIT:
        return foc_serial_float_bits(runtime->config.motion.iq_limit_a);
    case FOC_PARAM_BOUND_CURRENT_CEILING:
        return foc_serial_float_bits(serial->current_limit_ceiling_a);
    case FOC_PARAM_BOUND_TUNING_CURRENT_LIMIT:
        return foc_serial_float_bits(tuning->current_limit_a);
    default:
        return constant.u;
    }
}

/* Startup readback (IDs 32-45) from the bound live Runtime and encoder policy. */
static uint32_t foc_serial_boot_value(const struct foc_serial_boot_readback *view, uint16_t id)
{
    switch (id) {
    case FOC_PARAM_BOOT_CONFIGURATION:
        return view->configuration;
    case FOC_PARAM_CALIBRATION_INPUT:
        return view->calibration;
    case FOC_PARAM_CALIBRATION_REJECTION:
        return view->rejection;
    case FOC_PARAM_ADMISSION_FAILURE:
        return view->admission_failure;
    case FOC_PARAM_SUPPLY_ID:
        return view->supply_id;
    case FOC_PARAM_REVIEW_ID:
        return view->review_id;
    case FOC_PARAM_PHASE_A_OFFSET:
    case FOC_PARAM_PHASE_B_OFFSET:
    case FOC_PARAM_PHASE_C_OFFSET:
        return foc_serial_float_bits(view->phase_offset_count[id - FOC_PARAM_PHASE_A_OFFSET]);
    case FOC_PARAM_ENCODER_CPR:
        return view->encoder_counts_per_revolution;
    case FOC_PARAM_RUNTIME_DIRECTION:
        return foc_serial_direction_code(view->runtime_direction);
    case FOC_PARAM_SOURCE_DIRECTION:
        return foc_serial_direction_code(view->source_direction);
    case FOC_PARAM_CALIBRATION_DIRECTION:
        return foc_serial_direction_code(view->calibration_direction);
    default:
        return view->current_measurement_qualified ? 1U : 0U;
    }
}

/* Commissioning readback (IDs 88-95) of the latest on-MCU calibration. */
static uint32_t
foc_serial_commission_value(const struct foc_serial_commission_view *view, uint16_t id)
{
    switch (id) {
    case FOC_PARAM_COMMUTATION_VALID:
        return view->valid;
    case FOC_PARAM_COMMUTATION_SOURCE:
        return view->source;
    case FOC_PARAM_ADC_ZERO_STATE:
        return view->zero_state;
    case FOC_PARAM_COAST_STATUS:
        return view->coast_status;
    case FOC_PARAM_COAST_FLUX:
        return foc_serial_float_bits(view->coast_flux_wb);
    case FOC_PARAM_COAST_ANGLE_ERROR:
        return foc_serial_float_bits(view->coast_angle_error_rad);
    case FOC_PARAM_DRIVEN_FLUX:
        return foc_serial_float_bits(view->driven_flux_wb);
    case FOC_PARAM_COGGING_STATUS:
        return view->cogging_status;
    case FOC_PARAM_COGGING_PEAK:
        return foc_serial_float_bits(view->cogging_peak_a);
    case FOC_PARAM_COGGING_RMS:
        return foc_serial_float_bits(view->cogging_rms_a);
    case FOC_PARAM_COGGING_FRICTION:
        return foc_serial_float_bits(view->cogging_friction_a);
    case FOC_PARAM_COGGING_CONSISTENCY:
        return foc_serial_float_bits(view->cogging_consistency);
    case FOC_PARAM_ENCODER_LUT_STATUS:
        return view->encoder_status;
    case FOC_PARAM_ENCODER_LUT_PEAK:
        return foc_serial_float_bits(view->encoder_peak_rad);
    case FOC_PARAM_ENCODER_LUT_RMS:
        return foc_serial_float_bits(view->encoder_rms_rad);
    case FOC_PARAM_ENCODER_LUT_CONSISTENCY:
        return foc_serial_float_bits(view->encoder_consistency);
    default:
        return foc_serial_float_bits(view->dead_time_v);
    }
}

/* Persistent-storage readback (IDs 48-51); zeros without an attached store. */
static uint32_t foc_serial_store_value(const struct foc_serial *serial, uint16_t id)
{
    struct foc_serial_persistence_view view = {0};
    if (serial->persistence.read != NULL) {
        (void)serial->persistence.read(serial->persistence.context, &view);
    }
    return id == FOC_PARAM_STORE_STATE          ? view.state
        : id == FOC_PARAM_STORE_LOADED_SEQUENCE ? view.loaded_sequence
        : id == FOC_PARAM_STORE_STORED_SEQUENCE ? view.stored_sequence
                                                : view.calibration_restored;
}

/*
 * One parameter as the wire reports it, served by the table entry's group. A parameter
 * whose source is absent (no startup reader, no commissioning, protection disabled or an
 * unreadable Runtime) does not exist on this image.
 */
static bool foc_serial_parameter_read(
    const struct foc_serial *serial,
    const struct foc_runtime *runtime,
    uint16_t id,
    struct foc_serial_parameter *parameter
)
{
    const struct foc_parameter_entry *entry = foc_parameter_find(id);
    struct foc_runtime_tuning tuning = {0};
    if (entry == NULL) {
        return false;
    }
    *parameter = (struct foc_serial_parameter){
        .type = entry->type,
        .unit = entry->unit,
        .writable =
            entry->access == FOC_PARAM_ACCESS_RW || entry->access == FOC_PARAM_ACCESS_TUNING,
        .calibration_required = (entry->flags & FOC_PARAM_FLAG_CALIBRATION) != 0U,
        .tuning = entry->access == FOC_PARAM_ACCESS_TUNING,
    };
    switch (entry->group) {
    case FOC_PARAM_GROUP_SERIAL:
        parameter->value = foc_serial_field_get(serial, entry->offset);
        break;
    case FOC_PARAM_GROUP_TUNING:
    case FOC_PARAM_GROUP_RECEIPT:
    case FOC_PARAM_GROUP_DERIVED: {
        struct foc_runtime_tuning_result receipt;
        if (((entry->flags & FOC_PARAM_FLAG_TRIP) != 0U && !runtime->config.trip_enabled) ||
            foc_runtime_read_tuning(runtime, &tuning) != FOC_RUNTIME_OK ||
            foc_runtime_read_tuning_result((struct foc_runtime *)runtime, &receipt) !=
                FOC_RUNTIME_OK) {
            return false;
        }
        if (entry->group == FOC_PARAM_GROUP_TUNING) {
            parameter->value = foc_serial_field_get(&tuning, entry->offset);
        } else if (entry->group == FOC_PARAM_GROUP_RECEIPT) {
            parameter->value = id == FOC_PARAM_TUNING_REVISION ? receipt.revision
                : id == FOC_PARAM_TUNING_RESULT ? foc_serial_request_code(receipt.result)
                                                : receipt.request_sequence;
        } else {
            /* Kt = 1.5 x pole pairs x flux linkage, per peak q ampere; 0 without a flux. */
            parameter->value = foc_serial_float_bits(
                1.5f * (float)runtime->config.motion.rotor.pole_pairs * tuning.profile.flux_wb
            );
        }
        break;
    }
    case FOC_PARAM_GROUP_BOOT: {
        struct foc_serial_boot_readback view;
        if (serial->boot_reader == NULL || !serial->boot_reader(serial->boot_context, &view)) {
            return false;
        }
        parameter->value = foc_serial_boot_value(&view, id);
        break;
    }
    case FOC_PARAM_GROUP_STORE:
        parameter->value = foc_serial_store_value(serial, id);
        break;
    case FOC_PARAM_GROUP_COMMISSION: {
        struct foc_serial_commission_view view = {0};
        if (serial->calibration.commission == NULL) {
            return false;
        }
        (void)serial->calibration.commission(serial->calibration.context, &view);
        parameter->value = foc_serial_commission_value(&view, id);
        break;
    }
    case FOC_PARAM_GROUP_ROTOR:
        if (id == FOC_PARAM_POLE_PAIRS) {
            parameter->value = runtime->config.motion.rotor.pole_pairs;
        } else if (id == FOC_PARAM_ELECTRICAL_OFFSET) {
            parameter->value =
                foc_serial_float_bits(runtime->config.motion.rotor.electrical_offset_rad);
            /* A trim of the valid commutation group, [0, 2π). */
            parameter->writable = serial->calibration.trim != NULL;
        } else {
            parameter->value =
                foc_serial_float_bits(runtime->config.motion.position.velocity_limit_rad_s);
        }
        break;
    default: /* FOC_PARAM_GROUP_STATUS */
        if (id == FOC_PARAM_CAN_STATUS) {
            parameter->value = serial->can_link.status != NULL
                ? serial->can_link.status(serial->can_link.context)
                : 0U;
        } else if (id == FOC_PARAM_I2T_LOAD || id == FOC_PARAM_THERMAL_CURRENT_LIMIT) {
            float theta;
            float limit_a;
            if (foc_runtime_read_thermal((struct foc_runtime *)runtime, &theta, &limit_a) !=
                FOC_RUNTIME_OK) {
                return false;
            }
            parameter->value = foc_serial_float_bits(id == FOC_PARAM_I2T_LOAD ? theta : limit_a);
        } else {
            struct foc_runtime_snapshot view;
            if (foc_runtime_read_snapshot((struct foc_runtime *)runtime, &view) != FOC_RUNTIME_OK) {
                return false;
            }
            parameter->value = view.faults;
        }
        break;
    }
    if (parameter->writable) {
        parameter->minimum =
            foc_serial_bound(serial, runtime, &tuning, entry->min_bound, entry->minimum);
        parameter->maximum =
            foc_serial_bound(serial, runtime, &tuning, entry->max_bound, entry->maximum);
    } else {
        parameter->minimum = parameter->maximum = parameter->value;
    }
    return true;
}

/* Replace one field of the live tuning and publish the whole candidate to H0. */
static int foc_serial_tuning_write(
    struct foc_serial *serial,
    struct foc_runtime *runtime,
    const struct foc_parameter_entry *entry,
    uint32_t value
)
{
    struct foc_runtime_tuning_result before;
    struct foc_runtime_tuning_result after;
    struct foc_runtime_tuning tuning;
    /* A candidate built from a copy torn by a concurrent apply could revert it. */
    if (foc_runtime_read_tuning_result(runtime, &before) != FOC_RUNTIME_OK ||
        before.result == FOC_RUNTIME_REQUEST_PENDING ||
        foc_runtime_read_tuning(runtime, &tuning) != FOC_RUNTIME_OK ||
        foc_runtime_read_tuning_result(runtime, &after) != FOC_RUNTIME_OK ||
        after.result == FOC_RUNTIME_REQUEST_PENDING || after.revision != before.revision) {
        return FOC_PROTO_BUSY;
    }
    foc_serial_field_set(&tuning, entry->offset, value);
    /* Unified refresh (contract 13.3), for profile fields only: a model or bandwidth change
     * re-derives the current PI, a peak current or maximum speed tightens the limits it
     * bounds; Kt, the inertia, the velocity bandwidth or its 1/5 bound re-derive the outer
     * gains. A written limit is never adjusted: H0 refuses one that breaks the ordering. */
    if ((entry->flags & FOC_PARAM_FLAG_REFRESH_PROFILE) != 0U &&
        !foc_board_profile_refresh(
            runtime->config.h0.period_us,
            runtime->pipeline.current.config.pwm.voltage_limit_v,
            runtime->config.motion.rotor.pole_pairs,
            &tuning,
            (entry->flags & FOC_PARAM_FLAG_REDESIGN_CURRENT) != 0U,
            (entry->flags & FOC_PARAM_FLAG_REDESIGN_VELOCITY) != 0U,
            NULL
        )) {
        return FOC_PROTO_INVALID_ARGUMENT;
    }
    const uint32_t sequence = serial->tuning_sequence + 1U;
    const int result = foc_runtime_publish_tuning(runtime, &tuning, sequence);
    if (result == FOC_RUNTIME_ERR_BUSY) {
        return FOC_PROTO_BUSY;
    }
    if (result != FOC_RUNTIME_OK) {
        return FOC_PROTO_INVALID_ARGUMENT;
    }
    serial->tuning_sequence = sequence;
    /* The remote current cap (ID 2) stays within the outer q-current limit it serves. */
    if (serial->current_limit_a > tuning.iq_limit_a) {
        serial->current_limit_a = tuning.iq_limit_a;
    }
    return FOC_PROTO_ACCEPTED;
}

static int foc_serial_parameter(
    struct foc_serial *serial,
    struct foc_runtime *runtime,
    const struct foc_protocol_frame *request,
    uint32_t now_us,
    struct foc_protocol_frame *response
)
{
    const bool writing = request->type == FOC_MSG_PARAM_WRITE;
    if (request->length != (writing ? 7U : 2U)) {
        return FOC_PROTO_INVALID_ARGUMENT;
    }
    const uint16_t id = foc_protocol_read_u16(request->payload);
    struct foc_serial_parameter parameter;
    if (!foc_serial_parameter_read(serial, runtime, id, &parameter)) {
        return FOC_PROTO_INVALID_ARGUMENT;
    }
    if (writing) {
        const struct foc_parameter_entry *entry = foc_parameter_find(id);
        struct foc_runtime_snapshot view;
        if (!parameter.writable) {
            return FOC_PROTO_NOT_SUPPORTED;
        }
        if (foc_runtime_read_snapshot(runtime, &view) != FOC_RUNTIME_OK) {
            return FOC_PROTO_BUSY;
        }
        if (view.state != FOC_SAFE_STATE_DISABLED || view.inhibit_pending ||
            !view.output_confirmed || view.action != FOC_DEVICE_OUTPUT_INHIBIT) {
            return FOC_PROTO_BAD_STATE;
        }
        const uint32_t value = foc_protocol_read_u32(request->payload + 3);
        if (request->payload[2] != parameter.type) {
            return FOC_PROTO_INVALID_ARGUMENT;
        }
        if (parameter.type == PARAM_U32) {
            if (value < parameter.minimum || value > parameter.maximum) {
                return FOC_PROTO_INVALID_ARGUMENT;
            }
        } else {
            const float floating = foc_serial_bits_float(value);
            if (!isfinite(floating) || floating < foc_serial_bits_float(parameter.minimum) ||
                floating > foc_serial_bits_float(parameter.maximum)) {
                return FOC_PROTO_INVALID_ARGUMENT;
            }
        }
        if (!foc_parameter_allowed(entry, value)) {
            return FOC_PROTO_INVALID_ARGUMENT;
        }
        if (parameter.tuning) {
            const int published = foc_serial_tuning_write(serial, runtime, entry, value);
            if (published != FOC_PROTO_ACCEPTED) {
                return published;
            }
            serial->dirty = true;
            parameter.value = value;
            foc_protocol_write_u16(response->payload + 1, id);
            response->payload[3] = parameter.type;
            response->payload[4] = parameter.unit;
            /* Writable and calibration bits only: RAM application is reported by ID 61. */
            response->payload[5] = 1U;
            foc_protocol_write_u32(response->payload + 6, parameter.minimum);
            foc_protocol_write_u32(response->payload + 10, parameter.maximum);
            foc_protocol_write_u32(response->payload + 14, parameter.value);
            response->length = 18;
            return FOC_PROTO_ACCEPTED;
        }
        if (entry->access == FOC_PARAM_ACCESS_TRIM) {
            const int trimmed =
                serial->calibration
                    .trim(serial->calibration.context, now_us, foc_serial_bits_float(value));
            if (trimmed != FOC_PROTO_APPLIED) {
                return trimmed;
            }
        } else {
            const struct foc_serial_thermal_settings before = serial->thermal;
            foc_serial_field_set(serial, entry->offset, value);
            if (!foc_serial_thermal_settings_valid(&serial->thermal)) {
                serial->thermal = before;
                return FOC_PROTO_INVALID_ARGUMENT;
            }
        }
        serial->dirty = true;
        parameter.value = value;
    }
    foc_protocol_write_u16(response->payload + 1, id);
    response->payload[3] = parameter.type;
    response->payload[4] = parameter.unit;
    response->payload[5] =
        2U | (parameter.writable ? 1U : 0U) | (parameter.calibration_required ? 4U : 0U);
    foc_protocol_write_u32(response->payload + 6, parameter.minimum);
    foc_protocol_write_u32(response->payload + 10, parameter.maximum);
    foc_protocol_write_u32(response->payload + 14, parameter.value);
    response->length = 18;
    return writing ? FOC_PROTO_APPLIED : FOC_PROTO_OK;
}

int foc_serial_impedance(
    struct foc_serial *serial,
    struct foc_runtime *runtime,
    const float target[5],
    uint32_t now_us
)
{
    /* Output side in, rotor side out: position plus the output zero and velocity scale
     * by the gear ratio, stiffness and damping by its inverse square, torque by its
     * inverse (output torque = gear x rotor torque). */
    const float gear = foc_serial_gear_ratio(runtime);
    for (unsigned int index = 0U; index < 5U; ++index) {
        if (!isfinite(target[index])) {
            return FOC_PROTO_INVALID_ARGUMENT;
        }
    }
    const float position = (target[0] + serial->output_zero_rad) * gear;
    const float velocity = target[1] * gear;
    if (fabsf(target[0]) > serial->position_limit_rad || !isfinite(position) ||
        fabsf(position) > FOC_RUNTIME_IMPEDANCE_POSITION_LIMIT_RAD ||
        fabsf(velocity) > runtime->config.motion.position.velocity_limit_rad_s ||
        !(target[2] >= 0.0f) || !(target[3] >= 0.0f)) {
        return FOC_PROTO_INVALID_ARGUMENT;
    }
    struct foc_runtime_snapshot view;
    if (foc_runtime_read_snapshot(runtime, &view) != FOC_RUNTIME_OK) {
        return FOC_PROTO_BUSY;
    }
    if (view.state == FOC_SAFE_STATE_FAULT_LATCHED || view.inhibit_pending ||
        view.mode != FOC_RUNTIME_MODE_IMPEDANCE) {
        return FOC_PROTO_BAD_STATE;
    }
    const uint32_t seq = serial->publication_seq + 1U;
    const struct foc_runtime_command command = {
        .mode = FOC_RUNTIME_MODE_IMPEDANCE,
        .seq = seq,
        .produced_us = now_us,
        .target.impedance = {
            .position_rad = position,
            .velocity_rad_s = velocity,
            .stiffness_nm_per_rad = target[2] / (gear * gear),
            .damping_nm_s_per_rad = target[3] / (gear * gear),
            .torque_feedforward_nm = target[4] / gear,
            .mechanical_frame_id = runtime->config.motion.impedance.mechanical_frame_id,
            .source_session = view.motion_session,
        },
    };
    const int result = foc_runtime_publish_impedance_command(runtime, &command, now_us);
    if (result == FOC_RUNTIME_ERR_INPUT || result == FOC_RUNTIME_ERR_MOTION_LIMIT) {
        return FOC_PROTO_INVALID_ARGUMENT;
    }
    if (result != FOC_RUNTIME_OK) {
        return FOC_PROTO_BAD_STATE;
    }
    serial->publication_seq = seq;
    return FOC_PROTO_ACCEPTED;
}

static int foc_serial_setpoint(
    struct foc_serial *serial,
    struct foc_runtime *runtime,
    const struct foc_protocol_frame *request,
    uint32_t now_us
)
{
    const uint8_t mode = request->length > 0U ? request->payload[0] : 0xFFU;
    if (mode == FOC_RUNTIME_MODE_IMPEDANCE) {
        /* Mode 3: position, velocity, stiffness, damping and feedforward torque, f32 each,
         * output side. */
        float target[5];
        if (request->length != 21U) {
            return FOC_PROTO_INVALID_ARGUMENT;
        }
        for (unsigned int index = 0U; index < 5U; ++index) {
            target[index] =
                foc_serial_bits_float(foc_protocol_read_u32(request->payload + 1U + 4U * index));
        }
        return foc_serial_impedance(serial, runtime, target, now_us);
    }
    if (mode > FOC_RUNTIME_MODE_POSITION ||
        request->length != (mode == FOC_RUNTIME_MODE_CURRENT ? 9U : 5U)) {
        return FOC_PROTO_INVALID_ARGUMENT;
    }
    const float first = foc_serial_bits_float(foc_protocol_read_u32(request->payload + 1));
    const float second = mode == FOC_RUNTIME_MODE_CURRENT
        ? foc_serial_bits_float(foc_protocol_read_u32(request->payload + 5))
        : 0.0f;
    if (!isfinite(first) || !isfinite(second)) {
        return FOC_PROTO_INVALID_ARGUMENT;
    }
    const float inner_limit = runtime->pipeline.current.config.current_limit_a;
    const float current_cap =
        serial->current_limit_a < inner_limit ? serial->current_limit_a : inner_limit;
    /* Velocity and position targets are output-side; the Runtime runs on the rotor side. */
    const float gear = foc_serial_gear_ratio(runtime);
    const float rotor_ref =
        mode == FOC_RUNTIME_MODE_POSITION ? (first + serial->output_zero_rad) * gear : first * gear;
    if ((mode == FOC_RUNTIME_MODE_CURRENT &&
         first * first + second * second > current_cap * current_cap) ||
        (mode == FOC_RUNTIME_MODE_VELOCITY &&
         fabsf(rotor_ref) > runtime->config.motion.position.velocity_limit_rad_s) ||
        (mode == FOC_RUNTIME_MODE_POSITION &&
         (fabsf(first) > serial->position_limit_rad || !isfinite(rotor_ref)))) {
        return FOC_PROTO_INVALID_ARGUMENT;
    }
    struct foc_runtime_snapshot view;
    if (foc_runtime_read_snapshot(runtime, &view) != FOC_RUNTIME_OK) {
        return FOC_PROTO_BUSY;
    }
    if (view.state == FOC_SAFE_STATE_FAULT_LATCHED || view.inhibit_pending || mode != view.mode) {
        return FOC_PROTO_BAD_STATE;
    }
    const uint32_t seq = serial->publication_seq + 1U;
    int result;
    if (mode == FOC_RUNTIME_MODE_CURRENT) {
        const struct foc_pipeline_command command = {{first, second}, seq, now_us};
        result = foc_runtime_publish_command(runtime, &command, now_us);
    } else {
        const struct foc_runtime_command command = {
            .mode = (enum foc_runtime_mode)mode,
            .seq = seq,
            .produced_us = now_us,
            .target.motion_ref = rotor_ref,
        };
        result = foc_runtime_publish_motion_command(runtime, &command, now_us);
    }
    if (result != FOC_RUNTIME_OK) {
        return FOC_PROTO_BAD_STATE;
    }
    serial->publication_seq = seq;
    return FOC_PROTO_ACCEPTED;
}

/* Publish ARM, START or SET_MODE for the next due H0; ACCEPTED carries its identity. */
static int foc_serial_lifecycle(
    struct foc_serial *serial,
    struct foc_runtime *runtime,
    enum foc_runtime_action action,
    enum foc_runtime_mode requested_mode,
    uint32_t now_us,
    struct foc_protocol_frame *response
)
{
    struct foc_runtime_snapshot view;
    if (foc_runtime_read_snapshot(runtime, &view) != FOC_RUNTIME_OK) {
        return FOC_PROTO_BUSY;
    }
    /* H0 rechecks; refusing here gives the Host an immediate answer. */
    if (action == FOC_RUNTIME_ACTION_SET_MODE &&
        (view.state != FOC_SAFE_STATE_DISABLED || view.inhibit_pending || !view.output_confirmed ||
         view.action != FOC_DEVICE_OUTPUT_INHIBIT)) {
        return FOC_PROTO_BAD_STATE;
    }
    const struct foc_runtime_lifecycle_request lifecycle_request = {
        .action = action,
        .requested_mode = action == FOC_RUNTIME_ACTION_SET_MODE ? requested_mode : view.mode,
        .request_sequence = serial->lifecycle_sequence + 1U,
        .source_session = view.motion_session,
        .produced_us = now_us,
    };
    const int publish = foc_runtime_publish_lifecycle_request(runtime, &lifecycle_request, now_us);
    if (publish != FOC_RUNTIME_OK) {
        return publish == FOC_RUNTIME_ERR_BUSY ? FOC_PROTO_BUSY : FOC_PROTO_BAD_STATE;
    }
    serial->lifecycle_sequence = lifecycle_request.request_sequence;
    foc_protocol_write_u32(response->payload + 1, lifecycle_request.request_sequence);
    response->length = 5;
    return FOC_PROTO_ACCEPTED;
}

static int foc_serial_lifecycle_read(
    struct foc_serial *serial,
    struct foc_runtime *runtime,
    struct foc_protocol_frame *response
)
{
    struct foc_runtime_lifecycle_result result;
    if (foc_serial_read_lifecycle_result(serial, runtime, &result) != FOC_RUNTIME_OK) {
        return FOC_PROTO_BUSY;
    }
    uint8_t *body = response->payload;
    body[1] = (uint8_t)foc_serial_request_code(result.result);
    body[2] = (uint8_t)result.action;
    body[3] = (uint8_t)result.requested_mode;
    foc_protocol_write_u32(body + 4, result.request_sequence);
    body[8] = (uint8_t)result.state;
    foc_protocol_write_u32(body + 9, result.faults);
    foc_protocol_write_u32(body + 13, result.h0_us);
    foc_protocol_write_u32(body + 17, (uint32_t)(int32_t)result.pipeline_status);
    foc_protocol_write_u32(body + 21, result.applied_session);
    response->length = 25;
    return FOC_PROTO_OK;
}

/* Save the live settings, or erase storage, only while DISABLED with INHIBIT confirmed. */
static int foc_serial_store(
    struct foc_serial *serial,
    struct foc_runtime *runtime,
    uint8_t type,
    uint32_t now_us,
    struct foc_protocol_frame *response
)
{
    if (serial->persistence.save == NULL || serial->persistence.erase == NULL) {
        return FOC_PROTO_NOT_SUPPORTED;
    }
    struct foc_runtime_snapshot view;
    if (foc_runtime_read_snapshot(runtime, &view) != FOC_RUNTIME_OK) {
        return FOC_PROTO_BUSY;
    }
    if (view.state != FOC_SAFE_STATE_DISABLED || view.inhibit_pending || !view.output_confirmed ||
        view.action != FOC_DEVICE_OUTPUT_INHIBIT) {
        return FOC_PROTO_BAD_STATE;
    }
    if (type == FOC_MSG_CONFIG_ERASE) {
        const int erased = serial->persistence.erase(serial->persistence.context);
        if (erased != FOC_PROTO_APPLIED || serial->calibration.reset == NULL) {
            return erased;
        }
        const int reset = serial->calibration.reset(serial->calibration.context, now_us);
        if (reset == FOC_PROTO_APPLIED) {
            serial->dirty = false;
        }
        return reset;
    }
    uint32_t sequence = 0U;
    const int result =
        serial->persistence.save(serial->persistence.context, serial, runtime, &sequence);
    if (result == FOC_PROTO_APPLIED) {
        foc_protocol_write_u32(response->payload + 1, sequence);
        response->length = 5;
        serial->dirty = false;
    }
    return result;
}

/* The measurements latched at the first protection trip (39 bytes). */
static int
foc_serial_fault_snapshot(struct foc_runtime *runtime, struct foc_protocol_frame *response)
{
    struct foc_runtime_fault_snapshot snapshot;
    if (foc_runtime_read_fault_snapshot(runtime, &snapshot) != FOC_RUNTIME_OK) {
        return FOC_PROTO_BUSY;
    }
    uint8_t *body = response->payload;
    body[1] = snapshot.valid ? 1U : 0U;
    body[2] = (uint8_t)snapshot.mode;
    foc_protocol_write_u32(body + 3, snapshot.faults);
    foc_protocol_write_u32(body + 7, snapshot.h0_us);
    const float values[] = {
        snapshot.speed_rad_s,
        snapshot.speed_ref_rad_s,
        snapshot.measured_id_a,
        snapshot.measured_iq_a,
        snapshot.iq_ref_a,
        snapshot.phase_peak_a,
        snapshot.vbus_v,
    };
    for (unsigned int index = 0U; index < sizeof values / sizeof values[0]; ++index) {
        foc_protocol_write_u32(body + 11 + 4U * index, foc_serial_float_bits(values[index]));
    }
    response->length = 39;
    return FOC_PROTO_OK;
}

static bool foc_serial_calibrating(const struct foc_serial *serial)
{
    return serial->calibration.busy != NULL &&
        serial->calibration.busy(serial->calibration.context);
}

/* Messages that would change control state while a calibration owns the Runtime. */
static bool foc_serial_controls(uint8_t type)
{
    return type == FOC_MSG_SETPOINT || type == FOC_MSG_ARM || type == FOC_MSG_START ||
        type == FOC_MSG_SET_MODE || type == FOC_MSG_PARAM_WRITE || type == FOC_MSG_PARAM_SAVE ||
        type == FOC_MSG_CONFIG_ERASE || type == FOC_MSG_FAULT_CLEAR ||
        type == FOC_MSG_CALIBRATE_START || type == FOC_MSG_COMMUTATION_WRITE ||
        type == FOC_MSG_SET_ZERO;
}

/* COGGING_READ: 64 codes of the cogging table from an offset (u16). Body: bins u16, scale
 * f32 (A per code, 0 when empty), offset u16, count u8, codes int8. */
static int foc_serial_cogging_read(
    const struct foc_runtime *runtime,
    const struct foc_protocol_frame *request,
    struct foc_protocol_frame *response
)
{
    const struct foc_runtime_cogging_table *table = runtime->config.motion.cogging_table;
    if (table == NULL) {
        return FOC_PROTO_NOT_SUPPORTED;
    }
    if (request->length != 2U) {
        return FOC_PROTO_INVALID_ARGUMENT;
    }
    const uint32_t offset = foc_protocol_read_u16(request->payload);
    if (offset >= FOC_RUNTIME_COGGING_BINS) {
        return FOC_PROTO_INVALID_ARGUMENT;
    }
    const uint32_t remaining = FOC_RUNTIME_COGGING_BINS - offset;
    const uint32_t count = remaining < 64U ? remaining : 64U;
    uint8_t *body = response->payload;
    foc_protocol_write_u16(body + 1, (uint16_t)FOC_RUNTIME_COGGING_BINS);
    foc_protocol_write_u32(body + 3, foc_serial_float_bits(table->scale_a));
    foc_protocol_write_u16(body + 7, (uint16_t)offset);
    body[9] = (uint8_t)count;
    for (uint32_t index = 0U; index < count; ++index) {
        body[10U + index] = (uint8_t)table->code[offset + index];
    }
    response->length = (uint8_t)(10U + count);
    return FOC_PROTO_OK;
}

/* ENCODER_LUT_READ: 40 knots of the CAL-9 correction from an offset (u16). Body: knots u16,
 * counts per turn u32 (0: no table), active u8, offset u16, count u8, offsets int16. */
static int foc_serial_encoder_lut_read(
    const struct foc_serial *serial,
    const struct foc_protocol_frame *request,
    struct foc_protocol_frame *response
)
{
    struct foc_serial_encoder_lut_view view;
    if (serial->calibration.encoder_lut == NULL ||
        !serial->calibration.encoder_lut(serial->calibration.context, &view)) {
        return FOC_PROTO_NOT_SUPPORTED;
    }
    if (request->length != 2U) {
        return FOC_PROTO_INVALID_ARGUMENT;
    }
    const uint32_t offset = foc_protocol_read_u16(request->payload);
    if (offset >= view.knots) {
        return FOC_PROTO_INVALID_ARGUMENT;
    }
    const uint32_t remaining = view.knots - offset;
    const uint32_t count = remaining < 40U ? remaining : 40U;
    uint8_t *body = response->payload;
    foc_protocol_write_u16(body + 1, (uint16_t)view.knots);
    foc_protocol_write_u32(body + 3, view.counts_per_revolution);
    body[7] = view.active ? 1U : 0U;
    foc_protocol_write_u16(body + 8, (uint16_t)offset);
    body[10] = (uint8_t)count;
    for (uint32_t index = 0U; index < count; ++index) {
        foc_protocol_write_u16(body + 11U + 2U * index, (uint16_t)view.offsets[offset + index]);
    }
    response->length = (uint16_t)(11U + 2U * count);
    return FOC_PROTO_OK;
}

/* SET_ZERO: the present output position becomes zero. DISABLED with INHIBIT confirmed and
 * a fresh sensor, as for a parameter write; PARAM_SAVE stores it. */
static int foc_serial_set_zero(
    struct foc_serial *serial,
    struct foc_runtime *runtime,
    uint32_t now_us,
    struct foc_protocol_frame *response
)
{
    struct foc_runtime_snapshot view;
    if (foc_runtime_read_snapshot(runtime, &view) != FOC_RUNTIME_OK) {
        return FOC_PROTO_BUSY;
    }
    if (view.state != FOC_SAFE_STATE_DISABLED || view.inhibit_pending || !view.output_confirmed ||
        view.action != FOC_DEVICE_OUTPUT_INHIBIT) {
        return FOC_PROTO_BAD_STATE;
    }
    if (!view.sensor_valid ||
        !foc_safe_timestamp_fresh(now_us, view.sensor_us, runtime->config.angle_timeout_us)) {
        return FOC_PROTO_BUSY;
    }
    const float zero = view.mechanical_rad / foc_serial_gear_ratio(runtime);
    if (!isfinite(zero) || fabsf(zero) > FOC_SERIAL_OUTPUT_ZERO_MAX) {
        return FOC_PROTO_INVALID_ARGUMENT;
    }
    serial->output_zero_rad = zero;
    serial->dirty = true;
    foc_protocol_write_u32(response->payload + 1, foc_serial_float_bits(zero));
    response->length = 5;
    return FOC_PROTO_APPLIED;
}

static int
foc_serial_calibrate_read(const struct foc_serial *serial, struct foc_protocol_frame *response)
{
    struct foc_serial_calibration_view view;
    if (serial->calibration.read == NULL ||
        !serial->calibration.read(serial->calibration.context, &view)) {
        return FOC_PROTO_NOT_SUPPORTED;
    }
    uint8_t *body = response->payload;
    body[1] = view.state;
    body[2] = view.failure;
    body[3] = view.failed_state;
    body[4] = view.admission_failure;
    foc_protocol_write_u16(body + 5, view.samples);
    foc_protocol_write_u16(body + 7, view.pole_pairs);
    body[9] = (uint8_t)view.calibration_direction;
    body[10] = (uint8_t)view.runtime_direction;
    foc_protocol_write_u32(body + 11, foc_serial_float_bits(view.electrical_offset_rad));
    foc_protocol_write_u32(body + 15, foc_serial_float_bits(view.rotor_residual_rad));
    foc_protocol_write_u32(body + 19, foc_serial_float_bits(view.adc_offset_count[0]));
    foc_protocol_write_u32(body + 23, foc_serial_float_bits(view.adc_offset_count[1]));
    foc_protocol_write_u32(body + 27, foc_serial_float_bits(view.adc_offset_count[2]));
    foc_protocol_write_u32(body + 31, foc_serial_float_bits(view.adc_noise_rms_count));
    foc_protocol_write_u32(body + 35, view.applied_runs);
    const float electrical[] = {
        view.rs_ohm,
        view.ld_h,
        view.lq_h,
        view.flux_wb,
        view.kt_nm_per_a,
        view.kp_d_v_per_a,
        view.kp_q_v_per_a,
        view.ki_v_per_a_s
    };
    for (unsigned int index = 0U; index < sizeof electrical / sizeof electrical[0]; ++index) {
        foc_protocol_write_u32(body + 39 + 4U * index, foc_serial_float_bits(electrical[index]));
    }
    body[71] = view.committed_stages;
    body[72] = view.result_bits;
    body[73] = view.active_source;
    body[74] = view.ring_high_water;
    foc_protocol_write_u16(body + 75, view.active_pole_pairs);
    foc_protocol_write_u32(body + 77, foc_serial_float_bits(view.active_electrical_offset_rad));
    for (unsigned int index = 0U; index < 3U; ++index) {
        foc_protocol_write_u32(
            body + 81 + 4U * index,
            foc_serial_float_bits(view.active_adc_offset_count[index])
        );
    }
    response->length = 93;
    return FOC_PROTO_OK;
}

static int foc_serial_dispatch(
    struct foc_serial *serial,
    struct foc_uart_buffer *uart,
    struct foc_runtime *runtime,
    const struct foc_protocol_frame *request,
    uint32_t now_us,
    struct foc_protocol_frame *response
)
{
    if (foc_serial_calibrating(serial) && foc_serial_controls(request->type)) {
        return FOC_PROTO_BUSY;
    }
    if (request->type == FOC_MSG_PARAM_READ || request->type == FOC_MSG_PARAM_WRITE) {
        return foc_serial_parameter(serial, runtime, request, now_us, response);
    }
    if (request->type == FOC_MSG_SETPOINT) {
        const int result = foc_serial_setpoint(serial, runtime, request, now_us);
        if (result == FOC_PROTO_ACCEPTED) {
            foc_protocol_write_u32(response->payload + 1, serial->publication_seq);
            response->length = 5;
        }
        return result;
    }
    if (request->type == FOC_MSG_SET_MODE) {
        /* Mode selection changes software control state only; output policy is unchanged. */
        if (request->length != 1U || request->payload[0] > FOC_RUNTIME_MODE_IMPEDANCE) {
            return FOC_PROTO_INVALID_ARGUMENT;
        }
        return foc_serial_lifecycle(
            serial,
            runtime,
            FOC_RUNTIME_ACTION_SET_MODE,
            (enum foc_runtime_mode)request->payload[0],
            now_us,
            response
        );
    }
    if (request->type == FOC_MSG_CALIBRATE_START) {
        /* Optional stage mask: 1 rotor (CAL-1/2), 2 electrical (CAL-3/4/5); empty means both. */
        const unsigned int stages = request->length == 0U ? 3U : request->payload[0];
        /* 1-4 as before; 8 (CAL-7) alone or with the rotor/electrical bits (8-11). */
        if (request->length > 1U || stages < 1U ||
            (stages > 4U && (stages < 8U || stages > 11U) && stages != 16U && stages != 32U)) {
            return FOC_PROTO_INVALID_ARGUMENT;
        }
        return serial->calibration.start == NULL
            ? FOC_PROTO_NOT_SUPPORTED
            : serial->calibration.start(serial->calibration.context, now_us, stages);
    }
    if (request->type == FOC_MSG_COMMUTATION_WRITE) {
        if (serial->calibration.commutation == NULL) {
            return FOC_PROTO_NOT_SUPPORTED;
        }
        if (request->length != 7U) {
            return FOC_PROTO_INVALID_ARGUMENT;
        }
        const int result = serial->calibration.commutation(
            serial->calibration.context,
            now_us,
            foc_protocol_read_u16(request->payload),
            (int8_t)request->payload[2],
            foc_serial_bits_float(foc_protocol_read_u32(request->payload + 3))
        );
        if (result == FOC_PROTO_APPLIED) {
            serial->dirty = true;
        }
        return result;
    }
    if (request->type == FOC_MSG_COGGING_READ) {
        return foc_serial_cogging_read(runtime, request, response);
    }
    if (request->type == FOC_MSG_ENCODER_LUT_READ) {
        return foc_serial_encoder_lut_read(serial, request, response);
    }
    if (request->length != 0U) {
        return FOC_PROTO_INVALID_ARGUMENT;
    }
    switch (request->type) {
    case FOC_MSG_LIFECYCLE_READ:
        return foc_serial_lifecycle_read(serial, runtime, response);
    case FOC_MSG_CAPABILITY_READ:
        foc_serial_capability(serial, response);
        return FOC_PROTO_OK;
    case FOC_MSG_STATUS_READ:
    case FOC_MSG_FAULT_READ:
    case FOC_MSG_TELEMETRY_READ:
        return foc_serial_status(serial, uart, runtime, now_us, response);
    case FOC_MSG_FAULT_SNAPSHOT_READ:
        return foc_serial_fault_snapshot(runtime, response);
    case FOC_MSG_SET_ZERO:
        return foc_serial_set_zero(serial, runtime, now_us, response);
    case FOC_MSG_STOP:
        if (serial->calibration.abort != NULL) {
            serial->calibration.abort(serial->calibration.context);
        }
        foc_runtime_revoke_command(runtime);
        return FOC_PROTO_ACCEPTED;
    case FOC_MSG_CALIBRATE_READ:
        return foc_serial_calibrate_read(serial, response);
    case FOC_MSG_CALIBRATE_ABORT:
        if (serial->calibration.abort == NULL) {
            return FOC_PROTO_NOT_SUPPORTED;
        }
        serial->calibration.abort(serial->calibration.context);
        return FOC_PROTO_ACCEPTED;
    case FOC_MSG_ARM:
    case FOC_MSG_START: {
        if (serial->target_policy != FOC_SERIAL_TARGET_VIRTUAL_ONLY) {
            return FOC_PROTO_OUTPUT_LOCKED;
        }
        if (serial->calibration.gate != NULL) {
            const int admitted = serial->calibration.gate(serial->calibration.context);
            if (admitted != FOC_PROTO_OK) {
                return admitted;
            }
        }
        return foc_serial_lifecycle(
            serial,
            runtime,
            request->type == FOC_MSG_ARM ? FOC_RUNTIME_ACTION_ARM : FOC_RUNTIME_ACTION_START,
            FOC_RUNTIME_MODE_CURRENT,
            now_us,
            response
        );
    }
    case FOC_MSG_FAULT_CLEAR: {
        const int result = foc_runtime_request_timeout_clear(runtime, now_us);
        return result == FOC_RUNTIME_OK      ? FOC_PROTO_ACCEPTED
            : result == FOC_RUNTIME_ERR_BUSY ? FOC_PROTO_BUSY
                                             : FOC_PROTO_BAD_STATE;
    }
    case FOC_MSG_PARAM_SAVE:
    case FOC_MSG_CONFIG_ERASE:
        return foc_serial_store(serial, runtime, request->type, now_us, response);
    case FOC_MSG_TELEMETRY_STREAM:
        return FOC_PROTO_NOT_SUPPORTED;
    default:
        return FOC_PROTO_UNKNOWN_COMMAND;
    }
}

static int foc_serial_request(
    struct foc_serial *serial,
    struct foc_uart_buffer *uart,
    struct foc_runtime *runtime,
    const struct foc_protocol_frame *request,
    uint32_t now_us,
    struct foc_protocol_frame *response
)
{
    if (request->flags != FOC_PROTOCOL_REQUEST) {
        return FOC_PROTO_BAD_FRAME;
    }
    if (request->type == FOC_MSG_HELLO) {
        if (request->session != 0U || request->length != 0U) {
            return FOC_PROTO_INVALID_ARGUMENT;
        }
        if (serial->session == 0U || request->sequence != serial->hello_seq) {
            foc_runtime_revoke_command(runtime);
            serial->session++;
            if (serial->session == 0U) {
                serial->session = 1U;
            }
            serial->hello_seq = request->sequence;
            serial->request_seen = false;
        }
        response->session = serial->session;
        foc_serial_capability(serial, response);
        return FOC_PROTO_OK;
    }
    if (serial->session == 0U || request->session != serial->session) {
        return FOC_PROTO_BAD_SESSION;
    }
    if (!foc_safe_sequence_newer(request->sequence, serial->request_seq, serial->request_seen)) {
        return FOC_PROTO_REPLAY;
    }
    serial->request_seq = request->sequence;
    serial->request_seen = true;
    return foc_serial_dispatch(serial, uart, runtime, request, now_us, response);
}

static bool foc_serial_send(struct foc_uart_buffer *uart, const struct foc_protocol_frame *response)
{
    uint8_t wire[FOC_PROTOCOL_WIRE_MAX];
    const size_t length = foc_protocol_encode(response, wire, sizeof wire);
    return foc_uart_tx_submit(uart, wire, length);
}

/* Public Functions ------------------------------------------------------------- */

void foc_serial_init_with_policy(
    struct foc_serial *serial,
    const struct foc_runtime *runtime,
    enum foc_serial_target_policy policy
)
{
    *serial = (struct foc_serial){
        .current_limit_a = runtime->config.motion.iq_limit_a,
        .position_limit_rad = 100.0f,
        .rx_budget = FOC_SERIAL_RX_DEFAULT,
        .current_limit_ceiling_a = runtime->pipeline.current.config.current_limit_a,
        .target_policy = policy == FOC_SERIAL_TARGET_VIRTUAL_ONLY ? FOC_SERIAL_TARGET_VIRTUAL_ONLY
                                                                  : FOC_SERIAL_TARGET_LOCKED,
        .can = foc_serial_can_defaults(),
        .thermal = foc_serial_thermal_defaults(),
        .temperature = {NAN, NAN},
        .cogging = foc_serial_cogging_defaults(),
        .encoder = foc_serial_encoder_defaults(),
    };
    struct foc_runtime_lifecycle_result lifecycle;
    if (foc_runtime_read_lifecycle_result((struct foc_runtime *)runtime, &lifecycle) ==
        FOC_RUNTIME_OK) {
        serial->lifecycle_sequence = lifecycle.request_sequence;
    }
}

void foc_serial_init(struct foc_serial *serial, const struct foc_runtime *runtime)
{
    foc_serial_init_with_policy(serial, runtime, FOC_SERIAL_TARGET_LOCKED);
}

void foc_serial_attach_boot(
    struct foc_serial *serial,
    foc_serial_boot_reader reader,
    const void *context
)
{
    if (serial != NULL) {
        serial->boot_reader = reader;
        serial->boot_context = context;
    }
}

void foc_serial_attach_calibration(
    struct foc_serial *serial,
    const struct foc_serial_calibration *calibration
)
{
    if (serial != NULL) {
        serial->calibration =
            calibration != NULL ? *calibration : (struct foc_serial_calibration){0};
    }
}

void foc_serial_attach_persistence(
    struct foc_serial *serial,
    const struct foc_serial_persistence *persistence
)
{
    if (serial != NULL) {
        serial->persistence =
            persistence != NULL ? *persistence : (struct foc_serial_persistence){0};
    }
}

struct foc_serial_can_settings foc_serial_can_defaults(void)
{
    return (struct foc_serial_can_settings){
        .node_id = 63U,
        .bitrate = 1000000U,
        .heartbeat_ms = 100U,
        .estimates_ms = 10U,
        .protocol = FOC_CAN_PROTOCOL_CANSIMPLE,
        .master_id = 0U,
        .mit_p_max = 12.5f,
        .mit_v_max = 65.0f,
        .mit_t_max = 1.0f,
        .mit_kp_max = 500.0f,
        .mit_kd_max = 5.0f,
        .mit_reply = FOC_CAN_MIT_REPLY_ORIGINAL,
    };
}

static bool foc_serial_range(float value, float minimum, float maximum)
{
    return isfinite(value) && value >= minimum && value <= maximum;
}

bool foc_serial_can_settings_valid(const struct foc_serial_can_settings *settings)
{
    const uint32_t rate = settings->bitrate;
    return settings->node_id <= 63U &&
        (rate == 125000U || rate == 250000U || rate == 500000U || rate == 1000000U) &&
        settings->heartbeat_ms <= 10000U && settings->estimates_ms <= 10000U &&
        settings->protocol <= FOC_CAN_PROTOCOL_MIT && settings->master_id <= 2047U &&
        foc_serial_range(settings->mit_p_max, 0.01f, 1000.0f) &&
        foc_serial_range(settings->mit_v_max, 0.01f, 1000.0f) &&
        foc_serial_range(settings->mit_t_max, 0.001f, 1000.0f) &&
        foc_serial_range(settings->mit_kp_max, 0.001f, 10000.0f) &&
        foc_serial_range(settings->mit_kd_max, 0.001f, 1000.0f) &&
        settings->mit_reply <= FOC_CAN_MIT_REPLY_DAMIAO;
}

struct foc_serial_thermal_settings foc_serial_thermal_defaults(void)
{
    return (struct foc_serial_thermal_settings){
        .fet = {0U, 75.0f, 95.0f},
        .motor = {0U, 75.0f, 95.0f},
        .beta_k = 3950.0f,
        .r25_ohm = 10000.0f,
        .pullup_ohm = 10000.0f,
        .topology = 0U,
    };
}

static bool foc_serial_thermal_sensor_valid(const struct foc_serial_thermal_sensor *sensor)
{
    return sensor->enabled <= 1U && foc_serial_range(sensor->derate_c, -40.0f, 150.0f) &&
        foc_serial_range(sensor->limit_c, -40.0f, 150.0f) && sensor->derate_c < sensor->limit_c;
}

bool foc_serial_thermal_settings_valid(const struct foc_serial_thermal_settings *settings)
{
    return foc_serial_thermal_sensor_valid(&settings->fet) &&
        foc_serial_thermal_sensor_valid(&settings->motor) &&
        foc_serial_range(settings->beta_k, 1000.0f, 10000.0f) &&
        foc_serial_range(settings->r25_ohm, 100.0f, 1.0e6f) &&
        foc_serial_range(settings->pullup_ohm, 100.0f, 1.0e6f) && settings->topology <= 1U;
}

struct foc_serial_cogging_settings foc_serial_cogging_defaults(void)
{
    return (struct foc_serial_cogging_settings){.speed_rad_s = 0.25f, .keep_first_harmonic = 0U};
}

struct foc_serial_encoder_settings foc_serial_encoder_defaults(void)
{
    return (struct foc_serial_encoder_settings){.lut_enabled = 1U};
}

bool foc_serial_cogging_settings_valid(const struct foc_serial_cogging_settings *settings)
{
    return foc_serial_range(settings->speed_rad_s, 0.05f, 2.0f) &&
        settings->keep_first_harmonic <= 1U;
}

void foc_serial_attach_can_link(struct foc_serial *serial, const struct foc_serial_can_link *link)
{
    if (serial != NULL) {
        serial->can_link = link != NULL ? *link : (struct foc_serial_can_link){0};
    }
}

int foc_serial_execute(
    struct foc_serial *serial,
    struct foc_runtime *runtime,
    const struct foc_protocol_frame *request,
    uint32_t now_us,
    struct foc_protocol_frame *response
)
{
    *response = (struct foc_protocol_frame){
        .type = request->type,
        .flags = FOC_PROTOCOL_RESPONSE,
        .length = 1,
    };
    const int result = foc_serial_dispatch(serial, NULL, runtime, request, now_us, response);
    response->payload[0] = (uint8_t)result;
    if (result != FOC_PROTO_OK && result != FOC_PROTO_APPLIED && result != FOC_PROTO_ACCEPTED) {
        response->length = 1U;
    }
    return result;
}

int foc_serial_read_lifecycle_result(
    const struct foc_serial *serial,
    struct foc_runtime *runtime,
    struct foc_runtime_lifecycle_result *result
)
{
    (void)serial;
    return foc_runtime_read_lifecycle_result(runtime, result);
}

void foc_serial_poll(
    struct foc_serial *serial,
    struct foc_uart_buffer *uart,
    struct foc_runtime *runtime,
    foc_serial_clock_t clock,
    void *argument
)
{
    if (serial->stop_pending && foc_uart_tx_available(uart)) {
        const struct foc_protocol_frame response = {
            .type = FOC_MSG_STOP,
            .flags = FOC_PROTOCOL_RESPONSE,
            .sequence = serial->stop_seq,
            .session = serial->stop_session,
            .length = 1,
            .payload = {FOC_PROTO_ACCEPTED},
        };
        if (foc_serial_send(uart, &response)) {
            serial->stop_pending = false;
        }
    }
    for (uint32_t i = 0; i < serial->rx_budget; ++i) {
        uint16_t value;
        if (!foc_uart_rx_pop(uart, &value)) {
            return;
        }
        if (value == FOC_UART_RX_GAP) {
            serial->stream = (struct foc_protocol_stream){0};
            serial->dropped++;
            continue;
        }
        struct foc_protocol_frame request;
        const int parsed = foc_protocol_feed(&serial->stream, (uint8_t)value, &request);
        if (parsed == FOC_PROTO_MORE) {
            continue;
        }
        if (parsed != FOC_PROTO_OK && parsed != FOC_PROTO_UNKNOWN_COMMAND) {
            serial->crc_errors += parsed == FOC_PROTO_BAD_CRC;
            serial->dropped++;
            return;
        }
        const bool available = foc_uart_tx_available(uart);
        const bool stop = request.type == FOC_MSG_STOP && request.length == 0U &&
            request.flags == FOC_PROTOCOL_REQUEST && request.session == serial->session &&
            serial->session != 0U &&
            foc_safe_sequence_newer(request.sequence, serial->request_seq, serial->request_seen);
        if (!available && (!stop || serial->stop_pending)) {
            serial->dropped++;
            return;
        }
        struct foc_protocol_frame response = {
            .type = request.type,
            .flags = FOC_PROTOCOL_RESPONSE,
            .sequence = request.sequence,
            .session = request.session,
            .length = 1,
        };
        const int result =
            foc_serial_request(serial, uart, runtime, &request, clock(argument), &response);
        response.payload[0] = (uint8_t)result;
        if (result != FOC_PROTO_OK && result != FOC_PROTO_APPLIED && result != FOC_PROTO_ACCEPTED) {
            response.length = 1U;
        }
        if (available) {
            if (!foc_serial_send(uart, &response)) {
                serial->dropped++;
            }
        } else {
            serial->stop_seq = request.sequence;
            serial->stop_session = request.session;
            serial->stop_pending = true;
        }
        return;
    }
}

/* ---------------------------------------- end of file ----------------------------------------- */
