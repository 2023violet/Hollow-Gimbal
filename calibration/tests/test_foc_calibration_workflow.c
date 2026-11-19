/**
 * @file    test_foc_calibration_workflow.c
 * @brief   Workflow lifecycle, typed observations and rejected candidates.
 * @version v0.1.0
 * @date    2026.11.18
 *
 * Test role
 * ---------
 * Check source and session admission before invoking native estimators.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_calibration_workflow.h"

#include <math.h>
#include <stdio.h>

/* Private Helpers -------------------------------------------------------------- */

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return 1;                                                       \
        }                                                                   \
    } while (0)

static const struct foc_cal_workflow_identity identity = {
    .acquisition = {4095U, 0.002f, 1024U, 1, 0U, 4U},
    .motor_setup_id = 17U,
    .encoder_id = 31U,
    .mechanical_frame_id = 41U,
};

static struct foc_cal_workflow_request request(uint32_t session, uint32_t stages)
{
    return (struct foc_cal_workflow_request){
        .session = session,
        .profile = FOC_CAL_WORKFLOW_SELECTED_STAGES,
        .stages = stages,
        .identity = identity,
    };
}

static int open_adc(struct foc_cal_workflow *workflow)
{
    const struct foc_cal_workflow_stage_request stage = {
        .stage = FOC_CAL_WORKFLOW_ADC,
        .source = FOC_CAL_WORKFLOW_HOST_SYNTHETIC,
        .capture = 71U,
        .max_age_us = 50U,
        .policy.adc = {
            .identity = identity.acquisition,
            .required_valid_mask = 7U,
            .minimum_samples = 4U,
            .maximum_samples = 12U,
            .maximum_sample_step_counts = 8.0f,
            .maximum_noise_rms_counts = 2.0f,
            .minimum_offset_counts = 2020.0f,
            .maximum_offset_counts = 2080.0f,
            .maximum_interval_us = 30U,
        },
    };
    return foc_cal_workflow_open(workflow, &stage);
}

static struct foc_cal_workflow_sample adc(uint32_t session, uint32_t index)
{
    return (struct foc_cal_workflow_sample){
        .session = session,
        .capture = 71U,
        .identity = identity,
        .source = FOC_CAL_WORKFLOW_HOST_SYNTHETIC,
        .units = FOC_CAL_WORKFLOW_RAW_COUNTS,
        .valid = true,
        .data.adc = {
            .phase_a = (uint16_t)(2048U + index % 2U),
            .phase_b = 2046U,
            .phase_c = 2049U,
            .valid_mask = 7U,
            .sequence = UINT32_MAX - 1U + index,
            .captured_us = UINT32_MAX - 24U + index * 10U,
            .zero_current_confirmed = true,
        },
    };
}

