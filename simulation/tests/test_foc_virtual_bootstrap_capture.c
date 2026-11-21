/**
 * @file    test_foc_virtual_bootstrap_capture.c
 * @brief   Witness and revocation tests for Host open-circuit capture.
 * @version v0.1.0
 * @date    2026.11.20
 *
 * Test role
 * ---------
 * Verify the Host sensor boundary, bounded shaft response and Runtime inhibit
 * revocation against both virtual motor profiles.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_virtual_bootstrap_capture.h"

#include "foc_virtual_calibration.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* Private Types ---------------------------------------------------------------- */

enum test_injection {
    TEST_NONE,
    TEST_STOP,
    TEST_FAULT,
    TEST_REVOKE,
};

struct test_gate {
    struct foc_virtual_current original;
    enum test_injection injection;
    uint32_t revoke_us;
    bool fired;
};

/* Private Helpers -------------------------------------------------------------- */

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return 1;                                                       \
        }                                                                   \
    } while (0)

static const double TAU = 6.28318530717958647692;
static const int TEST_INHIBIT_REVOKED = -121;

static int permit(void *argument, uint32_t aperture_us)
{
    struct test_gate *gate = argument;
    if (!gate->fired && gate->injection != TEST_NONE && aperture_us >= gate->revoke_us) {
        gate->fired = true;
        if (gate->injection == TEST_STOP) {
            (void)foc_virtual_current_request_stop(&gate->original);
        } else if (gate->injection == TEST_FAULT) {
            foc_runtime_request_inhibit(&gate->original.runtime, FOC_SAFE_FAULT_SAMPLE_INVALID);
        } else {
            return TEST_INHIBIT_REVOKED;
        }
    }
    struct foc_runtime_snapshot snapshot;
    return foc_virtual_current_snapshot(&gate->original, &snapshot) == FOC_RUNTIME_OK &&
            snapshot.state == FOC_SAFE_STATE_DISABLED && snapshot.faults == 0U &&
            snapshot.output_confirmed && snapshot.action == FOC_DEVICE_OUTPUT_INHIBIT &&
            !snapshot.inhibit_pending && snapshot.updates == 0U
        ? 0
        : TEST_INHIBIT_REVOKED;
}

static int check_apertures(
    const struct foc_virtual_current_profile *profile,
    const struct foc_virtual_bootstrap_capture_request *request,
    const struct foc_virtual_bootstrap_capture_result *result
)
{
    CHECK(result->count == FOC_VIRTUAL_BOOTSTRAP_SAMPLE_CAPACITY);
    double maximum_motion_error = 0.0;
    double maximum_oracle_residual = 0.0;
    double maximum_offset_disagreement = 0.0;
    double fitted_offsets[FOC_BOOTSTRAP_SWEEPS] = {0};
    double offset_sine = 0.0;
    double offset_cosine = 0.0;
    uint32_t wraps = 0U;
    const double cpr = profile->encoder.counts_per_revolution;
    for (uint32_t i = 0U; i < result->count; ++i) {
        const struct foc_bootstrap_sample *sample = &result->samples[i];
        CHECK(sample->capture == request->capture);
        CHECK(sample->identity.acquisition.pole_pairs == 0U);
        CHECK(sample->identity.acquisition.encoder_direction == 0);
        CHECK(sample->sequence == i + 1U);
        CHECK(sample->sweep == i / FOC_VIRTUAL_BOOTSTRAP_SAMPLES_PER_SWEEP);
        CHECK(sample->valid && sample->output_inhibited);
        const double sign = sample->sweep % 2U == 0U ? 1.0 : -1.0;
        CHECK(sample->shaft_rad_s * sign > 7.2);
        CHECK(fabs(sample->shaft_rad_s) < 8.8);

        const double theta = atan2(-sign * sample->emf_alpha_v, sign * sample->emf_beta_v);
        /* Plant truth is used here solely as an independent test oracle. */
        const double raw_theta = TAU * sample->raw_code / cpr * profile->encoder.direction;
        const double offset = remainder(theta - profile->motor.pole_pairs * raw_theta, TAU);
        const double error = fabs(remainder(offset - profile->motor.electrical_offset_rad, TAU));
        maximum_oracle_residual = fmax(maximum_oracle_residual, error);
        CHECK(error < 0.07);
        offset_sine += sin(offset);
        offset_cosine += cos(offset);
        if ((i + 1U) % FOC_VIRTUAL_BOOTSTRAP_SAMPLES_PER_SWEEP == 0U) {
            const double average = atan2(offset_sine, offset_cosine);
            for (uint32_t j = 0U; j < sample->sweep; ++j) {
                const double disagreement = fabs(remainder(average - fitted_offsets[j], TAU));
                maximum_offset_disagreement = fmax(maximum_offset_disagreement, disagreement);
            }
            fitted_offsets[sample->sweep] = average;
            offset_sine = 0.0;
            offset_cosine = 0.0;
        }

        if (i > 0U) {
            const struct foc_bootstrap_sample *previous = &result->samples[i - 1U];
            CHECK(sample->captured_us > previous->captured_us);
            if (previous->sweep == sample->sweep) {
                const double elapsed_s = (sample->captured_us - previous->captured_us) * 1.0e-6;
                const double delta = (double)sample->raw_code - previous->raw_code;
                const double expected = (previous->shaft_rad_s + sample->shaft_rad_s) * 0.5 *
                    elapsed_s * profile->encoder.direction;
                const double motion_error = fabs(remainder(delta, cpr) * TAU / cpr - expected);
                maximum_motion_error = fmax(maximum_motion_error, motion_error);
                CHECK(motion_error < 0.001);
                wraps += fabs(delta) > cpr * 0.5;
            }
        }
    }
    CHECK(wraps >= 4U);
    CHECK(result->completed_us > result->samples[result->count - 1U].captured_us);
    printf(
        "samples=%u wraps=%u max_motion_error_rad=%.7f peak_raw_reference_error_rad=%.7f "
        "sweep_offset_disagreement_rad=%.7f\n",
        (unsigned int)result->count,
        (unsigned int)wraps,
        maximum_motion_error,
        maximum_oracle_residual,
        maximum_offset_disagreement
    );
    return 0;
}

