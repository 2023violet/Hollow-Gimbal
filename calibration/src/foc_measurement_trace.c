/**
 * @file    foc_measurement_trace.c
 * @brief   Bounded capture integrity and normalized observation conversion.
 * @version v0.1.0
 * @date    2026.11.19
 *
 * Implementation role
 * -------------------
 * Validate recorded time and acquisition evidence at the foreground boundary.
 * Only a validated observation is copied to the caller's output.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_measurement_trace.h"

#include <math.h>
#include <stddef.h>

/* Private Macros --------------------------------------------------------------- */

#define HALF_RANGE UINT32_C(0x80000000)
#define TWO_PI     6.283185307179586476925286766559

/* Private Helpers -------------------------------------------------------------- */

static bool has_flags(const struct foc_measurement_trace_record *record, uint32_t mask)
{
    return (record->flags & mask) == mask;
}

static bool clock_ordered(uint32_t later, uint32_t earlier, bool allow_wrap)
{
    return (allow_wrap || later >= earlier) && later - earlier < HALF_RANGE;
}

static uint32_t clock_distance(uint32_t left, uint32_t right, bool allow_wrap)
{
    const uint32_t difference = left >= right ? left - right : right - left;
    return allow_wrap && difference > HALF_RANGE ? 0U - difference : difference;
}

static uint32_t expected_units(enum foc_measurement_trace_stage stage)
{
    switch (stage) {
    case FOC_MEASUREMENT_TRACE_ADC:
        return FOC_CAL_WORKFLOW_RAW_COUNTS;
    case FOC_MEASUREMENT_TRACE_ELECTRICAL:
        return FOC_CAL_WORKFLOW_ELECTRICAL_SI;
    case FOC_MEASUREMENT_TRACE_ROTOR:
    case FOC_MEASUREMENT_TRACE_OFFSET:
        return FOC_CAL_WORKFLOW_RAD_PAIR;
    case FOC_MEASUREMENT_TRACE_LUT:
    case FOC_MEASUREMENT_TRACE_LUT_HOLDOUT:
        return FOC_CAL_WORKFLOW_ENCODER_COUNTS_RAD;
    case FOC_MEASUREMENT_TRACE_MECHANICAL:
        return FOC_CAL_WORKFLOW_MECHANICAL_SI;
    case FOC_MEASUREMENT_TRACE_KT:
        return FOC_MEASUREMENT_TRACE_KT_UNITS;
    default:
        return 0U;
    }
}

static bool identity_valid(const struct foc_cal_workflow_identity *identity)
{
    const struct foc_calibration_identity *setup = &identity->acquisition;
    return identity->motor_setup_id != 0U && identity->encoder_id != 0U &&
        identity->mechanical_frame_id != 0U && setup->adc_full_scale_counts != 0U &&
        setup->adc_full_scale_counts <= UINT16_MAX && isfinite(setup->current_a_per_count) &&
        setup->current_a_per_count > 0.0f && setup->encoder_counts_per_revolution >= 2U &&
        setup->encoder_counts_per_revolution <= 65536U &&
        (setup->encoder_direction == -1 || setup->encoder_direction == 1) &&
        setup->phase_order <= 1U && setup->pole_pairs != 0U;
}

static bool requires_reference(enum foc_measurement_trace_stage stage)
{
    return stage == FOC_MEASUREMENT_TRACE_ROTOR || stage == FOC_MEASUREMENT_TRACE_OFFSET ||
        stage == FOC_MEASUREMENT_TRACE_LUT || stage == FOC_MEASUREMENT_TRACE_LUT_HOLDOUT ||
        stage == FOC_MEASUREMENT_TRACE_MECHANICAL;
}