static int lifecycle(void)
{
    const uint32_t stages = FOC_CAL_WORKFLOW_STAGE_BIT(FOC_CAL_WORKFLOW_ADC);
    struct foc_cal_workflow first = {0};
    struct foc_cal_workflow second = {0};
    struct foc_cal_workflow_request selected = request(1U, stages);
    CHECK(foc_cal_workflow_start(&first, &selected) == FOC_CAL_WORKFLOW_ERR_STATE);
    CHECK(foc_cal_workflow_init(&first) == FOC_CAL_WORKFLOW_OK);
    CHECK(foc_cal_workflow_init(&second) == FOC_CAL_WORKFLOW_OK);
    selected.stages = FOC_CAL_WORKFLOW_STAGE_BIT(FOC_CAL_WORKFLOW_TUNING);
    CHECK(foc_cal_workflow_start(&first, &selected) == FOC_CAL_WORKFLOW_ERR_DEPENDENCY);
    selected.stages = stages;
    CHECK(foc_cal_workflow_start(&first, &selected) == FOC_CAL_WORKFLOW_OK);
    CHECK(foc_cal_workflow_start(&second, &selected) == FOC_CAL_WORKFLOW_OK);
    CHECK(foc_cal_workflow_start(&first, &selected) == FOC_CAL_WORKFLOW_ERR_STATE);
    CHECK(open_adc(&first) == FOC_CAL_WORKFLOW_OK && open_adc(&second) == FOC_CAL_WORKFLOW_OK);
    CHECK(foc_cal_workflow_init(&first) == FOC_CAL_WORKFLOW_ERR_STATE);
    for (uint32_t index = 0U; index < 6U; ++index) {
        const struct foc_cal_workflow_sample input = adc(1U, index);
        CHECK(
            foc_cal_workflow_push(&first, &input, input.data.adc.captured_us + 1U) ==
            FOC_CAL_WORKFLOW_OK
        );
    }
    CHECK(foc_cal_workflow_finish_stage(&first) == FOC_CAL_WORKFLOW_OK);
    CHECK(foc_cal_workflow_finish(&first) == FOC_CAL_WORKFLOW_OK);
    const struct foc_cal_workflow_bundle *out = foc_cal_workflow_result(&first);
    CHECK(out->state == FOC_CAL_WORKFLOW_COMPLETED && out->complete && out->passed == stages);
    CHECK(!out->applicable && out->adc.usable && out->adc.offset_a_counts == 2048.5f);
    CHECK(
        out->stages[FOC_CAL_WORKFLOW_ADC].primary.samples == 6U &&
        out->stages[FOC_CAL_WORKFLOW_ADC].primary.first_us >
            out->stages[FOC_CAL_WORKFLOW_ADC].primary.last_us
    );
    CHECK(foc_cal_workflow_review(&first, 1U, &identity, true) == FOC_CAL_WORKFLOW_ERR_REVIEW);
    CHECK(foc_cal_workflow_result(&second)->stages[FOC_CAL_WORKFLOW_ADC].primary.samples == 0U);
    CHECK(foc_cal_workflow_cancel(&second) == FOC_CAL_WORKFLOW_OK);
    CHECK(foc_cal_workflow_result(&second)->state == FOC_CAL_WORKFLOW_CANCELLED);
    CHECK(foc_cal_workflow_start(&second, &selected) == FOC_CAL_WORKFLOW_ERR_ORDER);
    selected.session = 2U;
    CHECK(foc_cal_workflow_start(&second, &selected) == FOC_CAL_WORKFLOW_OK);
    CHECK(open_adc(&second) == FOC_CAL_WORKFLOW_OK);
    struct foc_cal_workflow_sample input = adc(1U, 0U);
    CHECK(
        foc_cal_workflow_push(&second, &input, input.data.adc.captured_us) ==
        FOC_CAL_WORKFLOW_ERR_ORDER
    );
    input.session = 2U;
    input.identity.encoder_id++;
    CHECK(
        foc_cal_workflow_push(&second, &input, input.data.adc.captured_us) ==
        FOC_CAL_WORKFLOW_ERR_IDENTITY
    );
    CHECK(foc_cal_workflow_result(&second)->state == FOC_CAL_WORKFLOW_REJECTED);
    CHECK(out->adc.usable && out->complete);
    puts("WORKFLOW lifecycle, ADC quality, timestamp wrap, stale and foreign sessions PASS");
    return 0;
}

static int failed_capture(void)
{
    struct foc_cal_workflow context = {0};
    CHECK(foc_cal_workflow_init(&context) == FOC_CAL_WORKFLOW_OK);
    for (uint32_t attempt = 1U; attempt <= 5U; ++attempt) {
        const struct foc_cal_workflow_request selected = request(
            attempt,
            FOC_CAL_WORKFLOW_STAGE_BIT(FOC_CAL_WORKFLOW_ADC) |
                FOC_CAL_WORKFLOW_STAGE_BIT(FOC_CAL_WORKFLOW_LUT)
        );
        CHECK(foc_cal_workflow_start(&context, &selected) == FOC_CAL_WORKFLOW_OK);
        CHECK(open_adc(&context) == FOC_CAL_WORKFLOW_OK);
        struct foc_cal_workflow_sample sample = adc(attempt, 0U);
        uint32_t now = sample.data.adc.captured_us;
        int expected = FOC_CAL_WORKFLOW_ERR_ORDER;
        if (attempt == 1U) {
            CHECK(foc_cal_workflow_push(&context, &sample, now) == FOC_CAL_WORKFLOW_OK);
        } else if (attempt == 2U) {
            now += 51U;
        } else if (attempt == 3U) {
            sample.units = FOC_CAL_WORKFLOW_RAD_PAIR;
            expected = FOC_CAL_WORKFLOW_ERR_SOURCE;
        } else if (attempt == 4U) {
            sample.data.adc.zero_current_confirmed = false;
            expected = FOC_CAL_WORKFLOW_ERR_QUALITY;
        } else {
            CHECK(
                foc_cal_workflow_data_required(&context, FOC_CAL_WORKFLOW_ADC) ==
                FOC_CAL_WORKFLOW_OK
            );
            expected = FOC_CAL_WORKFLOW_ERR_STATE;
        }
        CHECK(foc_cal_workflow_push(&context, &sample, now) == expected);
        CHECK(
            foc_cal_workflow_data_required(&context, FOC_CAL_WORKFLOW_LUT) == FOC_CAL_WORKFLOW_OK
        );
        CHECK(foc_cal_workflow_finish(&context) == FOC_CAL_WORKFLOW_PARTIAL);
        const struct foc_cal_workflow_bundle *out = foc_cal_workflow_result(&context);
        CHECK(out->state == FOC_CAL_WORKFLOW_REJECTED && !out->applicable && out->missing != 0U);
    }
    puts("WORKFLOW duplicate, age, units, zero-current evidence and missing data PASS");
    return 0;
}

