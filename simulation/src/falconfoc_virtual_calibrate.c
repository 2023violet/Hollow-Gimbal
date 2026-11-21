/**
 * @file    falconfoc_virtual_calibrate.c
 * @brief   Host product entry for measured calibration, review and FOC comparison.
 * @version v0.1.0
 * @date    2026.11.19
 *
 * Implementation role
 * -------------------
 * Drive the Foreground calibration session and verify the existing Virtual
 * Current driver with a freshly initialized production Runtime. Report each
 * measurement and application phase as a JSON line.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_pmsm_reachability.h"
#include "foc_virtual_calibration.h"
#include "foc_virtual_calibration_package.h"
#include "foc_virtual_calibration_flux.h"
#include "foc_virtual_calibration_reference.h"
#include "falconfoc_virtual_calibrate_internal.h"
#include "foc_virtual_cogging.h"
#include "crc.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Private Types ---------------------------------------------------------------- */

struct current_stimulus {
    uint32_t change_us;
    float second_current_a;
    uint32_t load_on_us;
    uint32_t load_off_us;
    double load_nm;
};

struct cycle_trace {
    FILE *output;
    const char *group;
    const char *scenario;
    const struct current_stimulus *stimulus;
    bool startup;
};

struct control_metrics {
    double rms;
    double tail_rms;
    double overshoot;
    double peak_error;
    double tail_bias;
    uint32_t rise90_us;
    uint32_t max_sensor_age_us;
    uint32_t active_samples;
    uint32_t rejected_updates;
    uint32_t last_outside_us;
    uint32_t updates;
    uint32_t saturated;
    uint32_t faults;
    uint32_t virtual_us;
    bool stopped;
};

/* Private Constants ------------------------------------------------------------ */

const struct foc_cal_application_tuning_policy foc_virtual_cal_cli_tuning = {
    .current =
        {
            .bandwidth_hz = 130.0f,
            .sample_period_s = 0.00036f,
            .update_delay_s = 0.00026f,
            .initial_error_a = 0.6f,
            .available_voltage_v = 1.25f,
        },
    .max_rs_relative_difference = 0.05f,
    .velocity_natural_frequency_rad_s = 19.0f,
    .velocity_damping_ratio = 0.8f,
    .position_bandwidth_rad_s = 3.6f,
    .outer_extra_delay_s = 0.0006f,
    .tune_outer = true,
};

/* The 130 Hz Host policy fits omega*T <= 0.3 at 360 us and uses less
 * first-update voltage than the 6.0 V admitted bus and 2.4 V command cap can deliver. */
static const char *const mode_name[] = {"current", "velocity", "position", "impedance"};

/* Private Helpers -------------------------------------------------------------- */

static struct foc_cal_application_tuning_policy legacy_tuning(void)
{
    struct foc_cal_application_tuning_policy policy = foc_virtual_cal_cli_tuning;
    policy.current.bandwidth_hz = 36.0f;
    policy.current.available_voltage_v = 0.4f;
    return policy;
}

int foc_virtual_cal_cli_error(const char *reason, const char *stage, int native)
{
    printf("{\"error\":\"%s\",\"stage\":\"%s\",\"native_status\":%d}\n", reason, stage, native);
    return 2;
}

int foc_virtual_cal_cli_initialize(
    void *argument,
    struct foc_runtime *runtime,
    struct foc_encoder_context *encoder,
    const struct foc_pipeline_config *pipeline,
    const struct foc_virtual_current_profile *profile
)
{
    struct calibrated_init *init = argument;
    const struct foc_cal_application_output output = {
        .submit = pipeline->output_submit,
        .argument = pipeline->output_arg,
    };
    const int status = init->application != NULL ? foc_cal_application_apply_with_output(
                                                       init->application,
                                                       init->policy,
                                                       &init->candidate->gains,
                                                       &output,
                                                       runtime,
                                                       &init->sensor,
                                                       0U
                                                   )
                                                 : foc_cal_application_restore(
                                                       init->candidate,
                                                       init->candidate->gains.identity.encoder_id,
                                                       init->restored_lut,
                                                       &output,
                                                       runtime,
                                                       &init->sensor,
                                                       0U
                                                   );
    (void)profile;
    if (status != FOC_CAL_APPLICATION_APPLIED) {
        return status;
    }
    *encoder = init->sensor.encoder;
    return 0;
}

bool foc_virtual_cal_cli_readback(
    struct foc_virtual_current *driver,
    const struct foc_cal_application_candidate *candidate,
    bool emit
)
{
    struct foc_runtime_diagnostic view;
    struct foc_runtime *runtime = &driver->runtime;
    if (foc_runtime_diagnose(runtime, &view) != FOC_RUNTIME_OK) {
        return false;
    }
    const struct foc_device_adc_config *adc = &runtime->pipeline.adc_config;
    const struct foc_current_config *current = &runtime->pipeline.current.config;
    const struct foc_runtime_motion_config *motion = &view.effective.motion;
    const struct foc_runtime_motion_config *expected = &candidate->runtime.config.motion;
    const bool match = motion->rotor.pole_pairs == expected->rotor.pole_pairs &&
        motion->rotor.electrical_offset_rad == expected->rotor.electrical_offset_rad &&
        motion->velocity.kp_a_per_rad_s == expected->velocity.kp_a_per_rad_s &&
        motion->velocity.ki_a_per_rad == expected->velocity.ki_a_per_rad &&
        motion->position.kp_per_s == expected->position.kp_per_s &&
        motion->impedance.torque_constant_nm_per_a ==
            expected->impedance.torque_constant_nm_per_a &&
        motion->impedance.damping_filter_hz == expected->impedance.damping_filter_hz &&
        motion->impedance.mechanical_frame_id == expected->impedance.mechanical_frame_id &&
        motion->impedance.torque_limit_nm == expected->impedance.torque_limit_nm &&
        motion->iq_limit_a == expected->iq_limit_a &&
        adc->current_a_per_count == candidate->pipeline.adc.current_a_per_count &&
        adc->phase_order == candidate->pipeline.adc.phase_order &&
        adc->phase_a_offset_count == candidate->pipeline.adc.phase_a_offset_count &&
        adc->phase_b_offset_count == candidate->pipeline.adc.phase_b_offset_count &&
        adc->phase_c_offset_count == candidate->pipeline.adc.phase_c_offset_count &&
        current->kp_v_per_a.d == candidate->pipeline.current.kp_v_per_a.d &&
        current->kp_v_per_a.q == candidate->pipeline.current.kp_v_per_a.q &&
        current->ki_v_per_a_s.d == candidate->pipeline.current.ki_v_per_a_s.d &&
        current->ki_v_per_a_s.q == candidate->pipeline.current.ki_v_per_a_s.q &&
        driver->encoder.lut_offsets != NULL &&
        motion->cogging.enabled == expected->cogging.enabled && view.snapshot.output_confirmed &&
        view.snapshot.state == FOC_SAFE_STATE_DISABLED;
    if (match && emit) {
        printf(
            "{\"event\":\"CONFIGURATION_APPLIED\",\"adc\":[%.6f,%.6f,%.6f],"
            "\"pole_pairs\":%u,\"electrical_offset_rad\":%.8f,\"lut_nodes\":%" PRIu32 ","
            "\"pi\":[%.8f,%.8f,%.8f],\"outer\":[%.8f,%.8f,%.8f],\"kt\":%.8f,"
            "\"damping_filter_hz\":%.8g,\"cogging_enabled\":%s}\n",
            (double)adc->phase_a_offset_count,
            (double)adc->phase_b_offset_count,
            (double)adc->phase_c_offset_count,
            (unsigned)motion->rotor.pole_pairs,
            (double)motion->rotor.electrical_offset_rad,
            driver->encoder.lut_length,
            (double)current->kp_v_per_a.d,
            (double)current->kp_v_per_a.q,
            (double)current->ki_v_per_a_s.q,
            (double)motion->velocity.kp_a_per_rad_s,
            (double)motion->velocity.ki_a_per_rad,
            (double)motion->position.kp_per_s,
            (double)motion->impedance.torque_constant_nm_per_a,
            (double)motion->impedance.damping_filter_hz,
            motion->cogging.enabled ? "true" : "false"
        );
    }
    return match;
}

static int publish(
    struct foc_virtual_current *driver,
    enum foc_runtime_mode mode,
    float target,
    const struct foc_cal_application_gains *gains
)
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
            .stiffness_nm_per_rad = gains != NULL ? gains->impedance.stiffness_nm_per_rad : 0.5f,
            .damping_nm_s_per_rad = gains != NULL ? gains->impedance.damping_nm_s_per_rad : 0.05f,
            .mechanical_frame_id = driver->runtime.config.motion.impedance.mechanical_frame_id,
            .source_session = snapshot.motion_session,
        },
    };
    return foc_runtime_publish_impedance_command(
        &driver->runtime,
        &command,
        driver->virtual_time_us
    );
}

static bool lifecycle(
    struct foc_virtual_current *driver,
    enum foc_runtime_action action,
    enum foc_runtime_mode mode,
    uint32_t end_us
)
{
    struct foc_runtime_lifecycle_result receipt = {0};
    const int published = foc_virtual_current_publish_lifecycle(driver, action, mode);
    const int advanced =
        published == FOC_RUNTIME_OK ? foc_virtual_current_run_until(driver, end_us) : -1;
    const int result = advanced == 0 ? foc_virtual_current_read_lifecycle(driver, &receipt) : -1;
    if (result != FOC_RUNTIME_OK || receipt.result != FOC_RUNTIME_REQUEST_APPLIED) {
        fprintf(
            stderr,
            "lifecycle failed: mode=%d action=%d publish=%d advance=%d receipt=%d pipeline=%d "
            "time=%" PRIu32 "\n",
            mode,
            action,
            published,
            advanced,
            receipt.result,
            receipt.pipeline_status,
            driver->virtual_time_us
        );
        return false;
    }
    return true;
}

static int trace_cycle(
    const struct cycle_trace *trace,
    struct foc_virtual_current *driver,
    uint32_t elapsed,
    const struct foc_vector_dq *requested
)
{
    struct foc_runtime_snapshot view;
    struct foc_virtual_motor_observation plant;
    if (foc_virtual_current_snapshot(driver, &view) != FOC_RUNTIME_OK ||
        foc_virtual_motor_observe(&driver->motor, &plant) != 0) {
        return -1;
    }
    const struct foc_pipeline *pipeline = &driver->runtime.pipeline;
    const struct foc_current_config *cfg = &pipeline->current.config;
    const struct foc_device_pwm_request *output = &driver->output.request;
    const struct foc_runtime_sensor *sensor = &driver->runtime.observed_sensor;
    float aligned = 0.0f;
    const unsigned int slot =
        atomic_load_explicit(&driver->runtime.sensor_slot, memory_order_relaxed);
    const bool angle_valid = slot < 2U && view.sensor_valid &&
        foc_rotor_predict(
            &driver->runtime.config.motion.prediction,
            &driver->runtime.sensor_slots[slot].electrical,
            view.angle_prediction_us,
            &aligned
        ) == FOC_STATUS_OK;
    const double sin_angle = sin(aligned);
    const double cos_angle = cos(aligned);
    const double voltage_d =
        cos_angle * driver->output.v_alpha_v + sin_angle * driver->output.v_beta_v;
    const double voltage_q =
        cos_angle * driver->output.v_beta_v - sin_angle * driver->output.v_alpha_v;
    const double voltage = hypot(voltage_d, voltage_q);
    const double available = fmin(cfg->pwm.voltage_limit_v, driver->profile.vbus_v / sqrt(3.0));
    const bool active = view.state == FOC_SAFE_STATE_RUNNING && view.output_confirmed &&
        output->action == FOC_DEVICE_OUTPUT_UPDATE && driver->fault_events.last_update_seen &&
        driver->fault_events.last_accepted_update_us == view.trigger_us;
    return fprintf(
               trace->output,
               "%s,%s,%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ","
               "%u,%u,%u,%u,%" PRIu32 ",%" PRIu32 ",%u,"
               "%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,"
               "%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,"
               "%.9g,%.9g,%.9g,%.9g,%.9g,%.9g\n",
               trace->scenario,
               trace->group,
               elapsed,
               view.trigger_us,
               view.sample_us,
               driver->runtime.h1.trigger_us,
               view.sensor_us,
               view.angle_prediction_us,
               (unsigned)angle_valid,
               (unsigned)view.state,
               (unsigned)view.action,
               (unsigned)active,
               view.faults,
               view.motion_session,
               (unsigned)(active && voltage >= available - 0.002),
               (double)requested->d,
               (double)requested->q,
               (double)pipeline->measured_dq_a.d,
               (double)pipeline->measured_dq_a.q,
               plant.id_a,
               plant.iq_a,
               (double)aligned,
               plant.electrical_position_rad,
               (double)sensor->theta_elec_rad,
               (double)sensor->mechanical_rad,
               (double)sensor->mechanical_rad_s,
               plant.mechanical_position_rad,
               plant.mechanical_velocity_rad_s,
               voltage_d,
               voltage_q,
               (double)output->duty_a,
               (double)output->duty_b,
               (double)output->duty_c,
               (double)pipeline->current.integral_v.d,
               (double)pipeline->current.integral_v.q,
               (double)cfg->kp_v_per_a.d,
               (double)cfg->kp_v_per_a.q,
               (double)cfg->ki_v_per_a_s.d,
               (double)cfg->ki_v_per_a_s.q,
               driver->profile.vbus_v,
               available,
               plant.applied_load_torque_nm,
               (double)cfg->current_limit_a
           ) < 0
        ? -1
        : 0;
}

