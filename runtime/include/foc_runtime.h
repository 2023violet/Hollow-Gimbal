/**
 * @file    foc_runtime.h
 * @brief   Single-core Host execution of the FalconFOC current pipeline.
 * @version v0.1.0
 * @date    2026.11.10
 *
 * Design overview
 * ---------------
 * H0 owns Pipeline and output. H1 publishes sensor snapshots. Foreground
 * publishes commands and supervises H0 without writing its Safety state.
 *
 * Usage example
 * -------------
 * Initialize before enabling events. Publish a command and run H1 and H0
 * with matching source timestamps. Poll Foreground independently. Deliver
 * ARM and START at distinct H0 events with valid observations.
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * Caller owns static instance storage. For one instance the priority order
 * is H0 > H1 > Foreground, without reentry or parallel Host threads. H0
 * reads released lower-priority slots that cannot change during the copy.
 * Telemetry fields and the independent stop channel are lock-free atomics.
 * Only request_inhibit may run from an independent event source.
 *
 * Failure / recovery
 * ------------------
 * Timing or input failures prevent UPDATE. Pending stop masks the published
 * output until H0 has processed it. Fault clear and rearm use Pipeline rules.
 */
#pragma once

#include "foc_motion.h"
#include "foc_pipeline.h"
#include "foc_trajectory.h"

#include <stdatomic.h>

/* Public Types ---------------------------------------------------------------- */

/** Runtime service and snapshot outcomes. */
enum foc_runtime_status {
    /** Service or publication completed. For H0, one UPDATE was accepted. */
    FOC_RUNTIME_OK = 0,
    /** No task was due, or H0 served a valid inactive session. */
    FOC_RUNTIME_IDLE = 1,
    /** Missing or invalid input. */
    FOC_RUNTIME_ERR_INPUT = -1,
    /** Invalid monotonic time, producer timestamp, or sequence. */
    FOC_RUNTIME_ERR_ORDER = -2,
    /** A deadline or execution budget was exceeded. */
    FOC_RUNTIME_ERR_TIMING = -3,
    /** A pending stop prevented an UPDATE or session transition. */
    FOC_RUNTIME_ERR_STOP = -4,
    /** Pipeline or lifecycle operation rejected the control event. */
    FOC_RUNTIME_ERR_CONTROL = -5,
    /** The bounded telemetry read intersected an H0 publication. */
    FOC_RUNTIME_ERR_BUSY = -6,
    /** Runtime or Pipeline initialization settings were invalid. */
    FOC_RUNTIME_ERR_CONFIG = -7,
    /** Sensed angle quality, source time, or prediction policy blocked a cycle. */
    FOC_RUNTIME_ERR_ALIGNMENT = -8,
};

/** One lifecycle request executed by the H0 owner on a current event. */
enum foc_runtime_action {
    /** Normal sample service without a lifecycle request. */
    FOC_RUNTIME_ACTION_NONE = 0,
    /** Request DISABLED to READY after the sample is observed. */
    FOC_RUNTIME_ACTION_ARM,
    /** Request READY to RUNNING. The next sample is the first possible UPDATE. */
    FOC_RUNTIME_ACTION_START,
    /** Request explicit fault clear with confirmed inhibition. */
    FOC_RUNTIME_ACTION_CLEAR_FAULT,
    /** Select a closed-loop mode while disabled and confirmed inhibited. */
    FOC_RUNTIME_ACTION_SET_MODE,
};

/** Closed-loop modes. Legacy voltage open loop remains a separate Core path. */
enum foc_runtime_mode {
    /** Foreground supplies d/q current in A. */
    FOC_RUNTIME_MODE_CURRENT = 0,
    /** H1 maps mechanical rad/s to q current. */
    FOC_RUNTIME_MODE_VELOCITY,
    /** H1 cascades continuous mechanical rad to speed and q current. */
    FOC_RUNTIME_MODE_POSITION,
};

