#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "FreeRTOSConfig.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t     TickType_t;
typedef int          BaseType_t;
typedef unsigned int UBaseType_t;

/*
 * Host tests only need these opaque control-block types so application code
 * that uses CMSIS-RTOS2 static backing can compile against the lightweight
 * host RTOS shim.
 */
typedef struct {
    uintptr_t words[8];
} StaticTask_t;

typedef struct {
    uintptr_t words[8];
} StaticQueue_t;

typedef StaticQueue_t StaticSemaphore_t;

#ifndef pdFALSE
#define pdFALSE 0
#endif

#ifndef pdTRUE
#define pdTRUE 1
#endif

void *pvPortMalloc(size_t size);
void  vPortFree(void *ptr);

#ifdef __cplusplus
}
#endif
