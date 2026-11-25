/**
 * @file    foc_can.h
 * @brief   Board1 CANSimple adapter: CAN frame handoff and Foreground translation.
 * @version v0.1.0
 * @date    2026.11.24
 *
 * Design overview
 * ---------------
 * Speak the CANSimple frame layout (11-bit ID = node << 5 | command) and translate each
 * command into the same Foreground request a UART Host would send (foc_serial_execute),
 * so admission, limits, the output lock and the command lease are shared. Parameters are
 * read and written through RxSdo/TxSdo with FalconFOC parameter IDs (contract 25).
 *
 * Usage example
 * -------------
 * Zero the buffer, foc_can_init() with a port, attach foc_can_link_status() to the
 * serial adapter, start the CAN IRQ, then call foc_can_poll() after foc_serial_poll().
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * The CAN RX IRQ is the sole buffer producer. Foreground owns the adapter, sends frames
 * through the port and never blocks: a full transmitter keeps frames in the TX queue.
 *
 * Failure / recovery
 * ------------------
 * RX overflow and TX queue overflow drop frames and count them (ID 126). Refused
 * commands are counted; CANSimple has no command responses.
 */
#pragma once

#include "foc_serial.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

/* Public Types ---------------------------------------------------------------- */

enum foc_can_limits {
    /** RX ring depth (power of two) and TX queue depth in frames. */
    FOC_CAN_RX_CAPACITY = 32,
    FOC_CAN_TX_CAPACITY = 16,
    /** Frames examined per poll. */
    FOC_CAN_RX_BUDGET = 8,
    /** CANSimple node ID of an unaddressed device, also the broadcast node. */
    FOC_CAN_NODE_UNADDRESSED = 0x3F,
};

/** Link state, PARAM_READ ID 126 bits 0-7. */
enum foc_can_state {
    FOC_CAN_STATE_OFF = 0,
    FOC_CAN_STATE_ACTIVE = 1,
    FOC_CAN_STATE_PASSIVE = 2,
    FOC_CAN_STATE_BUS_OFF = 3,
    /** The board runs from the internal RC oscillator: too inaccurate for CAN. */
    FOC_CAN_STATE_NO_CRYSTAL = 4,
};

/** CANSimple command IDs (ID bits 0-4). */
enum foc_can_command {
    FOC_CAN_GET_VERSION = 0x00,
    FOC_CAN_HEARTBEAT = 0x01,
    FOC_CAN_ESTOP = 0x02,
    FOC_CAN_GET_ERROR = 0x03,
    FOC_CAN_RX_SDO = 0x04,
    FOC_CAN_TX_SDO = 0x05,
    FOC_CAN_ADDRESS = 0x06,
    FOC_CAN_SET_AXIS_STATE = 0x07,
    FOC_CAN_GET_ENCODER_ESTIMATES = 0x09,
    FOC_CAN_SET_CONTROLLER_MODE = 0x0B,
    FOC_CAN_SET_INPUT_POS = 0x0C,
    FOC_CAN_SET_INPUT_VEL = 0x0D,
    FOC_CAN_SET_INPUT_TORQUE = 0x0E,
    FOC_CAN_SET_LIMITS = 0x0F,
    FOC_CAN_SET_TRAJ_VEL_LIMIT = 0x11,
    FOC_CAN_SET_TRAJ_ACCEL_LIMITS = 0x12,
    FOC_CAN_GET_IQ = 0x14,
    FOC_CAN_GET_TEMPERATURE = 0x15,
    FOC_CAN_REBOOT = 0x16,
    FOC_CAN_GET_BUS_VOLTAGE_CURRENT = 0x17,
    FOC_CAN_CLEAR_ERRORS = 0x18,
    FOC_CAN_SET_ABSOLUTE_POSITION = 0x19,
    FOC_CAN_SET_POS_GAIN = 0x1A,
    FOC_CAN_SET_VEL_GAINS = 0x1B,
    FOC_CAN_GET_TORQUES = 0x1C,
};

/** CANSimple axis states reported in the heartbeat and requested by Set_Axis_State. */
enum foc_can_axis_state {
    FOC_CAN_AXIS_UNDEFINED = 0,
    FOC_CAN_AXIS_IDLE = 1,
    FOC_CAN_AXIS_FULL_CALIBRATION = 3,
    FOC_CAN_AXIS_MOTOR_CALIBRATION = 4,
    FOC_CAN_AXIS_ENCODER_OFFSET_CALIBRATION = 7,
    FOC_CAN_AXIS_CLOSED_LOOP = 8,
};

/** CANSimple procedure results (heartbeat byte 5). */
enum foc_can_procedure {
    FOC_CAN_PROCEDURE_SUCCESS = 0,
    FOC_CAN_PROCEDURE_BUSY = 1,
    FOC_CAN_PROCEDURE_CANCELLED = 2,
    FOC_CAN_PROCEDURE_DISARMED = 3,
    FOC_CAN_PROCEDURE_NO_RESPONSE = 4,
    FOC_CAN_PROCEDURE_POLE_PAIR_CPR_MISMATCH = 5,
    FOC_CAN_PROCEDURE_PHASE_RESISTANCE_OUT_OF_RANGE = 6,
    FOC_CAN_PROCEDURE_TIMEOUT = 11,
    FOC_CAN_PROCEDURE_INVALID_STATE = 13,
    FOC_CAN_PROCEDURE_NOT_CALIBRATED = 14,
    FOC_CAN_PROCEDURE_NOT_CONVERGING = 15,
};