static int startup_cycle(
    const struct cycle_trace *trace,
    struct foc_virtual_current *driver,
    const struct foc_vector_dq *reference
)
{
    struct foc_runtime_snapshot view;
    struct foc_virtual_motor_observation plant;
    if (foc_virtual_current_snapshot(driver, &view) != FOC_RUNTIME_OK ||
        foc_virtual_motor_observe(&driver->motor, &plant) != 0) {
        return -1;
    }
    const struct foc_pipeline *pipeline = &driver->runtime.pipeline;
    const struct foc_pipeline_watermark *published = &driver->runtime.published_command;
    const bool applied = view.state == FOC_SAFE_STATE_RUNNING && view.output_confirmed &&
        view.action == FOC_DEVICE_OUTPUT_UPDATE && driver->fault_events.last_update_seen &&
        driver->fault_events.last_accepted_update_us == view.trigger_us;
    const double voltage = hypot(driver->output.v_alpha_v, driver->output.v_beta_v);
    const double cap =
        fmin(pipeline->current.config.pwm.voltage_limit_v, driver->profile.vbus_v / sqrt(3.0));
    /* The Host adapter applies the two voltage components in its accepted UPDATE call. */
    return fprintf(
               trace->output,
               "%s,%s,%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ","
               "%u,%" PRIu32 ",%" PRIu32 ",%u,%u,%u,%" PRIu32 ",%" PRIu32 ",%" PRIu32 ","
               "%u,%" PRIu32 ",%u,%u,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g\n",
               trace->scenario,
               trace->group,
               driver->virtual_time_us,
               view.trigger_us,
               view.sample_us,
               driver->runtime.h1.trigger_us,
               view.sensor_us,
               (unsigned)published->seen,
               published->produced_us,
               view.command_us,
               (unsigned)view.state,
               (unsigned)view.action,
               (unsigned)view.output_confirmed,
               view.faults,
               view.motion_session,
               driver->output.accepted_updates,
               (unsigned)applied,
               applied ? driver->fault_events.last_accepted_update_us : 0U,
               (unsigned)view.sample_seen,
               (unsigned)(applied && voltage >= cap - 0.002),
               (double)reference->q,
               (double)pipeline->measured_dq_a.q,
               plant.iq_a,
               voltage,
               cap,
               (double)pipeline->current.integral_v.d,
               (double)pipeline->current.integral_v.q,
               plant.mechanical_velocity_rad_s,
               driver->profile.vbus_v,
               (double)pipeline->current.config.current_limit_a
           ) < 0
        ? -1
        : 0;
}

static int advance_with_startup_trace(
    struct foc_virtual_current *driver,
    uint32_t through_us,
    const struct cycle_trace *trace
)
{
    if (trace != NULL && trace->startup) {
        while (driver->next_h0_us <= through_us) {
            if (foc_virtual_current_run_until(driver, driver->next_h0_us) != 0 ||
                startup_cycle(trace, driver, &driver->target_a) != 0) {
                return -1;
            }
        }
    }
    return foc_virtual_current_run_until(driver, through_us);
}

static int compare_run(
    const struct foc_virtual_current_profile *profile,
    struct calibrated_init *init,
    enum foc_runtime_mode mode,
    float target,
    uint32_t duration_us,
    double tolerance,
    struct control_metrics *result,
    const struct cycle_trace *trace
)
{
    struct foc_virtual_current driver = {0};
    const struct foc_cal_application_gains *gains = init != NULL ? &init->candidate->gains : NULL;
    struct calibrated_init trial = {0};
    if (init != NULL) {
        trial.application = init->application;
        trial.policy = init->policy;
        trial.candidate = init->candidate;
        trial.restored_lut = init->restored_lut;
    }
    const uint32_t start_us = 1800U;
    if (foc_virtual_current_init_with(
            &driver,
            profile,
            init != NULL ? foc_virtual_cal_cli_initialize : foc_virtual_calibration_baseline,
            &trial
        ) != 0 ||
        (mode != FOC_RUNTIME_MODE_CURRENT &&
         !lifecycle(&driver, FOC_RUNTIME_ACTION_SET_MODE, mode, 0U)) ||
        advance_with_startup_trace(&driver, 700U, trace) != 0 ||
        publish(&driver, mode, target, gains) != FOC_RUNTIME_OK ||
        advance_with_startup_trace(&driver, 1300U, trace) != 0 ||
        !lifecycle(&driver, FOC_RUNTIME_ACTION_ARM, mode, 1400U) ||
        (trace != NULL && trace->startup && startup_cycle(trace, &driver, &driver.target_a) != 0) ||
        publish(&driver, mode, target, gains) != FOC_RUNTIME_OK ||
        advance_with_startup_trace(&driver, 1600U, trace) != 0 ||
        !lifecycle(&driver, FOC_RUNTIME_ACTION_START, mode, start_us)) {
        return -1;
    }
    *result = (struct control_metrics){0};
    unsigned int tail_count = 0U;
    if (trace != NULL) {
        const struct foc_vector_dq command = {0.0f, target};
        if ((trace->startup ? startup_cycle(trace, &driver, &command)
                            : trace_cycle(trace, &driver, 0U, &command)) != 0) {
            return -2;
        }
    }
    for (uint32_t elapsed = 1000U; elapsed <= duration_us; elapsed += 1000U) {
        struct foc_virtual_current_observation sample;
        const uint32_t interval_begin = elapsed - 1000U;
        const struct current_stimulus *stimulus = trace != NULL ? trace->stimulus : NULL;
        const float reference =
            stimulus != NULL && stimulus->change_us != 0U && interval_begin >= stimulus->change_us
            ? stimulus->second_current_a
            : target;
        if (stimulus != NULL && stimulus->load_on_us != 0U &&
            (interval_begin == stimulus->load_on_us || interval_begin == stimulus->load_off_us) &&
            foc_virtual_motor_set_load(
                &driver.motor,
                interval_begin == stimulus->load_on_us ? stimulus->load_nm : profile->load_torque_nm
            ) != 0) {
            return -2;
        }
        if (publish(&driver, mode, reference, gains) != FOC_RUNTIME_OK) {
            return -2;
        }
        if (trace != NULL) {
            for (uint32_t tick = elapsed - 800U; tick <= elapsed; tick += 200U) {
                const struct foc_vector_dq command = {0.0f, reference};
                if (foc_virtual_current_run_until(&driver, start_us + tick) != 0 ||
                    (trace->startup ? startup_cycle(trace, &driver, &command)
                                    : trace_cycle(trace, &driver, tick, &command)) != 0) {
                    return -2;
                }
            }
        } else if (foc_virtual_current_run_until(&driver, start_us + elapsed) != 0) {
            return -2;
        }
        if (foc_virtual_current_observe(&driver, &sample) != 0) {
            return -2;
        }
        const double value = mode == FOC_RUNTIME_MODE_CURRENT ? sample.iq_a
            : mode == FOC_RUNTIME_MODE_VELOCITY               ? sample.mechanical_velocity_rad_s
                                                              : sample.mechanical_position_rad;
        struct foc_runtime_snapshot view;
        if (foc_virtual_current_snapshot(&driver, &view) != FOC_RUNTIME_OK ||
            sample.safety_state != FOC_SAFE_STATE_RUNNING || !view.output_confirmed ||
            view.action != FOC_DEVICE_OUTPUT_UPDATE ||
            driver.fault_events.last_accepted_update_us != start_us + elapsed) {
            result->faults = sample.faults;
            return -3;
        }
        const double error = value - reference;
        const double delta = fabs(error);
        result->rms += error * error;
        result->active_samples++;
        result->peak_error = fmax(result->peak_error, delta);
        if (elapsed > duration_us * 3U / 4U) {
            result->tail_rms += error * error;
            result->tail_bias += error;
            tail_count++;
        }
        result->overshoot = fmax(result->overshoot, copysign(1.0, reference) * error);
        if (delta > tolerance) {
            result->last_outside_us = elapsed;
        }
        if (result->rise90_us == 0U && copysign(1.0, target) * value >= 0.9 * fabs(target)) {
            result->rise90_us = elapsed;
        }
        if (view.angle_prediction_us > result->max_sensor_age_us) {
            result->max_sensor_age_us = view.angle_prediction_us;
        }
    }
    result->rms = sqrt(result->rms / result->active_samples);
    result->tail_rms = sqrt(result->tail_rms / tail_count);
    result->tail_bias /= tail_count;
    result->rejected_updates = driver.output.rejected_updates;
    result->updates = driver.output.accepted_updates;
    result->saturated = driver.output.saturated_updates;
    if (foc_virtual_current_request_stop(&driver) != 0 ||
        advance_with_startup_trace(&driver, driver.virtual_time_us + 1000U, trace) != 0) {
        return -4;
    }
    struct foc_runtime_snapshot stopped;
    result->stopped = foc_virtual_current_snapshot(&driver, &stopped) == FOC_RUNTIME_OK &&
        stopped.state == FOC_SAFE_STATE_DISABLED && stopped.output_confirmed &&
        driver.output.mode == FOC_VIRTUAL_OUTPUT_HIGH_Z &&
        driver.output.accepted_updates == result->updates;
    if (trace != NULL && !trace->startup) {
        const struct foc_vector_dq revoked = {0};
        if (trace_cycle(trace, &driver, duration_us + 1000U, &revoked) != 0) {
            return -4;
        }
    }
    result->virtual_us = driver.virtual_time_us;
    return result->stopped && result->updates > 0U ? 0 : -5;
}

static bool verify_recovery(
    const struct foc_virtual_current_profile *profile,
    const struct calibrated_init *init,
    uint32_t *virtual_us
)
{
    struct calibrated_init trial = {
        .application = init->application,
        .policy = init->policy,
        .candidate = init->candidate,
        .restored_lut = init->restored_lut,
    };
    struct foc_virtual_current driver = {0};
    struct foc_runtime_snapshot view;
    const enum foc_runtime_mode mode = FOC_RUNTIME_MODE_CURRENT;
    if (foc_virtual_current_init_with(&driver, profile, foc_virtual_cal_cli_initialize, &trial) !=
            0 ||
        foc_virtual_current_run_until(&driver, 700U) != 0 ||
        foc_virtual_current_publish_iq(&driver, 0.0f, 0.25f) != FOC_RUNTIME_OK ||
        foc_virtual_current_run_until(&driver, 1300U) != 0 ||
        !lifecycle(&driver, FOC_RUNTIME_ACTION_ARM, mode, 1400U) ||
        !lifecycle(&driver, FOC_RUNTIME_ACTION_START, mode, 1600U) ||
        foc_virtual_current_run_until(&driver, 2000U) != 0 ||
        driver.output.accepted_updates == 0U ||
        foc_virtual_current_run_until(&driver, 4200U) != 0 ||
        foc_virtual_current_snapshot(&driver, &view) != FOC_RUNTIME_OK ||
        view.state != FOC_SAFE_STATE_FAULT_LATCHED ||
        view.faults != FOC_SAFE_FAULT_COMMAND_TIMEOUT ||
        driver.output.mode != FOC_VIRTUAL_OUTPUT_HIGH_Z) {
        return false;
    }
    const uint32_t fault_session = view.motion_session;
    const uint32_t fault_updates = driver.output.accepted_updates;
    if (foc_runtime_request_timeout_clear(&driver.runtime, 4200U) != FOC_RUNTIME_OK ||
        foc_virtual_current_run_until(&driver, 5200U) != 0 ||
        foc_virtual_current_snapshot(&driver, &view) != FOC_RUNTIME_OK ||
        view.state != FOC_SAFE_STATE_DISABLED || view.faults != 0U ||
        view.clear_result != FOC_RUNTIME_CLEAR_APPLIED || view.motion_session == fault_session ||
        driver.output.accepted_updates != fault_updates ||
        driver.output.mode != FOC_VIRTUAL_OUTPUT_HIGH_Z ||
        foc_virtual_current_publish_iq(&driver, 0.0f, -0.25f) != FOC_RUNTIME_OK ||
        foc_virtual_current_run_until(&driver, 5700U) != 0 ||
        !lifecycle(&driver, FOC_RUNTIME_ACTION_ARM, mode, 5800U) ||
        !lifecycle(&driver, FOC_RUNTIME_ACTION_START, mode, 6000U) ||
        foc_virtual_current_run_until(&driver, 6600U) != 0 ||
        foc_virtual_current_snapshot(&driver, &view) != FOC_RUNTIME_OK ||
        view.state != FOC_SAFE_STATE_RUNNING || driver.output.accepted_updates <= fault_updates) {
        return false;
    }
    const uint32_t recovered = driver.output.accepted_updates;
    if (foc_virtual_current_request_stop(&driver) != FOC_RUNTIME_OK ||
        foc_virtual_current_run_until(&driver, 7200U) != 0 ||
        foc_virtual_current_snapshot(&driver, &view) != FOC_RUNTIME_OK ||
        view.state != FOC_SAFE_STATE_DISABLED || !view.output_confirmed ||
        driver.output.mode != FOC_VIRTUAL_OUTPUT_HIGH_Z ||
        driver.output.accepted_updates != recovered) {
        return false;
    }
    printf(
        "{\"event\":\"FAULT_RECOVERY_VERIFIED\",\"fault\":\"COMMAND_TIMEOUT\","
        "\"fault_session\":%" PRIu32 ",\"accepted_updates_before_clear\":%" PRIu32
        ",\"accepted_updates_after_rearm\":%" PRIu32 ",\"stop_confirmed\":true}\n",
        fault_session,
        fault_updates,
        recovered
    );
    *virtual_us += driver.virtual_time_us;
    return true;
}

