/**
 * @file    test_foc_calibration_application.c
 * @brief   Exercise Host calibration binding and initialization transactions.
 * @version v0.1.0
 * @date    2026.11.18
 *
 * Test role
 * ---------
 * Supply an isolated, admitted workflow result to the application boundary.
 * The workflow and demo suites separately drive the real sample estimators.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_calibration_application.h"

#include <stdio.h>
#include <string.h>

/* Private Helpers -------------------------------------------------------------- */

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return 1;                                                       \
        }                                                                   \
    } while (0)

static const struct foc_pipeline_config pipeline_template = {
    .current = {{1.0f, 2.4f, 0.0f, 1.0f, 0.0003f}, {1.08f, 1.35f}, {540.0f, 540.0f}, 2.5f},
    .adc = {0.01f, 0.002f, 1.0f, 2048.0f, 2048.0f, 2048.0f, FOC_DEVICE_PHASE_ORDER_ABC},
    .pwm = {6000U, FOC_DEVICE_PHASE_ORDER_ABC},
    .safety = {5000U, 500U, 350U, 350U, 1U},
};

static const struct foc_runtime_config original_config = {
    .h0 = {100U, 20U, 10U},
    .h1 = {200U, 20U, 10U},
    .supervision = {100U, 20U, 10U},
    .telemetry = {10000U, 20U, 10U},
    .foreground_budget_us = 15U,
    .angle_timeout_us = 150U,
    .motion = {
        .enabled = true,
        .rotor = {7U, 0.25f},
        .prediction = {150U, 2000.0f, 0.001f},
        .velocity = {0.18f, 1.4f, 0.002f},
        .position = {4.0f, 12.0f},
        .iq_limit_a = 1.25f,
        .impedance = {true, 17U, 0.25f, 0.4f, 1.0f, 0.3f, 0.1f},
        .limits = {-1.0f, 2.0f, 0.125f, 17U, true},
        .following_error = {0.125f, 400U, 200U, true},
    },
};

static const struct foc_encoder_config encoder = {
    .counts_per_revolution = 4096U,
    .direction = FOC_ENCODER_DIRECTION_NORMAL,
    .max_speed_rad_s = 200.0f,
    .min_dt_us = 100U,
    .max_dt_us = 1000U,
    .speed_lpf_cutoff_hz = 100.0f,
    .tracker_alpha = 0.35f,
    .tracker_beta = 0.35f,
};

static const struct foc_cal_workflow_request request = {
    .session = 41U,
    .profile = FOC_CAL_WORKFLOW_SELECTED_STAGES,
    .stages = FOC_CAL_WORKFLOW_ADMISSION_MASK,
    .identity = {{4095U, 0.002f, 4096U, 1, 0U, 7U}, 61U, 51U, 17U},
    .admission = {2U, 2000.0f, 2100.0f, 4.0f, 0.01f, 0.01f},
};

struct fixture {
    struct foc_runtime original;
    struct foc_cal_workflow workflow;
    struct foc_cal_application application;
    uint32_t submissions;
    bool reject_inhibit;
};

static bool count_inhibit(void *argument, const struct foc_device_pwm_request *output)
{
    struct fixture *fixture = argument;
    ++fixture->submissions;
    return output->action == FOC_DEVICE_OUTPUT_INHIBIT && !fixture->reject_inhibit;
}

static void
inject_stop_on_read(struct foc_runtime *runtime, enum foc_runtime_hook_point point, void *argument)
{
    if (point == FOC_RUNTIME_HOOK_TELEMETRY_READING) {
        uint32_t *calls = argument;
        ++*calls;
        foc_runtime_request_inhibit(runtime, 0U);
    }
}

static int rejects_observer_hook_without_calling_it(void)
{
    struct fixture fixture = {0};
    uint32_t calls = 0U;
    struct foc_runtime_config config = original_config;
    config.hook = inject_stop_on_read;
    config.hook_arg = &calls;
    CHECK(foc_runtime_init(&fixture.original, &pipeline_template, &config, 10U) == FOC_RUNTIME_OK);
    CHECK(foc_cal_workflow_init(&fixture.workflow) == FOC_CAL_WORKFLOW_OK);
    const int status = foc_cal_application_start(
        &fixture.application,
        &fixture.workflow,
        &request,
        &fixture.original,
        &pipeline_template,
        &encoder
    );
    CHECK(calls == 0U);
    CHECK(status == FOC_CAL_APPLICATION_ERR_RUNTIME);
    CHECK(fixture.application.original == NULL);
    CHECK(foc_cal_workflow_result(&fixture.workflow)->state == FOC_CAL_WORKFLOW_IDLE);
    return 0;
}

