/**
 * @file    test_tm_shell_task.c
 * @brief   Host-side tests for tm_shell_task queue/backlog hardening
 * @version v1.0.0
 * @date    2026.04.14
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_rtos.h"
#include "tm_shell.h"
#include "app_shell_task.h"
#include "tmdef.h"

#define TEST_ASSERT(expr)                                                  \
    do {                                                                   \
        if (!(expr)) {                                                     \
            fprintf(stderr,                                                \
                    "[test_tm_shell_task] assertion failed: %s (%s:%d)\n", \
                    #expr,                                                 \
                    __FILE__,                                              \
                    __LINE__);                                             \
            exit(1);                                                       \
        }                                                                  \
    } while (0)

static const struct shell_cmd_entry *g_shell_begin = NULL;
static const struct shell_cmd_entry *g_shell_end   = NULL;
static char                          g_output[512];
static size_t                        g_output_len = 0U;

const struct shell_cmd_entry *themis_host_shell_cmd_begin(void)
{
    return g_shell_begin;
}

const struct shell_cmd_entry *themis_host_shell_cmd_end(void)
{
    return g_shell_end;
}

static void shell_capture_reset(void)
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

static void test_tm_shell_task_backlog_preserves_fifo_and_diagnostics(void)
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
    struct tm_shell_task_diag_snapshot snapshot;
    const char                        *echo_a = "echo a";
    const char                        *echo_b = "echo b";
    const char                        *echo_c = "echo c";
    const char                        *echo_d = "echo d";
    const char                        *echo_e = "echo e";
    const char                        *pos_a;
    const char                        *pos_b;
    const char                        *pos_c;
    const char                        *pos_d;

    host_rtos_reset();
    shell_table_select(entries, TM_ARRAY_SIZE(entries));
    shell_capture_reset();
    shell_init();
    shell_set_print(shell_capture);

    TEST_ASSERT(tm_shell_task_init() == 0);
    TEST_ASSERT(tm_shell_task_diag_snapshot(&snapshot));
    TEST_ASSERT(snapshot.initialized);
    TEST_ASSERT(!snapshot.transport_ready);
    TEST_ASSERT(!snapshot.ready);
    TEST_ASSERT(snapshot.queue_depth == 2U);
    TEST_ASSERT(snapshot.backlog_depth == 2U);

    tm_shell_task_notify_transport_ready();
    TEST_ASSERT(tm_shell_task_diag_snapshot(&snapshot));
    TEST_ASSERT(snapshot.initialized);
    TEST_ASSERT(snapshot.transport_ready);
    TEST_ASSERT(snapshot.ready);

    TEST_ASSERT(tm_shell_task_submit_line(echo_a, 6U) == 0);
    TEST_ASSERT(tm_shell_task_submit_line(echo_b, 6U) == 0);
    TEST_ASSERT(tm_shell_task_submit_line(echo_c, 6U) == 0);
    TEST_ASSERT(tm_shell_task_submit_line(echo_d, 6U) == 0);
    TEST_ASSERT(tm_shell_task_submit_line(echo_e, 6U) == -EAGAIN);

    TEST_ASSERT(tm_shell_task_diag_snapshot(&snapshot));
    TEST_ASSERT(snapshot.queue_used == 2U);
    TEST_ASSERT(snapshot.queue_high_water == 2U);
    TEST_ASSERT(snapshot.backlog_used == 2U);
    TEST_ASSERT(snapshot.backlog_high_water == 2U);
    TEST_ASSERT(snapshot.submit_ok_count == 2U);
    TEST_ASSERT(snapshot.submit_backlog_count == 2U);
    TEST_ASSERT(snapshot.submit_fail_count == 1U);

    TEST_ASSERT(tm_shell_task_host_process_once() == 1);
    TEST_ASSERT(tm_shell_task_host_process_once() == 1);
    TEST_ASSERT(tm_shell_task_host_process_once() == 1);
    TEST_ASSERT(tm_shell_task_host_process_once() == 1);
    TEST_ASSERT(tm_shell_task_host_process_once() == 0);

    pos_a = strstr(g_output, "echo:a");
    pos_b = strstr(g_output, "echo:b");
    pos_c = strstr(g_output, "echo:c");
    pos_d = strstr(g_output, "echo:d");
    TEST_ASSERT(pos_a != NULL);
    TEST_ASSERT(pos_b != NULL);
    TEST_ASSERT(pos_c != NULL);
    TEST_ASSERT(pos_d != NULL);
    TEST_ASSERT(pos_a < pos_b);
    TEST_ASSERT(pos_b < pos_c);
    TEST_ASSERT(pos_c < pos_d);

    TEST_ASSERT(tm_shell_task_diag_snapshot(&snapshot));
    TEST_ASSERT(snapshot.queue_used == 0U);
    TEST_ASSERT(snapshot.backlog_used == 0U);
}

int main(void)
{
    test_tm_shell_task_backlog_preserves_fifo_and_diagnostics();

    puts("[test_tm_shell_task] passed");
    return 0;
}
