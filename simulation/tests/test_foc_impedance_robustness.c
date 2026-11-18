/**
 * @file    test_foc_impedance_robustness.c
 * @brief   Model candidates and frozen Runtime impedance robustness scenarios.
 * @version v0.1.0
 * @date    2026.11.18
 *
 * Test role
 * ---------
 * Reuse the existing Runtime, Virtual Motor, encoder and H0 scheduler. Inject
 * known sensor and output delays at real boundaries and record dispositions.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_damping_noise_filter.h"
#include "foc_impedance.h"
#include "foc_impedance_feedforward.h"
#include "foc_impedance_tuning.h"
#include "foc_virtual_current.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Private Macros --------------------------------------------------------------- */

#define CHECK(condition)                                                  \
    do {                                                                  \
        if (!(condition)) {                                               \
            fprintf(stderr, "robustness:%d: %s\n", __LINE__, #condition); \
            return 1;                                                     \
        }                                                                 \
    } while (0)

/* Private Constants ------------------------------------------------------------ */

static const double two_pi = 6.28318530717958647692;
static const uint32_t frame_id = 79U;
static const uint32_t epoch_us = 2000U;
static const uint32_t period_us = 500U;
static const uint32_t tail_us = 300000U;
static const int16_t lut_offsets[] = {0, -8, 0, 8};
static const struct foc_encoder_lut lut = {lut_offsets, 4U, 65536U, 79U};

/* Private Types --------------------------------------------------------------- */

enum study_sensor {
    QUANTIZED,
    IDEAL,
    DISTORTED,
    CORRECTED,
    NOISY
};
enum study_design {
    FIXED,
    MATCHED,
    BIASED,
    NOMINAL,
    GENTLE,
    HIGH_DAMPING
};
enum study_stimulus {
    STEP,
    LOAD,
    REVERSE_LOAD,
    CRAWL
};

struct study_case {
    const char *name;
    double inertia_scale;
    enum study_design design;
    enum study_sensor sensor;
    enum study_stimulus stimulus;
    float target_rad;
    float cap_a;
    uint32_t capture_delay_us;
    uint32_t h1_wait_us;
    uint32_t plant_delay_us;
    uint32_t seed;
    bool expect_gate;
};

struct study_model {
    double inertia;
    double damping;
};

struct output_delay {
    struct foc_virtual_current *vm;
    bool (*submit)(void *argument, const struct foc_device_pwm_request *request);
    void *submit_arg;
    struct foc_device_pwm_request queued;
    uint32_t delay_us;
    uint32_t due_us;
    uint32_t last_applied_us;
    uint32_t last_submitted_us;
    uint32_t max_source_age_us;
    uint32_t inhibit_us;
    int inhibit_reason;
    bool pending;
    bool inhibited;
    bool overflow;
};

struct metrics {
    double sum_error2;
    double peak_error;
    double peak_over;
    double peak_speed;
    double peak_q;
    double peak_request;
    double loaded_error_sum;
    double tail_peak_error;
    double tail_v2;
    double tail_q2;
    double tail_qhp2;
    double tail_actual_qhp2;
    double tail_torque2;
    double tail_damping2;
    double tail_min_error;
    double tail_max_error;
    uint32_t samples;
    uint32_t load_samples;
    uint32_t noise_samples;
    uint32_t settle_us;
    uint32_t reverse_settle_us;
    uint32_t recover_us;
    uint32_t limited_us;
    uint32_t longest_limit_us;
    uint32_t repeats;
};

/* Private Helpers -------------------------------------------------------------- */

static bool submit_delayed(void *argument, const struct foc_device_pwm_request *request)
{
    struct output_delay *out = argument;
    struct foc_virtual_current *vm = out->vm;
    const struct foc_device_pwm_request active = vm->output.request;
    const enum foc_virtual_output_mode active_mode = vm->output.mode;
    if (request->action == FOC_DEVICE_OUTPUT_UPDATE && out->pending) {
        out->overflow = true;
        return false;
    }
    if (!out->submit(out->submit_arg, request)) {
        return false;
    }
    if (request->action == FOC_DEVICE_OUTPUT_INHIBIT) {
        out->pending = false;
        if (!out->inhibited) {
            out->inhibited = true;
            out->inhibit_us = vm->virtual_time_us;
            out->inhibit_reason = vm->runtime.pipeline.last_reason;
        }
        return true;
    }
    out->last_submitted_us = vm->virtual_time_us;
    const uint32_t age = vm->runtime.angle_prediction_us;
    if (age > out->max_source_age_us) {
        out->max_source_age_us = age;
    }
    if (out->delay_us == 0U) {
        out->last_applied_us = vm->virtual_time_us;
    } else {
        /* The original adapter still owns acceptance and journals the update.
         * Only the virtual Plant's held duty waits. INHIBIT bypasses the wait. */
        out->queued = *request;
        out->due_us = vm->virtual_time_us + out->delay_us;
        out->pending = true;
        vm->output.request = active;
        vm->output.mode = active_mode;
    }
    return true;
}

static int advance(struct output_delay *out, uint32_t to)
{
    struct foc_virtual_current *vm = out->vm;
    CHECK(to >= vm->virtual_time_us);
    while (vm->virtual_time_us < to) {
        uint32_t next = to;
        if (out->delay_us != 0U && vm->next_h0_us < next) {
            next = vm->next_h0_us;
        }
        if (out->pending && out->due_us < next) {
            next = out->due_us;
        }
        CHECK(foc_virtual_current_run_until(vm, next) == 0 && !out->overflow);
        if (out->pending && out->due_us == next) {
            CHECK(next - out->last_submitted_us == out->delay_us);
            vm->output.request = out->queued;
            vm->output.mode = FOC_VIRTUAL_OUTPUT_UPDATE;
            out->pending = false;
            out->last_applied_us = next;
        }
    }
    return 0;
}

static int publish(
    struct foc_virtual_current *vm,
    const struct foc_impedance_config *gains,
    float position,
    float speed,
    float feedforward
)
{
    struct foc_runtime_snapshot snapshot;
    CHECK(foc_virtual_current_snapshot(vm, &snapshot) == FOC_RUNTIME_OK);
    const struct foc_runtime_command command = {
        .mode = FOC_RUNTIME_MODE_IMPEDANCE,
        .seq = ++vm->command_seq,
        .produced_us = vm->virtual_time_us,
        .target.impedance = {
            .position_rad = position,
            .velocity_rad_s = speed,
            .stiffness_nm_per_rad = gains->stiffness_nm_per_rad,
            .damping_nm_s_per_rad = gains->damping_nm_s_per_rad,
            .torque_feedforward_nm = feedforward,
            .mechanical_frame_id = frame_id,
            .source_session = snapshot.motion_session,
        },
    };
    return foc_runtime_publish_impedance_command(&vm->runtime, &command, vm->virtual_time_us);
}

static int lifecycle(
    struct foc_virtual_current *vm,
    enum foc_runtime_action action,
    uint32_t at,
    enum foc_safe_state state
)
{
    CHECK(
        foc_virtual_current_publish_lifecycle(vm, action, FOC_RUNTIME_MODE_IMPEDANCE) ==
        FOC_RUNTIME_OK
    );
    CHECK(foc_virtual_current_run_until(vm, at) == 0);
    struct foc_runtime_lifecycle_result response;
    CHECK(
        foc_virtual_current_read_lifecycle(vm, &response) == FOC_RUNTIME_OK &&
        response.result == FOC_RUNTIME_REQUEST_APPLIED && response.h0_us == at &&
        response.state == state && response.request_sequence == vm->lifecycle_sequence
    );
    return 0;
}

static int design_candidate(
    const struct study_case *test,
    const struct study_model *model,
    const struct foc_virtual_current_profile *profile,
    struct foc_impedance_config *gains,
    struct foc_impedance_tuning_candidate *candidate,
    double *j_estimate,
    double *b_estimate
)
{
    const struct foc_runtime_impedance_config *policy = &profile->runtime.motion.impedance;
    *gains = (struct foc_impedance_config){
        .torque_constant_nm_per_a = policy->torque_constant_nm_per_a,
        .torque_limit_nm = policy->torque_limit_nm,
    };
    *candidate = (struct foc_impedance_tuning_candidate){0};
    if (test->design == FIXED) {
        gains->stiffness_nm_per_rad = 0.65f;
        gains->damping_nm_s_per_rad = 0.048f;
        *j_estimate = 0.0;
        *b_estimate = 0.0;
        return 0;
    }
    const bool matched = test->design == MATCHED;
    const bool biased = test->design == BIASED;
    const bool gentle = test->design == GENTLE;
    *j_estimate = model ? model->inertia
        : matched       ? profile->motor.inertia_kg_m2
        : biased        ? 0.75 * profile->motor.inertia_kg_m2
                        : 0.002;
    *b_estimate = model ? model->damping
        : biased        ? 1.5 * profile->motor.damping_nm_s
                        : profile->motor.damping_nm_s;
    const struct foc_impedance_tuning_input input = {
        .model_id = (uint32_t)test->design,
        .damping_source = FOC_IMPEDANCE_TUNING_DAMPING_PROVIDED,
        .inertia_kg_m2 = (float)*j_estimate,
        .viscous_damping_nm_s_per_rad = (float)*b_estimate,
        .target_natural_frequency_rad_s = gentle ? 12.0f : 18.0f,
        .target_damping_ratio = gentle     ? 0.80f
            : test->design == HIGH_DAMPING ? 1.20f
                                           : 0.75f,
        .torque_constant_nm_per_a = policy->torque_constant_nm_per_a,
        .max_stiffness_nm_per_rad = policy->max_stiffness_nm_per_rad,
        .max_damping_nm_s_per_rad = policy->max_damping_nm_s_per_rad,
        .iq_limit_a = test->cap_a,
        .sample_period_s = (float)profile->runtime.h1.period_us * 1.0e-6f,
        .extra_delay_s =
            (float)(profile->runtime.angle_timeout_us + test->plant_delay_us) * 1.0e-6f,
    };
    CHECK(
        foc_impedance_tuning_design(&input, candidate) == FOC_IMPEDANCE_TUNING_OK &&
        candidate->valid
    );
    gains->stiffness_nm_per_rad = candidate->stiffness_nm_per_rad;
    gains->damping_nm_s_per_rad = candidate->damping_nm_s_per_rad;
    return 0;
}

static int start_case(
    struct foc_virtual_current *vm,
    const struct foc_virtual_current_profile *profile,
    const struct foc_impedance_config *gains,
    enum study_sensor sensor
)
{
    CHECK(foc_virtual_current_init(vm, profile) == 0);
    if (sensor == CORRECTED) {
        CHECK(
            foc_encoder_init_with_lut(&vm->encoder, &profile->encoder, frame_id, &lut) ==
            FOC_ENCODER_STATUS_OK
        );
    }
    /* Establish the same zero and ordinary production sensor before the fixture
     * takes over capture. The distortion LUT has a zero correction at zero. */
    CHECK(foc_virtual_current_run_until(vm, 0U) == 0 && vm->runtime.pipeline.mock.output_confirmed);
    CHECK(lifecycle(vm, FOC_RUNTIME_ACTION_SET_MODE, 200U, FOC_SAFE_STATE_DISABLED) == 0);
    CHECK(foc_virtual_current_run_until(vm, 201U) == 0);
    CHECK(publish(vm, gains, 0.0f, 0.0f, 0.0f) == FOC_RUNTIME_OK);
    CHECK(foc_virtual_current_run_until(vm, 500U) == 0);
    CHECK(lifecycle(vm, FOC_RUNTIME_ACTION_ARM, 600U, FOC_SAFE_STATE_READY) == 0);
    CHECK(lifecycle(vm, FOC_RUNTIME_ACTION_START, 800U, FOC_SAFE_STATE_RUNNING) == 0);
    CHECK(foc_virtual_current_run_until(vm, 1500U) == 0);
    vm->next_h1_us = UINT32_MAX;
    return 0;
}

static int take_sample(
    struct foc_virtual_current *vm,
    enum study_sensor sensor,
    uint32_t *random,
    struct foc_runtime_sensor *sample,
    uint16_t *raw,
    float *raw_velocity
)
{
    const uint32_t capture = vm->virtual_time_us;
    *sample = (struct foc_runtime_sensor){
        .captured_us = capture,
        .seq = ++vm->encoder_seq,
        .valid = true,
        .electrical_phase_valid = true,
        .mechanical_frame_id = frame_id,
    };
    const struct foc_rotor_config *rotor = &vm->runtime.config.motion.rotor;
    if (sensor == IDEAL) {
        const double p = vm->motor.state.mechanical_position_rad;
        double angle = fmod(rotor->pole_pairs * p + rotor->electrical_offset_rad, two_pi);
        if (angle < 0.0) {
            angle += two_pi;
        }
        const float wrapped = (float)angle;
        sample->theta_elec_rad = wrapped >= (float)two_pi ? 0.0f : wrapped;
        sample->mechanical_rad = (float)p;
        sample->mechanical_rad_s = (float)vm->motor.state.mechanical_velocity_rad_s;
        return 0;
    }
    uint16_t unperturbed;
    CHECK(foc_virtual_motor_make_encoder_code(&vm->motor, &vm->profile.encoder, &unperturbed) == 0);
    int offset = 0;
    if (sensor == NOISY) {
        *random = *random * UINT32_C(1664525) + UINT32_C(1013904223);
        offset = (int)((*random >> 16U) % 13U) - 6;
    } else if (sensor == DISTORTED || sensor == CORRECTED) {
        const uint32_t node = unperturbed / 16384U;
        const double u = (unperturbed % 16384U) / 16384.0;
        offset = -(int)lround(
            lut_offsets[node] + u * (lut_offsets[(node + 1U) % 4U] - lut_offsets[node])
        );
    }
    *raw = (uint16_t)((int)unperturbed + offset);
    struct foc_encoder_feedback feedback;
    CHECK(
        foc_encoder_update(&vm->encoder, capture, *raw, FOC_ENCODER_TRANSPORT_OK, &feedback) ==
            FOC_ENCODER_STATUS_OK &&
        feedback.valid && feedback.speed_valid
    );
    CHECK(
        foc_rotor_phase_from_counts(
            rotor,
            feedback.mechanical_phase_count,
            vm->profile.encoder.counts_per_revolution,
            &sample->theta_elec_rad
        ) == FOC_STATUS_OK
    );
    sample->mechanical_rad = feedback.mechanical_rad;
    sample->mechanical_rad_s = feedback.filtered_velocity_rad_s;
    if (raw_velocity != NULL) {
        *raw_velocity = feedback.raw_velocity_rad_s;
    }
    return 0;
}

static void record(
    struct metrics *m,
    const struct study_case *test,
    const struct foc_virtual_motor_state *plant,
    const struct foc_runtime_sensor *sample,
    const struct foc_impedance_output *torque,
    float reference,
    float speed,
    float damping,
    uint32_t t,
    uint32_t end,
    uint32_t load_on,
    uint32_t load_off,
    uint32_t reversal,
    uint32_t *limit_run,
    double *q_lp,
    double *actual_lp
)
{
    const double error = reference - plant->mechanical_position_rad;
    const double magnitude = fabs(error);
    m->samples++;
    m->sum_error2 += error * error;
    m->peak_error = fmax(m->peak_error, magnitude);
    if (t < load_on && test->stimulus != CRAWL) {
        m->peak_over = fmax(m->peak_over, t >= reversal ? error : -error);
    }
    m->peak_speed = fmax(m->peak_speed, fabs(plant->mechanical_velocity_rad_s));
    m->peak_q = fmax(m->peak_q, fabs(plant->iq_a));
    m->peak_request = fmax(m->peak_request, fabs(torque->iq_ref_a));
    if (torque->limited) {
        *limit_run += period_us;
        m->limited_us += period_us;
        if (*limit_run > m->longest_limit_us) {
            m->longest_limit_us = *limit_run;
        }
    } else {
        *limit_run = 0U;
    }
    if (magnitude > 0.008) {
        if (t < reversal && t < load_on) {
            m->settle_us = t + period_us;
        } else if (t >= reversal && t < load_on) {
            m->reverse_settle_us = t - reversal + period_us;
        } else if (t >= load_off) {
            m->recover_us = t - load_off + period_us;
        }
    }
    if (load_off != UINT32_MAX && t >= load_off - 100000U && t < load_off) {
        m->loaded_error_sum += error;
        m->load_samples++;
    }
    *q_lp += 0.2 * (torque->iq_ref_a - *q_lp);
    *actual_lp += 0.2 * (plant->iq_a - *actual_lp);
    if (t >= end - tail_us) {
        const double q_hp = torque->iq_ref_a - *q_lp;
        const double actual_hp = plant->iq_a - *actual_lp;
        const double damping_torque = damping * (speed - sample->mechanical_rad_s);
        m->tail_peak_error = fmax(m->tail_peak_error, magnitude);
        m->tail_min_error = fmin(m->tail_min_error, error);
        m->tail_max_error = fmax(m->tail_max_error, error);
        m->tail_v2 += (double)sample->mechanical_rad_s * sample->mechanical_rad_s;
        m->tail_q2 += (double)torque->iq_ref_a * torque->iq_ref_a;
        m->tail_qhp2 += q_hp * q_hp;
        m->tail_actual_qhp2 += actual_hp * actual_hp;
        m->tail_torque2 += (double)torque->requested_torque_nm * torque->requested_torque_nm;
        m->tail_damping2 += damping_torque * damping_torque;
        m->noise_samples++;
    }
}

static unsigned quality_mask(const struct metrics *m, enum study_stimulus stimulus)
{
    unsigned fail = 0U;
    if (m->peak_over > 0.075) {
        fail |= 1U;
    }
    if (stimulus != CRAWL &&
        (m->settle_us > 650000U || m->reverse_settle_us > 550000U || m->recover_us > 650000U)) {
        fail |= 2U;
    }
    if (m->tail_peak_error > 0.008 || m->peak_error > 0.90) {
        fail |= 4U;
    }
    if (sqrt(m->tail_qhp2 / m->noise_samples) > 0.035 ||
        sqrt(m->tail_actual_qhp2 / m->noise_samples) > 0.035) {
        fail |= 8U;
    }
    if (m->longest_limit_us > 250000U) {
        fail |= 16U;
    }
    if (m->tail_max_error - m->tail_min_error > 0.016) {
        fail |= 32U;
    }
    return fail;
}

static int run_case(const struct study_case *test, const struct study_model *model, bool trace)
{
    struct foc_virtual_current_profile profile = foc_virtual_current_default_profile();
    profile.motor.inertia_kg_m2 *= test->inertia_scale;
    profile.current.current_limit_a = test->cap_a;
    profile.runtime.motion.iq_limit_a = test->cap_a;
    /* Explicit research profile to isolate delay while retaining both gates. */
    profile.runtime.motion.prediction.max_prediction_us = 600U;
    profile.runtime.motion.impedance = (struct foc_runtime_impedance_config){
        .enabled = true,
        .mechanical_frame_id = frame_id,
        .torque_constant_nm_per_a =
            (float)(1.5 * profile.motor.pole_pairs * profile.motor.psi_f_wb),
        .torque_limit_nm = 0.315f,
        .max_stiffness_nm_per_rad = 3.0f,
        .max_damping_nm_s_per_rad = 0.3f,
    };
    CHECK(
        profile.runtime.h0.period_us == 200U && profile.runtime.h1.period_us == period_us &&
        test->plant_delay_us < profile.runtime.h0.period_us
    );
    struct foc_impedance_config gains;
    struct foc_impedance_tuning_candidate candidate;
    double j_hat, b_hat;
    CHECK(design_candidate(test, model, &profile, &gains, &candidate, &j_hat, &b_hat) == 0);
    printf(
        "DESIGN,%s,%u,%u,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%u\n",
        test->name,
        (unsigned)test->design,
        candidate.model_id,
        profile.motor.inertia_kg_m2,
        profile.motor.damping_nm_s,
        j_hat,
        b_hat,
        gains.stiffness_nm_per_rad,
        gains.damping_nm_s_per_rad,
        candidate.ideal_natural_frequency_rad_s,
        candidate.ideal_damping_ratio,
        (unsigned)candidate.valid
    );
    struct foc_virtual_current vm;
    CHECK(start_case(&vm, &profile, &gains, test->sensor) == 0);
    struct output_delay output = {
        .vm = &vm,
        .delay_us = test->plant_delay_us,
        .submit = vm.runtime.pipeline.output_submit,
        .submit_arg = vm.runtime.pipeline.output_arg,
    };
    vm.runtime.pipeline.output_submit = submit_delayed;
    vm.runtime.pipeline.output_arg = &output;

    const bool with_load = test->stimulus == LOAD || test->stimulus == REVERSE_LOAD;
    const uint32_t reversal = test->stimulus == REVERSE_LOAD ? 700000U : UINT32_MAX;
    const uint32_t load_on = test->stimulus == LOAD ? 800000U
        : test->stimulus == REVERSE_LOAD            ? 1300000U
                                                    : UINT32_MAX;
    const uint32_t load_off = test->stimulus == LOAD ? 1500000U
        : test->stimulus == REVERSE_LOAD             ? 1750000U
                                                     : UINT32_MAX;
    const uint32_t end = with_load ? 2600000U : 1600000U;
    const uint32_t session = vm.runtime.motion_session;
    const uint32_t base_updates = vm.output.accepted_updates;
    uint32_t random = test->seed;
    uint32_t limit_run = 0U;
    uint16_t prev_raw = 0U;
    double q_lp = 0.0, actual_lp = 0.0;
    struct metrics m = {.tail_min_error = INFINITY, .tail_max_error = -INFINITY};
    for (uint32_t t = 0U; t < end; t += period_us) {
        const uint32_t deadline = epoch_us + t;
        const uint32_t capture = deadline - 1U - test->capture_delay_us;
        const uint32_t delivery = deadline + test->h1_wait_us;
        CHECK(advance(&output, capture) == 0);
        if (output.inhibited) {
            break;
        }
        struct foc_runtime_sensor sample;
        uint16_t raw = 0;
        CHECK(take_sample(&vm, test->sensor, &random, &sample, &raw, NULL) == 0);
        m.repeats += test->sensor != IDEAL && t != 0U && raw == prev_raw;
        prev_raw = raw;
        const float speed = test->stimulus == CRAWL ? 0.05f : 0.0f;
        const float reference = test->stimulus == CRAWL ? speed * (float)t * 1.0e-6f
            : t >= reversal                             ? -0.20f
                                                        : test->target_rad;
        CHECK(advance(&output, delivery - 1U) == 0);
        if (output.inhibited) {
            break;
        }
        CHECK(publish(&vm, &gains, reference, speed, 0.0f) == FOC_RUNTIME_OK);
        CHECK(foc_runtime_h1_step(&vm.runtime, &sample, delivery, 0U) == FOC_RUNTIME_OK);
        const struct foc_runtime_sensor_slot *slot =
            &vm.runtime.sensor_slots[atomic_load(&vm.runtime.sensor_slot)];
        CHECK(
            slot->target_valid && slot->mode == FOC_RUNTIME_MODE_IMPEDANCE &&
            slot->session == session && slot->sample.seq == sample.seq &&
            slot->sample.captured_us == capture && slot->target.seq == vm.command_seq &&
            slot->target.produced_us == delivery - 1U
        );
        CHECK(advance(&output, delivery) == 0);
        if (output.inhibited) {
            break;
        }
        if (t == load_on || t == load_off) {
            CHECK(foc_virtual_motor_set_load(&vm.motor, t == load_on ? 0.039 : 0.0) == 0);
        }
        CHECK(
            vm.runtime.pipeline.safety.state == FOC_SAFE_STATE_RUNNING &&
            vm.runtime.motion_session == session && vm.runtime.pipeline.safety.latched_faults == 0U
        );
        CHECK(
            vm.output.accepted_updates == base_updates + (delivery - 1600U) / 200U + 1U &&
            vm.output.accepted_updates == vm.runtime.pipeline.mock.updates &&
            vm.output.rejected_updates == 0U
        );
        const struct foc_impedance_input input = {
            .position_error_rad = reference - sample.mechanical_rad,
            .reference_velocity_rad_s = speed,
            .measured_velocity_rad_s = sample.mechanical_rad_s,
            .live_iq_limit_a = test->cap_a,
        };
        struct foc_impedance_output torque;
        CHECK(foc_impedance_step(&gains, &input, &torque) == FOC_STATUS_OK && torque.valid);
        CHECK(
            torque.iq_ref_a == slot->target.target_a.q && slot->target.target_a.d == 0.0f &&
            fabsf(torque.iq_ref_a) <= test->cap_a &&
            fabsf(vm.runtime.pipeline.target_a.q) <= test->cap_a &&
            vm.runtime.pipeline.current.config.current_limit_a == test->cap_a
        );
        const struct foc_virtual_motor_state *plant = &vm.motor.state;
        CHECK(
            isfinite(plant->mechanical_position_rad) &&
            isfinite(plant->mechanical_velocity_rad_s) && isfinite(plant->iq_a)
        );
        record(
            &m,
            test,
            plant,
            &sample,
            &torque,
            reference,
            speed,
            gains.damping_nm_s_per_rad,
            t,
            end,
            load_on,
            load_off,
            reversal,
            &limit_run,
            &q_lp,
            &actual_lp
        );
        if (trace) {
            printf(
                "TRACE,%s,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%.9g,%.12g,%.12g,%.9g,%.9g,%.12g,%.9g,%."
                "9g,"
                "%u\n",
                test->name,
                t,
                capture,
                delivery,
                output.last_submitted_us,
                output.last_applied_us,
                vm.command_seq,
                sample.seq,
                session,
                vm.output.accepted_updates,
                (unsigned)raw,
                (unsigned)torque.limited,
                reference,
                plant->mechanical_position_rad,
                plant->mechanical_velocity_rad_s,
                sample.mechanical_rad_s,
                torque.iq_ref_a,
                plant->iq_a,
                torque.requested_torque_nm,
                vm.motor.load_torque_nm,
                (unsigned)vm.runtime.pipeline.mock.last_request.action
            );
        }
    }
    if (test->expect_gate) {
        CHECK(
            output.inhibited && output.inhibit_reason == FOC_PIPELINE_ERR_SAMPLE &&
            vm.runtime.motion_session != session && !output.pending
        );
        const uint32_t accepted = vm.output.accepted_updates;
        CHECK(advance(&output, vm.virtual_time_us + 800U) == 0);
        CHECK(
            vm.output.accepted_updates == accepted &&
            vm.output.request.action == FOC_DEVICE_OUTPUT_INHIBIT
        );
        printf(
            "GATE,%s,%u,%d,%u,%u,%u,%u,%u\n",
            test->name,
            output.inhibit_us,
            output.inhibit_reason,
            accepted,
            vm.output.accepted_updates,
            output.max_source_age_us,
            (unsigned)vm.runtime.pipeline.safety.latched_faults,
            vm.runtime.motion_session
        );
        return 0;
    }
    CHECK(
        !output.inhibited && m.samples == end / period_us && m.noise_samples != 0U &&
        output.max_source_age_us <= profile.runtime.angle_timeout_us
    );
    if (test->stimulus == CRAWL) {
        CHECK(m.repeats > m.samples / 4U);
    }
    if (test->cap_a < 1.0f) {
        if (test->stimulus == STEP) {
            CHECK(m.limited_us == 0U);
        } else {
            CHECK(m.longest_limit_us >= 100000U && m.limited_us < end);
        }
    }
    const unsigned fail = quality_mask(&m, test->stimulus);
    printf(
        "RESULT,%s,%u,%.10g,%.10g,%.10g,%u,%u,%u,%.10g,%.10g,%.10g,%u,%u,%.10g,%.10g,%.10g,%.10g,%."
        "10g,%.10g,%.10g,%.10g,%u,%u,%u,%u\n",
        test->name,
        m.samples,
        sqrt(m.sum_error2 / m.samples),
        m.peak_error,
        m.peak_over,
        m.settle_us,
        m.reverse_settle_us,
        m.recover_us,
        m.peak_speed,
        m.peak_request,
        m.peak_q,
        m.limited_us,
        m.longest_limit_us,
        m.load_samples == 0U ? 0.0 : m.loaded_error_sum / m.load_samples,
        m.tail_peak_error,
        sqrt(m.tail_v2 / m.noise_samples),
        sqrt(m.tail_q2 / m.noise_samples),
        sqrt(m.tail_qhp2 / m.noise_samples),
        sqrt(m.tail_actual_qhp2 / m.noise_samples),
        sqrt(m.tail_torque2 / m.noise_samples),
        sqrt(m.tail_damping2 / m.noise_samples),
        m.repeats,
        output.max_source_age_us,
        vm.output.accepted_updates - base_updates,
        fail
    );
    printf(
        "QUALITY,%s,%s,%u\n",
        test->name,
        fail == 0U ? "HOST_QUALITY_PASS" : "HOST_QUALITY_FAIL",
        fail
    );

    /* An off-period STOP also cancels any duty waiting for Plant application. */
    if (output.delay_us != 0U) {
        CHECK(advance(&output, vm.next_h0_us) == 0 && output.pending);
    }
    CHECK(advance(&output, vm.virtual_time_us + 1U) == 0);
    const uint32_t accepted = vm.output.accepted_updates;
    const bool pending = output.pending;
    const uint32_t last_applied = output.last_applied_us;
    CHECK(foc_virtual_current_request_stop(&vm) == FOC_RUNTIME_OK);
    const struct foc_runtime_fast_input stop = {.now_us = vm.virtual_time_us};
    CHECK(foc_runtime_h0_step(&vm.runtime, &stop) == FOC_RUNTIME_ERR_STOP);
    CHECK(
        output.inhibited && !output.pending && vm.output.request.action == FOC_DEVICE_OUTPUT_INHIBIT
    );
    CHECK(advance(&output, vm.virtual_time_us + 400U) == 0);
    CHECK(
        vm.output.accepted_updates == accepted && output.last_applied_us == last_applied &&
        vm.runtime.motion_session != session && !vm.runtime.accepted_outer_seen
    );
    printf(
        "STOP,%s,%u,%u,%u,%u,%u\n",
        test->name,
        (unsigned)pending,
        accepted,
        vm.output.accepted_updates,
        session,
        vm.runtime.motion_session
    );
    return 0;
}

/* SERVO-02B Host Damping Study -------------------------------------------------- */

enum noise_stimulus {
    NOISE_HOLD,
    NOISE_MOTION_TRAIN,
    NOISE_MOTION_HOLD,
    NOISE_SATURATION_TRAIN,
    NOISE_SATURATION_HOLD
};

struct noise_case {
    const char *name;
    enum noise_stimulus stimulus;
    double inertia_scale;
    bool matched;
    uint32_t seed;
    float cap_a;
    bool jitter;
};

struct noise_trial {
    struct damping_noise_filter accepted;
    struct damping_noise_filter proposed;
    uint32_t accepted_us;
    uint32_t proposed_us;
    uint32_t proposed_seq;
    bool pending;
};

enum noise_channel {
    NOISE_K,
    NOISE_D,
    NOISE_FF,
    NOISE_PRE_LIMIT,
    NOISE_POST_LIMIT,
    NOISE_REQUEST,
    NOISE_MODEL_IQ,
    NOISE_RAW_SPEED_ERROR,
    NOISE_LPF_SPEED_ERROR,
    NOISE_D_SPEED_ERROR,
    NOISE_QUANT_SPEED_ERROR,
    NOISE_CODE_SPEED_ERROR,
    NOISE_LIMIT_IQ_DELTA,
    NOISE_PI_IQ_DELTA,
    NOISE_D_RAW,
    NOISE_HOST_FF,
    NOISE_CHANNELS
};

struct noise_metrics {
    double lp[NOISE_CHANNELS];
    double sum_hp2[NOISE_CHANNELS];
    double motion_error2[4];
    double peak_over_rad;
    double load_error_sum;
    double last_peak_error_rad;
    double peak_iq_a;
    uint32_t motion_samples[4];
    uint32_t load_samples;
    uint32_t hold_samples;
    uint32_t reverse_us;
    uint32_t recover_us;
    uint32_t first_limit_us;
    uint32_t limit_run_us;
    uint32_t longest_limit_us;
    uint32_t tail_limit_count;
};

static int noise_trial_offer(
    struct noise_trial *state,
    const struct foc_virtual_current *vm,
    const struct foc_runtime_sensor *sample,
    uint32_t session,
    float cutoff_hz
)
{
    const struct foc_runtime *runtime = &vm->runtime;
    if (runtime->motion_session != session ||
        runtime->pipeline.safety.state != FOC_SAFE_STATE_RUNNING) {
        *state = (struct noise_trial){0};
        return 0;
    }
    if (state->pending && runtime->accepted_outer_seen &&
        runtime->accepted_outer_seq == state->proposed_seq) {
        state->accepted = state->proposed;
        state->accepted_us = state->proposed_us;
    }
    if (!sample->valid || !isfinite(sample->mechanical_rad_s)) {
        state->pending = false;
        return 0;
    }
    state->proposed = state->accepted;
    const uint32_t dt =
        state->accepted.ready ? sample->captured_us - state->accepted_us : period_us;
    CHECK(damping_noise_filter_step(&state->proposed, sample->mechanical_rad_s, cutoff_hz, dt));
    state->proposed_seq = sample->seq;
    state->proposed_us = sample->captured_us;
    state->pending = true;
    return 0;
}

/* Four-piece continuous position program. The motion Holdout changes the crawl,
 * ramp rates, dwell and reversal amplitude. Times are on the H1 command grid. */
static unsigned noise_reference(enum noise_stimulus stimulus, uint32_t t, float *p, float *v)
{
    *p = 0.0f;
    *v = 0.0f;
    if (stimulus == NOISE_HOLD || stimulus == NOISE_SATURATION_TRAIN ||
        stimulus == NOISE_SATURATION_HOLD) {
        *p = stimulus == NOISE_SATURATION_HOLD   ? 0.62f
            : stimulus == NOISE_SATURATION_TRAIN ? 0.58f
                                                 : 0.0f;
        return 4U;
    }
    const bool holdout = stimulus == NOISE_MOTION_HOLD;
    const float crawl = holdout ? 0.035f : 0.05f;
    const uint32_t crawl_end = holdout ? 420000U : 330000U;
    const uint32_t ramp_us = holdout ? 210000U : 190000U;
    const uint32_t reversal = holdout ? 1120000U : 1020000U;
    const float accel = holdout ? 5.6f : 4.8f;
    const float duration_s = (float)ramp_us * 1.0e-6f;
    const float p0 = crawl * (float)crawl_end * 1.0e-6f;
    if (t < crawl_end) {
        *v = crawl;
        *p = crawl * (float)t * 1.0e-6f;
        return 0U;
    }
    if (t < crawl_end + ramp_us) {
        const float x = (float)(t - crawl_end) * 1.0e-6f;
        *v = crawl + accel * x;
        *p = p0 + crawl * x + 0.5f * accel * x * x;
        return 1U;
    }
    const float peak_v = crawl + accel * duration_s;
    const float p1 = p0 + crawl * duration_s + 0.5f * accel * duration_s * duration_s;
    if (t < crawl_end + 2U * ramp_us) {
        const float x = (float)(t - crawl_end - ramp_us) * 1.0e-6f;
        *v = peak_v * (1.0f - x / duration_s);
        *p = p1 + peak_v * x * (1.0f - 0.5f * x / duration_s);
        return 2U;
    }
    *p = p1 + 0.5f * peak_v * duration_s;
    if (t < reversal) {
        return 4U;
    }
    const float travel = holdout ? -0.28f : -0.16f;
    const uint32_t travel_us = holdout ? 330000U : 270000U;
    if (t < reversal + travel_us) {
        const float x = (float)(t - reversal) / (float)travel_us;
        *v = travel * 6.0f * x * (1.0f - x) * 1.0e6f / (float)travel_us;
        *p += travel * x * x * (3.0f - 2.0f * x);
        return 3U;
    }
    *p += travel;
    return 4U;
}

static void noise_record(
    struct noise_metrics *m,
    const double channel[NOISE_CHANNELS],
    uint32_t t,
    uint32_t end,
    uint32_t load_on,
    uint32_t load_off,
    unsigned stage,
    float reference,
    float speed,
    const struct foc_virtual_motor_state *plant,
    bool limited
)
{
    const double error = reference - plant->mechanical_position_rad;
    const double velocity = plant->mechanical_velocity_rad_s;
    for (unsigned i = 0; i < NOISE_CHANNELS; ++i) {
        m->lp[i] += 0.20 * (channel[i] - m->lp[i]);
        if (t >= end - tail_us) {
            const double hp = channel[i] - m->lp[i];
            m->sum_hp2[i] += hp * hp;
        }
    }
    if (stage < 4U) {
        const double v_error = speed - velocity;
        m->motion_samples[stage]++;
        m->motion_error2[stage] += v_error * v_error;
    }
    if (stage == 3U && m->reverse_us == UINT32_MAX && velocity <= -0.05) {
        m->reverse_us = t;
    }
    if (t < load_on && stage == 4U) {
        const double overshoot = t < 1000000U ? -error : error;
        m->peak_over_rad = fmax(m->peak_over_rad, overshoot);
    }
    if (t < load_off && t + 100000U >= load_off) {
        m->load_error_sum += error;
        m->load_samples++;
    }
    if (t >= load_off && fabs(error) > 0.008) {
        m->recover_us = t - load_off + period_us;
    }
    if (limited) {
        m->limit_run_us += period_us;
        m->longest_limit_us =
            m->longest_limit_us > m->limit_run_us ? m->longest_limit_us : m->limit_run_us;
        if (m->first_limit_us == UINT32_MAX) {
            m->first_limit_us = t;
        }
    } else {
        m->limit_run_us = 0U;
    }
    m->peak_iq_a = fmax(m->peak_iq_a, fabs(plant->iq_a));
    if (t >= end - tail_us) {
        m->tail_limit_count += limited;
        m->last_peak_error_rad = fmax(m->last_peak_error_rad, fabs(error));
        m->hold_samples++;
    }
}

static struct foc_virtual_current_profile noise_profile(double inertia_scale, float cap_a)
{
    struct foc_virtual_current_profile profile = foc_virtual_current_default_profile();
    profile.motor.inertia_kg_m2 *= inertia_scale;
    profile.current.current_limit_a = cap_a;
    profile.runtime.motion.iq_limit_a = cap_a;
    profile.runtime.motion.prediction.max_prediction_us = 600U;
    profile.runtime.motion.impedance = (struct foc_runtime_impedance_config){
        .enabled = true,
        .mechanical_frame_id = frame_id,
        .torque_constant_nm_per_a =
            (float)(1.5 * profile.motor.pole_pairs * profile.motor.psi_f_wb),
        .torque_limit_nm = 0.315f,
        .max_stiffness_nm_per_rad = 3.0f,
        .max_damping_nm_s_per_rad = 0.3f,
        .max_feedforward_nm = 0.315f,
    };
    return profile;
}

static int run_noise_case(const struct noise_case *test, char variant, float cutoff, bool trace)
{
    const struct foc_virtual_current_profile profile =
        noise_profile(test->inertia_scale, test->cap_a);
    struct foc_impedance_config gains;
    struct foc_impedance_tuning_candidate candidate;
    double j_hat, b_hat;
    const struct study_case design = {
        .design = test->matched ? MATCHED : NOMINAL,
        .cap_a = test->cap_a,
    };
    CHECK(design_candidate(&design, NULL, &profile, &gains, &candidate, &j_hat, &b_hat) == 0);
    if (variant == 'B') {
        gains.damping_nm_s_per_rad *= 0.75f;
    }

    struct foc_virtual_current vm;
    CHECK(start_case(&vm, &profile, &gains, NOISY) == 0);
    struct foc_encoder_context clean_encoder;
    CHECK(foc_encoder_init(&clean_encoder, &profile.encoder) == FOC_ENCODER_STATUS_OK);
    const uint32_t session = vm.runtime.motion_session;
    const uint32_t base_updates = vm.output.accepted_updates;
    const uint32_t end = test->stimulus == NOISE_HOLD ? 1600000U : 3000000U;
    const uint32_t load_on = test->stimulus == NOISE_HOLD ? UINT32_MAX
        : test->stimulus == NOISE_SATURATION_TRAIN        ? 850000U
        : test->stimulus == NOISE_SATURATION_HOLD         ? 930000U
                                                          : 1800000U;
    const uint32_t load_off = test->stimulus == NOISE_HOLD ? UINT32_MAX
        : test->stimulus == NOISE_SATURATION_TRAIN         ? 1450000U
        : test->stimulus == NOISE_SATURATION_HOLD          ? 1540000U
                                                           : 2260000U;
    uint32_t random = test->seed;
    double prior_position = vm.motor.state.mechanical_position_rad;
    uint32_t prior_capture = 0U;
    struct noise_trial filter = {0};
    struct noise_metrics m = {.reverse_us = UINT32_MAX, .first_limit_us = UINT32_MAX};
    for (uint32_t t = 0U; t < end; t += period_us) {
        const uint32_t delivery = epoch_us + t;
        const uint32_t capture = delivery - 1U - (test->jitter && (t / period_us) % 2U ? 36U : 0U);
        CHECK(foc_virtual_current_run_until(&vm, capture) == 0);
        if (t != 0U) {
            CHECK(
                vm.runtime.accepted_outer_seen && vm.runtime.accepted_outer_seq == vm.encoder_seq
            );
        }
        struct foc_runtime_sensor sample;
        uint16_t raw, clean;
        float raw_speed;
        CHECK(take_sample(&vm, NOISY, &random, &sample, &raw, &raw_speed) == 0);
        CHECK(foc_virtual_motor_make_encoder_code(&vm.motor, &profile.encoder, &clean) == 0);
        struct foc_encoder_feedback clean_sample;
        const int clean_status = foc_encoder_update(
            &clean_encoder,
            capture,
            clean,
            FOC_ENCODER_TRANSPORT_OK,
            &clean_sample
        );
        CHECK(t == 0U || (clean_status == FOC_ENCODER_STATUS_OK && clean_sample.speed_valid));
        const double position = vm.motor.state.mechanical_position_rad;
        const double true_delta_speed =
            t != 0U ? (position - prior_position) * 1.0e6 / (capture - prior_capture) : 0.0;
        prior_position = position;
        prior_capture = capture;

        float damping_speed = sample.mechanical_rad_s;
        if (variant == 'C') {
            CHECK(noise_trial_offer(&filter, &vm, &sample, session, cutoff) == 0 && filter.pending);
            damping_speed = filter.proposed.velocity_rad_s;
        }
        float reference, speed;
        const unsigned stage = noise_reference(test->stimulus, t, &reference, &speed);
        const float host_ff =
            gains.damping_nm_s_per_rad * (sample.mechanical_rad_s - damping_speed);
        /* Exact in real arithmetic. H1/H0 still see the original Sensor observation.
         * The Host-only correction enters through ordinary bounded feedforward. */
        CHECK(foc_virtual_current_run_until(&vm, delivery - 1U) == 0);
        CHECK(publish(&vm, &gains, reference, speed, host_ff) == FOC_RUNTIME_OK);
        CHECK(foc_runtime_h1_step(&vm.runtime, &sample, delivery, 0U) == FOC_RUNTIME_OK);
        const struct foc_runtime_sensor_slot *slot =
            &vm.runtime.sensor_slots[atomic_load(&vm.runtime.sensor_slot)];
        CHECK(
            slot->target_valid && slot->mode == FOC_RUNTIME_MODE_IMPEDANCE &&
            slot->session == session && slot->sample.seq == sample.seq &&
            slot->sample.captured_us == capture &&
            slot->sample.mechanical_rad_s == sample.mechanical_rad_s &&
            slot->sample.theta_elec_rad == sample.theta_elec_rad &&
            slot->electrical.angle_rad == sample.theta_elec_rad &&
            slot->electrical.velocity_rad_s ==
                (float)profile.motor.pole_pairs * sample.mechanical_rad_s &&
            slot->target.seq == vm.command_seq && slot->target.target_a.d == 0.0f &&
            fabsf(slot->target.target_a.q) <= test->cap_a
        );
        struct foc_impedance_input input = {
            .position_error_rad = reference - sample.mechanical_rad,
            .reference_velocity_rad_s = speed,
            .measured_velocity_rad_s = sample.mechanical_rad_s,
            .torque_feedforward_nm = host_ff,
            .live_iq_limit_a = test->cap_a,
        };
        struct foc_impedance_output runtime_torque, torque;
        CHECK(foc_impedance_step(&gains, &input, &runtime_torque) == FOC_STATUS_OK);
        CHECK(runtime_torque.iq_ref_a == slot->target.target_a.q);
        input.measured_velocity_rad_s = damping_speed;
        input.torque_feedforward_nm = 0.0f;
        CHECK(foc_impedance_step(&gains, &input, &torque) == FOC_STATUS_OK);
        CHECK(fabsf(runtime_torque.iq_ref_a - torque.iq_ref_a) <= 5.0e-7f);
        CHECK(fabsf(runtime_torque.requested_torque_nm - torque.requested_torque_nm) <= 8.0e-8f);

        /* Advance exactly to the first H0 seeing this target, even on odd H1 frames. */
        const uint32_t h0 = vm.next_h0_us;
        CHECK(foc_virtual_current_run_until(&vm, h0) == 0);
        CHECK(
            vm.runtime.pipeline.safety.state == FOC_SAFE_STATE_RUNNING &&
            vm.runtime.pipeline.safety.latched_faults == 0U &&
            vm.runtime.motion_session == session && vm.runtime.accepted_outer_seen &&
            vm.runtime.accepted_outer_seq == sample.seq &&
            vm.runtime.angle_prediction_us == h0 - sample.captured_us &&
            vm.runtime.observed_sensor.mechanical_rad_s == sample.mechanical_rad_s &&
            vm.runtime.observed_sensor.theta_elec_rad == sample.theta_elec_rad &&
            vm.runtime.pipeline.target_a.q == runtime_torque.iq_ref_a &&
            vm.runtime.pipeline.current.config.current_limit_a == test->cap_a &&
            vm.output.rejected_updates == 0U
        );
        if (t == load_on || t == load_off) {
            CHECK(foc_virtual_motor_set_load(&vm.motor, t == load_on ? 0.039 : 0.0) == 0);
        }
        const double kt = gains.torque_constant_nm_per_a;
        const struct foc_virtual_motor_state *plant = &vm.motor.state;
        const double channels[NOISE_CHANNELS] = {
            [NOISE_K] = gains.stiffness_nm_per_rad * input.position_error_rad,
            [NOISE_D] = gains.damping_nm_s_per_rad * (speed - damping_speed),
            [NOISE_FF] = input.torque_feedforward_nm,
            [NOISE_PRE_LIMIT] = runtime_torque.requested_torque_nm,
            [NOISE_POST_LIMIT] = runtime_torque.limited_torque_nm,
            [NOISE_REQUEST] = runtime_torque.iq_ref_a,
            [NOISE_MODEL_IQ] = plant->iq_a,
            [NOISE_RAW_SPEED_ERROR] = raw_speed - true_delta_speed,
            [NOISE_LPF_SPEED_ERROR] = sample.mechanical_rad_s - true_delta_speed,
            [NOISE_D_SPEED_ERROR] = damping_speed - true_delta_speed,
            [NOISE_QUANT_SPEED_ERROR] = clean_sample.raw_velocity_rad_s - true_delta_speed,
            [NOISE_CODE_SPEED_ERROR] = raw_speed - clean_sample.raw_velocity_rad_s,
            [NOISE_LIMIT_IQ_DELTA] =
                runtime_torque.iq_ref_a - runtime_torque.requested_torque_nm / kt,
            [NOISE_PI_IQ_DELTA] = plant->iq_a - runtime_torque.iq_ref_a,
            [NOISE_D_RAW] = gains.damping_nm_s_per_rad * (speed - sample.mechanical_rad_s),
            [NOISE_HOST_FF] = host_ff,
        };
        for (unsigned i = 0; i < NOISE_CHANNELS; ++i) {
            CHECK(isfinite(channels[i]));
        }
        noise_record(
            &m,
            channels,
            t,
            end,
            load_on,
            load_off,
            stage,
            reference,
            speed,
            plant,
            runtime_torque.limited
        );
        if (trace) {
            printf(
                "NOISE_TRACE,%s,%c,%.0f,%u,%u,%u,%u,%u,%u,%u,%u,%u,%.9g,%.9g,%.9g,%.9g,%u",
                test->name,
                variant,
                cutoff,
                t,
                capture,
                h0,
                session,
                sample.seq,
                vm.output.accepted_updates,
                (unsigned)raw,
                (unsigned)clean,
                (unsigned)runtime_torque.limited,
                reference,
                speed,
                plant->mechanical_position_rad,
                plant->mechanical_velocity_rad_s,
                stage
            );
            for (unsigned i = 0; i < NOISE_CHANNELS; ++i) {
                printf(",%.9g", channels[i]);
            }
            printf(
                ",%.9g,%.9g,%.9g,%.12g\n",
                sample.mechanical_rad_s,
                sample.mechanical_rad,
                sample.theta_elec_rad,
                true_delta_speed
            );
        }
    }
    printf(
        "NOISE_RESULT,%s,%c,%.0f,%u,%.9g,%.9g,%.9g,%u,%u,%u,%u,%u,%u,%.9g,%.9g,%.9g",
        test->name,
        variant,
        cutoff,
        test->seed,
        profile.motor.inertia_kg_m2,
        gains.stiffness_nm_per_rad,
        gains.damping_nm_s_per_rad,
        m.hold_samples,
        m.reverse_us,
        m.recover_us,
        m.first_limit_us,
        m.longest_limit_us,
        m.tail_limit_count,
        m.peak_over_rad,
        m.load_samples ? m.load_error_sum / m.load_samples : 0.0,
        m.last_peak_error_rad
    );
    printf(",%.9g,%u", m.peak_iq_a, vm.output.accepted_updates - base_updates);
    for (unsigned i = 0; i < 4U; ++i) {
        printf(",%.9g", m.motion_samples[i] ? sqrt(m.motion_error2[i] / m.motion_samples[i]) : 0.0);
    }
    for (unsigned i = 0; i < NOISE_CHANNELS; ++i) {
        printf(",%.9g", sqrt(m.sum_hp2[i] / m.hold_samples));
    }
    putchar('\n');
    return 0;
}

enum noise_boundary {
    NOISE_STOP,
    NOISE_ADAPTER_REJECT,
    NOISE_INVALID_SENSOR
};

static int test_noise_boundary(enum noise_boundary kind, float cutoff_hz, float motion_ff)
{
    const struct foc_virtual_current_profile profile = noise_profile(1.0, 2.2f);
    const struct foc_impedance_config gains = {
        .stiffness_nm_per_rad = 0.648f,
        .damping_nm_s_per_rad = 0.048f,
        .torque_constant_nm_per_a = 0.15f,
        .torque_limit_nm = 0.315f,
    };
    struct foc_virtual_current vm;
    CHECK(start_case(&vm, &profile, &gains, NOISY) == 0);
    struct noise_trial filter = {0};
    uint32_t random = 123047U;
    uint16_t raw;
    struct foc_runtime_sensor sample;
    const uint32_t session = vm.runtime.motion_session;
    CHECK(foc_virtual_current_run_until(&vm, 1999U) == 0);
    CHECK(take_sample(&vm, NOISY, &random, &sample, &raw, NULL) == 0);
    CHECK(noise_trial_offer(&filter, &vm, &sample, session, cutoff_hz) == 0);
    CHECK(!filter.accepted.ready && filter.proposed.ready && filter.pending);
    CHECK(publish(&vm, &gains, 0.03f, 0.0f, 0.0f) == FOC_RUNTIME_OK);
    CHECK(foc_runtime_h1_step(&vm.runtime, &sample, 2000U, 0U) == FOC_RUNTIME_OK);
    CHECK(foc_virtual_current_run_until(&vm, 2499U) == 0);
    CHECK(vm.runtime.accepted_outer_seq == sample.seq);
    const float accepted_speed = filter.proposed.velocity_rad_s;
    CHECK(take_sample(&vm, NOISY, &random, &sample, &raw, NULL) == 0);
    if (kind == NOISE_INVALID_SENSOR) {
        /* An invalid next sample acknowledges the prior H0 accept without
         * proposing a filter state of its own. */
        struct noise_trial acknowledged = filter;
        struct foc_runtime_sensor invalid = sample;
        invalid.mechanical_rad_s = NAN;
        CHECK(noise_trial_offer(&acknowledged, &vm, &invalid, session, cutoff_hz) == 0);
        CHECK(
            acknowledged.accepted.ready && acknowledged.accepted_us == 1999U &&
            acknowledged.accepted.velocity_rad_s == accepted_speed && !acknowledged.pending
        );
    }
    CHECK(noise_trial_offer(&filter, &vm, &sample, session, cutoff_hz) == 0);
    CHECK(
        filter.accepted.velocity_rad_s == accepted_speed && filter.accepted_us == 1999U &&
        filter.proposed_us == 2499U && filter.pending &&
        filter.proposed.velocity_rad_s != filter.accepted.velocity_rad_s
    );
    const float correction = motion_ff != 0.0f
        ? motion_ff
        : gains.damping_nm_s_per_rad * (sample.mechanical_rad_s - filter.proposed.velocity_rad_s);
    CHECK(publish(&vm, &gains, -0.02f, 0.0f, correction) == FOC_RUNTIME_OK);
    struct foc_runtime_command old_command =
        vm.runtime.command_slots[atomic_load(&vm.runtime.command_slot)];
    const uint32_t updates = vm.output.accepted_updates;
    const struct foc_vector_dq integral = vm.runtime.pipeline.current.integral_v;
    if (kind == NOISE_INVALID_SENSOR) {
        sample.valid = false;
        sample.mechanical_rad_s = NAN;
        CHECK(noise_trial_offer(&filter, &vm, &sample, session, cutoff_hz) == 0);
        CHECK(
            !filter.pending && filter.accepted.velocity_rad_s == accepted_speed &&
            filter.accepted_us == 1999U &&
            !damping_noise_filter_step(&filter.accepted, NAN, cutoff_hz, period_us) &&
            !damping_noise_filter_step(&filter.accepted, 0.0f, cutoff_hz, 0U) &&
            !damping_noise_filter_step(&filter.accepted, 0.0f, cutoff_hz, 2001U) &&
            filter.accepted.velocity_rad_s == accepted_speed
        );
        CHECK(foc_runtime_h1_step(&vm.runtime, &sample, 2500U, 0U) == FOC_RUNTIME_ERR_INPUT);
    } else {
        CHECK(foc_runtime_h1_step(&vm.runtime, &sample, 2500U, 0U) == FOC_RUNTIME_OK);
        CHECK(vm.runtime.accepted_outer_seq != filter.proposed_seq && filter.pending);
        if (kind == NOISE_STOP) {
            CHECK(foc_virtual_current_request_stop(&vm) == 0);
        } else {
            CHECK(foc_virtual_current_reject_next_update(&vm) == 0);
        }
    }
    CHECK(foc_virtual_current_run_until(&vm, 2600U) == 0);
    CHECK(vm.output.accepted_updates == updates);
    if (kind == NOISE_ADAPTER_REJECT) {
        CHECK(
            vm.output.rejected_updates == 1U &&
            vm.runtime.pipeline.current.integral_v.d == integral.d &&
            vm.runtime.pipeline.current.integral_v.q == integral.q
        );
    }
    /* Continued fresh observations isolate STOP and rejection from sample age. */
    for (uint32_t delivery = 3000U; delivery <= 4000U; delivery += period_us) {
        CHECK(foc_virtual_current_run_until(&vm, delivery - 1U) == 0);
        CHECK(take_sample(&vm, NOISY, &random, &sample, &raw, NULL) == 0);
        if (kind == NOISE_INVALID_SENSOR) {
            sample.valid = false;
            sample.mechanical_rad_s = NAN;
        }
        const int status = foc_runtime_h1_step(&vm.runtime, &sample, delivery, 0U);
        CHECK(status == (kind == NOISE_INVALID_SENSOR ? FOC_RUNTIME_ERR_INPUT : FOC_RUNTIME_OK));
        CHECK(foc_virtual_current_run_until(&vm, delivery) == 0);
    }
    CHECK(
        vm.output.accepted_updates == updates && vm.runtime.motion_session != session &&
        !vm.runtime.accepted_outer_seen && vm.output.request.action == FOC_DEVICE_OUTPUT_INHIBIT
    );
    if (kind == NOISE_INVALID_SENSOR) {
        CHECK(vm.runtime.pipeline.safety.state == FOC_SAFE_STATE_FAULT_LATCHED);
        CHECK((vm.runtime.pipeline.safety.latched_faults & FOC_SAFE_FAULT_SAMPLE_INVALID) != 0U);
    } else {
        CHECK(
            vm.runtime.pipeline.safety.state == FOC_SAFE_STATE_DISABLED &&
            vm.runtime.pipeline.safety.latched_faults == 0U
        );
    }
    CHECK(noise_trial_offer(&filter, &vm, &sample, session, cutoff_hz) == 0);
    CHECK(!filter.accepted.ready && !filter.proposed.ready && !filter.pending);
    old_command.seq++;
    old_command.produced_us = vm.virtual_time_us;
    const int expected =
        kind == NOISE_INVALID_SENSOR ? FOC_RUNTIME_ERR_CONTROL : FOC_RUNTIME_ERR_ORDER;
    CHECK(
        foc_runtime_publish_impedance_command(&vm.runtime, &old_command, vm.virtual_time_us) ==
        expected
    );
    CHECK(foc_virtual_current_run_until(&vm, 4200U) == 0 && vm.output.accepted_updates == updates);
    printf(
        "%s,%u,%u,%u,%u,%u,%u,%u",
        motion_ff == 0.0f ? "NOISE_BOUNDARY" : "MOTION_BOUNDARY",
        (unsigned)kind,
        updates,
        vm.output.accepted_updates,
        vm.output.rejected_updates,
        session,
        vm.runtime.motion_session,
        (unsigned)vm.runtime.pipeline.safety.latched_faults
    );
    if (motion_ff != 0.0f) {
        printf(",%.9g", motion_ff);
    }
    putchar('\n');
    return 0;
}

static int run_damping_noise_study(bool training, bool trace)
{
    static const struct noise_case train[] = {
        {"train-hold", NOISE_HOLD, 1.0, false, 71617U, 2.2f, false},
        {"train-load", NOISE_MOTION_TRAIN, 1.0, false, 71689U, 2.2f, false},
        {"train-sat", NOISE_SATURATION_TRAIN, 1.3, false, 61927U, 0.45f, false},
    };
    static const struct noise_case holdout[] = {
        {"hold-1x", NOISE_HOLD, 1.0, false, 891237U, 2.2f, true},
        {"hold-1p3x", NOISE_HOLD, 1.3, false, 637103U, 2.2f, true},
        {"hold-4x-matched", NOISE_HOLD, 4.0, true, 915331U, 2.2f, true},
        {"motion-1x", NOISE_MOTION_HOLD, 1.0, false, 371183U, 2.2f, true},
        {"motion-1p3x", NOISE_MOTION_HOLD, 1.3, false, 956311U, 2.2f, true},
        {"motion-4x-matched", NOISE_MOTION_HOLD, 4.0, true, 717329U, 2.2f, true},
        {"saturation-1p3x", NOISE_SATURATION_HOLD, 1.3, false, 442907U, 0.45f, true},
    };
    static const float sweep_hz[] = {12.0f, 24.0f, 48.0f};
    const float frozen_cutoff_hz = 48.0f; /* Highest training cutoff meeting all gates. */
    const struct noise_case *cases = training ? train : holdout;
    const size_t count =
        training ? sizeof train / sizeof train[0] : sizeof holdout / sizeof holdout[0];
    puts(
        "NOISE_RESULT,candidate,variant,cutoff_hz,seed,inertia,k,d,tail_samples,reversal_us,"
        "recovery_us,first_limit_us,longest_limit_us,tail_limit_count,over_rad,load_error_rad,"
        "tail_peak_error_rad,peak_model_iq_a,updates,crawl_error_rad_s,accel_error_rad_s,"
        "decel_error_rad_s,reverse_error_rad_s,k_hp_nm,d_hp_nm,ff_hp_nm,pre_limit_hp_nm,"
        "post_limit_hp_nm,request_hp_a,model_iq_hp_a,raw_error_hp_rad_s,lpf_error_hp_rad_s,"
        "damping_error_hp_rad_s,quant_error_hp_rad_s,code_error_hp_rad_s,limit_delta_hp_a,"
        "pi_delta_hp_a,d_raw_hp_nm,host_ff_hp_nm"
    );
    if (trace) {
        puts(
            "NOISE_TRACE,candidate,variant,cutoff_hz,t_us,capture_us,h0_us,session,sample_seq,"
            "updates,raw_code,clean_code,limited,reference_rad,reference_rad_s,model_rad,"
            "model_rad_s,motion_stage,k_nm,d_nm,ff_nm,pre_limit_nm,post_limit_nm,request_a,"
            "model_iq_a,raw_error_rad_s,lpf_error_rad_s,damping_error_rad_s,quant_error_rad_s,"
            "code_error_rad_s,limit_delta_a,pi_delta_a,d_raw_nm,host_ff_nm,sensor_velocity_rad_s,"
            "sensor_position_rad,sensor_phase_rad,true_delta_rad_s"
        );
    }
    for (size_t i = 0; i < count; ++i) {
        CHECK(run_noise_case(&cases[i], 'A', 0.0f, trace) == 0);
        CHECK(run_noise_case(&cases[i], 'B', 0.0f, trace) == 0);
        if (training) {
            for (size_t j = 0; j < sizeof sweep_hz / sizeof sweep_hz[0]; ++j) {
                CHECK(run_noise_case(&cases[i], 'C', sweep_hz[j], trace) == 0);
            }
        } else {
            CHECK(run_noise_case(&cases[i], 'C', frozen_cutoff_hz, trace) == 0);
        }
    }
    if (!training) {
        puts(
            "NOISE_BOUNDARY,kind,updates_before,updates_after,rejected,old_session,new_session,"
            "faults"
        );
        CHECK(test_noise_boundary(NOISE_STOP, frozen_cutoff_hz, 0.0f) == 0);
        CHECK(test_noise_boundary(NOISE_ADAPTER_REJECT, frozen_cutoff_hz, 0.0f) == 0);
        CHECK(test_noise_boundary(NOISE_INVALID_SENSOR, frozen_cutoff_hz, 0.0f) == 0);
    }
    return 0;
}

/* SERVO-02C Host Motion Feedforward Study ------------------------------------- */

enum motion_stage {
    MOTION_STILL,
    MOTION_CRAWL,
    MOTION_FORWARD_ACCEL,
    MOTION_FORWARD_DECEL,
    MOTION_REVERSE_ACCEL,
    MOTION_REVERSE_DECEL,
    MOTION_STAGE_COUNT
};

struct motion_segment {
    uint32_t start_us;
    uint32_t duration_us;
    double travel_rad;
};

struct motion_program {
    struct motion_segment crawl;
    struct motion_segment forward;
    struct motion_segment reverse;
    uint32_t end_us;
};

struct motion_reference {
    float position_rad;
    float velocity_rad_s;
    float acceleration_rad_s2;
    enum motion_stage stage;
};

struct motion_case {
    const char *name;
    const struct motion_program *program;
    double inertia_scale;
    float cap_a;
    enum study_sensor sensor;
    uint32_t seed;
    uint32_t capture_delay_us;
    uint32_t wait_us;
    uint32_t plant_delay_us;
    uint32_t delay_from_us;
    uint32_t load_on_us;
    uint32_t load_off_us;
    double load_nm;
    bool expect_gate;
    bool legacy_02b;
};

static struct motion_reference motion_segment_at(
    const struct motion_segment *segment,
    uint32_t t,
    double initial_rad,
    enum motion_stage accelerate,
    enum motion_stage decelerate
)
{
    if (t < segment->start_us) {
        return (struct motion_reference){.position_rad = (float)initial_rad};
    }
    const uint32_t elapsed = t - segment->start_us;
    if (elapsed >= segment->duration_us) {
        return (struct motion_reference){
            .position_rad = (float)(initial_rad + segment->travel_rad)
        };
    }
    const double duration = (double)segment->duration_us * 1.0e-6;
    const double u = (double)elapsed / segment->duration_us;
    const double w = 1.0 - u;
    return (struct motion_reference){
        .position_rad =
            (float)(initial_rad + segment->travel_rad * u * u * u * (10.0 + u * (6.0 * u - 15.0))),
        .velocity_rad_s = (float)(30.0 * segment->travel_rad * u * u * w * w / duration),
        .acceleration_rad_s2 =
            (float)(60.0 * segment->travel_rad * u * w * (1.0 - 2.0 * u) / (duration * duration)),
        .stage = u < 0.5 ? accelerate : decelerate,
    };
}

/* One analytical position law and its first two derivatives at one command time.
 * Each leg starts and ends with exactly zero velocity and acceleration. */
static struct motion_reference motion_reference_at(const struct motion_program *p, uint32_t t)
{
    if (t < p->forward.start_us) {
        return motion_segment_at(&p->crawl, t, 0.0, MOTION_CRAWL, MOTION_CRAWL);
    }
    if (t < p->reverse.start_us) {
        return motion_segment_at(
            &p->forward,
            t,
            p->crawl.travel_rad,
            MOTION_FORWARD_ACCEL,
            MOTION_FORWARD_DECEL
        );
    }
    return motion_segment_at(
        &p->reverse,
        t,
        p->crawl.travel_rad + p->forward.travel_rad,
        MOTION_REVERSE_ACCEL,
        MOTION_REVERSE_DECEL
    );
}

static struct motion_reference motion_legacy_reference_at(uint32_t t)
{
    struct motion_reference ref = {0};
    const unsigned stage =
        noise_reference(NOISE_MOTION_HOLD, t, &ref.position_rad, &ref.velocity_rad_s);
    static const enum motion_stage current_stage[] = {
        MOTION_CRAWL,
        MOTION_FORWARD_ACCEL,
        MOTION_FORWARD_DECEL,
        MOTION_REVERSE_ACCEL,
        MOTION_STILL
    };
    ref.stage = current_stage[stage];
    if (stage == 1U) {
        ref.acceleration_rad_s2 = 5.6f;
    } else if (stage == 2U) {
        ref.acceleration_rad_s2 = -(0.035f + 5.6f * 0.210f) / 0.210f;
    } else if (stage == 3U) {
        const float u = (float)(t - 1120000U) / 330000.0f;
        ref.acceleration_rad_s2 = -0.28f * 6.0f * (1.0f - 2.0f * u) / (0.330f * 0.330f);
        ref.stage = u < 0.5f ? MOTION_REVERSE_ACCEL : MOTION_REVERSE_DECEL;
    }
    return ref;
}

static int motion_request_ff(
    char variant,
    const struct foc_virtual_motor_params *actual,
    const struct motion_reference *reference,
    float limit_nm,
    bool legacy_02b,
    float *torque_nm
)
{
    if (variant == 'B') {
        *torque_nm = 0.0f;
        return 0;
    }
    const bool oracle = variant == 'C';
    const double j = oracle ? actual->inertia_kg_m2 : 0.002001755405;
    const double b = oracle ? actual->damping_nm_s : 0.00599673437;
    if (legacy_02b) {
        /* Diagnostic only: the original reference has acceleration discontinuities.
         * Use the segment acceleration to isolate missing motion torque in Host. */
        *torque_nm = (float)(j * reference->acceleration_rad_s2 + b * reference->velocity_rad_s);
        return 0;
    }
    const struct foc_impedance_feedforward_input input = {
        .inertia_kg_m2 = (float)j,
        .viscous_damping_nm_s_per_rad = (float)b,
        .reference_velocity_rad_s = reference->velocity_rad_s,
        .reference_acceleration_rad_s2 = reference->acceleration_rad_s2,
        .limit_nm = limit_nm,
        .model_id = oracle ? 101U : 100U,
        .model_frame_id = frame_id,
        .reference_frame_id = frame_id,
        .reference_qualified = true,
    };
    struct foc_impedance_feedforward_result output;
    CHECK(foc_impedance_feedforward_evaluate(&input, &output) == 0 && output.valid);
    CHECK(output.model_id == input.model_id && output.mechanical_frame_id == frame_id);
    CHECK(fabsf(output.torque_nm) <= limit_nm);
    *torque_nm = output.torque_nm;
    return 0;
}

struct motion_metrics {
    double velocity_error2[MOTION_STAGE_COUNT];
    double position_error2;
    double peak_required_nm;
    double peak_ff_nm;
    double peak_feedback_nm;
    double peak_request_a;
    double peak_model_a;
    double tail_peak_error;
    double torque_residual2;
    double q_lp;
    double iq_lp;
    double tail_q_hp2;
    double tail_iq_hp2;
    double reverse_q_hp2;
    uint32_t stage_count[MOTION_STAGE_COUNT];
    uint32_t frames;
    uint32_t tail_count;
    uint32_t reverse_count;
    uint32_t crossed_us;
    uint32_t settle_us;
    uint32_t recover_us;
    uint32_t over_budget_us;
    uint32_t limited_us;
    uint32_t limit_run_us;
    uint32_t peak_limit_run_us;
    uint32_t positive_ff_count;
    uint32_t negative_ff_count;
};

static void record_motion(
    struct motion_metrics *m,
    const struct motion_case *test,
    const struct motion_reference *ref,
    const struct foc_virtual_motor_state *plant,
    const struct foc_impedance_output *torque,
    double feedforward_nm,
    double required_nm,
    double electromechanical_nm,
    double capacity_nm,
    uint32_t t
)
{
    const double position_error = ref->position_rad - plant->mechanical_position_rad;
    const double velocity_error = ref->velocity_rad_s - plant->mechanical_velocity_rad_s;
    m->velocity_error2[ref->stage] += velocity_error * velocity_error;
    m->stage_count[ref->stage]++;
    m->position_error2 += position_error * position_error;
    m->frames++;
    m->peak_required_nm = fmax(m->peak_required_nm, fabs(required_nm));
    m->peak_ff_nm = fmax(m->peak_ff_nm, fabs(feedforward_nm));
    m->peak_feedback_nm =
        fmax(m->peak_feedback_nm, fabs(torque->requested_torque_nm - feedforward_nm));
    m->peak_request_a = fmax(m->peak_request_a, fabs(torque->iq_ref_a));
    m->peak_model_a = fmax(m->peak_model_a, fabs(plant->iq_a));
    m->positive_ff_count += feedforward_nm > 0.0;
    m->negative_ff_count += feedforward_nm < 0.0;
    if (fabs(required_nm) > capacity_nm) {
        m->over_budget_us += period_us;
    }
    if (torque->limited) {
        m->limited_us += period_us;
        m->limit_run_us += period_us;
        if (m->limit_run_us > m->peak_limit_run_us) {
            m->peak_limit_run_us = m->limit_run_us;
        }
    } else {
        m->limit_run_us = 0U;
    }
    m->q_lp += 0.20 * (torque->iq_ref_a - m->q_lp);
    m->iq_lp += 0.20 * (plant->iq_a - m->iq_lp);
    const double q_hp = torque->iq_ref_a - m->q_lp;
    const double iq_hp = plant->iq_a - m->iq_lp;
    if (t >= test->program->end_us - tail_us) {
        m->tail_peak_error = fmax(m->tail_peak_error, fabs(position_error));
        m->tail_q_hp2 += q_hp * q_hp;
        m->tail_iq_hp2 += iq_hp * iq_hp;
        m->tail_count++;
    }
    if (ref->stage == MOTION_REVERSE_ACCEL || ref->stage == MOTION_REVERSE_DECEL) {
        const double residual = required_nm - electromechanical_nm;
        m->torque_residual2 += residual * residual;
        m->reverse_q_hp2 += q_hp * q_hp;
        m->reverse_count++;
        if (m->crossed_us == UINT32_MAX && plant->mechanical_velocity_rad_s <= -0.05) {
            m->crossed_us = t - test->program->reverse.start_us;
        }
    }
    const bool within = fabs(position_error) <= 0.008;
    const uint32_t reverse_end =
        test->program->reverse.start_us + test->program->reverse.duration_us;
    if (t >= reverse_end && (test->load_on_us == 0U || t < test->load_on_us) && !within) {
        m->settle_us = t - reverse_end + period_us;
    }
    if (test->load_off_us != 0U && t >= test->load_off_us && !within) {
        m->recover_us = t - test->load_off_us + period_us;
    }
}

static int run_motion_case(const struct motion_case *test, char variant, bool trace)
{
    const struct foc_virtual_current_profile profile =
        noise_profile(test->inertia_scale, test->cap_a);
    const struct study_case design = {.design = NOMINAL, .cap_a = test->cap_a};
    struct foc_impedance_config gains;
    struct foc_impedance_tuning_candidate tuned;
    double tuned_j, tuned_b;
    CHECK(design_candidate(&design, NULL, &profile, &gains, &tuned, &tuned_j, &tuned_b) == 0);
    struct foc_virtual_current vm;
    const enum study_sensor sensor = test->sensor;
    CHECK(start_case(&vm, &profile, &gains, sensor) == 0);
    struct output_delay output = {
        .vm = &vm,
        .delay_us = test->plant_delay_us,
        .submit = vm.runtime.pipeline.output_submit,
        .submit_arg = vm.runtime.pipeline.output_arg,
    };
    vm.runtime.pipeline.output_submit = submit_delayed;
    vm.runtime.pipeline.output_arg = &output;
    const uint32_t session = vm.runtime.motion_session;
    const uint32_t base_updates = vm.output.accepted_updates;
    const float kt = gains.torque_constant_nm_per_a;
    const double capacity = fmin(gains.torque_limit_nm, kt * test->cap_a);
    uint32_t random = test->seed;
    struct motion_metrics m = {.crossed_us = UINT32_MAX};
    for (uint32_t t = 0U; t < test->program->end_us; t += period_us) {
        const uint32_t deadline = epoch_us + t;
        const bool delayed = t >= test->delay_from_us;
        const uint32_t early = test->legacy_02b ? 36U * ((t / period_us) % 2U)
            : delayed                           ? test->capture_delay_us
                                                : 0U;
        const uint32_t capture = deadline - 1U - early;
        const uint32_t delivery = deadline + (delayed ? test->wait_us : 0U);
        CHECK(advance(&output, capture) == 0);
        if (output.inhibited) {
            break;
        }
        struct foc_runtime_sensor sample;
        uint16_t raw;
        CHECK(take_sample(&vm, sensor, &random, &sample, &raw, NULL) == 0);
        const struct motion_reference ref = test->legacy_02b
            ? motion_legacy_reference_at(t)
            : motion_reference_at(test->program, t);
        CHECK(
            fabsf(ref.velocity_rad_s) <= 2.0f && fabsf(ref.acceleration_rad_s2) <= 20.0f &&
            isfinite(ref.position_rad)
        );
        float torque_ff;
        CHECK(
            motion_request_ff(
                variant,
                &profile.motor,
                &ref,
                profile.runtime.motion.impedance.max_feedforward_nm,
                test->legacy_02b,
                &torque_ff
            ) == 0
        );
        CHECK(advance(&output, delivery - 1U) == 0);
        if (output.inhibited) {
            break;
        }
        CHECK(
            publish(&vm, &gains, ref.position_rad, ref.velocity_rad_s, torque_ff) == FOC_RUNTIME_OK
        );
        CHECK(foc_runtime_h1_step(&vm.runtime, &sample, delivery, 0U) == FOC_RUNTIME_OK);
        const struct foc_runtime_sensor_slot *slot =
            &vm.runtime.sensor_slots[atomic_load(&vm.runtime.sensor_slot)];
        CHECK(
            slot->target_valid && slot->mode == FOC_RUNTIME_MODE_IMPEDANCE &&
            slot->session == session && slot->sample.seq == sample.seq &&
            slot->sample.captured_us == capture && slot->target.seq == vm.command_seq &&
            slot->target.produced_us == delivery - 1U &&
            slot->electrical.velocity_rad_s ==
                (float)profile.motor.pole_pairs * sample.mechanical_rad_s
        );
        const struct foc_impedance_input input = {
            .position_error_rad = ref.position_rad - sample.mechanical_rad,
            .reference_velocity_rad_s = ref.velocity_rad_s,
            .measured_velocity_rad_s = sample.mechanical_rad_s,
            .torque_feedforward_nm = torque_ff,
            .live_iq_limit_a = test->cap_a,
        };
        struct foc_impedance_output torque;
        CHECK(foc_impedance_step(&gains, &input, &torque) == FOC_STATUS_OK && torque.valid);
        CHECK(slot->target.target_a.d == 0.0f && slot->target.target_a.q == torque.iq_ref_a);
        const uint32_t h0 = vm.next_h0_us;
        CHECK(advance(&output, h0) == 0);
        if (output.inhibited) {
            break;
        }
        CHECK(
            vm.runtime.pipeline.safety.state == FOC_SAFE_STATE_RUNNING &&
            vm.runtime.pipeline.safety.latched_faults == 0U &&
            vm.runtime.motion_session == session && vm.runtime.accepted_outer_seq == sample.seq &&
            vm.runtime.angle_prediction_us == h0 - capture &&
            vm.runtime.pipeline.target_a.q == torque.iq_ref_a &&
            fabsf(torque.iq_ref_a) <= test->cap_a &&
            vm.output.accepted_updates == vm.runtime.pipeline.mock.updates &&
            vm.output.rejected_updates == 0U
        );
        const struct foc_virtual_motor_state *plant = &vm.motor.state;
        const double p = profile.motor.pole_pairs;
        const double electromagnetic = 1.5 * p *
            (profile.motor.psi_f_wb * plant->iq_a +
             (profile.motor.ld_h - profile.motor.lq_h) * plant->id_a * plant->iq_a);
        const double actual_acceleration =
            (electromagnetic - profile.motor.damping_nm_s * plant->mechanical_velocity_rad_s -
             vm.motor.load_torque_nm) /
            profile.motor.inertia_kg_m2;
        const double required = profile.motor.inertia_kg_m2 * ref.acceleration_rad_s2 +
            profile.motor.damping_nm_s * ref.velocity_rad_s + vm.motor.load_torque_nm;
        CHECK(isfinite(actual_acceleration) && isfinite(required));
        record_motion(
            &m,
            test,
            &ref,
            plant,
            &torque,
            torque_ff,
            required,
            electromagnetic,
            capacity,
            t
        );
        if (trace) {
            printf(
                "MOTION_TRACE,%s,%c,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u",
                test->name,
                variant,
                t,
                capture,
                delivery,
                h0,
                vm.runtime.angle_prediction_us,
                output.last_submitted_us,
                output.last_applied_us,
                sample.seq,
                session,
                vm.output.accepted_updates,
                (unsigned)ref.stage,
                (unsigned)raw
            );
            printf(
                ",%.9g,%.9g,%.9g,%.12g,%.12g,%.12g,%.12g,%.9g,%.9g",
                ref.position_rad,
                ref.velocity_rad_s,
                ref.acceleration_rad_s2,
                plant->mechanical_position_rad,
                plant->mechanical_velocity_rad_s,
                actual_acceleration,
                ref.velocity_rad_s - plant->mechanical_velocity_rad_s,
                sample.mechanical_rad_s,
                sample.theta_elec_rad
            );
            printf(
                ",%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.12g,%.12g,%.12g,%.12g,%.9g,%u,%u,%u\n",
                gains.stiffness_nm_per_rad * input.position_error_rad,
                gains.damping_nm_s_per_rad *
                    (input.reference_velocity_rad_s - input.measured_velocity_rad_s),
                torque_ff,
                torque.requested_torque_nm,
                torque.limited_torque_nm,
                torque.iq_ref_a,
                plant->iq_a,
                plant->id_a,
                electromagnetic,
                required,
                vm.motor.load_torque_nm,
                (unsigned)torque.limited,
                (unsigned)(fabs(required) > capacity),
                (unsigned)output.inhibited
            );
        }
        if (test->load_on_us != 0U && (t == test->load_on_us || t == test->load_off_us)) {
            CHECK(
                foc_virtual_motor_set_load(
                    &vm.motor,
                    t == test->load_on_us ? test->load_nm : 0.0
                ) == 0
            );
        }
    }
    if (test->expect_gate) {
        const uint32_t updates = vm.output.accepted_updates;
        CHECK(
            output.inhibited && output.inhibit_reason == FOC_PIPELINE_ERR_SAMPLE &&
            vm.runtime.motion_session != session && !output.pending
        );
        CHECK(advance(&output, vm.virtual_time_us + 800U) == 0);
        CHECK(
            vm.output.accepted_updates == updates &&
            vm.output.request.action == FOC_DEVICE_OUTPUT_INHIBIT &&
            (vm.runtime.pipeline.safety.latched_faults & FOC_SAFE_FAULT_SAMPLE_INVALID) != 0U
        );
        printf(
            "MOTION_GATE,%s,%c,%u,%d,%u,%u,%u,%u,%u,%.9g\n",
            test->name,
            variant,
            output.inhibit_us,
            output.inhibit_reason,
            updates,
            vm.output.accepted_updates,
            session,
            vm.runtime.motion_session,
            output.max_source_age_us,
            m.peak_ff_nm
        );
        return 0;
    }
    CHECK(
        !output.inhibited && m.frames == test->program->end_us / period_us && m.tail_count == 600U
    );
    CHECK(m.reverse_count == test->program->reverse.duration_us / period_us);
    CHECK(variant == 'B' || (m.positive_ff_count > 0U && m.negative_ff_count > 0U));
    if (test->cap_a < 1.0f) {
        CHECK(m.limited_us > 0U && m.over_budget_us > 0U);
    }
    double total_error2 = 0.0;
    for (unsigned i = 0; i < MOTION_STAGE_COUNT; ++i) {
        CHECK(m.stage_count[i] > 0U);
        total_error2 += m.velocity_error2[i];
    }
    const double reverse_error2 =
        m.velocity_error2[MOTION_REVERSE_ACCEL] + m.velocity_error2[MOTION_REVERSE_DECEL];
    printf(
        "MOTION_RESULT,%s,%c,%u,%.9g,%.9g,%.9g,%.9g,%u,%.9g,%.9g,%.9g",
        test->name,
        variant,
        test->seed,
        profile.motor.inertia_kg_m2,
        profile.motor.damping_nm_s,
        test->cap_a,
        capacity,
        m.frames,
        sqrt(total_error2 / m.frames),
        sqrt(m.position_error2 / m.frames),
        sqrt(reverse_error2 / m.reverse_count)
    );
    for (unsigned i = 0; i < MOTION_STAGE_COUNT; ++i) {
        printf(",%.9g", sqrt(m.velocity_error2[i] / m.stage_count[i]));
    }
    printf(
        ",%u,%u,%u,%u,%u,%u,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%u,%u,%u,%u\n",
        m.crossed_us,
        m.settle_us,
        m.recover_us,
        m.over_budget_us,
        m.limited_us,
        m.peak_limit_run_us,
        m.peak_required_nm,
        m.peak_ff_nm,
        m.peak_feedback_nm,
        m.peak_request_a,
        m.peak_model_a,
        m.tail_peak_error,
        sqrt(m.tail_q_hp2 / m.tail_count),
        sqrt(m.tail_iq_hp2 / m.tail_count),
        sqrt(m.reverse_q_hp2 / m.reverse_count),
        sqrt(m.torque_residual2 / m.reverse_count),
        m.positive_ff_count,
        m.negative_ff_count,
        vm.output.accepted_updates - base_updates,
        output.max_source_age_us
    );
    return 0;
}

enum motion_dataset {
    MOTION_TRAINING,
    MOTION_HOLDOUT,
    MOTION_LEGACY
};

static int run_motion_feedforward_study(enum motion_dataset dataset, bool trace)
{
    static const struct motion_program training = {
        .crawl = {60000U, 330000U, 0.012},
        .forward = {450000U, 360000U, 0.18},
        .reverse = {1100000U, 360000U, -0.25},
        .end_us = 2500000U,
    };
    static const struct motion_program holdout = {
        .crawl = {40000U, 400000U, 0.0115},
        .forward = {510000U, 420000U, 0.206},
        .reverse = {1120000U, 330000U, -0.28},
        .end_us = 3000000U,
    };
    static const struct motion_program legacy = {
        .reverse = {1120000U, 330000U, -0.28},
        .end_us = 3000000U,
    };
    static const struct motion_case train[] = {
        {
            .name = "train-1x",
            .program = &training,
            .inertia_scale = 1.0,
            .cap_a = 2.2f,
            .sensor = NOISY,
            .seed = 87351U,
        },
        {
            .name = "train-1p3x",
            .program = &training,
            .inertia_scale = 1.3,
            .cap_a = 2.2f,
            .sensor = NOISY,
            .seed = 92013U,
        },
    };
    static const struct motion_case held[] = {
        {
            .name = "hold-1x",
            .program = &holdout,
            .inertia_scale = 1.0,
            .cap_a = 2.2f,
            .sensor = QUANTIZED,
        },
        {
            .name = "hold-1p3x",
            .program = &holdout,
            .inertia_scale = 1.3,
            .cap_a = 2.2f,
            .sensor = QUANTIZED,
        },
        {
            .name = "hold-noisy-1p3x",
            .program = &holdout,
            .inertia_scale = 1.3,
            .cap_a = 2.2f,
            .sensor = NOISY,
            .seed = 430097U,
        },
        {
            .name = "hold-load-legal-delay",
            .program = &holdout,
            .inertia_scale = 1.3,
            .cap_a = 2.2f,
            .sensor = QUANTIZED,
            .capture_delay_us = 99U,
            .wait_us = 80U,
            .plant_delay_us = 120U,
            .load_on_us = 1800000U,
            .load_off_us = 2240000U,
            .load_nm = 0.039,
        },
        {
            .name = "hold-lowcap-1p3x",
            .program = &holdout,
            .inertia_scale = 1.3,
            .cap_a = 0.16f,
            .sensor = QUANTIZED,
        },
        {
            .name = "hold-over-age",
            .program = &holdout,
            .inertia_scale = 1.3,
            .cap_a = 2.2f,
            .sensor = NOISY,
            .seed = 304441U,
            .capture_delay_us = 299U,
            .wait_us = 80U,
            .plant_delay_us = 120U,
            .delay_from_us = 1200000U,
            .expect_gate = true,
        },
    };
    static const struct motion_case legacy_case = {
        .name = "02b-reference-probe",
        .program = &legacy,
        .inertia_scale = 1.3,
        .cap_a = 2.2f,
        .sensor = NOISY,
        .seed = 956311U,
        .load_on_us = 1800000U,
        .load_off_us = 2260000U,
        .load_nm = 0.039,
        .legacy_02b = true,
    };
    puts(
        "MOTION_RESULT,case,variant,seed,plant_j,plant_b,iq_limit_a,capacity_nm,frames,"
        "velocity_rms_rad_s,position_rms_rad,reversal_rms_rad_s,still_rms_rad_s,crawl_rms_rad_s,"
        "forward_accel_rms_rad_s,forward_decel_rms_rad_s,reverse_accel_rms_rad_s,reverse_decel_rms_"
        "rad_s,"
        "crossed_us,settle_us,recover_us,over_budget_us,limited_us,peak_limit_run_us,peak_required_"
        "nm,"
        "peak_ff_nm,peak_feedback_nm,peak_request_a,peak_model_a,tail_peak_position_error_rad,"
        "tail_request_hp_a,tail_model_hp_a,reverse_request_hp_a,reversal_torque_residual_rms_nm,"
        "positive_ff_frames,negative_ff_frames,updates,max_accepted_source_age_us"
    );
    puts(
        "MOTION_GATE,case,variant,inhibit_us,reason,updates_before,updates_after,old_session,"
        "new_session,max_accepted_source_age_us,peak_ff_nm"
    );
    if (trace) {
        puts(
            "MOTION_TRACE,case,variant,t_us,capture_us,h1_us,h0_us,prediction_age_us,"
            "latest_submit_us,latest_applied_us,sample_seq,session,updates,stage,encoder_raw,"
            "ref_position_rad,ref_velocity_rad_s,ref_acceleration_rad_s2,model_position_rad,"
            "model_velocity_rad_s,model_acceleration_rad_s2,velocity_error_rad_s,sensor_velocity_"
            "rad_s,"
            "sensor_phase_rad,k_nm,d_nm,feedforward_nm,request_nm,limited_nm,iq_request_a,model_iq_"
            "a,"
            "model_id_a,electromagnetic_nm,reference_required_nm,load_nm,limited,reference_over_"
            "budget,inhibit"
        );
    }
    if (dataset == MOTION_LEGACY) {
        CHECK(run_motion_case(&legacy_case, 'B', trace) == 0);
        CHECK(run_motion_case(&legacy_case, 'C', trace) == 0);
        return 0;
    }
    const struct motion_case *cases = dataset == MOTION_TRAINING ? train : held;
    const size_t count =
        dataset == MOTION_TRAINING ? sizeof train / sizeof train[0] : sizeof held / sizeof held[0];
    for (size_t i = 0; i < count; ++i) {
        for (char variant = 'B'; variant <= 'D'; ++variant) {
            CHECK(run_motion_case(&cases[i], variant, trace) == 0);
        }
    }
    if (dataset == MOTION_HOLDOUT) {
        const struct foc_virtual_current_profile profile = noise_profile(1.3, 2.2f);
        const struct motion_reference ref = motion_reference_at(
            &holdout,
            holdout.reverse.start_us + holdout.reverse.duration_us / 4U
        );
        float motion_ff;
        CHECK(motion_request_ff('D', &profile.motor, &ref, 0.315f, false, &motion_ff) == 0);
        CHECK(motion_ff < -0.01f);
        puts(
            "MOTION_BOUNDARY,kind,updates_before,updates_after,rejected,old_session,new_session,"
            "faults,feedforward_nm"
        );
        CHECK(test_noise_boundary(NOISE_STOP, 48.0f, motion_ff) == 0);
        CHECK(test_noise_boundary(NOISE_ADAPTER_REJECT, 48.0f, motion_ff) == 0);
        CHECK(test_noise_boundary(NOISE_INVALID_SENSOR, 48.0f, motion_ff) == 0);
    }
    return 0;
}

/* Test Entry ------------------------------------------------------------------- */

#define INERTIA_CASES(label, scale)                                                          \
    {label "-fixed", scale, FIXED, QUANTIZED, LOAD, 0.30f, 2.2f, 0, 0, 0, 0, false},         \
        {label "-matched", scale, MATCHED, QUANTIZED, LOAD, 0.30f, 2.2f, 0, 0, 0, 0, false}, \
    {                                                                                        \
        label "-biased", scale, BIASED, QUANTIZED, LOAD, 0.30f, 2.2f, 0, 0, 0, 0, false      \
    }

/* Frozen, independently acquired ID candidates are provided by the Host runner. */
static int run_mechanical_id_comparison(int argc, char **argv)
{
    static const struct study_case cases[] = {
        {"jb-nominal-A", 1.0, FIXED, QUANTIZED, REVERSE_LOAD, 0.25f, 2.2f, 0, 0, 0, 0, false},
        {"jb-nominal-B", 1.0, MATCHED, QUANTIZED, REVERSE_LOAD, 0.25f, 2.2f, 0, 0, 0, 0, false},
        {"jb-nominal-C", 1.0, NOMINAL, QUANTIZED, REVERSE_LOAD, 0.25f, 2.2f, 0, 0, 0, 0, false},
        {"jb-quad-A", 4.0, FIXED, QUANTIZED, REVERSE_LOAD, 0.25f, 2.2f, 0, 0, 0, 0, false},
        {"jb-quad-B", 4.0, MATCHED, QUANTIZED, REVERSE_LOAD, 0.25f, 2.2f, 0, 0, 0, 0, false},
        {"jb-quad-C", 4.0, NOMINAL, QUANTIZED, REVERSE_LOAD, 0.25f, 2.2f, 0, 0, 0, 0, false},
        {"jb-delay-A", 4.0, FIXED, QUANTIZED, REVERSE_LOAD, 0.25f, 2.2f, 99U, 80U, 120U, 0, false},
        {"jb-delay-B",
         4.0,
         MATCHED,
         QUANTIZED,
         REVERSE_LOAD,
         0.25f,
         2.2f,
         99U,
         80U,
         120U,
         0,
         false},
        {"jb-delay-C",
         4.0,
         NOMINAL,
         QUANTIZED,
         REVERSE_LOAD,
         0.25f,
         2.2f,
         99U,
         80U,
         120U,
         0,
         false},
        {"jb-noise-A", 4.0, FIXED, NOISY, STEP, 0.0f, 2.2f, 0, 0, 0, 34027U, false},
        {"jb-noise-B", 4.0, MATCHED, NOISY, STEP, 0.0f, 2.2f, 0, 0, 0, 34027U, false},
        {"jb-noise-C", 4.0, NOMINAL, NOISY, STEP, 0.0f, 2.2f, 0, 0, 0, 34027U, false},
        {"jb-cap-A", 4.0, FIXED, QUANTIZED, REVERSE_LOAD, 0.65f, 0.45f, 0, 0, 0, 0, false},
        {"jb-cap-B", 4.0, MATCHED, QUANTIZED, REVERSE_LOAD, 0.65f, 0.45f, 0, 0, 0, 0, false},
        {"jb-cap-C", 4.0, NOMINAL, QUANTIZED, REVERSE_LOAD, 0.65f, 0.45f, 0, 0, 0, 0, false},
        {"jb-invalid-age-C", 4.0, NOMINAL, QUANTIZED, STEP, 0.30f, 2.2f, 299U, 80U, 120U, 0, true},
    };
    const bool trace = argc == 7 && strcmp(argv[6], "--trace") == 0;
    if (argc != 6 && !trace) {
        return 2;
    }
    struct study_model models[2];
    for (size_t i = 0; i < sizeof models / sizeof models[0]; ++i) {
        const char *const j = argv[2U + 2U * i];
        const char *const b = argv[3U + 2U * i];
        char *end_j, *end_b;
        models[i].inertia = strtod(j, &end_j);
        models[i].damping = strtod(b, &end_b);
        if (end_j == j || end_b == b || *end_j != '\0' || *end_b != '\0' ||
            !isfinite(models[i].inertia) || !isfinite(models[i].damping) ||
            models[i].inertia <= 0.0 || models[i].damping < 0.0) {
            return 2;
        }
    }
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        const struct study_case *test = &cases[i];
        const struct study_model *model =
            test->design == NOMINAL ? &models[test->inertia_scale == 4.0 ? 1U : 0U] : NULL;
        CHECK(run_case(test, model, trace) == 0);
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 &&
        (strcmp(argv[1], "--motion-feedforward-train") == 0 ||
         strcmp(argv[1], "--motion-feedforward-holdout") == 0 ||
         strcmp(argv[1], "--motion-feedforward-legacy") == 0)) {
        const bool trace = argc == 3 && strcmp(argv[2], "--trace") == 0;
        if (argc != 2 && !trace) {
            return 2;
        }
        const enum motion_dataset dataset = strcmp(argv[1], "--motion-feedforward-train") == 0
            ? MOTION_TRAINING
            : strcmp(argv[1], "--motion-feedforward-holdout") == 0 ? MOTION_HOLDOUT
                                                                   : MOTION_LEGACY;
        return run_motion_feedforward_study(dataset, trace);
    }
    if (argc > 1 &&
        (strcmp(argv[1], "--damping-noise-train") == 0 ||
         strcmp(argv[1], "--damping-noise-holdout") == 0)) {
        const bool trace = argc == 3 && strcmp(argv[2], "--trace") == 0;
        if (argc != 2 && !trace) {
            return 2;
        }
        return run_damping_noise_study(strcmp(argv[1], "--damping-noise-train") == 0, trace);
    }
    static const struct study_case training[] = {
        INERTIA_CASES("j05", 0.5),
        INERTIA_CASES("j10", 1.0),
        INERTIA_CASES("j20", 2.0),
        INERTIA_CASES("j40", 4.0),
        {"sensor-ideal", 1.0, NOMINAL, IDEAL, STEP, 0.30f, 2.2f, 0, 0, 0, 0, false},
        {"sensor-quantized", 1.0, NOMINAL, QUANTIZED, STEP, 0.30f, 2.2f, 0, 0, 0, 0, false},
        {"sensor-distorted", 1.0, NOMINAL, DISTORTED, STEP, 0.30f, 2.2f, 0, 0, 0, 0, false},
        {"sensor-lut", 1.0, NOMINAL, CORRECTED, STEP, 0.30f, 2.2f, 0, 0, 0, 0, false},
        {"sensor-crawl", 1.0, NOMINAL, QUANTIZED, CRAWL, 0.0f, 2.2f, 0, 0, 0, 0, false},
        {"noise-nominal", 1.0, NOMINAL, NOISY, STEP, 0.0f, 2.2f, 0, 0, 0, 1234U, false},
        {"noise-high", 1.0, HIGH_DAMPING, NOISY, STEP, 0.0f, 2.2f, 0, 0, 0, 1234U, false},
        {"noise-gentle", 1.0, GENTLE, NOISY, STEP, 0.0f, 2.2f, 0, 0, 0, 1234U, false},
    };
    static const struct study_case holdout[] = {
        {"hold-mismatch", 1.30, NOMINAL, QUANTIZED, REVERSE_LOAD, 0.25f, 2.2f, 0, 0, 0, 0, false},
        {"hold-noise-nominal", 1.30, NOMINAL, NOISY, STEP, 0.0f, 2.2f, 0, 0, 0, 57979U, false},
        {"hold-noise-high", 1.30, HIGH_DAMPING, NOISY, STEP, 0.0f, 2.2f, 0, 0, 0, 57979U, false},
        {"hold-noise-gentle", 1.30, GENTLE, NOISY, STEP, 0.0f, 2.2f, 0, 0, 0, 57979U, false},
        {"hold-delay-normal", 1.30, NOMINAL, QUANTIZED, STEP, 0.30f, 2.2f, 0, 0, 0, 0, false},
        {"hold-delay-small", 1.30, NOMINAL, QUANTIZED, STEP, 0.30f, 2.2f, 59U, 0, 80U, 0, false},
        {"hold-delay-edge", 1.30, NOMINAL, QUANTIZED, STEP, 0.30f, 2.2f, 99U, 80U, 120U, 0, false},
        {"hold-delay-reject",
         1.30,
         NOMINAL,
         QUANTIZED,
         STEP,
         0.30f,
         2.2f,
         299U,
         80U,
         120U,
         0,
         true},
        {"hold-small-unsat", 1.30, NOMINAL, QUANTIZED, STEP, 0.025f, 0.25f, 0, 0, 0, 0, false},
        {"hold-saturated-045",
         1.30,
         NOMINAL,
         QUANTIZED,
         REVERSE_LOAD,
         0.65f,
         0.45f,
         0,
         0,
         0,
         0,
         false},
        {"hold-load-beyond-025", 1.30, NOMINAL, QUANTIZED, LOAD, 0.30f, 0.25f, 0, 0, 0, 0, false},
    };
    if (argc > 1 && strcmp(argv[1], "--mechanical-id-fit") == 0) {
        return run_mechanical_id_comparison(argc, argv);
    }
    bool train = false, trace = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--train") == 0) {
            train = true;
        } else if (strcmp(argv[i], "--trace") == 0) {
            trace = true;
        } else {
            return 2;
        }
    }
    const struct study_case *cases = train ? training : holdout;
    const size_t count =
        train ? sizeof training / sizeof training[0] : sizeof holdout / sizeof holdout[0];
    for (size_t i = 0; i < count; ++i) {
        CHECK(run_case(&cases[i], NULL, trace) == 0);
    }
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
