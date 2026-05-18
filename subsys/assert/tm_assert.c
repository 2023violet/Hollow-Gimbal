/**
 * @file    tm_assert.c
 * @brief   Themis assertion reporting and fatal-fault escalation backend
 * @version v5.1.0
 * @date    2026.03.28
 *
 * Implementation role
 * -------------------
 * The backend intentionally streams short fragments directly to the configured
 * writer. That keeps the stack footprint bounded even when asserts fire from
 * already-compromised code paths where allocating a larger formatting buffer
 * would increase failure risk.
 *
 * Key collaborators
 * -----------------
 * - `tm_assert.h`: public frontend macros and configuration contract
 * - `tm_runtime`: IRQ critical-section helpers used for the reentry gate
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "tm_assert.h"
#include "tm_runtime.h"

#if TM_ASSERT_ENABLE

/* Private State ------------------------------------------------------------------------------- */
static struct tm_assert_config _assert_cfg = {0};

/*
 * Re-entry gate:
 * Kept volatile so the compiler does not cache it across the short IRQ critical
 * section. This prevents a nested assert from overwriting the first failure's
 * report path.
 */
static volatile bool _assert_active = false;

#if TM_ASSERT_USE_ERROR_MAP
static const struct tm_assert_error_map *_custom_map     = NULL;
static size_t                            _custom_map_cnt = 0;

/* Default System Errors ------------------------------------------------------------------------ */
static const struct tm_assert_error_map _default_map[] = {
    {0x01, "GEN_ERR"},
    {0x02, "INV_PARAM"},
    {0x03, "NO_MEM"},
    {0x04, "DEV_ERR"},
    {0x05, "TIMEOUT"},
    {0x06, "BUF_OVF"},
};
#endif /* TM_ASSERT_USE_ERROR_MAP */

/* Private Helpers ----------------------------------------------------------------------------- */
static void _uint32_to_str(uint32_t val, char *buf);
static void _safe_out(const char *s);
static bool _assert_try_enter(void);
static void _assert_leave(void);

/*
 * _get_error_str is only needed when the error-map feature is compiled in.
 * Guard the prototype and definition together to avoid an -Wunused-function
 * diagnostic when TM_ASSERT_USE_ERROR_MAP == 0.
 */
#if TM_ASSERT_USE_ERROR_MAP
static const char *_get_error_str(uint32_t id);
#endif

/* Public Functions ----------------------------------------------------------------------------- */

void tm_assert_init(const struct tm_assert_config *config)
{
    if (config) {
        /* Copy-by-value lets callers provide stack-local config structures safely. */
        _assert_cfg = *config;
    }
}

#if TM_ASSERT_USE_ERROR_MAP
void tm_assert_register_map(const struct tm_assert_error_map *map, size_t count)
{
    /* Borrow the table directly; the caller retains lifetime ownership. */
    _custom_map     = map;
    _custom_map_cnt = count;
}
#endif

void _tm_assert_failed(const char          *tag,
                       uint32_t             line,
                       enum tm_assert_level level,
                       const char          *func,
                       uint32_t             id,
                       const char          *msg,
                       const char          *cond)
{
    char num_buf[12];

    if (level >= TM_ASSERT_LEVEL_MAX) {
        level = TM_ASSERT_LEVEL_FATAL;
    }

    /* Reentrancy protection: if assert fires again during output, stop immediately
     * so the original fault context is not overwritten. Only the gate mutation stays
     * inside the IRQ critical section; output itself must not globally stall IRQs.
     */
    if (!_assert_try_enter()) {
        if (_assert_cfg.handler) {
            _assert_cfg.handler();
        }
        while (1)
            ;
    }

    /*
     * Streaming format:
     *   [A/TAG L:123] FUNC: Msg/Condition
     *
     * Pieces are emitted separately so the assert path never needs a large
     * temporary formatting buffer.
     */

    _safe_out("\r\n[A/");
    _safe_out(tag ? tag : "SYS");

    _safe_out(" L:");
    _uint32_to_str(line, num_buf);
    _safe_out(num_buf);

    _safe_out("] ");

    if (func) {
        _safe_out(func);
        _safe_out(": ");
    }

#if TM_ASSERT_USE_ERROR_MAP
    if (id != 0) {
        _safe_out("Err=");
        _safe_out(_get_error_str(id));
        _safe_out(" ");
    }
#else
    (void)id;
#endif

    if (msg) {
        _safe_out(msg);
    } else {
        _safe_out("Failed: ");
        _safe_out(cond);
    }
    _safe_out("\r\n");

    /* Handle Fatal Level */
    if (level == TM_ASSERT_LEVEL_FATAL) {
        if (_assert_cfg.handler) {
            _assert_cfg.handler();
        }
        /* Fallback dead loop if handler returns or is NULL */
        while (1)
            ;
    }

    _assert_leave();
}

/* Private Functions ---------------------------------------------------------------------------- */

/**
 * @brief  Stream one string fragment if a backend is installed.
 */
static void _safe_out(const char *s)
{
    if (_assert_cfg.write && s) {
        /* Keep this path freestanding-friendly and avoid pulling in string helpers. */
        size_t      len = 0;
        const char *p   = s;
        while (*p++)
            len++;
        _assert_cfg.write(s, (uint16_t)len);
    }
}

/**
 * @brief  Convert one unsigned integer to decimal text without libc helpers.
 */
static void _uint32_to_str(uint32_t val, char *buf)
{
    char    *p      = buf;
    uint32_t temp   = val;
    int      digits = 0;

    if (val == 0) {
        *p++ = '0';
        *p   = '\0';
        return;
    }

    /* Count digits first so the write pass can fill backwards once. */
    while (temp > 0) {
        temp /= 10;
        digits++;
    }

    /* Fill backwards to avoid an additional temporary buffer or reverse step. */
    p += digits;
    *p = '\0';

    temp = val;
    while (temp > 0) {
        *(--p) = (char)('0' + (temp % 10));
        temp /= 10;
    }
}

#if TM_ASSERT_USE_ERROR_MAP
/**
 * @brief  Resolve one error-id to text through the custom table first.
 */
static const char *_get_error_str(uint32_t id)
{
    /* Project-specific mappings intentionally override the built-in defaults. */
    if (_custom_map) {
        for (size_t i = 0; i < _custom_map_cnt; i++) {
            if (_custom_map[i].error_id == (uint16_t)id)
                return _custom_map[i].error_str;
        }
    }
    /* Fall back to the minimal built-in map so diagnostics remain readable. */
    for (size_t i = 0; i < sizeof(_default_map) / sizeof(_default_map[0]); i++) {
        if (_default_map[i].error_id == (uint16_t)id)
            return _default_map[i].error_str;
    }

    return "UNKNOWN";
}
#endif /* TM_ASSERT_USE_ERROR_MAP */

static bool _assert_try_enter(void)
{
    uint32_t level;
    bool     entered = false;

    level = tm_irq_critical_enter();
    if (!_assert_active) {
        _assert_active = true;
        entered        = true;
    }
    /* Only the gate mutation stays inside the IRQ critical section. */
    tm_irq_critical_exit(level);

    return entered;
}

static void _assert_leave(void)
{
    uint32_t level;

    level          = tm_irq_critical_enter();
    _assert_active = false;
    tm_irq_critical_exit(level);
}

#endif /* TM_ASSERT_ENABLE */

/* ---------------------------------------- end of file ----------------------------------------- */
