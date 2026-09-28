/**
 * @file    tm_core_config.h
 * @brief   FalconFoc-lite core configuration
 * @version v1.0.0
 * @date    2026.05.13
 *
 * Live scope
 * ----------
 * This file is the single live config owner for the FalconFoc-lite runtime.
 * It intentionally keeps only the compile-time knobs that still belong to the
 * current FOC image: profile, flavor, RTOS budget, shell budget, log/assert,
 * IPC, daemon, and console/telemetry policy.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Build Profile ------------------------------------------------------------------------------- */

#if !defined(TM_BUILD_PROFILE_TINY) && !defined(TM_BUILD_PROFILE_STANDARD) && \
    !defined(TM_BUILD_PROFILE_FULL_DIAG)
#define TM_BUILD_PROFILE_FULL_DIAG 1
#endif

#if defined(TM_BUILD_PROFILE_TINY)
#define TM_DEVDRV_PROFILE_NAME      "tiny"
#define TM_DEVDRV_PROFILE_TINY      (1U)
#define TM_DEVDRV_PROFILE_STANDARD  (0U)
#define TM_DEVDRV_PROFILE_FULL_DIAG (0U)
#elif defined(TM_BUILD_PROFILE_FULL_DIAG)
#define TM_DEVDRV_PROFILE_NAME      "full-diag"
#define TM_DEVDRV_PROFILE_TINY      (0U)
#define TM_DEVDRV_PROFILE_STANDARD  (0U)
#define TM_DEVDRV_PROFILE_FULL_DIAG (1U)
#else
#define TM_DEVDRV_PROFILE_NAME      "standard"
#define TM_DEVDRV_PROFILE_TINY      (0U)
#define TM_DEVDRV_PROFILE_STANDARD  (1U)
#define TM_DEVDRV_PROFILE_FULL_DIAG (0U)
#endif

/* Product Flavor ------------------------------------------------------------------------------ */

#if !defined(TM_PRODUCT_FLAVOR_CARRIER_DEMO) && !defined(TM_PRODUCT_FLAVOR_HEADLESS_TINY)
#define TM_PRODUCT_FLAVOR_CARRIER_DEMO 1
#endif

#if defined(TM_PRODUCT_FLAVOR_HEADLESS_TINY)
#define TM_PRODUCT_FLAVOR_NAME             "headless-tiny"
#define TM_PRODUCT_FLAVOR_IS_CARRIER_DEMO  (0U)
#define TM_PRODUCT_FLAVOR_IS_HEADLESS_TINY (1U)
#else
#define TM_PRODUCT_FLAVOR_NAME             "carrier-demo"
#define TM_PRODUCT_FLAVOR_IS_CARRIER_DEMO  (1U)
#define TM_PRODUCT_FLAVOR_IS_HEADLESS_TINY (0U)
#endif

/* Feature Toggles ----------------------------------------------------------------------------- */

#if !defined(TM_FEATURE_SHELL)
#if TM_DEVDRV_PROFILE_FULL_DIAG
#define TM_FEATURE_SHELL (1U)
#else
#define TM_FEATURE_SHELL (0U)
#endif
#endif

#if !defined(TM_FEATURE_VOFA)
#define TM_FEATURE_VOFA (1U)
#endif

#if !defined(TM_BENCH_DIAG)
#define TM_BENCH_DIAG (0U)
#endif

#if (TM_BENCH_DIAG != 0U)
#undef TM_FEATURE_VOFA
#define TM_FEATURE_VOFA (0U)
#endif

#if !defined(TM_FEATURE_IPC)
#define TM_FEATURE_IPC (0U)
#endif

#if !defined(TM_FEATURE_TASK_BUDGET)
#define TM_FEATURE_TASK_BUDGET (0U)
#endif

#if !defined(TM_FEATURE_VERBOSE_LOG)
#define TM_FEATURE_VERBOSE_LOG (0U)
#endif

/* Device/Driver Profile Markers --------------------------------------------------------------- */

