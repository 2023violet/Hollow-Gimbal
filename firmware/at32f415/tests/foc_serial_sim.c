/**
 * @file    foc_serial_sim.c
 * @brief   Host-only binary stdio device for Python/C compatibility tests.
 * @version v0.1.0
 * @date    2026.11.10
 *
 * Test role
 * ---------
 * Present the production byte queues and protocol adapter to a subprocess pipe.
 * Clock zero is intentional. Device and electrical observations remain absent.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_board.h"
#include "foc_serial.h"

#include <stdio.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

/* Private Helpers -------------------------------------------------------------- */

static uint32_t clock_us(void *argument)
{
    (void)argument;
    return 0U;
}

/* Test Entry ------------------------------------------------------------------- */

int main(void)
{
#ifdef _WIN32
    (void)_setmode(_fileno(stdin), _O_BINARY);
    (void)_setmode(_fileno(stdout), _O_BINARY);
#endif
    struct foc_runtime runtime;
    struct foc_board_output output;
    struct foc_serial serial;
    struct foc_uart_buffer uart = {0};
    if (foc_board_init(&runtime, &output, 0U) != FOC_RUNTIME_OK) {
        return 1;
    }
    foc_serial_init(&serial, &runtime);
    int byte;
    while ((byte = getchar()) != EOF) {
        struct foc_runtime_foreground_result result;
        (void)foc_runtime_foreground_poll(&runtime, 0U, 0U, 0U, &result);
        foc_uart_rx_push(&uart, (uint8_t)byte, false);
        foc_serial_poll(&serial, &uart, &runtime, clock_us, NULL);
        const struct foc_runtime_fast_input input = {0};
        (void)foc_runtime_h0_step(&runtime, &input);
        uint8_t sent;
        while (foc_uart_tx_pop(&uart, &sent)) {
            if (putchar(sent) == EOF) {
                return 1;
            }
        }
        fflush(stdout);
    }
    return ferror(stdin) ? 1 : 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
