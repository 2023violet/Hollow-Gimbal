/**
 * @file    test_foc_board_config.c
 * @brief   Host validation of calibration admission and explicit Board1 mapping.
 * @version v0.1.0
 * @date    2026.11.13
 *
 * Test role
 * ---------
 * Prove that a complete CAL-01 snapshot reaches ADC decoding, Sensor
 * direction, Rotor mapping, and the H1 publication without changing the
 * locked default Board1 initialization path.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_board.h"

#include <math.h>
#include <stdio.h>

/* Private Macros --------------------------------------------------------------- */

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return 1;                                                       \
        }                                                                   \
    } while (0)

/* Private Helpers -------------------------------------------------------------- */

static float wrap_2pi(float angle)
{
    float wrapped = fmodf(angle, 6.28318530717958647692f);

    if (wrapped < 0.0f) {
        wrapped += 6.28318530717958647692f;
    }
    return wrapped >= 6.28318530717958647692f ? 0.0f : wrapped;
}

static uint16_t synthetic_adc_value(float target, uint32_t index)
{
    const float base_float = floorf(target);
    const unsigned int base = (unsigned int)base_float;
    const unsigned int high_count = (unsigned int)lroundf((target - base_float) * 64.0f);

    return (uint16_t)(base + (index < high_count ? 1U : 0U));
}

static int make_snapshot(
    float offset_a,
    float offset_b,
    float offset_c,
    int32_t calibration_direction,
    float electrical_offset,
    struct foc_calibration_snapshot *snapshot
)
{
    const struct foc_calibration_identity expected = {
        .adc_full_scale_counts = 4095U,
        .current_a_per_count = 3.3f / (4095.0f * 0.5f),
        .encoder_counts_per_revolution = 65536U,
        .encoder_direction = FOC_ENCODER_DIRECTION_REVERSED,
        .phase_order = FOC_DEVICE_PHASE_ORDER_ABC,
        .pole_pairs = 7U,
    };
    const struct foc_calibration_adc_config adc_config = {
        .identity = expected,
        .required_valid_mask = 0x07U,
        .minimum_samples = 32U,
        .maximum_samples = 64U,
        .maximum_sample_step_counts = 20.0f,
        .maximum_noise_rms_counts = 2.0f,
        .minimum_offset_counts = 32.0f,
        .maximum_offset_counts = 4063.0f,
        .maximum_interval_us = 1000U,
    };
    struct foc_calibration_adc_context adc_context = {0};
    if (foc_calibration_adc_init(&adc_context, &adc_config) != 0) {
        return -1;
    }
    for (uint32_t index = 0U; index < 64U; ++index) {
        const struct foc_calibration_adc_sample sample = {
            .phase_a = synthetic_adc_value(offset_a, index),
            .phase_b = synthetic_adc_value(offset_b, index),
            .phase_c = synthetic_adc_value(offset_c, index),
            .valid_mask = 0x07U,
            .sequence = index,
            .captured_us = (index + 1U) * 100U,
            .zero_current_confirmed = true,
        };
        if (foc_calibration_adc_collect(&adc_context, &sample) != 0) {
            return -1;
        }
    }
    struct foc_calibration_adc_result adc;
    if (foc_calibration_adc_finalize(&adc_context, &adc) != 0) {
        return -1;
    }
    struct foc_calibration_identity rotor_identity = expected;
    rotor_identity.encoder_direction = calibration_direction;
    const struct foc_calibration_rotor_config rotor_config = {
        .identity = expected,
        .minimum_pole_pairs = 1U,
        .maximum_pole_pairs = 20U,
        .minimum_mechanical_span_rad = 8.0f,
        .maximum_residual_rad = 0.02f,
        .minimum_candidate_separation_rad = 0.05f,
        .maximum_interval_us = 100000U,
        .maximum_samples = 64U,
    };
    struct foc_calibration_rotor_context rotor_context = {0};
    if (foc_calibration_rotor_init(&rotor_context, &rotor_config) != 0) {
        return -1;
    }
    for (uint32_t index = 0U; index < 40U; ++index) {
        const float mechanical = -4.2f + (float)index * 0.73f;
        const struct foc_calibration_angle_sample sample = {
            .mechanical_rad = mechanical,
            .electrical_rad =
                wrap_2pi((float)calibration_direction * 7.0f * mechanical + electrical_offset),
            .captured_us = (index + 1U) * 1000U,
            .sequence = index,
            .valid = true,
        };
        if (foc_calibration_rotor_collect(&rotor_context, &sample) != 0) {
            return -1;
        }
    }
    struct foc_calibration_rotor_result rotor;
    if (foc_calibration_rotor_finalize(&rotor_context, &rotor) != 0) {
        return -1;
    }
    const struct foc_calibration_offset_config offset_config = {
        .identity = rotor_identity,
        .maximum_residual_rad = 0.02f,
        .maximum_interval_us = 100000U,
        .maximum_samples = 64U,
    };
    struct foc_calibration_offset_context offset_context = {0};
    if (foc_calibration_offset_init(&offset_context, &offset_config) != 0) {
        return -1;
    }
    for (uint32_t index = 0U; index < 20U; ++index) {
        const float mechanical = -2.0f + (float)index * 0.37f;
        const struct foc_calibration_angle_sample sample = {
            .mechanical_rad = mechanical,
            .electrical_rad =
                wrap_2pi((float)calibration_direction * 7.0f * mechanical + electrical_offset),
            .captured_us = (index + 1U) * 1000U,
            .sequence = index,
            .valid = true,
        };
        if (foc_calibration_offset_collect(&offset_context, &sample) != 0) {
            return -1;
        }
    }
    struct foc_calibration_offset_result offset;
    if (foc_calibration_offset_finalize(&offset_context, &offset) != 0) {
        return -1;
    }
    const struct foc_calibration_admission_input input = {
        .adc = &adc,
        .rotor = &rotor,
        .offset = &offset,
        .expected_identity = &expected,
        .limits = {
            .minimum_adc_samples = 32U,
            .minimum_offset_counts = 32.0f,
            .maximum_offset_counts = 4063.0f,
            .maximum_noise_rms_counts = 2.0f,
            .maximum_rotor_residual_rad = 0.02f,
            .maximum_offset_residual_rad = 0.02f,
        },
    };
    enum foc_calibration_admission_failure failure;
    return foc_calibration_admit(&input, snapshot, &failure);
}

