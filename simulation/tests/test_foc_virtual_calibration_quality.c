/**
 * @file    test_foc_virtual_calibration_quality.c
 * @brief   Execute a seeded Host calibration and independent held-out trials.
 * @version v0.1.0
 * @date    2026.11.20
 *
 * Test role
 * ---------
 * Reuse the real Bootstrap, Workflow and Control Plane application. Keep
 * ideal plant values and held-out observations on the evaluation side.
 */

/* Includes --------------------------------------------------------------------- */

#include "falconfoc_virtual_calibrate_internal.h"
#include "foc_virtual_calibration_trial.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* Private Constants ------------------------------------------------------------ */

static const double TAU = 6.28318530717958647692;
static const struct foc_cal_application_tuning_policy policy = {
    .current = {130.0f, 0.00036f, 0.00026f, 0.6f, 1.25f},
    .max_rs_relative_difference = 0.05f,
    .velocity_natural_frequency_rad_s = 19.0f,
    .velocity_damping_ratio = 0.8f,
    .position_bandwidth_rad_s = 3.6f,
    .outer_extra_delay_s = 0.0006f,
    .tune_outer = true,
};

/* Private Types ---------------------------------------------------------------- */

struct quality_files {
    FILE *training;
    FILE *validation;
    FILE *control;
};

struct quality_application {
    const struct foc_cal_application *binding;
    const struct foc_cal_application_candidate *candidate;
    struct foc_cal_application_sensor sensor;
};

/* Private Helpers -------------------------------------------------------------- */

static void measure(const char *name, double estimate, double oracle)
{
    printf("{\"event\":\"MEASUREMENT\",\"name\":\"%s\",\"estimate\":%.12g,\"oracle\":%.12g}\n",
           name, estimate, oracle);
}

static void record_workflow(
    void *argument,
    enum foc_virtual_calibration_stage stage,
    const struct foc_cal_workflow_sample *sample
)
{
    FILE *file = argument;
    const char *name = foc_virtual_calibration_stage_name(stage);
    if (stage == FOC_VIRTUAL_CAL_ADC) {
        const struct foc_calibration_adc_sample *row = &sample->data.adc;
        fprintf(file, "%s,%u,%u,%u,%u,%u,0,%u,%u,%u,0,0,0\n", name, sample->session, sample->capture,
                row->sequence, row->captured_us, row->captured_us, row->phase_a, row->phase_b,
                row->phase_c);
    } else if (stage == FOC_VIRTUAL_CAL_LUT) {
        const struct foc_encoder_cal_sample *row = &sample->data.lut;
        fprintf(file, "%s,%u,%u,%u,%u,%u,%u,%.12g,0,0,0,0,0\n", name, sample->session,
                sample->capture, row->sequence, row->encoder_us, row->reference_us, row->raw_code,
                row->reference_rad);
    } else if (stage == FOC_VIRTUAL_CAL_ROTOR || stage == FOC_VIRTUAL_CAL_OFFSET) {
        const struct foc_calibration_angle_sample *row = &sample->data.angle;
        fprintf(file, "%s,%u,%u,%u,%u,%u,%u,%.12g,%.12g,0,0,0,0\n", name, sample->session,
                sample->capture, row->sequence, row->captured_us, row->captured_us,
                sample->encoder_raw_code, (double)row->mechanical_rad, (double)row->electrical_rad);
    } else if (stage == FOC_VIRTUAL_CAL_MECHANICAL) {
        const struct foc_mechanical_id_sample *row = &sample->data.mechanical;
        fprintf(file, "%s,%u,%u,%u,%u,%u,0,%.12g,%.12g,%.12g,0,0,0\n", name, sample->session,
                sample->capture, row->sequence, row->interval_begin_us, row->speed_us,
                (double)row->speed_rad_s, (double)row->torque_nm, (double)row->load_nm);
    }
}

