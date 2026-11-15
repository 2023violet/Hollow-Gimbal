/**
 * @file    test_foc_robustness.c
 * @brief   Deterministic Float32 closed-loop robustness study with a double RK4 plant.
 * @version v0.1.0
 * @date    2026.11.14
 *
 * Design overview
 * ---------------
 * The production Current, Velocity, Position, and Trajectory controllers are
 * driven at H0=200 us and H1=500 us. This executable owns an independent
 * double-precision PMSM plant, encoder model, delay history, and parameter
 * sweep. It is a research witness, not a hardware model.
 *
 * Test role
 * ---------
 * Quantify nominal response, parameter sensitivity, sensor error, delay, and
 * saturation without changing production control code or Board defaults.
 *
 * Implementation role
 * -------------------
 * Keep plant equations, measurement faults, and metrics local to this test.
 * A fixed seed makes every CSV row reproducible.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_current.h"
#include "foc_motion.h"
#include "foc_trajectory.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Private Constants ------------------------------------------------------------ */

static const double ROBUST_PI = 3.14159265358979323846;
static const double ROBUST_TWO_PI = 6.28318530717958647692;
static const double ROBUST_SQRT3 = 1.73205080756887729353;
static const double H0_S = 0.0002;
static const double H1_S = 0.0005;
enum {
    ROBUST_SIM_TICKS = 20000U
};

/* Private Types ---------------------------------------------------------------- */

enum robust_mode {
    ROBUST_CURRENT,
    ROBUST_VELOCITY,
    ROBUST_POSITION,
};

struct robust_motor {
    unsigned int pole_pairs;
    double rs_ohm;
    double ld_h;
    double lq_h;
    double psi_f_wb;
    double inertia_kg_m2;
    double damping_nm_s;
};

struct robust_state {
    double id_a;
    double iq_a;
    double speed_rad_s;
    double position_rad;
};

struct robust_faults {
    double angle_bias_rad;
    double speed_noise_rad_s;
    unsigned int sensor_delay_us;
    unsigned int drop_every;
    bool feedforward;
    bool trajectory;
};

struct robust_metrics {
    enum robust_mode mode;
    const char *label;
    double rmse;
    double tail_rmse;
    double peak_error;
    double max_iq;
    double max_voltage;
    double torque_rms;
    double torque_peak;
    double torque_sum_sq;
    unsigned int torque_samples;
    double max_requested_voltage;
    double max_feedforward_voltage;
    double minimum_steady_margin;
    double integral_abs_max;
    unsigned int first_saturation_tick;
    unsigned int longest_saturation;
    unsigned int saturated_steps;
    unsigned int rejected_samples;
    unsigned int tail_samples;
    bool unstable;
};

struct robust_rng {
    uint32_t state;
};

struct robust_controller_override {
    float kp_d;
    float kp_q;
    float ki_d;
    float ki_q;
    bool use_feedforward_model;
    struct foc_current_feedforward_model feedforward_model;
};

static struct robust_controller_override robust_override = {
    .kp_d = 1.08f,
    .kp_q = 1.35f,
    .ki_d = 540.0f,
    .ki_q = 540.0f,
};
static double robust_initial_speed_rad_s;
static FILE *robust_trace_file;

/* Private Helpers -------------------------------------------------------------- */

static uint32_t robust_next(struct robust_rng *rng)
{
    rng->state = rng->state * 1664525U + 1013904223U;
    return rng->state;
}

static double robust_noise(struct robust_rng *rng, double amplitude)
{
    const double unit = (double)(robust_next(rng) & 0x00ffffffU) / 16777215.0;
    return (2.0 * unit - 1.0) * amplitude;
}

static double robust_clamp(double value, double low, double high)
{
    return fmax(low, fmin(value, high));
}

static double robust_angle(double mechanical, const struct robust_motor *motor)
{
    return (double)motor->pole_pairs * mechanical + 0.37;
}

