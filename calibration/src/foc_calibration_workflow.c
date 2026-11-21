/**
 * @file    foc_calibration_workflow.c
 * @brief   Qualified calibration capture and explicit review orchestration.
 * @version v0.1.0
 * @date    2026.11.18
 *
 * Implementation role
 * -------------------
 * Bind existing estimators to profile, source, time and setup without
 * acquiring measurements or owning an output, Runtime, or storage device.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_calibration_workflow.h"

#include <math.h>
#include <string.h>

/* Private Constants ------------------------------------------------------------ */

#define HALF_RANGE UINT32_C(0x80000000)

/* Native init routines return zero or a negative algorithm status. */
enum {
    OPEN_IDENTITY_MISMATCH = 1
};

/* Private Helpers -------------------------------------------------------------- */

static bool newer(uint32_t value, uint32_t previous)
{
    const uint32_t delta = value - previous;
    return delta != 0U && delta < HALF_RANGE;
}

static bool live(const struct foc_cal_workflow *workflow)
{
    return workflow->result.state == FOC_CAL_WORKFLOW_READY ||
        workflow->result.state == FOC_CAL_WORKFLOW_WAITING_FOR_DATA ||
        workflow->result.state == FOC_CAL_WORKFLOW_RUNNING;
}

static bool stage_valid(enum foc_cal_workflow_stage stage)
{
    return (unsigned int)stage < FOC_CAL_WORKFLOW_STAGE_COUNT;
}

static uint32_t dependencies(enum foc_cal_workflow_stage stage, uint32_t selected)
{
    switch (stage) {
    case FOC_CAL_WORKFLOW_ROTOR:
        return selected & FOC_CAL_WORKFLOW_STAGE_BIT(FOC_CAL_WORKFLOW_LUT);
    case FOC_CAL_WORKFLOW_OFFSET:
        return FOC_CAL_WORKFLOW_STAGE_BIT(FOC_CAL_WORKFLOW_ROTOR);
    case FOC_CAL_WORKFLOW_TUNING:
        return FOC_CAL_WORKFLOW_STAGE_BIT(FOC_CAL_WORKFLOW_MECHANICAL);
    default:
        return 0U;
    }
}

static bool capture_used(const struct foc_cal_workflow_bundle *bundle, uint32_t capture)
{
    for (unsigned int i = 0U; i < FOC_CAL_WORKFLOW_STAGE_COUNT; ++i) {
        if (bundle->stages[i].primary.id == capture || bundle->stages[i].holdout.id == capture) {
            return true;
        }
    }
    return bundle->electrical_q_capture.id == capture;
}

static bool identity_valid(const struct foc_cal_workflow_identity *identity)
{
    const struct foc_calibration_identity *acquisition = &identity->acquisition;
    return identity->motor_setup_id != 0U && identity->encoder_id != 0U &&
        identity->mechanical_frame_id != 0U && acquisition->adc_full_scale_counts != 0U &&
        acquisition->adc_full_scale_counts <= UINT16_MAX &&
        isfinite(acquisition->current_a_per_count) && acquisition->current_a_per_count > 0.0f &&
        acquisition->encoder_counts_per_revolution >= 2U &&
        acquisition->encoder_counts_per_revolution <= 65536U &&
        (acquisition->encoder_direction == -1 || acquisition->encoder_direction == 1) &&
        acquisition->phase_order <= 1U && acquisition->pole_pairs != 0U;
}

static bool source_valid(enum foc_cal_workflow_source source)
{
    return source == FOC_CAL_WORKFLOW_HOST_SYNTHETIC ||
        source == FOC_CAL_WORKFLOW_HOST_VIRTUAL_MEASUREMENT ||
        source == FOC_CAL_WORKFLOW_EXTERNAL_RECORDED_DATA;
}

static enum foc_cal_workflow_quality quality_for(enum foc_calibration_failure failure)
{
    switch (failure) {
    case FOC_CALIBRATION_FAILURE_SAMPLE_COUNT:
        return FOC_CAL_WORKFLOW_INSUFFICIENT_DATA;
    case FOC_CALIBRATION_FAILURE_UNIDENTIFIABLE:
        return FOC_CAL_WORKFLOW_UNIDENTIFIABLE;
    case FOC_CALIBRATION_FAILURE_CONFIG_MISMATCH:
        return FOC_CAL_WORKFLOW_IDENTITY_MISMATCH;
    case FOC_CALIBRATION_FAILURE_INVALID_SAMPLE:
    case FOC_CALIBRATION_FAILURE_TIME_ORDER:
    case FOC_CALIBRATION_FAILURE_PRECONDITION:
        return FOC_CAL_WORKFLOW_INVALID_OBSERVATION;
    default:
        return FOC_CAL_WORKFLOW_REJECTED_BY_QUALITY;
    }
}

static void revoke_candidates(struct foc_cal_workflow_bundle *bundle)
{
    bundle->passed = 0U;
    bundle->missing = bundle->request.stages;
    bundle->complete = false;
    bundle->applicable = false;
    bundle->reviewed = false;
    bundle->adc.usable = false;
    bundle->electrical.usable = false;
    bundle->electrical_q.usable = false;
    bundle->electrical_model = (struct foc_motor_rl_model){0};
    bundle->rotor.identifiable = false;
    bundle->offset.usable = false;
    bundle->lut.state = FOC_CALIBRATION_STATE_INVALID;
    bundle->mechanical.usable = false;
    bundle->tuning.valid = false;
    bundle->kt.qualified = false;
    bundle->snapshot.accepted = false;
}

