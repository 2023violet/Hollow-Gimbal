/**
 * @file    test_foc_motion_model.c
 * @brief   Compare-driven electrical and mechanical PMSM closed-loop fixture.
 * @version v0.1.0
 * @date    2026.11.10
 *
 * Test role
 * ---------
 * Exercise the actual cascade with quantified dynamic gates and replay.
 * Machine parameters are synthetic and the plant only accepts Mock UPDATE.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_current.h"
#include "foc_device_sim.h"
#include "foc_motion.h"
#include "foc_runtime.h"

#include <math.h>
#include <stdio.h>

/* Private Macros --------------------------------------------------------------- */

#define TEST_ASSERT(condition)                                                                  \
    do {                                                                                        \
        if (!(condition)) {                                                                     \
            fprintf(stderr, "assertion failed: %s at %s:%d\n", #condition, __FILE__, __LINE__); \
            return 1;                                                                           \
        }                                                                                       \
    } while (0)

/* Private Types ---------------------------------------------------------------- */

struct plant_config {
    uint16_t poles;
    double rs;
    double ld;
    double lq;
    double flux;
    double inertia;
    double damping;
    double offset;
};

struct plant_state {
    double id;
    double iq;
    double speed;
    double position;
};

struct case_stage {
    double ref;
    double load_nm;
    unsigned int ticks;
    double tail_limit;
    double rise_limit_s;
    double settling_limit_s;
    double overshoot_limit;
    double disturbance_limit;
};

struct stage_result {
    double position;
    double speed;
    double iq;
    double iq_ref;
    double rise_s;
    double overshoot;
    double settle_s;
    double tail_error;
    double disturbance;
    double peak_iq_ref;
    double peak_v;
    unsigned int voltage_limited;
};

struct model_replay {
    struct plant_state state;
    struct foc_vector_dq inner_integral;
    float outer_integral;
    uint32_t hash;
    uint32_t faults;
};

/* Private Constants ------------------------------------------------------------ */

static const double step_s = 0.0001;
static const double bus_v = 12.0;
static const double sqrt3 = 1.73205080756887729353;
static const struct plant_config motor = {4U, 0.6, 0.0012, 0.0015, 0.025, 0.002, 0.006, 0.37};
static const struct foc_current_config current_config =
    {{1.0f, 2.4f, 0.0f, 1.0f, 0.0003f}, {1.08f, 1.35f}, {540.0f, 540.0f}, 2.5f};
static const struct foc_velocity_config velocity_config = {0.18f, 1.4f, 0.002f};
static const struct foc_position_config position_config = {4.0f, 12.0f};
static const struct foc_device_pwm_config pwm_config = {6000U, FOC_DEVICE_PHASE_ORDER_ABC};
static const struct foc_device_adc_config adc_config =
    {0.01f, 0.002f, 1.0f, 2048.0f, 2048.0f, 2048.0f, FOC_DEVICE_PHASE_ORDER_ABC};
static const struct foc_safe_config safety_config = {3500U, 500U, 400U, 1500U, 3U};

/* Gates fixed before execution. Electrical min tau=2 ms, speed poles near
 * 10 rad/s, position bandwidth 4 rad/s, speed capped at 12 rad/s. */
static const struct case_stage speed_cases[] = {
    {10.0, 0.00, 14000U, 0.15, 0.65, 1.10, 2.40, 0.00},
    {10.0, 0.09, 12000U, 0.15, 0.00, 0.95, 0.00, 2.00},
    {10.0, 0.00, 12000U, 0.15, 0.00, 0.95, 0.00, 2.00},
    {-10.0, 0.00, 16000U, 0.15, 0.65, 1.10, 2.40, 0.00},
};
static const struct case_stage position_cases[] = {
    {7.0, 0.00, 26000U, 0.035, 1.40, 2.10, 1.00, 0.00},
    {-3.0, 0.00, 26000U, 0.035, 1.55, 2.10, 1.00, 0.00},
};

/* Private Helpers -------------------------------------------------------------- */

static double plant_angle(const struct plant_config *config, struct plant_state state)
{
    return config->poles * state.position + config->offset;
}

static struct plant_state plant_derivative(
    const struct plant_config *config,
    struct plant_state state,
    double alpha_v,
    double beta_v,
    double load_nm
)
{
    const double theta = plant_angle(config, state);
    const double vd = cos(theta) * alpha_v + sin(theta) * beta_v;
    const double vq = cos(theta) * beta_v - sin(theta) * alpha_v;
    const double we = config->poles * state.speed;
    const double torque = 1.5 * config->poles *
        (config->flux * state.iq + (config->ld - config->lq) * state.id * state.iq);
    return (struct plant_state){
        (vd - config->rs * state.id + we * config->lq * state.iq) / config->ld,
        (vq - config->rs * state.iq - we * (config->ld * state.id + config->flux)) / config->lq,
        (torque - config->damping * state.speed - load_nm) / config->inertia,
        state.speed,
    };
}

static struct plant_state
plant_add(struct plant_state state, struct plant_state derivative, double dt)
{
    return (
        struct plant_state
    ){state.id + dt * derivative.id,
      state.iq + dt * derivative.iq,
      state.speed + dt * derivative.speed,
      state.position + dt * derivative.position};
}

static double plant_apply(
    const struct plant_config *config,
    struct plant_state *state,
    const uint32_t compare[3],
    double load_nm
)
{
    /* ABC mapping is CH3/2/1. Pole voltages share an unconnected neutral. */
    const double a = (double)compare[2] / pwm_config.period_counts;
    const double b = (double)compare[1] / pwm_config.period_counts;
    const double c = (double)compare[0] / pwm_config.period_counts;
    const double alpha = bus_v * (2.0 * a - b - c) / 3.0;
    const double beta = bus_v * (b - c) / sqrt3;
    const struct plant_state k1 = plant_derivative(config, *state, alpha, beta, load_nm);
    const struct plant_state k2 =
        plant_derivative(config, plant_add(*state, k1, step_s / 2.0), alpha, beta, load_nm);
    const struct plant_state k3 =
        plant_derivative(config, plant_add(*state, k2, step_s / 2.0), alpha, beta, load_nm);
    const struct plant_state k4 =
        plant_derivative(config, plant_add(*state, k3, step_s), alpha, beta, load_nm);
    const struct plant_state sum = plant_add(plant_add(plant_add(k1, k2, 2.0), k3, 2.0), k4, 1.0);
    *state = plant_add(*state, sum, step_s / 6.0);
    return hypot(alpha, beta);
}

static struct foc_device_adc_raw plant_adc(
    const struct plant_config *config,
    struct plant_state state,
    uint32_t seq,
    uint32_t tick_us
)
{
    const double theta = plant_angle(config, state);
    const double alpha = cos(theta) * state.id - sin(theta) * state.iq;
    const double beta = sin(theta) * state.id + cos(theta) * state.iq;
    return (struct foc_device_adc_raw){
        .vbus_raw = 1200U,
        .rank2_raw = (uint16_t)lround(2048.0 + alpha * 500.0),
        .rank3_raw = (uint16_t)lround(2048.0 + (-alpha + sqrt3 * beta) * 250.0),
        .rank4_raw = (uint16_t)lround(2048.0 + (-alpha - sqrt3 * beta) * 250.0),
        .valid_mask = FOC_DEVICE_SAMPLE_ALL_VALID,
        .sample_seq = seq,
        .sample_tick = tick_us,
    };
}

static int test_plant(void)
{
    const struct plant_state state = {0.3, -0.8, 9.0, 0.47};
    const double load = 0.027;
    const double alpha = 1.2;
    const double beta = -0.4;
    const double theta = plant_angle(&motor, state);
    const double ia = cos(theta) * state.id - sin(theta) * state.iq;
    const double ib = sin(theta) * state.id + cos(theta) * state.iq;
    const double expected_power =
        1.5 * (alpha * ia + beta * ib - motor.rs * (state.id * state.id + state.iq * state.iq)) -
        motor.damping * state.speed * state.speed - load * state.speed;
    const struct plant_state d = plant_derivative(&motor, state, alpha, beta, load);
    const double energy_rate = 1.5 * (motor.ld * state.id * d.id + motor.lq * state.iq * d.iq) +
        motor.inertia * state.speed * d.speed;
    TEST_ASSERT(fabs(expected_power - energy_rate) < 1.0e-12);
    const uint32_t centered[3] = {3000U, 3000U, 3000U};
    struct plant_state rest = {0};
    TEST_ASSERT(plant_apply(&motor, &rest, centered, 0.0) == 0.0);
    TEST_ASSERT(rest.id == 0.0 && rest.iq == 0.0 && rest.speed == 0.0 && rest.position == 0.0);
    return 0;
}

static int direct_output(
    struct foc_current_context *current,
    const struct foc_device_adc_raw *raw,
    const struct foc_electrical_state *electrical,
    float iq,
    uint32_t compare[3],
    bool *limited
)
{
    struct foc_device_measurement measurement;
    struct foc_current_output output;
    struct foc_device_pwm_request request;
    struct foc_device_sim_registers registers;
    TEST_ASSERT(foc_device_decode_sample(raw, &adc_config, &measurement) == FOC_DEVICE_STATUS_OK);
    const struct foc_current_input input = {
        .phase_current_a = measurement.phase_current_a,
        .theta_elec_rad = electrical->angle_rad,
        .target_a = {0.0f, iq},
        .vbus_v = measurement.core.vbus_v,
        .dt_s = (float)step_s,
    };
    TEST_ASSERT(
        foc_current_step(current, &input, &output) == FOC_STATUS_OK && output.output_enable != 0U
    );
    const struct foc_output mapped = {.output_enable = output.output_enable, .svpwm = output.svpwm};
    TEST_ASSERT(foc_device_map_output(&mapped, &pwm_config, &request) == FOC_DEVICE_STATUS_OK);
    TEST_ASSERT(request.action == FOC_DEVICE_OUTPUT_UPDATE);
    TEST_ASSERT(
        foc_device_sim_submit(&registers, &request, pwm_config.period_counts) ==
        FOC_DEVICE_STATUS_OK
    );
    compare[0] = registers.channel1_compare;
    compare[1] = registers.channel2_compare;
    compare[2] = registers.channel3_compare;
    *limited = output.svpwm.saturated != 0U;
    return 0;
}

static struct foc_runtime_config
model_runtime_config(const struct plant_config *config, bool trajectory_enabled)
{
    return (struct foc_runtime_config){
        .h0 = {100U, 20U, 10U},
        .h1 = {500U, 20U, 10U},
        .supervision = {1000U, 20U, 10U},
        .telemetry = {10000U, 20U, 10U},
        .foreground_budget_us = 15U,
        .angle_timeout_us = 600U,
        .motion = {
            .enabled = true,
            .rotor = {config->poles, (float)config->offset},
            .prediction = {400U, 2000.0f, 0.0003f},
            .velocity = velocity_config,
            .position = position_config,
            .iq_limit_a = current_config.current_limit_a,
            .trajectory = {
                .enabled = trajectory_enabled,
                .max_velocity_rad_s = 12.0f,
                .max_accel_rad_s2 = 20.0f,
                .max_dt_s = 0.002f,
                .position_tolerance_rad = 0.01f,
            },
        },
    };
}

static int model_command(
    struct foc_runtime *runtime,
    enum foc_runtime_mode mode,
    float reference,
    uint32_t seq,
    uint32_t now_us
)
{
    const struct foc_runtime_command command = {mode, seq, now_us, {.motion_ref = reference}};
    return foc_runtime_publish_motion_command(runtime, &command, now_us);
}

static int model_sensor(struct foc_runtime *runtime, struct plant_state state, uint32_t tick)
{
    const struct foc_runtime_sensor sample = {
        .captured_us = tick * 100U,
        .seq = tick,
        .valid = true,
        .mechanical_rad = (float)state.position,
        .mechanical_rad_s = (float)state.speed,
    };
    return foc_runtime_h1_step(runtime, &sample, tick * 100U, 0U);
}

static int model_foreground(struct foc_runtime *runtime, uint32_t tick)
{
    struct foc_runtime_foreground_result foreground;
    const int status = foc_runtime_foreground_poll(runtime, tick * 100U + 2U, 0U, 0U, &foreground);
    TEST_ASSERT(status == FOC_RUNTIME_OK || status == FOC_RUNTIME_IDLE);
    return 0;
}

static int begin_model_runtime(
    struct foc_runtime *runtime,
    const struct plant_config *plant,
    enum foc_runtime_mode mode,
    float reference
)
{
    const struct foc_runtime_config config = model_runtime_config(plant, false);
    const struct foc_pipeline_config pipeline = {
        .current = current_config,
        .adc = adc_config,
        .pwm = pwm_config,
        .safety = safety_config,
    };
    const struct plant_state rest = {0};
    TEST_ASSERT(foc_runtime_init(runtime, &pipeline, &config, 0U) == FOC_RUNTIME_OK);
    for (uint32_t tick = 0U; tick <= 6U; ++tick) {
        struct foc_runtime_fast_input input = {
            .adc = plant_adc(plant, rest, tick, 100U * tick),
            .now_us = 100U * tick + 1U
        };
        if (tick == 0U) {
            input.action = FOC_RUNTIME_ACTION_SET_MODE;
            input.requested_mode = mode;
        } else if (tick == 5U) {
            TEST_ASSERT(model_command(runtime, mode, reference, 1U, 500U) == FOC_RUNTIME_OK);
            input.action = FOC_RUNTIME_ACTION_ARM;
        } else if (tick == 6U) {
            input.action = FOC_RUNTIME_ACTION_START;
        }
        if (tick % 5U == 0U) {
            TEST_ASSERT(model_sensor(runtime, rest, tick) == FOC_RUNTIME_OK);
        }
        TEST_ASSERT(foc_runtime_h0_step(runtime, &input) == FOC_RUNTIME_IDLE);
        TEST_ASSERT(runtime->pipeline.mock.updates == 0U);
        TEST_ASSERT(model_foreground(runtime, tick) == 0);
    }
    TEST_ASSERT(runtime->pipeline.safety.state == FOC_SAFE_STATE_RUNNING);
    return 0;
}

static int begin_trajectory_runtime(
    struct foc_runtime *runtime,
    const struct plant_config *plant,
    enum foc_runtime_mode mode,
    float reference
)
{
    const struct foc_runtime_config config = model_runtime_config(plant, true);
    const struct foc_pipeline_config pipeline = {
        .current = current_config,
        .adc = adc_config,
        .pwm = pwm_config,
        .safety = safety_config,
    };
    const struct plant_state rest = {0};
    TEST_ASSERT(foc_runtime_init(runtime, &pipeline, &config, 0U) == FOC_RUNTIME_OK);
    for (uint32_t tick = 0U; tick <= 6U; ++tick) {
        struct foc_runtime_fast_input input = {
            .adc = plant_adc(plant, rest, tick, 100U * tick),
            .now_us = 100U * tick + 1U
        };
        if (tick == 0U) {
            input.action = FOC_RUNTIME_ACTION_SET_MODE;
            input.requested_mode = mode;
        } else if (tick == 5U) {
            TEST_ASSERT(model_command(runtime, mode, reference, 1U, 500U) == FOC_RUNTIME_OK);
            input.action = FOC_RUNTIME_ACTION_ARM;
        } else if (tick == 6U) {
            input.action = FOC_RUNTIME_ACTION_START;
        }
        if (tick % 5U == 0U) {
            TEST_ASSERT(model_sensor(runtime, rest, tick) == FOC_RUNTIME_OK);
        }
        TEST_ASSERT(foc_runtime_h0_step(runtime, &input) == FOC_RUNTIME_IDLE);
        TEST_ASSERT(model_foreground(runtime, tick) == 0);
    }
    TEST_ASSERT(runtime->config.motion.trajectory.enabled);
    TEST_ASSERT(runtime->pipeline.safety.state == FOC_SAFE_STATE_RUNNING);
    return 0;
}

static int runtime_output(
    struct foc_runtime *runtime,
    const struct plant_config *plant,
    struct plant_state state,
    uint32_t tick,
    uint32_t compare[3],
    float *iq_ref
)
{
    const struct foc_runtime_fast_input input = {
        .adc = plant_adc(plant, state, tick, 100U * tick),
        .now_us = 100U * tick + 1U
    };
    if (tick % 5U == 0U) {
        TEST_ASSERT(model_sensor(runtime, state, tick) == FOC_RUNTIME_OK);
    }
    TEST_ASSERT(foc_runtime_h0_step(runtime, &input) == FOC_RUNTIME_OK);
    struct foc_runtime_snapshot snapshot;
    TEST_ASSERT(foc_runtime_read_snapshot(runtime, &snapshot) == FOC_RUNTIME_OK);
    TEST_ASSERT(snapshot.output_confirmed && snapshot.action == FOC_DEVICE_OUTPUT_UPDATE);
    TEST_ASSERT(snapshot.updates == tick - 6U && snapshot.angle_prediction_us == tick % 5U * 100U);
    const unsigned int slot = atomic_load(&runtime->sensor_slot);
    TEST_ASSERT(
        snapshot.accepted_outer_seen &&
        snapshot.accepted_outer_seq == runtime->sensor_slots[slot].sample.seq
    );
    TEST_ASSERT(snapshot.command_us == runtime->sensor_slots[slot].target.produced_us);
    TEST_ASSERT(runtime->sensor_slots[slot].target.target_a.d == 0.0f);
    *iq_ref = runtime->sensor_slots[slot].target.target_a.q;
    for (unsigned int channel = 0U; channel < 3U; ++channel) {
        compare[channel] = snapshot.compare[channel];
    }
    TEST_ASSERT(model_foreground(runtime, tick) == 0);
    return 0;
}

static int run_model(bool position, bool integrated, bool trace, struct model_replay *replay)
{
    struct foc_current_context current = {0};
    struct foc_velocity_context velocity = {0};
    struct plant_state state = {0};
    struct foc_runtime runtime = {0};
    const struct foc_rotor_config rotor = {motor.poles, (float)motor.offset};
    const struct case_stage *cases = position ? position_cases : speed_cases;
    const unsigned int count = position ? 2U : 4U;
    uint32_t tick = integrated ? 7U : 0U;
    uint32_t command_seq = 1U;
    uint32_t hash = UINT32_C(2166136261);
    const enum foc_runtime_mode mode =
        position ? FOC_RUNTIME_MODE_POSITION : FOC_RUNTIME_MODE_VELOCITY;
    float iq_ref = 0.0f;
    TEST_ASSERT(foc_current_init(&current, &current_config) == FOC_STATUS_OK);
    TEST_ASSERT(foc_velocity_init(&velocity, &velocity_config) == FOC_STATUS_OK);
    if (integrated) {
        TEST_ASSERT(begin_model_runtime(&runtime, &motor, mode, (float)cases[0].ref) == 0);
    }
    for (unsigned int stage = 0U; stage < count; ++stage) {
        const struct case_stage *test = &cases[stage];
        const double initial = position ? state.position : state.speed;
        const double direction = test->ref >= initial ? 1.0 : -1.0;
        double ten_s = -1.0;
        struct stage_result result = {.rise_s = -1.0};
        for (unsigned int index = 0U; index < test->ticks; ++index, ++tick) {
            if (!integrated && tick % 5U == 0U) {
                struct foc_position_output pos;
                struct foc_velocity_output vel;
                if (position) {
                    TEST_ASSERT(
                        foc_position_step(
                            &position_config,
                            (float)test->ref,
                            (float)state.position,
                            &pos
                        ) == FOC_STATUS_OK
                    );
                }
                const struct foc_velocity_input input = {
                    position ? pos.velocity_ref_rad_s : (float)test->ref,
                    (float)state.speed,
                    0.0005f,
                    current_config.current_limit_a,
                    true
                };
                TEST_ASSERT(foc_velocity_step(&velocity, &input, &vel) == FOC_STATUS_OK);
                iq_ref = vel.iq_ref_a;
            }
            uint32_t compare[3];
            bool limited = false;
            if (integrated) {
                if (index % 20U == 0U) {
                    TEST_ASSERT(
                        model_command(
                            &runtime,
                            mode,
                            (float)test->ref,
                            ++command_seq,
                            tick * 100U
                        ) == FOC_RUNTIME_OK
                    );
                }
                TEST_ASSERT(runtime_output(&runtime, &motor, state, tick, compare, &iq_ref) == 0);
            } else {
                struct foc_electrical_state electrical;
                const struct foc_device_adc_raw raw = plant_adc(&motor, state, tick, 100U * tick);
                TEST_ASSERT(
                    foc_rotor_electrical(
                        &rotor,
                        (float)state.position,
                        (float)state.speed,
                        &electrical
                    ) == FOC_STATUS_OK
                );
                TEST_ASSERT(
                    direct_output(&current, &raw, &electrical, iq_ref, compare, &limited) == 0
                );
            }
            for (unsigned int channel = 0U; channel < 3U; ++channel) {
                hash = (hash ^ compare[channel]) * UINT32_C(16777619);
            }
            const double previous_speed = state.speed;
            const double applied_v = plant_apply(&motor, &state, compare, test->load_nm);
            TEST_ASSERT(fabs(state.speed - previous_speed) * motor.poles / step_s <= 2000.0);
            result.peak_v = fmax(result.peak_v, applied_v);
            result.peak_iq_ref = fmax(result.peak_iq_ref, fabs(iq_ref));
            result.voltage_limited += integrated ? applied_v >= 2.395 : limited;
            const double actual = position ? state.position : state.speed;
            const double elapsed = (index + 1U) * step_s;
            const double error = fabs(actual - test->ref);
            const double fraction =
                direction * (actual - initial) / fmax(fabs(test->ref - initial), 1.0e-9);
            if (ten_s < 0.0 && fraction >= 0.1) {
                ten_s = elapsed;
            }
            if (result.rise_s < 0.0 && ten_s >= 0.0 && fraction >= 0.9) {
                result.rise_s = elapsed - ten_s;
            }
            result.overshoot = fmax(result.overshoot, direction * (actual - test->ref));
            result.disturbance = fmax(result.disturbance, error);
            if (error > (position ? 0.08 : 0.30)) {
                result.settle_s = elapsed;
            }
            if (index >= test->ticks - 1500U) {
                result.tail_error = fmax(result.tail_error, error);
            }
            if (trace && ((index + 1U) % 1000U == 0U || index == 0U)) {
                printf(
                    "trace,%s,%u,%.4f,%.3f,%.6f,%.6f,%.6f,%.6f,%.4f,%.6f\n",
                    position ? "position" : "speed",
                    stage,
                    elapsed,
                    test->ref,
                    state.position,
                    state.speed,
                    iq_ref,
                    state.iq,
                    test->load_nm,
                    applied_v
                );
            }
        }
        result.position = state.position;
        result.speed = state.speed;
        result.iq = state.iq;
        result.iq_ref = iq_ref;
        printf(
            "%s %s %u ref=%.3f pos=%.6f speed=%.6f iq_ref=%.6f iq=%.6f rise=%.4f settle=%.4f "
            "overshoot=%.6f tail=%.6f peak_error=%.6f peak_iq=%.6f peak_v=%.6f limited=%u\n",
            integrated ? "runtime" : "direct",
            position ? "position" : "speed",
            stage,
            test->ref,
            result.position,
            result.speed,
            result.iq_ref,
            result.iq,
            result.rise_s,
            result.settle_s,
            result.overshoot,
            result.tail_error,
            result.disturbance,
            result.peak_iq_ref,
            result.peak_v,
            result.voltage_limited
        );
        TEST_ASSERT(
            result.tail_error <= test->tail_limit && result.settle_s <= test->settling_limit_s
        );
        if (test->rise_limit_s > 0.0) {
            TEST_ASSERT(result.rise_s >= 0.0 && result.rise_s <= test->rise_limit_s);
            TEST_ASSERT(result.overshoot <= test->overshoot_limit);
        } else {
            TEST_ASSERT(result.disturbance <= test->disturbance_limit);
        }
        TEST_ASSERT(result.peak_v <= 2.404 && result.peak_iq_ref <= 2.500001);
        if (stage == 0U || (!position && stage == 3U)) {
            TEST_ASSERT(result.voltage_limited > 0U);
        }
    }
    if (integrated) {
        const struct foc_vector_dq integral = runtime.pipeline.current.integral_v;
        const float outer_integral = runtime.outer_velocity.integral_a;
        const struct foc_runtime_fast_input last = {.now_us = tick * 100U + 1U};
        foc_runtime_request_inhibit(&runtime, FOC_SAFE_FAULT_WDT);
        TEST_ASSERT(foc_runtime_h0_step(&runtime, &last) == FOC_RUNTIME_ERR_STOP);
        struct foc_runtime_snapshot snapshot;
        TEST_ASSERT(foc_runtime_read_snapshot(&runtime, &snapshot) == FOC_RUNTIME_OK);
        TEST_ASSERT(snapshot.action == FOC_DEVICE_OUTPUT_INHIBIT && snapshot.output_confirmed);
        TEST_ASSERT(snapshot.faults == FOC_SAFE_FAULT_WDT && snapshot.updates == tick - 7U);
        TEST_ASSERT(
            runtime.pipeline.current.integral_v.d == integral.d &&
            runtime.pipeline.current.integral_v.q == integral.q
        );
        TEST_ASSERT(runtime.outer_velocity.integral_a == outer_integral);
        *replay = (struct model_replay){state, integral, outer_integral, hash, snapshot.faults};
    }
    return 0;
}

static int test_runtime_replay(void)
{
    struct model_replay first;
    struct model_replay second;
    TEST_ASSERT(run_model(false, true, true, &first) == 0);
    TEST_ASSERT(run_model(false, true, false, &second) == 0);
    TEST_ASSERT(first.hash == second.hash && first.faults == second.faults);
    TEST_ASSERT(first.state.id == second.state.id && first.state.iq == second.state.iq);
    TEST_ASSERT(
        first.state.position == second.state.position && first.state.speed == second.state.speed
    );
    TEST_ASSERT(
        first.inner_integral.d == second.inner_integral.d &&
        first.inner_integral.q == second.inner_integral.q
    );
    TEST_ASSERT(first.outer_integral == second.outer_integral);
    printf(
        "runtime replay hash=%08x fault=%08x\n",
        (unsigned int)first.hash,
        (unsigned int)first.faults
    );
    return run_model(true, true, true, &second);
}

static int test_trajectory_runtime_model(void)
{
    struct foc_runtime velocity_runtime = {0};
    struct foc_runtime position_runtime = {0};
    struct plant_state velocity_state = {0};
    struct plant_state position_state = {0};
    float previous_plan = 0.0f;
    float max_plan = 0.0f;
    float max_plan_accel = 0.0f;
    bool plan_seen = false;
    uint32_t velocity_command_seq = 1U;

    TEST_ASSERT(
        begin_trajectory_runtime(&velocity_runtime, &motor, FOC_RUNTIME_MODE_VELOCITY, 8.0f) == 0
    );
    for (uint32_t tick = 7U; tick <= 10007U; ++tick) {
        const float velocity_target = tick < 4007U ? 8.0f : -8.0f;
        if (tick % 20U == 0U || tick == 4007U) {
            TEST_ASSERT(
                model_command(
                    &velocity_runtime,
                    FOC_RUNTIME_MODE_VELOCITY,
                    velocity_target,
                    ++velocity_command_seq,
                    tick * 100U
                ) == FOC_RUNTIME_OK
            );
        }
        uint32_t compare[3];
        float iq_ref;
        TEST_ASSERT(
            runtime_output(&velocity_runtime, &motor, velocity_state, tick, compare, &iq_ref) == 0
        );
        (void)plant_apply(&motor, &velocity_state, compare, 0.0);
        if (velocity_runtime.trajectory.initialized &&
            velocity_runtime.trajectory.state == FOC_TRAJECTORY_STATE_ACTIVE) {
            const float planned = velocity_runtime.trajectory.planned_velocity_rad_s;
            max_plan = fmaxf(max_plan, fabsf(planned));
            if (plan_seen) {
                max_plan_accel = fmaxf(max_plan_accel, fabsf(planned - previous_plan) / 0.0005f);
            }
            previous_plan = planned;
            plan_seen = true;
        }
    }
    TEST_ASSERT(max_plan <= 12.0001f);
    TEST_ASSERT(max_plan_accel <= 20.05f);
    TEST_ASSERT(velocity_state.speed < -2.0);

    TEST_ASSERT(
        begin_trajectory_runtime(&position_runtime, &motor, FOC_RUNTIME_MODE_POSITION, 2.0f) == 0
    );
    uint32_t position_command_seq = 1U;
    for (uint32_t tick = 7U; tick <= 14007U; ++tick) {
        const float position_target = tick < 7007U ? 2.0f : -1.0f;
        if (tick % 20U == 0U || tick == 7007U) {
            TEST_ASSERT(
                model_command(
                    &position_runtime,
                    FOC_RUNTIME_MODE_POSITION,
                    position_target,
                    ++position_command_seq,
                    tick * 100U
                ) == FOC_RUNTIME_OK
            );
        }
        uint32_t compare[3];
        float iq_ref;
        TEST_ASSERT(
            runtime_output(&position_runtime, &motor, position_state, tick, compare, &iq_ref) == 0
        );
        (void)plant_apply(&motor, &position_state, compare, 0.0);
    }
    TEST_ASSERT(isfinite(position_state.position) && isfinite(position_state.speed));
    TEST_ASSERT(fabs(position_state.position) < 4.0);

    const uint32_t rejected_tick = 14010U;
    const struct foc_runtime_fast_input rejected_input = {
        .adc = plant_adc(&motor, position_state, rejected_tick, rejected_tick * 100U),
        .now_us = rejected_tick * 100U + 1U,
    };
    TEST_ASSERT(model_sensor(&position_runtime, position_state, rejected_tick) == FOC_RUNTIME_OK);
    const float committed_plan = position_runtime.trajectory.planned_velocity_rad_s;
    position_runtime.pipeline.mock.reject_update = true;
    const int rejected_status = foc_runtime_h0_step(&position_runtime, &rejected_input);
    TEST_ASSERT(rejected_status != FOC_RUNTIME_OK);
    TEST_ASSERT(position_runtime.trajectory.planned_velocity_rad_s == committed_plan);
    position_runtime.pipeline.mock.reject_update = false;
    return 0;
}

static int test_two_motors(void)
{
    const struct plant_config second_motor =
        {3U, 0.7, 0.0014, 0.0017, 0.028, 0.0016, 0.0045, -0.21};
    struct foc_runtime first = {0};
    struct foc_runtime second = {0};
    struct plant_state first_state = {0};
    struct plant_state second_state = {0};
    uint32_t sequence = 1U;

    TEST_ASSERT(begin_model_runtime(&first, &motor, FOC_RUNTIME_MODE_VELOCITY, 4.0f) == 0);
    TEST_ASSERT(begin_model_runtime(&second, &second_motor, FOC_RUNTIME_MODE_VELOCITY, -4.0f) == 0);
    for (uint32_t tick = 7U; tick <= 12006U; ++tick) {
        if (tick % 20U == 0U) {
            sequence++;
            TEST_ASSERT(
                model_command(&first, FOC_RUNTIME_MODE_VELOCITY, 4.0f, sequence, tick * 100U) ==
                FOC_RUNTIME_OK
            );
            TEST_ASSERT(
                model_command(&second, FOC_RUNTIME_MODE_VELOCITY, -4.0f, sequence, tick * 100U) ==
                FOC_RUNTIME_OK
            );
        }
        uint32_t compare[3];
        float iq;
        TEST_ASSERT(runtime_output(&first, &motor, first_state, tick, compare, &iq) == 0);
        (void)plant_apply(&motor, &first_state, compare, 0.0);
        TEST_ASSERT(runtime_output(&second, &second_motor, second_state, tick, compare, &iq) == 0);
        (void)plant_apply(&second_motor, &second_state, compare, 0.0);
    }
    TEST_ASSERT(fabs(first_state.speed - 4.0) <= 0.15 && fabs(second_state.speed + 4.0) <= 0.15);
    TEST_ASSERT(first.outer_velocity.integral_a > 0.0f && second.outer_velocity.integral_a < 0.0f);
    foc_runtime_request_inhibit(&first, FOC_SAFE_FAULT_WDT);
    const struct foc_runtime_fast_input stop = {.now_us = 1200701U};
    TEST_ASSERT(foc_runtime_h0_step(&first, &stop) == FOC_RUNTIME_ERR_STOP);
    uint32_t compare[3];
    float iq;
    TEST_ASSERT(runtime_output(&second, &second_motor, second_state, 12007U, compare, &iq) == 0);
    TEST_ASSERT(first.pipeline.safety.latched_faults == FOC_SAFE_FAULT_WDT);
    TEST_ASSERT(
        second.pipeline.safety.latched_faults == 0U && second.pipeline.mock.updates == 12001U
    );
    printf(
        "instances p=%u speed=%.6f, p=%u speed=%.6f\n",
        (unsigned int)motor.poles,
        first_state.speed,
        (unsigned int)second_motor.poles,
        second_state.speed
    );
    return 0;
}

/* Test Entry ------------------------------------------------------------------ */

int main(void)
{
    return test_plant() || run_model(false, false, false, NULL) ||
        run_model(true, false, false, NULL) || test_runtime_replay() ||
        test_trajectory_runtime_model() || test_two_motors();
}

/* ---------------------------------------- end of file ----------------------------------------- */
