/**
 * @file    foc_board_boot.c
 * @brief   Board1 startup qualification of a read-only calibration supply.
 * @version v0.1.0
 * @date    2026.11.21
 *
 * Implementation role
 * -------------------
 * Recompute the admission from the supplied candidates, apply Board facts and
 * independent expectations, then commit exactly one complete configuration.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_board_boot.h"

#include <math.h>
#include <string.h>

/* Private Macros --------------------------------------------------------------- */

#define FOC_BOARD_BOOT_TWO_PI 6.28318530717958647692f

/* Private Constants ------------------------------------------------------------ */

static const struct foc_board_boot_ops foc_board_boot_default_ops = {
    .calibrated = foc_board_init_with_calibration,
    .nominal = foc_board_init,
};

/* Private Helpers -------------------------------------------------------------- */

static bool foc_board_boot_same_float(float left, float right)
{
    uint32_t left_bits;
    uint32_t right_bits;
    memcpy(&left_bits, &left, sizeof left_bits);
    memcpy(&right_bits, &right, sizeof right_bits);
    return left_bits == right_bits;
}

static bool foc_board_boot_same_identity(
    const struct foc_calibration_identity *left,
    const struct foc_calibration_identity *right
)
{
    return left->adc_full_scale_counts == right->adc_full_scale_counts &&
        foc_board_boot_same_float(left->current_a_per_count, right->current_a_per_count) &&
        left->encoder_counts_per_revolution == right->encoder_counts_per_revolution &&
        left->encoder_direction == right->encoder_direction &&
        left->phase_order == right->phase_order && left->pole_pairs == right->pole_pairs;
}

/* Bit-exact field comparison; struct padding is never compared. */
static bool foc_board_boot_same_snapshot(
    const struct foc_calibration_snapshot *left,
    const struct foc_calibration_snapshot *right
)
{
    return foc_board_boot_same_identity(&left->source_identity, &right->source_identity) &&
        foc_board_boot_same_float(left->phase_a_offset_count, right->phase_a_offset_count) &&
        foc_board_boot_same_float(left->phase_b_offset_count, right->phase_b_offset_count) &&
        foc_board_boot_same_float(left->phase_c_offset_count, right->phase_c_offset_count) &&
        left->calibration_direction == right->calibration_direction &&
        left->runtime_encoder_direction == right->runtime_encoder_direction &&
        left->pole_pairs == right->pole_pairs &&
        foc_board_boot_same_float(left->electrical_offset_rad, right->electrical_offset_rad) &&
        left->accepted == right->accepted;
}

static bool foc_board_boot_offset_qualified(float offset)
{
    return isfinite(offset) && offset >= (float)FOC_BOARD_OFFSET_MIN_COUNTS &&
        offset <= (float)FOC_BOARD_OFFSET_MAX_COUNTS;
}

/* Discard every trace of a rejected or failed candidate before the next step. */
static void foc_board_boot_scrub(
    struct foc_runtime *runtime,
    struct foc_board_output *output,
    struct foc_board_encoder *encoder
)
{
    memset(runtime, 0, sizeof *runtime);
    memset(output, 0, sizeof *output);
    memset(encoder, 0, sizeof *encoder);
}

static void foc_board_boot_bind(
    struct foc_board_boot *boot,
    enum foc_board_effective_configuration configuration,
    const struct foc_runtime *runtime,
    const struct foc_board_encoder *encoder
)
{
    boot->configuration = configuration;
    boot->runtime = runtime;
    boot->encoder = encoder;
    boot->runtime_ready = true;
}

/* Public Functions ------------------------------------------------------------- */

