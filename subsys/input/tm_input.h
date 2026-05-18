/**
 * @file    tm_input.h
 * @brief   Shared physical input gesture subsystem
 * @version v3.0.0
 * @date    2026.04.23
 *
 * Design overview
 * ---------------
 * `tm_input` is the shared gesture layer above `bsp_input`:
 * - `bsp_input` owns board topology and raw pressed snapshots
 * - `tm_input` owns debounce, click/long/repeat interpretation, and one
 *   shared dispatch contract
 * - product routing, page semantics, and task cadence stay outside this
 *   subsystem
 *
 * Usage example
 * -------------
 * struct bsp_input_sample sample = {0};
 *
 * struct button_config config = {
 *     .active_low            = true,
 *     .trigger_mode          = BUTTON_TRIGGER_ON_RELEASE,
 *     .debounce_ticks        = 1U,
 *     .long_press_ticks      = 100U,
 *     .repeat_start_ticks    = 100U,
 *     .repeat_interval_ticks = 10U,
 * };
 *
 * tm_input_init(&config);
 * tm_input_set_handler(handler, ctx);
 * bsp_input_sample(&sample);
 * tm_input_process_sample(&sample);
 *
 * Ownership / lifecycle / context
 * --------------------------------
 * - Caller-owned task owners provide one explicit button timing policy, then
 *   call `tm_input_init()` and `tm_input_process_sample()`.
 * - The installed handler is producer-side routing glue only; it must not
 *   become a second sampling owner.
 * - Board input ids and input count come from `bsp_input.h`; this subsystem
 *   must not mirror board topology with private public enums.
 *
 * Blocking
 * --------
 * - `tm_input_process_sample()` is one bounded owner-side sample step.
 * - `tm_input_set_handler()` stays non-blocking.
 * - Snapshot helpers and lookup helpers are read-only.
 *
 * Failure / recovery
 * ------------------
 * - `tm_input_status()` is the formal shared health probe for init/runtime
 *   faults seen by the sampling owner.
 * - Invalid debug injection requests fail with explicit `-errno`.
 * - Retry cadence and any re-init policy remain with the caller-owned task
 *   that samples the hardware.
 */

#pragma once

/* Includes --------------------------------------------------------------------- */
#include <stdbool.h>
#include <stdint.h>
#include "bsp_input.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Public Macros ---------------------------------------------------------------- */

/**
 * @brief Recommended periodic sampling cadence for the default button model.
 */
#define TM_INPUT_PROCESS_TICK_MS (10U)

/* Public Types ----------------------------------------------------------------- */

struct button_config;

/**
 * @brief Shared input dispatch results.
 */
enum tm_input_dispatch_result {
    /** Event was consumed normally. */
    TM_INPUT_DISPATCH_HANDLED = 0,
    /** Event was not recognized by the handler. */
    TM_INPUT_DISPATCH_IGNORED = 2,
};

/**
 * @brief One interpreted physical input event.
 */
struct tm_input_event {
    /** Button event bitmask from `button.h`. */
    uint16_t event_mask;
    /** Board input source that produced the event. */
    enum bsp_input_id input_id;
    /** Click-window count for click-class events. */
    uint8_t click_count;
};

/**
 * @brief Producer-side event routing callback.
 *
 * @note Role: producer-side routing glue only.
 * @note Context: task-only / ISR-forbidden.
 * @note Positive success values other than `TM_INPUT_DISPATCH_IGNORED` are
 *       normalized to `TM_INPUT_DISPATCH_HANDLED`.
 */
typedef int (*tm_input_event_handler_t)(const struct tm_input_event *event, void *context);

/**
 * @brief Per-input diagnostic counters.
 */
struct tm_input_channel_diag {
    /** Number of click-class events seen on this input. */
    uint32_t click_count;
    /** Number of long-press events seen on this input. */
    uint32_t long_press_count;
    /** Number of repeat events seen on this input. */
    uint32_t repeat_count;
    /** Last event mask observed on this input. */
    uint16_t last_event_mask;
    /** Last click-window count observed on this input. */
    uint8_t last_click_count;
};

/**
 * @brief Lightweight shared runtime snapshot.
 */
struct tm_input_state_snapshot {
    /** Last handler result or negative `-errno`. */
    int last_dispatch_result;
    /** Last dispatched button event mask. */
    uint16_t last_event_mask;
    /** Last dispatched board input id. */
    enum bsp_input_id last_input_id;
    /** Last dispatched click count. */
    uint8_t last_click_count;
    /** `true` after init completed successfully. */
    bool initialized;
    /** `true` when one routing handler is installed. */
    bool handler_installed;
};

/**
 * @brief Full shared diagnostic snapshot.
 */