static bool verify_calibrated_torque_limit(
    const struct foc_virtual_current_profile *profile,
    const struct calibrated_init *init,
    uint32_t *virtual_us
)
{
    const enum foc_runtime_mode mode = FOC_RUNTIME_MODE_IMPEDANCE;
    for (int sign = -1; sign <= 1; sign += 2) {
        struct calibrated_init trial = {
            .application = init->application,
            .policy = init->policy,
            .candidate = init->candidate,
            .restored_lut = init->restored_lut,
        };
        struct foc_virtual_current driver = {0};
        const struct foc_cal_application_gains *gains = &init->candidate->gains;
        const float target = 20.0f * sign;
        if (foc_virtual_current_init_with(
                &driver,
                profile,
                foc_virtual_cal_cli_initialize,
                &trial
            ) != 0 ||
            !lifecycle(&driver, FOC_RUNTIME_ACTION_SET_MODE, mode, 0U) ||
            foc_virtual_current_run_until(&driver, 700U) != 0 ||
            publish(&driver, mode, target, gains) != FOC_RUNTIME_OK ||
            foc_virtual_current_run_until(&driver, 1300U) != 0 ||
            !lifecycle(&driver, FOC_RUNTIME_ACTION_ARM, mode, 1400U) ||
            publish(&driver, mode, target, gains) != FOC_RUNTIME_OK ||
            foc_virtual_current_run_until(&driver, 1600U) != 0 ||
            !lifecycle(&driver, FOC_RUNTIME_ACTION_START, mode, 1800U) ||
            publish(&driver, mode, target, gains) != FOC_RUNTIME_OK) {
            return false;
        }
        const struct foc_runtime_motion_config *motion = &driver.runtime.config.motion;
        const float kt = motion->impedance.torque_constant_nm_per_a;
        const float torque = motion->impedance.torque_limit_nm;
        const float cap = fminf(motion->iq_limit_a, torque / kt);
        const double load = 1.25 * torque * sign;
        struct foc_virtual_current_observation view = {0};
        const int load_status = foc_virtual_motor_set_load(&driver.motor, load);
        const int advance = foc_virtual_current_run_until(&driver, 2600U);
        const int observation = foc_virtual_current_observe(&driver, &view);
        if (load_status != 0 || advance != 0 || observation != 0 ||
            view.safety_state != FOC_SAFE_STATE_RUNNING || view.faults != 0U ||
            view.accepted_updates == 0U || sign * view.iq_ref_a < 0.85f * cap ||
            fabsf(view.iq_ref_a) > cap + 1e-6f) {
            fprintf(
                stderr,
                "torque-limit load=%d advance=%d observe=%d state=%d faults=%" PRIu32
                " updates=%" PRIu32 " target=%.8g cap=%.8g\n",
                load_status,
                advance,
                observation,
                view.safety_state,
                view.faults,
                view.accepted_updates,
                (double)view.iq_ref_a,
                (double)cap
            );
            return false;
        }
        if (foc_virtual_current_request_stop(&driver) != FOC_RUNTIME_OK ||
            foc_virtual_current_run_until(&driver, 3200U) != 0 ||
            driver.output.mode != FOC_VIRTUAL_OUTPUT_HIGH_Z ||
            driver.output.accepted_updates != view.accepted_updates) {
            fprintf(
                stderr,
                "torque-limit STOP mode=%d updates before=%" PRIu32 " after=%" PRIu32 "\n",
                driver.output.mode,
                view.accepted_updates,
                driver.output.accepted_updates
            );
            return false;
        }
        printf(
            "{\"event\":\"CALIBRATED_TORQUE_LIMIT_VERIFIED\",\"load_nm\":%.9g,"
            "\"kt\":%.9g,\"iq_ref_a\":%.9g,\"cap_a\":%.9g,\"stop_confirmed\":true}\n",
            load,
            (double)kt,
            (double)view.iq_ref_a,
            (double)cap
        );
        *virtual_us += driver.virtual_time_us;
    }
    return true;
}

static void print_metrics(
    enum foc_runtime_mode mode,
    const char *config,
    float ref,
    const struct control_metrics *result,
    int status
)
{
    printf(
        "{\"event\":\"CONTROL_TRIAL\",\"mode\":\"%s\",\"configuration\":\"%s\","
        "\"reference\":%.6g,\"status\":%d,\"rms\":%.8g,\"tail_rms\":%.8g,"
        "\"overshoot\":%.8g,\"peak_error\":%.8g,\"tail_bias\":%.8g,"
        "\"rise90_by_us\":%" PRIu32 ",\"max_sensor_age_at_metric_sample_us\":%" PRIu32 ","
        "\"active_samples\":%" PRIu32 ",\"last_outside_us\":%" PRIu32 ",\"updates\":%" PRIu32 ","
        "\"rejected_updates\":%" PRIu32 ",\"saturated_updates\":%" PRIu32 ",\"faults\":%" PRIu32
        ",\"stop_confirmed\":%s}\n",
        mode_name[mode],
        config,
        (double)ref,
        status,
        result->rms,
        result->tail_rms,
        result->overshoot,
        result->peak_error,
        result->tail_bias,
        result->rise90_us,
        result->max_sensor_age_us,
        result->active_samples,
        result->last_outside_us,
        result->updates,
        result->rejected_updates,
        result->saturated,
        result->faults,
        result->stopped ? "true" : "false"
    );
}

static bool verify_control(
    const struct foc_virtual_current_profile *profile,
    struct calibrated_init *init,
    int64_t *virtual_us
)
{
    /* New small-signal acceptance cases. The installed historical suites retain
     * their own references, horizons and acceptance thresholds. */
    static const struct {
        float reference;
        uint32_t duration_us;
        double tolerance;
        double overshoot;
    } cases[] = {
        {0.45f, 60000U, 0.04, 0.15},
        {1.5f, 700000U, 0.12, 0.65},
        {0.25f, 1400000U, 0.025, 0.06},
        {0.18f, 1000000U, 0.025, 0.06}
    };
    bool pass = true;
    double baseline_current_rms = 0.0;
    double applied_current_rms = 0.0;
    for (unsigned int i = 0U; i < sizeof cases / sizeof cases[0]; ++i) {
        for (int sign = -1; sign <= 1; sign += 2) {
            const enum foc_runtime_mode mode = (enum foc_runtime_mode)i;
            const float ref = (float)sign * cases[i].reference;
            struct control_metrics baseline = {0}, tuned = {0};
            const int base_status = compare_run(
                profile,
                NULL,
                mode,
                ref,
                cases[i].duration_us,
                cases[i].tolerance,
                &baseline,
                NULL
            );
            const int tuned_status = compare_run(
                profile,
                init,
                mode,
                ref,
                cases[i].duration_us,
                cases[i].tolerance,
                &tuned,
                NULL
            );
            if (mode == FOC_RUNTIME_MODE_CURRENT) {
                baseline_current_rms = fmax(baseline_current_rms, baseline.rms);
                applied_current_rms = fmax(applied_current_rms, tuned.rms);
            }
            print_metrics(mode, "before", ref, &baseline, base_status);
            print_metrics(mode, "after", ref, &tuned, tuned_status);
            if (baseline.virtual_us == 0U || tuned.virtual_us == 0U) {
                *virtual_us = -1;
            } else if (*virtual_us >= 0) {
                *virtual_us += baseline.virtual_us + tuned.virtual_us;
            }
            if (base_status != 0 || tuned_status != 0 || tuned.tail_rms > cases[i].tolerance ||
                tuned.overshoot > cases[i].overshoot ||
                tuned.last_outside_us >= cases[i].duration_us * 3U / 4U) {
                pass = false;
            }
        }
    }
    const bool benefit = applied_current_rms < baseline_current_rms;
    const bool recommended =
        init->policy->current.bandwidth_hz == foc_virtual_cal_cli_tuning.current.bandwidth_hz;
    printf(
        "{\"event\":\"CURRENT_BENEFIT_%s\",\"policy_hz\":%.6g,"
        "\"baseline_worst_rms_a\":%.8g,\"applied_worst_rms_a\":%.8g}\n",
        benefit ? "PASS" : "FAIL",
        (double)init->policy->current.bandwidth_hz,
        baseline_current_rms,
        applied_current_rms
    );
    return pass && (!recommended || benefit);
}

static int study_trial(
    const struct foc_virtual_current_profile *profile,
    struct calibrated_init *init,
    const struct cycle_trace *trace,
    float reference
)
{
    struct control_metrics metrics = {0};
    const int status = compare_run(
        profile,
        init,
        FOC_RUNTIME_MODE_CURRENT,
        reference,
        60000U,
        0.04,
        &metrics,
        trace
    );
    print_metrics(FOC_RUNTIME_MODE_CURRENT, trace->group, reference, &metrics, status);
    return status;
}

static void
set_current_gains(struct foc_current_config *current, const struct foc_motor_pi_candidate *gains)
{
    current->kp_v_per_a = (struct foc_vector_dq){gains->kp_d_v_per_a, gains->kp_q_v_per_a};
    current->ki_v_per_a_s = (struct foc_vector_dq){gains->ki_v_per_a_s, gains->ki_v_per_a_s};
}

static int current_holdout(
    FILE *output,
    const struct foc_virtual_current_profile *original,
    const struct foc_cal_application_candidate *full,
    const struct foc_encoder_lut *lut
)
{
    static const char *const names[] =
        {"small_reversal", "low_bus_reverse_exit", "plant_mismatch_load"};
    const struct current_stimulus stimuli[] = {
        {.change_us = 20000U, .second_current_a = -0.15f},
        {.change_us = 30000U, .second_current_a = -0.35f},
        {.load_on_us = 20000U, .load_off_us = 40000U, .load_nm = 0.02},
    };
    const float references[] = {0.15f, 2.20f, 0.38f};
    const struct foc_motor_pi_request previous = legacy_tuning().current;
    struct foc_motor_pi_candidate legacy_gains;
    if (foc_motor_pi_design(&full->gains.model, &previous, &legacy_gains) != FOC_MOTOR_ID_OK) {
        return -1;
    }
    for (unsigned int i = 0U; i < sizeof names / sizeof names[0]; ++i) {
        struct foc_virtual_current_profile profile = *original;
        struct foc_cal_application_candidate variant = *full;
        struct calibrated_init init = {.candidate = &variant, .restored_lut = lut};
        if (i == 1U) {
            profile.vbus_v = 6.1f;
            variant.pipeline.current.pwm.voltage_limit_v = 0.72f;
        } else if (i == 2U) {
            profile.motor.rs_ohm *= 1.25;
            profile.motor.ld_h *= 0.80;
            profile.motor.lq_h *= 1.25;
            profile.motor.psi_f_wb *= 0.85;
            profile.motor.inertia_kg_m2 *= 1.25;
            profile.motor.damping_nm_s *= 0.75;
            profile.motor.electrical_offset_rad += 0.07;
            profile.initial_state.mechanical_velocity_rad_s = 8.0;
            profile.vbus_v = 6.3f;
        }
        /* Both policies keep exactly the same sensors, Plant, timing and caps. */
        set_current_gains(&variant.pipeline.current, &legacy_gains);
        const struct cycle_trace legacy =
            {.output = output, .group = "old_36hz", .scenario = names[i], .stimulus = &stimuli[i]};
        const struct cycle_trace selected = {
            .output = output,
            .group = "selected_130hz",
            .scenario = names[i],
            .stimulus = &stimuli[i]
        };
        if (study_trial(&profile, &init, &legacy, references[i]) != 0) {
            return -1;
        }
        variant.pipeline.current.kp_v_per_a = full->pipeline.current.kp_v_per_a;
        variant.pipeline.current.ki_v_per_a_s = full->pipeline.current.ki_v_per_a_s;
        if (study_trial(&profile, &init, &selected, references[i]) != 0) {
            return -1;
        }
    }
    return 0;
}

