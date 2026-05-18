/**
 * @file    tm_led.c
 * @brief   Shared LED timing subsystem
 * @version v3.0.0
 * @date    2026.04.23
 */

/* Includes --------------------------------------------------------------------- */
#include "tm_led.h"
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include "FreeRTOS.h"
#include "cmsis_os2.h"
#include "tm_runtime.h"

/* Private Macros --------------------------------------------------------------- */

#define TM_LED_STATUS_INIT (-EAGAIN)

/* Private Types ---------------------------------------------------------------- */

struct tm_led_channel_state {
    uint32_t pulse_count;
    uint32_t remaining_ms;
    uint32_t pulse_duration_ms;
    uint32_t blink_on_ms;
    uint32_t blink_off_ms;
    bool     pulse_pending;
    bool     blink_active;
    bool     hold_active;
    bool     sync_pending;
    bool     logical_on;
};

/* Private Helpers -------------------------------------------------------------- */

static void _tm_led_signal_owner(void);
static bool _tm_led_work_pending_unlocked(void);
static bool _tm_led_state_matches_hold_unlocked(const struct tm_led_channel_state *state, bool on);
static void _tm_led_snapshot_fill_unlocked(const struct tm_led_channel_state *state,
                                           struct tm_led_channel_snapshot    *snapshot);
static bool _tm_led_state_is_active_unlocked(const struct tm_led_channel_state *state);
static void _tm_led_state_clear_modes_unlocked(struct tm_led_channel_state *state);
static void _tm_led_state_set_hold_unlocked(struct tm_led_channel_state *state, bool on);
static int  _tm_led_lock(void);
static void _tm_led_unlock(void);
static uint32_t _tm_led_delay_min(uint32_t current_ms, uint32_t candidate_ms);
static void     _tm_led_pulse_start(enum bsp_led_id led);
static void     _tm_led_pulse_advance(struct tm_led_channel_state *state, uint32_t elapsed_ms);
static void     _tm_led_blink_restart(struct tm_led_channel_state *state);
static void     _tm_led_blink_advance(struct tm_led_channel_state *state, uint32_t elapsed_ms);
static int      _tm_led_apply_outputs(const bool *desired);
static void     _tm_led_reset_runtime(void);

/* Private State ---------------------------------------------------------------- */

static osSemaphoreId_t         _tm_led_wakeup_sem = NULL;
static osMutexId_t             _tm_led_mutex      = NULL;
static StaticSemaphore_t       _tm_led_wakeup_sem_storage;
static StaticSemaphore_t       _tm_led_mutex_storage;
static const osSemaphoreAttr_t _tm_led_wakeup_sem_attr = {
    .name    = "tm_led_wakeup",
    .cb_mem  = &_tm_led_wakeup_sem_storage,
    .cb_size = sizeof(_tm_led_wakeup_sem_storage),
};
static const osMutexAttr_t _tm_led_mutex_attr = {
    .name    = "tm_led_mutex",
    .cb_mem  = &_tm_led_mutex_storage,
    .cb_size = sizeof(_tm_led_mutex_storage),
};
static struct tm_led_channel_state _tm_led_channels[BSP_LED_ID_COUNT] = {0};
static int                         _tm_led_status                     = TM_LED_STATUS_INIT;

/* Public Functions ------------------------------------------------------------- */

int tm_led_init(void)
{
    int result;

    if (!bsp_led_is_ready()) {
        _tm_led_status = bsp_led_status();
        return _tm_led_status;
    }

    if (_tm_led_mutex == NULL) {
        _tm_led_mutex = osMutexNew(&_tm_led_mutex_attr);
        if (_tm_led_mutex == NULL) {
            _tm_led_status = -ENOMEM;
            return _tm_led_status;
        }
    }

    if (_tm_led_wakeup_sem == NULL) {
        _tm_led_wakeup_sem = osSemaphoreNew(1U, 0U, &_tm_led_wakeup_sem_attr);
        if (_tm_led_wakeup_sem == NULL) {
            _tm_led_status = -ENOMEM;
            return _tm_led_status;
        }
    }

    _tm_led_reset_runtime();

    result = bsp_led_all_off();
    if (result != 0) {
        _tm_led_status = result;
        return result;
    }

    _tm_led_status = 0;
    return 0;
}

int tm_led_status(void)
{
    int status = _tm_led_status;

    if (_tm_led_lock() == 0) {
        status = _tm_led_status;
        _tm_led_unlock();
    }

    return status;
}

