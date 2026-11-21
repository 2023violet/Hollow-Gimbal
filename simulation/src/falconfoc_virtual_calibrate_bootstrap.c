/**
 * @file    falconfoc_virtual_calibrate_bootstrap.c
 * @brief   Qualify and acquire a Host-only electrical reference before full calibration.
 * @version v0.1.0
 * @date    2026.11.20
 *
 * Implementation role
 * -------------------
 * Hold the original Runtime inhibited during an external mechanical scan.
 * Pass only synchronized Host instrument observations to the portable fitter.
 */

/* Includes --------------------------------------------------------------------- */

#include "falconfoc_virtual_calibrate_internal.h"
#include "foc_virtual_bootstrap_capture.h"
#include "foc_virtual_bootstrap_instrument.h"
#include "foc_virtual_calibration_trial.h"
#include "crc.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Private Types ---------------------------------------------------------------- */

struct scan_gate {
    struct foc_virtual_current *owner;
    const char *scenario;
    const char *failure;
};

/* Private Helpers -------------------------------------------------------------- */

static bool safe_original(struct foc_virtual_current *owner)
{
    struct foc_runtime_snapshot view;
    struct foc_runtime_lifecycle_result lifecycle;
    return foc_virtual_current_snapshot(owner, &view) == FOC_RUNTIME_OK &&
        foc_runtime_read_lifecycle_result(&owner->runtime, &lifecycle) == FOC_RUNTIME_OK &&
        view.state == FOC_SAFE_STATE_DISABLED && view.faults == 0U && view.output_confirmed &&
        view.action == FOC_DEVICE_OUTPUT_INHIBIT && !view.inhibit_pending &&
        lifecycle.result != FOC_RUNTIME_REQUEST_PENDING && owner->output.accepted_updates == 0U;
}

static int permit_scan(void *argument, uint32_t aperture_us)
{
    struct scan_gate *gate = argument;
    if (aperture_us >= 400000U) {
        if (strcmp(gate->scenario, "stop") == 0) {
            foc_runtime_request_inhibit(&gate->owner->runtime, 0U);
            gate->failure = "STOP";
        } else if (strcmp(gate->scenario, "fault") == 0) {
            foc_runtime_request_inhibit(&gate->owner->runtime, FOC_SAFE_FAULT_WDT);
            gate->failure = "FAULT";
        } else if (strcmp(gate->scenario, "timeout") == 0) {
            gate->failure = "TIMEOUT";
        } else if (strcmp(gate->scenario, "cancel") == 0) {
            gate->failure = "SESSION_CANCELLED";
        } else if (strcmp(gate->scenario, "identity-change") == 0) {
            gate->failure = "IDENTITY_MISMATCH";
        } else if (strcmp(gate->scenario, "output-reject") == 0) {
            gate->failure = "INHIBIT_CONFIRMATION_LOST";
        }
    }
    if (!safe_original(gate->owner) && gate->failure == NULL) {
        gate->failure = "SAFETY_BLOCKED";
    }
    return gate->failure == NULL ? 0 : -1;
}

