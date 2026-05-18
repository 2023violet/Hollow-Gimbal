# Themis 时间片调度组件使用指南

> 组件路径：`components/utils/sched/`
> 版本：`v2.0.0`
> 适用对象：小资源 MCU 裸机主循环中的协作式周期任务调度

---

## 1. 最小接入

### 1.1 声明 opaque storage

```c
#include "sched.h"

SCHED_CORE_STORAGE(g_sched_core_storage) = {0};
SCHED_TASK_STORAGE(g_sensor_task_storage) = {0};

static struct sched_core *g_sched_core = NULL;
static struct sched_task *g_sensor_task = NULL;

static void sensor_task_handler(void *user_data)
{
    (void)user_data;
    /* 非阻塞周期逻辑 */
}
```

### 1.2 初始化并注册任务

```c
void app_sched_init(void)
{
    (void)sched_core_init(g_sched_core_storage,
                          sizeof(g_sched_core_storage),
                          NULL,
                          &g_sched_core);

    (void)sched_task_init(g_sensor_task_storage,
                          sizeof(g_sensor_task_storage),
                          "sensor",
                          10U,
                          10U,
                          0U,
                          sensor_task_handler,
                          NULL,
                          &g_sensor_task);

    (void)sched_core_register(g_sched_core, g_sensor_task);
}
```

### 1.3 接入 tick 和 run

```c
void SysTick_Handler(void)
{
    (void)sched_core_tick(g_sched_core, 1U);
}

int main(void)
{
    app_sched_init();

    while (1) {
        (void)sched_core_run(g_sched_core, NULL);
    }
}
```

默认语义：

- 任务注册后不会立即触发。
- 首次触发发生在 `now + phase_ticks`。
- 时间域是单调 `uint64_t`，不再依赖 32 位半程回绕技巧。
- 如果主循环控制路径和 `tick()` 不并发，可以不安装锁钩子。

---

## 2. 并发模型

这个版本只接受一种并发策略：

- `sched_core_tick()` 运行在定时 ISR，或者运行在与控制路径相同的串行上下文。
- `register / unregister / enable / disable / restart / set_phase / run / idle / get_info`
  这些任务上下文 API 如果要与 `tick()` 并发，必须统一通过调用者提供的临界区钩子串行化。
- 组件内部不再暴露或混用原子字段。

### 2.1 安装锁钩子

```c
static void sched_enter_lock(void *user_data)
{
    (void)user_data;
    __disable_irq();
}

static void sched_exit_lock(void *user_data)
{
    (void)user_data;
    __enable_irq();
}

static const struct sched_lock_iface g_sched_lock = {
    .enter = sched_enter_lock,
    .exit = sched_exit_lock,
    .user_data = NULL,
};

void app_sched_init(void)
{
    (void)sched_core_init(g_sched_core_storage,
                          sizeof(g_sched_core_storage),
                          &g_sched_lock,
                          &g_sched_core);
}
```

注意：

- `sched_core_tick()` 自己不会调用这些钩子。
- handler 和 idle hook 都在解锁后执行。
- 没有锁钩子时，组件要求所有 API 都运行在同一执行上下文。

---

## 3. 相位、重启与暂停

### 3.1 错峰首发

```c
(void)sched_task_init(task_a_mem, sizeof(task_a_mem), "a", 10U, 1U, 0U, task_a, NULL, &task_a);
(void)sched_task_init(task_b_mem, sizeof(task_b_mem), "b", 10U, 4U, 0U, task_b, NULL, &task_b);
(void)sched_task_init(task_c_mem, sizeof(task_c_mem), "c", 10U, 7U, 0U, task_c, NULL, &task_c);
```

规则：

- `phase_ticks` 合法范围是 `1..period_ticks`。
- phase 只影响注册和显式 restart 后的首个 release。

### 3.2 运行中重启

```c
(void)sched_task_restart(g_sensor_task);
```

效果：

- 丢弃当前 pending release。
- 从当前 `now_ticks + phase_ticks` 重新播种下一次 release。

### 3.3 暂停与恢复

```c
(void)sched_task_disable(g_sensor_task);
(void)sched_task_enable(g_sensor_task);
```

效果：

- disable 会清掉当前 pending。
- disable 期间 `tick()` 完全跳过该任务，不制造 backlog。
- re-enable 时如果旧 deadline 已经过期，会在任务上下文一次性快进到未来窗口。

---

## 4. 预算运行与 idle hook

### 4.1 给主循环设置运行预算

```c
while (1) {
    poll_inputs();
    service_uart();
    (void)sched_core_run_budget(g_sched_core, 2U, NULL);
}
```

语义：

- 每轮最多派发 2 个 ready task。
- 剩余 pending 留在下一轮继续处理。
- 仍保持“低数值优先级优先、同优先级 FIFO”的顺序。

### 4.2 空闲钩子

```c
static void app_idle_hook(void *user_data)
{
    (void)user_data;
    __WFI();
}

(void)sched_core_set_idle_hook(g_sched_core, app_idle_hook, NULL);

while (1) {
    (void)sched_core_run(g_sched_core, NULL);
    (void)sched_core_idle(g_sched_core, NULL);
}
```

`sched_core_idle()` 只会在当前没有 ready work 时执行 hook。

---

## 5. 诊断查询

### 5.1 查询 core 快照

```c
struct sched_core_info info;

if (sched_core_get_info(g_sched_core, &info) == 0) {
    printf("now=%llu tasks=%u enabled=%u ready=%u\n",
           (unsigned long long)info.now_ticks,
           (unsigned)info.task_count,
           (unsigned)info.enabled_count,
           (unsigned)info.ready_count);
}
```

### 5.2 查询 task 快照

```c
struct sched_task_info info;

if (sched_task_get_info(g_sensor_task, &info) == 0) {
    printf("%s next=%llu remain=%llu run=%lu overrun=%lu\n",
           info.name ? info.name : "-",
           (unsigned long long)info.next_release_tick,
           (unsigned long long)info.remaining_ticks,
           (unsigned long)info.run_count,
           (unsigned long)info.overrun_count);
}
```

### 5.3 按调度顺序枚举任务

```c
struct sched_task_info info;

if (sched_core_peek_next_ready(g_sched_core, &info) == 0) {
    printf("next=%s prio=%u pending=%u\n",
           info.name ? info.name : "-",
           (unsigned)info.priority,
           info.pending ? 1U : 0U);
}
```

如果需要完整的注册顺序快照，再使用 `sched_core_get_task_info_at()` 进行遍历；如果只关心下一次会跑谁，优先用 `sched_core_peek_next_ready()`。

---

## 6. 契约边界

推荐：

- 周期任务数量少而稳定。
- 每个 handler 都是短小、无阻塞、无长时间临界区的状态机。
- 只把 release 调度交给这个组件，不把它当成 RTOS 替代品。

不推荐：

- 在 handler 中做长时间阻塞操作。
- 在没有锁钩子的情况下让 `tick()` 与控制路径并发。
- 让系统 tick 单位过细，以至于设备寿命逼近 `UINT64_MAX`。

---

## 7. 一句话总结

这个版本的 `sched` 已经不再是“公开裸 struct 的轻量玩具”，而是：

- public surface 完全 opaque；
- 时间模型统一到 64 位单调 tick；
- 并发策略只保留一种可证明模型；
- 查询面只通过权威快照 API 暴露。