static int fixture_start(
    struct fixture *fixture,
    const struct foc_cal_workflow_request *setup,
    bool different_pipeline
)
{
    struct foc_pipeline_config pipeline = pipeline_template;
    pipeline.output_submit = count_inhibit;
    pipeline.output_arg = fixture;
    CHECK(foc_runtime_init(&fixture->original, &pipeline, &original_config, 10U) == FOC_RUNTIME_OK);
    CHECK(foc_cal_workflow_init(&fixture->workflow) == FOC_CAL_WORKFLOW_OK);
    if (different_pipeline) {
        pipeline.pwm.period_counts = 2U * pipeline_template.pwm.period_counts;
        pipeline.safety.sample_timeout_us = 2U * pipeline_template.safety.sample_timeout_us;
        pipeline.current.current_limit_a = 2.0f * pipeline_template.current.current_limit_a;
    }
    CHECK(
        foc_cal_application_start(
            &fixture->application,
            &fixture->workflow,
            setup,
            &fixture->original,
            &pipeline,
            &encoder
        ) == FOC_CAL_APPLICATION_OK
    );
    return 0;
}

static int fixture_review(struct fixture *fixture, int direction)
{
    struct foc_cal_workflow_bundle *bundle = &fixture->workflow.result;
    const struct foc_calibration_identity source = bundle->request.identity.acquisition;
    struct foc_calibration_identity resolved = source;
    resolved.encoder_direction = direction;
    bundle->adc = (struct foc_calibration_adc_result){
        .identity = source,
        .offset_a_counts = 2044.0f,
        .offset_b_counts = 2045.0f,
        .offset_c_counts = 2046.0f,
        .samples = 16U,
        .state = FOC_CALIBRATION_STATE_VALID,
        .usable = true,
    };
    bundle->rotor = (struct foc_calibration_rotor_result){
        .identity = resolved,
        .direction = direction,
        .pole_pairs = 7U,
        .electrical_offset_rad = 0.5f,
        .candidate_separation_rad = 1.0f,
        .state = FOC_CALIBRATION_STATE_VALID,
        .identifiable = true,
    };
    bundle->offset = (struct foc_calibration_offset_result){
        .identity = resolved,
        .electrical_offset_rad = 0.5f,
        .state = FOC_CALIBRATION_STATE_VALID,
        .usable = true,
    };
    const struct foc_calibration_admission_input input = {
        .adc = &bundle->adc,
        .rotor = &bundle->rotor,
        .offset = &bundle->offset,
        .expected_identity = &source,
        .limits = bundle->request.admission,
    };
    CHECK(foc_calibration_admit(&input, &bundle->snapshot, NULL) == FOC_CALIBRATION_ADMISSION_OK);
    bundle->passed = bundle->request.stages;
    bundle->complete = true;
    bundle->applicable = true;
    bundle->state = FOC_CAL_WORKFLOW_COMPLETED;
    CHECK(
        foc_cal_workflow_review(
            &fixture->workflow,
            bundle->request.session,
            &bundle->request.identity,
            true
        ) == FOC_CAL_WORKFLOW_OK
    );
    return 0;
}

