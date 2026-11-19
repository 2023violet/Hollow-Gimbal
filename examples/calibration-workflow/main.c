/**
 * @file    main.c
 * @brief   Exercise a full calibration profile and apply it to a new Host Runtime.
 * @version v0.1.0
 * @date    2026.11.18
 *
 * Implementation role
 * -------------------
 * Feed deterministic, independently qualified synthetic observations through
 * public C11 APIs. Read the candidate and both Runtime instances back.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_calibration_application.h"
#include "foc_virtual_current.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Private Constants ------------------------------------------------------------ */

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "calibration demo:%d: %s\n", __LINE__, #condition); \
            return 1;                                                           \
        }                                                                       \
    } while (0)

static const double tau = 6.28318530717958647692;
static const double fixture_rs_ohm = 0.57;
static const double fixture_l_h = 0.00135;
static const double fixture_j_kg_m2 = 0.0024;
static const double fixture_b_nm_s_per_rad = 0.007;
static const float fixture_kt_nm_per_a = 0.15f;
static const float fixture_electrical_offset_rad = 1.2f;
static const uint32_t fixture_torque_model = 13U;
static const uint32_t fixture_reference = 41U;

/* Measurement CSV v1 values used by the Host fixture. Both apertures use clock 1. */
enum fixture_trace_v1 {
    TRACE_HOLDOUT_STAGE = 7,
    TRACE_KT_UNITS = 6,
    TRACE_VALID = 1U,
    TRACE_OUTPUT_CONFIRMED = 2U,
    TRACE_FRAME_CONFIRMED = 8U,
    TRACE_WINDOW_CONFIRMED = 16U,
    TRACE_LOAD_OBSERVED = 32U,
    TRACE_ZERO_CONFIRMED = 1U,
    TRACE_TERMINAL_VIRTUAL = 3U,
    TRACE_INDEPENDENT_ANGLE = 4U,
    TRACE_OBSERVED_DQ = 5U,
    TRACE_INDEPENDENT_KT = 7U,
    TRACE_ADC_ABC = 1U | (2U << 4U) | (3U << 8U),
};

static const char *const stage_names[] = {
    "ADC zero",
    "Electrical R/L (d axis)",
    "Rotor",
    "Electrical offset",
    "Encoder LUT",
    "Mechanical J/B",
    "Servo K/D",
};
static const struct foc_cal_workflow_request full_profile = {
    .session = 3U,
    .profile = FOC_CAL_WORKFLOW_FULL_PROFILE,
    .identity =
        {
            .acquisition =
                {
                    .adc_full_scale_counts = 4095U,
                    .current_a_per_count = 0.001f,
                    .encoder_counts_per_revolution = 1024U,
                    .encoder_direction = FOC_ENCODER_DIRECTION_REVERSED,
                    .phase_order = FOC_DEVICE_PHASE_ORDER_ABC,
                    .pole_pairs = 4U,
                },
            .motor_setup_id = 201U,
            .encoder_id = 101U,
            .mechanical_frame_id = 71U,
        },
    .admission = {
        .minimum_adc_samples = 32U,
        .minimum_offset_counts = 32.0f,
        .maximum_offset_counts = 4063.0f,
        .maximum_noise_rms_counts = 2.0f,
        .maximum_rotor_residual_rad = 0.02f,
        .maximum_offset_residual_rad = 0.02f,
    },
};

/* Private Helpers -------------------------------------------------------------- */

static struct foc_cal_workflow_stage_request capture(enum foc_cal_workflow_stage stage, uint32_t id)
{
    return (struct foc_cal_workflow_stage_request){
        .stage = stage,
        .source = FOC_CAL_WORKFLOW_HOST_SYNTHETIC,
        .capture = id,
        .max_age_us = 50U,
    };
}

static struct foc_cal_workflow_sample observation(
    const struct foc_cal_workflow_request *work,
    uint32_t id,
    enum foc_cal_workflow_units units
)
{
    return (struct foc_cal_workflow_sample){
        .session = work->session,
        .capture = id,
        .identity = work->identity,
        .source = FOC_CAL_WORKFLOW_HOST_SYNTHETIC,
        .units = units,
        .valid = true,
    };
}

static int open_capture(
    struct foc_cal_workflow *workflow,
    const struct foc_cal_workflow_stage_request *request
)
{
    CHECK(foc_cal_workflow_open(workflow, request) == FOC_CAL_WORKFLOW_OK);
    const struct foc_cal_workflow_bundle *bundle = foc_cal_workflow_result(workflow);
    CHECK(bundle != NULL && bundle->active == request->stage);
    CHECK(bundle->state == FOC_CAL_WORKFLOW_WAITING_FOR_DATA);
    printf(
        "%s: WAITING_FOR_DATA capture=%" PRIu32 "\n",
        stage_names[request->stage],
        request->capture
    );
    return 0;
}

static int print_pass(const struct foc_cal_workflow *workflow, enum foc_cal_workflow_stage stage)
{
    const struct foc_cal_workflow_stage_result *result =
        &foc_cal_workflow_result(workflow)->stages[stage];
    CHECK(result->quality == FOC_CAL_WORKFLOW_PASS && result->status == FOC_CAL_WORKFLOW_OK);
    CHECK(result->algorithm_status == 0 && result->failure == FOC_CALIBRATION_FAILURE_NONE);
    if (stage != FOC_CAL_WORKFLOW_TUNING) {
        CHECK(result->primary.samples != 0U);
        CHECK(result->source == FOC_CAL_WORKFLOW_HOST_SYNTHETIC);
        printf(
            "%s: PASS source=HOST_SYNTHETIC samples=%" PRIu32 " first_us=%" PRIu32
            " last_us=%" PRIu32 "\n",
            stage_names[stage],
            result->primary.samples,
            result->primary.first_us,
            result->primary.last_us
        );
    } else {
        printf("%s: PASS derived from qualified inputs\n", stage_names[stage]);
    }
    return 0;
}

