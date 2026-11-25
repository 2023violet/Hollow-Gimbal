/**
 * @file    foc_serial.h
 * @brief   Board1 UART byte handoff and Foreground V1 command adapter.
 * @version v0.1.0
 * @date    2026.11.10
 *
 * Design overview
 * ---------------
 * Hand bytes between a low-priority IRQ and a bounded Foreground service.
 *
 * Usage example
 * -------------
 * Zero both contexts before enabling the IRQ. Service Runtime before each poll.
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * IRQ is the sole RX producer and TX consumer. Foreground owns the parser,
 * session and RAM policy. The lifetime of both contexts spans the IRQ service.
 *
 * Failure / recovery
 * ------------------
 * RX loss inserts a gap marker after a delimiter. Ordinary requests require a
 * free response slot, with one additional acknowledgement reserved for STOP.
 */
#pragma once

#include "foc_protocol.h"
#include "foc_runtime.h"

/* Public Macros ---------------------------------------------------------------- */

/* Parameter bounds (parameters.json). Board1 facts bounding the protection thresholds: the ±3.3 A current-sense span,
 * the 24 V DRV8311 VM absolute maximum and its 3 V recommended minimum. */
#define FOC_SERIAL_TRIP_CURRENT_MAX_A 3.2f
#define FOC_SERIAL_TRIP_VBUS_MAX_V    24.0f
#define FOC_SERIAL_TRIP_VBUS_MIN_V    3.0f
#define FOC_SERIAL_TRIP_SPEED_MAX     1000.0f
#define FOC_SERIAL_OUTPUT_ZERO_MAX    1000.0f

/* Public Types ---------------------------------------------------------------- */

/** Startup readback exposed through read-only PARAM_READ IDs 32–45. */
struct foc_serial_boot_readback {
    /** Effective configuration, calibration input and rejection codes. */
    uint32_t configuration;
    uint32_t calibration;
    uint32_t rejection;
    uint32_t admission_failure;
    /** Supply and review identities, zero when absent. */
    uint32_t supply_id;
    uint32_t review_id;
    /** Effective physical phase A/B/C decode offsets in raw ADC counts. */
    float phase_offset_count[3];
    /** Effective Sensor counts per mechanical revolution. */
    uint32_t encoder_counts_per_revolution;
    /** Runtime, raw source and calibration directions: +1, -1, or 0 for none. */
    int32_t runtime_direction;
    int32_t source_direction;
    int32_t calibration_direction;
    /** Physical current measurement qualification. */
    bool current_measurement_qualified;
};

/** Fill a readback from a ready startup report; false when none is ready. */
typedef bool (*foc_serial_boot_reader)(const void *context,
                                       struct foc_serial_boot_readback *readback);

struct foc_serial;

/** Persistent-storage outcome exposed through read-only PARAM_READ IDs 48–50. */
struct foc_serial_persistence_view {
    /** Boot outcome: 0 unavailable, 1 empty, 2 loaded, 3 invalid record,
     * 4 identity mismatch, 5 value rejected, 6 I/O error. */
    uint32_t state;
    /** Sequence of the record applied at boot, zero when none. */
    uint32_t loaded_sequence;
    /** Sequence of the newest committed record, zero when storage is erased. */
    uint32_t stored_sequence;
    /** 1 when boot restore applied a stored on-MCU calibration. */
    uint32_t calibration_restored;
};

/** Optional storage owner. save and erase return a FOC_PROTO_* result code. */
struct foc_serial_persistence {
    void *context;
    int (*save)(void *context,
                struct foc_serial *serial,
                struct foc_runtime *runtime,
                uint32_t *sequence);
    int (*erase)(void *context);
    bool (*read)(const void *context, struct foc_serial_persistence_view *view);
};

