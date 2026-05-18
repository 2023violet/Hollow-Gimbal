/**
 * @file    screen_row_editor.h
 * @brief   Shared row-based editor helper for Screen Stack pages
 * @version v1.0.0
 * @date    2026.04.14
 *
 * Design overview
 * ---------------
 * `screen_row_editor` captures the tiny browse/edit state machine reused by
 * settings-style Screen Stack pages:
 * - one cursor moving through a vertical row list
 * - one edit mode entered by long-press
 * - one repeat gate that prevents accidental fast adjustment on the first hold
 *
 * It is intentionally not a widget framework. Callers still own row
 * descriptors, value formatting, page transitions, and all product semantics.
 *
 * Ownership / lifecycle / context
 * --------------------------------
 * - Screen pages own each `struct screen_row_editor_state` instance.
 * - This helper stays heap-free and stores no hidden global runtime state.
 * - Public APIs are task-only / ISR-forbidden and assume one caller-owned
 *   page/input/render path.
 * - The draw helper borrows caller-owned canvas/font/text inputs only for the
 *   duration of each call.
 *
 * Blocking / resources / diagnostics
 * ----------------------------------
 * - All helpers are synchronous, bounded, and heap-free.
 * - `handle_step_key()` returns one explicit action enum so the page owner can
 *   decide whether semantic changes should trigger product behavior.
 * - `draw_row()` stays a caller-owned render helper and returns explicit
 *   `-errno` evidence for invalid call contracts.
 *
 * Failure / recovery
 * ------------------
 * - Contract violations degrade to explicit no-op / `-EINVAL` evidence rather
 *   than hidden state repair.
 * - This helper owns no retry loop, persistence, or recovery counters.
 * - If a page needs fallback cursor policy or fault reporting, that policy
 *   belongs to the page/service owner above this shared mechanism.
 *
 * Usage example
 * -------------
 * screen_row_editor_prepare(&state, &layout, remembered_cursor);
 *
 * action = screen_row_editor_handle_step_key(&state, &layout, event, +1);
 * if (action == SCREEN_ROW_EDITOR_ACTION_CURSOR_MOVED) {
 *     render_settings_page();
 * }
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include <stdbool.h>
#include <stdint.h>
#include "mono_canvas.h"
#include "screen.h"

#ifdef __cplusplus
extern "C" {
#endif

struct screen_row_editor_state {
    /** Current selected row index in the logical item list. */
    uint8_t cursor;
    /** First visible row index for page-local scroll management. */
    uint8_t scroll_offset;
    /** `true` while the page is actively editing the selected row value. */
    bool    adjust_mode;
    /** `true` after the first repeat-safe hold step has already been consumed. */
    bool    repeat_enabled;
};

struct screen_row_editor_layout {
    /** X coordinate of the cursor indicator glyph. */
    uint16_t cursor_x;
    /** X coordinate of the left-aligned label column. */
    uint16_t label_x;
    /** Right-edge X coordinate of the value column. */
    uint16_t value_right_x;
    /** Total logical row count owned by the page. */
    uint8_t  item_count;
    /** Number of rows visible in one render pass. */
    uint8_t  visible_rows;
};

enum screen_row_editor_action {
    /** No semantic change was accepted. */
    SCREEN_ROW_EDITOR_ACTION_NONE = 0,
    /** Cursor moved to another logical row. */
    SCREEN_ROW_EDITOR_ACTION_CURSOR_MOVED,
    /** Long-press entered edit mode on the selected row. */
    SCREEN_ROW_EDITOR_ACTION_ENTER_EDIT,
    /** One bounded value adjustment was accepted. */
    SCREEN_ROW_EDITOR_ACTION_ADJUST,
    /** One first-repeat fast adjustment was accepted. */
    SCREEN_ROW_EDITOR_ACTION_ADJUST_FAST_ONCE,
    /** One repeated fast adjustment was accepted. */
    SCREEN_ROW_EDITOR_ACTION_ADJUST_FAST_REPEAT,
};

/**
 * @brief Reset editor browse/edit state back to its default idle baseline.
 *
 * @param  state  Caller-owned editor state object.
 */
void                          screen_row_editor_state_reset(struct screen_row_editor_state *state);

/**
 * @brief Prepare one editor state for a page entry or reopen operation.
 *
 * @param  state              Caller-owned editor state object.
 * @param  layout             Borrowed page row layout contract.
 * @param  remembered_cursor  Preferred cursor row from the page owner.
 */
void                          screen_row_editor_prepare(struct screen_row_editor_state        *state,
                                                        const struct screen_row_editor_layout *layout,
                                                        uint8_t                                remembered_cursor);

/**
 * @brief Leave edit mode while preserving the current cursor position.
 *
 * @param  state  Caller-owned editor state object.
 */
void                          screen_row_editor_leave_edit(struct screen_row_editor_state *state);

/**
 * @brief Move the browse cursor by one signed step and refresh scroll state.
 *
 * @param  state   Caller-owned editor state object.
 * @param  layout  Borrowed page row layout contract.
 * @param  step    Signed cursor delta, typically `+1` or `-1`.
 */
void                          screen_row_editor_cursor_step(struct screen_row_editor_state        *state,
                                                            const struct screen_row_editor_layout *layout,
                                                            int8_t                                 step);

/**
 * @brief Consume one page-owned screen event as a row-step/editor action.
 *
 * @param  state      Caller-owned editor state object.
 * @param  layout     Borrowed page row layout contract.
 * @param  event      Borrowed screen event already mapped by the page owner.
 * @param  direction  Signed row direction associated with that event.
 *
 * @return One explicit editor action describing the accepted semantic change.
 */
enum screen_row_editor_action screen_row_editor_handle_step_key(
    struct screen_row_editor_state        *state,
    const struct screen_row_editor_layout *layout,
    const struct screen_event             *event,
    int8_t                                 direction);

/**
 * @brief Return the page-owned cursor marker string for one visible row.
 *
 * @param  state       Caller-owned editor state object.
 * @param  item_index  Logical row index being rendered.
 *
 * @return Stable short cursor marker text.
 */
const char *screen_row_editor_cursor_text(const struct screen_row_editor_state *state,
                                          uint32_t                              item_index);

/**
 * @brief Draw one caller-owned row into the supplied mono canvas.
 *
 * @param  canvas      Borrowed destination canvas.
 * @param  state       Caller-owned editor state object.
 * @param  layout      Borrowed page row layout contract.
 * @param  item_index  Logical row index being rendered.
 * @param  y           Top Y coordinate for this row.
 * @param  font        Borrowed mono font used for label/value text.
 * @param  label       Borrowed row label string.
 * @param  value       Borrowed row value string.
 *
 * @return `0` on success, or negative `-errno` on invalid draw inputs.
 */
int         screen_row_editor_draw_row(struct mono_canvas                    *canvas,
                                       const struct screen_row_editor_state  *state,
                                       const struct screen_row_editor_layout *layout,
                                       uint32_t                               item_index,
                                       uint16_t                               y,
                                       const struct mono_font                *font,
                                       const char                            *label,
                                       const char                            *value);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