enum foc_board_calibration_rejection foc_board_boot_qualify(
    const struct foc_board_calibration_supply *supply,
    const struct foc_board_calibration_expectation *expected,
    struct foc_calibration_snapshot *snapshot,
    enum foc_calibration_admission_failure *admission_failure
)
{
    if (admission_failure != NULL) {
        *admission_failure = FOC_CALIBRATION_ADMISSION_NONE;
    }
    if (supply == NULL || supply->format != FOC_BOARD_CALIBRATION_SUPPLY_FORMAT) {
        return FOC_BOARD_REJECTION_FORMAT;
    }
    if (expected == NULL || supply->supply_id == 0U || supply->supply_id != expected->supply_id) {
        return FOC_BOARD_REJECTION_SUPPLY_IDENTITY;
    }
    /* Presence only: review_id is a reference, not independently matched. */
    if (supply->review_id == 0U) {
        return FOC_BOARD_REJECTION_REVIEW_REFERENCE;
    }
    if (supply->coordinate != FOC_BOARD_COORDINATE_RAW_NO_LUT) {
        return FOC_BOARD_REJECTION_COORDINATE;
    }
    if (expected->pole_pairs == 0U || expected->pole_pairs > FOC_BOARD_POLE_PAIRS_MAX) {
        return FOC_BOARD_REJECTION_BOARD_IDENTITY;
    }

    /* Expected identity comes from Board facts and the independent expectation. */
    struct foc_calibration_identity board_identity;
    foc_board_calibration_identity(expected->pole_pairs, &board_identity);
    const struct foc_calibration_admission_input input = {
        .adc = &supply->adc,
        .rotor = &supply->rotor,
        .offset = &supply->offset,
        .expected_identity = &board_identity,
        .limits = supply->limits,
    };
    struct foc_calibration_snapshot recomputed;
    enum foc_calibration_admission_failure failure = FOC_CALIBRATION_ADMISSION_NONE;
    if (foc_calibration_admit(&input, &recomputed, &failure) != FOC_CALIBRATION_ADMISSION_OK) {
        if (admission_failure != NULL) {
            *admission_failure = failure;
        }
        return FOC_BOARD_REJECTION_ADMISSION;
    }
    if (!foc_board_boot_same_snapshot(&recomputed, &supply->snapshot)) {
        return FOC_BOARD_REJECTION_SNAPSHOT_MISMATCH;
    }
    if (!foc_calibration_identity_matches(&board_identity, &recomputed.source_identity)) {
        return FOC_BOARD_REJECTION_BOARD_IDENTITY;
    }
    if (recomputed.runtime_encoder_direction !=
        board_identity.encoder_direction * recomputed.calibration_direction) {
        return FOC_BOARD_REJECTION_DIRECTION_PRODUCT;
    }
    if (!foc_calibration_snapshot_valid(&recomputed) ||
        !foc_board_boot_offset_qualified(recomputed.phase_a_offset_count) ||
        !foc_board_boot_offset_qualified(recomputed.phase_b_offset_count) ||
        !foc_board_boot_offset_qualified(recomputed.phase_c_offset_count) ||
        recomputed.pole_pairs > FOC_BOARD_POLE_PAIRS_MAX ||
        !(recomputed.electrical_offset_rad >= 0.0f &&
          recomputed.electrical_offset_rad < FOC_BOARD_BOOT_TWO_PI)) {
        return FOC_BOARD_REJECTION_BOARD_RANGE;
    }
    if (snapshot != NULL) {
        *snapshot = recomputed;
    }
    return FOC_BOARD_REJECTION_NONE;
}

int foc_board_boot_select_with(
    struct foc_board_boot *boot,
    struct foc_runtime *runtime,
    struct foc_board_output *output,
    struct foc_board_encoder *encoder,
    const struct foc_board_calibration_supply *supply,
    const struct foc_board_calibration_expectation *expected,
    uint32_t now_us,
    const struct foc_board_boot_ops *ops
)
{
    if (boot == NULL) {
        return FOC_RUNTIME_ERR_INPUT;
    }
    *boot = (struct foc_board_boot){0};
    if (runtime == NULL || output == NULL || encoder == NULL || ops == NULL ||
        ops->calibrated == NULL || ops->nominal == NULL) {
        return FOC_RUNTIME_ERR_INPUT;
    }
    /* A started Sensor would keep continuity state from another configuration. */
    if (encoder->sensor.initialized || encoder->config_valid) {
        return FOC_RUNTIME_ERR_CONFIG;
    }

    if (supply == NULL) {
        boot->calibration = FOC_BOARD_CALIBRATION_NOT_AVAILABLE;
    } else {
        boot->supply_id = supply->supply_id;
        boot->review_id = supply->review_id;
        struct foc_calibration_snapshot snapshot;
        boot->rejection =
            foc_board_boot_qualify(supply, expected, &snapshot, &boot->admission_failure);
        if (boot->rejection == FOC_BOARD_REJECTION_NONE) {
            boot->calibrated_status = ops->calibrated(runtime, output, encoder, &snapshot, now_us);
            if (boot->calibrated_status == FOC_RUNTIME_OK) {
                boot->calibration = FOC_BOARD_CALIBRATION_ACCEPTED;
                boot->snapshot = snapshot;
                foc_board_boot_bind(boot, FOC_BOARD_CALIBRATED_CONFIGURATION, runtime, encoder);
                return FOC_RUNTIME_OK;
            }
            boot->rejection = FOC_BOARD_REJECTION_INITIALIZATION;
        }
        boot->calibration = FOC_BOARD_CALIBRATION_REJECTED;
        foc_board_boot_scrub(runtime, output, encoder);
    }

    boot->nominal_status = ops->nominal(runtime, output, now_us);
    if (boot->nominal_status == FOC_RUNTIME_OK) {
        foc_board_boot_bind(boot, FOC_BOARD_NOMINAL_CONFIGURATION, runtime, encoder);
        return FOC_RUNTIME_OK;
    }
    foc_board_boot_scrub(runtime, output, encoder);
    return boot->nominal_status;
}