static void conclude_stage(
    struct foc_cal_workflow *workflow,
    enum foc_cal_workflow_stage stage,
    enum foc_cal_workflow_quality quality,
    enum foc_calibration_failure failure,
    int status,
    int algorithm_status
)
{
    struct foc_cal_workflow_bundle *bundle = &workflow->result;
    struct foc_cal_workflow_stage_result *result = &bundle->stages[stage];
    result->quality = quality;
    result->failure = failure;
    result->status = status;
    result->algorithm_status = algorithm_status;
    if (quality == FOC_CAL_WORKFLOW_PASS) {
        bundle->passed |= FOC_CAL_WORKFLOW_STAGE_BIT(stage);
        bundle->missing &= ~FOC_CAL_WORKFLOW_STAGE_BIT(stage);
    }
    bundle->active = FOC_CAL_WORKFLOW_STAGE_COUNT;
    bundle->state = FOC_CAL_WORKFLOW_READY;
    workflow->holdout = false;
}

static int reject_identity(struct foc_cal_workflow *workflow, enum foc_cal_workflow_stage stage)
{
    conclude_stage(
        workflow,
        stage,
        FOC_CAL_WORKFLOW_IDENTITY_MISMATCH,
        FOC_CALIBRATION_FAILURE_CONFIG_MISMATCH,
        FOC_CAL_WORKFLOW_ERR_IDENTITY,
        0
    );
    revoke_candidates(&workflow->result);
    workflow->result.state = FOC_CAL_WORKFLOW_REJECTED;
    return FOC_CAL_WORKFLOW_ERR_IDENTITY;
}

static bool uses_frozen_lut(const struct foc_cal_workflow *workflow)
{
    return (workflow->result.request.stages & FOC_CAL_WORKFLOW_STAGE_BIT(FOC_CAL_WORKFLOW_LUT)) !=
        0U;
}

static int setup_angle_encoder(struct foc_cal_workflow *workflow)
{
    if (!uses_frozen_lut(workflow)) {
        return FOC_ENCODER_STATUS_OK;
    }
    struct foc_encoder_lut lut;
    if (foc_encoder_cal_make_lut(&workflow->result.lut, &lut) != FOC_ENCODER_CAL_OK) {
        return FOC_ENCODER_STATUS_ERR_CONFIG;
    }
    struct foc_encoder_config encoder = workflow->lut_encoder;
    /* The LUT corrects raw counts. The rotor fit still owns the relative sign. */
    encoder.direction =
        (enum foc_encoder_direction)workflow->result.request.identity.acquisition.encoder_direction;
    return foc_encoder_init_with_lut(
        &workflow->angle_encoder,
        &encoder,
        workflow->result.request.identity.encoder_id,
        &lut
    );
}

static int
open_native(struct foc_cal_workflow *workflow, const struct foc_cal_workflow_stage_request *request)
{
    const struct foc_cal_workflow_identity *identity = &workflow->result.request.identity;
    const bool external = request->source == FOC_CAL_WORKFLOW_EXTERNAL_RECORDED_DATA;
    switch (request->stage) {
    case FOC_CAL_WORKFLOW_ADC:
        if (!foc_calibration_identity_matches(
                &identity->acquisition,
                &request->policy.adc.identity
            )) {
            return OPEN_IDENTITY_MISMATCH;
        }
        return foc_calibration_adc_init(&workflow->scratch.adc, &request->policy.adc);
    case FOC_CAL_WORKFLOW_ELECTRICAL:
        if (request->policy.electrical.setup_id != identity->motor_setup_id ||
            request->policy.electrical.source !=
                (external ? FOC_MOTOR_ID_MEASURED : FOC_MOTOR_ID_VIRTUAL)) {
            return OPEN_IDENTITY_MISMATCH;
        }
        return foc_motor_id_init(
            &workflow->scratch.electrical,
            &request->policy.electrical,
            request->capture
        );
    case FOC_CAL_WORKFLOW_ROTOR:
        if (!foc_calibration_identity_matches(
                &identity->acquisition,
                &request->policy.rotor.identity
            )) {
            return OPEN_IDENTITY_MISMATCH;
        }
        if (setup_angle_encoder(workflow) != FOC_ENCODER_STATUS_OK) {
            return FOC_ENCODER_STATUS_ERR_CONFIG;
        }
        return foc_calibration_rotor_init(&workflow->scratch.rotor, &request->policy.rotor);
    case FOC_CAL_WORKFLOW_OFFSET: {
        if (setup_angle_encoder(workflow) != FOC_ENCODER_STATUS_OK) {
            return FOC_ENCODER_STATUS_ERR_CONFIG;
        }
        struct foc_calibration_offset_config config = request->policy.offset;
        config.identity = workflow->result.rotor.identity;
        return foc_calibration_offset_init(&workflow->scratch.offset, &config);
    }
    case FOC_CAL_WORKFLOW_LUT: {
        const struct foc_encoder_cal_config *config = &request->policy.lut;
        const bool rotor_selected = (workflow->result.request.stages &
                                     FOC_CAL_WORKFLOW_STAGE_BIT(FOC_CAL_WORKFLOW_ROTOR)) != 0U;
        if (config->encoder_identity != identity->encoder_id ||
            config->encoder.counts_per_revolution !=
                identity->acquisition.encoder_counts_per_revolution ||
            (!rotor_selected &&
             config->encoder.direction != identity->acquisition.encoder_direction) ||
            config->source !=
                (external ? FOC_ENCODER_CAL_SOURCE_EXTERNAL : FOC_ENCODER_CAL_SOURCE_VIRTUAL)) {
            return OPEN_IDENTITY_MISMATCH;
        }
        const int status = foc_encoder_cal_init(&workflow->scratch.lut, config, request->capture);
        if (status == FOC_ENCODER_CAL_OK) {
            workflow->lut_encoder = config->encoder;
        }
        return status;
    }
    case FOC_CAL_WORKFLOW_MECHANICAL:
        if (request->policy.mechanical.setup_id != identity->motor_setup_id) {
            return OPEN_IDENTITY_MISMATCH;
        }
        return foc_mechanical_id_init(
            &workflow->scratch.mechanical,
            &request->policy.mechanical,
            request->capture
        );
    default:
        return FOC_CAL_WORKFLOW_ERR_INPUT;
    }
}