static struct foc_cal_workflow_stage_request
adc_capture(const struct foc_cal_workflow_request *work, uint32_t id)
{
    struct foc_cal_workflow_stage_request request = capture(FOC_CAL_WORKFLOW_ADC, id);
    request.policy.adc = (struct foc_calibration_adc_config){
        .identity = work->identity.acquisition,
        .required_valid_mask = 7U,
        .minimum_samples = 32U,
        .maximum_samples = 64U,
        .maximum_sample_step_counts = 20.0f,
        .maximum_noise_rms_counts = 2.0f,
        .minimum_offset_counts = 32.0f,
        .maximum_offset_counts = 4063.0f,
        .maximum_interval_us = 1000U,
    };
    return request;
}

static struct foc_cal_workflow_sample
adc_observation(const struct foc_cal_workflow_request *work, uint32_t id, uint32_t sequence)
{
    struct foc_cal_workflow_sample sample = observation(work, id, FOC_CAL_WORKFLOW_RAW_COUNTS);
    const int variance = (int)(sequence % 3U) - 1;
    sample.data.adc = (struct foc_calibration_adc_sample){
        .phase_a = (uint16_t)(2043 + variance),
        .phase_b = (uint16_t)(2052 - variance),
        .phase_c = (uint16_t)(2048 + sequence % 2U),
        .valid_mask = 7U,
        .sequence = sequence,
        .captured_us = 1000U + sequence * 10U,
        /* Current is zero by fixture construction, independent of raw counts. */
        .zero_current_confirmed = true,
    };
    return sample;
}

static int run_adc(struct foc_cal_workflow *workflow, FILE *trace)
{
    const struct foc_cal_workflow_stage_request request = adc_capture(&full_profile, 10U);
    CHECK(open_capture(workflow, &request) == 0);
    for (uint32_t i = 0U; i < 64U; ++i) {
        const struct foc_cal_workflow_sample sample =
            adc_observation(&full_profile, request.capture, i);
        const struct foc_calibration_adc_sample *adc = &sample.data.adc;
        const uint32_t now_us = adc->captured_us + 3U;
        if (trace != NULL) {
            CHECK(
                fprintf(
                    trace,
                    "%u,%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",,,1,,"
                    "%u,%u,%u,%u,%u,%u,%u,,,\n",
                    (unsigned int)request.stage,
                    sample.capture,
                    adc->sequence,
                    adc->captured_us,
                    now_us,
                    (unsigned int)sample.units,
                    (unsigned int)TRACE_ADC_ABC,
                    (unsigned int)(TRACE_VALID | TRACE_WINDOW_CONFIRMED),
                    (unsigned int)TRACE_ZERO_CONFIRMED,
                    (unsigned int)adc->phase_a,
                    (unsigned int)adc->phase_b,
                    (unsigned int)adc->phase_c
                ) >= 0
            );
        }
        CHECK(foc_cal_workflow_push(workflow, &sample, now_us) == 0);
    }
    CHECK(foc_cal_workflow_result(workflow)->state == FOC_CAL_WORKFLOW_RUNNING);
    CHECK(foc_cal_workflow_finish_stage(workflow) == 0);
    CHECK(print_pass(workflow, request.stage) == 0);
    const struct foc_calibration_adc_result *adc = &foc_cal_workflow_result(workflow)->adc;
    printf(
        "  offsets_counts=(%.3f, %.3f, %.3f) RMS_noise_counts=(%.3f, %.3f, %.3f)\n",
        adc->offset_a_counts,
        adc->offset_b_counts,
        adc->offset_c_counts,
        adc->noise_rms_a_counts,
        adc->noise_rms_b_counts,
        adc->noise_rms_c_counts
    );
    return 0;
}

static double
electrical_response(double current, double voltage, double seconds, double rs, double l)
{
    const double steady_current = voltage / rs;
    return steady_current + (current - steady_current) * exp(-seconds * rs / l);
}

static int run_electrical(struct foc_cal_workflow *workflow, FILE *trace)
{
    struct foc_cal_workflow_stage_request request = capture(FOC_CAL_WORKFLOW_ELECTRICAL, 11U);
    request.policy.electrical = (struct foc_motor_id_config){
        .axis = FOC_MOTOR_ID_D,
        .source = FOC_MOTOR_ID_VIRTUAL,
        .voltage_source = FOC_MOTOR_ID_VOLTAGE_VIRTUAL_TERMINAL,
        .setup_id = full_profile.identity.motor_setup_id,
        .min_interval_us = 25U,
        .max_interval_us = 50U,
        .intervals_per_window = 8U,
        .min_windows = 6U,
        .max_abs_current_a = 3.0f,
        .max_abs_voltage_v = 3.0f,
        .min_current_rms_a = 0.1f,
        .min_slew_rms_a_per_s = 5.0f,
        .min_voltage_rms_v = 0.05f,
        .max_residual_rms_v = 0.002f,
        .min_rs_ohm = 0.03f,
        .max_rs_ohm = 2.0f,
        .min_inductance_h = 0.0004f,
        .max_inductance_h = 0.01f,
    };
    CHECK(open_capture(workflow, &request) == 0);
    struct foc_cal_workflow_sample sample =
        observation(&full_profile, request.capture, FOC_CAL_WORKFLOW_ELECTRICAL_SI);
    double current = 0.0;
    for (uint32_t i = 0U; i <= 320U; ++i) {
        static const double held_voltage_v[] = {0.8, -1.1, 0.4, 1.6, -0.7, -1.2, 0.6};
        const double voltage = held_voltage_v[(i / 19U) % 7U];
        const uint32_t time = 10000U + i * 40U;
        if (i != 0U) {
            current = electrical_response(current, voltage, 40e-6, fixture_rs_ohm, fixture_l_h);
        }
        sample.output_confirmed = i != 0U;
        sample.data.electrical = (struct foc_motor_id_sample){
            .setup_id = request.policy.electrical.setup_id,
            .capture_id = request.capture,
            .source = request.policy.electrical.source,
            .sequence = i,
            .captured_us = time,
            .voltage_begin_us = i == 0U ? time : time - 40U,
            .voltage_end_us = time,
            .voltage_source = request.policy.electrical.voltage_source,
            .current_a = (float)(current + 0.00003 * sin((double)i)),
            .mean_voltage_v = (float)(voltage + 0.0002 * cos((double)i)),
            .current_valid = true,
            .voltage_valid = true,
            .output_confirmed = i != 0U,
            .frame_confirmed = true,
            .stationary_confirmed = true,
        };
        const uint32_t now_us = time + 3U;
        if (trace != NULL) {
            const struct foc_motor_id_sample *electrical = &sample.data.electrical;
            unsigned int flags = TRACE_VALID | TRACE_FRAME_CONFIRMED | TRACE_WINDOW_CONFIRMED;
            if (sample.output_confirmed) {
                flags |= TRACE_OUTPUT_CONFIRMED;
            }
            CHECK(
                fprintf(
                    trace,
                    "%u,%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",,%" PRIu32 ",1,1,"
                    "%u,%u,%u,%u,%.9g,%.9g,%" PRIu32 ",%.9g,,\n",
                    (unsigned int)request.stage,
                    sample.capture,
                    electrical->sequence,
                    electrical->captured_us,
                    now_us,
                    electrical->voltage_end_us,
                    (unsigned int)sample.units,
                    (unsigned int)request.policy.electrical.axis + 1U,
                    flags,
                    (unsigned int)TRACE_TERMINAL_VIRTUAL,
                    (double)electrical->current_a,
                    (double)electrical->mean_voltage_v,
                    electrical->voltage_begin_us,
                    (double)electrical->omega_e_rad_s
                ) >= 0
            );
        }
        CHECK(foc_cal_workflow_push(workflow, &sample, now_us) == 0);
    }
    CHECK(foc_cal_workflow_finish_stage(workflow) == 0);
    CHECK(print_pass(workflow, request.stage) == 0);
    const struct foc_motor_id_result *rl = &foc_cal_workflow_result(workflow)->electrical;
    printf(
        "  Rs_ohm=%.7f L_d_H=%.8f residual_V=%.7f excitation=%.5f\n",
        rl->rs_ohm,
        rl->inductance_h,
        rl->residual_rms_v,
        rl->excitation
    );
    return 0;
}

