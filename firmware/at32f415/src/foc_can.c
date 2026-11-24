/**
 * @file    foc_can.c
 * @brief   CANSimple translation onto the Foreground request path (contract 25).
 * @version v0.1.0
 * @date    2026.11.24
 *
 * Implementation role
 * -------------------
 * Decode one CANSimple frame, build the matching V1 request and run it through
 * foc_serial_execute(). Positions are output turns, velocities output turns/s and
 * torques output N·m; the Runtime works in rotor rad, so values pass through the gear
 * ratio and the output zero (ID 124) as UART values do.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_can.h"

#include <math.h>
#include <string.h>

/* Private Constants ------------------------------------------------------------ */

#define CAN_TWO_PI            6.2831853f
/* Closed-loop requests retry ARM/START until fresh sensing admits them, within this window. */
#define CAN_SEQUENCE_US       250000U
#define CAN_SEQUENCE_RETRY_US 1000U
/* Trajectory-done tolerance in output rad and output rad/s. */
#define CAN_TRAJ_DONE_RAD     0.01f
#define CAN_TRAJ_DONE_RAD_S   0.05f

enum {
    PARAM_U32 = 1,
    PARAM_F32 = 2,
    PARAM_CURRENT_LIMIT = 2,
    PARAM_VELOCITY_KP = 69,
    PARAM_VELOCITY_KI = 70,
    PARAM_POSITION_KP = 71,
    PARAM_VELOCITY_REFERENCE_LIMIT = 72,
    PARAM_ACCEL = 106,
    PARAM_DECEL = 107,
    PARAM_CAN_NODE_ID = 120,
    PARAM_OUTPUT_ZERO = 124,
    /* Get_Version: protocol 2, Board1 hardware 1.0, FalconFOC V2 firmware 2.0.0 (unreleased). */
    VERSION_PROTOCOL = 2,
    VERSION_HW_MAJOR = 1,
    VERSION_FW_MAJOR = 2,
};

/* Private Helpers -------------------------------------------------------------- */

static void can_put_u16(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
}

static void can_put_u32(uint8_t *data, uint32_t value)
{
    for (unsigned int index = 0U; index < 4U; ++index) {
        data[index] = (uint8_t)(value >> (8U * index));
    }
}

static void can_put_f32(uint8_t *data, float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    can_put_u32(data, bits);
}

static uint32_t can_get_u32(const uint8_t *data)
{
    return (uint32_t)data[0] | (uint32_t)data[1] << 8 | (uint32_t)data[2] << 16 |
        (uint32_t)data[3] << 24;
}

