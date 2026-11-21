/**
 * @file    falconfoc_virtual_calibrate_timing.c
 * @brief   Report Host calibration progress and separate native time sources.
 * @version v0.1.0
 * @date    2026.11.20
 *
 * Implementation role
 * -------------------
 * Observe serial CLI stages without changing the calibration workflow.
 */

#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

/* Includes --------------------------------------------------------------------- */

#include "falconfoc_virtual_calibrate_internal.h"

#include <inttypes.h>
#include <stdio.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
#endif

/* Private Constants ------------------------------------------------------------ */

static const char *const stage_names[CAL_CLI_STAGE_COUNT] = {
    [CAL_CLI_BOOTSTRAP_ADC] = "BOOTSTRAP_ADC",
    [CAL_CLI_BOOTSTRAP_SCAN] = "BOOTSTRAP_SCAN",
    [CAL_CLI_BOOTSTRAP_FIT] = "BOOTSTRAP_FIT",
    [CAL_CLI_LIVE_ADC] = "LIVE_ADC_CHECK",
    [CAL_CLI_LIVE_ENCODER] = "LIVE_ENCODER_CHECK",
    [CAL_CLI_REUSE_PLAN] = "REUSE_PLAN",
    [CAL_CLI_REUSE_RUNTIME] = "REUSE_RUNTIME_INIT",
    [CAL_CLI_QUIESCENT] = "QUIESCENT_CHECK",
    [CAL_CLI_COGGING] = "COGGING",
    [CAL_CLI_GAIN_CANDIDATE] = "GAIN_CANDIDATE",
    [CAL_CLI_REVIEW] = "REVIEW",
    [CAL_CLI_APPLICATION] = "APPLICATION",
    [CAL_CLI_CONTROL_DIAGNOSTIC] = "CONTROL_DIAGNOSTIC",
    [CAL_CLI_CONTROL_SAFETY] = "CONTROL_SAFETY",
    [CAL_CLI_CONTROL_HOLDOUT] = "CONTROL_HOLDOUT",
    [CAL_CLI_POST_APPLY_QUIESCENT] = "POST_APPLY_QUIESCENT",
    [CAL_CLI_FCAL_SAVE] = "FCAL_SAVE",
    [CAL_CLI_FCAL_LOAD] = "FCAL_LOAD",
};

static const char *const status_names[] = {
    [CAL_CLI_PENDING] = "PENDING",
    [CAL_CLI_RUNNING] = "RUNNING",
    [CAL_CLI_COMPLETED] = "COMPLETED",
    [CAL_CLI_FAILED] = "FAILED",
    [CAL_CLI_REUSED] = "REUSED",
    [CAL_CLI_SKIPPED] = "SKIPPED",
};

/* Private Helpers -------------------------------------------------------------- */

#if defined(_WIN32)
static uint64_t host_wall_us(void)
{
    LARGE_INTEGER ticks;
    LARGE_INTEGER frequency;
    if (!QueryPerformanceCounter(&ticks) || !QueryPerformanceFrequency(&frequency) ||
        frequency.QuadPart <= 0) {
        return UINT64_MAX;
    }
    const uint64_t hz = (uint64_t)frequency.QuadPart;
    const uint64_t count = (uint64_t)ticks.QuadPart;
    return count / hz * UINT64_C(1000000) + count % hz * UINT64_C(1000000) / hz;
}

static uint64_t file_time_ticks(FILETIME time)
{
    return ((uint64_t)time.dwHighDateTime << 32U) | time.dwLowDateTime;
}

static uint64_t host_compute_us(void)
{
    FILETIME created;
    FILETIME exited;
    FILETIME kernel;
    FILETIME user;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        return UINT64_MAX;
    }
    return (file_time_ticks(kernel) + file_time_ticks(user)) / 10U;
}
#else
static uint64_t clock_us(clockid_t clock)
{
    struct timespec stamp;
    if (clock_gettime(clock, &stamp) != 0) {
        return UINT64_MAX;
    }
    return (uint64_t)stamp.tv_sec * UINT64_C(1000000) + (uint64_t)stamp.tv_nsec / 1000U;
}

static uint64_t host_wall_us(void)
{
    return clock_us(CLOCK_MONOTONIC);
}

static uint64_t host_compute_us(void)
{
    return clock_us(CLOCK_PROCESS_CPUTIME_ID);
}
#endif

static uint64_t elapsed_us(uint64_t start, uint64_t end)
{
    return end == UINT64_MAX || start > end ? UINT64_MAX : end - start;
}

static const char *stage_name(enum foc_virtual_cal_cli_stage stage)
{
    return stage <= CAL_CLI_TUNE
        ? foc_virtual_calibration_stage_name((enum foc_virtual_calibration_stage)stage)
        : stage_names[stage];
}

