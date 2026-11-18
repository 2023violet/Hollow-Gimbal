/**
 * @file    test_foc_impedance_virtual.c
 * @brief   Host impedance request and Runtime Current closed-loop holdout.
 * @version v0.1.0
 * @date    2026.11.18
 *
 * Test role
 * ---------
 * Compose the portable controller with fresh Encoder data and the existing
 * Runtime Current command entry. Measure the actual Virtual PMSM plant.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_impedance.h"
#include "foc_virtual_current.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* Private Macros --------------------------------------------------------------- */

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return 1;                                                       \
        }                                                                   \
    } while (0)

/* Private Constants ------------------------------------------------------------ */

static const uint32_t epoch_us = 1000U;
static const uint32_t step_us = 500U;
static const uint32_t window_us = 100000U;
static const float nominal_iq_a = 2.2f;
static const double stiffness = 0.65;
static const double hold_band_rad = 0.008;
static const double load_torque_nm = 0.039;

/* Private Types --------------------------------------------------------------- */

struct impedance_case {
    const char *name;
    uint32_t end_us;
    uint32_t release_or_reverse_us;
    float reference_rad;
    float damping;
    bool load;
    bool cascade;
    bool derate;
};

struct impedance_metrics {
    double error_sum_sq;
    double peak_error;
    double overshoot;
    double max_speed;
    double min_target_iq;
    double max_target_iq;
    double max_actual_iq;
    double max_actual_id;
    double pre_error;
    double loaded_error_sum;
    double final_error;
    double final_min_position;
    double final_max_position;
    double final_position;
    uint32_t samples;
    uint32_t loaded_samples;
    uint32_t settled_us;
    uint32_t limited_us;
    uint32_t max_limited_run_us;
    uint32_t commands;
    uint32_t updates;
};

/* Private Helpers -------------------------------------------------------------- */

static struct foc_impedance_config
impedance_config(const struct foc_virtual_current_profile *profile, float damping)
{
    return (struct foc_impedance_config){
        .stiffness_nm_per_rad = (float)stiffness,
        .damping_nm_s_per_rad = damping,
        .torque_constant_nm_per_a =
            (float)(1.5 * profile->motor.pole_pairs * profile->motor.psi_f_wb),
        .torque_limit_nm = 0.315f,
    };
}

static int start_case(struct foc_virtual_current *vm, bool cascade)
{
    struct foc_virtual_current_profile profile = foc_virtual_current_default_profile();
    profile.current.current_limit_a = nominal_iq_a;
    profile.runtime.motion.iq_limit_a = nominal_iq_a;
    CHECK(foc_virtual_current_init(vm, &profile) == 0);
    if (cascade) {
        CHECK(foc_virtual_current_queue_set_mode(vm, FOC_RUNTIME_MODE_POSITION) == 0);
    }
    CHECK(foc_virtual_current_run_until(vm, 100U) == 0);
    CHECK(
        (cascade ? foc_virtual_current_publish_motion(vm, FOC_RUNTIME_MODE_POSITION, 0.0f)
                 : foc_virtual_current_publish_iq(vm, 0.0f, 0.0f)) == FOC_RUNTIME_OK
    );
    CHECK(foc_virtual_current_run_until(vm, 500U) == 0);
    CHECK(foc_virtual_current_queue_action(vm, FOC_RUNTIME_ACTION_ARM) == 0);
    CHECK(foc_virtual_current_run_until(vm, 600U) == 0);
    CHECK(foc_virtual_current_queue_action(vm, FOC_RUNTIME_ACTION_START) == 0);
    CHECK(foc_virtual_current_run_until(vm, 800U) == 0);
    CHECK(vm->runtime.pipeline.safety.state == FOC_SAFE_STATE_RUNNING);
    return 0;
}

