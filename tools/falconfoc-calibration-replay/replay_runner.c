/**
 * @file    replay_runner.c
 * @brief   Replay a validated V1 sample stream into a detached Host Workflow.
 * @version v0.1.0
 * @date    2026.11.19
 *
 * Implementation role
 * -------------------
 * Decode the Host transfer, use the portable adapter and formal Workflow,
 * and report evidence. The process has no live Runtime or storage state.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_measurement_trace.h"
#include "foc_parameter.h"

#include <errno.h>
#include <float.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Private Types ---------------------------------------------------------------- */

enum {
    STAGE_COUNT = 8,
    LINE_CAPACITY = 514,
    FIELD_COUNT = 19,
    PAYLOAD_START = 13,
    MAX_ROWS = 100000,
    MAX_ANGLE_SKEW_US = 50,
};

struct replay_policy {
    struct foc_cal_workflow_request workflow;
    enum foc_measurement_trace_source source;
    uint32_t device;
    uint32_t adc_channels;
    uint32_t clock;
    uint32_t age;
    uint32_t skew;
    bool wrap;
    uint32_t reference;
    double reference_zero;
    int32_t frame_direction;
    uint32_t torque_model;
    uint32_t rows[STAGE_COUNT];
};

struct capture_report {
    uint32_t id;
    uint32_t first_us;
    uint32_t last_us;
    uint32_t seen;
    uint32_t rejected;
    uint32_t skipped;
    uint32_t first_rejected_row;
    uint32_t evidence_mask;
    uint32_t channel;
    uint32_t native_accepted;
    uint32_t native_rejected_row;
    uint32_t native_skipped;
    uint64_t pair_sum;
    uint64_t pair_xor;
    uint64_t holdout_prefix_sum;
    uint64_t holdout_prefix_xor;
    const char *reason;
    struct foc_measurement_trace_report trace;
    int workflow_status;
};

struct replay {
    struct replay_policy policy;
    struct foc_cal_workflow workflow;
    struct foc_measurement_trace trace;
    struct foc_cal_workflow_kt kt;
    struct capture_report reports[STAGE_COUNT];
    const char *reason;
    int active;
    bool validate_only;
    uint32_t rejected;
    uint32_t line_number;
};

/* Private Constants ----------------------------------------------------------- */

static const char *const stage_names[STAGE_COUNT] = {
    "adc",
    "electrical",
    "rotor",
    "offset",
    "lut",
    "mechanical",
    "tuning",
    "lut_holdout",
};
static const char *const quality_names[] = {
    "NOT_SELECTED",
    "PENDING",
    "COLLECTING",
    "PASS",
    "DATA_REQUIRED",
    "INSUFFICIENT_DATA",
    "UNIDENTIFIABLE",
    "IDENTITY_MISMATCH",
    "INVALID_OBSERVATION",
    "REJECTED_BY_QUALITY",
    "CANCELLED",
};
static const char *const failure_names[] = {
    "NONE",
    "INVALID_CONFIG",
    "PRECONDITION",
    "SAMPLE_COUNT",
    "INVALID_SAMPLE",
    "TIME_ORDER",
    "INPUT_CHANGED",
    "NOISE",
    "RANGE",
    "UNIDENTIFIABLE",
    "RESIDUAL",
    "CONFIG_MISMATCH",
    "NUMERIC",
};
static const uint32_t stage_intervals[STAGE_COUNT] = {
    1000U,
    50U,
    100000U,
    100000U,
    2000U,
    1100U,
    1U,
    2000U,
};
static const char csv_header[] =
    "stage,capture,sequence,source_us,completion_us,trigger_us,reference_us,clock_id,"
    "reference_clock_id,unit,channel,flags,evidence,p1,p2,p3,p4,p5,p6";

static const struct foc_impedance_tuning_input tuning_policy = {
    .target_natural_frequency_rad_s = 18.0f,
    .target_damping_ratio = 0.75f,
    .max_stiffness_nm_per_rad = 3.0f,
    .max_damping_nm_s_per_rad = 0.3f,
    .iq_limit_a = 2.2f,
    .sample_period_s = 0.0005f,
    .extra_delay_s = 0.0006f,
};

/* Private Helpers ------------------------------------------------------------- */

static bool parse_u32(const char *field, uint32_t *value)
{
    uint32_t out = 0U;
    if (*field == '\0') {
        return false;
    }
    for (; *field != '\0'; ++field) {
        if (*field < '0' || *field > '9') {
            return false;
        }
        uint32_t digit = (uint32_t)(*field - '0');
        if (out > (UINT32_MAX - digit) / 10U) {
            return false;
        }
        out = out * 10U + digit;
    }
    *value = out;
    return true;
}

static bool parse_real(const char *field, double *value)
{
    for (const char *p = field; *p != '\0'; ++p) {
        if ((*p < '0' || *p > '9') && strchr("+-.eE", *p) == NULL) {
            return false;
        }
    }
    char *end;
    errno = 0;
    double out = strtod(field, &end);
    if (end == field || *end != '\0' || errno == ERANGE || !isfinite(out)) {
        return false;
    }
    *value = out;
    return true;
}

static bool parse_float(const char *field, float *value)
{
    double out;
    if (!parse_real(field, &out) || fabs(out) > FLT_MAX || (out != 0.0 && (float)out == 0.0f)) {
        return false;
    }
    *value = (float)out;
    return true;
}

static bool parse_direction(const char *field, int32_t *direction)
{
    if (strcmp(field, "1") == 0) {
        *direction = 1;
    } else if (strcmp(field, "-1") == 0) {
        *direction = -1;
    } else {
        return false;
    }
    return true;
}