/** Outcome of the single Foreground-requested command-timeout recovery. */
enum foc_runtime_clear_result {
    /** No recovery was requested in this instance lifetime. */
    FOC_RUNTIME_CLEAR_NONE,
    /** Foreground published a request awaiting H0 validation. */
    FOC_RUNTIME_CLEAR_PENDING,
    /** H0 completed the restricted clear and entered DISABLED. */
    FOC_RUNTIME_CLEAR_APPLIED,
    /** The request was cancelled or failed H0 validation. */
    FOC_RUNTIME_CLEAR_REJECTED,
};

/** Result of a Foreground-published lifecycle request after H0 observation. */
enum foc_runtime_request_result {
    /** No request has reached the result publication. */
    FOC_RUNTIME_REQUEST_NONE,
    /** The request is stored and waiting for a due H0 boundary. */
    FOC_RUNTIME_REQUEST_PENDING,
    /** H0 completed the requested software lifecycle transition. */
    FOC_RUNTIME_REQUEST_APPLIED,
    /** H0 rejected the request without applying the transition. */
    FOC_RUNTIME_REQUEST_REJECTED,
    /** STOP, a fatal fault, or session invalidation cancelled the request. */
    FOC_RUNTIME_REQUEST_CANCELLED,
};

/** Foreground command with a source identity shared by all closed-loop modes. */
struct foc_runtime_command {
    /** Mode to which this command applies. */
    enum foc_runtime_mode mode;
    /** Strictly advancing source sequence, including zero. */
    uint32_t seq;
    /** Original producer time in monotonic microseconds. */
    uint32_t produced_us;
    /** Mode-specific reference. */
    union {
        /** d/q current target in A, for CURRENT. */
        struct foc_vector_dq current_a;
        /** Mechanical rad/s for VELOCITY or continuous rad for POSITION. */
        float motion_ref;
    } target;
};

/** Foreground-to-H0 lifecycle request with explicit producer identity. */
struct foc_runtime_lifecycle_request {
    /** Existing H0-owned lifecycle action. CLEAR_FAULT is not mailbox-published. */
    enum foc_runtime_action action;
    /** Requested mode for SET_MODE; ignored for ARM and START. */
    enum foc_runtime_mode requested_mode;
    /** Strictly newer producer sequence, including zero. */
    uint32_t request_sequence;
    /** Motion session observed by the producer before publication. */
    uint32_t source_session;
    /** Producer timestamp in the same monotonic clock as H0. */
    uint32_t produced_us;
};

/** H0-published lifecycle request result. */
struct foc_runtime_lifecycle_result {
    /** Terminal or pending request state. */
    enum foc_runtime_request_result result;
    /** Action copied from the request for result identity. */
    enum foc_runtime_action action;
    /** Mode copied from the request for result identity. */
    enum foc_runtime_mode requested_mode;
    /** Request sequence associated with this result. */
    uint32_t request_sequence;
    /** Motion session after H0 handled the request. */
    uint32_t applied_session;
    /** H0 timestamp that produced the result. */
    uint32_t h0_us;
    /** Published Safety state after H0 handling. */
    enum foc_safe_state state;
    /** Latched Safety causes after H0 handling. */
    uint32_t faults;
    /** Runtime/Pipeline status explaining a rejection or cancellation. */
    int pipeline_status;
};

/** Deterministic Host preemption positions. */
enum foc_runtime_hook_point {
    /** H1 has begun writing its inactive slot. */
    FOC_RUNTIME_HOOK_SENSOR_WRITING,
    /** Foreground has begun writing its inactive command slot. */
    FOC_RUNTIME_HOOK_COMMAND_WRITING,
    /** H0 has captured inputs before running Pipeline. */
    FOC_RUNTIME_HOOK_H0_INPUTS,
    /** The Safety gate passed, immediately before serial Mock admission. */
    FOC_RUNTIME_HOOK_BEFORE_SUBMIT,
    /** Pipeline has returned from the serial Mock and PI transaction. */
    FOC_RUNTIME_HOOK_AFTER_CONTROL,
    /** A Foreground snapshot read has loaded part of the publication. */
    FOC_RUNTIME_HOOK_TELEMETRY_READING,
};

struct foc_runtime;

