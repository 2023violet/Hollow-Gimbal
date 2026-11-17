/**
 * @file    test_foc_virtual_motor.c
 * @brief   Contract tests for the shared virtual PMSM plant.
 * @version v0.1.0
 * @date    2026.11.15
 *
 * Test role
 * ---------
 * Check validation, deterministic dynamics, multi-instance isolation, and
 * accepted-duty behavior.
 */

#include "foc_virtual_motor.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* Includes --------------------------------------------------------------------- */

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return 1;                                                       \
        }                                                                   \
    } while (0)

static const struct foc_virtual_motor_params params = {
    .rs_ohm = 0.6,
    .ld_h = 0.0012,
    .lq_h = 0.0015,
    .psi_f_wb = 0.025,
    .pole_pairs = 4U,
    .electrical_offset_rad = 0.37,
    .inertia_kg_m2 = 0.002,
    .damping_nm_s = 0.006,
};

/* Private Helpers -------------------------------------------------------------- */

static int test_validation_and_zero_input(void)
{
    struct foc_virtual_motor motor;
    const struct foc_virtual_motor_state initial = {0.3, -0.8, 0.47, 9.0};
    CHECK(foc_virtual_motor_init(&motor, &params, &initial) == 0);
    CHECK(foc_virtual_motor_set_load(&motor, 0.027) == 0);
    const struct foc_virtual_motor_state before = motor.state;
    CHECK(foc_virtual_motor_step(&motor, 0.0, 0.0, 0.0) != 0);
    CHECK(memcmp(&motor.state, &before, sizeof before) == 0);
    CHECK(foc_virtual_motor_step(&motor, 0.0, 0.0, 0.0001) == 0);
    CHECK(isfinite(motor.state.id_a) && isfinite(motor.state.iq_a));
    return 0;
}

static int test_power_consistency_and_instances(void)
{
    struct foc_virtual_motor first;
    struct foc_virtual_motor second;
    CHECK(foc_virtual_motor_init(&first, &params, NULL) == 0);
    CHECK(foc_virtual_motor_init(&second, &params, NULL) == 0);
    CHECK(foc_virtual_motor_step(&first, 1.2, -0.4, 0.0001) == 0);
    CHECK(first.state.id_a != second.state.id_a || first.state.iq_a != second.state.iq_a);
    CHECK(foc_virtual_motor_step(&second, 0.0, 0.0, 0.0001) == 0);
    CHECK(second.state.mechanical_position_rad == 0.0);
    return 0;
}

static int test_duty_and_invalid_parameters(void)
{
    struct foc_virtual_motor motor;
    struct foc_virtual_motor_params invalid = params;
    struct foc_device_pwm_request request = {
        .action = FOC_DEVICE_OUTPUT_UPDATE,
        .output_enable_request = 1U,
        .duty_a = 0.6f,
        .duty_b = 0.4f,
        .duty_c = 0.5f,
    };
    CHECK(foc_virtual_motor_init(&motor, &params, NULL) == 0);
    invalid.ld_h = 0.0;
    CHECK(foc_virtual_motor_init(&motor, &invalid, NULL) != 0);
    CHECK(foc_virtual_motor_apply_duty(&motor, &request, 12.0f, 0.0001) == 0);
    request.duty_a = NAN;
    CHECK(foc_virtual_motor_apply_duty(&motor, &request, 12.0f, 0.0001) != 0);
    return 0;
}

static int test_load_changes_mechanical_trajectory(void)
{
    struct foc_virtual_motor unloaded;
    struct foc_virtual_motor loaded;
    CHECK(foc_virtual_motor_init(&unloaded, &params, NULL) == 0);
    CHECK(foc_virtual_motor_init(&loaded, &params, NULL) == 0);
    CHECK(foc_virtual_motor_set_load(&unloaded, 0.0) == 0);
    CHECK(foc_virtual_motor_set_load(&loaded, 0.03) == 0);
    for (unsigned int step = 0U; step < 200U; ++step) {
        CHECK(foc_virtual_motor_step(&unloaded, 1.0, 0.5, 0.0001) == 0);
        CHECK(foc_virtual_motor_step(&loaded, 1.0, 0.5, 0.0001) == 0);
    }
    CHECK(
        fabs(unloaded.state.mechanical_velocity_rad_s - loaded.state.mechanical_velocity_rad_s) >
        1.0e-3
    );
    CHECK(
        fabs(unloaded.state.mechanical_position_rad - loaded.state.mechanical_position_rad) > 1.0e-5
    );
    return 0;
}

/* Test Entry ------------------------------------------------------------------- */

int main(void)
{
    return test_validation_and_zero_input() || test_power_consistency_and_instances() ||
        test_duty_and_invalid_parameters() || test_load_changes_mechanical_trajectory();
}

/* ---------------------------------------- end of file ----------------------------------------- */
