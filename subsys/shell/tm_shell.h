/**
 * @file    tm_shell.h
 * @brief   Themis shell command registry and line-dispatch contract
 * @version v4.1.0
 * @date    2026.03.28
 *
 * Design overview
 * ---------------
 * Commands are exported into a dedicated linker section by
 * `SHELL_CMD_EXPORT()`. The runtime engine walks that section directly, so
 * command registration is static, deterministic, and heap-free.
 *
 * The shell core intentionally stays project-agnostic:
 * - priority is only a numeric sort key
 * - category strings come from each command descriptor
 * - transport output is installed by the caller through a print callback
 * - the engine does not embed project-specific command groups or board facts
 *
 * Usage example
 * -------------
 * static int shell_cmd_ping(int argc, char *argv[])
 * {
 *     (void)argc;
 *     (void)argv;
 *     shell_print("pong");
 *     return SHELL_OK;
 * }
 *
 * SHELL_CMD_EXPORT(ping, shell_cmd_ping, 50U, "ping", "diag");
 *
 * shell_init();
 * shell_set_print(uart_shell_print);
 * shell_process_line("ping", 4U);
 *
 * Ownership / lifecycle / context
 * --------------------------------
 * - `shell_init()` resets the module to a no-op output backend.
 * - `shell_set_print()` installs a borrowed transport callback; the shell core
 *   never owns the UART/USB/console backend itself.
 * - `SHELL_CMD_EXPORT()` descriptors live in static storage for the whole image
 *   lifetime.
 * - Public APIs are task-context only and assume a single shell owner task, or
 *   an equivalent external serialization rule supplied by the caller.
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Return Codes --------------------------------------------------------------------------------- */

/** Command completed successfully. */
#define SHELL_OK              (0)
/** Command rejected due to wrong argument count. */
#define SHELL_ERR_ARGC        (1)
/** Command rejected due to invalid argument content. */
#define SHELL_ERR_ARG         (2)
/** Command rejected because runtime state is not ready. */
#define SHELL_ERR_STATE       (3)
/** Command executed but reported a generic failure. */
#define SHELL_ERR_CMD         (4)

/* Configuration -------------------------------------------------------------------------------- */

/** Maximum token count parsed from one input line. */
#define SHELL_ARGC_MAX        (12U)
/** Maximum accepted input line length, excluding the terminating NUL. */
#define SHELL_LINE_MAX        (128U)
/** Prompt emitted after each processed command line. */
#define SHELL_PROMPT          "\r\n> "
/** Scratch buffer size used by shell_printf(). */
#define SHELL_PRINTF_BUF_SIZE (256U)
/** Maximum exported commands listed by the built-in help walker. */
#define SHELL_CMD_MAX         (32U)

/* Command Descriptor --------------------------------------------------------------------------- */

/** Magic sentinel used to validate linker-section entries. */
#define SHELL_CMD_MAGIC       (0x53484C4CU)

/** Standard shell command callback signature. */
typedef int (*shell_cmd_fn_t)(int argc, char *argv[]);

/**
 * @brief One statically exported shell command descriptor.
 */
struct shell_cmd_entry {
    /** Must remain `SHELL_CMD_MAGIC`. */
    uint32_t magic;
    /** Numeric help-list ordering key; smaller values sort first. */
    uint32_t priority;
    /** Borrowed command name string. */
    const char *name;
    /** Command callback executed in task context. */
    shell_cmd_fn_t func;
    /** Borrowed usage/help summary. */
    const char *help;
    /** Borrowed grouping string for the built-in help list. */
    const char *category;
};

/* Export Macro --------------------------------------------------------------------------------- */

#if defined(__IAR_SYSTEMS_ICC__)
/**
 * @brief Export one command descriptor into the linker-managed shell table.
 *
 * All string pointers and the callback function are borrowed static-storage
 * objects that must stay valid for the full image lifetime.
 */
#define SHELL_CMD_EXPORT(_name, _func, _prio, _help, _cat)                                       \
    __attribute__((used, aligned(4), section(".shell_cmd"))) static const struct shell_cmd_entry \
        _shell_cmd_##_name = {                                                                   \
            .magic    = SHELL_CMD_MAGIC,                                                         \
            .priority = (_prio),                                                                 \
            .name     = #_name,                                                                  \
            .func     = (_func),                                                                 \
            .help     = (_help),                                                                 \
            .category = (_cat),                                                                  \
    };                                                                                           \
    _Pragma("required=_shell_cmd_" #_name)
#else
/**
 * @brief Export one command descriptor into the linker-managed shell table.
 *
 * All string pointers and the callback function are borrowed static-storage
 * objects that must stay valid for the full image lifetime.
 */
#define SHELL_CMD_EXPORT(_name, _func, _prio, _help, _cat)                                       \
    __attribute__((used, aligned(4), section(".shell_cmd"))) static const struct shell_cmd_entry \
        _shell_cmd_##_name = {                                                                   \
            .magic    = SHELL_CMD_MAGIC,                                                         \
            .priority = (_prio),                                                                 \
            .name     = #_name,                                                                  \
            .func     = (_func),                                                                 \
            .help     = (_help),                                                                 \
            .category = (_cat),                                                                  \
    }
#endif

/* Output Function ------------------------------------------------------------------------------ */

/**
 * @brief Shell transport output hook.
 *
 * The shell core passes already-formatted text fragments to this callback. The
 * callback is borrowed and must stay valid until replaced by another
 * `shell_set_print()` call.
 */
typedef void (*shell_print_fn_t)(const char *str, uint16_t len);

/* Public API ----------------------------------------------------------------------------------- */

/**
 * @brief  Reset the shell engine to its default no-op output backend.
 *
 * Registered commands remain intact; only the borrowed output callback is
 * returned to the internal sink.
 */
void shell_init(void);

/**
 * @brief  Install or replace the transport output hook.
 *
 * @param  fn  Borrowed print callback. `NULL` restores the internal no-op sink.
 */
void shell_set_print(shell_print_fn_t fn);

/**
 * @brief  Parse and execute one caller-supplied command line.
 *
 * The input line is copied into an internal stack buffer and tokenized in
 * place, so the original caller buffer remains read-only from the shell's
 * point of view.
 *
 * @param  line  Input line bytes.
 * @param  len   Valid byte count in @p line.
 *
 * @note The shell emits its normal prompt/output through the currently
 *       installed print callback only.
 */
void shell_process_line(const char *line, uint16_t len);

/**
 * @brief  Emit one raw string through the installed print backend.
 *
 * @param  str  Borrowed NUL-terminated string. `NULL` is ignored.
 */
void shell_print(const char *str);

/**
 * @brief  Format one message into the shell scratch buffer and print it.
 *
 * @param  fmt  `printf`-style format string.
 */
void shell_printf(const char *fmt, ...);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