static void record_rl(void *argument, enum foc_motor_id_axis axis,
                      const struct foc_motor_id_sample *row)
{
    FILE *file = argument;
    fprintf(file, "%s,0,%u,%u,%u,%u,0,%.12g,%.12g,0,0,0,0\n",
            axis == FOC_MOTOR_ID_D ? "ELECTRICAL_D" : "ELECTRICAL_Q", row->capture_id,
            row->sequence, row->voltage_begin_us, row->captured_us, (double)row->current_a,
            (double)row->mean_voltage_v);
}

static void record_flux(void *argument, const struct foc_flux_id_interval *row)
{
    FILE *file = argument;
    fprintf(file, "FLUX,%u,%u,%u,%u,%u,0,%.12g,%.12g,%.12g,%.12g,%.12g,%.12g\n",
            row->identity.session, row->identity.capture, row->sequence, row->begin_us, row->end_us,
            row->id_begin_a, row->iq_begin_a, row->id_end_a, row->iq_end_a,
            row->mean_omega_e_rad_s, row->mean_vq_v);
}

static void measure_parameters(
    const struct foc_virtual_current_profile *profile,
    const struct foc_cal_workflow_bundle *bundle,
    const struct foc_flux_id_result *flux,
    const struct foc_cal_application_candidate *candidate
)
{
    const struct foc_virtual_motor_params *motor = &profile->motor;
    const struct foc_cal_application_gains *gains = &candidate->gains;
    const double ke = motor->pole_pairs * motor->psi_f_wb;
    const double kt = 1.5 * ke;
    const double omega_current = TAU * 130.0;
    const double damping_velocity = 2.0 * 0.8 * 19.0 * motor->inertia_kg_m2 - motor->damping_nm_s;
    measure("adc_a_counts", bundle->adc.offset_a_counts, profile->adc.phase_a_offset_count);
    measure("adc_b_counts", bundle->adc.offset_b_counts, profile->adc.phase_b_offset_count);
    measure("adc_c_counts", bundle->adc.offset_c_counts, profile->adc.phase_c_offset_count);
    measure("lut_relative_direction", bundle->rotor.direction, 1.0);
    measure("offset_rad", bundle->offset.electrical_offset_rad, motor->electrical_offset_rad);
    measure("rs_ohm", bundle->electrical_model.rs_ohm, motor->rs_ohm);
    measure("rs_d_ohm", bundle->electrical.rs_ohm, motor->rs_ohm);
    measure("rs_q_ohm", bundle->electrical_q.rs_ohm, motor->rs_ohm);
    measure("ld_h", bundle->electrical_model.ld_h, motor->ld_h);
    measure("lq_h", bundle->electrical_model.lq_h, motor->lq_h);
    measure("flux_wb", flux->flux_wb, motor->psi_f_wb);
    measure("flux_positive_wb", flux->positive.flux_wb, motor->psi_f_wb);
    measure("flux_negative_wb", flux->negative.flux_wb, motor->psi_f_wb);
    measure("ke", flux->ke_phase_peak_v_per_mechanical_rad_s, ke);
    measure("kt", flux->kt_nm_per_peak_q_amp, kt);
    measure("j_kg_m2", bundle->mechanical.inertia_kg_m2, motor->inertia_kg_m2);
    measure("b_nm_s_rad", bundle->mechanical.damping_nm_s, motor->damping_nm_s);
    measure("current_kp_d", gains->current.kp_d_v_per_a, omega_current * motor->ld_h);
    measure("current_kp_q", gains->current.kp_q_v_per_a, omega_current * motor->lq_h);
    measure("current_ki", gains->current.ki_v_per_a_s, omega_current * motor->rs_ohm);
    measure("velocity_kp", gains->velocity.kp_a_per_rad_s, damping_velocity / kt);
    measure("velocity_ki", gains->velocity.ki_a_per_rad, 19.0 * 19.0 * motor->inertia_kg_m2 / kt);
    measure("position_kp", gains->position.kp_per_s, 3.6);
    measure("impedance_k", gains->impedance.stiffness_nm_per_rad, 15.0 * 15.0 * motor->inertia_kg_m2);
    measure("impedance_d", gains->impedance.damping_nm_s_per_rad,
            2.0 * 0.8 * 15.0 * motor->inertia_kg_m2 - motor->damping_nm_s);
    measure("impedance_filter_hz", candidate->runtime.config.motion.impedance.damping_filter_hz,
            48.0);
}

