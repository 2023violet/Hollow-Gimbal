/**
 * @file    test_foc_config_store.c
 * @brief   Dual-slot fault injection and recovery checks.
 * @version v0.1.0
 * @date    2026.11.18
 *
 * Test role
 * ---------
 * Interrupt all actual device steps and compare every restored static value.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_config_store_host.h"

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

static const struct foc_pipeline_config pipeline = {
    .current = {{1.0f, 2.4f, 0.0f, 1.0f, 0.0003f}, {1.08f, 1.35f}, {540.0f, 540.0f}, 2.5f},
    .adc = {0.01f, 0.002f, 1.0f, 2048.0f, 2048.0f, 2048.0f, FOC_DEVICE_PHASE_ORDER_ABC},
    .pwm = {6000U, FOC_DEVICE_PHASE_ORDER_ABC},
    .safety = {5000U, 500U, 350U, 350U, 1U},
};

static const struct foc_runtime_config base = {
    .h0 = {100U, 20U, 10U},
    .h1 = {200U, 20U, 10U},
    .supervision = {100U, 20U, 10U},
    .telemetry = {10000U, 20U, 10U},
    .foreground_budget_us = 15U,
    .angle_timeout_us = 150U,
    .motion = {
        .enabled = true,
        .rotor = {7U, 0.25f},
        .prediction = {150U, 2000.0f, 0.001f},
        .velocity = {0.18f, 1.4f, 0.002f},
        .position = {4.0f, 12.0f},
        .iq_limit_a = 1.25f,
        .impedance = {true, 17U, 0.25f, 0.4f, 1.0f, 0.3f, 0.1f},
        .limits = {-1.0f, 2.0f, 0.125f, 17U, true},
        .following_error = {0.125f, 400U, 200U, true},
    },
};

static const struct foc_config_store_policy policy = {
    .pipeline = &pipeline,
    .base = &base,
    .identity = {4096U, -1, 17U, 51U, 61U, 7U},
};

static bool same_configuration(
    const struct foc_parameter_candidate *left,
    const struct foc_parameter_candidate *right
)
{
    for (size_t index = 0U; index < foc_parameter_count(); ++index) {
        const struct foc_parameter_descriptor *desc = foc_parameter_at(index);
        if (desc->access != FOC_PARAMETER_INITIAL_CONFIG) {
            continue;
        }
        struct foc_parameter_value a, b;
        if (foc_parameter_read_config(&left->config, desc->id, &a) != FOC_PARAMETER_OK ||
            foc_parameter_read_config(&right->config, desc->id, &b) != FOC_PARAMETER_OK ||
            memcmp(&a.data, &b.data, sizeof(a.data)) != 0) {
            return false;
        }
    }
    return true;
}

static int power_cuts(void)
{
    struct foc_config_store_host baseline;
    foc_config_store_host_init(&baseline);
    const struct foc_config_store_driver writer = foc_config_store_host_driver(&baseline);
    struct foc_config_store_workspace workspace;
    struct foc_config_store_report report;
    struct foc_parameter_candidate discard;
    CHECK(
        foc_config_store_load(&writer, &policy, &workspace, &discard, &report) ==
        FOC_CONFIG_STORE_EMPTY
    );
    const struct foc_parameter_candidate initial = {.config = base};
    CHECK(
        foc_config_store_save(&writer, &policy, &workspace, &initial, &report) ==
        FOC_CONFIG_STORE_OK
    );
    CHECK(report.active_slot == FOC_CONFIG_STORE_SLOT_A && report.record.sequence == 0U);
    struct foc_parameter_candidate old = initial;
    old.config.motion.limits.position_max_rad = 1.0f;
    CHECK(
        foc_config_store_save(&writer, &policy, &workspace, &old, &report) == FOC_CONFIG_STORE_OK
    );
    CHECK(report.active_slot == FOC_CONFIG_STORE_SLOT_B && report.record.sequence == 1U);

    struct foc_parameter_candidate next = initial;
    next.config.motion.iq_limit_a = 0.75f;
    next.config.motion.impedance.torque_limit_nm = 0.2f;
    next.config.motion.impedance.max_stiffness_nm_per_rad = 0.5f;
    next.config.motion.impedance.max_damping_nm_s_per_rad = 0.1f;
    next.config.motion.impedance.max_feedforward_nm = 0.05f;
    next.config.motion.limits.position_min_rad = -0.5f;
    next.config.motion.limits.position_max_rad = 1.5f;
    next.config.motion.limits.enabled = false;
    next.config.motion.limits.tolerance_rad = 0.25f;
    next.config.motion.following_error.threshold_rad = 0.25f;
    next.config.motion.following_error.duration_us = 800U;
    next.config.motion.following_error.max_gap_us = 400U;
    next.config.motion.following_error.enabled = false;
    struct foc_config_store_host trial = baseline;
    const struct foc_config_store_driver driver = foc_config_store_host_driver(&trial);
    foc_config_store_host_inject(&trial, 0U, FOC_CONFIG_STORE_HOST_BEFORE);
    CHECK(
        foc_config_store_save(&driver, &policy, &workspace, &next, &report) == FOC_CONFIG_STORE_OK
    );
    const uint32_t steps = trial.steps;
    CHECK(steps == 110U && report.active_slot == FOC_CONFIG_STORE_SLOT_A);
    unsigned old_seen = 0U, new_seen = 0U;
    for (int cut = FOC_CONFIG_STORE_HOST_BEFORE; cut <= FOC_CONFIG_STORE_HOST_AFTER; ++cut) {
        for (uint32_t step = 1U; step <= steps; ++step) {
            trial = baseline;
            foc_config_store_host_inject(&trial, step, (enum foc_config_store_host_cut)cut);
            const int saved = foc_config_store_save(&driver, &policy, &workspace, &next, &report);
            CHECK(saved == FOC_CONFIG_STORE_ERR_IO || saved == FOC_CONFIG_STORE_ERR_UNCERTAIN);
            CHECK(memcmp(trial.slots[1], baseline.slots[1], sizeof(trial.slots[1])) == 0);
            foc_config_store_host_reboot(&trial);
            struct foc_parameter_candidate loaded;
            CHECK(
                foc_config_store_load(&driver, &policy, &workspace, &loaded, &report) ==
                FOC_CONFIG_STORE_OK
            );
            const bool committed =
                step == steps || (step == steps - 1U && cut == FOC_CONFIG_STORE_HOST_AFTER);
            CHECK(report.record.sequence == (committed ? 2U : 1U));
            CHECK(
                report.active_slot ==
                (committed ? FOC_CONFIG_STORE_SLOT_A : FOC_CONFIG_STORE_SLOT_B)
            );
            CHECK(same_configuration(&loaded, committed ? &next : &old));
            if (committed) {
                new_seen++;
                CHECK(
                    foc_config_store_save(&driver, &policy, &workspace, &next, &report) ==
                    FOC_CONFIG_STORE_OK
                );
                CHECK(report.record.sequence == 2U);
                CHECK(memcmp(trial.slots[1], baseline.slots[1], sizeof(trial.slots[1])) == 0);
            } else {
                old_seen++;
            }
            printf(
                "CUT kind=%d step=%lu save=%d recovered=%s sequence=%u\n",
                cut,
                (unsigned long)step,
                saved,
                committed ? "new" : "old",
                committed ? 2U : 1U
            );
        }
    }
    printf(
        "POWER_FAIL steps=%lu cases=%u old=%u new=%u\n",
        (unsigned long)steps,
        old_seen + new_seen,
        old_seen,
        new_seen
    );
    return 0;
}

static void
record(const struct foc_parameter_candidate *candidate, uint32_t sequence, uint8_t *slot)
{
    size_t size = 0U;
    memset(slot, 0xFF, FOC_CONFIG_STORE_SLOT_BYTES);
    if (foc_config_store_encode(
            &policy,
            candidate,
            sequence,
            slot,
            FOC_CONFIG_STORE_SLOT_BYTES,
            &size
        ) == FOC_CONFIG_STORE_OK) {
        memcpy(slot, "\x7e\x4c\x5a\xc3", 4U);
    }
}

static void fix_crc(uint8_t *bytes)
{
    const size_t crc_offset = FOC_CONFIG_STORE_HEADER_BYTES - 4U;
    uint32_t crc = crc32_ieee(CRC32_IEEE_INIT, bytes + 4U, crc_offset - 4U);
    crc = CRC32_IEEE_FINALIZE(crc32_ieee(crc, bytes + FOC_CONFIG_STORE_HEADER_BYTES, bytes[16]));
    for (unsigned int index = 0U; index < 4U; ++index) {
        bytes[crc_offset + index] = (uint8_t)(crc >> (8U * index));
    }
}

static int slot_selection(void)
{
    struct foc_config_store_host host;
    foc_config_store_host_init(&host);
    const struct foc_config_store_driver driver = foc_config_store_host_driver(&host);
    struct foc_config_store_workspace workspace;
    struct foc_config_store_report report;
    const struct foc_parameter_candidate old = {.config = base};
    struct foc_parameter_candidate next = old;
    next.config.motion.limits.position_max_rad = 1.0f;
    struct foc_parameter_candidate result;

    record(&old, UINT32_MAX, host.slots[0]);
    record(&next, 0U, host.slots[1]);
    CHECK(
        foc_config_store_load(&driver, &policy, &workspace, &result, &report) == FOC_CONFIG_STORE_OK
    );
    CHECK(
        report.active_slot == FOC_CONFIG_STORE_SLOT_B && report.record.sequence == 0U &&
        same_configuration(&result, &next)
    );
    record(&old, UINT32_MAX, host.slots[0]);
    memset(host.slots[1], 0xFF, FOC_CONFIG_STORE_SLOT_BYTES);
    CHECK(
        foc_config_store_save(&driver, &policy, &workspace, &next, &report) == FOC_CONFIG_STORE_OK
    );
    CHECK(report.active_slot == FOC_CONFIG_STORE_SLOT_B && report.record.sequence == 0U);

    record(&old, 10U, host.slots[0]);
    record(&old, 10U, host.slots[1]);
    CHECK(
        foc_config_store_load(&driver, &policy, &workspace, &result, &report) ==
            FOC_CONFIG_STORE_OK &&
        report.active_slot == FOC_CONFIG_STORE_SLOT_A
    );
    record(&next, 10U, host.slots[1]);
    CHECK(
        foc_config_store_load(&driver, &policy, &workspace, &result, &report) ==
        FOC_CONFIG_STORE_ERR_SEQUENCE
    );
    record(&next, 0x8000000AU, host.slots[1]);
    CHECK(
        foc_config_store_save(&driver, &policy, &workspace, &next, &report) ==
        FOC_CONFIG_STORE_ERR_SEQUENCE
    );

    record(&next, 11U, host.slots[1]);
    host.slots[1][8] = 2U;
    CHECK(
        foc_config_store_load(&driver, &policy, &workspace, &result, &report) == FOC_CONFIG_STORE_OK
    );
    CHECK(
        report.active_slot == FOC_CONFIG_STORE_SLOT_A &&
        report.slot_status[1] == FOC_CONFIG_STORE_ERR_CRC && same_configuration(&result, &old)
    );
    fix_crc(host.slots[1]);
    CHECK(
        foc_config_store_save(&driver, &policy, &workspace, &next, &report) ==
        FOC_CONFIG_STORE_ERR_FORMAT
    );
    record(&next, 11U, host.slots[1]);
    host.slots[1][10] = 3U;
    fix_crc(host.slots[1]);
    CHECK(
        foc_config_store_load(&driver, &policy, &workspace, &result, &report) ==
        FOC_CONFIG_STORE_ERR_SCHEMA
    );
    CHECK(report.active_slot == FOC_CONFIG_STORE_SLOT_B && same_configuration(&result, &old));

    record(&next, 11U, host.slots[1]);
    host.slots[1][36] ^= 1U;
    fix_crc(host.slots[1]);
    CHECK(
        foc_config_store_load(&driver, &policy, &workspace, &result, &report) ==
        FOC_CONFIG_STORE_ERR_IDENTITY
    );
    CHECK(
        foc_config_store_save(&driver, &policy, &workspace, &next, &report) ==
        FOC_CONFIG_STORE_ERR_IDENTITY
    );
    host.slots[0][FOC_CONFIG_STORE_HEADER_BYTES] ^= 1U;
    host.slots[1][FOC_CONFIG_STORE_HEADER_BYTES] ^= 1U;
    CHECK(
        foc_config_store_load(&driver, &policy, &workspace, &result, &report) ==
        FOC_CONFIG_STORE_ERR_NO_VALID
    );
    CHECK(
        foc_config_store_save(&driver, &policy, &workspace, &next, &report) ==
        FOC_CONFIG_STORE_ERR_NO_VALID
    );
    CHECK(same_configuration(&result, &old));
    puts("STORAGE serial wrap/tie/half-range, corrupt fallback and foreign-format refusal: passed");
    return 0;
}

static int migrated_save(void)
{
    struct foc_config_store_host host;
    foc_config_store_host_init(&host);
    const struct foc_config_store_driver driver = foc_config_store_host_driver(&host);
    const struct foc_parameter_candidate initial = {.config = base};
    record(&initial, 7U, host.slots[0]);
    host.slots[0][10] = FOC_CONFIG_STORE_SCHEMA_LEGACY;
    host.slots[0][16] = 40U;
    host.slots[0][18] = 5U;
    fix_crc(host.slots[0]);
    const struct foc_config_store_host previous = host;
    struct foc_config_store_workspace workspace;
    struct foc_config_store_report report;
    struct foc_parameter_candidate loaded;
    CHECK(
        foc_config_store_load(&driver, &policy, &workspace, &loaded, &report) ==
        FOC_CONFIG_STORE_ERR_RECONFIRM
    );
    struct foc_config_store_policy reviewed = policy;
    reviewed.confirmed_protection = &initial;
    CHECK(
        foc_config_store_load(&driver, &reviewed, &workspace, &loaded, &report) ==
        FOC_CONFIG_STORE_MIGRATED
    );
    CHECK(
        report.record.schema == 1U && report.record.sequence == 7U &&
        same_configuration(&loaded, &initial)
    );
    CHECK(memcmp(host.slots, previous.slots, sizeof(host.slots)) == 0);
    CHECK(
        foc_config_store_save(&driver, &reviewed, &workspace, &loaded, &report) ==
        FOC_CONFIG_STORE_OK
    );
    CHECK(
        report.active_slot == FOC_CONFIG_STORE_SLOT_B && report.record.schema == 2U &&
        report.record.sequence == 8U &&
        memcmp(host.slots[0], previous.slots[0], sizeof(host.slots[0])) == 0
    );
    CHECK(
        foc_config_store_load(&driver, &policy, &workspace, &loaded, &report) ==
            FOC_CONFIG_STORE_OK &&
        same_configuration(&loaded, &initial)
    );
    puts("STORAGE old schema read-only migration and separate explicit save: passed");
    return 0;
}

static int device_rules(void)
{
    struct foc_config_store_host host;
    foc_config_store_host_init(&host);
    const struct foc_config_store_driver driver = foc_config_store_host_driver(&host);
    const uint8_t zero[4] = {0U, 0U, 0U, 0U};
    const uint8_t erased[4] = {0xFFU, 0xFFU, 0xFFU, 0xFFU};
    uint8_t output[4];
    CHECK(driver.erase(driver.context, 2U) != 0);
    CHECK(
        driver.read(driver.context, 0U, FOC_CONFIG_STORE_SLOT_BYTES, output, sizeof(output)) != 0
    );
    CHECK(driver.program(driver.context, 0U, 2U, zero, sizeof(zero)) != 0);
    CHECK(driver.program(driver.context, 0U, 4U, zero, 3U) != 0);
    CHECK(host.steps == 0U);
    CHECK(driver.program(driver.context, 0U, 4U, zero, sizeof(zero)) == 0);
    CHECK(driver.program(driver.context, 0U, 4U, erased, sizeof(erased)) != 0);
    CHECK(
        driver.read(driver.context, 0U, 4U, output, sizeof(output)) == 0 &&
        memcmp(output, zero, sizeof(output)) == 0
    );
    CHECK(driver.erase(driver.context, 0U) == 0);
    CHECK(
        driver.read(driver.context, 0U, 4U, output, sizeof(output)) == 0 &&
        memcmp(output, erased, sizeof(output)) == 0
    );
    puts("DEVICE geometry and forbidden bit transitions: passed");
    return 0;
}

static int runtime_cycle(struct foc_runtime *runtime, uint32_t now, enum foc_runtime_action action)
{
    if (now % 200U == 0U) {
        const struct foc_runtime_sensor sample = {
            .theta_elec_rad = 0.25f,
            .captured_us = now,
            .seq = now / 200U,
            .mechanical_frame_id = 17U,
            .valid = true,
            .electrical_phase_valid = true,
        };
        if (foc_runtime_h1_step(runtime, &sample, now, 0U) != FOC_RUNTIME_OK) {
            return FOC_RUNTIME_ERR_INPUT;
        }
    }
    const struct foc_runtime_fast_input sample = {
        .adc = {1200U, 2048U, 2048U, 2048U, FOC_DEVICE_SAMPLE_ALL_VALID, now / 100U, now},
        .now_us = now,
        .action = action,
    };
    struct foc_runtime_foreground_result foreground;
    const int status = foc_runtime_h0_step(runtime, &sample);
    (void)foc_runtime_foreground_poll(runtime, now, 0U, 0U, &foreground);
    return status;
}

static int running_gate(void)
{
    struct foc_config_store_host host;
    foc_config_store_host_init(&host);
    const struct foc_config_store_driver driver = foc_config_store_host_driver(&host);
    struct foc_config_store_workspace workspace;
    struct foc_config_store_report report;
    struct foc_runtime_config motion = base;
    motion.motion.limits.enabled = false;
    motion.motion.following_error.enabled = false;
    struct foc_runtime runtime;
    CHECK(foc_runtime_init(&runtime, &pipeline, &motion, 0U) == FOC_RUNTIME_OK);
    CHECK(runtime_cycle(&runtime, 0U, FOC_RUNTIME_ACTION_NONE) == FOC_RUNTIME_IDLE);
    const struct foc_parameter_edit iq = {
        FOC_PARAMETER_MOTION_IQ_LIMIT,
        {FOC_PARAMETER_F32, {.f32 = 0.75f}}
    };
    struct foc_runtime_snapshot view;
    CHECK(
        foc_runtime_read_snapshot(&runtime, &view) == FOC_RUNTIME_OK &&
        view.state == FOC_SAFE_STATE_DISABLED && view.output_confirmed
    );
    CHECK(
        foc_config_store_save_active(
            &driver,
            &pipeline,
            &policy.identity,
            &runtime,
            view.motion_session + 1U,
            &iq,
            1U,
            &workspace,
            &report
        ) == FOC_CONFIG_STORE_ERR_GATE
    );
    CHECK(report.parameter_status == FOC_PARAMETER_ERR_SESSION && host.steps == 0U);
    CHECK(
        foc_config_store_save_active(
            &driver,
            &pipeline,
            &policy.identity,
            &runtime,
            view.motion_session,
            &iq,
            1U,
            &workspace,
            &report
        ) == FOC_CONFIG_STORE_OK
    );
    CHECK(runtime.config.motion.iq_limit_a == 1.25f && report.record.sequence == 0U);
    struct foc_runtime_snapshot after;
    CHECK(
        foc_runtime_read_snapshot(&runtime, &after) == FOC_RUNTIME_OK && after.output_confirmed &&
        after.action == FOC_DEVICE_OUTPUT_INHIBIT && after.motion_session == view.motion_session &&
        after.updates == 0U
    );

    const struct foc_pipeline_command command = {{0.0f, 0.1f}, 1U, 1U};
    CHECK(foc_runtime_publish_command(&runtime, &command, 1U) == FOC_RUNTIME_OK);
    const struct foc_runtime_lifecycle_request arm = {
        .action = FOC_RUNTIME_ACTION_ARM,
        .request_sequence = 1U,
        .source_session = view.motion_session,
        .produced_us = 1U,
    };
    CHECK(foc_runtime_publish_lifecycle_request(&runtime, &arm, 1U) == FOC_RUNTIME_OK);
    const uint32_t before = host.steps;
    CHECK(
        foc_config_store_save_active(
            &driver,
            &pipeline,
            &policy.identity,
            &runtime,
            view.motion_session,
            &iq,
            1U,
            &workspace,
            &report
        ) == FOC_CONFIG_STORE_ERR_GATE
    );
    CHECK(report.parameter_status == FOC_PARAMETER_ERR_BUSY && host.steps == before);
    CHECK(runtime_cycle(&runtime, 100U, FOC_RUNTIME_ACTION_NONE) == FOC_RUNTIME_IDLE);
    CHECK(runtime_cycle(&runtime, 200U, FOC_RUNTIME_ACTION_START) == FOC_RUNTIME_IDLE);
    CHECK(runtime_cycle(&runtime, 300U, FOC_RUNTIME_ACTION_NONE) == FOC_RUNTIME_OK);
    CHECK(
        foc_config_store_save_active(
            &driver,
            &pipeline,
            &policy.identity,
            &runtime,
            view.motion_session,
            &iq,
            1U,
            &workspace,
            &report
        ) == FOC_CONFIG_STORE_ERR_GATE
    );
    CHECK(report.parameter_status == FOC_PARAMETER_ERR_STATE && host.steps == before);
    foc_runtime_request_inhibit(&runtime, FOC_SAFE_FAULT_WDT);
    CHECK(runtime_cycle(&runtime, 400U, FOC_RUNTIME_ACTION_NONE) == FOC_RUNTIME_ERR_STOP);
    CHECK(
        foc_config_store_save_active(
            &driver,
            &pipeline,
            &policy.identity,
            &runtime,
            view.motion_session,
            &iq,
            1U,
            &workspace,
            &report
        ) == FOC_CONFIG_STORE_ERR_GATE
    );
    CHECK(report.parameter_status == FOC_PARAMETER_ERR_FAULT && host.steps == before);
    puts("ACTIVE_SAVE DISABLED success, stale Session/pending/RUNNING/FAULT refused: passed");
    return 0;
}

/* Test Entry ------------------------------------------------------------------- */

int main(void)
{
    CHECK(device_rules() == 0);
    CHECK(power_cuts() == 0);
    CHECK(slot_selection() == 0);
    CHECK(migrated_save() == 0);
    CHECK(running_gate() == 0);
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