static int explicit_apply_retains_safety(void)
{
    struct fixture fixture = {0};
    CHECK(fixture_start(&fixture, &request, true) == 0);
    CHECK(fixture_review(&fixture, 1) == 0);
    struct foc_cal_application_candidate candidate;
    CHECK(
        foc_cal_application_prepare(&fixture.application, &candidate) ==
        FOC_CAL_APPLICATION_PREPARED
    );
    const struct foc_config_diff_input left = {&fixture.application.original_view.effective, 0};
    const struct foc_config_diff_input right = {&candidate.runtime.config, 0};
    struct foc_config_diff_entry changes[32];
    size_t count;
    CHECK(foc_config_diff(&left, &right, changes, 32U, &count) == FOC_CONFIG_STORE_OK);
    size_t changed = 0U;
    for (size_t index = 0; index < count; ++index) {
        if (changes[index].kind == FOC_DIAG_VALUE_CHANGED) {
            CHECK(changes[index].parameter->id == FOC_PARAMETER_ELECTRICAL_OFFSET);
            ++changed;
        }
    }
    CHECK(changed == 1U && candidate.store_status == FOC_CONFIG_STORE_ERR_IDENTITY);
    struct foc_runtime destination = {0};
    struct foc_cal_application_sensor sensor = {0};
    CHECK(
        foc_cal_application_apply(&fixture.application, &destination, &sensor, 20U) ==
        FOC_CAL_APPLICATION_APPLIED
    );
    struct foc_runtime_diagnostic applied;
    CHECK(foc_runtime_diagnose(&destination, &applied) == FOC_RUNTIME_OK);
    CHECK(foc_config_application_status(&right, &applied) == FOC_DIAG_MATCHES_EFFECTIVE);
    CHECK(applied.snapshot.state == FOC_SAFE_STATE_DISABLED && applied.snapshot.output_confirmed);
    CHECK(applied.snapshot.action == FOC_DEVICE_OUTPUT_INHIBIT && applied.snapshot.updates == 0U);
    CHECK(
        applied.effective.motion.limits.enabled && applied.effective.motion.following_error.enabled
    );
    CHECK(destination.pipeline.adc_config.phase_a_offset_count == 2044.0f);
    CHECK(destination.pipeline.adc_config.phase_b_offset_count == 2045.0f);
    CHECK(destination.pipeline.adc_config.phase_c_offset_count == 2046.0f);
    CHECK(destination.pipeline.pwm_config.period_counts == 6000U);
    CHECK(destination.pipeline.safety.config.sample_timeout_us == 500U);
    CHECK(destination.pipeline.current.config.current_limit_a == 2.5f);
    CHECK(sensor.encoder.initialized && sensor.encoder.lut_offsets == NULL);
    CHECK(fixture.submissions == 1U && destination.pipeline.output_submit == NULL);
    CHECK(fixture.original.pipeline.adc_config.phase_a_offset_count == 2048.0f);
    CHECK(fixture.original.config.motion.rotor.electrical_offset_rad == 0.25f);
    CHECK(
        foc_cal_application_apply(&fixture.application, &destination, &sensor, 21U) ==
        FOC_CAL_APPLICATION_ERR_INPUT
    );
    return 0;
}

static int installed_lut_survives_workflow_restart(void)
{
    struct foc_cal_workflow_request setup = request;
    setup.stages |= FOC_CAL_WORKFLOW_STAGE_BIT(FOC_CAL_WORKFLOW_LUT);
    setup.identity.acquisition.encoder_direction = -1;
    struct fixture fixture = {0};
    CHECK(fixture_start(&fixture, &setup, false) == 0);
    CHECK(fixture_review(&fixture, -1) == 0);
    fixture.workflow.result.lut = (struct foc_encoder_cal_result){
        .setup = {51U, 21U, 4096U, FOC_ENCODER_DIRECTION_NORMAL, FOC_ENCODER_CAL_SOURCE_EXTERNAL},
        .state = FOC_CALIBRATION_STATE_VALID,
        .nodes = 4U,
        .offsets = {3, 5, -3, -5},
    };
    struct foc_runtime destination = {0};
    struct foc_cal_application_sensor sensor = {0};
    CHECK(
        foc_cal_application_apply(&fixture.application, &destination, &sensor, 20U) ==
        FOC_CAL_APPLICATION_APPLIED
    );
    CHECK(sensor.encoder.config.direction == FOC_ENCODER_DIRECTION_NORMAL);
    CHECK(foc_cal_workflow_cancel(&fixture.workflow) == FOC_CAL_WORKFLOW_OK);
    ++setup.session;
    CHECK(foc_cal_workflow_start(&fixture.workflow, &setup) == FOC_CAL_WORKFLOW_OK);
    uint32_t corrected;
    CHECK(foc_encoder_correct_code(&sensor.encoder, 1024U, &corrected) == FOC_ENCODER_STATUS_OK);
    CHECK(corrected == 1029U);
    struct foc_cal_application_candidate candidate;
    CHECK(
        foc_cal_application_prepare(&fixture.application, &candidate) ==
        FOC_CAL_APPLICATION_ERR_IDENTITY
    );
    return 0;
}

