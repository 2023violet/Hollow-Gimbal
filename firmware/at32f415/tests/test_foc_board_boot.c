/**
 * @file    test_foc_board_boot.c
 * @brief   Formal Board1 startup selection with the synthetic verification supply.
 * @version v0.1.0
 * @date    2026.11.21
 *
 * Test role
 * ---------
 * Link the same supply and expectation translation units as the locked
 * verification image, call the production foc_board_boot_select(), and follow
 * the selected configuration through the real Board ADC and Encoder ports, the
 * Runtime and the UART V1 adapter. Every rejection must leave a complete nominal
 * configuration with no candidate residue.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_adc_test_clock.h"
#include "foc_board_boot_test.h"

#include <math.h>
#include <stdio.h>

/* Private Macros --------------------------------------------------------------- */

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return 1;                                                       \
        }                                                                   \
    } while (0)

#define TWO_PI 6.28318530717958647692f

/* Private Constants ------------------------------------------------------------ */

enum {
    PARAM_POLE_PAIRS = 16,
    PARAM_ELECTRICAL_OFFSET = 17,
    PARAM_BOOT_CONFIGURATION = 32,
    PARAM_CALIBRATION_INPUT = 33,
    PARAM_CALIBRATION_REJECTION = 34,
    PARAM_ADMISSION_FAILURE = 35,
    PARAM_SUPPLY_ID = 36,
    PARAM_REVIEW_ID = 37,
    PARAM_PHASE_A_OFFSET = 38,
    PARAM_ENCODER_CPR = 41,
    PARAM_RUNTIME_DIRECTION = 42,
    PARAM_SOURCE_DIRECTION = 43,
    PARAM_CALIBRATION_DIRECTION = 44,
    PARAM_MEASUREMENT_QUALIFIED = 45,
};

/* Private Variables ------------------------------------------------------------ */

static struct foc_boot_test fixture;

/* Private Helpers -------------------------------------------------------------- */

static float wrap_2pi(float angle)
{
    float wrapped = fmodf(angle, TWO_PI);
    if (wrapped < 0.0f) {
        wrapped += TWO_PI;
    }
    return wrapped >= TWO_PI ? 0.0f : wrapped;
}

static const struct foc_board_calibration_supply *supply(void)
{
    return foc_board_calibration_supply();
}

static struct foc_board_calibration_expectation expected(void)
{
    return foc_board_calibration_expectation();
}

static void reset_fixture(void)
{
    memset(&fixture, 0, sizeof fixture);
}

/* Complete nominal configuration and no calibrated residue anywhere. */
static int check_nominal(const struct foc_boot_test *test)
{
    struct foc_board_boot_view view;
    struct foc_encoder_config policy;
    CHECK(test->boot.runtime_ready && test->boot.configuration == FOC_BOARD_NOMINAL_CONFIGURATION);
    CHECK(foc_board_boot_read(&test->boot, &view));
    CHECK(view.phase_a_offset_count == 2048.0f && view.phase_b_offset_count == 2048.0f);
    CHECK(view.phase_c_offset_count == 2048.0f);
    CHECK(view.pole_pairs == 7U && view.electrical_offset_rad == 0.0f);
    CHECK(view.runtime_encoder_direction == FOC_ENCODER_DIRECTION_REVERSED);
    CHECK(view.calibration_direction == 0 && !view.current_measurement_qualified);
    CHECK(!test->encoder.config_valid && !test->encoder.sensor.initialized);
    foc_board_encoder_policy(&test->encoder, &policy);
    CHECK(policy.direction == FOC_ENCODER_DIRECTION_REVERSED);
    CHECK(test->runtime.pipeline.output_arg == &test->output);
    CHECK(test->output.result == FOC_BOARD_INHIBIT_SOFTWARE && test->output.declined_updates == 0U);
    return 0;
}

static int expect_rejected(
    const struct foc_board_calibration_supply *candidate,
    const struct foc_board_calibration_expectation *expectation,
    enum foc_board_calibration_rejection rejection,
    enum foc_calibration_admission_failure admission
)
{
    reset_fixture();
    CHECK(foc_boot_test_select(&fixture, candidate, expectation) == FOC_RUNTIME_OK);
    CHECK(fixture.boot.calibration == FOC_BOARD_CALIBRATION_REJECTED);
    if (fixture.boot.rejection != rejection || fixture.boot.admission_failure != admission) {
        fprintf(
            stderr,
            "rejection %d/%d, expected %d/%d\n",
            (int)fixture.boot.rejection,
            (int)fixture.boot.admission_failure,
            (int)rejection,
            (int)admission
        );
        return 1;
    }
    CHECK(fixture.boot.calibrated_status == 0 || rejection == FOC_BOARD_REJECTION_INITIALIZATION);
    CHECK(check_nominal(&fixture) == 0);
    struct foc_boot_test_parameter parameter;
    CHECK(foc_boot_test_hello(&fixture) == 0);
    CHECK(foc_boot_test_read(&fixture, PARAM_CALIBRATION_REJECTION, &parameter) == FOC_PROTO_OK);
    CHECK(parameter.value == (uint32_t)rejection);
    CHECK(foc_boot_test_read(&fixture, PARAM_ADMISSION_FAILURE, &parameter) == FOC_PROTO_OK);
    CHECK(parameter.value == (uint32_t)admission);
    CHECK(foc_boot_test_read(&fixture, PARAM_BOOT_CONFIGURATION, &parameter) == FOC_PROTO_OK);
    CHECK(parameter.value == FOC_BOARD_NOMINAL_CONFIGURATION);
    return 0;
}

static uint16_t synthetic_adc_value(float target, uint32_t index)
{
    const float base_float = floorf(target);
    const unsigned int base = (unsigned int)base_float;
    const unsigned int high_count = (unsigned int)lroundf((target - base_float) * 64.0f);
    return (uint16_t)(base + (index < high_count ? 1U : 0U));
}