static enum foc_cal_workflow_units sample_metadata(
    enum foc_cal_workflow_stage stage,
    const struct foc_cal_workflow_sample *sample,
    uint32_t *time,
    uint32_t *sequence
)
{
    switch (stage) {
    case FOC_CAL_WORKFLOW_ADC:
        *time = sample->data.adc.captured_us;
        *sequence = sample->data.adc.sequence;
        return FOC_CAL_WORKFLOW_RAW_COUNTS;
    case FOC_CAL_WORKFLOW_ELECTRICAL:
        *time = sample->data.electrical.captured_us;
        *sequence = sample->data.electrical.sequence;
        return FOC_CAL_WORKFLOW_ELECTRICAL_SI;
    case FOC_CAL_WORKFLOW_ROTOR:
    case FOC_CAL_WORKFLOW_OFFSET:
        *time = sample->data.angle.captured_us;
        *sequence = sample->data.angle.sequence;
        return FOC_CAL_WORKFLOW_RAD_PAIR;
    case FOC_CAL_WORKFLOW_LUT:
        *time = sample->data.lut.encoder_us;
        *sequence = sample->data.lut.sequence;
        return FOC_CAL_WORKFLOW_ENCODER_COUNTS_RAD;
    case FOC_CAL_WORKFLOW_MECHANICAL:
        *time = sample->data.mechanical.speed_us;
        *sequence = sample->data.mechanical.sequence;
        return FOC_CAL_WORKFLOW_MECHANICAL_SI;
    default:
        return 0;
    }
}

static int push_native(
    struct foc_cal_workflow *workflow,
    const struct foc_cal_workflow_sample *sample,
    const struct foc_calibration_angle_sample *angle
)
{
    switch (workflow->result.active) {
    case FOC_CAL_WORKFLOW_ADC:
        return foc_calibration_adc_collect(&workflow->scratch.adc, &sample->data.adc);
    case FOC_CAL_WORKFLOW_ELECTRICAL:
        return foc_motor_id_push(&workflow->scratch.electrical, &sample->data.electrical);
    case FOC_CAL_WORKFLOW_ROTOR:
        return foc_calibration_rotor_collect(&workflow->scratch.rotor, angle);
    case FOC_CAL_WORKFLOW_OFFSET:
        return foc_calibration_offset_collect(&workflow->scratch.offset, angle);
    case FOC_CAL_WORKFLOW_LUT:
        return foc_encoder_cal_push(&workflow->scratch.lut, &sample->data.lut);
    case FOC_CAL_WORKFLOW_MECHANICAL:
        return foc_mechanical_id_push(&workflow->scratch.mechanical, &sample->data.mechanical);
    default:
        return FOC_CAL_WORKFLOW_ERR_STATE;
    }
}

static enum foc_calibration_failure native_failure(const struct foc_cal_workflow *workflow)
{
    switch (workflow->result.active) {
    case FOC_CAL_WORKFLOW_ADC:
        return workflow->scratch.adc.result.failure;
    case FOC_CAL_WORKFLOW_ELECTRICAL:
        return workflow->scratch.electrical.result.failure;
    case FOC_CAL_WORKFLOW_ROTOR:
        return workflow->scratch.rotor.result.failure;
    case FOC_CAL_WORKFLOW_OFFSET:
        return workflow->scratch.offset.result.failure;
    case FOC_CAL_WORKFLOW_LUT:
        return workflow->scratch.lut.result.failure;
    case FOC_CAL_WORKFLOW_MECHANICAL:
        return workflow->scratch.mechanical.result.failure;
    default:
        return FOC_CALIBRATION_FAILURE_INVALID_CONFIG;
    }
}

static bool electrical_q_active(const struct foc_cal_workflow *workflow)
{
    return workflow->result.active == FOC_CAL_WORKFLOW_ELECTRICAL &&
        workflow->result.request.require_dual_axis &&
        workflow->scratch.electrical.config.axis == FOC_MOTOR_ID_Q;
}

