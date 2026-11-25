/**
 * @file    foc_core.h
 * @brief   Host-portable voltage-open-loop FOC control-core contract.
 * @version v0.1.0
 * @date    2026.11.09
 *
 * Design overview
 * ---------------
 * This header defines the pure-C data and API boundary between a caller and
 * the FalconFOC mathematical core. The core owns only its configuration and
 * electrical-angle state. It does not own ADC, PWM, sensor, transport, RTOS,
 * or board state.
 *
 * Usage example
 * -------------
 * 1. Validate a foc_config value and call foc_init() once.
 * 2. Build a validated motion command, measurement, and timing snapshot.
 * 3. Call foc_step() at the caller-owned control boundary.
 * 4. Consume the complete output snapshot only when the status is OK.
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * The caller owns all structures and their storage. A foc_context must be
 * initialized before foc_step() and must not be concurrently modified by
 * another execution domain. The API is non-blocking and has no dynamic
 * allocation. The caller owns the conversion from foc_output to hardware
 * actions.
 *
 * Failure / recovery
 * ------------------
 * Every function returns a status. foc_step() initializes a complete centered
 * disabled output before validation and commits the electrical angle only
 * after inverse Park and SVPWM succeed. Invalid input leaves the context
 * state unchanged.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Public Types ---------------------------------------------------------------- */

/** Result status returned by every fallible Core operation. */
enum foc_status {
    /** Operation completed successfully. */
    FOC_STATUS_OK = 0,
    /** A required pointer was NULL. */
    FOC_STATUS_ERR_NULL = -1,
    /** An input or computed result was not finite. */
    FOC_STATUS_ERR_NONFINITE = -2,
    /** The supplied control period was outside the configured range. */
    FOC_STATUS_ERR_INVALID_DT = -3,
    /** The supplied bus voltage was invalid or below the configured minimum. */
    FOC_STATUS_ERR_INVALID_VBUS = -4,
    /** The Core configuration or control mode was invalid. */
    FOC_STATUS_ERR_INVALID_CONFIG = -5,
    /** The requested operation requires an initialized controller. */
    FOC_STATUS_ERR_NOT_INITIALIZED = -6,
    /** The available duty precision could not honor the requested voltage limit. */
    FOC_STATUS_ERR_VOLTAGE_LIMIT = -7,
    /** Rotor angle extrapolation cannot meet its configured error bound. */
    FOC_STATUS_ERR_PREDICTION = -8,
};

/** Control modes supported by the G2 Core. */
enum foc_control_mode {
    /** Request a complete disabled output without advancing the angle. */
    FOC_CONTROL_DISABLED = 0,
    /** Apply a d/q voltage command with caller-supplied electrical speed. */
    FOC_CONTROL_VOLTAGE_OPEN_LOOP = 1,
};

/** Three-phase quantity in physical phase order. */
struct foc_vector_abc {
    /** Phase A quantity. */
    float a;
    /** Phase B quantity. */
    float b;
    /** Phase C quantity. */
    float c;
};

/** Stationary-frame alpha/beta vector in the same physical unit as its input. */
struct foc_vector_alpha_beta {
    /** Stationary alpha component. */
    float alpha;
    /** Stationary beta component. */
    float beta;
};

/** Rotating-frame d/q vector in the same physical unit as its input. */
struct foc_vector_dq {
    /** Direct-axis component. */
    float d;
    /** Quadrature-axis component. */
    float q;
};

/** Normalized SVPWM result and limiting information. */
struct foc_svpwm_output {
    /** Phase A duty in the normalized range configured by foc_config. */
    float duty_a;
    /** Phase B duty in the normalized range configured by foc_config. */
    float duty_b;
    /** Phase C duty in the normalized range configured by foc_config. */
    float duty_c;
    /** Direction sector in the range 1 through 6, or 0 for a safe output. */
    unsigned int sector;
    /** Non-zero when vector or duty limiting changed the result. */
    unsigned int saturated;
};

/** Numeric and output limits owned by the Core caller. */
struct foc_config {
    /** Minimum accepted bus voltage in volts. */
    float vbus_min_v;
    /** Maximum requested voltage-vector magnitude in volts. */
    float voltage_limit_v;
    /** Minimum normalized duty accepted by the output contract. */
    float duty_min;
    /** Maximum normalized duty accepted by the output contract. */
    float duty_max;
    /** Maximum accepted control period in seconds. */
    float max_dt_s;
};

/** Caller-validated command consumed by foc_step(). */
struct foc_validated_motion_command {
    /** Control mode selected by the caller. */
    enum foc_control_mode mode;
    /** Non-zero requests an enabled output when the mode permits it. */
    unsigned int enable;
    /** Direct-axis voltage command in volts. */
    float vd_v;
    /** Quadrature-axis voltage command in volts. */
    float vq_v;
    /** Electrical angular speed in radians per second. */
    float electrical_speed_rad_s;
};

/** Explicit alias for code that uses a validated-command type suffix. */
typedef struct foc_validated_motion_command foc_validated_motion_command_t;

/** Measurement snapshot required by the G2 voltage-open-loop path. */
struct foc_measurement {
    /** Measured DC bus voltage in volts. */
    float vbus_v;
};

/** Caller-supplied timing snapshot. */
struct foc_timing {
    /** Actual control period in seconds. */
    float dt_s;
};

