/**
 * @file    tm_input.c
 * @brief   Shared physical input gesture subsystem
 * @version v3.0.0
 * @date    2026.04.23
 */

/* Includes --------------------------------------------------------------------- */
#include "tm_input.h"
#include <errno.h>
#include <stdint.h>
#include "FreeRTOS.h"
#include "button.h"
#include "cmsis_os2.h"
#include "task.h"

/* Private Macros --------------------------------------------------------------- */

#define TM_INPUT_STATUS_INIT (-EAGAIN)

/* Private Types ---------------------------------------------------------------- */

struct tm_input_channel_ctx {
    struct button                button;
    struct tm_input_channel_diag diag;
};

/* Private Helpers -------------------------------------------------------------- */

static int  _tm_input_dispatch_event(enum bsp_input_id input_id, int event, uint8_t click_count);
static void _tm_input_runtime_reset(void);
static int  _tm_input_buttons_init(const struct button_config *cfg);
static void _tm_input_dispatch_record(enum bsp_input_id input_id, int event, uint8_t click_count);
static int  _tm_input_dispatch_result_normalize(int result);
static void _tm_input_dispatch_result_note(int result);
static uint32_t                      _tm_input_lock_timeout_get(void);
static int                           _tm_input_lock(void);
static void                          _tm_input_unlock(void);
static struct tm_input_channel_diag *_tm_input_channel_diag_get(enum bsp_input_id input_id);
static int                           _tm_input_handle_event(enum bsp_input_id input_id, int event);

/* Private State ---------------------------------------------------------------- */

static struct tm_input_channel_ctx _tm_input_channels[BSP_INPUT_ID_COUNT] = {0};
static osMutexId_t                 _tm_input_mutex                        = NULL;
static StaticSemaphore_t           _tm_input_mutex_storage;
static const osMutexAttr_t         _tm_input_mutex_attr = {
            .name    = "tm_input",
            .cb_mem  = &_tm_input_mutex_storage,
            .cb_size = sizeof(_tm_input_mutex_storage),
};
static tm_input_event_handler_t _tm_input_handler                   = NULL;
static void                    *_tm_input_handler_context           = NULL;
static int                      _tm_input_status                    = TM_INPUT_STATUS_INIT;
static uint32_t                 _tm_input_sample_count              = 0U;
static uint32_t                 _tm_input_dispatch_count            = 0U;
static uint32_t                 _tm_input_dispatch_error_count      = 0U;
static uint32_t                 _tm_input_dispatch_handled_count    = 0U;
static uint32_t                 _tm_input_dispatch_ignored_count    = 0U;
static int                      _tm_input_last_dispatch_result      = 0;
static uint16_t                 _tm_input_last_dispatch_event_mask  = 0U;
static enum bsp_input_id        _tm_input_last_dispatch_input_id    = BSP_INPUT_ID_INVALID;
static uint8_t                  _tm_input_last_dispatch_click_count = 0U;

/* Public Functions ------------------------------------------------------------- */

int tm_input_init(const struct button_config *config)
{
    int result;

    if (config == NULL) {
        return -EINVAL;
    }

    if (_tm_input_status == 0) {
        return 0;
    }

    if (_tm_input_mutex == NULL) {
        _tm_input_mutex = osMutexNew(&_tm_input_mutex_attr);
        if (_tm_input_mutex == NULL) {
            _tm_input_status = -ENOMEM;
            return _tm_input_status;
        }
    }

    if (!bsp_input_is_ready()) {
        _tm_input_status = bsp_input_status();
        return _tm_input_status;
    }

    result = _tm_input_buttons_init(config);
    if (result != 0) {
        _tm_input_status = result;
        return result;
    }

    if (_tm_input_lock() != 0) {
        _tm_input_status = -EIO;
        return _tm_input_status;
    }
    _tm_input_runtime_reset();
    _tm_input_status = 0;
    _tm_input_unlock();
    return 0;
}

int tm_input_set_handler(tm_input_event_handler_t handler, void *context)
{
    if (_tm_input_lock() != 0) {
        return -EIO;
    }
    _tm_input_handler         = handler;
    _tm_input_handler_context = context;
    _tm_input_unlock();
    return 0;
}

