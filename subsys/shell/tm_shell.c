/**
 * @file    tm_shell.c
 * @brief   Themis shell engine with linker-section static command dispatch
 * @version v4.1.0
 * @date    2026.03.28
 *
 * Implementation role
 * -------------------
 * The engine stays intentionally small and synchronous:
 * - command descriptors live in a linker section
 * - command dispatch happens inline in the caller's task context
 * - help output is derived directly from the exported descriptors, so there is
 *   no duplicate manual registry to keep in sync
 *
 * Key collaborators
 * -----------------
 * - `tm_shell.h`: public command-registry contract
 * - `tm_format`: bounded string formatting for shell output
 * - `tm_runtime`: minimal runtime helpers used by assertions and diagnostics
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "tm_shell.h"
#include <stdarg.h>
#include <string.h>
#include "tm_assert.h"
#include "tm_format.h"
#include "tm_runtime.h"

#if TM_ASSERT_ENABLE
static const char *TAG = "shell";
#endif

/* Linker Section Boundaries -------------------------------------------------------------------- */

#if defined(THEMIS_HOST_TEST)

const struct shell_cmd_entry *themis_host_shell_cmd_begin(void);
const struct shell_cmd_entry *themis_host_shell_cmd_end(void);

#define _p_start (themis_host_shell_cmd_begin())
#define _p_end   (themis_host_shell_cmd_end())

#elif defined(__CC_ARM) || defined(__ARMCC_VERSION)
extern const struct shell_cmd_entry Image$$ER_SHELL_CMD$$Base;
extern const struct shell_cmd_entry Image$$ER_SHELL_CMD$$Limit;

#define _p_start ((const struct shell_cmd_entry *)&Image$$ER_SHELL_CMD$$Base)
#define _p_end   ((const struct shell_cmd_entry *)&Image$$ER_SHELL_CMD$$Limit)

#elif defined(__IAR_SYSTEMS_ICC__)
#pragma section = ".shell_cmd"
#define _p_start ((const struct shell_cmd_entry *)__section_begin(".shell_cmd"))
#define _p_end   ((const struct shell_cmd_entry *)__section_end(".shell_cmd"))

#elif defined(__GNUC__)
extern const struct shell_cmd_entry _shell_cmd_start[];
extern const struct shell_cmd_entry _shell_cmd_end[];

#define _p_start (_shell_cmd_start)
#define _p_end   (_shell_cmd_end)

#else
#error "tm_shell.c: Unsupported compiler toolchain."
#endif

/* Module State --------------------------------------------------------------------------------- */

static shell_print_fn_t _print_fn = NULL;

/* Private Helpers ------------------------------------------------------------------------------ */

/* Default sink keeps early boot or unbound shells harmlessly silent. */
static void _noop_print(const char *str, uint16_t len)
{
    (void)str;
    (void)len;
}

/* Linear walk is acceptable because the exported command table is intentionally small. */
static const struct shell_cmd_entry *_find_cmd(const char *name)
{
    const struct shell_cmd_entry *entry = _p_start;

    while (entry < _p_end) {
        if ((entry->magic == SHELL_CMD_MAGIC) && (entry->name != NULL) &&
            (strcmp(entry->name, name) == 0)) {
            return entry;
        }
        entry++;
    }

    return NULL;
}

static int _cmd_help(int argc, char *argv[])
{
    const struct shell_cmd_entry *entries[SHELL_CMD_MAX];
    const struct shell_cmd_entry *entry;
    const char                   *last_category = NULL;
    uint32_t                      count         = 0U;

    (void)argc;
    (void)argv;

    entry = _p_start;
    while (entry < _p_end) {
        if ((entry->magic == SHELL_CMD_MAGIC) && (entry->name != NULL) && (entry->help != NULL) &&
            (count < SHELL_CMD_MAX)) {
            entries[count++] = entry;
        }
        entry++;
    }

    for (uint32_t i = 1U; i < count; i++) {
        const struct shell_cmd_entry *key = entries[i];
        int                           j   = (int)i - 1;

        /* Insertion sort keeps the implementation tiny for the expected command count. */
        while ((j >= 0) && (entries[j]->priority > key->priority)) {
            entries[(uint32_t)(j + 1)] = entries[j];
            j--;
        }
        entries[(uint32_t)(j + 1)] = key;
    }

    shell_printf("\r\n[help] count=%lu\r\n", (unsigned long)(count + 1U));

    for (uint32_t i = 0U; i < count; i++) {
        const char *category = entries[i]->category;
        bool        new_group;

        new_group = ((category == NULL) != (last_category == NULL));
        if (!new_group && (category != NULL) && (last_category != NULL)) {
            new_group = (strcmp(category, last_category) != 0);
        }

        if (new_group) {
            if (category != NULL) {
                shell_printf("  [%s]\r\n", category);
            } else {
                shell_print("  [Misc]\r\n");
            }
            last_category = category;
        }

        shell_printf("  cmd    : name=%s help=%s\r\n", entries[i]->name, entries[i]->help);
    }

    shell_print("  [Built-in]\r\n");
    shell_print("  cmd    : name=help help=Show command list\r\n");

    return SHELL_OK;
}

