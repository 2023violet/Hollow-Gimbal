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

#include <float.h>
#include <math.h>
#include <string.h>

/* Private Constants ------------------------------------------------------------ */

/* Board1 facts bounding the protection thresholds: the ±3.3 A current-sense span,
 * the 24 V DRV8311 VM absolute maximum and its 3 V recommended minimum. */
#define FOC_SERIAL_TRIP_CURRENT_MAX_A 3.2f
#define FOC_SERIAL_TRIP_VBUS_MAX_V    24.0f
#define FOC_SERIAL_TRIP_VBUS_MIN_V    3.0f
#define FOC_SERIAL_TRIP_SPEED_MAX     1000.0f
#define FOC_SERIAL_OUTPUT_ZERO_MAX    1000.0f

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
    /* Persistent storage readback (M2). */
    PARAM_STORE_STATE = 48,
    PARAM_STORE_LOADED_SEQUENCE = 49,
    PARAM_STORE_STORED_SEQUENCE = 50,
    PARAM_STORE_CALIBRATION = 51,
    /* Commutation readback (step 3). */
    PARAM_COMMUTATION_VALID = 88,
    PARAM_COMMUTATION_SOURCE = 89,
    PARAM_ADC_ZERO_STATE = 90,
    /* Coast check of the latest calibration run, read only. */
    PARAM_COAST_STATUS = 91,
    PARAM_COAST_FLUX = 92,
    PARAM_COAST_ANGLE_ERROR = 93,
    PARAM_DRIVEN_FLUX = 94,
    PARAM_DEAD_TIME = 95,
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
    /* Motor model (contract 3.1); Kt is derived and read only. */
    PARAM_MODEL_RS = 80,
    PARAM_MODEL_LD = 81,
    PARAM_MODEL_LQ = 82,
    PARAM_MODEL_FLUX = 83,
    PARAM_MODEL_KT = 84,
    PARAM_MODEL_INERTIA = 85,
    PARAM_MODEL_DAMPING = 86,
    /* User profile (contract 3.3) and the current-loop bandwidth (3.4). */
    PARAM_PROFILE_RATED_CURRENT = 96,
    PARAM_PROFILE_PEAK_CURRENT = 97,
    PARAM_PROFILE_MAX_SPEED = 98,
    PARAM_PROFILE_GEAR_RATIO = 99,
    PARAM_CURRENT_BANDWIDTH = 104,
    /* Trajectory acceleration and deceleration (contract 3.4), rotor-side, 0 unlimited. */
    PARAM_ACCEL = 106,
    PARAM_DECEL = 107,
    /* Velocity bandwidth for the J-derived outer gains and the CAL-7 travel (contract 18). */
    PARAM_VELOCITY_BANDWIDTH = 108,
    PARAM_INERTIA_TRAVEL = 109,
    /* Motor-protection thresholds, writable while DISABLED; applied by H0. */
    PARAM_TRIP_OVERCURRENT = 112,
    PARAM_TRIP_OVERVOLTAGE = 113,
    PARAM_TRIP_UNDERVOLTAGE = 114,
    PARAM_TRIP_OVERSPEED = 115,
    PARAM_TRIP_SUSTAINED = 116,
    PARAM_TRIP_STALL = 117,
    PARAM_TRIP_DIVERGENCE = 118,
    /* Output frame (contract 3.7): host positions are output-side and relative to it. */
    PARAM_OUTPUT_ZERO = 124,
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
    const uint8_t capability[] = {FOC_PROTO_OK, 1, 0, 1, 0, 7, 0, bits, 0};
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
    const bool trip = id >= PARAM_TRIP_OVERCURRENT && id <= PARAM_TRIP_DIVERGENCE;
    const bool profile = (id >= PARAM_MODEL_RS && id <= PARAM_MODEL_DAMPING) ||
        (id >= PARAM_PROFILE_RATED_CURRENT && id <= PARAM_PROFILE_GEAR_RATIO) ||
        id == PARAM_CURRENT_BANDWIDTH || id == PARAM_VELOCITY_BANDWIDTH ||
        id == PARAM_INERTIA_TRAVEL;
    const bool ramp = id == PARAM_ACCEL || id == PARAM_DECEL;
    if ((trip ? !runtime->config.trip_enabled
              : !profile && !ramp && (id < PARAM_TUNING_REVISION || id > PARAM_COMMAND_LEASE)) ||
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
            1000.0f,
            UNIT_A_PER_RAD_S
        );
        return true;
    case PARAM_VELOCITY_KI:
        foc_serial_tuning_float(
            parameter,
            tuning.velocity_ki_a_per_rad,
            0.0f,
            100000.0f,
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
    case PARAM_ACCEL:
    case PARAM_DECEL:
        /* 0 is unlimited; H0 refuses values between 0 and FOC_RUNTIME_RAMP_MIN_RAD_S2. */
        foc_serial_tuning_float(
            parameter,
            id == PARAM_ACCEL ? tuning.accel_rad_s2 : tuning.decel_rad_s2,
            0.0f,
            FOC_RUNTIME_RAMP_MAX_RAD_S2,
            UNIT_RAD_S2
        );
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
    case PARAM_MODEL_RS:
        foc_serial_tuning_float(parameter, tuning.profile.rs_ohm, 0.0f, 100.0f, UNIT_OHM);
        return true;
    case PARAM_MODEL_LD:
    case PARAM_MODEL_LQ:
        foc_serial_tuning_float(
            parameter,
            id == PARAM_MODEL_LD ? tuning.profile.ld_h : tuning.profile.lq_h,
            0.0f,
            1.0f,
            UNIT_HENRY
        );
        return true;
    case PARAM_MODEL_FLUX:
        foc_serial_tuning_float(parameter, tuning.profile.flux_wb, 0.0f, 1.0f, UNIT_WEBER);
        return true;
    case PARAM_MODEL_KT: {
        /* Kt = 1.5 x pole pairs x flux linkage, per peak q ampere; 0 without a flux. */
        const float kt =
            1.5f * (float)runtime->config.motion.rotor.pole_pairs * tuning.profile.flux_wb;
        foc_serial_tuning_float(parameter, kt, kt, kt, UNIT_NM_PER_A);
        parameter->writable = false;
        parameter->tuning = false;
        return true;
    }
    case PARAM_MODEL_INERTIA:
        foc_serial_tuning_float(parameter, tuning.profile.inertia_kg_m2, 0.0f, 1.0f, UNIT_KG_M2);
        return true;
    case PARAM_MODEL_DAMPING:
        foc_serial_tuning_float(
            parameter,
            tuning.profile.damping_nm_s_per_rad,
            0.0f,
            10.0f,
            UNIT_NM_S_PER_RAD
        );
        return true;
    case PARAM_PROFILE_RATED_CURRENT:
    case PARAM_PROFILE_PEAK_CURRENT:
        /* A motor may be rated above the board: ID 68 keeps the board ceiling. */
        foc_serial_tuning_float(
            parameter,
            id == PARAM_PROFILE_RATED_CURRENT ? tuning.profile.rated_current_a
                                              : tuning.profile.peak_current_a,
            0.0f,
            1000.0f,
            UNIT_AMPERE
        );
        return true;
    case PARAM_PROFILE_MAX_SPEED:
        foc_serial_tuning_float(
            parameter,
            tuning.profile.max_speed_rad_s,
            0.0f,
            FOC_SERIAL_TRIP_SPEED_MAX,
            UNIT_RAD_S
        );
        return true;
    case PARAM_PROFILE_GEAR_RATIO:
        foc_serial_tuning_float(parameter, tuning.profile.gear_ratio, 1.0f, 1000.0f, UNIT_NONE);
        return true;
    case PARAM_CURRENT_BANDWIDTH:
        foc_serial_tuning_float(
            parameter,
            tuning.profile.current_bandwidth_hz,
            (float)FOC_BOARD_PROFILE_BANDWIDTH_MIN_HZ,
            (float)FOC_BOARD_PROFILE_BANDWIDTH_MAX_HZ,
            UNIT_HERTZ
        );
        return true;
    case PARAM_VELOCITY_BANDWIDTH:
        /* 0 keeps manual outer gains; the refresh refuses more than 1/5 of ID 104. */
        foc_serial_tuning_float(
            parameter,
            tuning.profile.velocity_bandwidth_hz,
            0.0f,
            (float)FOC_BOARD_PROFILE_VELOCITY_BANDWIDTH_MAX_HZ,
            UNIT_HERTZ
        );
        return true;
    case PARAM_INERTIA_TRAVEL:
        foc_serial_tuning_float(
            parameter,
            tuning.profile.inertia_travel_rad,
            0.5f,
            1000.0f,
            UNIT_RADIAN
        );
        return true;
    case PARAM_TRIP_OVERCURRENT:
        /* H0 also requires it strictly above the current-vector limit. */
        foc_serial_tuning_float(
            parameter,
            tuning.trips.overcurrent_a,
            tuning.current_limit_a,
            FOC_SERIAL_TRIP_CURRENT_MAX_A,
            UNIT_AMPERE
        );
        return true;
    case PARAM_TRIP_OVERVOLTAGE:
    case PARAM_TRIP_UNDERVOLTAGE:
        /* H0 also requires under-voltage below over-voltage. */
        foc_serial_tuning_float(
            parameter,
            id == PARAM_TRIP_OVERVOLTAGE ? tuning.trips.overvoltage_v : tuning.trips.undervoltage_v,
            FOC_SERIAL_TRIP_VBUS_MIN_V,
            FOC_SERIAL_TRIP_VBUS_MAX_V,
            UNIT_VOLT
        );
        return true;
    case PARAM_TRIP_OVERSPEED:
        /* 0 follows 1.25 x ID 72; otherwise H0 requires at least ID 72. */
        foc_serial_tuning_float(
            parameter,
            tuning.trips.overspeed_rad_s,
            0.0f,
            FOC_SERIAL_TRIP_SPEED_MAX,
            UNIT_RAD_S
        );
        return true;
    case PARAM_TRIP_SUSTAINED:
    case PARAM_TRIP_STALL:
    case PARAM_TRIP_DIVERGENCE:
        *parameter = (struct foc_serial_parameter){
            .value = id == PARAM_TRIP_SUSTAINED ? tuning.trips.sustained_ms
                : id == PARAM_TRIP_STALL        ? tuning.trips.stall_ms
                                                : tuning.trips.divergence_ms,
            .minimum = 0U,
            .maximum = id == PARAM_TRIP_SUSTAINED ? FOC_RUNTIME_TRIP_WINDOW_MAX_MS : 10000U,
            .type = PARAM_U32,
            .unit = UNIT_MILLISECOND,
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
    case PARAM_ACCEL:
        tuning.accel_rad_s2 = floating;
        break;
    case PARAM_DECEL:
        tuning.decel_rad_s2 = floating;
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
    case PARAM_TRIP_OVERCURRENT:
        tuning.trips.overcurrent_a = floating;
        break;
    case PARAM_TRIP_OVERVOLTAGE:
        tuning.trips.overvoltage_v = floating;
        break;
    case PARAM_TRIP_UNDERVOLTAGE:
        tuning.trips.undervoltage_v = floating;
        break;
    case PARAM_TRIP_OVERSPEED:
        tuning.trips.overspeed_rad_s = floating;
        break;
    case PARAM_TRIP_SUSTAINED:
        tuning.trips.sustained_ms = value;
        break;
    case PARAM_TRIP_STALL:
        tuning.trips.stall_ms = value;
        break;
    case PARAM_TRIP_DIVERGENCE:
        tuning.trips.divergence_ms = value;
        break;
    case PARAM_MODEL_RS:
        tuning.profile.rs_ohm = floating;
        break;
    case PARAM_MODEL_LD:
        tuning.profile.ld_h = floating;
        break;
    case PARAM_MODEL_LQ:
        tuning.profile.lq_h = floating;
        break;
    case PARAM_MODEL_FLUX:
        tuning.profile.flux_wb = floating;
        break;
    case PARAM_MODEL_INERTIA:
        tuning.profile.inertia_kg_m2 = floating;
        break;
    case PARAM_MODEL_DAMPING:
        tuning.profile.damping_nm_s_per_rad = floating;
        break;
    case PARAM_PROFILE_RATED_CURRENT:
        tuning.profile.rated_current_a = floating;
        break;
    case PARAM_PROFILE_PEAK_CURRENT:
        tuning.profile.peak_current_a = floating;
        break;
    case PARAM_PROFILE_MAX_SPEED:
        tuning.profile.max_speed_rad_s = floating;
        break;
    case PARAM_PROFILE_GEAR_RATIO:
        tuning.profile.gear_ratio = floating;
        break;
    case PARAM_CURRENT_BANDWIDTH:
        tuning.profile.current_bandwidth_hz = floating;
        break;
    case PARAM_VELOCITY_BANDWIDTH:
        tuning.profile.velocity_bandwidth_hz = floating;
        break;
    case PARAM_INERTIA_TRAVEL:
        tuning.profile.inertia_travel_rad = floating;
        break;
    default:
        return FOC_PROTO_INTERNAL_ERROR;
    }
    /* Unified refresh (contract 13.3), for profile fields only: a model or bandwidth change
     * re-derives the current PI, a peak current or maximum speed tightens the limits it
     * bounds. A written limit is never adjusted: H0 refuses one that breaks the ordering. */
    const bool profile = (id >= PARAM_MODEL_RS && id <= PARAM_MODEL_DAMPING) ||
        (id >= PARAM_PROFILE_RATED_CURRENT && id <= PARAM_PROFILE_GEAR_RATIO) ||
        id == PARAM_CURRENT_BANDWIDTH || id == PARAM_VELOCITY_BANDWIDTH;
    const bool redesign = id == PARAM_MODEL_RS || id == PARAM_MODEL_LD || id == PARAM_MODEL_LQ ||
        id == PARAM_CURRENT_BANDWIDTH;
    /* Kt, the inertia, the velocity bandwidth or its 1/5 bound re-derive the outer gains. */
    const bool redesign_velocity = id == PARAM_MODEL_FLUX || id == PARAM_MODEL_INERTIA ||
        id == PARAM_CURRENT_BANDWIDTH || id == PARAM_VELOCITY_BANDWIDTH;
    if (profile &&
        !foc_board_profile_refresh(
            runtime->config.h0.period_us,
            runtime->pipeline.current.config.pwm.voltage_limit_v,
            runtime->config.motion.rotor.pole_pairs,
            &tuning,
            redesign,
            redesign_velocity,
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
    if (id >= PARAM_COMMUTATION_VALID && id <= PARAM_DEAD_TIME &&
        serial->calibration.commission != NULL) {
        struct foc_serial_commission_view view = {0};
        (void)serial->calibration.commission(serial->calibration.context, &view);
        if (id >= PARAM_COAST_FLUX) {
            const float value = id == PARAM_COAST_FLUX ? view.coast_flux_wb
                : id == PARAM_COAST_ANGLE_ERROR        ? view.coast_angle_error_rad
                : id == PARAM_DRIVEN_FLUX              ? view.driven_flux_wb
                                                       : view.dead_time_v;
            *parameter = (struct foc_serial_parameter){
                .type = PARAM_F32,
                .unit = id == PARAM_COAST_ANGLE_ERROR ? UNIT_RADIAN
                    : id == PARAM_DEAD_TIME           ? UNIT_VOLT
                                                      : UNIT_WEBER,
                .value = foc_serial_float_bits(value),
            };
        } else {
            *parameter = (struct foc_serial_parameter){.type = PARAM_U32, .unit = UNIT_NONE};
            parameter->value = id == PARAM_COMMUTATION_VALID ? view.valid
                : id == PARAM_COMMUTATION_SOURCE             ? view.source
                : id == PARAM_ADC_ZERO_STATE                 ? view.zero_state
                                                             : view.coast_status;
        }
        parameter->minimum = parameter->maximum = parameter->value;
        return true;
    }
    if (id >= PARAM_STORE_STATE && id <= PARAM_STORE_CALIBRATION) {
        struct foc_serial_persistence_view view = {0};
        if (serial->persistence.read != NULL) {
            (void)serial->persistence.read(serial->persistence.context, &view);
        }
        *parameter = (struct foc_serial_parameter){.type = PARAM_U32, .unit = UNIT_NONE};
        parameter->value = id == PARAM_STORE_STATE ? view.state
            : id == PARAM_STORE_LOADED_SEQUENCE    ? view.loaded_sequence
            : id == PARAM_STORE_STORED_SEQUENCE    ? view.stored_sequence
                                                   : view.calibration_restored;
        parameter->minimum = parameter->maximum = parameter->value;
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
    case PARAM_OUTPUT_ZERO:
        parameter->value = foc_serial_float_bits(serial->output_zero_rad);
        parameter->minimum = foc_serial_float_bits(-FOC_SERIAL_OUTPUT_ZERO_MAX);
        parameter->maximum = foc_serial_float_bits(FOC_SERIAL_OUTPUT_ZERO_MAX);
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
        /* A trim of the valid commutation group, [0, 2π). */
        parameter->writable = serial->calibration.trim != NULL;
        parameter->minimum = foc_serial_float_bits(0.0f);
        parameter->maximum = foc_serial_float_bits(6.283185f);
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
        case PARAM_OUTPUT_ZERO:
            serial->output_zero_rad = foc_serial_bits_float(value);
            break;
        case PARAM_ELECTRICAL_OFFSET: {
            const int trimmed =
                serial->calibration
                    .trim(serial->calibration.context, now_us, foc_serial_bits_float(value));
            if (trimmed != FOC_PROTO_APPLIED) {
                return trimmed;
            }
            break;
        }
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
    if (request->type == FOC_MSG_CALIBRATE_START) {
        /* Optional stage mask: 1 rotor (CAL-1/2), 2 electrical (CAL-3/4/5); empty means both. */
        const unsigned int stages = request->length == 0U ? 3U : request->payload[0];
        /* 1-4 as before; 8 (CAL-7) alone or with the rotor/electrical bits (8-11). */
        if (request->length > 1U || stages < 1U || (stages > 4U && (stages < 8U || stages > 11U))) {
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