static bool parse_policy(int count, char **args, struct replay_policy *policy)
{
    enum {
        VERSION,
        SOURCE,
        SESSION,
        MASK,
        DEVICE,
        MOTOR,
        ENCODER,
        FRAME,
        ADC_MAX,
        GAIN,
        ADC_MAP,
        CPR,
        RAW_DIRECTION,
        PHASE,
        POLES,
        CLOCK,
        AGE,
        SKEW,
        WRAP,
        REFERENCE,
        ZERO,
        FRAME_DIRECTION,
        TORQUE_MODEL,
        FIRST_COUNT,
        POLICY_FIELDS = FIRST_COUNT + STAGE_COUNT,
    };
    uint32_t metadata[POLICY_FIELDS] = {0};
    float gain;
    int32_t raw_direction;
    if (count != POLICY_FIELDS) {
        return false;
    }
    for (int i = 0; i < POLICY_FIELDS; ++i) {
        if (i == GAIN || i == ZERO || i == RAW_DIRECTION || i == FRAME_DIRECTION) {
            continue;
        }
        if (!parse_u32(args[i], &metadata[i])) {
            return false;
        }
    }
    if (!parse_float(args[GAIN], &gain) || gain <= 0.0f ||
        !parse_real(args[ZERO], &policy->reference_zero) ||
        !parse_direction(args[RAW_DIRECTION], &raw_direction) ||
        !parse_direction(args[FRAME_DIRECTION], &policy->frame_direction) ||
        metadata[VERSION] != FOC_MEASUREMENT_TRACE_VERSION || metadata[SOURCE] < 1U ||
        metadata[SOURCE] > 4U || metadata[SESSION] == 0U || metadata[MASK] == 0U ||
        metadata[MASK] > FOC_CAL_WORKFLOW_ALL || metadata[DEVICE] == 0U || metadata[MOTOR] == 0U ||
        metadata[ENCODER] == 0U || metadata[FRAME] == 0U || metadata[ADC_MAX] == 0U ||
        metadata[ADC_MAX] > UINT16_MAX || metadata[ADC_MAP] > 4095U || metadata[CPR] < 2U ||
        metadata[CPR] > 65536U || metadata[PHASE] > 1U || metadata[POLES] == 0U ||
        metadata[POLES] > 255U || metadata[CLOCK] == 0U || metadata[AGE] >= UINT32_C(0x80000000) ||
        metadata[SKEW] > MAX_ANGLE_SKEW_US || metadata[WRAP] > 1U) {
        return false;
    }
    uint32_t a = metadata[ADC_MAP] & 15U;
    uint32_t b = (metadata[ADC_MAP] >> 4) & 15U;
    uint32_t c = metadata[ADC_MAP] >> 8;
    if (a == b || b == c || a == c) {
        return false;
    }
    uint32_t total = 0U;
    for (int i = 0; i < STAGE_COUNT; ++i) {
        uint32_t rows = metadata[FIRST_COUNT + i];
        if (rows > MAX_ROWS - total || (i == FOC_MEASUREMENT_TRACE_KT && rows > 1U)) {
            return false;
        }
        policy->rows[i] = rows;
        total += rows;
    }
    const float rail_margin = (float)metadata[ADC_MAX] * (32.0f / 4095.0f);
    policy->workflow = (struct foc_cal_workflow_request){
        .session = metadata[SESSION],
        .profile = metadata[MASK] == FOC_CAL_WORKFLOW_ALL ? FOC_CAL_WORKFLOW_FULL_PROFILE
                                                          : FOC_CAL_WORKFLOW_SELECTED_STAGES,
        .stages = metadata[MASK],
        .identity =
            {
                .acquisition =
                    {
                        .adc_full_scale_counts = (uint16_t)metadata[ADC_MAX],
                        .current_a_per_count = gain,
                        .encoder_counts_per_revolution = metadata[CPR],
                        .encoder_direction = raw_direction,
                        .phase_order = (uint8_t)metadata[PHASE],
                        .pole_pairs = (uint16_t)metadata[POLES],
                    },
                .motor_setup_id = metadata[MOTOR],
                .encoder_id = metadata[ENCODER],
                .mechanical_frame_id = metadata[FRAME],
            },
        .admission = {
            .minimum_adc_samples = 32U,
            .minimum_offset_counts = rail_margin,
            .maximum_offset_counts = (float)metadata[ADC_MAX] - rail_margin,
            .maximum_noise_rms_counts = 2.0f,
            .maximum_rotor_residual_rad = 0.02f,
            .maximum_offset_residual_rad = 0.02f,
        },
    };
    policy->source = (enum foc_measurement_trace_source)metadata[SOURCE];
    policy->device = metadata[DEVICE];
    policy->adc_channels = metadata[ADC_MAP];
    policy->clock = metadata[CLOCK];
    policy->age = metadata[AGE];
    policy->skew = metadata[SKEW];
    policy->wrap = metadata[WRAP] != 0U;
    policy->reference = metadata[REFERENCE];
    policy->torque_model = metadata[TORQUE_MODEL];
    return true;
}

static int read_line(char line[LINE_CAPACITY])
{
    size_t length = 0U;
    int ch;
    while ((ch = getchar()) != EOF && ch != '\n') {
        if (ch < 0x20 && ch != '\r') {
            return -1;
        }
        if (length + 1U >= LINE_CAPACITY) {
            return -1;
        }
        line[length++] = (char)ch;
    }
    if (ch == EOF && (length == 0U || ferror(stdin))) {
        return ferror(stdin) ? -1 : 0;
    }
    if (length != 0U && line[length - 1U] == '\r') {
        --length;
    }
    if (length > LINE_CAPACITY - 2U) {
        return -1;
    }
    line[length] = '\0';
    return 1;
}

static bool parse_fields(char *line, const char *fields[FIELD_COUNT])
{
    for (size_t i = 0; i < FIELD_COUNT; ++i) {
        fields[i] = line;
        char *separator = strchr(line, ',');
        if (i == FIELD_COUNT - 1U) {
            return separator == NULL;
        }
        if (separator == NULL) {
            return false;
        }
        *separator = '\0';
        line = separator + 1;
    }
    return false;
}

