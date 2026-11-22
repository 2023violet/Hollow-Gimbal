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

#include <float.h>
#include <math.h>
#include <string.h>

/* Private Constants ------------------------------------------------------------ */

enum {
    PARAM_RX_BUDGET = 1,
    PARAM_CURRENT_LIMIT = 2,
    PARAM_POSITION_LIMIT = 3,
    PARAM_POLE_PAIRS = 16,
    PARAM_ELECTRICAL_OFFSET = 17,
    PARAM_VELOCITY_LIMIT = 18,
    /* Read-only startup readback, valid only with an attached ready report. */
    PARAM_BOOT_CONFIGURATION = 32,
    PARAM_CALIBRATION_INPUT = 33,
    PARAM_CALIBRATION_REJECTION = 34,
    PARAM_ADMISSION_FAILURE = 35,
    PARAM_SUPPLY_ID = 36,
    PARAM_REVIEW_ID = 37,
    PARAM_PHASE_A_OFFSET = 38,
    PARAM_PHASE_B_OFFSET = 39,
    PARAM_PHASE_C_OFFSET = 40,
    PARAM_ENCODER_CPR = 41,
    PARAM_RUNTIME_DIRECTION = 42,
    PARAM_SOURCE_DIRECTION = 43,
    PARAM_CALIBRATION_DIRECTION = 44,
    PARAM_MEASUREMENT_QUALIFIED = 45,
    /* Controller tuning receipt, read only. */
    PARAM_TUNING_REVISION = 60,
    PARAM_TUNING_RESULT = 61,
    PARAM_TUNING_SEQUENCE = 62,
    /* Controller tuning, writable while DISABLED; applied by H0. */
    PARAM_CURRENT_KP_D = 64,
    PARAM_CURRENT_KP_Q = 65,
    PARAM_CURRENT_KI_D = 66,
    PARAM_CURRENT_KI_Q = 67,
    PARAM_CURRENT_VECTOR_LIMIT = 68,
    PARAM_VELOCITY_KP = 69,
    PARAM_VELOCITY_KI = 70,
    PARAM_POSITION_KP = 71,
    PARAM_VELOCITY_REFERENCE_LIMIT = 72,
    PARAM_IQ_LIMIT = 73,
    PARAM_COMMAND_LEASE = 74,
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
    /* Capability bits: lock, RAM policy, polling telemetry, UART, mode switch, tuning. */
    CAPABILITY_BITS = 63,
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

static void foc_serial_capability(struct foc_protocol_frame *response)
{
    const uint8_t capability[] = {FOC_PROTO_OK, 1, 0, 1, 0, 7, 0, CAPABILITY_BITS, 0};
    memcpy(response->payload, capability, sizeof capability);
    response->length = sizeof capability;
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
        foc_protocol_write_u32(body + 38, foc_serial_float_bits(view.mechanical_rad));
        foc_protocol_write_u32(body + 42, foc_serial_float_bits(view.mechanical_rad_s));
        foc_protocol_write_u32(body + 46, view.sensor_us);
        foc_protocol_write_u32(body + 50, serial->crc_errors);
        foc_protocol_write_u32(body + 54, serial->dropped);
        foc_protocol_write_u32(body + 58, atomic_load(&uart->rx_lost));
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
        response->length = 75;
    }
    return FOC_PROTO_OK;
}

static uint32_t foc_serial_direction_code(int32_t direction)
{
    return direction == 1 ? DIRECTION_POSITIVE
        : direction == -1 ? DIRECTION_NEGATIVE
                          : DIRECTION_NONE;
}

