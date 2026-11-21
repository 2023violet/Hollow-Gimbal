/**
 * @file    falconfoc_virtual_calibrate_reuse.c
 * @brief   Hold reviewed measurement history and compose new Host calibration.
 * @version v0.1.0
 * @date    2026.11.20
 *
 * Implementation role
 * -------------------
 * The serialized command loop projects qualified measurements into the normal
 * workflow, reports required captures, and publishes only a verified fresh
 * Runtime. Provenance history lives for the lifetime of this Host process.
 */

/* Includes --------------------------------------------------------------------- */

#include "falconfoc_virtual_calibrate_internal.h"
#include "foc_virtual_calibration_package.h"
#include "foc_virtual_calibration_flux.h"
#include "foc_virtual_calibration_reference.h"
#include "foc_virtual_calibration_reuse.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/* Private Types ---------------------------------------------------------------- */

struct reuse_runtime {
    struct foc_virtual_current driver;
    struct calibrated_init init;
};

struct reuse_session {
    const char *bootstrap;
    struct foc_virtual_cal_cli_timing *timing;
    struct foc_virtual_calibration measurement;
    struct foc_cal_workflow_bundle qualified;
    struct foc_virtual_cal_flux_result qualified_flux;
    struct foc_flux_id_result flux_evidence;
    struct foc_flux_id_result qualified_flux_evidence;
    struct foc_virtual_current_profile qualified_profile;
    struct foc_virtual_current_profile target;
    struct foc_cal_workflow_request request;
    struct foc_cal_reuse_history history;
    struct foc_cal_reuse_history proposed;
    struct foc_cal_reuse_setup target_setup;
    struct foc_cal_application_candidate candidate;
    struct foc_cal_application_tuning_policy policy;
    struct foc_virtual_cogging_result cogging;
    struct foc_virtual_cogging_result qualified_cogging;
    struct foc_runtime_snapshot reviewed_original;
    struct reuse_runtime runtime[2];
    uint32_t retained;
    uint32_t revision;
    uint32_t review_id;
    uint32_t numerical_id;
    uint32_t flux_signature;
    uint32_t boot_signature;
    uint32_t qualified_boot_signature;
    unsigned int active;
    bool historic;
    bool pending;
    bool authorized;
    bool use_cogging;
    bool reference_required;
    bool revoked;
};

/* Private Helpers -------------------------------------------------------------- */

#define REUSE_BIT(parameter) (UINT32_C(1) << (parameter))

static uint32_t
stage_capture(const struct foc_cal_workflow_bundle *bundle, enum foc_cal_workflow_stage stage)
{
    return bundle->stages[stage].primary.id;
}

static struct foc_cal_reuse_setup reuse_initial_setup(
    const struct foc_cal_workflow_identity *identity
)
{
    struct foc_cal_reuse_setup setup = {.temperature_c = 25.0f};
    for (unsigned int i = 0; i < FOC_CAL_REUSE_ID_COUNT; ++i) {
        setup.id[i] = 1U;
    }
    setup.id[FOC_CAL_REUSE_ID_MOTOR] = identity->motor_setup_id;
    setup.id[FOC_CAL_REUSE_ID_ENCODER] = identity->encoder_id;
    setup.id[FOC_CAL_REUSE_ID_PHASE] = (uint32_t)identity->acquisition.phase_order + 1U;
    setup.id[FOC_CAL_REUSE_ID_MECH_FRAME] = identity->mechanical_frame_id;
    return setup;
}

static bool disabled_runtime(struct reuse_session *reuse, struct foc_runtime_snapshot *snapshot)
{
    if (!reuse->historic) {
        return true;
    }
    struct foc_virtual_current *driver = &reuse->runtime[reuse->active].driver;
    struct foc_runtime_lifecycle_result lifecycle;
    return foc_runtime_read_lifecycle_result(&driver->runtime, &lifecycle) == FOC_RUNTIME_OK &&
        lifecycle.result != FOC_RUNTIME_REQUEST_PENDING &&
        foc_virtual_current_snapshot(driver, snapshot) == FOC_RUNTIME_OK &&
        snapshot->state == FOC_SAFE_STATE_DISABLED && snapshot->faults == 0U &&
        snapshot->output_confirmed && snapshot->action == FOC_DEVICE_OUTPUT_INHIBIT &&
        !snapshot->inhibit_pending && snapshot->clear_result != FOC_RUNTIME_CLEAR_PENDING;
}

static bool matches_reviewed_original(struct reuse_session *reuse)
{
    struct foc_runtime_snapshot snapshot = {0};
    return disabled_runtime(reuse, &snapshot) && snapshot.mode == reuse->reviewed_original.mode &&
        snapshot.motion_session == reuse->reviewed_original.motion_session &&
        snapshot.updates == reuse->reviewed_original.updates;
}

