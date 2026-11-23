/**
 * @file    foc_board_store.c
 * @brief   Board1 persistent settings record, two-slot transaction and boot restore.
 * @version v0.1.0
 * @date    2026.11.22
 *
 * Implementation role
 * -------------------
 * Record "FBP1", format 3, 212 bytes, little-endian. Format 2 is bytes 0–135 with
 * its CRC at 136; format 1 is bytes 0–103 with its CRC at 104 and no calibration.
 * Both still decode, keeping the boot profile and protection thresholds:
 *
 *   0  commit word 0xC35A4C7E (erased 0xFFFFFFFF), programmed last
 *   4  magic "FBP1"          8  format u16 = 3     10 length u16 = 212
 *   12 sequence u32
 *   16 configuration u32     20 pole pairs u32     24 encoder CPR u32
 *   28 runtime encoder direction i32   32 electrical offset f32
 *   36/40/44 phase A/B/C ADC offset f32
 *   48/52 current Kp d/q     56/60 current Ki d/q  64 current-vector limit
 *   68/72 velocity Kp/Ki     76 position Kp        80 speed reference limit
 *   84 q-current limit (all f32)                   88 command lease u32 µs
 *   92 UART RX budget u32    96 remote current cap f32   100 remote position cap f32
 *   104 flags u32 (bit 0: calibration present)
 *   108/112/116 calibrated phase A/B/C ADC offset f32   120 pole pairs u32
 *   124 electrical offset f32   128 calibration direction i32   132 Runtime direction i32
 *   136/140/144/148 Rs Ω, Ld H, Lq H, flux Wb   152/156 inertia, damping
 *   160/164/168 rated A, peak A, max speed rad/s   172 gear ratio   176 bandwidth Hz
 *   180/184/188/192 trips: over-current A, over/under-voltage V, over-speed rad/s
 *   196/200/204 sustained, stall, divergence windows u32 ms
 *   208 CRC-32 (IEEE, reflected) of bytes 4–207
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_board_store.h"

#include <math.h>
#include <string.h>

/* Private Macros --------------------------------------------------------------- */

#define STORE_COMMIT    UINT32_C(0xC35A4C7E)
#define STORE_MAGIC     UINT32_C(0x31504246) /* "FBP1" */
#define STORE_FORMAT    3U
#define STORE_CRC_AT    208U
#define STORE_V2_BYTES  140U
#define STORE_V2_CRC_AT 136U
#define STORE_V1_BYTES  108U
#define STORE_V1_CRC_AT 104U
#define STORE_FLAG_CAL  1U
#define STORE_HALF      UINT32_C(0x80000000)

_Static_assert(FOC_BOARD_STORE_RECORD_BYTES == STORE_CRC_AT + 4U, "record layout");
_Static_assert(FOC_BOARD_STORE_RECORD_BYTES % FOC_BOARD_STORE_PROGRAM_BYTES == 0, "program unit");
_Static_assert(FOC_BOARD_STORE_RECORD_BYTES <= FOC_BOARD_STORE_SLOT_BYTES, "slot size");

/* Private Helpers -------------------------------------------------------------- */

static uint32_t store_crc32(const uint8_t *bytes, size_t size)
{
    uint32_t crc = UINT32_MAX;
    for (size_t index = 0U; index < size; ++index) {
        crc ^= bytes[index];
        for (unsigned int bit = 0U; bit < 8U; ++bit) {
            crc = (crc >> 1) ^ (UINT32_C(0xEDB88320) & (0U - (crc & 1U)));
        }
    }
    return crc ^ UINT32_MAX;
}

static void put_u32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put_f32(uint8_t *p, float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    put_u32(p, bits);
}

