/**
 * @file    foc_board.h
 * @brief   Host-testable Board1 configuration and locked V2 port.
 * @version v0.1.0
 * @date    2026.11.10
 *
 * Design overview
 * ---------------
 * Translate Board1 samples and output requests at the existing Runtime boundary.
 *
 * Usage example
 * -------------
 * Initialize a static runtime with foc_board_init before starting its producers.
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * Initialize once before IRQs. ADC and encoder producers own separate state.
 *
 * Failure / recovery
 * ------------------
 * Initialization errors leave the caller without permission to start producers.
 */
#pragma once

#include "foc_calibration.h"
#include "foc_encoder.h"
#include "foc_runtime.h"
#include "tm_platform_foc_timing.h"

/* Public Types ---------------------------------------------------------------- */

/** Board schedule and range constants shared with the IRQ binding. */
enum foc_board_limits {
    /** AT32F415 timer input after the APB timer multiplier. */
    FOC_BOARD_TIMER_HZ = TM_PLATFORM_TMR_CLK_HZ,
    /** Counter ticks per second on the V2 TMR2/TMR5 timebase. */
    FOC_BOARD_US_HZ = TM_PLATFORM_V2_TIMEBASE_HZ,
    /** Nominal TMR1 event interval in microseconds. */
    FOC_BOARD_H0_US = 200,
    FOC_BOARD_ADC_COMPARE_US = 40,
    FOC_BOARD_ADC_LATEST_US = 110,
    FOC_BOARD_ADC_FRAME_US = 80,
    /** Maximum width of the startup and observation clock brackets. */
    FOC_BOARD_ADC_BRACKET_US = 2,
    /** Nominal TMR5 event interval in microseconds. */
    FOC_BOARD_H1_US = 500,
    /** Identity of Board1's continuous rotor frame (encoder zero and direction) for IMPEDANCE. */
    FOC_BOARD_MECHANICAL_FRAME = 1,
    /** TMR1 period and maximum diagnostic compare. */
    FOC_BOARD_COMPARE_MAX = FOC_BOARD_TIMER_HZ / FOC_BOARD_US_HZ * FOC_BOARD_H0_US - 1,
};

/** Last disposition of a complete output request, with software scope only. */
enum foc_board_output_result {
    /** INHIBIT was accepted as software policy for this locked image. */
    FOC_BOARD_INHIBIT_SOFTWARE,
    /** An eligible UPDATE was checked but never applied. */
    FOC_BOARD_UPDATE_NOT_APPLIED,
    /** ZERO_DUTY cannot be executed by the locked image. */
    FOC_BOARD_ZERO_DUTY_LOCKED,
    /** ACTIVE_BRAKE cannot be executed by the locked image. */
    FOC_BOARD_ACTIVE_BRAKE_LOCKED,
    /** The action or its payload failed validation. */
    FOC_BOARD_OUTPUT_INVALID,
    /** An attached backend applied a validated action. Appended; earlier values unchanged. */
    FOC_BOARD_OUTPUT_APPLIED,
};

/**
 * Optional output backend for a validated request. The locked AT32 image
 * attaches none, so every UPDATE stays FOC_BOARD_UPDATE_NOT_APPLIED. A Host
 * SIL attaches a virtual plant. Called from H0 only; return true when the
 * request took effect at the backend.
 */
typedef bool (*foc_board_output_backend)(void *argument,
                                         const struct foc_device_pwm_request *request);

#if defined(FALCONFOC_RT02_TIMING_TRACE)
enum foc_board_trace_point {
    FOC_BOARD_TRACE_SOFTWARE_TRIGGER = 1U,
    FOC_BOARD_TRACE_PCCE_ENTRY = 2U,
    FOC_BOARD_TRACE_H0 = 4U,
    FOC_BOARD_TRACE_ADAPTER = 8U,
    FOC_BOARD_TRACE_UPDATE = 16U,
    FOC_BOARD_TRACE_H0_INPUTS = 32U,
    FOC_BOARD_TRACE_H0_GUARD = 64U,
    FOC_BOARD_TRACE_H0_CONTROL_DONE = 128U,
};