static int finish_native(struct foc_cal_workflow *workflow)
{
    struct foc_cal_workflow_bundle *bundle = &workflow->result;
    switch (bundle->active) {
    case FOC_CAL_WORKFLOW_ADC:
        return foc_calibration_adc_finalize(&workflow->scratch.adc, &bundle->adc);
    case FOC_CAL_WORKFLOW_ELECTRICAL:
        return foc_motor_id_finish(
            &workflow->scratch.electrical,
            electrical_q_active(workflow) ? &bundle->electrical_q : &bundle->electrical
        );
    case FOC_CAL_WORKFLOW_ROTOR:
        return foc_calibration_rotor_finalize(&workflow->scratch.rotor, &bundle->rotor);
    case FOC_CAL_WORKFLOW_OFFSET:
        return foc_calibration_offset_finalize(&workflow->scratch.offset, &bundle->offset);
    case FOC_CAL_WORKFLOW_LUT: {
        const int status = foc_encoder_cal_finish(&workflow->scratch.lut);
        if (status != FOC_ENCODER_CAL_OK) {
            bundle->lut = workflow->scratch.lut.result;
            return status;
        }
        return foc_encoder_cal_export(&workflow->scratch.lut, &bundle->lut);
    }
    case FOC_CAL_WORKFLOW_MECHANICAL:
        return foc_mechanical_id_finish(&workflow->scratch.mechanical, &bundle->mechanical);
    default:
        return FOC_CAL_WORKFLOW_ERR_STATE;
    }
}

/* Public Functions ------------------------------------------------------------- */

bool foc_cal_workflow_identity_matches(
    const struct foc_cal_workflow_identity *left,
    const struct foc_cal_workflow_identity *right
)
{
    return left != NULL && right != NULL && left->motor_setup_id == right->motor_setup_id &&
        left->encoder_id == right->encoder_id &&
        left->mechanical_frame_id == right->mechanical_frame_id &&
        foc_calibration_identity_matches(&left->acquisition, &right->acquisition);
}

int foc_cal_workflow_init(struct foc_cal_workflow *workflow)
{
    if (workflow == NULL) {
        return FOC_CAL_WORKFLOW_ERR_INPUT;
    }
    if (workflow->result.state != FOC_CAL_WORKFLOW_UNINITIALIZED) {
        return FOC_CAL_WORKFLOW_ERR_STATE;
    }
    memset(workflow, 0, sizeof(*workflow));
    workflow->result.state = FOC_CAL_WORKFLOW_IDLE;
    workflow->result.active = FOC_CAL_WORKFLOW_STAGE_COUNT;
    return FOC_CAL_WORKFLOW_OK;
}

int foc_cal_workflow_start(
    struct foc_cal_workflow *workflow,
    const struct foc_cal_workflow_request *request
)
{
    if (workflow == NULL || request == NULL || request->session == 0U ||
        !identity_valid(&request->identity)) {
        return FOC_CAL_WORKFLOW_ERR_INPUT;
    }
    if (workflow->result.state == FOC_CAL_WORKFLOW_UNINITIALIZED || live(workflow)) {
        return FOC_CAL_WORKFLOW_ERR_STATE;
    }
    if (workflow->result.request.session != 0U &&
        !newer(request->session, workflow->result.request.session)) {
        return FOC_CAL_WORKFLOW_ERR_ORDER;
    }
    const uint32_t stages =
        request->profile == FOC_CAL_WORKFLOW_FULL_PROFILE ? FOC_CAL_WORKFLOW_ALL : request->stages;
    if ((request->profile != FOC_CAL_WORKFLOW_FULL_PROFILE &&
         request->profile != FOC_CAL_WORKFLOW_SELECTED_STAGES) ||
        stages == 0U || (stages & ~FOC_CAL_WORKFLOW_ALL) != 0U) {
        return FOC_CAL_WORKFLOW_ERR_INPUT;
    }
    if (request->require_dual_axis &&
        ((stages & FOC_CAL_WORKFLOW_STAGE_BIT(FOC_CAL_WORKFLOW_ELECTRICAL)) == 0U ||
         !isfinite(request->max_rs_relative_difference) ||
         request->max_rs_relative_difference < 0.0f ||
         request->max_rs_relative_difference >= 1.0f)) {
        return FOC_CAL_WORKFLOW_ERR_INPUT;
    }
    for (unsigned int i = 0U; i < FOC_CAL_WORKFLOW_STAGE_COUNT; ++i) {
        const uint32_t bit = FOC_CAL_WORKFLOW_STAGE_BIT(i);
        const uint32_t required = dependencies((enum foc_cal_workflow_stage)i, stages);
        if ((stages & bit) != 0U && (stages & required) != required) {
            return FOC_CAL_WORKFLOW_ERR_DEPENDENCY;
        }
    }
    const struct foc_cal_workflow_request next = *request;
    memset(workflow, 0, sizeof(*workflow));
    workflow->result.request = next;
    workflow->result.request.stages = stages;
    workflow->result.missing = stages;
    workflow->result.state = FOC_CAL_WORKFLOW_READY;
    workflow->result.active = FOC_CAL_WORKFLOW_STAGE_COUNT;
    for (unsigned int i = 0U; i < FOC_CAL_WORKFLOW_STAGE_COUNT; ++i) {
        if ((stages & FOC_CAL_WORKFLOW_STAGE_BIT(i)) != 0U) {
            workflow->result.stages[i].quality = FOC_CAL_WORKFLOW_PENDING;
        }
    }
    return FOC_CAL_WORKFLOW_OK;
}