int tm_input_status(void)
{
    int status = _tm_input_status;

    if (_tm_input_lock() == 0) {
        status = _tm_input_status;
        _tm_input_unlock();
    }

    return status;
}

int tm_input_process_sample(const struct bsp_input_sample *sample)
{
    int result;

    if (sample == NULL) {
        return -EINVAL;
    }

    result = tm_input_status();
    if (result != 0) {
        return result;
    }

    if (_tm_input_lock() != 0) {
        return -EIO;
    }
    _tm_input_sample_count++;
    _tm_input_unlock();

    for (uint32_t i = 0U; i < (uint32_t)BSP_INPUT_ID_COUNT; i++) {
        const enum bsp_input_id input_id = (enum bsp_input_id)i;
        const int event = button_update_pressed(&_tm_input_channels[i].button, sample->pressed[i]);

        if (event < 0) {
            if (_tm_input_lock() == 0) {
                _tm_input_status = event;
                _tm_input_unlock();
            } else {
                _tm_input_status = event;
            }
            return event;
        }

        if (event > 0) {
            if (_tm_input_lock() != 0) {
                return -EIO;
            }
            _tm_input_channels[i].diag.last_event_mask = (uint16_t)event;
            _tm_input_unlock();
        }

        result = _tm_input_handle_event(input_id, event);
        if (result != 0) {
            if (_tm_input_lock() == 0) {
                _tm_input_status = result;
                _tm_input_unlock();
            } else {
                _tm_input_status = result;
            }
            return result;
        }
    }

    return 0;
}

int tm_input_debug_inject_event(enum bsp_input_id input_id,
                                uint16_t          event_mask,
                                uint8_t           click_count)
{
    struct tm_input_channel_diag *diag;
    uint8_t                       effective_click_count;
    int                           result;

    if ((event_mask == 0U) || !bsp_input_id_valid(input_id)) {
        return -EINVAL;
    }

    result = tm_input_status();
    if (result != 0) {
        return result;
    }

    diag = _tm_input_channel_diag_get(input_id);
    if (diag == NULL) {
        return -EINVAL;
    }

    effective_click_count =
        ((event_mask & BUTTON_EVENT_CLICKED) != 0U) ? ((click_count == 0U) ? 1U : click_count) : 0U;

    if (_tm_input_lock() != 0) {
        return -EIO;
    }
    diag->last_event_mask = event_mask;
    if ((event_mask & BUTTON_EVENT_CLICKED) != 0U) {
        diag->click_count++;
        diag->last_click_count = effective_click_count;
    } else {
        diag->last_click_count = 0U;
    }
    if ((event_mask & BUTTON_EVENT_LONG_PRESSED) != 0U) {
        diag->long_press_count++;
    }
    if ((event_mask & BUTTON_EVENT_REPEATED) != 0U) {
        diag->repeat_count++;
    }
    _tm_input_unlock();

    result = _tm_input_dispatch_event(input_id, (int)event_mask, effective_click_count);
    if (result != 0) {
        return result;
    }

    if (_tm_input_lock() != 0) {
        return -EIO;
    }
    result = _tm_input_last_dispatch_result;
    _tm_input_unlock();
    return result;
}

bool tm_input_state_snapshot_get(struct tm_input_state_snapshot *snapshot)
{
    if (snapshot == NULL) {
        return false;
    }

    if (_tm_input_lock() != 0) {
        return false;
    }
    *snapshot = (const struct tm_input_state_snapshot){
        .last_dispatch_result = _tm_input_last_dispatch_result,
        .last_event_mask      = _tm_input_last_dispatch_event_mask,
        .last_input_id        = _tm_input_last_dispatch_input_id,
        .last_click_count     = _tm_input_last_dispatch_click_count,
        .initialized          = (_tm_input_status == 0),
        .handler_installed    = (_tm_input_handler != NULL),
    };
    _tm_input_unlock();
    return true;
}

