/**
 * @file    foc_current.h
 * @brief   Portable d/q current controller with explicit feedback and timing.
 * @version v0.1.0
 * @date    2026.11.10
 *
 * Design overview
 * ---------------
 * Each instance owns two PI integrators. The caller provides currents and the
 * corresponding electrical angle in the phase and Park convention of foc_core.h.
 * The output describes ideal modulation. Device and Safety own bridge control.
 *
 * Usage example
 * -------------
 * Zero-initialize a context, call foc_current_init(), then call
 * foc_current_step() once per valid sample. Submit the duties only after the
 * output is usable and the caller's final safety checks pass.
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * The caller owns the storage and serializes calls on each instance. Treat
 * context fields as read-only outside the API. A value copy can be used to
 * stage a step before a downstream output commit. Arguments must not overlap.
 * Calls use bounded work and do not allocate or access hardware.
 *
 * Failure / recovery
 * ------------------
 * Failed initialization preserves the context. Failed steps preserve it and
 * clear any supplied output. Reset clears both integrators before a new run.
 */
#pragma once

#include "foc_core.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Public Types ---------------------------------------------------------------- */

/** Initialization settings copied into one current controller. */
struct foc_current_config {
    /** Shared Core limits. The duty interval must straddle 0.5. */
    struct foc_config pwm;
    /** Nonnegative d/q proportional gains in V/A. */
    struct foc_vector_dq kp_v_per_a;
    /** Nonnegative d/q integral gains in V/(A s). Zero disables that integrator. */
    struct foc_vector_dq ki_v_per_a_s;
    /** Positive maximum magnitude of the requested current vector in A. */
    float current_limit_a;
};

/** Read-only feedback and command for one current step. All fields must be finite. */
struct foc_current_input {
    /** Signed phase currents in A. Supply coherent, balanced or reconstructed samples. */
    struct foc_vector_abc phase_current_a;
    /** Electrical angle in rad at the current sample instant. Finite angles may wrap. */
    float theta_elec_rad;
    /** Requested d/q currents in A. Larger vectors are scaled to the configured magnitude. */
    struct foc_vector_dq target_a;
    /** Measured DC bus voltage in V, at least pwm.vbus_min_v. */
    float vbus_v;
    /** Actual elapsed control time in s, greater than zero and at most pwm.max_dt_s. */
    float dt_s;
};

/** Complete step result. All members are zero on error. */
struct foc_current_output {
    /** Nonzero permits evaluation for normal update by the caller's safety layer. */
    unsigned int output_enable;
    /** Nonzero when the requested current vector was scaled. */
    unsigned int target_limited;
    /** Measured rotating-frame current in A. */
    struct foc_vector_dq measured_dq_a;
    /** Ideal equivalent d/q voltage in V reconstructed from the final duties and bus. */
    struct foc_vector_dq voltage_dq_v;
    /** Final duties and sector. Saturated includes limiting seen by anti-windup. */
    struct foc_svpwm_output svpwm;
};

/** Instance storage. Zero-initialize before first use. */
struct foc_current_context {
    /** Private configuration copy established at initialization. */
    struct foc_current_config config;
    /** Signed integral contribution to each voltage command in V. */
    struct foc_vector_dq integral_v;
    /** Nonzero after successful initialization. */
    unsigned int initialized;
};

/** Validated PMSM parameters used only by the optional voltage feedforward path. */
struct foc_current_feedforward_model {
    /** d-axis inductance in H. */
    float ld_h;
    /** q-axis inductance in H. */
    float lq_h;
    /** Permanent-magnet flux linkage in Wb. */
    float psi_f_wb;
};

/** Caller-owned, immutable-after-init optional feedforward configuration. */
struct foc_current_feedforward_context {
    /** Validated motor model. It is intentionally unused while disabled. */
    struct foc_current_feedforward_model model;
    /** True when the caller explicitly enables d/q decoupling and back-EMF feedforward. */
    bool enabled;
    /** Set only after foc_current_feedforward_init() succeeds. */
    bool initialized;
};

/** Feedforward result in the same d/q voltage units as the PI output. */
struct foc_current_feedforward_output {
    /** d-axis cross-coupling compensation in V. */
    float voltage_d_v;
    /** q-axis cross-coupling plus back-EMF compensation in V. */
    float voltage_q_v;
    /** True only when a validated enabled context produced this result. */
    bool enabled;
};

/* Public Functions ------------------------------------------------------------- */

/* Lifecycle ------------------------------------------------------------------- */

/**
 * Validate settings and initialize one instance with zero integral history.
 *
 * @param context Caller-owned instance, unchanged on failure.
 * @param config Caller-owned settings, read only during this call.
 * @return FOC_STATUS_OK or a pointer, non-finite, or configuration error.
 */
int foc_current_init(struct foc_current_context *context, const struct foc_current_config *config);

/**
 * Clear both integrators while retaining settings.
 *
 * @param context Previously initialized instance.
 * @return FOC_STATUS_OK, FOC_STATUS_ERR_NULL, or FOC_STATUS_ERR_NOT_INITIALIZED.
 */
int foc_current_reset(struct foc_current_context *context);

/** Validate and initialize optional feedforward parameters; disabled mode accepts an empty model. */
int foc_current_feedforward_init(struct foc_current_feedforward_context *context,
                                 const struct foc_current_feedforward_model *model,
                                 bool enabled);

/** Clear the optional feedforward context while retaining its validated model and enabled state. */
int foc_current_feedforward_reset(struct foc_current_feedforward_context *context);

/** Calculate validated target-current d/q feedforward for one signed electrical speed. */
int foc_current_feedforward_compute(const struct foc_current_feedforward_context *context,
                                    float omega_e_rad_s,
                                    struct foc_vector_dq target_a,
                                    struct foc_current_feedforward_output *output);

/* Control --------------------------------------------------------------------- */

/**
 * Execute one transactional current-feedback step.
 *
 * @param context Initialized instance with exclusive access during this call.
 * @param input Coherent caller-owned snapshot, read only during this call.
 * @param output Destination cleared on failure. Zeroed duties are diagnostic only.
 * @return FOC_STATUS_OK or a deterministic validation or calculation error.
 */
int foc_current_step(struct foc_current_context *context,
                     const struct foc_current_input *input,
                     struct foc_current_output *output);

/**
 * Execute one current step with optional d/q feedforward.
 *
 * A disabled context follows the original foc_current_step() path. An enabled
 * context requires a finite electrical speed and uses target-current
 * feedforward before the existing SVPWM limit, voltage reconstruction, and
 * transactional integral admission. The Runtime/Board caller must provide a
 * speed sample valid at the same control instant; this function does not read
 * clocks or synthesize speed from an angle alone.
 */
int foc_current_step_with_feedforward(struct foc_current_context *context,
                                      const struct foc_current_input *input,
                                      const struct foc_current_feedforward_context *feedforward,
                                      float omega_e_rad_s,
                                      struct foc_current_output *output);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
