/**
 * @file    app_shell_task.c
 * @brief   Application-owned interactive shell session task
 * @version v1.0.0
 * @date    2026.05.12
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "app_shell_task.h"

#include "FreeRTOS.h"
#include "cmsis_os2.h"
#include "config/tm_core_config.h"
#include "console_session_config.h"
#include "tm_runtime.h"
#include "tm_shell.h"

#include <errno.h>
#include <string.h>
#if (TM_APP_FEATURE_BUDGET_PROBE != 0U)
#include "tm_task_budget_probe.h"
#endif

#if (TM_SHELL_TASK_QUEUE_DEPTH == 0U)
#error "TM_SHELL_TASK_QUEUE_DEPTH must be greater than zero."
#endif

#if (TM_SHELL_ENABLE != 0U)

/* Private Macros ------------------------------------------------------------------------------- */

#define TM_SHELL_TASK_STATUS_INIT (-EAGAIN)

/* Private State -------------------------------------------------------------------------------- */

struct tm_shell_task_line_msg {
    uint16_t len;
    char line[SHELL_LINE_MAX + 1U];
};

static osThreadId_t _tm_shell_task_handle = NULL;
static osMessageQueueId_t _tm_shell_task_queue = NULL;
static StaticQueue_t _tm_shell_task_queue_cb;
static uint8_t
    _tm_shell_task_queue_storage[TM_SHELL_TASK_QUEUE_DEPTH * sizeof(struct tm_shell_task_line_msg)];

static const osMessageQueueAttr_t _tm_shell_task_queue_attr = {
    .name = "app_shell_task",
    .cb_mem = &_tm_shell_task_queue_cb,
    .cb_size = sizeof(_tm_shell_task_queue_cb),
    .mq_mem = _tm_shell_task_queue_storage,
    .mq_size = sizeof(_tm_shell_task_queue_storage),
};

static const osThreadAttr_t _tm_shell_task_attr = {
    .name = "app_shell_task",
    .stack_size = TM_APP_TASK_STACK_SHELL_BYTES,
    .priority = (osPriority_t)osPriorityBelowNormal,
};

static int _tm_shell_task_status = TM_SHELL_TASK_STATUS_INIT;
static tm_shell_task_banner_fn_t _tm_shell_task_banner_hook = NULL;
static bool _tm_shell_task_transport_ready = false;

static struct {
    uint16_t queue_used;
    uint16_t queue_high_water;
    uint32_t submit_ok_count;
    uint32_t submit_fail_count;
} _tm_shell_task_diag = {0};

/* Private Helpers ------------------------------------------------------------------------------ */

static void _tm_shell_task_entry(void *argument);
static void _tm_shell_task_wait_transport_ready(void);

/* Private Functions ---------------------------------------------------------------------------- */

static void _tm_shell_task_queue_reserve_locked(void)
{
    _tm_shell_task_diag.queue_used++;
    if (_tm_shell_task_diag.queue_used > _tm_shell_task_diag.queue_high_water) {
        _tm_shell_task_diag.queue_high_water = _tm_shell_task_diag.queue_used;
    }
}

static void _tm_shell_task_queue_release_locked(void)
{
    if (_tm_shell_task_diag.queue_used > 0U) {
        _tm_shell_task_diag.queue_used--;
    }
}

static int _tm_shell_task_take_next_line(struct tm_shell_task_line_msg *msg, uint32_t timeout)
{
    osStatus_t status;

    if (msg == NULL) {
        return -EINVAL;
    }

    status = osMessageQueueGet(_tm_shell_task_queue, msg, NULL, timeout);
    if (status == osOK) {
        tm_task_critical_enter();
        _tm_shell_task_queue_release_locked();
        tm_task_critical_exit();
        return 1;
    }

    if ((status == osErrorResource) || (status == osErrorTimeout)) {
        return 0;
    }

    return -EIO;
}

static int _tm_shell_task_process_once(uint32_t timeout)
{
    struct tm_shell_task_line_msg msg;
    int result;

    result = _tm_shell_task_take_next_line(&msg, timeout);
    if (result <= 0) {
        return result;
    }

    shell_process_line(msg.line, msg.len);
    return 1;
}

/* Public Functions ----------------------------------------------------------------------------- */

int tm_shell_task_init(void)
{
    if (_tm_shell_task_handle != NULL) {
        return 0;
    }

    _tm_shell_task_transport_ready = false;

    _tm_shell_task_queue = osMessageQueueNew(
        TM_SHELL_TASK_QUEUE_DEPTH,
        sizeof(struct tm_shell_task_line_msg),
        &_tm_shell_task_queue_attr
    );
    if (_tm_shell_task_queue == NULL) {
        _tm_shell_task_status = -ENOMEM;
        return _tm_shell_task_status;
    }

    _tm_shell_task_handle = osThreadNew(_tm_shell_task_entry, NULL, &_tm_shell_task_attr);
    if (_tm_shell_task_handle == NULL) {
        (void)osMessageQueueDelete(_tm_shell_task_queue);
        _tm_shell_task_queue = NULL;
        _tm_shell_task_status = -ENOMEM;
        return _tm_shell_task_status;
    }

#if (TM_APP_FEATURE_BUDGET_PROBE != 0U)
    tm_task_budget_probe_register("shell", _tm_shell_task_handle, TM_APP_TASK_STACK_SHELL_BYTES);
#endif
    _tm_shell_task_status = 0;
    return 0;
}