static struct robust_state robust_derivative(
    const struct robust_motor *motor,
    struct robust_state state,
    double alpha_v,
    double beta_v,
    double load_nm
)
{
    const double theta = robust_angle(state.position_rad, motor);
    const double cosine = cos(theta);
    const double sine = sin(theta);
    const double vd = cosine * alpha_v + sine * beta_v;
    const double vq = cosine * beta_v - sine * alpha_v;
    const double omega_e = (double)motor->pole_pairs * state.speed_rad_s;
    const double torque = 1.5 * (double)motor->pole_pairs *
        (motor->psi_f_wb * state.iq_a + (motor->ld_h - motor->lq_h) * state.id_a * state.iq_a);

    return (struct robust_state){
        (vd - motor->rs_ohm * state.id_a + omega_e * motor->lq_h * state.iq_a) / motor->ld_h,
        (vq - motor->rs_ohm * state.iq_a - omega_e * (motor->ld_h * state.id_a + motor->psi_f_wb)) /
            motor->lq_h,
        (torque - motor->damping_nm_s * state.speed_rad_s - load_nm) / motor->inertia_kg_m2,
        state.speed_rad_s,
    };
}

static struct robust_state
robust_add(struct robust_state state, struct robust_state derivative, double dt)
{
    return (struct robust_state){
        state.id_a + dt * derivative.id_a,
        state.iq_a + dt * derivative.iq_a,
        state.speed_rad_s + dt * derivative.speed_rad_s,
        state.position_rad + dt * derivative.position_rad,
    };
}

static double robust_apply_voltage(
    const struct robust_motor *motor,
    struct robust_state *state,
    struct foc_svpwm_output output,
    double vbus_v,
    double load_nm
)
{
    const double alpha = vbus_v * (2.0 * output.duty_a - output.duty_b - output.duty_c) / 3.0;
    const double beta = vbus_v * (output.duty_b - output.duty_c) / ROBUST_SQRT3;
    const struct robust_state k1 = robust_derivative(motor, *state, alpha, beta, load_nm);
    const struct robust_state k2 =
        robust_derivative(motor, robust_add(*state, k1, H0_S / 2.0), alpha, beta, load_nm);
    const struct robust_state k3 =
        robust_derivative(motor, robust_add(*state, k2, H0_S / 2.0), alpha, beta, load_nm);
    const struct robust_state k4 =
        robust_derivative(motor, robust_add(*state, k3, H0_S), alpha, beta, load_nm);
    const struct robust_state sum =
        robust_add(robust_add(robust_add(k1, k2, 2.0), k3, 2.0), k4, 1.0);
    *state = robust_add(*state, sum, H0_S / 6.0);
    return hypot(alpha, beta);
}

static struct foc_vector_abc
robust_phase_current(const struct robust_motor *motor, struct robust_state state)
{
    const double theta = robust_angle(state.position_rad, motor);
    const double alpha = cos(theta) * state.id_a - sin(theta) * state.iq_a;
    const double beta = sin(theta) * state.id_a + cos(theta) * state.iq_a;
    return (struct foc_vector_abc){
        (float)alpha,
        (float)(-0.5 * alpha + 0.5 * ROBUST_SQRT3 * beta),
        (float)(-0.5 * alpha - 0.5 * ROBUST_SQRT3 * beta),
    };
}

static double robust_quantized_position(double position_rad, const struct robust_faults *faults)
{
    const double counts = 65536.0;
    const double quantized = round(position_rad * counts / ROBUST_TWO_PI) * ROBUST_TWO_PI / counts;
    return quantized + faults->angle_bias_rad;
}

static void robust_metrics_update(
    struct robust_metrics *metrics,
    double error,
    double iq,
    double voltage,
    unsigned int saturated,
    unsigned int tick
)
{
    const double squared = error * error;
    metrics->rmse += squared;
    if (tick >= ROBUST_SIM_TICKS * 3U / 4U) {
        metrics->tail_rmse += squared;
        ++metrics->tail_samples;
    }
    metrics->peak_error = fmax(metrics->peak_error, fabs(error));
    metrics->max_iq = fmax(metrics->max_iq, fabs(iq));
    metrics->max_voltage = fmax(metrics->max_voltage, fabs(voltage));
    metrics->saturated_steps += saturated != 0U;
}

