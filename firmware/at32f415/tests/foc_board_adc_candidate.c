/**
 * @file    foc_board_adc_candidate.c
 * @brief   Host adapter: Board1 ADC_ZERO trace -> existing ADC estimator -> isolated Runtime.
 * @version v0.1.0
 * @date    2026.11.21
 *
 * Test role
 * ---------
 * FW-ADC-01 cross-module adapter. The input is the samples.csv that the
 * production importer (board_capture.py) prepares from a CRC-, identity- and
 * time-checked Capture V1 image: logical A/B/C come from ranks 2/3/4. This tool
 * adds no statistics of its own. It feeds every row to the existing
 * foc_calibration_adc_collect()/finalize(), composes an isolated supply from the
 * resulting ADC candidate and the reviewed Rotor/Offset candidates, boots an
 * isolated Board1 instance through foc_board_boot_select(), and reads back the
 * Runtime offsets, a decode with foc_device_decode_sample() and the UART
 * readback. A separately booted formal instance must remain unchanged.
 *
 * Usage:
 *   foc-board-adc-candidate <samples.csv> --capture-id N --review-id N
 *       --source HOST_SYNTHETIC|HOST_VIRTUAL|BOARD_RECORDED
 *       --zero-current QUALIFIED|UNQUALIFIED [--fault NAME]
 * Exit: 0 candidate accepted in the isolated Runtime, 3 no qualified candidate,
 *       4 isolated integration rejected (nominal fallback), 5 atomicity or lock
 *       violation, 2 input error.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_board_boot_test.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Private Macros --------------------------------------------------------------- */

#define FOC_ADC_CANDIDATE_SUPPLY_ID 0x46414443U /* isolated FW-ADC-01 supply ("CDAF") */
#define FOC_ADC_CANDIDATE_MAX_ROWS  64U
#define FOC_ADC_LINE_MAX            512U

/* Private Types ---------------------------------------------------------------- */

struct options {
    const char *path;
    uint32_t capture_id;
    uint32_t review_id;
    const char *source;
    bool zero_current;
    const char *fault;
};

/* Private Variables ------------------------------------------------------------ */

static struct foc_boot_test formal;
static struct foc_boot_test isolated;
static struct foc_calibration_adc_sample samples[FOC_ADC_CANDIDATE_MAX_ROWS];

/* Private Helpers -------------------------------------------------------------- */

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

static const char *failure_name(enum foc_calibration_failure failure)
{
    return (unsigned int)failure < sizeof failure_names / sizeof failure_names[0]
        ? failure_names[failure]
        : "UNKNOWN";
}

static bool fault(const struct options *options, const char *name)
{
    return options->fault != NULL && strcmp(options->fault, name) == 0;
}

static bool parse_u32(const char *text, uint32_t *value)
{
    char *end = NULL;
    if (text == NULL || *text == '\0' || *text == '-') {
        return false;
    }
    const unsigned long long parsed = strtoull(text, &end, 0);
    if (*end != '\0' || parsed > UINT32_MAX) {
        return false;
    }
    *value = (uint32_t)parsed;
    return true;
}

static int parse_options(int argc, char **argv, struct options *options)
{
    *options = (struct options){0};
    if (argc < 2) {
        return -1;
    }
    options->path = argv[1];
    bool zero_seen = false;
    for (int index = 2; index + 1 < argc; index += 2) {
        const char *key = argv[index];
        const char *value = argv[index + 1];
        if (strcmp(key, "--capture-id") == 0 && parse_u32(value, &options->capture_id)) {
        } else if (strcmp(key, "--review-id") == 0 && parse_u32(value, &options->review_id)) {
        } else if (
            strcmp(key, "--source") == 0 &&
            (strcmp(value, "HOST_SYNTHETIC") == 0 || strcmp(value, "HOST_VIRTUAL") == 0 ||
             strcmp(value, "BOARD_RECORDED") == 0)
        ) {
            options->source = value;
        } else if (
            strcmp(key, "--zero-current") == 0 &&
            (strcmp(value, "QUALIFIED") == 0 || strcmp(value, "UNQUALIFIED") == 0)
        ) {
            options->zero_current = strcmp(value, "QUALIFIED") == 0;
            zero_seen = true;
        } else if (strcmp(key, "--fault") == 0) {
            options->fault = value;
        } else {
            return -1;
        }
    }
    if ((argc % 2) != 0 || options->capture_id == 0U || options->source == NULL || !zero_seen) {
        return -1;
    }
    /* Board-recorded data has no independent zero-current witness in this image. */
    if (strcmp(options->source, "BOARD_RECORDED") == 0 && options->zero_current) {
        return -2;
    }
    return 0;
}