static void use_qualified_stages(struct reuse_session *reuse, uint32_t mask)
{
    struct foc_virtual_calibration *session = &reuse->measurement;
    struct foc_cal_workflow_bundle *bundle = &session->workflow.result;
    const struct foc_cal_workflow_bundle *old = &reuse->qualified;
    const struct foc_calibration_identity identity = bundle->request.identity.acquisition;
    const struct {
        enum foc_cal_workflow_stage stage;
        uint32_t required;
        uint32_t timed;
    } groups[] = {
        {FOC_CAL_WORKFLOW_ADC, REUSE_BIT(FOC_CAL_REUSE_ADC), CAL_CLI_BIT(CAL_CLI_ADC)},
        {FOC_CAL_WORKFLOW_ELECTRICAL, REUSE_BIT(FOC_CAL_REUSE_RS) | REUSE_BIT(FOC_CAL_REUSE_RL),
         CAL_CLI_BIT(CAL_CLI_ELECTRICAL_D) | CAL_CLI_BIT(CAL_CLI_ELECTRICAL_Q)},
        {FOC_CAL_WORKFLOW_ROTOR, REUSE_BIT(FOC_CAL_REUSE_POLES) | REUSE_BIT(FOC_CAL_REUSE_OFFSET),
         CAL_CLI_BIT(CAL_CLI_ROTOR)},
        {FOC_CAL_WORKFLOW_OFFSET, REUSE_BIT(FOC_CAL_REUSE_OFFSET), CAL_CLI_BIT(CAL_CLI_OFFSET)},
        {FOC_CAL_WORKFLOW_LUT, REUSE_BIT(FOC_CAL_REUSE_LUT), CAL_CLI_BIT(CAL_CLI_LUT)},
        {FOC_CAL_WORKFLOW_MECHANICAL, REUSE_BIT(FOC_CAL_REUSE_JB), CAL_CLI_BIT(CAL_CLI_MECHANICAL)},
        {FOC_CAL_WORKFLOW_TUNING, REUSE_BIT(FOC_CAL_REUSE_SERVO), CAL_CLI_BIT(CAL_CLI_TUNE)},
    };
    for (size_t i = 0; i < sizeof groups / sizeof groups[0]; ++i) {
        if ((mask & groups[i].required) != groups[i].required) {
            continue;
        }
        const enum foc_cal_workflow_stage stage = groups[i].stage;
        foc_virtual_cal_cli_timing_omit(reuse->timing, groups[i].timed, CAL_CLI_REUSED);
        const uint32_t bit = FOC_CAL_WORKFLOW_STAGE_BIT(stage);
        bundle->stages[stage] = old->stages[stage];
        bundle->passed |= bit;
        bundle->missing &= ~bit;
        switch (stage) {
        case FOC_CAL_WORKFLOW_ADC:
            bundle->adc = old->adc;
            bundle->adc.identity = identity;
            break;
        case FOC_CAL_WORKFLOW_ELECTRICAL:
            bundle->electrical = old->electrical;
            bundle->electrical_q = old->electrical_q;
            bundle->electrical_q_capture = old->electrical_q_capture;
            bundle->electrical_model = old->electrical_model;
            if (session->time_us < old->electrical_q_capture.last_us) {
                session->time_us = old->electrical_q_capture.last_us;
            }
            break;
        case FOC_CAL_WORKFLOW_ROTOR:
            bundle->rotor = old->rotor;
            bundle->rotor.identity = identity;
            bundle->rotor.identity.encoder_direction = old->rotor.identity.encoder_direction;
            if (session->time_us < old->stages[stage].primary.last_us) {
                session->time_us = old->stages[stage].primary.last_us;
            }
            break;
        case FOC_CAL_WORKFLOW_OFFSET:
            bundle->offset = old->offset;
            bundle->offset.identity = identity;
            bundle->offset.identity.encoder_direction = old->offset.identity.encoder_direction;
            break;
        case FOC_CAL_WORKFLOW_LUT:
            bundle->lut = old->lut;
            session->workflow.lut_encoder = session->profile.encoder;
            break;
        case FOC_CAL_WORKFLOW_MECHANICAL:
            bundle->mechanical = old->mechanical;
            break;
        case FOC_CAL_WORKFLOW_TUNING:
            bundle->tuning = old->tuning;
            bundle->kt = old->kt;
            bundle->kt.identity = bundle->request.identity;
            break;
        default:
            break;
        }
    }
    if ((mask & REUSE_BIT(FOC_CAL_REUSE_FLUX)) != 0U) {
        foc_virtual_cal_cli_timing_omit(reuse->timing, CAL_CLI_BIT(CAL_CLI_FLUX), CAL_CLI_REUSED);
        session->flux = reuse->qualified_flux;
        reuse->flux_evidence = reuse->qualified_flux_evidence;
        session->flux.kt.identity = bundle->request.identity;
        if (session->time_us < session->flux.last_us) {
            session->time_us = session->flux.last_us;
        }
    }
    session->electrical.decoded_adc.phase_a_offset_count = bundle->adc.offset_a_counts;
    session->electrical.decoded_adc.phase_b_offset_count = bundle->adc.offset_b_counts;
    session->electrical.decoded_adc.phase_c_offset_count = bundle->adc.offset_c_counts;
}

static bool change_reuse_target(struct reuse_session *reuse, const char *change)
{
    uint32_t *ids = reuse->target_setup.id;
    struct foc_virtual_current_profile *target = &reuse->target;
    struct foc_cal_workflow_identity *identity = &reuse->request.identity;
    reuse->reference_required = false;
    if (strcmp(change, "same") == 0) {
        return true;
    }
    if (strcmp(change, "encoder") == 0) {
        identity->encoder_id = ++ids[FOC_CAL_REUSE_ID_ENCODER];
        ++ids[FOC_CAL_REUSE_ID_ASSEMBLY];
        target->encoder_fundamental_error_rad *= 0.65;
    } else if (strcmp(change, "load") == 0) {
        ++ids[FOC_CAL_REUSE_ID_AXIS_LOAD];
        target->motor.inertia_kg_m2 *= 1.25;
        target->motor.damping_nm_s *= 1.15;
    } else if (strcmp(change, "motor") == 0) {
        const bool was_generic = identity->motor_setup_id == 201U;
        const char *name = was_generic ? "CALIBRATION_SALIENT_MOTOR" : "GENERIC_RESEARCH_MOTOR";
        struct foc_virtual_current_profile other;
        struct foc_cal_workflow_request other_request;
        if (foc_virtual_calibration_profile(name, &other, &other_request) != FOC_VIRTUAL_CAL_OK) {
            return false;
        }
        target->motor = other.motor;
        identity->motor_setup_id = other_request.identity.motor_setup_id;
        ids[FOC_CAL_REUSE_ID_MOTOR] = identity->motor_setup_id;
        ++ids[FOC_CAL_REUSE_ID_ANGLE_REFERENCE];
        ++ids[FOC_CAL_REUSE_ID_ASSEMBLY];
    } else if (strcmp(change, "adc") == 0) {
        ++ids[FOC_CAL_REUSE_ID_ADC_CHAIN];
        target->adc.phase_a_offset_count += 11.0f;
        target->adc.current_a_per_count *= 1.08f;
        identity->acquisition.current_a_per_count = target->adc.current_a_per_count;
    } else if (strcmp(change, "phase") == 0) {
        const enum foc_device_phase_order order =
            target->adc.phase_order == FOC_DEVICE_PHASE_ORDER_ABC ? FOC_DEVICE_PHASE_ORDER_ACB
                                                                  : FOC_DEVICE_PHASE_ORDER_ABC;
        ids[FOC_CAL_REUSE_ID_PHASE] = (uint32_t)order + 1U;
        target->pwm.phase_order = order;
        target->adc.phase_order = order;
        identity->acquisition.phase_order = order;
    } else if (strcmp(change, "home") == 0) {
        identity->mechanical_frame_id = ++ids[FOC_CAL_REUSE_ID_MECH_FRAME];
        target->runtime.motion.impedance.mechanical_frame_id = identity->mechanical_frame_id;
    } else if (strcmp(change, "offset") == 0) {
        ++ids[FOC_CAL_REUSE_ID_OFFSET_REFERENCE];
        target->motor.electrical_offset_rad += 0.08;
    } else if (strcmp(change, "lut") == 0) {
        ++ids[FOC_CAL_REUSE_ID_LUT_REFERENCE];
        target->encoder_third_error_rad *= 0.6;
    } else if (strcmp(change, "kt") == 0) {
        ++ids[FOC_CAL_REUSE_ID_KT_REFERENCE];
        /* Re-run the same terminal fit with a new measurement instance. */
    } else if (strcmp(change, "limits") == 0) {
        ++ids[FOC_CAL_REUSE_ID_CONTROL_LIMITS];
        target->runtime.motion.impedance.torque_limit_nm *= 0.9f;
    } else if (strcmp(change, "voltage") == 0) {
        ++ids[FOC_CAL_REUSE_ID_VOLTAGE_SOURCE];
        reuse->reference_required = true;
    } else if (strcmp(change, "angle") == 0) {
        ++ids[FOC_CAL_REUSE_ID_ANGLE_REFERENCE];
        reuse->reference_required = true;
    } else if (strcmp(change, "torque") == 0) {
        ++ids[FOC_CAL_REUSE_ID_TORQUE_SOURCE];
        reuse->reference_required = true;
    } else if (strcmp(change, "temperature") == 0) {
        reuse->target_setup.temperature_c += 10.0f;
        reuse->reference_required = true;
    } else {
        return false;
    }
    return true;
}

