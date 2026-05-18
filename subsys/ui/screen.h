/**
 * @file    screen.h
 * @brief   Shared Screen Stack contracts for upper-layer UI routing
 * @version v1.0.0
 * @date    2026.04.14
 *
 * Design overview
 * ---------------
 * Screen Stack is the shared upper-layer UI routing model for Themis:
 * caller-owned UI bridges normalize product input into generic `screen_event`
 * objects, the active page interprets page-local semantics, and a render owner
 * asks the stack to draw the current page.
 *
 * Ownership / lifecycle / context
 * --------------------------------
 * - Each exported `screen_xxx_get()` returns one caller-borrowed static screen
 *   object; screens are not heap-allocated.
 * - `screen_manager` owns stack topology, but individual screens own their
 *   page semantics and any private state reachable from `screen->ctx`.
 * - `on_draw()` and `on_key()` run on caller-owned task-context paths and must
 *   therefore keep shared state explicit and small.
 * - All callback hooks are borrowed function pointers. They execute inline on
 *   caller-owned owner paths; this contract exports no mutex, queue, or heap
 *   policy of its own.
 *
 * Blocking / resources / diagnostics
 * ----------------------------------
 * - This header is pure shared contract surface: no hidden allocation, no
 *   queue ownership, and no blocking behavior are introduced here.
 * - Callback cost is paid entirely on the caller-owned owner path that invokes
 *   the hook.
 * - Evidence stays explicit through callback return values and caller-owned
 *   diagnostics rather than hidden screen-stack state.
 *
 * Failure / recovery
 * ------------------
 * - Missing hooks mean the corresponding capability is absent; the manager or
 *   outer owner must surface that fact explicitly.
 * - Screens do not own retry, recovery, or transport fallback policy.
 * - Page-local fault evidence should live with the page/service owner instead
 *   of being hidden inside this shared contract header.
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Public Macros -------------------------------------------------------------------------------- */

/**
 * @brief Sentinel page id meaning the descriptor is not yet bound to the registry.
 */
#define SCREEN_PAGE_ID_UNBOUND (UINT16_MAX)

/* Public Types --------------------------------------------------------------------------------- */

/**
 * @brief Stack-level result produced by one page key handler.
 */
enum screen_result {
    /** Event was consumed and navigation is unchanged. */
    SCREEN_HANDLED = 0,
    /** Event was consumed and the stack should pop. */
    SCREEN_POP = 1,
    /** Event was not recognized by the current page. */
    SCREEN_IGNORED = 2,
};

/**
 * @brief Generic input gesture normalized by the caller-owned UI bridge.
 */
enum screen_event_gesture {
    /** No actionable gesture was associated with the event. */
    SCREEN_EVENT_GESTURE_NONE = 0,
    /** One accepted activate/select gesture. */
    SCREEN_EVENT_GESTURE_SELECT = 1,
    /** One accepted hold/long-press gesture. */
    SCREEN_EVENT_GESTURE_HOLD = 2,
    /** One accepted held-repeat gesture. */
    SCREEN_EVENT_GESTURE_REPEAT = 3,
};

struct screen;
struct mono_canvas;

/**
 * @brief Caller-normalized input event forwarded to the active page.
 *
 * @note  Product-specific button ids, keypad topology, and debounce-engine
 *        details stay above this framework contract.
 */
struct screen_event {
    /** Generic gesture selected by the caller-owned input bridge. */
    uint8_t gesture;
    /** Opaque caller-defined semantic source id, or `0` when not applicable. */
    uint8_t key_id;
    /** Click-window count for click-class events. */
    uint8_t click_count;
};

/**
 * @brief Owner-only lifecycle hook invoked by stack mutations.
 *
 * @note Role: owner-only.
 * @note Context: task-only / ISR-forbidden.
 * @note Blocking/resources: must stay bounded; do not hide long BSP/storage
 *        work inside lifecycle hooks.
 */
typedef void (*screen_lifecycle_fn)(struct screen *self);
/**
 * @brief Owner-only draw hook for the active page.
 *
 * @note Role: owner-only render callback.
 * @note Context: task-only / ISR-forbidden.
 * @note Blocking/resources: executes inline on the render owner's path, so it
 *        must remain heap-free and avoid long blocking side effects.
 */
typedef void (*screen_draw_fn)(struct screen *self, struct mono_canvas *canvas);
/**
 * @brief Owner-only physical key dispatcher for the active page.
 *
 * @note Role: owner-only input callback.
 * @note Context: task-only / ISR-forbidden.
 * @note Errors/evidence: returns `SCREEN_HANDLED`, `SCREEN_POP`, or
 *        `SCREEN_IGNORED`; page-local faults should still be surfaced through
 *        the owning service/page diagnostics instead of hidden here.
 */
typedef enum screen_result (*screen_key_fn)(struct screen *self, const struct screen_event *event);
/**
 * @brief Read the next owner wakeup delay desired by the active page.
 *
 * @note Role: state-only timing callback.
 * @note Context: task-only / ISR-forbidden.
 * @note Blocking/resources: must be read-only, bounded, and allocation-free.
 */
typedef uint32_t (*screen_wakeup_delay_fn)(struct screen *self);
/**
 * @brief Consume one owner-side wakeup tick for the active page.
 *
 * @note Role: owner-only timed callback.
 * @note Context: task-only / ISR-forbidden.
 * @note Errors/evidence: negative returns should use `-errno` style and are
 *        surfaced by the owner that called `screen_manager_process_wakeup()`.
 */
typedef int (*screen_wakeup_fn)(struct screen *self);

/**
 * @brief Static page descriptor borrowed by the screen stack.
 *
 * @note  The manager borrows this object for the life of the process. The
 *        caller must keep both the descriptor and `ctx` storage valid.
 * @note  The descriptor is pure contract/state wiring only; it owns no
 *        dynamic allocation and exports no hidden init-after-register step.
 */
struct screen {
    /** Optional first-enter hook. */
    screen_lifecycle_fn on_enter;
    /** Optional final-exit hook. */
    screen_lifecycle_fn on_exit;
    /** Optional resume hook after child pop. */
    screen_lifecycle_fn on_resume;
    /** Draw hook for active-page rendering. */
    screen_draw_fn on_draw;
    /** Physical key-event handler. */
    screen_key_fn on_key;
    /** Optional owner wakeup-budget hook. */
    screen_wakeup_delay_fn next_wakeup_ms;
    /** Optional owner wakeup callback. */
    screen_wakeup_fn on_wakeup;
    /** Registry page id, or `SCREEN_PAGE_ID_UNBOUND` before registration. */
    uint16_t page_id;
    /** Caller-owned private state borrowed by the page implementation. */
    void *ctx;
};

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
