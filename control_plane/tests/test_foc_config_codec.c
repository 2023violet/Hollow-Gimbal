/**
 * @file    test_foc_config_codec.c
 * @brief   Stable configuration record and detached recovery tests.
 * @version v0.1.0
 * @date    2026.11.18
 *
 * Test role
 * ---------
 * Exercise exact wire bytes, corruption and recovery through the public codec.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_config_store.h"
#include "crc.h"

#include <stdio.h>
#include <string.h>

/* Private Helpers -------------------------------------------------------------- */

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return 1;                                                       \
        }                                                                   \
    } while (0)

enum {
    PAYLOAD = FOC_CONFIG_STORE_HEADER_BYTES,
    CRC = PAYLOAD - 4U,
};

static void put_u32(uint8_t *bytes, uint32_t value)
{
    for (unsigned int index = 0U; index < 4U; ++index) {
        bytes[index] = (uint8_t)(value >> (8U * index));
    }
}

static void repair_crc(uint8_t *bytes)
{
    const uint32_t header = crc32_ieee(CRC32_IEEE_INIT, bytes + 4U, CRC - 4U);
    const size_t size = (size_t)bytes[16] + (size_t)bytes[17] * 256U;
    put_u32(bytes + CRC, CRC32_IEEE_FINALIZE(crc32_ieee(header, bytes + PAYLOAD, size)));
}

static void set_count(uint8_t *bytes, uint8_t count)
{
    bytes[18] = count;
    bytes[16] = (uint8_t)(count * 8U);
    repair_crc(bytes);
}

static int check_corruption(
    const struct foc_config_store_policy *policy,
    const uint8_t *committed,
    size_t size
)
{
    struct foc_parameter_candidate result = {.config.motion.iq_limit_a = 77.0f};
    uint8_t bad[FOC_CONFIG_STORE_SLOT_BYTES];
    static const struct {
        size_t offset;
        uint8_t value;
        bool repair;
        int status;
    } changes[] = {
        {4U, 0U, false, FOC_CONFIG_STORE_ERR_RECORD},
        {8U, 9U, false, FOC_CONFIG_STORE_ERR_CRC},
        {8U, 9U, true, FOC_CONFIG_STORE_ERR_FORMAT},
        {10U, 9U, true, FOC_CONFIG_STORE_ERR_SCHEMA},
        {12U, 0U, false, FOC_CONFIG_STORE_ERR_CRC},
        {16U, 0xFEU, false, FOC_CONFIG_STORE_ERR_RECORD},
        {18U, 9U, true, FOC_CONFIG_STORE_ERR_RECORD},
        {24U, 1U, true, FOC_CONFIG_STORE_ERR_IDENTITY},
        {CRC, 0U, false, FOC_CONFIG_STORE_ERR_CRC},
        {PAYLOAD + 2U, 1U, true, FOC_CONFIG_STORE_ERR_RECORD},
        {PAYLOAD + 3U, 3U, true, FOC_CONFIG_STORE_ERR_RECORD},
        {PAYLOAD + 8U, 5U, true, FOC_CONFIG_STORE_ERR_RECORD},
        {PAYLOAD + 8U, 4U, true, FOC_CONFIG_STORE_ERR_SCHEMA},
        {PAYLOAD + 12U, 2U, true, FOC_CONFIG_STORE_ERR_CONFIG},
        {PAYLOAD + 44U, 18U, true, FOC_CONFIG_STORE_ERR_IDENTITY},
        {PAYLOAD + 68U, 0U, false, FOC_CONFIG_STORE_ERR_CRC},
    };
    for (size_t index = 0U; index < sizeof(changes) / sizeof(changes[0]); ++index) {
        memcpy(bad, committed, size);
        bad[changes[index].offset] = changes[index].value;
        if (changes[index].repair) {
            repair_crc(bad);
        }
        CHECK(foc_config_store_decode(policy, bad, size, &result) == changes[index].status);
        CHECK(result.config.motion.iq_limit_a == 77.0f);
    }
    CHECK(
        foc_config_store_decode(policy, committed, size - 1U, &result) ==
        FOC_CONFIG_STORE_ERR_RECORD
    );

    const uint32_t invalid_floats[] = {0x7FC00000U, 0x7F800000U, 0xFF800000U};
    for (size_t index = 0U; index < sizeof(invalid_floats) / sizeof(invalid_floats[0]); ++index) {
        memcpy(bad, committed, size);
        put_u32(bad + PAYLOAD + 4U, invalid_floats[index]);
        repair_crc(bad);
        CHECK(foc_config_store_decode(policy, bad, size, &result) == FOC_CONFIG_STORE_ERR_RECORD);
    }
    memcpy(bad, committed, size);
    put_u32(bad + PAYLOAD + 4U, 0x40400000U);
    repair_crc(bad);
    CHECK(foc_config_store_decode(policy, bad, size, &result) == FOC_CONFIG_STORE_ERR_CONFIG);

    memcpy(bad, committed, size);
    bad[PAYLOAD + 12U] = 0U; /* Limits disabled still requires a valid complete group. */
    put_u32(bad + PAYLOAD + 20U, 0x40400000U);
    repair_crc(bad);
    CHECK(foc_config_store_decode(policy, bad, size, &result) == FOC_CONFIG_STORE_ERR_CONFIG);
    set_count(bad, 9U);
    CHECK(foc_config_store_decode(policy, bad, size, &result) == FOC_CONFIG_STORE_ERR_INCOMPLETE);
    CHECK(result.config.motion.iq_limit_a == 77.0f);
    puts("CODEC bad CRC/header/type/value/length/count/duplicate/missing/group: rejected");
    return 0;
}

