/**
 * @file    foc_pipeline.h
 * @brief   Single-owner current-control pipeline and output journal.
 * @version v0.1.0
 * @date    2026.11.10
 *
 * Design overview
 * ---------------
 * Connects Device decoding and mapping, Current Core, and the Safety state
 * machine. A staged PI step is committed only after the output accepts UPDATE.
 * The optional feedforward policy consumes only a coherent H1 electrical
 * feedback snapshot supplied with the H0 input.
 *
 * Usage example
 * -------------
 * Zero-initialize and initialize an instance, publish independent heartbeats
 * and a command, then step a fresh sample while disabled. ARM and START at
 * the caller's monotonic time. Step each subsequent coherent sample once.
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * Caller owns all storage and serializes every operation on one instance.
 * Members are read-only to the caller except for Mock rejection controls
 * and calls to the two Safety heartbeat functions. The optional output callback
 * and its argument are retained for the pipeline lifetime.
 *
 * Failure / recovery
 * ------------------
 * Rejected running steps preserve PI state and attempt INHIBIT. Output is
 * unconfirmed if that attempt fails. Start a new session with fresh inputs
 * and explicit ARM and START. START resets the integrators.
 */
#pragma once

#include "foc_current.h"
#include "foc_device_sim.h"
#include "foc_motion.h"
#include "foc_safe.h"

/* Public Types ---------------------------------------------------------------- */

/** Serial final admission check invoked after Safety and before Mock UPDATE. */
typedef bool (*foc_pipeline_update_guard_t)(void *argument);

/**
 * Final synchronous output submission, called by the Pipeline owner only.
 * True acknowledges the requested action at the adapter's scope. UPDATE may
 * return true only after application. A locked adapter may acknowledge a
 * software-only INHIBIT, which does not certify the physical bridge state.
 * The request is borrowed for this call. The callback must not block or reenter.
 */
typedef bool (*foc_pipeline_output_submit_t)(void *argument,
                                             const struct foc_device_pwm_request *request);

/** Pipeline result, separate from the Safety fault bit mask. */
enum foc_pipeline_status {
    /** Operation succeeded. A successful step accepted one complete UPDATE. */
    FOC_PIPELINE_OK = 0,
    /** A required pointer was NULL. */
    FOC_PIPELINE_ERR_NULL = -1,
    /** Core, Device, or Safety settings failed validation. */
    FOC_PIPELINE_ERR_CONFIG = -2,
    /** The pipeline or Safety state did not permit this operation. */
    FOC_PIPELINE_ERR_STATE = -3,
    /** Command is invalid, stale, replayed, or from a prior session. */
    FOC_PIPELINE_ERR_COMMAND = -4,
    /** ADC decoding or the corresponding angle failed validation. */
    FOC_PIPELINE_ERR_SAMPLE = -5,
    /** The sample is stale, from the future, replayed, or out of order. */
    FOC_PIPELINE_ERR_SAMPLE_ORDER = -6,
    /** Current Core rejected the staged calculation. */
    FOC_PIPELINE_ERR_CORE = -7,
    /** Device rejected the Core-to-PWM mapping. */
    FOC_PIPELINE_ERR_MAP = -8,
    /** Safety refused the control cycle. See safety_reasons. */
    FOC_PIPELINE_ERR_GATE = -9,
    /** Output rejected the staged UPDATE and accepted fallback INHIBIT. */
    FOC_PIPELINE_ERR_UPDATE = -10,
    /** Output rejected INHIBIT. Its retained registers are unconfirmed. */
    FOC_PIPELINE_ERR_INHIBIT = -11,
    /** An explicit inhibit preempted a staged UPDATE. */
    FOC_PIPELINE_ERR_STOP = -12,
    /** The supplied current time moved backward or crossed the half range. */
    FOC_PIPELINE_ERR_TIME = -13,
    /** The enabled feedforward path received an invalid or stale electrical-speed snapshot. */
    FOC_PIPELINE_ERR_FEEDBACK = -14,
};

/** Optional current-feedforward policy owned by the Pipeline. */
struct foc_pipeline_feedforward_config {
    /** False preserves the original PI-only control path. */
    bool enabled;
    /** Validated PMSM model copied into the Pipeline at initialization. */
    struct foc_current_feedforward_model model;
    /** Maximum permitted age from the H1 source to the ADC sample, in µs. */
    uint32_t max_age_us;
};

/** Settings copied into a caller-owned pipeline. */
struct foc_pipeline_config {
    /** Portable d/q current-loop settings. */
    struct foc_current_config current;
    /** Device ADC rank scaling, offset, and phase order. */
    struct foc_device_adc_config adc;
    /** Device PWM compare period and phase order. */
    struct foc_device_pwm_config pwm;
    /** Safety timeouts and invalid-sample threshold. */
    struct foc_safe_config safety;
    /** Optional H1 electrical-speed feedforward policy. */
    struct foc_pipeline_feedforward_config feedforward;
    /** Optional final adapter. NULL preserves the closed-gate Host Mock. */
    foc_pipeline_output_submit_t output_submit;
    /** Caller-owned adapter storage, valid throughout the pipeline lifetime. */
    void *output_arg;
};