struct tm_input_diag_snapshot {
    /** Per-input diagnostic counters indexed by `enum bsp_input_id`. */
    struct tm_input_channel_diag channels[BSP_INPUT_ID_COUNT];
    /** Total board samples consumed by `tm_input_process_sample()`. */
    uint32_t sample_count;
    /** Total interpreted events dispatched through the shared handler contract. */
    uint32_t dispatch_count;
    /** Total dispatch attempts that returned a negative error. */
    uint32_t dispatch_error_count;
    /** Total dispatch results equal to `TM_INPUT_DISPATCH_HANDLED`. */
    uint32_t dispatch_handled_count;
    /** Total dispatch results equal to `TM_INPUT_DISPATCH_IGNORED`. */
    uint32_t dispatch_ignored_count;
    /** Current shared status latch. */
    int status;
    /** Last handler result or negative `-errno`. */
    int last_dispatch_result;
    /** Last dispatched button event mask. */
    uint16_t last_dispatch_event_mask;
    /** Last dispatched board input id. */
    enum bsp_input_id last_dispatch_input_id;
    /** Last dispatched click count. */
    uint8_t last_dispatch_click_count;
    /** `true` after init completed successfully. */
    bool initialized;
};

/* Public Functions ------------------------------------------------------------- */

/**
 * @brief Initialize the shared input gesture subsystem.
 *
 * @param  config  Caller-selected button timing policy copied into each shared
 *                 channel during the first successful init.
 *
 * @return `0` on success, or negative `-errno` on failure.
 *
 * @retval -EINVAL  `config` is NULL.
 *
 * @note Repeated successful calls are idempotent and keep the first applied
 *       policy until reboot.
 */
int tm_input_init(const struct button_config *config);

/**
 * @brief Consume one board input sample snapshot.
 *
 * @param  sample  Caller-owned sample snapshot from `bsp_input_sample()`.
 *
 * @return `0` on success, or negative `-errno` on failure.
 */
int tm_input_process_sample(const struct bsp_input_sample *sample);

/**
 * @brief Install or replace the shared routing handler.
 *
 * @param  handler   Callback to receive interpreted events, or `NULL`.
 * @param  context   Borrowed caller context forwarded to `handler`.
 *
 * @return `0` on success, or negative `-errno` on failure.
 */
int tm_input_set_handler(tm_input_event_handler_t handler, void *context);

/**
 * @brief Inject one already-interpreted input event through the formal dispatch contract.
 *
 * @param  input_id      Board input id to inject.
 * @param  event_mask    Button event bits such as `BUTTON_EVENT_CLICKED`.
 * @param  click_count   Click-window count for click-class events.
 *
 * @return Non-negative `TM_INPUT_DISPATCH_*` on success, or negative `-errno`
 *         on failure. `-EINVAL` reports invalid ids or zero event masks,
 *         `-EIO` reports an internal lock failure, and installed handler
 *         failures are forwarded unchanged.
 *
 * @note  Ordinary runtime callers stay in task context. Debugger-driven calls
 *        may also reuse this helper while the scheduler is suspended because
 *        the lock path degrades to a no-wait probe.
 */
int tm_input_debug_inject_event(enum bsp_input_id input_id,
                                uint16_t          event_mask,
                                uint8_t           click_count);

/**
 * @brief Return the current shared input status latch.
 *
 * @return `0` when healthy, or negative `-errno` when init/runtime failed.
 */
int tm_input_status(void);

/**
 * @brief Read one lightweight shared runtime snapshot.
 *
 * @param  snapshot  Output structure owned by the caller.
 *
 * @return `true` when the snapshot was produced successfully.
 */
bool tm_input_state_snapshot_get(struct tm_input_state_snapshot *snapshot);

/**
 * @brief Read one full shared diagnostic snapshot.
 *
 * @param  snapshot  Output structure owned by the caller.
 *
 * @return `true` when the snapshot was produced successfully.
 */
bool tm_input_diag_snapshot(struct tm_input_diag_snapshot *snapshot);

/**
 * @brief Return the board-facing name for one input id.
 *
 * @param  input_id  Board input id to describe.
 *
 * @return Stable board-facing input name, or `"none"` / `"unknown"` when the
 *         id is invalid.
 */
const char *tm_input_name_get(enum bsp_input_id input_id);

/**
 * @brief Return a short human-readable name for one event mask.
 *
 * @param  event_mask  Button event mask to describe.
 *
 * @return Short stable name such as `"click"` or `"long"`.
 */
const char *tm_input_event_name_get(uint16_t event_mask);

/**
 * @brief Return a short human-readable name for one dispatch result.
 *
 * @param  result  One `TM_INPUT_DISPATCH_*` code or a negative `-errno`.
 *
 * @return Short stable result name.
 */
const char *tm_input_dispatch_result_name_get(int result);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
