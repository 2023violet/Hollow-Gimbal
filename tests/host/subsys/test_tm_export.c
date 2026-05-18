/**
 * @file    test_tm_export.c
 * @brief   Host-side tests for Themis export ordering and error reporting
 * @version v1.0.0
 * @date    2026.04.03
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tm_export.h"

#define TEST_ASSERT(expr)                                              \
    do {                                                               \
        if (!(expr)) {                                                 \
            fprintf(stderr,                                            \
                    "[test_tm_export] assertion failed: %s (%s:%d)\n", \
                    #expr,                                             \
                    __FILE__,                                          \
                    __LINE__);                                         \
            exit(1);                                                   \
        }                                                              \
    } while (0)

#define EXPORT_TEST_ITEM_INT(_name, _func, _level, _priority) \
    {                                                         \
        .magic_head = EXPORT_ID_INIT,                         \
        .name       = (_name),                                \
        .func       = {.int_fn = (_func)},                    \
        .ctx        = NULL,                                   \
        .level      = (_level),                               \
        .priority   = (_priority),                            \
        .flags      = 0U,                                     \
        .reserved   = 0U,                                     \
        .magic_tail = EXPORT_ID_INIT,                         \
    }

#define EXPORT_TEST_ITEM_VOID(_name, _func, _level, _priority) \
    {                                                          \
        .magic_head = EXPORT_ID_INIT,                          \
        .name       = (_name),                                 \
        .func       = {.void_fn = (_func)},                    \
        .ctx        = NULL,                                    \
        .level      = (_level),                                \
        .priority   = (_priority),                             \
        .flags      = EXPORT_FLAG_VOID,                        \
        .reserved   = 0U,                                      \
        .magic_tail = EXPORT_ID_INIT,                          \
    }

#define EXPORT_TEST_ITEM_CTX(_name, _func, _ctx, _level, _priority) \
    {                                                               \
        .magic_head = EXPORT_ID_INIT,                               \
        .name       = (_name),                                      \
        .func       = {.int_ctx_fn = (_func)},                      \
        .ctx        = (void *)(_ctx),                               \
        .level      = (_level),                                     \
        .priority   = (_priority),                                  \
        .flags      = EXPORT_FLAG_CONTEXT,                          \
        .reserved   = 0U,                                           \
        .magic_tail = EXPORT_ID_INIT,                               \
    }

static const struct export_item *g_export_begin = NULL;
static const struct export_item *g_export_end   = NULL;
static char                      g_trace[16];
static size_t                    g_trace_len = 0U;

const struct export_item *themis_host_export_init_begin(void)
{
    return g_export_begin;
}

const struct export_item *themis_host_export_init_end(void)
{
    return g_export_end;
}

static void trace_reset(void)
{
    memset(g_trace, 0, sizeof(g_trace));
    g_trace_len = 0U;
}

static void trace_push(char marker)
{
    TEST_ASSERT(g_trace_len < (sizeof(g_trace) - 1U));
    g_trace[g_trace_len++] = marker;
    g_trace[g_trace_len]   = '\0';
}

static void export_table_select(const struct export_item *items, size_t count)
{
    g_export_begin = items;
    g_export_end   = items + count;
    trace_reset();
}

static int hook_driver_default(void)
{
    trace_push('D');
    return 0;
}

static void hook_board_high(void)
{
    trace_push('H');
}

static int hook_app_ctx(void *ctx)
{
    trace_push(*(const char *)ctx);
    return 0;
}

static int hook_fail(void)
{
    trace_push('F');
    return -EIO;
}

static int hook_late(void)
{
    trace_push('L');
    return 0;
}

static void test_export_executes_in_level_priority_order(void)
{
    static const char               ctx_marker = 'A';
    static const struct export_item items[]    = {
        EXPORT_TEST_ITEM_INT("driver_default",
                             hook_driver_default,
                             EXPORT_LEVEL_DRIVER,
                             EXPORT_PRIO_DEFAULT),
        EXPORT_TEST_ITEM_VOID("board_high",
                              hook_board_high,
                              EXPORT_LEVEL_BOARD_EARLY,
                              EXPORT_PRIO_HIGH),
        EXPORT_TEST_ITEM_CTX("app_ctx",
                             hook_app_ctx,
                             &ctx_marker,
                             EXPORT_LEVEL_APP,
                             EXPORT_PRIO_DEFAULT),
    };
    int rc;

    export_table_select(items, TM_ARRAY_SIZE(items));
    rc = export_init_execute_range(EXPORT_LEVEL_BOARD_EARLY, EXPORT_LEVEL_APP);

    TEST_ASSERT(rc == 0);
    TEST_ASSERT(strcmp(g_trace, "HDA") == 0);
}

static void test_export_fail_fast_records_last_error(void)
{
    struct tm_export_last_error     error   = {0};
    static const struct export_item items[] = {
        EXPORT_TEST_ITEM_VOID("board_high",
                              hook_board_high,
                              EXPORT_LEVEL_BOARD_EARLY,
                              EXPORT_PRIO_HIGH),
        EXPORT_TEST_ITEM_INT("failing_driver", hook_fail, EXPORT_LEVEL_DRIVER, EXPORT_PRIO_DEFAULT),
        EXPORT_TEST_ITEM_INT("late_app", hook_late, EXPORT_LEVEL_APP, EXPORT_PRIO_DEFAULT),
    };
    int rc;

    export_table_select(items, TM_ARRAY_SIZE(items));
    rc = export_init_execute(EXPORT_LEVEL_APP);

    TEST_ASSERT(rc == -EIO);
    TEST_ASSERT(strcmp(g_trace, "HF") == 0);
    TEST_ASSERT(export_init_last_error_get(&error));
    TEST_ASSERT(strcmp(error.name, "failing_driver") == 0);
    TEST_ASSERT(error.level == EXPORT_LEVEL_DRIVER);
    TEST_ASSERT(error.priority == EXPORT_PRIO_DEFAULT);
    TEST_ASSERT(error.status == -EIO);
}

static void test_export_rejects_reversed_ranges(void)
{
    static const struct export_item items[] = {
        EXPORT_TEST_ITEM_VOID("board_high",
                              hook_board_high,
                              EXPORT_LEVEL_BOARD_EARLY,
                              EXPORT_PRIO_HIGH),
    };

    export_table_select(items, TM_ARRAY_SIZE(items));
    TEST_ASSERT(export_init_execute_range(EXPORT_LEVEL_APP, EXPORT_LEVEL_BOARD_EARLY) == -EINVAL);
}

int main(void)
{
    test_export_executes_in_level_priority_order();
    test_export_fail_fast_records_last_error();
    test_export_rejects_reversed_ranges();

    puts("[test_tm_export] passed");
    return 0;
}