static enum foc_measurement_trace_status record_identity(
    const struct foc_measurement_trace_contract *contract,
    const struct foc_measurement_trace_record *record
)
{
    if (record->stage != contract->stage) {
        return FOC_MEASUREMENT_TRACE_ERR_TYPE;
    }
    if (record->units != expected_units(contract->stage)) {
        return FOC_MEASUREMENT_TRACE_ERR_UNITS;
    }
    if (record->channel != contract->channel) {
        return FOC_MEASUREMENT_TRACE_ERR_IDENTITY;
    }
    if (contract->stage == FOC_MEASUREMENT_TRACE_KT) {
        const struct foc_cal_workflow_kt *kt = &record->payload.kt;
        if (kt->capture != contract->capture ||
            !foc_cal_workflow_identity_matches(&contract->identity, &kt->identity)) {
            return FOC_MEASUREMENT_TRACE_ERR_IDENTITY;
        }
    } else {
        const struct foc_cal_workflow_sample *sample = &record->payload.sample;
        if (sample->session != contract->session || sample->capture != contract->capture ||
            !foc_cal_workflow_identity_matches(&contract->identity, &sample->identity)) {
            return FOC_MEASUREMENT_TRACE_ERR_IDENTITY;
        }
    }
    return FOC_MEASUREMENT_TRACE_OK;
}

static enum foc_measurement_trace_status record_timing(
    const struct foc_measurement_trace *context,
    const struct foc_measurement_trace_record *record
)
{
    const struct foc_measurement_trace_contract *contract = &context->contract;
    const bool paired = requires_reference(contract->stage);
    const bool electrical = contract->stage == FOC_MEASUREMENT_TRACE_ELECTRICAL;
    const bool voltage_endpoint = electrical && record->reference_clock_id != 0U;
    const bool frame_encoder = record->frame_encoder_clock_id != 0U;
    if (record->clock_id != contract->clock_id ||
        ((paired || voltage_endpoint) && record->reference_clock_id != contract->clock_id) ||
        (!paired && !voltage_endpoint && record->reference_clock_id != 0U)) {
        return FOC_MEASUREMENT_TRACE_ERR_CLOCK;
    }
    if ((frame_encoder && (!electrical || record->frame_encoder_clock_id != contract->clock_id)) ||
        (!frame_encoder && record->frame_encoder_us != 0U)) {
        return FOC_MEASUREMENT_TRACE_ERR_CLOCK;
    }
    if (record->trigger_observed &&
        !clock_ordered(record->source_us, record->trigger_us, contract->allow_wrap)) {
        return FOC_MEASUREMENT_TRACE_ERR_TIME_ORDER;
    }
    if (record->completion_observed) {
        const bool previous_completion =
            context->report.accepted != context->report.completion_unobserved;
        if (previous_completion &&
            !clock_ordered(
                record->completion_us,
                context->last_completion_us,
                contract->allow_wrap
            )) {
            return FOC_MEASUREMENT_TRACE_ERR_TIME_ORDER;
        }
        if (!clock_ordered(record->completion_us, record->source_us, contract->allow_wrap) ||
            ((paired || voltage_endpoint) &&
             !clock_ordered(record->completion_us, record->reference_us, contract->allow_wrap)) ||
            (frame_encoder &&
             !clock_ordered(
                 record->completion_us,
                 record->frame_encoder_us,
                 contract->allow_wrap
             ))) {
            return FOC_MEASUREMENT_TRACE_ERR_TIME_ORDER;
        }
        if (record->completion_us - record->source_us > contract->max_age_us ||
            ((paired || voltage_endpoint) &&
             record->completion_us - record->reference_us > contract->max_age_us) ||
            (frame_encoder &&
             record->completion_us - record->frame_encoder_us > contract->max_age_us)) {
            return FOC_MEASUREMENT_TRACE_ERR_AGE;
        }
    }
    if (frame_encoder &&
        clock_distance(record->source_us, record->frame_encoder_us, contract->allow_wrap) >
            contract->max_skew_us) {
        return FOC_MEASUREMENT_TRACE_ERR_ALIGNMENT;
    }
    if (paired || voltage_endpoint) {
        const bool angular = contract->stage == FOC_MEASUREMENT_TRACE_ROTOR ||
            contract->stage == FOC_MEASUREMENT_TRACE_OFFSET;
        const uint32_t maximum_skew = angular ? contract->max_skew_us : 0U;
        if (clock_distance(record->source_us, record->reference_us, contract->allow_wrap) >
            maximum_skew) {
            return FOC_MEASUREMENT_TRACE_ERR_ALIGNMENT;
        }
    }
    if (context->report.accepted != 0U) {
        if (record->source_us == context->last_source_us ||
            !clock_ordered(record->source_us, context->last_source_us, contract->allow_wrap) ||
            (paired &&
             (record->reference_us == context->last_reference_us ||
              !clock_ordered(
                  record->reference_us,
                  context->last_reference_us,
                  contract->allow_wrap
              )))) {
            return FOC_MEASUREMENT_TRACE_ERR_TIME_ORDER;
        }
        if (record->source_us - context->last_source_us > contract->max_interval_us) {
            return FOC_MEASUREMENT_TRACE_ERR_GAP;
        }
    }
    return FOC_MEASUREMENT_TRACE_OK;
}