static struct foc_cal_workflow_stage_request mechanical_capture(void)
{
    return (struct foc_cal_workflow_stage_request){
        .stage = FOC_CAL_WORKFLOW_MECHANICAL,
        .source = FOC_CAL_WORKFLOW_HOST_SYNTHETIC,
        .capture = 81U,
        .max_age_us = 20U,
        .policy.mechanical = {
            .setup_id = 17U,
            .torque_model_id = 13U,
            .torque_source = FOC_MECHANICAL_ID_TORQUE_EQUIVALENT,
            .load_source = FOC_MECHANICAL_ID_LOAD_OBSERVED,
            .min_interval_us = 900U,
            .max_interval_us = 1100U,
            .intervals_per_window = 100U,
            .min_windows = 6U,
            .torque_constant_nm_per_a = 0.15f,
            .max_abs_id_a = 0.1f,
            .speed_resolution_rad_s = 0.001f,
            .max_abs_speed_rad_s = 20.0f,
            .max_abs_torque_nm = 2.0f,
            .min_accel_rms_rad_s2 = 0.1f,
            .min_speed_rms_rad_s = 0.1f,
            .min_excitation = 0.1f,
            .max_residual_rms_nm = 0.0001f,
            .min_inertia_kg_m2 = 0.0005f,
            .max_inertia_kg_m2 = 0.1f,
            .max_damping_nm_s = 0.1f,
        },
    };
}

static int mechanical_samples(struct foc_cal_workflow *workflow, uint32_t session, bool excitation)
{
    uint32_t previous = 0U, now = 0U;
    for (uint32_t index = 0U; index <= 2400U; ++index) {
        if (index != 0U) {
            now += (index % 2U) != 0U ? 940U : 1060U;
        }
        const double p = 6.283185307179586 * (double)now * 1e-6;
        const double speed = excitation ? 2.0 * sin(p) + 0.5 * cos(p * 0.5) : 0.0;
        const double acceleration =
            excitation ? 12.566370614359172 * cos(p) - 1.570796326794897 * sin(p * 0.5) : 0.0;
        const double load = 0.025 + 0.004 * cos(p);
        const struct foc_cal_workflow_sample sample = {
            .session = session,
            .capture = 81U,
            .identity = identity,
            .source = FOC_CAL_WORKFLOW_HOST_SYNTHETIC,
            .units = FOC_CAL_WORKFLOW_MECHANICAL_SI,
            .valid = true,
            .output_confirmed = true,
            .data.mechanical = {
                .setup_id = 17U,
                .torque_model_id = 13U,
                .capture_id = 81U,
                .torque_source = FOC_MECHANICAL_ID_TORQUE_EQUIVALENT,
                .load_source = FOC_MECHANICAL_ID_LOAD_OBSERVED,
                .sequence = index,
                .speed_us = now,
                .torque_us = now,
                .interval_begin_us = previous,
                .speed_rad_s = (float)speed,
                .torque_nm = (float)(0.002 * acceleration + 0.006 * speed + load),
                .load_nm = (float)load,
                .speed_valid = true,
                .torque_valid = true,
                .frame_confirmed = true,
                .interval_confirmed = true,
            },
        };
        CHECK(foc_cal_workflow_push(workflow, &sample, now + 7U) == FOC_CAL_WORKFLOW_OK);
        previous = now;
    }
    return 0;
}

