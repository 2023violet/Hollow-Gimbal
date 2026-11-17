/**
 * @file    foc_virtual_motor.h
 * @brief   Deterministic Host-only PMSM plant and virtual sensor boundary.
 * @version v0.1.0
 * @date    2026.11.15
 *
 * Design overview
 * ---------------
 * Integrate an average-value PMSM in SI units and translate its state through
 * the existing ADC and Encoder contracts.
 *
 * Usage example
 * -------------
 * Initialize a caller-owned instance, apply accepted alpha/beta or PWM
 * voltage, then request ADC and encoder observations at a source timestamp.
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * The caller owns storage and serializes all operations. The module has no
 * dynamic allocation, global mutable state, MCU dependency, or retained input
 * pointers.
 *
 * Failure / recovery
 * ------------------
 * Invalid parameters, time steps, requests, or sensor configurations return
 * an error and do not partially commit the affected Plant state.
 *
 * Implementation role
 * -------------------
 * Provide one reusable average-value PMSM model for Host closed-loop fixtures.
 * The model owns no controller state and has no MCU, RTOS, transport, or GPIO
 * dependency.
 */
#pragma once

#include "foc_device.h"
#include "foc_encoder.h"

#include <stdbool.h>
#include <stdint.h>

/* Public Types ---------------------------------------------------------------- */

/** Finite SI-unit parameters for one virtual PMSM instance. */
struct foc_virtual_motor_params {
    double rs_ohm;
    double ld_h;
    double lq_h;
    double psi_f_wb;
    unsigned int pole_pairs;
    double electrical_offset_rad;
    double inertia_kg_m2;
    double damping_nm_s;
};

/** Caller-selected initial or observed PMSM state in SI units. */
struct foc_virtual_motor_state {
    double id_a;
    double iq_a;
    double mechanical_position_rad;
    double mechanical_velocity_rad_s;
};

/** Caller-owned plant with copied immutable parameters and mutable state. */
struct foc_virtual_motor {
    struct foc_virtual_motor_params params;
    struct foc_virtual_motor_state state;
    double load_torque_nm;
    bool initialized;
};

/* Public Functions ------------------------------------------------------------- */

/** Initialize one independent plant atomically after validating parameters/state. */
int foc_virtual_motor_init(struct foc_virtual_motor *motor,
                           const struct foc_virtual_motor_params *params,
                           const struct foc_virtual_motor_state *initial_state);

/** Set a finite external load torque in N·m. */
int foc_virtual_motor_set_load(struct foc_virtual_motor *motor, double load_torque_nm);

/** Advance the plant with constant alpha/beta voltage using one RK4 step. */
int foc_virtual_motor_step(struct foc_virtual_motor *motor,
                           double v_alpha_v,
                           double v_beta_v,
                           double dt_s);

/** Apply an accepted normalized PWM request as average alpha/beta voltage. */
int foc_virtual_motor_apply_duty(struct foc_virtual_motor *motor,
                                 const struct foc_device_pwm_request *request,
                                 float vbus_v,
                                 double dt_s);

/* Virtual sensors ------------------------------------------------------------- */

/** Quantize the current plant state into the existing four-rank ADC contract. */
int foc_virtual_motor_make_adc(const struct foc_virtual_motor *motor,
                               const struct foc_device_adc_config *config,
                               float vbus_v,
                               uint32_t sequence,
                               uint32_t timestamp_us,
                               struct foc_device_adc_raw *raw);

/** Quantize continuous mechanical position into one encoder raw code. */
int foc_virtual_motor_make_encoder_code(const struct foc_virtual_motor *motor,
                                        const struct foc_encoder_config *config,
                                        uint16_t *raw_code);

/* ---------------------------------------- end of file ----------------------------------------- */