static enum foc_measurement_trace_status check_electrical(
    const struct foc_measurement_trace *context,
    const struct foc_measurement_trace_record *record
)
{
    const struct foc_motor_id_sample *sample = &record->payload.sample.data.electrical;
    if (sample->setup_id != context->contract.identity.motor_setup_id ||
        sample->capture_id != context->contract.capture) {
        return FOC_MEASUREMENT_TRACE_ERR_IDENTITY;
    }
    const bool interval = context->report.accepted != 0U;
    const bool measured = context->contract.source == FOC_MEASUREMENT_TRACE_EXTERNAL_RECORDED ||
        context->contract.source == FOC_MEASUREMENT_TRACE_BOARD_RECORDED;
    const enum foc_measurement_trace_evidence voltage =
        measured ? FOC_MEASUREMENT_TRACE_TERMINAL_MEASURED : FOC_MEASUREMENT_TRACE_TERMINAL_VIRTUAL;
    if (record->evidence != voltage &&
        (interval || record->evidence != FOC_MEASUREMENT_TRACE_EVIDENCE_NONE)) {
        return FOC_MEASUREMENT_TRACE_ERR_DATA_REQUIRED;
    }
    if (!has_flags(
            record,
            FOC_MEASUREMENT_TRACE_FRAME_CONFIRMED | FOC_MEASUREMENT_TRACE_WINDOW_CONFIRMED
        ) ||
        sample->omega_e_rad_s != 0.0f ||
        (interval && !has_flags(record, FOC_MEASUREMENT_TRACE_OUTPUT_CONFIRMED))) {
        return FOC_MEASUREMENT_TRACE_ERR_PRECONDITION_REQUIRED;
    }
    if (!isfinite(sample->current_a) || (interval && !isfinite(sample->mean_voltage_v))) {
        return FOC_MEASUREMENT_TRACE_ERR_RANGE;
    }
    if ((interval &&
         (sample->voltage_begin_us != context->last_source_us ||
          sample->voltage_end_us != record->source_us)) ||
        (record->reference_clock_id != 0U && sample->voltage_end_us != record->reference_us)) {
        return FOC_MEASUREMENT_TRACE_ERR_ALIGNMENT;
    }
    return FOC_MEASUREMENT_TRACE_OK;
}

static enum foc_measurement_trace_status check_mechanical(
    const struct foc_measurement_trace *context,
    const struct foc_measurement_trace_record *record
)
{
    const struct foc_mechanical_id_sample *sample = &record->payload.sample.data.mechanical;
    if (sample->setup_id != context->contract.identity.motor_setup_id ||
        sample->capture_id != context->contract.capture || sample->torque_model_id == 0U) {
        return FOC_MEASUREMENT_TRACE_ERR_IDENTITY;
    }
    const bool observed_dq = record->evidence == FOC_MEASUREMENT_TRACE_OBSERVED_DQ;
    if (!observed_dq && record->evidence != FOC_MEASUREMENT_TRACE_SHAFT_MEASURED) {
        return FOC_MEASUREMENT_TRACE_ERR_TORQUE_SOURCE_REQUIRED;
    }
    const bool observed_load = has_flags(record, FOC_MEASUREMENT_TRACE_LOAD_OBSERVED);
    const bool negligible_load = has_flags(record, FOC_MEASUREMENT_TRACE_LOAD_NEGLIGIBLE);
    if (observed_load == negligible_load || (negligible_load && sample->load_nm != 0.0f)) {
        return FOC_MEASUREMENT_TRACE_ERR_LOAD_REQUIRED;
    }
    if (!has_flags(
            record,
            FOC_MEASUREMENT_TRACE_FRAME_CONFIRMED | FOC_MEASUREMENT_TRACE_WINDOW_CONFIRMED |
                FOC_MEASUREMENT_TRACE_OUTPUT_CONFIRMED
        )) {
        return FOC_MEASUREMENT_TRACE_ERR_PRECONDITION_REQUIRED;
    }
    if (!isfinite(sample->speed_rad_s) || !isfinite(sample->load_nm) ||
        (observed_dq ? !isfinite(sample->id_a) || !isfinite(sample->iq_a)
                     : !isfinite(sample->torque_nm))) {
        return FOC_MEASUREMENT_TRACE_ERR_RANGE;
    }
    const uint32_t expected_begin =
        context->report.accepted == 0U ? record->source_us : context->last_source_us;
    if (sample->interval_begin_us != expected_begin) {
        return FOC_MEASUREMENT_TRACE_ERR_ALIGNMENT;
    }
    return FOC_MEASUREMENT_TRACE_OK;
}

