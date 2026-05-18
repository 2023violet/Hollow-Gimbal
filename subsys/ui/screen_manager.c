/**
 * @file    screen_manager.c
 * @brief   Shared Screen Stack navigation core
 * @version v1.0.0
 * @date    2026.04.13
 *
 * Implementation role
 * -------------------
 * This module intentionally stays small and deterministic. It owns shared
 * stack topology plus lifecycle callback sequencing, so it can be unit-tested
 * without RTOS, BSP, or display dependencies.
 *
 * Ownership / lifecycle / context
 * --------------------------------
 * - The stack stores borrowed static descriptors only; it never allocates.
 * - `push/pop/replace/reset` own lifecycle sequencing.
 * - Rendering and physical input dispatch stay on caller-owned service paths.
 *
 * Key collaborators
 * -----------------
 * - `screen.h`: shared page descriptor and callback contract
 * - caller-owned render/input services: invoke draw and key paths externally
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "screen_manager.h"
#include <errno.h>
#include <stddef.h>
#include <stdint.h>

/* Private State -------------------------------------------------------------------------------- */

static struct screen *_screen_manager_stack[SCREEN_STACK_DEPTH] = {0};
static uint32_t       _screen_manager_depth                     = 0U;
static uint32_t       _screen_manager_nav_generation            = 0U;

/* Private Helpers ------------------------------------------------------------------------------ */

static void _screen_manager_generation_bump(void)
{
    _screen_manager_nav_generation++;
}

static void _screen_manager_call_enter(struct screen *screen)
{
    if ((screen != NULL) && (screen->on_enter != NULL)) {
        screen->on_enter(screen);
    }
}

static void _screen_manager_call_exit(struct screen *screen)
{
    if ((screen != NULL) && (screen->on_exit != NULL)) {
        screen->on_exit(screen);
    }
}

static void _screen_manager_call_resume(struct screen *screen)
{
    if ((screen != NULL) && (screen->on_resume != NULL)) {
        screen->on_resume(screen);
    }
}

/* Public Functions ----------------------------------------------------------------------------- */

int screen_manager_init(void)
{
    for (uint32_t i = 0U; i < SCREEN_STACK_DEPTH; i++) {
        _screen_manager_stack[i] = NULL;
    }

    _screen_manager_depth          = 0U;
    _screen_manager_nav_generation = 0U;
    return 0;
}

struct screen *screen_manager_current(void)
{
    if (_screen_manager_depth == 0U) {
        return NULL;
    }

    return _screen_manager_stack[_screen_manager_depth - 1U];
}

uint32_t screen_manager_nav_generation_get(void)
{
    return _screen_manager_nav_generation;
}

int screen_manager_push(struct screen *screen)
{
    if (screen == NULL) {
        return -EINVAL;
    }

    if (_screen_manager_depth >= SCREEN_STACK_DEPTH) {
        return -ENOSPC;
    }

    _screen_manager_stack[_screen_manager_depth++] = screen;
    _screen_manager_call_enter(screen);
    _screen_manager_generation_bump();
    return 0;
}

int screen_manager_pop(void)
{
    struct screen *current;
    struct screen *resume;

    if (_screen_manager_depth <= 1U) {
        return 0;
    }

    current = _screen_manager_stack[_screen_manager_depth - 1U];
    _screen_manager_call_exit(current);

    _screen_manager_stack[_screen_manager_depth - 1U] = NULL;
    _screen_manager_depth--;

    resume = screen_manager_current();
    _screen_manager_call_resume(resume);
    _screen_manager_generation_bump();
    return 0;
}

int screen_manager_replace(struct screen *screen)
{
    struct screen *current;

    if (screen == NULL) {
        return -EINVAL;
    }

    current = screen_manager_current();
    if (current == NULL) {
        return screen_manager_push(screen);
    }

    if (current == screen) {
        return 0;
    }

    _screen_manager_call_exit(current);
    _screen_manager_stack[_screen_manager_depth - 1U] = screen;
    _screen_manager_call_enter(screen);
    _screen_manager_generation_bump();
    return 0;
}

int screen_manager_reset(struct screen *screen)
{
    struct screen *current;

    if (screen == NULL) {
        return -EINVAL;
    }

    if ((_screen_manager_depth == 1U) && (_screen_manager_stack[0] == screen)) {
        return 0;
    }

    while (_screen_manager_depth > 0U) {
        current = _screen_manager_stack[_screen_manager_depth - 1U];
        _screen_manager_call_exit(current);
        _screen_manager_stack[_screen_manager_depth - 1U] = NULL;
        _screen_manager_depth--;
    }

    _screen_manager_stack[0] = screen;
    _screen_manager_depth    = 1U;
    _screen_manager_call_enter(screen);
    _screen_manager_generation_bump();
    return 0;
}

int screen_manager_dispatch_key(const struct screen_event *event)
{
    struct screen     *current;
    enum screen_result result;

    if (event == NULL) {
        return -EINVAL;
    }

    current = screen_manager_current();
    if ((current == NULL) || (current->on_key == NULL)) {
        return -ENODEV;
    }

    result = current->on_key(current, event);
    if (result == SCREEN_POP) {
        if (_screen_manager_depth > 1U) {
            (void)screen_manager_pop();
            return SCREEN_POP;
        }

        return SCREEN_HANDLED;
    }

    return (int)result;
}

int screen_manager_draw(struct mono_canvas *canvas)
{
    struct screen *current;

    if (canvas == NULL) {
        return -EINVAL;
    }

    current = screen_manager_current();
    if ((current == NULL) || (current->on_draw == NULL)) {
        return -ENODEV;
    }

    current->on_draw(current, canvas);
    return 0;
}

uint32_t screen_manager_next_wakeup_ms(void)
{
    struct screen *current = screen_manager_current();

    if ((current == NULL) || (current->next_wakeup_ms == NULL)) {
        return UINT32_MAX;
    }

    return current->next_wakeup_ms(current);
}

int screen_manager_process_wakeup(void)
{
    struct screen *current = screen_manager_current();

    if ((current == NULL) || (current->on_wakeup == NULL)) {
        return 0;
    }

    return current->on_wakeup(current);
}

/* ---------------------------------------- end of file ----------------------------------------- */
