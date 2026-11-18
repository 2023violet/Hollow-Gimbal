/**
 * @file    test_foc_sensor_est.c
 * @brief   Matched-input speed estimator and native virtual control experiments.
 * @version v0.1.0
 * @date    2026.11.27
 *
 * Test role
 * ---------
 * Reuse Encoder admission, LUT and timestamped feedback. Keep analytic
 * trajectory truth and the experimental PLL outside production.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_virtual_current.h"
#include "foc_sensor_pll_experiment.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Private Helpers -------------------------------------------------------------- */

#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            fprintf(stderr, "sensor estimator: %d: %s\n", __LINE__, #c); \
            return 1;                                                    \
        }                                                                \
    } while (0)

static const double tau = 6.28318530717958647692;
static const int16_t crawl_offsets[] = {
    -21, -35, -49, -62, -73, -82, -89, -92, -93, -91, -86, -79, -71, -62, -53, -46,
    -40, -35, -33, -33, -34, -36, -38, -40, -42, -42, -40, -35, -28, -18, -6,  8,
    24,  40,  55,  68,  79,  87,  92,  93,  92,  88,  82,  74,  66,  58,  50,  43,
    38,  35,  33,  33,  34,  37,  39,  41,  42,  41,  39,  34,  26,  17,  5,   -8,
};
static const struct foc_encoder_lut crawl_lut = {crawl_offsets, 64U, 16384U, 19U};

enum speed_estimator {
    RAW,
    LPF,
    ALPHA_BETA,
    PLL,
    SPEED_ESTIMATORS
};

struct speed_segment {
    const char *name;
    uint32_t duration_us;
    double first_speed;
    double last_speed;
    double step;
    unsigned noise_codes;
};

struct speed_error {
    double square;
    double signed_sum;
    double peak;
    uint32_t count;
    uint32_t inside_since;
    bool inside;
};

struct speed_study {
    const char *name;
    const struct speed_segment *segments;
    unsigned length;
    bool variable_dt;
    bool use_lut;
    enum foc_encoder_direction direction;
    int32_t initial_turns;
    uint32_t seed;
};

static const struct speed_segment training_segments[] = {
    {"quiet", 120000U, 0.0, 0.0, 0.0, 1U},
    {"step", 80000U, 20.0, 20.0, 20.0, 0U},
    {"reverse", 80000U, -20.0, -20.0, -40.0, 0U},
    {"crawl", 280000U, 0.9, 0.9, 0.0, 0U},
};
static const struct speed_segment holdout_segments[] = {
    {"quiet", 140000U, 0.0, 0.0, 0.0, 1U},
    {"crawl", 7000000U, 0.9, 0.9, 0.9, 0U},
    {"accel", 120000U, 0.9, 18.9, 0.0, 0U},
    {"step", 90000U, 35.0, 35.0, 16.1, 0U},
    {"reverse", 90000U, -28.0, -28.0, -63.0, 0U},
    {"decel", 140000U, -28.0, 0.0, 0.0, 0U},
    {"stop", 100000U, 0.0, 0.0, 0.0, 0U},
    {"restart", 90000U, -0.35, -0.35, -0.35, 0U},
    {"fast-positive", 100000U, 220.0, 220.0, 220.35, 0U},
    {"fast-negative", 100000U, -240.0, -240.0, -460.0, 0U},
};
static const struct speed_segment long_segments[] = {
    {"long-crawl", 400000U, -0.9, -0.9, 0.0, 0U},
    {"long-accel", 120000U, -0.9, 12.0, 0.0, 0U},
    {"long-steady", 180000U, 12.0, 12.0, 0.0, 0U},
};

static int encoder_raw(
    const struct foc_virtual_motor *motor,
    const struct foc_encoder_config *config,
    unsigned noise_codes,
    uint32_t *random,
    uint16_t *raw
)
{
    struct foc_virtual_motor observation = *motor;
    const double sign = config->direction;
    const double raw_phase = sign * motor->state.mechanical_position_rad;
    const double distortion = 0.028 * sin(raw_phase + 0.41) + 0.011 * sin(3.0 * raw_phase - 0.23);
    observation.state.mechanical_position_rad += sign * distortion;
    CHECK(foc_virtual_motor_make_encoder_code(&observation, config, raw) == 0);
    if (noise_codes != 0U) {
        *random = *random * UINT32_C(1664525) + UINT32_C(1013904223);
        const int noise = (int)((*random >> 16) % (2U * noise_codes + 1U)) - (int)noise_codes;
        *raw = (uint16_t)(((uint32_t)((int)*raw + (int)config->counts_per_revolution + noise)) %
                          config->counts_per_revolution);
    }
    return 0;
}

static void speed_error_add(struct speed_error *error, double value)
{
    error->square += value * value;
    error->signed_sum += value;
    error->peak = fmax(error->peak, fabs(value));
    ++error->count;
}