static enum foc_measurement_trace_status record_observation(
    const struct foc_measurement_trace *context,
    const struct foc_measurement_trace_record *record
)
{
    const struct foc_cal_workflow_sample *sample = &record->payload.sample;
    const struct foc_calibration_identity *setup = &context->contract.identity.acquisition;
    switch (context->contract.stage) {
    case FOC_MEASUREMENT_TRACE_ADC:
        if (record->evidence != FOC_MEASUREMENT_TRACE_ZERO_CONFIRMED ||
            !has_flags(record, FOC_MEASUREMENT_TRACE_WINDOW_CONFIRMED) ||
            sample->data.adc.valid_mask != 7U) {
            return FOC_MEASUREMENT_TRACE_ERR_PRECONDITION_REQUIRED;
        }
        if (sample->data.adc.phase_a > setup->adc_full_scale_counts ||
            sample->data.adc.phase_b > setup->adc_full_scale_counts ||
            sample->data.adc.phase_c > setup->adc_full_scale_counts) {
            return FOC_MEASUREMENT_TRACE_ERR_RANGE;
        }
        break;
    case FOC_MEASUREMENT_TRACE_ELECTRICAL:
        return check_electrical(context, record);
    case FOC_MEASUREMENT_TRACE_ROTOR:
    case FOC_MEASUREMENT_TRACE_OFFSET:
        if (record->evidence != FOC_MEASUREMENT_TRACE_INDEPENDENT_ANGLE) {
            return FOC_MEASUREMENT_TRACE_ERR_REFERENCE_REQUIRED;
        }
        if (!sample->encoder_raw_valid) {
            return FOC_MEASUREMENT_TRACE_ERR_DATA_REQUIRED;
        }
        if (sample->encoder_raw_code >= setup->encoder_counts_per_revolution ||
            !isfinite(sample->data.angle.electrical_rad) ||
            !isfinite(sample->data.angle.mechanical_rad)) {
            return FOC_MEASUREMENT_TRACE_ERR_RANGE;
        }
        break;
    case FOC_MEASUREMENT_TRACE_LUT:
    case FOC_MEASUREMENT_TRACE_LUT_HOLDOUT: {
        const struct foc_encoder_cal_sample *lut = &sample->data.lut;
        if (record->evidence != FOC_MEASUREMENT_TRACE_INDEPENDENT_ANGLE ||
            lut->setup.reference_identity == 0U ||
            lut->setup.reference_identity == lut->setup.encoder_identity) {
            return FOC_MEASUREMENT_TRACE_ERR_REFERENCE_REQUIRED;
        }
        if (lut->capture_id != context->contract.capture ||
            lut->setup.encoder_identity != context->contract.identity.encoder_id ||
            lut->setup.reference_identity != context->contract.lut_reference_id ||
            lut->setup.counts_per_revolution != setup->encoder_counts_per_revolution ||
            (int32_t)lut->setup.direction != context->contract.lut_direction) {
            return FOC_MEASUREMENT_TRACE_ERR_IDENTITY;
        }
        if (lut->raw_code >= setup->encoder_counts_per_revolution ||
            !isfinite(lut->reference_rad) || lut->reference_rad < 0.0 ||
            lut->reference_rad >= TWO_PI) {
            return FOC_MEASUREMENT_TRACE_ERR_RANGE;
        }
        break;
    }
    case FOC_MEASUREMENT_TRACE_MECHANICAL:
        return check_mechanical(context, record);
    case FOC_MEASUREMENT_TRACE_KT: {
        const struct foc_cal_workflow_kt *kt = &record->payload.kt;
        if (record->evidence != FOC_MEASUREMENT_TRACE_INDEPENDENT_KT || kt->torque_model_id == 0U) {
            return FOC_MEASUREMENT_TRACE_ERR_DATA_REQUIRED;
        }
        if (!isfinite(kt->nm_per_a) || kt->nm_per_a <= 0.0f) {
            return FOC_MEASUREMENT_TRACE_ERR_RANGE;
        }
        break;
    }
    }
    return FOC_MEASUREMENT_TRACE_OK;
}