bool tm_input_diag_snapshot(struct tm_input_diag_snapshot *snapshot)
{
    if (snapshot == NULL) {
        return false;
    }

    if (_tm_input_lock() != 0) {
        return false;
    }
    snapshot->status                    = _tm_input_status;
    snapshot->initialized               = (_tm_input_status == 0);
    snapshot->sample_count              = _tm_input_sample_count;
    snapshot->dispatch_count            = _tm_input_dispatch_count;
    snapshot->dispatch_error_count      = _tm_input_dispatch_error_count;
    snapshot->dispatch_handled_count    = _tm_input_dispatch_handled_count;
    snapshot->dispatch_ignored_count    = _tm_input_dispatch_ignored_count;
    snapshot->last_dispatch_result      = _tm_input_last_dispatch_result;
    snapshot->last_dispatch_event_mask  = _tm_input_last_dispatch_event_mask;
    snapshot->last_dispatch_input_id    = _tm_input_last_dispatch_input_id;
    snapshot->last_dispatch_click_count = _tm_input_last_dispatch_click_count;
    for (uint32_t i = 0U; i < (uint32_t)BSP_INPUT_ID_COUNT; i++) {
        snapshot->channels[i] = _tm_input_channels[i].diag;
    }
    _tm_input_unlock();
    return true;
}

const char *tm_input_name_get(enum bsp_input_id input_id)
{
    if (input_id == BSP_INPUT_ID_INVALID) {
        return "none";
    }

    return bsp_input_name_get(input_id);
}

const char *tm_input_event_name_get(uint16_t event_mask)
{
    if ((event_mask & BUTTON_EVENT_LONG_PRESSED) != 0U) {
        return "long";
    }
    if ((event_mask & BUTTON_EVENT_CLICKED) != 0U) {
        return "click";
    }
    if ((event_mask & BUTTON_EVENT_REPEATED) != 0U) {
        return "repeat";
    }
    if ((event_mask & BUTTON_EVENT_PRESSED) != 0U) {
        return "press";
    }
    if ((event_mask & BUTTON_EVENT_RELEASED) != 0U) {
        return "release";
    }

    return "none";
}

const char *tm_input_dispatch_result_name_get(int result)
{
    if (result < 0) {
        return "error";
    }

    switch (_tm_input_dispatch_result_normalize(result)) {
    case TM_INPUT_DISPATCH_IGNORED:
        return "ignored";
    default:
        return "handled";
    }
}

/* Private Helpers -------------------------------------------------------------- */

static int _tm_input_dispatch_event(enum bsp_input_id input_id, int event, uint8_t click_count)
{
    const struct tm_input_event input_event = {
        .event_mask  = (uint16_t)event,
        .input_id    = input_id,
        .click_count = click_count,
    };
    tm_input_event_handler_t handler;
    void                    *handler_context;
    int                      raw_result;
    int                      result;

    _tm_input_dispatch_record(input_id, event, click_count);

    if (_tm_input_lock() != 0) {
        return -EIO;
    }
    handler         = _tm_input_handler;
    handler_context = _tm_input_handler_context;
    _tm_input_unlock();

    if (handler == NULL) {
        raw_result = TM_INPUT_DISPATCH_IGNORED;
    } else {
        raw_result = handler(&input_event, handler_context);
    }
    result = _tm_input_dispatch_result_normalize(raw_result);

    if (_tm_input_lock() != 0) {
        return -EIO;
    }
    _tm_input_last_dispatch_result = result;
    if (raw_result < 0) {
        _tm_input_dispatch_error_count++;
        _tm_input_unlock();
        return raw_result;
    }
    _tm_input_unlock();

    _tm_input_dispatch_result_note(result);
    return 0;
}

static void _tm_input_runtime_reset(void)
{
    for (uint32_t i = 0U; i < (uint32_t)BSP_INPUT_ID_COUNT; i++) {
        _tm_input_channels[i].diag = (const struct tm_input_channel_diag){0};
    }

    _tm_input_sample_count              = 0U;
    _tm_input_dispatch_count            = 0U;
    _tm_input_dispatch_error_count      = 0U;
    _tm_input_dispatch_handled_count    = 0U;
    _tm_input_dispatch_ignored_count    = 0U;
    _tm_input_last_dispatch_result      = 0;
    _tm_input_last_dispatch_event_mask  = 0U;
    _tm_input_last_dispatch_input_id    = BSP_INPUT_ID_INVALID;
    _tm_input_last_dispatch_click_count = 0U;
}