/* Split one CSV line into exactly `count` fields; empty fields are allowed. */
static bool split(char *line, char **fields, unsigned int count)
{
    unsigned int index = 0U;
    fields[index++] = line;
    for (char *cursor = line; *cursor != '\0'; ++cursor) {
        if (*cursor == ',') {
            if (index == count) {
                return false;
            }
            *cursor = '\0';
            fields[index++] = cursor + 1;
        }
    }
    return index == count;
}

/* Read the production Trace V1 ADC_ZERO rows. Returns the row count or -1. */
static int read_trace(const struct options *options, const char **error)
{
    static const char header[] =
        "stage,capture,sequence,source_us,completion_us,trigger_us,reference_us,"
        "clock_id,reference_clock_id,unit,channel,flags,evidence,p1,p2,p3,p4,p5,p6";
    FILE *file = fopen(options->path, "rb");
    if (file == NULL) {
        *error = "TRACE_IO";
        return -1;
    }
    char line[FOC_ADC_LINE_MAX];
    int rows = 0;
    bool header_seen = false;
    while (fgets(line, sizeof line, file) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        if (!header_seen) {
            header_seen = strcmp(line, header) == 0;
            if (!header_seen) {
                break;
            }
            continue;
        }
        char *field[19];
        uint32_t stage, capture, sequence, source_us, clock, value[3];
        if (rows >= (int)FOC_ADC_CANDIDATE_MAX_ROWS || !split(line, field, 19U) ||
            !parse_u32(field[0], &stage) || !parse_u32(field[1], &capture) ||
            !parse_u32(field[2], &sequence) || !parse_u32(field[3], &source_us) ||
            !parse_u32(field[7], &clock) || !parse_u32(field[13], &value[0]) ||
            !parse_u32(field[14], &value[1]) || !parse_u32(field[15], &value[2]) ||
            *field[16] != '\0' || *field[17] != '\0' || *field[18] != '\0' || stage != 0U ||
            clock != 1U) {
            fclose(file);
            *error = "TRACE_FORMAT";
            return -1;
        }
        if (capture != options->capture_id) {
            fclose(file);
            *error = "CAPTURE_ID_MISMATCH";
            return -1;
        }
        if (value[0] > 0xFFFFU || value[1] > 0xFFFFU || value[2] > 0xFFFFU) {
            fclose(file);
            *error = "TRACE_FORMAT";
            return -1;
        }
        /* Logical A/B/C, source sequence and time travel unchanged into the estimator. */
        samples[rows++] = (struct foc_calibration_adc_sample){
            .phase_a = (uint16_t)value[0],
            .phase_b = (uint16_t)value[1],
            .phase_c = (uint16_t)value[2],
            .valid_mask = 7U,
            .sequence = sequence,
            .captured_us = source_us,
            .zero_current_confirmed = options->zero_current,
        };
    }
    fclose(file);
    if (!header_seen) {
        *error = "TRACE_FORMAT";
        return -1;
    }
    return rows;
}

static void print_float_field(const char *name, float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    printf(
        "\"%s\": %.6f, \"%s_bits\": \"0x%08lx\", ",
        name,
        (double)value,
        name,
        (unsigned long)bits
    );
}

/* Configuration that the formal instance exposes; must be identical before and after. */
struct formal_digest {
    struct foc_device_adc_config adc;
    struct foc_rotor_config rotor;
    struct foc_encoder_config encoder;
    struct foc_board_output output;
    struct foc_board_boot boot;
    uint32_t uart_offset_bits[3];
};

static bool digest(struct foc_boot_test *test, struct formal_digest *out)
{
    memset(out, 0, sizeof *out);
    out->adc = test->runtime.pipeline.adc_config;
    out->rotor = test->runtime.config.motion.rotor;
    foc_board_encoder_policy(&test->encoder, &out->encoder);
    out->output = test->output;
    out->boot = test->boot;
    for (uint16_t index = 0U; index < 3U; ++index) {
        struct foc_boot_test_parameter parameter;
        if (foc_boot_test_read(test, (uint16_t)(38U + index), &parameter) != FOC_PROTO_OK) {
            return false;
        }
        out->uart_offset_bits[index] = parameter.value;
    }
    return true;
}

