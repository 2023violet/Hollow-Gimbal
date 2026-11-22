/**
 * @file    test_foc_board_store.c
 * @brief   Board1 persistent record: codec, slot selection, torn saves and boot restore.
 * @version v0.1.0
 * @date    2026.11.22
 *
 * Test role
 * ---------
 * Drive the production store through a fake NOR device with power-loss cuts,
 * stuck cells and damaged slots, and the boot restore through the production
 * boot selection.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_board.h"
#include "foc_board_boot.h"
#include "foc_board_store.h"

#include <math.h>
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
            },
        .rx_budget = 24U,
        .remote_current_cap_a = 2.5f,
        .remote_position_cap_rad = 100.0f,
    };
}

/* Tests ------------------------------------------------------------------------ */

static int test_codec(void)
{
    uint8_t record[FOC_BOARD_STORE_RECORD_BYTES];
    const struct foc_board_settings in = settings_with_kp(2.0f);
    foc_board_store_encode(&nominal, &in, 7U, record);
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
    /* Any flipped byte in the covered range fails the CRC. */
    for (size_t index = 4U; index < FOC_BOARD_STORE_RECORD_BYTES; ++index) {
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

    /* Damaged storage reports INVALID_RECORD and keeps the boot configuration. */
    memset(fixture.flash.slots, 0x00, sizeof fixture.flash.slots);
    CHECK(restore_boot(&fixture, true) == 0);
    CHECK(fixture.persistence.state == FOC_BOARD_STORE_INVALID_RECORD);
    fixture.flash.fail_read = true;
    CHECK(restore_boot(&fixture, true) == 0);
    CHECK(fixture.persistence.state == FOC_BOARD_STORE_IO_ERROR);
    return 0;
}

/* Test Entry ------------------------------------------------------------------- */

int main(void)
{
    return test_codec() || test_slots() || test_sequence_wrap() || test_restore();
}

/* ---------------------------------------- end of file ----------------------------------------- */