static bool verify_adc(
    struct foc_virtual_current *owner,
    const struct foc_virtual_current_profile *profile,
    const struct foc_calibration_identity *identity
)
{
    struct foc_calibration_adc_context context;
    const struct foc_calibration_adc_config policy = {
        .identity = *identity,
        .required_valid_mask = 7U,
        .minimum_samples = 48U,
        .maximum_samples = 48U,
        .maximum_sample_step_counts = 6.0f,
        .maximum_noise_rms_counts = 2.0f,
        .minimum_offset_counts = 32.0f,
        .maximum_offset_counts = 4063.0f,
        .maximum_interval_us = 250U,
    };
    if (foc_calibration_adc_init(&context, &policy) != 0) {
        return false;
    }
    for (uint32_t i = 0U; i < 48U; ++i) {
        struct foc_device_adc_raw raw;
        struct foc_device_measurement measured;
        struct foc_virtual_bootstrap_shaft shaft;
        const uint32_t time = (i + 1U) * 200U;
        if (!safe_original(owner) || foc_virtual_motor_measure_shaft(&owner->motor, &shaft) != 0 ||
            foc_virtual_current_acquire_adc(profile, &owner->motor, i, time, &raw) != 0 ||
            foc_device_decode_sample(&raw, &profile->adc, &measured) != FOC_DEVICE_STATUS_OK) {
            return false;
        }
        const bool abc = profile->adc.phase_order == FOC_DEVICE_PHASE_ORDER_ABC;
        const struct foc_calibration_adc_sample sample = {
            .phase_a = raw.rank2_raw,
            .phase_b = abc ? raw.rank3_raw : raw.rank4_raw,
            .phase_c = abc ? raw.rank4_raw : raw.rank3_raw,
            .valid_mask = raw.valid_mask == FOC_DEVICE_SAMPLE_ALL_VALID ? 7U : 0U,
            .sequence = i,
            .captured_us = raw.sample_tick,
            .zero_current_confirmed = shaft.zero_current && fabs(shaft.speed_rad_s) < 1e-6,
        };
        if (foc_calibration_adc_collect(&context, &sample) != 0) {
            return false;
        }
    }
    struct foc_calibration_adc_result result;
    return foc_calibration_adc_finalize(&context, &result) == 0 && result.usable;
}

static const char *status_name(enum foc_bootstrap_status status)
{
    static const char *const names[] = {
        [FOC_BOOTSTRAP_MEASUREMENT_INVALID] = "MEASUREMENT_INVALID",
        [FOC_BOOTSTRAP_READY_WITH_TRUSTED_REFERENCE] = "READY_WITH_TRUSTED_REFERENCE",
        [FOC_BOOTSTRAP_REFERENCE_ACQUISITION_REQUIRED] = "REFERENCE_ACQUISITION_REQUIRED",
        [FOC_BOOTSTRAP_INSUFFICIENT_EXCITATION] = "INSUFFICIENT_EXCITATION",
        [FOC_BOOTSTRAP_REFERENCE_UNIDENTIFIABLE] = "REFERENCE_UNIDENTIFIABLE",
        [FOC_BOOTSTRAP_IDENTITY_MISMATCH] = "IDENTITY_MISMATCH",
        [FOC_BOOTSTRAP_SAFETY_BLOCKED] = "SAFETY_BLOCKED",
        [FOC_BOOTSTRAP_REFERENCE_REVIEW_REQUIRED] = "REFERENCE_REVIEW_REQUIRED",
    };
    return (unsigned int)status < sizeof names / sizeof names[0] ? names[status] : names[0];
}

static bool
write_capture(const char *path, const struct foc_virtual_bootstrap_capture_result *capture)
{
    if (path == NULL) {
        return true;
    }
    FILE *file = NULL;
#if defined(_MSC_VER)
    (void)fopen_s(&file, path, "wbx");
#else
    file = fopen(path, "wbx");
#endif
    if (file == NULL) {
        return false;
    }
    bool good = fputs(
                    "motor,encoder,frame,phase_order,capture,sequence,aperture_us,sweep,"
                    "raw_encoder,shaft_rad_s,emf_alpha_v,emf_beta_v,valid,output_inhibited\n",
                    file
                ) >= 0;
    for (uint32_t i = 0U; good && i < capture->count; ++i) {
        const struct foc_bootstrap_sample *row = &capture->samples[i];
        good = fprintf(
                   file,
                   "%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ","
                   "%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%.9g,%.9g,%.9g,%d,%d\n",
                   row->identity.motor_setup_id,
                   row->identity.encoder_id,
                   row->identity.mechanical_frame_id,
                   row->identity.acquisition.phase_order,
                   row->capture,
                   row->sequence,
                   row->captured_us,
                   row->sweep,
                   row->raw_code,
                   (double)row->shaft_rad_s,
                   (double)row->emf_alpha_v,
                   (double)row->emf_beta_v,
                   row->valid,
                   row->output_inhibited
               ) >= 0;
    }
    return fclose(file) == 0 && good;
}