/* Values come from the bound live Runtime and encoder policy, never a copy. */
static bool foc_serial_boot_parameter(
    const struct foc_serial *serial,
    uint16_t id,
    struct foc_serial_parameter *parameter
)
{
    struct foc_serial_boot_readback view;
    if (id < PARAM_BOOT_CONFIGURATION || id > PARAM_MEASUREMENT_QUALIFIED ||
        serial->boot_reader == NULL || !serial->boot_reader(serial->boot_context, &view)) {
        return false;
    }
    *parameter = (struct foc_serial_parameter){.type = PARAM_U32, .unit = UNIT_NONE};
    switch (id) {
    case PARAM_BOOT_CONFIGURATION:
        parameter->value = view.configuration;
        break;
    case PARAM_CALIBRATION_INPUT:
        parameter->value = view.calibration;
        break;
    case PARAM_CALIBRATION_REJECTION:
        parameter->value = view.rejection;
        break;
    case PARAM_ADMISSION_FAILURE:
        parameter->value = view.admission_failure;
        break;
    case PARAM_SUPPLY_ID:
        parameter->value = view.supply_id;
        break;
    case PARAM_REVIEW_ID:
        parameter->value = view.review_id;
        break;
    case PARAM_PHASE_A_OFFSET:
    case PARAM_PHASE_B_OFFSET:
    case PARAM_PHASE_C_OFFSET:
        parameter->type = PARAM_F32;
        parameter->unit = UNIT_COUNT;
        parameter->value =
            foc_serial_float_bits(view.phase_offset_count[id - PARAM_PHASE_A_OFFSET]);
        parameter->calibration_required = true;
        break;
    case PARAM_ENCODER_CPR:
        parameter->value = view.encoder_counts_per_revolution;
        break;
    case PARAM_RUNTIME_DIRECTION:
        parameter->value = foc_serial_direction_code(view.runtime_direction);
        parameter->calibration_required = true;
        break;
    case PARAM_SOURCE_DIRECTION:
        parameter->value = foc_serial_direction_code(view.source_direction);
        break;
    case PARAM_CALIBRATION_DIRECTION:
        parameter->value = foc_serial_direction_code(view.calibration_direction);
        parameter->calibration_required = true;
        break;
    default:
        parameter->value = view.current_measurement_qualified ? 1U : 0U;
        break;
    }
    parameter->minimum = parameter->maximum = parameter->value;
    return true;
}

static void foc_serial_tuning_float(
    struct foc_serial_parameter *parameter,
    float value,
    float minimum,
    float maximum,
    uint8_t unit
)
{
    *parameter = (struct foc_serial_parameter){
        .value = foc_serial_float_bits(value),
        .minimum = foc_serial_float_bits(minimum),
        .maximum = foc_serial_float_bits(maximum),
        .type = PARAM_F32,
        .unit = unit,
        .writable = true,
        .tuning = true,
    };
}

static uint32_t foc_serial_request_code(enum foc_runtime_request_result result)
{
    return (uint32_t)result; /* NONE 0, PENDING 1, APPLIED 2, REJECTED 3, CANCELLED 4 */
}

/* Controller tuning and its receipt, from the live Runtime. */
static bool foc_serial_tuning_parameter(
    const struct foc_serial *serial,
    const struct foc_runtime *runtime,
    uint16_t id,
    struct foc_serial_parameter *parameter
)
{
    struct foc_runtime_tuning tuning;
    struct foc_runtime_tuning_result receipt;
    if (id < PARAM_TUNING_REVISION || id > PARAM_COMMAND_LEASE ||
        foc_runtime_read_tuning(runtime, &tuning) != FOC_RUNTIME_OK ||
        foc_runtime_read_tuning_result((struct foc_runtime *)runtime, &receipt) != FOC_RUNTIME_OK) {
        return false;
    }
    switch (id) {
    case PARAM_TUNING_REVISION:
    case PARAM_TUNING_RESULT:
    case PARAM_TUNING_SEQUENCE:
        *parameter = (struct foc_serial_parameter){.type = PARAM_U32, .unit = UNIT_NONE};
        parameter->value = id == PARAM_TUNING_REVISION ? receipt.revision
            : id == PARAM_TUNING_RESULT                ? foc_serial_request_code(receipt.result)
                                                       : receipt.request_sequence;
        parameter->minimum = parameter->maximum = parameter->value;
        return true;
    case PARAM_CURRENT_KP_D:
    case PARAM_CURRENT_KP_Q:
        foc_serial_tuning_float(
            parameter,
            id == PARAM_CURRENT_KP_D ? tuning.current_kp_v_per_a.d : tuning.current_kp_v_per_a.q,
            0.0f,
            20.0f,
            UNIT_V_PER_A
        );
        return true;
    case PARAM_CURRENT_KI_D:
    case PARAM_CURRENT_KI_Q:
        foc_serial_tuning_float(
            parameter,
            id == PARAM_CURRENT_KI_D ? tuning.current_ki_v_per_a_s.d
                                     : tuning.current_ki_v_per_a_s.q,
            0.0f,
            20000.0f,
            UNIT_V_PER_A_S
        );
        return true;
    case PARAM_CURRENT_VECTOR_LIMIT:
        foc_serial_tuning_float(
            parameter,
            tuning.current_limit_a,
            0.05f,
            serial->current_limit_ceiling_a,
            UNIT_AMPERE
        );
        return true;
    case PARAM_VELOCITY_KP:
        foc_serial_tuning_float(
            parameter,
            tuning.velocity_kp_a_per_rad_s,
            0.0f,
            10.0f,
            UNIT_A_PER_RAD_S
        );
        return true;
    case PARAM_VELOCITY_KI:
        foc_serial_tuning_float(
            parameter,
            tuning.velocity_ki_a_per_rad,
            0.0f,
            1000.0f,
            UNIT_A_PER_RAD
        );
        return true;
    case PARAM_POSITION_KP:
        foc_serial_tuning_float(
            parameter,
            tuning.position_kp_per_s,
            0.0f,
            1000.0f,
            UNIT_PER_SECOND
        );
        return true;
    case PARAM_VELOCITY_REFERENCE_LIMIT:
        foc_serial_tuning_float(parameter, tuning.velocity_limit_rad_s, 0.1f, 1000.0f, UNIT_RAD_S);
        return true;
    case PARAM_IQ_LIMIT:
        foc_serial_tuning_float(
            parameter,
            tuning.iq_limit_a,
            0.05f,
            tuning.current_limit_a,
            UNIT_AMPERE
        );
        return true;
    case PARAM_COMMAND_LEASE:
        *parameter = (struct foc_serial_parameter){
            .value = tuning.command_timeout_us,
            .minimum = FOC_RUNTIME_LEASE_MIN_US,
            .maximum = FOC_RUNTIME_LEASE_MAX_US,
            .type = PARAM_U32,
            .unit = UNIT_MICROSECOND,
            .writable = true,
            .tuning = true,
        };
        return true;
    default:
        return false;
    }
}