static float wrap_angle(float angle)
{
    const float wrapped = fmodf(angle, (float)tau);
    return wrapped < 0.0f ? wrapped + (float)tau : wrapped;
}

/* Continuous synthetic position. Only the fixture producer uses the ideal table;
 * the workflow and sensor each see raw counts and the independently fitted LUT. */
static double fixture_position_rad(uint32_t count)
{
    static const int correction_counts[] = {0, 4, 8, 5, 0, -4, -7, -3};
    const uint32_t raw = count % 1024U;
    const uint32_t node = raw / 128U;
    const double ratio = (double)(raw % 128U) / 128.0;
    const double correction =
        correction_counts[node] * (1.0 - ratio) + correction_counts[(node + 1U) % 8U] * ratio;
    return (count + correction) * tau / 1024.0;
}

static int run_angle(struct foc_cal_workflow *workflow, bool identification, FILE *trace)
{
    const enum foc_cal_workflow_stage stage =
        identification ? FOC_CAL_WORKFLOW_ROTOR : FOC_CAL_WORKFLOW_OFFSET;
    struct foc_cal_workflow_stage_request request = capture(stage, identification ? 14U : 15U);
    if (identification) {
        request.policy.rotor = (struct foc_calibration_rotor_config){
            .identity = full_profile.identity.acquisition,
            .minimum_pole_pairs = 1U,
            .maximum_pole_pairs = 20U,
            .minimum_mechanical_span_rad = 8.0f,
            .maximum_residual_rad = 0.02f,
            .minimum_candidate_separation_rad = 0.05f,
            .maximum_interval_us = 100000U,
            .maximum_samples = 64U,
        };
    } else {
        request.policy.offset = (struct foc_calibration_offset_config){
            .maximum_residual_rad = 0.02f,
            .maximum_interval_us = 100000U,
            .maximum_samples = 64U,
        };
    }
    CHECK(open_capture(workflow, &request) == 0);
    struct foc_cal_workflow_sample sample =
        observation(&full_profile, request.capture, FOC_CAL_WORKFLOW_RAD_PAIR);
    sample.encoder_raw_valid = true;
    for (uint32_t i = 0U; i < 40U; ++i) {
        const uint32_t count = (identification ? 3U : 5U) + 40U * i;
        const double noise = 0.0005 * sin((double)i);
        const uint32_t time = (identification ? 90000U : 140000U) + 1000U * i;
        const double reference =
            4.0 * fixture_position_rad(count) + fixture_electrical_offset_rad + noise;
        sample.encoder_raw_code = count % 1024U;
        sample.data.angle = (struct foc_calibration_angle_sample){
            .mechanical_rad = (float)(-(double)count * tau / 1024.0),
            .electrical_rad = wrap_angle((float)reference),
            .captured_us = time,
            .sequence = i,
            .valid = true,
        };
        const uint32_t now_us = time + 3U;
        if (trace != NULL) {
            const struct foc_calibration_angle_sample *angle = &sample.data.angle;
            CHECK(
                fprintf(
                    trace,
                    "%u,%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",,%" PRIu32 ",1,1,"
                    "%u,%" PRIu32 ",%u,%u,%" PRIu32 ",%.9g,%.9g,,,\n",
                    (unsigned int)stage,
                    sample.capture,
                    angle->sequence,
                    angle->captured_us,
                    now_us,
                    angle->captured_us,
                    (unsigned int)sample.units,
                    sample.identity.encoder_id,
                    (unsigned int)(TRACE_VALID | TRACE_FRAME_CONFIRMED),
                    (unsigned int)TRACE_INDEPENDENT_ANGLE,
                    sample.encoder_raw_code,
                    (double)angle->mechanical_rad,
                    (double)angle->electrical_rad
                ) >= 0
            );
        }
        CHECK(foc_cal_workflow_push(workflow, &sample, now_us) == 0);
    }
    CHECK(foc_cal_workflow_finish_stage(workflow) == 0);
    CHECK(print_pass(workflow, stage) == 0);
    const struct foc_cal_workflow_bundle *bundle = foc_cal_workflow_result(workflow);
    if (identification) {
        printf(
            "  direction=%" PRId32 " pole_pairs=%u residual_rad=%.6f separation_rad=%.6f\n",
            bundle->rotor.direction,
            (unsigned int)bundle->rotor.pole_pairs,
            bundle->rotor.residual_rms_rad,
            bundle->rotor.candidate_separation_rad
        );
    } else {
        printf(
            "  offset_rad=%.7f residual_rad=%.6f\n",
            bundle->offset.electrical_offset_rad,
            bundle->offset.residual_rms_rad
        );
    }
    return 0;
}