static bool validate_lut(
    const struct foc_virtual_current_profile *profile,
    const struct foc_cal_workflow_bundle *bundle,
    const struct foc_virtual_cal_trial *training,
    FILE *file
)
{
    struct foc_encoder_lut lut;
    struct foc_encoder_context encoder;
    if (foc_encoder_cal_make_lut(&bundle->lut, &lut) != FOC_ENCODER_CAL_OK ||
        foc_encoder_init_with_lut(&encoder, &profile->encoder, bundle->request.identity.encoder_id,
                                  &lut) != FOC_ENCODER_STATUS_OK) {
        return false;
    }
    /* Separate Plant instances, direction, starting angles, aperture grids
     * and speeds; the candidate can only read raw encoder observations. */
    for (int direction = -1; direction <= 1; direction += 2) {
        struct foc_virtual_motor plant;
        const struct foc_virtual_motor_state initial = {
            .mechanical_position_rad = training->initial_position_rad + 1.37 * direction,
        };
        const uint32_t capture = 100U + (direction > 0);
        if (foc_virtual_motor_init(&plant, &profile->motor, &initial) != 0 ||
            foc_virtual_motor_set_fixture_speed(&plant, direction > 0 ? 9.3 : -5.1) != 0) {
            return false;
        }
        for (uint32_t i = 0U; i < 600U; ++i) {
            uint16_t raw;
            uint32_t fixed;
            struct foc_virtual_motor_observation actual;
            if (foc_virtual_motor_step_open_circuit(&plant, 0.0, 0.0, 0.002173) != 0 ||
                foc_virtual_current_acquire_encoder(profile, &plant, &raw) != 0 ||
                foc_encoder_correct_code(&encoder, raw, &fixed) != FOC_ENCODER_STATUS_OK ||
                foc_virtual_motor_observe(&plant, &actual) != 0) {
                return false;
            }
            const double scale = TAU * profile->encoder.direction /
                profile->encoder.counts_per_revolution;
            const double raw_rad = raw * scale;
            const double fixed_rad = fixed * scale;
            const double phi = fixed_rad * bundle->rotor.pole_pairs +
                bundle->offset.electrical_offset_rad;
            fprintf(file, "%u,%u,%u,%d,%.12g,%.12g,%.12g,%.12g,%u,%u\n", capture, i,
                    training->started_us + 35023U + (i + 1U) * 2173U, direction,
                    actual.mechanical_position_rad, raw_rad, fixed_rad,
                    remainder(phi - actual.electrical_position_rad, TAU), raw, fixed);
        }
    }
    return !ferror(file);
}

static int initialize(
    void *argument,
    struct foc_runtime *runtime,
    struct foc_encoder_context *encoder,
    const struct foc_pipeline_config *pipeline,
    const struct foc_virtual_current_profile *profile
)
{
    struct quality_application *application = argument;
    const struct foc_cal_application_output output = {pipeline->output_submit, pipeline->output_arg};
    (void)profile;
    const int applied = foc_cal_application_apply_with_output(application->binding, &policy,
        &application->candidate->gains, &output, runtime, &application->sensor, 0U);
    if (applied != FOC_CAL_APPLICATION_APPLIED) {
        return applied;
    }
    *encoder = application->sensor.encoder;
    return 0;
}

static bool lifecycle(struct foc_virtual_current *driver, enum foc_runtime_mode mode,
                      enum foc_runtime_action action, uint32_t until)
{
    struct foc_runtime_lifecycle_result result;
    return foc_virtual_current_publish_lifecycle(driver, action, mode) == FOC_RUNTIME_OK &&
        foc_virtual_current_run_until(driver, until) == 0 &&
        foc_virtual_current_read_lifecycle(driver, &result) == FOC_RUNTIME_OK &&
        result.result == FOC_RUNTIME_REQUEST_APPLIED;
}