static enum foc_measurement_trace_status validate_record(
    const struct foc_measurement_trace *context,
    const struct foc_measurement_trace_record *record
)
{
    const enum foc_measurement_trace_status identification =
        record_identity(&context->contract, record);
    if (identification != FOC_MEASUREMENT_TRACE_OK) {
        return identification;
    }
    const uint32_t known_flags = FOC_MEASUREMENT_TRACE_VALID |
        FOC_MEASUREMENT_TRACE_OUTPUT_CONFIRMED | FOC_MEASUREMENT_TRACE_SATURATED |
        FOC_MEASUREMENT_TRACE_FRAME_CONFIRMED | FOC_MEASUREMENT_TRACE_WINDOW_CONFIRMED |
        FOC_MEASUREMENT_TRACE_LOAD_OBSERVED | FOC_MEASUREMENT_TRACE_LOAD_NEGLIGIBLE |
        FOC_MEASUREMENT_TRACE_SOURCE_PROXY | FOC_MEASUREMENT_TRACE_LIMITED;
    if (!has_flags(record, FOC_MEASUREMENT_TRACE_VALID) || (record->flags & ~known_flags) != 0U) {
        return FOC_MEASUREMENT_TRACE_ERR_INVALID;
    }
    if ((record->flags & (FOC_MEASUREMENT_TRACE_SATURATED | FOC_MEASUREMENT_TRACE_LIMITED)) != 0U) {
        return FOC_MEASUREMENT_TRACE_ERR_SATURATION;
    }
    const enum foc_measurement_trace_status timing = record_timing(context, record);
    if (timing != FOC_MEASUREMENT_TRACE_OK) {
        return timing;
    }
    const enum foc_measurement_trace_status observation = record_observation(context, record);
    if (observation != FOC_MEASUREMENT_TRACE_OK) {
        return observation;
    }
    const uint32_t rows = context->report.accepted + context->report.rejected;
    const uint32_t sequence_delta = record->sequence - (context->contract.first_sequence + rows);
    if (sequence_delta != 0U) {
        return sequence_delta < HALF_RANGE ? FOC_MEASUREMENT_TRACE_ERR_GAP
                                           : FOC_MEASUREMENT_TRACE_ERR_SEQUENCE;
    }
    if (rows >= context->contract.expected_samples) {
        return FOC_MEASUREMENT_TRACE_ERR_INCOMPLETE;
    }
    return FOC_MEASUREMENT_TRACE_OK;
}

