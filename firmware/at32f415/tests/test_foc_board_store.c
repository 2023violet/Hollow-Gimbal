/**
 * @file    test_foc_board_store.c
 * @brief   Board1 persistent record: codec, slot selection, torn saves and boot restore.
 * @version v0.1.0
 * @date    2026.11.22
 *
 * Test role
 * ---------
 * Drive the production store through a fake NOR device with power-loss cuts,
 * stuck cells and damaged slots, the format-9 TLV rules (unknown, missing,
 * duplicate and mistyped entries, old formats refused) and the boot restore
 * through the production boot selection.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_board.h"
#include "foc_board_boot.h"
#include "foc_board_store.h"
#include "foc_parameter_table.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return 1;                                                       \
        }                                                                   \
    } while (0)

/* Private Helpers: fake NOR device ---------------------------------------------- */

struct fake_flash {
    uint8_t slots[2][FOC_BOARD_STORE_SLOT_BYTES];
    /* Fail the Nth program call (1-based) by stopping before it: a power loss. */
    unsigned int cut_program;
    unsigned int programs;
    bool fail_read;
    bool stuck_bits;
};

static int fake_read(void *context, unsigned int slot, size_t offset, uint8_t *bytes, size_t size)
{
    struct fake_flash *flash = context;
    if (flash->fail_read || slot > 1U || offset + size > FOC_BOARD_STORE_SLOT_BYTES) {
        return -1;
    }
    memcpy(bytes, &flash->slots[slot][offset], size);
    return 0;
}

static int fake_erase(void *context, unsigned int slot)
{
    struct fake_flash *flash = context;
    memset(flash->slots[slot], 0xFF, FOC_BOARD_STORE_SLOT_BYTES);
    return 0;
}

static int
fake_program(void *context, unsigned int slot, size_t offset, const uint8_t *bytes, size_t size)
{
    struct fake_flash *flash = context;
    if (++flash->programs == flash->cut_program) {
        return -1;
    }
    if (offset % FOC_BOARD_STORE_PROGRAM_BYTES != 0U ||
        size % FOC_BOARD_STORE_PROGRAM_BYTES != 0U) {
        return -1;
    }
    for (size_t index = 0U; index < size; ++index) {
        /* A worn cell keeps one bit set: the write cannot be verified. */
        const uint8_t value =
            flash->stuck_bits && index == 9U ? (uint8_t)(bytes[index] | 1U) : bytes[index];
        flash->slots[slot][offset + index] &= value;
    }
    return 0;
}

static struct foc_board_store_driver driver_for(struct fake_flash *flash)
{
    memset(flash->slots, 0xFF, sizeof flash->slots);
    return (struct foc_board_store_driver){flash, fake_read, fake_erase, fake_program};
}

/* Fixtures --------------------------------------------------------------------- */

static const struct foc_board_store_identity nominal = {
    .configuration = 1U,
    .pole_pairs = 7U,
    .encoder_counts_per_revolution = 65536U,
    .runtime_encoder_direction = -1,
    .electrical_offset_rad = 0.0f,
    .phase_offset_count = {2048.0f, 2048.0f, 2048.0f},
};

static struct foc_board_settings settings_with_kp(float kp)
{
    return (struct foc_board_settings){
        .tuning =
            {
                .current_kp_v_per_a = {1.08f, kp},
                .current_ki_v_per_a_s = {540.0f, 540.0f},
                .current_limit_a = 2.5f,
                .velocity_kp_a_per_rad_s = 0.18f,
                .velocity_ki_a_per_rad = 1.4f,
                .position_kp_per_s = 4.0f,
                .velocity_limit_rad_s = 12.0f,
                .iq_limit_a = 2.5f,
                .command_timeout_us = 2500U,
                /* Board1 initial protection and profile, as a format-3 record carries. */
                .trips = {3.0f, 22.0f, 5.0f, 0.0f, 1000U, 200U, 300U},
                .profile = {.gear_ratio = 1.0f, .current_bandwidth_hz = 150.0f},
            },
        .rx_budget = 24U,
        .remote_current_cap_a = 2.5f,
        .remote_position_cap_rad = 100.0f,
        .can = foc_serial_can_defaults(),
    };
}

/* Tests ------------------------------------------------------------------------ */

static size_t find_entry(const uint8_t *record, uint16_t id);
static size_t drop_entry(uint8_t *record, size_t index);