/** One transaction, in CYCCNT cycles. Phase is a TMR1 counter sample. */
struct foc_board_adc_trace {
    uint32_t compare_irq_cycles;
    uint32_t compare_irq_phase_ticks;
    uint32_t trigger_proxy_cycles;
    uint32_t trigger_write_return_cycles;
    uint32_t pcce_irq_cycles;
    uint32_t ranks_ready_cycles;
    uint32_t h0_start_cycles;
    uint32_t h0_finish_cycles;
    uint32_t adapter_start_cycles;
    uint32_t adapter_return_cycles;
    uint32_t update_start_cycles;
    uint32_t update_return_cycles;
    uint32_t h0_inputs_cycles;
    uint32_t h0_guard_cycles;
    uint32_t h0_control_done_cycles;
    uint32_t sensor_capture_us;
    uint32_t sensor_seq;
    uint32_t prediction_us;
    uint32_t motion_session;
    uint32_t accepted_outer_seq;
    enum foc_board_output_result adapter_result;
    enum foc_board_output_result update_result;
    unsigned int points;
    bool adapter_accepted;
    bool update_accepted;
    bool sensor_used;
    bool outer_accepted;
};

typedef uint32_t (*foc_board_trace_clock)(void);
struct foc_board_adc;
#endif

/** H0-owned attempt journal, separate from the Pipeline result. */
struct foc_board_output {
    /** Last complete action received. */
    struct foc_device_pwm_request request;
    /** Disposition of request. Never establishes physical confirmation. */
    enum foc_board_output_result result;
    /** Number of valid UPDATEs declined by the board. */
    uint32_t declined_updates;
    /** Number of valid UPDATEs applied by the attached backend. */
    uint32_t applied_updates;
    /** Optional backend and its argument; NULL keeps the locked policy. */
    foc_board_output_backend backend;
    void *backend_argument;
#if defined(FALCONFOC_RT02_TIMING_TRACE)
    struct foc_board_adc *trace_owner;
#endif
};

/** Sticky counter witnesses acknowledged by the IRQ binding after a coherent read. */
enum foc_board_adc_clock_flags {
    FOC_BOARD_TIME_WRAP = 1U,
    FOC_BOARD_TIME_HALF = 2U,
    FOC_BOARD_SLOT_WRAP = 4U,
};

/** Live TMR1 phase bracketed by TMR2, with timer configuration verified by the binding. */
struct foc_board_adc_event {
    uint32_t before_us;
    uint32_t now_us;
    uint32_t phase_ticks;
    unsigned int flags;
    bool coherent;
#if defined(FALCONFOC_RT02_TIMING_TRACE)
    uint32_t irq_entry_cycles;
    uint32_t irq_entry_phase_ticks;
    uint32_t ranks_ready_cycles;
#endif
};

enum foc_board_adc_state {
    FOC_BOARD_ADC_IDLE,
    FOC_BOARD_ADC_PENDING,
    FOC_BOARD_ADC_DRAIN,
    FOC_BOARD_ADC_COMPLETED,
    FOC_BOARD_ADC_REJECTED,
};

enum foc_board_adc_rejection {
    FOC_BOARD_ADC_NO_REJECTION,
    FOC_BOARD_ADC_AMBIGUOUS_CLOCK,
    FOC_BOARD_ADC_TRIGGER_WINDOW,
    FOC_BOARD_ADC_DUPLICATE_TRIGGER,
    FOC_BOARD_ADC_OVERLAP,
    FOC_BOARD_ADC_UNDRAINED_PCCE,
    FOC_BOARD_ADC_UNMATCHED_PCCE,
    FOC_BOARD_ADC_LATE,
    FOC_BOARD_ADC_BAD_RANKS,
};

/** TIMER_SLOT_ONLY; the load marker is the logical next timer boundary.
 * Board1 always reports PHYSICAL_OUTPUT_NOT_APPLIED, including after H0. */
struct foc_board_adc_transaction {
    uint32_t slot_id;
    uint32_t generation;
    /** Request/ADC source proxy before PCSWTRG, not the physical aperture. */
    uint32_t requested_us;
    uint32_t completed_us;
    uint32_t diagnostic_load_us;
    int h0_result;
    enum foc_board_adc_state state;
    enum foc_board_adc_rejection rejection;
    enum foc_board_output_result final_output;
    bool adc_valid;
    bool h0_called;
    bool h0_consumed;
    bool update_accepted;
    bool update_declined;
    bool physical_output_applied;
#if defined(FALCONFOC_RT02_TIMING_TRACE)
    struct foc_board_adc_trace trace;
#endif
};

/** Counters saturate; an event can have one rejection and increment its diagnostic subset. */
struct foc_board_adc_diagnostics {
    /** Distinct slots with a coherent CH4 or PCCE observation. */
    uint32_t observed_slots;
    /** Closed slots without a timely valid ADC frame, including gaps without IRQs. */
    uint32_t missing_slots;
    uint32_t trigger_requests;
    /** Drained PCCE matched to a timely token, before validating ranks. */
    uint32_t adc_completions;
    /** PCCE without a live token (duplicate or otherwise orphaned). */
    uint32_t unmatched_completions;
    uint32_t late_completions;
    uint32_t overlap_rejections;
    /** Count of new ADC watermarks observed inside the actual Runtime. */
    uint32_t h0_consumed;
    uint32_t timing_rejections;
    uint32_t unattributable_events;
    uint32_t slot_id;
    bool clock_valid;
    enum foc_board_adc_rejection last_rejection;
    struct foc_board_adc_transaction transaction;
};