static int check_extension_and_migration(
    struct foc_config_store_policy policy,
    const struct foc_parameter_candidate *confirmed,
    const uint8_t *committed,
    size_t size
)
{
    struct foc_parameter_candidate result = {.config.motion.iq_limit_a = 77.0f};
    uint8_t bytes[FOC_CONFIG_STORE_SLOT_BYTES];
    memcpy(bytes, committed, size);
    memcpy(bytes + size, "\x10\x80\x02\x04\x00\x00\x00\x80", 8U);
    memcpy(bytes + size + 8U, "\x11\x80\x04\x04\x01\x00\x00\x00", 8U);
    set_count(bytes, 12U);
    CHECK(foc_config_store_decode(&policy, bytes, size + 16U, &result) == FOC_CONFIG_STORE_OK);
    bytes[size + 8U] = 0x10U;
    repair_crc(bytes);
    CHECK(
        foc_config_store_decode(&policy, bytes, size + 16U, &result) == FOC_CONFIG_STORE_ERR_RECORD
    );

    memcpy(bytes, committed, size);
    bytes[10] = 1U;
    set_count(bytes, 1U);
    const size_t legacy_size = PAYLOAD + 8U;
    CHECK(
        foc_config_store_decode(&policy, bytes, legacy_size, &result) ==
        FOC_CONFIG_STORE_ERR_RECONFIRM
    );
    struct foc_parameter_candidate protection = *confirmed;
    protection.config.motion.limits.position_max_rad = 1.5f;
    protection.config.motion.iq_limit_a = 2.0f;
    policy.confirmed_protection = &protection;
    CHECK(
        foc_config_store_decode(&policy, bytes, legacy_size, &result) == FOC_CONFIG_STORE_MIGRATED
    );
    CHECK(result.config.motion.iq_limit_a == 1.0f);
    CHECK(result.config.motion.limits.position_max_rad == 1.5f);
    CHECK(result.config.motion.limits.enabled && result.config.motion.following_error.enabled);
    protection.config.motion.following_error.duration_us = 0U;
    CHECK(
        foc_config_store_decode(&policy, bytes, legacy_size, &result) == FOC_CONFIG_STORE_ERR_CONFIG
    );
    CHECK(result.config.motion.limits.position_max_rad == 1.5f);
    puts("CODEC optional scalar extension and separate schema 1 reconfirmation: passed");
    return 0;
}

static int check_identity(struct foc_config_store_policy policy, const uint8_t *bytes, size_t size)
{
    struct foc_parameter_candidate result = {.config.motion.iq_limit_a = 77.0f};
    const struct foc_config_store_identity identity = policy.identity;
    const struct foc_runtime_config *base = policy.base;
    policy.identity.encoder_cpr = 2048U;
    CHECK(foc_config_store_decode(&policy, bytes, size, &result) == FOC_CONFIG_STORE_ERR_IDENTITY);
    policy.identity = identity;
    policy.identity.encoder_direction = 1;
    CHECK(foc_config_store_decode(&policy, bytes, size, &result) == FOC_CONFIG_STORE_ERR_IDENTITY);
    policy.identity = identity;
    policy.identity.encoder_identity++;
    CHECK(foc_config_store_decode(&policy, bytes, size, &result) == FOC_CONFIG_STORE_ERR_IDENTITY);
    policy.identity = identity;
    policy.identity.motor_setup_id++;
    CHECK(foc_config_store_decode(&policy, bytes, size, &result) == FOC_CONFIG_STORE_ERR_IDENTITY);
    policy.identity = identity;
    policy.identity.encoder_identity = 0U;
    CHECK(
        foc_config_store_decode(&policy, bytes, size, &result) ==
        FOC_CONFIG_STORE_ERR_IDENTITY_UNKNOWN
    );
    policy.identity = identity;
    struct foc_runtime_config changed = *base;
    policy.base = &changed;
    changed.motion.rotor.pole_pairs = ++policy.identity.pole_pairs;
    CHECK(foc_config_store_decode(&policy, bytes, size, &result) == FOC_CONFIG_STORE_ERR_IDENTITY);
    policy.identity = identity;
    changed = *base;
    changed.motion.rotor.electrical_offset_rad += 0.125f;
    CHECK(foc_config_store_decode(&policy, bytes, size, &result) == FOC_CONFIG_STORE_ERR_IDENTITY);
    changed = *base;
    changed.motion.limits.mechanical_frame_id = ++policy.identity.mechanical_frame_id;
    CHECK(foc_config_store_decode(&policy, bytes, size, &result) == FOC_CONFIG_STORE_ERR_IDENTITY);
    CHECK(result.config.motion.iq_limit_a == 77.0f);
    puts("CODEC CPR/direction/poles/motor/encoder/frame/offset/unknown identity: rejected");
    return 0;
}