static void normalize(
    const struct foc_measurement_trace_contract *contract,
    const struct foc_measurement_trace_record *record,
    union foc_measurement_trace_payload *output
)
{
    const enum foc_cal_workflow_source source =
        foc_measurement_trace_workflow_source(contract->source);
    const bool recorded = source == FOC_CAL_WORKFLOW_EXTERNAL_RECORDED_DATA;
    *output = record->payload;
    if (contract->stage == FOC_MEASUREMENT_TRACE_KT) {
        output->kt.source = source;
        output->kt.captured_us = record->source_us;
        output->kt.qualified = true;
        return;
    }

    struct foc_cal_workflow_sample *sample = &output->sample;
    sample->source = source;
    sample->units = (enum foc_cal_workflow_units)record->units;
    sample->valid = true;
    sample->saturated = false;
    sample->output_confirmed = has_flags(record, FOC_MEASUREMENT_TRACE_OUTPUT_CONFIRMED);
    switch (contract->stage) {
    case FOC_MEASUREMENT_TRACE_ADC:
        sample->data.adc.sequence = record->sequence;
        sample->data.adc.captured_us = record->source_us;
        sample->data.adc.zero_current_confirmed = true;
        break;
    case FOC_MEASUREMENT_TRACE_ELECTRICAL: {
        struct foc_motor_id_sample *electrical = &sample->data.electrical;
        electrical->sequence = record->sequence;
        electrical->captured_us = record->source_us;
        electrical->source = recorded ? FOC_MOTOR_ID_MEASURED : FOC_MOTOR_ID_VIRTUAL;
        electrical->current_valid = true;
        electrical->voltage_valid = record->evidence != FOC_MEASUREMENT_TRACE_EVIDENCE_NONE;
        electrical->voltage_source = FOC_MOTOR_ID_VOLTAGE_UNVERIFIED;
        if (electrical->voltage_valid) {
            electrical->voltage_source = recorded ? FOC_MOTOR_ID_VOLTAGE_MEASURED_TERMINAL
                                                  : FOC_MOTOR_ID_VOLTAGE_VIRTUAL_TERMINAL;
        }
        electrical->frame_confirmed = true;
        electrical->stationary_confirmed = true;
        electrical->output_confirmed = sample->output_confirmed;
        electrical->saturated = false;
        break;
    }
    case FOC_MEASUREMENT_TRACE_ROTOR:
    case FOC_MEASUREMENT_TRACE_OFFSET:
        sample->data.angle.sequence = record->sequence;
        sample->data.angle.captured_us = record->source_us;
        sample->data.angle.valid = true;
        break;
    case FOC_MEASUREMENT_TRACE_LUT:
    case FOC_MEASUREMENT_TRACE_LUT_HOLDOUT: {
        struct foc_encoder_cal_sample *lut = &sample->data.lut;
        lut->sequence = record->sequence;
        lut->encoder_us = record->source_us;
        lut->reference_us = record->reference_us;
        lut->setup.source =
            recorded ? FOC_ENCODER_CAL_SOURCE_EXTERNAL : FOC_ENCODER_CAL_SOURCE_VIRTUAL;
        lut->encoder_valid = true;
        lut->reference_valid = true;
        lut->reference_confirmed = true;
        break;
    }
    case FOC_MEASUREMENT_TRACE_MECHANICAL: {
        struct foc_mechanical_id_sample *mechanical = &sample->data.mechanical;
        mechanical->sequence = record->sequence;
        mechanical->speed_us = record->source_us;
        mechanical->torque_us = record->reference_us;
        mechanical->torque_source = record->evidence == FOC_MEASUREMENT_TRACE_OBSERVED_DQ
            ? FOC_MECHANICAL_ID_TORQUE_OBSERVED_DQ
            : FOC_MECHANICAL_ID_TORQUE_EQUIVALENT;
        mechanical->load_source = has_flags(record, FOC_MEASUREMENT_TRACE_LOAD_OBSERVED)
            ? FOC_MECHANICAL_ID_LOAD_OBSERVED
            : FOC_MECHANICAL_ID_LOAD_NEGLIGIBLE;
        mechanical->speed_valid = true;
        mechanical->torque_valid = true;
        mechanical->frame_confirmed = true;
        mechanical->interval_confirmed = true;
        break;
    }
    case FOC_MEASUREMENT_TRACE_KT:
        break;
    }
}

/* Public Functions ------------------------------------------------------------- */