static bool parse_record(
    const char *const fields[FIELD_COUNT],
    struct foc_measurement_trace_record *record,
    uint32_t *capture
)
{
    uint32_t stage, evidence;
    if (!parse_u32(fields[0], &stage) || stage >= STAGE_COUNT || !parse_u32(fields[1], capture) ||
        *capture == 0U || !parse_u32(fields[2], &record->sequence) ||
        !parse_u32(fields[3], &record->source_us) || !parse_u32(fields[7], &record->clock_id) ||
        !parse_u32(fields[9], &record->units) || !parse_u32(fields[10], &record->channel) ||
        !parse_u32(fields[11], &record->flags) || !parse_u32(fields[12], &evidence) ||
        evidence > FOC_MEASUREMENT_TRACE_DERIVED) {
        return false;
    }
    record->stage = (enum foc_measurement_trace_stage)stage;
    record->evidence = (enum foc_measurement_trace_evidence)evidence;
    record->completion_observed = *fields[4] != '\0';
    record->trigger_observed = *fields[5] != '\0';
    bool paired = stage != FOC_MEASUREMENT_TRACE_ADC && stage != FOC_MEASUREMENT_TRACE_KT;
    return (!record->completion_observed || parse_u32(fields[4], &record->completion_us)) &&
        (!record->trigger_observed || parse_u32(fields[5], &record->trigger_us)) &&
        (paired ? parse_u32(fields[6], &record->reference_us) &&
                 parse_u32(fields[8], &record->reference_clock_id)
                : *fields[6] == '\0' && *fields[8] == '\0');
}

static enum foc_mechanical_id_torque_source torque_source(
    const struct foc_measurement_trace_record *record
)
{
    if (record->evidence == FOC_MEASUREMENT_TRACE_OBSERVED_DQ) {
        return FOC_MECHANICAL_ID_TORQUE_OBSERVED_DQ;
    }
    if (record->evidence == FOC_MEASUREMENT_TRACE_SHAFT_MEASURED) {
        return FOC_MECHANICAL_ID_TORQUE_EQUIVALENT;
    }
    return FOC_MECHANICAL_ID_TORQUE_UNVERIFIED;
}

static enum foc_mechanical_id_load_source load_source(
    const struct foc_measurement_trace_record *record
)
{
    if ((record->flags & FOC_MEASUREMENT_TRACE_LOAD_OBSERVED) != 0U) {
        return FOC_MECHANICAL_ID_LOAD_OBSERVED;
    }
    if ((record->flags & FOC_MEASUREMENT_TRACE_LOAD_NEGLIGIBLE) != 0U) {
        return FOC_MECHANICAL_ID_LOAD_NEGLIGIBLE;
    }
    return FOC_MECHANICAL_ID_LOAD_UNKNOWN;
}

static enum foc_motor_id_voltage_source voltage_source(
    const struct foc_measurement_trace_record *record
)
{
    if (record->evidence == FOC_MEASUREMENT_TRACE_TERMINAL_VIRTUAL) {
        return FOC_MOTOR_ID_VOLTAGE_VIRTUAL_TERMINAL;
    }
    if (record->evidence == FOC_MEASUREMENT_TRACE_TERMINAL_MEASURED) {
        return FOC_MOTOR_ID_VOLTAGE_MEASURED_TERMINAL;
    }
    return FOC_MOTOR_ID_VOLTAGE_UNVERIFIED;
}

static bool host_source(const struct replay_policy *policy)
{
    return policy->source == FOC_MEASUREMENT_TRACE_HOST_SYNTHETIC ||
        policy->source == FOC_MEASUREMENT_TRACE_HOST_VIRTUAL;
}

static bool populate_payload(
    struct foc_measurement_trace_record *record,
    const char *const p[6],
    uint32_t capture,
    const struct replay_policy *policy
)
{
    const struct foc_cal_workflow_identity identity = policy->workflow.identity;
    struct foc_cal_workflow_sample *s = &record->payload.sample;
    uint32_t a, b, c;
    switch (record->stage) {
    case FOC_MEASUREMENT_TRACE_KT:
        record->payload.kt = (struct foc_cal_workflow_kt){
            .identity = identity,
            .torque_model_id = policy->torque_model,
            .capture = capture,
        };
        return parse_float(p[0], &record->payload.kt.nm_per_a) && !*p[1] && !*p[2] && !*p[3] &&
            !*p[4] && !*p[5];
    default:
        break;
    }
    s->session = policy->workflow.session;
    s->capture = capture;
    s->identity = identity;
    s->units = (enum foc_cal_workflow_units)record->units;
    switch (record->stage) {
    case FOC_MEASUREMENT_TRACE_ADC:
        if (!parse_u32(p[0], &a) || !parse_u32(p[1], &b) || !parse_u32(p[2], &c) ||
            a > UINT16_MAX || b > UINT16_MAX || c > UINT16_MAX || *p[3] || *p[4] || *p[5]) {
            return false;
        }
        s->data.adc = (struct foc_calibration_adc_sample){
            .phase_a = (uint16_t)a,
            .phase_b = (uint16_t)b,
            .phase_c = (uint16_t)c,
            .valid_mask = 7U,
        };
        return true;
    case FOC_MEASUREMENT_TRACE_ELECTRICAL:
        s->data.electrical = (struct foc_motor_id_sample){
            .setup_id = identity.motor_setup_id,
            .capture_id = capture,
            .source = host_source(policy) ? FOC_MOTOR_ID_VIRTUAL : FOC_MOTOR_ID_MEASURED,
            .voltage_source = voltage_source(record),
            .voltage_end_us = record->reference_us,
        };
        return parse_float(p[0], &s->data.electrical.current_a) &&
            parse_float(p[1], &s->data.electrical.mean_voltage_v) &&
            parse_u32(p[2], &s->data.electrical.voltage_begin_us) &&
            parse_float(p[3], &s->data.electrical.omega_e_rad_s) &&
            (*p[4] != '\0' ? parse_u32(p[4], &record->frame_encoder_us) &&
                     parse_u32(p[5], &record->frame_encoder_clock_id) &&
                     record->frame_encoder_clock_id != 0U
                           : *p[5] == '\0');
    case FOC_MEASUREMENT_TRACE_ROTOR:
    case FOC_MEASUREMENT_TRACE_OFFSET:
        s->encoder_raw_valid = true;
        return parse_u32(p[0], &s->encoder_raw_code) &&
            parse_float(p[1], &s->data.angle.mechanical_rad) &&
            parse_float(p[2], &s->data.angle.electrical_rad) && !*p[3] && !*p[4] && !*p[5];
    case FOC_MEASUREMENT_TRACE_LUT:
    case FOC_MEASUREMENT_TRACE_LUT_HOLDOUT:
        s->data.lut = (struct foc_encoder_cal_sample){
            .setup =
                {
                    .encoder_identity = identity.encoder_id,
                    .reference_identity = policy->reference,
                    .counts_per_revolution = identity.acquisition.encoder_counts_per_revolution,
                    .direction = policy->frame_direction,
                    .source = host_source(policy) ? FOC_ENCODER_CAL_SOURCE_VIRTUAL
                                                  : FOC_ENCODER_CAL_SOURCE_EXTERNAL,
                },
            .capture_id = capture,
        };
        return parse_u32(p[0], &s->data.lut.raw_code) &&
            parse_real(p[1], &s->data.lut.reference_rad) && !*p[2] && !*p[3] && !*p[4] && !*p[5];
    case FOC_MEASUREMENT_TRACE_MECHANICAL:
        s->data.mechanical = (struct foc_mechanical_id_sample){
            .setup_id = identity.motor_setup_id,
            .torque_model_id = policy->torque_model,
            .capture_id = capture,
            .torque_source = torque_source(record),
            .load_source = load_source(record),
        };
        return parse_float(p[0], &s->data.mechanical.speed_rad_s) &&
            parse_float(p[1], &s->data.mechanical.iq_a) &&
            parse_float(p[2], &s->data.mechanical.id_a) &&
            parse_float(p[3], &s->data.mechanical.torque_nm) &&
            parse_float(p[4], &s->data.mechanical.load_nm) &&
            parse_u32(p[5], &s->data.mechanical.interval_begin_us);
    default:
        return false;
    }
}

