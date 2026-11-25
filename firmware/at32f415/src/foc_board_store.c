/**
 * @file    foc_board_store.c
 * @brief   Board1 persistent settings record, two-slot transaction and boot restore.
 * @version v0.1.0
 * @date    2026.11.22
 *
 * Implementation role
 * -------------------
 * Record "FBP1" format 9: a fixed header and 8-byte TLV entries, little-endian.
 * Formats 1-8 (fixed offsets) are no longer read; such a record reports
 * INVALID_RECORD and the boot configuration stays in use.
 *
 *   0  commit word 0xC35A4C7E (erased 0xFFFFFFFF), programmed last
 *   4  magic "FBP1"   8 format u16 = 9   10 length u16 (whole record, CRC included)
 *   12 sequence u32   16 entry count u16  18 reserved u16 = 0
 *   20 entries: id u16 | type u8 (1 u32, 2 f32) | reserved u8 = 0 | value u32
 *   length - 4: CRC-32 (IEEE, reflected) of bytes 4 .. length - 5
 *
 * Entry IDs are the UART/CAN parameter IDs of every persistent table entry
 * (foc_parameter_table.h), plus 0xF000-0xF007 for the boot identity and 0xF100
 * (present flag) with 0xF101-0xF107 for an applied rotor calibration. Decoding
 * ignores unknown IDs and records which parameters were present; the restore
 * keeps the boot value of any parameter the record does not carry.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_board_store.h"

#include <math.h>
#include <string.h>

/* Private Macros --------------------------------------------------------------- */

#define STORE_COMMIT       UINT32_C(0xC35A4C7E)
#define STORE_MAGIC        UINT32_C(0x31504246) /* "FBP1" */
#define STORE_FORMAT       9U
#define STORE_HEADER       20U
#define STORE_ENTRY        8U
#define STORE_HALF         UINT32_C(0x80000000)
#define STORE_ID_IDENTITY  0xF000U
#define STORE_IDENTITY_N   8U
#define STORE_ID_CAL       0xF100U
#define STORE_CAL_N        7U
#define STORE_TYPE_U32     1U
#define STORE_TYPE_F32     2U
#define STORE_VERIFY_CHUNK 64U

_Static_assert(
    FOC_BOARD_STORE_RECORD_BYTES ==
        STORE_HEADER + 4U +
            STORE_ENTRY * (FOC_PARAMETER_PERSIST_COUNT + STORE_IDENTITY_N + 1U + STORE_CAL_N),
    "record size"
);
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

static bool erased(const uint8_t *bytes, size_t size)
{
    for (size_t index = 0U; index < size; ++index) {
        if (bytes[index] != 0xFFU) {
            return false;
        }
    }
    return true;
}