static int publish(struct foc_virtual_current *driver, enum foc_runtime_mode mode, float target,
                   const struct foc_cal_application_gains *gains)
{
    if (mode == FOC_RUNTIME_MODE_CURRENT) {
        return foc_virtual_current_publish_iq(driver, 0.0f, target);
    }
    if (mode != FOC_RUNTIME_MODE_IMPEDANCE) {
        return foc_virtual_current_publish_motion(driver, mode, target);
    }
    struct foc_runtime_snapshot snapshot;
    if (foc_virtual_current_snapshot(driver, &snapshot) != FOC_RUNTIME_OK) {
        return FOC_RUNTIME_ERR_BUSY;
    }
    const struct foc_runtime_command command = {
        .mode = mode,
        .seq = ++driver->command_seq,
        .produced_us = driver->virtual_time_us,
        .target.impedance = {
            .position_rad = target,
            .stiffness_nm_per_rad = gains->impedance.stiffness_nm_per_rad,
            .damping_nm_s_per_rad = gains->impedance.damping_nm_s_per_rad,
            .mechanical_frame_id = driver->runtime.config.motion.impedance.mechanical_frame_id,
            .source_session = snapshot.motion_session,
        },
    };
    return foc_runtime_publish_impedance_command(&driver->runtime, &command,
                                                  driver->virtual_time_us);
}

static bool control_trial(
    const struct foc_virtual_current_profile *profile,
    const struct foc_cal_application *binding,
    const struct foc_cal_application_candidate *candidate,
    const struct foc_virtual_cal_trial *training,
    enum foc_runtime_mode mode,
    int direction,
    FILE *file
)
{
    static const uint32_t durations[] = {80000U, 550000U, 1400000U, 850000U};
    static const double targets[] = {0.37, 3.7, 0.19, 0.105};
    struct foc_virtual_current_profile holdout = *profile;
    holdout.initial_state.mechanical_position_rad =
        -0.83 + 0.16 * (double)(training->seed % 9U) + 0.23 * direction;
    const double scale = 1.0 + 0.04 * (double)(training->seed % 5U);
    const double start_rad = holdout.initial_state.mechanical_position_rad;
    const uint32_t start_us = 1800U;
    const uint32_t duration = durations[mode];
    const float target = (float)(direction * targets[mode] * scale +
        (mode == FOC_RUNTIME_MODE_POSITION || mode == FOC_RUNTIME_MODE_IMPEDANCE ? start_rad : 0.0));
    struct foc_virtual_current driver = {0};
    struct quality_application application = {.binding = binding, .candidate = candidate};
    if (foc_virtual_current_init_with(&driver, &holdout, initialize, &application) != 0 ||
        (mode != FOC_RUNTIME_MODE_CURRENT &&
         !lifecycle(&driver, mode, FOC_RUNTIME_ACTION_SET_MODE, 0U)) ||
        foc_virtual_current_run_until(&driver, 700U) != 0 ||
        publish(&driver, mode, target, &candidate->gains) != FOC_RUNTIME_OK ||
        foc_virtual_current_run_until(&driver, 1300U) != 0 ||
        !lifecycle(&driver, mode, FOC_RUNTIME_ACTION_ARM, 1400U) ||
        publish(&driver, mode, target, &candidate->gains) != FOC_RUNTIME_OK ||
        foc_virtual_current_run_until(&driver, 1600U) != 0 ||
        !lifecycle(&driver, mode, FOC_RUNTIME_ACTION_START, start_us)) {
        return false;
    }

    for (uint32_t elapsed = 1000U; elapsed <= duration; elapsed += 1000U) {
        const double load = elapsed > duration / 3U && elapsed <= 2U * duration / 3U
            ? (double)direction * 0.0019 : 0.0;
        struct foc_virtual_current_observation sample;
        struct foc_virtual_motor_observation actual;
        struct foc_runtime_snapshot snapshot;
        if (foc_virtual_motor_set_load(&driver.motor, load) != 0 ||
            publish(&driver, mode, target, &candidate->gains) != FOC_RUNTIME_OK ||
            foc_virtual_current_run_until(&driver, start_us + elapsed) != 0 ||
            foc_virtual_current_observe(&driver, &sample) != 0 ||
            foc_virtual_motor_observe(&driver.motor, &actual) != 0 ||
            foc_virtual_current_snapshot(&driver, &snapshot) != FOC_RUNTIME_OK ||
            snapshot.state != FOC_SAFE_STATE_RUNNING || !snapshot.output_confirmed ||
            snapshot.action != FOC_DEVICE_OUTPUT_UPDATE || snapshot.faults != 0U ||
            driver.fault_events.last_accepted_update_us != start_us + elapsed) {
            return false;
        }
        fprintf(file,
                "%d,%d,%u,%u,%.12g,%.12g,%.12g,%.12g,%.12g,%.12g,%.12g,%.12g,%.12g,%.12g,%u,%u\n",
                mode, direction, elapsed, snapshot.motion_session, (double)target, start_rad,
                actual.id_a, actual.iq_a, (double)sample.iq_ref_a, actual.mechanical_position_rad,
                actual.mechanical_velocity_rad_s, actual.electromagnetic_torque_nm, load,
                hypot(sample.v_alpha_v, sample.v_beta_v), sample.accepted_updates,
                sample.saturated_updates);
    }
    const uint32_t updates = driver.output.accepted_updates;
    struct foc_runtime_snapshot stopped;
    return foc_virtual_current_request_stop(&driver) == 0 &&
        foc_virtual_current_run_until(&driver, driver.virtual_time_us + 1000U) == 0 &&
        foc_virtual_current_snapshot(&driver, &stopped) == FOC_RUNTIME_OK &&
        stopped.state == FOC_SAFE_STATE_DISABLED && stopped.output_confirmed &&
        stopped.action == FOC_DEVICE_OUTPUT_INHIBIT && driver.output.accepted_updates == updates &&
        !ferror(file);
}

