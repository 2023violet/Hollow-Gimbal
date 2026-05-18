/**
 * @file    tm_export.h
 * @brief   Themis Static Init Export Table
 * @version v5.0.0
 * @date    2026.03.22
 *
 * Design overview
 * ---------------
 * Themis stores init hooks in Flash-only export sections and walks them in
 * stage + priority order at boot time.
 *
 * Stage model:
 * - BOARD_EARLY        : pure board / hardware pre-kernel hooks
 * - PLATFORM           : OS-free foundation hooks
 * - PRE_KERNEL_DRIVER  : OS-free early drivers
 * - DRIVER             : post-kernel driver registration and glue
 * - SUBSYS             : framework subsystems
 * - SERVICE            : middleware / shared services
 * - DEVICE             : late device binding and board default instances
 * - ENV                : environment attach / mount / backend binding
 * - APP                : product application bootstrap
 * - USER               : final project-private extension point
 *
 * Hook signatures:
 * - int  func(void)
 * - void func(void)
 * - int  func(void *ctx)
 * - void func(void *ctx)
 *
 * Automatic export hooks intentionally do not support arbitrary runtime
 * parameter lists. Export items are static link-time descriptors, so only a
 * fixed callback signature plus an optional static context pointer is stable
 * across toolchains and boot stages.
 *
 * Ordering contract:
 * - level asc
 * - priority asc inside one level
 * - linker address asc as the final tiebreaker
 *
 * The address tiebreak keeps identical level/priority hooks in the same
 * deterministic order as the linker arranged them in Flash. Exit walks the
 * same order in reverse.
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include <stdbool.h>
#include <stdint.h>
#include "tmdef.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Public Macros -------------------------------------------------------------------------------- */

#define EXPORT_ID_INIT                (0xA5A5A5A5U)
#define EXPORT_ID_EXIT                (0x5A5A5A5AU)

/*
 * Exit Export Section
 * -------------------
 * This switch must stay aligned with the linker script / scatter file.
 * The current Themis project keeps .export_exit disabled, so exit-section
 * symbols must not be referenced unless the section is explicitly enabled.
 */
#define TM_EXPORT_EXIT_SECTION_ENABLE (0U)

/*
 * Init failure policy
 * -------------------
 * Boot-time init hooks are fail-fast by default so later stages do not keep
 * executing on top of a half-initialized system. Projects that explicitly want
 * "record first error but continue" may override this macro before including
 * this header.
 */
#ifndef TM_EXPORT_INIT_FAIL_FAST
#define TM_EXPORT_INIT_FAIL_FAST (1U)
#endif

/*
 * Export Stages
 * -------------
 * Themis now uses explicit stage macros instead of enum values so the boot
 * contract is easier to freeze in documents, linker notes, and cross-module
 * checks.
 */
#define EXPORT_LEVEL_BOARD_EARLY       (0U)
#define EXPORT_LEVEL_PLATFORM          (1U)
#define EXPORT_LEVEL_PRE_KERNEL_DRIVER (2U)
#define EXPORT_LEVEL_DRIVER            (3U)
#define EXPORT_LEVEL_SUBSYS            (4U)
#define EXPORT_LEVEL_SERVICE           (5U)
#define EXPORT_LEVEL_DEVICE            (6U)
#define EXPORT_LEVEL_ENV               (7U)
#define EXPORT_LEVEL_APP               (8U)
#define EXPORT_LEVEL_USER              (9U)
#define EXPORT_LEVEL_MAX               (10U)

/*
 * Compatibility Aliases
 * ---------------------
 * Keep the old names visible so existing code and documents do not break
 * immediately while the project is migrating to the more explicit vocabulary.
 */
#define EXPORT_LEVEL_BSP               EXPORT_LEVEL_BOARD_EARLY
#define EXPORT_LEVEL_HW_INDEPENDENT    EXPORT_LEVEL_PLATFORM
#define EXPORT_LEVEL_MIDWARE           EXPORT_LEVEL_SERVICE

/*
 * Common Stage Ranges
 * -------------------
 * These macros make the split boot chain explicit at call sites.
 */
