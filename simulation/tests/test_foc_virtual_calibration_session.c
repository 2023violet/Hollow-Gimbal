/**
 * @file    test_foc_virtual_calibration_session.c
 * @brief   Abort and rejection paths through the Host calibration entry point.
 * @version v0.1.0
 * @date    2026.11.19
 *
 * Test role
 * ---------
 * Exercise the production Runtime STOP, fault and lifecycle interfaces while
 * the Host Foreground driver owns an isolated capture session.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_virtual_calibration.h"

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

enum abort_cause {
    EXPLICIT_CANCEL,
    STOP_PENDING,
    STOP_ACKNOWLEDGED,
    FAULT_ACKNOWLEDGED,
    ARM_PENDING,
    SESSION_TIMEOUT,
};

static int start_generic(struct foc_virtual_calibration *session)
{
    struct foc_virtual_current_profile profile;
    struct foc_cal_workflow_request request;
    CHECK(foc_virtual_calibration_profile("GENERIC_RESEARCH_MOTOR", &profile, &request) == 0);
    return foc_virtual_calibration_start(session, &profile, &request);
}

static int assert_application_revoked(struct foc_virtual_calibration *session)
{
    const struct foc_cal_workflow_bundle *bundle = foc_cal_workflow_result(&session->workflow);
    CHECK(bundle != NULL && !bundle->reviewed && !bundle->applicable);
    CHECK(!bundle->adc.usable && !bundle->electrical.usable && !bundle->electrical_q.usable);
    CHECK(bundle->state == FOC_CAL_WORKFLOW_CANCELLED);
    struct foc_cal_application_candidate candidate;
    CHECK(foc_cal_application_prepare(&session->application, &candidate) != FOC_CAL_APPLICATION_OK);
    CHECK(session->original.output.mode == FOC_VIRTUAL_OUTPUT_HIGH_Z);
    CHECK(session->original.output.accepted_updates == 0U);
    return 0;
}

static int test_abort(enum abort_cause cause)
{
    struct foc_virtual_calibration session = {0};
    CHECK(start_generic(&session) == FOC_VIRTUAL_CAL_PENDING);
    CHECK(start_generic(&session) == FOC_VIRTUAL_CAL_ERR_SETUP);
    CHECK(foc_virtual_calibration_poll(&session) == FOC_VIRTUAL_CAL_PENDING);
    CHECK(session.stage == FOC_VIRTUAL_CAL_LUT && session.workflow.result.adc.usable);
    struct foc_runtime_snapshot before;
    CHECK(foc_virtual_current_snapshot(&session.original, &before) == FOC_RUNTIME_OK);
    CHECK(before.state == FOC_SAFE_STATE_DISABLED && before.output_confirmed);

    switch (cause) {
    case EXPLICIT_CANCEL:
        foc_virtual_calibration_cancel(&session);
        break;
    case STOP_PENDING:
    case STOP_ACKNOWLEDGED:
        CHECK(foc_virtual_current_request_stop(&session.original) == 0);
        if (cause == STOP_ACKNOWLEDGED) {
            CHECK(
                foc_virtual_current_run_until(
                    &session.original,
                    session.profile.runtime.h0.period_us
                ) == 0
            );
            struct foc_runtime_snapshot stopped;
            CHECK(foc_virtual_current_snapshot(&session.original, &stopped) == FOC_RUNTIME_OK);
            CHECK(!stopped.inhibit_pending && stopped.motion_session != before.motion_session);
        }
        break;
    case FAULT_ACKNOWLEDGED: {
        foc_runtime_request_inhibit(&session.original.runtime, FOC_SAFE_FAULT_SAMPLE_INVALID);
        CHECK(
            foc_virtual_current_run_until(
                &session.original,
                session.profile.runtime.h0.period_us
            ) == 0
        );
        struct foc_runtime_snapshot faulted;
        CHECK(foc_virtual_current_snapshot(&session.original, &faulted) == FOC_RUNTIME_OK);
        CHECK(faulted.state == FOC_SAFE_STATE_FAULT_LATCHED);
        CHECK((faulted.faults & FOC_SAFE_FAULT_SAMPLE_INVALID) != 0U);
        break;
    }
    case ARM_PENDING: {
        const uint32_t now_us = session.original.virtual_time_us;
        const struct foc_runtime_lifecycle_request arm = {
            .action = FOC_RUNTIME_ACTION_ARM,
            .source_session = before.motion_session,
            .produced_us = now_us,
        };
        CHECK(
            foc_runtime_publish_lifecycle_request(&session.original.runtime, &arm, now_us) ==
            FOC_RUNTIME_OK
        );
        break;
    }
    case SESSION_TIMEOUT:
        session.time_us = 60000000U;
        break;
    }
    const int expected =
        cause == SESSION_TIMEOUT ? FOC_VIRTUAL_CAL_ERR_TIMEOUT : FOC_VIRTUAL_CAL_ERR_TERMINATED;
    CHECK(foc_virtual_calibration_poll(&session) == expected);
    CHECK(foc_virtual_calibration_poll(&session) == expected);
    CHECK(session.stage == FOC_VIRTUAL_CAL_LUT);
    struct foc_virtual_motor_observation plant;
    CHECK(foc_virtual_motor_observe(&session.motor, &plant) == 0);
    CHECK(!plant.fixture_active && plant.v_alpha_v == 0.0 && plant.v_beta_v == 0.0);
    CHECK(assert_application_revoked(&session) == 0);
    return 0;
}

static int test_invalid_setup_and_reference(void)
{
    struct foc_virtual_calibration session = {0};
    struct foc_virtual_current_profile profile;
    struct foc_cal_workflow_request request;
    CHECK(foc_virtual_calibration_poll(&session) == FOC_VIRTUAL_CAL_ERR_SETUP);
    CHECK(
        foc_virtual_calibration_profile("UNKNOWN_MOTOR", &profile, &request) ==
        FOC_VIRTUAL_CAL_ERR_SETUP
    );
    CHECK(foc_virtual_calibration_profile("GENERIC_RESEARCH_MOTOR", &profile, &request) == 0);
    profile.motor.psi_f_wb = NAN;
    CHECK(foc_virtual_calibration_start(&session, &profile, &request) == FOC_VIRTUAL_CAL_ERR_SETUP);

    struct foc_virtual_calibration absent_reference = {0};
    CHECK(start_generic(&absent_reference) == FOC_VIRTUAL_CAL_PENDING);
    CHECK(foc_virtual_calibration_poll(&absent_reference) == FOC_VIRTUAL_CAL_PENDING);
    CHECK(foc_virtual_calibration_poll(&absent_reference) == FOC_VIRTUAL_CAL_PENDING);
    CHECK(absent_reference.stage == FOC_VIRTUAL_CAL_ROTOR);
    /* Inject loss of the dedicated experiment plant. The original Runtime is still live. */
    absent_reference.motor.initialized = false;
    CHECK(foc_virtual_calibration_poll(&absent_reference) == FOC_VIRTUAL_CAL_ERR_REFERENCE);
    CHECK(absent_reference.native_status == FOC_CAL_WORKFLOW_ERR_DATA_REQUIRED);
    CHECK(assert_application_revoked(&absent_reference) == 0);
    return 0;
}