static uint32_t expected_channel(const struct replay_policy *policy, int stage, uint32_t channel)
{
    switch (stage) {
    case FOC_MEASUREMENT_TRACE_ADC:
        return policy->adc_channels;
    case FOC_MEASUREMENT_TRACE_ELECTRICAL:
        return channel == 2U ? 2U : 1U;
    case FOC_MEASUREMENT_TRACE_KT:
    case FOC_MEASUREMENT_TRACE_MECHANICAL:
        return policy->torque_model;
    default:
        return policy->workflow.identity.encoder_id;
    }
}

static struct foc_cal_workflow_stage_request stage_policy(
    const struct replay *replay,
    const struct foc_measurement_trace_record *record,
    uint32_t capture
)
{
    const struct replay_policy *policy = &replay->policy;
    const struct foc_calibration_identity identity = policy->workflow.identity.acquisition;
    struct foc_cal_workflow_stage_request stage = {
        .stage = (enum foc_cal_workflow_stage)record->stage,
        .source = foc_measurement_trace_workflow_source(policy->source),
        .capture = capture,
        .max_age_us = policy->age,
    };
    switch (record->stage) {
    case FOC_MEASUREMENT_TRACE_ADC:
        stage.policy.adc = (struct foc_calibration_adc_config){
            .identity = identity,
            .required_valid_mask = 7U,
            .minimum_samples = 32U,
            .maximum_samples = 65535U,
            .maximum_sample_step_counts = 20.0f,
            .maximum_noise_rms_counts = 2.0f,
            .minimum_offset_counts = policy->workflow.admission.minimum_offset_counts,
            .maximum_offset_counts = policy->workflow.admission.maximum_offset_counts,
            .maximum_interval_us = stage_intervals[record->stage],
        };
        break;
    case FOC_MEASUREMENT_TRACE_ELECTRICAL:
        stage.policy.electrical = (struct foc_motor_id_config){
            .axis = record->channel == 2U ? FOC_MOTOR_ID_Q : FOC_MOTOR_ID_D,
            .source = record->payload.sample.data.electrical.source,
            .voltage_source = host_source(policy) ? FOC_MOTOR_ID_VOLTAGE_VIRTUAL_TERMINAL
                                                  : FOC_MOTOR_ID_VOLTAGE_MEASURED_TERMINAL,
            .setup_id = policy->workflow.identity.motor_setup_id,
            .min_interval_us = 25U,
            .max_interval_us = stage_intervals[record->stage],
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
        break;
    case FOC_MEASUREMENT_TRACE_ROTOR:
        stage.policy.rotor = (struct foc_calibration_rotor_config){
            .identity = identity,
            .minimum_pole_pairs = 1U,
            .maximum_pole_pairs = 20U,
            .minimum_mechanical_span_rad = 8.0f,
            .maximum_residual_rad = 0.02f,
            .minimum_candidate_separation_rad = 0.05f,
            .maximum_interval_us = stage_intervals[record->stage],
            .maximum_samples = 64U,
        };
        break;
    case FOC_MEASUREMENT_TRACE_OFFSET:
        stage.policy.offset = (struct foc_calibration_offset_config){
            .maximum_residual_rad = 0.02f,
            .maximum_interval_us = stage_intervals[record->stage],
            .maximum_samples = 64U,
        };
        break;
    case FOC_MEASUREMENT_TRACE_LUT:
        stage.policy.lut = (struct foc_encoder_cal_config){
            .encoder =
                {
                    .counts_per_revolution = identity.encoder_counts_per_revolution,
                    .direction = policy->frame_direction,
                    .min_dt_us = 1U,
                    .max_dt_us = stage_intervals[record->stage],
                    .max_speed_rad_s = 1000.0f,
                    .speed_lpf_cutoff_hz = 80.0f,
                    .tracker_alpha = 0.85f,
                    .tracker_beta = 0.2f,
                },
            .encoder_identity = policy->workflow.identity.encoder_id,
            .reference_identity = policy->reference,
            .source = record->payload.sample.data.lut.setup.source,
            .nodes = 8U,
            .min_samples_per_segment = 8U,
            .reference_zero_rad = policy->reference_zero,
            .max_zero_bias_rad = 0.004,
            .max_training_segment_rms_rad = 0.004,
            .max_holdout_rms_rad = 0.004,
            .max_holdout_peak_rad = 0.006,
        };
        break;
    case FOC_MEASUREMENT_TRACE_MECHANICAL:
        stage.policy.mechanical = (struct foc_mechanical_id_config){
            .setup_id = policy->workflow.identity.motor_setup_id,
            .torque_model_id = policy->torque_model,
            .torque_source = torque_source(record),
            .load_source = load_source(record),
            .min_interval_us = 900U,
            .max_interval_us = stage_intervals[record->stage],
            .intervals_per_window = 100U,
            .min_windows = 6U,
            .torque_constant_nm_per_a = replay->kt.qualified ? replay->kt.nm_per_a : 0.0f,
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
        break;
    default:
        break;
    }
    return stage;
}

static void reject_row(struct replay *replay, struct capture_report *report, const char *reason)
{
    if (replay->reason == NULL) {
        replay->reason = reason;
    }
    if (report != NULL && report->reason == NULL) {
        report->reason = reason;
        report->first_rejected_row = replay->line_number;
    }
    if (report != NULL) {
        ++report->rejected;
    }
    ++replay->rejected;
}

static void check_missing(struct replay *replay, int index)
{
    enum foc_cal_workflow_stage stage = (enum foc_cal_workflow_stage)index;
    if (index == FOC_MEASUREMENT_TRACE_LUT_HOLDOUT) {
        stage = FOC_CAL_WORKFLOW_LUT;
    }
    if (!replay->validate_only) {
        (void)foc_cal_workflow_data_required(&replay->workflow, stage);
    }
}

/* Detect a replay of the whole training payload under a different capture ID.
 * This diagnostic does not authenticate the acquisition. Unchanged calibration
 * points may occur in two independently observed captures. */
static void record_angle_pair(
    struct capture_report *report,
    const struct foc_encoder_cal_sample *sample,
    uint32_t holdout_rows
)
{
    const double rad = sample->reference_rad == 0.0 ? 0.0 : sample->reference_rad;
    unsigned char bytes[sizeof(rad)];
    memcpy(bytes, &rad, sizeof(bytes));
    uint64_t pair = UINT64_C(14695981039346656037) ^ sample->raw_code;
    for (size_t i = 0; i < sizeof(bytes); ++i) {
        pair = (pair ^ bytes[i]) * UINT64_C(1099511628211);
    }
    report->pair_sum += pair;
    report->pair_xor ^= pair;
    if (report->trace.accepted <= holdout_rows) {
        report->holdout_prefix_sum += pair;
        report->holdout_prefix_xor ^= pair;
    }
}

static void close_capture(struct replay *replay, bool entering_holdout)
{
    if (replay->active < 0) {
        return;
    }
    const int index = replay->active;
    struct capture_report *report = &replay->reports[index];
    enum foc_measurement_trace_status status = foc_measurement_trace_finish(&replay->trace);
    report->trace = replay->trace.report;
    if (report->reason == NULL && status != FOC_MEASUREMENT_TRACE_OK) {
        report->reason = foc_measurement_trace_status_name(status);
        if (replay->reason == NULL) {
            replay->reason = report->reason;
        }
    }
    if (report->reason == NULL && index == FOC_MEASUREMENT_TRACE_LUT_HOLDOUT) {
        const struct capture_report *train = &replay->reports[FOC_MEASUREMENT_TRACE_LUT];
        if (report->trace.accepted <= train->trace.accepted &&
            report->pair_sum == train->holdout_prefix_sum &&
            report->pair_xor == train->holdout_prefix_xor) {
            report->reason = "HOLDOUT_REUSED_PAYLOAD";
            if (replay->reason == NULL) {
                replay->reason = report->reason;
            }
        }
    }
    if (report->reason != NULL) {
        check_missing(replay, index);
    } else if (
        !replay->validate_only && report->workflow_status == FOC_CAL_WORKFLOW_OK &&
        index != FOC_MEASUREMENT_TRACE_KT
    ) {
        if (index == FOC_MEASUREMENT_TRACE_LUT && !entering_holdout) {
            check_missing(replay, index);
        } else if (index != FOC_MEASUREMENT_TRACE_LUT) {
            report->workflow_status = foc_cal_workflow_finish_stage(&replay->workflow);
            if (index == FOC_MEASUREMENT_TRACE_LUT_HOLDOUT) {
                replay->reports[FOC_MEASUREMENT_TRACE_LUT].workflow_status =
                    report->workflow_status;
            }
        }
    }
    replay->active = -1;
}

static bool selected(const struct replay_policy *policy, int index)
{
    if (index == FOC_MEASUREMENT_TRACE_LUT_HOLDOUT) {
        index = FOC_MEASUREMENT_TRACE_LUT;
    }
    return (policy->workflow.stages & FOC_CAL_WORKFLOW_STAGE_BIT(index)) != 0U;
}

static bool legal_capture(const struct replay *replay, int index, uint32_t id)
{
    if (replay->reports[index].seen != 0U || replay->policy.rows[index] == 0U ||
        (!selected(&replay->policy, index) &&
         !(index == FOC_MEASUREMENT_TRACE_KT &&
           selected(&replay->policy, FOC_MEASUREMENT_TRACE_MECHANICAL)))) {
        return false;
    }
    for (int i = 0; i < STAGE_COUNT; ++i) {
        if (replay->reports[i].id == id) {
            return false;
        }
    }
    return true;
}

static const char *init_capture(
    struct replay *replay,
    const struct foc_measurement_trace_record *record,
    uint32_t capture
)
{
    int index = (int)record->stage;
    struct capture_report *report = &replay->reports[index];
    if (!legal_capture(replay, index, capture)) {
        return "CAPTURE_ORDER";
    }
    if (index == FOC_MEASUREMENT_TRACE_LUT_HOLDOUT) {
        const struct capture_report *train = &replay->reports[FOC_MEASUREMENT_TRACE_LUT];
        uint32_t elapsed = record->source_us - train->last_us;
        if (!train->trace.complete || elapsed == 0U || elapsed >= UINT32_C(0x80000000) ||
            (!replay->policy.wrap && record->source_us < train->last_us)) {
            return "HOLDOUT_NOT_INDEPENDENT";
        }
    }
    if ((index == FOC_MEASUREMENT_TRACE_ROTOR || index == FOC_MEASUREMENT_TRACE_OFFSET) &&
        selected(&replay->policy, FOC_MEASUREMENT_TRACE_LUT) &&
        !replay->reports[FOC_MEASUREMENT_TRACE_LUT_HOLDOUT].trace.complete) {
        return "HOLDOUT_REQUIRED";
    }
    if (index == FOC_MEASUREMENT_TRACE_OFFSET &&
        !replay->reports[FOC_MEASUREMENT_TRACE_ROTOR].trace.complete) {
        return "ROTOR_REQUIRED";
    }

    const struct replay_policy *policy = &replay->policy;
    struct foc_measurement_trace_contract contract = {
        .version = FOC_MEASUREMENT_TRACE_VERSION,
        .source = policy->source,
        .provenance = host_source(policy) ? FOC_MEASUREMENT_TRACE_PROVENANCE_DECLARED
                                          : FOC_MEASUREMENT_TRACE_PROVENANCE_UNVERIFIED,
        .stage = record->stage,
        .session = policy->workflow.session,
        .capture = capture,
        .clock_id = policy->clock,
        .channel = expected_channel(policy, index, record->channel),
        .first_sequence = record->sequence,
        .expected_samples = policy->rows[index],
        .max_age_us = policy->age,
        .max_interval_us = stage_intervals[index],
        .max_skew_us = policy->skew,
        .lut_reference_id = policy->reference,
        .lut_direction = policy->frame_direction,
        .allow_wrap = policy->wrap,
        .identity = policy->workflow.identity,
    };
    enum foc_measurement_trace_status status =
        foc_measurement_trace_init(&replay->trace, &contract);
    if (status != FOC_MEASUREMENT_TRACE_OK) {
        return foc_measurement_trace_status_name(status);
    }
    report->id = capture;
    report->channel = record->channel;
    replay->active = index;
    return NULL;
}

static void submit(
    struct replay *replay,
    struct capture_report *report,
    const struct foc_measurement_trace_record *record,
    const union foc_measurement_trace_payload *payload,
    uint32_t now
)
{
    if (record->stage == FOC_MEASUREMENT_TRACE_KT) {
        replay->kt = payload->kt;
        return;
    }
    if (replay->validate_only) {
        return;
    }
    if (report->trace.accepted == 1U) {
        if (record->stage == FOC_MEASUREMENT_TRACE_LUT_HOLDOUT) {
            report->workflow_status = foc_cal_workflow_begin_holdout(&replay->workflow, report->id);
        } else {
            const struct foc_cal_workflow_stage_request policy =
                stage_policy(replay, record, report->id);
            report->workflow_status = foc_cal_workflow_open(&replay->workflow, &policy);
        }
    }
    if (report->workflow_status == FOC_CAL_WORKFLOW_OK) {
        report->workflow_status = foc_cal_workflow_push(&replay->workflow, &payload->sample, now);
        if (report->workflow_status == FOC_CAL_WORKFLOW_OK) {
            ++report->native_accepted;
        } else {
            report->native_rejected_row = replay->line_number;
        }
    } else {
        ++report->native_skipped;
    }
}

static const char *
check_evidence(const struct replay *replay, const struct foc_measurement_trace_record *record)
{
    switch (record->stage) {
    case FOC_MEASUREMENT_TRACE_ROTOR:
    case FOC_MEASUREMENT_TRACE_OFFSET:
    case FOC_MEASUREMENT_TRACE_LUT:
    case FOC_MEASUREMENT_TRACE_LUT_HOLDOUT:
        return replay->policy.reference == 0U ||
                replay->policy.reference == replay->policy.workflow.identity.encoder_id
            ? "REFERENCE_REQUIRED"
            : NULL;
    case FOC_MEASUREMENT_TRACE_MECHANICAL:
        if (replay->policy.torque_model == 0U) {
            return "TORQUE_SOURCE_REQUIRED";
        }
        if (record->evidence == FOC_MEASUREMENT_TRACE_OBSERVED_DQ && !replay->kt.qualified) {
            return "KT_REQUIRED_FOR_DQ";
        }
        return NULL;
    default:
        return NULL;
    }
}

static void receive(struct replay *replay, char *line)
{
    const char *fields[FIELD_COUNT];
    struct foc_measurement_trace_record record = {0};
    uint32_t capture;
    if (!parse_fields(line, fields) || !parse_record(fields, &record, &capture)) {
        reject_row(replay, NULL, "INVALID_RECORD");
        return;
    }
    int index = (int)record.stage;
    struct capture_report *report = &replay->reports[index];
    if (replay->active != index) {
        const int previous = replay->active;
        close_capture(replay, index == FOC_MEASUREMENT_TRACE_LUT_HOLDOUT);
        const char *reason = index == FOC_MEASUREMENT_TRACE_LUT_HOLDOUT &&
                previous != FOC_MEASUREMENT_TRACE_LUT && report->seen == 0U
            ? "HOLDOUT_NOT_INDEPENDENT"
            : init_capture(replay, &record, capture);
        if (reason != NULL) {
            ++report->seen;
            reject_row(replay, report, reason);
            if (index == FOC_MEASUREMENT_TRACE_LUT_HOLDOUT) {
                check_missing(replay, index);
            }
            return;
        }
    }
    ++report->seen;
    report->evidence_mask |= UINT32_C(1) << record.evidence;
    if (report->reason != NULL) {
        ++report->skipped;
        return;
    }
    if (capture != report->id ||
        !populate_payload(&record, fields + PAYLOAD_START, capture, &replay->policy)) {
        reject_row(replay, report, capture != report->id ? "CAPTURE_ORDER" : "INVALID_PAYLOAD");
        return;
    }
    const char *reason = check_evidence(replay, &record);
    if (reason != NULL) {
        reject_row(replay, report, reason);
        return;
    }
    union foc_measurement_trace_payload payload;
    uint32_t now;
    enum foc_measurement_trace_status status =
        foc_measurement_trace_convert(&replay->trace, &record, &payload, &now);
    report->trace = replay->trace.report;
    if (status != FOC_MEASUREMENT_TRACE_OK) {
        reject_row(replay, report, foc_measurement_trace_status_name(status));
        return;
    }
    if (report->trace.accepted == 1U) {
        report->first_us = record.source_us;
    }
    report->last_us = record.source_us;
    if (index == FOC_MEASUREMENT_TRACE_LUT || index == FOC_MEASUREMENT_TRACE_LUT_HOLDOUT) {
        record_angle_pair(
            report,
            &payload.sample.data.lut,
            replay->policy.rows[FOC_MEASUREMENT_TRACE_LUT_HOLDOUT]
        );
    }
    submit(replay, report, &record, &payload, now);
}

static void complete_replay(struct replay *replay)
{
    close_capture(replay, false);
    for (int i = 0; i < STAGE_COUNT; ++i) {
        struct capture_report *report = &replay->reports[i];
        if (report->reason != NULL) {
            check_missing(replay, i);
        } else if (report->seen != replay->policy.rows[i]) {
            report->reason = "INCOMPLETE";
        } else if (selected(&replay->policy, i) && report->seen == 0U) {
            report->reason = "DATA_REQUIRED";
        }
        if (report->reason != NULL && replay->reason == NULL) {
            replay->reason = report->reason;
        }
    }
    if (!replay->validate_only) {
        if (selected(&replay->policy, FOC_MEASUREMENT_TRACE_KT)) {
            struct capture_report *kt = &replay->reports[FOC_MEASUREMENT_TRACE_KT];
            kt->workflow_status = foc_cal_workflow_tune(
                &replay->workflow,
                kt->trace.complete && kt->reason == NULL ? &replay->kt : NULL,
                &tuning_policy
            );
        }
        (void)foc_cal_workflow_finish(&replay->workflow);
    }
}

static const char *state_name(const struct replay *replay)
{
    if (replay->validate_only) {
        return "NOT_RUN";
    }
    if (replay->workflow.result.state == FOC_CAL_WORKFLOW_COMPLETED && replay->reason == NULL) {
        return "COMPLETED";
    }
    if (replay->workflow.result.complete) {
        return "REJECTED";
    }
    return replay->workflow.result.passed == 0U ? "REJECTED" : "PARTIAL";
}

static const char *json_bool(bool value)
{
    return value ? "true" : "false";
}

static void print_stage(const struct replay *replay, int index)
{
    const struct capture_report *report = &replay->reports[index];
    enum foc_cal_workflow_stage stage = index == FOC_MEASUREMENT_TRACE_LUT_HOLDOUT
        ? FOC_CAL_WORKFLOW_LUT
        : (enum foc_cal_workflow_stage)index;
    const struct foc_cal_workflow_stage_result *result = &replay->workflow.result.stages[stage];
    printf(
        "%s\"%s\":{\"capture\":%" PRIu32 ",\"seen\":%" PRIu32 ",\"valid_samples\":%" PRIu32
        ",\"rejected_samples\":%" PRIu32 ",\"skipped_samples\":%" PRIu32
        ",\"first_rejected_row\":%" PRIu32 ",\"channel\":%" PRIu32 ",\"evidence_mask\":%" PRIu32
        ",\"workflow_accepted_samples\":%" PRIu32 ",\"workflow_rejected_row\":%" PRIu32
        ",\"workflow_skipped_samples\":%" PRIu32 ",\"first_us\":%" PRIu32 ",\"last_us\":%" PRIu32
        ",\"completion_unobserved\":%" PRIu32 ",\"trigger_observed\":%" PRIu32
        ",\"source_proxy\":%" PRIu32
        ",\"trace_complete\":%s,\"reason\":\"%s\",\"quality\":\"%s\",\"workflow_status\":%d"
        ",\"algorithm_status\":%d,\"workflow_failure\":\"%s\"}",
        index == 0 ? "" : ",",
        stage_names[index],
        report->id,
        report->seen,
        report->trace.accepted,
        report->rejected,
        report->skipped,
        report->first_rejected_row,
        report->channel,
        report->evidence_mask,
        report->native_accepted,
        report->native_rejected_row,
        report->native_skipped,
        report->first_us,
        report->last_us,
        report->trace.completion_unobserved,
        report->trace.trigger_observed,
        report->trace.source_proxy,
        json_bool(report->trace.complete && report->reason == NULL),
        report->reason != NULL ? report->reason : "NONE",
        replay->validate_only ? "NOT_RUN" : quality_names[result->quality],
        report->workflow_status != 0 ? report->workflow_status : result->status,
        result->algorithm_status,
        failure_names[result->failure]
    );
}

static void print_metrics(const struct foc_cal_workflow_bundle *result)
{
    printf(
        "\"metrics\":{\"adc\":{\"offset_counts\":[%.9g,%.9g,%.9g],\"noise_rms_counts\":[%.9g,%.9g,%"
        ".9g]},"
        "\"electrical\":{\"rs_ohm\":%.9g,\"inductance_h\":%.9g,\"residual_rms_v\":%.17g,"
        "\"excitation\":%.17g},"
        "\"rotor\":{\"direction\":%" PRId32 ",\"pole_pairs\":%u,\"residual_rms_rad\":%.9g,"
        "\"candidate_separation_rad\":%.9g},"
        "\"offset\":{\"electrical_offset_rad\":%.9g,\"residual_rms_rad\":%.9g},"
        "\"lut\":{\"nodes\":%" PRIu32 ",\"training_rms_rad\":%.17g,\"holdout_raw_rms_rad\":%.17g,"
        "\"holdout_rms_rad\":%.17g,\"holdout_peak_rad\":%.17g},"
        "\"mechanical\":{\"inertia_kg_m2\":%.9g,\"damping_nm_s\":%.9g,\"residual_rms_nm\":%.17g,"
        "\"excitation\":%.17g,\"windows\":%" PRIu32 "},"
        "\"tuning\":{\"kt_nm_per_a\":%.9g,\"stiffness_nm_per_rad\":%.9g,\"damping_nm_s_per_rad\":%."
        "9g,"
        "\"ideal_damping_ratio\":%.9g}}",
        result->adc.offset_a_counts,
        result->adc.offset_b_counts,
        result->adc.offset_c_counts,
        result->adc.noise_rms_a_counts,
        result->adc.noise_rms_b_counts,
        result->adc.noise_rms_c_counts,
        result->electrical.rs_ohm,
        result->electrical.inductance_h,
        result->electrical.residual_rms_v,
        result->electrical.excitation,
        result->rotor.direction,
        (unsigned int)result->rotor.pole_pairs,
        result->rotor.residual_rms_rad,
        result->rotor.candidate_separation_rad,
        result->offset.electrical_offset_rad,
        result->offset.residual_rms_rad,
        result->lut.nodes,
        result->lut.training_rms_rad,
        result->lut.holdout_raw_rms_rad,
        result->lut.holdout_rms_rad,
        result->lut.holdout_peak_rad,
        result->mechanical.inertia_kg_m2,
        result->mechanical.damping_nm_s,
        result->mechanical.residual_rms_nm,
        result->mechanical.excitation,
        result->mechanical.windows,
        result->kt.nm_per_a,
        result->tuning.stiffness_nm_per_rad,
        result->tuning.damping_nm_s_per_rad,
        result->tuning.ideal_damping_ratio
    );
}

static void print_candidate(const struct foc_cal_workflow_bundle *result, bool available)
{
    printf(",\"candidate\":");
    if (!available) {
        printf("null");
        return;
    }
    const struct foc_calibration_snapshot *snapshot = &result->snapshot;
    const enum foc_parameter_id ids[] = {
        FOC_PARAMETER_POLE_PAIRS,
        FOC_PARAMETER_ELECTRICAL_OFFSET,
        FOC_PARAMETER_KT,
    };
    const double values[] = {
        snapshot->pole_pairs,
        snapshot->electrical_offset_rad,
        result->kt.nm_per_a,
    };
    printf("{\"reviewed\":false,\"runtime_readback\":null,\"parameters\":[");
    const size_t n = result->kt.qualified ? 3U : 2U;
    for (size_t i = 0; i < n; ++i) {
        const struct foc_parameter_descriptor *parameter = foc_parameter_find(ids[i]);
        printf(
            "%s{\"id\":%u,\"name\":\"%s\",\"value\":%.9g}",
            i == 0U ? "" : ",",
            (unsigned int)ids[i],
            parameter->name,
            values[i]
        );
    }
    printf(
        "],\"adc_offsets_counts\":[%.9g,%.9g,%.9g],\"runtime_encoder_direction\":%" PRId32
        ",\"lut_offsets_counts\":[",
        snapshot->phase_a_offset_count,
        snapshot->phase_b_offset_count,
        snapshot->phase_c_offset_count,
        snapshot->runtime_encoder_direction
    );
    for (uint32_t i = 0U; i < result->lut.nodes; ++i) {
        printf("%s%d", i == 0U ? "" : ",", (int)result->lut.offsets[i]);
    }
    printf("]}");
}

static void print_report(const struct replay *replay)
{
    const struct foc_cal_workflow_bundle *result = &replay->workflow.result;
    const bool candidate = !replay->validate_only && replay->reason == NULL && result->applicable;
    const char *reason = replay->reason;
    if (reason == NULL && !replay->validate_only && !result->complete) {
        reason = "WORKFLOW_QUALITY_OR_DEPENDENCY";
    } else if (
        reason == NULL && !replay->validate_only && result->complete && !result->applicable &&
        (result->request.stages & FOC_CAL_WORKFLOW_ADMISSION_MASK) ==
            FOC_CAL_WORKFLOW_ADMISSION_MASK
    ) {
        reason = "CANDIDATE_ADMISSION_REJECTED";
    }
    printf(
        "{\"trace_valid\":%s,\"calibration_state\":\"%s\",\"candidate_available\":%s,"
        "\"review_required\":%s,\"reason\":\"%s\",\"rejected_samples\":%" PRIu32
        ",\"admission_failure\":%d,\"stages\":{",
        json_bool(replay->reason == NULL),
        state_name(replay),
        json_bool(candidate),
        json_bool(candidate),
        reason != NULL ? reason : "NONE",
        replay->rejected,
        result->admission_failure
    );
    for (int i = 0; i < STAGE_COUNT; ++i) {
        print_stage(replay, i);
    }
    printf(
        "},\"result_identity\":{\"device_id\":%" PRIu32 ",\"motor_id\":%" PRIu32
        ",\"encoder_id\":%" PRIu32 ",\"frame_id\":%" PRIu32 ",\"session\":%" PRIu32
        ",\"encoder_cpr\":%" PRIu32 ",\"acquisition_direction\":%" PRId32
        ",\"runtime_direction\":%" PRId32 "},",
        replay->policy.device,
        replay->policy.workflow.identity.motor_setup_id,
        replay->policy.workflow.identity.encoder_id,
        replay->policy.workflow.identity.mechanical_frame_id,
        replay->policy.workflow.session,
        replay->policy.workflow.identity.acquisition.encoder_counts_per_revolution,
        replay->policy.workflow.identity.acquisition.encoder_direction,
        candidate ? result->snapshot.runtime_encoder_direction : 0
    );
    print_metrics(result);
    print_candidate(result, candidate);
    puts("}");
}

/* Public Functions ------------------------------------------------------------- */

int main(int argc, char **argv)
{
    struct replay replay = {.active = -1};
    int skip = 1;
    if (argc > 1 && strcmp(argv[1], "--validate") == 0) {
        replay.validate_only = true;
        ++skip;
    }
    if (!parse_policy(argc - skip, argv + skip, &replay.policy)) {
        fputs("Invalid normalized trace metadata\n", stderr);
        return 2;
    }
    if (foc_cal_workflow_init(&replay.workflow) != FOC_CAL_WORKFLOW_OK ||
        foc_cal_workflow_start(&replay.workflow, &replay.policy.workflow) != FOC_CAL_WORKFLOW_OK) {
        replay.reason = "INVALID_PROFILE";
        print_report(&replay);
        return 0;
    }
    char line[LINE_CAPACITY];
    int status = read_line(line);
    if (status != 1 || strcmp(line, csv_header) != 0) {
        replay.reason = "INVALID_HEADER";
    } else {
        replay.line_number = 1U;
        while ((status = read_line(line)) > 0) {
            if (++replay.line_number > MAX_ROWS + 1U) {
                replay.reason = "ROW_LIMIT";
                break;
            }
            receive(&replay, line);
        }
        if (status < 0) {
            reject_row(&replay, NULL, "INVALID_LINE");
        }
    }
    complete_replay(&replay);
    print_report(&replay);
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