static int push_lut(
    struct foc_cal_workflow *workflow,
    const struct foc_cal_workflow_stage_request *request,
    bool holdout,
    FILE *trace
)
{
    const uint32_t id = request->capture + (holdout ? 1U : 0U);
    const uint32_t stride = holdout ? 6U : 4U;
    struct foc_cal_workflow_sample sample =
        observation(&full_profile, id, FOC_CAL_WORKFLOW_ENCODER_COUNTS_RAD);
    uint32_t sequence = 0U;
    for (uint32_t raw = holdout ? 1U : 0U; raw < 1024U; raw += stride) {
        const double noise = (holdout ? 0.025 : 0.015) * sin((double)sequence * 0.61);
        const double angle = request->policy.lut.reference_zero_rad + fixture_position_rad(raw) +
            noise * tau / 1024.0;
        const uint32_t time = (holdout ? 60000U : 40000U) + sequence * 10U;
        sample.data.lut = (struct foc_encoder_cal_sample){
            .setup =
                {
                    .encoder_identity = request->policy.lut.encoder_identity,
                    .reference_identity = request->policy.lut.reference_identity,
                    .counts_per_revolution = request->policy.lut.encoder.counts_per_revolution,
                    .direction = request->policy.lut.encoder.direction,
                    .source = request->policy.lut.source,
                },
            .capture_id = id,
            .sequence = sequence,
            .encoder_us = time,
            .reference_us = time,
            .raw_code = raw,
            .reference_rad = angle - floor(angle / tau) * tau,
            .encoder_valid = true,
            .reference_valid = true,
            .reference_confirmed = true,
        };
        const uint32_t now_us = time + 3U;
        if (trace != NULL) {
            const struct foc_encoder_cal_sample *lut = &sample.data.lut;
            CHECK(
                fprintf(
                    trace,
                    "%u,%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",,%" PRIu32 ",1,1,"
                    "%u,%" PRIu32 ",%u,%u,%" PRIu32 ",%.17g,,,,\n",
                    holdout ? (unsigned int)TRACE_HOLDOUT_STAGE : (unsigned int)request->stage,
                    sample.capture,
                    lut->sequence,
                    lut->encoder_us,
                    now_us,
                    lut->reference_us,
                    (unsigned int)sample.units,
                    lut->setup.encoder_identity,
                    (unsigned int)(TRACE_VALID | TRACE_FRAME_CONFIRMED),
                    (unsigned int)TRACE_INDEPENDENT_ANGLE,
                    lut->raw_code,
                    lut->reference_rad
                ) >= 0
            );
        }
        CHECK(foc_cal_workflow_push(workflow, &sample, now_us) == 0);
        ++sequence;
    }
    return 0;
}

static int
run_lut(struct foc_cal_workflow *workflow, const struct foc_encoder_config *encoder, FILE *trace)
{
    struct foc_cal_workflow_stage_request request = capture(FOC_CAL_WORKFLOW_LUT, 12U);
    request.policy.lut = (struct foc_encoder_cal_config){
        .encoder = *encoder,
        .encoder_identity = full_profile.identity.encoder_id,
        .reference_identity = fixture_reference,
        .source = FOC_ENCODER_CAL_SOURCE_VIRTUAL,
        .nodes = 8U,
        .min_samples_per_segment = 8U,
        .reference_zero_rad = 0.31,
        .max_zero_bias_rad = 0.004,
        .max_training_segment_rms_rad = 0.004,
        .max_holdout_rms_rad = 0.004,
        .max_holdout_peak_rad = 0.006,
    };
    CHECK(open_capture(workflow, &request) == 0);
    CHECK(push_lut(workflow, &request, false, trace) == 0);
    CHECK(foc_cal_workflow_begin_holdout(workflow, request.capture + 1U) == 0);
    CHECK(push_lut(workflow, &request, true, trace) == 0);
    CHECK(foc_cal_workflow_finish_stage(workflow) == 0);
    CHECK(print_pass(workflow, request.stage) == 0);
    const struct foc_cal_workflow_bundle *bundle = foc_cal_workflow_result(workflow);
    const struct foc_cal_workflow_capture *held = &bundle->stages[request.stage].holdout;
    CHECK(held->id != request.capture && held->first_us > bundle->lut.training.last_us);
    CHECK(bundle->lut.holdout_raw_rms_rad > bundle->lut.holdout_rms_rad);
    printf(
        "  LUT nodes=%" PRIu32 " train_RMS_rad=%.7f holdout_capture=%" PRIu32
        " holdout_samples=%" PRIu32 " holdout_us=%" PRIu32 "..%" PRIu32
        " raw_RMS_rad=%.7f corrected_RMS_rad=%.7f peak_rad=%.7f\n",
        bundle->lut.nodes,
        bundle->lut.training_rms_rad,
        held->id,
        held->samples,
        held->first_us,
        held->last_us,
        bundle->lut.holdout_raw_rms_rad,
        bundle->lut.holdout_rms_rad,
        bundle->lut.holdout_peak_rad
    );
    return 0;
}