static int model_dependency(void)
{
    struct foc_cal_workflow workflow = {0};
    CHECK(foc_cal_workflow_init(&workflow) == FOC_CAL_WORKFLOW_OK);
    struct foc_cal_workflow_request selected = request(
        1U,
        FOC_CAL_WORKFLOW_STAGE_BIT(FOC_CAL_WORKFLOW_MECHANICAL) |
            FOC_CAL_WORKFLOW_STAGE_BIT(FOC_CAL_WORKFLOW_TUNING)
    );
    CHECK(foc_cal_workflow_start(&workflow, &selected) == FOC_CAL_WORKFLOW_OK);
    const struct foc_impedance_tuning_input dynamics = {
        .target_natural_frequency_rad_s = 10.0f,
        .target_damping_ratio = 0.7f,
        .max_stiffness_nm_per_rad = 1.0f,
        .max_damping_nm_s_per_rad = 0.3f,
        .iq_limit_a = 1.0f,
        .sample_period_s = 0.001f,
    };
    CHECK(foc_cal_workflow_tune(&workflow, NULL, &dynamics) == FOC_CAL_WORKFLOW_ERR_DEPENDENCY);
    struct foc_cal_workflow_stage_request capture = mechanical_capture();
    capture.policy.mechanical.load_source = FOC_MECHANICAL_ID_LOAD_UNKNOWN;
    CHECK(foc_cal_workflow_open(&workflow, &capture) == FOC_CAL_WORKFLOW_ERR_INPUT);
    capture = mechanical_capture();
    CHECK(foc_cal_workflow_open(&workflow, &capture) == FOC_CAL_WORKFLOW_OK);
    CHECK(mechanical_samples(&workflow, 1U, false) == 0);
    CHECK(foc_cal_workflow_finish_stage(&workflow) == FOC_CAL_WORKFLOW_ERR_QUALITY);
    const struct foc_cal_workflow_bundle *result = foc_cal_workflow_result(&workflow);
    CHECK(result->stages[FOC_CAL_WORKFLOW_MECHANICAL].quality == FOC_CAL_WORKFLOW_UNIDENTIFIABLE);
    CHECK(!result->mechanical.usable);
    CHECK(foc_cal_workflow_tune(&workflow, NULL, &dynamics) == FOC_CAL_WORKFLOW_ERR_DEPENDENCY);
    CHECK(foc_cal_workflow_finish(&workflow) == FOC_CAL_WORKFLOW_PARTIAL);

    selected.session = 2U;
    CHECK(foc_cal_workflow_start(&workflow, &selected) == FOC_CAL_WORKFLOW_OK);
    CHECK(foc_cal_workflow_open(&workflow, &capture) == FOC_CAL_WORKFLOW_OK);
    CHECK(mechanical_samples(&workflow, 2U, true) == 0);
    CHECK(foc_cal_workflow_finish_stage(&workflow) == FOC_CAL_WORKFLOW_OK);
    CHECK(foc_cal_workflow_tune(&workflow, NULL, &dynamics) == FOC_CAL_WORKFLOW_ERR_DATA_REQUIRED);
    CHECK(result->stages[FOC_CAL_WORKFLOW_TUNING].quality == FOC_CAL_WORKFLOW_DATA_REQUIRED);
    CHECK(!result->tuning.valid);
    const struct foc_cal_workflow_kt kt = {
        .identity = identity,
        .torque_model_id = 13U,
        .nm_per_a = 0.15f,
        .qualified = true,
        .source = FOC_CAL_WORKFLOW_HOST_SYNTHETIC,
        .capture = 82U,
        .captured_us = 5U,
    };
    CHECK(foc_cal_workflow_tune(&workflow, &kt, &dynamics) == FOC_CAL_WORKFLOW_OK);
    CHECK(foc_cal_workflow_finish(&workflow) == FOC_CAL_WORKFLOW_OK);
    CHECK(
        result->complete && !result->applicable && result->tuning.valid &&
        result->mechanical.usable && fabsf(result->mechanical.inertia_kg_m2 - 0.002f) < 1e-6f
    );
    puts("WORKFLOW J/B jitter, unidentifiable fit, unknown load, qualified Kt and K/D PASS");
    return 0;
}