static double speed_rms(const struct speed_error *error)
{
    return error->count != 0U ? sqrt(error->square / error->count) : 0.0;
}

/* Double reference solves the two-state implicit equations directly.
 * It does not use the scalar pole formula from the candidate. */
struct pll_reference {
    double phase_error;
    double velocity;
    bool seen;
};

static void pll_reference_step(
    struct pll_reference *reference,
    const struct foc_encoder_feedback *observation,
    double omega,
    double limit
)
{
    const double dt = observation->dt_s;
    const double measured_rate = observation->raw_velocity_rad_s;
    if (!reference->seen) {
        *reference = (struct pll_reference){0.0, measured_rate, true};
        return;
    }
    const double a11 = 1.0 + dt * 2.0 * omega;
    const double a12 = -dt;
    const double a21 = dt * omega * omega;
    const double b1 = reference->phase_error - dt * measured_rate;
    const double b2 = reference->velocity;
    const double determinant = a11 - a12 * a21;
    reference->phase_error = (b1 - a12 * b2) / determinant;
    reference->velocity = fmax(-limit, fmin(limit, (a11 * b2 - a21 * b1) / determinant));
}

static int speed_study_run(
    const struct speed_study *study,
    struct speed_error (*metrics)[SPEED_ESTIMATORS],
    float omega,
    bool trace
)
{
    const struct foc_virtual_current_profile profile = foc_virtual_current_default_profile();
    struct foc_encoder_config config = profile.encoder;
    config.counts_per_revolution = crawl_lut.counts_per_revolution;
    config.direction = study->direction;
    struct foc_virtual_motor motor;
    CHECK(foc_virtual_motor_init(&motor, &profile.motor, NULL) == 0);
    struct foc_encoder_context sensor;
    CHECK(
        (study->use_lut ? foc_encoder_init_with_lut(&sensor, &config, 19U, &crawl_lut)
                        : foc_encoder_init(&sensor, &config)) == 0
    );
    double phase = -0.073;
    uint32_t tick = UINT32_MAX - 4800U;
    uint32_t random = study->seed;
    uint16_t raw;
    motor.state.mechanical_position_rad = phase;
    CHECK(encoder_raw(&motor, &config, 0U, &random, &raw) == 0);
    struct foc_encoder_feedback observation;
    struct foc_sensor_pll_experiment pll = {0};
    struct pll_reference reference = {0};
    if (omega != 0.0f) {
        CHECK(foc_sensor_pll_experiment_init(&pll, omega, config.max_speed_rad_s));
    }
    CHECK(foc_encoder_update(&sensor, tick, raw, FOC_ENCODER_TRANSPORT_OK, &observation) == 0);
    sensor.position_counts += (int64_t)study->initial_turns * config.counts_per_revolution;
    static const int32_t capture_jitter[] = {0, -23, -4, -14, 10, 16};
    uint32_t global_us = 0U;
    for (unsigned i = 0U; i < study->length; ++i) {
        const struct speed_segment *segment = &study->segments[i];
        const double accel =
            (segment->last_speed - segment->first_speed) * 1.0e6 / segment->duration_us;
        uint32_t local_us = 0U;
        for (uint32_t k = 1U; k <= segment->duration_us / 500U; ++k) {
            const uint32_t nominal = k * 500U;
            const int32_t shift =
                study->variable_dt && nominal != segment->duration_us ? capture_jitter[k % 6U] : 0;
            const uint32_t target = (uint32_t)((int32_t)nominal + shift);
            const uint32_t dt = target - local_us;
            CHECK(dt >= 477U && dt <= 524U);
            local_us = target;
            global_us += dt;
            tick += dt;
            const double seconds = local_us * 1.0e-6;
            const double truth = segment->first_speed + accel * seconds;
            motor.state.mechanical_position_rad =
                phase + segment->first_speed * seconds + 0.5 * accel * seconds * seconds;
            CHECK(encoder_raw(&motor, &config, segment->noise_codes, &random, &raw) == 0);
            CHECK(
                foc_encoder_update(&sensor, tick, raw, FOC_ENCODER_TRANSPORT_OK, &observation) == 0
            );
            CHECK(observation.valid && observation.captured_us == tick);
            const int64_t cycle = (sensor.position_counts % config.counts_per_revolution +
                                   config.counts_per_revolution) %
                config.counts_per_revolution;
            CHECK(observation.mechanical_phase_count == (uint32_t)cycle);
            if (omega != 0.0f) {
                CHECK(foc_sensor_pll_experiment_step(&pll, &observation));
                pll_reference_step(&reference, &observation, omega, config.max_speed_rad_s);
                CHECK(
                    fabs(pll.velocity_rad_s - reference.velocity) <=
                    0.00002 + 3e-6 * fabs(reference.velocity)
                );
                CHECK(fabs(pll.phase_error_rad - reference.phase_error) <= 0.000002);
            }
            const double rates[] = {
                observation.raw_velocity_rad_s,
                observation.filtered_velocity_rad_s,
                observation.tracked_velocity_rad_s,
                pll.velocity_rad_s,
            };
            const unsigned estimators = omega != 0.0f ? SPEED_ESTIMATORS : PLL;
            for (unsigned k = 0U; k < estimators; ++k) {
                struct speed_error *m = &metrics[i][k];
                if (accel != 0.0 || local_us >= 25000U) {
                    speed_error_add(m, rates[k] - truth);
                }
                if (segment->step != 0.0) {
                    const bool inside = fabs(rates[k] - truth) <= 0.1 * fabs(segment->step);
                    if (inside && !m->inside) {
                        m->inside_since = local_us;
                    }
                    m->inside = inside;
                }
            }
            if (trace) {
                printf(
                    "INPUT,%s,%s,%u,%u,%u,%u,%lld,%.12g,%.9g,%.9g,%.9g,%.9g\n",
                    study->name,
                    segment->name,
                    global_us,
                    tick,
                    raw,
                    observation.corrected_code,
                    (long long)sensor.position_counts,
                    truth,
                    rates[RAW],
                    rates[LPF],
                    rates[ALPHA_BETA],
                    rates[PLL]
                );
            }
        }
        phase = motor.state.mechanical_position_rad;
    }
    return 0;
}