uint32_t tm_led_next_wakeup_ms(void)
{
    uint32_t delay_ms = TM_LED_WAIT_FOREVER_MS;

    if (tm_led_status() != 0) {
        return 0U;
    }

    if (_tm_led_lock() != 0) {
        return 0U;
    }
    if (_tm_led_work_pending_unlocked()) {
        _tm_led_unlock();
        return 0U;
    }

    for (uint32_t i = 0U; i < (uint32_t)BSP_LED_ID_COUNT; i++) {
        const struct tm_led_channel_state *state = &_tm_led_channels[i];

        if (state->blink_active || (state->logical_on && !state->hold_active)) {
            delay_ms = _tm_led_delay_min(delay_ms, state->remaining_ms);
        }
    }
    _tm_led_unlock();

    return delay_ms;
}

int tm_led_wait(uint32_t timeout_ms)
{
    osStatus_t status;

    if (_tm_led_status != 0) {
        return _tm_led_status;
    }

    if (_tm_led_wakeup_sem == NULL) {
        return 0;
    }

    if (_tm_led_lock() != 0) {
        return -EIO;
    }
    if (_tm_led_work_pending_unlocked() || (timeout_ms == 0U)) {
        _tm_led_unlock();
        (void)osSemaphoreAcquire(_tm_led_wakeup_sem, 0U);
        return 0;
    }
    _tm_led_unlock();

    status =
        osSemaphoreAcquire(_tm_led_wakeup_sem,
                           (timeout_ms == TM_LED_WAIT_FOREVER_MS) ? osWaitForever : timeout_ms);
    if ((status == osOK) || (status == osErrorTimeout)) {
        return 0;
    }

    return -EIO;
}

int tm_led_process(uint32_t elapsed_ms)
{
    bool desired[BSP_LED_ID_COUNT];
    int  result;

    if (tm_led_status() != 0) {
        return tm_led_status();
    }

    if (_tm_led_lock() != 0) {
        return -EIO;
    }
    for (uint32_t i = 0U; i < (uint32_t)BSP_LED_ID_COUNT; i++) {
        struct tm_led_channel_state *state = &_tm_led_channels[i];

        if (state->pulse_pending) {
            state->pulse_pending = false;
            _tm_led_pulse_start((enum bsp_led_id)i);
        }

        if (state->blink_active) {
            if (state->sync_pending && !state->logical_on && (state->remaining_ms == 0U)) {
                _tm_led_blink_restart(state);
            }
            state->sync_pending = false;
            _tm_led_blink_advance(state, elapsed_ms);
        } else if (state->hold_active) {
            state->sync_pending = false;
        } else {
            state->sync_pending = false;
            _tm_led_pulse_advance(state, elapsed_ms);
        }

        desired[i] = state->logical_on;
    }
    _tm_led_unlock();

    result = _tm_led_apply_outputs(desired);
    if (result != 0) {
        _tm_led_status = result;
        return result;
    }

    return 0;
}

int tm_led_submit_pulse(enum bsp_led_id led, uint32_t duration_ms)
{
    struct tm_led_channel_state *state;

    if (_tm_led_status != 0) {
        return _tm_led_status;
    }

    if (!bsp_led_id_valid(led) || (duration_ms == 0U)) {
        return -EINVAL;
    }

    if (_tm_led_lock() != 0) {
        return -EIO;
    }
    state                    = &_tm_led_channels[(uint32_t)led];
    state->hold_active       = false;
    state->pulse_pending     = true;
    state->pulse_duration_ms = duration_ms;
    _tm_led_unlock();

    _tm_led_signal_owner();
    return 0;
}

int tm_led_set_blink(enum bsp_led_id led, bool active, uint32_t on_ms, uint32_t off_ms)
{
    struct tm_led_channel_state *state;
    bool                         changed;

    if (_tm_led_status != 0) {
        return _tm_led_status;
    }

    if (!bsp_led_id_valid(led)) {
        return -EINVAL;
    }

    if (active && ((on_ms == 0U) || (off_ms == 0U))) {
        return -EINVAL;
    }

    if (_tm_led_lock() != 0) {
        return -EIO;
    }
    state   = &_tm_led_channels[(uint32_t)led];
    changed = (state->blink_active != active) || (state->blink_on_ms != on_ms) ||
              (state->blink_off_ms != off_ms);
    state->blink_active = active;
    state->blink_on_ms  = on_ms;
    state->blink_off_ms = off_ms;
    state->hold_active  = false;
    state->sync_pending = changed;
    if (!active) {
        state->remaining_ms = 0U;
        state->logical_on   = false;
    }
    _tm_led_unlock();

    if (changed) {
        _tm_led_signal_owner();
    }

    return 0;
}