/** CANSimple error bits (heartbeat, Get_Error). */
enum foc_can_error {
    FOC_CAN_ERROR_SYSTEM_LEVEL = 0x2U,
    FOC_CAN_ERROR_TIMING = 0x4U,
    FOC_CAN_ERROR_MISSING_ESTIMATE = 0x8U,
    FOC_CAN_ERROR_DRV_FAULT = 0x20U,
    FOC_CAN_ERROR_DC_BUS_OVER_VOLTAGE = 0x100U,
    FOC_CAN_ERROR_DC_BUS_UNDER_VOLTAGE = 0x200U,
    FOC_CAN_ERROR_CURRENT_LIMIT_VIOLATION = 0x1000U,
    FOC_CAN_ERROR_VELOCITY_LIMIT_VIOLATION = 0x8000U,
    FOC_CAN_ERROR_WATCHDOG_TIMER_EXPIRED = 0x1000000U,
    FOC_CAN_ERROR_ESTOP_REQUESTED = 0x2000000U,
    FOC_CAN_ERROR_CALIBRATION = 0x40000000U,
};

/** One classic CAN frame with a standard 11-bit identifier. */
struct foc_can_frame {
    uint16_t id;
    uint8_t length;
    bool rtr;
    uint8_t data[8];
};

/** SPSC RX ring: the CAN RX IRQ produces, Foreground consumes. */
struct foc_can_buffer {
    struct foc_can_frame rx[FOC_CAN_RX_CAPACITY];
    /** IRQ-owned monotonically advancing write position. */
    atomic_uint rx_head;
    /** Foreground-owned monotonically advancing read position. */
    atomic_uint rx_tail;
    /** Frames dropped because the ring was full. */
    atomic_uint rx_lost;
};

/** Board port supplied by main.c or the SIL. */
struct foc_can_port {
    void *context;
    /** Queue one frame in a transmitter; false when none is free. */
    bool (*send)(void *context, const struct foc_can_frame *frame);
    /** Link state (enum foc_can_state). */
    uint32_t (*state)(const void *context);
    /** Optional system reset for Reboot action 0; NULL ignores the request. */
    void (*reboot)(void *context);
    /** 48-bit device serial number for Address (MCU UID folded). */
    uint64_t serial_number;
};

/** Foreground-owned adapter state. */
struct foc_can {
    struct foc_can_port port;
    struct foc_can_buffer *buffer;
    /** Frames waiting for a free transmitter. */
    struct foc_can_frame tx[FOC_CAN_TX_CAPACITY];
    uint32_t tx_head;
    uint32_t tx_count;
    /** TX queue overflows and refused commands (ID 126). */
    uint32_t dropped;
    uint32_t refused;
    /** Times the last cyclic frames were sent. */
    uint32_t heartbeat_us;
    uint32_t estimates_us;
    /** CANSimple view: last procedure result, disarm reason and Host-requested flags. */
    uint8_t procedure_result;
    uint32_t disarm_reason;
    bool estop;
    bool calibration_error;
    /** Axis state of a calibration requested over CAN while it runs, 0 otherwise. */
    uint8_t calibrating_state;
    /** Closed-loop request: 0 none, 1 waiting for ARM, 2 waiting for START. */
    uint8_t sequence;
    uint32_t sequence_request;
    uint32_t sequence_started_us;
    uint32_t sequence_retry_us;
    /** MIT enter: 0 idle, 1 waiting for IMPEDANCE, 2 in the ARM/START sequence. */
    uint8_t mit_stage;
    uint32_t mit_started_us;
    uint32_t mit_retry_us;
    /** Last torque and output-position targets for Get_Torques and the heartbeat. */
    float torque_target_nm;
    float position_target_rad;
    bool position_target_valid;
};

/* Public Functions ------------------------------------------------------------- */

/** IRQ only. Queue one received standard frame; a full ring counts it as lost. */
void foc_can_rx_push(struct foc_can_buffer *buffer, const struct foc_can_frame *frame);

/** Initialize the adapter for a fresh boot before the CAN IRQ starts. */
void foc_can_init(struct foc_can *can,
                  struct foc_can_buffer *buffer,
                  const struct foc_can_port *port);

/** Foreground only. Send queued frames, handle received ones and emit cyclic frames. */
void foc_can_poll(struct foc_can *can,
                  struct foc_serial *serial,
                  struct foc_runtime *runtime,
                  uint32_t now_us);

/** PARAM_READ ID 126 for foc_serial_attach_can_link(); the context is the adapter. */
uint32_t foc_can_link_status(const void *context);

/** CANSimple error bits for latched FalconFOC fault bits. */
uint32_t foc_can_errors_from_faults(uint32_t faults);

/* ---------------------------------------- end of file ----------------------------------------- */
