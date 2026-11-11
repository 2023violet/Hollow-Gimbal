/**
 * @file    foc_device_sim.h
 * @brief   Closed-gate host simulator for the G3-NP Core/Device chain.
 * @version v0.1.0
 * @date    2026.11.09
 *
 * Design overview
 * ---------------
 * The simulator models `raw sample -> Device decode -> foc_step -> Device
 * mapping -> mock registers`. It is a host-only validation harness. The mock
 * bridge gate is permanently closed, so compare writes are observations of a
 * request and never represent physical bridge enable.
 *
 * Usage example
 * -------------
 * 1. Fill a foc_device_sim_config with Core, ADC, PWM, and age limits.
 * 2. Call foc_device_sim_init(), then foc_device_sim_set_running().
 * 3. Submit one foc_device_sim_input to foc_device_sim_step().
 * 4. Inspect status, fault bits, request action, and mock registers.
 * 5. Clear a latched fault explicitly, then re-enter running state.
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * The caller owns simulator storage and input/output snapshots. The simulator
 * owns its Core context, freshness state, fault state, and mock registers.
 * Calls are non-blocking and intended for one deterministic host execution
 * context. Production ISR or register ownership is deliberately absent.
 *
 * Failure / recovery
 * ------------------
 * Disabled state and latched faults inhibit before sample, timing, Core, or
 * output decisions. Invalid or stale samples inhibit without consuming stale
 * data. Only foc_device_sim_clear_fault() followed by an explicit running
 * transition can recover from a latched fault.
 */
#pragma once

#include "foc_device.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Public Types ---------------------------------------------------------------- */

/** Simulator execution state. */
enum foc_device_sim_state {
    /** No control request may update the mock output. */
    FOC_DEVICE_SIM_DISABLED = 0,
    /** Samples and Core output requests may be evaluated. */
    FOC_DEVICE_SIM_RUNNING = 1,
    /** A latched fault has priority until explicitly cleared. */
    FOC_DEVICE_SIM_FAULT_LATCHED = 2,
};

/** Fault bits reported by one simulator step or retained as a latch. */
enum foc_device_sim_fault {
    /** No fault was observed. */
    FOC_DEVICE_SIM_FAULT_NONE = 0U,
    /** The simulator was not in running state. */
    FOC_DEVICE_SIM_FAULT_NOT_RUNNING = 1U << 0,
    /** The raw sample was incomplete or physically invalid. */
    FOC_DEVICE_SIM_FAULT_SAMPLE_INVALID = 1U << 1,
    /** The raw sample was repeated or too old. */
    FOC_DEVICE_SIM_FAULT_SAMPLE_STALE = 1U << 2,
    /** The supplied control period was invalid. */
    FOC_DEVICE_SIM_FAULT_TIME_INVALID = 1U << 3,
    /** The Core rejected the Device-provided snapshot or output. */
    FOC_DEVICE_SIM_FAULT_CORE = 1U << 4,
};

/** Complete configuration for one deterministic simulator instance. */
struct foc_device_sim_config {
    /** G2 Core numeric and duty limits. */
    struct foc_config core_config;
    /** Board1 raw ADC conversion and phase-order facts. */
    struct foc_device_adc_config adc_config;
    /** Board1 period and phase-to-channel facts. */
    struct foc_device_pwm_config pwm_config;
    /** Maximum accepted age between sample tick and current tick. */
    uint32_t max_sample_age_ticks;
};

/** Mock register snapshot. The bridge gate is always closed in G3-NP. */
struct foc_device_sim_registers {
    /** Non-zero means the simulated safety gate remains closed. */
    unsigned int bridge_gate_closed;
    /** Always zero in the no-power simulator. */
    unsigned int bridge_enabled;
    /** Mock TMR1 channel 1 compare value. */
    uint32_t channel1_compare;
    /** Mock TMR1 channel 2 compare value. */
    uint32_t channel2_compare;
    /** Mock TMR1 channel 3 compare value. */
    uint32_t channel3_compare;
};

/** Caller-owned simulator state and internally owned validation state. */
struct foc_device_sim {
    /** Embedded G2 Core context. */
    struct foc_context core;
    /** Configuration copied during initialization. */
    struct foc_device_sim_config config;
    /** Current simulator state. */
    enum foc_device_sim_state state;
    /** Fault bits retained after explicit latching. */
    uint32_t latched_faults;
    /** Last accepted sample sequence. */
    uint32_t last_sample_seq;
    /** Last accepted sample timestamp. */
    uint32_t last_sample_tick;
    /** Closed-gate mock register state. */
    struct foc_device_sim_registers registers;
};

/** One host-side input snapshot for a simulated control step. */
struct foc_device_sim_input {
    /** Validated command passed to foc_step(). */
    struct foc_validated_motion_command command;
    /** Raw ADC sample presented to the Device decoder. */
    struct foc_device_adc_raw sample;
    /** Current tick in the same monotonic domain as sample_tick. */
    uint32_t now_tick;
    /** Actual control period supplied to the Core. */
    struct foc_timing timing;
};

/** Complete result of a simulator step, including intermediate evidence. */
struct foc_device_sim_result {
    /** Device-level status for this step. */
    int status;
    /** Raw Core status before Device translation. */
    int core_status;
    /** Transient or latched fault bits observed by this step. */
    uint32_t fault_bits;
    /** Complete Core output snapshot. */
    struct foc_output core_output;
    /** Explicit Device action and mapped compare request. */
    struct foc_device_pwm_request pwm_request;
    /** Mock register state after applying the request. */
    struct foc_device_sim_registers registers;
};

/* Public Functions ------------------------------------------------------------- */

/* Lifecycle ------------------------------------------------------------------- */

/** Initialize Core, Device, state, freshness, and closed-gate registers. */
int foc_device_sim_init(struct foc_device_sim *sim, const struct foc_device_sim_config *config);

/** Enter running state unless a fault remains latched. */
int foc_device_sim_set_running(struct foc_device_sim *sim);

/** Enter disabled state and clear mock compare values. */
int foc_device_sim_set_disabled(struct foc_device_sim *sim);

/** Latch a non-zero fault bit set and close the mock gate. */
int foc_device_sim_latch_fault(struct foc_device_sim *sim, uint32_t fault_bits);

/** Clear a latched fault, reset Core angle, and return to disabled state. */
int foc_device_sim_clear_fault(struct foc_device_sim *sim);

/* Runtime step ---------------------------------------------------------------- */

/**
 * Apply a complete UPDATE or INHIBIT to closed-gate mock registers.
 *
 * @param registers Caller-owned destination, unchanged on rejection.
 * @param request Fully mapped Device request, read only.
 * @param period_counts Inclusive compare limit for UPDATE.
 * @return FOC_DEVICE_STATUS_OK or a pointer or invalid-output error.
 */
int foc_device_sim_submit(struct foc_device_sim_registers *registers,
                          const struct foc_device_pwm_request *request,
                          uint32_t period_counts);

/**
 * Execute one offline Sample-to-Core-to-Output step.
 *
 * @param sim Initialized simulator owned by the caller.
 * @param input Caller-owned raw sample, command, tick, and timing snapshot.
 * @param result Destination for deterministic status, faults, requests, and registers.
 * @return Device status. A failed step always emits an inhibit request when
 *         result and sim are valid.
 */
int foc_device_sim_step(struct foc_device_sim *sim,
                        const struct foc_device_sim_input *input,
                        struct foc_device_sim_result *result);

/* ---------------------------------------- end of file ----------------------------------------- */