static void stop_during_electrical(
    void *argument,
    enum foc_motor_id_axis axis,
    const struct foc_motor_id_sample *sample
)
{
    if (axis == FOC_MOTOR_ID_D && sample->sequence == 1U) {
        (void)foc_virtual_current_request_stop(argument);
    }
}

static int test_stop_during_capture(void)
{
    struct foc_virtual_calibration session = {0};
    CHECK(start_generic(&session) == FOC_VIRTUAL_CAL_PENDING);
    for (unsigned int i = 0U; i < FOC_VIRTUAL_CAL_ELECTRICAL_D; ++i) {
        CHECK(foc_virtual_calibration_poll(&session) == FOC_VIRTUAL_CAL_PENDING);
    }
    CHECK(session.stage == FOC_VIRTUAL_CAL_ELECTRICAL_D);
    session.electrical.rl_endpoint = stop_during_electrical;
    session.electrical.rl_endpoint_argument = &session.original;
    CHECK(foc_virtual_calibration_poll(&session) == FOC_VIRTUAL_CAL_ERR_TERMINATED);
    CHECK(session.workflow.result.stages[FOC_CAL_WORKFLOW_ELECTRICAL].primary.samples == 2U);
    CHECK(assert_application_revoked(&session) == 0);
    return 0;
}