static int publish_impedance(
    struct foc_virtual_current *vm,
    const struct foc_impedance_config *config,
    float reference_rad,
    float live_iq_a,
    struct foc_impedance_output *output
)
{
    *output = (struct foc_impedance_output){0};
    /* The simulator is quiescent here. Use the same accepted code/time and
     * LPF estimate that the ordinary H1 producer just used. */
    const unsigned index = atomic_load_explicit(&vm->runtime.sensor_slot, memory_order_acquire);
    if (index >= sizeof vm->runtime.sensor_slots / sizeof vm->runtime.sensor_slots[0] ||
        !vm->encoder.velocity_seen || vm->encoder.continuity_lost ||
        vm->encoder.previous_us != vm->virtual_time_us) {
        return FOC_RUNTIME_ERR_INPUT;
    }
    const struct foc_runtime_sensor *sample = &vm->runtime.sensor_slots[index].sample;
    if (!sample->valid || sample->seq != vm->encoder_seq ||
        sample->captured_us != vm->encoder.previous_us ||
        sample->mechanical_rad_s != vm->encoder.lpf_velocity_rad_s) {
        return FOC_RUNTIME_ERR_INPUT;
    }
    const double measured_rad = vm->encoder.position_counts * 6.28318530717958647692 /
        vm->encoder.config.counts_per_revolution;
    const struct foc_impedance_input input = {
        .position_error_rad = (float)((double)reference_rad - measured_rad),
        .reference_velocity_rad_s = 0.0f,
        .measured_velocity_rad_s = sample->mechanical_rad_s,
        .torque_feedforward_nm = 0.0f,
        .live_iq_limit_a = live_iq_a,
    };
    if (foc_impedance_step(config, &input, output) != FOC_STATUS_OK || !output->valid) {
        return FOC_RUNTIME_ERR_INPUT;
    }
    return foc_virtual_current_publish_iq(vm, 0.0f, output->iq_ref_a);
}

static int advance_running(struct foc_virtual_current *vm, uint32_t target_us)
{
    const uint32_t next_h0 = vm->next_h0_us;
    const uint32_t period = vm->profile.runtime.h0.period_us;
    const uint32_t due = target_us >= next_h0 ? 1U + (target_us - next_h0) / period : 0U;
    const uint32_t accepted = vm->output.accepted_updates;
    CHECK(foc_virtual_current_run_until(vm, target_us) == 0);
    CHECK(vm->output.accepted_updates == accepted + due);
    CHECK(vm->runtime.pipeline.safety.state == FOC_SAFE_STATE_RUNNING);
    CHECK(vm->runtime.pipeline.safety.latched_faults == 0U && vm->output.rejected_updates == 0U);
    return 0;
}

static float live_limit(const struct impedance_case *test, uint32_t t)
{
    if (test->derate && t < 180000U) {
        return t < 40000U ? 0.45f : 0.25f;
    }
    return nominal_iq_a;
}

