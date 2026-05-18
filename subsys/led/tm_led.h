/**
 * @file    tm_led.h
 * @brief   Shared LED timing subsystem
 * @version v3.0.0
 * @date    2026.04.23
 *
 * Design overview
 * ---------------
 * `tm_led` is the shared timing layer above `bsp_led`:
 * - `bsp_led` owns board LED topology and raw/logical hardware access
 * - `tm_led` owns one-shot pulse, simple blink timing, and steady logical hold
 * - product meaning such as "key feedback" or "heating heartbeat" stays in
 *   higher product owners
 *
 * Usage example
 * -------------
 * tm_led_init();
 * tm_led_set_logical(BSP_LED_ID_1, true);
 * tm_led_process(0U);
 *
 * Ownership / lifecycle / context
 * --------------------------------
 * - Caller-owned task owners call `tm_led_init()`, `tm_led_wait()`, and
 *   `tm_led_process()`.
 * - Producer-side callers submit pulse/blink/logical requests only.
 * - Board LED ids come from `bsp_led.h`; this subsystem must not mirror board
 *   topology with second-owner public enums.
 *
 * Blocking
 * --------
 * - `tm_led_wait()` is owner-only and may block until new work or the next
 *   timing deadline.
 * - Control-plane setters are task-context, bounded, and non-blocking.
 * - State/diag getters are read-only snapshots.
 *
 * Failure / recovery
 * ------------------
 * - `tm_led_status()` is the shared health probe for init/wait/runtime faults
 *   seen by the owner task.
 * - Invalid LED ids or illegal timing arguments fail with explicit `-errno`.
 * - Retry cadence and re-init policy remain with the caller-owned task owner.
 */

#pragma once

/* Includes --------------------------------------------------------------------- */
#include <stdbool.h>
#include <stdint.h>
#include "bsp_led.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Public Macros ---------------------------------------------------------------- */

/**
 * @brief Sentinel timeout meaning "wait until work arrives".
 */
#define TM_LED_WAIT_FOREVER_MS (UINT32_MAX)

/* Public Types ----------------------------------------------------------------- */

/**
 * @brief Per-LED diagnostic counters and runtime state.
 */
struct tm_led_channel_diag {
    /** Number of pulse requests started for this LED. */
    uint32_t pulse_count;
    /** Remaining active timing window in milliseconds. */
    uint32_t remaining_ms;
    /** Configured blink-on duration in milliseconds. */
    uint32_t blink_on_ms;
    /** Configured blink-off duration in milliseconds. */
    uint32_t blink_off_ms;
    /** `true` when a newly submitted pulse is waiting to start. */
    bool pulse_pending;
    /** `true` while blink mode is active. */
    bool blink_active;
    /** Current logical output state. */
    bool logical_on;
};

/**
 * @brief Lightweight per-LED runtime snapshot.
 */
struct tm_led_channel_snapshot {
    /** Remaining active timing window in milliseconds. */
    uint32_t remaining_ms;
    /** Configured blink-on duration in milliseconds. */
    uint32_t blink_on_ms;
    /** Configured blink-off duration in milliseconds. */
    uint32_t blink_off_ms;
    /** `true` while blink mode is active. */
    bool blink_active;
    /** Current logical output state. */
    bool logical_on;
};

/**
 * @brief Full shared LED diagnostic snapshot.
 */
struct tm_led_diag_snapshot {
    /** Per-LED diagnostics indexed by `enum bsp_led_id`. */
    struct tm_led_channel_diag leds[BSP_LED_ID_COUNT];
    /** Current shared status latch. */
    int status;
    /** `true` after init completed successfully. */
    bool initialized;
};

/* Public Functions ------------------------------------------------------------- */

/**
 * @brief Initialize the shared LED timing subsystem.
 *
 * @return `0` on success, or negative `-errno` on failure.
 */
int tm_led_init(void);

/**
 * @brief Return the owner's next requested wakeup delay.
 *
 * @return Milliseconds until the next owner wakeup, `0` when work is pending,
 *         or `TM_LED_WAIT_FOREVER_MS` when no timed work exists.
 */
uint32_t tm_led_next_wakeup_ms(void);

/**
 * @brief Wait until new work arrives or the next timing deadline expires.
 *
 * @param  timeout_ms  Maximum wait duration in milliseconds.
 *
 * @return `0` on success, or negative `-errno` on failure.
 */
int tm_led_wait(uint32_t timeout_ms);

/**
 * @brief Advance one owner-side timing step.
 *
 * @param  elapsed_ms  Milliseconds elapsed since the previous owner cycle.
 *
 * @return `0` on success, or negative `-errno` on failure.
 */
int tm_led_process(uint32_t elapsed_ms);

/**
 * @brief Queue one one-shot pulse request for a board LED.
 *
 * @param  led          Board LED id to pulse.
 * @param  duration_ms  Pulse duration in milliseconds.
 *
 * @return `0` on success, or negative `-errno` on failure.
 */
int tm_led_submit_pulse(enum bsp_led_id led, uint32_t duration_ms);

/**
 * @brief Enable or disable blink timing for one board LED.
 *
 * @param  led      Board LED id.
 * @param  active   Whether blink mode should be active.
 * @param  on_ms    Blink-on duration when active.
 * @param  off_ms   Blink-off duration when active.
 *
 * @return `0` on success, or negative `-errno` on failure.
 */
int tm_led_set_blink(enum bsp_led_id led, bool active, uint32_t on_ms, uint32_t off_ms);

/**
 * @brief Force one board LED into a steady logical state.
 *
 * @param  led  Board LED id.
 * @param  on   Target logical state.
 *
 * @return `0` on success, or negative `-errno` on failure.
 */
int tm_led_set_logical(enum bsp_led_id led, bool on);

/**
 * @brief Toggle one board LED's steady logical state.
 *
 * @param  led  Board LED id.
 *
 * @return `0` on success, or negative `-errno` on failure.
 */
int tm_led_toggle_logical(enum bsp_led_id led);

/**
 * @brief Clear all shared LED runtime modes and drive all LEDs off.
 *
 * @return `0` on success, or negative `-errno` on failure.
 */
int tm_led_all_off(void);

/**
 * @brief Return the current shared LED status latch.
 *
 * @return `0` when healthy, or negative `-errno` when init/runtime failed.
 */
int tm_led_status(void);

/**
 * @brief Read one lightweight per-LED snapshot.
 *
 * @param  led       Board LED id.
 * @param  snapshot  Output structure owned by the caller.
 *
 * @return `true` when the snapshot was produced successfully.
 */
bool tm_led_snapshot_get(enum bsp_led_id led, struct tm_led_channel_snapshot *snapshot);

/**
 * @brief Read one full shared LED diagnostic snapshot.
 *
 * @param  snapshot  Output structure owned by the caller.
 *
 * @return `true` when the snapshot was produced successfully.
 */
bool tm_led_diag_snapshot(struct tm_led_diag_snapshot *snapshot);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