int tm_led_set_logical(enum bsp_led_id led, bool on)
{
    struct tm_led_channel_state *state;
    bool                         changed;

    if (_tm_led_status != 0) {
        return _tm_led_status;
    }

    if (!bsp_led_id_valid(led)) {
        return -EINVAL;
    }

    if (_tm_led_lock() != 0) {
        return -EIO;
    }
    state   = &_tm_led_channels[(uint32_t)led];
    changed = !_tm_led_state_matches_hold_unlocked(state, on);
    _tm_led_state_set_hold_unlocked(state, on);
    _tm_led_unlock();

    if (changed) {
        _tm_led_signal_owner();
    }

    return 0;
}

int tm_led_all_off(void)
{
    bool changed = false;

    if (_tm_led_status != 0) {
        return _tm_led_status;
    }

    if (_tm_led_lock() != 0) {
        return -EIO;
    }
    for (uint32_t i = 0U; i < (uint32_t)BSP_LED_ID_COUNT; i++) {
        struct tm_led_channel_state *state = &_tm_led_channels[i];

        changed = changed || _tm_led_state_is_active_unlocked(state);
        _tm_led_state_clear_modes_unlocked(state);
    }
    _tm_led_unlock();

    if (changed) {
        _tm_led_signal_owner();
    }

    return 0;
}

int tm_led_toggle_logical(enum bsp_led_id led)
{
    struct tm_led_channel_state *state;
    bool                         next_on;
    bool                         changed;

    if (_tm_led_status != 0) {
        return _tm_led_status;
    }

    if (!bsp_led_id_valid(led)) {
        return -EINVAL;
    }

    if (_tm_led_lock() != 0) {
        return -EIO;
    }
    state   = &_tm_led_channels[(uint32_t)led];
    next_on = !state->logical_on;
    changed = !_tm_led_state_matches_hold_unlocked(state, next_on);
    _tm_led_state_set_hold_unlocked(state, next_on);
    _tm_led_unlock();

    if (changed) {
        _tm_led_signal_owner();
    }

    return 0;
}

bool tm_led_snapshot_get(enum bsp_led_id led, struct tm_led_channel_snapshot *snapshot)
{
    if ((snapshot == NULL) || !bsp_led_id_valid(led)) {
        return false;
    }

    if (_tm_led_lock() != 0) {
        return false;
    }
    _tm_led_snapshot_fill_unlocked(&_tm_led_channels[(uint32_t)led], snapshot);
    _tm_led_unlock();
    return true;
}

bool tm_led_diag_snapshot(struct tm_led_diag_snapshot *snapshot)
{
    if (snapshot == NULL) {
        return false;
    }

    if (_tm_led_lock() != 0) {
        return false;
    }
    snapshot->status      = _tm_led_status;
    snapshot->initialized = (_tm_led_status == 0);
    for (uint32_t i = 0U; i < (uint32_t)BSP_LED_ID_COUNT; i++) {
        snapshot->leds[i] = (const struct tm_led_channel_diag){
            .pulse_count   = _tm_led_channels[i].pulse_count,
            .remaining_ms  = _tm_led_channels[i].remaining_ms,
            .blink_on_ms   = _tm_led_channels[i].blink_on_ms,
            .blink_off_ms  = _tm_led_channels[i].blink_off_ms,
            .pulse_pending = _tm_led_channels[i].pulse_pending,
            .blink_active  = _tm_led_channels[i].blink_active,
            .logical_on    = _tm_led_channels[i].logical_on,
        };
    }
    _tm_led_unlock();
    return true;
}

/* Private Helpers -------------------------------------------------------------- */

static void _tm_led_signal_owner(void)
{
    osStatus_t status;

    if (_tm_led_wakeup_sem == NULL) {
        return;
    }

    status = osSemaphoreRelease(_tm_led_wakeup_sem);
    if ((status == osOK) || (status == osErrorResource)) {
        return;
    }
}

static bool _tm_led_work_pending_unlocked(void)
{
    for (uint32_t i = 0U; i < (uint32_t)BSP_LED_ID_COUNT; i++) {
        if (_tm_led_channels[i].pulse_pending || _tm_led_channels[i].sync_pending) {
            return true;
        }
    }

    return false;
}

static bool _tm_led_state_matches_hold_unlocked(const struct tm_led_channel_state *state, bool on)
{
    if (state == NULL) {
        return false;
    }

    return !state->blink_active && !state->pulse_pending && !state->sync_pending &&
           (state->remaining_ms == 0U) && (state->hold_active == on) && (state->logical_on == on);
}

static void _tm_led_snapshot_fill_unlocked(const struct tm_led_channel_state *state,
                                           struct tm_led_channel_snapshot    *snapshot)
{
    if ((state == NULL) || (snapshot == NULL)) {
        return;
    }