/** Host injection callback. Only higher-priority entries or atomic stop may nest. */
typedef void (*foc_runtime_hook_t)(struct foc_runtime *runtime,
                                   enum foc_runtime_hook_point point,
                                   void *argument);

/** Timing policy for one fixed service. All values use monotonic microseconds. */
struct foc_runtime_lane_config {
    /** Positive nominal period below half the clock range. */
    uint32_t period_us;
    /** Maximum delay from the oldest outstanding deadline, below half range. */
    uint32_t max_delay_us;
    /** Positive simulated execution budget, below half range. */
    uint32_t budget_us;
};

/** Optional mechanical-feedback and outer-loop policy, copied at initialization. */
struct foc_runtime_motion_config {
    /** True selects mechanical feedback and enables the two outer modes. */
    bool enabled;
    /** Mechanical/electrical mapping for this motor. */
    struct foc_rotor_config rotor;
    /** Forward angle prediction limits, separate from sensor age. */
    struct foc_prediction_config prediction;
    /** H1 velocity PI gains and maximum actual interval. */
    struct foc_velocity_config velocity;
    /** Position P gain and velocity reference limit for both outer modes. */
    struct foc_position_config position;
    /** Symmetric q current limit in A, at most the inner current limit. */
    float iq_limit_a;
    /** Optional velocity/position trajectory shaping, disabled by default. */
    struct foc_trajectory_config trajectory;
};

/** Caller-selected schedules and Host injection configuration. */
struct foc_runtime_config {
    /** H0 sample service timing. */
    struct foc_runtime_lane_config h0;
    /** H1 sensor service timing. */
    struct foc_runtime_lane_config h1;
    /** Foreground safety service timing. */
    struct foc_runtime_lane_config supervision;
    /** Foreground telemetry service timing. */
    struct foc_runtime_lane_config telemetry;
    /** Total simulated cost permitted per Foreground poll, in microseconds. */
    uint32_t foreground_budget_us;
    /** Inclusive angle age limit at H0 consumption, in microseconds. */
    uint32_t angle_timeout_us;
    /** Optional Host hook. Null for ordinary execution. */
    foc_runtime_hook_t hook;
    /** Caller-owned hook data, valid for the lifetime of this instance. */
    void *hook_arg;
    /** Optional sensed motion configuration. Zero initialization preserves SW01B. */
    struct foc_runtime_motion_config motion;
};

/** Coherent H1 snapshot. */
struct foc_runtime_sensor {
    /**
     * Electrical phase in rad, from the same capture as position and speed.
     * Motion-enabled producers supply [0, Float32 2*pi) with poles/offset applied
     * from this Runtime's immutable rotor config, independently of continuous
     * Float32 position. Direct-angle mode retains its finite-angle contract.
     */
    float theta_elec_rad;
    /** Angle capture time in monotonic microseconds. */
    uint32_t captured_us;
    /** Producer sequence, including zero. */
    uint32_t seq;
    /** True if the producer considers the sample usable. */
    bool valid;
    /** Continuous mechanical position in rad when motion feedback is enabled. */
    float mechanical_rad;
    /** Coherent mechanical velocity in rad/s when motion feedback is enabled. */
    float mechanical_rad_s;
    /**
     * Explicit periodic-phase provenance/admission from the producer; required
     * only in motion-enabled mode, together with valid. Zero initialization is
     * not a valid zero angle. No fallback to accumulated mechanical position.
     */
    bool electrical_phase_valid;
};

/** One complete H1 publication. Its sensor timestamp dates angle, speed, and any derived target. */
struct foc_runtime_sensor_slot {
    /** Source feedback and quality from one capture. */
    struct foc_runtime_sensor sample;
    /** Electrical state at that same source time, before H0 prediction. */
    struct foc_electrical_state electrical;
    /** Derived target with original Foreground command sequence and timestamp. */
    struct foc_pipeline_command target;
    /** H0 mode under which the target was calculated. */
    enum foc_runtime_mode mode;
    /** H0 session generation under which the target was calculated. */
    uint32_t session;
    /** True only when this publication carries an eligible derived target. */
    bool target_valid;
};