enum foc_measurement_trace_status foc_measurement_trace_init(
    struct foc_measurement_trace *context,
    const struct foc_measurement_trace_contract *contract
)
{
    if (context == NULL || contract == NULL) {
        return FOC_MEASUREMENT_TRACE_ERR_INPUT;
    }
    if (contract->version != FOC_MEASUREMENT_TRACE_VERSION) {
        return FOC_MEASUREMENT_TRACE_ERR_VERSION;
    }
    if (foc_measurement_trace_workflow_source(contract->source) == FOC_CAL_WORKFLOW_SOURCE_NONE ||
        contract->provenance < FOC_MEASUREMENT_TRACE_PROVENANCE_DECLARED ||
        contract->provenance > FOC_MEASUREMENT_TRACE_PROVENANCE_UNVERIFIED) {
        return FOC_MEASUREMENT_TRACE_ERR_SOURCE;
    }
    if (expected_units(contract->stage) == 0U) {
        return FOC_MEASUREMENT_TRACE_ERR_TYPE;
    }
    if (!identity_valid(&contract->identity) || contract->session == 0U ||
        contract->capture == 0U) {
        return FOC_MEASUREMENT_TRACE_ERR_IDENTITY;
    }
    if (contract->clock_id == 0U) {
        return FOC_MEASUREMENT_TRACE_ERR_CLOCK;
    }
    if (contract->stage == FOC_MEASUREMENT_TRACE_LUT ||
        contract->stage == FOC_MEASUREMENT_TRACE_LUT_HOLDOUT) {
        if (contract->lut_reference_id == 0U ||
            contract->lut_reference_id == contract->identity.encoder_id) {
            return FOC_MEASUREMENT_TRACE_ERR_REFERENCE_REQUIRED;
        }
        if (contract->lut_direction != 1 && contract->lut_direction != -1) {
            return FOC_MEASUREMENT_TRACE_ERR_IDENTITY;
        }
    }
    if (contract->max_age_us >= HALF_RANGE || contract->max_interval_us >= HALF_RANGE ||
        contract->max_skew_us >= HALF_RANGE || contract->expected_samples == 0U ||
        contract->expected_samples > FOC_MEASUREMENT_TRACE_MAX_SAMPLES ||
        (contract->stage == FOC_MEASUREMENT_TRACE_KT && contract->expected_samples != 1U)) {
        return FOC_MEASUREMENT_TRACE_ERR_RANGE;
    }
    context->contract = *contract;
    context->report = (struct foc_measurement_trace_report){0};
    context->last_source_us = 0U;
    context->last_reference_us = 0U;
    context->last_completion_us = 0U;
    context->active = true;
    return FOC_MEASUREMENT_TRACE_OK;
}

enum foc_measurement_trace_status foc_measurement_trace_convert(
    struct foc_measurement_trace *context,
    const struct foc_measurement_trace_record *record,
    union foc_measurement_trace_payload *payload_out,
    uint32_t *now_us_out
)
{
    if (context == NULL || record == NULL || payload_out == NULL || now_us_out == NULL) {
        return FOC_MEASUREMENT_TRACE_ERR_INPUT;
    }
    if (!context->active) {
        return FOC_MEASUREMENT_TRACE_ERR_STATE;
    }
    struct foc_measurement_trace_report *report = &context->report;
    report->last_status = validate_record(context, record);
    if (report->last_status != FOC_MEASUREMENT_TRACE_OK) {
        if (report->rejected == 0U) {
            report->first_failure = report->last_status;
            report->first_rejected_sequence = record->sequence;
        }
        if (report->rejected < UINT32_MAX - report->accepted) {
            ++report->rejected;
        }
        return report->last_status;
    }
    normalize(&context->contract, record, payload_out);
    *now_us_out = record->completion_observed ? record->completion_us : record->source_us;
    context->last_source_us = record->source_us;
    context->last_reference_us = record->reference_us;
    if (record->completion_observed) {
        context->last_completion_us = record->completion_us;
    }
    ++report->accepted;
    report->completion_unobserved += !record->completion_observed;
    report->trigger_observed += record->trigger_observed;
    report->source_proxy += has_flags(record, FOC_MEASUREMENT_TRACE_SOURCE_PROXY);
    return FOC_MEASUREMENT_TRACE_OK;
}

enum foc_measurement_trace_status foc_measurement_trace_finish(
    struct foc_measurement_trace *context
)
{
    if (context == NULL) {
        return FOC_MEASUREMENT_TRACE_ERR_INPUT;
    }
    if (!context->active) {
        return FOC_MEASUREMENT_TRACE_ERR_STATE;
    }
    context->active = false;
    if (context->report.first_failure != FOC_MEASUREMENT_TRACE_OK) {
        context->report.last_status = context->report.first_failure;
    } else if (context->report.accepted != context->contract.expected_samples) {
        context->report.last_status = FOC_MEASUREMENT_TRACE_ERR_INCOMPLETE;
    } else {
        context->report.complete = true;
    }
    return context->report.last_status;
}