static int
evaluate_case(const struct impedance_case *test, bool trace, struct impedance_metrics *metrics)
{
    struct foc_virtual_current vm;
    CHECK(start_case(&vm, test->cascade) == 0);
    const struct foc_impedance_config config = impedance_config(&vm.profile, test->damping);
    CHECK(config.torque_constant_nm_per_a == 0.15f);
    const uint32_t expected_session = vm.runtime.motion_session;
    const enum foc_runtime_mode expected_mode =
        test->cascade ? FOC_RUNTIME_MODE_POSITION : FOC_RUNTIME_MODE_CURRENT;
    const uint32_t initial_updates = vm.output.accepted_updates;
    *metrics =
        (struct impedance_metrics){.final_min_position = INFINITY, .final_max_position = -INFINITY};
    uint32_t limited_run_us = 0U;
    for (uint32_t t = 0; t < test->end_us; t += step_us) {
        const uint32_t capture = epoch_us + t;
        CHECK(advance_running(&vm, capture) == 0);
        CHECK(vm.virtual_time_us == capture && vm.encoder.previous_us == capture);
        if (test->load && (t == 800000U || t == 1600000U)) {
            CHECK(foc_virtual_motor_set_load(&vm.motor, t == 800000U ? load_torque_nm : 0.0) == 0);
        }
        const bool reversing =
            !test->load && test->release_or_reverse_us != 0U && t >= test->release_or_reverse_us;
        const float reference = reversing ? -0.20f : test->reference_rad;
        const float iq_cap = live_limit(test, t);
        const uint32_t prior_seq = vm.command_seq;
        struct foc_impedance_output result = {0};
        float iq_target = vm.runtime.pipeline.target_a.q;
        if (test->cascade) {
            CHECK(
                foc_virtual_current_publish_motion(&vm, expected_mode, reference) == FOC_RUNTIME_OK
            );
        } else {
            CHECK(publish_impedance(&vm, &config, reference, iq_cap, &result) == FOC_RUNTIME_OK);
            CHECK(result.valid && fabsf(result.limited_torque_nm) <= config.torque_limit_nm);
            iq_target = result.iq_ref_a;
        }
        CHECK(vm.command_seq == prior_seq + 1U && fabsf(iq_target) <= iq_cap);
        const double position = vm.motor.state.mechanical_position_rad;
        const double speed = vm.motor.state.mechanical_velocity_rad_s;
        const double id = vm.motor.state.id_a;
        const double iq = vm.motor.state.iq_a;
        CHECK(isfinite(position) && isfinite(speed) && isfinite(iq) && isfinite(id));
        const double error = (double)reference - position;
        metrics->error_sum_sq += error * error;
        metrics->peak_error = fmax(metrics->peak_error, fabs(error));
        metrics->overshoot = fmax(metrics->overshoot, reversing ? error : -error);
        metrics->max_speed = fmax(metrics->max_speed, fabs(speed));
        metrics->min_target_iq = fmin(metrics->min_target_iq, iq_target);
        metrics->max_target_iq = fmax(metrics->max_target_iq, iq_target);
        metrics->max_actual_iq = fmax(metrics->max_actual_iq, fabs(iq));
        metrics->max_actual_id = fmax(metrics->max_actual_id, fabs(id));
        metrics->samples++;
        if (result.limited) {
            metrics->limited_us += step_us;
            limited_run_us += step_us;
            if (limited_run_us > metrics->max_limited_run_us) {
                metrics->max_limited_run_us = limited_run_us;
            }
        } else {
            limited_run_us = 0U;
        }
        const uint32_t pre_boundary = test->load ? 800000U : test->release_or_reverse_us;
        if (pre_boundary != 0U && t < pre_boundary && t >= pre_boundary - window_us) {
            metrics->pre_error = fmax(metrics->pre_error, fabs(error));
        }
        if (test->load && t >= 1600000U - window_us && t < 1600000U) {
            metrics->loaded_error_sum += error;
            metrics->loaded_samples++;
        }
        if (t >= test->end_us - window_us) {
            metrics->final_error = fmax(metrics->final_error, fabs(error));
            metrics->final_min_position = fmin(metrics->final_min_position, position);
            metrics->final_max_position = fmax(metrics->final_max_position, position);
        }
        metrics->final_position = position;
        if (t >= test->release_or_reverse_us && fabs(error) > hold_band_rad) {
            metrics->settled_us = t - test->release_or_reverse_us + step_us;
        }
        CHECK(advance_running(&vm, vm.next_h0_us) == 0);
        CHECK(vm.runtime.mode == expected_mode && vm.runtime.motion_session == expected_session);
        CHECK(vm.runtime.pipeline.mock.last_request.action == FOC_DEVICE_OUTPUT_UPDATE);
        if (!test->cascade) {
            CHECK(vm.runtime.pipeline.command.seq == vm.command_seq);
            CHECK(vm.runtime.pipeline.command.produced_us == capture);
            CHECK(
                vm.runtime.pipeline.target_a.d == 0.0f &&
                vm.runtime.pipeline.target_a.q == iq_target
            );
        }
        if (trace) {
            printf(
                "TRACE,%s,%u,%u,%u,%.9g,%.12g,%.12g,%.9g,%.9g,%.12g,%.12g,%.8g,%u,%.8g,%u,%u,%u,%"
                "u\n",
                test->name,
                t,
                capture,
                vm.encoder_seq,
                reference,
                position,
                speed,
                iq_target,
                iq_cap,
                iq,
                id,
                vm.motor.load_torque_nm,
                (unsigned)result.limited,
                result.limited_torque_nm,
                vm.command_seq,
                vm.runtime.pipeline.command.produced_us,
                vm.virtual_time_us,
                vm.output.accepted_updates
            );
        }
    }
    metrics->commands = vm.command_seq - 1U;
    metrics->updates = vm.output.accepted_updates - initial_updates;
    CHECK(metrics->commands == metrics->samples);
    printf(
        "SUMMARY,%s,%u,%.10g,%.10g,%.10g,%u,%.10g,%.10g,%.10g,%.10g,%.10g,%u,%u,"
        "%.10g,%.10g,%.10g,%.10g,%.10g,%u,%u\n",
        test->name,
        metrics->samples,
        sqrt(metrics->error_sum_sq / metrics->samples),
        metrics->peak_error,
        metrics->overshoot,
        metrics->settled_us,
        metrics->max_speed,
        metrics->min_target_iq,
        metrics->max_target_iq,
        metrics->max_actual_iq,
        metrics->max_actual_id,
        metrics->limited_us,
        metrics->max_limited_run_us,
        metrics->pre_error,
        metrics->loaded_samples ? metrics->loaded_error_sum / metrics->loaded_samples : 0.0,
        metrics->final_error,
        metrics->final_max_position - metrics->final_min_position,
        metrics->final_position,
        metrics->commands,
        metrics->updates
    );
    return 0;
}