static int current_study(
    FILE *output,
    const struct foc_virtual_current_profile *profile,
    struct calibrated_init *measured,
    const struct foc_virtual_calibration *session
)
{
    const struct foc_cal_application_candidate *full = measured->candidate;
    struct foc_encoder_lut lut;
    if (foc_encoder_cal_make_lut(&session->workflow.result.lut, &lut) != FOC_ENCODER_CAL_OK ||
        fputs(
            "scenario,group,elapsed_us,h0_us,sample_us,h1_us,encoder_us,encoder_age_us,"
            "angle_valid,safety_state,action,update_applied,faults,session,near_voltage_cap,"
            "id_ref_a,iq_ref_a,id_measured_a,iq_measured_a,id_plant_a,iq_plant_a,"
            "theta_h0_rad,theta_plant_rad,theta_h1_rad,encoder_rad,encoder_speed_rad_s,"
            "shaft_rad,shaft_speed_rad_s,vd_v,vq_v,duty_a,duty_b,duty_c,id_integral_v,"
            "iq_integral_v,kp_d,kp_q,ki_d,ki_q,vbus_v,voltage_limit_v,load_nm,current_limit_a\n",
            output
        ) < 0) {
        return -1;
    }
    struct foc_cal_application_candidate variant = *full;
    struct calibrated_init alternate = {.candidate = &variant, .restored_lut = &lut};
    const struct foc_motor_rl_model oracle = {
        .rs_ohm = (float)profile->motor.rs_ohm,
        .ld_h = (float)profile->motor.ld_h,
        .lq_h = (float)profile->motor.lq_h,
    };
    const struct foc_device_adc_config uncalibrated_adc = {
        .vbus_v_per_count = profile->adc.vbus_v_per_count,
        .current_a_per_count = profile->adc.current_a_per_count,
        .vbus_min_v = profile->adc.vbus_min_v,
        .phase_a_offset_count = 2048.0f,
        .phase_b_offset_count = 2048.0f,
        .phase_c_offset_count = 2048.0f,
        .phase_order = profile->adc.phase_order,
    };
    const struct foc_motor_pi_request previous = legacy_tuning().current;
    struct foc_motor_pi_candidate prior_gains;
    if (foc_motor_pi_design(&full->gains.model, &previous, &prior_gains) != FOC_MOTOR_ID_OK) {
        return -1;
    }
    struct foc_current_config previous_current = full->pipeline.current;
    set_current_gains(&previous_current, &prior_gains);
    /* The list defines one frozen A/B stimulus, then the counterfactuals. */
    static const char *const groups[] = {
        "A_default",
        "B_measured",
        "C_oracle_rl",
        "D_measured_default_pi",
        "E_adc_only",
        "E_electrical_angle_only",
        "E_lut_only",
        "E_pi_only",
        "F_60hz",
        "F_96hz",
        "F_120hz",
        "F_130hz",
    };
    for (unsigned int group = 0U; group < sizeof groups / sizeof groups[0]; ++group) {
        struct calibrated_init *selected = &alternate;
        variant = *full;
        alternate.restored_lut = &lut;
        if (group == 0U) {
            selected = NULL;
        } else if (group == 1U) {
            variant.pipeline.current = previous_current;
        } else if (group == 2U) {
            struct foc_motor_pi_candidate gains;
            if (foc_motor_pi_design(&oracle, &previous, &gains) != FOC_MOTOR_ID_OK) {
                return -1;
            }
            set_current_gains(&variant.pipeline.current, &gains);
        } else if (group == 3U) {
            variant.pipeline.current = profile->current;
        } else if (group < 8U) {
            /* Keep all other sensor and controller terms at the A setting. */
            variant.runtime.config = profile->runtime;
            variant.pipeline.adc = group == 4U ? full->pipeline.adc : uncalibrated_adc;
            variant.runtime.config.motion.rotor =
                group == 5U ? full->runtime.config.motion.rotor : profile->runtime.motion.rotor;
            variant.pipeline.current = group == 7U ? previous_current : profile->current;
            variant.has_lut = group == 6U;
            alternate.restored_lut = variant.has_lut ? &lut : NULL;
        } else {
            static const float bandwidths[] = {60.0f, 96.0f, 120.0f, 130.0f};
            struct foc_motor_pi_request request = measured->policy->current;
            request.bandwidth_hz = bandwidths[group - 8U];
            /* Keep the same 6 V admitted bus and 2.4 V command cap. */
            request.available_voltage_v = 1.25f;
            struct foc_motor_pi_candidate gains;
            if (foc_motor_pi_design(&full->gains.model, &request, &gains) != FOC_MOTOR_ID_OK) {
                return -1;
            }
            set_current_gains(&variant.pipeline.current, &gains);
        }
        for (int direction = -1; direction <= 1; direction += 2) {
            const struct cycle_trace trace = {
                .output = output,
                .group = groups[group],
                .scenario = direction > 0 ? "step_positive" : "step_negative",
            };
            if (study_trial(profile, selected, &trace, 0.45f * (float)direction) != 0) {
                return -1;
            }
        }
    }
    return current_holdout(output, profile, full, &lut);
}

static int current_startup_study(
    FILE *output,
    const struct foc_virtual_current_profile *original,
    const struct foc_cal_application_candidate *approved,
    const struct foc_virtual_calibration *session
)
{
    struct foc_encoder_lut lut;
    if (foc_encoder_cal_make_lut(&session->workflow.result.lut, &lut) != FOC_ENCODER_CAL_OK ||
        fputs(
            "scenario,direction,time_us,h0_us,sample_us,h1_us,encoder_us,command_seen,"
            "published_us,consumed_us,state,action,output_confirmed,faults,session,updates,"
            "applied,model_apply_us,adc_seen,near_voltage_cap,iq_ref_a,iq_adc_a,iq_plant_a,"
            "voltage_v,voltage_limit_v,id_integral_v,iq_integral_v,shaft_speed_rad_s,"
            "vbus_v,current_limit_a\n",
            output
        ) < 0) {
        return -1;
    }
    static const struct {
        const char *name;
        float reference_a;
        float reverse_a;
    } scenarios[] = {
        {"cold_small", 0.15f, 0.0f},
        {"cold_nominal", 0.45f, 0.0f},
        {"cold_large", 2.20f, 0.0f},
        {"running_reverse", 0.45f, 0.45f},
        {"limited_reverse", 2.20f, 0.35f},
        {"mismatch_fast_electrical", 0.45f, 0.0f},
        {"mismatch_slow_electrical", 0.45f, 0.0f},
    };
    for (unsigned int i = 0U; i < sizeof scenarios / sizeof scenarios[0]; ++i) {
        struct foc_virtual_current_profile profile = *original;
        struct foc_cal_application_candidate variant = *approved;
        struct calibrated_init init = {.candidate = &variant, .restored_lut = &lut};
        if (i == 4U) {
            profile.vbus_v = 6.1f;
            variant.pipeline.current.pwm.voltage_limit_v = 0.72f;
        } else if (i == 5U) {
            profile.motor.rs_ohm *= 1.25;
            profile.motor.ld_h *= 0.80;
            profile.motor.lq_h *= 0.75;
        } else if (i == 6U) {
            profile.motor.rs_ohm *= 0.80;
            profile.motor.ld_h *= 1.20;
            profile.motor.lq_h *= 1.25;
        }
        for (int direction = -1; direction <= 1; direction += 2) {
            const struct current_stimulus stimulus = {
                .change_us = scenarios[i].reverse_a > 0.0f ? 30000U : 0U,
                .second_current_a = -scenarios[i].reverse_a * (float)direction,
            };
            const struct cycle_trace trace = {
                .output = output,
                .group = direction > 0 ? "positive" : "negative",
                .scenario = scenarios[i].name,
                .stimulus = &stimulus,
                .startup = true,
            };
            struct control_metrics metrics;
            if (compare_run(
                    &profile,
                    &init,
                    FOC_RUNTIME_MODE_CURRENT,
                    scenarios[i].reference_a * (float)direction,
                    60000U,
                    0.04,
                    &metrics,
                    &trace
                ) != 0) {
                return -1;
            }
        }
    }
    return 0;
}

static void print_parameters(const struct foc_virtual_calibration *session)
{
    const struct foc_cal_workflow_bundle *bundle = &session->workflow.result;
    printf(
        "{\"event\":\"PARAMETERS_IDENTIFIED\",\"source\":\"HOST_VIRTUAL_MEASUREMENT\","
        "\"rs_ohm\":%.8g,\"ld_h\":%.8g,\"lq_h\":%.8g,"
        "\"rs_d\":%.8g,\"rs_q\":%.8g,\"d_capture\":%" PRIu32 ",\"q_capture\":%" PRIu32 ","
        "\"d_residual_v\":%.8g,\"q_residual_v\":%.8g,\"d_excitation\":%.8g,\"q_excitation\":%.8g,"
        "\"d_aperture_us\":[%" PRIu32 ",%" PRIu32 "],\"q_aperture_us\":[%" PRIu32 ",%" PRIu32 "],"
        "\"flux_wb\":%.8g,\"ke_phase_peak_v_per_mechanical_rad_s\":%.8g,"
        "\"kt_nm_per_peak_q_amp\":%.8g,\"flux_residual_v\":%.8g,"
        "\"rotor_direction\":%" PRId32 ",\"rotor_pole_pairs\":%u,"
        "\"electrical_offset_rad\":%.8g,\"lut_holdout_rms_rad\":%.8g,"
        "\"j_kg_m2\":%.8g,\"b_nm_s\":%.8g,\"jb_residual_nm\":%.8g,"
        "\"stiffness_nm_per_rad\":%.8g,\"damping_nm_s_per_rad\":%.8g}\n",
        (double)bundle->electrical_model.rs_ohm,
        (double)bundle->electrical_model.ld_h,
        (double)bundle->electrical_model.lq_h,
        (double)bundle->electrical.rs_ohm,
        (double)bundle->electrical_q.rs_ohm,
        bundle->electrical.capture_id,
        bundle->electrical_q.capture_id,
        bundle->electrical.residual_rms_v,
        bundle->electrical_q.residual_rms_v,
        bundle->electrical.excitation,
        bundle->electrical_q.excitation,
        bundle->electrical.first_us,
        bundle->electrical.last_us,
        bundle->electrical_q.first_us,
        bundle->electrical_q.last_us,
        (double)session->flux.flux_wb,
        (double)session->flux.ke_phase_peak_v_per_mechanical_rad_s,
        (double)session->flux.kt_nm_per_peak_q_amp,
        (double)session->flux.residual_rms_v,
        bundle->rotor.direction,
        (unsigned)bundle->rotor.pole_pairs,
        (double)bundle->snapshot.electrical_offset_rad,
        bundle->lut.holdout_rms_rad,
        (double)bundle->mechanical.inertia_kg_m2,
        (double)bundle->mechanical.damping_nm_s,
        bundle->mechanical.residual_rms_nm,
        (double)bundle->tuning.stiffness_nm_per_rad,
        (double)bundle->tuning.damping_nm_s_per_rad
    );
}