static void revoke_reuse_review(struct reuse_session *reuse)
{
    if (reuse->authorized) {
        foc_virtual_cal_cli_timing_omit(reuse->timing, CAL_CLI_BIT(CAL_CLI_REVIEW), CAL_CLI_FAILED);
    }
    reuse->revoked |= reuse->pending;
    reuse->pending = false;
    reuse->authorized = false;
}

static uint32_t required_parameters(const struct reuse_session *reuse)
{
    return (REUSE_BIT(FOC_CAL_REUSE_COUNT) - 1U) &
        ~(reuse->use_cogging ? 0U : REUSE_BIT(FOC_CAL_REUSE_COGGING));
}

static const char *source_name(enum foc_cal_reuse_source source)
{
    static const char *const names[] = {
        "UNKNOWN",
        "HOST_VIRTUAL",
        "EXTERNAL_RECORDED",
        "INSTRUMENT",
        "MANUFACTURER",
        "DERIVED",
    };
    return (unsigned int)source < sizeof names / sizeof names[0] ? names[source] : names[0];
}

static int plan_reuse(struct reuse_session *reuse, const char *change)
{
    if (!reuse->historic) {
        return foc_virtual_cal_cli_error("HISTORY_REQUIRED", "REUSE", -1);
    }
    reuse->target = reuse->qualified_profile;
    reuse->request = reuse->qualified.request;
    reuse->target_setup = reuse->history.setup;
    if (!change_reuse_target(reuse, change)) {
        /* Disallow measure after a failed plan until a valid target is selected. */
        revoke_reuse_review(reuse);
        reuse->reference_required = true;
        return foc_virtual_cal_cli_error("INVALID_REUSE_CHANGE", "REUSE", -1);
    }
    revoke_reuse_review(reuse);
    enum foc_cal_reuse_reason reasons[FOC_CAL_REUSE_COUNT];
    reuse->retained = foc_cal_reuse_evaluate(&reuse->history, &reuse->target_setup, reasons);
    printf(
        "{\"event\":\"REUSE_PLAN\",\"change\":\"%s\",\"retained_mask\":%" PRIu32 "}\n",
        change,
        reuse->retained
    );
    for (unsigned int i = 0; i < FOC_CAL_REUSE_COUNT; ++i) {
        const struct foc_cal_reuse_record *record = &reuse->history.records[i];
        const bool keep = (reuse->retained & REUSE_BIT(i)) != 0U;
        printf(
            "{\"event\":\"REUSE_PARAMETER\",\"parameter\":\"%s\",\"status\":\"%s\","
            "\"reason\":\"%s\",\"source\":\"%s\",\"session\":%" PRIu32 ","
            "\"capture\":%" PRIu32 ",\"revision\":%" PRIu32 "}\n",
            foc_cal_reuse_parameter_name((enum foc_cal_reuse_parameter)i),
            keep                                                    ? "REUSE"
                : i == FOC_CAL_REUSE_COGGING && !reuse->use_cogging ? "NOT_CONFIGURED"
                                                                    : "REMEASURE",
            foc_cal_reuse_reason_name(reasons[i]),
            source_name(record->source),
            record->session,
            record->capture,
            record->revision
        );
    }
    return 0;
}