static const char *robust_mode_name(enum robust_mode mode)
{
    return mode == ROBUST_CURRENT ? "current" : mode == ROBUST_VELOCITY ? "velocity" : "position";
}

static FILE *robust_open_trace(void)
{
#ifdef _WIN32
    FILE *file = NULL;
    return fopen_s(&file, "robustness-trace.csv", "w") == 0 ? file : NULL;
#else
    return fopen("robustness-trace.csv", "w");
#endif
}

static int robust_run_case(
    const struct robust_motor *motor,
    enum robust_mode mode,
    const char *label,
    double reference,
    double load_nm,
    struct robust_faults faults,
    struct robust_metrics *metrics
)
{
    const struct foc_current_config current_config = {
        .pwm =
            {.vbus_min_v = 6.0f,
             .voltage_limit_v = 12.0f,
             .duty_min = 0.02f,
             .duty_max = 0.98f,
             .max_dt_s = 0.001f},
        .kp_v_per_a = {0.0f, 0.0f},
        .ki_v_per_a_s = {0.0f, 0.0f},
        .current_limit_a = 2.5f,
    };
    const struct foc_velocity_config velocity_config = {
        .kp_a_per_rad_s = 0.18f,
        .ki_a_per_rad = 1.4f,
        .max_dt_s = 0.002f,
    };
    const struct foc_position_config position_config = {
        .kp_per_s = 4.0f,
        .velocity_limit_rad_s = 12.0f,
    };
    const struct foc_trajectory_config trajectory_config = {
        .enabled = faults.trajectory,
        .max_velocity_rad_s = 12.0f,
        .max_accel_rad_s2 = 20.0f,
        .max_dt_s = 0.002f,
        .position_tolerance_rad = 0.01f,
    };
    struct foc_current_context current = {0};
    struct foc_current_feedforward_context feedforward = {0};
    struct foc_velocity_context velocity = {0};
    struct foc_trajectory_context trajectory = {0};
    struct robust_state plant = {.speed_rad_s = robust_initial_speed_rad_s};
    struct robust_state history[ROBUST_SIM_TICKS + 1U] = {0};
    struct foc_vector_dq target = {0};
    struct foc_electrical_state electrical = {0};
    struct robust_rng rng = {.state = 0x91e10da5U};
    unsigned int h1_seen = 0U;
    unsigned int h0_count = 0U;
    unsigned int rejected_samples = 0U;
    unsigned int saturation_streak = 0U;

    struct foc_current_config tuned_current_config = current_config;
    tuned_current_config.kp_v_per_a =
        (struct foc_vector_dq){robust_override.kp_d, robust_override.kp_q};
    tuned_current_config.ki_v_per_a_s =
        (struct foc_vector_dq){robust_override.ki_d, robust_override.ki_q};
    const struct foc_current_feedforward_model feedforward_model =
        robust_override.use_feedforward_model ? robust_override.feedforward_model
                                              : (struct foc_current_feedforward_model){
                                                    (float)motor->ld_h,
                                                    (float)motor->lq_h,
                                                    (float)motor->psi_f_wb,
                                                };

    if (foc_current_init(&current, &tuned_current_config) != FOC_STATUS_OK ||
        foc_current_feedforward_init(&feedforward, &feedforward_model, faults.feedforward) !=
            FOC_STATUS_OK ||
        foc_velocity_init(&velocity, &velocity_config) != FOC_STATUS_OK ||
        foc_trajectory_init(&trajectory, &trajectory_config) != FOC_TRAJECTORY_STATUS_OK ||
        foc_trajectory_reset(&trajectory, 0.0f) != FOC_TRAJECTORY_STATUS_OK) {
        return 1;
    }

    *metrics = (struct robust_metrics){.mode = mode, .label = label};
    metrics->minimum_steady_margin = INFINITY;
    for (unsigned int tick = 0U; tick < ROBUST_SIM_TICKS; ++tick) {
        const double time_s = (double)tick * 0.0001;
        history[tick] = plant;

        if (tick % 5U == 0U) {
            const unsigned int delay_ticks = faults.sensor_delay_us / 100U;
            const unsigned int delayed_tick = tick > delay_ticks ? tick - delay_ticks : 0U;
            struct robust_state sensed = history[delayed_tick];
            const double measured_position =
                robust_quantized_position(sensed.position_rad, &faults);
            const double measured_speed =
                sensed.speed_rad_s + robust_noise(&rng, faults.speed_noise_rad_s);
            if (faults.drop_every != 0U && (tick / 5U) % faults.drop_every == 0U && tick != 0U) {
                ++rejected_samples;
            } else {
                if (mode == ROBUST_POSITION) {
                    float velocity_reference;
                    if (faults.trajectory) {
                        struct foc_trajectory_output planned;
                        if (foc_trajectory_position_step(
                                &trajectory,
                                (float)reference,
                                (float)measured_position,
                                (float)H1_S,
                                &planned
                            ) != FOC_TRAJECTORY_STATUS_OK) {
                            return 1;
                        }
                        velocity_reference = planned.velocity_ref_rad_s;
                    } else {
                        struct foc_position_output position_output;
                        if (foc_position_step(
                                &position_config,
                                (float)reference,
                                (float)measured_position,
                                &position_output
                            ) != FOC_STATUS_OK) {
                            return 1;
                        }
                        velocity_reference = position_output.velocity_ref_rad_s;
                    }
                    target = (struct foc_vector_dq){0.0f, velocity_reference};
                } else if (mode == ROBUST_VELOCITY) {
                    target = (struct foc_vector_dq){0.0f, reference};
                } else {
                    target = (struct foc_vector_dq){0.0f, reference};
                }
                if (mode != ROBUST_CURRENT) {
                    struct foc_velocity_output velocity_output;
                    if (foc_velocity_step(
                            &velocity,
                            &(struct foc_velocity_input){
                                .ref_rad_s = target.q,
                                .measured_rad_s = (float)measured_speed,
                                .dt_s = (float)H1_S,
                                .iq_limit_a = current_config.current_limit_a,
                                .enabled = true,
                            },
                            &velocity_output
                        ) != FOC_STATUS_OK) {
                        return 1;
                    }
                    target.q = velocity_output.iq_ref_a;
                }
                if (foc_rotor_electrical(
                        &(struct foc_rotor_config){motor->pole_pairs, 0.37f},
                        (float)measured_position,
                        (float)measured_speed,
                        &electrical
                    ) != FOC_STATUS_OK) {
                    return 1;
                }
                h1_seen = 1U;
            }
        }

        if (tick % 2U == 0U) {
            struct foc_current_output output;
            const double theta =
                h1_seen ? (double)electrical.angle_rad : robust_angle(plant.position_rad, motor);
            const double omega_e = h1_seen ? (double)electrical.velocity_rad_s
                                           : (double)motor->pole_pairs * plant.speed_rad_s;
            const struct foc_current_input current_input = {
                .phase_current_a = robust_phase_current(motor, plant),
                .theta_elec_rad = (float)theta,
                .target_a = target,
                .vbus_v = 24.0f,
                .dt_s = (float)H0_S,
            };
            const struct foc_vector_dq previous_integral = current.integral_v;
            const double current_alpha = (double)current_input.phase_current_a.a;
            const double current_beta = ((double)current_input.phase_current_a.b -
                                         (double)current_input.phase_current_a.c) /
                ROBUST_SQRT3;
            const double measured_d = current_alpha * cos(theta) + current_beta * sin(theta);
            const double measured_q = current_beta * cos(theta) - current_alpha * sin(theta);
            const double error_d = (double)target.d - measured_d;
            const double error_q = (double)target.q - measured_q;
            const double feedforward_d =
                faults.feedforward ? -omega_e * feedforward_model.lq_h * (double)target.q : 0.0;
            const double feedforward_q = faults.feedforward
                ? omega_e * (feedforward_model.ld_h * (double)target.d + feedforward_model.psi_f_wb)
                : 0.0;
            const double trial_integral_d = (double)previous_integral.d +
                (double)tuned_current_config.ki_v_per_a_s.d * H0_S * error_d;
            const double trial_integral_q = (double)previous_integral.q +
                (double)tuned_current_config.ki_v_per_a_s.q * H0_S * error_q;
            const double requested_d = (double)tuned_current_config.kp_v_per_a.d * error_d +
                trial_integral_d + feedforward_d;
            const double requested_q = (double)tuned_current_config.kp_v_per_a.q * error_q +
                trial_integral_q + feedforward_q;
            const double steady_vd =
                motor->rs_ohm * (double)target.d - omega_e * motor->lq_h * (double)target.q;
            const double steady_vq = motor->rs_ohm * (double)target.q +
                omega_e * (motor->ld_h * (double)target.d + motor->psi_f_wb);
            const double steady_required = hypot(steady_vd, steady_vq);
            const double voltage_limit = fmin(12.0, 24.0 / ROBUST_SQRT3);
            const int status = faults.feedforward
                ? foc_current_step_with_feedforward(
                      &current,
                      &current_input,
                      &feedforward,
                      (float)omega_e,
                      &output
                  )
                : foc_current_step(&current, &current_input, &output);
            if (status != FOC_STATUS_OK || output.output_enable == 0U) {
                ++rejected_samples;
                (void)robust_apply_voltage(
                    motor,
                    &plant,
                    (struct foc_svpwm_output){0.5f, 0.5f, 0.5f, 1U, 1U},
                    24.0,
                    load_nm
                );
            } else {
                const double voltage =
                    robust_apply_voltage(motor, &plant, output.svpwm, 24.0, load_nm);
                const double torque = 1.5 * (double)motor->pole_pairs *
                    (motor->psi_f_wb * plant.iq_a +
                     (motor->ld_h - motor->lq_h) * plant.id_a * plant.iq_a);
                const double control_error = mode == ROBUST_CURRENT ? target.q - plant.iq_a
                    : mode == ROBUST_VELOCITY                       ? reference - plant.speed_rad_s
                                              : reference - plant.position_rad;
                robust_metrics_update(
                    metrics,
                    control_error,
                    plant.iq_a,
                    voltage,
                    output.svpwm.saturated,
                    tick
                );
                metrics->max_requested_voltage =
                    fmax(metrics->max_requested_voltage, hypot(requested_d, requested_q));
                metrics->max_feedforward_voltage =
                    fmax(metrics->max_feedforward_voltage, hypot(feedforward_d, feedforward_q));
                metrics->minimum_steady_margin =
                    fmin(metrics->minimum_steady_margin, voltage_limit - steady_required);
                metrics->integral_abs_max = fmax(
                    metrics->integral_abs_max,
                    fmax(fabs((double)current.integral_v.d), fabs((double)current.integral_v.q))
                );
                if (output.svpwm.saturated != 0U) {
                    if (metrics->saturated_steps == 1U) {
                        metrics->first_saturation_tick = tick;
                    }
                    ++saturation_streak;
                    metrics->longest_saturation = metrics->longest_saturation > saturation_streak
                        ? metrics->longest_saturation
                        : saturation_streak;
                } else {
                    saturation_streak = 0U;
                }
                if (robust_trace_file != NULL &&
                    (strstr(label, "inductance") != NULL || strstr(label, "current") != NULL ||
                     strstr(label, "ld-") != NULL)) {
                    fprintf(
                        robust_trace_file,
                        "%u,%s,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%u,%.9g,"
                        "%.9g\n",
                        tick,
                        label,
                        plant.id_a,
                        plant.iq_a,
                        target.d,
                        target.q,
                        omega_e,
                        requested_d,
                        requested_q,
                        output.voltage_dq_v.d,
                        output.voltage_dq_v.q,
                        feedforward_d,
                        feedforward_q,
                        current.integral_v.q,
                        output.svpwm.saturated,
                        steady_required,
                        voltage_limit - steady_required
                    );
                }
                metrics->torque_sum_sq += torque * torque;
                metrics->torque_peak = fmax(metrics->torque_peak, fabs(torque));
                ++metrics->torque_samples;
                ++h0_count;
            }
        }

        if (!isfinite(plant.id_a) || !isfinite(plant.iq_a) || !isfinite(plant.speed_rad_s) ||
            !isfinite(plant.position_rad) || fabs(plant.speed_rad_s) > 1000.0 ||
            fabs(plant.id_a) > 1000.0 || fabs(plant.iq_a) > 1000.0) {
            metrics->unstable = true;
            break;
        }
        (void)time_s;
    }

    if (h0_count == 0U) {
        metrics->unstable = true;
        return 0;
    }
    metrics->rmse = sqrt(metrics->rmse / (double)h0_count);
    metrics->tail_rmse = sqrt(metrics->tail_rmse / fmax(1U, metrics->tail_samples));
    metrics->torque_rms = sqrt(metrics->torque_sum_sq / fmax(1U, metrics->torque_samples));
    metrics->rejected_samples = rejected_samples;
    return 0;
}