uint32_t foc_virtual_cal_cli_flux_signature(const struct foc_flux_id_result *fit)
{
    const struct foc_flux_id_identity *id = &fit->identity;
    const uint32_t fields[] = {
        id->session,
        id->capture,
        id->motor,
        id->dq_frame,
        id->current_chain,
        id->voltage_chain,
        id->speed_chain,
        id->clock,
        id->rl_revision,
        id->poles_revision,
        id->phase_order,
        id->pole_pairs,
        fit->state,
        fit->failure,
        fit->source,
        fit->voltage_source,
        fit->flux_provenance,
        fit->kt_provenance,
        fit->first_us,
        fit->last_us,
        fit->intervals,
        fit->positive.windows,
        fit->negative.windows,
    };
    const double measurements[] = {
        fit->rl.rs_ohm,
        fit->rl.ld_h,
        fit->rl.lq_h,
        fit->flux_wb,
        fit->ke_phase_peak_v_per_mechanical_rad_s,
        fit->kt_nm_per_peak_q_amp,
        fit->positive.flux_wb,
        fit->negative.flux_wb,
        fit->positive.residual_rms_v,
        fit->negative.residual_rms_v,
        fit->positive.minimum_abs_speed_rad_s,
        fit->positive.maximum_abs_speed_rad_s,
        fit->negative.minimum_abs_speed_rad_s,
        fit->negative.maximum_abs_speed_rad_s,
        fit->residual_rms_v,
        fit->omega_e_rms_rad_s,
        fit->direction_difference_relative,
    };
    const uint32_t crc = crc32_ieee(UINT32_MAX, (const uint8_t *)fields, sizeof fields);
    return crc32_ieee(crc, (const uint8_t *)measurements, sizeof measurements) ^ UINT32_MAX;
}

void foc_virtual_cal_cli_flux_preview(const struct foc_flux_id_result *fit)
{
    const struct foc_flux_id_identity *id = &fit->identity;
    printf(
        "{\"event\":\"FLUX_CANDIDATE\",\"flux_origin\":\"FLUX_FROM_MEASUREMENT\","
        "\"kt_origin\":\"KT_DERIVED_FROM_FLUX\",\"voltage_source\":\"VIRTUAL_TERMINAL\","
        "\"session\":%" PRIu32 ",\"capture\":%" PRIu32 ",\"motor\":%" PRIu32 ","
        "\"pole_pairs\":%u,\"rl_revision\":%" PRIu32 ",\"poles_revision\":%" PRIu32 ","
        "\"current_revision\":%" PRIu32 ",\"dq_frame\":%" PRIu32 ","
        "\"voltage_chain\":%" PRIu32 ",\"phase_order\":%" PRIu32 ","
        "\"flux_wb\":%.9g,\"kt_nm_per_peak_q_amp\":%.9g,"
        "\"ke_phase_peak_v_per_mechanical_rad_s\":%.9g,\"residual_rms_v\":%.17g,"
        "\"direction_difference_relative\":%.17g,\"signed_flux_wb\":[%.17g,%.17g],"
        "\"signed_residual_rms_v\":[%.17g,%.17g],\"windows\":[%" PRIu32 ",%" PRIu32 "],"
        "\"positive_speed_rad_s\":[%.8g,%.8g],\"negative_speed_rad_s\":[%.8g,%.8g],"
        "\"intervals\":%" PRIu32 ",\"aperture_us\":[%" PRIu32 ",%" PRIu32 "],"
        "\"evidence_id\":\"%08" PRIx32 "\"}\n",
        id->session,
        id->capture,
        id->motor,
        (unsigned int)id->pole_pairs,
        id->rl_revision,
        id->poles_revision,
        id->current_chain,
        id->dq_frame,
        id->voltage_chain,
        id->phase_order,
        (double)fit->flux_wb,
        (double)fit->kt_nm_per_peak_q_amp,
        (double)fit->ke_phase_peak_v_per_mechanical_rad_s,
        fit->residual_rms_v,
        fit->direction_difference_relative,
        fit->positive.flux_wb,
        fit->negative.flux_wb,
        fit->positive.residual_rms_v,
        fit->negative.residual_rms_v,
        fit->positive.windows,
        fit->negative.windows,
        fit->positive.minimum_abs_speed_rad_s,
        fit->positive.maximum_abs_speed_rad_s,
        -fit->negative.maximum_abs_speed_rad_s,
        -fit->negative.minimum_abs_speed_rad_s,
        fit->intervals,
        fit->first_us,
        fit->last_us,
        foc_virtual_cal_cli_flux_signature(fit)
    );
}

void foc_virtual_cal_cli_cogging_fixture(struct foc_virtual_current_profile *profile)
{
    profile->cogging_amplitude_nm = 0.012;
    profile->cogging_cycles_per_turn = 3U;
    profile->cogging_phase_rad = 0.35;
}

static bool permit_cogging(void *argument)
{
    return foc_virtual_calibration_poll(argument) == FOC_VIRTUAL_CAL_OK;
}

int foc_virtual_cal_cli_cogging_capture(
    const struct foc_virtual_current_profile *profile,
    struct foc_virtual_calibration *session,
    struct foc_virtual_cogging_result *capture
)
{
    struct foc_virtual_motor motor;
    if (foc_virtual_motor_init(&motor, &profile->motor, NULL) != 0 ||
        foc_virtual_motor_set_cogging(
            &motor,
            profile->cogging_amplitude_nm,
            profile->cogging_cycles_per_turn,
            profile->cogging_phase_rad
        ) != 0) {
        return FOC_VIRTUAL_COGGING_ERR_INPUT;
    }
    const struct foc_cal_workflow_identity *identity = &session->workflow.result.request.identity;
    const struct foc_virtual_cogging_scan_config config = {
        .motor_id = identity->motor_setup_id,
        .mechanical_frame_id = identity->mechanical_frame_id,
        .training_capture_id = 21U,
        .holdout_capture_id = 22U,
        .speed_rad_s = 0.8,
        .max_holdout_rms_nm = 0.002,
        .permit = permit_cogging,
        .permit_argument = session,
    };
    const int status = foc_virtual_cogging_calibrate(&motor, &config, capture);
    if (status == FOC_VIRTUAL_COGGING_OK) {
        printf(
            "{\"event\":\"COGGING_QUALIFIED\",\"training_capture\":21,"
            "\"holdout_capture\":22,\"holdout_before_rms_nm\":%.8g,"
            "\"holdout_after_rms_nm\":%.8g}\n",
            capture->holdout_baseline_rms_nm,
            capture->holdout_compensated_rms_nm
        );
    }
    return status;
}

int foc_virtual_cal_cli_verify(
    struct foc_virtual_current_profile *profile,
    struct calibrated_init *init,
    struct foc_virtual_calibration *session,
    const char *save_config,
    const char *current_study_path,
    bool startup_study,
    struct foc_virtual_cal_cli_timing *timing
)
{
    foc_virtual_cal_cli_timing_start(timing, CAL_CLI_APPLICATION);
    struct foc_virtual_current driver = {0};
    if (init->candidate->runtime.config.motion.cogging.enabled) {
        foc_virtual_cal_cli_cogging_fixture(profile);
    }
    if (foc_virtual_current_init_with(&driver, profile, foc_virtual_cal_cli_initialize, init) !=
            0 ||
        !foc_virtual_cal_cli_readback(&driver, init->candidate, true)) {
        foc_virtual_cal_cli_timing_finish(timing, CAL_CLI_APPLICATION, false, 0);
        return foc_virtual_cal_cli_error("FRESH_RUNTIME_INIT_FAILED", "APPLICATION", -1);
    }
    foc_virtual_cal_cli_timing_finish(timing, CAL_CLI_APPLICATION, true, 0);
    if (current_study_path != NULL) {
        foc_virtual_cal_cli_timing_start(timing, CAL_CLI_CONTROL_DIAGNOSTIC);
        FILE *trace = NULL;
#if defined(_MSC_VER)
        (void)fopen_s(&trace, current_study_path, "wbx");
#else
        trace = fopen(current_study_path, "wbx");
#endif
        if (trace == NULL) {
            foc_virtual_cal_cli_timing_finish(timing, CAL_CLI_CONTROL_DIAGNOSTIC, false, 0);
            return foc_virtual_cal_cli_error("CURRENT_STUDY_IO", "CONTROL", -1);
        }
        const int status = startup_study
            ? current_startup_study(trace, profile, init->candidate, session)
            : current_study(trace, profile, init, session);
        const int closed = fclose(trace);
        foc_virtual_cal_cli_timing_finish(
            timing, CAL_CLI_CONTROL_DIAGNOSTIC, status == 0 && closed == 0, -1
        );
        if (status != 0 || closed != 0) {
            return foc_virtual_cal_cli_error("CURRENT_STUDY_FAILED", "CONTROL", status);
        }
    }
    uint32_t safety_us = 0U;
    foc_virtual_cal_cli_timing_start(timing, CAL_CLI_CONTROL_SAFETY);
    if (!verify_recovery(profile, init, &safety_us)) {
        foc_virtual_cal_cli_timing_finish(timing, CAL_CLI_CONTROL_SAFETY, false, -1);
        return foc_virtual_cal_cli_error("FAULT_RECOVERY_FAILED", "CONTROL", -1);
    }
    if (!verify_calibrated_torque_limit(profile, init, &safety_us)) {
        foc_virtual_cal_cli_timing_finish(timing, CAL_CLI_CONTROL_SAFETY, false, -1);
        return foc_virtual_cal_cli_error("CALIBRATED_TORQUE_LIMIT_FAILED", "CONTROL", -1);
    }
    foc_virtual_cal_cli_timing_finish(timing, CAL_CLI_CONTROL_SAFETY, true, safety_us);
    int64_t holdout_us = 0;
    foc_virtual_cal_cli_timing_start(timing, CAL_CLI_CONTROL_HOLDOUT);
    const bool quality = verify_control(profile, init, &holdout_us);
    foc_virtual_cal_cli_timing_finish(timing, CAL_CLI_CONTROL_HOLDOUT, quality, holdout_us);
    bool persisted = init->restored_lut != NULL;
    if (save_config != NULL && quality) {
        foc_virtual_cal_cli_timing_start(timing, CAL_CLI_FCAL_SAVE);
        const int status =
            foc_virtual_calibration_package_save(save_config, session, init->candidate);
        foc_virtual_cal_cli_timing_finish(
            timing, CAL_CLI_FCAL_SAVE, status == FOC_VIRTUAL_PACKAGE_OK, 0
        );
        if (status != FOC_VIRTUAL_PACKAGE_OK) {
            return foc_virtual_cal_cli_error(
                foc_virtual_calibration_package_reason(status),
                "SAVE_CONFIG",
                status
            );
        }
        persisted = true;
    }
    printf(
        "{\"event\":\"CONFIGURATION_PERSISTED\",\"host\":%s,\"mcu_flash\":false}\n",
        persisted ? "true" : "false"
    );
    printf(
        "{\"event\":\"CONTROL_QUALITY_%s\",\"session_bytes\":%zu,\"driver_bytes\":%zu}\n",
        quality ? "PASS" : "FAIL",
        sizeof *session,
        sizeof driver
    );
    return quality ? 0 : 3;
}

void foc_virtual_cal_cli_preview(
    const struct foc_cal_application_candidate *candidate,
    uint32_t review_id
)
{
    const struct foc_cal_application_gains *gains = &candidate->gains;
    const struct foc_device_adc_config *adc = &candidate->pipeline.adc;
    printf(
        "{\"event\":\"CANDIDATE_PREVIEW\",\"review_id\":\"%08" PRIx32 "\","
        "\"pi\":[%.8g,%.8g,%.8g],\"outer\":[%.8g,%.8g,%.8g],"
        "\"impedance\":[%.8g,%.8g],\"damping_filter_hz\":%.8g,"
        "\"adc\":[%.8g,%.8g,%.8g],\"cogging_enabled\":%s}\n",
        review_id,
        (double)gains->current.kp_d_v_per_a,
        (double)gains->current.kp_q_v_per_a,
        (double)gains->current.ki_v_per_a_s,
        (double)gains->velocity.kp_a_per_rad_s,
        (double)gains->velocity.ki_a_per_rad,
        (double)gains->position.kp_per_s,
        (double)gains->impedance.stiffness_nm_per_rad,
        (double)gains->impedance.damping_nm_s_per_rad,
        (double)candidate->runtime.config.motion.impedance.damping_filter_hz,
        (double)adc->phase_a_offset_count,
        (double)adc->phase_b_offset_count,
        (double)adc->phase_c_offset_count,
        gains->cogging.enabled ? "true" : "false"
    );
}

bool foc_virtual_cal_cli_review_matches(const char *approved, uint32_t review_id)
{
    char expected[9];
    (void)snprintf(expected, sizeof expected, "%08" PRIx32, review_id);
    return strcmp(expected, approved) == 0;
}

