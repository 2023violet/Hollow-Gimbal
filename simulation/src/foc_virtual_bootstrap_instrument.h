/**
 * @file    foc_virtual_bootstrap_instrument.h
 * @brief   Private Host shaft and open-circuit terminal instruments.
 * @version v0.1.0
 * @date    2026.11.20
 *
 * Design overview
 * ---------------
 * Generate simulated instrument observations without exposing Plant parameters
 * or its electrical-angle state to acquisition and calibration callers.
 *
 * Usage example
 * -------------
 * Advance the open winding with a bounded external drive, then read the shaft
 * instruments and terminal voltage at the current aperture.
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * The Host caller owns and serializes the Plant and result storage. The
 * instruments retain no pointers and do not authorize electrical output.
 *
 * Failure / recovery
 * ------------------
 * A failed open-circuit step preserves the previous Plant state. A failed
 * sensor read leaves the result storage unchanged.
 */

#pragma once

#include <stdbool.h>

struct foc_virtual_motor;

/* Public Types ----------------------------------------------------------------- */

struct foc_virtual_bootstrap_shaft {
    /** Independent shaft-angle readout in [0, 2*pi), in the fixture frame. */
    double position_rad;
    /** Signed shaft-speed readout in the same positive rotation convention. */
    double speed_rad_s;
    /** Separately measured equivalent motor torque and applied fixture load. */
    double equivalent_torque_nm;
    double load_nm;
    /** Independent zero-current confirmation for the Host zero-capture fixture. */
    bool zero_current;
};

/* Public Functions ------------------------------------------------------------- */

/** Advance the open winding with a torque-limited external shaft drive. */
int foc_virtual_motor_step_open_circuit(struct foc_virtual_motor *motor,
                                        double target_rad_s,
                                        double torque_limit_nm,
                                        double dt_s);

/** Read independent quantized mechanical instruments at the current aperture. */
int foc_virtual_motor_measure_shaft(const struct foc_virtual_motor *motor,
                                    struct foc_virtual_bootstrap_shaft *shaft);

/** Read floating terminal alpha/beta voltage with zero winding current and drive. */
int foc_virtual_motor_measure_open_circuit(const struct foc_virtual_motor *motor,
                                           double *emf_alpha_v,
                                           double *emf_beta_v);

/* ---------------------------------------- end of file ----------------------------------------- */