/** Coherent H1 electrical feedback supplied with one H0 ADC sample. */
struct foc_pipeline_electrical_feedback {
    /** Electrical phase and signed electrical speed at source time. */
    struct foc_electrical_state state;
    /** H1 source capture time in the same clock as adc.sample_tick. */
    uint32_t source_us;
    /** H1 publication identity, retained for diagnostics and tests. */
    uint32_t source_seq;
    /** True only when the complete feedback sample passed H1 validation. */
    bool valid;
};

/** Host-published current target, copied on successful publication. */
struct foc_pipeline_command {
    /** Finite d/q target in A, limited by Current Core during a step. */
    struct foc_vector_dq target_a;
    /** Source sequence, including zero, strictly advancing modulo 2^32. */
    uint32_t seq;
    /** Original producer time in monotonic microseconds, never retimed by receipt. */
    uint32_t produced_us;
};

/** Deterministic Host events applied after mapping and before the final gate. */
struct foc_pipeline_gate_injection {
    /** Simulated time elapsed during calculation in microseconds, below half range. */
    uint32_t delay_us;
    /** Nonzero Safety cause bits to latch before the final gate. */
    uint32_t fault_bits;
    /** Request immediate inhibition before the final gate. */
    bool inhibit;
};

/** Read-only caller snapshot for one control cycle. */
struct foc_pipeline_input {
    /** Raw ADC snapshot, with sample_tick expressed in monotonic microseconds. */
    struct foc_device_adc_raw adc;
    /** Electrical phase in rad measured or predicted for the ADC sample instant. */
    float theta_elec_rad;
    /** Evaluation time of that angle, required to equal adc.sample_tick. */
    uint32_t angle_sample_us;
    /** Current time at cycle entry in the same monotonic clock. */
    uint32_t now_us;
    /** Actual caller-supplied control interval in seconds. */
    float dt_s;
    /** Electrical angle and speed from one coherent H1 publication. */
    struct foc_pipeline_electrical_feedback electrical_feedback;
    /** Test-only event, zero for a normal cycle. */
    struct foc_pipeline_gate_injection before_gate;
    /** Optional derived current target carrying the already observed source command identity. */
    const struct foc_pipeline_command *cycle_target;
    /** Reject UPDATE if the owner requires a derived target and none was supplied. */
    bool require_cycle_target;
    /**
     * Calibration drive: the command target is a d/q voltage in V at theta_elec_rad,
     * applied without the current PI. Phase current is still decoded every cycle and
     * a vector above current_limit_a stops the output. Safety, lease, gate and
     * submission are unchanged.
     */
    bool voltage_drive;
};

/** Monotonic input watermark that survives fault clear and explicit disable. */
struct foc_pipeline_watermark {
    /** Latest consumed sequence. */
    uint32_t seq;
    /** Original time of the latest consumed observation, in microseconds. */
    uint32_t produced_us;
    /** False until the first sequence is consumed, including a possible zero. */
    bool seen;
};

/** Observations from a closed-gate Mock, with two Host failure injection controls. */
struct foc_pipeline_mock {
    /** Last confirmed compare registers. Only use while output_confirmed is true. */
    struct foc_device_sim_registers registers;
    /** Full most recently attempted action and three channel values. */
    struct foc_device_pwm_request last_request;
    /** Last accepted UPDATE for diagnostics, regardless of current action. */
    struct foc_device_pwm_request last_update;
    /** Last rejected UPDATE, retained when a fallback INHIBIT is attempted. */
    struct foc_device_pwm_request last_rejected_update;
    /** Total submission attempts, including rejected actions. */
    uint32_t attempts;
    /** Number of accepted UPDATE actions. */
    uint32_t updates;
    /** Attempt counter value of the latest INHIBIT request. */
    uint32_t last_inhibit_attempt;
    /** Whether the latest attempted action succeeded. */
    bool last_accepted;
    /** Last action was acknowledged at the adapter's declared scope. */
    bool output_confirmed;
    /** Host-only injection of an UPDATE submission failure. */
    bool reject_update;
    /** Host-only injection of an INHIBIT submission failure. */
    bool reject_inhibit;
};