static void speed_print(
    const struct speed_study *study,
    struct speed_error (*metrics)[SPEED_ESTIMATORS],
    unsigned first,
    unsigned estimators,
    float omega
)
{
    for (unsigned i = 0U; i < study->length; ++i) {
        for (unsigned k = first; k < estimators; ++k) {
            const struct speed_error *m = &metrics[i][k];
            const uint32_t settling =
                m->inside && study->segments[i].duration_us - m->inside_since >= 5000U
                ? m->inside_since
                : 0U;
            printf(
                "SPEED,%s,%s,%u,%.9g,%u,%.9g,%.9g,%.9g,%u\n",
                study->name,
                study->segments[i].name,
                k,
                (double)omega,
                m->count,
                speed_rms(m),
                m->peak,
                m->signed_sum / m->count,
                settling
            );
        }
    }
}

static uint32_t settled(const struct speed_segment *segment, const struct speed_error *error)
{
    return error->inside && segment->duration_us - error->inside_since >= 5000U
        ? error->inside_since
        : 0U;
}

static bool comparable_t90(uint32_t baseline, uint32_t candidate)
{
    return baseline != 0U && candidate != 0U && 5U * candidate >= 4U * baseline &&
        4U * candidate <= 5U * baseline;
}

static int speed_select(const struct speed_study *train, float *selected)
{
    static const float omega_grid[] = {600.0f, 800.0f, 1000.0f, 1200.0f};
    double best = HUGE_VAL;
    *selected = 0.0f;
    for (unsigned i = 0U; i < sizeof omega_grid / sizeof omega_grid[0]; ++i) {
        struct speed_error fit[4][SPEED_ESTIMATORS] = {0};
        const float omega = omega_grid[i];
        CHECK(speed_study_run(train, fit, omega, false) == 0);
        speed_print(train, fit, i == 0U ? RAW : PLL, SPEED_ESTIMATORS, omega);
        const bool fair = comparable_t90(
                              settled(&train->segments[1], &fit[1][LPF]),
                              settled(&train->segments[1], &fit[1][PLL])
                          ) &&
            comparable_t90(settled(&train->segments[2], &fit[2][LPF]),
                           settled(&train->segments[2], &fit[2][PLL])) &&
            speed_rms(&fit[0][PLL]) <= 1.10 * speed_rms(&fit[0][LPF]);
        const double crawl = speed_rms(&fit[3][PLL]);
        printf("CANDIDATE,%.9g,%u,%.9g\n", (double)omega, (unsigned)fair, crawl);
        if (fair && crawl < best) {
            best = crawl;
            *selected = omega;
        }
    }
    CHECK(*selected > 0.0f);
    printf("SELECTED,%.9g\n", (double)*selected);
    return 0;
}

static bool
speed_holdout_gate(const struct speed_study *study, struct speed_error (*hold)[SPEED_ESTIMATORS])
{
    const bool quiet = speed_rms(&hold[0][PLL]) <= 1.10 * speed_rms(&hold[0][LPF]);
    const bool crawl = speed_rms(&hold[1][PLL]) <= 0.85 * speed_rms(&hold[1][LPF]);
    const bool accel = speed_rms(&hold[2][PLL]) <= 1.15 * speed_rms(&hold[2][LPF]);
    const bool decel = speed_rms(&hold[5][PLL]) <= 1.15 * speed_rms(&hold[5][LPF]);
    bool dynamics = true;
    for (unsigned i = 3U; i <= 4U; ++i) {
        const uint32_t base = settled(&study->segments[i], &hold[i][LPF]);
        const uint32_t trial = settled(&study->segments[i], &hold[i][PLL]);
        dynamics &= base != 0U && trial != 0U && 4U * trial <= 5U * base;
    }
    const bool fast = speed_rms(&hold[8][PLL]) <= 1.25 * speed_rms(&hold[8][LPF]) &&
        speed_rms(&hold[9][PLL]) <= 1.25 * speed_rms(&hold[9][LPF]);
    printf(
        "GATE,speed,quiet=%u,crawl=%u,accel=%u,decel=%u,step_reverse=%u,fast=%u\n",
        (unsigned)quiet,
        (unsigned)crawl,
        (unsigned)accel,
        (unsigned)decel,
        (unsigned)dynamics,
        (unsigned)fast
    );
    return quiet && crawl && accel && decel && dynamics && fast;
}