int foc_virtual_cal_cli_draft(
    const struct foc_virtual_calibration *session,
    const struct foc_cal_application_tuning_policy *policy,
    struct foc_cal_application_candidate *candidate
)
{
    /* Prepare a detached preview under the same policy. The live workflow
     * remains unreviewed until the operator accepts the matching fingerprint. */
    struct foc_cal_workflow workflow = session->workflow;
    struct foc_cal_application application = session->application;
    application.workflow = &workflow;
    const struct foc_cal_workflow_request *request = &workflow.result.request;
    return foc_cal_workflow_review(&workflow, request->session, &request->identity, true) == 0
        ? foc_cal_application_prepare_tuned(&application, policy, candidate)
        : FOC_CAL_APPLICATION_ERR_REVIEW;
}

static bool read_operating_point(
    char **fields,
    enum foc_pmsm_target_kind kind,
    struct foc_pmsm_operating_point *point
)
{
    float values[4];
    for (unsigned int i = 0U; i < 4U; ++i) {
        char *end;
        errno = 0;
        values[i] = strtof(fields[i], &end);
        if (end == fields[i] || *end != '\0' || errno == ERANGE || !isfinite(values[i])) {
            return false;
        }
    }
    *point = (struct foc_pmsm_operating_point){kind, values[0], values[1], values[2], values[3]};
    return point->vbus_v > 0.0f;
}

static void report_reachability(
    const struct foc_virtual_calibration_package *package,
    const struct foc_pmsm_operating_point *point
)
{
    const struct foc_cal_application_candidate *candidate = &package->candidate;
    const struct foc_cal_application_gains *gains = &candidate->gains;
    const struct foc_runtime_motion_config *motion = &candidate->runtime.config.motion;
    const bool torque = point->kind == FOC_PMSM_TARGET_TORQUE;
    const struct foc_pmsm_qualified_model model = {
        .rs_ohm = gains->model.rs_ohm,
        .ld_h = gains->model.ld_h,
        .lq_h = gains->model.lq_h,
        .flux_wb = package->flux_wb,
        .pole_pairs = motion->rotor.pole_pairs,
        .motor_id = gains->identity.motor_setup_id,
        .review_id = package->review_id,
        .qualified = !torque || motion->impedance.enabled,
    };
    struct foc_pmsm_reachability_limits limits = {
        .pwm = candidate->pipeline.current.pwm,
        .current_limit_a = candidate->pipeline.current.current_limit_a,
    };
    limits.pwm.vbus_min_v = fmaxf(limits.pwm.vbus_min_v, candidate->pipeline.adc.vbus_min_v);
    if (torque) {
        limits.current_limit_a = fminf(limits.current_limit_a, motion->iq_limit_a);
        limits.torque_limit_nm = motion->impedance.torque_limit_nm;
    }
    struct foc_pmsm_reachability report;
    (void)foc_pmsm_reachability_evaluate(&model, &limits, point, &report);
    static const char *const verdicts[] = {
        "INDETERMINATE",
        "MODEL_FEASIBLE",
        "MODEL_INFEASIBLE",
    };
    printf(
        "{\"event\":\"REACHABILITY\",\"version\":1,\"source\":\"HOST_CONFIG_V1\","
        "\"configuration_applied\":false,\"physical_applied\":false,"
        "\"assumption\":\"STEADY_STATE_ZERO_DQ_CURRENT_DERIVATIVES\","
        "\"control_mode\":\"%s\",\"verdict\":\"%s\",\"reason_bits\":%" PRIu32 ","
        "\"motor_setup_id\":%" PRIu32 ",\"review_id\":\"%08" PRIx32 "\","
        "\"mechanical_rad_s\":%.9g,\"electrical_rad_s\":%.9g,\"vbus_v\":%.9g,"
        "\"target\":%.9g,\"id_a\":%.9g,\"iq_a\":%.9g,\"current_a\":%.9g,\"torque_nm\":%.9g,"
        "\"torque_per_q_amp\":%.9g,"
        "\"resistive_dq_v\":[%.9g,%.9g],\"coupled_dq_v\":[%.9g,%.9g],\"back_emf_q_v\":%.9g,"
        "\"required_dq_v\":[%.9g,%.9g],\"required_v\":%.9g,\"margin_v\":%.9g,"
        "\"all_angle_limit_v\":%.9g,\"best_angle_limit_v\":%.9g,\"near_voltage_limit\":%s,"
        "\"configured_limits\":{\"current_a\":%.9g,\"voltage_v\":%.9g,"
        "\"min_vbus_v\":%.9g,\"duty_min\":%.9g,\"duty_max\":%.9g,\"torque_nm\":%.9g},"
        "\"range_valid\":%s,\"iq_range_a\":[%.9g,%.9g],\"torque_range_nm\":[%.9g,%.9g],"
        "\"reasons\":[",
        torque ? "impedance" : "current",
        verdicts[report.verdict],
        report.reasons,
        report.motor_id,
        report.review_id,
        (double)point->mechanical_rad_s,
        report.electrical_rad_s,
        (double)point->vbus_v,
        (double)point->target,
        report.id_a,
        report.iq_a,
        report.current_a,
        report.torque_nm,
        report.torque_per_q_amp,
        report.resistive_d_v,
        report.resistive_q_v,
        report.coupled_d_v,
        report.coupled_q_v,
        report.back_emf_q_v,
        report.vd_v,
        report.vq_v,
        report.demanded_v,
        report.voltage_margin_v,
        report.all_angle_limit_v,
        report.best_angle_limit_v,
        report.near_voltage_limit ? "true" : "false",
        (double)limits.current_limit_a,
        (double)limits.pwm.voltage_limit_v,
        (double)limits.pwm.vbus_min_v,
        (double)limits.pwm.duty_min,
        (double)limits.pwm.duty_max,
        (double)limits.torque_limit_nm,
        report.range_valid ? "true" : "false",
        report.iq_min_a,
        report.iq_max_a,
        report.torque_min_nm,
        report.torque_max_nm
    );
    static const struct {
        uint32_t bit;
        const char *name;
    } reasons[] = {
        {FOC_PMSM_REASON_MODEL_UNQUALIFIED, "MODEL_UNQUALIFIED"},
        {FOC_PMSM_REASON_INVALID_INPUT, "INVALID_INPUT"},
        {FOC_PMSM_REASON_BUS_MINIMUM, "BUS_MINIMUM"},
        {FOC_PMSM_REASON_CURRENT_LIMIT, "CURRENT_LIMIT"},
        {FOC_PMSM_REASON_TORQUE_LIMIT, "TORQUE_LIMIT"},
        {FOC_PMSM_REASON_VOLTAGE_LIMIT, "VOLTAGE_LIMIT"},
        {FOC_PMSM_REASON_ANGLE_DEPENDENT, "ANGLE_DEPENDENT"},
        {FOC_PMSM_REASON_TORQUE_UNDEFINED, "TORQUE_UNDEFINED"},
    };
    const char *separator = "";
    for (unsigned int i = 0U; i < sizeof reasons / sizeof reasons[0]; ++i) {
        if ((report.reasons & reasons[i].bit) != 0U) {
            printf("%s\"%s\"", separator, reasons[i].name);
            separator = ",";
        }
    }
    puts("]}");
}

int foc_virtual_cal_cli_restore(
    const char *path,
    struct foc_virtual_current_profile *profile,
    const struct foc_cal_workflow_identity *identity,
    const char *approved,
    const struct foc_pmsm_operating_point *diagnostic,
    struct foc_virtual_cal_cli_timing *timing,
    const char *quick_scenario
)
{
    struct foc_virtual_calibration_package package;
    const struct foc_cal_application_tuning_policy legacy = legacy_tuning();
    foc_virtual_cal_cli_timing_start(timing, CAL_CLI_FCAL_LOAD);
    const int status = foc_virtual_calibration_package_load(
        path,
        profile,
        identity,
        &foc_virtual_cal_cli_tuning,
        &legacy,
        &package
    );
    foc_virtual_cal_cli_timing_finish(timing, CAL_CLI_FCAL_LOAD, status == FOC_VIRTUAL_PACKAGE_OK, 0);
    if (status != FOC_VIRTUAL_PACKAGE_OK) {
        return foc_virtual_cal_cli_error(
            foc_virtual_calibration_package_reason(status),
            "LOAD_CONFIG",
            status
        );
    }
    printf(
        "{\"event\":\"CONFIGURATION_LOADED\",\"source\":\"HOST_CONFIG_V1\","
        "\"rs_ohm\":%.8g,\"ld_h\":%.8g,\"lq_h\":%.8g,\"flux_wb\":%.8g,"
        "\"j_kg_m2\":%.8g,\"b_nm_s\":%.8g}\n",
        (double)package.candidate.gains.model.rs_ohm,
        (double)package.candidate.gains.model.ld_h,
        (double)package.candidate.gains.model.lq_h,
        (double)package.flux_wb,
        (double)package.inertia_kg_m2,
        (double)package.damping_nm_s
    );
    if (diagnostic != NULL) {
        report_reachability(&package, diagnostic);
        return 0;
    }
    if (timing != NULL) {
        puts(
            "{\"event\":\"PRODUCT_FCAL_SOURCE\",\"format\":\"FCAL_V1\","
            "\"numerical_config\":true,\"measurement_sources_restored\":false,"
            "\"dependency_history_restored\":false,\"independent_holdout_restored\":false,"
            "\"review_history_restored\":false,\"quality\":\"SOURCE_CONFIDENCE_INSUFFICIENT\"}"
        );
        const int live = foc_virtual_cal_cli_quick_check(
            profile, identity, &package, quick_scenario, timing
        );
        if (live != 0) {
            return live;
        }
        foc_virtual_cal_cli_timing_omit(
            timing, CAL_CLI_BOOTSTRAP_STAGES | CAL_CLI_BIT(CAL_CLI_FCAL_SAVE), CAL_CLI_SKIPPED
        );
        foc_virtual_cal_cli_timing_omit(
            timing, CAL_CLI_CAPTURE_STAGES | CAL_CLI_BIT(CAL_CLI_GAIN_CANDIDATE), CAL_CLI_REUSED
        );
    }
    foc_virtual_cal_cli_preview(&package.candidate, package.review_id);
    if (approved == NULL) {
        puts("{\"event\":\"REVIEW_REQUIRED\",\"configuration_applied\":false}");
        return 0;
    }
    foc_virtual_cal_cli_timing_start(timing, CAL_CLI_REVIEW);
    const bool review_matches = foc_virtual_cal_cli_review_matches(approved, package.review_id);
    foc_virtual_cal_cli_timing_finish(timing, CAL_CLI_REVIEW, review_matches, 0);
    if (!review_matches) {
        return foc_virtual_cal_cli_error("REVIEW_ID_MISMATCH", "LOAD_CONFIG", -1);
    }
    printf(
        "{\"event\":\"CANDIDATE_REVIEWED\",\"review_id\":\"%08" PRIx32 "\"}\n",
        package.review_id
    );
    struct calibrated_init init = {
        .policy = package.uses_legacy_policy ? &legacy : &foc_virtual_cal_cli_tuning,
        .candidate = &package.candidate,
        .restored_lut = &package.lut,
    };
    return foc_virtual_cal_cli_verify(profile, &init, NULL, NULL, NULL, false, timing);
}

struct endpoint_trace {
    FILE *file;
    bool failed;
};

static void trace_flux_interval(void *argument, const struct foc_flux_id_interval *sample)
{
    struct endpoint_trace *trace = argument;
    const struct foc_flux_id_identity *id = &sample->identity;
    if (fprintf(
            trace->file,
            "%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ","
            "%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ","
            "%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ","
            "%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%d,%d,%d,%d,%d,%d,%d,%d\n",
            id->session,
            id->capture,
            id->motor,
            sample->sequence,
            sample->begin_us,
            sample->end_us,
            sample->voltage_begin_us,
            sample->voltage_end_us,
            sample->speed_begin_us,
            sample->speed_end_us,
            id->rl_revision,
            id->poles_revision,
            id->current_chain,
            sample->id_begin_a,
            sample->iq_begin_a,
            sample->id_end_a,
            sample->iq_end_a,
            sample->mean_omega_e_rad_s,
            sample->mean_vq_v,
            (int)sample->voltage_source,
            sample->current_valid,
            sample->voltage_valid,
            sample->speed_valid,
            sample->frame_confirmed,
            sample->output_confirmed,
            sample->terminal_voltage_witnessed,
            sample->saturated
        ) < 0) {
        trace->failed = true;
    }
}