static int run_trial(const char *name, uint32_t session_id, const char *boot_path,
                     const struct foc_virtual_cal_trial *trial, const struct quality_files *files)
{
    struct foc_virtual_current_profile profile;
    struct foc_cal_workflow_request request;
    if (foc_virtual_calibration_profile(name, &profile, &request) != FOC_VIRTUAL_CAL_OK) {
        return 2;
    }
    request.session = session_id;
    measure("offset_oracle_rad", profile.motor.electrical_offset_rad,
            profile.motor.electrical_offset_rad);
    measure("pole_pairs_oracle", profile.motor.pole_pairs, profile.motor.pole_pairs);
    measure("encoder_direction_oracle", profile.encoder.direction, profile.encoder.direction);
    const uint32_t phase_order = request.identity.acquisition.phase_order;
    const uint32_t encoder_id = request.identity.encoder_id;
    const uint32_t motor_id = request.identity.motor_setup_id;
    const uint32_t frame_id = request.identity.mechanical_frame_id;
    uint32_t signature;
    if (foc_virtual_cal_cli_bootstrap_sampled("unknown", "nominal", boot_path, &profile, &request,
                                              &signature, trial) != 0) {
        return 2;
    }
    if (request.identity.acquisition.phase_order != phase_order ||
        request.identity.encoder_id != encoder_id || request.identity.motor_setup_id != motor_id ||
        request.identity.mechanical_frame_id != frame_id) {
        return foc_virtual_cal_cli_error("IDENTITY_CHANGED", "BOOTSTRAP", -1);
    }
    struct foc_virtual_calibration session = {0};
    struct foc_flux_id_result flux;
    const struct foc_virtual_flux_options options = {
        .rl_revision = session_id, .poles_revision = session_id, .current_revision = session_id,
        .voltage_chain = 1U, .angle_reference = 402U, .interval = record_flux,
        .argument = files->training,
    };
    int status = foc_virtual_calibration_start(&session, &profile, &request);
    session.electrical.rl_endpoint = record_rl;
    session.electrical.rl_endpoint_argument = files->training;
    while (status == FOC_VIRTUAL_CAL_PENDING) {
        const enum foc_virtual_calibration_stage stage = session.stage;
        status = foc_virtual_calibration_poll_sampled(&session, &options, &flux, trial);
        if (status < 0) {
            return foc_virtual_cal_cli_error("CAPTURE_FAILED", foc_virtual_calibration_stage_name(stage),
                                             session.native_status);
        }
    }
    struct foc_cal_application_candidate candidate;
    if (status != FOC_VIRTUAL_CAL_OK || !foc_virtual_flux_agrees(&session, &flux) ||
        foc_cal_workflow_review(&session.workflow, request.session, &request.identity, true) !=
            FOC_CAL_WORKFLOW_OK ||
        foc_cal_application_prepare_tuned(&session.application, &policy, &candidate) !=
            FOC_CAL_APPLICATION_PREPARED) {
        return foc_virtual_cal_cli_error("REVIEW_FAILED", "REVIEW", status);
    }
    measure_parameters(&profile, &session.workflow.result, &flux, &candidate);
    if (!validate_lut(&profile, &session.workflow.result, trial, files->validation)) {
        return foc_virtual_cal_cli_error("LUT_VALIDATION_FAILED", "HOLDOUT", -1);
    }
    for (enum foc_runtime_mode mode = FOC_RUNTIME_MODE_CURRENT;
         mode <= FOC_RUNTIME_MODE_IMPEDANCE; ++mode) {
        for (int direction = -1; direction <= 1; direction += 2) {
            if (!control_trial(&profile, &session.application, &candidate, trial, mode, direction,
                               files->control)) {
                return foc_virtual_cal_cli_error("CONTROL_HOLDOUT_FAILED", "HOLDOUT", mode);
            }
        }
    }
    if (session.original.output.accepted_updates != 0U ||
        session.original.output.mode != FOC_VIRTUAL_OUTPUT_HIGH_Z) {
        return foc_virtual_cal_cli_error("ORIGINAL_OUTPUT_CHANGED", "HOLDOUT", -1);
    }
    printf("{\"event\":\"TRIAL_COMPLETE\",\"session\":%u,\"boot_signature\":%u,"
           "\"training_last_us\":%u,\"original_updates\":0}\n", session_id, signature, session.time_us);
    return 0;
}