static int test_default_path_is_unchanged(void)
{
    struct foc_runtime runtime;
    struct foc_board_output output;

    CHECK(foc_board_init(&runtime, &output, 0U) == FOC_RUNTIME_OK);
    CHECK(runtime.pipeline.adc_config.phase_a_offset_count == 2048.0f);
    CHECK(runtime.pipeline.adc_config.phase_b_offset_count == 2048.0f);
    CHECK(runtime.pipeline.adc_config.phase_c_offset_count == 2048.0f);
    CHECK(runtime.config.motion.rotor.pole_pairs == 7U);
    CHECK(runtime.config.motion.rotor.electrical_offset_rad == 0.0f);
    CHECK(output.result == FOC_BOARD_INHIBIT_SOFTWARE);
    return 0;
}

static int test_snapshot_reaches_control_inputs(void)
{
    struct foc_calibration_snapshot nominal;
    struct foc_calibration_snapshot calibrated;
    CHECK(make_snapshot(2048.0f, 2048.0f, 2048.0f, 1, 0.0f, &nominal) == 0);
    CHECK(make_snapshot(2042.98f, 2052.02f, 2048.50f, -1, 1.2f, &calibrated) == 0);
    CHECK(nominal.runtime_encoder_direction == -1);
    CHECK(calibrated.runtime_encoder_direction == 1);

    struct foc_runtime nominal_runtime;
    struct foc_board_output nominal_output;
    struct foc_board_encoder nominal_encoder = {0};
    CHECK(
        foc_board_init_with_calibration(
            &nominal_runtime,
            &nominal_output,
            &nominal_encoder,
            &nominal,
            0U
        ) == FOC_RUNTIME_OK
    );
    CHECK(nominal_encoder.config.direction == FOC_ENCODER_DIRECTION_REVERSED);

    struct foc_runtime runtime;
    struct foc_board_output output;
    struct foc_board_encoder encoder = {0};
    CHECK(
        foc_board_init_with_calibration(&runtime, &output, &encoder, &calibrated, 0U) ==
        FOC_RUNTIME_OK
    );
    CHECK(runtime.pipeline.adc_config.phase_a_offset_count == calibrated.phase_a_offset_count);
    CHECK(runtime.pipeline.adc_config.phase_b_offset_count == calibrated.phase_b_offset_count);
    CHECK(runtime.pipeline.adc_config.phase_c_offset_count == calibrated.phase_c_offset_count);
    CHECK(runtime.config.motion.rotor.pole_pairs == calibrated.pole_pairs);
    CHECK(fabsf(runtime.config.motion.rotor.electrical_offset_rad - 1.2f) < 1.0e-6f);
    CHECK(encoder.config.direction == FOC_ENCODER_DIRECTION_NORMAL);
    CHECK(output.result == FOC_BOARD_INHIBIT_SOFTWARE);
    CHECK(foc_encoder_init(&encoder.sensor, &encoder.config) == FOC_ENCODER_STATUS_OK);

    const struct foc_device_adc_raw raw = {
        .vbus_raw = 1241U,
        .rank2_raw = 2050U,
        .rank3_raw = 2048U,
        .rank4_raw = 2047U,
        .valid_mask = FOC_DEVICE_SAMPLE_ALL_VALID,
        .sample_seq = 0U,
        .sample_tick = 0U,
    };
    struct foc_device_measurement measurement;
    CHECK(foc_device_decode_sample(&raw, &runtime.pipeline.adc_config, &measurement) == 0);
    CHECK(
        fabsf(
            measurement.phase_current_a.a -
            (2050.0f - calibrated.phase_a_offset_count) *
                runtime.pipeline.adc_config.current_a_per_count
        ) < 1.0e-7f
    );
    CHECK(
        fabsf(
            measurement.phase_current_a.b -
            (2048.0f - calibrated.phase_b_offset_count) *
                runtime.pipeline.adc_config.current_a_per_count
        ) < 1.0e-7f
    );

    struct foc_encoder_feedback feedback;
    CHECK(
        foc_encoder_update(&encoder.sensor, 0U, 1000U, FOC_ENCODER_TRANSPORT_OK, &feedback) ==
        FOC_ENCODER_STATUS_OK
    );
    CHECK(
        foc_encoder_update(&encoder.sensor, 1000U, 1010U, FOC_ENCODER_TRANSPORT_OK, &feedback) ==
            FOC_ENCODER_STATUS_OK &&
        feedback.valid && feedback.mechanical_rad > 0.0f
    );
    struct foc_electrical_state electrical;
    CHECK(
        foc_rotor_electrical(
            &runtime.config.motion.rotor,
            feedback.mechanical_rad,
            feedback.filtered_velocity_rad_s,
            &electrical
        ) == FOC_STATUS_OK
    );
    CHECK(
        fabsf(
            electrical.angle_rad -
            wrap_2pi(7.0f * feedback.mechanical_rad + calibrated.electrical_offset_rad)
        ) < 1.0e-5f
    );

    const struct foc_runtime_sensor sample = {
        .theta_elec_rad = electrical.angle_rad,
        .captured_us = 0U,
        .seq = 0U,
        .valid = true,
        .mechanical_rad = feedback.mechanical_rad,
        .mechanical_rad_s = feedback.filtered_velocity_rad_s,
    };
    CHECK(foc_runtime_h1_step(&runtime, &sample, 0U, 0U) == FOC_RUNTIME_OK);
    const unsigned int slot = atomic_load(&runtime.sensor_slot);
    CHECK(slot < 2U && runtime.sensor_slots[slot].sample.valid);
    CHECK(fabsf(runtime.sensor_slots[slot].electrical.angle_rad - electrical.angle_rad) < 1.0e-5f);

    (void)nominal_runtime;
    (void)nominal_output;
    (void)nominal_encoder;
    return 0;
}