int foc_cal_workflow_open(
    struct foc_cal_workflow *workflow,
    const struct foc_cal_workflow_stage_request *request
)
{
    if (workflow == NULL || request == NULL || !stage_valid(request->stage) ||
        request->stage == FOC_CAL_WORKFLOW_TUNING || !source_valid(request->source) ||
        request->capture == 0U || request->max_age_us >= HALF_RANGE) {
        return FOC_CAL_WORKFLOW_ERR_INPUT;
    }
    struct foc_cal_workflow_stage_result *stage = &workflow->result.stages[request->stage];
    if (workflow->result.state != FOC_CAL_WORKFLOW_READY ||
        stage->quality != FOC_CAL_WORKFLOW_PENDING) {
        return FOC_CAL_WORKFLOW_ERR_STATE;
    }
    const uint32_t required = dependencies(request->stage, workflow->result.request.stages);
    if ((workflow->result.passed & required) != required) {
        return FOC_CAL_WORKFLOW_ERR_DEPENDENCY;
    }
    if (capture_used(&workflow->result, request->capture)) {
        return FOC_CAL_WORKFLOW_ERR_ORDER;
    }
    const bool dual_axis =
        request->stage == FOC_CAL_WORKFLOW_ELECTRICAL && workflow->result.request.require_dual_axis;
    const bool next_q = dual_axis && workflow->result.electrical.usable;
    if (dual_axis &&
        request->policy.electrical.axis != (next_q ? FOC_MOTOR_ID_Q : FOC_MOTOR_ID_D)) {
        return FOC_CAL_WORKFLOW_ERR_DEPENDENCY;
    }
    if (next_q && request->source != stage->source) {
        return FOC_CAL_WORKFLOW_ERR_SOURCE;
    }
    const int native = open_native(workflow, request);
    if (native != 0) {
        if (native == OPEN_IDENTITY_MISMATCH) {
            stage->status = FOC_CAL_WORKFLOW_ERR_IDENTITY;
            stage->failure = FOC_CALIBRATION_FAILURE_CONFIG_MISMATCH;
        } else {
            stage->status = FOC_CAL_WORKFLOW_ERR_INPUT;
            stage->failure = FOC_CALIBRATION_FAILURE_INVALID_CONFIG;
            stage->algorithm_status = native;
        }
        return stage->status;
    }
    if (next_q) {
        workflow->result.electrical_q_capture.id = request->capture;
        stage->quality = FOC_CAL_WORKFLOW_COLLECTING;
        stage->failure = FOC_CALIBRATION_FAILURE_NONE;
        stage->status = FOC_CAL_WORKFLOW_OK;
        stage->algorithm_status = FOC_MOTOR_ID_OK;
    } else {
        *stage = (struct foc_cal_workflow_stage_result){
            .quality = FOC_CAL_WORKFLOW_COLLECTING,
            .source = request->source,
            .primary.id = request->capture,
        };
    }
    workflow->result.active = request->stage;
    workflow->result.state = FOC_CAL_WORKFLOW_WAITING_FOR_DATA;
    workflow->max_age_us = request->max_age_us;
    return FOC_CAL_WORKFLOW_OK;
}

int foc_cal_workflow_push(
    struct foc_cal_workflow *workflow,
    const struct foc_cal_workflow_sample *sample,
    uint32_t now_us
)
{
    if (workflow == NULL || sample == NULL) {
        return FOC_CAL_WORKFLOW_ERR_INPUT;
    }
    const enum foc_cal_workflow_stage stage = workflow->result.active;
    if (!live(workflow) || !stage_valid(stage)) {
        return FOC_CAL_WORKFLOW_ERR_STATE;
    }
    struct foc_cal_workflow_stage_result *result = &workflow->result.stages[stage];
    const bool q_axis = electrical_q_active(workflow);
    struct foc_cal_workflow_capture *capture = q_axis
        ? &workflow->result.electrical_q_capture
        : (workflow->holdout ? &result->holdout : &result->primary);
    if (sample->session != workflow->result.request.session || sample->capture != capture->id) {
        return FOC_CAL_WORKFLOW_ERR_ORDER;
    }
    if (!foc_cal_workflow_identity_matches(&workflow->result.request.identity, &sample->identity)) {
        return reject_identity(workflow, stage);
    }

    uint32_t time = 0U, sequence = 0U;
    const enum foc_cal_workflow_units expected = sample_metadata(stage, sample, &time, &sequence);
    int status = FOC_CAL_WORKFLOW_OK;
    enum foc_calibration_failure failure = FOC_CALIBRATION_FAILURE_NONE;
    if (sample->source != result->source || sample->units != expected) {
        status = FOC_CAL_WORKFLOW_ERR_SOURCE;
        failure = FOC_CALIBRATION_FAILURE_INVALID_SAMPLE;
    } else if (
        now_us - time > workflow->max_age_us || capture->samples == UINT32_MAX ||
        (q_axis && capture->samples == 0U && !newer(time, result->primary.last_us)) ||
        (capture->samples != 0U &&
         (!newer(time, capture->last_us) || !newer(sequence, capture->last_sequence)))
    ) {
        status = FOC_CAL_WORKFLOW_ERR_ORDER;
        failure = FOC_CALIBRATION_FAILURE_TIME_ORDER;
    } else if (
        !sample->valid || sample->saturated ||
        ((stage == FOC_CAL_WORKFLOW_MECHANICAL ||
          (stage == FOC_CAL_WORKFLOW_ELECTRICAL && capture->samples != 0U)) &&
         !sample->output_confirmed)
    ) {
        status = FOC_CAL_WORKFLOW_ERR_QUALITY;
        failure = FOC_CALIBRATION_FAILURE_PRECONDITION;
    }
    if (status != FOC_CAL_WORKFLOW_OK) {
        conclude_stage(workflow, stage, quality_for(failure), failure, status, 0);
        return status;
    }

    struct foc_calibration_angle_sample measured;
    const struct foc_calibration_angle_sample *angle = NULL;
    if (stage == FOC_CAL_WORKFLOW_ROTOR || stage == FOC_CAL_WORKFLOW_OFFSET) {
        angle = &sample->data.angle;
        if (uses_frozen_lut(workflow)) {
            struct foc_encoder_feedback feedback;
            const int observed = sample->encoder_raw_valid ? foc_encoder_update(
                                                                 &workflow->angle_encoder,
                                                                 time,
                                                                 sample->encoder_raw_code,
                                                                 FOC_ENCODER_TRANSPORT_OK,
                                                                 &feedback
                                                             )
                                                           : FOC_ENCODER_STATUS_ERR_SAMPLE;
            if (observed != FOC_ENCODER_STATUS_OK) {
                conclude_stage(
                    workflow,
                    stage,
                    FOC_CAL_WORKFLOW_INVALID_OBSERVATION,
                    FOC_CALIBRATION_FAILURE_INVALID_SAMPLE,
                    FOC_CAL_WORKFLOW_ERR_QUALITY,
                    observed
                );
                return FOC_CAL_WORKFLOW_ERR_QUALITY;
            }
            measured = sample->data.angle;
            measured.mechanical_rad = feedback.mechanical_rad;
            angle = &measured;
        }
    }
    const int native = push_native(workflow, sample, angle);
    if (native != 0) {
        failure = native_failure(workflow);
        conclude_stage(
            workflow,
            stage,
            quality_for(failure),
            failure,
            FOC_CAL_WORKFLOW_ERR_QUALITY,
            native
        );
        return FOC_CAL_WORKFLOW_ERR_QUALITY;
    }
    if (capture->samples == 0U) {
        capture->first_us = time;
    }
    capture->last_us = time;
    capture->last_sequence = sequence;
    ++capture->samples;
    workflow->result.state = FOC_CAL_WORKFLOW_RUNNING;
    return FOC_CAL_WORKFLOW_OK;
}