enum foc_measurement_trace_status foc_measurement_trace_cancel(
    struct foc_measurement_trace *context
)
{
    if (context == NULL) {
        return FOC_MEASUREMENT_TRACE_ERR_INPUT;
    }
    if (!context->active) {
        return FOC_MEASUREMENT_TRACE_ERR_STATE;
    }
    context->active = false;
    context->report.last_status = FOC_MEASUREMENT_TRACE_ERR_CANCELLED;
    return context->report.last_status;
}

enum foc_cal_workflow_source foc_measurement_trace_workflow_source(
    enum foc_measurement_trace_source source
)
{
    switch (source) {
    case FOC_MEASUREMENT_TRACE_HOST_SYNTHETIC:
        return FOC_CAL_WORKFLOW_HOST_SYNTHETIC;
    case FOC_MEASUREMENT_TRACE_HOST_VIRTUAL:
        return FOC_CAL_WORKFLOW_HOST_VIRTUAL_MEASUREMENT;
    case FOC_MEASUREMENT_TRACE_EXTERNAL_RECORDED:
    case FOC_MEASUREMENT_TRACE_BOARD_RECORDED:
        return FOC_CAL_WORKFLOW_EXTERNAL_RECORDED_DATA;
    default:
        return FOC_CAL_WORKFLOW_SOURCE_NONE;
    }
}

const char *foc_measurement_trace_status_name(enum foc_measurement_trace_status status)
{
    switch (status) {
    case FOC_MEASUREMENT_TRACE_OK:
        return "OK";
    case FOC_MEASUREMENT_TRACE_ERR_INPUT:
        return "INPUT";
    case FOC_MEASUREMENT_TRACE_ERR_STATE:
        return "STATE";
    case FOC_MEASUREMENT_TRACE_ERR_VERSION:
        return "VERSION";
    case FOC_MEASUREMENT_TRACE_ERR_SOURCE:
        return "SOURCE";
    case FOC_MEASUREMENT_TRACE_ERR_IDENTITY:
        return "IDENTITY";
    case FOC_MEASUREMENT_TRACE_ERR_TYPE:
        return "TYPE";
    case FOC_MEASUREMENT_TRACE_ERR_UNITS:
        return "UNITS";
    case FOC_MEASUREMENT_TRACE_ERR_CLOCK:
        return "CLOCK";
    case FOC_MEASUREMENT_TRACE_ERR_SEQUENCE:
        return "SEQUENCE";
    case FOC_MEASUREMENT_TRACE_ERR_TIME_ORDER:
        return "TIME_ORDER";
    case FOC_MEASUREMENT_TRACE_ERR_AGE:
        return "AGE";
    case FOC_MEASUREMENT_TRACE_ERR_ALIGNMENT:
        return "ALIGNMENT";
    case FOC_MEASUREMENT_TRACE_ERR_GAP:
        return "GAP";
    case FOC_MEASUREMENT_TRACE_ERR_RANGE:
        return "RANGE";
    case FOC_MEASUREMENT_TRACE_ERR_INVALID:
        return "INVALID";
    case FOC_MEASUREMENT_TRACE_ERR_SATURATION:
        return "SATURATION";
    case FOC_MEASUREMENT_TRACE_ERR_PRECONDITION_REQUIRED:
        return "PRECONDITION_REQUIRED";
    case FOC_MEASUREMENT_TRACE_ERR_DATA_REQUIRED:
        return "DATA_REQUIRED";
    case FOC_MEASUREMENT_TRACE_ERR_REFERENCE_REQUIRED:
        return "REFERENCE_REQUIRED";
    case FOC_MEASUREMENT_TRACE_ERR_TORQUE_SOURCE_REQUIRED:
        return "TORQUE_SOURCE_REQUIRED";
    case FOC_MEASUREMENT_TRACE_ERR_LOAD_REQUIRED:
        return "LOAD_REQUIRED";
    case FOC_MEASUREMENT_TRACE_ERR_INCOMPLETE:
        return "INCOMPLETE";
    case FOC_MEASUREMENT_TRACE_ERR_CANCELLED:
        return "CANCELLED";
    default:
        return "UNKNOWN";
    }
}

/* ---------------------------------------- end of file ----------------------------------------- */