    *snapshot = (const struct tm_led_channel_snapshot){
        .remaining_ms = state->remaining_ms,
        .blink_on_ms  = state->blink_on_ms,
        .blink_off_ms = state->blink_off_ms,
        .blink_active = state->blink_active,
        .logical_on   = state->logical_on,
    };
}

static bool _tm_led_state_is_active_unlocked(const struct tm_led_channel_state *state)
{
    if (state == NULL) {
        return false;
    }

    return state->blink_active || state->hold_active || state->pulse_pending ||
           (state->remaining_ms != 0U) || state->logical_on;
}

static void _tm_led_state_clear_modes_unlocked(struct tm_led_channel_state *state)
{
    if (state == NULL) {
        return;
    }

    state->pulse_pending     = false;
    state->pulse_duration_ms = 0U;
    state->blink_active      = false;
    state->blink_on_ms       = 0U;
    state->blink_off_ms      = 0U;
    state->hold_active       = false;
    state->sync_pending      = false;
    state->remaining_ms      = 0U;
    state->logical_on        = false;
}

static void _tm_led_state_set_hold_unlocked(struct tm_led_channel_state *state, bool on)
{
    _tm_led_state_clear_modes_unlocked(state);

    if (state == NULL) {
        return;
    }

    state->hold_active = on;
    state->logical_on  = on;
}

static int _tm_led_lock(void)
{
    if (_tm_led_mutex == NULL) {
        return -EIO;
    }

    return (osMutexAcquire(_tm_led_mutex, osWaitForever) == osOK) ? 0 : -EIO;
}

static void _tm_led_unlock(void)
{
    if (_tm_led_mutex == NULL) {
        return;
    }

    (void)osMutexRelease(_tm_led_mutex);
}

static uint32_t _tm_led_delay_min(uint32_t current_ms, uint32_t candidate_ms)
{
    if (current_ms == TM_LED_WAIT_FOREVER_MS) {
        return candidate_ms;
    }

    if (candidate_ms == TM_LED_WAIT_FOREVER_MS) {
        return current_ms;
    }

    return (candidate_ms < current_ms) ? candidate_ms : current_ms;
}

static void _tm_led_pulse_start(enum bsp_led_id led)
{
    struct tm_led_channel_state *state;

    if (!bsp_led_id_valid(led)) {
        return;
    }

    state = &_tm_led_channels[(uint32_t)led];
    state->pulse_count++;
    state->hold_active  = false;
    state->logical_on   = true;
    state->remaining_ms = state->pulse_duration_ms;
}

static void _tm_led_pulse_advance(struct tm_led_channel_state *state, uint32_t elapsed_ms)
{
    if ((state == NULL) || !state->logical_on || (elapsed_ms == 0U)) {
        return;
    }

    if (elapsed_ms >= state->remaining_ms) {
        state->remaining_ms = 0U;
        state->logical_on   = false;
        return;
    }

    state->remaining_ms -= elapsed_ms;
}

static void _tm_led_blink_restart(struct tm_led_channel_state *state)
{
    if (state == NULL) {
        return;
    }

    state->logical_on   = true;
    state->remaining_ms = state->blink_on_ms;
}

static void _tm_led_blink_advance(struct tm_led_channel_state *state, uint32_t elapsed_ms)
{
    uint32_t remaining_ms = elapsed_ms;

    if ((state == NULL) || !state->blink_active) {
        return;
    }

    if (state->remaining_ms == 0U) {
        _tm_led_blink_restart(state);
    }

    while (remaining_ms > 0U) {
        if (remaining_ms < state->remaining_ms) {
            state->remaining_ms -= remaining_ms;
            break;
        }

        remaining_ms -= state->remaining_ms;
        state->logical_on   = !state->logical_on;
        state->remaining_ms = state->logical_on ? state->blink_on_ms : state->blink_off_ms;
    }
}

static int _tm_led_apply_outputs(const bool *desired)
{
    bool current = false;
    int  result;

    if (desired == NULL) {
        return -EINVAL;
    }

    for (uint32_t i = 0U; i < (uint32_t)BSP_LED_ID_COUNT; i++) {
        if (!bsp_led_state_get((enum bsp_led_id)i, &current) || (current != desired[i])) {
            result = bsp_led_set((enum bsp_led_id)i, desired[i]);
            if (result != 0) {
                return result;
            }
        }
    }

    return 0;
}

static void _tm_led_reset_runtime(void)
{
    for (uint32_t i = 0U; i < (uint32_t)BSP_LED_ID_COUNT; i++) {
        _tm_led_channels[i] = (const struct tm_led_channel_state){0};
    }
}

/* ---------------------------------------- end of file ----------------------------------------- */