int foc_cal_workflow_begin_holdout(struct foc_cal_workflow *workflow, uint32_t capture)
{
    if (workflow == NULL || capture == 0U) {
        return FOC_CAL_WORKFLOW_ERR_INPUT;
    }
    if (!live(workflow) || workflow->result.active != FOC_CAL_WORKFLOW_LUT || workflow->holdout) {
        return FOC_CAL_WORKFLOW_ERR_STATE;
    }
    struct foc_cal_workflow_stage_result *result = &workflow->result.stages[FOC_CAL_WORKFLOW_LUT];
    if (capture_used(&workflow->result, capture)) {
        return FOC_CAL_WORKFLOW_ERR_ORDER;
    }
    int status = foc_encoder_cal_freeze(&workflow->scratch.lut);
    if (status == FOC_ENCODER_CAL_OK) {
        status = foc_encoder_cal_begin_holdout(&workflow->scratch.lut, capture);
    }
    if (status != FOC_ENCODER_CAL_OK) {
        const enum foc_calibration_failure failure = native_failure(workflow);
        workflow->result.lut = workflow->scratch.lut.result;
        conclude_stage(
            workflow,
            FOC_CAL_WORKFLOW_LUT,
            quality_for(failure),
            failure,
            FOC_CAL_WORKFLOW_ERR_QUALITY,
            status
        );
        return FOC_CAL_WORKFLOW_ERR_QUALITY;
    }
    result->holdout.id = capture;
    workflow->holdout = true;
    workflow->result.state = FOC_CAL_WORKFLOW_WAITING_FOR_DATA;
    return FOC_CAL_WORKFLOW_OK;
}

int foc_cal_workflow_finish_stage(struct foc_cal_workflow *workflow)
{
    if (workflow == NULL) {
        return FOC_CAL_WORKFLOW_ERR_INPUT;
    }
    const enum foc_cal_workflow_stage stage = workflow->result.active;
    if (!live(workflow) || !stage_valid(stage)) {
        return FOC_CAL_WORKFLOW_ERR_STATE;
    }
    if (stage == FOC_CAL_WORKFLOW_LUT && !workflow->holdout) {
        return FOC_CAL_WORKFLOW_ERR_DEPENDENCY;
    }
    const int status = finish_native(workflow);
    if (status != 0) {
        const enum foc_calibration_failure failure = native_failure(workflow);
        conclude_stage(
            workflow,
            stage,
            quality_for(failure),
            failure,
            FOC_CAL_WORKFLOW_ERR_QUALITY,
            status
        );
        return FOC_CAL_WORKFLOW_ERR_QUALITY;
    }
    struct foc_cal_workflow_bundle *bundle = &workflow->result;
    if (stage == FOC_CAL_WORKFLOW_ELECTRICAL && bundle->request.require_dual_axis) {
        if (!electrical_q_active(workflow)) {
            conclude_stage(
                workflow,
                stage,
                FOC_CAL_WORKFLOW_PENDING,
                FOC_CALIBRATION_FAILURE_NONE,
                FOC_CAL_WORKFLOW_PARTIAL,
                status
            );
            return FOC_CAL_WORKFLOW_PARTIAL;
        }
        const int comparison = foc_motor_id_make_model(
            &bundle->electrical,
            &bundle->electrical_q,
            bundle->request.max_rs_relative_difference,
            &bundle->electrical_model
        );
        if (comparison != FOC_MOTOR_ID_OK) {
            conclude_stage(
                workflow,
                stage,
                FOC_CAL_WORKFLOW_REJECTED_BY_QUALITY,
                FOC_CALIBRATION_FAILURE_RESIDUAL,
                FOC_CAL_WORKFLOW_ERR_QUALITY,
                comparison
            );
            return FOC_CAL_WORKFLOW_ERR_QUALITY;
        }
    }
    conclude_stage(
        workflow,
        stage,
        FOC_CAL_WORKFLOW_PASS,
        FOC_CALIBRATION_FAILURE_NONE,
        FOC_CAL_WORKFLOW_OK,
        status
    );
    return FOC_CAL_WORKFLOW_OK;
}