static void print_duration(const char *field, uint64_t value)
{
    printf(",\"%s\":", field);
    if (value == UINT64_MAX) {
        fputs("null", stdout);
    } else {
        printf("%" PRIu64, value);
    }
}

static void print_stages(
    const struct foc_virtual_cal_cli_timing *timing,
    const char *field,
    enum foc_virtual_cal_cli_stage_status status
)
{
    printf(",\"%s\":[", field);
    const char *separator = "";
    for (unsigned int i = 0U; i < CAL_CLI_STAGE_COUNT; ++i) {
        if ((timing->planned & CAL_CLI_BIT(i)) != 0U && timing->states[i] == status) {
            printf("%s\"%s\"", separator, stage_name((enum foc_virtual_cal_cli_stage)i));
            separator = ",";
        }
    }
    putchar(']');
}

static void print_progress(
    const struct foc_virtual_cal_cli_timing *timing,
    enum foc_virtual_cal_cli_stage stage,
    uint64_t wall,
    uint64_t compute,
    int64_t simulated_motor_us
)
{
    unsigned int total = 0U;
    for (unsigned int i = 0U; i < CAL_CLI_STAGE_COUNT; ++i) {
        total += (timing->planned & CAL_CLI_BIT(i)) != 0U;
    }
    const bool reviewed = timing->states[CAL_CLI_REVIEW] == CAL_CLI_COMPLETED;
    printf(
        "{\"event\":\"CAL_STAGE\",\"mode\":\"%s\",\"stage\":\"%s\",\"status\":\"%s\","
        "\"total_stages\":%u,\"reviewed\":%s,\"can_apply\":%s",
        timing->mode,
        stage_name(stage),
        status_names[timing->states[stage]],
        total,
        reviewed ? "true" : "false",
        reviewed && timing->states[CAL_CLI_APPLICATION] == CAL_CLI_PENDING ? "true" : "false"
    );
    print_duration("host_wall_us", wall);
    print_duration("host_compute_us", compute);
    print_duration("simulated_motor_us",
                   simulated_motor_us < 0 ? UINT64_MAX : (uint64_t)simulated_motor_us);
    fputs(",\"user_interaction_us\":null,\"eta_us\":null", stdout);
    print_stages(timing, "completed_stages", CAL_CLI_COMPLETED);
    print_stages(timing, "reused_stages", CAL_CLI_REUSED);
    print_stages(timing, "skipped_stages", CAL_CLI_SKIPPED);
    print_stages(timing, "failed_stages", CAL_CLI_FAILED);
    print_stages(timing, "remaining_stages", CAL_CLI_PENDING);
    puts("}");
    fflush(stdout);
}

/* Public Functions ------------------------------------------------------------- */

void foc_virtual_cal_cli_timing_reset(struct foc_virtual_cal_cli_timing *timing, uint32_t planned)
{
    if (timing != NULL) {
        *timing = (struct foc_virtual_cal_cli_timing){.mode = timing->mode, .planned = planned};
    }
}

void foc_virtual_cal_cli_timing_start(
    struct foc_virtual_cal_cli_timing *timing,
    enum foc_virtual_cal_cli_stage stage
)
{
    if (timing != NULL) {
        timing->planned |= CAL_CLI_BIT(stage);
        timing->states[stage] = CAL_CLI_RUNNING;
        print_progress(timing, stage, 0U, 0U, 0);
        timing->host_wall_start_us = host_wall_us();
        timing->host_compute_start_us = host_compute_us();
    }
}

void foc_virtual_cal_cli_timing_finish(
    struct foc_virtual_cal_cli_timing *timing,
    enum foc_virtual_cal_cli_stage stage,
    bool success,
    int64_t simulated_motor_us
)
{
    if (timing != NULL) {
        const uint64_t compute = elapsed_us(timing->host_compute_start_us, host_compute_us());
        const uint64_t wall = elapsed_us(timing->host_wall_start_us, host_wall_us());
        timing->states[stage] = success ? CAL_CLI_COMPLETED : CAL_CLI_FAILED;
        print_progress(timing, stage, wall, compute, simulated_motor_us);
    }
}

void foc_virtual_cal_cli_timing_omit(
    struct foc_virtual_cal_cli_timing *timing,
    uint32_t stages,
    enum foc_virtual_cal_cli_stage_status status
)
{
    if (timing != NULL) {
        timing->planned |= stages;
        for (unsigned int i = 0U; i < CAL_CLI_STAGE_COUNT; ++i) {
            if ((stages & CAL_CLI_BIT(i)) != 0U) {
                timing->states[i] = status;
                print_progress(timing, (enum foc_virtual_cal_cli_stage)i, 0U, 0U, 0);
            }
        }
    }
}

/* ---------------------------------------- end of file ----------------------------------------- */
