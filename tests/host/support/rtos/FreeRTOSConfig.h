#pragma once

/*
 * Host-test FreeRTOS shim configuration.
 *
 * This file is intentionally minimal: it only exposes the compile-time knobs
 * that the lightweight host RTOS headers need in order to compile shared
 * runtime helpers on the native toolchain.
 */

#define configUSE_16_BIT_TICKS 0
#define configTICK_RATE_HZ     1000
#define configTOTAL_HEAP_SIZE  ((size_t)(10U * 1024U))

/* ---------------------------------------- end of file ----------------------------------------- */
