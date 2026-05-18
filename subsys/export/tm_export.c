/**
 * @file    tm_export.c
 * @brief   Themis static-init export executor
 * @version v5.0.0
 * @date    2026.03.22
 *
 * Implementation role
 * -------------------
 * This translation unit keeps the legacy prefix executor for backward
 * compatibility and adds the ordered range executor that the split-boot model
 * needs.
 *
 * Split-boot rationale
 * --------------------
 * The original Themis export API supported only prefix execution:
 *
 *   export_init_execute(EXPORT_LEVEL_APP)
 *
 * That means "run BSP through APP". This is correct for simple one-shot
 * boot, but it cannot express staged boot without replaying lower levels.
 *
 * Example of the old problem:
 *
 *   export_init_execute(EXPORT_LEVEL_DRIVER);
 *   osKernelInitialize();
 *   export_init_execute(EXPORT_LEVEL_APP);
 *
 * The second call replays BSP / HW_INDEPENDENT / DRIVER again.
 *
 * This file keeps the prefix API for backward compatibility and adds a real
 * range executor so staged boot can be expressed exactly once per level.
 *
 * Current project constraint
 * --------------------------
 * The current device/object registration path creates CMSIS-RTOS2 mutexes and
 * semaphores. Therefore DRIVER and above currently belong after
 * `osKernelInitialize()` unless that dependency is refactored away first.
 *
 * v5 update
 * ---------
 * - stage macros replace enum values
 * - priority is now explicit inside one stage
 * - hooks may be void / int and optional static-context based
 * - ordered execution uses a zero-heap selection walk with item-address
 *   tiebreak, so same level/priority hooks stay linker-stable without qsort
 * - init execution is fail-fast by default so later hooks do not run on a
 *   half-initialized boot path
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "tm_export.h"
#include <errno.h>

/* Toolchain Section Symbols -------------------------------------------------------------------- */

#if defined(THEMIS_HOST_TEST)

const struct export_item *themis_host_export_init_begin(void);
const struct export_item *themis_host_export_init_end(void);

#define EXPORT_INIT_BEGIN (themis_host_export_init_begin())
#define EXPORT_INIT_END   (themis_host_export_init_end())

#if TM_EXPORT_EXIT_SECTION_ENABLE
const struct export_item *themis_host_export_exit_begin(void);
const struct export_item *themis_host_export_exit_end(void);

#define EXPORT_EXIT_BEGIN (themis_host_export_exit_begin())
#define EXPORT_EXIT_END   (themis_host_export_exit_end())
#endif

#elif defined(__CC_ARM) || (defined(__ARMCC_VERSION) && (__ARMCC_VERSION >= 6000000))

extern const struct export_item Image$$ER_EXPORT_INIT$$Base;
extern const struct export_item Image$$ER_EXPORT_INIT$$Limit;

#define EXPORT_INIT_BEGIN (&Image$$ER_EXPORT_INIT$$Base)
#define EXPORT_INIT_END   (&Image$$ER_EXPORT_INIT$$Limit)

#if TM_EXPORT_EXIT_SECTION_ENABLE
extern const struct export_item Image$$ER_EXPORT_EXIT$$Base;
extern const struct export_item Image$$ER_EXPORT_EXIT$$Limit;

#define EXPORT_EXIT_BEGIN (&Image$$ER_EXPORT_EXIT$$Base)
#define EXPORT_EXIT_END   (&Image$$ER_EXPORT_EXIT$$Limit)
#endif

#elif defined(__IAR_SYSTEMS_ICC__)

extern const struct export_item _export_init_start[];
extern const struct export_item _export_init_end[];

#define EXPORT_INIT_BEGIN (_export_init_start)
#define EXPORT_INIT_END   (_export_init_end)

#if TM_EXPORT_EXIT_SECTION_ENABLE
extern const struct export_item _export_exit_start[];
extern const struct export_item _export_exit_end[];

#define EXPORT_EXIT_BEGIN (_export_exit_start)
#define EXPORT_EXIT_END   (_export_exit_end)
#endif

#elif defined(__GNUC__)

extern const struct export_item _export_init_start[];
extern const struct export_item _export_init_end[];

#define EXPORT_INIT_BEGIN (_export_init_start)
#define EXPORT_INIT_END   (_export_init_end)

#if TM_EXPORT_EXIT_SECTION_ENABLE
extern const struct export_item _export_exit_start[];
extern const struct export_item _export_exit_end[];

#define EXPORT_EXIT_BEGIN (_export_exit_start)
#define EXPORT_EXIT_END   (_export_exit_end)
#endif