/* Recreate the reviewed candidates with the existing CAL-01 estimators. */
static int estimate_candidates(
    struct foc_calibration_adc_result *adc,
    struct foc_calibration_rotor_result *rotor,
    struct foc_calibration_offset_result *offset
)
{
    const struct foc_calibration_snapshot *reviewed = &supply()->snapshot;
    struct foc_calibration_identity identity;
    foc_board_calibration_identity(expected().pole_pairs, &identity);
    const struct foc_calibration_adc_config adc_config = {
        .identity = identity,
        .required_valid_mask = 0x07U,
        .minimum_samples = 32U,
        .maximum_samples = 64U,
        .maximum_sample_step_counts = 20.0f,
        .maximum_noise_rms_counts = 2.0f,
        .minimum_offset_counts = 32.0f,
        .maximum_offset_counts = 4063.0f,
        .maximum_interval_us = 1000U,
    };
    struct foc_calibration_adc_context adc_context = {0};
    CHECK(foc_calibration_adc_init(&adc_context, &adc_config) == 0);
    for (uint32_t index = 0U; index < 64U; ++index) {
        const struct foc_calibration_adc_sample sample = {
            .phase_a = synthetic_adc_value(reviewed->phase_a_offset_count, index),
            .phase_b = synthetic_adc_value(reviewed->phase_b_offset_count, index),
            .phase_c = synthetic_adc_value(reviewed->phase_c_offset_count, index),
            .valid_mask = 0x07U,
            .sequence = index,
            .captured_us = (index + 1U) * 100U,
            .zero_current_confirmed = true,
        };
        CHECK(foc_calibration_adc_collect(&adc_context, &sample) == 0);
    }
    CHECK(foc_calibration_adc_finalize(&adc_context, adc) == 0);

    struct foc_calibration_identity search = identity;
    search.pole_pairs = 0U;
    const struct foc_calibration_rotor_config rotor_config = {
        .identity = search,
        .minimum_pole_pairs = 1U,
        .maximum_pole_pairs = 20U,
        .minimum_mechanical_span_rad = 8.0f,
        .maximum_residual_rad = 0.02f,
        .minimum_candidate_separation_rad = 0.05f,
        .maximum_interval_us = 100000U,
        .maximum_samples = 64U,
    };
    struct foc_calibration_rotor_context rotor_context = {0};
    CHECK(foc_calibration_rotor_init(&rotor_context, &rotor_config) == 0);
    const float electrical_zero = 1.25f;
    const float direction = (float)reviewed->calibration_direction;
    const float poles = (float)reviewed->pole_pairs;
    for (uint32_t index = 0U; index < 40U; ++index) {
        const float mechanical = -4.2f + (float)index * 0.73f;
        const struct foc_calibration_angle_sample sample = {
            .mechanical_rad = mechanical,
            .electrical_rad = wrap_2pi(direction * poles * mechanical + electrical_zero),
            .captured_us = (index + 1U) * 1000U,
            .sequence = index,
            .valid = true,
        };
        CHECK(foc_calibration_rotor_collect(&rotor_context, &sample) == 0);
    }
    CHECK(foc_calibration_rotor_finalize(&rotor_context, rotor) == 0);

    struct foc_calibration_identity offset_identity = identity;
    offset_identity.encoder_direction = rotor->direction;
    offset_identity.pole_pairs = rotor->pole_pairs;
    const struct foc_calibration_offset_config offset_config = {
        .identity = offset_identity,
        .maximum_residual_rad = 0.02f,
        .maximum_interval_us = 100000U,
        .maximum_samples = 64U,
    };
    struct foc_calibration_offset_context offset_context = {0};
    CHECK(foc_calibration_offset_init(&offset_context, &offset_config) == 0);
    for (uint32_t index = 0U; index < 20U; ++index) {
        const float mechanical = -2.0f + (float)index * 0.37f;
        const struct foc_calibration_angle_sample sample = {
            .mechanical_rad = mechanical,
            .electrical_rad = wrap_2pi(direction * poles * mechanical + electrical_zero),
            .captured_us = (index + 1U) * 1000U,
            .sequence = index,
            .valid = true,
        };
        CHECK(foc_calibration_offset_collect(&offset_context, &sample) == 0);
    }
    CHECK(foc_calibration_offset_finalize(&offset_context, offset) == 0);
    return 0;
}