static int pll_rejection_contract(float omega)
{
    struct foc_encoder_config config = foc_virtual_current_default_profile().encoder;
    config.counts_per_revolution = 16384U;
    struct foc_encoder_context sensor;
    struct foc_sensor_pll_experiment pll;
    CHECK(foc_encoder_init_with_lut(&sensor, &config, 19U, &crawl_lut) == 0);
    CHECK(foc_sensor_pll_experiment_init(&pll, omega, config.max_speed_rad_s));
    CHECK(!foc_sensor_pll_experiment_init(&pll, NAN, config.max_speed_rad_s));
    CHECK(!foc_sensor_pll_experiment_init(&pll, 10.0f, config.max_speed_rad_s));
    struct foc_encoder_feedback sample;
    const uint32_t begin = UINT32_MAX - 250U;
    CHECK(foc_encoder_update(&sensor, begin, 16383U, FOC_ENCODER_TRANSPORT_OK, &sample) == 0);
    CHECK(!foc_sensor_pll_experiment_step(&pll, &sample) && !pll.seeded);
    CHECK(foc_encoder_update(&sensor, begin + 500U, 0U, FOC_ENCODER_TRANSPORT_OK, &sample) == 0);
    CHECK(foc_sensor_pll_experiment_step(&pll, &sample));
    const struct foc_sensor_pll_experiment prior = pll;
    struct foc_encoder_feedback invalid = sample;
    invalid.dt_s = NAN;
    CHECK(!foc_sensor_pll_experiment_step(&pll, &invalid));
    invalid = sample;
    invalid.raw_velocity_rad_s = INFINITY;
    CHECK(!foc_sensor_pll_experiment_step(&pll, &invalid));
    CHECK(
        foc_encoder_update(&sensor, begin + 500U, 8U, FOC_ENCODER_TRANSPORT_OK, &sample) ==
        FOC_ENCODER_STATUS_ERR_ORDER
    );
    CHECK(!foc_sensor_pll_experiment_step(&pll, &sample));
    CHECK(
        foc_encoder_update(&sensor, begin + 1000U, 2U, FOC_ENCODER_TRANSPORT_TIMEOUT, &sample) ==
        FOC_ENCODER_STATUS_ERR_TRANSPORT
    );
    CHECK(!foc_sensor_pll_experiment_step(&pll, &sample));
    CHECK(
        pll.velocity_rad_s == prior.velocity_rad_s && pll.phase_error_rad == prior.phase_error_rad
    );
    CHECK(foc_encoder_update(&sensor, begin + 1500U, 3U, FOC_ENCODER_TRANSPORT_OK, &sample) == 0);
    CHECK(foc_sensor_pll_experiment_step(&pll, &sample));
    const float recovered = pll.velocity_rad_s;
    CHECK(sample.dt_s == 0.001f && pll.velocity_rad_s > 0.0f);
    CHECK(
        foc_encoder_update(&sensor, begin + 1100U, 5U, FOC_ENCODER_TRANSPORT_OK, &sample) ==
        FOC_ENCODER_STATUS_ERR_ORDER
    );
    CHECK(sensor.continuity_lost && !foc_sensor_pll_experiment_step(&pll, &sample));
    CHECK(
        foc_encoder_update(&sensor, begin + 2000U, 8U, FOC_ENCODER_TRANSPORT_OK, &sample) ==
        FOC_ENCODER_STATUS_ERR_DISCONTINUITY
    );
    CHECK(!foc_sensor_pll_experiment_step(&pll, &sample) && pll.velocity_rad_s == recovered);
    CHECK(foc_encoder_reset(&sensor) == 0);
    CHECK(foc_sensor_pll_experiment_init(&pll, omega, config.max_speed_rad_s));
    CHECK(foc_encoder_update(&sensor, 0U, 1000U, FOC_ENCODER_TRANSPORT_OK, &sample) == 0);
    CHECK(!foc_sensor_pll_experiment_step(&pll, &sample) && !pll.seeded);
    CHECK(foc_encoder_update(&sensor, 500U, 999U, FOC_ENCODER_TRANSPORT_OK, &sample) == 0);
    CHECK(foc_sensor_pll_experiment_step(&pll, &sample) && pll.velocity_rad_s < 0.0f);
    /* Sensor-valid data can violate the experiment's timing policy. A later
     * increment cannot silently resume from the old accepted endpoint. */
    CHECK(foc_encoder_reset(&sensor) == 0);
    CHECK(foc_sensor_pll_experiment_init(&pll, 2000.0f, config.max_speed_rad_s));
    CHECK(foc_encoder_update(&sensor, 0U, 1000U, FOC_ENCODER_TRANSPORT_OK, &sample) == 0);
    CHECK(!foc_sensor_pll_experiment_step(&pll, &sample));
    CHECK(foc_encoder_update(&sensor, 500U, 1001U, FOC_ENCODER_TRANSPORT_OK, &sample) == 0);
    CHECK(foc_sensor_pll_experiment_step(&pll, &sample));
    const float last_rate = pll.velocity_rad_s;
    CHECK(foc_encoder_update(&sensor, 2500U, 1007U, FOC_ENCODER_TRANSPORT_OK, &sample) == 0);
    CHECK(sample.valid && !foc_sensor_pll_experiment_step(&pll, &sample));
    CHECK(foc_encoder_update(&sensor, 3000U, 1008U, FOC_ENCODER_TRANSPORT_OK, &sample) == 0);
    CHECK(sample.valid && !foc_sensor_pll_experiment_step(&pll, &sample));
    CHECK(pll.velocity_rad_s == last_rate);
    CHECK(foc_sensor_pll_experiment_init(&pll, 2000.0f, config.max_speed_rad_s));
    CHECK(foc_sensor_pll_experiment_step(&pll, &sample));
    printf(
        "FAULTS,wrap=1,duplicate_hold=1,timeout_hold=1,recovered_interval_us=1000,"
        "recovery_after_drop_us=500,stale_latched=1,reset_reseed_us=500,endpoint_gap_reset=1\n"
    );
    return 0;
}