#if !defined(TM_DEVDRV_ENABLE_TYPED_STATS)
#define TM_DEVDRV_ENABLE_TYPED_STATS ((TM_DEVDRV_PROFILE_TINY != 0U) ? 0U : 1U)
#endif

#if !defined(TM_DEVDRV_ENABLE_EXTENDED_DIAG)
#define TM_DEVDRV_ENABLE_EXTENDED_DIAG ((TM_DEVDRV_PROFILE_FULL_DIAG != 0U) ? 1U : 0U)
#endif

/* Application Surface ------------------------------------------------------------------------- */

#if !defined(TM_APP_FW_VERSION_STRING)
#define TM_APP_FW_VERSION_STRING "v1.0.0"
#endif

#if !defined(TM_APP_FEATURE_UART_TELEMETRY)
#define TM_APP_FEATURE_UART_TELEMETRY (0U)
#endif

#if !defined(TM_APP_FEATURE_UART_PASSIVE_TELEMETRY)
#define TM_APP_FEATURE_UART_PASSIVE_TELEMETRY (0U)
#endif

#if !defined(TM_APP_FEATURE_BUDGET_PROBE)
#define TM_APP_FEATURE_BUDGET_PROBE (TM_FEATURE_TASK_BUDGET)
#endif

#if !defined(TM_TASK_PERF_ENABLE)
#define TM_TASK_PERF_ENABLE (0U)
#endif

/* RTOS Budget --------------------------------------------------------------------------------- */

#if !defined(TM_FREERTOS_USE_TIMERS)
#if TM_DEVDRV_PROFILE_TINY
#define TM_FREERTOS_USE_TIMERS (0U)
#else
#define TM_FREERTOS_USE_TIMERS (1U)
#endif
#endif

#if !defined(TM_FREERTOS_USE_OS2_TIMER)
#if TM_FREERTOS_USE_TIMERS
#define TM_FREERTOS_USE_OS2_TIMER (1U)
#else
#define TM_FREERTOS_USE_OS2_TIMER (0U)
#endif
#endif

#if !defined(TM_FREERTOS_USE_OS2_EVENTFLAGS_FROM_ISR)
#if TM_FREERTOS_USE_TIMERS
#define TM_FREERTOS_USE_OS2_EVENTFLAGS_FROM_ISR (1U)
#else
#define TM_FREERTOS_USE_OS2_EVENTFLAGS_FROM_ISR (0U)
#endif
#endif

#if !defined(TM_FREERTOS_INCLUDE_TIMER_PEND_FUNCTION_CALL)
#if TM_FREERTOS_USE_TIMERS
#define TM_FREERTOS_INCLUDE_TIMER_PEND_FUNCTION_CALL (1U)
#else
#define TM_FREERTOS_INCLUDE_TIMER_PEND_FUNCTION_CALL (0U)
#endif
#endif

#if !defined(TM_FREERTOS_HEAP_SIZE_BYTES)
#define TM_FREERTOS_HEAP_SIZE_BYTES (10U * 1024U)
#endif

#if !defined(TM_FREERTOS_MINIMAL_STACK_WORDS)
#if TM_DEVDRV_PROFILE_TINY
#define TM_FREERTOS_MINIMAL_STACK_WORDS (64U)
#else
#define TM_FREERTOS_MINIMAL_STACK_WORDS (128U)
#endif
#endif

/* Shell Budget -------------------------------------------------------------------------------- */

#if !defined(TM_SHELL_TASK_STACK_BYTES)
#define TM_SHELL_TASK_STACK_BYTES (192U * 4U)
#endif

#if !defined(TM_SHELL_TASK_QUEUE_DEPTH)
#define TM_SHELL_TASK_QUEUE_DEPTH (3U)
#endif

#if !defined(TM_APP_TASK_STACK_SHELL_BYTES)
#if defined(TM_BENCH_DIAG) && (TM_BENCH_DIAG != 0U)
#define TM_APP_TASK_STACK_SHELL_BYTES (512U * 4U)
#else
#define TM_APP_TASK_STACK_SHELL_BYTES (384U * 4U)
#endif
#endif

/* Core Limits --------------------------------------------------------------------------------- */