/** Calibration progress and the latest results, as reported by CALIBRATE_READ. */
struct foc_serial_calibration_view {
    uint8_t state;
    uint8_t failure;
    uint8_t failed_state;
    uint8_t admission_failure;
    uint16_t samples;
    uint16_t pole_pairs;
    int8_t calibration_direction;
    int8_t runtime_direction;
    float electrical_offset_rad;
    float rotor_residual_rad;
    float adc_offset_count[3];
    float adc_noise_rms_count;
    uint32_t applied_runs;
    /* M3b electrical results; zero until available. */
    float rs_ohm;
    float ld_h;
    float lq_h;
    float flux_wb;
    float kt_nm_per_a;
    float kp_d_v_per_a;
    float kp_q_v_per_a;
    float ki_v_per_a_s;
    /* M3-R: what the latest run committed and what is active now. */
    uint8_t committed_stages;
    uint8_t result_bits;
    uint8_t active_source;
    uint8_t ring_high_water;
    uint16_t active_pole_pairs;
    float active_electrical_offset_rad;
    float active_adc_offset_count[3];
};

/** Optional on-MCU calibration owner. start returns a FOC_PROTO_* result code. */
/** Commutation state behind PARAM_READ IDs 88-90. */
struct foc_serial_commission_view {
    /** 1 when the commutation group is valid (not the boot placeholder). */
    uint8_t valid;
    /** CALIBRATE_READ byte 73 source: boot 0, run 1, storage 2, host 3, supply 4. */
    uint8_t source;
    /** ADC zero of this boot: pending 0, ready 1, last attempt refused 2. */
    uint8_t zero_state;
    /** Coast check of the latest run (IDs 91-94): status, flux, angle error, driven flux. */
    uint8_t coast_status;
    float coast_flux_wb;
    float coast_angle_error_rad;
    float driven_flux_wb;
    /** CAL-3 inverter dead-time voltage of the latest run (ID 95), zero if not identified. */
    float dead_time_v;
    /** CAL-8 result (IDs 152-156): status, peak and RMS of the table, friction current and
     * the forward/backward consistency. */
    uint8_t cogging_status;
    float cogging_peak_a;
    float cogging_rms_a;
    float cogging_friction_a;
    float cogging_consistency;
    /** CAL-9 result (IDs 158-161): status, peak and RMS correction in rad, consistency. */
    uint8_t encoder_status;
    float encoder_peak_rad;
    float encoder_rms_rad;
    float encoder_consistency;
};

/** CAL-9 table for ENCODER_LUT_READ: knots, counts per turn (0 empty), active, offsets. */
struct foc_serial_encoder_lut_view {
    uint32_t knots;
    uint32_t counts_per_revolution;
    bool active;
    const int16_t *offsets;
};

struct foc_serial_calibration {
    void *context;
    int (*start)(void *context, uint32_t now_us, unsigned int stages);
    void (*abort)(void *context);
    bool (*busy)(const void *context);
    bool (*read)(const void *context, struct foc_serial_calibration_view *view);
    /** COMMUTATION_WRITE: pole pairs, encoder direction (+1/-1) and offset in rad. */
    int (*commutation)(void *context,
                       uint32_t now_us,
                       uint16_t pole_pairs,
                       int8_t direction,
                       float offset_rad);
    /** PARAM_WRITE ID 17: a new electrical offset for the valid group. */
    int (*trim)(void *context, uint32_t now_us, float offset_rad);
    /** ARM/START admission: OK, NOT_CALIBRATED or BUSY. */
    int (*gate)(const void *context);
    bool (*commission)(const void *context, struct foc_serial_commission_view *view);
    /** After CONFIG_ERASE: return the instance to the boot configuration. */
    int (*reset)(void *context, uint32_t now_us);
    /** ENCODER_LUT_READ source; NULL: NOT_SUPPORTED. Appended member. */
    bool (*encoder_lut)(const void *context, struct foc_serial_encoder_lut_view *view);
};

/** CAN settings (contract 25), owned by Foreground and saved with the parameters. */
/** CAN protocol (ID 128) and MIT reply format (ID 134). */
enum foc_serial_can_protocol {
    FOC_CAN_PROTOCOL_CANSIMPLE = 0,
    FOC_CAN_PROTOCOL_MIT = 1,
    FOC_CAN_MIT_REPLY_ORIGINAL = 0,
    FOC_CAN_MIT_REPLY_DAMIAO = 1,
};