static void
trace_rl(void *argument, enum foc_motor_id_axis axis, const struct foc_motor_id_sample *sample)
{
    struct endpoint_trace *trace = argument;
    if (fprintf(
            trace->file,
            "%c,%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32
            ",%.9g,%.9g,%u,%u,%u,%u\n",
            axis == FOC_MOTOR_ID_D ? 'd' : 'q',
            sample->setup_id,
            sample->capture_id,
            sample->sequence,
            sample->captured_us,
            sample->voltage_begin_us,
            sample->voltage_end_us,
            (double)sample->current_a,
            (double)sample->mean_voltage_v,
            (unsigned)sample->voltage_source,
            (unsigned)sample->voltage_valid,
            (unsigned)sample->stationary_confirmed,
            (unsigned)sample->output_confirmed
        ) < 0) {
        trace->failed = true;
    }
}

static int review_parameters(
    const char *selected,
    const struct foc_virtual_current_profile *profile,
    struct foc_cal_workflow_request *request,
    const char *approved
)
{
    const bool rs = strcmp(selected, "rs") == 0;
    if (!rs && strcmp(selected, "jb") != 0) {
        return foc_virtual_cal_cli_error("INVALID_PARAMETER_SELECTION", "SETUP", -1);
    }
    request->profile = FOC_CAL_WORKFLOW_SELECTED_STAGES;
    request->require_dual_axis = false;
    request->stages = rs ? FOC_CAL_WORKFLOW_STAGE_BIT(FOC_CAL_WORKFLOW_ADC) |
            FOC_CAL_WORKFLOW_STAGE_BIT(FOC_CAL_WORKFLOW_ELECTRICAL)
                         : FOC_CAL_WORKFLOW_STAGE_BIT(FOC_CAL_WORKFLOW_MECHANICAL);
    struct foc_virtual_calibration session = {0};
    int status = foc_virtual_calibration_start(&session, profile, request);
    while (status == FOC_VIRTUAL_CAL_PENDING) {
        status = foc_virtual_calibration_poll(&session);
    }
    const struct foc_cal_workflow_bundle *bundle = &session.workflow.result;
    if (status != FOC_VIRTUAL_CAL_OK || !bundle->complete || bundle->applicable) {
        return foc_virtual_cal_cli_error("PARAMETER_CAPTURE_FAILED", "REVIEW", status);
    }
    const float first = rs ? bundle->electrical.rs_ohm : bundle->mechanical.inertia_kg_m2;
    const float second = rs ? 0.0f : bundle->mechanical.damping_nm_s;
    const uint32_t capture = rs ? bundle->electrical.capture_id : bundle->mechanical.capture_id;
    const uint32_t identity[] = {
        request->session,
        request->identity.motor_setup_id,
        request->identity.encoder_id,
        request->identity.mechanical_frame_id,
        request->stages,
        capture,
    };
    const float values[] = {first, second};
    const uint32_t crc = crc32_ieee(UINT32_MAX, (const uint8_t *)identity, sizeof identity);
    const uint32_t id = crc32_ieee(crc, (const uint8_t *)values, sizeof values) ^ UINT32_MAX;
    printf(
        "{\"event\":\"PARAMETER_PREVIEW\",\"parameter\":\"%s\","
        "\"review_id\":\"%08" PRIx32 "\",\"source\":\"HOST_VIRTUAL\","
        "\"capture\":%" PRIu32 ",\"values\":[%.9g,%.9g],\"runtime_applicable\":false}\n",
        rs ? "RS" : "JB",
        id,
        capture,
        (double)first,
        (double)second
    );
    if (approved == NULL) {
        return 0;
    }
    if (!foc_virtual_cal_cli_review_matches(approved, id) ||
        foc_cal_workflow_review(&session.workflow, request->session, &request->identity, true) !=
            FOC_CAL_WORKFLOW_OK) {
        return foc_virtual_cal_cli_error("REVIEW_ID_MISMATCH", "REVIEW", -1);
    }
    struct foc_cal_application_candidate impossible;
    if (foc_cal_application_prepare(&session.application, &impossible) !=
        FOC_CAL_APPLICATION_ERR_REVIEW) {
        return foc_virtual_cal_cli_error("UNADMITTED_CONFIGURATION", "APPLICATION", -1);
    }
    puts("{\"event\":\"PARAMETER_REVIEWED\",\"runtime_applicable\":false}");
    return 0;
}

/* Public Functions ------------------------------------------------------------- */