/** Caller-owned pipeline, including live control state and diagnostic snapshot. */
struct foc_pipeline {
    /** Formal current controller state. */
    struct foc_current_context current;
    /** Safety owner, also the destination of independent heartbeat observations. */
    struct foc_safe_context safety;
    /** Device input conversion settings. */
    struct foc_device_adc_config adc_config;
    /** Device output mapping settings. */
    struct foc_device_pwm_config pwm_config;
    /** Optional, validated current feedforward context. */
    struct foc_current_feedforward_context feedforward;
    /** Maximum accepted H1-to-ADC age for the feedforward snapshot. */
    uint32_t feedforward_max_age_us;
    /** Latest accepted d/q target in A. */
    struct foc_vector_dq target_a;
    /** Command sequence/time retained across recovery. */
    struct foc_pipeline_watermark command;
    /** ADC sequence/time retained across recovery, including invalid samples. */
    struct foc_pipeline_watermark sample;
    /** Closed-gate Mock and submission journal. */
    struct foc_pipeline_mock mock;
    /** Optional adapter used after local request validation. */
    foc_pipeline_output_submit_t output_submit;
    /** Lifetime-bound argument passed to the adapter. */
    void *output_arg;
    /** Feedback in A for the latest accepted cycle, cleared on rejection. */
    struct foc_vector_dq measured_dq_a;
    /** Current target in A of the latest accepted cycle, cleared on rejection. */
    struct foc_vector_dq target_dq_a;
    /** Safety causes for the last step or explicit stop. */
    uint32_t safety_reasons;
    /** Step or stop cause prior to any fallback INHIBIT submission failure. */
    enum foc_pipeline_status last_reason;
    /** Latest accepted current time in monotonic microseconds. */
    uint32_t now_us;
    /** Recovery boundary. Fresh command and sample times must follow it. */
    uint32_t rearm_after_us;
    /** True after the first supplied current time, including zero. */
    bool time_seen;
    /** True after an explicit stop, running-session rejection, or fault clear. */
    bool rearm_boundary_seen;
    /** True after initialization and acknowledgement of the initial INHIBIT. */
    bool initialized;
};

/* Public Functions ------------------------------------------------------------- */

/* Lifecycle ------------------------------------------------------------------- */

/**
 * Start a new instance lifetime with copied settings and acknowledged INHIBIT.
 *
 * @param pipeline Caller-owned destination, unchanged on failure.
 * @param config Read-only settings copied during initialization.
 * @return FOC_PIPELINE_OK or a pointer, configuration, or submission error.
 */
int foc_pipeline_init(struct foc_pipeline *pipeline, const struct foc_pipeline_config *config);

/** Enter READY from DISABLED with confirmed INHIBIT and fresh inputs at now_us in µs. */
int foc_pipeline_arm(struct foc_pipeline *pipeline, uint32_t now_us);

/** Enter RUNNING from READY and reset PI after checking inputs and heartbeats at now_us in µs. */
int foc_pipeline_start(struct foc_pipeline *pipeline, uint32_t now_us);

/**
 * Revoke the run session and immediately submit INHIBIT, even if now_us is invalid.
 *
 * @param pipeline Initialized instance.
 * @param now_us Current monotonic time in microseconds.
 * @param fault_bits Safety cause bits to latch, or zero for a non-latching stop.
 * @return FOC_PIPELINE_OK or a pointer, state, time, or inhibit-submission error.
 */
int foc_pipeline_request_inhibit(struct foc_pipeline *pipeline,
                                 uint32_t now_us,
                                 uint32_t fault_bits);

/** Clear a latch with confirmed INHIBIT and require observations after now_us in µs. */
int foc_pipeline_clear_fault(struct foc_pipeline *pipeline, uint32_t now_us);

/* Control --------------------------------------------------------------------- */

/**
 * Publish a newer command within its original age window. Replays never refresh it.
 *
 * @param pipeline Initialized instance.
 * @param command Read-only producer snapshot, copied only on acceptance.
 * @param now_us Current monotonic receipt time in microseconds.
 * @return FOC_PIPELINE_OK or a pointer, state, time, or command error.
 */
int foc_pipeline_set_command(struct foc_pipeline *pipeline,
                             const struct foc_pipeline_command *command,
                             uint32_t now_us);

/**
 * Execute one serial Host cycle. Only an accepted UPDATE changes PI state.
 *
 * @param pipeline Initialized, exclusively owned instance and diagnostic destination.
 * @param input Coherent ADC, angle, and time snapshot. Never modified.
 * @return FOC_PIPELINE_OK on UPDATE acceptance, otherwise the rejection status.
 */
int foc_pipeline_step(struct foc_pipeline *pipeline, const struct foc_pipeline_input *input);

/**
 * Execute the same step with an optional final UPDATE guard.
 *
 * @param pipeline Initialized instance with a single execution owner.
 * @param input Read-only ADC, angle and timing snapshot.
 * @param guard Optional non-blocking admission check. It must not reenter Pipeline.
 * @param argument Caller-owned guard data, used only during the call.
 * @return Step status. A false guard or invalid feedforward snapshot requests
 * INHIBIT and leaves PI unchanged.
 */
int foc_pipeline_step_guarded(struct foc_pipeline *pipeline,
                              const struct foc_pipeline_input *input,
                              foc_pipeline_update_guard_t guard,
                              void *argument);

/* ---------------------------------------- end of file ----------------------------------------- */