int foc_board_boot_select(
    struct foc_board_boot *boot,
    struct foc_runtime *runtime,
    struct foc_board_output *output,
    struct foc_board_encoder *encoder,
    const struct foc_board_calibration_supply *supply,
    const struct foc_board_calibration_expectation *expected,
    uint32_t now_us
)
{
    return foc_board_boot_select_with(
        boot,
        runtime,
        output,
        encoder,
        supply,
        expected,
        now_us,
        &foc_board_boot_default_ops
    );
}

bool foc_board_boot_read(const struct foc_board_boot *boot, struct foc_board_boot_view *view)
{
    if (boot == NULL || view == NULL || !boot->runtime_ready || boot->runtime == NULL ||
        boot->encoder == NULL) {
        return false;
    }
    struct foc_encoder_config policy;
    struct foc_calibration_identity board_identity;
    foc_board_encoder_policy(boot->encoder, &policy);
    foc_board_calibration_identity(0U, &board_identity);
    const struct foc_device_adc_config *adc = &boot->runtime->pipeline.adc_config;
    const bool calibrated = boot->configuration == FOC_BOARD_CALIBRATED_CONFIGURATION;
    *view = (struct foc_board_boot_view){
        .configuration = boot->configuration,
        .calibration = boot->calibration,
        .rejection = boot->rejection,
        .admission_failure = boot->admission_failure,
        .supply_id = boot->supply_id,
        .review_id = boot->review_id,
        .phase_a_offset_count = adc->phase_a_offset_count,
        .phase_b_offset_count = adc->phase_b_offset_count,
        .phase_c_offset_count = adc->phase_c_offset_count,
        .pole_pairs = boot->runtime->config.motion.rotor.pole_pairs,
        .electrical_offset_rad = boot->runtime->config.motion.rotor.electrical_offset_rad,
        .encoder_counts_per_revolution = policy.counts_per_revolution,
        .runtime_encoder_direction = policy.direction,
        .source_encoder_direction = board_identity.encoder_direction,
        .calibration_direction = calibrated ? boot->snapshot.calibration_direction : 0,
        .current_measurement_qualified = false,
    };
    return true;
}

bool foc_board_boot_capture_profile(
    const struct foc_board_boot *boot,
    uint32_t build_tag,
    struct foc_capture_profile *profile
)
{
    struct foc_board_boot_view view;
    if (profile == NULL || !foc_board_boot_read(boot, &view)) {
        return false;
    }
    *profile = (struct foc_capture_profile){
        .build_tag = build_tag,
        .encoder_cpr = view.encoder_counts_per_revolution,
        .encoder_direction = view.runtime_encoder_direction,
        .pole_pairs = view.pole_pairs,
        .phase_order = (uint32_t)boot->runtime->pipeline.adc_config.phase_order,
        .configuration = view.configuration == FOC_BOARD_CALIBRATED_CONFIGURATION
            ? FOC_CAPTURE_CONFIGURATION_CALIBRATED
            : FOC_CAPTURE_CONFIGURATION_NOMINAL,
    };
    return true;
}

bool foc_board_boot_serial_readback(const void *boot, struct foc_serial_boot_readback *readback)
{
    struct foc_board_boot_view view;
    if (readback == NULL || !foc_board_boot_read(boot, &view)) {
        return false;
    }
    *readback = (struct foc_serial_boot_readback){
        .configuration = (uint32_t)view.configuration,
        .calibration = (uint32_t)view.calibration,
        .rejection = (uint32_t)view.rejection,
        .admission_failure = (uint32_t)view.admission_failure,
        .supply_id = view.supply_id,
        .review_id = view.review_id,
        .phase_offset_count =
            {view.phase_a_offset_count, view.phase_b_offset_count, view.phase_c_offset_count},
        .encoder_counts_per_revolution = view.encoder_counts_per_revolution,
        .runtime_direction = view.runtime_encoder_direction,
        .source_direction = view.source_encoder_direction,
        .calibration_direction = view.calibration_direction,
        .current_measurement_qualified = view.current_measurement_qualified,
    };
    return true;
}

/* ---------------------------------------- end of file ----------------------------------------- */