/** Caller snapshot of one H0 trigger. */
struct foc_runtime_fast_input {
    /** Raw ADC snapshot with source timestamp in monotonic microseconds. */
    struct foc_device_adc_raw adc;
    /** Time of this H0 trigger in microseconds. */
    uint32_t now_us;
    /** Host-injected cost for budget checks, in microseconds. */
    uint32_t cost_us;
    /** Optional lifecycle operation for this event only. */
    enum foc_runtime_action action;
    /** Requested closed-loop mode for SET_MODE only. */
    enum foc_runtime_mode requested_mode;
};

/** Result of one Foreground poll. */
struct foc_runtime_foreground_result {
    /** True after completed safety supervision. */
    bool supervised;
    /** True when R2 permits feeding on this completed supervision cycle. */
    bool watchdog_permitted;
    /** True when a due, affordable telemetry copy was coherent. */
    bool telemetry_ready;
};

/** Coherent H0 output and health view copied by Foreground. */
struct foc_runtime_snapshot {
    /** Safety main state at publication. */
    enum foc_safe_state state;
    /** Latched and pending causes. */
    uint32_t faults;
    /** Current effective software output action. */
    enum foc_device_output_action action;
    /** Three compare values in channel order. Zero metadata when stop is pending. */
    uint32_t compare[3];
    /** Total accepted UPDATE count. */
    uint32_t updates;
    /** Latest observed ADC interval in microseconds, zero before a pair exists. */
    uint32_t interval_us;
    /** Actual H0 trigger time in microseconds. */
    uint32_t trigger_us;
    /** H0 delay from its oldest outstanding deadline in microseconds. */
    uint32_t delay_us;
    /** H0 skipped nominal periods. */
    uint32_t missed;
    /** Latest published Pipeline step, lifecycle, or stop status. */
    int pipeline_status;
    /** Last completed Fast service time in microseconds. */
    uint32_t fast_us;
    /** Time of the last accepted command in microseconds. */
    uint32_t command_us;
    /** Runtime publication identity of the most recently observed command. */
    uint32_t command_seq;
    /** Time of the last valid ADC observation in microseconds. */
    uint32_t sample_us;
    /** True if the output action was acknowledged at the adapter's declared scope. */
    bool output_confirmed;
    /** True when independent stop is pending H0 acknowledgement. */
    bool inhibit_pending;
    /** True after actual H0 progress. */
    bool fast_seen;
    /** True if Safety has a command observation. */
    bool command_seen;
    /** True if Safety has a valid sample observation. */
    bool sample_seen;
    /** H0-owned active closed-loop mode. */
    enum foc_runtime_mode mode;
    /** Generation invalidating targets across stops, clears, and mode changes. */
    uint32_t motion_session;
    /** Sensor sequence of the last H1 target accepted in this session. */
    uint32_t accepted_outer_seq;
    /** True if an H1 target was accepted in this session. */
    bool accepted_outer_seen;
    /** Forward prediction interval for the latest aligned H0 sample in µs. */
    uint32_t angle_prediction_us;
    /** Recovery mailbox result, independent of the telemetry generation. */
    enum foc_runtime_clear_result clear_result;
    /** H0-consumed mechanical observation with its original source time. */
    float mechanical_rad;
    /** Mechanical velocity in rad/s from the same H0-consumed sensor view. */
    float mechanical_rad_s;
    /** Original capture time in monotonic microseconds. */
    uint32_t sensor_us;
    /** Valid at H0 publication. A consumer must also check its current age. */
    bool sensor_valid;
};

/** Diagnostic counters for one domain. Inspect its non-atomic fields when quiescent. */
struct foc_runtime_lane {
    /** Next scheduled deadline in monotonic microseconds. */
    uint32_t next_us;
    /** Last accepted call time in monotonic microseconds. */
    uint32_t last_call_us;
    /** Actual time of the last due invocation in microseconds. */
    uint32_t trigger_us;
    /** Oldest outstanding deadline at that invocation, in microseconds. */
    uint32_t deadline_us;
    /** Trigger minus deadline in microseconds. */
    uint32_t delay_us;
    /** Number of expired periods skipped. */
    uint32_t missed;
    /** Number of due invocations. */
    uint32_t due_count;
    /** Number of invocations past the delay limit. */
    uint32_t late_count;
    /** Number of invocations over budget. */
    uint32_t budget_count;
};