#define EXPORT_LEVEL_FIRST             EXPORT_LEVEL_BOARD_EARLY
#define EXPORT_LEVEL_LAST              EXPORT_LEVEL_USER
#define EXPORT_LEVEL_PRE_KERNEL_FIRST  EXPORT_LEVEL_BOARD_EARLY
#define EXPORT_LEVEL_PRE_KERNEL_LAST   EXPORT_LEVEL_PRE_KERNEL_DRIVER
#define EXPORT_LEVEL_POST_KERNEL_FIRST EXPORT_LEVEL_DRIVER
#define EXPORT_LEVEL_POST_KERNEL_LAST  EXPORT_LEVEL_USER

/*
 * Export Priorities
 * -----------------
 * Smaller value means earlier execution inside the same stage.
 * When level and priority are identical, Themis preserves the original link
 * order by comparing the item address stored in the export section.
 */
#define EXPORT_PRIO_HIGHEST            (0U)
#define EXPORT_PRIO_HIGH               (64U)
#define EXPORT_PRIO_DEFAULT            (128U)
#define EXPORT_PRIO_LOW                (192U)
#define EXPORT_PRIO_LOWEST             (255U)

/*
 * Export Item Flags
 * -----------------
 * Context and return-signature information are encoded explicitly so the
 * executor can support both status-returning hooks and legacy void hooks.
 */
#define EXPORT_FLAG_EXIT               (0x01U)
#define EXPORT_FLAG_CONTEXT            (0x02U)
#define EXPORT_FLAG_VOID               (0x04U)

/* Public Types --------------------------------------------------------------------------------- */

typedef int (*export_int_func_t)(void);
typedef int (*export_int_ctx_func_t)(void *ctx);
typedef void (*export_void_func_t)(void);
typedef void (*export_void_ctx_func_t)(void *ctx);

union export_func_ref {
    void                  *obj;
    export_int_func_t      int_fn;
    export_int_ctx_func_t  int_ctx_fn;
    export_void_func_t     void_fn;
    export_void_ctx_func_t void_ctx_fn;
};

struct tm_export_last_error {
    /** Name of the last failing init hook. */
    const char *name;
    /** Stage level of the last failing init hook. */
    uint8_t level;
    /** Priority inside the failing stage. */
    uint8_t priority;
    /** Status returned by the failing hook. */
    int status;
};

/**
 * @brief  One item stored in the init/exit export sections.
 */
struct export_item {
    /** Validation header magic. */
    uint32_t magic_head;
    /** Function name string used for diagnostics and tracing. */
    const char *name;
    /** Stored callback function pointer. */
    union export_func_ref func;
    /** Optional static context pointer. */
    void *ctx;
    /** Execution stage. */
    uint8_t level;
    /** Execution priority inside one stage. */
    uint8_t priority;
    /** Exit / context / void-signature flags. */
    uint8_t flags;
    /** Padding reserved for alignment. */
    uint8_t reserved;
    /** Validation tail magic. */
    uint32_t magic_tail;
};

/* Public Macros -------------------------------------------------------------------------------- */

/**
 * @brief  Register a legacy void init hook into the export table.
 *
 * @param  _func   Function with signature `void func(void)`.
 * @param  _level  One export stage macro.
 */
#define INIT_EXPORT(_func, _level) INIT_EXPORT_VOID_NAMED(_func, _func, _level, EXPORT_PRIO_DEFAULT)

/**
 * @brief  Register a legacy void exit hook into the export table.
 *
 * @param  _func   Function with signature `void func(void)`.
 * @param  _level  One export stage macro.
 */
#define EXIT_EXPORT(_func, _level) EXIT_EXPORT_VOID_NAMED(_func, _func, _level, EXPORT_PRIO_DEFAULT)

#define _EXPORT_REGISTER(_symbol, _name, _func_member, _func, _ctx, _level, _priority, _flags, _magic, _section) \
    TM_USED static const struct export_item _symbol TM_SECTION(_section) = {                                        \
        .magic_head = (_magic),                                                                                     \
        .name       = #_name,                                                                                       \
        .func       = {. _func_member = (_func)},                                                                   \
        .ctx        = (void *)(_ctx),                                                                               \
        .level      = (uint8_t)(_level),                                                                            \
        .priority   = (uint8_t)(_priority),                                                                         \
        .flags      = (uint8_t)(_flags),                                                                            \
        .reserved   = 0U,                                                                                           \
        .magic_tail = (_magic),                                                                                     \
    }

