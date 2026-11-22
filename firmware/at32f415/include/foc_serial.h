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
struct foc_serial_calibration {
    void *context;
    int (*start)(void *context, uint32_t now_us, unsigned int stages);
    void (*abort)(void *context);
    bool (*busy)(const void *context);
    bool (*read)(const void *context, struct foc_serial_calibration_view *view);
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
    /** Applied RAM-only absolute position-reference limit in mechanical rad. */
    float position_limit_rad;
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
