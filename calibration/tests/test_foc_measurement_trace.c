/**
 * @file    test_foc_measurement_trace.c
 * @brief   Trace timing, evidence and delivery into the calibration Workflow.
 * @version v0.1.0
 * @date    2026.11.19
 *
 * Test role
 * ---------
 * Verify timing, evidence and transactional rejection through the public API.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_measurement_trace.h"

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

/* Private Helpers -------------------------------------------------------------- */

static const struct foc_cal_workflow_identity identity = {
    .acquisition = {4095U, 0.002f, 1024U, 1, 0U, 4U},
    .motor_setup_id = 17U,
    .encoder_id = 31U,
    .mechanical_frame_id = 41U,
};

static struct foc_measurement_trace_contract
contract(enum foc_measurement_trace_stage stage, uint32_t samples)
{
    return (struct foc_measurement_trace_contract){
        .version = FOC_MEASUREMENT_TRACE_VERSION,
        .source = FOC_MEASUREMENT_TRACE_HOST_SYNTHETIC,
        .provenance = FOC_MEASUREMENT_TRACE_PROVENANCE_DECLARED,
        .stage = stage,
        .session = 1U,
        .capture = 71U,
        .clock_id = 4U,
        .channel = 7U,
        .expected_samples = samples,
        .max_age_us = 10U,
        .max_interval_us = 30U,
        .max_skew_us = 8U,
        .allow_wrap = true,
        .identity = identity,
    };
}

static struct foc_measurement_trace_record adc(uint32_t sequence, uint32_t source_us)
{
    return (struct foc_measurement_trace_record){
        .stage = FOC_MEASUREMENT_TRACE_ADC,
        .evidence = FOC_MEASUREMENT_TRACE_ZERO_CONFIRMED,
        .clock_id = 4U,
        .units = FOC_CAL_WORKFLOW_RAW_COUNTS,
        .channel = 7U,
        .flags = FOC_MEASUREMENT_TRACE_VALID | FOC_MEASUREMENT_TRACE_WINDOW_CONFIRMED,
        .sequence = sequence,
        .source_us = source_us,
        .payload.sample = {
            .session = 1U,
            .capture = 71U,
            .identity = identity,
            .data.adc = {
                .phase_a = (uint16_t)(2048U + sequence % 2U),
                .phase_b = 2046U,
                .phase_c = 2049U,
                .valid_mask = 7U,
            },
        },
    };
}

static int wrap_flows_through_workflow(void)
{
    struct foc_cal_workflow staged = {0};
    const struct foc_cal_workflow_request request = {
        .session = 1U,
        .profile = FOC_CAL_WORKFLOW_SELECTED_STAGES,
        .stages = FOC_CAL_WORKFLOW_STAGE_BIT(FOC_CAL_WORKFLOW_ADC),
        .identity = identity,
    };
    const struct foc_cal_workflow_stage_request stage = {
        .stage = FOC_CAL_WORKFLOW_ADC,
        .source = FOC_CAL_WORKFLOW_HOST_SYNTHETIC,
        .capture = 71U,
        .max_age_us = 10U,
        .policy.adc = {
            .identity = identity.acquisition,
            .required_valid_mask = 7U,
            .minimum_samples = 4U,
            .maximum_samples = 6U,
            .maximum_sample_step_counts = 8.0f,
            .maximum_noise_rms_counts = 2.0f,
            .minimum_offset_counts = 2020.0f,
            .maximum_offset_counts = 2080.0f,
            .maximum_interval_us = 30U,
        },
    };
    struct foc_measurement_trace context;
    struct foc_measurement_trace_contract spec = contract(FOC_MEASUREMENT_TRACE_ADC, 6U);
    spec.first_sequence = UINT32_MAX - 1U;
    CHECK(foc_measurement_trace_init(&context, &spec) == FOC_MEASUREMENT_TRACE_OK);
    CHECK(foc_cal_workflow_init(&staged) == FOC_CAL_WORKFLOW_OK);
    CHECK(foc_cal_workflow_start(&staged, &request) == FOC_CAL_WORKFLOW_OK);
    CHECK(foc_cal_workflow_open(&staged, &stage) == FOC_CAL_WORKFLOW_OK);
    for (uint32_t i = 0U; i < 6U; ++i) {
        struct foc_measurement_trace_record record =
            adc(UINT32_MAX - 1U + i, UINT32_MAX - 24U + i * 10U);
        record.completion_observed = (i % 2U) == 0U;
        record.completion_us = record.source_us + 6U;
        record.trigger_observed = true;
        record.trigger_us = record.source_us - 2U;
        record.flags |= FOC_MEASUREMENT_TRACE_SOURCE_PROXY;
        union foc_measurement_trace_payload output;
        uint32_t now;
        CHECK(foc_measurement_trace_convert(&context, &record, &output, &now) == 0);
        CHECK(output.sample.data.adc.captured_us == record.source_us);
        CHECK(output.sample.data.adc.sequence == record.sequence);
        CHECK(now == (record.completion_observed ? record.completion_us : record.source_us));
        CHECK(foc_cal_workflow_push(&staged, &output.sample, now) == 0);
    }
    CHECK(foc_measurement_trace_finish(&context) == 0);
    CHECK(
        context.report.complete && context.report.accepted == 6U && context.report.rejected == 0U
    );
    CHECK(context.report.completion_unobserved == 3U && context.report.trigger_observed == 6U);
    CHECK(context.report.source_proxy == 6U);
    CHECK(foc_cal_workflow_finish_stage(&staged) == 0 && foc_cal_workflow_finish(&staged) == 0);
    const struct foc_cal_workflow_bundle *result = foc_cal_workflow_result(&staged);
    CHECK(result->adc.usable && result->adc.offset_a_counts == 2048.5f);
    CHECK(result->stages[FOC_CAL_WORKFLOW_ADC].primary.last_us == 25U);
    return 0;
}