/** Number of atomic payload words in the fixed telemetry publication. */
enum {
    FOC_RUNTIME_TELEMETRY_WORDS = 23
};

/** Caller-owned instance. Internal fields are read-only outside their owners. */
struct foc_runtime {
    /** Validated immutable execution settings. */
    struct foc_runtime_config config;
    /** Initialization time, in microseconds, used for first Fast heartbeat grace. */
    uint32_t boot_us;
    /** Set once initialization completes. */
    bool initialized;
    /** H0-owned control, Safety, and Mock. Inspect only when quiescent. */
    struct foc_pipeline pipeline;
    /** H0 schedule and diagnostics. */
    struct foc_runtime_lane h0;
    /** H1 schedule and diagnostics. */
    struct foc_runtime_lane h1;
    /** Foreground supervision schedule and diagnostics. */
    struct foc_runtime_lane supervision;
    /** Foreground telemetry schedule and diagnostics. */
    struct foc_runtime_lane telemetry;
    /** Foreground low-priority samples skipped or dropped. */
    uint32_t telemetry_dropped;
    /** Latest ADC source watermark, owned by H0. */
    struct foc_pipeline_watermark adc;
    /** Latest command consumed by H0, including a rejected observation. */
    struct foc_pipeline_watermark consumed_command;
    /** Time of the latest Foreground heartbeat imported by H0, in microseconds. */
    uint32_t consumed_foreground_us;
    /** True once H0 imports a Foreground heartbeat. */
    bool consumed_foreground_seen;
    /** Actual H0 sample interval in microseconds. */
    uint32_t interval_us;
    /** Last Pipeline step or lifecycle result, owned by H0. */
    int pipeline_status;
    /** H0-owned closed-loop mode, initially CURRENT. */
    enum foc_runtime_mode mode;
    /** H0-owned motion session generation. */
    uint32_t motion_session;
    /** H0-owned acknowledgement of the accepted H1 target. */
    uint32_t accepted_outer_seq;
    /** Whether H0 has acknowledged a target in this session. */
    bool accepted_outer_seen;
    /** H0-owned prediction interval in µs, zero on rejection or exact alignment. */
    uint32_t angle_prediction_us;
    /** Latest H0-consumed sensor view for coherent telemetry publication. */
    struct foc_runtime_sensor observed_sensor;
    /** H1-owned committed velocity PI state. */
    struct foc_velocity_context outer_velocity;
    /** H1-owned integral proposal for its latest target. */
    float outer_trial_a;
    /** True when the last H1 publication has an integral proposal. */
    bool outer_trial_seen;
    /** H1-owned trajectory state for the current session. */
    struct foc_trajectory_context trajectory;
    /** Trial trajectory state awaiting H0 acceptance. */
    struct foc_trajectory_context trajectory_trial;
    /** H1-written slots. H0 reads only the published slot. */
    struct foc_runtime_sensor_slot sensor_slots[2];
    /** Foreground-written slots. H0 reads only the published slot. */
    struct foc_runtime_command command_slots[2];
    /** Sensor slot index or the unpublished sentinel. */
    atomic_uint sensor_slot;
    /** Command slot index or the unpublished sentinel. */
    atomic_uint command_slot;
    /** Foreground-owned heartbeat timestamp in monotonic microseconds. */
    atomic_uint foreground_us;
    /** Zero before the first completed supervision, one thereafter. */
    atomic_uint foreground_seen;
    /** Independent pending stop and fault bits, atomically merged and drained. */
    atomic_uint stop_requests;
    /** H0 publication sequence, odd during a write. */
    atomic_uint telemetry_sequence;
    /** Fixed H0 telemetry payload, with atomic loads and stores for each word. */
    atomic_uint telemetry_words[FOC_RUNTIME_TELEMETRY_WORDS];
    /** Signed Pipeline result published with the same telemetry sequence. */
    atomic_int telemetry_pipeline_status;
    /** Foreground publication time for the single recovery request. */
    uint32_t clear_requested_us;
    /** Foreground publishes PENDING; H0 completes or rejects this request. */
    atomic_uint clear_result;
    /** Foreground-written bounded lifecycle request slots. */
    struct foc_runtime_lifecycle_request lifecycle_requests[2];
    /** H0-written bounded lifecycle result slots. */
    struct foc_runtime_lifecycle_result lifecycle_results[2];
    /** Published request slot index; release after the complete record write. */
    atomic_uint lifecycle_request_slot;
    /** 0 idle, 1 published, 2 claimed by H0. */
    atomic_uint lifecycle_request_pending;
    /** Published result slot index; release after the complete result write. */
    atomic_uint lifecycle_result_slot;
    /** Last accepted request sequence and whether it has been initialized. */
    atomic_uint lifecycle_last_sequence;
    atomic_uint lifecycle_sequence_seen;
};