#define INIT_EXPORT_VOID_NAMED(_name, _func, _level, _priority) \
    _EXPORT_REGISTER(_export_init_##_name,                      \
                     _name,                                     \
                     void_fn,                                   \
                     _func,                                     \
                     NULL,                                      \
                     _level,                                    \
                     _priority,                                 \
                     EXPORT_FLAG_VOID,                          \
                     EXPORT_ID_INIT,                            \
                     ".export_init")

#define INIT_EXPORT_INT_NAMED(_name, _func, _level, _priority) \
    _EXPORT_REGISTER(_export_init_##_name,                     \
                     _name,                                    \
                     int_fn,                                   \
                     _func,                                    \
                     NULL,                                     \
                     _level,                                   \
                     _priority,                                \
                     0U,                                       \
                     EXPORT_ID_INIT,                           \
                     ".export_init")

#define INIT_EXPORT_VOID_CTX(_name, _func, _ctx, _level, _priority) \
    _EXPORT_REGISTER(_export_init_##_name,                          \
                     _name,                                         \
                     void_ctx_fn,                                   \
                     _func,                                         \
                     _ctx,                                          \
                     _level,                                        \
                     _priority,                                     \
                     (EXPORT_FLAG_VOID | EXPORT_FLAG_CONTEXT),      \
                     EXPORT_ID_INIT,                                \
                     ".export_init")

#define INIT_EXPORT_INT_CTX(_name, _func, _ctx, _level, _priority) \
    _EXPORT_REGISTER(_export_init_##_name,                         \
                     _name,                                        \
                     int_ctx_fn,                                   \
                     _func,                                        \
                     _ctx,                                         \
                     _level,                                       \
                     _priority,                                    \
                     EXPORT_FLAG_CONTEXT,                          \
                     EXPORT_ID_INIT,                               \
                     ".export_init")

#define EXIT_EXPORT_VOID_NAMED(_name, _func, _level, _priority) \
    _EXPORT_REGISTER(_export_exit_##_name,                      \
                     _name,                                     \
                     void_fn,                                   \
                     _func,                                     \
                     NULL,                                      \
                     _level,                                    \
                     _priority,                                 \
                     (EXPORT_FLAG_VOID | EXPORT_FLAG_EXIT),     \
                     EXPORT_ID_EXIT,                            \
                     ".export_exit")

#define EXIT_EXPORT_INT_NAMED(_name, _func, _level, _priority) \
    _EXPORT_REGISTER(_export_exit_##_name,                     \
                     _name,                                    \
                     int_fn,                                   \
                     _func,                                    \
                     NULL,                                     \
                     _level,                                   \
                     _priority,                                \
                     EXPORT_FLAG_EXIT,                         \
                     EXPORT_ID_EXIT,                           \
                     ".export_exit")

#define EXIT_EXPORT_VOID_CTX(_name, _func, _ctx, _level, _priority)               \
    _EXPORT_REGISTER(_export_exit_##_name,                                        \
                     _name,                                                       \
                     void_ctx_fn,                                                 \
                     _func,                                                       \
                     _ctx,                                                        \
                     _level,                                                      \
                     _priority,                                                   \
                     (EXPORT_FLAG_VOID | EXPORT_FLAG_CONTEXT | EXPORT_FLAG_EXIT), \
                     EXPORT_ID_EXIT,                                              \
                     ".export_exit")

#define EXIT_EXPORT_INT_CTX(_name, _func, _ctx, _level, _priority) \
    _EXPORT_REGISTER(_export_exit_##_name,                         \
                     _name,                                        \
                     int_ctx_fn,                                   \
                     _func,                                        \
                     _ctx,                                         \
                     _level,                                       \
                     _priority,                                    \
                     (EXPORT_FLAG_CONTEXT | EXPORT_FLAG_EXIT),     \
                     EXPORT_ID_EXIT,                               \
                     ".export_exit")

#define INIT_EXPORT_VOID(_func, _level, _priority) \
    INIT_EXPORT_VOID_NAMED(_func, _func, _level, _priority)

#define INIT_EXPORT_INT(_func, _level, _priority) \
    INIT_EXPORT_INT_NAMED(_func, _func, _level, _priority)