int main(int argc, char *argv[])
{
    const char *profile_name = "GENERIC_RESEARCH_MOTOR";
    const char *cancel_after = NULL;
    const char *save_config = NULL;
    const char *load_config = NULL;
    const char *bootstrap = NULL;
    const char *boot_scenario = "nominal";
    const char *boot_trace = NULL;
    const char *trace_path = NULL;
    const char *current_study_path = NULL;
    const char *approved = NULL;
    const char *selected = NULL;
    bool reuse_session = false;
    bool flux_trace = false;
    bool use_cogging = false;
    bool startup_study = false;
    bool capabilities = false;
    bool product_timing = false;
    struct foc_pmsm_operating_point point;
    const struct foc_pmsm_operating_point *diagnostic = NULL;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--profile-id") == 0 && i + 1 < argc) {
            profile_name = argv[++i];
        } else if (strcmp(argv[i], "--approve-candidate") == 0 && i + 1 < argc) {
            approved = argv[++i];
        } else if (strcmp(argv[i], "--cogging") == 0) {
            use_cogging = true;
        } else if (strcmp(argv[i], "--save-config") == 0 && i + 1 < argc) {
            save_config = argv[++i];
        } else if (strcmp(argv[i], "--load-config") == 0 && i + 1 < argc) {
            load_config = argv[++i];
        } else if (strcmp(argv[i], "--bootstrap") == 0 && i + 1 < argc && bootstrap == NULL) {
            bootstrap = argv[++i];
        } else if (strcmp(argv[i], "--boot-scenario") == 0 && i + 1 < argc) {
            boot_scenario = argv[++i];
        } else if (strcmp(argv[i], "--boot-trace") == 0 && i + 1 < argc && boot_trace == NULL) {
            boot_trace = argv[++i];
        } else if (strcmp(argv[i], "--capabilities") == 0) {
            capabilities = true;
        } else if (strcmp(argv[i], "--stages") == 0 && i + 1 < argc && selected == NULL) {
            selected = argv[++i];
        } else if (strcmp(argv[i], "--reuse-session") == 0) {
            reuse_session = true;
        } else if (strcmp(argv[i], "--product-timing") == 0) {
            product_timing = true;
        } else if (
            strcmp(argv[i], "--reachability") == 0 || strcmp(argv[i], "--reachability-torque") == 0
        ) {
            const bool torque = strcmp(argv[i], "--reachability-torque") == 0;
            if (diagnostic != NULL || argc - i <= 4 ||
                !read_operating_point(
                    &argv[i + 1],
                    torque ? FOC_PMSM_TARGET_TORQUE : FOC_PMSM_TARGET_CURRENT,
                    &point
                )) {
                return foc_virtual_cal_cli_error("INVALID_REACHABILITY_POINT", "SETUP", -1);
            }
            if (torque && point.id_a != 0.0f) {
                return foc_virtual_cal_cli_error("IMPEDANCE_REQUIRES_ZERO_ID", "SETUP", -1);
            }
            diagnostic = &point;
            i += 4;
        } else if (
            (strcmp(argv[i], "--cq-current-csv") == 0 ||
             strcmp(argv[i], "--cq-startup-csv") == 0) &&
            i + 1 < argc && current_study_path == NULL
        ) {
            startup_study = strcmp(argv[i], "--cq-startup-csv") == 0;
            current_study_path = argv[++i];
        } else if (
            (strcmp(argv[i], "--trace") == 0 || strcmp(argv[i], "--flux-trace") == 0) &&
            i + 1 < argc && trace_path == NULL
        ) {
            flux_trace = strcmp(argv[i], "--flux-trace") == 0;
            trace_path = argv[++i];
        } else if (strcmp(argv[i], "--cancel-after") == 0 && i + 1 < argc) {
            cancel_after = argv[++i];
        } else {
            return foc_virtual_cal_cli_error("INVALID_ARGUMENT", "SETUP", -1);
        }
    }
    puts("{\"event\":\"PHYSICAL_CALIBRATION_NOT_VERIFIED\"}");
    struct foc_virtual_current_profile profile;
    struct foc_cal_workflow_request request;
    struct foc_virtual_calibration session = {0};
    const bool known_boot = bootstrap != NULL && strcmp(bootstrap, "known") == 0;
    const bool measured_boot = bootstrap != NULL && !known_boot;
    if (product_timing && !known_boot && (!measured_boot || strcmp(bootstrap, "unknown") != 0)) {
        return foc_virtual_cal_cli_error("INVALID_PRODUCT_ARGUMENTS", "SETUP", -1);
    }
    struct foc_virtual_cal_cli_timing product = {
        .mode = reuse_session ? "selective" : known_boot ? "quick" : "full",
    };
    struct foc_virtual_cal_cli_timing *timing = product_timing ? &product : NULL;
    if ((bootstrap == NULL && (boot_trace != NULL || strcmp(boot_scenario, "nominal") != 0)) ||
        (known_boot &&
         (load_config == NULL || boot_trace != NULL ||
          (!product_timing && strcmp(boot_scenario, "nominal") != 0))) ||
        (measured_boot && (load_config != NULL || selected != NULL || use_cogging)) ||
        (bootstrap != NULL && (capabilities || diagnostic != NULL || selected != NULL))) {
        return foc_virtual_cal_cli_error("BOOTSTRAP_ARGUMENT_CONFLICT", "SETUP", -1);
    }
    if (reuse_session) {
        if (bootstrap != NULL && strcmp(bootstrap, "offset") != 0 &&
            strcmp(bootstrap, "unknown") != 0) {
            return foc_virtual_cal_cli_error("BOOTSTRAP_ARGUMENT_CONFLICT", "SETUP", -1);
        }
        if (load_config != NULL || approved != NULL || save_config != NULL || capabilities ||
            current_study_path != NULL || trace_path != NULL || cancel_after != NULL ||
            diagnostic != NULL || selected != NULL) {
            return foc_virtual_cal_cli_error("REUSE_SESSION_ARGUMENT_CONFLICT", "SETUP", -1);
        }
        if (boot_trace != NULL || strcmp(boot_scenario, "nominal") != 0) {
            return foc_virtual_cal_cli_error("BOOTSTRAP_ARGUMENT_CONFLICT", "SETUP", -1);
        }
        return foc_virtual_cal_cli_reuse_session(profile_name, use_cogging, bootstrap, timing);
    }
    if (capabilities) {
        if (load_config != NULL || approved != NULL || save_config != NULL ||
            current_study_path != NULL || trace_path != NULL || cancel_after != NULL ||
            use_cogging || diagnostic != NULL || selected != NULL) {
            return foc_virtual_cal_cli_error("CAPABILITIES_IS_READ_ONLY", "SETUP", -1);
        }
        puts(
            "{\"event\":\"HOST_CAPABILITIES\",\"version\":1,\"source\":\"HOST_VIRTUAL\","
            "\"profiles\":[\"GENERIC_RESEARCH_MOTOR\",\"CALIBRATION_SALIENT_MOTOR\"],"
            "\"virtual_modes\":[\"current\",\"velocity\",\"position\",\"impedance\"],"
            "\"config_format\":\"FCAL_V1\",\"reachability_version\":1,"
            "\"review_required_to_apply\":true,\"physical_applied\":false,"
            "\"output_locked_on_board1\":true,\"mcu_flash\":false,\"target_wcet_verified\":false}"
        );
        return 0;
    }
    if (diagnostic != NULL && (load_config == NULL || approved != NULL)) {
        return foc_virtual_cal_cli_error("REACHABILITY_REQUIRES_LOAD_ONLY", "SETUP", -1);
    }
    if ((save_config != NULL && approved == NULL) ||
        (current_study_path != NULL && (approved == NULL || load_config != NULL)) ||
        (load_config != NULL &&
         (save_config != NULL || cancel_after != NULL || trace_path != NULL || use_cogging))) {
        return foc_virtual_cal_cli_error("EXPLICIT_REVIEW_REQUIRED", "SETUP", -1);
    }
    if (foc_virtual_calibration_profile(profile_name, &profile, &request) != 0) {
        return foc_virtual_cal_cli_error("INVALID_PROFILE_OR_RUNTIME", "SETUP", -1);
    }
    profile.runtime.motion.impedance.damping_filter_hz = 48.0f;
    if (selected != NULL) {
        if (load_config != NULL || save_config != NULL || current_study_path != NULL ||
            trace_path != NULL || cancel_after != NULL || use_cogging || diagnostic != NULL) {
            return foc_virtual_cal_cli_error("PARAMETER_REVIEW_ONLY", "SETUP", -1);
        }
        return review_parameters(selected, &profile, &request, approved);
    }
    if (load_config != NULL) {
        foc_virtual_cal_cli_timing_reset(
            timing,
            CAL_CLI_APPLY_STAGES | CAL_CLI_CAPTURE_STAGES | CAL_CLI_BOOTSTRAP_STAGES |
                CAL_CLI_BIT(CAL_CLI_QUIESCENT) | CAL_CLI_BIT(CAL_CLI_LIVE_ADC) |
                CAL_CLI_BIT(CAL_CLI_LIVE_ENCODER) | CAL_CLI_BIT(CAL_CLI_GAIN_CANDIDATE) |
                CAL_CLI_BIT(CAL_CLI_FCAL_LOAD) | CAL_CLI_BIT(CAL_CLI_FCAL_SAVE)
        );
        const int status = foc_virtual_cal_cli_restore(
            load_config,
            &profile,
            &request.identity,
            approved,
            diagnostic,
            timing,
            boot_scenario
        );
        if (known_boot && status == 0) {
            printf(
                "{\"event\":\"BOOTSTRAP_TRUSTED_REFERENCE\",\"status\":\"%s\","
                "\"mechanical_scan_required\":false,\"electrical_drive\":false}\n",
                approved == NULL ? "REFERENCE_REVIEW_REQUIRED" : "READY_WITH_TRUSTED_REFERENCE"
            );
        }
        return status;
    }
    uint32_t boot_signature = 0U;
    foc_virtual_cal_cli_timing_reset(
        timing,
        CAL_CLI_APPLY_STAGES | CAL_CLI_CAPTURE_STAGES | CAL_CLI_BOOTSTRAP_STAGES |
            CAL_CLI_BIT(CAL_CLI_QUIESCENT) | CAL_CLI_BIT(CAL_CLI_GAIN_CANDIDATE) |
            CAL_CLI_BIT(CAL_CLI_FCAL_SAVE) | CAL_CLI_BIT(CAL_CLI_FCAL_LOAD) |
            (current_study_path != NULL ? CAL_CLI_BIT(CAL_CLI_CONTROL_DIAGNOSTIC) : 0U)
    );
    foc_virtual_cal_cli_timing_omit(
        timing,
        CAL_CLI_BIT(CAL_CLI_FCAL_LOAD) | (save_config == NULL ? CAL_CLI_BIT(CAL_CLI_FCAL_SAVE) : 0U),
        CAL_CLI_SKIPPED
    );
    if (measured_boot) {
        const int status = foc_virtual_cal_cli_bootstrap_timed(
            bootstrap,
            boot_scenario,
            boot_trace,
            &profile,
            &request,
            &boot_signature,
            timing
        );
        if (status != 0 || strcmp(bootstrap, "inspect") == 0) {
            return status;
        }
    }
    foc_virtual_cal_cli_timing_start(timing, CAL_CLI_QUIESCENT);
    const int started = foc_virtual_calibration_start(&session, &profile, &request);
    foc_virtual_cal_cli_timing_finish(
        timing, CAL_CLI_QUIESCENT, started == FOC_VIRTUAL_CAL_PENDING, 0
    );
    if (started != FOC_VIRTUAL_CAL_PENDING) {
        return foc_virtual_cal_cli_error("INVALID_PROFILE_OR_RUNTIME", "SETUP", session.status);
    }
    printf(
        "{\"event\":\"CALIBRATION_STARTED\",\"profile_id\":\"%s\",\"session\":%" PRIu32 "}\n",
        profile_name,
        request.session
    );
    struct endpoint_trace trace = {0};
    struct foc_flux_id_result flux_evidence;
    const struct foc_virtual_flux_options flux_options = {
        .rl_revision = request.session,
        .poles_revision = request.session,
        .current_revision = request.session,
        .voltage_chain = 1U,
        .angle_reference = measured_boot ? 402U : 1U,
        .interval = flux_trace ? trace_flux_interval : NULL,
        .argument = &trace,
    };
    if (trace_path != NULL) {
#if defined(_MSC_VER)
        (void)fopen_s(&trace.file, trace_path, "wbx");
#else
        trace.file = fopen(trace_path, "wbx");
#endif
        if (trace.file == NULL) {
            foc_virtual_calibration_cancel(&session);
            return foc_virtual_cal_cli_error("TRACE_IO", "SETUP", -1);
        }
        trace.failed = flux_trace
            ? fputs(
                  "session,capture,motor,sequence,begin_us,end_us,voltage_begin_us,voltage_end_us,"
                  "speed_begin_us,speed_end_us,rl_revision,poles_revision,current_revision,"
                  "id_begin_a,iq_begin_a,id_end_a,iq_end_a,omega_e_rad_s,mean_vq_v,voltage_source,"
                  "current_valid,voltage_valid,speed_valid,frame_confirmed,output_confirmed,"
                  "terminal_voltage_witnessed,saturated\n",
                  trace.file
              ) < 0
            : fputs(
                  "axis,motor_setup_id,capture_id,sequence,aperture_us,begin_us,end_us,"
                  "current_a,terminal_v,voltage_source,voltage_valid,stationary,output_confirmed\n",
                  trace.file
              ) < 0;
        if (!flux_trace) {
            session.electrical.rl_endpoint = trace_rl;
            session.electrical.rl_endpoint_argument = &trace;
        }
    }
    while (session.status == FOC_VIRTUAL_CAL_PENDING) {
        const char *stage = foc_virtual_calibration_stage_name(session.stage);
        const enum foc_virtual_cal_cli_stage timed_stage =
            (enum foc_virtual_cal_cli_stage)session.stage;
        const uint32_t before = session.time_us;
        const bool measured_stage = session.stage != FOC_VIRTUAL_CAL_REVIEW;
        if (measured_stage) {
            foc_virtual_cal_cli_timing_start(timing, timed_stage);
        }
        const int status = measured_boot
            ? foc_virtual_calibration_poll_referenced(&session, &flux_options, &flux_evidence)
            : foc_virtual_calibration_poll_observed(
                  &session,
                  flux_trace ? &flux_options : NULL,
                  &flux_evidence
              );
        if (measured_stage) {
            foc_virtual_cal_cli_timing_finish(
                timing, timed_stage, status >= 0 && !trace.failed, session.time_us - before
            );
        }
        if (status < 0 || trace.failed) {
            if (trace.file != NULL) {
                fclose(trace.file);
            }
            if (trace.failed) {
                foc_virtual_calibration_cancel(&session);
                return foc_virtual_cal_cli_error("TRACE_IO", stage, -1);
            }
            const char *reason = status == FOC_VIRTUAL_CAL_ERR_REFERENCE ? "REFERENCE_REQUIRED"
                : status == FOC_VIRTUAL_CAL_ERR_TIMEOUT                  ? "CALIBRATION_TIMEOUT"
                : status == FOC_VIRTUAL_CAL_ERR_TERMINATED               ? "SESSION_TERMINATED"
                                                                         : "CAPTURE_FAILED";
            return foc_virtual_cal_cli_error(reason, stage, session.native_status);
        }
        printf(
            "{\"event\":\"%s\",\"stage\":\"%s\",\"virtual_us\":%" PRIu32 "}\n",
            strcmp(stage, "TUNING") == 0 ? "GAIN_DESIGN_COMPLETED" : "MEASUREMENT_CAPTURED",
            stage,
            session.time_us
        );
        if (cancel_after != NULL && strcmp(cancel_after, stage) == 0) {
            if (trace.file != NULL) {
                fclose(trace.file);
            }
            foc_virtual_calibration_cancel(&session);
            return foc_virtual_cal_cli_error("CALIBRATION_CANCELLED", stage, session.status);
        }
    }
    if (trace.file != NULL && fclose(trace.file) != 0) {
        foc_virtual_calibration_cancel(&session);
        return foc_virtual_cal_cli_error("TRACE_IO", "ELECTRICAL", -1);
    }
    if (cancel_after != NULL) {
        foc_virtual_calibration_cancel(&session);
        return foc_virtual_cal_cli_error("UNKNOWN_CANCEL_STAGE", "SETUP", -1);
    }
    struct foc_cal_application_tuning_policy policy = foc_virtual_cal_cli_tuning;
    struct foc_virtual_cogging_result cogging;
    if (use_cogging) {
        foc_virtual_cal_cli_cogging_fixture(&profile);
        const int status = foc_virtual_cal_cli_cogging_capture(&profile, &session, &cogging);
        if (status != FOC_VIRTUAL_COGGING_OK) {
            foc_virtual_calibration_cancel(&session);
            return foc_virtual_cal_cli_error(
                "COGGING_CAPTURE_OR_HOLDOUT_FAILED",
                "COGGING",
                status
            );
        }
        policy.cogging_lut = &cogging.lut;
        policy.cogging_iq_limit_a = 0.15f;
    }
    if (!foc_virtual_flux_agrees(&session, &flux_evidence) ||
        flux_evidence.identity.session != request.session) {
        return foc_virtual_cal_cli_error("FLUX_EVIDENCE_MISMATCH", "REVIEW", -1);
    }
    foc_virtual_cal_cli_flux_preview(&flux_evidence);
    print_parameters(&session);
    puts("{\"event\":\"CALIBRATION_QUALIFIED\"}");
    struct foc_cal_application_candidate candidate;
    uint32_t review_id;
    foc_virtual_cal_cli_timing_start(timing, CAL_CLI_GAIN_CANDIDATE);
    const int prepared = foc_virtual_cal_cli_draft(&session, &policy, &candidate);
    const bool gain_candidate = prepared == FOC_CAL_APPLICATION_PREPARED &&
        foc_virtual_calibration_package_id(&session, &candidate, &review_id) ==
            FOC_VIRTUAL_PACKAGE_OK;
    foc_virtual_cal_cli_timing_finish(timing, CAL_CLI_GAIN_CANDIDATE, gain_candidate, 0);
    if (!gain_candidate) {
        return foc_virtual_cal_cli_error("GAIN_ADMISSION_FAILED", "REVIEW", prepared);
    }
    if (measured_boot) {
        const uint32_t numerical_id = review_id;
        review_id ^= boot_signature ^ foc_virtual_cal_cli_flux_signature(&flux_evidence);
        printf(
            "{\"event\":\"BOOTSTRAP_FINAL_REFERENCE\",\"quality\":\"QUALIFIED_OFFSET_CANDIDATE\","
            "\"pole_pairs\":%u,\"encoder_direction\":%d,\"relative_direction\":%d,"
            "\"offset_rad\":%.9g,\"offset_rms_rad\":%.9g,\"lut_holdout_rms_rad\":%.9g,"
            "\"fcal_review_id\":\"%08" PRIx32 "\",\"review_id\":\"%08" PRIx32 "\"}\n",
            (unsigned int)session.workflow.result.rotor.pole_pairs,
            (int)session.workflow.result.snapshot.runtime_encoder_direction,
            (int)session.workflow.result.rotor.direction,
            (double)session.workflow.result.offset.electrical_offset_rad,
            (double)session.workflow.result.offset.residual_rms_rad,
            session.workflow.result.lut.holdout_rms_rad,
            numerical_id,
            review_id
        );
    }
    foc_virtual_cal_cli_preview(&candidate, review_id);
    if (approved == NULL) {
        puts("{\"event\":\"REVIEW_REQUIRED\",\"configuration_applied\":false}");
        return 0;
    }
    foc_virtual_cal_cli_timing_start(timing, CAL_CLI_REVIEW);
    if (!foc_virtual_cal_cli_review_matches(approved, review_id)) {
        foc_virtual_cal_cli_timing_finish(timing, CAL_CLI_REVIEW, false, 0);
        return foc_virtual_cal_cli_error("REVIEW_ID_MISMATCH", "REVIEW", -1);
    }
    if (foc_virtual_calibration_poll(&session) != FOC_VIRTUAL_CAL_OK ||
        !foc_virtual_flux_agrees(&session, &flux_evidence) ||
        foc_cal_workflow_review(&session.workflow, request.session, &request.identity, true) != 0) {
        foc_virtual_cal_cli_timing_finish(timing, CAL_CLI_REVIEW, false, 0);
        return foc_virtual_cal_cli_error("REVIEW_BINDING_REVOKED", "REVIEW", -1);
    }
    foc_virtual_cal_cli_timing_finish(timing, CAL_CLI_REVIEW, true, 0);
    printf("{\"event\":\"CANDIDATE_REVIEWED\",\"review_id\":\"%08" PRIx32 "\"}\n", review_id);
    struct calibrated_init init = {
        .application = &session.application,
        .policy = &policy,
        .candidate = &candidate,
    };
    return foc_virtual_cal_cli_verify(
        &profile,
        &init,
        &session,
        save_config,
        current_study_path,
        startup_study,
        timing
    );
}

/* ---------------------------------------- end of file ----------------------------------------- */
