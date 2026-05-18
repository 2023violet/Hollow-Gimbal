/**
 * @file    test_tm_log.c
 * @brief   Host-side tests for Themis log formatting and guards
 * @version v1.0.0
 * @date    2026.03.30
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_rtos.h"
#include "tm_log.h"

LOG_TAG("test_tm_log");

#define TEST_ASSERT(expr)                                           \
    do {                                                            \
        if (!(expr)) {                                              \
            fprintf(stderr,                                         \
                    "[test_tm_log] assertion failed: %s (%s:%d)\n", \
                    #expr,                                          \
                    __FILE__,                                       \
                    __LINE__);                                      \
            exit(1);                                                \
        }                                                           \
    } while (0)

static char     g_capture[256];
static uint16_t g_capture_len     = 0U;
static bool     g_backend_recurse = false;

static void capture_reset(void)
{
    memset(g_capture, 0, sizeof(g_capture));
    g_capture_len     = 0U;
    g_backend_recurse = false;
}

static void capture_backend(const char *data, uint16_t len)
{
    size_t copy_len = (len < (uint16_t)(sizeof(g_capture) - 1U)) ? len : (sizeof(g_capture) - 1U);

    memcpy(g_capture, data, copy_len);
    g_capture[copy_len] = '\0';
    g_capture_len       = (uint16_t)copy_len;

    if (g_backend_recurse) {
        g_backend_recurse = false;
        log_error("nested");
    }
}

static void test_tm_log_formats_headers_and_payload(void)
{
    struct tm_log_config config = {
        .write = capture_backend,
    };

    host_rtos_reset();
    host_rtos_set_tick_count(123U);
    capture_reset();

    tm_log_init(&config);
    tm_log_set_level(TM_LOG_LEVEL_DEBUG);
    log_info("value=%d", 42);

    TEST_ASSERT(g_capture_len > 0U);
    TEST_ASSERT(strstr(g_capture, "[I/test_tm_log") != NULL);
    TEST_ASSERT(strstr(g_capture, "T:123") != NULL);
    TEST_ASSERT(strstr(g_capture, "value=42") != NULL);
}

static void test_tm_log_drops_isr_and_recursive_calls(void)
{
    capture_reset();
    host_rtos_set_ipsr(5U);
    log_error("ignored");
    host_rtos_set_ipsr(0U);
    TEST_ASSERT(g_capture_len == 0U);

    capture_reset();
    g_backend_recurse = true;
    log_warn("outer");
    TEST_ASSERT(g_capture_len > 0U);
    TEST_ASSERT(strstr(g_capture, "outer") != NULL);
    TEST_ASSERT(strstr(g_capture, "nested") == NULL);
}

static void test_tm_log_drops_output_when_mutex_acquire_fails(void)
{
    capture_reset();
    host_rtos_fail_next_mutex_acquire(osErrorResource);
    log_info("mutex_fail");
    TEST_ASSERT(g_capture_len == 0U);
}

int main(void)
{
    test_tm_log_formats_headers_and_payload();
    test_tm_log_drops_isr_and_recursive_calls();
    test_tm_log_drops_output_when_mutex_acquire_fails();

    puts("[test_tm_log] passed");
    return 0;
}