static int wire_contract(void)
{
    const struct foc_pipeline_config pipeline = {.current.current_limit_a = 2.5f};
    const struct foc_runtime_config config = {
        .h0.period_us = 100U,
        .h1.period_us = 200U,
        .motion = {
            .enabled = true,
            .rotor = {7U, 0.25f},
            .iq_limit_a = 1.0f,
            .limits = {-1.0f, 2.0f, 0.125f, 17U, true},
            .following_error = {0.125f, 400U, 200U, true},
        },
    };
    const struct foc_config_store_policy policy = {
        .pipeline = &pipeline,
        .base = &config,
        .identity = {4096U, -1, 17U, 51U, 61U, 7U},
    };
    const struct foc_parameter_candidate input = {.config = config};
    uint8_t bytes[FOC_CONFIG_STORE_SLOT_BYTES];
    memset(bytes, 0xA5, sizeof(bytes));
    size_t size = 3U;
    CHECK(
        foc_config_store_encode(&policy, &input, 17U, bytes, 55U, &size) ==
        FOC_CONFIG_STORE_ERR_CAPACITY
    );
    CHECK(size == 3U);
    for (size_t index = 0U; index < sizeof(bytes); ++index) {
        CHECK(bytes[index] == 0xA5U);
    }
    CHECK(
        foc_config_store_encode(&policy, &input, 0x01020304U, bytes, sizeof(bytes), &size) ==
        FOC_CONFIG_STORE_OK
    );
    CHECK(size == 136U);
    const uint8_t golden[] = "\xff\xff\xff\xff"
                             "FCFG"
                             "\x01\x00\x02\x00\x04\x03\x02\x01"
                             "\x50\x00\x0a\x00\x07\x00\x00\x00\x00\x10\x00\x00\xff\xff\xff\xff"
                             "\x11\x00\x00\x00\x33\x00\x00\x00\x00\x00\x80\x3e\x3d\x00\x00\x00"
                             "\x00\x00\x00\x00\xe6\xe7\x77\x4f"
                             "\x05\x10\x03\x04\x00\x00\x80\x3f\x0b\x10\x01\x04\x01\x00\x00\x00"
                             "\x0c\x10\x03\x04\x00\x00\x80\xbf\x0d\x10\x03\x04\x00\x00\x00\x40"
                             "\x0e\x10\x03\x04\x00\x00\x00\x3e\x0f\x10\x01\x04\x11\x00\x00\x00"
                             "\x10\x10\x01\x04\x01\x00\x00\x00\x11\x10\x03\x04\x00\x00\x00\x3e"
                             "\x12\x10\x01\x04\x90\x01\x00\x00\x13\x10\x01\x04\xc8\x00\x00\x00";
    CHECK(size == sizeof(golden) - 1U && memcmp(bytes, golden, size) == 0);
    CHECK(
        memcmp(bytes + FOC_CONFIG_STORE_HEADER_BYTES, "\x05\x10\x03\x04\x00\x00\x80\x3f", 8U) == 0
    );
    CHECK(
        CRC32_IEEE_FINALIZE(crc32_ieee(CRC32_IEEE_INIT, (const uint8_t *)"123456789", 9U)) ==
        0xcbf43926U
    );
    struct foc_config_store_record record;
    CHECK(foc_config_store_inspect(bytes, size, &record) == FOC_CONFIG_STORE_ERR_UNCOMMITTED);
    memcpy(bytes, "\x7e\x4c\x5a\xc3", 4U);
    CHECK(foc_config_store_inspect(bytes, size, &record) == FOC_CONFIG_STORE_OK);
    CHECK(record.sequence == 0x01020304U && record.schema == 2U && record.count == 10U);
    struct foc_parameter_candidate recovered = {0};
    CHECK(foc_config_store_decode(&policy, bytes, size, &recovered) == FOC_CONFIG_STORE_OK);
    CHECK(
        recovered.config.motion.limits.enabled && recovered.config.motion.following_error.enabled
    );
    CHECK(recovered.config.motion.limits.position_max_rad == 2.0f);
    CHECK(check_corruption(&policy, bytes, size) == 0);
    CHECK(check_extension_and_migration(policy, &input, bytes, size) == 0);
    CHECK(check_identity(policy, bytes, size) == 0);
    puts("CODEC golden record CRC=0x4F77E7E6 length=136, standard CRC=0xCBF43926");
    return 0;
}

/* Test Entry ------------------------------------------------------------------- */

int main(void)
{
    return wire_contract();
}

/* ---------------------------------------- end of file ----------------------------------------- */