static int run_mechanical(struct foc_cal_workflow *workflow, FILE *trace)
{
    struct foc_cal_workflow_stage_request request = capture(FOC_CAL_WORKFLOW_MECHANICAL, 16U);
    request.policy.mechanical = (struct foc_mechanical_id_config){
        .setup_id = full_profile.identity.motor_setup_id,
        .torque_model_id = fixture_torque_model,
        .torque_source = FOC_MECHANICAL_ID_TORQUE_OBSERVED_DQ,
        .load_source = FOC_MECHANICAL_ID_LOAD_OBSERVED,
        .min_interval_us = 900U,
        .max_interval_us = 1100U,
        .intervals_per_window = 100U,
        .min_windows = 6U,
        .torque_constant_nm_per_a = fixture_kt_nm_per_a,
        .max_abs_id_a = 0.1f,
        .speed_resolution_rad_s = 0.001f,
        .max_abs_speed_rad_s = 20.0f,
        .max_abs_torque_nm = 2.0f,
        .min_accel_rms_rad_s2 = 0.1f,
        .min_speed_rms_rad_s = 0.1f,
        .min_excitation = 0.1f,
        .max_residual_rms_nm = 0.0001f,
        .min_inertia_kg_m2 = 0.0005f,
        .max_inertia_kg_m2 = 0.1f,
        .max_damping_nm_s = 0.1f,
    };
    CHECK(open_capture(workflow, &request) == 0);
    struct foc_cal_workflow_sample sample =
        observation(&full_profile, request.capture, FOC_CAL_WORKFLOW_MECHANICAL_SI);
    sample.output_confirmed = true;
    const uint32_t first_us = 200000U;
    uint32_t time = first_us;
    for (uint32_t i = 0U; i <= 2400U; ++i) {
        const uint32_t previous = time;
        if (i != 0U) {
            time += i % 2U ? 940U : 1060U;
        }
        const double phase = (time - first_us) * 1e-6 * tau;
        const double speed = 2.0 * sin(phase) + 0.5 * cos(0.5 * phase);
        const double acceleration = 2.0 * tau * cos(phase) - 0.25 * tau * sin(0.5 * phase);
        const double load = 0.025 * cos(0.45 * phase) + 0.004 * sin(2.2 * phase);
        const double torque =
            fixture_j_kg_m2 * acceleration + fixture_b_nm_s_per_rad * speed + load;
        sample.data.mechanical = (struct foc_mechanical_id_sample){
            .setup_id = request.policy.mechanical.setup_id,
            .torque_model_id = request.policy.mechanical.torque_model_id,
            .capture_id = request.capture,
            .torque_source = request.policy.mechanical.torque_source,
            .load_source = request.policy.mechanical.load_source,
            .sequence = i,
            .speed_us = time,
            .torque_us = time,
            .interval_begin_us = previous,
            .speed_rad_s = (float)(speed + 0.00005 * sin((double)i)),
            .iq_a = (float)(torque / fixture_kt_nm_per_a),
            .load_nm = (float)load,
            .speed_valid = true,
            .torque_valid = true,
            .frame_confirmed = true,
            .interval_confirmed = true,
        };
        const uint32_t now_us = time + 3U;
        if (trace != NULL) {
            const struct foc_mechanical_id_sample *mechanical = &sample.data.mechanical;
            CHECK(
                fprintf(
                    trace,
                    "%u,%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",,%" PRIu32 ",1,1,"
                    "%u,%" PRIu32 ",%u,%u,%.9g,%.9g,%.9g,%.9g,%.9g,%" PRIu32 "\n",
                    (unsigned int)request.stage,
                    sample.capture,
                    mechanical->sequence,
                    mechanical->speed_us,
                    now_us,
                    mechanical->torque_us,
                    (unsigned int)sample.units,
                    mechanical->torque_model_id,
                    (unsigned int)(TRACE_VALID | TRACE_OUTPUT_CONFIRMED | TRACE_FRAME_CONFIRMED |
                                   TRACE_WINDOW_CONFIRMED | TRACE_LOAD_OBSERVED),
                    (unsigned int)TRACE_OBSERVED_DQ,
                    (double)mechanical->speed_rad_s,
                    (double)mechanical->iq_a,
                    (double)mechanical->id_a,
                    (double)mechanical->torque_nm,
                    (double)mechanical->load_nm,
                    mechanical->interval_begin_us
                ) >= 0
            );
        }
        CHECK(foc_cal_workflow_push(workflow, &sample, now_us) == 0);
    }
    CHECK(foc_cal_workflow_finish_stage(workflow) == 0);
    CHECK(print_pass(workflow, request.stage) == 0);
    const struct foc_mechanical_id_result *jb = &foc_cal_workflow_result(workflow)->mechanical;
    printf(
        "  J_kg_m2=%.8f B_Nm_s_per_rad=%.8f residual_Nm=%.8f excitation=%.5f"
        " windows=%" PRIu32 " dt_us=940/1060\n",
        jb->inertia_kg_m2,
        jb->damping_nm_s,
        jb->residual_rms_nm,
        jb->excitation,
        jb->windows
    );
    return 0;
}

static int run_tuning(struct foc_cal_workflow *workflow, const struct foc_cal_workflow_kt *kt)
{
    const struct foc_impedance_tuning_input target = {
        .target_natural_frequency_rad_s = 18.0f,
        .target_damping_ratio = 0.75f,
        .max_stiffness_nm_per_rad = 3.0f,
        .max_damping_nm_s_per_rad = 0.3f,
        .iq_limit_a = 2.2f,
        .sample_period_s = 0.0005f,
        .extra_delay_s = 0.0006f,
    };
    CHECK(foc_cal_workflow_tune(workflow, kt, &target) == 0);
    CHECK(print_pass(workflow, FOC_CAL_WORKFLOW_TUNING) == 0);
    const struct foc_impedance_tuning_candidate *gains = &foc_cal_workflow_result(workflow)->tuning;
    printf(
        "  Kt_Nm_per_A=%.4f source=HOST_SYNTHETIC capture=%" PRIu32 " captured_us=%" PRIu32
        " K_Nm_per_rad=%.7f D_Nm_s_per_rad=%.7f ideal_zeta=%.7f\n",
        kt->nm_per_a,
        kt->capture,
        kt->captured_us,
        gains->stiffness_nm_per_rad,
        gains->damping_nm_s_per_rad,
        gains->ideal_damping_ratio
    );
    return 0;
}