int tm_shell_task_submit_line(const char *line, uint16_t len)
{
    struct tm_shell_task_line_msg msg = {0};
    bool use_queue;
    osStatus_t status;

    if (((line == NULL) && (len > 0U)) || (len > SHELL_LINE_MAX)) {
        return -EINVAL;
    }

    if ((line != NULL) && (len > 0U)) {
        memcpy(msg.line, line, len);
    }
    msg.line[len] = '\0';
    msg.len = len;

    use_queue = false;
    tm_task_critical_enter();
    if ((_tm_shell_task_queue == NULL) || (_tm_shell_task_handle == NULL) ||
        (_tm_shell_task_status != 0) || !tm_scheduler_is_running() || tm_is_in_isr()) {
        _tm_shell_task_diag.submit_fail_count++;
        tm_task_critical_exit();
        return -EAGAIN;
    }

    if (_tm_shell_task_diag.queue_used < TM_SHELL_TASK_QUEUE_DEPTH) {
        _tm_shell_task_queue_reserve_locked();
        use_queue = true;
    } else {
        _tm_shell_task_diag.submit_fail_count++;
        tm_task_critical_exit();
        return -EAGAIN;
    }
    tm_task_critical_exit();

    status = osMessageQueuePut(_tm_shell_task_queue, &msg, 0U, 0U);
    if (status == osOK) {
        tm_task_critical_enter();
        _tm_shell_task_diag.submit_ok_count++;
        tm_task_critical_exit();
        return 0;
    }

    if (use_queue) {
        tm_task_critical_enter();
        _tm_shell_task_queue_release_locked();
        _tm_shell_task_diag.submit_fail_count++;
        tm_task_critical_exit();
    }

    return -EAGAIN;
}

bool tm_shell_task_diag_snapshot(struct tm_shell_task_diag_snapshot *snapshot)
{
    bool scheduler_running;

    if (snapshot == NULL) {
        return false;
    }

    scheduler_running = tm_scheduler_is_running();
    tm_task_critical_enter();
    snapshot->status = _tm_shell_task_status;
    snapshot->initialized =
        ((_tm_shell_task_queue != NULL) && (_tm_shell_task_handle != NULL) &&
         (_tm_shell_task_status == 0));
    snapshot->transport_ready = snapshot->initialized && _tm_shell_task_transport_ready;
    snapshot->ready = snapshot->initialized && snapshot->transport_ready && scheduler_running;
    snapshot->queue_depth = TM_SHELL_TASK_QUEUE_DEPTH;
    snapshot->queue_used = _tm_shell_task_diag.queue_used;
    snapshot->queue_high_water = _tm_shell_task_diag.queue_high_water;
    snapshot->submit_ok_count = _tm_shell_task_diag.submit_ok_count;
    snapshot->submit_fail_count = _tm_shell_task_diag.submit_fail_count;
    tm_task_critical_exit();

    return true;
}

void tm_shell_task_set_banner_hook(tm_shell_task_banner_fn_t fn)
{
    tm_task_critical_enter();
    _tm_shell_task_banner_hook = fn;
    tm_task_critical_exit();
}

void tm_shell_task_notify_transport_ready(void)
{
    tm_task_critical_enter();
    _tm_shell_task_transport_ready = true;
    tm_task_critical_exit();
}

#if defined(THEMIS_HOST_TEST)
int tm_shell_task_host_process_once(void)
{
    return _tm_shell_task_process_once(0U);
}
#endif

static void _tm_shell_task_entry(void *argument)
{
    tm_shell_task_banner_fn_t banner_hook;

    (void)argument;

    _tm_shell_task_wait_transport_ready();

    tm_task_critical_enter();
    banner_hook = _tm_shell_task_banner_hook;
    tm_task_critical_exit();
    if (banner_hook != NULL) {
        banner_hook();
    }

    for (;;) {
        (void)_tm_shell_task_process_once(osWaitForever);
    }
}

static void _tm_shell_task_wait_transport_ready(void)
{
    for (;;) {
        bool ready;

        tm_task_critical_enter();
        ready = _tm_shell_task_transport_ready;
        tm_task_critical_exit();
        if (ready) {
            return;
        }

        osDelay(1U);
    }
}

#endif /* TM_SHELL_ENABLE != 0U */

/* ---------------------------------------- end of file ----------------------------------------- */