#else
#error "Unsupported compiler toolchain."
#endif

/* Private Helpers ------------------------------------------------------------------------------ */

static struct tm_export_last_error _export_last_error = {0};

static inline bool               _export_validate_item(const struct export_item *item);
static inline int                _export_execute_item(const struct export_item *item);
static inline uint8_t            _export_clamp_level(uint8_t level);
static inline bool               _export_normalize_range(uint8_t *min_level, uint8_t *max_level);
static inline bool               _export_is_exit_item(const struct export_item *item);
static inline bool               _export_has_context(const struct export_item *item);
static inline bool               _export_is_void_hook(const struct export_item *item);
static inline bool               _export_item_in_range(const struct export_item *item,
                                                       bool                      want_exit,
                                                       uint8_t                   min_level,
                                                       uint8_t                   max_level);
static inline int                _export_compare_order(const struct export_item *lhs,
                                                       const struct export_item *rhs);
static const struct export_item *_export_find_ordered_item(const struct export_item *begin,
                                                           const struct export_item *end,
                                                           bool                      want_exit,
                                                           uint8_t                   min_level,
                                                           uint8_t                   max_level,
                                                           const struct export_item *last_item,
                                                           bool                      reverse);
static inline void _export_record_last_error(const struct export_item *item, int status);
static int         _export_execute_init_range(const struct export_item *begin,
                                              const struct export_item *end,
                                              uint8_t                   min_level,
                                              uint8_t                   max_level);
#if TM_EXPORT_EXIT_SECTION_ENABLE
static int _export_execute_exit_range(const struct export_item *begin,
                                      const struct export_item *end,
                                      uint8_t                   min_level,
                                      uint8_t                   max_level);
#endif

/* Public Functions ----------------------------------------------------------------------------- */

int export_init_execute(uint8_t max_level)
{
    return export_init_execute_range(EXPORT_LEVEL_FIRST, max_level);
}

int export_init_execute_range(uint8_t min_level, uint8_t max_level)
{
    return _export_execute_init_range(EXPORT_INIT_BEGIN, EXPORT_INIT_END, min_level, max_level);
}

int export_init_execute_from(uint8_t min_level)
{
    return export_init_execute_range(min_level, EXPORT_LEVEL_LAST);
}

bool export_init_last_error_get(struct tm_export_last_error *info)
{
    if (info == NULL) {
        return false;
    }

    *info = _export_last_error;
    return (_export_last_error.status != 0) && (_export_last_error.name != NULL);
}

int export_exit_execute(uint8_t min_level)
{
#if TM_EXPORT_EXIT_SECTION_ENABLE
    return _export_execute_exit_range(EXPORT_EXIT_BEGIN,
                                      EXPORT_EXIT_END,
                                      min_level,
                                      EXPORT_LEVEL_LAST);
#else
    (void)min_level;
    return 0;
#endif
}

/* Private Helpers ------------------------------------------------------------------------------ */

static inline bool _export_validate_item(const struct export_item *item)
{
    uint32_t expected_magic;

    if ((item == NULL) || (item->func.obj == NULL) || (item->name == NULL)) {
        return false;
    }
    if (item->magic_head != item->magic_tail) {
        return false;
    }

    if (item->level >= EXPORT_LEVEL_MAX) {
        return false;
    }

    expected_magic = _export_is_exit_item(item) ? EXPORT_ID_EXIT : EXPORT_ID_INIT;
    if (item->magic_head != expected_magic) {
        return false;
    }

    return true;
}

static inline int _export_execute_item(const struct export_item *item)
{
    if (_export_has_context(item)) {
        if (_export_is_void_hook(item)) {
            item->func.void_ctx_fn(item->ctx);
            return 0;
        }

        return item->func.int_ctx_fn(item->ctx);
    }

    if (_export_is_void_hook(item)) {
        item->func.void_fn();
        return 0;
    }

    return item->func.int_fn();
}

static inline uint8_t _export_clamp_level(uint8_t level)
{
    if (level >= EXPORT_LEVEL_MAX) {
        return (uint8_t)EXPORT_LEVEL_LAST;
    }

    return level;
}

static inline bool _export_is_exit_item(const struct export_item *item)
{
    return ((item->flags & EXPORT_FLAG_EXIT) != 0U);
}

static inline bool _export_has_context(const struct export_item *item)
{
    return ((item->flags & EXPORT_FLAG_CONTEXT) != 0U);
}

static inline bool _export_is_void_hook(const struct export_item *item)
{
    return ((item->flags & EXPORT_FLAG_VOID) != 0U);
}