static bool same_digest(const struct formal_digest *a, const struct formal_digest *b)
{
    return memcmp(&a->adc, &b->adc, sizeof a->adc) == 0 &&
        memcmp(&a->rotor, &b->rotor, sizeof a->rotor) == 0 &&
        memcmp(&a->encoder, &b->encoder, sizeof a->encoder) == 0 &&
        memcmp(&a->output, &b->output, sizeof a->output) == 0 &&
        memcmp(&a->boot, &b->boot, sizeof a->boot) == 0 &&
        memcmp(a->uart_offset_bits, b->uart_offset_bits, sizeof a->uart_offset_bits) == 0;
}

static int fail_calibrated(
    struct foc_runtime *runtime,
    struct foc_board_output *output,
    struct foc_board_encoder *encoder,
    const struct foc_calibration_snapshot *snapshot,
    uint32_t now_us
)
{
    (void)foc_board_init_with_calibration(runtime, output, encoder, snapshot, now_us);
    return FOC_RUNTIME_ERR_CONFIG;
}

/* Test Entry ------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    struct options options;
    const int parsed = parse_options(argc, argv, &options);
    if (parsed != 0) {
        printf(
            "{\"status\": \"REJECTED\", \"reason\": \"%s\"}\n",
            parsed == -2 ? "PRECONDITION_UNVERIFIED" : "USAGE"
        );
        return 2;
    }

    /* 1. Formal instance, booted nominal like the production default image. */
    if (foc_boot_test_select(&formal, NULL, NULL) != FOC_RUNTIME_OK ||
        foc_boot_test_hello(&formal) != 0) {
        printf("{\"status\": \"REJECTED\", \"reason\": \"FORMAL_BOOT\"}\n");
        return 2;
    }
    struct formal_digest before;
    if (!digest(&formal, &before)) {
        printf("{\"status\": \"REJECTED\", \"reason\": \"FORMAL_READBACK\"}\n");
        return 2;
    }

    /* 2. Production trace rows -> existing ADC estimator. */
    const char *error = NULL;
    const int rows = read_trace(&options, &error);
    if (rows < 0) {
        printf("{\"status\": \"REJECTED\", \"reason\": \"%s\"}\n", error);
        return 2;
    }
    const int sample_count = fault(&options, "short") ? (rows < 31 ? rows : 31) : rows;
    struct foc_calibration_identity identity;
    foc_board_calibration_identity(11U, &identity);
    if (fault(&options, "identity-cpr")) {
        identity.encoder_counts_per_revolution = 4096U;
    } else if (fault(&options, "identity-phase")) {
        identity.phase_order = 1U;
    } else if (fault(&options, "identity-gain")) {
        identity.current_a_per_count *= 2.0f;
    }
    const struct foc_calibration_adc_config adc_config = {
        .identity = identity,
        .required_valid_mask = 7U,
        .minimum_samples = fault(&options, "adc-init") ? 0U : 32U,
        .maximum_samples = FOC_ADC_CANDIDATE_MAX_ROWS,
        .maximum_sample_step_counts = 20.0f,
        .maximum_noise_rms_counts = 2.0f,
        .minimum_offset_counts = (float)FOC_BOARD_OFFSET_MIN_COUNTS,
        .maximum_offset_counts = (float)FOC_BOARD_OFFSET_MAX_COUNTS,
        .maximum_interval_us = 1000U,
    };
    struct foc_calibration_adc_context context;
    struct foc_calibration_adc_result candidate = {0};
    int collect_status = 0;
    int first_rejected = -1;
    const int init_status = foc_calibration_adc_init(&context, &adc_config);
    int finalize_status = init_status;
    if (init_status == 0) {
        for (int index = 0; index < sample_count && collect_status == 0; ++index) {
            collect_status = foc_calibration_adc_collect(&context, &samples[index]);
            if (collect_status != 0) {
                first_rejected = index;
            }
        }
        finalize_status = foc_calibration_adc_finalize(&context, &candidate);
    }
    const bool qualified = init_status == 0 && finalize_status == 0 && candidate.usable;
    printf(
        "{\"source\": \"%s\", \"zero_current\": \"%s\", \"capture_id\": %lu, \"rows\": %d, "
        "\"samples_collected\": %lu, \"first_sequence\": %lu, \"last_sequence\": %lu, "
        "\"first_source_us\": %lu, \"last_source_us\": %lu, \"adc_init_status\": %d, "
        "\"collect_status\": %d, \"first_rejected_row\": %d, \"finalize_status\": %d, "
        "\"candidate_failure\": \"%s\", \"candidate_qualified\": %s, ",
        options.source,
        options.zero_current ? "HOST_FIXTURE_DECLARED" : "UNQUALIFIED",
        (unsigned long)options.capture_id,
        rows,
        (unsigned long)candidate.samples,
        rows > 0 ? (unsigned long)samples[0].sequence : 0UL,
        rows > 0 ? (unsigned long)samples[rows - 1].sequence : 0UL,
        rows > 0 ? (unsigned long)samples[0].captured_us : 0UL,
        rows > 0 ? (unsigned long)samples[rows - 1].captured_us : 0UL,
        init_status,
        collect_status,
        first_rejected,
        finalize_status,
        init_status != 0 ? "INVALID_CONFIG" : failure_name(candidate.failure),
        qualified ? "true" : "false"
    );
    print_float_field("offset_a", candidate.offset_a_counts);
    print_float_field("offset_b", candidate.offset_b_counts);
    print_float_field("offset_c", candidate.offset_c_counts);
    print_float_field("noise_a", candidate.noise_rms_a_counts);
    print_float_field("noise_b", candidate.noise_rms_b_counts);
    print_float_field("noise_c", candidate.noise_rms_c_counts);
    if (!qualified) {
        struct formal_digest after;
        const bool untouched = digest(&formal, &after) && same_digest(&before, &after);
        printf(
            "\"status\": \"NO_QUALIFIED_CANDIDATE\", \"formal_untouched\": %s, "
            "\"scope\": \"ADC zero-offset only\"}\n",
            untouched ? "true" : "false"
        );
        return untouched ? 3 : 5;
    }

    /* 3. Isolated supply: ADC candidate + reviewed Rotor/Offset candidates. */
    struct foc_board_calibration_supply supply = *foc_board_calibration_supply();
    supply.supply_id = FOC_ADC_CANDIDATE_SUPPLY_ID;
    supply.review_id = options.review_id;
    supply.adc = candidate;
    if (fault(&options, "rotor-pole-pairs")) {
        supply.rotor.pole_pairs = 7U;
        supply.rotor.identity.pole_pairs = 7U;
    }
    struct foc_calibration_identity expected_identity;
    foc_board_calibration_identity(11U, &expected_identity);
    const struct foc_calibration_admission_input input = {
        .adc = &supply.adc,
        .rotor = &supply.rotor,
        .offset = &supply.offset,
        .expected_identity = &expected_identity,
        .limits = supply.limits,
    };
    enum foc_calibration_admission_failure admission = FOC_CALIBRATION_ADMISSION_NONE;
    if (foc_calibration_admit(&input, &supply.snapshot, &admission) != 0) {
        memset(&supply.snapshot, 0, sizeof supply.snapshot);
    }
    const struct foc_board_calibration_expectation expectation = {FOC_ADC_CANDIDATE_SUPPLY_ID, 11U};
    const struct foc_board_boot_ops runtime_fault = {fail_calibrated, foc_board_init};
    int status;
    if (fault(&options, "runtime-init")) {
        status = foc_board_boot_select_with(
            &isolated.boot,
            &isolated.runtime,
            &isolated.output,
            &isolated.encoder,
            &supply,
            &expectation,
            0U,
            &runtime_fault
        );
        if (status == FOC_RUNTIME_OK) {
            foc_serial_init(&isolated.serial, &isolated.runtime);
            foc_serial_attach_boot(
                &isolated.serial,
                foc_board_boot_serial_readback,
                &isolated.boot
            );
        }
    } else {
        status = foc_boot_test_select(&isolated, &supply, &expectation);
    }
    struct foc_board_boot_view view;
    const bool ready = status == FOC_RUNTIME_OK && foc_board_boot_read(&isolated.boot, &view);
    const bool accepted = ready && view.configuration == FOC_BOARD_CALIBRATED_CONFIGURATION;
    printf(
        "\"admission_failure\": %d, \"isolated_configuration\": %d, \"isolated_calibration\": %d, "
        "\"isolated_rejection\": %d, ",
        (int)admission,
        ready ? (int)view.configuration : 0,
        ready ? (int)view.calibration : 0,
        ready ? (int)view.rejection : -1
    );

    /* 4. Readback: Runtime, decode, UART, locked output. */
    bool consistent = ready;
    if (accepted) {
        const float runtime_offsets[3] = {
            isolated.runtime.pipeline.adc_config.phase_a_offset_count,
            isolated.runtime.pipeline.adc_config.phase_b_offset_count,
            isolated.runtime.pipeline.adc_config.phase_c_offset_count,
        };
        const float candidate_offsets[3] = {
            candidate.offset_a_counts,
            candidate.offset_b_counts,
            candidate.offset_c_counts,
        };
        const struct foc_device_adc_raw raw = {
            1241U,
            (uint16_t)lroundf(candidate.offset_a_counts),
            (uint16_t)lroundf(candidate.offset_b_counts),
            (uint16_t)lroundf(candidate.offset_c_counts),
            FOC_DEVICE_SAMPLE_ALL_VALID,
            0U,
            0U,
        };
        struct foc_device_measurement measured;
        const float gain = isolated.runtime.pipeline.adc_config.current_a_per_count;
        const bool decoded =
            foc_device_decode_sample(&raw, &isolated.runtime.pipeline.adc_config, &measured) == 0;
        const float decoded_a[3] = {
            measured.phase_current_a.a,
            measured.phase_current_a.b,
            measured.phase_current_a.c,
        };
        bool uart = foc_boot_test_hello(&isolated) == 0;
        for (unsigned int index = 0U; index < 3U; ++index) {
            struct foc_boot_test_parameter parameter;
            uart = uart &&
                foc_boot_test_read(&isolated, (uint16_t)(38U + index), &parameter) == 0 &&
                parameter.value == foc_boot_test_bits(runtime_offsets[index]);
            consistent = consistent && runtime_offsets[index] == candidate_offsets[index] &&
                decoded &&
                fabsf(
                    decoded_a[index] -
                    ((float)lroundf(candidate_offsets[index]) - candidate_offsets[index]) * gain
                ) < 1.0e-6f &&
                fabsf(decoded_a[index]) <= 0.5f * gain + 1.0e-7f;
        }
        struct foc_boot_test_parameter input_state;
        uart = uart && foc_boot_test_read(&isolated, 33U, &input_state) == 0 &&
            input_state.value == FOC_BOARD_CALIBRATION_ACCEPTED;
        struct foc_protocol_frame response;
        const bool locked = foc_boot_test_call(&isolated, FOC_MSG_ARM, NULL, 0U, &response) ==
                FOC_PROTO_OUTPUT_LOCKED &&
            foc_boot_test_call(&isolated, FOC_MSG_PARAM_SAVE, NULL, 0U, &response) ==
                FOC_PROTO_NOT_SUPPORTED &&
            isolated.output.declined_updates == 0U && isolated.runtime.pipeline.mock.updates == 0U;
        const struct foc_output duties = {
            .output_enable = 1U,
            .svpwm = {.duty_a = 0.4f, .duty_b = 0.5f, .duty_c = 0.6f},
        };
        struct foc_device_pwm_request update;
        const bool refused =
            foc_device_map_output(&duties, &isolated.runtime.pipeline.pwm_config, &update) == 0 &&
            update.action == FOC_DEVICE_OUTPUT_UPDATE &&
            !foc_board_output_submit(&isolated.output, &update) &&
            isolated.output.result == FOC_BOARD_UPDATE_NOT_APPLIED;
        consistent = consistent && uart && locked && refused;
        printf(
            "\"runtime_offsets_equal_candidate\": %s, \"decoded_current_a\": [%.9f, %.9f, %.9f], "
            "\"uart_offsets_equal_runtime\": %s, \"output_locked\": %s, \"update_refused\": %s, ",
            consistent ? "true" : "false",
            (double)decoded_a[0],
            (double)decoded_a[1],
            (double)decoded_a[2],
            uart ? "true" : "false",
            locked ? "true" : "false",
            refused ? "true" : "false"
        );
    } else if (ready) {
        /* Rejected: the isolated instance must be the complete nominal configuration. */
        consistent = view.configuration == FOC_BOARD_NOMINAL_CONFIGURATION &&
            view.phase_a_offset_count == 2048.0f && view.phase_b_offset_count == 2048.0f &&
            view.phase_c_offset_count == 2048.0f && !isolated.encoder.config_valid;
    }
    struct formal_digest after;
    const bool untouched = digest(&formal, &after) && same_digest(&before, &after);
    const char *verdict = !untouched || !consistent ? "ATOMICITY_OR_LOCK_VIOLATION"
        : accepted                                  ? "CANDIDATE_ACCEPTED_ISOLATED"
                                                    : "CANDIDATE_REJECTED_NOMINAL_FALLBACK";
    printf(
        "\"formal_untouched\": %s, \"status\": \"%s\", "
        "\"scope\": \"ADC zero-offset only; Rotor/Offset are the reviewed synthetic "
        "candidates\"}\n",
        untouched ? "true" : "false",
        verdict
    );
    if (!untouched || !consistent) {
        return 5;
    }
    return accepted ? 0 : 4;
}

/* ---------------------------------------- end of file ----------------------------------------- */