static bool record_proposed(struct reuse_session *reuse)
{
    const struct foc_cal_workflow_bundle *bundle = &reuse->measurement.workflow.result;
    const uint32_t rotor = stage_capture(bundle, FOC_CAL_WORKFLOW_ROTOR);
    const uint32_t electrical = stage_capture(bundle, FOC_CAL_WORKFLOW_ELECTRICAL);
    const uint32_t mechanics = stage_capture(bundle, FOC_CAL_WORKFLOW_MECHANICAL);
    const uint32_t captures[FOC_CAL_REUSE_COUNT] = {
        [FOC_CAL_REUSE_ADC] = stage_capture(bundle, FOC_CAL_WORKFLOW_ADC),
        [FOC_CAL_REUSE_RS] = electrical,
        [FOC_CAL_REUSE_RL] = electrical,
        [FOC_CAL_REUSE_LUT] = stage_capture(bundle, FOC_CAL_WORKFLOW_LUT),
        [FOC_CAL_REUSE_POLES] = rotor,
        [FOC_CAL_REUSE_OFFSET] = stage_capture(bundle, FOC_CAL_WORKFLOW_OFFSET),
        /* Home and limits come from the authoritative Host fixture/profile. */
        [FOC_CAL_REUSE_HOME] = bundle->request.session,
        [FOC_CAL_REUSE_FLUX] = reuse->measurement.flux.capture,
        [FOC_CAL_REUSE_JB] = mechanics,
        [FOC_CAL_REUSE_CURRENT_PI] = electrical,
        [FOC_CAL_REUSE_SERVO] = mechanics,
        [FOC_CAL_REUSE_COGGING] = reuse->cogging.lut.training_capture_id,
        [FOC_CAL_REUSE_LIMITS] = bundle->request.session,
    };
    static const enum foc_cal_reuse_parameter order[] = {
        FOC_CAL_REUSE_ADC,
        FOC_CAL_REUSE_RS,
        FOC_CAL_REUSE_LUT,
        FOC_CAL_REUSE_POLES,
        FOC_CAL_REUSE_OFFSET,
        FOC_CAL_REUSE_RL,
        FOC_CAL_REUSE_HOME,
        FOC_CAL_REUSE_FLUX,
        FOC_CAL_REUSE_JB,
        FOC_CAL_REUSE_LIMITS,
        FOC_CAL_REUSE_CURRENT_PI,
        FOC_CAL_REUSE_SERVO,
        FOC_CAL_REUSE_COGGING,
    };
    reuse->proposed = (struct foc_cal_reuse_history){.setup = reuse->target_setup};
    for (size_t i = 0U; i < sizeof order / sizeof order[0]; ++i) {
        const enum foc_cal_reuse_parameter parameter = order[i];
        const uint32_t bit = REUSE_BIT(parameter);
        if ((reuse->retained & bit) != 0U) {
            reuse->proposed.records[parameter] = reuse->history.records[parameter];
            continue;
        }
        if (parameter == FOC_CAL_REUSE_COGGING && !reuse->use_cogging) {
            continue;
        }
        const struct foc_cal_reuse_record record = {
            .reviewed = true, /* Provisional copy becomes history only after actual approval. */
            .session = bundle->request.session,
            .capture = captures[parameter],
            .revision = reuse->revision,
            .source = parameter == FOC_CAL_REUSE_CURRENT_PI || parameter == FOC_CAL_REUSE_SERVO ||
                    parameter == FOC_CAL_REUSE_LIMITS || parameter == FOC_CAL_REUSE_HOME
                ? FOC_CAL_REUSE_SOURCE_DERIVED
                : FOC_CAL_REUSE_SOURCE_HOST_VIRTUAL,
            .uses_adc = parameter == FOC_CAL_REUSE_RS || parameter == FOC_CAL_REUSE_RL ||
                parameter == FOC_CAL_REUSE_FLUX,
            .uses_mapped_angle = parameter == FOC_CAL_REUSE_OFFSET ||
                parameter == FOC_CAL_REUSE_COGGING ||
                (reuse->bootstrap != NULL &&
                 (parameter == FOC_CAL_REUSE_RL || parameter == FOC_CAL_REUSE_FLUX)),
            .uses_rl = parameter == FOC_CAL_REUSE_FLUX,
        };
        if (!foc_cal_reuse_bind_record(&reuse->proposed, parameter, &record)) {
            return false;
        }
    }
    return foc_cal_reuse_evaluate(&reuse->proposed, &reuse->target_setup, NULL) ==
        required_parameters(reuse);
}

static uint32_t
proposed_revision(const struct reuse_session *reuse, enum foc_cal_reuse_parameter parameter)
{
    return (reuse->retained & REUSE_BIT(parameter)) != 0U
        ? reuse->history.records[parameter].revision
        : reuse->revision;
}

static bool reuse_flux_agrees(const struct reuse_session *reuse)
{
    const struct foc_flux_id_identity *id = &reuse->flux_evidence.identity;
    const struct foc_cal_reuse_record *origin = &reuse->proposed.records[FOC_CAL_REUSE_FLUX];
    const uint32_t *setup = reuse->target_setup.id;
    return foc_virtual_flux_agrees(&reuse->measurement, &reuse->flux_evidence) &&
        origin->session == id->session && origin->capture == id->capture &&
        origin->dependency[FOC_CAL_REUSE_RL] == id->rl_revision &&
        origin->dependency[FOC_CAL_REUSE_POLES] == id->poles_revision &&
        origin->dependency[FOC_CAL_REUSE_ADC] == id->current_chain &&
        setup[FOC_CAL_REUSE_ID_ANGLE_REFERENCE] == id->dq_frame &&
        setup[FOC_CAL_REUSE_ID_ANGLE_REFERENCE] == id->speed_chain &&
        setup[FOC_CAL_REUSE_ID_VOLTAGE_SOURCE] == id->voltage_chain;
}