static int setup_fixture(
    const char *scenario,
    struct foc_virtual_current_profile *instrument,
    struct foc_virtual_current *owner
)
{
    /* These named negative fixtures affect the instruments/Plant only. The
     * fit receives no fixture setting, except the declared capture metadata. */
    if (strcmp(scenario, "current-limit") == 0) {
        instrument->current.current_limit_a = 0.0005f;
    } else if (strcmp(scenario, "partial") == 0) {
        instrument->motor.inertia_kg_m2 *= 8.0;
    } else if (strcmp(scenario, "weak-emf") == 0) {
        instrument->motor.psi_f_wb = 0.0;
    }
    struct foc_virtual_current_profile guard = *instrument;
    guard.runtime.motion = (struct foc_runtime_motion_config){0};
    if (foc_virtual_current_init(owner, &guard) != 0 || !safe_original(owner)) {
        return -1;
    }
    if (strcmp(scenario, "stalled") == 0) {
        return foc_virtual_motor_set_fixture_speed(&owner->motor, 0.0);
    }
    if (strcmp(scenario, "load") == 0) {
        return foc_virtual_motor_set_load(&owner->motor, 0.55);
    }
    return 0;
}

/* Public Functions ------------------------------------------------------------- */

int foc_virtual_cal_cli_bootstrap_sampled(
    const char *mode,
    const char *scenario,
    const char *trace,
    struct foc_virtual_current_profile *profile,
    struct foc_cal_workflow_request *request,
    uint32_t *signature,
    const struct foc_virtual_cal_trial *trial
)
{
    const bool inspect = strcmp(mode, "inspect") == 0;
    const bool partial = strcmp(mode, "offset") == 0;
    if (!inspect && !partial && strcmp(mode, "unknown") != 0) {
        return foc_virtual_cal_cli_error("INVALID_BOOTSTRAP_MODE", "BOOTSTRAP", -1);
    }
    static const char *const scenarios[] = {
        "nominal",
        "load",
        "partial",
        "stalled",
        "encoder",
        "current-limit",
        "phase",
        "direction",
        "weak-emf",
        "stop",
        "fault",
        "timeout",
        "cancel",
        "identity-change",
        "output-reject",
    };
    bool known_scenario = false;
    for (size_t i = 0; i < sizeof scenarios / sizeof scenarios[0]; ++i) {
        known_scenario |= strcmp(scenario, scenarios[i]) == 0;
    }
    if (!known_scenario || (!partial && strcmp(scenario, "direction") == 0)) {
        return foc_virtual_cal_cli_error("INVALID_BOOTSTRAP_SCENARIO", "BOOTSTRAP", -1);
    }
    const struct foc_cal_workflow_identity configured = request->identity;
    struct foc_cal_workflow_identity identity = configured;
    identity.acquisition.pole_pairs = partial ? configured.acquisition.pole_pairs : 0U;
    identity.acquisition.encoder_direction = partial ? configured.acquisition.encoder_direction : 0;
    if (partial && strcmp(scenario, "direction") == 0) {
        identity.acquisition.encoder_direction = -identity.acquisition.encoder_direction;
    }
    struct foc_virtual_current_profile instrument = *profile;
    struct foc_virtual_current original = {0};
    if (setup_fixture(scenario, &instrument, &original) != 0) {
        return foc_virtual_cal_cli_error("SAFETY_BLOCKED", "BOOTSTRAP_SETUP", -1);
    }
    if (trial != NULL) {
        /* Change the stationary experiment start without altering motor truth. */
        struct foc_virtual_motor_state start = {0};
        start.mechanical_position_rad = trial->initial_position_rad;
        if (foc_virtual_motor_init(&original.motor, &instrument.motor, &start) != 0) {
            return foc_virtual_cal_cli_error("INVALID_TRIAL", "BOOTSTRAP_SETUP", -1);
        }
    }
    struct foc_cal_workflow_identity observed = identity;
    observed.acquisition.phase_order = instrument.adc.phase_order;
    if (strcmp(scenario, "phase") == 0) {
        observed.acquisition.phase_order ^= 1U;
    }
    const struct foc_bootstrap_prerequisites preconditions = {
        .current_mapping_and_zero_verified =
            verify_adc(&original, &instrument, &identity.acquisition),
        .phase_and_direction_verified = instrument.adc.phase_order == instrument.pwm.phase_order,
        .encoder_and_clock_verified = instrument.encoder.counts_per_revolution ==
            identity.acquisition.encoder_counts_per_revolution,
        .bus_and_limits_verified =
            isfinite(instrument.vbus_v) && instrument.vbus_v >= instrument.current.pwm.vbus_min_v,
        .acquisition_verified = true,
        .safety_permitted = safe_original(&original),
    };
    enum foc_bootstrap_status status =
        foc_bootstrap_qualify(&identity, &observed, &preconditions, NULL);
    printf(
        "{\"event\":\"BOOTSTRAP_PLAN\",\"mode\":\"%s\",\"status\":\"%s\","
        "\"known_pole_pairs\":%u,\"known_direction\":%d,\"offset_known\":false,"
        "\"mechanical_scan_required\":true,\"electrical_drive\":false}\n",
        mode,
        status_name(status),
        (unsigned int)identity.acquisition.pole_pairs,
        (int)identity.acquisition.encoder_direction
    );
    if (status != FOC_BOOTSTRAP_REFERENCE_ACQUISITION_REQUIRED) {
        return foc_virtual_cal_cli_error(status_name(status), "BOOTSTRAP_QUALIFY", (int)status);
    }
    if (inspect) {
        return 0;
    }
    struct scan_gate gate = {.owner = &original, .scenario = scenario};
    const struct foc_virtual_bootstrap_capture_request capture_policy = {
        .identity = identity,
        .capture = 30U + (trial != NULL ? request->session : 0U),
        .started_us = 9600U + (trial != NULL ? trial->started_us : 0U),
        .encoder_unavailable = strcmp(scenario, "encoder") == 0,
        .permit = permit_scan,
        .argument = &gate,
        .trial = trial,
    };
    struct foc_virtual_bootstrap_capture_result capture;
    const int acquired =
        foc_virtual_bootstrap_capture(&original.motor, &instrument, &capture_policy, &capture);
    if (!write_capture(trace, &capture)) {
        return foc_virtual_cal_cli_error("TRACE_IO", "BOOTSTRAP_SCAN", -1);
    }
    const bool original_safe = safe_original(&original);
    if (!original_safe) {
        /* Service a pending STOP/fault on the old disabled owner. It has no
         * motion configuration and can only continue to inhibit. */
        (void)foc_virtual_current_run_until(&original, 800U);
    }
    struct foc_runtime_snapshot final_view = {0};
    (void)foc_virtual_current_snapshot(&original, &final_view);
    printf(
        "{\"event\":\"BOOTSTRAP_CAPTURE\",\"samples\":%" PRIu32 ","
        "\"completed_us\":%" PRIu32 ",\"accepted_updates\":%" PRIu32 ",\"faults\":%" PRIu32 ","
        "\"output_inhibited\":%s}\n",
        capture.count,
        capture.completed_us,
        original.output.accepted_updates,
        final_view.faults,
        original.output.mode == FOC_VIRTUAL_OUTPUT_HIGH_Z ? "true" : "false"
    );
    if (acquired != 0 || !original_safe) {
        const char *reason = gate.failure != NULL
            ? gate.failure
            : foc_virtual_bootstrap_capture_reason_name(capture.reason);
        return foc_virtual_cal_cli_error(reason, "BOOTSTRAP_SCAN", acquired);
    }
    const struct foc_bootstrap_config fit_policy = {
        .identity = identity,
        .capture = capture_policy.capture,
        .source = FOC_CAL_WORKFLOW_HOST_VIRTUAL_MEASUREMENT,
        .minimum_pole_pairs = 1U,
        .maximum_pole_pairs = 14U,
        .expected_pole_pairs = identity.acquisition.pole_pairs,
        .expected_encoder_direction = identity.acquisition.encoder_direction,
        .minimum_samples_per_sweep = 40U,
        .maximum_interval_us = 5000U,
        .minimum_mechanical_span_rad = 0.7f,
        .maximum_residual_rad = 0.085f,
        .minimum_candidate_separation_rad = 0.08f,
        .maximum_offset_disagreement_rad = 0.10f,
        .minimum_emf_v = 0.04f,
        .minimum_abs_speed_rad_s = 1.0f,
        .maximum_abs_speed_rad_s = 10.0f,
        .maximum_motion_error_rad = 0.003f,
    };
    struct foc_bootstrap_result result;
    status = foc_bootstrap_fit(&fit_policy, capture.samples, capture.count, &result);
    if (status != FOC_BOOTSTRAP_REFERENCE_REVIEW_REQUIRED ||
        result.quality != FOC_BOOTSTRAP_COARSE_ELECTRICAL_REFERENCE) {
        return foc_virtual_cal_cli_error(status_name(status), "BOOTSTRAP_FIT", (int)status);
    }
    request->identity.acquisition.pole_pairs = result.pole_pairs;
    request->identity.acquisition.encoder_direction = result.encoder_direction;
    profile->encoder.direction = (enum foc_encoder_direction)result.encoder_direction;
    profile->runtime.motion.rotor.pole_pairs = result.pole_pairs;
    const uint32_t binding[] = {
        identity.motor_setup_id,
        identity.encoder_id,
        identity.mechanical_frame_id,
        identity.acquisition.phase_order,
        result.capture,
        result.pole_pairs,
        (uint32_t)result.encoder_direction
    };
    const float metrics[] = {
        result.electrical_offset_rad,
        result.maximum_residual_rad,
        result.minimum_candidate_separation_rad,
        result.minimum_mechanical_span_rad,
        result.maximum_offset_disagreement_rad
    };
    const uint32_t crc = crc32_ieee(UINT32_MAX, (const uint8_t *)binding, sizeof binding);
    *signature = crc32_ieee(crc, (const uint8_t *)metrics, sizeof metrics) ^ UINT32_MAX;
    printf(
        "{\"event\":\"BOOTSTRAP_COARSE_REFERENCE\",\"status\":\"%s\","
        "\"quality\":\"COARSE_ELECTRICAL_REFERENCE\",\"source\":\"HOST_OPEN_CIRCUIT_EMF\","
        "\"motor\":%" PRIu32 ",\"encoder\":%" PRIu32 ",\"frame\":%" PRIu32 ","
        "\"phase_order\":%" PRIu32 ",\"capture\":%" PRIu32 ",\"pole_pairs\":%u,"
        "\"encoder_direction\":%d,\"offset_rad\":%.9g,\"max_residual_rad\":%.9g,"
        "\"candidate_separation_rad\":%.9g,\"span_rad\":%.9g,\"offset_disagreement_rad\":%.9g,"
        "\"runtime_applicable\":false}\n",
        status_name(status),
        identity.motor_setup_id,
        identity.encoder_id,
        identity.mechanical_frame_id,
        identity.acquisition.phase_order,
        result.capture,
        (unsigned int)result.pole_pairs,
        (int)result.encoder_direction,
        (double)metrics[0],
        (double)metrics[1],
        (double)metrics[2],
        (double)metrics[3],
        (double)metrics[4]
    );
    return 0;
}

int foc_virtual_cal_cli_bootstrap(
    const char *mode,
    const char *scenario,
    const char *trace,
    struct foc_virtual_current_profile *profile,
    struct foc_cal_workflow_request *request,
    uint32_t *signature
)
{
    return foc_virtual_cal_cli_bootstrap_sampled(mode, scenario, trace, profile, request, signature,
                                                NULL);
}

/* ---------------------------------------- end of file ----------------------------------------- */