static int test_deadline_during_capture(void)
{
    struct foc_virtual_calibration session = {0};
    CHECK(start_generic(&session) == FOC_VIRTUAL_CAL_PENDING);
    session.time_us = 11999999U;
    CHECK(foc_virtual_calibration_poll(&session) == FOC_VIRTUAL_CAL_ERR_TIMEOUT);
    CHECK(assert_application_revoked(&session) == 0);
    return 0;
}

static int test_mechanical_identification(const char *name, bool missing_torque_source)
{
    struct foc_virtual_calibration session = {0};
    struct foc_virtual_current_profile profile;
    struct foc_cal_workflow_request request;
    CHECK(foc_virtual_calibration_profile(name, &profile, &request) == 0);
    CHECK(foc_virtual_calibration_start(&session, &profile, &request) == FOC_VIRTUAL_CAL_PENDING);
    for (unsigned int i = 0U; i < FOC_VIRTUAL_CAL_MECHANICAL; ++i) {
        CHECK(foc_virtual_calibration_poll(&session) == FOC_VIRTUAL_CAL_PENDING);
    }
    CHECK(session.stage == FOC_VIRTUAL_CAL_MECHANICAL && session.flux.kt.qualified);
    if (missing_torque_source) {
        /* Model-derived Kt still needs a qualified, independent capture identity. */
        session.flux.kt.source = FOC_CAL_WORKFLOW_SOURCE_NONE;
        CHECK(foc_virtual_calibration_poll(&session) == FOC_VIRTUAL_CAL_ERR_REFERENCE);
        CHECK(session.workflow.result.stages[FOC_CAL_WORKFLOW_MECHANICAL].primary.samples == 0U);
        CHECK(assert_application_revoked(&session) == 0);
        return 0;
    }
    CHECK(foc_virtual_calibration_poll(&session) == FOC_VIRTUAL_CAL_PENDING);
    const struct foc_mechanical_id_result *fit = &session.workflow.result.mechanical;
    CHECK(fit->usable && fit->torque_source == FOC_MECHANICAL_ID_TORQUE_EQUIVALENT);
    CHECK(fit->load_source == FOC_MECHANICAL_ID_LOAD_OBSERVED);
    CHECK(fabs(fit->inertia_kg_m2 / profile.motor.inertia_kg_m2 - 1.0) < 0.02);
    CHECK(fabs(fit->damping_nm_s / profile.motor.damping_nm_s - 1.0) < 0.02);
    printf(
        "MECHANICAL %s J=%.6g B=%.6g residual=%.6g\n",
        name,
        fit->inertia_kg_m2,
        fit->damping_nm_s,
        fit->residual_rms_nm
    );
    CHECK(foc_virtual_calibration_poll(&session) == FOC_VIRTUAL_CAL_OK);
    CHECK(session.stage == FOC_VIRTUAL_CAL_REVIEW && session.workflow.result.applicable);
    CHECK(foc_virtual_current_request_stop(&session.original) == 0);
    CHECK(foc_virtual_calibration_poll(&session) == FOC_VIRTUAL_CAL_ERR_TERMINATED);
    CHECK(!session.flux.qualified && !session.flux.kt.qualified);
    CHECK(assert_application_revoked(&session) == 0);
    return 0;
}

/* Test Entry ------------------------------------------------------------------- */

int main(void)
{
    CHECK(test_abort(EXPLICIT_CANCEL) == 0);
    CHECK(test_abort(STOP_PENDING) == 0);
    CHECK(test_abort(STOP_ACKNOWLEDGED) == 0);
    CHECK(test_abort(FAULT_ACKNOWLEDGED) == 0);
    CHECK(test_abort(ARM_PENDING) == 0);
    CHECK(test_abort(SESSION_TIMEOUT) == 0);
    CHECK(test_invalid_setup_and_reference() == 0);
    CHECK(test_stop_during_capture() == 0);
    CHECK(test_deadline_during_capture() == 0);
    CHECK(test_mechanical_identification("GENERIC_RESEARCH_MOTOR", false) == 0);
    CHECK(test_mechanical_identification("CALIBRATION_SALIENT_MOTOR", false) == 0);
    CHECK(test_mechanical_identification("GENERIC_RESEARCH_MOTOR", true) == 0);
    puts("CALIBRATION session cancel, STOP, fault, lifecycle, timeout and reference refusal PASS");
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