/* Isolated Host consumers ------------------------------------------------------ */

enum chain_arm {
    CHAIN_RAW_LPF,
    CHAIN_LUT_LPF,
    CHAIN_LUT_AB,
    CHAIN_LUT_PLL,
    CHAIN_ARMS
};

struct chain_stats {
    struct speed_error tracking;
    struct speed_error early_tracking;
    struct speed_error feedback;
    struct speed_error angle;
    struct speed_error prediction_speed;
    struct speed_error d;
    struct speed_error q;
    uint32_t updates;
    uint32_t saturated;
    uint32_t pending_recovery_us;
    bool recovered;
};

static double chain_short(double angle)
{
    return remainder(angle, tau);
}

/* Use the same real scheduler/Plant for every arm. Integrate to the boundary
 * first, delaying only due-at-this-time H0/foreground calls until after the
 * test supplies H1. All actual Runtime timestamps and periods are preserved. */
static int chain_integrate(struct foc_virtual_current *vm, uint32_t time)
{
    const uint32_t h0 = vm->next_h0_us;
    const uint32_t fg = vm->next_foreground_us;
    if (h0 == time) {
        vm->next_h0_us = UINT32_MAX;
    }
    if (fg == time) {
        vm->next_foreground_us = UINT32_MAX;
    }
    CHECK(foc_virtual_current_run_until(vm, time) == 0);
    if (h0 == time) {
        vm->next_h0_us = h0;
    }
    if (fg == time) {
        vm->next_foreground_us = fg;
    }
    return 0;
}

static int chain_h1(
    struct foc_virtual_current *vm,
    struct foc_sensor_pll_experiment *pll,
    enum chain_arm arm,
    bool injected
)
{
    uint16_t code;
    uint32_t unused = 0U;
    CHECK(encoder_raw(&vm->motor, &vm->profile.encoder, 0U, &unused, &code) == 0);
    struct foc_encoder_feedback feedback;
    const int result = foc_encoder_update(
        &vm->encoder,
        vm->virtual_time_us,
        code,
        injected ? FOC_ENCODER_TRANSPORT_FRAME_ERROR : FOC_ENCODER_TRANSPORT_OK,
        &feedback
    );
    CHECK(result == (injected ? FOC_ENCODER_STATUS_ERR_TRANSPORT : FOC_ENCODER_STATUS_OK));
    const bool estimate = arm != CHAIN_LUT_PLL || foc_sensor_pll_experiment_step(pll, &feedback);
    float phase = 0.0f;
    const bool phase_valid = feedback.valid && estimate &&
        foc_rotor_phase_from_counts(
            &vm->profile.runtime.motion.rotor,
            feedback.mechanical_phase_count,
            vm->profile.encoder.counts_per_revolution,
            &phase
        ) == 0;
    const struct foc_runtime_sensor sample = {
        .theta_elec_rad = phase,
        .captured_us = feedback.captured_us,
        .seq = ++vm->encoder_seq,
        .valid = phase_valid,
        .mechanical_rad = feedback.mechanical_rad,
        .mechanical_rad_s = !phase_valid ? 0.0f
            : arm == CHAIN_LUT_PLL       ? pll->velocity_rad_s
            : arm == CHAIN_LUT_AB        ? feedback.tracked_velocity_rad_s
                                         : feedback.filtered_velocity_rad_s,
        .electrical_phase_valid = phase_valid,
    };
    const int status = foc_runtime_h1_step(&vm->runtime, &sample, vm->virtual_time_us, 0U);
    CHECK(
        status == (injected || vm->virtual_time_us == 0U ? FOC_RUNTIME_ERR_INPUT : FOC_RUNTIME_OK)
    );
    CHECK(phase_valid == (!injected && vm->virtual_time_us != 0U));
    return 0;
}