static int rejected_apply_preserves_destinations(void)
{
    for (unsigned int scenario = 0U; scenario < 11U; ++scenario) {
        struct fixture fixture = {0};
        CHECK(fixture_start(&fixture, &request, false) == 0);
        CHECK(fixture_review(&fixture, scenario == 7U ? -1 : 1) == 0);
        int expected;
        switch (scenario) {
        case 0:
            CHECK(foc_cal_workflow_cancel(&fixture.workflow) == FOC_CAL_WORKFLOW_OK);
            expected = FOC_CAL_APPLICATION_ERR_REVIEW;
            break;
        case 1:
            foc_runtime_request_inhibit(&fixture.original, 0U);
            expected = FOC_CAL_APPLICATION_ERR_INHIBIT;
            break;
        case 2:
            foc_runtime_request_inhibit(&fixture.original, FOC_SAFE_FAULT_FATAL_RESET_REQUIRED);
            expected = FOC_CAL_APPLICATION_ERR_FAULT;
            break;
        case 3: {
            const struct foc_runtime_lifecycle_request lifecycle = {
                .action = FOC_RUNTIME_ACTION_SET_MODE,
                .requested_mode = FOC_RUNTIME_MODE_IMPEDANCE,
                .produced_us = 10U,
            };
            CHECK(
                foc_runtime_publish_lifecycle_request(&fixture.original, &lifecycle, 10U) ==
                FOC_RUNTIME_OK
            );
            expected = FOC_CAL_APPLICATION_ERR_LIFECYCLE;
            break;
        }
        case 4:
            CHECK(
                foc_runtime_init(&fixture.original, &pipeline_template, &original_config, 30U) ==
                FOC_RUNTIME_OK
            );
            expected = FOC_CAL_APPLICATION_ERR_STALE;
            break;
        case 5: {
            struct foc_cal_workflow_request next = request;
            ++next.session;
            CHECK(foc_cal_workflow_start(&fixture.workflow, &next) == FOC_CAL_WORKFLOW_OK);
            expected = FOC_CAL_APPLICATION_ERR_IDENTITY;
            break;
        }
        case 6:
            /* Inject invalid init policy into the detached binding. Runtime must
             * still reject at its own admission without changing destination. */
            fixture.application.pipeline.pwm.period_counts = 0U;
            expected = FOC_CAL_APPLICATION_ERR_CONFIG;
            break;
        case 7:
            expected = FOC_CAL_APPLICATION_ERR_MAPPING;
            break;
        case 8:
            CHECK(
                foc_cal_workflow_review(
                    &fixture.workflow,
                    request.session,
                    &request.identity,
                    false
                ) == FOC_CAL_WORKFLOW_OK
            );
            expected = FOC_CAL_APPLICATION_ERR_REVIEW;
            break;
        default: {
            fixture.reject_inhibit = scenario == 10U;
            foc_runtime_request_inhibit(&fixture.original, 0U);
            const struct foc_runtime_fast_input input = {
                .adc = {2400U, 2048U, 2048U, 2048U, FOC_DEVICE_SAMPLE_ALL_VALID, 0U, 10U},
                .now_us = 10U,
            };
            (void)foc_runtime_h0_step(&fixture.original, &input);
            struct foc_runtime_snapshot stopped;
            CHECK(foc_runtime_read_snapshot(&fixture.original, &stopped) == FOC_RUNTIME_OK);
            CHECK(stopped.output_confirmed != fixture.reject_inhibit);
            CHECK(
                stopped.motion_session != fixture.application.original_view.snapshot.motion_session
            );
            expected = fixture.reject_inhibit ? FOC_CAL_APPLICATION_ERR_INHIBIT
                                              : FOC_CAL_APPLICATION_ERR_STALE;
            break;
        }
        }
        struct foc_runtime destination = {0};
        unsigned char runtime_before[sizeof(destination)];
        memcpy(runtime_before, &destination, sizeof(destination));
        struct foc_cal_application_sensor sensor = {0};
        unsigned char sensor_before[sizeof(sensor)];
        memcpy(sensor_before, &sensor, sizeof(sensor));
        CHECK(
            foc_cal_application_apply(&fixture.application, &destination, &sensor, 40U) == expected
        );
        CHECK(memcmp(runtime_before, &destination, sizeof(destination)) == 0);
        CHECK(memcmp(sensor_before, &sensor, sizeof(sensor)) == 0);
        CHECK(fixture.original.config.motion.rotor.electrical_offset_rad == 0.25f);
    }
    return 0;
}

/* Public Functions ------------------------------------------------------------- */

int main(void)
{
    return rejects_observer_hook_without_calling_it() || explicit_apply_retains_safety() ||
        installed_lut_survives_workflow_restart() || rejected_apply_preserves_destinations();
}

/* ---------------------------------------- end of file ----------------------------------------- */
