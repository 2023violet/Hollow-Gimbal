/**
 * @file    foc_safe.h
 * @brief   No-power G3-SAFE state, freshness, heartbeat, WDT, and Flash contract.
 * @version v0.1.0
 * @date    2026.11.09
 *
 * Design overview
 * ---------------
 * This header defines the independent safety decision layer between a future
 * Runtime/Device owner and a PWM request. It never touches registers. It
 * accepts an explicit monotonic time and returns an explicit output action.
 *
 * Usage example
 * -------------
 * 1. Call foc_safe_init(), then foc_safe_boot_complete().
 * 2. Arm and start only through foc_safe_arm() and foc_safe_start().
 * 3. Publish command/sample observations and both heartbeats with timestamps.
 * 4. Call foc_safe_supervisor_tick() and foc_safe_gate_output() before any
 *    future Device output commit.
 * 5. Treat a false watchdog permission or an inhibited decision as a stop.
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * The caller owns the context and configuration storage. The context is
 * single-owner state for one safety domain and must not be concurrently
 * modified. Functions are non-blocking and use no dynamic allocation. A
 * production fast domain must own the final hardware inhibit point.
 *
 * Failure / recovery
 * ------------------
 * Unsafe state, stale command/sample, stale heartbeat, invalid request, or a
 * latched fault yields an INHIBIT request. Fault clear returns to DISABLED and
 * never starts the output automatically. WDT and Flash functions expose only
 * permission decisions in this host-only slice.
 */
#pragma once

#include "foc_device.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Public Types ---------------------------------------------------------------- */

/** Safety state used by the independent G3-SAFE state machine. */
enum foc_safe_state {
    /** Startup state that always inhibits output. */
    FOC_SAFE_STATE_BOOT = 0,
    /** Disabled state after boot or explicit disable. */
    FOC_SAFE_STATE_DISABLED = 1,
    /** Armed state that still cannot submit an enabled output. */
    FOC_SAFE_STATE_READY = 2,
    /** Only state that can pass a healthy update request. */
    FOC_SAFE_STATE_RUNNING = 3,
    /** Latched fault state requiring explicit clear. */
    FOC_SAFE_STATE_FAULT_LATCHED = 4,
};

/** Status returned by a safety contract operation. */
enum foc_safe_status {
    /** Operation completed successfully. */
    FOC_SAFE_STATUS_OK = 0,
    /** A required pointer was NULL. */
    FOC_SAFE_STATUS_ERR_NULL = -1,
    /** Safety configuration was invalid. */
    FOC_SAFE_STATUS_ERR_CONFIG = -2,
    /** Requested state transition was not permitted. */
    FOC_SAFE_STATUS_ERR_STATE = -3,
    /** A monotonic timestamp, sequence, or observation was invalid. */
    FOC_SAFE_STATUS_ERR_INVALID_INPUT = -4,
    /** A freshness or heartbeat window has expired. */
    FOC_SAFE_STATUS_ERR_TIMEOUT = -5,
    /** A fault is latched and must be cleared explicitly. */
    FOC_SAFE_STATUS_ERR_FAULT_LATCHED = -6,
    /** A WDT or Flash permission check was denied. */
    FOC_SAFE_STATUS_ERR_NOT_PERMITTED = -7,
};