static int chain_angle_measure(
    const struct foc_virtual_current *vm,
    double capture_velocity,
    struct chain_stats *stats
)
{
    const struct foc_runtime_config *policy = &vm->runtime.config;
    const unsigned index = atomic_load(&vm->runtime.sensor_slot);
    CHECK(index < 2U);
    const struct foc_runtime_sensor_slot *slot = &vm->runtime.sensor_slots[index];
    const uint32_t age = vm->virtual_time_us - slot->sample.captured_us;
    CHECK(slot->sample.valid && age == vm->runtime.angle_prediction_us);
    float phase;
    CHECK(foc_rotor_predict(&policy->motion.prediction, &slot->electrical, age, &phase) == 0);
    struct foc_electrical_state true_rate = slot->electrical;
    true_rate.velocity_rad_s = (float)(policy->motion.rotor.pole_pairs * capture_velocity);
    float phase_true_rate;
    CHECK(foc_rotor_predict(&policy->motion.prediction, &true_rate, age, &phase_true_rate) == 0);
    const double true_angle =
        vm->motor.state.mechanical_position_rad * vm->profile.motor.pole_pairs +
        vm->profile.motor.electrical_offset_rad;
    struct foc_device_adc_raw adc;
    struct foc_device_measurement sample;
    CHECK(
        foc_virtual_motor_make_adc(
            &vm->motor,
            &vm->profile.adc,
            vm->profile.vbus_v,
            vm->sample_seq,
            vm->virtual_time_us,
            &adc
        ) == 0
    );
    CHECK(foc_device_decode_sample(&adc, &vm->profile.adc, &sample) == 0);
    const double alpha = sample.phase_current_a.a;
    const double beta = (sample.phase_current_a.b - sample.phase_current_a.c) / sqrt(3.0);
    const struct foc_vector_dq measured = vm->runtime.pipeline.measured_dq_a;
    CHECK(fabs(measured.d - (cos(phase) * alpha + sin(phase) * beta)) < 2e-6);
    CHECK(fabs(measured.q - (cos(phase) * beta - sin(phase) * alpha)) < 2e-6);
    speed_error_add(&stats->angle, chain_short(phase - true_angle));
    speed_error_add(&stats->prediction_speed, chain_short(phase - phase_true_rate));
    speed_error_add(&stats->d, measured.d - (cos(true_angle) * alpha + sin(true_angle) * beta));
    speed_error_add(&stats->q, measured.q - (cos(true_angle) * beta - sin(true_angle) * alpha));
    speed_error_add(&stats->feedback, slot->sample.mechanical_rad_s - capture_velocity);
    return 0;
}

static float chain_target(bool position, uint32_t t)
{
    if (t < 40000U) {
        return position ? 0.08f : 0.9f;
    }
    if (t < 90000U) {
        return position ? 2.0f : 20.0f;
    }
    if (t < 145000U) {
        return position ? -0.17f : -2.0f;
    }
    return position ? -0.025f : 0.35f;
}