static int test_codec(void)
{
    uint8_t record[FOC_BOARD_STORE_RECORD_BYTES];
    const struct foc_board_settings in = settings_with_kp(2.0f);
    const size_t length = foc_board_store_encode(&nominal, &in, 7U, record);
    CHECK(length == FOC_BOARD_STORE_RECORD_BYTES - 8U * 8U); /* no calibration entries */
    struct foc_board_store_identity identity;
    struct foc_board_settings out;
    uint32_t sequence = 0U;
    /* Uncommitted: the commit word is still erased. */
    CHECK(foc_board_store_decode(record, &identity, &out, &sequence) == FOC_BOARD_STORE_ERR_RECORD);
    record[0] = 0x7EU;
    record[1] = 0x4CU;
    record[2] = 0x5AU;
    record[3] = 0xC3U;
    CHECK(foc_board_store_decode(record, &identity, &out, &sequence) == FOC_BOARD_STORE_OK);
    CHECK(sequence == 7U && out.tuning.current_kp_v_per_a.q == 2.0f && out.rx_budget == 24U);
    CHECK(identity.pole_pairs == 7U && identity.runtime_encoder_direction == -1);
    CHECK(out.tuning.command_timeout_us == 2500U && out.remote_position_cap_rad == 100.0f);
    /* Every parameter of the table was carried. */
    for (size_t index = 0U; index < FOC_PARAMETER_COUNT; ++index) {
        CHECK(
            foc_board_settings_present(&out, index) ==
            ((foc_parameter_table[index].flags & FOC_PARAM_FLAG_PERSIST) != 0U)
        );
    }
    /* Any flipped byte in the covered range fails the CRC. */
    for (size_t index = 4U; index < length; ++index) {
        record[index] ^= 0x10U;
        CHECK(
            foc_board_store_decode(record, &identity, &out, &sequence) == FOC_BOARD_STORE_ERR_RECORD
        );
        record[index] ^= 0x10U;
    }
    return 0;
}