static float can_get_f32(const uint8_t *data)
{
    const uint32_t bits = can_get_u32(data);
    float value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

static float can_gear(const struct foc_runtime *runtime)
{
    const float gear = runtime->config.profile.gear_ratio;
    return isfinite(gear) && gear >= 1.0f ? gear : 1.0f;
}

/* Output torque per q ampere: Kt (1.5 x pole pairs x flux) times the gear ratio; 0 unknown. */
static float can_torque_constant(const struct foc_runtime *runtime)
{
    struct foc_runtime_tuning tuning;
    if (foc_runtime_read_tuning(runtime, &tuning) != FOC_RUNTIME_OK) {
        return 0.0f;
    }
    const float kt = 1.5f * (float)runtime->config.motion.rotor.pole_pairs * tuning.profile.flux_wb;
    return isfinite(kt) && kt > 0.0f ? kt * can_gear(runtime) : 0.0f;
}

static void
can_queue(struct foc_can *can, uint8_t node, uint8_t command, const uint8_t *data, uint8_t length)
{
    if (can->tx_count == FOC_CAN_TX_CAPACITY) {
        can->dropped++;
        return;
    }
    struct foc_can_frame *frame = &can->tx[(can->tx_head + can->tx_count) % FOC_CAN_TX_CAPACITY];
    *frame = (struct foc_can_frame){
        .id = (uint16_t)((uint16_t)node << 5 | command),
        .length = length,
    };
    memcpy(frame->data, data, length);
    can->tx_count++;
}

static void can_flush(struct foc_can *can)
{
    while (can->tx_count != 0U && can->port.send != NULL &&
           can->port.send(can->port.context, &can->tx[can->tx_head])) {
        can->tx_head = (can->tx_head + 1U) % FOC_CAN_TX_CAPACITY;
        can->tx_count--;
    }
}

static int can_run(
    struct foc_serial *serial,
    struct foc_runtime *runtime,
    uint8_t type,
    const uint8_t *payload,
    uint16_t length,
    uint32_t now_us,
    struct foc_protocol_frame *response
)
{
    struct foc_protocol_frame request = {
        .type = type,
        .flags = FOC_PROTOCOL_REQUEST,
        .length = length,
    };
    if (length != 0U) {
        memcpy(request.payload, payload, length);
    }
    return foc_serial_execute(serial, runtime, &request, now_us, response);
}

static bool can_accepted(int result)
{
    return result == FOC_PROTO_OK || result == FOC_PROTO_ACCEPTED || result == FOC_PROTO_APPLIED;
}

/* PARAM_WRITE through the UART admission; the type comes from a PARAM_READ first. */
static int can_param_write(
    struct foc_serial *serial,
    struct foc_runtime *runtime,
    uint16_t id,
    uint32_t value,
    uint32_t now_us,
    struct foc_protocol_frame *response
)
{
    uint8_t payload[7];
    can_put_u16(payload, id);
    const int read = can_run(serial, runtime, FOC_MSG_PARAM_READ, payload, 2U, now_us, response);
    if (read != FOC_PROTO_OK) {
        return read;
    }
    payload[2] = response->payload[3];
    can_put_u32(payload + 3, value);
    return can_run(serial, runtime, FOC_MSG_PARAM_WRITE, payload, 7U, now_us, response);
}

static int can_param_write_f32(
    struct foc_serial *serial,
    struct foc_runtime *runtime,
    uint16_t id,
    float value,
    uint32_t now_us
)
{
    struct foc_protocol_frame response;
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    return isfinite(value) ? can_param_write(serial, runtime, id, bits, now_us, &response)
                           : FOC_PROTO_INVALID_ARGUMENT;
}

static int can_setpoint(
    struct foc_serial *serial,
    struct foc_runtime *runtime,
    uint8_t mode,
    float first,
    float second,
    uint32_t now_us
)
{
    uint8_t payload[9] = {mode};
    can_put_f32(payload + 1, first);
    can_put_f32(payload + 5, second);
    struct foc_protocol_frame response;
    return can_run(
        serial,
        runtime,
        FOC_MSG_SETPOINT,
        payload,
        mode == FOC_RUNTIME_MODE_CURRENT ? 9U : 5U,
        now_us,
        &response
    );
}

static uint8_t can_node(const struct foc_serial *serial)
{
    return (uint8_t)(serial->can.node_id & 0x3FU);
}

static uint8_t can_calibration_procedure(uint8_t failure)
{
    switch (failure) {
    case 11U: /* ABORTED */
        return FOC_CAN_PROCEDURE_CANCELLED;
    case 12U: /* TIMEOUT */
        return FOC_CAN_PROCEDURE_TIMEOUT;
    case 1U: /* STATE */
    case 3U: /* LIFECYCLE */
        return FOC_CAN_PROCEDURE_INVALID_STATE;
    case 4U: /* DRIVE */
        return FOC_CAN_PROCEDURE_DISARMED;
    case 6U: /* SENSOR */
        return FOC_CAN_PROCEDURE_NO_RESPONSE;
    case 7U: /* ROTOR */
        return FOC_CAN_PROCEDURE_POLE_PAIR_CPR_MISMATCH;
    case 13U: /* RL */
        return FOC_CAN_PROCEDURE_PHASE_RESISTANCE_OUT_OF_RANGE;
    default:
        return FOC_CAN_PROCEDURE_NOT_CONVERGING;
    }
}

static uint32_t can_errors(const struct foc_can *can, uint32_t faults)
{
    return foc_can_errors_from_faults(faults) | (can->estop ? FOC_CAN_ERROR_ESTOP_REQUESTED : 0U) |
        (can->calibration_error ? FOC_CAN_ERROR_CALIBRATION : 0U);
}

static bool can_calibrating(const struct foc_serial *serial)
{
    return serial->calibration.busy != NULL &&
        serial->calibration.busy(serial->calibration.context);
}

/* Closed loop: publish ARM, then START once ARM is applied. */
static void can_sequence_publish(
    struct foc_can *can,
    struct foc_serial *serial,
    struct foc_runtime *runtime,
    uint32_t now_us
)
{
    struct foc_protocol_frame response;
    const uint8_t type = can->sequence == 1U ? FOC_MSG_ARM : FOC_MSG_START;
    const int result = can_run(serial, runtime, type, NULL, 0U, now_us, &response);
    can->sequence_retry_us = now_us;
    if (result == FOC_PROTO_ACCEPTED) {
        can->sequence_request = can_get_u32(response.payload + 1);
        return;
    }
    if (result == FOC_PROTO_BUSY) {
        can->sequence_request = 0U;
        return;
    }
    can->procedure_result = result == FOC_PROTO_NOT_CALIBRATED ? FOC_CAN_PROCEDURE_NOT_CALIBRATED
                                                               : FOC_CAN_PROCEDURE_INVALID_STATE;
    can->sequence = 0U;
    can->refused++;
}

static void can_sequence_poll(
    struct foc_can *can,
    struct foc_serial *serial,
    struct foc_runtime *runtime,
    uint32_t now_us
)
{
    if (can->sequence == 0U) {
        return;
    }
    if (now_us - can->sequence_started_us > CAN_SEQUENCE_US) {
        can->sequence = 0U;
        can->procedure_result = FOC_CAN_PROCEDURE_INVALID_STATE;
        return;
    }
    struct foc_runtime_lifecycle_result result;
    if (can->sequence_request != 0U &&
        foc_serial_read_lifecycle_result(serial, runtime, &result) == FOC_RUNTIME_OK &&
        result.request_sequence == can->sequence_request) {
        if (result.result == FOC_RUNTIME_REQUEST_PENDING ||
            result.result == FOC_RUNTIME_REQUEST_NONE) {
            return;
        }
        if (result.result == FOC_RUNTIME_REQUEST_APPLIED) {
            if (can->sequence == 2U) {
                can->sequence = 0U;
                can->procedure_result = FOC_CAN_PROCEDURE_SUCCESS;
                return;
            }
            can->sequence = 2U;
            can_sequence_publish(can, serial, runtime, now_us);
            return;
        }
        can->sequence_request = 0U; /* refused by H0: retry after fresh samples */
    }
    if (can->sequence_request == 0U && now_us - can->sequence_retry_us >= CAN_SEQUENCE_RETRY_US) {
        can_sequence_publish(can, serial, runtime, now_us);
    }
}

static void can_calibration_poll(struct foc_can *can, struct foc_serial *serial)
{
    struct foc_serial_calibration_view view;
    if (can->calibrating_state == 0U || can_calibrating(serial)) {
        return;
    }
    can->calibrating_state = 0U;
    if (serial->calibration.read == NULL ||
        !serial->calibration.read(serial->calibration.context, &view)) {
        return;
    }
    if (view.state == 9U) { /* DONE */
        can->procedure_result = FOC_CAN_PROCEDURE_SUCCESS;
        return;
    }
    can->procedure_result = can_calibration_procedure(view.failure);
    can->calibration_error = view.failure != 11U;
}

static void can_set_axis_state(
    struct foc_can *can,
    struct foc_serial *serial,
    struct foc_runtime *runtime,
    uint32_t requested,
    uint32_t now_us
)
{
    struct foc_protocol_frame response;
    if (requested == FOC_CAN_AXIS_IDLE) {
        if (can->calibrating_state != 0U) {
            can->procedure_result = FOC_CAN_PROCEDURE_CANCELLED;
            can->calibrating_state = 0U;
        }
        can->sequence = 0U;
        (void)can_run(serial, runtime, FOC_MSG_STOP, NULL, 0U, now_us, &response);
        return;
    }
    if (requested == FOC_CAN_AXIS_CLOSED_LOOP) {
        struct foc_runtime_snapshot view;
        if (foc_runtime_read_snapshot(runtime, &view) == FOC_RUNTIME_OK &&
            view.state == FOC_SAFE_STATE_RUNNING) {
            return;
        }
        can->sequence = 1U;
        can->sequence_started_us = now_us;
        can->procedure_result = FOC_CAN_PROCEDURE_BUSY;
        can_sequence_publish(can, serial, runtime, now_us);
        return;
    }
    const uint8_t stages = requested == FOC_CAN_AXIS_FULL_CALIBRATION ? 3U
        : requested == FOC_CAN_AXIS_MOTOR_CALIBRATION                 ? 2U
        : requested == FOC_CAN_AXIS_ENCODER_OFFSET_CALIBRATION        ? 1U
                                                                      : 0U;
    const int result = stages == 0U
        ? FOC_PROTO_INVALID_ARGUMENT
        : can_run(serial, runtime, FOC_MSG_CALIBRATE_START, &stages, 1U, now_us, &response);
    if (!can_accepted(result)) {
        can->procedure_result = FOC_CAN_PROCEDURE_INVALID_STATE;
        can->refused++;
        return;
    }
    can->calibrating_state = (uint8_t)requested;
    can->calibration_error = false;
    can->procedure_result = FOC_CAN_PROCEDURE_BUSY;
}

/* Set_Controller_Mode: torque 1, velocity 2, position 3; passthrough, velocity ramp or
 * trapezoidal input (the ramp itself follows IDs 106/107). */
static int can_set_mode(
    struct foc_serial *serial,
    struct foc_runtime *runtime,
    uint32_t control,
    uint32_t input,
    uint32_t now_us
)
{
    const bool input_ok =
        input == 1U || (input == 2U && control == 2U) || (input == 5U && control == 3U);
    if (control < 1U || control > 3U || !input_ok) {
        return FOC_PROTO_INVALID_ARGUMENT;
    }
    const uint8_t mode = control == 1U ? FOC_RUNTIME_MODE_CURRENT
        : control == 2U                ? FOC_RUNTIME_MODE_VELOCITY
                                       : FOC_RUNTIME_MODE_POSITION;
    struct foc_protocol_frame response;
    return can_run(serial, runtime, FOC_MSG_SET_MODE, &mode, 1U, now_us, &response);
}

static void
can_reply_sdo(struct foc_can *can, uint8_t node, int result, uint16_t id, uint32_t value)
{
    uint8_t data[8] = {(uint8_t)result};
    can_put_u16(data + 1, id);
    can_put_u32(data + 4, value);
    can_queue(can, node, FOC_CAN_TX_SDO, data, 8U);
}

static void can_sdo(
    struct foc_can *can,
    struct foc_serial *serial,
    struct foc_runtime *runtime,
    const struct foc_can_frame *frame,
    uint32_t now_us
)
{
    const uint16_t id = (uint16_t)(frame->data[1] | frame->data[2] << 8);
    const uint32_t value = can_get_u32(frame->data + 4);
    struct foc_protocol_frame response;
    uint8_t payload[2];
    can_put_u16(payload, id);
    int result;
    if (frame->data[0] == 0U) {
        result = can_run(serial, runtime, FOC_MSG_PARAM_READ, payload, 2U, now_us, &response);
    } else if (frame->data[0] == 1U) {
        result = can_param_write(serial, runtime, id, value, now_us, &response);
    } else {
        result = FOC_PROTO_INVALID_ARGUMENT;
        response.length = 1U;
    }
    if (!can_accepted(result)) {
        can->refused++;
    }
    can_reply_sdo(
        can,
        can_node(serial),
        result,
        id,
        response.length >= 18U ? can_get_u32(response.payload + 14) : 0U
    );
}

static void can_address_reply(struct foc_can *can, const struct foc_serial *serial)
{
    uint8_t data[8] = {can_node(serial)};
    for (unsigned int index = 0U; index < 6U; ++index) {
        data[1U + index] = (uint8_t)(can->port.serial_number >> (8U * index));
    }
    can_queue(can, can_node(serial), FOC_CAN_ADDRESS, data, 8U);
}

static void can_address(
    struct foc_can *can,
    struct foc_serial *serial,
    struct foc_runtime *runtime,
    const struct foc_can_frame *frame,
    uint32_t now_us
)
{
    if (frame->rtr || frame->length == 0U) {
        can_address_reply(can, serial);
        return;
    }
    if (frame->length != 8U) {
        can->refused++;
        return;
    }
    uint64_t serial_number = 0U;
    for (unsigned int index = 0U; index < 6U; ++index) {
        serial_number |= (uint64_t)frame->data[1U + index] << (8U * index);
    }
    if (serial_number != 0U && serial_number != (can->port.serial_number & 0xFFFFFFFFFFFFULL)) {
        return; /* another device's assignment, or another device's reply */
    }
    struct foc_protocol_frame response;
    if (!can_accepted(
            can_param_write(serial, runtime, PARAM_CAN_NODE_ID, frame->data[0], now_us, &response)
        )) {
        can->refused++;
        return;
    }
    can_address_reply(can, serial);
}

static void can_reply_get(
    struct foc_can *can,
    struct foc_serial *serial,
    struct foc_runtime *runtime,
    uint8_t command,
    uint32_t now_us
);

static bool can_length_is(struct foc_can *can, const struct foc_can_frame *frame, uint8_t length)
{
    if (frame->rtr || frame->length != length) {
        can->refused++;
        return false;
    }
    return true;
}

static void can_handle(
    struct foc_can *can,
    struct foc_serial *serial,
    struct foc_runtime *runtime,
    const struct foc_can_frame *frame,
    uint32_t now_us
)
{
    const uint8_t node = (uint8_t)(frame->id >> 5);
    const uint8_t command = (uint8_t)(frame->id & 0x1FU);
    const bool mine = node == can_node(serial);
    if (!mine && node != FOC_CAN_NODE_UNADDRESSED) {
        return;
    }
    if (command == FOC_CAN_ADDRESS) {
        can_address(can, serial, runtime, frame, now_us);
        return;
    }
    const bool request = frame->rtr || frame->length == 0U;
    switch (command) {
    case FOC_CAN_GET_VERSION:
    case FOC_CAN_HEARTBEAT:
    case FOC_CAN_GET_ERROR:
    case FOC_CAN_GET_ENCODER_ESTIMATES:
    case FOC_CAN_GET_IQ:
    case FOC_CAN_GET_TEMPERATURE:
    case FOC_CAN_GET_BUS_VOLTAGE_CURRENT:
    case FOC_CAN_GET_TORQUES:
        /* Replies only for this node: broadcast requests would collide in content. */
        if (request && mine) {
            can_reply_get(can, serial, runtime, command, now_us);
        }
        return;
    case FOC_CAN_TX_SDO:
        return; /* another device's reply */
    default:
        break;
    }
    struct foc_protocol_frame response;
    int result = FOC_PROTO_OK;
    const float gear = can_gear(runtime);
    switch (command) {
    case FOC_CAN_ESTOP:
        (void)can_run(serial, runtime, FOC_MSG_STOP, NULL, 0U, now_us, &response);
        can->estop = true;
        can->sequence = 0U;
        can->calibrating_state = 0U;
        return;
    case FOC_CAN_RX_SDO:
        if (can_length_is(can, frame, 8U)) {
            can_sdo(can, serial, runtime, frame, now_us);
        }
        return;
    case FOC_CAN_SET_AXIS_STATE:
        if (can_length_is(can, frame, 4U)) {
            can_set_axis_state(can, serial, runtime, can_get_u32(frame->data), now_us);
        }
        return;
    case FOC_CAN_SET_CONTROLLER_MODE:
        if (!can_length_is(can, frame, 8U)) {
            return;
        }
        result = can_set_mode(
            serial,
            runtime,
            can_get_u32(frame->data),
            can_get_u32(frame->data + 4),
            now_us
        );
        break;
    case FOC_CAN_SET_INPUT_POS: {
        if (!can_length_is(can, frame, 8U)) {
            return;
        }
        const bool feedforward =
            (frame->data[4] | frame->data[5] | frame->data[6] | frame->data[7]) != 0U;
        const float target = can_get_f32(frame->data) * CAN_TWO_PI;
        result = feedforward
            ? FOC_PROTO_NOT_SUPPORTED
            : can_setpoint(serial, runtime, FOC_RUNTIME_MODE_POSITION, target, 0.0f, now_us);
        if (can_accepted(result)) {
            can->position_target_rad = target;
            can->position_target_valid = true;
        }
        break;
    }
    case FOC_CAN_SET_INPUT_VEL:
        if (!can_length_is(can, frame, 8U)) {
            return;
        }
        result = can_get_f32(frame->data + 4) != 0.0f ? FOC_PROTO_NOT_SUPPORTED
                                                      : can_setpoint(
                                                            serial,
                                                            runtime,
                                                            FOC_RUNTIME_MODE_VELOCITY,
                                                            can_get_f32(frame->data) * CAN_TWO_PI,
                                                            0.0f,
                                                            now_us
                                                        );
        break;
    case FOC_CAN_SET_INPUT_TORQUE: {
        if (!can_length_is(can, frame, 4U)) {
            return;
        }
        const float torque = can_get_f32(frame->data);
        const float per_amp = can_torque_constant(runtime);
        result = per_amp <= 0.0f || !isfinite(torque) ? FOC_PROTO_NOT_CALIBRATED
                                                      : can_setpoint(
                                                            serial,
                                                            runtime,
                                                            FOC_RUNTIME_MODE_CURRENT,
                                                            0.0f,
                                                            torque / per_amp,
                                                            now_us
                                                        );
        if (can_accepted(result)) {
            can->torque_target_nm = torque;
        }
        break;
    }
    case FOC_CAN_SET_LIMITS:
        if (!can_length_is(can, frame, 8U)) {
            return;
        }
        result = can_param_write_f32(
            serial,
            runtime,
            PARAM_VELOCITY_REFERENCE_LIMIT,
            can_get_f32(frame->data) * CAN_TWO_PI * gear,
            now_us
        );
        if (can_accepted(result)) {
            result = can_param_write_f32(
                serial,
                runtime,
                PARAM_CURRENT_LIMIT,
                can_get_f32(frame->data + 4),
                now_us
            );
        }
        break;
    case FOC_CAN_SET_TRAJ_VEL_LIMIT:
        if (!can_length_is(can, frame, 4U)) {
            return;
        }
        result = can_param_write_f32(
            serial,
            runtime,
            PARAM_VELOCITY_REFERENCE_LIMIT,
            can_get_f32(frame->data) * CAN_TWO_PI * gear,
            now_us
        );
        break;
    case FOC_CAN_SET_TRAJ_ACCEL_LIMITS:
        if (!can_length_is(can, frame, 8U)) {
            return;
        }
        result = can_param_write_f32(
            serial,
            runtime,
            PARAM_ACCEL,
            can_get_f32(frame->data) * CAN_TWO_PI * gear,
            now_us
        );
        if (can_accepted(result)) {
            result = can_param_write_f32(
                serial,
                runtime,
                PARAM_DECEL,
                can_get_f32(frame->data + 4) * CAN_TWO_PI * gear,
                now_us
            );
        }
        break;
    case FOC_CAN_SET_POS_GAIN:
        /* (turns/s)/turn equals 1/s: the same number as ID 71. */
        result = can_length_is(can, frame, 4U) ? can_param_write_f32(
                                                     serial,
                                                     runtime,
                                                     PARAM_POSITION_KP,
                                                     can_get_f32(frame->data),
                                                     now_us
                                                 )
                                               : FOC_PROTO_OK;
        break;
    case FOC_CAN_SET_VEL_GAINS: {
        if (!can_length_is(can, frame, 8U)) {
            return;
        }
        /* N·m per output turn/s -> q A per rotor rad/s: divide by 2π, Kt and gear². */
        const float scale = can_torque_constant(runtime) * CAN_TWO_PI * gear;
        result = scale <= 0.0f ? FOC_PROTO_NOT_CALIBRATED
                               : can_param_write_f32(
                                     serial,
                                     runtime,
                                     PARAM_VELOCITY_KP,
                                     can_get_f32(frame->data) / scale,
                                     now_us
                                 );
        if (can_accepted(result)) {
            result = can_param_write_f32(
                serial,
                runtime,
                PARAM_VELOCITY_KI,
                can_get_f32(frame->data + 4) / scale,
                now_us
            );
        }
        break;
    }
    case FOC_CAN_SET_ABSOLUTE_POSITION: {
        if (!can_length_is(can, frame, 4U)) {
            return;
        }
        struct foc_runtime_snapshot view;
        if (foc_runtime_read_snapshot(runtime, &view) != FOC_RUNTIME_OK || !view.sensor_valid) {
            result = FOC_PROTO_BUSY;
            break;
        }
        result = can_param_write_f32(
            serial,
            runtime,
            PARAM_OUTPUT_ZERO,
            view.mechanical_rad / gear - can_get_f32(frame->data) * CAN_TWO_PI,
            now_us
        );
        break;
    }
    case FOC_CAN_CLEAR_ERRORS:
        if (!can_length_is(can, frame, 1U)) {
            return;
        }
        (void)can_run(serial, runtime, FOC_MSG_FAULT_CLEAR, NULL, 0U, now_us, &response);
        can->estop = false;
        can->calibration_error = false;
        can->disarm_reason = 0U;
        return;
    case FOC_CAN_REBOOT: {
        if (!can_length_is(can, frame, 1U)) {
            return;
        }
        const uint8_t action = frame->data[0];
        if (action == 1U || action == 2U) {
            result = can_run(
                serial,
                runtime,
                action == 1U ? FOC_MSG_PARAM_SAVE : FOC_MSG_CONFIG_ERASE,
                NULL,
                0U,
                now_us,
                &response
            );
            break;
        }
        struct foc_runtime_snapshot view;
        if (action == 0U && can->port.reboot != NULL &&
            foc_runtime_read_snapshot(runtime, &view) == FOC_RUNTIME_OK &&
            view.state == FOC_SAFE_STATE_DISABLED) {
            can_flush(can);
            can->port.reboot(can->port.context);
            return;
        }
        result = FOC_PROTO_NOT_SUPPORTED;
        break;
    }
    default:
        result = FOC_PROTO_UNKNOWN_COMMAND;
        break;
    }
    if (!can_accepted(result)) {
        can->refused++;
    }
}

static void can_reply_get(
    struct foc_can *can,
    struct foc_serial *serial,
    struct foc_runtime *runtime,
    uint8_t command,
    uint32_t now_us
)
{
    (void)now_us;
    uint8_t data[8] = {0};
    uint8_t length = 8U;
    struct foc_runtime_snapshot view;
    const bool viewed = foc_runtime_read_snapshot(runtime, &view) == FOC_RUNTIME_OK;
    const float gear = can_gear(runtime);
    const float per_amp = can_torque_constant(runtime);
    const float nan = nanf("");
    switch (command) {
    case FOC_CAN_GET_VERSION:
        data[0] = VERSION_PROTOCOL;
        data[1] = VERSION_HW_MAJOR;
        data[4] = VERSION_FW_MAJOR;
        data[7] = 1U;
        break;
    case FOC_CAN_HEARTBEAT: {
        const uint32_t faults = viewed ? view.faults : 0U;
        uint8_t state = FOC_CAN_AXIS_UNDEFINED;
        if (can->calibrating_state != 0U) {
            state = can->calibrating_state;
        } else if (viewed && view.state == FOC_SAFE_STATE_RUNNING) {
            state = FOC_CAN_AXIS_CLOSED_LOOP;
        } else if (viewed && view.state != FOC_SAFE_STATE_BOOT) {
            state = FOC_CAN_AXIS_IDLE;
        }
        bool done = true;
        if (viewed && state == FOC_CAN_AXIS_CLOSED_LOOP && view.mode == FOC_RUNTIME_MODE_POSITION &&
            can->position_target_valid) {
            const float position = view.mechanical_rad / gear - serial->output_zero_rad;
            done = fabsf(position - can->position_target_rad) < CAN_TRAJ_DONE_RAD &&
                fabsf(view.mechanical_rad_s / gear) < CAN_TRAJ_DONE_RAD_S;
        }
        can_put_u32(data, can_errors(can, faults));
        data[4] = state;
        data[5] = can->procedure_result;
        data[6] = done ? 1U : 0U;
        length = 7U;
        break;
    }
    case FOC_CAN_GET_ERROR:
        can_put_u32(data, can_errors(can, viewed ? view.faults : 0U));
        can_put_u32(data + 4, can->disarm_reason);
        break;
    case FOC_CAN_GET_ENCODER_ESTIMATES:
        can_put_f32(
            data,
            viewed ? (view.mechanical_rad / gear - serial->output_zero_rad) / CAN_TWO_PI : nan
        );
        can_put_f32(data + 4, viewed ? view.mechanical_rad_s / gear / CAN_TWO_PI : nan);
        break;
    case FOC_CAN_GET_IQ:
        can_put_f32(data, viewed && view.current_valid ? view.target_iq_a : 0.0f);
        can_put_f32(data + 4, viewed && view.current_valid ? view.measured_iq_a : 0.0f);
        break;
    case FOC_CAN_GET_TORQUES:
        can_put_f32(
            data,
            viewed && view.mode == FOC_RUNTIME_MODE_CURRENT
                ? can->torque_target_nm
                : (viewed && view.current_valid ? view.target_iq_a * per_amp : 0.0f)
        );
        can_put_f32(data + 4, viewed && view.current_valid ? view.measured_iq_a * per_amp : 0.0f);
        break;
    default:
        /* Temperature (no NTC) and bus voltage/current (no live readback): NaN. */
        can_put_f32(data, nan);
        can_put_f32(data + 4, nan);
        break;
    }
    can_queue(can, can_node(serial), command, data, length);
}

static bool can_due(uint32_t now_us, uint32_t last_us, uint32_t period_ms)
{
    return period_ms != 0U && now_us - last_us >= period_ms * 1000U;
}

/* Public Functions ------------------------------------------------------------- */

void foc_can_rx_push(struct foc_can_buffer *buffer, const struct foc_can_frame *frame)
{
    const unsigned int head = atomic_load_explicit(&buffer->rx_head, memory_order_relaxed);
    const unsigned int tail = atomic_load_explicit(&buffer->rx_tail, memory_order_acquire);
    if (head - tail >= FOC_CAN_RX_CAPACITY) {
        atomic_fetch_add_explicit(&buffer->rx_lost, 1U, memory_order_relaxed);
        return;
    }
    buffer->rx[head % FOC_CAN_RX_CAPACITY] = *frame;
    atomic_store_explicit(&buffer->rx_head, head + 1U, memory_order_release);
}

void foc_can_init(
    struct foc_can *can,
    struct foc_can_buffer *buffer,
    const struct foc_can_port *port
)
{
    *can = (struct foc_can){
        .port = port != NULL ? *port : (struct foc_can_port){0},
        .buffer = buffer,
    };
}

void foc_can_poll(
    struct foc_can *can,
    struct foc_serial *serial,
    struct foc_runtime *runtime,
    uint32_t now_us
)
{
    can_flush(can);
    for (unsigned int count = 0U; can->buffer != NULL && count < FOC_CAN_RX_BUDGET; ++count) {
        const unsigned int tail = atomic_load_explicit(&can->buffer->rx_tail, memory_order_relaxed);
        if (atomic_load_explicit(&can->buffer->rx_head, memory_order_acquire) == tail) {
            break;
        }
        const struct foc_can_frame frame = can->buffer->rx[tail % FOC_CAN_RX_CAPACITY];
        atomic_store_explicit(&can->buffer->rx_tail, tail + 1U, memory_order_release);
        can_handle(can, serial, runtime, &frame, now_us);
    }
    can_sequence_poll(can, serial, runtime, now_us);
    can_calibration_poll(can, serial);
    struct foc_runtime_snapshot view;
    if (can->disarm_reason == 0U && foc_runtime_read_snapshot(runtime, &view) == FOC_RUNTIME_OK &&
        (view.state == FOC_SAFE_STATE_FAULT_LATCHED || can->estop)) {
        can->disarm_reason = can_errors(can, view.faults);
    }
    /* An unaddressed device sends no cyclic frames. */
    if (can_node(serial) != FOC_CAN_NODE_UNADDRESSED) {
        if (can_due(now_us, can->heartbeat_us, serial->can.heartbeat_ms)) {
            can->heartbeat_us = now_us;
            can_reply_get(can, serial, runtime, FOC_CAN_HEARTBEAT, now_us);
        }
        if (can_due(now_us, can->estimates_us, serial->can.estimates_ms)) {
            can->estimates_us = now_us;
            can_reply_get(can, serial, runtime, FOC_CAN_GET_ENCODER_ESTIMATES, now_us);
        }
    }
    can_flush(can);
}

uint32_t foc_can_link_status(const void *context)
{
    const struct foc_can *can = context;
    uint32_t lost = 0U;
    if (can->buffer != NULL) {
        lost = atomic_load((atomic_uint *)&can->buffer->rx_lost);
    }
    const uint32_t state = can->port.state != NULL ? can->port.state(can->port.context) : 0U;
    const uint32_t dropped = can->dropped + lost;
    const uint32_t refused = can->refused;
    return (state & 0xFFU) | (refused > 0xFFU ? 0xFFU : refused) << 8 |
        (dropped > 0xFFFFU ? 0xFFFFU : dropped) << 16;
}

uint32_t foc_can_errors_from_faults(uint32_t faults)
{
    static const struct {
        uint32_t fault;
        uint32_t error;
    } map[] = {
        {FOC_SAFE_FAULT_COMMAND_TIMEOUT, FOC_CAN_ERROR_WATCHDOG_TIMER_EXPIRED},
        {FOC_SAFE_FAULT_SAMPLE_TIMEOUT, FOC_CAN_ERROR_MISSING_ESTIMATE},
        {FOC_SAFE_FAULT_SAMPLE_INVALID, FOC_CAN_ERROR_MISSING_ESTIMATE},
        {FOC_SAFE_FAULT_FAST_HEARTBEAT, FOC_CAN_ERROR_TIMING},
        {FOC_SAFE_FAULT_FOREGROUND_HEARTBEAT, FOC_CAN_ERROR_TIMING},
        {FOC_SAFE_FAULT_WDT, FOC_CAN_ERROR_TIMING},
        {FOC_SAFE_FAULT_INVALID_STATE, FOC_CAN_ERROR_SYSTEM_LEVEL},
        {FOC_SAFE_FAULT_FLASH, FOC_CAN_ERROR_SYSTEM_LEVEL},
        {FOC_SAFE_FAULT_FATAL_RESET_REQUIRED, FOC_CAN_ERROR_SYSTEM_LEVEL},
        {FOC_SAFE_FAULT_OVER_CURRENT, FOC_CAN_ERROR_CURRENT_LIMIT_VIOLATION},
        {FOC_SAFE_FAULT_SUSTAINED_CURRENT, FOC_CAN_ERROR_CURRENT_LIMIT_VIOLATION},
        {FOC_SAFE_FAULT_OVER_VOLTAGE, FOC_CAN_ERROR_DC_BUS_OVER_VOLTAGE},
        {FOC_SAFE_FAULT_UNDER_VOLTAGE, FOC_CAN_ERROR_DC_BUS_UNDER_VOLTAGE},
        {FOC_SAFE_FAULT_OVER_SPEED, FOC_CAN_ERROR_VELOCITY_LIMIT_VIOLATION},
        {FOC_SAFE_FAULT_STALL, FOC_CAN_ERROR_SYSTEM_LEVEL},
        {FOC_SAFE_FAULT_TRACKING_DIVERGENCE, FOC_CAN_ERROR_SYSTEM_LEVEL},
        {FOC_SAFE_FAULT_DRIVER, FOC_CAN_ERROR_DRV_FAULT},
    };
    uint32_t errors = 0U;
    uint32_t known = 0U;
    for (unsigned int index = 0U; index < sizeof map / sizeof map[0]; ++index) {
        known |= map[index].fault;
        if ((faults & map[index].fault) != 0U) {
            errors |= map[index].error;
        }
    }
    /* A fault bit without a CANSimple counterpart still reads as a system error. */
    return errors | ((faults & ~known) != 0U ? FOC_CAN_ERROR_SYSTEM_LEVEL : 0U);
}

/* ---------------------------------------- end of file ----------------------------------------- */