static float get_f32(const uint8_t *p)
{
    const uint32_t bits = get_u32(p);
    float value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

static bool erased(const uint8_t *bytes, size_t size)
{
    for (size_t index = 0U; index < size; ++index) {
        if (bytes[index] != 0xFFU) {
            return false;
        }
    }
    return true;
}

static bool
identity_equal(const struct foc_board_store_identity *a, const struct foc_board_store_identity *b)
{
    /* Bit equality: a recalibrated offset is a different configuration. */
    uint8_t left[36];
    uint8_t right[36];
    const struct foc_board_store_identity *pair[] = {a, b};
    uint8_t *out[] = {left, right};
    for (unsigned int side = 0U; side < 2U; ++side) {
        const struct foc_board_store_identity *id = pair[side];
        put_u32(out[side], id->configuration);
        put_u32(out[side] + 4, id->pole_pairs);
        put_u32(out[side] + 8, id->encoder_counts_per_revolution);
        put_u32(out[side] + 12, (uint32_t)id->runtime_encoder_direction);
        put_f32(out[side] + 16, id->electrical_offset_rad);
        put_f32(out[side] + 20, id->phase_offset_count[0]);
        put_f32(out[side] + 24, id->phase_offset_count[1]);
        put_f32(out[side] + 28, id->phase_offset_count[2]);
        put_u32(out[side] + 32, 0U);
    }
    return memcmp(left, right, sizeof left) == 0;
}

/* Read one slot. Returns OK with its decoded record, ERR_EMPTY when erased. */
static int read_slot(
    const struct foc_board_store_driver *driver,
    unsigned int slot,
    struct foc_board_store_identity *identity,
    struct foc_board_settings *settings,
    uint32_t *sequence
)
{
    uint8_t record[FOC_BOARD_STORE_RECORD_BYTES];
    if (driver->read(driver->context, slot, 0U, record, sizeof record) != 0) {
        return FOC_BOARD_STORE_ERR_IO;
    }
    if (erased(record, sizeof record)) {
        return FOC_BOARD_STORE_ERR_EMPTY;
    }
    return foc_board_store_decode(record, identity, settings, sequence);
}

/* Newest committed slot, or NONE (2). Erased/invalid outcomes are reported per slot. */
static int scan(
    const struct foc_board_store_driver *driver,
    struct foc_board_store_identity *identity,
    struct foc_board_settings *settings,
    uint32_t *sequence,
    unsigned int *slot
)
{
    struct foc_board_store_identity ids[2];
    struct foc_board_settings values[2];
    uint32_t sequences[2] = {0U, 0U};
    int status[2];
    for (unsigned int index = 0U; index < 2U; ++index) {
        status[index] = read_slot(driver, index, &ids[index], &values[index], &sequences[index]);
        if (status[index] == FOC_BOARD_STORE_ERR_IO) {
            return FOC_BOARD_STORE_ERR_IO;
        }
    }
    unsigned int chosen = 2U;
    if (status[0] == FOC_BOARD_STORE_OK && status[1] == FOC_BOARD_STORE_OK) {
        const uint32_t delta = sequences[1] - sequences[0];
        chosen = delta != 0U && delta < STORE_HALF ? 1U : 0U;
    } else if (status[0] == FOC_BOARD_STORE_OK) {
        chosen = 0U;
    } else if (status[1] == FOC_BOARD_STORE_OK) {
        chosen = 1U;
    }
    if (chosen == 2U) {
        *slot = 2U;
        return status[0] == FOC_BOARD_STORE_ERR_EMPTY && status[1] == FOC_BOARD_STORE_ERR_EMPTY
            ? FOC_BOARD_STORE_ERR_EMPTY
            : FOC_BOARD_STORE_ERR_RECORD;
    }
    *identity = ids[chosen];
    *settings = values[chosen];
    *sequence = sequences[chosen];
    *slot = chosen;
    return FOC_BOARD_STORE_OK;
}

static bool settings_from_live(
    struct foc_runtime *runtime,
    const struct foc_serial *serial,
    struct foc_board_settings *settings
)
{
    struct foc_runtime_tuning_result before;
    struct foc_runtime_tuning_result after;
    /* The same torn-copy guard as a UART tuning write. */
    if (foc_runtime_read_tuning_result(runtime, &before) != FOC_RUNTIME_OK ||
        before.result == FOC_RUNTIME_REQUEST_PENDING ||
        foc_runtime_read_tuning(runtime, &settings->tuning) != FOC_RUNTIME_OK ||
        foc_runtime_read_tuning_result(runtime, &after) != FOC_RUNTIME_OK ||
        after.result == FOC_RUNTIME_REQUEST_PENDING || after.revision != before.revision) {
        return false;
    }
    settings->rx_budget = serial->rx_budget;
    settings->remote_current_cap_a = serial->current_limit_a;
    settings->remote_position_cap_rad = serial->position_limit_rad;
    return true;
}

/* The UART RAM caps use the same ranges as their PARAM_WRITE entries (IDs 1–3). */
static bool serial_caps_valid(const struct foc_board_settings *settings)
{
    return settings->rx_budget >= 8U && settings->rx_budget <= 32U &&
        isfinite(settings->remote_current_cap_a) && settings->remote_current_cap_a >= 0.05f &&
        settings->remote_current_cap_a <= settings->tuning.iq_limit_a &&
        isfinite(settings->remote_position_cap_rad) && settings->remote_position_cap_rad >= 1.0f &&
        settings->remote_position_cap_rad <= 1000.0f;
}

/* Public Functions ------------------------------------------------------------- */

bool foc_board_store_identity_from_boot(
    const struct foc_board_boot *boot,
    struct foc_board_store_identity *identity
)
{
    struct foc_board_boot_view view;
    if (identity == NULL || !foc_board_boot_read(boot, &view)) {
        return false;
    }
    *identity = (struct foc_board_store_identity){
        .configuration = (uint32_t)view.configuration,
        .pole_pairs = view.pole_pairs,
        .encoder_counts_per_revolution = view.encoder_counts_per_revolution,
        .runtime_encoder_direction = view.runtime_encoder_direction,
        .electrical_offset_rad = view.electrical_offset_rad,
        .phase_offset_count =
            {view.phase_a_offset_count, view.phase_b_offset_count, view.phase_c_offset_count},
    };
    return true;
}

void foc_board_store_encode(
    const struct foc_board_store_identity *identity,
    const struct foc_board_settings *settings,
    uint32_t sequence,
    uint8_t record[FOC_BOARD_STORE_RECORD_BYTES]
)
{
    const struct foc_runtime_tuning *t = &settings->tuning;
    memset(record, 0, FOC_BOARD_STORE_RECORD_BYTES);
    put_u32(record, UINT32_MAX);
    put_u32(record + 4, STORE_MAGIC);
    record[8] = (uint8_t)STORE_FORMAT;
    record[10] = (uint8_t)FOC_BOARD_STORE_RECORD_BYTES;
    put_u32(record + 12, sequence);
    put_u32(record + 16, identity->configuration);
    put_u32(record + 20, identity->pole_pairs);
    put_u32(record + 24, identity->encoder_counts_per_revolution);
    put_u32(record + 28, (uint32_t)identity->runtime_encoder_direction);
    put_f32(record + 32, identity->electrical_offset_rad);
    put_f32(record + 36, identity->phase_offset_count[0]);
    put_f32(record + 40, identity->phase_offset_count[1]);
    put_f32(record + 44, identity->phase_offset_count[2]);
    put_f32(record + 48, t->current_kp_v_per_a.d);
    put_f32(record + 52, t->current_kp_v_per_a.q);
    put_f32(record + 56, t->current_ki_v_per_a_s.d);
    put_f32(record + 60, t->current_ki_v_per_a_s.q);
    put_f32(record + 64, t->current_limit_a);
    put_f32(record + 68, t->velocity_kp_a_per_rad_s);
    put_f32(record + 72, t->velocity_ki_a_per_rad);
    put_f32(record + 76, t->position_kp_per_s);
    put_f32(record + 80, t->velocity_limit_rad_s);
    put_f32(record + 84, t->iq_limit_a);
    put_u32(record + 88, t->command_timeout_us);
    put_u32(record + 92, settings->rx_budget);
    put_f32(record + 96, settings->remote_current_cap_a);
    put_f32(record + 100, settings->remote_position_cap_rad);
    const struct foc_calibration_snapshot *cal = &settings->calibration;
    if (settings->calibration_present) {
        put_u32(record + 104, STORE_FLAG_CAL);
        put_f32(record + 108, cal->phase_a_offset_count);
        put_f32(record + 112, cal->phase_b_offset_count);
        put_f32(record + 116, cal->phase_c_offset_count);
        put_u32(record + 120, cal->pole_pairs);
        put_f32(record + 124, cal->electrical_offset_rad);
        put_u32(record + 128, (uint32_t)cal->calibration_direction);
        put_u32(record + 132, (uint32_t)cal->runtime_encoder_direction);
    }
    const struct foc_runtime_profile *profile = &t->profile;
    const float profile_values[] = {
        profile->rs_ohm,
        profile->ld_h,
        profile->lq_h,
        profile->flux_wb,
        profile->inertia_kg_m2,
        profile->damping_nm_s_per_rad,
        profile->rated_current_a,
        profile->peak_current_a,
        profile->max_speed_rad_s,
        profile->gear_ratio,
        profile->current_bandwidth_hz,
    };
    for (size_t index = 0U; index < sizeof profile_values / sizeof profile_values[0]; ++index) {
        put_f32(record + 136 + 4U * index, profile_values[index]);
    }
    put_f32(record + 180, t->trips.overcurrent_a);
    put_f32(record + 184, t->trips.overvoltage_v);
    put_f32(record + 188, t->trips.undervoltage_v);
    put_f32(record + 192, t->trips.overspeed_rad_s);
    put_u32(record + 196, t->trips.sustained_ms);
    put_u32(record + 200, t->trips.stall_ms);
    put_u32(record + 204, t->trips.divergence_ms);
    put_u32(record + STORE_CRC_AT, store_crc32(record + 4, STORE_CRC_AT - 4U));
}

int foc_board_store_decode(
    const uint8_t record[FOC_BOARD_STORE_RECORD_BYTES],
    struct foc_board_store_identity *identity,
    struct foc_board_settings *settings,
    uint32_t *sequence
)
{
    const unsigned int format = record[8] | (record[9] << 8);
    const unsigned int length = record[10] | (record[11] << 8);
    const bool v1 = format == 1U && length == STORE_V1_BYTES;
    const bool v2 = format == 2U && length == STORE_V2_BYTES;
    const bool v3 = format == STORE_FORMAT && length == FOC_BOARD_STORE_RECORD_BYTES;
    const size_t crc_at = v1 ? STORE_V1_CRC_AT : v2 ? STORE_V2_CRC_AT : STORE_CRC_AT;
    if (get_u32(record) != STORE_COMMIT || get_u32(record + 4) != STORE_MAGIC ||
        !(v1 || v2 || v3) || get_u32(record + crc_at) != store_crc32(record + 4, crc_at - 4U) ||
        (!v1 && (get_u32(record + 104) & ~STORE_FLAG_CAL) != 0U)) {
        return FOC_BOARD_STORE_ERR_RECORD;
    }
    *sequence = get_u32(record + 12);
    *identity = (struct foc_board_store_identity){
        .configuration = get_u32(record + 16),
        .pole_pairs = get_u32(record + 20),
        .encoder_counts_per_revolution = get_u32(record + 24),
        .runtime_encoder_direction = (int32_t)get_u32(record + 28),
        .electrical_offset_rad = get_f32(record + 32),
        .phase_offset_count = {get_f32(record + 36), get_f32(record + 40), get_f32(record + 44)},
    };
    *settings = (struct foc_board_settings){
        .tuning =
            {
                .current_kp_v_per_a = {get_f32(record + 48), get_f32(record + 52)},
                .current_ki_v_per_a_s = {get_f32(record + 56), get_f32(record + 60)},
                .current_limit_a = get_f32(record + 64),
                .velocity_kp_a_per_rad_s = get_f32(record + 68),
                .velocity_ki_a_per_rad = get_f32(record + 72),
                .position_kp_per_s = get_f32(record + 76),
                .velocity_limit_rad_s = get_f32(record + 80),
                .iq_limit_a = get_f32(record + 84),
                .command_timeout_us = get_u32(record + 88),
            },
        .rx_budget = get_u32(record + 92),
        .remote_current_cap_a = get_f32(record + 96),
        .remote_position_cap_rad = get_f32(record + 100),
        .calibration_present = !v1 && (get_u32(record + 104) & STORE_FLAG_CAL) != 0U,
        .extended = v3,
    };
    if (v3) {
        settings->tuning.profile = (struct foc_runtime_profile){
            .rs_ohm = get_f32(record + 136),
            .ld_h = get_f32(record + 140),
            .lq_h = get_f32(record + 144),
            .flux_wb = get_f32(record + 148),
            .inertia_kg_m2 = get_f32(record + 152),
            .damping_nm_s_per_rad = get_f32(record + 156),
            .rated_current_a = get_f32(record + 160),
            .peak_current_a = get_f32(record + 164),
            .max_speed_rad_s = get_f32(record + 168),
            .gear_ratio = get_f32(record + 172),
            .current_bandwidth_hz = get_f32(record + 176),
        };
        settings->tuning.trips = (struct foc_runtime_trip_limits){
            .overcurrent_a = get_f32(record + 180),
            .overvoltage_v = get_f32(record + 184),
            .undervoltage_v = get_f32(record + 188),
            .overspeed_rad_s = get_f32(record + 192),
            .sustained_ms = get_u32(record + 196),
            .stall_ms = get_u32(record + 200),
            .divergence_ms = get_u32(record + 204),
        };
    }
    if (settings->calibration_present) {
        const uint16_t pole_pairs = (uint16_t)get_u32(record + 120);
        settings->calibration = (struct foc_calibration_snapshot){
            .phase_a_offset_count = get_f32(record + 108),
            .phase_b_offset_count = get_f32(record + 112),
            .phase_c_offset_count = get_f32(record + 116),
            .calibration_direction = (int32_t)get_u32(record + 128),
            .runtime_encoder_direction = (int32_t)get_u32(record + 132),
            .pole_pairs = pole_pairs,
            .electrical_offset_rad = get_f32(record + 124),
            .accepted = get_u32(record + 120) <= UINT16_MAX,
        };
        /* The source identity is Board1's own fact set at the stored pole pairs. */
        foc_board_calibration_identity(pole_pairs, &settings->calibration.source_identity);
    }
    return FOC_BOARD_STORE_OK;
}

int foc_board_store_load(
    const struct foc_board_store_driver *driver,
    struct foc_board_store_identity *identity,
    struct foc_board_settings *settings,
    uint32_t *sequence,
    unsigned int *slot
)
{
    unsigned int chosen;
    if (driver == NULL || identity == NULL || settings == NULL || sequence == NULL) {
        return FOC_BOARD_STORE_ERR_INPUT;
    }
    const int status = scan(driver, identity, settings, sequence, &chosen);
    if (slot != NULL) {
        *slot = chosen;
    }
    return status;
}

int foc_board_store_save(
    const struct foc_board_store_driver *driver,
    const struct foc_board_store_identity *identity,
    const struct foc_board_settings *settings,
    uint32_t *sequence
)
{
    if (driver == NULL || identity == NULL || settings == NULL || sequence == NULL) {
        return FOC_BOARD_STORE_ERR_INPUT;
    }
    struct foc_board_store_identity old_identity;
    struct foc_board_settings old_settings;
    uint32_t old_sequence = 0U;
    unsigned int active = 2U;
    const int status = scan(driver, &old_identity, &old_settings, &old_sequence, &active);
    if (status == FOC_BOARD_STORE_ERR_IO) {
        return status;
    }
    /* Never overwrite the selected record: write the other slot. */
    const unsigned int target = active == 0U ? 1U : 0U;
    const uint32_t next = status == FOC_BOARD_STORE_OK ? old_sequence + 1U : 1U;
    uint8_t record[FOC_BOARD_STORE_RECORD_BYTES];
    uint8_t check[FOC_BOARD_STORE_RECORD_BYTES];
    foc_board_store_encode(identity, settings, next, record);
    if (driver->erase(driver->context, target) != 0 ||
        driver->read(driver->context, target, 0U, check, sizeof check) != 0 ||
        !erased(check, sizeof check)) {
        return FOC_BOARD_STORE_ERR_IO;
    }
    if (driver->program(driver->context, target, 4U, record + 4, sizeof record - 4U) != 0 ||
        driver->read(driver->context, target, 0U, check, sizeof check) != 0 ||
        memcmp(check + 4, record + 4, sizeof record - 4U) != 0) {
        return FOC_BOARD_STORE_ERR_VERIFY;
    }
    uint8_t commit[4];
    put_u32(commit, STORE_COMMIT);
    put_u32(record, STORE_COMMIT);
    if (driver->program(driver->context, target, 0U, commit, sizeof commit) != 0 ||
        driver->read(driver->context, target, 0U, check, sizeof check) != 0 ||
        memcmp(check, record, sizeof record) != 0) {
        return FOC_BOARD_STORE_ERR_VERIFY;
    }
    *sequence = next;
    return FOC_BOARD_STORE_OK;
}

int foc_board_store_erase(const struct foc_board_store_driver *driver)
{
    if (driver == NULL) {
        return FOC_BOARD_STORE_ERR_INPUT;
    }
    uint8_t check[FOC_BOARD_STORE_RECORD_BYTES];
    for (unsigned int slot = 0U; slot < 2U; ++slot) {
        if (driver->erase(driver->context, slot) != 0 ||
            driver->read(driver->context, slot, 0U, check, sizeof check) != 0 ||
            !erased(check, sizeof check)) {
            return FOC_BOARD_STORE_ERR_IO;
        }
    }
    return FOC_BOARD_STORE_OK;
}

void foc_board_persistence_restore(
    struct foc_board_persistence *persistence,
    const struct foc_board_boot *boot,
    struct foc_runtime *runtime,
    struct foc_board_output *output,
    struct foc_board_encoder *encoder,
    struct foc_serial *serial
)
{
    if (persistence == NULL) {
        return;
    }
    persistence->loaded_sequence = 0U;
    persistence->stored_sequence = 0U;
    persistence->stored = false;
    persistence->calibration_restored = false;
    if (!foc_board_store_identity_from_boot(boot, &persistence->identity)) {
        persistence->state = FOC_BOARD_STORE_UNAVAILABLE;
        return;
    }
    struct foc_board_store_identity identity;
    struct foc_board_settings settings;
    uint32_t sequence = 0U;
    const int status =
        foc_board_store_load(&persistence->driver, &identity, &settings, &sequence, NULL);
    if (status == FOC_BOARD_STORE_ERR_EMPTY) {
        persistence->state = FOC_BOARD_STORE_EMPTY;
        return;
    }
    if (status == FOC_BOARD_STORE_ERR_IO) {
        persistence->state = FOC_BOARD_STORE_IO_ERROR;
        return;
    }
    if (status != FOC_BOARD_STORE_OK) {
        persistence->state = FOC_BOARD_STORE_INVALID_RECORD;
        return;
    }
    persistence->stored_sequence = sequence;
    persistence->stored = true;
    if (!identity_equal(&identity, &persistence->identity)) {
        persistence->state = FOC_BOARD_STORE_IDENTITY_MISMATCH;
        return;
    }
    /* The boot current limit stays the ceiling, as for a UART write (ID 68). Tuning is
     * checked on the boot instance first, so a refused value changes nothing. */
    struct foc_runtime_tuning boot_tuning;
    if (!serial_caps_valid(&settings) ||
        !(settings.tuning.current_limit_a <= serial->current_limit_ceiling_a) ||
        foc_runtime_read_tuning(runtime, &boot_tuning) != FOC_RUNTIME_OK) {
        persistence->state = FOC_BOARD_STORE_VALUE_REJECTED;
        return;
    }
    /* Formats 1 and 2 carry no profile or protection thresholds: keep the boot values. */
    if (!settings.extended) {
        settings.tuning.trips = boot_tuning.trips;
        settings.tuning.profile = boot_tuning.profile;
    }
    if (foc_runtime_apply_tuning(runtime, &settings.tuning) != FOC_RUNTIME_OK) {
        persistence->state = FOC_BOARD_STORE_VALUE_REJECTED;
        return;
    }
    if (settings.calibration_present) {
        /* Re-initialization validates the snapshot against Board1's facts; it keeps
         * the instance unchanged when it refuses, so the boot tuning is put back. */
        if (output == NULL || encoder == NULL ||
            foc_board_reconfigure(
                runtime,
                output,
                encoder,
                &settings.calibration,
                runtime->boot_us
            ) != FOC_RUNTIME_OK) {
            (void)foc_runtime_apply_tuning(runtime, &boot_tuning);
            persistence->state = FOC_BOARD_STORE_VALUE_REJECTED;
            return;
        }
        /* Re-initialization starts from defaults: the already checked tuning again. */
        (void)foc_runtime_apply_tuning(runtime, &settings.tuning);
        persistence->restored_calibration = settings.calibration;
        persistence->calibration_restored = true;
    }
    serial->rx_budget = settings.rx_budget;
    serial->current_limit_a = settings.remote_current_cap_a;
    serial->position_limit_rad = settings.remote_position_cap_rad;
    persistence->loaded_sequence = sequence;
    persistence->state = FOC_BOARD_STORE_LOADED;
}

/* UART adapter callbacks -------------------------------------------------------- */

static int persistence_save(
    void *context,
    struct foc_serial *serial,
    struct foc_runtime *runtime,
    uint32_t *sequence
)
{
    struct foc_board_persistence *persistence = context;
    struct foc_board_settings settings;
    if (persistence->state == FOC_BOARD_STORE_UNAVAILABLE) {
        return FOC_PROTO_NOT_SUPPORTED;
    }
    if (!settings_from_live(runtime, serial, &settings)) {
        return FOC_PROTO_BUSY;
    }
    if (!serial_caps_valid(&settings)) {
        return FOC_PROTO_BAD_STATE;
    }
    const struct foc_calibration_snapshot *calibration = persistence->calibration_source;
    settings.calibration_present = calibration != NULL && calibration->accepted;
    if (settings.calibration_present) {
        settings.calibration = *calibration;
    }
    if (foc_board_store_save(&persistence->driver, &persistence->identity, &settings, sequence) !=
        FOC_BOARD_STORE_OK) {
        return FOC_PROTO_INTERNAL_ERROR;
    }
    persistence->stored_sequence = *sequence;
    persistence->stored = true;
    return FOC_PROTO_APPLIED;
}

static int persistence_erase(void *context)
{
    struct foc_board_persistence *persistence = context;
    if (persistence->state == FOC_BOARD_STORE_UNAVAILABLE) {
        return FOC_PROTO_NOT_SUPPORTED;
    }
    if (foc_board_store_erase(&persistence->driver) != FOC_BOARD_STORE_OK) {
        return FOC_PROTO_INTERNAL_ERROR;
    }
    persistence->stored = false;
    persistence->stored_sequence = 0U;
    persistence->calibration_restored = false;
    return FOC_PROTO_APPLIED;
}

static bool persistence_read(const void *context, struct foc_serial_persistence_view *view)
{
    const struct foc_board_persistence *persistence = context;
    *view = (struct foc_serial_persistence_view){
        .state = (uint32_t)persistence->state,
        .loaded_sequence = persistence->loaded_sequence,
        .stored_sequence = persistence->stored ? persistence->stored_sequence : 0U,
        .calibration_restored = persistence->calibration_restored ? 1U : 0U,
    };
    return true;
}

struct foc_serial_persistence foc_board_persistence_serial(
    struct foc_board_persistence *persistence
)
{
    return (struct foc_serial_persistence){
        .context = persistence,
        .save = persistence_save,
        .erase = persistence_erase,
        .read = persistence_read,
    };
}

/* ---------------------------------------- end of file ----------------------------------------- */