static int test_slots(void)
{
    struct fake_flash flash = {0};
    const struct foc_board_store_driver driver = driver_for(&flash);
    struct foc_board_store_identity identity;
    struct foc_board_settings out;
    uint32_t sequence = 0U;
    unsigned int slot = 0U;
    CHECK(
        foc_board_store_load(&driver, &identity, &out, &sequence, &slot) ==
        FOC_BOARD_STORE_ERR_EMPTY
    );

    /* Saves alternate slots with increasing sequences; load picks the newest. */
    struct foc_board_settings in = settings_with_kp(1.5f);
    CHECK(
        foc_board_store_save(&driver, &nominal, &in, &sequence) == FOC_BOARD_STORE_OK &&
        sequence == 1U
    );
    in = settings_with_kp(1.6f);
    CHECK(
        foc_board_store_save(&driver, &nominal, &in, &sequence) == FOC_BOARD_STORE_OK &&
        sequence == 2U
    );
    CHECK(foc_board_store_load(&driver, &identity, &out, &sequence, &slot) == FOC_BOARD_STORE_OK);
    CHECK(sequence == 2U && slot == 1U && out.tuning.current_kp_v_per_a.q == 1.6f);
    in = settings_with_kp(1.7f);
    CHECK(
        foc_board_store_save(&driver, &nominal, &in, &sequence) == FOC_BOARD_STORE_OK &&
        sequence == 3U
    );
    CHECK(foc_board_store_load(&driver, &identity, &out, &sequence, &slot) == FOC_BOARD_STORE_OK);
    CHECK(sequence == 3U && slot == 0U && out.tuning.current_kp_v_per_a.q == 1.7f);

    /* Power lost after the body, before the commit word: the old record stays selected. */
    flash.programs = 0U;
    flash.cut_program = 2U;
    in = settings_with_kp(1.8f);
    CHECK(foc_board_store_save(&driver, &nominal, &in, &sequence) == FOC_BOARD_STORE_ERR_VERIFY);
    CHECK(foc_board_store_load(&driver, &identity, &out, &sequence, &slot) == FOC_BOARD_STORE_OK);
    CHECK(sequence == 3U && out.tuning.current_kp_v_per_a.q == 1.7f);
    /* Power lost before the body: the target slot is erased, the old record stays. */
    flash.programs = 0U;
    flash.cut_program = 1U;
    CHECK(foc_board_store_save(&driver, &nominal, &in, &sequence) == FOC_BOARD_STORE_ERR_VERIFY);
    CHECK(foc_board_store_load(&driver, &identity, &out, &sequence, &slot) == FOC_BOARD_STORE_OK);
    CHECK(sequence == 3U && out.tuning.current_kp_v_per_a.q == 1.7f);
    flash.cut_program = 0U;
    /* A cell that cannot be programmed fails verification; nothing new is selected. */
    flash.stuck_bits = true;
    CHECK(foc_board_store_save(&driver, &nominal, &in, &sequence) == FOC_BOARD_STORE_ERR_VERIFY);
    flash.stuck_bits = false;
    CHECK(foc_board_store_load(&driver, &identity, &out, &sequence, &slot) == FOC_BOARD_STORE_OK);
    CHECK(sequence == 3U);
    /* The retry succeeds and continues the sequence. */
    CHECK(
        foc_board_store_save(&driver, &nominal, &in, &sequence) == FOC_BOARD_STORE_OK &&
        sequence == 4U
    );

    /* A corrupted newest copy falls back to the other committed copy. */
    CHECK(foc_board_store_load(&driver, &identity, &out, &sequence, &slot) == FOC_BOARD_STORE_OK);
    flash.slots[slot][60] ^= 0x01U;
    CHECK(foc_board_store_load(&driver, &identity, &out, &sequence, &slot) == FOC_BOARD_STORE_OK);
    CHECK(sequence == 3U && out.tuning.current_kp_v_per_a.q == 1.7f);
    flash.slots[slot][60] ^= 0x01U;
    /* Both damaged: invalid, not empty. */
    flash.slots[0][20] ^= 0x01U;
    flash.slots[1][20] ^= 0x01U;
    CHECK(
        foc_board_store_load(&driver, &identity, &out, &sequence, &slot) ==
        FOC_BOARD_STORE_ERR_RECORD
    );
    flash.fail_read = true;
    CHECK(
        foc_board_store_load(&driver, &identity, &out, &sequence, &slot) == FOC_BOARD_STORE_ERR_IO
    );
    flash.fail_read = false;
    CHECK(foc_board_store_erase(&driver) == FOC_BOARD_STORE_OK);
    CHECK(
        foc_board_store_load(&driver, &identity, &out, &sequence, &slot) ==
        FOC_BOARD_STORE_ERR_EMPTY
    );
    return 0;
}

static int test_sequence_wrap(void)
{
    struct fake_flash flash = {0};
    const struct foc_board_store_driver driver = driver_for(&flash);
    uint8_t record[FOC_BOARD_STORE_RECORD_BYTES];
    const struct foc_board_settings in = settings_with_kp(1.5f);
    /* Slot A holds UINT32_MAX, slot B holds 0: B is newer modulo 2^32. */
    foc_board_store_encode(&nominal, &in, UINT32_MAX, record);
    memcpy(flash.slots[0], record, sizeof record);
    foc_board_store_encode(&nominal, &in, 0U, record);
    memcpy(flash.slots[1], record, sizeof record);
    const uint8_t commit[] = {0x7EU, 0x4CU, 0x5AU, 0xC3U};
    memcpy(flash.slots[0], commit, 4U);
    memcpy(flash.slots[1], commit, 4U);
    struct foc_board_store_identity identity;
    struct foc_board_settings out;
    uint32_t sequence = 1U;
    unsigned int slot = 2U;
    CHECK(foc_board_store_load(&driver, &identity, &out, &sequence, &slot) == FOC_BOARD_STORE_OK);
    CHECK(slot == 1U && sequence == 0U);
    return 0;
}

/* Boot restore through the production boot path ---------------------------------- */

struct restore_fixture {
    struct foc_runtime runtime;
    struct foc_board_output output;
    struct foc_board_encoder encoder;
    struct foc_board_boot boot;
    struct foc_serial serial;
    struct foc_board_persistence persistence;
    struct fake_flash flash;
};

