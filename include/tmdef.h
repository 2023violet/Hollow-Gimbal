/**
 * @file    tmdef.h
 * @brief   Themis low-level macro boundary
 * @version v3.0.0
 * @date    2026.03.24
 *
 * Ownership
 * ---------
 * `tmdef.h` is not a project-wide "common include everything" header.
 * It only owns:
 * - compiler abstraction macros used by `subsys/export`, object model, and list helpers
 * - a small set of container/alignment utilities already used by the current tree
 * - the minimum type surface needed by those low-level facilities
 * - three frozen preprocessor helpers: TM_STRINGIFY / TM_CONCAT / TM_BIT
 *
 * Non-ownership
 * -------------
 * New code must include its real dependencies directly instead of relying on
 * `tmdef.h` transitively. In particular, this header does not own:
 * - generic math / bit / time helper grab-bags
 * - platform newline or wait-time constants
 * - arbitrary standard library headers unrelated to low-level runtime macros
 *
 * Freeze note
 * -----------
 * The helper surface in this header is intentionally frozen after
 * `TM_STRINGIFY`, `TM_CONCAT`, and `TM_BIT`. New convenience macros such as
 * `MIN/MAX`, `GENMASK`, `CLAMP`, or generic arithmetic helpers must not be
 * added here without an explicit architecture-level review.
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(__has_include)
#if __has_include(<sys/types.h>)
#include <sys/types.h>
#endif
#endif

#include "tm_errno.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Keil AC6 / ArmClang bare-metal headers do not guarantee a native `ssize_t`.
 * Themis uses `ssize_t` in the device contract as the signed counterpart of
 * `size_t`, so we provide the minimum portable fallback here.
 */
#if !defined(_SSIZE_T_DEFINED) && !defined(__ssize_t_defined) && !defined(_SSIZE_T_DECLARED) && \
    !defined(_SSIZE_T_)
typedef ptrdiff_t ssize_t;
#define _SSIZE_T_DEFINED
#endif

/* Compiler Abstraction ------------------------------------------------------------------------- */

#if defined(__CC_ARM) || defined(__CLANG_ARM)

#define TM_SECTION(x) __attribute__((section(x)))
#define TM_USED       __attribute__((used))
#define TM_ALIGN(n)   __attribute__((aligned(n)))

#if defined(__CC_ARM) && (__ARMCC_VERSION < 6010050)
#define TM_PACKED(declare) __packed declare
#else
#define TM_PACKED(declare) declare __attribute__((packed))
#endif

#define TM_WEAK          __attribute__((weak))
#define TM_TYPEOF        __typeof__
#define TM_NORETURN      __attribute__((noreturn))
#define TM_INLINE        static inline
#define TM_ALWAYS_INLINE static inline __attribute__((always_inline))

#elif defined(__IAR_SYSTEMS_ICC__)

#define TM_PRAGMA(x)       _Pragma(#x)
#define TM_SECTION(x)      @x
#define TM_USED            __root
#define TM_ALIGN(n)        TM_PRAGMA(data_alignment = n)
#define TM_PACKED(declare) __packed declare
#define TM_WEAK            __weak
#define TM_TYPEOF          __typeof__
#define TM_NORETURN
#define TM_INLINE          static inline
#define TM_ALWAYS_INLINE TM_INLINE

#elif defined(__GNUC__)

#define TM_SECTION(x)      __attribute__((section(x)))
#define TM_USED            __attribute__((used))
#define TM_ALIGN(n)        __attribute__((aligned(n)))
#define TM_PACKED(declare) declare __attribute__((packed))
#define TM_WEAK            __attribute__((weak))
#define TM_TYPEOF          __typeof__
#define TM_NORETURN        __attribute__((noreturn))
#define TM_INLINE          static inline
#define TM_ALWAYS_INLINE   static inline __attribute__((always_inline))

#else
#error "The current compiler is NOT supported!"
#endif

/* Small Core Utilities ------------------------------------------------------------------------- */

#define TM_STRINGIFY_INNER(x) #x
#define TM_STRINGIFY(x)       TM_STRINGIFY_INNER(x)
#define TM_CONCAT_INNER(a, b) a##b
#define TM_CONCAT(a, b)       TM_CONCAT_INNER(a, b)
/*
 * Keep TM_BIT() as a minimal register / feature-mask helper only.
 * Callers remain responsible for keeping the shift below the width of
 * unsigned long on the target toolchain.
 */
#define TM_BIT(bit)           (1UL << (bit))

#define TM_UNUSED(x) \
    do {             \
        ((void)(x)); \
    } while (0)

#define TM_ARRAY_SIZE(array)       (sizeof(array) / sizeof((array)[0]))
#define TM_IS_ALIGNED(addr, align) (((uintptr_t)(addr) & ((uintptr_t)(align) - 1U)) == 0U)
#define TM_ALIGN_UP(size, align) \
    ((((size_t)(size)) + ((size_t)(align) - 1U)) & ~((size_t)(align) - 1U))
#define TM_ALIGN_DOWN(size, align) (((size_t)(size)) & ~((size_t)(align) - 1U))

/*
 * `TM_CONTAINER_OF` stays here because it is part of the current list/object
 * model contract. New higher-level modules should only use it when there is a
 * clear intrusive-structure reason.
 */
#if defined(__GNUC__) && !defined(__STRICT_ANSI__)
#define TM_CONTAINER_OF(ptr, type, member)                    \
    ({                                                        \
        const TM_TYPEOF(((type *)0)->member) *__mptr = (ptr); \
        (type *)((char *)__mptr - offsetof(type, member));    \
    })
#else
#define TM_CONTAINER_OF(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#endif

/* Compile-Time Assertions ---------------------------------------------------------------------- */

#if defined(__cplusplus)
#define TM_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#elif defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
#define TM_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#else
#define TM_STATIC_ASSERT(cond, msg) typedef char static_assert_##msg[(cond) ? 1 : -1]
#endif

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