static int _tm_input_buttons_init(const struct button_config *cfg)
{
    int result;

    if (cfg == NULL) {
        return -EINVAL;
    }

    for (uint32_t i = 0U; i < (uint32_t)BSP_INPUT_ID_COUNT; i++) {
        result = button_init(&_tm_input_channels[i].button, cfg);
        if (result != 0) {
            return result;
        }
    }

    return 0;
}

static void _tm_input_dispatch_record(enum bsp_input_id input_id, int event, uint8_t click_count)
{
    if (_tm_input_lock() != 0) {
        return;
    }
    _tm_input_dispatch_count++;
    _tm_input_last_dispatch_input_id    = input_id;
    _tm_input_last_dispatch_event_mask  = (uint16_t)event;
    _tm_input_last_dispatch_click_count = click_count;
    _tm_input_unlock();
}

static int _tm_input_dispatch_result_normalize(int result)
{
    if (result < 0) {
        return result;
    }

    return (result == TM_INPUT_DISPATCH_IGNORED) ? TM_INPUT_DISPATCH_IGNORED
                                                 : TM_INPUT_DISPATCH_HANDLED;
}

static void _tm_input_dispatch_result_note(int result)
{
    if (_tm_input_lock() != 0) {
        return;
    }
    if (result == TM_INPUT_DISPATCH_IGNORED) {
        _tm_input_dispatch_ignored_count++;
        _tm_input_unlock();
        return;
    }

    _tm_input_dispatch_handled_count++;
    _tm_input_unlock();
}

static uint32_t _tm_input_lock_timeout_get(void)
{
    /* GDB function calls run with the scheduler suspended and must not block. */
    if (xTaskGetSchedulerState() == taskSCHEDULER_SUSPENDED) {
        return 0U;
    }

    return osWaitForever;
}

static int _tm_input_lock(void)
{
    if (_tm_input_mutex == NULL) {
        return -EIO;
    }

    return (osMutexAcquire(_tm_input_mutex, _tm_input_lock_timeout_get()) == osOK) ? 0 : -EIO;
}

static void _tm_input_unlock(void)
{
    if (_tm_input_mutex == NULL) {
        return;
    }

    (void)osMutexRelease(_tm_input_mutex);
}

static struct tm_input_channel_diag *_tm_input_channel_diag_get(enum bsp_input_id input_id)
{
    if (!bsp_input_id_valid(input_id)) {
        return NULL;
    }

    return &_tm_input_channels[(uint32_t)input_id].diag;
}

static int _tm_input_handle_event(enum bsp_input_id input_id, int event)
{
    struct tm_input_channel_diag *diag;

    if ((event == 0) || !bsp_input_id_valid(input_id)) {
        return (event == 0) ? 0 : -EINVAL;
    }

    diag = &_tm_input_channels[(uint32_t)input_id].diag;
    if ((event & (BUTTON_EVENT_LONG_PRESSED | BUTTON_EVENT_REPEATED)) != 0) {
        if (_tm_input_lock() != 0) {
            return -EIO;
        }
        if ((event & BUTTON_EVENT_LONG_PRESSED) != 0) {
            diag->long_press_count++;
        }
        if ((event & BUTTON_EVENT_REPEATED) != 0U) {
            diag->repeat_count++;
        }
        diag->last_click_count = 0U;
        _tm_input_unlock();
        return _tm_input_dispatch_event(input_id, event, 0U);
    }

    if ((event & BUTTON_EVENT_CLICKED) == 0) {
        return 0;
    }

    if (_tm_input_lock() != 0) {
        return -EIO;
    }
    diag->click_count++;
    diag->last_click_count = 1U;
    _tm_input_unlock();
    return _tm_input_dispatch_event(input_id, event, 1U);
}

/* ---------------------------------------- end of file ----------------------------------------- */