static int restore_boot(struct restore_fixture *fixture, bool keep_flash)
{
    struct fake_flash saved = fixture->flash;
    memset(fixture, 0, sizeof *fixture);
    const struct foc_board_calibration_expectation expected = foc_board_calibration_expectation();
    CHECK(
        foc_board_boot_select(
            &fixture->boot,
            &fixture->runtime,
            &fixture->output,
            &fixture->encoder,
            foc_board_calibration_supply(),
            &expected,
            0U
        ) == FOC_RUNTIME_OK
    );
    foc_serial_init(&fixture->serial, &fixture->runtime);
    fixture->persistence.driver = driver_for(&fixture->flash);
    if (keep_flash) {
        fixture->flash = saved;
    }
    foc_board_persistence_restore(
        &fixture->persistence,
        &fixture->boot,
        &fixture->runtime,
        &fixture->output,
        &fixture->encoder,
        &fixture->serial
    );
    return 0;
}

static int write_record(
    struct fake_flash *flash,
    const struct foc_board_store_identity *identity,
    const struct foc_board_settings *settings
)
{
    const struct foc_board_store_driver driver = {flash, fake_read, fake_erase, fake_program};
    uint32_t sequence = 0U;
    CHECK(foc_board_store_save(&driver, identity, settings, &sequence) == FOC_BOARD_STORE_OK);
    return 0;
}

static int test_restore(void)
{
    static struct restore_fixture fixture;
    CHECK(restore_boot(&fixture, false) == 0);
    CHECK(fixture.persistence.state == FOC_BOARD_STORE_EMPTY);
    struct foc_board_store_identity boot_identity;
    CHECK(foc_board_store_identity_from_boot(&fixture.boot, &boot_identity));
    CHECK(boot_identity.configuration == 1U && boot_identity.pole_pairs == 7U);

    /* A matching record is applied before any producer: tuning and UART caps. */
    struct foc_board_settings in = settings_with_kp(2.2f);
    in.tuning.command_timeout_us = 30000U;
    in.rx_budget = 16U;
    in.remote_position_cap_rad = 50.0f;
    CHECK(write_record(&fixture.flash, &boot_identity, &in) == 0);
    CHECK(restore_boot(&fixture, true) == 0);
    CHECK(fixture.persistence.state == FOC_BOARD_STORE_LOADED);
    CHECK(fixture.persistence.loaded_sequence == 1U);
    CHECK(fixture.runtime.pipeline.current.config.kp_v_per_a.q == 2.2f);
    CHECK(fixture.runtime.pipeline.safety.config.command_timeout_us == 30000U);
    CHECK(fixture.serial.rx_budget == 16U && fixture.serial.position_limit_rad == 50.0f);
    struct foc_runtime_tuning_result result;
    CHECK(foc_runtime_read_tuning_result(&fixture.runtime, &result) == FOC_RUNTIME_OK);
    CHECK(result.result == FOC_RUNTIME_REQUEST_APPLIED && result.revision == 1U);

    /* A record for another motor configuration is not applied. */
    struct foc_board_store_identity other = boot_identity;
    other.pole_pairs = 11U;
    memset(fixture.flash.slots, 0xFF, sizeof fixture.flash.slots);
    CHECK(write_record(&fixture.flash, &other, &in) == 0);
    CHECK(restore_boot(&fixture, true) == 0);
    CHECK(fixture.persistence.state == FOC_BOARD_STORE_IDENTITY_MISMATCH);
    CHECK(fixture.runtime.pipeline.current.config.kp_v_per_a.q == 1.35f);
    CHECK(fixture.persistence.loaded_sequence == 0U && fixture.persistence.stored_sequence == 1U);
    /* A recalibrated offset is a different configuration too. */
    other = boot_identity;
    other.phase_offset_count[1] = 2050.0f;
    memset(fixture.flash.slots, 0xFF, sizeof fixture.flash.slots);
    CHECK(write_record(&fixture.flash, &other, &in) == 0);
    CHECK(restore_boot(&fixture, true) == 0);
    CHECK(fixture.persistence.state == FOC_BOARD_STORE_IDENTITY_MISMATCH);

    /* Values the Runtime would refuse are not applied, nothing partially. */
    struct foc_board_settings bad = in;
    bad.tuning.iq_limit_a = 3.0f;
    memset(fixture.flash.slots, 0xFF, sizeof fixture.flash.slots);
    CHECK(write_record(&fixture.flash, &boot_identity, &bad) == 0);
    CHECK(restore_boot(&fixture, true) == 0);
    CHECK(fixture.persistence.state == FOC_BOARD_STORE_VALUE_REJECTED);
    CHECK(fixture.runtime.pipeline.current.config.kp_v_per_a.q == 1.35f);
    CHECK(fixture.serial.rx_budget == FOC_SERIAL_RX_DEFAULT);
    /* The boot current limit stays the ceiling. */
    bad = in;
    bad.tuning.current_limit_a = 3.0f;
    memset(fixture.flash.slots, 0xFF, sizeof fixture.flash.slots);
    CHECK(write_record(&fixture.flash, &boot_identity, &bad) == 0);
    CHECK(restore_boot(&fixture, true) == 0);
    CHECK(fixture.persistence.state == FOC_BOARD_STORE_VALUE_REJECTED);
    /* A UART cap outside its PARAM_WRITE range is rejected as well. */
    bad = in;
    bad.rx_budget = 64U;
    memset(fixture.flash.slots, 0xFF, sizeof fixture.flash.slots);
    CHECK(write_record(&fixture.flash, &boot_identity, &bad) == 0);
    CHECK(restore_boot(&fixture, true) == 0);
    CHECK(fixture.persistence.state == FOC_BOARD_STORE_VALUE_REJECTED);

    /* A record without a parameter keeps its boot value (dead-time compensation 0, CAN
     * defaults here) and still applies the rest. */
    {
        struct foc_board_settings partial = in;
        partial.tuning.dead_time_v = 0.05f;
        partial.can.node_id = 9U;
        uint8_t record[FOC_BOARD_STORE_RECORD_BYTES];
        foc_board_store_encode(&boot_identity, &partial, 1U, record);
        (void)drop_entry(record, find_entry(record, FOC_PARAM_DEAD_TIME_COMP));
        (void)drop_entry(record, find_entry(record, FOC_PARAM_CAN_NODE_ID));
        memset(fixture.flash.slots, 0xFF, sizeof fixture.flash.slots);
        memcpy(fixture.flash.slots[0], record, sizeof record);
        CHECK(restore_boot(&fixture, true) == 0);
        CHECK(fixture.persistence.state == FOC_BOARD_STORE_LOADED);
        CHECK(fixture.runtime.pipeline.current.config.kp_v_per_a.q == 2.2f);
        struct foc_runtime_tuning applied;
        CHECK(foc_runtime_read_tuning(&fixture.runtime, &applied) == FOC_RUNTIME_OK);
        CHECK(applied.dead_time_v == 0.0f);
        CHECK(fixture.serial.can.node_id == 63U && fixture.serial.rx_budget == 16U);
    }

    /* Damaged storage reports INVALID_RECORD and keeps the boot configuration. */
    memset(fixture.flash.slots, 0x00, sizeof fixture.flash.slots);
    CHECK(restore_boot(&fixture, true) == 0);
    CHECK(fixture.persistence.state == FOC_BOARD_STORE_INVALID_RECORD);
    fixture.flash.fail_read = true;
    CHECK(restore_boot(&fixture, true) == 0);
    CHECK(fixture.persistence.state == FOC_BOARD_STORE_IO_ERROR);
    return 0;
}