/* Public API ----------------------------------------------------------------------------------- */

void shell_init(void)
{
    _print_fn = _noop_print;
}

void shell_set_print(shell_print_fn_t fn)
{
    tm_assert(!tm_is_in_isr());
    _print_fn = (fn != NULL) ? fn : _noop_print;
}

void shell_process_line(const char *line, uint16_t len)
{
    char   token_buf[SHELL_LINE_MAX + 1U];
    char  *argv[SHELL_ARGC_MAX];
    size_t argc = 0U;

    tm_assert(!tm_is_in_isr());

    if ((line == NULL) || (len == 0U)) {
        shell_print(SHELL_PROMPT);
        return;
    }

    if (len > SHELL_LINE_MAX) {
        len = SHELL_LINE_MAX;
    }

    /* Copy once and tokenize in-place so callers can pass immutable buffers. */
    memcpy(token_buf, line, len);
    token_buf[len] = '\0';

    char *cursor = token_buf;
    while ((*cursor != '\0') && (argc < SHELL_ARGC_MAX)) {
        while ((*cursor == ' ') || (*cursor == '\t')) {
            cursor++;
        }
        if (*cursor == '\0') {
            break;
        }

        argv[argc++] = cursor;
        while ((*cursor != '\0') && (*cursor != ' ') && (*cursor != '\t')) {
            cursor++;
        }
        if (*cursor != '\0') {
            *cursor++ = '\0';
        }
    }

    if (argc == 0U) {
        shell_print(SHELL_PROMPT);
        return;
    }

    const char *cmd_name = argv[0];
    if (strcmp(cmd_name, "help") == 0) {
        /* Keep "help" built-in so every image has a minimum introspection path. */
        _cmd_help((int)argc, argv);
        shell_print(SHELL_PROMPT);
        return;
    }

    const struct shell_cmd_entry *entry = _find_cmd(cmd_name);
    if (entry == NULL) {
        shell_printf("\r\n[shell] cmd=%s result=error rc=-2\r\n", cmd_name);
        shell_print(SHELL_PROMPT);
        return;
    }

    int ret = entry->func((int)argc, argv);

    switch (ret) {
    case SHELL_OK:
        break;
    case SHELL_ERR_ARGC:
        shell_printf("\r\n[shell] cmd=%s result=error rc=-3\r\n", entry->name);
        break;
    case SHELL_ERR_ARG:
        shell_printf("\r\n[shell] cmd=%s result=error rc=-4\r\n", entry->name);
        break;
    case SHELL_ERR_STATE:
        shell_printf("\r\n[shell] cmd=%s result=error rc=-5\r\n", entry->name);
        break;
    case SHELL_ERR_CMD:
        shell_printf("\r\n[shell] cmd=%s result=error rc=-6\r\n", entry->name);
        break;
    default:
        shell_printf("\r\n[shell] cmd=%s result=error rc=%d\r\n", entry->name, ret);
        break;
    }

    shell_print(SHELL_PROMPT);
}

void shell_print(const char *str)
{
    shell_print_fn_t print_fn;

    tm_assert(!tm_is_in_isr());

    if (str == NULL) {
        return;
    }

    print_fn = (_print_fn != NULL) ? _print_fn : _noop_print;
    print_fn(str, (uint16_t)strlen(str));
}

void shell_printf(const char *fmt, ...)
{
    char             buffer[SHELL_PRINTF_BUF_SIZE];
    shell_print_fn_t print_fn;
    va_list          args;
    size_t           len;

    tm_assert(!tm_is_in_isr());

    if (fmt == NULL) {
        return;
    }

    print_fn = (_print_fn != NULL) ? _print_fn : _noop_print;

    va_start(args, fmt);
    len = tm_format_vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    if (len > 0) {
        /* Truncation is intentional; shell output favors bounded stack over completeness. */
        uint16_t out_len = (uint16_t)((len < sizeof(buffer)) ? len : (sizeof(buffer) - 1U));
        print_fn(buffer, out_len);
    }
}

/* ---------------------------------------- end of file ----------------------------------------- */