static int independent_holdout(const struct foc_cal_workflow_bundle *bundle)
{
    double measured_i = -0.32;
    double predicted_i = measured_i;
    double current_sse = 0.0;
    double torque_sse = 0.0;
    double time = 0.0;
    for (unsigned int i = 0U; i < 240U; ++i) {
        const double voltage = 0.55 * cos((double)i * 0.031) + 0.49 * sin((double)i * 0.173);
        const double electrical_dt = i % 2U ? 35e-6 : 45e-6;
        measured_i =
            electrical_response(measured_i, voltage, electrical_dt, fixture_rs_ohm, fixture_l_h);
        predicted_i = electrical_response(
            predicted_i,
            voltage,
            electrical_dt,
            bundle->electrical.rs_ohm,
            bundle->electrical.inductance_h
        );
        const double current_error = predicted_i - (measured_i + 0.00008 * sin((double)i));
        current_sse += current_error * current_error;

        time += i % 2U ? 0.00093 : 0.00107;
        const double speed = 1.3 * cos(7.0 * time) + 0.9 * sin(12.0 * time);
        const double acceleration = -9.1 * sin(7.0 * time) + 10.8 * cos(12.0 * time);
        const double load = -0.02 * cos(11.0 * time);
        const double measured_torque = fixture_j_kg_m2 * acceleration +
            fixture_b_nm_s_per_rad * speed + load + 0.00001 * cos((double)i);
        const double predicted_torque = bundle->mechanical.inertia_kg_m2 * acceleration +
            bundle->mechanical.damping_nm_s * speed + load;
        const double torque_error = predicted_torque - measured_torque;
        torque_sse += torque_error * torque_error;
    }
    const double current_rms = sqrt(current_sse / 240.0);
    const double torque_rms = sqrt(torque_sse / 240.0);
    printf(
        "HOST_SYNTHETIC held waveforms R/L current_RMS_A=%.8f J/B torque_RMS_Nm=%.8f\n",
        current_rms,
        torque_rms
    );
    CHECK(current_rms < 0.001 && torque_rms < 0.0001);
    return 0;
}

static int rejected_and_cancelled(struct foc_cal_workflow *workflow)
{
    struct foc_cal_workflow_request negative = full_profile;
    negative.profile = FOC_CAL_WORKFLOW_SELECTED_STAGES;
    negative.stages = FOC_CAL_WORKFLOW_STAGE_BIT(FOC_CAL_WORKFLOW_ADC);
    negative.session = 4U;
    struct foc_cal_workflow_stage_request adc = adc_capture(&negative, 21U);
    CHECK(foc_cal_workflow_start(workflow, &negative) == 0);
    CHECK(open_capture(workflow, &adc) == 0);
    struct foc_cal_workflow_sample sample = adc_observation(&negative, adc.capture, 0U);
    CHECK(foc_cal_workflow_push(workflow, &sample, 1003U) == 0);
    const int rejected = foc_cal_workflow_push(workflow, &sample, 1003U);
    const struct foc_cal_workflow_bundle *bundle = foc_cal_workflow_result(workflow);
    CHECK(rejected == FOC_CAL_WORKFLOW_ERR_ORDER && !bundle->applicable);
    CHECK(bundle->stages[FOC_CAL_WORKFLOW_ADC].failure == FOC_CALIBRATION_FAILURE_TIME_ORDER);
    printf(
        "Session %" PRIu32 " duplicate sample: rejected status=%d reason=TIME_ORDER\n",
        negative.session,
        rejected
    );
    CHECK(foc_cal_workflow_finish(workflow) != FOC_CAL_WORKFLOW_OK);

    ++negative.session;
    ++adc.capture;
    CHECK(foc_cal_workflow_start(workflow, &negative) == 0);
    CHECK(open_capture(workflow, &adc) == 0);
    sample = adc_observation(&negative, adc.capture, 0U);
    CHECK(foc_cal_workflow_push(workflow, &sample, 1003U) == 0);
    CHECK(foc_cal_workflow_cancel(workflow) == 0);
    bundle = foc_cal_workflow_result(workflow);
    CHECK(bundle->state == FOC_CAL_WORKFLOW_CANCELLED && !bundle->applicable && !bundle->reviewed);
    CHECK(bundle->stages[FOC_CAL_WORKFLOW_ADC].quality == FOC_CAL_WORKFLOW_STAGE_CANCELLED);
    printf("Session %" PRIu32 ": CANCELLED candidate_applicable=false\n", negative.session);
    return 0;
}

static bool inhibited(const struct foc_runtime_diagnostic *view)
{
    return view->snapshot.state == FOC_SAFE_STATE_DISABLED && view->snapshot.faults == 0U &&
        view->snapshot.action == FOC_DEVICE_OUTPUT_INHIBIT && view->snapshot.output_confirmed &&
        !view->snapshot.inhibit_pending && view->snapshot.updates == 0U;
}

static void print_value(const struct foc_parameter_value *value)
{
    if (value->type == FOC_PARAMETER_F32) {
        printf("%.7g", (double)value->data.f32);
    } else {
        printf("%" PRIu32, value->data.u32);
    }
}

static int print_diff(
    const struct foc_config_diff_input *original,
    const struct foc_config_diff_input *prepared
)
{
    struct foc_config_diff_entry entries[32];
    size_t count;
    CHECK(
        foc_config_diff(
            original,
            prepared,
            entries,
            sizeof(entries) / sizeof(entries[0]),
            &count
        ) == FOC_CONFIG_STORE_OK
    );
    CHECK(count == foc_config_diff_count());
    bool offset_changed = false;
    for (size_t i = 0U; i < count; ++i) {
        const struct foc_config_diff_entry *entry = &entries[i];
        if (entry->kind != FOC_DIAG_VALUE_CHANGED) {
            continue;
        }
        const struct foc_parameter_descriptor *parameter = entry->parameter;
        CHECK(parameter->requires_reinit || parameter->access == FOC_PARAMETER_READ_ONLY);
        CHECK(entry->left.type == parameter->type && entry->right.type == parameter->type);
        printf("  0x%04x %s [%s] ", (unsigned int)parameter->id, parameter->name, parameter->unit);
        print_value(&entry->left);
        printf(" -> ");
        print_value(&entry->right);
        printf(" REINITIALIZATION_REQUIRED\n");
        offset_changed |= parameter->id == FOC_PARAMETER_ELECTRICAL_OFFSET;
    }
    CHECK(offset_changed);
    return 0;
}