/* Fails after the real calibrated init has committed, to prove the scrub. */
static int fail_after_commit(
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

static int fail_calibrated(
    struct foc_runtime *runtime,
    struct foc_board_output *output,
    struct foc_board_encoder *encoder,
    const struct foc_calibration_snapshot *snapshot,
    uint32_t now_us
)
{
    (void)runtime;
    (void)output;
    (void)encoder;
    (void)snapshot;
    (void)now_us;
    return FOC_RUNTIME_ERR_CONFIG;
}

static int
fail_nominal(struct foc_runtime *runtime, struct foc_board_output *output, uint32_t now_us)
{
    (void)foc_board_init(runtime, output, now_us);
    return FOC_RUNTIME_ERR_CONFIG;
}

/* Test Cases ------------------------------------------------------------------- */

static int test_verification_supply_is_accepted(void)
{
    const struct foc_board_calibration_supply *reviewed = supply();
    const struct foc_board_calibration_expectation expectation = expected();
    CHECK(reviewed != NULL && reviewed->format == FOC_BOARD_CALIBRATION_SUPPLY_FORMAT);
    CHECK(expectation.supply_id == reviewed->supply_id && expectation.pole_pairs == 11U);

    reset_fixture();
    CHECK(foc_boot_test_select(&fixture, reviewed, &expectation) == FOC_RUNTIME_OK);
    CHECK(fixture.boot.configuration == FOC_BOARD_CALIBRATED_CONFIGURATION);
    CHECK(fixture.boot.calibration == FOC_BOARD_CALIBRATION_ACCEPTED);
    CHECK(fixture.boot.rejection == FOC_BOARD_REJECTION_NONE && fixture.boot.runtime_ready);

    struct foc_board_boot_view view;
    CHECK(foc_board_boot_read(&fixture.boot, &view));
    CHECK(view.phase_a_offset_count == 2031.5f && view.phase_b_offset_count == 2063.25f);
    CHECK(view.phase_c_offset_count == 2017.75f);
    CHECK(fixture.runtime.pipeline.adc_config.phase_a_offset_count == 2031.5f);
    CHECK(view.pole_pairs == 11U && fixture.runtime.config.motion.rotor.pole_pairs == 11U);
    CHECK(view.electrical_offset_rad == reviewed->snapshot.electrical_offset_rad);
    CHECK(view.encoder_counts_per_revolution == 65536U);
    CHECK(view.source_encoder_direction == -1 && view.calibration_direction == -1);
    CHECK(view.runtime_encoder_direction == 1);
    CHECK(
        view.runtime_encoder_direction == view.source_encoder_direction * view.calibration_direction
    );
    CHECK(fixture.encoder.config_valid && fixture.encoder.config.direction == 1);
    CHECK(!view.current_measurement_qualified);
    CHECK(fixture.output.result == FOC_BOARD_INHIBIT_SOFTWARE);
    CHECK(fixture.runtime.pipeline.output_arg == &fixture.output);

    struct foc_capture_profile profile;
    CHECK(foc_board_boot_capture_profile(&fixture.boot, 5U, &profile));
    CHECK(profile.build_tag == 5U && profile.encoder_cpr == 65536U);
    CHECK(profile.encoder_direction == 1 && profile.pole_pairs == 11U && profile.phase_order == 0U);
    CHECK(profile.configuration == FOC_CAPTURE_CONFIGURATION_CALIBRATED);
    printf(
        "accepted supply 0x%08lx review 0x%08lx: offsets %.2f/%.2f/%.2f pp %u eoff %.7f dir "
        "src %ld x cal %ld = runtime %ld\n",
        (unsigned long)view.supply_id,
        (unsigned long)view.review_id,
        (double)view.phase_a_offset_count,
        (double)view.phase_b_offset_count,
        (double)view.phase_c_offset_count,
        (unsigned int)view.pole_pairs,
        (double)view.electrical_offset_rad,
        (long)view.source_encoder_direction,
        (long)view.calibration_direction,
        (long)view.runtime_encoder_direction
    );
    return 0;
}

static int test_audit_chain(void)
{
    const struct foc_board_calibration_supply *reviewed = supply();
    const struct foc_board_calibration_expectation expectation = expected();

    /* The reviewed candidates are reachable by the existing estimators. */
    struct foc_calibration_adc_result adc;
    struct foc_calibration_rotor_result rotor;
    struct foc_calibration_offset_result offset;
    CHECK(estimate_candidates(&adc, &rotor, &offset) == 0);
    CHECK(adc.usable && rotor.identifiable && offset.usable);
    printf(
        "estimators: offsets %.5f/%.5f/%.5f noise %.5f/%.5f/%.5f rotor dir %ld pp %u eoff %.6f "
        "offset eoff %.6f\n",
        (double)adc.offset_a_counts,
        (double)adc.offset_b_counts,
        (double)adc.offset_c_counts,
        (double)adc.noise_rms_a_counts,
        (double)adc.noise_rms_b_counts,
        (double)adc.noise_rms_c_counts,
        (long)rotor.direction,
        (unsigned int)rotor.pole_pairs,
        (double)rotor.electrical_offset_rad,
        (double)offset.electrical_offset_rad
    );
    /* Float accumulation in the streaming estimator; 0.001 count is far below ADC resolution. */
    CHECK(fabsf(adc.offset_a_counts - reviewed->adc.offset_a_counts) < 1.0e-3f);
    CHECK(fabsf(adc.offset_b_counts - reviewed->adc.offset_b_counts) < 1.0e-3f);
    CHECK(fabsf(adc.offset_c_counts - reviewed->adc.offset_c_counts) < 1.0e-3f);
    CHECK(fabsf(adc.noise_rms_a_counts - reviewed->adc.noise_rms_a_counts) < 0.02f);
    CHECK(fabsf(adc.noise_rms_b_counts - reviewed->adc.noise_rms_b_counts) < 0.02f);
    CHECK(rotor.direction == reviewed->rotor.direction);
    CHECK(rotor.pole_pairs == reviewed->rotor.pole_pairs);
    CHECK(fabsf(rotor.electrical_offset_rad - reviewed->rotor.electrical_offset_rad) < 2.0e-3f);
    CHECK(fabsf(offset.electrical_offset_rad - reviewed->offset.electrical_offset_rad) < 2.0e-3f);

    /* Board recomputes the snapshot bit-exactly from the supplied candidates. */
    struct foc_calibration_snapshot recomputed;
    enum foc_calibration_admission_failure failure;
    CHECK(
        foc_board_boot_qualify(reviewed, &expectation, &recomputed, &failure) ==
        FOC_BOARD_REJECTION_NONE
    );
    CHECK(failure == FOC_CALIBRATION_ADMISSION_NONE);
    CHECK(recomputed.phase_b_offset_count == reviewed->snapshot.phase_b_offset_count);
    CHECK(recomputed.runtime_encoder_direction == reviewed->snapshot.runtime_encoder_direction);
    CHECK(recomputed.electrical_offset_rad == reviewed->snapshot.electrical_offset_rad);

    /* accepted=true with unusable candidates proves nothing. */
    struct foc_board_calibration_supply forged = *reviewed;
    forged.adc.usable = false;
    CHECK(forged.snapshot.accepted);
    CHECK(
        expect_rejected(
            &forged,
            &expectation,
            FOC_BOARD_REJECTION_ADMISSION,
            FOC_CALIBRATION_ADMISSION_ADC_QUALITY
        ) == 0
    );
    return 0;
}

static int test_rejections_fall_back_to_nominal(void)
{
    const struct foc_board_calibration_supply *reviewed = supply();
    const struct foc_board_calibration_expectation expectation = expected();
    struct foc_board_calibration_supply bad;
    const float nan_value = nanf("");

    bad = *reviewed;
    bad.adc.offset_a_counts = nan_value;
    CHECK(
        expect_rejected(
            &bad,
            &expectation,
            FOC_BOARD_REJECTION_ADMISSION,
            FOC_CALIBRATION_ADMISSION_ADC_RANGE
        ) == 0
    );
    bad = *reviewed;
    bad.snapshot.phase_c_offset_count = nan_value;
    CHECK(
        expect_rejected(
            &bad,
            &expectation,
            FOC_BOARD_REJECTION_SNAPSHOT_MISMATCH,
            FOC_CALIBRATION_ADMISSION_NONE
        ) == 0
    );
    /* Admissible under the supply's permissive limits but outside the Board window. */
    bad = *reviewed;
    bad.limits.maximum_offset_counts = 4095.0f;
    bad.adc.offset_a_counts = 3000.0f;
    bad.snapshot.phase_a_offset_count = 3000.0f;
    CHECK(
        expect_rejected(
            &bad,
            &expectation,
            FOC_BOARD_REJECTION_BOARD_RANGE,
            FOC_CALIBRATION_ADMISSION_NONE
        ) == 0
    );

    const struct {
        const char *name;
        uint32_t cpr;
        int32_t source_direction;
        uint32_t phase_order;
        float gain_scale;
        uint16_t pole_pairs;
    } identities[] = {
        {"cpr", 4096U, -1, 0U, 1.0f, 11U},
        {"source direction", 65536U, 1, 0U, 1.0f, 11U},
        {"phase order", 65536U, -1, 1U, 1.0f, 11U},
        {"adc gain", 65536U, -1, 0U, 2.0f, 11U},
        {"pole pairs 0", 65536U, -1, 0U, 1.0f, 0U},
        {"pole pairs 7", 65536U, -1, 0U, 1.0f, 7U},
    };
    for (size_t index = 0U; index < sizeof identities / sizeof identities[0]; ++index) {
        bad = *reviewed;
        struct foc_calibration_identity *ids[] = {
            &bad.adc.identity,
            &bad.rotor.identity,
            &bad.offset.identity,
            &bad.snapshot.source_identity,
        };
        for (size_t id = 0U; id < 4U; ++id) {
            ids[id]->encoder_counts_per_revolution = identities[index].cpr;
            ids[id]->phase_order = identities[index].phase_order;
            ids[id]->current_a_per_count *= identities[index].gain_scale;
            ids[id]->pole_pairs = identities[index].pole_pairs;
        }
        bad.adc.identity.encoder_direction = identities[index].source_direction;
        bad.snapshot.source_identity.encoder_direction = identities[index].source_direction;
        bad.rotor.pole_pairs = identities[index].pole_pairs;
        bad.snapshot.pole_pairs = identities[index].pole_pairs;
        if (expect_rejected(
                &bad,
                &expectation,
                FOC_BOARD_REJECTION_ADMISSION,
                FOC_CALIBRATION_ADMISSION_ADC_IDENTITY
            ) != 0) {
            fprintf(stderr, "identity case: %s\n", identities[index].name);
            return 1;
        }
    }

    /* The expectation, not the candidate, decides the pole-pair identity. */
    const struct foc_board_calibration_expectation seven = {expectation.supply_id, 7U};
    CHECK(
        expect_rejected(
            reviewed,
            &seven,
            FOC_BOARD_REJECTION_ADMISSION,
            FOC_CALIBRATION_ADMISSION_ADC_IDENTITY
        ) == 0
    );
    const struct foc_board_calibration_expectation zero = {expectation.supply_id, 0U};
    CHECK(
        expect_rejected(
            reviewed,
            &zero,
            FOC_BOARD_REJECTION_BOARD_IDENTITY,
            FOC_CALIBRATION_ADMISSION_NONE
        ) == 0
    );

    /* Direction-product conflict in the supplied output. */
    bad = *reviewed;
    bad.snapshot.runtime_encoder_direction = -1;
    CHECK(
        expect_rejected(
            &bad,
            &expectation,
            FOC_BOARD_REJECTION_SNAPSHOT_MISMATCH,
            FOC_CALIBRATION_ADMISSION_NONE
        ) == 0
    );
    /* Rotor and offset candidates disagree on the calibration direction. */
    bad = *reviewed;
    bad.offset.identity.encoder_direction = 1;
    CHECK(
        expect_rejected(
            &bad,
            &expectation,
            FOC_BOARD_REJECTION_ADMISSION,
            FOC_CALIBRATION_ADMISSION_OFFSET_IDENTITY
        ) == 0
    );

    /* review_id: a nonzero reference is required, but it never substitutes for checks. */
    CHECK(reviewed->review_id != 0U);
    bad = *reviewed;
    bad.review_id = 0U;
    CHECK(
        expect_rejected(
            &bad,
            &expectation,
            FOC_BOARD_REJECTION_REVIEW_REFERENCE,
            FOC_CALIBRATION_ADMISSION_NONE
        ) == 0
    );
    /* Valid review reference, wrong supply identity. */
    bad = *reviewed;
    bad.supply_id ^= 1U;
    CHECK(bad.review_id != 0U);
    CHECK(
        expect_rejected(
            &bad,
            &expectation,
            FOC_BOARD_REJECTION_SUPPLY_IDENTITY,
            FOC_CALIBRATION_ADMISSION_NONE
        ) == 0
    );
    /* Valid review reference, mathematically unqualified candidate. */
    bad = *reviewed;
    bad.rotor.residual_rms_rad = 0.5f;
    CHECK(
        expect_rejected(
            &bad,
            &expectation,
            FOC_BOARD_REJECTION_ADMISSION,
            FOC_CALIBRATION_ADMISSION_ROTOR_MAPPING
        ) == 0
    );
    /* Valid review reference, snapshot that the candidates do not reproduce. */
    bad = *reviewed;
    bad.snapshot.electrical_offset_rad += 0.01f;
    CHECK(
        expect_rejected(
            &bad,
            &expectation,
            FOC_BOARD_REJECTION_SNAPSHOT_MISMATCH,
            FOC_CALIBRATION_ADMISSION_NONE
        ) == 0
    );
    /* A different nonzero review reference is still accepted: it is not matched. */
    bad = *reviewed;
    bad.review_id ^= 0x00010000U;
    reset_fixture();
    CHECK(foc_boot_test_select(&fixture, &bad, &expectation) == FOC_RUNTIME_OK);
    CHECK(fixture.boot.configuration == FOC_BOARD_CALIBRATED_CONFIGURATION);
    CHECK(fixture.boot.review_id == bad.review_id);
    const struct foc_board_calibration_expectation none = {0U, 11U};
    bad = *reviewed;
    bad.supply_id = 0U;
    CHECK(
        expect_rejected(
            &bad,
            &none,
            FOC_BOARD_REJECTION_SUPPLY_IDENTITY,
            FOC_CALIBRATION_ADMISSION_NONE
        ) == 0
    );
    bad = *reviewed;
    bad.coordinate = FOC_BOARD_COORDINATE_LUT_CORRECTED;
    CHECK(
        expect_rejected(
            &bad,
            &expectation,
            FOC_BOARD_REJECTION_COORDINATE,
            FOC_CALIBRATION_ADMISSION_NONE
        ) == 0
    );
    bad.coordinate = FOC_BOARD_COORDINATE_UNKNOWN;
    CHECK(
        expect_rejected(
            &bad,
            &expectation,
            FOC_BOARD_REJECTION_COORDINATE,
            FOC_CALIBRATION_ADMISSION_NONE
        ) == 0
    );
    bad = *reviewed;
    bad.format ^= 0x01000000U;
    CHECK(
        expect_rejected(
            &bad,
            &expectation,
            FOC_BOARD_REJECTION_FORMAT,
            FOC_CALIBRATION_ADMISSION_NONE
        ) == 0
    );
    return 0;
}

static int test_explicit_entry_checks_direction_product(void)
{
    struct foc_calibration_snapshot conflict = supply()->snapshot;
    conflict.runtime_encoder_direction = -1;
    CHECK(foc_calibration_snapshot_valid(&conflict));
    reset_fixture();
    fixture.output.result = FOC_BOARD_UPDATE_NOT_APPLIED;
    fixture.runtime.boot_us = 0xA5A5A5A5U;
    CHECK(
        foc_board_init_with_calibration(
            &fixture.runtime,
            &fixture.output,
            &fixture.encoder,
            &conflict,
            0U
        ) == FOC_RUNTIME_ERR_CONFIG
    );
    CHECK(fixture.runtime.boot_us == 0xA5A5A5A5U && !fixture.runtime.initialized);
    CHECK(fixture.output.result == FOC_BOARD_UPDATE_NOT_APPLIED && !fixture.encoder.config_valid);
    return 0;
}

static int test_initialization_failures(void)
{
    const struct foc_board_calibration_supply *reviewed = supply();
    const struct foc_board_calibration_expectation expectation = expected();

    /* Calibrated init committed and then reported failure: nominal must replace it. */
    const struct foc_board_boot_ops after_commit = {fail_after_commit, foc_board_init};
    reset_fixture();
    CHECK(
        foc_board_boot_select_with(
            &fixture.boot,
            &fixture.runtime,
            &fixture.output,
            &fixture.encoder,
            reviewed,
            &expectation,
            0U,
            &after_commit
        ) == FOC_RUNTIME_OK
    );
    CHECK(fixture.boot.calibration == FOC_BOARD_CALIBRATION_REJECTED);
    CHECK(fixture.boot.rejection == FOC_BOARD_REJECTION_INITIALIZATION);
    CHECK(fixture.boot.calibrated_status == FOC_RUNTIME_ERR_CONFIG);
    CHECK(check_nominal(&fixture) == 0);

    /* Neither configuration initializes: locked stop, no report, no readback. */
    const struct foc_board_boot_ops both = {fail_calibrated, fail_nominal};
    reset_fixture();
    CHECK(
        foc_board_boot_select_with(
            &fixture.boot,
            &fixture.runtime,
            &fixture.output,
            &fixture.encoder,
            reviewed,
            &expectation,
            0U,
            &both
        ) == FOC_RUNTIME_ERR_CONFIG
    );
    CHECK(
        fixture.boot.configuration == FOC_BOARD_CONFIGURATION_NONE && !fixture.boot.runtime_ready
    );
    CHECK(fixture.boot.rejection == FOC_BOARD_REJECTION_INITIALIZATION);
    CHECK(fixture.boot.runtime == NULL && fixture.boot.encoder == NULL);
    CHECK(!fixture.runtime.initialized && fixture.runtime.pipeline.output_arg == NULL);
    CHECK(!fixture.encoder.config_valid);
    struct foc_board_boot_view view;
    struct foc_capture_profile profile;
    CHECK(!foc_board_boot_read(&fixture.boot, &view));
    CHECK(!foc_board_boot_capture_profile(&fixture.boot, 0U, &profile));
    /* The serial readback adapter bound to the failed report produces nothing. */
    struct foc_serial_boot_readback readback = {.configuration = 99U};
    CHECK(!foc_board_boot_serial_readback(&fixture.boot, &readback));
    CHECK(readback.configuration == 99U);

    /* No supply and a failing nominal init. */
    const struct foc_board_boot_ops nominal_only = {foc_board_init_with_calibration, fail_nominal};
    reset_fixture();
    CHECK(
        foc_board_boot_select_with(
            &fixture.boot,
            &fixture.runtime,
            &fixture.output,
            &fixture.encoder,
            NULL,
            NULL,
            0U,
            &nominal_only
        ) == FOC_RUNTIME_ERR_CONFIG
    );
    CHECK(fixture.boot.calibration == FOC_BOARD_CALIBRATION_NOT_AVAILABLE);
    CHECK(fixture.boot.configuration == FOC_BOARD_CONFIGURATION_NONE);
    CHECK(!fixture.runtime.initialized);

    /* A started encoder is refused before anything is written. */
    reset_fixture();
    fixture.encoder.sensor.initialized = true;
    fixture.runtime.boot_us = 0x5A5A5A5AU;
    CHECK(
        foc_board_boot_select(
            &fixture.boot,
            &fixture.runtime,
            &fixture.output,
            &fixture.encoder,
            reviewed,
            &expectation,
            0U
        ) == FOC_RUNTIME_ERR_CONFIG
    );
    CHECK(fixture.boot.configuration == FOC_BOARD_CONFIGURATION_NONE);
    CHECK(fixture.runtime.boot_us == 0x5A5A5A5AU && !fixture.runtime.initialized);
    CHECK(
        foc_board_boot_select(
            NULL,
            &fixture.runtime,
            &fixture.output,
            &fixture.encoder,
            NULL,
            NULL,
            0U
        ) == FOC_RUNTIME_ERR_INPUT
    );
    CHECK(
        foc_board_boot_select(
            &fixture.boot,
            NULL,
            &fixture.output,
            &fixture.encoder,
            NULL,
            NULL,
            0U
        ) == FOC_RUNTIME_ERR_INPUT
    );
    CHECK(!fixture.boot.runtime_ready);
    return 0;
}

static int test_unready_report_has_no_readback(void)
{
    const struct foc_board_boot_ops both = {fail_calibrated, fail_nominal};
    static struct foc_boot_test failed;
    memset(&failed, 0, sizeof failed);
    CHECK(
        foc_board_boot_select_with(
            &failed.boot,
            &failed.runtime,
            &failed.output,
            &failed.encoder,
            supply(),
            &(struct foc_board_calibration_expectation){0},
            0U,
            &both
        ) != FOC_RUNTIME_OK
    );
    /* Pair the failed report with a separately initialized nominal Runtime. */
    reset_fixture();
    CHECK(foc_boot_test_select(&fixture, NULL, NULL) == FOC_RUNTIME_OK);
    foc_serial_attach_boot(&fixture.serial, foc_board_boot_serial_readback, &failed.boot);
    CHECK(foc_boot_test_hello(&fixture) == 0);
    struct foc_boot_test_parameter parameter;
    CHECK(
        foc_boot_test_read(&fixture, PARAM_BOOT_CONFIGURATION, &parameter) ==
        FOC_PROTO_INVALID_ARGUMENT
    );
    CHECK(foc_boot_test_read(&fixture, PARAM_POLE_PAIRS, &parameter) == FOC_PROTO_OK);
    return 0;
}

/* Board ADC port -> Runtime -> decode with the consumed offsets. */
static int test_adc_port_consumes_offsets(void)
{
    reset_fixture();
    const struct foc_board_calibration_expectation expectation = expected();
    CHECK(foc_boot_test_select(&fixture, supply(), &expectation) == FOC_RUNTIME_OK);
    struct foc_board_adc adc = {0};
    struct foc_adc_test_clock clock = {0};
    const uint16_t ranks[] = {1241U, 2032U, 2063U, 2018U};
    CHECK(foc_board_adc_init_timing(&adc, 0U, 0U, 0U));
    struct foc_board_adc_event event = foc_adc_test_event(&clock, 40U);
    CHECK(foc_board_adc_begin(&fixture.runtime, &adc, &event, false));
    event = foc_adc_test_event(&clock, 48U);
    (void)foc_board_adc_complete(&fixture.runtime, &adc, &fixture.output, ranks, 4U, &event, true);
    CHECK(fixture.runtime.adc.seen && fixture.runtime.adc.seq == 0U);
    CHECK(adc.live.transaction.h0_called && !adc.live.transaction.physical_output_applied);

    const struct foc_device_adc_raw raw = {
        ranks[0],
        ranks[1],
        ranks[2],
        ranks[3],
        FOC_DEVICE_SAMPLE_ALL_VALID,
        0U,
        0U,
    };
    struct foc_device_measurement measured;
    CHECK(foc_device_decode_sample(&raw, &fixture.runtime.pipeline.adc_config, &measured) == 0);
    const float gain = fixture.runtime.pipeline.adc_config.current_a_per_count;
    CHECK(fabsf(measured.phase_current_a.a - 0.5f * gain) < 1.0e-6f);
    CHECK(fabsf(measured.phase_current_a.b - (-0.25f) * gain) < 1.0e-6f);
    CHECK(fabsf(measured.phase_current_a.c - 0.25f * gain) < 1.0e-6f);
    /* Nominal offsets would read these ranks as tens of milliamperes. */
    CHECK(fabsf((2032.0f - 2048.0f) * gain - measured.phase_current_a.a) > 0.01f);
    CHECK(fixture.output.declined_updates == 0U && fixture.runtime.pipeline.mock.updates == 0U);
    return 0;
}

/* Board Encoder port begin/complete -> Runtime sensor slot. */
static int encoder_phase(
    const struct foc_board_calibration_supply *candidate,
    const struct foc_board_calibration_expectation *expectation,
    struct foc_runtime_sensor *sample
)
{
    reset_fixture();
    CHECK(foc_boot_test_select(&fixture, candidate, expectation) == FOC_RUNTIME_OK);
    struct foc_board_adc adc = {0};
    /* The formal selection starts the Runtime epoch at 0, as main() does. */
    const uint32_t base = 0U;
    struct foc_adc_test_clock clock = {.epoch = base};
    const uint16_t ranks[] = {1241U, 2048U, 2048U, 2048U};
    CHECK(foc_board_adc_init_timing(&adc, base, base, 0U));
    CHECK(foc_board_encoder_begin(&fixture.encoder, base));
    CHECK(
        foc_board_encoder_complete(
            &fixture.runtime,
            &fixture.encoder,
            base + 70U,
            FOC_BOARD_ENCODER_OK,
            0U
        ) == FOC_RUNTIME_ERR_INPUT
    );
    for (uint32_t offset = 80U; offset < 500U; offset += FOC_BOARD_H0_US) {
        struct foc_board_adc_event event = foc_adc_test_event(&clock, offset);
        CHECK(foc_board_adc_begin(&fixture.runtime, &adc, &event, false));
        event = foc_adc_test_event(&clock, offset + 8U);
        (void)foc_board_adc_complete(
            &fixture.runtime,
            &adc,
            &fixture.output,
            ranks,
            4U,
            &event,
            true
        );
    }
    CHECK(foc_board_encoder_begin(&fixture.encoder, 500U));
    CHECK(
        foc_board_encoder_complete(
            &fixture.runtime,
            &fixture.encoder,
            570U,
            FOC_BOARD_ENCODER_OK,
            1U
        ) == FOC_RUNTIME_OK
    );
    *sample = fixture.runtime.sensor_slots[atomic_load(&fixture.runtime.sensor_slot)].sample;
    CHECK(sample->valid && sample->electrical_phase_valid);
    struct foc_encoder_config policy;
    foc_board_encoder_policy(&fixture.encoder, &policy);
    CHECK(fixture.encoder.sensor.config.direction == policy.direction);
    CHECK(fixture.output.declined_updates == 0U && fixture.runtime.pipeline.mock.updates == 0U);
    return 0;
}

static int test_encoder_port_consumes_direction_and_rotor(void)
{
    const struct foc_board_calibration_expectation expectation = expected();
    struct foc_runtime_sensor calibrated;
    struct foc_runtime_sensor nominal;
    CHECK(encoder_phase(supply(), &expectation, &calibrated) == 0);
    const float count_rad = TWO_PI / 65536.0f;
    const float offset = supply()->snapshot.electrical_offset_rad;
    CHECK(fabsf(calibrated.mechanical_rad - count_rad) < 1.0e-8f);
    CHECK(fabsf(calibrated.theta_elec_rad - wrap_2pi(11.0f * count_rad + offset)) < 1.0e-5f);

    /* Reverse validation: the same raw codes under the nominal policy move the other way. */
    CHECK(encoder_phase(NULL, NULL, &nominal) == 0);
    CHECK(fabsf(nominal.mechanical_rad + count_rad) < 1.0e-8f);
    CHECK(fabsf(nominal.theta_elec_rad - wrap_2pi(-7.0f * count_rad)) < 1.0e-5f);
    /* A direction error under the calibrated rotor would land here instead. */
    const float wrong_direction = wrap_2pi(-11.0f * count_rad + offset);
    CHECK(fabsf(calibrated.theta_elec_rad - wrong_direction) > 1.0e-3f);
    printf(
        "reverse check, raw codes 0->1: calibrated mech %+.8f rad theta %.7f; nominal mech %+.8f "
        "rad theta %.7f; calibrated rotor with wrong direction would give theta %.7f\n",
        (double)calibrated.mechanical_rad,
        (double)calibrated.theta_elec_rad,
        (double)nominal.mechanical_rad,
        (double)nominal.theta_elec_rad,
        (double)wrong_direction
    );
    return 0;
}

/* One Board-cadence cycle: Foreground, a fresh H1 sample, H0, then Foreground publication. */
static void boot_tick(struct foc_boot_test *test, uint32_t at)
{
    struct foc_runtime_foreground_result result;
    test->time = at;
    (void)foc_runtime_foreground_poll(&test->runtime, at, 0U, 0U, &result);
    struct foc_runtime_sensor sensor = {.captured_us = at, .seq = at, .valid = true};
    sensor.electrical_phase_valid = foc_rotor_phase_from_counts(
                                        &test->runtime.config.motion.rotor,
                                        0U,
                                        2U,
                                        &sensor.theta_elec_rad
                                    ) == FOC_STATUS_OK;
    (void)foc_runtime_h1_step(&test->runtime, &sensor, at, 0U);
    const struct foc_runtime_fast_input input = {
        .adc = {1241U, 2032U, 2063U, 2018U, FOC_DEVICE_SAMPLE_ALL_VALID, at, at},
        .now_us = at + 1U,
    };
    (void)foc_runtime_h0_step(&test->runtime, &input);
    (void)foc_runtime_foreground_poll(&test->runtime, at + 2U, 0U, 0U, &result);
}

static int check_locked_uart(struct foc_boot_test *test, bool calibrated)
{
    struct foc_protocol_frame response;
    struct foc_boot_test_parameter parameter;
    struct foc_board_boot_view view;
    CHECK(foc_board_boot_read(&test->boot, &view));
    CHECK(foc_boot_test_hello(test) == 0);

    CHECK(foc_boot_test_read(test, PARAM_BOOT_CONFIGURATION, &parameter) == FOC_PROTO_OK);
    CHECK(parameter.type == 1U && parameter.attributes == 2U);
    CHECK(
        parameter.value ==
        (calibrated ? FOC_BOARD_CALIBRATED_CONFIGURATION : FOC_BOARD_NOMINAL_CONFIGURATION)
    );
    CHECK(foc_boot_test_read(test, PARAM_CALIBRATION_INPUT, &parameter) == FOC_PROTO_OK);
    CHECK(parameter.value == (uint32_t)view.calibration);
    CHECK(foc_boot_test_read(test, PARAM_SUPPLY_ID, &parameter) == FOC_PROTO_OK);
    CHECK(parameter.value == view.supply_id);
    CHECK(foc_boot_test_read(test, PARAM_REVIEW_ID, &parameter) == FOC_PROTO_OK);
    CHECK(parameter.value == view.review_id);
    CHECK(parameter.value == (calibrated ? 0x01210001U : 0U));
    CHECK(parameter.type == 1U && parameter.attributes == 2U);
    const float offsets[] = {
        test->runtime.pipeline.adc_config.phase_a_offset_count,
        test->runtime.pipeline.adc_config.phase_b_offset_count,
        test->runtime.pipeline.adc_config.phase_c_offset_count,
    };
    for (uint16_t index = 0U; index < 3U; ++index) {
        CHECK(foc_boot_test_read(test, PARAM_PHASE_A_OFFSET + index, &parameter) == FOC_PROTO_OK);
        CHECK(parameter.type == 2U && parameter.unit == 4U && parameter.attributes == 6U);
        CHECK(parameter.value == foc_boot_test_bits(offsets[index]));
        CHECK(parameter.minimum == parameter.value && parameter.maximum == parameter.value);
    }
    CHECK(foc_boot_test_read(test, PARAM_POLE_PAIRS, &parameter) == FOC_PROTO_OK);
    CHECK(parameter.value == test->runtime.config.motion.rotor.pole_pairs);
    CHECK(foc_boot_test_read(test, PARAM_ELECTRICAL_OFFSET, &parameter) == FOC_PROTO_OK);
    CHECK(
        parameter.value ==
        foc_boot_test_bits(test->runtime.config.motion.rotor.electrical_offset_rad)
    );
    CHECK(foc_boot_test_read(test, PARAM_ENCODER_CPR, &parameter) == FOC_PROTO_OK);
    CHECK(parameter.value == 65536U);
    CHECK(foc_boot_test_read(test, PARAM_RUNTIME_DIRECTION, &parameter) == FOC_PROTO_OK);
    CHECK(parameter.value == (test->encoder.config_valid ? 1U : 2U));
    CHECK(parameter.value == (calibrated ? 1U : 2U));
    CHECK(foc_boot_test_read(test, PARAM_SOURCE_DIRECTION, &parameter) == FOC_PROTO_OK);
    CHECK(parameter.value == 2U);
    CHECK(foc_boot_test_read(test, PARAM_CALIBRATION_DIRECTION, &parameter) == FOC_PROTO_OK);
    CHECK(parameter.value == (calibrated ? 2U : 0U));
    CHECK(foc_boot_test_read(test, PARAM_MEASUREMENT_QUALIFIED, &parameter) == FOC_PROTO_OK);
    CHECK(parameter.value == 0U);
    CHECK(foc_boot_test_read(test, 46U, &parameter) == FOC_PROTO_INVALID_ARGUMENT);

    /* Read-only: the readback and controller IDs reject writes. */
    const uint8_t write_offset[] = {PARAM_PHASE_A_OFFSET, 0, 2, 0, 0, 0, 0x45};
    CHECK(foc_boot_test_call(test, FOC_MSG_PARAM_WRITE, write_offset, 7U, &response) == 13);
    const uint8_t write_poles[] = {PARAM_POLE_PAIRS, 0, 1, 7, 0, 0, 0};
    CHECK(foc_boot_test_call(test, FOC_MSG_PARAM_WRITE, write_poles, 7U, &response) == 13);
    CHECK(test->runtime.pipeline.adc_config.phase_a_offset_count == offsets[0]);

    /* Session and replay handling are unchanged. */
    uint8_t read_id[2];
    foc_protocol_write_u16(read_id, PARAM_BOOT_CONFIGURATION);
    struct foc_protocol_frame request = {
        .flags = FOC_PROTOCOL_REQUEST,
        .type = FOC_MSG_PARAM_READ,
        .session = test->session,
        .sequence = test->sequence,
        .length = 2U,
        .payload = {read_id[0], read_id[1]},
    };
    CHECK(foc_boot_test_exchange(test, &request, &response) == FOC_PROTO_REPLAY);
    request.session = test->session + 1U;
    request.sequence = test->sequence + 1U;
    CHECK(foc_boot_test_exchange(test, &request, &response) == FOC_PROTO_BAD_SESSION);

    /* Locked lifecycle and persistence policy. */
    CHECK(foc_boot_test_call(test, FOC_MSG_ARM, NULL, 0U, &response) == FOC_PROTO_OUTPUT_LOCKED);
    CHECK(foc_boot_test_call(test, FOC_MSG_START, NULL, 0U, &response) == FOC_PROTO_OUTPUT_LOCKED);
    CHECK(
        foc_boot_test_call(test, FOC_MSG_PARAM_SAVE, NULL, 0U, &response) == FOC_PROTO_NOT_SUPPORTED
    );
    CHECK(foc_boot_test_call(test, FOC_MSG_TELEMETRY_READ, NULL, 0U, &response) == FOC_PROTO_OK);
    /* M1 appended target d/q and tuning revision: all zero in the locked image. */
    CHECK(response.length == 83U && response.payload[62] == 0U);
    for (unsigned int index = 30U; index < 38U; ++index) {
        CHECK(response.payload[index] == 0U);
    }
    for (unsigned int index = 63U; index < 75U; ++index) {
        CHECK(response.payload[index] == 0U);
    }

    /* STOP is accepted by Foreground and consumed by H0. */
    CHECK(foc_boot_test_call(test, FOC_MSG_STOP, NULL, 0U, &response) == FOC_PROTO_ACCEPTED);
    CHECK(atomic_load(&test->runtime.stop_requests) != 0U);
    test->time = 40U;
    const struct foc_runtime_fast_input input = {
        .adc = {1241U, 2048U, 2048U, 2048U, FOC_DEVICE_SAMPLE_ALL_VALID, test->time, test->time},
        .now_us = test->time,
    };
    (void)foc_runtime_h0_step(&test->runtime, &input);
    CHECK(atomic_load(&test->runtime.stop_requests) == 0U);
    CHECK(test->output.result == FOC_BOARD_INHIBIT_SOFTWARE);
    /* Fault clear: only a lone command timeout qualifies; none exists while locked. */
    CHECK(
        foc_boot_test_call(test, FOC_MSG_FAULT_CLEAR, NULL, 0U, &response) == FOC_PROTO_BAD_STATE
    );
    CHECK(foc_boot_test_call(test, FOC_MSG_FAULT_READ, NULL, 0U, &response) == FOC_PROTO_OK);
    CHECK(response.payload[1] == FOC_SAFE_STATE_DISABLED && (response.payload[3] & 1U) != 0U);

    /* M1: mode selection is control state only, so the locked image accepts it.
     * The request waits for H0; its receipt is readable through LIFECYCLE_READ. */
    /* CURRENT is the active mode: the receipt path is exercised without side effects. */
    const uint8_t current_mode[] = {FOC_RUNTIME_MODE_CURRENT};
    const uint8_t bad_mode[] = {FOC_RUNTIME_MODE_POSITION + 1U};
    CHECK(
        foc_boot_test_call(test, FOC_MSG_SET_MODE, bad_mode, 1U, &response) ==
        FOC_PROTO_INVALID_ARGUMENT
    );
    CHECK(
        foc_boot_test_call(test, FOC_MSG_SET_MODE, current_mode, 1U, &response) ==
        FOC_PROTO_ACCEPTED
    );
    CHECK(response.length == 5U && foc_protocol_read_u32(response.payload + 1) != 0U);
    const uint32_t mode_request = foc_protocol_read_u32(response.payload + 1);
    CHECK(foc_boot_test_call(test, FOC_MSG_LIFECYCLE_READ, NULL, 0U, &response) == FOC_PROTO_OK);
    CHECK(response.length == 25U && response.payload[1] == FOC_RUNTIME_REQUEST_PENDING);
    CHECK(response.payload[2] == FOC_RUNTIME_ACTION_SET_MODE);
    CHECK(response.payload[3] == FOC_RUNTIME_MODE_CURRENT);
    CHECK(foc_protocol_read_u32(response.payload + 4) == mode_request);
    /* Controller tuning reads the live configuration; ID 74 is the command lease. */
    const uint8_t lease_id[] = {74U, 0U};
    CHECK(foc_boot_test_call(test, FOC_MSG_PARAM_READ, lease_id, 2U, &response) == FOC_PROTO_OK);
    CHECK(response.length == 18U && foc_protocol_read_u32(response.payload + 14) == 2500U);
    CHECK((response.payload[5] & 1U) != 0U);

    /* Fault request consumed by H0. The locked image cannot reach RUNNING, so the lone
     * COMMAND_TIMEOUT that qualifies for a clear is latched through the public Safety API;
     * the UART request, Runtime admission and H0 decision are the production code. */
    boot_tick(test, 200U);
    CHECK(
        foc_safe_latch_fault(&test->runtime.pipeline.safety, FOC_SAFE_FAULT_COMMAND_TIMEOUT) ==
        FOC_SAFE_STATUS_OK
    );
    boot_tick(test, 400U);
    CHECK(test->runtime.pipeline.safety.state == FOC_SAFE_STATE_FAULT_LATCHED);
    CHECK(test->runtime.pipeline.safety.latched_faults == FOC_SAFE_FAULT_COMMAND_TIMEOUT);
    CHECK(test->output.result == FOC_BOARD_INHIBIT_SOFTWARE);
    test->time = 410U;
    CHECK(foc_boot_test_call(test, FOC_MSG_FAULT_CLEAR, NULL, 0U, &response) == FOC_PROTO_ACCEPTED);
    /* Foreground only records the request; Safety is still latched until H0 decides. */
    CHECK(atomic_load(&test->runtime.clear_result) == FOC_RUNTIME_CLEAR_PENDING);
    CHECK(test->runtime.pipeline.safety.state == FOC_SAFE_STATE_FAULT_LATCHED);
    boot_tick(test, 600U);
    boot_tick(test, 800U);
    CHECK(atomic_load(&test->runtime.clear_result) == FOC_RUNTIME_CLEAR_APPLIED);
    CHECK(test->runtime.pipeline.safety.state == FOC_SAFE_STATE_DISABLED);
    CHECK(test->runtime.pipeline.safety.latched_faults == FOC_SAFE_FAULT_NONE);
    test->time = 810U;
    CHECK(foc_boot_test_call(test, FOC_MSG_FAULT_READ, NULL, 0U, &response) == FOC_PROTO_OK);
    CHECK(response.payload[1] == FOC_SAFE_STATE_DISABLED);
    CHECK(response.payload[17] == FOC_RUNTIME_CLEAR_APPLIED);
    CHECK(test->output.result == FOC_BOARD_INHIBIT_SOFTWARE);
    printf(
        "%s boot: FAULT_CLEAR ACCEPTED -> PENDING (Safety latched) -> H0 APPLIED -> DISABLED\n",
        calibrated ? "calibrated" : "nominal"
    );

    /* Normal chain produced no UPDATE; the adapter still refuses a legal one directly. */
    CHECK(test->output.declined_updates == 0U && test->runtime.pipeline.mock.updates == 0U);
    const struct foc_output duties = {
        .output_enable = 1U,
        .svpwm = {.duty_a = 0.4f, .duty_b = 0.5f, .duty_c = 0.6f},
    };
    struct foc_device_pwm_request update;
    CHECK(foc_device_map_output(&duties, &test->runtime.pipeline.pwm_config, &update) == 0);
    CHECK(update.action == FOC_DEVICE_OUTPUT_UPDATE);
    CHECK(!foc_board_output_submit(&test->output, &update));
    CHECK(
        test->output.result == FOC_BOARD_UPDATE_NOT_APPLIED && test->output.declined_updates == 1U
    );
    return 0;
}

static int test_uart_readback_matches_consumption(void)
{
    const struct foc_board_calibration_expectation expectation = expected();
    reset_fixture();
    CHECK(foc_boot_test_select(&fixture, supply(), &expectation) == FOC_RUNTIME_OK);
    CHECK(check_locked_uart(&fixture, true) == 0);
    reset_fixture();
    CHECK(foc_boot_test_select(&fixture, NULL, NULL) == FOC_RUNTIME_OK);
    CHECK(fixture.boot.calibration == FOC_BOARD_CALIBRATION_NOT_AVAILABLE);
    CHECK(check_locked_uart(&fixture, false) == 0);
    return 0;
}

/*
 * Why the fault-clear case injects its timeout: with a fresh UART setpoint and Safety
 * forced to RUNNING through its public lifecycle API, the first H0 computes a valid
 * UPDATE, the locked Board adapter declines it, and the Pipeline leaves RUNNING without a
 * latched fault. Command age therefore never accrues, so a real lone COMMAND_TIMEOUT
 * cannot arise in this image.
 */
static int test_locked_running_cannot_persist(void)
{
    const struct foc_board_calibration_expectation expectation = expected();
    reset_fixture();
    CHECK(foc_boot_test_select(&fixture, supply(), &expectation) == FOC_RUNTIME_OK);
    CHECK(foc_boot_test_hello(&fixture) == 0);
    boot_tick(&fixture, 0U);
    boot_tick(&fixture, 200U);
    const uint8_t setpoint[9] = {FOC_RUNTIME_MODE_CURRENT};
    struct foc_protocol_frame response;
    fixture.time = 300U;
    CHECK(
        foc_boot_test_call(&fixture, FOC_MSG_SETPOINT, setpoint, 9U, &response) ==
        FOC_PROTO_ACCEPTED
    );
    boot_tick(&fixture, 400U);
    CHECK(foc_safe_arm(&fixture.runtime.pipeline.safety) == FOC_SAFE_STATUS_OK);
    CHECK(foc_safe_start(&fixture.runtime.pipeline.safety) == FOC_SAFE_STATUS_OK);
    CHECK(fixture.runtime.pipeline.safety.state == FOC_SAFE_STATE_RUNNING);
    boot_tick(&fixture, 600U);
    CHECK(fixture.output.declined_updates == 1U);
    CHECK(fixture.runtime.pipeline.safety.state == FOC_SAFE_STATE_DISABLED);
    for (uint32_t at = 800U; at <= 4000U; at += FOC_BOARD_H0_US) {
        boot_tick(&fixture, at);
    }
    /* 3.4 ms past the command timeout of 2.5 ms: still no timeout and no further UPDATE. */
    CHECK(fixture.runtime.pipeline.safety.state == FOC_SAFE_STATE_DISABLED);
    CHECK(fixture.runtime.pipeline.safety.latched_faults == FOC_SAFE_FAULT_NONE);
    CHECK(fixture.output.declined_updates == 1U && fixture.runtime.pipeline.mock.updates == 0U);
    CHECK(fixture.output.result == FOC_BOARD_INHIBIT_SOFTWARE);
    printf(
        "locked RUNNING: 1 UPDATE declined, back to DISABLED, no COMMAND_TIMEOUT after 3.4 ms\n"
    );
    return 0;
}

/* Test Entry ------------------------------------------------------------------- */

int main(void)
{
    if (test_verification_supply_is_accepted() || test_audit_chain() ||
        test_rejections_fall_back_to_nominal() || test_explicit_entry_checks_direction_product() ||
        test_initialization_failures() || test_unready_report_has_no_readback() ||
        test_adc_port_consumes_offsets() || test_encoder_port_consumes_direction_and_rotor() ||
        test_uart_readback_matches_consumption() || test_locked_running_cannot_persist()) {
        return 1;
    }
    printf("test-foc-board-boot: all cases passed\n");
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