/** Latched and transient safety causes. */
enum foc_safe_fault {
    /** No safety fault. */
    FOC_SAFE_FAULT_NONE = 0U,
    /** Command timestamp exceeded command_timeout_us. */
    FOC_SAFE_FAULT_COMMAND_TIMEOUT = 1U << 0,
    /** Sample timestamp exceeded sample_timeout_us. */
    FOC_SAFE_FAULT_SAMPLE_TIMEOUT = 1U << 1,
    /** Invalid samples reached the configured latch threshold. */
    FOC_SAFE_FAULT_SAMPLE_INVALID = 1U << 2,
    /** Fast heartbeat exceeded fast_heartbeat_timeout_us. */
    FOC_SAFE_FAULT_FAST_HEARTBEAT = 1U << 3,
    /** Foreground heartbeat exceeded foreground_heartbeat_timeout_us. */
    FOC_SAFE_FAULT_FOREGROUND_HEARTBEAT = 1U << 4,
    /** An output request or state was invalid. */
    FOC_SAFE_FAULT_INVALID_STATE = 1U << 5,
    /** A WDT permission or service contract failed. */
    FOC_SAFE_FAULT_WDT = 1U << 6,
    /** A Flash guard contract failed. */
    FOC_SAFE_FAULT_FLASH = 1U << 7,
    /** This fault explicitly requires an independent watchdog reset. */
    FOC_SAFE_FAULT_FATAL_RESET_REQUIRED = 1U << 8,
    /** A phase current exceeded the over-current trip (foc_protect). */
    FOC_SAFE_FAULT_OVER_CURRENT = 1U << 9,
    /** The bus voltage stayed above the over-voltage trip (foc_protect). */
    FOC_SAFE_FAULT_OVER_VOLTAGE = 1U << 10,
    /** The bus voltage stayed below the under-voltage trip (foc_protect). */
    FOC_SAFE_FAULT_UNDER_VOLTAGE = 1U << 11,
    /** The mechanical speed exceeded the over-speed trip (foc_protect). */
    FOC_SAFE_FAULT_OVER_SPEED = 1U << 12,
    /** The current stayed near its limit for the sustained window (foc_protect). */
    FOC_SAFE_FAULT_SUSTAINED_CURRENT = 1U << 13,
    /** Near-limit current at near-zero speed for the stall window (foc_protect). */
    FOC_SAFE_FAULT_STALL = 1U << 14,
    /** Saturated outer loop moving against its reference without converging (foc_protect). */
    FOC_SAFE_FAULT_TRACKING_DIVERGENCE = 1U << 15,
    /** The gate driver reports a fault on its nFAULT line (board input). */
    FOC_SAFE_FAULT_DRIVER = 1U << 16,
    /** Power-stage NTC above its trip temperature or broken (board foc_thermal). */
    FOC_SAFE_FAULT_OVER_TEMPERATURE_FET = 1U << 17,
    /** Motor-winding NTC above its trip temperature or broken (board foc_thermal). */
    FOC_SAFE_FAULT_OVER_TEMPERATURE_MOTOR = 1U << 18,
    /** I²t state reached FOC_THERMAL_I2T_TRIP despite the fold-back (Runtime). */
    FOC_SAFE_FAULT_OVERLOAD = 1U << 19,
};

/** Motor-protection causes. FAULT_CLEAR may clear these, unlike the other latched causes. */
#define FOC_SAFE_FAULT_PROTECTION_MASK                                                          \
    (FOC_SAFE_FAULT_OVER_CURRENT | FOC_SAFE_FAULT_OVER_VOLTAGE | FOC_SAFE_FAULT_UNDER_VOLTAGE | \
     FOC_SAFE_FAULT_OVER_SPEED | FOC_SAFE_FAULT_SUSTAINED_CURRENT | FOC_SAFE_FAULT_STALL |      \
     FOC_SAFE_FAULT_TRACKING_DIVERGENCE | FOC_SAFE_FAULT_OVER_TEMPERATURE_FET |                 \
     FOC_SAFE_FAULT_OVER_TEMPERATURE_MOTOR | FOC_SAFE_FAULT_OVERLOAD)

/** Timeout and invalid-sample policy owned by the safety caller. */
struct foc_safe_config {
    /** Command freshness limit in monotonic microseconds. */
    uint32_t command_timeout_us;
    /** Sample freshness limit in monotonic microseconds. */
    uint32_t sample_timeout_us;
    /** Fast heartbeat freshness limit in monotonic microseconds. */
    uint32_t fast_heartbeat_timeout_us;
    /** Foreground heartbeat freshness limit in monotonic microseconds. */
    uint32_t foreground_heartbeat_timeout_us;
    /** Consecutive invalid samples required to latch a fault. */
    uint32_t invalid_sample_latch_after;
};

/** Caller-owned safety context and observed health state. */
struct foc_safe_context {
    /** Validated policy copied during foc_safe_init(). */
    struct foc_safe_config config;
    /** Current safety state. */
    enum foc_safe_state state;
    /** Latched fault causes that cannot be overwritten by commands. */
    uint32_t latched_faults;
    /** Last accepted command sequence. */
    uint32_t command_seq;
    /** Last accepted sample sequence. */
    uint32_t sample_seq;
    /** Last accepted command time. */
    uint32_t last_command_us;
    /** Last accepted sample time. */
    uint32_t last_sample_us;
    /** Last fast heartbeat time. */
    uint32_t last_fast_heartbeat_us;
    /** Last foreground heartbeat time. */
    uint32_t last_foreground_heartbeat_us;
    /** Number of consecutive invalid samples. */
    uint32_t invalid_sample_streak;
    /** Whether a command observation exists. */
    bool command_seen;
    /** Whether a valid sample observation exists. */
    bool sample_seen;
    /** Whether the most recently observed sample is valid for this cycle. */
    bool sample_current_valid;
    /** Whether a fast heartbeat exists. */
    bool fast_heartbeat_seen;
    /** Whether a foreground heartbeat exists. */
    bool foreground_heartbeat_seen;
};