static int
verify_applied_phase(struct foc_encoder_context *encoder, const struct foc_rotor_config *rotor)
{
    const uint32_t cpr = full_profile.identity.acquisition.encoder_counts_per_revolution;
    double sum_squares = 0.0;
    double peak = 0.0;
    unsigned int samples = 0U;
    /* Codes congruent to 2 modulo 4 were absent from both LUT captures and
     * the two angle captures. Skip the first frame while the sensor warms up. */
    uint32_t time = 3001000U;
    for (uint32_t raw = 2U; raw < cpr; raw += 16U) {
        struct foc_encoder_feedback feedback;
        CHECK(
            foc_encoder_update(encoder, time, raw, FOC_ENCODER_TRANSPORT_OK, &feedback) ==
            FOC_ENCODER_STATUS_OK
        );
        time += 1000U;
        if (raw == 2U) {
            continue;
        }
        CHECK(feedback.valid && feedback.speed_valid);
        float applied_phase;
        CHECK(
            foc_rotor_phase_from_counts(
                rotor,
                feedback.mechanical_phase_count,
                cpr,
                &applied_phase
            ) == FOC_STATUS_OK
        );
        const double reference = 4.0 * fixture_position_rad(raw) + fixture_electrical_offset_rad;
        const double error = remainder(applied_phase - reference, tau);
        sum_squares += error * error;
        peak = fmax(peak, fabs(error));
        ++samples;
    }
    CHECK(samples == 63U);
    const double rms = sqrt(sum_squares / samples);
    printf(
        "HOST_SYNTHETIC applied phase holdout: samples=%u RMS_rad=%.7f peak_rad=%.7f\n",
        samples,
        rms,
        peak
    );
    CHECK(rms < 0.01 && peak < 0.02);
    return 0;
}

static int compare_and_apply(
    const struct foc_cal_application *application,
    const struct foc_cal_workflow_bundle *bundle
)
{
    struct foc_cal_application_candidate candidate;
    CHECK(foc_cal_application_prepare(application, &candidate) == FOC_CAL_APPLICATION_PREPARED);
    CHECK(candidate.has_lut);
    CHECK(candidate.store_status == FOC_CONFIG_STORE_ERR_IDENTITY);
    CHECK(candidate.pipeline.adc.phase_a_offset_count == bundle->adc.offset_a_counts);
    CHECK(candidate.pipeline.adc.phase_b_offset_count == bundle->adc.offset_b_counts);
    CHECK(candidate.pipeline.adc.phase_c_offset_count == bundle->adc.offset_c_counts);
    const struct foc_config_diff_input original = {
        .config = &application->original_view.effective,
        .status = FOC_CONFIG_STORE_OK,
    };
    const struct foc_config_diff_input prepared = {
        .config = &candidate.runtime.config,
        .status = FOC_CONFIG_STORE_OK,
    };
    CHECK(
        foc_config_application_status(&prepared, &application->original_view) ==
        FOC_DIAG_REINITIALIZATION_REQUIRED
    );
    printf(
        "CANDIDATE_ACCEPTED identity=MATCH application=REINITIALIZATION_REQUIRED "
        "store_validation=IDENTITY_MISMATCH(%d)\n",
        candidate.store_status
    );
    CHECK(print_diff(&original, &prepared) == 0);

    struct foc_runtime fresh = {0};
    struct foc_cal_application_sensor sensor = {0};
    CHECK(
        foc_cal_application_apply(application, &fresh, &sensor, 3000000U) ==
        FOC_CAL_APPLICATION_APPLIED
    );
    struct foc_runtime_diagnostic readback;
    CHECK(foc_runtime_diagnose(&fresh, &readback) == FOC_RUNTIME_OK);
    CHECK(inhibited(&readback));
    CHECK(
        readback.effective.motion.limits.enabled &&
        readback.effective.motion.following_error.enabled
    );
    CHECK(foc_config_application_status(&prepared, &readback) == FOC_DIAG_MATCHES_EFFECTIVE);
    CHECK(
        readback.effective.motion.rotor.electrical_offset_rad ==
        bundle->offset.electrical_offset_rad
    );
    uint32_t corrected_code = 0U;
    CHECK(
        foc_encoder_correct_code(&sensor.encoder, 256U, &corrected_code) == FOC_ENCODER_STATUS_OK
    );
    CHECK(corrected_code == 264U);
    printf(
        "CONFIGURATION_APPLIED new_runtime=INHIBIT updates=%" PRIu32
        " electrical_offset_rad=%.7f LUT_code=256->%" PRIu32 "\n",
        readback.snapshot.updates,
        readback.effective.motion.rotor.electrical_offset_rad,
        corrected_code
    );
    CHECK(verify_applied_phase(&sensor.encoder, &readback.effective.motion.rotor) == 0);
    puts("CONFIGURATION_PERSISTED=false");
    return 0;
}