/* Format 9: TLV entries, an applied rotor calibration, every persistent parameter. */
static uint32_t test_crc32(const uint8_t *bytes, size_t size)
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

static void put_word(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

static size_t record_length(const uint8_t *record)
{
    return (size_t)record[10] | ((size_t)record[11] << 8);
}

/* Commit, fix count, length and CRC after editing the entries of a record of `length`. */
static void reseal(uint8_t *record, size_t length)
{
    const size_t count = (length - 24U) / 8U;
    record[10] = (uint8_t)length;
    record[11] = (uint8_t)(length >> 8);
    record[16] = (uint8_t)count;
    record[17] = (uint8_t)(count >> 8);
    put_word(record + length - 4U, test_crc32(record + 4, length - 8U));
    put_word(record, UINT32_C(0xC35A4C7E));
}

/* Index of the entry carrying `id`, or SIZE_MAX. */
static size_t find_entry(const uint8_t *record, uint16_t id)
{
    const size_t count = (record_length(record) - 24U) / 8U;
    for (size_t index = 0U; index < count; ++index) {
        const uint8_t *entry = record + 20U + 8U * index;
        if ((uint16_t)(entry[0] | (entry[1] << 8)) == id) {
            return index;
        }
    }
    return SIZE_MAX;
}

/* Drop one entry; returns the new length. */
static size_t drop_entry(uint8_t *record, size_t index)
{
    const size_t length = record_length(record);
    uint8_t *entry = record + 20U + 8U * index;
    memmove(entry, entry + 8U, length - 4U - (size_t)(entry + 8U - record));
    const size_t shorter = length - 8U;
    memset(record + shorter, 0xFF, length - shorter);
    reseal(record, shorter);
    return shorter;
}

/* Board1-consistent calibration: raw source -1, fitted -1, Runtime +1. */
static struct foc_calibration_snapshot calibrated(void)
{
    struct foc_calibration_snapshot snapshot = {
        .phase_a_offset_count = 2031.5f,
        .phase_b_offset_count = 2063.25f,
        .phase_c_offset_count = 2017.75f,
        .calibration_direction = -1,
        .runtime_encoder_direction = 1,
        .pole_pairs = 11U,
        .electrical_offset_rad = 1.25f,
        .accepted = true,
    };
    foc_board_calibration_identity(11U, &snapshot.source_identity);
    return snapshot;
}

static int test_calibration_record(void)
{
    uint8_t record[FOC_BOARD_STORE_RECORD_BYTES];
    struct foc_board_settings in = settings_with_kp(2.0f);
    in.calibration_present = true;
    in.calibration = calibrated();
    CHECK(foc_board_store_encode(&nominal, &in, 3U, record) == FOC_BOARD_STORE_RECORD_BYTES);
    put_word(record, UINT32_C(0xC35A4C7E));
    struct foc_board_store_identity identity;
    struct foc_board_settings out;
    uint32_t sequence = 0U;
    CHECK(foc_board_store_decode(record, &identity, &out, &sequence) == FOC_BOARD_STORE_OK);
    CHECK(out.calibration_present && out.calibration.accepted);
    CHECK(out.calibration.pole_pairs == 11U && out.calibration.electrical_offset_rad == 1.25f);
    CHECK(out.calibration.phase_b_offset_count == 2063.25f);
    CHECK(
        out.calibration.calibration_direction == -1 &&
        out.calibration.runtime_encoder_direction == 1
    );
    CHECK(out.calibration.source_identity.pole_pairs == 11U);
    CHECK(out.tuning.current_kp_v_per_a.q == 2.0f);
    /* A calibration flag other than 0/1 is refused. */
    uint8_t bad[FOC_BOARD_STORE_RECORD_BYTES];
    memcpy(bad, record, sizeof bad);
    put_word(bad + 20U + 8U * find_entry(bad, 0xF100U) + 4U, 3U);
    reseal(bad, record_length(bad));
    CHECK(foc_board_store_decode(bad, &identity, &out, &sequence) == FOC_BOARD_STORE_ERR_RECORD);
    /* A calibration missing one of its fields is refused. */
    memcpy(bad, record, sizeof bad);
    (void)drop_entry(bad, find_entry(bad, 0xF104U));
    CHECK(foc_board_store_decode(bad, &identity, &out, &sequence) == FOC_BOARD_STORE_ERR_RECORD);
    return 0;
}

/* Every persistent parameter round-trips: profile, protection, output zero, ramps,
 * dead-time compensation and CAN. */
static int test_profile_record(void)
{
    uint8_t record[FOC_BOARD_STORE_RECORD_BYTES];
    struct foc_board_settings in = settings_with_kp(2.0f);
    in.tuning.profile = (struct foc_runtime_profile){
        .rs_ohm = 0.45f,
        .ld_h = 0.0009f,
        .lq_h = 0.0013f,
        .flux_wb = 0.008f,
        .inertia_kg_m2 = 2.0e-5f,
        .damping_nm_s_per_rad = 1.0e-4f,
        .rated_current_a = 1.2f,
        .peak_current_a = 2.0f,
        .max_speed_rad_s = 80.0f,
        .gear_ratio = 9.0f,
        .current_bandwidth_hz = 120.0f,
        .velocity_bandwidth_hz = 12.0f,
        .inertia_travel_rad = 1.5f,
    };
    in.tuning.trips = (struct foc_runtime_trip_limits){
        .overcurrent_a = 3.1f,
        .overvoltage_v = 21.0f,
        .undervoltage_v = 6.0f,
        .overspeed_rad_s = 90.0f,
        .sustained_ms = 1500U,
        .stall_ms = 250U,
        .divergence_ms = 400U,
    };
    in.output_zero_rad = -2.5f;
    in.tuning.accel_rad_s2 = 40.0f;
    in.tuning.decel_rad_s2 = 120.0f;
    in.tuning.dead_time_v = 0.08f;
    in.can = (struct foc_serial_can_settings){
        .node_id = 5U,
        .bitrate = 500000U,
        .heartbeat_ms = 250U,
        .estimates_ms = 20U,
    };
    foc_board_store_encode(&nominal, &in, 7U, record);
    put_word(record, UINT32_C(0xC35A4C7E));
    CHECK(record[8] == 9U);
    struct foc_board_store_identity identity;
    struct foc_board_settings out;
    uint32_t sequence = 0U;
    CHECK(foc_board_store_decode(record, &identity, &out, &sequence) == FOC_BOARD_STORE_OK);
    CHECK(sequence == 7U);
    CHECK(memcmp(&out.tuning.profile, &in.tuning.profile, sizeof in.tuning.profile) == 0);
    CHECK(memcmp(&out.tuning.trips, &in.tuning.trips, sizeof in.tuning.trips) == 0);
    CHECK(out.output_zero_rad == -2.5f);
    CHECK(out.tuning.accel_rad_s2 == 40.0f && out.tuning.decel_rad_s2 == 120.0f);
    CHECK(out.tuning.dead_time_v == 0.08f);
    CHECK(out.can.node_id == 5U && out.can.bitrate == 500000U);
    CHECK(out.can.heartbeat_ms == 250U && out.can.estimates_ms == 20U);
    CHECK(memcmp(&identity, &nominal, sizeof nominal) == 0);
    return 0;
}

/* TLV rules: unknown IDs are ignored, missing ones reported, duplicates, mistyped entries,
 * a missing identity and the old fixed-offset formats are refused. */
static int test_tlv_rules(void)
{
    uint8_t record[FOC_BOARD_STORE_RECORD_BYTES];
    uint8_t edit[FOC_BOARD_STORE_RECORD_BYTES];
    const struct foc_board_settings in = settings_with_kp(2.0f);
    const size_t length = foc_board_store_encode(&nominal, &in, 5U, record);
    put_word(record, UINT32_C(0xC35A4C7E));
    struct foc_board_store_identity identity;
    struct foc_board_settings out;
    uint32_t sequence = 0U;

    /* An entry this firmware does not know (a newer firmware's parameter) is ignored. */
    memcpy(edit, record, sizeof edit);
    const uint8_t unknown[8] = {0x21U, 0x03U, 1U, 0U, 0x78U, 0x56U, 0x34U, 0x12U}; /* ID 801 */
    memcpy(edit + length - 4U, unknown, sizeof unknown);
    reseal(edit, length + 8U);
    CHECK(length + 8U <= FOC_BOARD_STORE_RECORD_BYTES);
    CHECK(foc_board_store_decode(edit, &identity, &out, &sequence) == FOC_BOARD_STORE_OK);
    CHECK(out.tuning.current_kp_v_per_a.q == 2.0f);

    /* A missing parameter is reported absent; the others stay present. */
    memcpy(edit, record, sizeof edit);
    (void)drop_entry(edit, find_entry(edit, FOC_PARAM_DEAD_TIME_COMP));
    CHECK(foc_board_store_decode(edit, &identity, &out, &sequence) == FOC_BOARD_STORE_OK);
    const size_t dead_time =
        (size_t)(foc_parameter_find(FOC_PARAM_DEAD_TIME_COMP) - foc_parameter_table);
    const size_t kp_q = (size_t)(foc_parameter_find(FOC_PARAM_CURRENT_KP_Q) - foc_parameter_table);
    CHECK(!foc_board_settings_present(&out, dead_time) && foc_board_settings_present(&out, kp_q));

    /* A duplicated parameter is refused. */
    memcpy(edit, record, sizeof edit);
    memcpy(edit + length - 4U, record + 20U + 8U * find_entry(record, FOC_PARAM_CURRENT_KP_Q), 8U);
    reseal(edit, length + 8U);
    CHECK(foc_board_store_decode(edit, &identity, &out, &sequence) == FOC_BOARD_STORE_ERR_RECORD);

    /* A parameter with the wrong wire type is refused. */
    memcpy(edit, record, sizeof edit);
    edit[20U + 8U * find_entry(edit, FOC_PARAM_CURRENT_KP_Q) + 2U] = 1U;
    reseal(edit, length);
    CHECK(foc_board_store_decode(edit, &identity, &out, &sequence) == FOC_BOARD_STORE_ERR_RECORD);

    /* A record without its full identity is refused. */
    memcpy(edit, record, sizeof edit);
    (void)drop_entry(edit, find_entry(edit, 0xF002U));
    CHECK(foc_board_store_decode(edit, &identity, &out, &sequence) == FOC_BOARD_STORE_ERR_RECORD);

    /* Non-zero reserved bytes and a count that disagrees with the length are refused. */
    memcpy(edit, record, sizeof edit);
    edit[20U + 3U] = 1U;
    reseal(edit, length);
    CHECK(foc_board_store_decode(edit, &identity, &out, &sequence) == FOC_BOARD_STORE_ERR_RECORD);
    memcpy(edit, record, sizeof edit);
    edit[16] ^= 1U;
    put_word(edit + length - 4U, test_crc32(edit + 4, length - 8U));
    CHECK(foc_board_store_decode(edit, &identity, &out, &sequence) == FOC_BOARD_STORE_ERR_RECORD);

    /* A committed, CRC-valid format-8 record (fixed 244 bytes) is no longer read. */
    memset(edit, 0xFF, sizeof edit);
    put_word(edit, UINT32_C(0xC35A4C7E));
    put_word(edit + 4, UINT32_C(0x31504246));
    edit[8] = 8U;
    edit[9] = 0U;
    edit[10] = 244U;
    edit[11] = 0U;
    memset(edit + 12, 0, 228U);
    put_word(edit + 240, test_crc32(edit + 4, 236U));
    CHECK(foc_board_store_decode(edit, &identity, &out, &sequence) == FOC_BOARD_STORE_ERR_RECORD);
    return 0;
}

static int test_restore_calibration(void)
{
    static struct restore_fixture fixture;
    CHECK(restore_boot(&fixture, false) == 0);
    struct foc_board_store_identity boot_identity;
    CHECK(foc_board_store_identity_from_boot(&fixture.boot, &boot_identity));

    /* A stored calibration re-initializes the instance, then the tuning applies. */
    struct foc_board_settings in = settings_with_kp(2.2f);
    in.calibration_present = true;
    in.calibration = calibrated();
    CHECK(write_record(&fixture.flash, &boot_identity, &in) == 0);
    CHECK(restore_boot(&fixture, true) == 0);
    CHECK(fixture.persistence.state == FOC_BOARD_STORE_LOADED);
    CHECK(fixture.persistence.calibration_restored);
    CHECK(fixture.runtime.config.motion.rotor.pole_pairs == 11U);
    CHECK(fixture.runtime.config.motion.rotor.electrical_offset_rad == 1.25f);
    CHECK(fixture.runtime.pipeline.adc_config.phase_b_offset_count == 2063.25f);
    CHECK(
        fixture.encoder.config_valid &&
        fixture.encoder.config.direction == FOC_ENCODER_DIRECTION_NORMAL
    );
    CHECK(fixture.runtime.pipeline.current.config.kp_v_per_a.q == 2.2f);
    CHECK(fixture.persistence.restored_calibration.pole_pairs == 11U);

    /* An inconsistent calibration (direction product) is refused and nothing changes,
     * the tuning included. */
    struct foc_board_settings bad = in;
    bad.calibration.runtime_encoder_direction = -1;
    memset(fixture.flash.slots, 0xFF, sizeof fixture.flash.slots);
    CHECK(write_record(&fixture.flash, &boot_identity, &bad) == 0);
    CHECK(restore_boot(&fixture, true) == 0);
    CHECK(fixture.persistence.state == FOC_BOARD_STORE_VALUE_REJECTED);
    CHECK(!fixture.persistence.calibration_restored);
    CHECK(fixture.runtime.config.motion.rotor.pole_pairs == 7U);
    CHECK(fixture.runtime.pipeline.current.config.kp_v_per_a.q == 1.35f);
    CHECK(!fixture.encoder.config_valid);
    return 0;
}

/* Test Entry ------------------------------------------------------------------- */

int main(void)
{
    return test_codec() || test_slots() || test_sequence_wrap() || test_restore() ||
        test_calibration_record() || test_profile_record() || test_tlv_rules() ||
        test_restore_calibration();
}

/* ---------------------------------------- end of file ----------------------------------------- */