static int rejects_bad_time_or_sequence(void)
{
    const struct {
        uint32_t next_sequence;
        uint32_t next_source;
        enum foc_measurement_trace_status expected;
    } cases[] = {
        {0U, 101U, FOC_MEASUREMENT_TRACE_ERR_SEQUENCE},
        {2U, 101U, FOC_MEASUREMENT_TRACE_ERR_GAP},
        {1U, 100U, FOC_MEASUREMENT_TRACE_ERR_TIME_ORDER},
        {1U, 99U, FOC_MEASUREMENT_TRACE_ERR_TIME_ORDER},
        {1U, UINT32_C(0x80000064), FOC_MEASUREMENT_TRACE_ERR_TIME_ORDER},
        {1U, UINT32_C(0x80000065), FOC_MEASUREMENT_TRACE_ERR_TIME_ORDER},
        {1U, 131U, FOC_MEASUREMENT_TRACE_ERR_GAP},
    };
    struct foc_measurement_trace context;
    const struct foc_measurement_trace_contract spec = contract(FOC_MEASUREMENT_TRACE_ADC, 2U);
    const struct foc_measurement_trace_record first = adc(0U, 100U);
    for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        union foc_measurement_trace_payload out;
        uint32_t now;
        CHECK(foc_measurement_trace_init(&context, &spec) == 0);
        CHECK(foc_measurement_trace_convert(&context, &first, &out, &now) == 0);
        const struct foc_measurement_trace_record next =
            adc(cases[i].next_sequence, cases[i].next_source);
        CHECK(foc_measurement_trace_convert(&context, &next, &out, &now) == cases[i].expected);
        CHECK(out.sample.data.adc.captured_us == 100U && now == 100U);
        CHECK(context.report.accepted == 1U && context.report.rejected == 1U);
        CHECK(context.report.first_rejected_sequence == cases[i].next_sequence);
        CHECK(foc_measurement_trace_finish(&context) == cases[i].expected);
    }

    struct foc_measurement_trace_contract linear = spec;
    linear.allow_wrap = false;
    const struct foc_measurement_trace_record before_wrap = adc(0U, UINT32_MAX);
    const struct foc_measurement_trace_record after_wrap = adc(1U, 2U);
    union foc_measurement_trace_payload out;
    uint32_t now;
    CHECK(foc_measurement_trace_init(&context, &linear) == 0);
    CHECK(foc_measurement_trace_convert(&context, &before_wrap, &out, &now) == 0);
    CHECK(
        foc_measurement_trace_convert(&context, &after_wrap, &out, &now) ==
        FOC_MEASUREMENT_TRACE_ERR_TIME_ORDER
    );

    struct foc_measurement_trace_record timing = adc(0U, 100U);
    timing.completion_observed = true;
    timing.completion_us = 111U;
    CHECK(foc_measurement_trace_init(&context, &spec) == 0);
    CHECK(
        foc_measurement_trace_convert(&context, &timing, &out, &now) ==
        FOC_MEASUREMENT_TRACE_ERR_AGE
    );
    timing.completion_us = 99U;
    CHECK(foc_measurement_trace_init(&context, &spec) == 0);
    CHECK(
        foc_measurement_trace_convert(&context, &timing, &out, &now) ==
        FOC_MEASUREMENT_TRACE_ERR_TIME_ORDER
    );
    timing.completion_us = 110U;
    timing.trigger_observed = true;
    timing.trigger_us = 101U;
    CHECK(foc_measurement_trace_init(&context, &spec) == 0);
    CHECK(
        foc_measurement_trace_convert(&context, &timing, &out, &now) ==
        FOC_MEASUREMENT_TRACE_ERR_TIME_ORDER
    );

    CHECK(foc_measurement_trace_init(&context, &spec) == 0);
    CHECK(foc_measurement_trace_convert(&context, &first, &out, &now) == 0);
    CHECK(foc_measurement_trace_finish(&context) == FOC_MEASUREMENT_TRACE_ERR_INCOMPLETE);
    CHECK(!context.report.complete);
    return 0;
}