static uint32_t get_u16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t f32_bits(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

static float bits_f32(uint32_t bits)
{
    float value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

/* A persistent 4-byte field of struct foc_board_settings, by the table's offset. */
static uint32_t settings_get(const struct foc_board_settings *settings, uint16_t offset)
{
    uint32_t value;
    memcpy(&value, (const uint8_t *)settings + offset, sizeof value);
    return value;
}

static void settings_set(struct foc_board_settings *settings, uint16_t offset, uint32_t value)
{
    memcpy((uint8_t *)settings + offset, &value, sizeof value);
}

static size_t put_entry(uint8_t *record, size_t at, uint32_t id, uint32_t type, uint32_t value)
{
    record[at] = (uint8_t)id;
    record[at + 1U] = (uint8_t)(id >> 8);
    record[at + 2U] = (uint8_t)type;
    record[at + 3U] = 0U;
    put_u32(record + at + 4U, value);
    return at + STORE_ENTRY;
}

/* Read back [offset, offset + size) in chunks and compare with bytes, or with erased
 * 0xFF when bytes is NULL. */
static bool slot_matches(
    const struct foc_board_store_driver *driver,
    unsigned int slot,
    size_t offset,
    const uint8_t *bytes,
    size_t size
)
{
    uint8_t chunk[STORE_VERIFY_CHUNK];
    for (size_t done = 0U; done < size;) {
        const size_t step = size - done < sizeof chunk ? size - done : sizeof chunk;
        if (driver->read(driver->context, slot, offset + done, chunk, step) != 0) {
            return false;
        }
        if (bytes == NULL ? !erased(chunk, step) : memcmp(chunk, bytes + done, step) != 0) {
            return false;
        }
        done += step;
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
    settings->output_zero_rad = serial->output_zero_rad;
    settings->can = serial->can;
    return true;
}

/* The UART RAM caps use the same ranges as their PARAM_WRITE entries (IDs 1–3). */
static bool serial_caps_valid(const struct foc_board_settings *settings)
{
    return settings->rx_budget >= 8U && settings->rx_budget <= 32U &&
        isfinite(settings->remote_current_cap_a) && settings->remote_current_cap_a >= 0.05f &&
        settings->remote_current_cap_a <= settings->tuning.iq_limit_a &&
        isfinite(settings->remote_position_cap_rad) && settings->remote_position_cap_rad >= 1.0f &&
        settings->remote_position_cap_rad <= 1000.0f && isfinite(settings->output_zero_rad) &&
        fabsf(settings->output_zero_rad) <= 1000.0f &&
        foc_serial_can_settings_valid(&settings->can);
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

size_t foc_board_store_encode(
    const struct foc_board_store_identity *identity,
    const struct foc_board_settings *settings,
    uint32_t sequence,
    uint8_t record[FOC_BOARD_STORE_RECORD_BYTES]
)
{
    memset(record, 0xFF, FOC_BOARD_STORE_RECORD_BYTES);
    size_t at = STORE_HEADER;
    const uint32_t identity_values[STORE_IDENTITY_N] = {
        identity->configuration,
        identity->pole_pairs,
        identity->encoder_counts_per_revolution,
        (uint32_t)identity->runtime_encoder_direction,
        f32_bits(identity->electrical_offset_rad),
        f32_bits(identity->phase_offset_count[0]),
        f32_bits(identity->phase_offset_count[1]),
        f32_bits(identity->phase_offset_count[2]),
    };
    for (uint32_t index = 0U; index < STORE_IDENTITY_N; ++index) {
        at = put_entry(
            record,
            at,
            STORE_ID_IDENTITY + index,
            index < 4U ? STORE_TYPE_U32 : STORE_TYPE_F32,
            identity_values[index]
        );
    }
    if (settings->calibration_present) {
        const struct foc_calibration_snapshot *cal = &settings->calibration;
        const uint32_t cal_values[STORE_CAL_N] = {
            f32_bits(cal->phase_a_offset_count),
            f32_bits(cal->phase_b_offset_count),
            f32_bits(cal->phase_c_offset_count),
            cal->pole_pairs,
            f32_bits(cal->electrical_offset_rad),
            (uint32_t)cal->calibration_direction,
            (uint32_t)cal->runtime_encoder_direction,
        };
        static const uint8_t cal_types[STORE_CAL_N] = {
            STORE_TYPE_F32,
            STORE_TYPE_F32,
            STORE_TYPE_F32,
            STORE_TYPE_U32,
            STORE_TYPE_F32,
            STORE_TYPE_U32,
            STORE_TYPE_U32,
        };
        at = put_entry(record, at, STORE_ID_CAL, STORE_TYPE_U32, 1U);
        for (uint32_t index = 0U; index < STORE_CAL_N; ++index) {
            at = put_entry(
                record,
                at,
                STORE_ID_CAL + 1U + index,
                cal_types[index],
                cal_values[index]
            );
        }
    }
    for (size_t index = 0U; index < FOC_PARAMETER_COUNT; ++index) {
        const struct foc_parameter_entry *entry = &foc_parameter_table[index];
        if ((entry->flags & FOC_PARAM_FLAG_PERSIST) != 0U) {
            at = put_entry(
                record,
                at,
                entry->id,
                entry->type,
                settings_get(settings, entry->persist_offset)
            );
        }
    }
    const size_t length = at + 4U;
    put_u32(record, UINT32_MAX);
    put_u32(record + 4, STORE_MAGIC);
    record[8] = (uint8_t)STORE_FORMAT;
    record[9] = 0U;
    record[10] = (uint8_t)length;
    record[11] = (uint8_t)(length >> 8);
    put_u32(record + 12, sequence);
    const size_t count = (at - STORE_HEADER) / STORE_ENTRY;
    record[16] = (uint8_t)count;
    record[17] = (uint8_t)(count >> 8);
    record[18] = 0U;
    record[19] = 0U;
    put_u32(record + at, store_crc32(record + 4, at - 4U));
    return length;
}

bool foc_board_settings_present(const struct foc_board_settings *settings, size_t index)
{
    return index < FOC_PARAMETER_COUNT &&
        (settings->present[index / 8U] & (1U << (index % 8U))) != 0U;
}

int foc_board_store_decode(
    const uint8_t record[FOC_BOARD_STORE_RECORD_BYTES],
    struct foc_board_store_identity *identity,
    struct foc_board_settings *settings,
    uint32_t *sequence
)
{
    const size_t length = get_u16(record + 10);
    const size_t count = get_u16(record + 16);
    if (get_u32(record) != STORE_COMMIT || get_u32(record + 4) != STORE_MAGIC ||
        get_u16(record + 8) != STORE_FORMAT || length < STORE_HEADER + 4U ||
        length > FOC_BOARD_STORE_RECORD_BYTES ||
        count * STORE_ENTRY + STORE_HEADER + 4U != length || get_u16(record + 18) != 0U ||
        get_u32(record + length - 4U) != store_crc32(record + 4, length - 8U)) {
        return FOC_BOARD_STORE_ERR_RECORD;
    }
    uint32_t identity_values[STORE_IDENTITY_N] = {0};
    uint32_t cal_values[STORE_CAL_N] = {0};
    uint32_t identity_seen = 0U;
    uint32_t cal_seen = 0U;
    uint32_t cal_flag = 0U;
    bool cal_flag_seen = false;
    *settings = (struct foc_board_settings){0};
    for (size_t index = 0U; index < count; ++index) {
        const uint8_t *entry_bytes = record + STORE_HEADER + index * STORE_ENTRY;
        const uint32_t id = get_u16(entry_bytes);
        const uint32_t type = entry_bytes[2];
        const uint32_t value = get_u32(entry_bytes + 4);
        if (entry_bytes[3] != 0U || (type != STORE_TYPE_U32 && type != STORE_TYPE_F32)) {
            return FOC_BOARD_STORE_ERR_RECORD;
        }
        if (id >= STORE_ID_IDENTITY && id < STORE_ID_IDENTITY + STORE_IDENTITY_N) {
            const uint32_t slot = id - STORE_ID_IDENTITY;
            if ((identity_seen & (1U << slot)) != 0U ||
                type != (slot < 4U ? STORE_TYPE_U32 : STORE_TYPE_F32)) {
                return FOC_BOARD_STORE_ERR_RECORD;
            }
            identity_seen |= 1U << slot;
            identity_values[slot] = value;
        } else if (id == STORE_ID_CAL) {
            if (cal_flag_seen || type != STORE_TYPE_U32 || value > 1U) {
                return FOC_BOARD_STORE_ERR_RECORD;
            }
            cal_flag_seen = true;
            cal_flag = value;
        } else if (id > STORE_ID_CAL && id <= STORE_ID_CAL + STORE_CAL_N) {
            const uint32_t slot = id - STORE_ID_CAL - 1U;
            if ((cal_seen & (1U << slot)) != 0U) {
                return FOC_BOARD_STORE_ERR_RECORD;
            }
            cal_seen |= 1U << slot;
            cal_values[slot] = value;
        } else {
            const struct foc_parameter_entry *entry = foc_parameter_find((uint16_t)id);
            if (entry == NULL || (entry->flags & FOC_PARAM_FLAG_PERSIST) == 0U) {
                continue; /* written by another firmware: ignored */
            }
            const size_t table_index = (size_t)(entry - foc_parameter_table);
            if (type != entry->type || foc_board_settings_present(settings, table_index)) {
                return FOC_BOARD_STORE_ERR_RECORD;
            }
            settings_set(settings, entry->persist_offset, value);
            settings->present[table_index / 8U] |= (uint8_t)(1U << (table_index % 8U));
        }
    }
    if (identity_seen != (1U << STORE_IDENTITY_N) - 1U ||
        (cal_flag == 1U && cal_seen != (1U << STORE_CAL_N) - 1U)) {
        return FOC_BOARD_STORE_ERR_RECORD;
    }
    *sequence = get_u32(record + 12);
    *identity = (struct foc_board_store_identity){
        .configuration = identity_values[0],
        .pole_pairs = identity_values[1],
        .encoder_counts_per_revolution = identity_values[2],
        .runtime_encoder_direction = (int32_t)identity_values[3],
        .electrical_offset_rad = bits_f32(identity_values[4]),
        .phase_offset_count = {
            bits_f32(identity_values[5]),
            bits_f32(identity_values[6]),
            bits_f32(identity_values[7])
        },
    };
    settings->calibration_present = cal_flag == 1U;
    if (settings->calibration_present) {
        const uint16_t pole_pairs = (uint16_t)cal_values[3];
        settings->calibration = (struct foc_calibration_snapshot){
            .phase_a_offset_count = bits_f32(cal_values[0]),
            .phase_b_offset_count = bits_f32(cal_values[1]),
            .phase_c_offset_count = bits_f32(cal_values[2]),
            .calibration_direction = (int32_t)cal_values[5],
            .runtime_encoder_direction = (int32_t)cal_values[6],
            .pole_pairs = pole_pairs,
            .electrical_offset_rad = bits_f32(cal_values[4]),
            .accepted = cal_values[3] <= UINT16_MAX,
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
    const size_t length = foc_board_store_encode(identity, settings, next, record);
    if (driver->erase(driver->context, target) != 0 ||
        !slot_matches(driver, target, 0U, NULL, FOC_BOARD_STORE_RECORD_BYTES)) {
        return FOC_BOARD_STORE_ERR_IO;
    }
    if (driver->program(driver->context, target, 4U, record + 4, length - 4U) != 0 ||
        !slot_matches(driver, target, 4U, record + 4, length - 4U)) {
        return FOC_BOARD_STORE_ERR_VERIFY;
    }
    uint8_t commit[4];
    put_u32(commit, STORE_COMMIT);
    if (driver->program(driver->context, target, 0U, commit, sizeof commit) != 0 ||
        !slot_matches(driver, target, 0U, commit, sizeof commit) ||
        !slot_matches(driver, target, 4U, record + 4, length - 4U)) {
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
    for (unsigned int slot = 0U; slot < 2U; ++slot) {
        if (driver->erase(driver->context, slot) != 0 ||
            !slot_matches(driver, slot, 0U, NULL, FOC_BOARD_STORE_RECORD_BYTES)) {
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
    /* Start from the boot values and overlay every parameter the record carries; one it
     * does not carry (a newer firmware's addition) keeps its boot value. */
    struct foc_runtime_tuning boot_tuning;
    if (foc_runtime_read_tuning(runtime, &boot_tuning) != FOC_RUNTIME_OK) {
        persistence->state = FOC_BOARD_STORE_VALUE_REJECTED;
        return;
    }
    struct foc_board_settings merged = {
        .tuning = boot_tuning,
        .rx_budget = serial->rx_budget,
        .remote_current_cap_a = serial->current_limit_a,
        .remote_position_cap_rad = serial->position_limit_rad,
        .output_zero_rad = serial->output_zero_rad,
        .can = serial->can,
        .calibration_present = settings.calibration_present,
        .calibration = settings.calibration,
    };
    for (size_t index = 0U; index < FOC_PARAMETER_COUNT; ++index) {
        if (foc_board_settings_present(&settings, index)) {
            const uint16_t offset = foc_parameter_table[index].persist_offset;
            settings_set(&merged, offset, settings_get(&settings, offset));
        }
    }
    settings = merged;
    /* The boot current limit stays the ceiling, as for a UART write (ID 68). Tuning is
     * checked on the boot instance first, so a refused value changes nothing. */
    if (!serial_caps_valid(&settings) ||
        !(settings.tuning.current_limit_a <= serial->current_limit_ceiling_a)) {
        persistence->state = FOC_BOARD_STORE_VALUE_REJECTED;
        return;
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
    serial->output_zero_rad = settings.output_zero_rad;
    serial->can = settings.can;
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