static int run_demo(FILE *trace)
{
    const struct foc_cal_workflow_identity *identity = &full_profile.identity;
    const struct foc_cal_workflow_kt kt = {
        .identity = *identity,
        .torque_model_id = fixture_torque_model,
        .source = FOC_CAL_WORKFLOW_HOST_SYNTHETIC,
        .capture = 9U,
        .captured_us = 500U,
        .nm_per_a = fixture_kt_nm_per_a,
        .qualified = true,
    };
    if (trace != NULL) {
        CHECK(
            fputs(
                "stage,capture,sequence,source_us,completion_us,trigger_us,reference_us,clock_id,"
                "reference_clock_id,unit,channel,flags,evidence,p1,p2,p3,p4,p5,p6\n",
                trace
            ) >= 0
        );
        /* Freeze independent Kt before the observed-current J/B capture. */
        CHECK(
            fprintf(
                trace,
                "%u,%" PRIu32 ",0,%" PRIu32 ",,,,1,,%u,%" PRIu32 ",%u,%u,%.9g,,,,,\n",
                (unsigned int)FOC_CAL_WORKFLOW_TUNING,
                kt.capture,
                kt.captured_us,
                (unsigned int)TRACE_KT_UNITS,
                kt.torque_model_id,
                (unsigned int)TRACE_VALID,
                (unsigned int)TRACE_INDEPENDENT_KT,
                (double)kt.nm_per_a
            ) >= 0
        );
    }
    struct foc_virtual_current_profile profile = foc_virtual_current_default_profile();
    profile.adc.current_a_per_count = identity->acquisition.current_a_per_count;
    profile.encoder.counts_per_revolution = identity->acquisition.encoder_counts_per_revolution;
    profile.runtime.motion.impedance = (struct foc_runtime_impedance_config){
        .enabled = true,
        .mechanical_frame_id = identity->mechanical_frame_id,
        .torque_constant_nm_per_a = fixture_kt_nm_per_a,
        .torque_limit_nm = 0.33f,
        .max_stiffness_nm_per_rad = 3.0f,
        .max_damping_nm_s_per_rad = 0.3f,
        .max_feedforward_nm = 0.1f,
    };
    profile.runtime.motion.limits = (struct foc_motion_limits_config){
        .position_min_rad = -0.8f,
        .position_max_rad = 0.8f,
        .tolerance_rad = 0.005f,
        .mechanical_frame_id = identity->mechanical_frame_id,
        .enabled = true,
    };
    profile.runtime.motion.following_error = (struct foc_following_error_config){
        .threshold_rad = 0.1f,
        .duration_us = 20000U,
        .max_gap_us = 1000U,
        .enabled = true,
    };
    const struct foc_pipeline_config pipeline = {
        .current = profile.current,
        .adc = profile.adc,
        .pwm = profile.pwm,
        .safety = profile.safety,
    };
    struct foc_runtime original = {0};
    struct foc_runtime_diagnostic before;
    CHECK(foc_runtime_init(&original, &pipeline, &profile.runtime, 0U) == FOC_RUNTIME_OK);
    CHECK(foc_runtime_diagnose(&original, &before) == FOC_RUNTIME_OK && inhibited(&before));

    struct foc_cal_workflow workflow = {0};
    struct foc_cal_application application = {0};
    CHECK(foc_cal_workflow_init(&workflow) == FOC_CAL_WORKFLOW_OK);
    CHECK(
        foc_cal_application_start(
            &application,
            &workflow,
            &full_profile,
            &original,
            &pipeline,
            &profile.encoder
        ) == FOC_CAL_APPLICATION_OK
    );
    CHECK(foc_cal_workflow_result(&workflow)->state == FOC_CAL_WORKFLOW_READY);
    CHECK(foc_cal_workflow_identity_matches(
        &foc_cal_workflow_result(&workflow)->request.identity,
        identity
    ));
    printf(
        "Session %" PRIu32 " READY profile=FULL_PROFILE source=HOST_SYNTHETIC "
        "motor=%" PRIu32 " encoder=%" PRIu32 " frame=%" PRIu32 "\n",
        full_profile.session,
        identity->motor_setup_id,
        identity->encoder_id,
        identity->mechanical_frame_id
    );
    CHECK(run_adc(&workflow, trace) == 0);
    CHECK(run_electrical(&workflow, trace) == 0);
    CHECK(run_lut(&workflow, &profile.encoder, trace) == 0);
    CHECK(run_angle(&workflow, true, trace) == 0);
    CHECK(run_angle(&workflow, false, trace) == 0);
    CHECK(run_mechanical(&workflow, trace) == 0);
    CHECK(run_tuning(&workflow, &kt) == 0);
    CHECK(foc_cal_workflow_finish(&workflow) == FOC_CAL_WORKFLOW_OK);
    const struct foc_cal_workflow_bundle *bundle = foc_cal_workflow_result(&workflow);
    CHECK(bundle->complete && bundle->applicable && !bundle->reviewed);
    CHECK(bundle->state == FOC_CAL_WORKFLOW_COMPLETED && bundle->passed == FOC_CAL_WORKFLOW_ALL);
    CHECK(bundle->rotor.direction == FOC_ENCODER_DIRECTION_REVERSED);
    CHECK(bundle->snapshot.runtime_encoder_direction == FOC_ENCODER_DIRECTION_NORMAL);
    printf(
        "Encoder direction acquisition=%" PRId32 " rotor_fit=%" PRId32 " runtime=%" PRId32 "\n",
        bundle->request.identity.acquisition.encoder_direction,
        bundle->rotor.direction,
        bundle->snapshot.runtime_encoder_direction
    );
    CHECK(independent_holdout(bundle) == 0);
    puts("CALIBRATION_PASS stages=7 reviewed=false runtime_applied=false");
    struct foc_cal_workflow_identity other_identity = *identity;
    ++other_identity.encoder_id;
    CHECK(
        foc_cal_workflow_review(&workflow, full_profile.session, &other_identity, true) ==
        FOC_CAL_WORKFLOW_ERR_IDENTITY
    );
    struct foc_cal_application_candidate unreviewed;
    CHECK(foc_cal_application_prepare(&application, &unreviewed) == FOC_CAL_APPLICATION_ERR_REVIEW);
    puts("Review refused for a different encoder identity. Unreviewed application refused.");
    CHECK(foc_cal_workflow_review(&workflow, full_profile.session, identity, true) == 0);
    CHECK(compare_and_apply(&application, bundle) == 0);
    CHECK(rejected_and_cancelled(&workflow) == 0);

    struct foc_runtime_diagnostic after;
    CHECK(foc_runtime_diagnose(&original, &after) == FOC_RUNTIME_OK && inhibited(&after));
    const struct foc_config_diff_input initial = {
        .config = &before.effective,
        .status = FOC_CONFIG_STORE_OK,
    };
    CHECK(foc_config_application_status(&initial, &after) == FOC_DIAG_MATCHES_EFFECTIVE);
    CHECK(before.snapshot.motion_session == after.snapshot.motion_session);
    CHECK(
        before.snapshot.command_seq == after.snapshot.command_seq &&
        before.snapshot.command_seen == after.snapshot.command_seen
    );
    CHECK(before.snapshot.mode == after.snapshot.mode);
    printf(
        "Original Runtime preserved: config=MATCH state=DISABLED output=INHIBIT "
        "updates=%" PRIu32 " motion_session=%" PRIu32 "\n",
        after.snapshot.updates,
        after.snapshot.motion_session
    );
    return 0;
}

/* Public Functions ------------------------------------------------------------- */

int main(int argc, char *argv[])
{
    if (argc == 1) {
        return run_demo(NULL);
    }
    if (argc != 3 || strcmp(argv[1], "--export") != 0) {
        fprintf(stderr, "Usage: %s [--export samples.csv]\n", argv[0]);
        return 1;
    }
    FILE *trace = NULL;
#if defined(_MSC_VER)
    (void)fopen_s(&trace, argv[2], "wbx");
#else
    trace = fopen(argv[2], "wbx");
#endif
    if (trace == NULL) {
        perror(argv[2]);
        return 1;
    }
    const int result = run_demo(trace);
    if (fclose(trace) != 0) {
        perror(argv[2]);
        return 1;
    }
    return result;
}

/* ---------------------------------------- end of file ----------------------------------------- */