static int observed_completion_stays_ordered(void)
{
    struct foc_measurement_trace context;
    const struct foc_measurement_trace_contract spec = contract(FOC_MEASUREMENT_TRACE_ADC, 3U);
    struct foc_measurement_trace_record record = adc(0U, 100U);
    union foc_measurement_trace_payload out;
    uint32_t now;
    record.completion_observed = true;
    record.completion_us = 110U;
    CHECK(foc_measurement_trace_init(&context, &spec) == 0);
    CHECK(foc_measurement_trace_convert(&context, &record, &out, &now) == 0);
    record.sequence = 1U;
    record.source_us = 101U;
    record.completion_observed = false;
    CHECK(foc_measurement_trace_convert(&context, &record, &out, &now) == 0);
    struct foc_measurement_trace backwards = context;
    record.sequence = 2U;
    record.source_us = 102U;
    record.completion_observed = true;
    record.completion_us = 109U;
    CHECK(
        foc_measurement_trace_convert(&backwards, &record, &out, &now) ==
        FOC_MEASUREMENT_TRACE_ERR_TIME_ORDER
    );
    record.completion_us = 110U;
    CHECK(foc_measurement_trace_convert(&context, &record, &out, &now) == 0);
    CHECK(now == 110U);
    CHECK(foc_measurement_trace_finish(&context) == 0 && context.report.complete);
    return 0;
}

static struct foc_measurement_trace_record angle(uint32_t sequence, uint32_t us, uint32_t ref_us)
{
    return (struct foc_measurement_trace_record){
        .stage = FOC_MEASUREMENT_TRACE_ROTOR,
        .evidence = FOC_MEASUREMENT_TRACE_INDEPENDENT_ANGLE,
        .clock_id = 4U,
        .reference_clock_id = 4U,
        .units = FOC_CAL_WORKFLOW_RAD_PAIR,
        .channel = 7U,
        .flags = FOC_MEASUREMENT_TRACE_VALID,
        .sequence = sequence,
        .source_us = us,
        .reference_us = ref_us,
        .payload.sample = {
            .session = 1U,
            .capture = 71U,
            .identity = identity,
            .encoder_raw_valid = true,
            .encoder_raw_code = 256U,
            .data.angle = {.mechanical_rad = 1.5f, .electrical_rad = 0.9f},
        },
    };
}