/* Public Functions ------------------------------------------------------------- */

int foc_virtual_cal_cli_error(const char *reason, const char *stage, int native)
{
    printf("{\"error\":\"%s\",\"stage\":\"%s\",\"native_status\":%d}\n", reason, stage, native);
    return 2;
}

int main(int argc, char **argv)
{
    if (argc != 11) {
        return 2;
    }
    const struct quality_files files = {
        .training = fopen(argv[8], "wb"),
        .validation = fopen(argv[9], "wb"),
        .control = fopen(argv[10], "wb"),
    };
    if (files.training == NULL || files.validation == NULL || files.control == NULL) {
        return 2;
    }
    fputs("stage,session,capture,sequence,begin_us,end_us,encoder,a,b,c,d,e,f\n", files.training);
    fputs("capture,sequence,time_us,direction,shaft_rad,raw_rad,corrected_rad,electrical_error_rad,"
          "raw_count,corrected_count\n", files.validation);
    fputs("mode,direction,elapsed_us,session,target,start_rad,id_a,iq_a,iq_ref_a,position_rad,"
          "speed_rad_s,torque_nm,load_nm,voltage_v,updates,saturated\n", files.control);

    const uint32_t session_id = (uint32_t)strtoul(argv[2], NULL, 10);
    const struct foc_virtual_cal_trial trial = {
        .seed = (uint32_t)strtoul(argv[3], NULL, 10),
        .started_us = (uint32_t)strtoul(argv[4], NULL, 10),
        .initial_position_rad = strtod(argv[5], NULL),
        .reverse_first = strtol(argv[6], NULL, 10) != 0,
        .sample = record_workflow,
        .argument = files.training,
    };
    const int status = run_trial(argv[1], session_id, argv[7], &trial, &files);
    const bool training_ok = fclose(files.training) == 0;
    const bool validation_ok = fclose(files.validation) == 0;
    const bool control_ok = fclose(files.control) == 0;
    return training_ok && validation_ok && control_ok ? status : 2;
}

/* ---------------------------------------- end of file ----------------------------------------- */

