/**
 * @file    test_foc_virtual_calibrated_driver.c
 * @brief   Exercise initialize-time binding and independent virtual ADC bias.
 * @version v0.1.0
 * @date    2026.11.19
 *
 * Test role
 * ---------
 * Verify that initialized Runtime correction does not change physical ADC
 * values and that the existing Host adapter acknowledges initial INHIBIT.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_virtual_current.h"

#include <math.h>
#include <stdio.h>

#define CHECK(condition)                                       \
    do {                                                       \
        if (!(condition)) {                                    \
            fprintf(stderr, "%d: %s\n", __LINE__, #condition); \
            return 1;                                          \
        }                                                      \
    } while (0)

/* Private Helpers -------------------------------------------------------------- */

static int initialize(
    void *argument,
    struct foc_runtime *runtime,
    struct foc_encoder_context *encoder,
    const struct foc_pipeline_config *pipeline,
    const struct foc_virtual_current_profile *profile
)
{
    struct foc_pipeline_config candidate = *pipeline;
    candidate.adc.phase_a_offset_count = 2048.0f;
    candidate.adc.phase_b_offset_count = 2048.0f;
    candidate.adc.phase_c_offset_count = 2048.0f;
    (void)argument;
    return foc_encoder_init(encoder, &profile->encoder) == FOC_ENCODER_STATUS_OK &&
            foc_runtime_init(runtime, &candidate, &profile->runtime, 0U) == FOC_RUNTIME_OK
        ? 0
        : -1;
}

/* Test Entry ------------------------------------------------------------------- */

int main(void)
{
    struct foc_virtual_current_profile profile = foc_virtual_current_default_profile();
    struct foc_virtual_current context = {0};
    profile.adc.phase_a_offset_count = 2072.0f;
    profile.adc.phase_b_offset_count = 2036.0f;
    profile.adc.phase_c_offset_count = 2036.0f;
    CHECK(foc_virtual_current_init_with(&context, &profile, initialize, NULL) == 0);
    struct foc_device_adc_raw raw;
    CHECK(foc_virtual_current_acquire_adc(&profile, &context.motor, 1U, 0U, &raw) == 0);
    CHECK(raw.rank2_raw == 2072U && raw.rank3_raw == 2036U && raw.rank4_raw == 2036U);
    struct foc_device_measurement measurement;
    CHECK(
        foc_device_decode_sample(&raw, &context.runtime.pipeline.adc_config, &measurement) ==
            FOC_DEVICE_STATUS_OK &&
        fabsf(measurement.phase_current_a.a - 0.048f) < 1.0e-6f
    );
    CHECK(foc_virtual_current_run_until(&context, 1000U) == 0);
    CHECK(context.output.mode == FOC_VIRTUAL_OUTPUT_HIGH_Z);
    const uint32_t clock = context.virtual_time_us;
    CHECK(foc_virtual_current_init_with(&context, &profile, initialize, NULL) != 0);
    CHECK(context.initialized && context.virtual_time_us == clock);
    CHECK(context.runtime.pipeline.adc_config.phase_a_offset_count == 2048.0f);
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