#define TM_NAME_MAX        (16U)
#define TM_ALIGN_SIZE      (8U)
#define TM_TICK_PER_SECOND (1000U)

/* Core Features ------------------------------------------------------------------------------- */

#define TM_USING_HEAP        (0U)
#define TM_USING_CMSIS_RTOS2 (1U)

/* Assert -------------------------------------------------------------------------------------- */

#if TM_DEVDRV_PROFILE_FULL_DIAG
#define TM_ASSERT_ENABLE (1U)
#else
#define TM_ASSERT_ENABLE (0U)
#endif
#define TM_ASSERT_USE_ERROR_MAP (1U)

/* Log ----------------------------------------------------------------------------------------- */

#if TM_FEATURE_VERBOSE_LOG
#define TM_LOG_ENABLE             (1U)
#define TM_LOG_BUFFER_SIZE        (192U)
#define TM_LOG_LEVEL_DEFAULT      TM_LOG_LEVEL_DEBUG
#define TM_LOG_LEVEL_COMPILED_MAX TM_LOG_LEVEL_DEBUG
#else
#define TM_LOG_ENABLE             (0U)
#define TM_LOG_BUFFER_SIZE        (128U)
#define TM_LOG_LEVEL_DEFAULT      TM_LOG_LEVEL_INFO
#define TM_LOG_LEVEL_COMPILED_MAX TM_LOG_LEVEL_INFO
#endif

#define TM_LOG_COLOR_ENABLE     (1U)
#define TM_LOG_LOCK_ENABLE      (1U)
#define TM_LOG_TIMESTAMP_ENABLE (1U)
#define TM_LOG_HEADER_ENABLE    (1U)

/* IPC ----------------------------------------------------------------------------------------- */

#if (TM_FEATURE_IPC == 0U)
#define TM_IPC_ENABLE          (0U)
#define TM_IPC_SLAB_TINY_CNT   (0U)
#define TM_IPC_SLAB_SMALL_CNT  (0U)
#define TM_IPC_SLAB_MEDIUM_CNT (0U)
#define TM_IPC_SLAB_LARGE_CNT  (0U)
#elif TM_DEVDRV_PROFILE_FULL_DIAG
#define TM_IPC_ENABLE          (1U)
#define TM_IPC_SLAB_TINY_CNT   (12U)
#define TM_IPC_SLAB_SMALL_CNT  (8U)
#define TM_IPC_SLAB_MEDIUM_CNT (4U)
#define TM_IPC_SLAB_LARGE_CNT  (2U)
#else
#define TM_IPC_ENABLE          (1U)
#define TM_IPC_SLAB_TINY_CNT   (8U)
#define TM_IPC_SLAB_SMALL_CNT  (4U)
#define TM_IPC_SLAB_MEDIUM_CNT (2U)
#define TM_IPC_SLAB_LARGE_CNT  (1U)
#endif

#if !defined(TM_IPC_DEFER_DEPTH)
#if (TM_FEATURE_IPC == 0U)
#define TM_IPC_DEFER_DEPTH (0U)
#elif TM_DEVDRV_PROFILE_FULL_DIAG
#define TM_IPC_DEFER_DEPTH (16U)
#else
#define TM_IPC_DEFER_DEPTH (8U)
#endif
#endif

#define TM_IPC_BROKER_STACK_SIZE (512U)
#define TM_IPC_BROKER_PRIORITY   osPriorityRealtime
#define TM_IPC_NAME_LEN          (16U)

/* Daemon -------------------------------------------------------------------------------------- */

#if TM_DEVDRV_PROFILE_TINY
#define TM_DAEMON_ENABLE       (1U)
#define TM_DAEMON_STATS_ENABLE (0U)
#elif TM_DEVDRV_PROFILE_FULL_DIAG
#define TM_DAEMON_ENABLE       (1U)
#define TM_DAEMON_STATS_ENABLE (1U)
#else
#define TM_DAEMON_ENABLE       (1U)
#define TM_DAEMON_STATS_ENABLE (0U)
#endif

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