static int
chain_trial(bool position, enum chain_arm arm, float omega, struct chain_stats *stats, bool trace)
{
    const enum foc_runtime_mode mode =
        position ? FOC_RUNTIME_MODE_POSITION : FOC_RUNTIME_MODE_VELOCITY;
    const char *name = position ? "position" : "velocity";
    struct foc_virtual_current_profile profile = foc_virtual_current_default_profile();
    profile.encoder.counts_per_revolution = 16384U;
    profile.current.pwm.voltage_limit_v = 1.2f;
    profile.runtime.motion.prediction.max_prediction_us = 650U;
    profile.runtime.angle_timeout_us = 650U;
    profile.motor.electrical_offset_rad = profile.runtime.motion.rotor.electrical_offset_rad;
    struct foc_virtual_current vm;
    CHECK(foc_virtual_current_init(&vm, &profile) == 0);
    if (arm != CHAIN_RAW_LPF) {
        CHECK(foc_encoder_init_with_lut(&vm.encoder, &profile.encoder, 19U, &crawl_lut) == 0);
    }
    /* Restrict alternate speed sources to this test-only H1 producer. */
    vm.next_h1_us = UINT32_MAX;
    struct foc_sensor_pll_experiment pll;
    CHECK(foc_sensor_pll_experiment_init(&pll, omega, profile.encoder.max_speed_rad_s));
    CHECK(foc_virtual_current_queue_set_mode(&vm, mode) == 0);
    double capture_velocity = 0.0;
    uint32_t before_boundary = 0U;
    for (uint32_t t = 0U; t <= 242000U; t += 100U) {
        CHECK(chain_integrate(&vm, t) == 0);
        const float target = chain_target(position, t);
        if (t == 200U || (t != 0U && t < 240000U && t % 1000U == 0U)) {
            CHECK(foc_virtual_current_publish_motion(&vm, mode, target) == 0);
        }
        if (t == 600U || t == 800U) {
            CHECK(
                foc_virtual_current_queue_action(
                    &vm,
                    t == 600U ? FOC_RUNTIME_ACTION_ARM : FOC_RUNTIME_ACTION_START
                ) == 0
            );
        }
        if (t == 110000U || t == 160000U) {
            CHECK(foc_virtual_motor_set_load(&vm.motor, t == 110000U ? 0.010 : 0.0) == 0);
        }
        if (t == 240000U) {
            before_boundary = vm.output.accepted_updates;
            if (!position) {
                CHECK(foc_virtual_current_request_stop(&vm) == 0);
            }
        }
        if (t % 500U == 0U) {
            capture_velocity = vm.motor.state.mechanical_velocity_rad_s;
            CHECK(chain_h1(&vm, &pll, arm, position && t == 240000U) == 0);
        }
        const uint32_t previous = vm.output.accepted_updates;
        const uint32_t previous_sat = vm.output.saturated_updates;
        CHECK(foc_virtual_current_run_until(&vm, t) == 0);
        if (t >= 1000U && t < 240000U && t % 200U == 0U) {
            CHECK(vm.output.accepted_updates == previous + 1U);
            CHECK(vm.runtime.pipeline.safety.state == FOC_SAFE_STATE_RUNNING);
            CHECK(chain_angle_measure(&vm, capture_velocity, stats) == 0);
            const double observed = position ? vm.motor.state.mechanical_position_rad
                                             : vm.motor.state.mechanical_velocity_rad_s;
            speed_error_add(&stats->tracking, observed - target);
            if (t < 40000U) {
                speed_error_add(&stats->early_tracking, observed - target);
            }
            if (t >= 90000U && !stats->recovered && vm.output.saturated_updates == previous_sat) {
                stats->pending_recovery_us = t - 90000U;
                stats->recovered = true;
            }
            if (trace) {
                printf(
                    "CONTROL,%s,%u,%u,%.9g,%.9g,%.9g,%.9g,%u,%u,%u,%u\n",
                    name,
                    (unsigned)arm,
                    t,
                    (double)target,
                    observed,
                    vm.motor.state.mechanical_velocity_rad_s,
                    vm.motor.state.mechanical_position_rad,
                    vm.runtime.angle_prediction_us,
                    vm.output.accepted_updates,
                    vm.output.saturated_updates,
                    vm.output.rejected_updates
                );
            }
        } else if (t >= 240000U) {
            CHECK(vm.output.accepted_updates == before_boundary);
            CHECK(vm.output.request.action == FOC_DEVICE_OUTPUT_INHIBIT);
        }
    }
    stats->updates = vm.output.accepted_updates;
    stats->saturated = vm.output.saturated_updates;
    const enum foc_safe_state expected =
        position ? FOC_SAFE_STATE_FAULT_LATCHED : FOC_SAFE_STATE_DISABLED;
    CHECK(vm.runtime.pipeline.safety.state == expected);
    CHECK((vm.runtime.pipeline.safety.latched_faults != 0U) == position);
    CHECK(vm.output.rejected_updates == 0U);
    printf(
        "CHAIN,%s,%u,%u,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%u,%u,%u,%u,%u\n",
        name,
        (unsigned)arm,
        stats->tracking.count,
        speed_rms(&stats->tracking),
        speed_rms(&stats->early_tracking),
        speed_rms(&stats->feedback),
        speed_rms(&stats->angle),
        stats->angle.peak,
        speed_rms(&stats->prediction_speed),
        speed_rms(&stats->d),
        speed_rms(&stats->q),
        stats->updates,
        stats->saturated,
        stats->pending_recovery_us,
        (unsigned)vm.runtime.pipeline.safety.state,
        (unsigned)vm.runtime.pipeline.safety.latched_faults
    );
    return 0;
}