static int separate_channel_clocks_and_rates(void)
{
    const struct foc_measurement_trace_contract spec = contract(FOC_MEASUREMENT_TRACE_ROTOR, 3U);
    const struct foc_measurement_trace_contract adc_spec = contract(FOC_MEASUREMENT_TRACE_ADC, 2U);
    struct foc_measurement_trace rotor;
    struct foc_measurement_trace current;
    union foc_measurement_trace_payload out;
    uint32_t now;
    CHECK(foc_measurement_trace_init(&rotor, &spec) == 0);
    CHECK(foc_measurement_trace_init(&current, &adc_spec) == 0);
    const struct foc_measurement_trace_record first = angle(0U, 100U, 103U);
    const struct foc_measurement_trace_record second = angle(1U, 120U, 118U);
    struct foc_measurement_trace_record last = angle(2U, 141U, 143U);
    const struct foc_measurement_trace_record adc_first = adc(0U, 80U);
    const struct foc_measurement_trace_record adc_second = adc(1U, 86U);
    CHECK(foc_measurement_trace_convert(&rotor, &first, &out, &now) == 0);
    CHECK(foc_measurement_trace_convert(&current, &adc_first, &out, &now) == 0);
    CHECK(foc_measurement_trace_convert(&rotor, &second, &out, &now) == 0);
    CHECK(out.sample.data.angle.captured_us == 120U && now == 120U);
    CHECK(foc_measurement_trace_convert(&current, &adc_second, &out, &now) == 0);
    CHECK(foc_measurement_trace_convert(&rotor, &last, &out, &now) == 0);
    CHECK(foc_measurement_trace_finish(&current) == 0 && foc_measurement_trace_finish(&rotor) == 0);

    CHECK(foc_measurement_trace_init(&rotor, &spec) == 0);
    last = angle(0U, 90U, 99U);
    CHECK(
        foc_measurement_trace_convert(&rotor, &last, &out, &now) ==
        FOC_MEASUREMENT_TRACE_ERR_ALIGNMENT
    );
    last.reference_us = 90U;
    last.reference_clock_id = 0U;
    CHECK(
        foc_measurement_trace_convert(&rotor, &last, &out, &now) == FOC_MEASUREMENT_TRACE_ERR_CLOCK
    );
    last.reference_clock_id = 9U;
    CHECK(
        foc_measurement_trace_convert(&rotor, &last, &out, &now) == FOC_MEASUREMENT_TRACE_ERR_CLOCK
    );

    CHECK(foc_measurement_trace_init(&rotor, &spec) == 0);
    CHECK(foc_measurement_trace_convert(&rotor, &first, &out, &now) == 0);
    last = angle(1U, 107U, 102U);
    CHECK(
        foc_measurement_trace_convert(&rotor, &last, &out, &now) ==
        FOC_MEASUREMENT_TRACE_ERR_TIME_ORDER
    );
    CHECK(foc_measurement_trace_cancel(&rotor) == FOC_MEASUREMENT_TRACE_ERR_CANCELLED);
    CHECK(
        foc_measurement_trace_convert(&rotor, &second, &out, &now) ==
        FOC_MEASUREMENT_TRACE_ERR_STATE
    );
    return 0;
}

static int rejects_invalid_counts_and_mislabeled_evidence(void)
{
    struct foc_measurement_trace context;
    struct foc_measurement_trace_contract spec = contract(FOC_MEASUREMENT_TRACE_ADC, 1U);
    struct foc_measurement_trace_record record = adc(0U, 100U);
    union foc_measurement_trace_payload out;
    uint32_t now = 77U;
    memset(&out, 0x5a, sizeof(out));
    unsigned char before[sizeof(out)];
    memcpy(before, &out, sizeof(out));

    CHECK(foc_measurement_trace_init(&context, &spec) == 0);
    record.channel = 3U;
    CHECK(
        foc_measurement_trace_convert(&context, &record, &out, &now) ==
        FOC_MEASUREMENT_TRACE_ERR_IDENTITY
    );
    record.channel = 7U;
    record.payload.sample.identity.acquisition.encoder_counts_per_revolution *= 2U;
    CHECK(
        foc_measurement_trace_convert(&context, &record, &out, &now) ==
        FOC_MEASUREMENT_TRACE_ERR_IDENTITY
    );
    record.payload.sample.identity = identity;
    record.payload.sample.identity.acquisition.encoder_direction = -1;
    CHECK(
        foc_measurement_trace_convert(&context, &record, &out, &now) ==
        FOC_MEASUREMENT_TRACE_ERR_IDENTITY
    );
    record.payload.sample.identity = identity;
    record.units = FOC_CAL_WORKFLOW_RAD_PAIR;
    CHECK(
        foc_measurement_trace_convert(&context, &record, &out, &now) ==
        FOC_MEASUREMENT_TRACE_ERR_UNITS
    );
    record.units = FOC_CAL_WORKFLOW_RAW_COUNTS;
    record.flags |= FOC_MEASUREMENT_TRACE_SATURATED;
    CHECK(
        foc_measurement_trace_convert(&context, &record, &out, &now) ==
        FOC_MEASUREMENT_TRACE_ERR_SATURATION
    );
    record.flags &= ~(uint32_t)FOC_MEASUREMENT_TRACE_SATURATED;
    record.evidence = FOC_MEASUREMENT_TRACE_COMMAND;
    CHECK(
        foc_measurement_trace_convert(&context, &record, &out, &now) ==
        FOC_MEASUREMENT_TRACE_ERR_PRECONDITION_REQUIRED
    );
    record.evidence = FOC_MEASUREMENT_TRACE_ZERO_CONFIRMED;
    record.payload.sample.data.adc.phase_c = 4096U;
    CHECK(
        foc_measurement_trace_convert(&context, &record, &out, &now) ==
        FOC_MEASUREMENT_TRACE_ERR_RANGE
    );
    CHECK(now == 77U && memcmp(before, &out, sizeof(out)) == 0);
    CHECK(context.report.first_failure == FOC_MEASUREMENT_TRACE_ERR_IDENTITY);
    CHECK(strcmp(foc_measurement_trace_status_name(FOC_MEASUREMENT_TRACE_ERR_AGE), "AGE") == 0);

    spec.version++;
    CHECK(foc_measurement_trace_init(&context, &spec) == FOC_MEASUREMENT_TRACE_ERR_VERSION);
    CHECK(context.report.accepted == 0U && context.report.rejected == 7U);
    CHECK(foc_measurement_trace_finish(&context) == FOC_MEASUREMENT_TRACE_ERR_IDENTITY);
    return 0;
}

