/**
 * @file    test_foc_mechanical_id_virtual.c
 * @brief   Virtual Motor current/encoder capture for mechanical identification.
 * @version v0.1.0
 * @date    2026.11.18
 *
 * Test role
 * ---------
 * Exercise the production candidate API with actual quantized observations.
 * Keep the truth oracle and intentionally unqualified producers identifiable.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_mechanical_id.h"
#include "foc_virtual_current.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Private Macros --------------------------------------------------------------- */

#define CHECK(c)                                                     \
    do {                                                             \
        if (!(c)) {                                                  \
            fprintf(stderr, "mechanical VM:%d: %s\n", __LINE__, #c); \
            return 1;                                                \
        }                                                            \
    } while (0)

#define CENTER_TICKS  10U
#define HISTORY       (2U * CENTER_TICKS + 1U)
#define FIT_INTERVALS 16000U

/* Private Constants ------------------------------------------------------------ */

static const double tau = 6.28318530717958647692;
static const uint32_t period_us = 500U;

/* Private Types --------------------------------------------------------------- */

enum capture_kind {
    CLEAN,
    KNOWN_LOAD,
    NOISY,
    ORACLE,
    UNKNOWN,
    OMIT_LOAD,
    FRICTION,
    MISALIGNED,
    UNQUALIFIED_LPF,
    LPF
};

struct study_case {
    const char *name;
    double inertia_scale;
    double damping;
    enum capture_kind kind;
};

struct aperture {
    int64_t position_counts;
    uint32_t us;
    float raw_speed_lpf;
    float id;
    float iq;
    float request_iq;
    float truth_speed;
    float truth_torque;
    float load;
};

struct model {
    double inertia;
    double damping;
};

/* Private Helpers -------------------------------------------------------------- */

static struct foc_mechanical_id_config id_config(const struct study_case *test, float kt)
{
    return (struct foc_mechanical_id_config){
        .setup_id = 77U,
        .torque_model_id = test->kind == ORACLE ? 92U : 91U,
        .torque_source = test->kind == ORACLE ? FOC_MECHANICAL_ID_TORQUE_EQUIVALENT
                                              : FOC_MECHANICAL_ID_TORQUE_OBSERVED_DQ,
        .load_source = test->kind == KNOWN_LOAD ? FOC_MECHANICAL_ID_LOAD_OBSERVED
                                                : FOC_MECHANICAL_ID_LOAD_NEGLIGIBLE,
        .min_interval_us = period_us,
        .max_interval_us = period_us,
        .intervals_per_window = 400U,
        .min_windows = 30U,
        .torque_constant_nm_per_a = kt,
        .max_abs_id_a = 0.12f,
        .speed_resolution_rad_s = (float)(tau / 65536.0 / 0.010),
        .max_abs_speed_rad_s = 30.0f,
        .max_abs_torque_nm = 0.3f,
        .min_accel_rms_rad_s2 = 0.25f,
        .min_speed_rms_rad_s = 0.20f,
        .min_excitation = 0.20f,
        .max_residual_rms_nm = 0.0012f,
        .min_inertia_kg_m2 = 0.0004f,
        .max_inertia_kg_m2 = 0.020f,
        .max_damping_nm_s = 0.030f,
    };
}

static int
launch_current(struct foc_virtual_current *vm, const struct foc_virtual_current_profile *profile)
{
    CHECK(foc_virtual_current_init(vm, profile) == 0);
    CHECK(foc_virtual_current_run_until(vm, 0U) == 0);
    CHECK(foc_virtual_current_publish_iq(vm, 0.0f, 0.0f) == FOC_RUNTIME_OK);
    CHECK(foc_virtual_current_queue_action(vm, FOC_RUNTIME_ACTION_ARM) == 0);
    CHECK(foc_virtual_current_run_until(vm, 200U) == 0);
    CHECK(vm->runtime.pipeline.safety.state == FOC_SAFE_STATE_READY);
    CHECK(foc_virtual_current_queue_action(vm, FOC_RUNTIME_ACTION_START) == 0);
    CHECK(foc_virtual_current_run_until(vm, 400U) == 0);
    CHECK(vm->runtime.pipeline.safety.state == FOC_SAFE_STATE_RUNNING);
    return 0;
}

static int perturb(uint32_t *random, unsigned radius)
{
    *random = *random * UINT32_C(1664525) + UINT32_C(1013904223);
    return (int)((*random >> 16U) % (2U * radius + 1U)) - (int)radius;
}

static int capture(
    const struct foc_virtual_current *vm,
    struct foc_encoder_context *encoder,
    enum capture_kind kind,
    uint32_t seq,
    uint32_t *noise,
    struct aperture *out
)
{
    const uint32_t now = vm->virtual_time_us;
    *out = (struct aperture){
        .us = now,
        .request_iq = vm->target_a.q,
        .load = (float)vm->motor.load_torque_nm
    };
    if (kind == ORACLE) {
        const struct foc_virtual_motor_state *s = &vm->motor.state;
        const struct foc_virtual_motor_params *p = &vm->motor.params;
        out->truth_speed = (float)s->mechanical_velocity_rad_s;
        out->truth_torque =
            (float)(1.5 * p->pole_pairs * (p->psi_f_wb + (p->ld_h - p->lq_h) * s->id_a) * s->iq_a);
        return 0;
    }
    uint16_t raw_code;
    CHECK(foc_virtual_motor_make_encoder_code(&vm->motor, &vm->profile.encoder, &raw_code) == 0);
    struct foc_device_adc_raw raw_adc;
    CHECK(
        foc_virtual_motor_make_adc(
            &vm->motor,
            &vm->profile.adc,
            vm->profile.vbus_v,
            seq,
            now,
            &raw_adc
        ) == 0
    );
    if (kind == NOISY) {
        raw_code = (uint16_t)((int)raw_code + perturb(noise, 3U));
        raw_adc.rank2_raw = (uint16_t)((int)raw_adc.rank2_raw + perturb(noise, 1U));
        raw_adc.rank3_raw = (uint16_t)((int)raw_adc.rank3_raw + perturb(noise, 1U));
        raw_adc.rank4_raw = (uint16_t)((int)raw_adc.rank4_raw + perturb(noise, 1U));
    }
    struct foc_encoder_feedback encoder_out;
    CHECK(
        foc_encoder_update(encoder, now, raw_code, FOC_ENCODER_TRANSPORT_OK, &encoder_out) ==
        FOC_ENCODER_STATUS_OK
    );
    CHECK(encoder_out.captured_us == now && (seq == 0U || encoder_out.speed_valid));
    struct foc_device_measurement adc_out;
    CHECK(
        foc_device_decode_sample(&raw_adc, &vm->profile.adc, &adc_out) == FOC_DEVICE_STATUS_OK &&
        adc_out.sample_tick == now && adc_out.sample_seq == seq
    );
    float phase;
    CHECK(
        foc_rotor_phase_from_counts(
            &vm->profile.runtime.motion.rotor,
            encoder_out.mechanical_phase_count,
            vm->profile.encoder.counts_per_revolution,
            &phase
        ) == FOC_STATUS_OK
    );
    struct foc_vector_alpha_beta ab;
    struct foc_vector_dq dq;
    CHECK(
        foc_clarke(&adc_out.phase_current_a, &ab) == FOC_STATUS_OK &&
        foc_park(&ab, phase, &dq) == FOC_STATUS_OK
    );
    out->position_counts = encoder->position_counts;
    out->raw_speed_lpf = encoder_out.filtered_velocity_rad_s;
    out->id = dq.d;
    out->iq = dq.q;
    return 0;
}

static double residual(const struct foc_mechanical_id_context *context, const struct model *model)
{
    const struct foc_mechanical_id_gram *g = &context->gram;
    const double j = model->inertia;
    const double b = model->damping;
    const double r2 =
        g->zz - 2.0 * (j * g->xz + b * g->yz) + j * j * g->xx + 2.0 * j * b * g->xy + b * b * g->yy;
    return sqrt(fmax(0.0, r2) / context->result.windows);
}

static void
counterfactual(const struct study_case *test, const struct foc_mechanical_id_context *ctx)
{
    const struct foc_mechanical_id_gram *g = &ctx->gram;
    const double det = g->xx * g->yy - g->xy * g->xy;
    if (det > 0.0) {
        /* Diagnostic only. These records are never exported as usable candidates. */
        const struct model invalid = {
            (g->xz * g->yy - g->yz * g->xy) / det,
            (g->yz * g->xx - g->xz * g->xy) / det
        };
        printf(
            "COUNTERFACTUAL,%s,%.10g,%.10g,%.10g\n",
            test->name,
            invalid.inertia,
            invalid.damping,
            residual(ctx, &invalid)
        );
    }
}

static int run_capture(
    const struct study_case *test,
    uint32_t capture_id,
    const struct model *frozen,
    bool trace
)
{
    struct foc_virtual_current_profile profile = foc_virtual_current_default_profile();
    profile.motor.inertia_kg_m2 *= test->inertia_scale;
    profile.motor.damping_nm_s = test->damping;
    const float kt = (float)(1.5 * profile.motor.pole_pairs * profile.motor.psi_f_wb);
    const struct foc_mechanical_id_config policy = id_config(test, kt);
    struct foc_mechanical_id_context id;
    CHECK(foc_mechanical_id_init(&id, &policy, capture_id) == FOC_MECHANICAL_ID_OK);
    struct foc_virtual_current vm;
    CHECK(launch_current(&vm, &profile) == 0);
    if (test->kind == KNOWN_LOAD || test->kind == UNKNOWN || test->kind == OMIT_LOAD) {
        CHECK(foc_virtual_motor_set_load(&vm.motor, 0.018) == 0);
    }
    struct foc_encoder_context encoder;
    CHECK(foc_encoder_init(&encoder, &profile.encoder) == FOC_ENCODER_STATUS_OK);
    struct aperture history[HISTORY] = {0};
    uint32_t rng = frozen == NULL ? UINT32_C(42319) : UINT32_C(91673);
    double max_id = 0.0, max_speed = 0.0, max_q_discrepancy = 0.0;
    for (uint32_t i = 0U; i <= FIT_INTERVALS + 2U * CENTER_TICKS; ++i) {
        const uint32_t tick = 1000U + i * period_us;
        CHECK(foc_virtual_current_run_until(&vm, tick) == 0);
        CHECK(
            vm.runtime.pipeline.safety.state == FOC_SAFE_STATE_RUNNING &&
            vm.output.rejected_updates == 0U && vm.output.saturated_updates == 0U
        );
        CHECK(capture(&vm, &encoder, test->kind, i, &rng, &history[i % HISTORY]) == 0);
        if (i >= 2U * CENTER_TICKS) {
            const struct aperture *before = &history[(i - 2U * CENTER_TICKS) % HISTORY];
            const struct aperture *after = &history[i % HISTORY];
            const struct aperture *center = &history[(i - CENTER_TICKS) % HISTORY];
            CHECK(center->us - before->us == after->us - center->us);
            const float centered_speed =
                (float)((after->position_counts - before->position_counts) *
                        (tau / profile.encoder.counts_per_revolution) /
                        ((after->us - before->us) * 1e-6));
            struct foc_mechanical_id_sample sample = {
                .setup_id = policy.setup_id,
                .torque_model_id = policy.torque_model_id,
                .capture_id = capture_id,
                .torque_source = policy.torque_source,
                .load_source =
                    test->kind == UNKNOWN ? FOC_MECHANICAL_ID_LOAD_UNKNOWN : policy.load_source,
                .sequence = i - CENTER_TICKS,
                .speed_us = center->us,
                .torque_us = test->kind == MISALIGNED ? center->us + period_us : center->us,
                .interval_begin_us = i == 2U * CENTER_TICKS ? center->us : center->us - period_us,
                .speed_rad_s = test->kind == ORACLE                      ? center->truth_speed
                    : test->kind == LPF || test->kind == UNQUALIFIED_LPF ? center->raw_speed_lpf
                                                                         : centered_speed,
                .id_a = center->id,
                .iq_a = center->iq,
                .torque_nm = center->truth_torque,
                .load_nm = test->kind == KNOWN_LOAD ? center->load : 0.0f,
                .speed_valid = true,
                .torque_valid = true,
                .frame_confirmed = true,
                .interval_confirmed = test->kind != UNQUALIFIED_LPF,
            };
            const int status = foc_mechanical_id_push(&id, &sample);
            if (test->kind == UNKNOWN || test->kind == MISALIGNED ||
                test->kind == UNQUALIFIED_LPF) {
                CHECK(status < 0 && id.result.samples == 0U);
                const enum foc_calibration_failure why = test->kind == MISALIGNED
                    ? FOC_CALIBRATION_FAILURE_TIME_ORDER
                    : FOC_CALIBRATION_FAILURE_PRECONDITION;
                CHECK(id.result.failure == why);
                printf(
                    "REFUSAL,%s,%d,%u,%u\n",
                    test->name,
                    status,
                    (unsigned)why,
                    id.result.samples
                );
                CHECK(foc_virtual_current_request_stop(&vm) == FOC_RUNTIME_OK);
                const struct foc_runtime_fast_input stop = {.now_us = vm.virtual_time_us};
                CHECK(foc_runtime_h0_step(&vm.runtime, &stop) == FOC_RUNTIME_ERR_STOP);
                return 0;
            }
            CHECK(status == FOC_MECHANICAL_ID_OK);
            max_id = fmax(max_id, fabs(sample.id_a));
            max_speed = fmax(max_speed, fabs(sample.speed_rad_s));
            /* Compare with the simultaneous controller command, only as a diagnostic. */
            if (test->kind != ORACLE) {
                max_q_discrepancy = fmax(max_q_discrepancy, fabs(sample.iq_a - center->request_iq));
            }
            if (trace) {
                printf(
                    "OBS,%s,%u,%u,%.9g,%.9g,%.9g,%.9g,%.9g,%u\n",
                    test->name,
                    sample.sequence,
                    sample.speed_us,
                    sample.speed_rad_s,
                    sample.id_a,
                    sample.iq_a,
                    policy.torque_source == FOC_MECHANICAL_ID_TORQUE_EQUIVALENT
                        ? sample.torque_nm
                        : (double)kt * sample.iq_a,
                    sample.load_nm,
                    vm.output.accepted_updates
                );
            }
        }
        const double t = (tick - 1000U) * 1e-6;
        const float request = frozen == NULL
            ? (float)(0.32 * sin(tau * 0.9 * t) + 0.12 * sin(tau * 1.7 * t + 0.4))
            : (float)(0.28 * sin(tau * 0.64 * t + 0.3) + 0.13 * sin(tau * 1.31 * t - 0.6));
        CHECK(foc_virtual_current_publish_iq(&vm, 0.0f, request) == FOC_RUNTIME_OK);
        if (test->kind == FRICTION) {
            CHECK(
                foc_virtual_motor_set_load(
                    &vm.motor,
                    0.020 * tanh(vm.motor.state.mechanical_velocity_rad_s / 0.5)
                ) == 0
            );
        }
    }
    CHECK(
        id.result.windows == 40U && id.result.samples == FIT_INTERVALS + 1U &&
        id.result.accel_windows > 0U && id.result.decel_windows > 0U
    );
    const uint32_t updates = vm.output.accepted_updates;
    CHECK(foc_virtual_current_request_stop(&vm) == FOC_RUNTIME_OK);
    CHECK(foc_virtual_current_run_until(&vm, vm.virtual_time_us + 400U) == 0);
    CHECK(
        vm.output.accepted_updates == updates &&
        vm.output.request.action == FOC_DEVICE_OUTPUT_INHIBIT
    );
    if (frozen != NULL) {
        const double rms = residual(&id, frozen);
        printf(
            "PREDICT,%s,%.10g,%.10g,%.10g,%u,%u\n",
            test->name,
            frozen->inertia,
            frozen->damping,
            rms,
            id.result.windows,
            (unsigned)(rms <= 0.0012)
        );
        return 0;
    }
    struct foc_mechanical_id_result result;
    const int status = foc_mechanical_id_finish(&id, &result);
    const double j_error = result.usable
        ? 100.0 * fabs(result.inertia_kg_m2 / profile.motor.inertia_kg_m2 - 1.0)
        : 0.0;
    const double b_error =
        result.usable ? 100.0 * fabs(result.damping_nm_s / profile.motor.damping_nm_s - 1.0) : 0.0;
    const bool qualified = test->kind != OMIT_LOAD && test->kind != FRICTION && test->kind != LPF;
    const bool quality = qualified && result.usable && j_error <= 3.0 && b_error <= 8.0;
    printf(
        "FIT,%s,%d,%u,%u,%u,%.10g,%.10g,%.10g,%.10g,%.8g,%.8g,%.10g,%.9g,"
        "%.9g,%.9g,%.9g,%.9g,%u,%u,%u,%u\n",
        test->name,
        status,
        (unsigned)result.failure,
        result.samples,
        result.windows,
        profile.motor.inertia_kg_m2,
        profile.motor.damping_nm_s,
        (double)result.inertia_kg_m2,
        (double)result.damping_nm_s,
        j_error,
        b_error,
        result.residual_rms_nm,
        result.gram_condition,
        result.accel_rms_rad_s2,
        result.speed_rms_rad_s,
        max_id,
        max_speed,
        result.accel_windows,
        result.decel_windows,
        (unsigned)result.usable,
        (unsigned)quality
    );
    printf(
        "ACQUISITION,%s,%.9g,%u,%u\n",
        test->name,
        max_q_discrepancy,
        updates,
        vm.output.accepted_updates
    );
    if (!qualified) {
        counterfactual(test, &id);
    }
    if (test->kind == FRICTION || test->kind == OMIT_LOAD) {
        CHECK(
            !result.usable &&
            (result.failure == FOC_CALIBRATION_FAILURE_RESIDUAL ||
             result.failure == FOC_CALIBRATION_FAILURE_RANGE)
        );
    }
    return 0;
}

/* Test Entry ------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    static const struct study_case cases[] = {
        {"nominal", 1.0, 0.006, CLEAN},
        {"half", 0.5, 0.004, CLEAN},
        {"double", 2.0, 0.010, CLEAN},
        {"quad", 4.0, 0.006, CLEAN},
        {"known-load", 1.0, 0.009, KNOWN_LOAD},
        {"quantized-noise", 1.0, 0.006, NOISY},
        {"oracle", 4.0, 0.006, ORACLE},
        {"unknown", 1.0, 0.006, UNKNOWN},
        {"omitted-load", 1.0, 0.006, OMIT_LOAD},
        {"coulomb-like", 1.0, 0.006, FRICTION},
        {"timestamp-mismatch", 1.0, 0.006, MISALIGNED},
        {"unqualified-lpf", 1.0, 0.006, UNQUALIFIED_LPF},
        {"unaligned-lpf", 1.0, 0.006, LPF},
    };
    const bool trace = argc > 1 && strcmp(argv[argc - 1], "--trace") == 0;
    argc -= trace;
    if (argc == 12 && strcmp(argv[1], "--predict") == 0) {
        for (unsigned i = 0U; i < 5U; ++i) {
            char *end_j, *end_b;
            const struct model frozen = {
                strtod(argv[2U + 2U * i], &end_j),
                strtod(argv[3U + 2U * i], &end_b)
            };
            if (end_j == argv[2U + 2U * i] || end_b == argv[3U + 2U * i] || *end_j != '\0' ||
                *end_b != '\0' || !isfinite(frozen.inertia) || !isfinite(frozen.damping) ||
                frozen.inertia <= 0.0 || frozen.damping < 0.0) {
                return 2;
            }
            CHECK(run_capture(&cases[i], 200U + i, &frozen, trace) == 0);
        }
        return 0;
    }
    if (argc != 1) {
        return 2;
    }
    for (unsigned i = 0U; i < sizeof cases / sizeof cases[0]; ++i) {
        CHECK(run_capture(&cases[i], 100U + i, NULL, trace) == 0);
    }
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