/** Optional H0 observer of each valid ADC frame (ranks VBUS, A, B, C in rank order). */
typedef void (*foc_board_adc_tap)(void *argument,
                                  const uint16_t *ranks,
                                  uint32_t generation,
                                  uint32_t sample_us);

/** Optional observer called in H0 right after each Runtime H0 step of a valid frame. */
typedef void (*foc_board_h0_observer)(void *argument,
                                      const struct foc_runtime *runtime,
                                      uint32_t slot_id,
                                      int h0_result);

/** Optional gate-driver fault line reader for H0: true while the line is asserted (low). */
typedef bool (*foc_board_driver_fault_line)(void *argument);

/** H0 samples in a row with the driver fault line asserted before Runtime latches it. */
#define FOC_BOARD_DRIVER_FAULT_SAMPLES 2U

struct foc_board_thermal;

/** Two non-preempting IRQs own the clock, token and live diagnostics.
 * A single foreground reader requests a coherent copy through snapshot_state. */
struct foc_board_adc {
    uint32_t slot_start_us;
    uint32_t last_clock_us;
    uint32_t epoch_width_us;
    uint32_t next_generation;
    bool timing_initialized;
    bool slot_served;
    bool trigger_seen;
    struct foc_board_adc_diagnostics live;
    atomic_uint snapshot_state;
    struct foc_board_adc_diagnostics published;
    /** Optional observer, set before producers start; never set in the locked image path. */
    foc_board_adc_tap tap;
    void *tap_argument;
    /** Optional post-H0 observer, set before producers start. */
    foc_board_h0_observer observer;
    void *observer_argument;
    /** Optional DRV8311 nFAULT reader, set before producers start; debounced in H0. */
    foc_board_driver_fault_line driver_fault_line;
    void *driver_fault_argument;
    uint32_t driver_fault_samples;
    /** Optional NTC poller whose derating and trips H0 receives, set before producers start. */
    struct foc_board_thermal *thermal;
#if defined(FALCONFOC_RT02_TIMING_TRACE)
    foc_board_trace_clock trace_cycles;
#endif
};

/** Why the latest encoder transaction did not publish usable feedback. */
enum foc_board_encoder_result {
    /** Complete and timely 16-bit response. */
    FOC_BOARD_ENCODER_OK,
    /** No response by the bounded window. */
    FOC_BOARD_ENCODER_TIMEOUT,
    /** Peripheral reported an error or frame count was wrong. */
    FOC_BOARD_ENCODER_FRAME,
    /** Previous transaction still occupied the window. */
    FOC_BOARD_ENCODER_OVERLAP,
    /** Time or angle displacement was outside the permitted range. */
    FOC_BOARD_ENCODER_DISCONTINUITY,
};

#if defined(FALCONFOC_RT02_TIMING_TRACE)
/** One H1-owned close/decode/publication observation. Cycle spans include preemption. */
struct foc_board_encoder_trace {
    uint32_t capture_us;
    uint32_t close_us;
    uint32_t seq;
    uint32_t decode_start_cycles;
    uint32_t decode_done_cycles;
    uint32_t h1_start_cycles;
    uint32_t publish_proxy_cycles;
    uint32_t h1_finish_cycles;
    int h1_status;
    bool publishing;
};
#endif

/** H1-owned transaction and continuous mechanical position state. */
struct foc_board_encoder {
    /** Time of current frame start in microseconds. */
    uint32_t captured_us;
    /** Sequence of completed or failed observations. */
    uint32_t seq;
    /** Consecutive unsuccessful transactions, saturating. */
    uint32_t losses;
    /** Portable angle, unwrap, velocity, and quality owner. */
    struct foc_encoder_context sensor;
    /** Latest portable sensor result for local diagnostics. */
    struct foc_encoder_feedback feedback;
    /** Latest transaction disposition. */
    enum foc_board_encoder_result result;
    /** True between transport start and its single completion. */
    bool pending;
    /** True after a gap makes the continuous position ambiguous for this session. */
    bool continuity_lost;
    /** Explicit Sensor policy selected by a pre-initialization snapshot. */
    struct foc_encoder_config config;
    /** True when config is an explicit calibrated policy. */
    bool config_valid;
#if defined(FALCONFOC_RT02_TIMING_TRACE)
    foc_board_trace_clock trace_cycles;
    struct foc_board_encoder_trace trace;
#endif
};