static int recorded_electrical_preserves_voltage_evidence(void)
{
    struct foc_measurement_trace_contract spec = contract(FOC_MEASUREMENT_TRACE_ELECTRICAL, 2U);
    spec.source = FOC_MEASUREMENT_TRACE_BOARD_RECORDED;
    spec.provenance = FOC_MEASUREMENT_TRACE_PROVENANCE_UNVERIFIED;
    struct foc_measurement_trace_record record = adc(0U, 140U);
    record.stage = FOC_MEASUREMENT_TRACE_ELECTRICAL;
    record.evidence = FOC_MEASUREMENT_TRACE_EVIDENCE_NONE;
    record.units = FOC_CAL_WORKFLOW_ELECTRICAL_SI;
    record.flags |= FOC_MEASUREMENT_TRACE_FRAME_CONFIRMED;
    record.payload.sample.data.electrical = (struct foc_motor_id_sample){
        .setup_id = 17U,
        .capture_id = 71U,
        .current_a = 1.2f,
        .voltage_begin_us = 140U,
        .voltage_end_us = 140U,
    };
    union foc_measurement_trace_payload out;
    struct foc_measurement_trace context;
    uint32_t now;
    CHECK(foc_measurement_trace_init(&context, &spec) == 0);
    record.reference_clock_id = 4U;
    record.reference_us = 140U;
    record.payload.sample.data.electrical.voltage_end_us = 139U;
    CHECK(
        foc_measurement_trace_convert(&context, &record, &out, &now) ==
        FOC_MEASUREMENT_TRACE_ERR_ALIGNMENT
    );
    CHECK(foc_measurement_trace_init(&context, &spec) == 0);
    record.payload.sample.data.electrical.voltage_end_us = 140U;
    CHECK(foc_measurement_trace_convert(&context, &record, &out, &now) == 0);
    CHECK(out.sample.source == FOC_CAL_WORKFLOW_EXTERNAL_RECORDED_DATA);
    CHECK(out.sample.data.electrical.source == FOC_MOTOR_ID_MEASURED);
    CHECK(out.sample.data.electrical.voltage_source == FOC_MOTOR_ID_VOLTAGE_UNVERIFIED);
    CHECK(!out.sample.data.electrical.voltage_valid);
    CHECK(context.contract.provenance == FOC_MEASUREMENT_TRACE_PROVENANCE_UNVERIFIED);

    record.sequence = 1U;
    record.source_us = 150U;
    record.reference_us = 150U;
    record.evidence = FOC_MEASUREMENT_TRACE_TERMINAL_MEASURED;
    record.flags |= FOC_MEASUREMENT_TRACE_OUTPUT_CONFIRMED;
    record.payload.sample.data.electrical.voltage_end_us = 150U;
    record.payload.sample.data.electrical.mean_voltage_v = 7.3f;
    struct foc_measurement_trace misaligned = context;
    record.frame_encoder_clock_id = 4U;
    record.frame_encoder_us = 159U;
    CHECK(
        foc_measurement_trace_convert(&misaligned, &record, &out, &now) ==
        FOC_MEASUREMENT_TRACE_ERR_ALIGNMENT
    );
    record.frame_encoder_us = 155U;
    record.completion_observed = true;
    record.completion_us = 154U;
    CHECK(
        foc_measurement_trace_convert(&misaligned, &record, &out, &now) ==
        FOC_MEASUREMENT_TRACE_ERR_TIME_ORDER
    );
    record.completion_us = 156U;
    struct foc_measurement_trace unsuitable = context;
    record.evidence = FOC_MEASUREMENT_TRACE_COMMAND;
    CHECK(
        foc_measurement_trace_convert(&unsuitable, &record, &out, &now) ==
        FOC_MEASUREMENT_TRACE_ERR_DATA_REQUIRED
    );
    CHECK(out.sample.data.electrical.captured_us == 140U && now == 140U);
    record.evidence = FOC_MEASUREMENT_TRACE_TERMINAL_VIRTUAL;
    CHECK(
        foc_measurement_trace_convert(&unsuitable, &record, &out, &now) ==
        FOC_MEASUREMENT_TRACE_ERR_DATA_REQUIRED
    );
    record.evidence = FOC_MEASUREMENT_TRACE_TERMINAL_MEASURED;
    CHECK(foc_measurement_trace_convert(&context, &record, &out, &now) == 0);
    CHECK(
        out.sample.data.electrical.voltage_valid &&
        out.sample.data.electrical.voltage_source == FOC_MOTOR_ID_VOLTAGE_MEASURED_TERMINAL &&
        out.sample.data.electrical.mean_voltage_v == 7.3f
    );
    CHECK(foc_measurement_trace_finish(&context) == 0);
    CHECK(foc_measurement_trace_finish(&unsuitable) == FOC_MEASUREMENT_TRACE_ERR_DATA_REQUIRED);
    return 0;
}

