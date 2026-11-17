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

#include <math.h>
#include <stdio.h>

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
        /* Rejected START in DISABLED does not count a second invalid sample.
         * Baseline and migrated producer both stay disabled, without auto-arm. */
        CHECK(
            view.state == FOC_SAFE_STATE_DISABLED && view.faults == 0U &&
            view.action == FOC_DEVICE_OUTPUT_INHIBIT
        );
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

int main(void)
{
    return test_direct_angle_compatibility() || test_startup_transport_failure() ||
        test_long_chain() || test_startup(false, 0U) || test_startup(true, 0U) ||
        test_startup(true, 1U) || test_startup(true, 2U);
}

/* ---------------------------------------- end of file ----------------------------------------- */
