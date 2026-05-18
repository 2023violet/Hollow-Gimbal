/**
 * @file    test_tm_shell.c
 * @brief   Host-side tests for Themis shell parsing and dispatch
 * @version v1.0.0
 * @date    2026.04.03
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_rtos.h"
#include "tm_shell.h"
#include "tmdef.h"

#define TEST_ASSERT(expr)                                             \
    do {                                                              \
        if (!(expr)) {                                                \
            fprintf(stderr,                                           \
                    "[test_tm_shell] assertion failed: %s (%s:%d)\n", \
                    #expr,                                            \
                    __FILE__,                                         \
                    __LINE__);                                        \
            exit(1);                                                  \
        }                                                             \
    } while (0)

static const struct shell_cmd_entry *g_shell_begin = NULL;
static const struct shell_cmd_entry *g_shell_end   = NULL;
static char                          g_output[1024];
static size_t                        g_output_len = 0U;

const struct shell_cmd_entry *themis_host_shell_cmd_begin(void)
{
    return g_shell_begin;
}

const struct shell_cmd_entry *themis_host_shell_cmd_end(void)
{
    return g_shell_end;
}

static void shell_output_reset(void)
{
    memset(g_output, 0, sizeof(g_output));
    g_output_len = 0U;
}

static void shell_capture(const char *str, uint16_t len)
{
    TEST_ASSERT(str != NULL);
    TEST_ASSERT((g_output_len + len) < sizeof(g_output));

    memcpy(&g_output[g_output_len], str, len);
    g_output_len += len;
    g_output[g_output_len] = '\0';
}

static void shell_table_select(const struct shell_cmd_entry *entries, size_t count)
{
    g_shell_begin = entries;
    g_shell_end   = entries + count;
}

static int cmd_echo(int argc, char *argv[])
{
    if (argc != 2) {
        return SHELL_ERR_ARGC;
    }

    shell_printf("echo:%s", argv[1]);
    return SHELL_OK;
}

static int cmd_state(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    return SHELL_ERR_STATE;
}

static int cmd_arg(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    return SHELL_ERR_ARG;
}

static int cmd_fail(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    return -22;
}

static void shell_env_reset(const struct shell_cmd_entry *entries, size_t count)
{
    host_rtos_reset();
    shell_table_select(entries, count);
    shell_output_reset();
    shell_init();
    shell_set_print(shell_capture);
}

static void test_shell_help_lists_exported_commands_and_builtin_help(void)
{
    static const struct shell_cmd_entry entries[] = {
        {
            .magic    = SHELL_CMD_MAGIC,
            .priority = 20U,
            .name     = "state",
            .func     = cmd_state,
            .help     = "Return state error",
            .category = "Diag",
        },
        {
            .magic    = SHELL_CMD_MAGIC,
            .priority = 10U,
            .name     = "echo",
            .func     = cmd_echo,
            .help     = "Echo one arg",
            .category = "System",
        },
    };

    shell_env_reset(entries, TM_ARRAY_SIZE(entries));
    shell_process_line("help", 4U);

    TEST_ASSERT(strstr(g_output, "[help] count=3") != NULL);
    TEST_ASSERT(strstr(g_output, "[System]") != NULL);
    TEST_ASSERT(strstr(g_output, "[Diag]") != NULL);
    TEST_ASSERT(strstr(g_output, "  cmd    : name=echo help=Echo one arg") != NULL);
    TEST_ASSERT(strstr(g_output, "  cmd    : name=state help=Return state error") != NULL);
    TEST_ASSERT(strstr(g_output, "[Built-in]") != NULL);
    TEST_ASSERT(strstr(g_output, "  cmd    : name=help help=Show command list") != NULL);
    TEST_ASSERT(strstr(g_output, SHELL_PROMPT) != NULL);
}

static void test_shell_dispatches_known_command_and_reports_unknown_ones(void)
{
    static const struct shell_cmd_entry entries[] = {
        {
            .magic    = SHELL_CMD_MAGIC,
            .priority = 10U,
            .name     = "echo",
            .func     = cmd_echo,
            .help     = "Echo one arg",
            .category = "System",
        },
    };

    shell_env_reset(entries, TM_ARRAY_SIZE(entries));
    shell_process_line("echo alpha", 10U);

    TEST_ASSERT(strstr(g_output, "echo:alpha") != NULL);
    TEST_ASSERT(strstr(g_output, SHELL_PROMPT) != NULL);

    shell_output_reset();
    shell_process_line("missing", 7U);

    TEST_ASSERT(strstr(g_output, "[shell] cmd=missing result=error rc=-2") != NULL);
    TEST_ASSERT(strstr(g_output, SHELL_PROMPT) != NULL);
}

static void test_shell_reports_state_and_usage_errors(void)
{
    static const struct shell_cmd_entry entries[] = {
        {
            .magic    = SHELL_CMD_MAGIC,
            .priority = 10U,
            .name     = "echo",
            .func     = cmd_echo,
            .help     = "Echo one arg",
            .category = "System",
        },
        {
            .magic    = SHELL_CMD_MAGIC,
            .priority = 20U,
            .name     = "state",
            .func     = cmd_state,
            .help     = "Return state error",
            .category = "Diag",
        },
        {
            .magic    = SHELL_CMD_MAGIC,
            .priority = 30U,
            .name     = "arg",
            .func     = cmd_arg,
            .help     = "Return arg error",
            .category = "Diag",
        },
        {
            .magic    = SHELL_CMD_MAGIC,
            .priority = 40U,
            .name     = "fail",
            .func     = cmd_fail,
            .help     = "Return raw failure",
            .category = "Diag",
        },
    };

    shell_env_reset(entries, TM_ARRAY_SIZE(entries));
    shell_process_line("echo", 4U);
    TEST_ASSERT(strstr(g_output, "[shell] cmd=echo result=error rc=-3") != NULL);

    shell_output_reset();
    shell_process_line("state", 5U);
    TEST_ASSERT(strstr(g_output, "[shell] cmd=state result=error rc=-5") != NULL);
    TEST_ASSERT(strstr(g_output, SHELL_PROMPT) != NULL);

    shell_output_reset();
    shell_process_line("arg", 3U);
    TEST_ASSERT(strstr(g_output, "[shell] cmd=arg result=error rc=-4") != NULL);
    TEST_ASSERT(strstr(g_output, SHELL_PROMPT) != NULL);

    shell_output_reset();
    shell_process_line("fail", 4U);
    TEST_ASSERT(strstr(g_output, "[shell] cmd=fail result=error rc=-22") != NULL);
    TEST_ASSERT(strstr(g_output, SHELL_PROMPT) != NULL);
}

static void test_shell_blank_input_only_reprints_prompt(void)
{
    static const struct shell_cmd_entry entries[] = {
        {
            .magic    = SHELL_CMD_MAGIC,
            .priority = 10U,
            .name     = "echo",
            .func     = cmd_echo,
            .help     = "Echo one arg",
            .category = "System",
        },
    };

    shell_env_reset(entries, TM_ARRAY_SIZE(entries));
    shell_process_line("   ", 3U);

    TEST_ASSERT(strcmp(g_output, SHELL_PROMPT) == 0);
}

int main(void)
{
    test_shell_help_lists_exported_commands_and_builtin_help();
    test_shell_dispatches_known_command_and_reports_unknown_ones();
    test_shell_reports_state_and_usage_errors();
    test_shell_blank_input_only_reprints_prompt();

    puts("[test_tm_shell] passed");
    return 0;
}
