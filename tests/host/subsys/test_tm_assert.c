/**
 * @file    test_tm_assert.c
 * @brief   Host-side stress tests for Themis assert reporting
 * @version v1.0.0
 * @date    2026.04.04
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_rtos.h"
#include "tm_assert.h"

#define TEST_ASSERT(expr)                                              \
    do {                                                               \
        if (!(expr)) {                                                 \
            fprintf(stderr,                                            \
                    "[test_tm_assert] assertion failed: %s (%s:%d)\n", \
                    #expr,                                             \
                    __FILE__,                                          \
                    __LINE__);                                         \
            exit(1);                                                   \
        }                                                              \
    } while (0)

static char     g_capture[256];
static uint16_t g_capture_len = 0U;

static void capture_reset(void)
{
    memset(g_capture, 0, sizeof(g_capture));
    g_capture_len = 0U;
}

static void capture_backend(const char *data, uint16_t len)
{
    size_t copy_len = len;

    TEST_ASSERT(data != NULL);
    TEST_ASSERT((size_t)g_capture_len < sizeof(g_capture));

    if ((size_t)g_capture_len + copy_len >= sizeof(g_capture)) {
        copy_len = (sizeof(g_capture) - 1U) - (size_t)g_capture_len;
    }

    memcpy(&g_capture[g_capture_len], data, copy_len);
    g_capture_len += (uint16_t)copy_len;
    g_capture[g_capture_len] = '\0';
}

static void test_tm_assert_streams_condition_and_message_without_heap_buffers(void)
{
    struct tm_assert_config config = {
        .write   = capture_backend,
        .handler = NULL,
    };

    host_rtos_reset();
    tm_assert_init(&config);

    capture_reset();
    _tm_assert_failed("subsys", 42U, TM_ASSERT_LEVEL_ERROR, "worker", 0U, NULL, "state != NULL");
    TEST_ASSERT(strstr(g_capture, "[A/subsys L:42] worker: Failed: state != NULL") != NULL);

    capture_reset();
    _tm_assert_failed("subsys", 99U, TM_ASSERT_LEVEL_WARNING, "worker", 0U, "warn-path", "ignored");
    TEST_ASSERT(strstr(g_capture, "[A/subsys L:99] worker: warn-path") != NULL);
}

static void test_tm_assert_uses_custom_error_map_before_defaults(void)
{
#if TM_ASSERT_USE_ERROR_MAP
    static const struct tm_assert_error_map custom_map[] = {
        {0x55U, "CUSTOM_TIMEOUT"},
    };

    tm_assert_register_map(custom_map, sizeof(custom_map) / sizeof(custom_map[0]));

    capture_reset();
    _tm_assert_failed("map", 7U, TM_ASSERT_LEVEL_ERROR, "probe", 0x55U, "mapped", "cond");
    TEST_ASSERT(strstr(g_capture, "Err=CUSTOM_TIMEOUT") != NULL);
    TEST_ASSERT(strstr(g_capture, "mapped") != NULL);

    capture_reset();
    _tm_assert_failed("map", 8U, TM_ASSERT_LEVEL_ERROR, "probe", 0x05U, "fallback", "cond");
    TEST_ASSERT(strstr(g_capture, "Err=TIMEOUT") != NULL);
#endif
}

static void test_tm_assert_reentry_gate_recovers_after_many_nonfatal_reports(void)
{
    struct tm_assert_config config = {
        .write   = capture_backend,
        .handler = NULL,
    };

    tm_assert_init(&config);

    for (uint32_t i = 0U; i < 512U; ++i) {
        capture_reset();
        _tm_assert_failed("loop", i, TM_ASSERT_LEVEL_WARNING, "stress", 0U, "repeat", "cond");
        TEST_ASSERT(strstr(g_capture, "[A/loop L:") != NULL);
        TEST_ASSERT(strstr(g_capture, "stress: repeat") != NULL);
    }
}

int main(void)
{
    test_tm_assert_streams_condition_and_message_without_heap_buffers();
    test_tm_assert_uses_custom_error_map_before_defaults();
    test_tm_assert_reentry_gate_recovers_after_many_nonfatal_reports();

    puts("[test_tm_assert] passed");
    return 0;
}