static void robust_print(const struct robust_metrics *metrics)
{
    printf(
        "%s,%s,%.6g,%.6g,%.6g,%.6g,%.6g,%.6g,%.6g,%.6g,%.6g,%.6g,%.6g,%u,%u,%u,%u,%u\n",
        robust_mode_name(metrics->mode),
        metrics->label,
        metrics->rmse,
        metrics->tail_rmse,
        metrics->peak_error,
        metrics->max_iq,
        metrics->max_voltage,
        metrics->torque_rms,
        metrics->torque_peak,
        metrics->max_requested_voltage,
        metrics->max_feedforward_voltage,
        metrics->minimum_steady_margin,
        metrics->integral_abs_max,
        metrics->first_saturation_tick,
        metrics->longest_saturation,
        metrics->saturated_steps,
        metrics->rejected_samples,
        metrics->unstable ? 1U : 0U
    );
}

/* Public Functions ------------------------------------------------------------- */

int main(void)
{
    const struct robust_motor nominal = {
        .pole_pairs = 4U,
        .rs_ohm = 0.30,
        .ld_h = 0.0010,
        .lq_h = 0.0015,
        .psi_f_wb = 0.05,
        .inertia_kg_m2 = 0.002,
        .damping_nm_s = 0.006,
    };
    struct robust_metrics metrics;
    unsigned int unstable_cases = 0U;
    unsigned int total_cases = 0U;
    const struct {
        const char *label;
        struct robust_motor motor;
        enum robust_mode mode;
        double reference;
        double load_nm;
        struct robust_faults faults;
    } cases[] = {
        {"current-step", nominal, ROBUST_CURRENT, 1.0, 0.0, {0}},
        {"velocity-nominal", nominal, ROBUST_VELOCITY, 8.0, 0.0, {0}},
        {"velocity-load", nominal, ROBUST_VELOCITY, 8.0, 0.08, {0}},
        {"velocity-reverse", nominal, ROBUST_VELOCITY, -8.0, 0.0, {0}},
        {"position-trajectory", nominal, ROBUST_POSITION, 4.0, 0.0, {.trajectory = true}},
        {"position-direct", nominal, ROBUST_POSITION, 2.0, 0.0, {0}},
        {"rs-half",
         (struct robust_motor){4U, 0.15, 0.001, 0.0015, 0.05, 0.002, 0.006},
         ROBUST_VELOCITY,
         8.0,
         0.04,
         {0}},
        {"rs-high",
         (struct robust_motor){4U, 0.45, 0.001, 0.0015, 0.05, 0.002, 0.006},
         ROBUST_VELOCITY,
         8.0,
         0.04,
         {0}},
        {"inductance-half",
         (struct robust_motor){4U, 0.30, 0.0005, 0.00075, 0.05, 0.002, 0.006},
         ROBUST_CURRENT,
         1.5,
         0.0,
         {0}},
        {"inductance-high",
         (struct robust_motor){4U, 0.30, 0.0015, 0.00225, 0.05, 0.002, 0.006},
         ROBUST_CURRENT,
         1.5,
         0.0,
         {0}},
        {"flux-low",
         (struct robust_motor){4U, 0.30, 0.001, 0.0015, 0.04, 0.002, 0.006},
         ROBUST_VELOCITY,
         8.0,
         0.04,
         {0}},
        {"flux-high",
         (struct robust_motor){4U, 0.30, 0.001, 0.0015, 0.06, 0.002, 0.006},
         ROBUST_VELOCITY,
         8.0,
         0.04,
         {0}},
        {"inertia-quarter",
         (struct robust_motor){4U, 0.30, 0.001, 0.0015, 0.05, 0.0005, 0.006},
         ROBUST_VELOCITY,
         8.0,
         0.04,
         {0}},
        {"inertia-four",
         (struct robust_motor){4U, 0.30, 0.001, 0.0015, 0.05, 0.008, 0.006},
         ROBUST_VELOCITY,
         8.0,
         0.04,
         {0}},
        {"angle-bias", nominal, ROBUST_VELOCITY, 8.0, 0.04, {.angle_bias_rad = 0.03}},
        {"speed-noise", nominal, ROBUST_VELOCITY, 8.0, 0.04, {.speed_noise_rad_s = 0.25}},
        {"sensor-delay", nominal, ROBUST_VELOCITY, 8.0, 0.04, {.sensor_delay_us = 400U}},
        {"drop-and-feedforward",
         nominal,
         ROBUST_VELOCITY,
         8.0,
         0.04,
         {.drop_every = 9U, .feedforward = true}},
        {"combo-rs-lq-j",
         (struct robust_motor){4U, 0.45, 0.0005, 0.00075, 0.04, 0.008, 0.012},
         ROBUST_VELOCITY,
         8.0,
         0.10,
         {0}},
        {"combo-flux-delay",
         (struct robust_motor){4U, 0.30, 0.0015, 0.00225, 0.06, 0.0005, 0.003},
         ROBUST_VELOCITY,
         8.0,
         0.06,
         {.sensor_delay_us = 400U, .angle_bias_rad = 0.02}},
        {"random-combo-a",
         (struct robust_motor){4U, 0.22, 0.0007, 0.0019, 0.047, 0.0035, 0.009},
         ROBUST_POSITION,
         2.0,
         0.03,
         {.trajectory = true, .speed_noise_rad_s = 0.1}},
        {"random-combo-b",
         (struct robust_motor){4U, 0.41, 0.0014, 0.0010, 0.056, 0.001, 0.002},
         ROBUST_CURRENT,
         -1.5,
         0.0,
         {.feedforward = true}},
    };

    robust_trace_file = robust_open_trace();
    if (robust_trace_file != NULL) {
        fputs(
            "tick,label,id,iq,id_ref,iq_ref,omega_e,request_d,request_q,applied_d,applied_q,"
            "ff_d,ff_q,integral_q,saturated,steady_required,steady_margin\n",
            robust_trace_file
        );
    }
    puts(
        "mode,label,rmse,tail_rmse,peak_error,max_iq,max_voltage,torque_rms,torque_peak,"
        "max_requested_voltage,max_feedforward_voltage,min_steady_margin,integral_abs_max,"
        "first_saturation_tick,longest_saturation,saturated_steps,rejected_samples,"
        "unstable"
    );
    for (unsigned int index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        if (robust_run_case(
                &cases[index].motor,
                cases[index].mode,
                cases[index].label,
                cases[index].reference,
                cases[index].load_nm,
                cases[index].faults,
                &metrics
            ) != 0) {
            fprintf(stderr, "robustness case setup failed label=%s\n", cases[index].label);
            return 1;
        }
        robust_print(&metrics);
        unstable_cases += metrics.unstable;
        ++total_cases;
    }

    {
        const struct robust_motor ld_half = {4U, 0.30, 0.0005, 0.00075, 0.05, 0.002, 0.006};
        const struct robust_motor ld_high = {4U, 0.30, 0.0015, 0.00225, 0.05, 0.002, 0.006};
        const struct foc_current_feedforward_model nominal_feedforward = {
            (float)nominal.ld_h,
            (float)nominal.lq_h,
            (float)nominal.psi_f_wb,
        };
        robust_initial_speed_rad_s = 0.0;
        robust_override = (struct robust_controller_override){
            .kp_d = 1.08f,
            .kp_q = 1.35f,
            .ki_d = 540.0f,
            .ki_q = 540.0f,
        };
        if (robust_run_case(
                &ld_half,
                ROBUST_CURRENT,
                "ld-half-default-pi",
                1.5,
                0.0,
                (struct robust_faults){0},
                &metrics
            ) != 0) {
            return 1;
        }
        robust_print(&metrics);
        robust_initial_speed_rad_s = 20.0;
        if (robust_run_case(
                &ld_half,
                ROBUST_CURRENT,
                "ld-half-fixed-speed-default",
                1.5,
                0.33,
                (struct robust_faults){0},
                &metrics
            ) != 0) {
            return 1;
        }
        robust_print(&metrics);
        robust_override.kp_d = 0.5f;
        robust_override.kp_q = 0.75f;
        robust_override.ki_d = 300.0f;
        robust_override.ki_q = 300.0f;
        if (robust_run_case(
                &ld_half,
                ROBUST_CURRENT,
                "ld-half-scheduled-pi",
                1.5,
                0.0,
                (struct robust_faults){0},
                &metrics
            ) != 0) {
            return 1;
        }
        robust_print(&metrics);
        if (robust_run_case(
                &ld_half,
                ROBUST_CURRENT,
                "ld-half-fixed-speed-scheduled",
                1.5,
                0.33,
                (struct robust_faults){0},
                &metrics
            ) != 0) {
            return 1;
        }
        robust_print(&metrics);
        robust_initial_speed_rad_s = 0.0;
        robust_override.kp_d = 1.5f;
        robust_override.kp_q = 2.25f;
        robust_override.ki_d = 300.0f;
        robust_override.ki_q = 300.0f;
        if (robust_run_case(
                &ld_high,
                ROBUST_CURRENT,
                "ld-high-scheduled-pi",
                1.5,
                0.0,
                (struct robust_faults){0},
                &metrics
            ) != 0) {
            return 1;
        }
        robust_print(&metrics);

        robust_override = (struct robust_controller_override){
            .kp_d = 1.08f,
            .kp_q = 1.35f,
            .ki_d = 540.0f,
            .ki_q = 540.0f,
            .use_feedforward_model = true,
            .feedforward_model = nominal_feedforward,
        };
        robust_initial_speed_rad_s = 20.0;
        if (robust_run_case(
                &nominal,
                ROBUST_CURRENT,
                "ff-speed-off",
                1.0,
                0.30,
                (struct robust_faults){.feedforward = false},
                &metrics
            ) != 0) {
            return 1;
        }
        robust_print(&metrics);
        if (robust_run_case(
                &nominal,
                ROBUST_CURRENT,
                "ff-speed-model-match",
                1.0,
                0.30,
                (struct robust_faults){.feedforward = true},
                &metrics
            ) != 0) {
            return 1;
        }
        robust_print(&metrics);
        if (robust_run_case(
                &ld_half,
                ROBUST_CURRENT,
                "ff-plant-ld-half",
                1.0,
                0.30,
                (struct robust_faults){.feedforward = true},
                &metrics
            ) != 0) {
            return 1;
        }
        robust_print(&metrics);
        robust_override.feedforward_model =
            (struct foc_current_feedforward_model){0.0005f, 0.00075f, 0.04f};
        if (robust_run_case(
                &nominal,
                ROBUST_CURRENT,
                "ff-speed-model-wrong",
                1.0,
                0.30,
                (struct robust_faults){.feedforward = true},
                &metrics
            ) != 0) {
            return 1;
        }
        robust_print(&metrics);
        robust_initial_speed_rad_s = 0.0;
        robust_override = (struct robust_controller_override){
            .kp_d = 1.08f,
            .kp_q = 1.35f,
            .ki_d = 540.0f,
            .ki_q = 540.0f,
        };
        total_cases += 9U;
    }
    if (robust_trace_file != NULL) {
        fclose(robust_trace_file);
        robust_trace_file = NULL;
    }
    fprintf(
        stderr,
        "robustness_cases=%u unstable_cases=%u h0_us=200 h1_us=500 rk4=1\n",
        total_cases,
        unstable_cases
    );
    return unstable_cases == 0U ? 0 : 1;
}

/* ---------------------------------------- end of file ----------------------------------------- */
