/**
 * @file    fixture_profile.c
 * @brief   Bind the Host demo setup independently of the inspected image.
 * @version v0.1.0
 * @date    2026.11.18
 *
 * Implementation role
 * -------------------
 * Supply the qualified identity and impedance values used by the SOFT-03
 * storage demonstration.
 *
 * Private Helpers
 * ---------------
 * The public initializer delegates the baseline to the Simulation profile.
 * It assigns the policy's pointers in place against caller-owned inputs.
 */

/* Includes --------------------------------------------------------------------- */
#include "fixture_profile.h"

/* Public Functions ------------------------------------------------------------- */
void inspector_fixture_profile_init(struct inspector_fixture_profile *fixture)
{
    const uint32_t mechanical_frame = 71U;
    struct foc_virtual_current_profile *profile = &fixture->virtual_current;
    *profile = foc_virtual_current_default_profile();
    profile->runtime.motion.impedance = (struct foc_runtime_impedance_config){
        .enabled = true,
        .mechanical_frame_id = mechanical_frame,
        .torque_constant_nm_per_a =
            (float)(1.5 * profile->motor.pole_pairs * profile->motor.psi_f_wb),
        .torque_limit_nm = 0.315f,
        .max_stiffness_nm_per_rad = 1.0f,
        .max_damping_nm_s_per_rad = 0.10f,
        .max_feedforward_nm = 0.30f,
    };
    fixture->pipeline = (struct foc_pipeline_config){
        .current = profile->current,
        .adc = profile->adc,
        .pwm = profile->pwm,
        .safety = profile->safety,
    };
    fixture->policy = (struct foc_config_store_policy){
        .pipeline = &fixture->pipeline,
        .base = &profile->runtime,
        .identity = {
            .encoder_cpr = profile->encoder.counts_per_revolution,
            .encoder_direction = profile->encoder.direction,
            .mechanical_frame_id = mechanical_frame,
            .encoder_identity = 101U,
            .motor_setup_id = 201U,
            .pole_pairs = profile->runtime.motion.rotor.pole_pairs,
        },
    };
}

/* ---------------------------------------- end of file ----------------------------------------- */
