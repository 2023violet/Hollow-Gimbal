/**
 * @file    test_foc_virtual_sensor.c
 * @brief   Contract tests for virtual ADC and encoder quantization.
 * @version v0.1.0
 * @date    2026.11.15
 *
 * Test role
 * ---------
 * Check ADC conversion, phase-order handling, encoder quantization, and
 * source-timestamped feedback.
 */

#include "foc_virtual_motor.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* Includes --------------------------------------------------------------------- */

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return 1;                                                       \
        }                                                                   \
    } while (0)

/* Private Helpers -------------------------------------------------------------- */

/* Test Entry ------------------------------------------------------------------- */

int main(void)
{
    const struct foc_virtual_motor_params params = {
        .rs_ohm = 0.6,
        .ld_h = 0.0012,
        .lq_h = 0.0015,
        .psi_f_wb = 0.025,
        .pole_pairs = 4U,
        .electrical_offset_rad = 0.37,
        .inertia_kg_m2 = 0.002,
        .damping_nm_s = 0.006,
    };
    const struct foc_device_adc_config adc = {
        .vbus_v_per_count = 0.01f,
        .current_a_per_count = 0.002f,
        .vbus_min_v = 1.0f,
        .phase_a_offset_count = 2048.0f,
        .phase_b_offset_count = 2048.0f,
        .phase_c_offset_count = 2048.0f,
        .phase_order = FOC_DEVICE_PHASE_ORDER_ABC,
    };
    const struct foc_encoder_config encoder = {
        .counts_per_revolution = 65536U,
        .direction = FOC_ENCODER_DIRECTION_NORMAL,
        .max_speed_rad_s = 1000.0f,
        .min_dt_us = 1U,
        .max_dt_us = 2000U,
        .speed_lpf_cutoff_hz = 80.0f,
        .tracker_alpha = 0.85f,
        .tracker_beta = 0.2f,
    };
    struct foc_virtual_motor motor;
    struct foc_device_adc_raw raw = {0};
    struct foc_device_measurement measurement = {0};
    struct foc_device_adc_config acb = adc;
    struct foc_encoder_context sensor;
    struct foc_encoder_feedback feedback;
    uint16_t code = 0U;
    CHECK(foc_virtual_motor_init(&motor, &params, NULL) == 0);
    CHECK(foc_virtual_motor_make_adc(&motor, &adc, 12.0f, 1U, 200U, &raw) == 0);
    CHECK(
        raw.valid_mask == FOC_DEVICE_SAMPLE_ALL_VALID && raw.sample_seq == 1U &&
        raw.sample_tick == 200U
    );
    CHECK(foc_device_decode_sample(&raw, &adc, &measurement) == FOC_DEVICE_STATUS_OK);
    CHECK(fabsf(measurement.core.vbus_v - 12.0f) < 0.01f);
    CHECK(
        fabsf(measurement.phase_current_a.a) < 0.01f &&
        fabsf(measurement.phase_current_a.b) < 0.01f && fabsf(measurement.phase_current_a.c) < 0.01f
    );
    acb.phase_order = FOC_DEVICE_PHASE_ORDER_ACB;
    CHECK(foc_virtual_motor_make_adc(&motor, &acb, 12.0f, 2U, 400U, &raw) == 0);
    CHECK(foc_device_decode_sample(&raw, &acb, &measurement) == FOC_DEVICE_STATUS_OK);
    CHECK(foc_virtual_motor_make_encoder_code(&motor, &encoder, &code) == 0);
    CHECK(code == 0U);
    CHECK(foc_encoder_init(&sensor, &encoder) == FOC_ENCODER_STATUS_OK);
    CHECK(
        foc_encoder_update(&sensor, 0U, code, FOC_ENCODER_TRANSPORT_OK, &feedback) ==
        FOC_ENCODER_STATUS_OK
    );
    motor.state.mechanical_position_rad = 6.28318530717958647692 - 0.0001;
    CHECK(foc_virtual_motor_make_encoder_code(&motor, &encoder, &code) == 0);
    CHECK(code > 0U && code < encoder.counts_per_revolution);
    CHECK(
        foc_encoder_update(&sensor, 500U, code, FOC_ENCODER_TRANSPORT_OK, &feedback) ==
        FOC_ENCODER_STATUS_OK
    );
    CHECK(feedback.valid && fabsf(feedback.mechanical_rad) < 0.001f);

    motor.state.id_a = 0.25;
    motor.state.iq_a = 0.75;
    motor.state.mechanical_position_rad = 0.0;
    CHECK(foc_virtual_motor_make_adc(&motor, &adc, 12.0f, 3U, 600U, &raw) == 0);
    CHECK(foc_device_decode_sample(&raw, &adc, &measurement) == FOC_DEVICE_STATUS_OK);
    const float theta = (float)params.electrical_offset_rad;
    const float alpha = cosf(theta) * 0.25f - sinf(theta) * 0.75f;
    const float beta = sinf(theta) * 0.25f + cosf(theta) * 0.75f;
    const float phase_b = -0.5f * alpha + 0.5f * 1.7320508f * beta;
    const float phase_c = -0.5f * alpha - 0.5f * 1.7320508f * beta;
    CHECK(fabsf(measurement.phase_current_a.a - alpha) < 0.01f);
    CHECK(fabsf(measurement.phase_current_a.b - phase_b) < 0.01f);
    CHECK(fabsf(measurement.phase_current_a.c - phase_c) < 0.01f);
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