/** Safety-gated output decision with the original or inhibit request. */
struct foc_safe_output_decision {
    /** Non-zero when the request is permitted to proceed to Device. */
    unsigned int permitted;
    /** Fault or policy reasons for an inhibited decision. */
    uint32_t inhibit_reasons;
    /** Request copied or replaced with an explicit inhibit action. */
    struct foc_device_pwm_request request;
};

/* Public Functions ------------------------------------------------------------- */

/* Lifecycle and fault state --------------------------------------------------- */

/** Validate policy and initialize a caller-owned safety context in BOOT. */
int foc_safe_init(struct foc_safe_context *context, const struct foc_safe_config *config);

/** Complete boot checks and transition BOOT to DISABLED. */
int foc_safe_boot_complete(struct foc_safe_context *context);

/** Arm a disabled context into READY without enabling output. */
int foc_safe_arm(struct foc_safe_context *context);

/** Transition READY to RUNNING without bypassing output freshness gates. */
int foc_safe_start(struct foc_safe_context *context);

/** Disable output and transition any non-BOOT, non-fault state to DISABLED. */
int foc_safe_disable(struct foc_safe_context *context);

/** Latch a non-zero fault and immediately force FAULT_LATCHED. */
int foc_safe_latch_fault(struct foc_safe_context *context, uint32_t fault_bits);

/** Clear a latched fault, reset observations, and return to DISABLED. */
int foc_safe_clear_fault(struct foc_safe_context *context);

/* Observations and supervisor -------------------------------------------------- */

/** Return whether a sequence advances within half the modulus, or is the first observed. */
int foc_safe_sequence_newer(uint32_t sequence, uint32_t previous_sequence, bool seen);

/** Return whether the timestamp is within the inclusive age limit and half range. */
int foc_safe_timestamp_fresh(uint32_t now_us, uint32_t observed_us, uint32_t timeout_us);

/** Accept a newer command sequence at a monotonic timestamp. */
int foc_safe_observe_command(struct foc_safe_context *context,
                             uint32_t now_us,
                             uint32_t command_seq);

/** Accept a newer valid sample or count an invalid sample toward the latch threshold. */
int foc_safe_observe_sample(struct foc_safe_context *context,
                            uint32_t now_us,
                            uint32_t sample_seq,
                            bool sample_valid);

/** Publish one fast-domain heartbeat at a monotonic timestamp. */
int foc_safe_heartbeat_fast(struct foc_safe_context *context, uint32_t now_us);

/** Publish one foreground-domain heartbeat at a monotonic timestamp. */
int foc_safe_heartbeat_foreground(struct foc_safe_context *context, uint32_t now_us);

/** Evaluate heartbeat and running-state freshness and latch failures. */
int foc_safe_supervisor_tick(struct foc_safe_context *context, uint32_t now_us);

/** Return whether the independent watchdog may be fed at this timestamp. */
int foc_safe_watchdog_permitted(struct foc_safe_context *context, uint32_t now_us);

/* Output and persistence guards ------------------------------------------------ */

/**
 * Gate one Device PWM request using state, freshness, heartbeat, and latch rules.
 *
 * @param context Initialized safety context.
 * @param now_us Current monotonic microsecond timestamp.
 * @param pwm_config Device mapping configuration used to build inhibit requests.
 * @param request Device request produced by Core/Device layers.
 * @param decision Destination for permission, reasons, and safe request.
 * @return FOC_SAFE_STATUS_OK when a decision was produced.
 */
int foc_safe_gate_output(struct foc_safe_context *context,
                         uint32_t now_us,
                         const struct foc_device_pwm_config *pwm_config,
                         const struct foc_device_pwm_request *request,
                         struct foc_safe_output_decision *decision);

/** Permit a Flash transaction only while disabled and explicitly inhibited. */
int foc_safe_flash_permitted(const struct foc_safe_context *context,
                             enum foc_device_output_action output_action);

/* ---------------------------------------- end of file ----------------------------------------- */