struct foc_serial_can_settings {
    /** CANSimple node ID 0-63; 63 (0x3F) is unaddressed. */
    uint32_t node_id;
    /** Bit rate in bit/s: 125000, 250000, 500000 or 1000000; applied at boot. */
    uint32_t bitrate;
    /** Heartbeat and encoder-estimate periods in ms, 0 off. */
    uint32_t heartbeat_ms;
    uint32_t estimates_ms;
    /** 0 CANSimple, 1 MIT (ID 128). */
    uint32_t protocol;
    /** MIT: CAN ID of the replies (ID 121). */
    uint32_t master_id;
    /** MIT ranges, output side (IDs 129-133). */
    float mit_p_max;
    float mit_v_max;
    float mit_t_max;
    float mit_kp_max;
    float mit_kd_max;
    /** MIT reply: 0 original 6 bytes, 1 DaMiao 8 bytes (ID 134). */
    uint32_t mit_reply;
};

/** One NTC channel (IDs 138–140 FET, 141–143 motor). */
struct foc_serial_thermal_sensor {
    /** 0 off, 1 on. */
    uint32_t enabled;
    /** Linear derating from derate_c to zero current at limit_c; trip 5 °C above. */
    float derate_c;
    float limit_c;
};

/** NTC protection settings (IDs 138–146, 148); defaults from foc_serial_thermal_defaults(). */
struct foc_serial_thermal_settings {
    struct foc_serial_thermal_sensor fet;
    struct foc_serial_thermal_sensor motor;
    /** Shared β model: β in K, R25 and the divider resistor in Ω, topology 0 NTC to
     * ground (pull-up), 1 NTC to the reference (pull-down). */
    float beta_k;
    float r25_ohm;
    float pullup_ohm;
    uint32_t topology;
};

/** CAL-8 sweep settings (IDs 150, 151); defaults from foc_serial_cogging_defaults(). */
struct foc_serial_cogging_settings {
    /** Rotor sweep speed in rad/s, 0.05..2. */
    float speed_rad_s;
    /** 1 keeps the once-per-turn component in the table, 0 removes it. */
    uint32_t keep_first_harmonic;
};

/** CAL-9 encoder correction switch (ID 157); defaults from foc_serial_encoder_defaults(). */
struct foc_serial_encoder_settings {
    /** 1 uses a stored CAL-9 correction (applied while DISABLED), 0 the raw encoder. */
    uint32_t lut_enabled;
};

/** Latest filtered temperatures in °C, NaN without a reading (IDs 119, 147). */
struct foc_serial_temperatures {
    float fet_c;
    float motor_c;
};

/** Optional CAN link status for read-only PARAM_READ ID 126. */
struct foc_serial_can_link {
    const void *context;
    /** Bits 0-7 state (FOC_CAN_STATE_*), 8-15 refused commands, 16-31 dropped frames. */
    uint32_t (*status)(const void *context);
};

/** Fixed UART queue depth and default number of bytes examined per poll. */
enum foc_serial_limits {
    FOC_UART_RX_CAPACITY = 256,
    FOC_UART_RX_GAP = 256,
    FOC_SERIAL_RX_DEFAULT = 24,
};

/** Immutable authorization target selected before the adapter starts. */
enum foc_serial_target_policy {
    /** Board1 default: ARM/START and physical UPDATE remain locked. */
    FOC_SERIAL_TARGET_LOCKED,
    /** Explicit Host-only virtual target; never selectable over UART. */
    FOC_SERIAL_TARGET_VIRTUAL_ONLY,
};

/** SPSC transport state. Only atomics are written by one domain and read by both. */
struct foc_uart_buffer {
    /** RX ring elements, with one out-of-band gap marker. */
    uint16_t rx[FOC_UART_RX_CAPACITY];
    /** IRQ-owned monotonically advancing RX write position. */
    atomic_uint rx_head;
    /** Foreground-owned monotonically advancing RX read position. */
    atomic_uint rx_tail;
    /** Count of RX-overflow quarantine entries and UART error events. */
    atomic_uint rx_lost;
    /** Producer drops through a delimiter after RX corruption or overrun. */
    bool rx_discard;
    /** Complete response storage, immutable while TX size is nonzero. */
    uint8_t tx[FOC_PROTOCOL_WIRE_MAX];
    /** Published TX length, zero when the buffer can be reused. */
    atomic_uint tx_size;
    /** IRQ-owned index of the next TX byte. */
    unsigned int tx_next;
};