static int native_capture_rejections(void)
{
    struct foc_cal_workflow workflow = {0};
    CHECK(foc_cal_workflow_init(&workflow) == FOC_CAL_WORKFLOW_OK);
    const struct foc_cal_workflow_request selected = request(
        1U,
        FOC_CAL_WORKFLOW_STAGE_BIT(FOC_CAL_WORKFLOW_ELECTRICAL) |
            FOC_CAL_WORKFLOW_STAGE_BIT(FOC_CAL_WORKFLOW_ROTOR) |
            FOC_CAL_WORKFLOW_STAGE_BIT(FOC_CAL_WORKFLOW_LUT)
    );
    CHECK(foc_cal_workflow_start(&workflow, &selected) == FOC_CAL_WORKFLOW_OK);
    const struct foc_cal_workflow_stage_request electrical = {
        .stage = FOC_CAL_WORKFLOW_ELECTRICAL,
        .source = FOC_CAL_WORKFLOW_HOST_SYNTHETIC,
        .capture = 91U,
        .max_age_us = 30U,
        .policy.electrical = {
            .axis = FOC_MOTOR_ID_D,
            .source = FOC_MOTOR_ID_VIRTUAL,
            .voltage_source = FOC_MOTOR_ID_VOLTAGE_VIRTUAL_TERMINAL,
            .setup_id = 17U,
            .min_interval_us = 25U,
            .max_interval_us = 50U,
            .intervals_per_window = 4U,
            .min_windows = 6U,
            .max_abs_current_a = 3.0f,
            .max_abs_voltage_v = 45.0f,
            .min_current_rms_a = 0.1f,
            .min_slew_rms_a_per_s = 5.0f,
            .min_voltage_rms_v = 0.05f,
            .max_residual_rms_v = 0.04f,
            .min_rs_ohm = 0.03f,
            .max_rs_ohm = 2.0f,
            .min_inductance_h = 0.0004f,
            .max_inductance_h = 0.01f,
        },
    };
    CHECK(foc_cal_workflow_open(&workflow, &electrical) == FOC_CAL_WORKFLOW_OK);
    struct foc_cal_workflow_sample sample = {
        .session = 1U,
        .capture = 91U,
        .identity = identity,
        .source = FOC_CAL_WORKFLOW_HOST_SYNTHETIC,
        .units = FOC_CAL_WORKFLOW_ELECTRICAL_SI,
        .valid = true,
        .saturated = true,
    };
    CHECK(foc_cal_workflow_push(&workflow, &sample, 0U) == FOC_CAL_WORKFLOW_ERR_QUALITY);
    const struct foc_cal_workflow_bundle *bundle = foc_cal_workflow_result(&workflow);
    CHECK(
        bundle->stages[FOC_CAL_WORKFLOW_ELECTRICAL].quality == FOC_CAL_WORKFLOW_INVALID_OBSERVATION
    );

    const struct foc_cal_workflow_stage_request rotor = {
        .stage = FOC_CAL_WORKFLOW_ROTOR,
        .source = FOC_CAL_WORKFLOW_HOST_SYNTHETIC,
        .capture = 92U,
        .max_age_us = 30U,
        .policy.rotor = {
            .identity = identity.acquisition,
            .minimum_pole_pairs = 2U,
            .maximum_pole_pairs = 6U,
            .minimum_mechanical_span_rad = 0.2f,
            .maximum_residual_rad = 0.01f,
            .minimum_candidate_separation_rad = 0.01f,
            .maximum_interval_us = 30U,
            .maximum_samples = 16U,
        },
    };
    CHECK(foc_cal_workflow_open(&workflow, &rotor) == FOC_CAL_WORKFLOW_ERR_DEPENDENCY);
    /* This combined policy cannot fit angles against an unqualified LUT. */
    CHECK(foc_cal_workflow_data_required(&workflow, FOC_CAL_WORKFLOW_ROTOR) == FOC_CAL_WORKFLOW_OK);
    const struct foc_cal_workflow_stage_request lut = {
        .stage = FOC_CAL_WORKFLOW_LUT,
        .source = FOC_CAL_WORKFLOW_HOST_SYNTHETIC,
        .capture = 93U,
        .max_age_us = 30U,
        .policy.lut = {
            .encoder =
                {1024U, FOC_ENCODER_DIRECTION_NORMAL, 1000.0f, 1U, 2000U, 80.0f, 0.85f, 0.2f},
            .encoder_identity = 31U,
            .reference_identity = 41U,
            .source = FOC_ENCODER_CAL_SOURCE_VIRTUAL,
            .nodes = 8U,
            .min_samples_per_segment = 8U,
            .reference_zero_rad = 0.31,
            .max_zero_bias_rad = 0.004,
            .max_training_segment_rms_rad = 0.004,
            .max_holdout_rms_rad = 0.004,
            .max_holdout_peak_rad = 0.006,
        },
    };
    CHECK(foc_cal_workflow_open(&workflow, &lut) == FOC_CAL_WORKFLOW_OK);
    sample.capture = 93U;
    sample.units = FOC_CAL_WORKFLOW_ENCODER_COUNTS_RAD;
    sample.saturated = false;
    for (uint32_t index = 0U; index < 8U; ++index) {
        sample.data.lut = (struct foc_encoder_cal_sample){
            .setup =
                {31U, 41U, 1024U, FOC_ENCODER_DIRECTION_NORMAL, FOC_ENCODER_CAL_SOURCE_VIRTUAL},
            .capture_id = 93U,
            .sequence = index,
            .encoder_us = index * 10U,
            .reference_us = index * 10U,
            .raw_code = index,
            .reference_rad = 0.31 + (double)index * (6.283185307179586 / 1024.0),
            .encoder_valid = true,
            .reference_valid = true,
            .reference_confirmed = true,
        };
        CHECK(foc_cal_workflow_push(&workflow, &sample, index * 10U) == FOC_CAL_WORKFLOW_OK);
    }
    CHECK(foc_cal_workflow_finish_stage(&workflow) == FOC_CAL_WORKFLOW_ERR_DEPENDENCY);
    CHECK(foc_cal_workflow_begin_holdout(&workflow, 93U) == FOC_CAL_WORKFLOW_ERR_ORDER);
    CHECK(foc_cal_workflow_begin_holdout(&workflow, 94U) == FOC_CAL_WORKFLOW_ERR_QUALITY);
    CHECK(bundle->stages[FOC_CAL_WORKFLOW_LUT].quality == FOC_CAL_WORKFLOW_UNIDENTIFIABLE);
    CHECK(foc_cal_workflow_finish(&workflow) == FOC_CAL_WORKFLOW_PARTIAL);
    CHECK(!bundle->applicable && bundle->passed == 0U && bundle->missing == selected.stages);
    const struct foc_cal_workflow_request rotor_only =
        request(2U, FOC_CAL_WORKFLOW_STAGE_BIT(FOC_CAL_WORKFLOW_ROTOR));
    CHECK(foc_cal_workflow_start(&workflow, &rotor_only) == FOC_CAL_WORKFLOW_OK);
    CHECK(foc_cal_workflow_open(&workflow, &rotor) == FOC_CAL_WORKFLOW_OK);
    sample.session = 2U;
    sample.capture = 92U;
    sample.units = FOC_CAL_WORKFLOW_RAD_PAIR;
    sample.saturated = false;
    for (uint32_t index = 0U; index < 8U; ++index) {
        sample.data.angle = (struct foc_calibration_angle_sample){
            .mechanical_rad = 0.0f,
            .electrical_rad = 0.2f,
            .captured_us = index * 10U,
            .sequence = index,
            .valid = true,
        };
        CHECK(foc_cal_workflow_push(&workflow, &sample, index * 10U) == FOC_CAL_WORKFLOW_OK);
    }
    CHECK(foc_cal_workflow_finish_stage(&workflow) == FOC_CAL_WORKFLOW_ERR_QUALITY);
    CHECK(bundle->stages[FOC_CAL_WORKFLOW_ROTOR].quality == FOC_CAL_WORKFLOW_UNIDENTIFIABLE);

    CHECK(foc_cal_workflow_finish(&workflow) == FOC_CAL_WORKFLOW_PARTIAL);
    puts("WORKFLOW saturated R/L, ambiguous rotor and insufficient LUT coverage PASS");
    return 0;
}

/* Test Entry ------------------------------------------------------------------- */

int main(void)
{
    CHECK(lifecycle() == 0);
    CHECK(failed_capture() == 0);
    CHECK(model_dependency() == 0);
    CHECK(native_capture_rejections() == 0);
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