int foc_cal_workflow_data_required(
    struct foc_cal_workflow *workflow,
    enum foc_cal_workflow_stage stage
)
{
    if (workflow == NULL || !stage_valid(stage)) {
        return FOC_CAL_WORKFLOW_ERR_INPUT;
    }
    const enum foc_cal_workflow_quality quality = workflow->result.stages[stage].quality;
    if (!live(workflow) ||
        (workflow->result.active != FOC_CAL_WORKFLOW_STAGE_COUNT &&
         workflow->result.active != stage) ||
        (quality != FOC_CAL_WORKFLOW_COLLECTING && quality != FOC_CAL_WORKFLOW_PENDING)) {
        return FOC_CAL_WORKFLOW_ERR_STATE;
    }
    conclude_stage(
        workflow,
        stage,
        FOC_CAL_WORKFLOW_DATA_REQUIRED,
        FOC_CALIBRATION_FAILURE_SAMPLE_COUNT,
        FOC_CAL_WORKFLOW_ERR_DATA_REQUIRED,
        0
    );
    return FOC_CAL_WORKFLOW_OK;
}

int foc_cal_workflow_tune(
    struct foc_cal_workflow *workflow,
    const struct foc_cal_workflow_kt *kt,
    const struct foc_impedance_tuning_input *request
)
{
    if (workflow == NULL || request == NULL) {
        return FOC_CAL_WORKFLOW_ERR_INPUT;
    }
    struct foc_cal_workflow_bundle *bundle = &workflow->result;
    struct foc_cal_workflow_stage_result *stage = &bundle->stages[FOC_CAL_WORKFLOW_TUNING];
    if (bundle->state != FOC_CAL_WORKFLOW_READY ||
        (stage->quality != FOC_CAL_WORKFLOW_PENDING &&
         stage->quality != FOC_CAL_WORKFLOW_DATA_REQUIRED)) {
        return FOC_CAL_WORKFLOW_ERR_STATE;
    }
    const uint32_t required = dependencies(FOC_CAL_WORKFLOW_TUNING, bundle->request.stages);
    if ((bundle->passed & required) != required || !bundle->mechanical.usable) {
        return FOC_CAL_WORKFLOW_ERR_DEPENDENCY;
    }
    if (kt == NULL || !kt->qualified || !isfinite(kt->nm_per_a) || kt->nm_per_a <= 0.0f ||
        !source_valid(kt->source) || kt->capture == 0U) {
        conclude_stage(
            workflow,
            FOC_CAL_WORKFLOW_TUNING,
            FOC_CAL_WORKFLOW_DATA_REQUIRED,
            FOC_CALIBRATION_FAILURE_PRECONDITION,
            FOC_CAL_WORKFLOW_ERR_DATA_REQUIRED,
            0
        );
        return FOC_CAL_WORKFLOW_ERR_DATA_REQUIRED;
    }
    if (!foc_cal_workflow_identity_matches(&bundle->request.identity, &kt->identity) ||
        kt->torque_model_id == 0U ||
        (bundle->mechanical.torque_source != FOC_MECHANICAL_ID_TORQUE_EQUIVALENT &&
         (kt->torque_model_id != bundle->mechanical.torque_model_id ||
          kt->nm_per_a != bundle->mechanical.torque_constant_nm_per_a))) {
        return reject_identity(workflow, FOC_CAL_WORKFLOW_TUNING);
    }
    if (capture_used(bundle, kt->capture)) {
        return FOC_CAL_WORKFLOW_ERR_ORDER;
    }
    struct foc_impedance_tuning_input input = *request;
    input.model_id = bundle->mechanical.torque_model_id;
    input.inertia_kg_m2 = bundle->mechanical.inertia_kg_m2;
    input.viscous_damping_nm_s_per_rad = bundle->mechanical.damping_nm_s;
    input.damping_source = FOC_IMPEDANCE_TUNING_DAMPING_PROVIDED;
    input.torque_constant_nm_per_a = kt->nm_per_a;
    stage->source = bundle->stages[FOC_CAL_WORKFLOW_MECHANICAL].source;
    stage->primary = bundle->stages[FOC_CAL_WORKFLOW_MECHANICAL].primary;
    const int status = foc_impedance_tuning_design(&input, &bundle->tuning);
    if (status != FOC_IMPEDANCE_TUNING_OK || !bundle->tuning.valid) {
        conclude_stage(
            workflow,
            FOC_CAL_WORKFLOW_TUNING,
            FOC_CAL_WORKFLOW_REJECTED_BY_QUALITY,
            FOC_CALIBRATION_FAILURE_RANGE,
            FOC_CAL_WORKFLOW_ERR_QUALITY,
            status
        );
        return FOC_CAL_WORKFLOW_ERR_QUALITY;
    }
    bundle->kt = *kt;
    conclude_stage(
        workflow,
        FOC_CAL_WORKFLOW_TUNING,
        FOC_CAL_WORKFLOW_PASS,
        FOC_CALIBRATION_FAILURE_NONE,
        FOC_CAL_WORKFLOW_OK,
        status
    );
    return FOC_CAL_WORKFLOW_OK;
}