static int check_revocation(
    const struct foc_virtual_current_profile *profile,
    const struct foc_virtual_motor *motor,
    struct foc_virtual_bootstrap_capture_request *request,
    struct foc_virtual_bootstrap_capture_result *result
)
{
    for (enum test_injection injection = TEST_STOP; injection <= TEST_REVOKE; ++injection) {
        struct test_gate gate = {.injection = injection, .revoke_us = 600000U};
        CHECK(foc_virtual_current_init(&gate.original, profile) == 0);
        request->argument = &gate;
        CHECK(
            foc_virtual_bootstrap_capture(motor, profile, request, result) == TEST_INHIBIT_REVOKED
        );
        CHECK(gate.fired && result->reason == FOC_VIRTUAL_BOOTSTRAP_CAPTURE_GATE);
        CHECK(result->count == FOC_VIRTUAL_BOOTSTRAP_SAMPLES_PER_SWEEP);
        CHECK(result->completed_us < gate.revoke_us);
        CHECK(foc_virtual_current_run_until(&gate.original, 1000U) == 0);
        CHECK(gate.original.output.accepted_updates == 0U);
        CHECK(gate.original.output.mode == FOC_VIRTUAL_OUTPUT_HIGH_Z);
        struct foc_runtime_snapshot snapshot;
        CHECK(foc_virtual_current_snapshot(&gate.original, &snapshot) == FOC_RUNTIME_OK);
        CHECK(snapshot.action == FOC_DEVICE_OUTPUT_INHIBIT && snapshot.output_confirmed);
        CHECK(!snapshot.inhibit_pending);
        if (injection == TEST_FAULT) {
            CHECK(snapshot.state == FOC_SAFE_STATE_FAULT_LATCHED);
            CHECK((snapshot.faults & FOC_SAFE_FAULT_SAMPLE_INVALID) != 0U);
        }
    }
    request->argument = NULL;
    return 0;
}