/* Public Functions ------------------------------------------------------------- */

/** Initialize the reference control instance at the common microsecond epoch. */
int foc_board_init(struct foc_runtime *runtime, struct foc_board_output *output, uint32_t now_us);

/**
 * Initialize an explicit calibrated instance before any producer has started.
 *
 * The default foc_board_init() path is unchanged. This opt-in entry validates
 * the accepted snapshot against Board1's immutable ADC, phase, and encoder
 * facts, initializes Runtime from local candidate copies, and only then
 * commits the caller-owned Runtime, output journal, and Sensor policy. A
 * previously initialized encoder is rejected so its continuous state cannot
 * be changed in place.
 */
int foc_board_init_with_calibration(struct foc_runtime *runtime,
                                    struct foc_board_output *output,
                                    struct foc_board_encoder *encoder,
                                    const struct foc_calibration_snapshot *snapshot,
                                    uint32_t now_us);

/** Board1 calibration identity from immutable ADC, phase and raw encoder facts. */
void foc_board_calibration_identity(uint16_t pole_pairs, struct foc_calibration_identity *identity);

/** Sensor policy this encoder instance uses: explicit calibrated or Board1 nominal. */
void foc_board_encoder_policy(const struct foc_board_encoder *encoder,
                              struct foc_encoder_config *config);

/** Convert an inclusive half-range-valid interval to seconds. */
bool foc_board_elapsed_s(uint32_t now_us, uint32_t previous_us, uint32_t limit_us, float *seconds);

/** Set the epoch of a zero-initialized instance once before producers start. */
bool foc_board_adc_init_timing(struct foc_board_adc *adc,
                               uint32_t before_us,
                               uint32_t after_us,
                               uint32_t initial_phase_ticks);

/** Reserve one conversion token for a verified compare window with no stale PCCE. */
bool foc_board_adc_begin(struct foc_runtime *runtime,
                         struct foc_board_adc *adc,
                         const struct foc_board_adc_event *event,
                         bool pcce_pending);

/** Retire a uniquely matched group after acknowledging PCCE. */
int foc_board_adc_complete(struct foc_runtime *runtime,
                           struct foc_board_adc *adc,
                           struct foc_board_output *output,
                           const uint16_t *ranks,
                           unsigned int count,
                           const struct foc_board_adc_event *event,
                           bool pcce_drained);

/** IRQ owner serves one pending diagnostic request after handling a hardware event. */
void foc_board_adc_publish(struct foc_board_adc *adc);

/** Single foreground reader. BUSY requests/awaits a copy without modifying diagnostics. */
int foc_board_adc_read(struct foc_board_adc *adc, struct foc_board_adc_diagnostics *diagnostics);

/** Start one encoder transaction. Overlap invalidates the outstanding response. */
bool foc_board_encoder_begin(struct foc_board_encoder *encoder, uint32_t now_us);

/** Complete or fail one transaction and publish its quality to H1. */
int foc_board_encoder_complete(struct foc_runtime *runtime,
                               struct foc_board_encoder *encoder,
                               uint32_t now_us,
                               enum foc_board_encoder_result transport,
                               uint16_t code);

/**
 * Re-initialize a quiescent instance with the nominal boot configuration (after
 * CONFIG_ERASE). The caller holds H0/H1 producers off; the output backend survives.
 */
int foc_board_reconfigure_nominal(struct foc_runtime *runtime,
                                  struct foc_board_output *output,
                                  struct foc_board_encoder *encoder,
                                  uint32_t now_us);

/**
 * Re-initialize a quiescent instance with an accepted calibration snapshot.
 * The caller holds H0/H1 producers off for the duration. The output backend
 * survives; on failure Runtime, journal and encoder keep their previous state.
 */
int foc_board_reconfigure(struct foc_runtime *runtime,
                          struct foc_board_output *output,
                          struct foc_board_encoder *encoder,
                          const struct foc_calibration_snapshot *snapshot,
                          uint32_t now_us);

/** Validate a complete request and record its disposition. Without a backend
 * UPDATE, ZERO_DUTY and ACTIVE_BRAKE are declined and INHIBIT is software-only. */
bool foc_board_output_submit(void *argument, const struct foc_device_pwm_request *request);

/**
 * Attach a backend after boot selection and before any producer starts.
 * Boot selection rewrites the journal, so attaching earlier has no effect.
 * Rejects NULL arguments and a second attachment.
 */
bool foc_board_output_attach(struct foc_board_output *output,
                             foc_board_output_backend backend,
                             void *argument);

/* ---------------------------------------- end of file ----------------------------------------- */