static int prepare_reuse(struct reuse_session *reuse, bool capture_all)
{
    struct foc_virtual_calibration *session = &reuse->measurement;
    revoke_reuse_review(reuse);
    if (!capture_all && !reuse->historic) {
        return foc_virtual_cal_cli_error("HISTORY_REQUIRED", "REUSE", -1);
    }
    struct foc_virtual_cal_cli_timing *timing = reuse->timing;
    foc_virtual_cal_cli_timing_reset(
        timing, CAL_CLI_CAPTURE_STAGES | CAL_CLI_BOOTSTRAP_STAGES | CAL_CLI_APPLY_STAGES |
            CAL_CLI_BIT(CAL_CLI_QUIESCENT) | CAL_CLI_BIT(CAL_CLI_GAIN_CANDIDATE) |
            CAL_CLI_BIT(CAL_CLI_POST_APPLY_QUIESCENT) | CAL_CLI_BIT(CAL_CLI_REUSE_RUNTIME)
    );
    foc_virtual_cal_cli_timing_start(timing, CAL_CLI_QUIESCENT);
    const bool safe = disabled_runtime(reuse, &reuse->reviewed_original);
    foc_virtual_cal_cli_timing_finish(timing, CAL_CLI_QUIESCENT, safe, 0);
    if (!safe) {
        return foc_virtual_cal_cli_error("ORIGINAL_RUNTIME_UNSAFE", "REUSE", -1);
    }
    if (reuse->reference_required) {
        return foc_virtual_cal_cli_error("NEW_REFERENCE_REQUIRED", "REUSE", -1);
    }
    reuse->retained =
        capture_all ? 0U : foc_cal_reuse_evaluate(&reuse->history, &reuse->target_setup, NULL);
    /* The present d/q acquisition qualifies Rs and inductances as a single fit. */
    const uint32_t electrical = REUSE_BIT(FOC_CAL_REUSE_RS) | REUSE_BIT(FOC_CAL_REUSE_RL);
    if ((reuse->retained & electrical) != electrical) {
        reuse->retained &= ~electrical;
    }
    reuse->request.session = ++reuse->revision;
    if (reuse->bootstrap != NULL) {
        const uint32_t commutation =
            REUSE_BIT(FOC_CAL_REUSE_POLES) | REUSE_BIT(FOC_CAL_REUSE_OFFSET);
        reuse->boot_signature = reuse->qualified_boot_signature;
        if ((reuse->retained & commutation) != commutation) {
            const int bootstrap_status = foc_virtual_cal_cli_bootstrap_timed(
                reuse->bootstrap,
                "nominal",
                NULL,
                &reuse->target,
                &reuse->request,
                &reuse->boot_signature,
                timing
            );
            if (bootstrap_status != 0) {
                return bootstrap_status;
            }
        } else {
            foc_virtual_cal_cli_timing_omit(timing, CAL_CLI_BOOTSTRAP_STAGES, CAL_CLI_REUSED);
        }
    }
    *session = (struct foc_virtual_calibration){0};
    reuse->flux_evidence = (struct foc_flux_id_result){0};
    int result = foc_virtual_calibration_start(session, &reuse->target, &reuse->request);
    if (result != FOC_VIRTUAL_CAL_PENDING) {
        return foc_virtual_cal_cli_error("INVALID_PROFILE_OR_RUNTIME", "REUSE", result);
    }
    use_qualified_stages(reuse, reuse->retained);
    foc_virtual_calibration_schedule(session);
    const struct foc_virtual_flux_options flux_options = {
        .rl_revision = proposed_revision(reuse, FOC_CAL_REUSE_RL),
        .poles_revision = proposed_revision(reuse, FOC_CAL_REUSE_POLES),
        .current_revision = proposed_revision(reuse, FOC_CAL_REUSE_ADC),
        .voltage_chain = reuse->target_setup.id[FOC_CAL_REUSE_ID_VOLTAGE_SOURCE],
        .angle_reference = reuse->target_setup.id[FOC_CAL_REUSE_ID_ANGLE_REFERENCE],
    };
    while (session->status == FOC_VIRTUAL_CAL_PENDING) {
        const enum foc_virtual_calibration_stage stage = session->stage;
        const bool captured = stage != FOC_VIRTUAL_CAL_REVIEW;
        const enum foc_virtual_cal_cli_stage timed_stage = (enum foc_virtual_cal_cli_stage)stage;
        const uint32_t begin_us = session->time_us;
        if (captured) {
            foc_virtual_cal_cli_timing_start(timing, timed_stage);
        }
        result = reuse->bootstrap != NULL
            ? foc_virtual_calibration_poll_referenced(session, &flux_options, &reuse->flux_evidence)
            : foc_virtual_calibration_poll_observed(session, &flux_options, &reuse->flux_evidence);
        if (captured) {
            foc_virtual_cal_cli_timing_finish(timing, timed_stage, result >= 0,
                                             (uint32_t)(session->time_us - begin_us));
        }
        if (result < 0) {
            return foc_virtual_cal_cli_error(
                "CAPTURE_FAILED",
                foc_virtual_calibration_stage_name(stage),
                session->native_status
            );
        }
        if (stage != FOC_VIRTUAL_CAL_REVIEW) {
            printf(
                "{\"event\":\"REUSE_CAPTURED\",\"stage\":\"%s\"}\n",
                foc_virtual_calibration_stage_name(stage)
            );
        }
    }
    reuse->policy = foc_virtual_cal_cli_tuning;
    if (reuse->use_cogging) {
        if ((reuse->retained & REUSE_BIT(FOC_CAL_REUSE_COGGING)) != 0U) {
            reuse->cogging = reuse->qualified_cogging;
        } else {
            foc_virtual_cal_cli_cogging_fixture(&reuse->target);
            result = foc_virtual_cal_cli_cogging_capture(&reuse->target, session, &reuse->cogging);
            if (result != FOC_VIRTUAL_COGGING_OK) {
                return foc_virtual_cal_cli_error("COGGING_CAPTURE_FAILED", "REUSE", result);
            }
            puts("{\"event\":\"REUSE_CAPTURED\",\"stage\":\"COGGING\"}");
        }
        reuse->policy.cogging_lut = &reuse->cogging.lut;
        reuse->policy.cogging_iq_limit_a = 0.15f;
    }
    foc_virtual_cal_cli_timing_start(timing, CAL_CLI_GAIN_CANDIDATE);
    result = foc_virtual_cal_cli_draft(session, &reuse->policy, &reuse->candidate);
    const bool drafted = result == FOC_CAL_APPLICATION_PREPARED && record_proposed(reuse) &&
        reuse_flux_agrees(reuse) &&
        foc_virtual_calibration_package_id(session, &reuse->candidate, &reuse->numerical_id) ==
            FOC_VIRTUAL_PACKAGE_OK;
    foc_virtual_cal_cli_timing_finish(timing, CAL_CLI_GAIN_CANDIDATE, drafted, 0);
    if (!drafted) {
        return foc_virtual_cal_cli_error("COMBINATION_NOT_ADMITTED", "REUSE", result);
    }
    /* The owner only mutates setup/history by invalidating a pending revision.
     * The numerical fingerprint includes the complete FCAL payload, including the LUT. */
    reuse->flux_signature = foc_virtual_cal_cli_flux_signature(&reuse->flux_evidence);
    reuse->review_id = reuse->numerical_id ^ reuse->flux_signature ^ reuse->boot_signature ^
        (reuse->revision * UINT32_C(0x9E3779B1));
    reuse->pending = true;
    reuse->revoked = false;
    reuse->authorized = false;
    foc_virtual_cal_cli_preview(&reuse->candidate, reuse->review_id);
    foc_virtual_cal_cli_flux_preview(&reuse->flux_evidence);
    const struct foc_runtime_motion_config *motion = &reuse->candidate.runtime.config.motion;
    printf(
        "{\"event\":\"REUSE_CONTROL_CONFIGURATION\",\"motor_id\":%" PRIu32 ","
        "\"encoder_id\":%" PRIu32 ",\"mechanical_frame\":%" PRIu32 ","
        "\"pole_pairs\":%u,\"electrical_offset_rad\":%.9g,\"kt\":%.9g,"
        "\"torque_limit_nm\":%.9g,\"motion_limits_enabled\":%s,"
        "\"kt_independently_measured\":false,\"mechanical_home\":\"VIRTUAL_FIXTURE_ORIGIN\"}\n",
        reuse->request.identity.motor_setup_id,
        reuse->request.identity.encoder_id,
        reuse->request.identity.mechanical_frame_id,
        (unsigned int)motion->rotor.pole_pairs,
        (double)motion->rotor.electrical_offset_rad,
        (double)motion->impedance.torque_constant_nm_per_a,
        (double)motion->impedance.torque_limit_nm,
        motion->limits.enabled ? "true" : "false"
    );
    printf(
        "{\"event\":\"REUSE_PREVIEW\",\"review_id\":\"%08" PRIx32 "\","
        "\"numerical_id\":\"%08" PRIx32 "\",\"retained_mask\":%" PRIu32 "}\n",
        reuse->review_id,
        reuse->numerical_id,
        reuse->retained
    );
    return 0;
}