static int test_capture(const char *name)
{
    struct foc_virtual_current_profile profile;
    struct foc_cal_workflow_request workflow;
    CHECK(foc_virtual_calibration_profile(name, &profile, &workflow) == 0);
    profile.initial_state.mechanical_position_rad = TAU * 500000.0 - 2.3;
    profile.cogging_amplitude_nm = 0.003;
    profile.cogging_cycles_per_turn = 12U;
    profile.cogging_phase_rad = 0.19;
    struct test_gate gate = {0};
    CHECK(foc_virtual_current_init(&gate.original, &profile) == 0);
    struct foc_virtual_motor motor = gate.original.motor;
    struct foc_virtual_bootstrap_capture_request request = {
        .identity = workflow.identity,
        .capture = 71U,
        .started_us = 500U,
        .permit = permit,
        .argument = &gate,
    };
    request.identity.acquisition.pole_pairs = 0U;
    request.identity.acquisition.encoder_direction = 0;
    struct foc_virtual_bootstrap_capture_result result;
    CHECK(foc_virtual_bootstrap_capture(&motor, &profile, &request, &result) == 0);
    CHECK(result.reason == FOC_VIRTUAL_BOOTSTRAP_CAPTURE_NONE);
    printf("%s ", name);
    CHECK(check_apertures(&profile, &request, &result) == 0);
    CHECK(motor.state.mechanical_position_rad == profile.initial_state.mechanical_position_rad);
    CHECK(motor.state.mechanical_velocity_rad_s == 0.0);
    CHECK(gate.original.output.mode == FOC_VIRTUAL_OUTPUT_HIGH_Z);
    CHECK(gate.original.output.accepted_updates == 0U);

    struct foc_virtual_motor trial = motor;
    CHECK(foc_virtual_motor_set_fixture_speed(&trial, 0.0) == 0);
    CHECK(
        foc_virtual_bootstrap_capture(&trial, &profile, &request, &result) ==
        FOC_VIRTUAL_BOOTSTRAP_CAPTURE_ERR_EXCITATION
    );
    CHECK(result.reason == FOC_VIRTUAL_BOOTSTRAP_CAPTURE_STALLED && result.count == 0U);

    trial = motor;
    CHECK(foc_virtual_motor_set_load(&trial, 0.55) == 0);
    CHECK(
        foc_virtual_bootstrap_capture(&trial, &profile, &request, &result) ==
        FOC_VIRTUAL_BOOTSTRAP_CAPTURE_ERR_LIMIT
    );
    CHECK(result.reason == FOC_VIRTUAL_BOOTSTRAP_CAPTURE_LIMIT && result.count == 0U);

    trial = motor;
    trial.params.inertia_kg_m2 *= 8.0;
    CHECK(
        foc_virtual_bootstrap_capture(&trial, &profile, &request, &result) ==
        FOC_VIRTUAL_BOOTSTRAP_CAPTURE_ERR_EXCITATION
    );
    CHECK(result.reason == FOC_VIRTUAL_BOOTSTRAP_CAPTURE_FOLLOWING && result.count == 0U);

    trial = motor;
    trial.params.psi_f_wb = 0.0;
    CHECK(
        foc_virtual_bootstrap_capture(&trial, &profile, &request, &result) ==
        FOC_VIRTUAL_BOOTSTRAP_CAPTURE_ERR_EXCITATION
    );
    CHECK(result.reason == FOC_VIRTUAL_BOOTSTRAP_CAPTURE_LOW_SIGNAL && result.count == 0U);

    request.encoder_unavailable = true;
    CHECK(
        foc_virtual_bootstrap_capture(&motor, &profile, &request, &result) ==
        FOC_VIRTUAL_BOOTSTRAP_CAPTURE_ERR_MEASUREMENT
    );
    CHECK(result.reason == FOC_VIRTUAL_BOOTSTRAP_CAPTURE_ENCODER && result.count == 0U);
    request.encoder_unavailable = false;

    struct foc_virtual_current_profile limited = profile;
    limited.current.current_limit_a = 0.0005f;
    CHECK(
        foc_virtual_bootstrap_capture(&motor, &limited, &request, &result) ==
        FOC_VIRTUAL_BOOTSTRAP_CAPTURE_ERR_LIMIT
    );
    CHECK(result.reason == FOC_VIRTUAL_BOOTSTRAP_CAPTURE_LIMIT && result.count == 0U);

    limited = profile;
    limited.vbus_v = 1.1f;
    CHECK(
        foc_virtual_bootstrap_capture(&motor, &limited, &request, &result) ==
        FOC_VIRTUAL_BOOTSTRAP_CAPTURE_ERR_LIMIT
    );
    CHECK(result.reason == FOC_VIRTUAL_BOOTSTRAP_CAPTURE_LIMIT && result.count == 0U);

    request.identity.acquisition.phase_order = profile.adc.phase_order == FOC_DEVICE_PHASE_ORDER_ABC
        ? FOC_DEVICE_PHASE_ORDER_ACB
        : FOC_DEVICE_PHASE_ORDER_ABC;
    CHECK(
        foc_virtual_bootstrap_capture(&motor, &profile, &request, &result) ==
        FOC_VIRTUAL_BOOTSTRAP_CAPTURE_ERR_IDENTITY
    );
    CHECK(result.reason == FOC_VIRTUAL_BOOTSTRAP_CAPTURE_IDENTITY && result.count == 0U);
    request.identity.acquisition.phase_order = profile.adc.phase_order;

    CHECK(check_revocation(&profile, &motor, &request, &result) == 0);
    return 0;
}

/* Test Entry ------------------------------------------------------------------- */

int main(void)
{
    CHECK(test_capture("GENERIC_RESEARCH_MOTOR") == 0);
    CHECK(test_capture("CALIBRATION_SALIENT_MOTOR") == 0);
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