/* Public Functions ------------------------------------------------------------- */

/* Lifecycle ------------------------------------------------------------------- */

/** Initialize a new, quiescent instance with schedules starting at now_us in µs. */
int foc_runtime_init(struct foc_runtime *runtime,
                     const struct foc_pipeline_config *pipeline_config,
                     const struct foc_runtime_config *config,
                     uint32_t now_us);

/* Execution domains ----------------------------------------------------------- */

/** Publish a complete Foreground command, preserving producer time and sequence. */
int foc_runtime_publish_command(struct foc_runtime *runtime,
                                const struct foc_pipeline_command *command,
                                uint32_t now_us);

/** Publish a VELOCITY or POSITION command with its unchanged producer identity. */
int foc_runtime_publish_motion_command(struct foc_runtime *runtime,
                                       const struct foc_runtime_command *command,
                                       uint32_t now_us);

/** Publish one bounded ARM, START, or SET_MODE request for the next due H0. */
int foc_runtime_publish_lifecycle_request(struct foc_runtime *runtime,
                                          const struct foc_runtime_lifecycle_request *request,
                                          uint32_t now_us);

/** Read the latest coherent lifecycle result or PENDING request identity. */
int foc_runtime_read_lifecycle_result(struct foc_runtime *runtime,
                                      struct foc_runtime_lifecycle_result *result);

/** Validate and publish one due H1 sample. Current time and simulated cost are in µs. */
int foc_runtime_h1_step(struct foc_runtime *runtime,
                        const struct foc_runtime_sensor *sample,
                        uint32_t now_us,
                        uint32_t cost_us);

/** Service at most one due H0 control cycle, with stop priority even off-period. */
int foc_runtime_h0_step(struct foc_runtime *runtime, const struct foc_runtime_fast_input *input);

/** Run due supervision before telemetry. Costs are Host-injected microseconds. */
int foc_runtime_foreground_poll(struct foc_runtime *runtime,
                                uint32_t now_us,
                                uint32_t supervision_cost_us,
                                uint32_t telemetry_cost_us,
                                struct foc_runtime_foreground_result *result);

/* Independent stop and observation -------------------------------------------- */

/** Atomically request stop with foc_safe_fault bits. Zero requests a non-latching INHIBIT. */
void foc_runtime_request_inhibit(struct foc_runtime *runtime, uint32_t fault_bits);

/** Foreground only. Stop first, revoke old references, and cancel remote recovery. */
void foc_runtime_revoke_command(struct foc_runtime *runtime);

/**
 * Foreground only. Request recovery of an isolated command-timeout latch.
 * H0 rechecks fresh sensing, Foreground progress and software inhibition.
 * Other fault types cannot be cleared here. Query clear_result for completion.
 */
int foc_runtime_request_timeout_clear(struct foc_runtime *runtime, uint32_t now_us);

/** Copy coherent telemetry once, leaving the caller's output unchanged on ERR_BUSY. */
int foc_runtime_read_snapshot(struct foc_runtime *runtime, struct foc_runtime_snapshot *snapshot);

/* ---------------------------------------- end of file ----------------------------------------- */
