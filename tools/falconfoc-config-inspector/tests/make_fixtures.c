/**
 * @file    make_fixtures.c
 * @brief   Emit one real SOFT-03 Host backend snapshot to stdout for CLI tests.
 * @version v0.1.0
 * @date    2026.11.18
 *
 * Test role
 * ---------
 * Generate snapshots through production save/load and the Host fault
 * injection backend for the separate inspector executable.
 */

/* Includes --------------------------------------------------------------------- */
#include "fixture_profile.h"
#include "foc_config_store_host.h"
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

/* Private Helpers -------------------------------------------------------------- */
#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return 1;                                                       \
        }                                                                   \
    } while (0)

/* Test Entry ------------------------------------------------------------------- */
int main(int argc, char **argv)
{
    CHECK(argc == 2);
    const char *mode = argv[1];
    const bool other_encoder = strcmp(mode, "other-encoder") == 0;
    const bool interrupted = strcmp(mode, "interrupted") == 0;
    const bool ack_lost = strcmp(mode, "ack-lost") == 0;
    CHECK(other_encoder || interrupted || ack_lost || strcmp(mode, "normal") == 0);

    struct inspector_fixture_profile fixture;
    inspector_fixture_profile_init(&fixture);
    struct foc_config_store_policy policy = fixture.policy;
    if (other_encoder) {
        policy.identity.encoder_cpr /= 2U;
    }

    const struct foc_parameter_edit original_edits[] = {
        {FOC_PARAMETER_LIMITS_ENABLED, {FOC_PARAMETER_U32, {.u32 = 1U}}},
        {FOC_PARAMETER_LIMIT_POSITION_MIN, {FOC_PARAMETER_F32, {.f32 = 0.20f}}},
        {FOC_PARAMETER_LIMIT_POSITION_MAX, {FOC_PARAMETER_F32, {.f32 = 0.80f}}},
        {FOC_PARAMETER_LIMIT_TOLERANCE, {FOC_PARAMETER_F32, {.f32 = 0.005f}}},
        {FOC_PARAMETER_LIMIT_MECHANICAL_FRAME,
         {FOC_PARAMETER_U32, {.u32 = policy.identity.mechanical_frame_id}}},
        {FOC_PARAMETER_FOLLOWING_ENABLED, {FOC_PARAMETER_U32, {.u32 = 1U}}},
        {FOC_PARAMETER_FOLLOWING_THRESHOLD, {FOC_PARAMETER_F32, {.f32 = 0.10f}}},
        {FOC_PARAMETER_FOLLOWING_DURATION_US, {FOC_PARAMETER_U32, {.u32 = 20000U}}},
        {FOC_PARAMETER_FOLLOWING_MAX_GAP_US, {FOC_PARAMETER_U32, {.u32 = 1000U}}},
    };
    struct foc_parameter_candidate original;
    CHECK(
        foc_parameter_prepare_initial(
            policy.pipeline,
            policy.base,
            original_edits,
            sizeof(original_edits) / sizeof(original_edits[0]),
            &original
        ) == FOC_PARAMETER_VALIDATED
    );

    struct foc_config_store_host store;
    struct foc_config_store_workspace workspace;
    struct foc_config_store_report report;
    foc_config_store_host_init(&store);
    const struct foc_config_store_driver driver = foc_config_store_host_driver(&store);
    CHECK(
        foc_config_store_save(&driver, &policy, &workspace, &original, &report) ==
        FOC_CONFIG_STORE_OK
    );
    CHECK(report.active_slot == FOC_CONFIG_STORE_SLOT_A);
    if (!other_encoder) {
        const struct foc_parameter_edit updated_edits[] = {
            {FOC_PARAMETER_LIMIT_POSITION_MIN, {FOC_PARAMETER_F32, {.f32 = 0.30f}}},
            {FOC_PARAMETER_LIMIT_POSITION_MAX, {FOC_PARAMETER_F32, {.f32 = 0.70f}}},
            {FOC_PARAMETER_FOLLOWING_THRESHOLD, {FOC_PARAMETER_F32, {.f32 = 0.06f}}},
            {FOC_PARAMETER_FOLLOWING_DURATION_US, {FOC_PARAMETER_U32, {.u32 = 18000U}}},
        };
        struct foc_parameter_candidate updated;
        CHECK(
            foc_parameter_prepare_initial(
                policy.pipeline,
                &original.config,
                updated_edits,
                sizeof(updated_edits) / sizeof(updated_edits[0]),
                &updated
            ) == FOC_PARAMETER_VALIDATED
        );
        if (interrupted || ack_lost) {
            struct foc_config_store_host trial = store;
            const struct foc_config_store_driver trial_driver =
                foc_config_store_host_driver(&trial);
            foc_config_store_host_inject(&trial, 0U, FOC_CONFIG_STORE_HOST_BEFORE);
            CHECK(
                foc_config_store_save(&trial_driver, &policy, &workspace, &updated, &report) ==
                FOC_CONFIG_STORE_OK
            );
            /* The last two steps are commit program and commit readback. */
            foc_config_store_host_inject(
                &store,
                trial.steps - 1U,
                ack_lost ? FOC_CONFIG_STORE_HOST_AFTER : FOC_CONFIG_STORE_HOST_TORN
            );
        }
        const int status = foc_config_store_save(&driver, &policy, &workspace, &updated, &report);
        CHECK(
            status ==
            ((interrupted || ack_lost) ? FOC_CONFIG_STORE_ERR_UNCERTAIN : FOC_CONFIG_STORE_OK)
        );
    }

    foc_config_store_host_reboot(&store);
    struct foc_parameter_candidate restored;
    CHECK(
        foc_config_store_load(&driver, &policy, &workspace, &restored, &report) ==
        FOC_CONFIG_STORE_OK
    );
    CHECK(
        report.active_slot ==
        ((other_encoder || interrupted) ? FOC_CONFIG_STORE_SLOT_A : FOC_CONFIG_STORE_SLOT_B)
    );
#ifdef _WIN32
    CHECK(_setmode(_fileno(stdout), _O_BINARY) != -1);
#endif
    CHECK(fwrite(store.slots, 1U, sizeof(store.slots), stdout) == sizeof(store.slots));
    CHECK(fflush(stdout) == 0);
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
