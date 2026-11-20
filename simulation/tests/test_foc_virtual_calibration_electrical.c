/**
 * @file    test_foc_virtual_calibration_electrical.c
 * @brief   Virtual PMSM excitation, measured electrical identification and qualification tests.
 * @version v0.1.0
 * @date    2026.11.19
 *
 * Test role
 * ---------
 * Compare independently acquired R/L and back-EMF estimates against declared plant truth.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_virtual_calibration_electrical.h"

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

static int witness_rotor(struct foc_cal_workflow *workflow, struct foc_virtual_motor *motor)
{
    const struct foc_cal_workflow_stage_request stage = {
        .stage = FOC_CAL_WORKFLOW_ROTOR,
        .source = FOC_CAL_WORKFLOW_HOST_VIRTUAL_MEASUREMENT,
        .capture = 101U,
        .policy.rotor = {
            .identity = workflow->result.request.identity.acquisition,
            .minimum_pole_pairs = 2U,
            .maximum_pole_pairs = 8U,
            .minimum_mechanical_span_rad = 0.5f,
            .maximum_residual_rad = 0.002f,
            .minimum_candidate_separation_rad = 0.01f,
            .maximum_interval_us = 2000U,
            .maximum_samples = 32U,
        },
    };
    CHECK(foc_cal_workflow_open(workflow, &stage) == FOC_CAL_WORKFLOW_OK);
    CHECK(foc_virtual_motor_set_fixture_speed(motor, 20.0) == 0);
    for (uint32_t i = 0U; i < 32U; ++i) {
        for (unsigned int substep = 0U; substep < 40U; ++substep) {
            CHECK(foc_virtual_motor_step(motor, 0.0, 0.0, 0.00005) == 0);
        }
        struct foc_virtual_motor_observation observed;
        CHECK(foc_virtual_motor_observe(motor, &observed) == 0);
        const uint32_t time = 2000U * (i + 1U);
        const struct foc_cal_workflow_sample sample = {
            .capture = stage.capture,
            .identity = workflow->result.request.identity,
            .session = workflow->result.request.session,
            .source = stage.source,
            .units = FOC_CAL_WORKFLOW_RAD_PAIR,
            .valid = true,
            .data.angle = {
                .mechanical_rad = (float)observed.mechanical_position_rad,
                .electrical_rad = (float)observed.electrical_position_rad,
                .captured_us = time,
                .sequence = i,
                .valid = true,
            },
        };
        CHECK(foc_cal_workflow_push(workflow, &sample, time) == FOC_CAL_WORKFLOW_OK);
    }
    CHECK(foc_virtual_motor_set_fixture_speed(motor, 0.0) == 0);
    foc_virtual_motor_release_fixture(motor);
    return foc_cal_workflow_finish_stage(workflow);
}

static double relative_error(double estimated, double truth)
{
    return fabs(estimated - truth) / fabs(truth);
}

struct endpoint_evidence {
    uint32_t samples[2];
    uint32_t last_us[2];
    float peak_current_a[2];
    float peak_voltage_v[2];
    bool invalid;
};

static void observe_endpoint(
    void *argument,
    enum foc_motor_id_axis axis,
    const struct foc_motor_id_sample *sample
)
{
    struct endpoint_evidence *evidence = argument;
    const unsigned int index = (unsigned int)axis;
    const uint32_t count = evidence->samples[index];
    if (sample->sequence != count || sample->capture_id != 102U + index || !sample->current_valid ||
        !sample->frame_confirmed || !sample->stationary_confirmed ||
        (count != 0U &&
         (!sample->output_confirmed || !sample->voltage_valid ||
          sample->voltage_begin_us != evidence->last_us[index] ||
          sample->voltage_end_us != sample->captured_us ||
          sample->captured_us - sample->voltage_begin_us != 50U))) {
        evidence->invalid = true;
    }
    evidence->samples[index] = count + 1U;
    evidence->last_us[index] = sample->captured_us;
    evidence->peak_current_a[index] =
        fmaxf(evidence->peak_current_a[index], fabsf(sample->current_a));
    if (sample->voltage_valid) {
        evidence->peak_voltage_v[index] =
            fmaxf(evidence->peak_voltage_v[index], fabsf(sample->mean_voltage_v));
    }
}

static int test_plant(const struct foc_virtual_motor_params *truth)
{
    struct foc_virtual_motor motor;
    const struct foc_virtual_motor_state initial = {.mechanical_position_rad = 0.47};
    CHECK(foc_virtual_motor_init(&motor, truth, &initial) == 0);
    struct foc_virtual_cal_electrical_config config = foc_virtual_cal_electrical_default_config();
    config.sensor_adc.phase_a_offset_count += 11.0f;
    config.sensor_adc.phase_b_offset_count -= 15.0f;
    config.sensor_adc.phase_c_offset_count += 19.0f;
    config.sensor_adc.phase_order = FOC_DEVICE_PHASE_ORDER_ACB;
    config.decoded_adc = config.sensor_adc;
    config.decoded_adc.phase_b_offset_count += 0.25f;
    struct endpoint_evidence evidence = {0};
    config.rl_endpoint = observe_endpoint;
    config.rl_endpoint_argument = &evidence;
    const struct foc_cal_workflow_request request = {
        .session = 1U,
        .profile = FOC_CAL_WORKFLOW_SELECTED_STAGES,
        .stages = FOC_CAL_WORKFLOW_STAGE_BIT(FOC_CAL_WORKFLOW_ELECTRICAL) |
            FOC_CAL_WORKFLOW_STAGE_BIT(FOC_CAL_WORKFLOW_ROTOR),
        .identity =
            {
                .acquisition =
                    {
                        4095U,
                        config.sensor_adc.current_a_per_count,
                        65536U,
                        1,
                        FOC_DEVICE_PHASE_ORDER_ACB,
                        (uint16_t)truth->pole_pairs,
                    },
                .motor_setup_id = 120U,
                .encoder_id = 21U,
                .mechanical_frame_id = 32U,
            },
        .require_dual_axis = true,
        .max_rs_relative_difference = 0.03f,
    };
    struct foc_cal_workflow workflow = {0};
    CHECK(foc_cal_workflow_init(&workflow) == FOC_CAL_WORKFLOW_OK);
    CHECK(foc_cal_workflow_start(&workflow, &request) == FOC_CAL_WORKFLOW_OK);
    CHECK(witness_rotor(&workflow, &motor) == FOC_CAL_WORKFLOW_OK);
    CHECK(foc_virtual_motor_init(&motor, truth, &initial) == 0);
    struct foc_virtual_motor_observation observed;
    CHECK(foc_virtual_motor_observe(&motor, &observed) == 0);
    const double alignment = observed.electrical_position_rad;
    CHECK(
        foc_virtual_calibrate_rl_axis(
            &workflow,
            &motor,
            &config,
            FOC_MOTOR_ID_D,
            102U,
            100000U,
            alignment + 0.3
        ) == FOC_CAL_WORKFLOW_ERR_QUALITY
    );
    CHECK(
        foc_virtual_calibrate_rl_axis(
            &workflow,
            &motor,
            &config,
            FOC_MOTOR_ID_D,
            102U,
            100000U,
            alignment
        ) == FOC_CAL_WORKFLOW_PARTIAL
    );
    const struct foc_cal_workflow_bundle *bundle = foc_cal_workflow_result(&workflow);
    const uint32_t d_last = bundle->stages[FOC_CAL_WORKFLOW_ELECTRICAL].primary.last_us;
    CHECK(bundle->electrical.usable);
    CHECK(bundle->passed == FOC_CAL_WORKFLOW_STAGE_BIT(FOC_CAL_WORKFLOW_ROTOR));
    CHECK(
        foc_virtual_calibrate_rl_axis(
            &workflow,
            &motor,
            &config,
            FOC_MOTOR_ID_Q,
            103U,
            d_last + 200U,
            alignment
        ) == FOC_CAL_WORKFLOW_OK
    );
    CHECK(foc_virtual_motor_observe(&motor, &observed) == 0);
    CHECK(!observed.fixture_active && observed.mechanical_velocity_rad_s == 0.0);
    CHECK(observed.mechanical_position_rad == initial.mechanical_position_rad);
    CHECK(observed.v_alpha_v == 0.0 && observed.v_beta_v == 0.0);
    CHECK(relative_error(bundle->electrical_model.rs_ohm, truth->rs_ohm) < 0.02);
    CHECK(relative_error(bundle->electrical_model.ld_h, truth->ld_h) < 0.02);
    CHECK(relative_error(bundle->electrical_model.lq_h, truth->lq_h) < 0.02);
    CHECK(bundle->electrical_q.usable && bundle->electrical_q_capture.id == 103U);
    CHECK(bundle->electrical_q_capture.samples == bundle->electrical_q.samples);
    CHECK(!evidence.invalid && evidence.samples[0] == 769U && evidence.samples[1] == 769U);
    CHECK(
        evidence.last_us[0] == d_last && evidence.last_us[1] == bundle->electrical_q_capture.last_us
    );
    CHECK(evidence.peak_current_a[0] > 0.5f && evidence.peak_current_a[1] > 0.5f);
    CHECK(relative_error(evidence.peak_voltage_v[0], config.rl_amplitude_v) < 0.001);
    CHECK(relative_error(evidence.peak_voltage_v[1], config.rl_amplitude_v) < 0.001);

    struct foc_virtual_cal_flux_result flux;
    const uint32_t flux_start = bundle->electrical_q_capture.last_us + 200U;
    CHECK(
        foc_virtual_calibrate_flux(&motor, &config, bundle, 103U, flux_start, &flux) ==
        FOC_CAL_WORKFLOW_ERR_ORDER
    );
    CHECK(!flux.qualified);
    const int status = foc_virtual_calibrate_flux(&motor, &config, bundle, 104U, flux_start, &flux);
    CHECK(foc_virtual_motor_observe(&motor, &observed) == 0);
    CHECK(!observed.fixture_active && observed.mechanical_velocity_rad_s == 0.0);
    CHECK(observed.v_alpha_v == 0.0 && observed.v_beta_v == 0.0);
    if (truth->psi_f_wb == 0.0) {
        CHECK(status == FOC_CAL_WORKFLOW_ERR_QUALITY && !flux.qualified && !flux.kt.qualified);
        CHECK(flux.flux_wb == 0.0f && flux.failure == FOC_CALIBRATION_FAILURE_RANGE);
        return 0;
    }
    CHECK(status == FOC_CAL_WORKFLOW_OK && flux.qualified && flux.kt.qualified);
    CHECK(relative_error(flux.flux_wb, truth->psi_f_wb) < 0.025);
    CHECK(
        relative_error(
            flux.ke_phase_peak_v_per_mechanical_rad_s,
            truth->pole_pairs * truth->psi_f_wb
        ) < 0.025
    );
    CHECK(
        relative_error(flux.kt_nm_per_peak_q_amp, 1.5 * truth->pole_pairs * truth->psi_f_wb) < 0.025
    );
    CHECK(flux.capture == 104U && flux.samples > 200U && flux.first_us == flux_start);
    CHECK(flux.kt.captured_us == flux.last_us && flux.kt.capture == flux.capture);
    CHECK(flux.kt.source == FOC_CAL_WORKFLOW_HOST_VIRTUAL_MEASUREMENT);
    CHECK(foc_cal_workflow_identity_matches(&flux.kt.identity, &request.identity));
    printf(
        "Electrical: Rs %.6g Ld %.6g Lq %.6g residuals %.6g %.6g excitation %.6g %.6g "
        "flux %.6g Ke %.6g Kt %.6g flux residual %.6g electrical speed RMS %.6g\n",
        bundle->electrical_model.rs_ohm,
        bundle->electrical_model.ld_h,
        bundle->electrical_model.lq_h,
        bundle->electrical.residual_rms_v,
        bundle->electrical_q.residual_rms_v,
        bundle->electrical.excitation,
        bundle->electrical_q.excitation,
        flux.flux_wb,
        flux.ke_phase_peak_v_per_mechanical_rad_s,
        flux.kt_nm_per_peak_q_amp,
        flux.residual_rms_v,
        flux.omega_e_rms_rad_s
    );
    return 0;
}

/* Test Entry ------------------------------------------------------------------- */

int main(void)
{
    const struct foc_virtual_motor_params motors[] = {
        {0.6, 0.0012, 0.0015, 0.025, 4U, 0.37, 0.002, 0.006},
        {0.44, 0.0019, 0.0009, 0.016, 6U, -0.76, 0.0014, 0.01},
        {0.7, 0.0015, 0.0021, 0.0, 4U, 0.52, 0.002, 0.006},
    };
    for (unsigned int i = 0U; i < sizeof motors / sizeof motors[0]; ++i) {
        CHECK(test_plant(&motors[i]) == 0);
    }
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