static int test_reconfiguration_is_rejected(void)
{
    struct foc_calibration_snapshot snapshot;
    CHECK(make_snapshot(2042.0f, 2052.0f, 2048.0f, -1, 1.2f, &snapshot) == 0);
    struct foc_runtime runtime;
    struct foc_board_output output;
    struct foc_board_encoder encoder = {0};
    CHECK(foc_board_init_with_calibration(&runtime, &output, &encoder, &snapshot, 0U) == 0);
    CHECK(
        foc_board_init_with_calibration(&runtime, &output, &encoder, &snapshot, 100U) ==
        FOC_RUNTIME_ERR_CONFIG
    );

    struct foc_runtime untouched_runtime = {0};
    struct foc_board_output untouched_output = {.result = FOC_BOARD_UPDATE_NOT_APPLIED};
    struct foc_board_encoder untouched_encoder = {0};
    snapshot.accepted = false;
    CHECK(
        foc_board_init_with_calibration(
            &untouched_runtime,
            &untouched_output,
            &untouched_encoder,
            &snapshot,
            0U
        ) == FOC_RUNTIME_ERR_CONFIG
    );
    CHECK(!untouched_encoder.config_valid);
    CHECK(untouched_output.result == FOC_BOARD_UPDATE_NOT_APPLIED);
    return 0;
}

/* Test Entry ------------------------------------------------------------------- */

int main(void)
{
    return test_default_path_is_unchanged() || test_snapshot_reaches_control_inputs() ||
        test_reconfiguration_is_rejected();
}

/* ---------------------------------------- end of file ----------------------------------------- */