static int authorize_reuse(struct reuse_session *reuse, const char *approved)
{
    if (reuse->revoked || (reuse->pending && !matches_reviewed_original(reuse))) {
        return foc_virtual_cal_cli_error("REVIEW_BINDING_REVOKED", "REUSE", -1);
    }
    if (!reuse->pending) {
        return foc_virtual_cal_cli_error("REVIEW_REQUIRED", "REUSE", -1);
    }
    if (!foc_virtual_cal_cli_review_matches(approved, reuse->review_id)) {
        return foc_virtual_cal_cli_error("REVIEW_ID_MISMATCH", "REUSE", -1);
    }
    reuse->authorized = true;
    printf("{\"event\":\"REUSE_REVIEWED\",\"review_id\":\"%08" PRIx32 "\"}\n", reuse->review_id);
    return 0;
}

static int apply_reuse(struct reuse_session *reuse)
{
    if (reuse->revoked || (reuse->pending && !matches_reviewed_original(reuse))) {
        return foc_virtual_cal_cli_error("REVIEW_BINDING_REVOKED", "APPLICATION", -1);
    }
    if (!reuse->pending || !reuse->authorized) {
        return foc_virtual_cal_cli_error("REVIEW_REQUIRED", "APPLICATION", -1);
    }
    struct foc_virtual_calibration *session = &reuse->measurement;
    const struct foc_cal_workflow_request *request = &session->workflow.result.request;
    struct foc_cal_application_candidate exact;
    uint32_t fingerprint;
    const uint32_t still_valid =
        foc_cal_reuse_evaluate(&reuse->history, &reuse->target_setup, NULL);
    bool revisions_match = true;
    for (unsigned int i = 0U; i < FOC_CAL_REUSE_COUNT; ++i) {
        if ((reuse->retained & REUSE_BIT(i)) != 0U) {
            const struct foc_cal_reuse_record *approved = &reuse->proposed.records[i];
            const struct foc_cal_reuse_record *current = &reuse->history.records[i];
            revisions_match = revisions_match && current->revision == approved->revision &&
                current->session == approved->session && current->capture == approved->capture;
        }
    }
    if (!revisions_match || (still_valid & reuse->retained) != reuse->retained ||
        foc_virtual_calibration_poll(session) != FOC_VIRTUAL_CAL_OK || !reuse_flux_agrees(reuse) ||
        foc_virtual_cal_cli_flux_signature(&reuse->flux_evidence) != reuse->flux_signature ||
        foc_virtual_cal_cli_draft(session, &reuse->policy, &exact) !=
            FOC_CAL_APPLICATION_PREPARED ||
        foc_virtual_calibration_package_id(session, &exact, &fingerprint) !=
            FOC_VIRTUAL_PACKAGE_OK ||
        fingerprint != reuse->numerical_id ||
        foc_cal_workflow_review(&session->workflow, request->session, &request->identity, true) !=
            FOC_CAL_WORKFLOW_OK) {
        revoke_reuse_review(reuse);
        return foc_virtual_cal_cli_error("REVIEW_BINDING_REVOKED", "APPLICATION", -1);
    }
    const unsigned int next = 1U - reuse->active;
    struct reuse_runtime *fresh = &reuse->runtime[next];
    *fresh = (struct reuse_runtime){0};
    fresh->init = (struct calibrated_init){
        .application = &session->application,
        .policy = &reuse->policy,
        .candidate = &exact,
    };
    const struct foc_encoder_cal_result *lut = &session->workflow.result.lut;
    foc_virtual_cal_cli_timing_start(reuse->timing, CAL_CLI_REUSE_RUNTIME);
    if (foc_virtual_current_init_with(
            &fresh->driver,
            &reuse->target,
            foc_virtual_cal_cli_initialize,
            &fresh->init
        ) != 0 ||
        !foc_virtual_cal_cli_readback(&fresh->driver, &exact, false) ||
        fresh->driver.encoder.lut_length != lut->nodes ||
        memcmp(
            fresh->driver.encoder.lut_offsets,
            lut->offsets,
            lut->nodes * sizeof lut->offsets[0]
        ) != 0) {
        revoke_reuse_review(reuse);
        foc_virtual_cal_cli_timing_finish(reuse->timing, CAL_CLI_REUSE_RUNTIME, false, 0);
        return foc_virtual_cal_cli_error("FRESH_RUNTIME_INIT_FAILED", "APPLICATION", -1);
    }
    foc_virtual_cal_cli_timing_finish(reuse->timing, CAL_CLI_REUSE_RUNTIME, true, 0);
    struct calibrated_init trials = {
        .application = &session->application,
        .policy = &reuse->policy,
        .candidate = &exact,
    };
    struct foc_virtual_current_profile trial_profile = reuse->target;
    const int result =
        foc_virtual_cal_cli_verify(&trial_profile, &trials, session, NULL, NULL, false, reuse->timing);
    foc_virtual_cal_cli_timing_start(reuse->timing, CAL_CLI_POST_APPLY_QUIESCENT);
    const bool original_matches = result == 0 && matches_reviewed_original(reuse);
    foc_virtual_cal_cli_timing_finish(reuse->timing, CAL_CLI_POST_APPLY_QUIESCENT, original_matches, 0);
    if (!original_matches) {
        revoke_reuse_review(reuse);
        return result != 0 ? result
                           : foc_virtual_cal_cli_error("REVIEW_BINDING_REVOKED", "APPLICATION", -1);
    }
    reuse->candidate = exact;
    fresh->init.candidate = &reuse->candidate;
    reuse->active = next;
    reuse->qualified = session->workflow.result;
    reuse->qualified_flux = session->flux;
    reuse->qualified_flux_evidence = reuse->flux_evidence;
    reuse->qualified_boot_signature = reuse->boot_signature;
    reuse->qualified_profile = reuse->target;
    reuse->qualified_cogging = reuse->cogging;
    reuse->history = reuse->proposed;
    reuse->historic = true;
    reuse->pending = false;
    reuse->authorized = false;
    reuse->revoked = false;
    printf(
        "{\"event\":\"REUSE_APPLIED\",\"session\":%" PRIu32 ",\"retained_mask\":%" PRIu32
        ",\"fresh_runtime\":true,\"physical_applied\":false}\n",
        request->session,
        reuse->retained
    );
    return 0;
}