/* Replace one field of the live tuning and publish the whole candidate to H0. */
static int foc_serial_tuning_write(
    struct foc_serial *serial,
    struct foc_runtime *runtime,
    uint16_t id,
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
    const float floating = foc_serial_bits_float(value);
    switch (id) {
    case PARAM_CURRENT_KP_D:
        tuning.current_kp_v_per_a.d = floating;
        break;
    case PARAM_CURRENT_KP_Q:
        tuning.current_kp_v_per_a.q = floating;
        break;
    case PARAM_CURRENT_KI_D:
        tuning.current_ki_v_per_a_s.d = floating;
        break;
    case PARAM_CURRENT_KI_Q:
        tuning.current_ki_v_per_a_s.q = floating;
        break;
    case PARAM_CURRENT_VECTOR_LIMIT:
        tuning.current_limit_a = floating;
        break;
    case PARAM_VELOCITY_KP:
        tuning.velocity_kp_a_per_rad_s = floating;
        break;
    case PARAM_VELOCITY_KI:
        tuning.velocity_ki_a_per_rad = floating;
        break;
    case PARAM_POSITION_KP:
        tuning.position_kp_per_s = floating;
        break;
    case PARAM_VELOCITY_REFERENCE_LIMIT:
        tuning.velocity_limit_rad_s = floating;
        break;
    case PARAM_IQ_LIMIT:
        tuning.iq_limit_a = floating;
        break;
    case PARAM_COMMAND_LEASE:
        tuning.command_timeout_us = value;
        break;
    default:
        return FOC_PROTO_INTERNAL_ERROR;
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
    return FOC_PROTO_ACCEPTED;
}

static bool foc_serial_parameter_read(
    const struct foc_serial *serial,
    const struct foc_runtime *runtime,
    uint16_t id,
    struct foc_serial_parameter *parameter
)
{
    if (foc_serial_boot_parameter(serial, id, parameter) ||
        foc_serial_tuning_parameter(serial, runtime, id, parameter)) {
        return true;
    }
    *parameter = (struct foc_serial_parameter){.type = PARAM_F32};
    switch (id) {
    case PARAM_RX_BUDGET:
        *parameter = (struct foc_serial_parameter){
            .value = serial->rx_budget,
            .minimum = 8,
            .maximum = 32,
            .type = PARAM_U32,
            .unit = UNIT_NONE,
            .writable = true,
        };
        break;
    case PARAM_CURRENT_LIMIT:
        parameter->value = foc_serial_float_bits(serial->current_limit_a);
        parameter->minimum = foc_serial_float_bits(0.05f);
        parameter->maximum = foc_serial_float_bits(runtime->config.motion.iq_limit_a);
        parameter->unit = UNIT_AMPERE;
        parameter->writable = true;
        break;
    case PARAM_POSITION_LIMIT:
        parameter->value = foc_serial_float_bits(serial->position_limit_rad);
        parameter->minimum = foc_serial_float_bits(1.0f);
        parameter->maximum = foc_serial_float_bits(1000.0f);
        parameter->unit = UNIT_RADIAN;
        parameter->writable = true;
        break;
    case PARAM_POLE_PAIRS:
        parameter->type = PARAM_U32;
        parameter->value = runtime->config.motion.rotor.pole_pairs;
        parameter->calibration_required = true;
        break;
    case PARAM_ELECTRICAL_OFFSET:
        parameter->value =
            foc_serial_float_bits(runtime->config.motion.rotor.electrical_offset_rad);
        parameter->unit = UNIT_RADIAN;
        parameter->calibration_required = true;
        break;
    case PARAM_VELOCITY_LIMIT:
        parameter->value =
            foc_serial_float_bits(runtime->config.motion.position.velocity_limit_rad_s);
        parameter->unit = UNIT_RAD_S;
        break;
    default:
        return false;
    }
    if (!parameter->writable) {
        parameter->minimum = parameter->maximum = parameter->value;
    }
    return true;
}

static int foc_serial_parameter(
    struct foc_serial *serial,
    struct foc_runtime *runtime,
    const struct foc_protocol_frame *request,
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
        if (parameter.tuning) {
            const int published = foc_serial_tuning_write(serial, runtime, id, value);
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
        switch (id) {
        case PARAM_RX_BUDGET:
            serial->rx_budget = value;
            break;
        case PARAM_CURRENT_LIMIT:
            serial->current_limit_a = foc_serial_bits_float(value);
            break;
        case PARAM_POSITION_LIMIT:
            serial->position_limit_rad = foc_serial_bits_float(value);
            break;
        default:
            return FOC_PROTO_INTERNAL_ERROR;
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

static int foc_serial_setpoint(
    struct foc_serial *serial,
    struct foc_runtime *runtime,
    const struct foc_protocol_frame *request,
    uint32_t now_us
)
{
    if (request->length != 5U && request->length != 9U) {
        return FOC_PROTO_INVALID_ARGUMENT;
    }
    const uint8_t mode = request->payload[0];
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
    if ((mode == FOC_RUNTIME_MODE_CURRENT &&
         first * first + second * second > current_cap * current_cap) ||
        (mode == FOC_RUNTIME_MODE_VELOCITY &&
         fabsf(first) > runtime->config.motion.position.velocity_limit_rad_s) ||
        (mode == FOC_RUNTIME_MODE_POSITION && fabsf(first) > serial->position_limit_rad)) {
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
            .target.motion_ref = first,
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

static int foc_serial_dispatch(
    struct foc_serial *serial,
    struct foc_uart_buffer *uart,
    struct foc_runtime *runtime,
    const struct foc_protocol_frame *request,
    uint32_t now_us,
    struct foc_protocol_frame *response
)
{
    if (request->type == FOC_MSG_PARAM_READ || request->type == FOC_MSG_PARAM_WRITE) {
        return foc_serial_parameter(serial, runtime, request, response);
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
        if (request->length != 1U || request->payload[0] > FOC_RUNTIME_MODE_POSITION) {
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
    if (request->length != 0U) {
        return FOC_PROTO_INVALID_ARGUMENT;
    }
    switch (request->type) {
    case FOC_MSG_LIFECYCLE_READ:
        return foc_serial_lifecycle_read(serial, runtime, response);
    case FOC_MSG_CAPABILITY_READ:
        foc_serial_capability(response);
        return FOC_PROTO_OK;
    case FOC_MSG_STATUS_READ:
    case FOC_MSG_FAULT_READ:
    case FOC_MSG_TELEMETRY_READ:
        return foc_serial_status(serial, uart, runtime, now_us, response);
    case FOC_MSG_STOP:
        foc_runtime_revoke_command(runtime);
        return FOC_PROTO_ACCEPTED;
    case FOC_MSG_ARM:
    case FOC_MSG_START: {
        if (serial->target_policy != FOC_SERIAL_TARGET_VIRTUAL_ONLY) {
            return FOC_PROTO_OUTPUT_LOCKED;
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
        foc_serial_capability(response);
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