static int test_inhibit_boundaries(void)
{
    for (unsigned fault = 0U; fault <= 1U; ++fault) {
        struct foc_virtual_current vm;
        struct foc_impedance_output out;
        CHECK(start_case(&vm, false) == 0);
        const struct foc_impedance_config config = impedance_config(&vm.profile, 0.048f);
        CHECK(advance_running(&vm, 1000U) == 0);
        CHECK(publish_impedance(&vm, &config, 0.30f, nominal_iq_a, &out) == FOC_RUNTIME_OK);
        CHECK(out.valid && out.iq_ref_a > 0.0f);
        const uint32_t command = vm.command_seq;
        CHECK(publish_impedance(&vm, &config, NAN, nominal_iq_a, &out) == FOC_RUNTIME_ERR_INPUT);
        CHECK(!out.valid && out.iq_ref_a == 0.0f && vm.command_seq == command);
        CHECK(advance_running(&vm, 1100U) == 0);
        CHECK(publish_impedance(&vm, &config, 0.30f, nominal_iq_a, &out) == FOC_RUNTIME_ERR_INPUT);
        CHECK(!out.valid && vm.command_seq == command);
        CHECK(advance_running(&vm, 1400U) == 0);
        const uint32_t updates = vm.output.accepted_updates;
        const uint32_t session = vm.runtime.motion_session;
        if (fault != 0U) {
            CHECK(foc_virtual_current_mark_sensor_fault(&vm, 1500U) == 0);
            for (uint32_t tick = 1500U; tick <= 3000U; tick += step_us) {
                CHECK(foc_virtual_current_fail_next_sensor(&vm) == 0);
                CHECK(foc_virtual_current_run_until(&vm, tick) == 0);
                CHECK(
                    publish_impedance(&vm, &config, 0.30f, nominal_iq_a, &out) ==
                    FOC_RUNTIME_ERR_INPUT
                );
                CHECK(!out.valid && vm.command_seq == command);
            }
            CHECK(vm.fault_events.inhibit_accepted_seen && vm.fault_events.fault_latched_seen);
            CHECK(vm.runtime.pipeline.safety.state == FOC_SAFE_STATE_FAULT_LATCHED);
            CHECK(vm.runtime.pipeline.safety.latched_faults != 0U);
        } else {
            CHECK(foc_virtual_current_request_stop(&vm) == 0);
            CHECK(foc_virtual_current_run_until(&vm, 3000U) == 0);
            CHECK(vm.runtime.pipeline.safety.state == FOC_SAFE_STATE_DISABLED);
        }
        CHECK(vm.output.request.action == FOC_DEVICE_OUTPUT_INHIBIT);
        CHECK(vm.output.accepted_updates == updates && vm.runtime.motion_session != session);
        printf(
            "BOUNDARY,%s,%u,%u,%u,%u,%u,%u\n",
            fault ? "fault" : "stop",
            updates,
            vm.output.accepted_updates,
            (unsigned)vm.runtime.pipeline.safety.state,
            (unsigned)vm.runtime.pipeline.safety.latched_faults,
            session,
            vm.runtime.motion_session
        );
    }
    return 0;
}

