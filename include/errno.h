/**
 * @file    errno.h
 * @brief   Project-local errno compatibility shim for Themis
 * @version v1.1.0
 * @date    2026.03.21
 *
 * Components should keep using <errno.h>.
 * This shim first pulls the toolchain errno header when one exists, then
 * overlays the complete Themis/Linux-style errno macro set from tm_errno.h.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define TM_ERRNO_SHIM_ACTIVE (1)

#if defined(__has_include_next)
#if __has_include_next(<errno.h>)
#include_next <errno.h>
#endif
#elif defined(__GNUC__) || defined(__clang__) || defined(__CC_ARM)
#include_next <errno.h>
#endif

#include "tm_errno.h"

#undef TM_ERRNO_SHIM_ACTIVE

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