static int independent_reference_torque_and_kt(void)
{
    struct foc_measurement_trace context;
    union foc_measurement_trace_payload out;
    uint32_t now;
    struct foc_measurement_trace_record record = angle(0U, 150U, 150U);
    struct foc_measurement_trace_contract lut = contract(FOC_MEASUREMENT_TRACE_LUT, 1U);
    lut.lut_reference_id = 90U;
    lut.lut_direction = 1;
    lut.identity.acquisition.encoder_direction = -1;
    record.payload.sample.identity = lut.identity;
    record.stage = FOC_MEASUREMENT_TRACE_LUT;
    record.units = FOC_CAL_WORKFLOW_ENCODER_COUNTS_RAD;
    record.payload.sample.data.lut = (struct foc_encoder_cal_sample){
        .setup = {31U, 31U, 1024U, 1, FOC_ENCODER_CAL_SOURCE_VIRTUAL},
        .capture_id = 71U,
        .raw_code = 9U,
        .reference_rad = 0.12,
    };
    CHECK(foc_measurement_trace_init(&context, &lut) == 0);
    CHECK(
        foc_measurement_trace_convert(&context, &record, &out, &now) ==
        FOC_MEASUREMENT_TRACE_ERR_REFERENCE_REQUIRED
    );
    record.payload.sample.data.lut.setup.reference_identity = 90U;
    record.reference_us = 151U;
    CHECK(
        foc_measurement_trace_convert(&context, &record, &out, &now) ==
        FOC_MEASUREMENT_TRACE_ERR_ALIGNMENT
    );
    CHECK(foc_measurement_trace_init(&context, &lut) == 0);
    record.reference_us = 150U;
    record.payload.sample.data.lut.setup.reference_identity = 91U;
    CHECK(
        foc_measurement_trace_convert(&context, &record, &out, &now) ==
        FOC_MEASUREMENT_TRACE_ERR_IDENTITY
    );
    CHECK(foc_measurement_trace_init(&context, &lut) == 0);
    record.payload.sample.data.lut.setup.reference_identity = 90U;
    CHECK(foc_measurement_trace_convert(&context, &record, &out, &now) == 0);
    CHECK(out.sample.data.lut.encoder_us == 150U && out.sample.data.lut.reference_us == 150U);

    const struct foc_measurement_trace_contract mechanical =
        contract(FOC_MEASUREMENT_TRACE_MECHANICAL, 1U);
    record.stage = FOC_MEASUREMENT_TRACE_MECHANICAL;
    record.payload.sample.identity = identity;
    record.units = FOC_CAL_WORKFLOW_MECHANICAL_SI;
    record.evidence = FOC_MEASUREMENT_TRACE_COMMAND;
    record.flags |= FOC_MEASUREMENT_TRACE_OUTPUT_CONFIRMED | FOC_MEASUREMENT_TRACE_FRAME_CONFIRMED |
        FOC_MEASUREMENT_TRACE_WINDOW_CONFIRMED;
    record.payload.sample.data.mechanical = (struct foc_mechanical_id_sample){
        .setup_id = 17U,
        .torque_model_id = 36U,
        .capture_id = 71U,
        .speed_rad_s = -0.5f,
        .iq_a = 0.2f,
        .load_nm = 0.14f,
        .interval_begin_us = 150U,
    };
    CHECK(foc_measurement_trace_init(&context, &mechanical) == 0);
    CHECK(
        foc_measurement_trace_convert(&context, &record, &out, &now) ==
        FOC_MEASUREMENT_TRACE_ERR_TORQUE_SOURCE_REQUIRED
    );
    record.evidence = FOC_MEASUREMENT_TRACE_OBSERVED_DQ;
    CHECK(
        foc_measurement_trace_convert(&context, &record, &out, &now) ==
        FOC_MEASUREMENT_TRACE_ERR_LOAD_REQUIRED
    );
    record.flags |= FOC_MEASUREMENT_TRACE_LOAD_OBSERVED;
    CHECK(foc_measurement_trace_init(&context, &mechanical) == 0);
    CHECK(foc_measurement_trace_convert(&context, &record, &out, &now) == 0);
    CHECK(out.sample.data.mechanical.torque_source == FOC_MECHANICAL_ID_TORQUE_OBSERVED_DQ);
    CHECK(out.sample.data.mechanical.load_source == FOC_MECHANICAL_ID_LOAD_OBSERVED);
    CHECK(out.sample.data.mechanical.load_nm == 0.14f);
    CHECK(
        out.sample.data.mechanical.speed_us == 150U && out.sample.data.mechanical.torque_us == 150U
    );

    const struct foc_measurement_trace_contract kt = contract(FOC_MEASUREMENT_TRACE_KT, 1U);
    record.stage = FOC_MEASUREMENT_TRACE_KT;
    record.units = FOC_MEASUREMENT_TRACE_KT_UNITS;
    record.reference_clock_id = 0U;
    record.evidence = FOC_MEASUREMENT_TRACE_DERIVED;
    record.flags = FOC_MEASUREMENT_TRACE_VALID;
    record.payload.kt = (struct foc_cal_workflow_kt){
        .identity = identity,
        .torque_model_id = 36U,
        .nm_per_a = 0.15f,
        .capture = 71U,
    };
    CHECK(foc_measurement_trace_init(&context, &kt) == 0);
    CHECK(
        foc_measurement_trace_convert(&context, &record, &out, &now) ==
        FOC_MEASUREMENT_TRACE_ERR_DATA_REQUIRED
    );
    record.evidence = FOC_MEASUREMENT_TRACE_INDEPENDENT_KT;
    CHECK(foc_measurement_trace_init(&context, &kt) == 0);
    CHECK(foc_measurement_trace_convert(&context, &record, &out, &now) == 0);
    CHECK(out.kt.qualified && out.kt.captured_us == 150U && out.kt.nm_per_a == 0.15f);
    return 0;
}

/* Public Functions ------------------------------------------------------------- */

/* Test Entry ------------------------------------------------------------------- */

int main(void)
{
    CHECK(wrap_flows_through_workflow() == 0);
    CHECK(rejects_bad_time_or_sequence() == 0);
    CHECK(observed_completion_stays_ordered() == 0);
    CHECK(separate_channel_clocks_and_rates() == 0);
    CHECK(rejects_invalid_counts_and_mislabeled_evidence() == 0);
    CHECK(recorded_electrical_preserves_voltage_evidence() == 0);
    CHECK(independent_reference_torque_and_kt() == 0);
    puts("Measurement Trace time, channels, source evidence and Workflow ADC replay PASS");
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