static int chain_compare(float omega, bool trace, bool *gate)
{
    *gate = true;
    for (unsigned mode = 0U; mode < 2U; ++mode) {
        struct chain_stats stats[CHAIN_ARMS] = {0};
        for (unsigned arm = 0U; arm < CHAIN_ARMS; ++arm) {
            CHECK(chain_trial(mode != 0U, (enum chain_arm)arm, omega, &stats[arm], trace) == 0);
        }
        const struct chain_stats *b = &stats[CHAIN_LUT_LPF];
        const struct chain_stats *d = &stats[CHAIN_LUT_PLL];
        const bool tracking = speed_rms(&d->tracking) <= 1.05 * speed_rms(&b->tracking);
        const bool angle_dq = speed_rms(&d->angle) <= 1.05 * speed_rms(&b->angle) &&
            d->d.square + d->q.square <= 1.05 * 1.05 * (b->d.square + b->q.square);
        const bool safety = d->updates == b->updates;
        printf(
            "GATE,chain,%u,tracking=%u,angle_dq=%u,safety=%u\n",
            mode,
            (unsigned)tracking,
            (unsigned)angle_dq,
            (unsigned)safety
        );
        *gate = *gate && tracking && angle_dq && safety;
    }
    return 0;
}

/* Test Entry ------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    const bool baseline = argc == 2 && strcmp(argv[1], "--baseline") == 0;
    const bool trace = argc == 2 && strcmp(argv[1], "--trace") == 0;
    const bool speed_only = argc == 2 && strcmp(argv[1], "--speed-study") == 0;
    CHECK(argc == 1 || baseline || trace || speed_only);
    const struct speed_study train =
        {"train", training_segments, 4U, false, true, FOC_ENCODER_DIRECTION_NORMAL, 0, 718U};
    const struct speed_study holdout = {
        "holdout-lut",
        holdout_segments,
        10U,
        true,
        true,
        FOC_ENCODER_DIRECTION_REVERSED,
        0,
        2257U
    };
    const struct speed_study long_turn = {
        "long-positive",
        long_segments,
        3U,
        true,
        true,
        FOC_ENCODER_DIRECTION_NORMAL,
        1000000,
        718U
    };
    struct speed_error hold[10][SPEED_ESTIMATORS] = {0};
    struct speed_error run[3][SPEED_ESTIMATORS] = {0};
    if (baseline) {
        struct speed_error training[4][SPEED_ESTIMATORS] = {0};
        CHECK(speed_study_run(&train, training, 0.0f, false) == 0);
        speed_print(&train, training, RAW, PLL, 0.0f);
        CHECK(speed_study_run(&holdout, hold, 0.0f, false) == 0);
        speed_print(&holdout, hold, RAW, PLL, 0.0f);
        CHECK(speed_study_run(&long_turn, run, 0.0f, false) == 0);
        speed_print(&long_turn, run, RAW, PLL, 0.0f);
        return 0;
    }
    float omega;
    CHECK(speed_select(&train, &omega) == 0);
    CHECK(speed_study_run(&holdout, hold, omega, trace) == 0);
    speed_print(&holdout, hold, RAW, SPEED_ESTIMATORS, omega);
    const bool speed_ok = speed_holdout_gate(&holdout, hold);
    struct speed_study disabled = holdout;
    disabled.use_lut = false;
    disabled.name = "holdout-no-lut";
    memset(hold, 0, sizeof hold);
    CHECK(speed_study_run(&disabled, hold, omega, trace) == 0);
    speed_print(&disabled, hold, RAW, SPEED_ESTIMATORS, omega);

    struct speed_error near[3][SPEED_ESTIMATORS] = {0};
    struct speed_study near_turn = long_turn;
    near_turn.name = "long-near";
    near_turn.initial_turns = 0;
    CHECK(speed_study_run(&near_turn, near, omega, trace) == 0);
    speed_print(&near_turn, near, RAW, SPEED_ESTIMATORS, omega);
    for (int direction = -1; direction <= 1; direction += 2) {
        struct speed_study seeded = long_turn;
        seeded.name = direction < 0 ? "long-negative" : "long-positive";
        seeded.initial_turns = direction * 1000000;
        memset(run, 0, sizeof run);
        CHECK(speed_study_run(&seeded, run, omega, trace) == 0);
        speed_print(&seeded, run, RAW, SPEED_ESTIMATORS, omega);
        for (unsigned i = 0U; i < 3U; ++i) {
            CHECK(run[i][RAW].square == near[i][RAW].square);
            CHECK(run[i][LPF].square == near[i][LPF].square);
            CHECK(run[i][PLL].square == near[i][PLL].square);
        }
    }
    CHECK(pll_rejection_contract(omega) == 0);
    printf("GATE,sensor_only,%u\n", (unsigned)speed_ok);
    if (!speed_only) {
        bool chain_ok;
        CHECK(chain_compare(omega, trace, &chain_ok) == 0);
        printf(
            "DECISION,%s\n",
            speed_ok && chain_ok ? "RESOURCE_GATE_PENDING" : "KEEP_EXISTING_ESTIMATORS"
        );
    }
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