/** Complete result returned by a successful or failed Core step. */
struct foc_output {
    /** Non-zero requests an output update after Device safety checks. */
    unsigned int output_enable;
    /** Electrical angle used for this output in radians. */
    float theta_elec_rad;
    /** Stationary-frame voltage passed to SVPWM in volts. */
    struct foc_vector_alpha_beta voltage_alpha_beta_v;
    /** Normalized duties and saturation state. */
    struct foc_svpwm_output svpwm;
};

/** Caller-owned mutable Core state. */
struct foc_context {
    /** Configuration copied during foc_init(). */
    struct foc_config config;
    /** Current wrapped electrical angle in radians. */
    float theta_elec_rad;
};

/* Public Functions ------------------------------------------------------------- */

/* Configuration and lifecycle ------------------------------------------------ */

/**
 * Validate a Core configuration before initialization.
 *
 * @param config Caller-owned configuration to validate.
 * @return FOC_STATUS_OK or a specific validation error.
 */
int foc_config_validate(const struct foc_config *config);

/**
 * Initialize a caller-owned Core context and set its electrical angle to zero.
 *
 * @param context Context storage that remains owned by the caller.
 * @param config Configuration copied into the context after validation.
 * @return FOC_STATUS_OK or a validation error.
 */
int foc_init(struct foc_context *context, const struct foc_config *config);

/**
 * Reset the electrical angle of an initialized context to zero.
 *
 * @param context Initialized context owned by the caller.
 * @return FOC_STATUS_OK or FOC_STATUS_ERR_NULL.
 */
int foc_reset(struct foc_context *context);

/* Pure mathematical transforms ----------------------------------------------- */

/**
 * Normalize a finite angle in radians to [0, 2π), clearing output on error.
 *
 * Acceptance and precision are separate: every finite float remains accepted,
 * using the existing Float32 approximation of 2π as the reduction period.
 * For IEEE-754 binary32, round-to-nearest, a correct fmodf remainder, and no
 * fast-math transformations, abs(angle_rad) <= 6.2831854820251465f has circular
 * reduction error at most 5e-7 rad relative to the exact stored float value.
 * This bound excludes sensor error and conversion of an original angle to float.
 * Larger finite inputs are reduced, but have no phase-accuracy guarantee here.
 * Keep periodic phase separate from continuous multi-turn mechanical position.
 */
int foc_angle_normalize(float angle_rad, float *wrapped_rad);

/**
 * Apply the amplitude-invariant Clarke transform without mutating input.
 *
 * This reduced form uses alpha = a and beta = (b - c) / sqrt(3). Supply coherent,
 * balanced three-phase quantities (a + b + c = 0), or quantities reconstructed
 * under that convention. It does not check balance or remove zero sequence.
 *
 * @param input Phase-domain vector in physical phase order.
 * @param output Destination for the stationary alpha/beta vector.
 * @return FOC_STATUS_OK or a pointer/non-finite error.
 */
int foc_clarke(const struct foc_vector_abc *input, struct foc_vector_alpha_beta *output);

/**
 * Apply Park, d = alpha cos(theta) + beta sin(theta) and q = -alpha sin(theta) + beta cos(theta),
 * without mutating input.
 *
 * @param input Stationary alpha/beta vector.
 * @param theta_elec_rad Electrical angle in radians. Any finite angle is accepted;
 *        the reduction accuracy domain is specified by foc_angle_normalize().
 *        Trigonometric and vector arithmetic introduce their own rounding error.
 * @param output Destination for the rotating d/q vector.
 * @return FOC_STATUS_OK or a pointer/non-finite error.
 */
int foc_park(const struct foc_vector_alpha_beta *input,
             float theta_elec_rad,
             struct foc_vector_dq *output);

/**
 * Apply inverse Park, alpha = d cos(theta) - q sin(theta) and beta = d sin(theta) + q cos(theta).
 *
 * @param input Rotating d/q vector.
 * @param theta_elec_rad Electrical angle in radians. Any finite angle is accepted;
 *        the reduction accuracy domain is specified by foc_angle_normalize().
 *        Trigonometric and vector arithmetic introduce their own rounding error.
 * @param output Destination for the stationary alpha/beta vector.
 * @return FOC_STATUS_OK or a pointer/non-finite error.
 */
int foc_inverse_park(const struct foc_vector_dq *input,
                     float theta_elec_rad,
                     struct foc_vector_alpha_beta *output);

/* Output calculation ---------------------------------------------------------- */

/**
 * Convert a stationary voltage vector to bounded normalized SVPWM duties.
 *
 * @param input Stationary voltage vector in volts. The input is read-only.
 * @param vbus_v Measured DC bus voltage in volts.
 * @param config Validated voltage and duty limits.
 * @param output Destination for duties, sector, and saturation state.
 * @return FOC_STATUS_OK or a pointer, configuration, VBUS, or numeric error.
 */
int foc_svpwm(const struct foc_vector_alpha_beta *input,
              float vbus_v,
              const struct foc_config *config,
              struct foc_svpwm_output *output);

/* Runtime step ---------------------------------------------------------------- */

/**
 * Execute one transactional voltage-open-loop Core step.
 *
 * @param context Initialized Core state owned by the caller.
 * @param command Validated command snapshot consumed without mutation.
 * @param measurement Measurement snapshot containing VBUS in volts.
 * @param timing Actual period supplied by the caller in seconds.
 * @param output Complete output snapshot initialized on success or failure.
 * @return FOC_STATUS_OK or a deterministic validation/control error.
 */
int foc_step(struct foc_context *context,
             const struct foc_validated_motion_command *command,
             const struct foc_measurement *measurement,
             const struct foc_timing *timing,
             struct foc_output *output);

/* ---------------------------------------- end of file ----------------------------------------- */