/** Callback for the MCU clock, sampled when a frame finishes validation. */
typedef uint32_t (*foc_serial_clock_t)(void *argument);

/** Foreground-owned protocol state and configuration. */
struct foc_serial {
    /** Framing state for one UART. */
    struct foc_protocol_stream stream;
    /** Current logical session, zero before HELLO. */
    uint32_t session;
    /** Sequence of the HELLO that opened the current session. */
    uint32_t hello_seq;
    /** Last admitted ordinary request sequence. */
    uint32_t request_seq;
    /** Runtime publication sequence, continuous across wire sessions. */
    uint32_t publication_seq;
    /** Request sequence of the reserved STOP response. */
    uint32_t stop_seq;
    /** Session of the reserved STOP response. */
    uint32_t stop_session;
    /** Number of observed CRC failures. */
    uint32_t crc_errors;
    /** Number of frames/gap markers discarded by the consumer. */
    uint32_t dropped;
    /** Applied RAM-only remote current-vector limit in A. */
    float current_limit_a;
    /** Applied RAM-only absolute position-reference limit in output rad. */
    float position_limit_rad;
    /** Output-position zero offset in output rad (PARAM 124, SET_ZERO). */
    float output_zero_rad;
    /** Applied RAM-only RX service cap in bytes per poll. */
    uint32_t rx_budget;
    /** Whether the ordinary sequence watermark is initialized. */
    bool request_seen;
    /** Whether the reserved STOP response must still be sent. */
    bool stop_pending;
    /** Whether a parameter has changed since initialization. */
    bool dirty;
    /** Immutable target policy selected at initialization. */
    enum foc_serial_target_policy target_policy;
    /** Monotonic lifecycle request identity, separate from wire sequence. */
    uint32_t lifecycle_sequence;
    /** Monotonic controller-tuning request identity. */
    uint32_t tuning_sequence;
    /** Boot current-vector limit in A: remote tuning may lower it, never raise it above. */
    float current_limit_ceiling_a;
    /** Optional read-only startup readback source and its context. */
    foc_serial_boot_reader boot_reader;
    const void *boot_context;
    /** Optional persistent storage; PARAM_SAVE and CONFIG_ERASE are NOT_SUPPORTED without it. */
    struct foc_serial_persistence persistence;
    /** Optional calibration manager; CALIBRATE_* are NOT_SUPPORTED without it. */
    struct foc_serial_calibration calibration;
    /** CAN settings (IDs 120, 122, 123, 125); defaults from foc_serial_can_defaults(). */
    struct foc_serial_can_settings can;
    /** Optional CAN link status (ID 126). */
    struct foc_serial_can_link can_link;
    /** NTC protection settings, polled by the board's foc_board_thermal. */
    struct foc_serial_thermal_settings thermal;
    /** Written by foc_board_thermal_poll(); NaN until then. */
    struct foc_serial_temperatures temperature;
    /** CAL-8 sweep settings. */
    struct foc_serial_cogging_settings cogging;
    /** CAL-9 encoder correction switch. */
    struct foc_serial_encoder_settings encoder;
};

/* Public Functions ------------------------------------------------------------- */

/** IRQ only. Queue a byte or mark a peripheral error; bad byte content is ignored. */
void foc_uart_rx_push(struct foc_uart_buffer *buffer, uint8_t byte, bool error);

/** Foreground only. Consume one byte or gap marker. */
bool foc_uart_rx_pop(struct foc_uart_buffer *buffer, uint16_t *value);

/** Foreground only. True when the complete TX slot may be filled. */
bool foc_uart_tx_available(const struct foc_uart_buffer *buffer);

/** Foreground only. Copy a complete response if the TX slot is available. */
bool foc_uart_tx_submit(struct foc_uart_buffer *buffer, const uint8_t *data, size_t length);

/** IRQ only. Remove one byte for the UART data register. */
bool foc_uart_tx_pop(struct foc_uart_buffer *buffer, uint8_t *byte);

