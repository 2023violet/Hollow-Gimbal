/**
 * @file    screen_row_editor.c
 * @brief   Shared row-based editor helper for Screen Stack pages
 * @version v1.0.0
 * @date    2026.04.14
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "screen_row_editor.h"
#include <errno.h>

/* Private Helpers ------------------------------------------------------------------------------ */

static uint8_t _screen_row_editor_cursor_clamp(uint8_t                                cursor,
                                               const struct screen_row_editor_layout *layout)
{
    if ((layout == NULL) || (layout->item_count == 0U)) {
        return 0U;
    }

    if (cursor >= layout->item_count) {
        return 0U;
    }

    return cursor;
}

static void _screen_row_editor_scroll_sync(struct screen_row_editor_state        *state,
                                           const struct screen_row_editor_layout *layout)
{
    if ((state == NULL) || (layout == NULL) || (layout->visible_rows == 0U)) {
        return;
    }

    if (state->cursor < layout->visible_rows) {
        state->scroll_offset = 0U;
        return;
    }

    if (state->cursor < state->scroll_offset) {
        state->scroll_offset = state->cursor;
        return;
    }

    if (state->cursor >= (uint8_t)(state->scroll_offset + layout->visible_rows)) {
        state->scroll_offset = (uint8_t)(state->cursor - layout->visible_rows + 1U);
    }
}

static uint16_t _screen_row_editor_align_right_x(uint16_t                right_edge,
                                                 const struct mono_font *font,
                                                 const char             *text)
{
    uint16_t text_width = 0U;

    if ((font == NULL) || (text == NULL)) {
        return 0U;
    }

    if (mono_canvas_measure_text(text, font, &text_width, NULL) != 0) {
        return 0U;
    }

    if (text_width >= right_edge) {
        return 0U;
    }

    return (uint16_t)(right_edge - text_width);
}

/* Public Functions ----------------------------------------------------------------------------- */

void screen_row_editor_state_reset(struct screen_row_editor_state *state)
{
    if (state == NULL) {
        return;
    }

    state->cursor         = 0U;
    state->scroll_offset  = 0U;
    state->adjust_mode    = false;
    state->repeat_enabled = false;
}

void screen_row_editor_prepare(struct screen_row_editor_state        *state,
                               const struct screen_row_editor_layout *layout,
                               uint8_t                                remembered_cursor)
{
    if ((state == NULL) || (layout == NULL)) {
        return;
    }

    state->cursor         = _screen_row_editor_cursor_clamp(remembered_cursor, layout);
    state->adjust_mode    = false;
    state->repeat_enabled = false;
    _screen_row_editor_scroll_sync(state, layout);
}

void screen_row_editor_leave_edit(struct screen_row_editor_state *state)
{
    if (state == NULL) {
        return;
    }

    state->adjust_mode    = false;
    state->repeat_enabled = false;
}

void screen_row_editor_cursor_step(struct screen_row_editor_state        *state,
                                   const struct screen_row_editor_layout *layout,
                                   int8_t                                 step)
{
    int32_t next_cursor;
    int32_t item_count;

    if ((state == NULL) || (layout == NULL) || (layout->item_count == 0U)) {
        return;
    }

    item_count  = (int32_t)layout->item_count;
    next_cursor = (int32_t)state->cursor + (int32_t)step;
    while (next_cursor < 0) {
        next_cursor += item_count;
    }
    while (next_cursor >= item_count) {
        next_cursor -= item_count;
    }

    state->cursor = (uint8_t)next_cursor;
    _screen_row_editor_scroll_sync(state, layout);
}

enum screen_row_editor_action screen_row_editor_handle_step_key(
    struct screen_row_editor_state        *state,
    const struct screen_row_editor_layout *layout,
    const struct screen_event             *event,
    int8_t                                 direction)
{
    if ((state == NULL) || (layout == NULL) || (event == NULL) ||
        ((direction != 1) && (direction != -1))) {
        return SCREEN_ROW_EDITOR_ACTION_NONE;
    }

    if (event->gesture == SCREEN_EVENT_GESTURE_HOLD) {
        if (!state->adjust_mode) {
            state->adjust_mode    = true;
            state->repeat_enabled = false;
            return SCREEN_ROW_EDITOR_ACTION_ENTER_EDIT;
        }

        state->repeat_enabled = true;
        return SCREEN_ROW_EDITOR_ACTION_ADJUST_FAST_ONCE;
    }

    if (event->gesture == SCREEN_EVENT_GESTURE_REPEAT) {
        if (!state->adjust_mode || !state->repeat_enabled) {
            return SCREEN_ROW_EDITOR_ACTION_NONE;
        }

        return SCREEN_ROW_EDITOR_ACTION_ADJUST_FAST_REPEAT;
    }

    if (event->gesture == SCREEN_EVENT_GESTURE_SELECT) {
        if (state->adjust_mode) {
            state->repeat_enabled = false;
            return SCREEN_ROW_EDITOR_ACTION_ADJUST;
        }

        screen_row_editor_cursor_step(state, layout, direction);
        return SCREEN_ROW_EDITOR_ACTION_CURSOR_MOVED;
    }

    return SCREEN_ROW_EDITOR_ACTION_NONE;
}

const char *screen_row_editor_cursor_text(const struct screen_row_editor_state *state,
                                          uint32_t                              item_index)
{
    if ((state == NULL) || (item_index != (uint32_t)state->cursor)) {
        return " ";
    }

    return state->adjust_mode ? "*" : ">";
}

int screen_row_editor_draw_row(struct mono_canvas                    *canvas,
                               const struct screen_row_editor_state  *state,
                               const struct screen_row_editor_layout *layout,
                               uint32_t                               item_index,
                               uint16_t                               y,
                               const struct mono_font                *font,
                               const char                            *label,
                               const char                            *value)
{
    uint16_t value_x;
    int      result;

    if ((canvas == NULL) || (state == NULL) || (layout == NULL) || (font == NULL) ||
        (label == NULL) || (value == NULL)) {
        return -EINVAL;
    }

    value_x = _screen_row_editor_align_right_x(layout->value_right_x, font, value);

    result = mono_canvas_draw_text(canvas,
                                   layout->cursor_x,
                                   y,
                                   screen_row_editor_cursor_text(state, item_index),
                                   font);
    if (result != 0) {
        return result;
    }

    result = mono_canvas_draw_text(canvas, layout->label_x, y, label, font);
    if (result != 0) {
        return result;
    }

    return mono_canvas_draw_text(canvas, value_x, y, value, font);
}

/* ---------------------------------------- end of file ----------------------------------------- */