/* Test Entry ------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    static const struct impedance_case cases[] = {
        {"hold", 1600000U, 0U, 0.30f, 0.048f, false, false, false},
        {"soft-damping", 1600000U, 0U, 0.30f, 0.006f, false, false, false},
        {"load", 2400000U, 1600000U, 0.30f, 0.048f, true, false, false},
        {"cascade-load", 2400000U, 1600000U, 0.30f, 0.048f, true, true, false},
        {"reversal", 1500000U, 700000U, 0.25f, 0.048f, false, false, false},
        {"limited", 1500000U, 0U, 0.65f, 0.048f, false, false, true},
    };
    struct impedance_metrics results[sizeof cases / sizeof cases[0]];
    const bool trace = argc == 2 && strcmp(argv[1], "--trace") == 0;
    for (unsigned i = 0U; i < sizeof cases / sizeof cases[0]; ++i) {
        CHECK(evaluate_case(&cases[i], trace, &results[i]) == 0);
    }
    CHECK(test_inhibit_boundaries() == 0);
    const struct impedance_metrics *hold = &results[0];
    const struct impedance_metrics *soft = &results[1];
    const struct impedance_metrics *load = &results[2];
    const struct impedance_metrics *cascade = &results[3];
    const struct impedance_metrics *reverse = &results[4];
    const struct impedance_metrics *limited = &results[5];
    const bool quality[] = {
        hold->final_error <= hold_band_rad &&
            hold->final_max_position - hold->final_min_position <= 0.010 &&
            hold->overshoot <= 0.075 && hold->settled_us <= 650000U && hold->max_speed <= 8.0,
        soft->final_error <= 0.015 && soft->overshoot >= hold->overshoot + 0.06 &&
            soft->settled_us > hold->settled_us && soft->max_speed <= 8.0,
        load->pre_error <= hold_band_rad && load->final_error <= hold_band_rad &&
            fabs(load->loaded_error_sum / load->loaded_samples - load_torque_nm / stiffness) <=
                0.009 &&
            load->settled_us <= 650000U,
        cascade->pre_error <= 0.070 && cascade->final_error <= 0.050 &&
            fabs(cascade->loaded_error_sum / cascade->loaded_samples) <= 0.120,
        reverse->pre_error <= hold_band_rad && reverse->final_error <= hold_band_rad &&
            reverse->min_target_iq < 0.0 && reverse->max_target_iq > 0.0,
        limited->max_limited_run_us >= 100000U && limited->limited_us < cases[5].end_us &&
            limited->final_error <= 0.012,
    };
    bool pass = true;
    for (unsigned i = 0U; i < sizeof cases / sizeof cases[0]; ++i) {
        printf("QUALITY,%s,%s\n", cases[i].name, quality[i] ? "PASS" : "QUALITY_FAIL");
        pass = pass && quality[i];
    }
    return pass ? 0 : 1;
}

/* ---------------------------------------- end of file ----------------------------------------- */