/** Initialize the Foreground adapter for a fresh boot before receiving bytes. */
void foc_serial_init(struct foc_serial *serial, const struct foc_runtime *runtime);

/** Initialize an explicit target policy; Board1 callers should use foc_serial_init(). */
void foc_serial_init_with_policy(struct foc_serial *serial,
                                 const struct foc_runtime *runtime,
                                 enum foc_serial_target_policy policy);

/** Expose a startup readback through read-only PARAM_READ IDs 32–45. */
void foc_serial_attach_boot(struct foc_serial *serial,
                            foc_serial_boot_reader reader,
                            const void *context);

/** Attach persistent storage before receiving bytes. */
void foc_serial_attach_persistence(struct foc_serial *serial,
                                   const struct foc_serial_persistence *persistence);

/** Attach the calibration manager before receiving bytes. */
void foc_serial_attach_calibration(struct foc_serial *serial,
                                   const struct foc_serial_calibration *calibration);

/** Default CAN settings: unaddressed node 0x3F, 1 Mbit/s, heartbeat 100 ms, estimates 10 ms. */
struct foc_serial_can_settings foc_serial_can_defaults(void);

/** True when the settings are within the ranges of their PARAM_WRITE entries. */
bool foc_serial_can_settings_valid(const struct foc_serial_can_settings *settings);

/** Default NTC settings: both channels off, 75/95 °C, 10 kΩ β 3950 to ground, 10 kΩ pull-up. */
struct foc_serial_thermal_settings foc_serial_thermal_defaults(void);

/** True within the PARAM_WRITE ranges with each channel's derate_c below its limit_c. */
bool foc_serial_thermal_settings_valid(const struct foc_serial_thermal_settings *settings);

/** Default CAL-8 settings: 0.25 rad/s, once-per-turn component removed. */
struct foc_serial_cogging_settings foc_serial_cogging_defaults(void);

/** True within the PARAM_WRITE ranges of IDs 150 and 151. */
bool foc_serial_cogging_settings_valid(const struct foc_serial_cogging_settings *settings);

/** Default CAL-9 switch: on (a correction applies once one exists). */
struct foc_serial_encoder_settings foc_serial_encoder_defaults(void);

/** Attach the CAN link status before receiving bytes. */
void foc_serial_attach_can_link(struct foc_serial *serial, const struct foc_serial_can_link *link);

/**
 * Foreground only. Publish an IMPEDANCE command from output-side values: target[0]
 * position in rad (relative to the output zero, ID 124), [1] velocity in rad/s, [2]
 * stiffness in N*m/rad, [3] damping in N*m*s/rad, [4] feedforward torque in N*m. The
 * gear ratio converts them to the rotor side. Returns ACCEPTED, INVALID_ARGUMENT (range),
 * BAD_STATE (not in IMPEDANCE, faulted, inhibiting, or refused) or BUSY. Used by UART
 * SETPOINT mode 3 and the CAN MIT protocol.
 */
int foc_serial_impedance(struct foc_serial *serial,
                         struct foc_runtime *runtime,
                         const float target[5],
                         uint32_t now_us);

/**
 * Foreground only. Run one decoded request for another transport (CAN) without a wire
 * session or sequence: the same admission as a UART request after its session check.
 * Fills response (payload[0] is the result) and returns the FOC_PROTO_* result code.
 */
int foc_serial_execute(struct foc_serial *serial,
                       struct foc_runtime *runtime,
                       const struct foc_protocol_frame *request,
                       uint32_t now_us,
                       struct foc_protocol_frame *response);

/** Read the Runtime lifecycle result associated with the latest Host request. */
int foc_serial_read_lifecycle_result(const struct foc_serial *serial,
                                     struct foc_runtime *runtime,
                                     struct foc_runtime_lifecycle_result *result);

/** After Runtime supervision, examine at most rx_budget bytes and one complete frame. */
void foc_serial_poll(struct foc_serial *serial,
                     struct foc_uart_buffer *uart,
                     struct foc_runtime *runtime,
                     foc_serial_clock_t clock,
                     void *argument);

/* ---------------------------------------- end of file ----------------------------------------- */
