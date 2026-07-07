/**
 * @file    test_bsp_console_contract.c
 * @brief   Host-side contract tests for the FalconFoc bsp console port
 * @version v1.0.0
 * @date    2026.06.02
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "bsp_console.h"
#include "host_rtos.h"
#include "task.h"

#define TEST_ASSERT(expr)                                                    \
    do {                                                                     \
        if (!(expr)) {                                                       \
            fprintf(stderr,                                                  \
                    "[test_bsp_console_contract] assertion failed: %s (%s:%d)\n", \
                    #expr,                                                   \
                    __FILE__,                                                \
                    __LINE__);                                               \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

static void test_bsp_console_shell_rx_start_validates_args_and_context(void)
{
    uint8_t rx_buf[16] = {0};

    host_rtos_reset();
    host_rtos_set_ipsr(5U);
    TEST_ASSERT(bsp_console_shell_rx_start(rx_buf, sizeof(rx_buf)) == -EPERM);

    host_rtos_set_ipsr(0U);
    TEST_ASSERT(bsp_console_shell_rx_start(NULL, sizeof(rx_buf)) == -EINVAL);
    TEST_ASSERT(bsp_console_shell_rx_start(rx_buf, 0U) == -EINVAL);

    host_rtos_set_scheduler_state(taskSCHEDULER_NOT_STARTED);
    TEST_ASSERT(bsp_console_shell_rx_start(rx_buf, sizeof(rx_buf)) == 0);

    host_rtos_set_scheduler_state(taskSCHEDULER_RUNNING);
    TEST_ASSERT(bsp_console_shell_rx_start(rx_buf, sizeof(rx_buf)) == 0);
}

static void test_bsp_console_write_poll_validates_args_and_context(void)
{
    static const char frame[] = "abc";

    host_rtos_reset();
    host_rtos_set_ipsr(7U);
    TEST_ASSERT(bsp_console_write_poll(frame, 3U, 1U, 1U, 1U) == -EPERM);

    host_rtos_set_ipsr(0U);
    TEST_ASSERT(bsp_console_write_poll(NULL, 3U, 1U, 1U, 1U) == -EINVAL);
    TEST_ASSERT(bsp_console_write_poll(frame, 0U, 1U, 1U, 1U) == -EINVAL);

    host_rtos_set_scheduler_state(taskSCHEDULER_NOT_STARTED);
    TEST_ASSERT(bsp_console_write_poll(frame, 3U, 1U, 1U, 1U) == 0);

    host_rtos_set_scheduler_state(taskSCHEDULER_RUNNING);
    TEST_ASSERT(bsp_console_write_poll(frame, 3U, 1U, 1U, 1U) == 0);
}

static void test_bsp_console_write_dma_validates_args_and_context(void)
{
    static const uint8_t frame[] = {0x11U, 0x22U, 0x33U};

    host_rtos_reset();
    host_rtos_set_ipsr(9U);
    TEST_ASSERT(bsp_console_write_dma(frame, 3U) == -EPERM);

    host_rtos_set_ipsr(0U);
    TEST_ASSERT(bsp_console_write_dma(NULL, 3U) == -EINVAL);
    TEST_ASSERT(bsp_console_write_dma(frame, 0U) == -EINVAL);

    host_rtos_set_scheduler_state(taskSCHEDULER_NOT_STARTED);
    TEST_ASSERT(bsp_console_write_dma(frame, 3U) == 0);

    host_rtos_set_scheduler_state(taskSCHEDULER_RUNNING);
    TEST_ASSERT(bsp_console_write_dma(frame, 3U) == 0);
}

static void test_bsp_console_query_helpers_keep_safe_host_defaults(void)
{
    host_rtos_reset();

    TEST_ASSERT(bsp_console_shell_rx_position(0U) == 0U);
    TEST_ASSERT(bsp_console_shell_rx_position(64U) == 0U);
    TEST_ASSERT(bsp_console_shell_irq_idle_pending() == false);
    bsp_console_shell_irq_ack_idle();
}

int main(void)
{
    test_bsp_console_shell_rx_start_validates_args_and_context();
    test_bsp_console_write_poll_validates_args_and_context();
    test_bsp_console_write_dma_validates_args_and_context();
    test_bsp_console_query_helpers_keep_safe_host_defaults();

    puts("[test_bsp_console_contract] passed");
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