int foc_cal_workflow_finish(struct foc_cal_workflow *workflow)
{
    if (workflow == NULL) {
        return FOC_CAL_WORKFLOW_ERR_INPUT;
    }
    struct foc_cal_workflow_bundle *bundle = &workflow->result;
    if (bundle->state != FOC_CAL_WORKFLOW_READY) {
        return FOC_CAL_WORKFLOW_ERR_STATE;
    }
    if (bundle->missing != 0U) {
        for (unsigned int i = 0U; i < FOC_CAL_WORKFLOW_STAGE_COUNT; ++i) {
            if (bundle->stages[i].quality == FOC_CAL_WORKFLOW_PENDING) {
                const uint32_t required =
                    dependencies((enum foc_cal_workflow_stage)i, bundle->request.stages);
                bundle->stages[i].quality = FOC_CAL_WORKFLOW_DATA_REQUIRED;
                bundle->stages[i].status = (bundle->passed & required) == required
                    ? FOC_CAL_WORKFLOW_ERR_DATA_REQUIRED
                    : FOC_CAL_WORKFLOW_ERR_DEPENDENCY;
            }
        }
        bundle->state = FOC_CAL_WORKFLOW_REJECTED;
        return FOC_CAL_WORKFLOW_PARTIAL;
    }
    bundle->complete = true;
    bundle->state = FOC_CAL_WORKFLOW_COMPLETED;
    if ((bundle->passed & FOC_CAL_WORKFLOW_ADMISSION_MASK) == FOC_CAL_WORKFLOW_ADMISSION_MASK) {
        const struct foc_calibration_admission_input input = {
            .adc = &bundle->adc,
            .rotor = &bundle->rotor,
            .offset = &bundle->offset,
            .expected_identity = &bundle->request.identity.acquisition,
            .limits = bundle->request.admission,
        };
        if (foc_calibration_admit(&input, &bundle->snapshot, &bundle->admission_failure) !=
            FOC_CALIBRATION_ADMISSION_OK) {
            bundle->state = FOC_CAL_WORKFLOW_REJECTED;
            return FOC_CAL_WORKFLOW_ERR_QUALITY;
        }
        if ((bundle->passed & FOC_CAL_WORKFLOW_STAGE_BIT(FOC_CAL_WORKFLOW_LUT)) != 0U &&
            bundle->lut.setup.direction != bundle->snapshot.runtime_encoder_direction) {
            bundle->snapshot.accepted = false;
            bundle->admission_failure = FOC_CALIBRATION_ADMISSION_UNSUPPORTED_MAPPING;
            bundle->state = FOC_CAL_WORKFLOW_REJECTED;
            return FOC_CAL_WORKFLOW_ERR_IDENTITY;
        }
        bundle->applicable = true;
    }
    return FOC_CAL_WORKFLOW_OK;
}

int foc_cal_workflow_review(
    struct foc_cal_workflow *workflow,
    uint32_t session,
    const struct foc_cal_workflow_identity *identity,
    bool accepted
)
{
    if (workflow == NULL || identity == NULL) {
        return FOC_CAL_WORKFLOW_ERR_INPUT;
    }
    struct foc_cal_workflow_bundle *bundle = &workflow->result;
    if (bundle->state != FOC_CAL_WORKFLOW_COMPLETED || !bundle->complete) {
        return FOC_CAL_WORKFLOW_ERR_REVIEW;
    }
    if (session != bundle->request.session) {
        return FOC_CAL_WORKFLOW_ERR_ORDER;
    }
    if (!foc_cal_workflow_identity_matches(&bundle->request.identity, identity)) {
        return FOC_CAL_WORKFLOW_ERR_IDENTITY;
    }
    bundle->reviewed = accepted;
    return FOC_CAL_WORKFLOW_OK;
}

int foc_cal_workflow_cancel(struct foc_cal_workflow *workflow)
{
    if (workflow == NULL) {
        return FOC_CAL_WORKFLOW_ERR_INPUT;
    }
    struct foc_cal_workflow_bundle *bundle = &workflow->result;
    if (!live(workflow) && bundle->state != FOC_CAL_WORKFLOW_COMPLETED) {
        return FOC_CAL_WORKFLOW_ERR_STATE;
    }
    for (unsigned int i = 0U; i < FOC_CAL_WORKFLOW_STAGE_COUNT; ++i) {
        if (bundle->stages[i].quality == FOC_CAL_WORKFLOW_PENDING ||
            bundle->stages[i].quality == FOC_CAL_WORKFLOW_COLLECTING) {
            bundle->stages[i].quality = FOC_CAL_WORKFLOW_STAGE_CANCELLED;
        }
    }
    revoke_candidates(bundle);
    bundle->active = FOC_CAL_WORKFLOW_STAGE_COUNT;
    bundle->state = FOC_CAL_WORKFLOW_CANCELLED;
    workflow->holdout = false;
    return FOC_CAL_WORKFLOW_OK;
}

const struct foc_cal_workflow_bundle *foc_cal_workflow_result(
    const struct foc_cal_workflow *workflow
)
{
    return workflow != NULL && workflow->result.state != FOC_CAL_WORKFLOW_UNINITIALIZED
        ? &workflow->result
        : NULL;
}

/* ---------------------------------------- end of file ----------------------------------------- */