static int exercise_original(struct reuse_session *reuse, const char *action)
{
    if (!reuse->historic) {
        return foc_virtual_cal_cli_error("HISTORY_REQUIRED", "REUSE", -1);
    }
    revoke_reuse_review(reuse);
    struct foc_virtual_current *driver = &reuse->runtime[reuse->active].driver;
    const uint32_t until = driver->virtual_time_us + 2000U;
    if (strcmp(action, "fault") == 0) {
        foc_runtime_request_inhibit(&driver->runtime, FOC_SAFE_FAULT_COMMAND_TIMEOUT);
    } else if (strcmp(action, "recover") == 0) {
        if (foc_runtime_request_timeout_clear(&driver->runtime, driver->virtual_time_us) !=
            FOC_RUNTIME_OK) {
            return foc_virtual_cal_cli_error("SAFE_RECOVERY_FAILED", "REUSE", -1);
        }
    } else if (foc_virtual_current_request_stop(driver) != FOC_RUNTIME_OK) {
        return foc_virtual_cal_cli_error("STOP_FAILED", "REUSE", -1);
    }
    struct foc_runtime_snapshot snapshot;
    if (foc_virtual_current_run_until(driver, until) != 0 ||
        foc_virtual_current_snapshot(driver, &snapshot) != FOC_RUNTIME_OK ||
        !snapshot.output_confirmed || snapshot.action != FOC_DEVICE_OUTPUT_INHIBIT ||
        snapshot.state !=
            (strcmp(action, "fault") == 0 ? FOC_SAFE_STATE_FAULT_LATCHED
                                          : FOC_SAFE_STATE_DISABLED)) {
        return foc_virtual_cal_cli_error("ORIGINAL_RUNTIME_UNSAFE", "REUSE", -1);
    }
    printf(
        "{\"event\":\"REUSE_ORIGINAL_INHIBITED\",\"action\":\"%s\",\"faults\":%" PRIu32 "}\n",
        action,
        snapshot.faults
    );
    return 0;
}

