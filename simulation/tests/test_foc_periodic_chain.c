/**
 * @file    test_foc_periodic_chain.c
 * @brief   Real Encoder, phase, H1/H0/Current and virtual adapter acceptance.
 * @version v0.1.0
 * @date    2026.11.16
 *
 * Test role
 * ---------
 * Preserve long signed position while independently checking Current phase.
 * Test actual virtual producer identity and both authorized startup timelines.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_virtual_current.h"
#include "foc_system_angle_oracle.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* Private Macros --------------------------------------------------------------- */

#define CHECK(c)                                                          \
    do {                                                                  \
        if (!(c)) {                                                       \
            fprintf(stderr, "chain assertion: %s at %d\n", #c, __LINE__); \
            return 1;                                                     \
        }                                                                 \
    } while (0)

/* Private Helpers -------------------------------------------------------------- */

static double distance(double a, double b)
{
    return fabs(atan2(sin(a - b), cos(a - b)));
}

static int step(
    struct foc_virtual_current *vm,
    uint32_t at,
    uint32_t seq,
    enum foc_runtime_action action,
    const struct foc_runtime_sensor *sensor
)
{
    vm->virtual_time_us = at;
    CHECK(foc_runtime_h1_step(&vm->runtime, sensor, at, 0U) == 0);
    const struct foc_runtime_fast_input input = {
        .adc = {1200U, 2548U, 1798U, 1798U, FOC_DEVICE_SAMPLE_ALL_VALID, seq, at},
        .now_us = at + 1U,
        .action = action,
    };
    const int status = foc_runtime_h0_step(&vm->runtime, &input);
    CHECK(status == (seq < 3U ? FOC_RUNTIME_IDLE : FOC_RUNTIME_OK));
    struct foc_runtime_foreground_result foreground;
    CHECK(foc_runtime_foreground_poll(&vm->runtime, at + 2U, 0U, 0U, &foreground) >= 0);
    return 0;
}

static int verify_frame(
    const struct foc_encoder_context *encoder,
    const struct foc_encoder_feedback *frame,
    float offset,
    float iq
)
{
    struct foc_virtual_current vm;
    struct foc_virtual_current_profile profile = foc_virtual_current_default_profile();
    profile.runtime.motion.rotor = (struct foc_rotor_config){7U, offset};
    profile.runtime.h1.period_us = profile.runtime.h0.period_us;
    profile.runtime.motion.prediction.max_error_rad = .002f;
    CHECK(foc_virtual_current_init(&vm, &profile) == 0);
    const struct foc_pipeline_config pipeline = {
        .current = profile.current,
        .adc = profile.adc,
        .pwm = profile.pwm,
        .safety = profile.safety,
        .output_submit = vm.runtime.pipeline.output_submit,
        .output_arg = &vm,
    };
    const uint32_t base = frame->captured_us - 800U;
    CHECK(foc_runtime_init(&vm.runtime, &pipeline, &profile.runtime, base) == 0);
    const struct foc_pipeline_command command = {{0.0f, iq}, 1U, base};
    CHECK(foc_runtime_publish_command(&vm.runtime, &command, base) == 0);
    for (uint32_t i = 0; i < 4; ++i) {
        const uint32_t at = base + i * 200U;
        struct foc_runtime_sensor seed = {
            .captured_us = at,
            .seq = i,
            .valid = true,
            .electrical_phase_valid = true,
        };
        CHECK(
            foc_rotor_phase_from_counts(
                &vm.runtime.config.motion.rotor,
                0,
                2,
                &seed.theta_elec_rad
            ) == 0
        );
        const enum foc_runtime_action action = i == 1
            ? FOC_RUNTIME_ACTION_ARM
            : (i == 2 ? FOC_RUNTIME_ACTION_START : FOC_RUNTIME_ACTION_NONE);
        CHECK(step(&vm, at, i, action, &seed) == 0);
    }
    struct foc_runtime_sensor sample = {
        .captured_us = frame->captured_us,
        .seq = 4U,
        .valid = frame->valid,
        .mechanical_rad = frame->mechanical_rad,
        .mechanical_rad_s = frame->filtered_velocity_rad_s,
    };
    sample.electrical_phase_valid = frame->valid &&
        foc_rotor_phase_from_counts(
            &vm.runtime.config.motion.rotor,
            frame->mechanical_phase_count,
            encoder->config.counts_per_revolution,
            &sample.theta_elec_rad
        ) == 0;
    CHECK(step(&vm, frame->captured_us, 4U, FOC_RUNTIME_ACTION_NONE, &sample) == 0);
    struct foc_runtime_snapshot view;
    CHECK(foc_runtime_read_snapshot(&vm.runtime, &view) == 0);
    const unsigned slot = atomic_load(&vm.runtime.sensor_slot);
    CHECK(slot < 2U);
    const struct foc_runtime_sensor_slot *published = &vm.runtime.sensor_slots[slot];
    CHECK(
        view.state == FOC_SAFE_STATE_RUNNING && view.action == FOC_DEVICE_OUTPUT_UPDATE &&
        view.output_confirmed && view.updates == 2U && vm.output.accepted_updates == 2U
    );
    CHECK(
        view.mechanical_rad == frame->mechanical_rad && view.sensor_us == frame->captured_us &&
        published->sample.seq == sample.seq && published->sample.electrical_phase_valid
    );
    int64_t cycle = encoder->position_counts % (int64_t)encoder->config.counts_per_revolution;
    if (cycle < 0) {
        cycle += encoder->config.counts_per_revolution;
    }
    const double expected = (double)cycle / encoder->config.counts_per_revolution *
            6.283185307179586476925286766559005768 * 7.0 +
        (double)offset;
    const double phase_error = distance(published->electrical.angle_rad, expected);
    /* ADC scale is .002 A/count: (1,-.5,-.5) A, so measured dq exposes H0's phase. */
    const double h0_phase =
        atan2(-vm.runtime.pipeline.measured_dq_a.q, vm.runtime.pipeline.measured_dq_a.d);
    const double dq_error = hypot(
        vm.runtime.pipeline.measured_dq_a.d - cos(expected),
        vm.runtime.pipeline.measured_dq_a.q + sin(expected)
    );
    printf(
        "counts=%lld raw=%u capture=%u seq=%u position=%.12g H1=%.12g H0=%.12g "
        "phase_error=%.12g dq_error=%.12g iq_ref=%.3g duties=%.8g,%.8g,%.8g updates=%u\n",
        (long long)encoder->position_counts,
        frame->raw_code,
        frame->captured_us,
        sample.seq,
        (double)view.mechanical_rad,
        (double)published->electrical.angle_rad,
        h0_phase,
        phase_error,
        dq_error,
        (double)iq,
        (double)vm.output.request.duty_a,
        (double)vm.output.request.duty_b,
        (double)vm.output.request.duty_c,
        vm.output.accepted_updates
    );
    CHECK(phase_error <= 1e-5 && distance(h0_phase, expected) <= 1e-5 && dq_error <= 1e-5);
    CHECK((vm.runtime.pipeline.target_a.q > 0) == (iq > 0));
    const struct foc_position_config position = {4.0f, 12.0f};
    struct foc_position_output position_output;
    CHECK(
        foc_position_step(
            &position,
            frame->mechanical_rad + .5f,
            frame->mechanical_rad,
            &position_output
        ) == 0
    );
    CHECK(position_output.velocity_ref_rad_s == 2.0f);
    return 0;
}

static int test_long_chain(void)
{
    const unsigned milestones[] = {64U, 8192U, 16384U, 65536U, 1048576U, 16777216U};
    for (int direction = -1; direction <= 1; direction += 2) {
        const struct foc_encoder_config policy =
            {65536U, (enum foc_encoder_direction)direction, 1000.0f, 1U, 1500U, 80.0f, .85f, .2f};
        struct foc_encoder_context encoder;
        struct foc_encoder_feedback frame;
        CHECK(foc_encoder_init(&encoder, &policy) == 0);
        uint32_t time = 0U;
        uint32_t raw = 123U;
        CHECK(foc_encoder_update(&encoder, time, raw, FOC_ENCODER_TRANSPORT_OK, &frame) == 0);
        unsigned milestone = 0U;
        for (unsigned i = 1U; i <= milestones[5]; ++i) {
            time += 500U;
            raw = (raw + 1024U) % 65536U;
            CHECK(
                foc_encoder_update(&encoder, time, raw, FOC_ENCODER_TRANSPORT_OK, &frame) == 0 &&
                frame.valid && !frame.continuity_lost
            );
            if (i == milestones[milestone]) {
                CHECK(verify_frame(&encoder, &frame, 0.0f, .45f) == 0);
                CHECK(verify_frame(&encoder, &frame, -.71f, -.45f) == 0);
                ++milestone;
            }
        }
    }
    return 0;
}

static int producer_identity(const struct foc_virtual_current *vm)
{
    const unsigned slot = atomic_load(&vm->runtime.sensor_slot);
    CHECK(slot < 2U);
    const struct foc_runtime_sensor_slot *published = &vm->runtime.sensor_slots[slot];
    CHECK(published->sample.captured_us == vm->encoder.previous_us);
    CHECK(
        published->sample.mechanical_rad ==
        (float)vm->encoder.position_counts *
            (6.28318530717958647692f / (float)vm->encoder.config.counts_per_revolution)
    );
    int64_t cycle = vm->encoder.position_counts % vm->encoder.config.counts_per_revolution;
    if (cycle < 0) {
        cycle += vm->encoder.config.counts_per_revolution;
    }
    const double expected = (double)cycle / vm->encoder.config.counts_per_revolution *
            6.283185307179586476925286766559005768 * vm->runtime.config.motion.rotor.pole_pairs +
        vm->runtime.config.motion.rotor.electrical_offset_rad;
    CHECK(
        published->sample.valid && published->sample.electrical_phase_valid &&
        distance(published->electrical.angle_rad, expected) <= 1e-5
    );
    return 0;
}

static int test_startup(bool early, unsigned boundary)
{
    struct foc_virtual_current vm;
    struct foc_virtual_current_profile profile = foc_virtual_current_default_profile();
    /* Controller nominal config is deliberately different from Plant truth. */
    profile.runtime.motion.rotor = (struct foc_rotor_config){7U, -.71f};
    CHECK(foc_virtual_current_init(&vm, &profile) == 0);
    CHECK(foc_virtual_current_publish_iq(&vm, 0.0f, .45f) == 0);
    if (early) {
        CHECK(foc_virtual_current_queue_action(&vm, FOC_RUNTIME_ACTION_ARM) == 0);
    }
    for (uint32_t at = 0U; at <= 600U; at += 200U) {
        if (at == 200U || (!early && at == 400U)) {
            CHECK(
                foc_virtual_current_queue_action(
                    &vm,
                    at == 200U && !early ? FOC_RUNTIME_ACTION_ARM : FOC_RUNTIME_ACTION_START
                ) == 0
            );
        }
        if (at == 400U && boundary != 0U) {
            foc_runtime_request_inhibit(
                &vm.runtime,
                boundary == 2U ? FOC_SAFE_FAULT_SAMPLE_INVALID : 0U
            );
        }
        CHECK(foc_virtual_current_run_until(&vm, at) == 0);
        CHECK(producer_identity(&vm) == 0);
        struct foc_runtime_snapshot view;
        CHECK(foc_runtime_read_snapshot(&vm.runtime, &view) == 0);
        printf(
            "startup early=%u boundary=%u time=%u velocity_seen=%u source=%u updates=%u state=%d\n",
            early,
            boundary,
            at,
            vm.encoder.velocity_seen,
            view.sensor_us,
            view.updates,
            view.state
        );
        if (at <= 400U) {
            CHECK(!vm.encoder.velocity_seen);
            CHECK(
                vm.runtime.sensor_slots[atomic_load(&vm.runtime.sensor_slot)]
                    .sample.mechanical_rad_s == 0.0f
            );
        }
        if (boundary == 0U) {
            CHECK(
                view.updates == (at == 600U ? (early ? 2U : 1U) : (at == 400U && early ? 1U : 0U))
            );
        } else if (at >= 400U) {
            CHECK(view.updates == 0U && view.action == FOC_DEVICE_OUTPUT_INHIBIT);
        }
    }
    return 0;
}

/* Test Entry ------------------------------------------------------------------- */

static int test_startup_transport_failure(void)
{
    struct foc_virtual_current vm;
    const struct foc_virtual_current_profile profile = foc_virtual_current_default_profile();
    CHECK(foc_virtual_current_init(&vm, &profile) == 0);
    CHECK(foc_virtual_current_publish_iq(&vm, 0.0f, .45f) == 0);
    CHECK(foc_virtual_current_fail_next_sensor(&vm) == 0);
    CHECK(foc_virtual_current_queue_action(&vm, FOC_RUNTIME_ACTION_ARM) == 0);
    for (uint32_t at = 0U; at <= 1000U; at += 200U) {
        if (at == 200U) {
            CHECK(foc_virtual_current_queue_action(&vm, FOC_RUNTIME_ACTION_START) == 0);
        }
        CHECK(foc_virtual_current_run_until(&vm, at) == 0);
        const unsigned slot = atomic_load(&vm.runtime.sensor_slot);
        CHECK(slot < 2U && vm.output.accepted_updates == 0U);
        if (at == 0U) {
            CHECK(
                !vm.runtime.sensor_slots[slot].sample.valid &&
                !vm.runtime.sensor_slots[slot].sample.electrical_phase_valid &&
                !vm.encoder.previous_seen
            );
        }
        struct foc_runtime_snapshot view;
        CHECK(foc_runtime_read_snapshot(&vm.runtime, &view) == 0);
        printf(
            "startup transport failure time=%u phase_valid=%u speed_seen=%u state=%d updates=%u\n",
            at,
            vm.runtime.sensor_slots[slot].sample.electrical_phase_valid,
            vm.encoder.velocity_seen,
            view.state,
            view.updates
        );
        /* Rejected ARM/START cannot hide invalid observations. The third bad
         * cycle latches the existing sample fault; later feedback cannot rearm. */
        const bool latched = at >= 400U;
        CHECK(
            view.state == (latched ? FOC_SAFE_STATE_FAULT_LATCHED : FOC_SAFE_STATE_DISABLED) &&
            view.faults == (latched ? FOC_SAFE_FAULT_SAMPLE_INVALID : 0U) &&
            view.action == FOC_DEVICE_OUTPUT_INHIBIT
        );
        if (at <= 400U) {
            CHECK(vm.runtime.pipeline.safety.invalid_sample_streak == at / 200U + 1U);
        }
    }
    CHECK(vm.encoder.velocity_seen); /* Later valid phase/speed cannot auto-restart. */
    return 0;
}

static int test_direct_angle_compatibility(void)
{
    struct foc_virtual_current vm;
    struct foc_virtual_current_profile profile = foc_virtual_current_default_profile();
    profile.runtime.motion = (struct foc_runtime_motion_config){0};
    CHECK(foc_virtual_current_init(&vm, &profile) == 0);
    CHECK(foc_virtual_current_run_until(&vm, 0U) == 0);
    const unsigned slot = atomic_load(&vm.runtime.sensor_slot);
    CHECK(slot < 2U && vm.runtime.sensor_slots[slot].sample.valid);
    CHECK(vm.runtime.sensor_slots[slot].sample.captured_us == vm.encoder.previous_us);
    const float actual = vm.runtime.sensor_slots[slot].sample.theta_elec_rad;
    printf(
        "direct-angle mode initial phase=%.9g expected=%.9g\n",
        (double)actual,
        profile.motor.electrical_offset_rad
    );
    CHECK(fabs((double)actual - profile.motor.electrical_offset_rad) <= 1e-7);
    CHECK(!vm.runtime.sensor_slots[slot].sample.electrical_phase_valid);
    return 0;
}

/* System angle acceptance ------------------------------------------------------ */

struct system_angle_case {
    const char *name;
    struct system_angle_reference motion;
    unsigned delay_us;
    int stamp_bias_us;
    /** Bits: 1=capture stamp, 2=ADC sample time, 4=H1/H0 execution. */
    unsigned jitter;
    uint32_t origin;
    int direction;
    unsigned poles;
};

struct system_angle_stats {
    unsigned samples;
    double square_error;
    double maximum[13];
};

static int system_capture(
    struct foc_virtual_current *vm,
    const struct system_angle_case *test,
    int64_t capture,
    int64_t stamp,
    unsigned sequence,
    struct system_speed_reference *reference,
    struct foc_encoder_feedback *feedback,
    struct foc_runtime_sensor *sample
)
{
    /* Prescribed physical motion, not a replacement estimator or controller. */
    vm->motor.state.mechanical_position_rad = system_position(&test->motion, capture);
    uint16_t code;
    CHECK(foc_virtual_motor_make_encoder_code(&vm->motor, &vm->profile.encoder, &code) == 0);
    const uint32_t cpr = vm->encoder.config.counts_per_revolution;
    const int64_t count = system_quantize(vm->motor.state.mechanical_position_rad, cpr);
    const uint32_t cycle = system_cycle(count, cpr);
    CHECK(code == (test->direction > 0 || cycle == 0U ? cycle : cpr - cycle));
    CHECK(
        foc_encoder_update(
            &vm->encoder,
            test->origin + (uint32_t)stamp,
            code,
            FOC_ENCODER_TRANSPORT_OK,
            feedback
        ) == 0
    );
    system_reference_speed(
        reference,
        count,
        stamp,
        cpr,
        (double)vm->encoder.config.speed_lpf_cutoff_hz
    );
    if (reference->observations > 1U) {
        CHECK(feedback->valid && feedback->speed_valid);
        CHECK(fabs((double)feedback->raw_velocity_rad_s - reference->raw_rad_s) <= 0.001);
        CHECK(fabs((double)feedback->filtered_velocity_rad_s - reference->filtered_rad_s) <= 0.001);
    }
    CHECK(feedback->mechanical_phase_count == cycle);
    float phase;
    CHECK(foc_rotor_phase_from_counts(&vm->runtime.config.motion.rotor, cycle, cpr, &phase) == 0);
    *sample = (struct foc_runtime_sensor){
        .theta_elec_rad = phase,
        .captured_us = feedback->captured_us,
        .seq = sequence,
        .valid = feedback->valid,
        .mechanical_rad = feedback->mechanical_rad,
        .mechanical_rad_s = feedback->filtered_velocity_rad_s,
        .electrical_phase_valid = feedback->valid,
    };
    return 0;
}

static int system_angle_run(const struct system_angle_case *test, unsigned rejection, FILE *csv)
{
    const double tau = 6.283185307179586476925286766559;
    struct foc_virtual_current vm;
    struct foc_virtual_current_profile profile = foc_virtual_current_default_profile();
    profile.runtime.motion.rotor.pole_pairs = (uint16_t)test->poles;
    profile.motor.pole_pairs = test->poles;
    profile.motor.electrical_offset_rad =
        (double)profile.runtime.motion.rotor.electrical_offset_rad;
    profile.encoder.direction = (enum foc_encoder_direction)test->direction;
    /* Test initialization uses Board's prediction policy, retaining 200/500 us scheduling. */
    profile.runtime.motion.prediction = (struct foc_prediction_config){650U, 2000.0f, .00065f};
    profile.runtime.angle_timeout_us = 650U;
    CHECK(foc_virtual_current_init(&vm, &profile) == 0);
    const struct foc_pipeline_config pipeline = {
        .current = profile.current,
        .adc = profile.adc,
        .pwm = profile.pwm,
        .safety = profile.safety,
        .output_submit = vm.runtime.pipeline.output_submit,
        .output_arg = &vm
    };
    CHECK(foc_runtime_init(&vm.runtime, &pipeline, &profile.runtime, test->origin) == 0);
    struct system_speed_reference reference = {0};
    struct foc_encoder_feedback feedback;
    struct foc_runtime_sensor sample;
    for (int64_t t = -20000; t < 0; t += 500) {
        CHECK(
            system_capture(
                &vm,
                test,
                t - test->delay_us,
                t - test->delay_us + test->stamp_bias_us,
                0U,
                &reference,
                &feedback,
                &sample
            ) == 0
        );
    }
    struct system_angle_stats stats = {0};
    int64_t capture = 0, stamp = 0, published_us = 0;
    unsigned sensor_sequence = 0U;
    uint32_t random = UINT32_C(0x51a72026);
    bool rejected = false;
    for (unsigned t = 0U; t <= (rejection ? 2000U : 40000U); t += 100U) {
        random = random * UINT32_C(1664525) + UINT32_C(1013904223);
        const int jitter = (int)((random >> 16) % 41U) - 20;
        if (t % 1000U == 0U) {
            vm.virtual_time_us = test->origin + t;
            CHECK(foc_virtual_current_publish_iq(&vm, 0.0f, .45f) == 0);
        }
        if (t % 500U == 0U && !(rejection == 2U && t >= 1000U)) {
            capture = (int64_t)t - test->delay_us;
            stamp = capture + test->stamp_bias_us + ((test->jitter & 1U) ? jitter : 0);
            CHECK(
                system_capture(
                    &vm,
                    test,
                    capture,
                    stamp,
                    ++sensor_sequence,
                    &reference,
                    &feedback,
                    &sample
                ) == 0
            );
            if ((rejection == 1U || rejection == 3U) && t == 1000U) {
                CHECK(
                    foc_encoder_update(
                        &vm.encoder,
                        test->origin + (uint32_t)stamp,
                        rejection == 3U ? vm.encoder.config.counts_per_revolution
                                        : feedback.raw_code,
                        rejection == 3U ? FOC_ENCODER_TRANSPORT_OK
                                        : FOC_ENCODER_TRANSPORT_FRAME_ERROR,
                        &feedback
                    ) < 0
                );
                sample.valid = feedback.valid;
                sample.electrical_phase_valid = feedback.valid;
            }
            if (rejection >= 4U && t == 1000U) {
                const unsigned previous_slot = atomic_load(&vm.runtime.sensor_slot);
                const struct foc_runtime_sensor_slot previous_payload =
                    vm.runtime.sensor_slots[previous_slot];
                const struct foc_runtime_sensor previous = previous_payload.sample;
                /* Rejected contents must not leak into the still-fresh retained source. */
                sample.theta_elec_rad = 2.1f;
                sample.mechanical_rad_s = 100.0f;
                if (rejection == 4U) {
                    sample.captured_us = test->origin + t + 100U;
                } else if (rejection == 5U) {
                    sample.seq = previous.seq;
                } else {
                    sample.captured_us = test->origin;
                }
                CHECK(
                    foc_runtime_h1_step(&vm.runtime, &sample, test->origin + t + 10U, 0U) ==
                    FOC_RUNTIME_ERR_ORDER
                );
                CHECK(atomic_load(&vm.runtime.sensor_slot) == previous_slot);
                const struct foc_runtime_sensor_slot *retained =
                    &vm.runtime.sensor_slots[previous_slot];
                CHECK(
                    retained->electrical.angle_rad == previous_payload.electrical.angle_rad &&
                    retained->electrical.velocity_rad_s ==
                        previous_payload.electrical.velocity_rad_s
                );
                CHECK(
                    retained->sample.theta_elec_rad == previous.theta_elec_rad &&
                    retained->sample.mechanical_rad == previous.mechanical_rad &&
                    retained->sample.mechanical_rad_s == previous.mechanical_rad_s &&
                    retained->sample.valid == previous.valid &&
                    retained->sample.electrical_phase_valid == previous.electrical_phase_valid &&
                    retained->sample.seq == previous.seq &&
                    retained->sample.captured_us == previous.captured_us
                );
                CHECK(
                    retained->target.target_a.d == previous_payload.target.target_a.d &&
                    retained->target.target_a.q == previous_payload.target.target_a.q &&
                    retained->target.seq == previous_payload.target.seq &&
                    retained->target.produced_us == previous_payload.target.produced_us &&
                    retained->mode == previous_payload.mode &&
                    retained->session == previous_payload.session &&
                    retained->target_valid == previous_payload.target_valid
                );
                const unsigned updates_before = vm.output.accepted_updates;
                struct foc_runtime_foreground_result foreground;
                CHECK(
                    foc_runtime_foreground_poll(
                        &vm.runtime,
                        test->origin + t + 25U,
                        0U,
                        0U,
                        &foreground
                    ) >= 0
                );
                const struct foc_runtime_fast_input cached_input = {
                    .adc =
                        {1200U,
                         2548U,
                         1798U,
                         1798U,
                         FOC_DEVICE_SAMPLE_ALL_VALID,
                         6U,
                         test->origin + t},
                    .now_us = test->origin + t + 30U,
                };
                CHECK(foc_runtime_h0_step(&vm.runtime, &cached_input) == FOC_RUNTIME_OK);
                CHECK(vm.output.accepted_updates == updates_before + 1U);
                CHECK(
                    vm.runtime.observed_sensor.seq == previous.seq &&
                    vm.runtime.observed_sensor.captured_us == previous.captured_us
                );
                const double expected = (double)previous_payload.electrical.angle_rad +
                    (double)previous_payload.electrical.velocity_rad_s * .0005;
                CHECK(
                    fabs(vm.runtime.pipeline.measured_dq_a.d - cos(expected)) <= 2e-6 &&
                    fabs(vm.runtime.pipeline.measured_dq_a.q + sin(expected)) <= 2e-6
                );
                printf(
                    "system_discarded case=%s candidate_rejected=1 cached_fresh_update=1 "
                    "source_seq=%u source_us=%u adc_us=%u updates=%u\n",
                    test->name,
                    previous.seq,
                    previous.captured_us,
                    cached_input.adc.sample_tick,
                    vm.output.accepted_updates
                );
                /* Rejected arrival cannot refresh the retained source's original age. */
                for (unsigned after = 1200U; after <= 1400U; after += 200U) {
                    CHECK(
                        foc_runtime_foreground_poll(
                            &vm.runtime,
                            test->origin + after + 25U,
                            0U,
                            0U,
                            &foreground
                        ) >= 0
                    );
                    struct foc_runtime_fast_input expired_input = cached_input;
                    expired_input.adc.sample_seq = after / 200U + 1U;
                    expired_input.adc.sample_tick = test->origin + after;
                    expired_input.now_us = test->origin + after + 30U;
                    const int expiry_status = foc_runtime_h0_step(&vm.runtime, &expired_input);
                    /* First expiry is alignment; later STOP/FAULT may take priority. */
                    CHECK(
                        after == 1200U ? expiry_status == FOC_RUNTIME_ERR_ALIGNMENT
                                       : expiry_status < 0 || expiry_status == FOC_RUNTIME_IDLE
                    );
                    CHECK(
                        vm.output.accepted_updates == updates_before + 1U &&
                        vm.output.request.action == FOC_DEVICE_OUTPUT_INHIBIT
                    );
                }
                printf(
                    "system_cache_expired case=%s adc_us=1200 source_us=%u updates=%u\n",
                    test->name,
                    previous.captured_us,
                    vm.output.accepted_updates
                );
                return 0;
            }
            published_us = t + 10U + ((test->jitter & 4U) ? (unsigned)(jitter + 20) / 4U : 0U);
            CHECK(capture + 5 <= published_us);
            const int status = foc_runtime_h1_step(
                &vm.runtime,
                &sample,
                test->origin + (uint32_t)published_us,
                0U
            );
            CHECK(
                status ==
                ((rejection == 1U || rejection == 3U) && t == 1000U ? FOC_RUNTIME_ERR_INPUT
                                                                    : FOC_RUNTIME_OK)
            );
        }
        if (t % 200U == 0U) {
            struct foc_runtime_foreground_result result;
            CHECK(
                foc_runtime_foreground_poll(&vm.runtime, test->origin + t + 25U, 0U, 0U, &result) >=
                0
            );
        }
        if (t % 200U != 0U) {
            continue;
        }
        const int64_t adc_time = (int64_t)t + ((test->jitter & 2U) ? jitter / 4 : 0);
        const struct foc_runtime_fast_input input = {
            .adc =
                {1200U,
                 2548U,
                 1798U,
                 1798U,
                 FOC_DEVICE_SAMPLE_ALL_VALID,
                 t / 200U + 1U,
                 test->origin + (uint32_t)adc_time},
            .now_us =
                test->origin + t + 30U + ((test->jitter & 4U) ? (unsigned)(jitter + 20) / 4U : 0U),
            .action = t == 200U ? FOC_RUNTIME_ACTION_ARM
                : t == 400U     ? FOC_RUNTIME_ACTION_START
                                : FOC_RUNTIME_ACTION_NONE,
        };
        const struct foc_current_context before = vm.runtime.pipeline.current;
        const uint32_t updates = vm.output.accepted_updates;
        const int status = foc_runtime_h0_step(&vm.runtime, &input);
        if (((rejection == 1U || rejection == 3U) && t >= 1000U) ||
            (rejection == 2U && t >= 1200U)) {
            CHECK(status < 0 || status == FOC_RUNTIME_IDLE);
            CHECK(vm.output.accepted_updates == updates);
            CHECK(vm.output.request.action == FOC_DEVICE_OUTPUT_INHIBIT);
            rejected = true;
            if (t == (rejection == 2U ? 1200U : 1000U)) {
                printf(
                    "system_rejected case=%s reason=%u at=%u status=%d updates=%u\n",
                    test->name,
                    rejection,
                    t,
                    status,
                    updates
                );
            }
            continue;
        }
        CHECK(status == (t < 600U ? FOC_RUNTIME_IDLE : FOC_RUNTIME_OK));
        if (t < 600U) {
            continue;
        }
        CHECK(vm.output.accepted_updates == updates + 1U);
        const unsigned slot = atomic_load(&vm.runtime.sensor_slot);
        CHECK(slot < 2U);
        const struct foc_runtime_sensor_slot *published = &vm.runtime.sensor_slots[slot];
        CHECK(published->sample.captured_us == test->origin + (uint32_t)stamp);
        CHECK(published->sample.mechanical_rad_s == feedback.filtered_velocity_rad_s);
        const uint32_t age = input.adc.sample_tick - published->sample.captured_us;
        CHECK(age <= 650U && vm.runtime.angle_prediction_us == age);
        CHECK(fabs(published->electrical.velocity_rad_s) <= 7000.0f);
        const double dt = (double)(adc_time - capture) / 1000000.0;
        const double reported_dt = (double)age / 1000000.0;
        const double poles = test->poles;
        const double offset = (double)profile.runtime.motion.rotor.electrical_offset_rad;
        const double true_capture = poles * system_position(&test->motion, capture) + offset;
        const double true_angle = poles * system_position(&test->motion, adc_time) + offset;
        const double quantized =
            poles * (double)reference.count * tau / vm.encoder.config.counts_per_revolution +
            offset;
        const double speed_true = poles * system_velocity(&test->motion, capture);
        const double speed_estimate = published->electrical.velocity_rad_s;
        const double expected_prediction =
            (double)published->electrical.angle_rad + speed_estimate * reported_dt;
        float phase;
        CHECK(
            foc_rotor_predict(
                &profile.runtime.motion.prediction,
                &published->electrical,
                age,
                &phase
            ) == 0
        );
        const double numeric = system_short((double)phase - expected_prediction);
        CHECK(fabs(numeric) <= 2e-6);
        const double phase_error = system_short((double)phase - true_angle);
        const double quant_error = system_short(quantized - true_capture);
        const double mapping = system_short((double)published->electrical.angle_rad - quantized);
        const double raw_error = (poles * feedback.raw_velocity_rad_s - speed_true) * dt;
        const double lpf_error = (speed_estimate - poles * feedback.raw_velocity_rad_s) * dt;
        const double time_error = speed_estimate * (reported_dt - dt);
        const double model_error = -0.5 * poles * test->motion.acceleration_rad_s2 * dt * dt;
        const double quant_bound = poles * tau / (2.0 * vm.encoder.config.counts_per_revolution);
        CHECK(fabs(quant_error) <= quant_bound + 1e-12 && fabs(mapping) <= 1e-5);
        CHECK(
            fabs(system_short(
                phase_error - quant_error - mapping - raw_error - lpf_error - time_error -
                model_error - numeric
            )) <= 1e-10
        );
        const double speed_bound =
            fabs(poles * reference.filtered_rad_s - speed_true) + poles * .001;
        const double bound = quant_bound + 1e-5 + speed_bound * dt +
            (fabs(poles * reference.filtered_rad_s) + poles * .001) * fabs(reported_dt - dt) +
            fabs(model_error) + 2e-6;
        CHECK(fabs(phase_error) <= bound);
        const struct foc_vector_dq measured = vm.runtime.pipeline.measured_dq_a;
        const double id_true = cos(true_angle), iq_true = -sin(true_angle);
        const double id_error = measured.d - id_true, iq_error = measured.q - iq_true;
        CHECK(fabs(measured.d - cos((double)phase)) <= 3e-7);
        CHECK(fabs(measured.q + sin((double)phase)) <= 3e-7);
        CHECK(fabs(hypot(id_error, iq_error) - 2.0 * fabs(sin(phase_error / 2.0))) <= 5e-7);
        struct foc_current_context actual_trial = before, ideal_trial = before;
        struct foc_current_input current_input = {
            .phase_current_a = {1.0f, -.5f, -.5f},
            .theta_elec_rad = phase,
            .target_a = {0.0f, .45f},
            .vbus_v = 12.0f,
            .dt_s = (float)vm.runtime.interval_us * 1.0e-6f,
        };
        struct foc_current_output actual_output, ideal_output;
        CHECK(foc_current_step(&actual_trial, &current_input, &actual_output) == 0);
        CHECK(
            actual_output.measured_dq_a.d == measured.d &&
            actual_output.measured_dq_a.q == measured.q
        );
        CHECK(actual_output.svpwm.duty_a == vm.output.request.duty_a);
        current_input.theta_elec_rad = (float)system_short(true_angle);
        CHECK(foc_current_step(&ideal_trial, &current_input, &ideal_output) == 0);
        const double duty_delta = fabs(actual_output.svpwm.duty_a - ideal_output.svpwm.duty_a);
        const double values[] = {
            phase_error,
            id_error,
            iq_error,
            quant_error,
            raw_error,
            lpf_error,
            time_error,
            model_error,
            numeric,
            speed_estimate - speed_true,
            system_short((double)published->electrical.angle_rad - true_angle),
            bound,
            duty_delta
        };
        ++stats.samples;
        stats.square_error += phase_error * phase_error;
        if (fabs(phase_error) > stats.maximum[0]) {
            printf(
                "system_worst case=%s t=%u capture=%lld stamp=%lld adc=%lld theta_true=%.17g "
                "theta_h0=%.17g phase_error=%.17g id_true=%.17g iq_true=%.17g id=%.17g iq=%.17g\n",
                test->name,
                t,
                (long long)capture,
                (long long)stamp,
                (long long)adc_time,
                system_short(true_angle),
                (double)phase,
                phase_error,
                id_true,
                iq_true,
                (double)measured.d,
                (double)measured.q
            );
        }
        for (unsigned i = 0U; i < 13U; ++i) {
            stats.maximum[i] = fmax(stats.maximum[i], fabs(values[i]));
        }
        if (csv != NULL) {
            fprintf(
                csv,
                "%s,%u,%u,%u,%lld,%lld,%lld,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,"
                "%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g",
                test->name,
                t,
                test->poles,
                vm.encoder.config.counts_per_revolution,
                (long long)capture,
                (long long)stamp,
                (long long)adc_time,
                system_short(true_angle),
                (double)phase,
                phase_error,
                id_true,
                iq_true,
                (double)measured.d,
                (double)measured.q,
                id_error,
                iq_error,
                quant_error,
                raw_error,
                lpf_error,
                time_error,
                model_error,
                numeric,
                speed_true,
                speed_estimate,
                bound,
                duty_delta
            );
            fprintf(
                csv,
                ",%.17g,%.17g,%.17g,%.17g,%lld,%lld,%u,%.17g,%.17g\n",
                system_short(true_capture),
                system_short(quantized),
                (double)published->electrical.angle_rad,
                system_position(&test->motion, adc_time),
                (long long)published_us,
                (long long)(capture + 5),
                input.now_us - test->origin,
                (double)feedback.raw_velocity_rad_s,
                (double)feedback.filtered_velocity_rad_s
            );
        }
    }
    CHECK(stats.samples > 0U && (!rejection || rejected));
    printf(
        "system_summary case=%s samples=%u rms=%.17g max_phase=%.17g max_id=%.17g max_iq=%.17g "
        "quant=%.17g raw_velocity=%.17g lpf=%.17g timestamp=%.17g model=%.17g numeric=%.17g "
        "speed_error_e=%.17g no_prediction=%.17g bound=%.17g duty_delta=%.17g\n",
        test->name,
        stats.samples,
        sqrt(stats.square_error / stats.samples),
        stats.maximum[0],
        stats.maximum[1],
        stats.maximum[2],
        stats.maximum[3],
        stats.maximum[4],
        stats.maximum[5],
        stats.maximum[6],
        stats.maximum[7],
        stats.maximum[8],
        stats.maximum[9],
        stats.maximum[10],
        stats.maximum[11],
        stats.maximum[12]
    );
    return 0;
}

static int test_system_angles(FILE *csv)
{
    const struct system_angle_case cases[] = {
        {"static", {.12345, 0, 0}, 0, 0, false, 1000000U, 1, 7},
        {"low-speed", {.12345, .05, 0}, 0, 0, false, 1000000U, 1, 7},
        {"constant", {.12345, 100, 0}, 0, 0, false, 1000000U, 1, 7},
        {"capture-delay", {.12345, 100, 0}, 150, 0, false, 1000000U, 1, 7},
        {"accelerate", {.12345, 20, 250}, 150, 0, false, 1000000U, 1, 7},
        {"reverse", {6.28, 5, -250}, 150, 0, false, 1000000U, 1, 7},
        {"stamp-bias", {.12345, 100, 0}, 150, 20, false, 1000000U, 1, 7},
        {"stamp-jitter", {.12345, 100, 0}, 150, 0, 1U, 1000000U, 1, 7},
        {"adc-jitter", {.12345, 100, 0}, 150, 0, 2U, 1000000U, 1, 7},
        {"execution-jitter", {.12345, 100, 0}, 150, 0, 4U, 1000000U, 1, 7},
        {"clock-wrap", {6.28, 100, 0}, 150, 0, false, UINT32_MAX - 2000U, -1, 7},
        {"combined", {6.28, 100, 250}, 150, 10, 7U, UINT32_MAX - 2000U, -1, 7},
        {"virtual-poles", {.12345, 100, 0}, 150, 0, false, 1000000U, 1, 4},
    };
    /* Literal Oracle checks precede every production comparison. */
    CHECK(fabs(system_short(6.28318530717958647692 + .25) - .25) < 1e-14);
    const struct system_angle_reference fixture = {1, 2, 4};
    CHECK(system_position(&fixture, 500000) == 2.5 && system_velocity(&fixture, 500000) == 4.0);
    CHECK(system_quantize(0.0, 16U) == 0 && system_quantize(6.28318530717958647692, 16U) == 16);
    struct system_speed_reference speed_fixture = {0};
    const double pi = 3.14159265358979323846;
    system_reference_speed(&speed_fixture, 0, 0, 16U, 1.0 / (2.0 * pi));
    system_reference_speed(&speed_fixture, 1, 1000000, 16U, 1.0 / (2.0 * pi));
    CHECK(fabs(speed_fixture.filtered_rad_s - pi / 8.0) < 1e-14);
    system_reference_speed(&speed_fixture, 1, 2000000, 16U, 1.0 / (2.0 * pi));
    CHECK(fabs(speed_fixture.filtered_rad_s - pi / 16.0) < 1e-14);
    if (csv != NULL) {
        fprintf(
            csv,
            "case,t_us,poles,cpr,capture_us,stamp_us,adc_us,theta_true,theta_h0,phase_error_rad,"
            "id_true_a,iq_true_a,id_measured_a,iq_measured_a,id_error_a,iq_error_a,e_quant,e_raw_"
            "velocity,"
            "e_lpf,e_time,e_model,e_numeric,omega_true_e,omega_estimate_e,bound_rad,duty_delta,"
            "theta_capture_e,theta_quantized_e,theta_h1_e,mechanical_true_rad,h1_publish_us,"
            "assumed_transport_complete_us,h0_execute_us,raw_speed_m,filtered_speed_m\n"
        );
    }
    for (unsigned i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        CHECK(system_angle_run(&cases[i], 0U, csv) == 0);
    }
    const char *rejections[] = {
        "transport-error",
        "expired-source",
        "invalid-code",
        "future-candidate",
        "duplicate-candidate",
        "expired-candidate"
    };
    for (unsigned i = 0U; i < 6U; ++i) {
        struct system_angle_case invalid = cases[0];
        invalid.name = rejections[i];
        CHECK(system_angle_run(&invalid, i + 1U, csv) == 0);
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--system-angle-csv") == 0) {
        FILE *csv = fopen(argv[2], "w");
        CHECK(csv != NULL);
        const int status = test_system_angles(csv);
        return fclose(csv) == 0 ? status : 1;
    }
    CHECK(argc == 1);
    return test_direct_angle_compatibility() || test_startup_transport_failure() ||
        test_long_chain() || test_startup(false, 0U) || test_startup(true, 0U) ||
        test_startup(true, 1U) || test_startup(true, 2U) || test_system_angles(NULL);
}

/* ---------------------------------------- end of file ----------------------------------------- */