static inline bool _export_item_in_range(const struct export_item *item,
                                         bool                      want_exit,
                                         uint8_t                   min_level,
                                         uint8_t                   max_level)
{
    if (!_export_validate_item(item)) {
        return false;
    }
    if (_export_is_exit_item(item) != want_exit) {
        return false;
    }

    return (item->level >= min_level) && (item->level <= max_level);
}

static inline int _export_compare_order(const struct export_item *lhs,
                                        const struct export_item *rhs)
{
    uintptr_t lhs_addr;
    uintptr_t rhs_addr;

    if (lhs == rhs) {
        return 0;
    }
    if (lhs->level != rhs->level) {
        return (lhs->level < rhs->level) ? -1 : 1;
    }
    if (lhs->priority != rhs->priority) {
        return (lhs->priority < rhs->priority) ? -1 : 1;
    }

    lhs_addr = (uintptr_t)lhs;
    rhs_addr = (uintptr_t)rhs;

    return (lhs_addr < rhs_addr) ? -1 : 1;
}

static const struct export_item *_export_find_ordered_item(const struct export_item *begin,
                                                           const struct export_item *end,
                                                           bool                      want_exit,
                                                           uint8_t                   min_level,
                                                           uint8_t                   max_level,
                                                           const struct export_item *last_item,
                                                           bool                      reverse)
{
    const struct export_item *candidate = NULL;
    const struct export_item *item;

    for (item = begin; item < end; item++) {
        int compare_result;

        if (!_export_item_in_range(item, want_exit, min_level, max_level)) {
            continue;
        }

        if (last_item != NULL) {
            compare_result = _export_compare_order(item, last_item);
            if ((!reverse && (compare_result <= 0)) || (reverse && (compare_result >= 0))) {
                continue;
            }
        }

        if (candidate == NULL) {
            candidate = item;
            continue;
        }

        compare_result = _export_compare_order(item, candidate);
        if ((!reverse && (compare_result < 0)) || (reverse && (compare_result > 0))) {
            candidate = item;
        }
    }

    return candidate;
}

static inline void _export_record_last_error(const struct export_item *item, int status)
{
    if ((item == NULL) || (status == 0)) {
        _export_last_error.name     = NULL;
        _export_last_error.level    = 0U;
        _export_last_error.priority = 0U;
        _export_last_error.status   = 0;
        return;
    }

    _export_last_error.name     = item->name;
    _export_last_error.level    = item->level;
    _export_last_error.priority = item->priority;
    _export_last_error.status   = status;
}

static inline bool _export_normalize_range(uint8_t *min_level, uint8_t *max_level)
{
    if ((min_level == NULL) || (max_level == NULL)) {
        return false;
    }

    *min_level = _export_clamp_level(*min_level);
    *max_level = _export_clamp_level(*max_level);

    return *min_level <= *max_level;
}

static int _export_execute_init_range(const struct export_item *begin,
                                      const struct export_item *end,
                                      uint8_t                   min_level,
                                      uint8_t                   max_level)
{
    const struct export_item *item;
    const struct export_item *last_item = NULL;
#if !TM_EXPORT_INIT_FAIL_FAST
    int first_error = 0;
#endif

    _export_record_last_error(NULL, 0);

    if (!_export_normalize_range(&min_level, &max_level)) {
        return -EINVAL;
    }

    while (true) {
        int result;

        item = _export_find_ordered_item(begin, end, false, min_level, max_level, last_item, false);
        if (item == NULL) {
            break;
        }

        last_item = item;
        result    = _export_execute_item(item);
        if (result != 0) {
            _export_record_last_error(item, result);
#if TM_EXPORT_INIT_FAIL_FAST
            return result;
#else
            if (first_error == 0) {
                first_error = result;
            }
#endif
        }
    }

#if TM_EXPORT_INIT_FAIL_FAST
    return 0;
#else
    return first_error;
#endif
}

#if TM_EXPORT_EXIT_SECTION_ENABLE
static int _export_execute_exit_range(const struct export_item *begin,
                                      const struct export_item *end,
                                      uint8_t                   min_level,
                                      uint8_t                   max_level)
{
    const struct export_item *item;
    const struct export_item *last_item   = NULL;
    int                       first_error = 0;

    if (!_export_normalize_range(&min_level, &max_level)) {
        return -EINVAL;
    }

    while (true) {
        int result;

        item = _export_find_ordered_item(begin, end, true, min_level, max_level, last_item, true);
        if (item == NULL) {
            break;
        }

        last_item = item;
        result    = _export_execute_item(item);
        if ((result != 0) && (first_error == 0)) {
            first_error = result;
        }
    }

    return first_error;
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