#define EXIT_EXPORT_VOID(_func, _level, _priority) \
    EXIT_EXPORT_VOID_NAMED(_func, _func, _level, _priority)

#define EXIT_EXPORT_INT(_func, _level, _priority) \
    EXIT_EXPORT_INT_NAMED(_func, _func, _level, _priority)

/*
 * Stage-Specific Convenience Wrappers
 * ----------------------------------
 * These mirror the style used by large RTOS projects and make call sites more
 * self-describing than a raw generic INIT_EXPORT(..., level).
 */
#define BOARD_EXPORT(_func)             INIT_EXPORT(_func, EXPORT_LEVEL_BOARD_EARLY)
#define PLATFORM_EXPORT(_func)          INIT_EXPORT(_func, EXPORT_LEVEL_PLATFORM)
#define PRE_KERNEL_DRIVER_EXPORT(_func) INIT_EXPORT(_func, EXPORT_LEVEL_PRE_KERNEL_DRIVER)
#define DRIVER_EXPORT(_func)            INIT_EXPORT(_func, EXPORT_LEVEL_DRIVER)
#define SUBSYS_EXPORT(_func)            INIT_EXPORT(_func, EXPORT_LEVEL_SUBSYS)
#define SERVICE_EXPORT(_func)           INIT_EXPORT(_func, EXPORT_LEVEL_SERVICE)
#define DEVICE_EXPORT(_func)            INIT_EXPORT(_func, EXPORT_LEVEL_DEVICE)
#define ENV_EXPORT(_func)               INIT_EXPORT(_func, EXPORT_LEVEL_ENV)
#define APP_EXPORT(_func)               INIT_EXPORT(_func, EXPORT_LEVEL_APP)
#define USER_EXPORT(_func)              INIT_EXPORT(_func, EXPORT_LEVEL_USER)

/* Public Functions ----------------------------------------------------------------------------- */

/**
 * @brief  Execute init hooks from FIRST up to @p max_level.
 *
 * @details
 * This is the legacy prefix executor kept for backward compatibility.
 * Calling `export_init_execute(EXPORT_LEVEL_APP)` means:
 *   BOARD_EARLY -> ... -> APP.
 *
 * For staged boot, prefer export_init_execute_range() instead.
 *
 * @param  max_level  Highest init level to execute (inclusive).
 *
 * @retval 0          Success or only legacy void hooks were executed.
 * @retval non-zero   First non-zero status returned by a status hook.
 *
 * @note   When `TM_EXPORT_INIT_FAIL_FAST != 0`, execution stops immediately on
 *         the first failing init hook.
 */
int export_init_execute(uint8_t max_level);

/**
 * @brief  Execute only the init hooks inside one inclusive level range.
 *
 * @param  min_level  Lowest level to execute.
 * @param  max_level  Highest level to execute.
 *
 * @retval 0          Success or only legacy void hooks were executed.
 * @retval non-zero   First non-zero status returned by a status hook.
 *
 * @note   When `TM_EXPORT_INIT_FAIL_FAST != 0`, execution stops immediately on
 *         the first failing init hook.
 */
int export_init_execute_range(uint8_t min_level, uint8_t max_level);

/**
 * @brief  Execute init hooks from @p min_level through USER level.
 *
 * @param  min_level  Lowest level to execute.
 *
 * @retval 0          Success or only legacy void hooks were executed.
 * @retval non-zero   First non-zero status returned by a status hook.
 *
 * @note   When `TM_EXPORT_INIT_FAIL_FAST != 0`, execution stops immediately on
 *         the first failing init hook.
 */
int export_init_execute_from(uint8_t min_level);

/**
 * @brief  Read the last failing init hook recorded by the executor.
 *
 * @param  info  Output buffer. Must not be NULL.
 *
 * @return `true` when a failing hook has been recorded; otherwise `false`.
 */
bool export_init_last_error_get(struct tm_export_last_error *info);

/**
 * @brief  Execute exit hooks from USER level down to @p min_level.
 *
 * @param  min_level  Lowest level to execute during reverse teardown.
 *
 * @retval 0          Success or only legacy void hooks were executed.
 * @retval non-zero   First non-zero status returned by a status hook.
 */
int export_exit_execute(uint8_t min_level);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