static int reuse_command(struct reuse_session *reuse, const char *command, char *argument)
{
    if (strcmp(command, "plan") == 0) {
        foc_virtual_cal_cli_timing_reset(reuse->timing, CAL_CLI_BIT(CAL_CLI_REUSE_PLAN));
        foc_virtual_cal_cli_timing_start(reuse->timing, CAL_CLI_REUSE_PLAN);
        const int status = plan_reuse(reuse, argument);
        foc_virtual_cal_cli_timing_finish(reuse->timing, CAL_CLI_REUSE_PLAN, status == 0, 0);
        return status;
    }
    if (strcmp(command, "capture") == 0 || strcmp(command, "measure") == 0) {
        return prepare_reuse(reuse, strcmp(command, "capture") == 0);
    }
    if (strcmp(command, "review") == 0 || strcmp(command, "approve") == 0) {
        foc_virtual_cal_cli_timing_start(reuse->timing, CAL_CLI_REVIEW);
        const int status = authorize_reuse(reuse, argument);
        foc_virtual_cal_cli_timing_finish(reuse->timing, CAL_CLI_REVIEW, status == 0, 0);
        return status == 0 && strcmp(command, "approve") == 0 ? apply_reuse(reuse) : status;
    }
    if (strcmp(command, "reject") == 0) {
        if (!reuse->pending) {
            return foc_virtual_cal_cli_error("REVIEW_REQUIRED", "REUSE", -1);
        }
        if (!foc_virtual_cal_cli_review_matches(argument, reuse->review_id)) {
            return foc_virtual_cal_cli_error("REVIEW_ID_MISMATCH", "REUSE", -1);
        }
        revoke_reuse_review(reuse);
        puts("{\"event\":\"REUSE_REJECTED\",\"configuration_applied\":false}");
        return 0;
    }
    if (strcmp(command, "apply") == 0) {
        return apply_reuse(reuse);
    }
    if (strcmp(command, "stop") == 0 || strcmp(command, "fault") == 0 ||
        strcmp(command, "recover") == 0) {
        return exercise_original(reuse, command);
    }
    if (strcmp(command, "invalidate") == 0) {
        if (!reuse->historic) {
            return foc_virtual_cal_cli_error("HISTORY_REQUIRED", "REUSE", -1);
        }
        for (unsigned int i = 0U; i < FOC_CAL_REUSE_COUNT; ++i) {
            if (strcmp(argument, foc_cal_reuse_parameter_name((enum foc_cal_reuse_parameter)i)) ==
                0) {
                reuse->history.records[i].reviewed = false;
                revoke_reuse_review(reuse);
                return 0;
            }
        }
        return foc_virtual_cal_cli_error("INVALID_PARAMETER_SELECTION", "REUSE", -1);
    }
    if (strcmp(command, "save") == 0) {
        const uint32_t valid = foc_cal_reuse_evaluate(&reuse->history, &reuse->history.setup, NULL);
        if (!reuse->historic || reuse->pending || !reuse->measurement.workflow.result.reviewed ||
            valid != required_parameters(reuse)) {
            return foc_virtual_cal_cli_error("REVIEW_REQUIRED", "SAVE_CONFIG", -1);
        }
        /* FCAL recreates control limits from local policy and stores no provenance. */
        if (reuse->history.setup.id[FOC_CAL_REUSE_ID_CONTROL_LIMITS] != 1U) {
            return foc_virtual_cal_cli_error("FCAL_POLICY_NOT_REPRESENTED", "SAVE_CONFIG", -1);
        }
        foc_virtual_cal_cli_timing_start(reuse->timing, CAL_CLI_FCAL_SAVE);
        const int status =
            foc_virtual_calibration_package_save(argument, &reuse->measurement, &reuse->candidate);
        foc_virtual_cal_cli_timing_finish(reuse->timing, CAL_CLI_FCAL_SAVE,
                                         status == FOC_VIRTUAL_PACKAGE_OK, 0);
        if (status != FOC_VIRTUAL_PACKAGE_OK) {
            return foc_virtual_cal_cli_error(
                foc_virtual_calibration_package_reason(status),
                "SAVE_CONFIG",
                status
            );
        }
        printf(
            "{\"event\":\"REUSE_NUMERICAL_SAVED\",\"review_id\":\"%08" PRIx32 "\","
            "\"dependency_metadata_persisted\":false}\n",
            reuse->numerical_id
        );
        return 0;
    }
    if (strcmp(command, "load") == 0) {
        struct foc_runtime_snapshot snapshot;
        if (!disabled_runtime(reuse, &snapshot)) {
            return foc_virtual_cal_cli_error("ORIGINAL_RUNTIME_UNSAFE", "LOAD_CONFIG", -1);
        }
        char *approval = strrchr(argument, ' ');
        if (approval != NULL && strlen(approval + 1) == 8U &&
            strspn(approval + 1, "0123456789abcdef") == 8U) {
            *approval++ = '\0';
        } else {
            approval = NULL;
        }
        struct foc_virtual_current_profile restored_profile = reuse->target;
        foc_virtual_cal_cli_timing_reset(
            reuse->timing, CAL_CLI_APPLY_STAGES | CAL_CLI_BIT(CAL_CLI_FCAL_LOAD) |
                CAL_CLI_BIT(CAL_CLI_QUIESCENT) | CAL_CLI_BIT(CAL_CLI_LIVE_ADC) |
                CAL_CLI_BIT(CAL_CLI_LIVE_ENCODER)
        );
        const int status = foc_virtual_cal_cli_restore(
            argument,
            &restored_profile,
            &reuse->request.identity,
            approval,
            NULL,
            reuse->timing,
            "nominal"
        );
        if (status == 0) {
            puts("{\"event\":\"REUSE_LEGACY_FCAL\",\"dependency_history_imported\":false}");
        }
        return status;
    }
    return foc_virtual_cal_cli_error("INVALID_REUSE_COMMAND", "REUSE", -1);
}

static bool known_reuse_command(const char *command)
{
    static const char *const names[] = {
        "capture",
        "plan",
        "measure",
        "review",
        "reject",
        "approve",
        "apply",
        "save",
        "load",
        "stop",
        "fault",
        "recover",
        "invalidate",
        "quit",
    };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; ++i) {
        if (strcmp(command, names[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* Public Functions ------------------------------------------------------------- */

int foc_virtual_cal_cli_reuse_session(
    const char *name,
    bool cogging,
    const char *bootstrap,
    struct foc_virtual_cal_cli_timing *timing
)
{
    struct reuse_session reuse = {.use_cogging = cogging, .bootstrap = bootstrap, .timing = timing};
    if (foc_virtual_calibration_profile(name, &reuse.target, &reuse.request) !=
        FOC_VIRTUAL_CAL_OK) {
        return foc_virtual_cal_cli_error("INVALID_PROFILE_OR_RUNTIME", "SETUP", -1);
    }
    reuse.target.runtime.motion.impedance.damping_filter_hz = 48.0f;
    reuse.target_setup = reuse_initial_setup(&reuse.request.identity);
    if (bootstrap != NULL) {
        reuse.target_setup.id[FOC_CAL_REUSE_ID_ANGLE_REFERENCE] = 402U;
    }
    puts(
        "{\"event\":\"REUSE_SESSION_READY\",\"source\":\"HOST_VIRTUAL\","
        "\"dependency_history\":\"PROCESS_MEMORY\"}"
    );
    fflush(stdout);
    char line[2048];
    while (fgets(line, sizeof line, stdin) != NULL) {
        const size_t length = strcspn(line, "\r\n");
        if (line[length] == '\0' && !feof(stdin)) {
            int input;
            do {
                input = getchar();
            } while (input != '\n' && input != EOF);
            foc_virtual_cal_cli_error("REUSE_COMMAND_TOO_LONG", "REUSE", -1);
            puts("{\"event\":\"REUSE_COMMAND_RESULT\",\"command\":\"invalid\",\"ok\":false}");
            fflush(stdout);
            continue;
        }
        line[length] = '\0';
        char *argument = strchr(line, ' ');
        if (argument == NULL) {
            argument = line + length;
        } else {
            *argument++ = '\0';
        }
        const bool valid = known_reuse_command(line);
        const bool quit = valid && strcmp(line, "quit") == 0;
        const int status = quit ? 0 : reuse_command(&reuse, line, argument);
        printf(
            "{\"event\":\"REUSE_COMMAND_RESULT\",\"command\":\"%s\",\"ok\":%s}\n",
            valid ? line : "invalid",
            status == 0 ? "true" : "false"
        );
        fflush(stdout);
        if (quit) {
            return 0;
        }
    }
    return ferror(stdin) == 0 ? 0 : foc_virtual_cal_cli_error("REUSE_INPUT_FAILED", "REUSE", -1);
}

/* ---------------------------------------- end of file ----------------------------------------- */
