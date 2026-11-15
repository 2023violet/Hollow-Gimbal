/**
 * @file    test_foc_pmsm_reference.c
 * @brief   Independent PMSM RK4 and feedforward sign reference.
 * @version v0.1.0
 * @date    2026.11.14
 *
 * Design overview
 * ---------------
 * Recompute d/q cross-coupling and permanent-magnet back-EMF terms in double
 * precision, then integrate the continuous PMSM current equations with a
 * fourth-order Runge-Kutta step. The production feedforward entry is compared
 * with the independent equations; the plant itself remains test-owned.
 *
 * Test role
 * ---------
 * Cover positive and negative electrical speed, saliency, reverse targets,
 * and the steady-state cancellation implied by the model equations.
 *
 * Implementation role
 * -------------------
 * Keep the plant equations local to this executable and avoid reusing any
 * production transform or model helper for the expected values.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_current.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>

/* Private Types ---------------------------------------------------------------- */

struct pmsm_state {
    double id_a;
    double iq_a;
};

struct pmsm_parameters {
    double resistance_ohm;
    double ld_h;
    double lq_h;
    double psi_f_wb;
};

struct pmsm_derivative {
    double did_dt;
    double diq_dt;
};

/* Private Helpers -------------------------------------------------------------- */

static uint32_t next_random(uint32_t *state)
{
    *state = *state * 1664525U + 1013904223U;
    return *state;
}

static double random_double(uint32_t *state, double low, double high)
{
    const double unit = (double)(next_random(state) & 0x00ffffffU) / 16777215.0;
    return low + (high - low) * unit;
}

static struct pmsm_derivative derivative(
    const struct pmsm_state *state,
    const struct pmsm_parameters *parameters,
    double omega_e,
    double vd,
    double vq
)
{
    return (struct pmsm_derivative){
        .did_dt = (vd - parameters->resistance_ohm * state->id_a +
                   omega_e * parameters->lq_h * state->iq_a) /
            parameters->ld_h,
        .diq_dt = (vq - parameters->resistance_ohm * state->iq_a -
                   omega_e * (parameters->ld_h * state->id_a + parameters->psi_f_wb)) /
            parameters->lq_h,
    };
}

static void rk4_step(
    struct pmsm_state *state,
    const struct pmsm_parameters *parameters,
    double omega_e,
    double vd,
    double vq,
    double dt_s
)
{
    const struct pmsm_derivative k1 = derivative(state, parameters, omega_e, vd, vq);
    const struct pmsm_state s2 = {
        state->id_a + 0.5 * dt_s * k1.did_dt,
        state->iq_a + 0.5 * dt_s * k1.diq_dt,
    };
    const struct pmsm_derivative k2 = derivative(&s2, parameters, omega_e, vd, vq);
    const struct pmsm_state s3 = {
        state->id_a + 0.5 * dt_s * k2.did_dt,
        state->iq_a + 0.5 * dt_s * k2.diq_dt,
    };
    const struct pmsm_derivative k3 = derivative(&s3, parameters, omega_e, vd, vq);
    const struct pmsm_state s4 = {
        state->id_a + dt_s * k3.did_dt,
        state->iq_a + dt_s * k3.diq_dt,
    };
    const struct pmsm_derivative k4 = derivative(&s4, parameters, omega_e, vd, vq);

    state->id_a += dt_s / 6.0 * (k1.did_dt + 2.0 * k2.did_dt + 2.0 * k3.did_dt + k4.did_dt);
    state->iq_a += dt_s / 6.0 * (k1.diq_dt + 2.0 * k2.diq_dt + 2.0 * k3.diq_dt + k4.diq_dt);
}

static int close_enough(double actual, double expected, double tolerance)
{
    const double scale = fmax(1.0, fmax(fabs(actual), fabs(expected)));
    return fabs(actual - expected) <= tolerance * scale;
}

/* Public Functions ------------------------------------------------------------- */

int main(void)
{
    struct foc_current_feedforward_context context = {0};
    const struct foc_current_feedforward_model model = {
        .ld_h = 0.0008f,
        .lq_h = 0.0013f,
        .psi_f_wb = 0.042f,
    };
    const struct pmsm_parameters parameters = {
        .resistance_ohm = 0.21,
        .ld_h = model.ld_h,
        .lq_h = model.lq_h,
        .psi_f_wb = model.psi_f_wb,
    };
    struct pmsm_state plant = {0.0, 0.0};
    uint32_t seed = 0x94b3e12cU;
    double max_equation_error = 0.0;
    double max_steady_state_error = 0.0;

    if (foc_current_feedforward_init(&context, &model, true) != FOC_STATUS_OK) {
        fprintf(stderr, "PMSM reference initialization failed\n");
        return 1;
    }

    for (unsigned int index = 0U; index < 5000U; ++index) {
        const double omega_e = random_double(&seed, -900.0, 900.0);
        const double target_d = random_double(&seed, -4.0, 4.0);
        const double target_q = random_double(&seed, -4.0, 4.0);
        const double dt_s = random_double(&seed, 0.00001, 0.0002);
        const double expected_d = -omega_e * parameters.lq_h * target_q;
        const double expected_q = omega_e * (parameters.ld_h * target_d + parameters.psi_f_wb);
        struct foc_current_feedforward_output output;

        if (foc_current_feedforward_compute(
                &context,
                (float)omega_e,
                (struct foc_vector_dq){(float)target_d, (float)target_q},
                &output
            ) != FOC_STATUS_OK ||
            !close_enough(output.voltage_d_v, expected_d, 3.0e-6) ||
            !close_enough(output.voltage_q_v, expected_q, 3.0e-6) || !output.enabled) {
            fprintf(stderr, "feedforward equation mismatch index=%u\n", index);
            return 1;
        }
        max_equation_error = fmax(
            max_equation_error,
            fmax(
                fabs((double)output.voltage_d_v - expected_d),
                fabs((double)output.voltage_q_v - expected_q)
            )
        );

        /* At the target, resistive drop plus feedforward is an equilibrium input. */
        plant.id_a = target_d;
        plant.iq_a = target_q;
        rk4_step(
            &plant,
            &parameters,
            omega_e,
            parameters.resistance_ohm * target_d + expected_d,
            parameters.resistance_ohm * target_q + expected_q,
            dt_s
        );
        max_steady_state_error = fmax(
            max_steady_state_error,
            fmax(fabs(plant.id_a - target_d), fabs(plant.iq_a - target_q))
        );
    }

    if (max_steady_state_error > 2.0e-11) {
        fprintf(stderr, "PMSM RK4 equilibrium drift %.9g\n", max_steady_state_error);
        return 1;
    }
    printf(
        "test_foc_pmsm_reference passed samples=5000 max_equation_error=%.9g "
        "max_rk4_equilibrium_error=%.9g reverse_speed=1 saliency=1\n",
        max_equation_error,
        max_steady_state_error
    );
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
