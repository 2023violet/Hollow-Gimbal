# Themis 时间片调度组件设计方案

> 组件路径：`components/utils/sched/`
> 版本：`v2.0.0`
> 日期：`2026.05.02`
> 定位：portability-first 的裸机协作式周期调度内核

---

## 1. 设计目标

这次重写的目标不是把 `sched` 继续堆成“迷你 RTOS”，而是把它收敛成一套边界清晰、对外契约干净、可长期作为通用组件复用的协作式调度器。

必须达成的结果：

- public surface 不再暴露任何内部字段。
- 调用者只能通过 opaque handle 和 caller-owned storage 使用组件。
- tick 时间模型从旧的 32 位绕回技巧升级为 64 位单调时间域。
- 并发模型只保留一种：调用者锁钩子串行化任务上下文 API 与 tick 上下文。
- 任务状态、剩余时间、统计、ready 数量全部通过权威查询 API 返回。

---

## 2. 为什么彻底放弃 public struct

旧模型把 `struct sched_core` 和 `struct sched_task` 完整暴露给调用者，短期看接入成本低，长期看有三类硬伤：

- 任何字段布局调整都会破坏 ABI 和静态初始化习惯。
- 测试、诊断、业务代码都会开始直接依赖内部字段，封装彻底失效。
- 一旦引入原子或并发修补，public header 会被实现细节污染。

重写后的选择是：

- `sched.h` 只保留前向声明。
- 调用者通过 `SCHED_CORE_STORAGE()` 和 `SCHED_TASK_STORAGE()` 提供对齐后的原始存储。
- `sched.c` 独占内部布局定义。

这样做的代价是调用者必须走初始化函数，收益是后续可以继续演进内部状态而不再把库契约绑死在结构体字段上。

---

## 3. 时间模型

### 3.1 旧问题

旧版依赖 32 位 tick 的“有符号差值小于半环”语义。它在短周期、小 uptime 场景可用，但契约本身并不自解释：

- 大于半环的 period/phase/delay 会直接失真。
- 调用者必须记住隐藏的 `< 2^31` 限制。
- 诊断字段只看绝对值时很难判断状态是否已跨环。

### 3.2 新方案

新版本统一采用 `uint64_t now_ticks`：

- `sched_core_tick()` 只做单调加法。
- `next_release_tick`、remaining、fast-forward、overrun 计算都在 64 位空间完成。
- 所有可能溢出的加法和乘法都显式做 `-ERANGE` 检查。

这个模型的好处是：

- 数学语义直接、无需 half-range 记忆。
- 调用者只需要确保 tick 单位足够粗，使命周期远小于 `UINT64_MAX`。
- phase/restart/remaining query 的结果在 API 级别天然可解释。

---

## 4. 并发模型

### 4.1 旧问题

旧实现最大的问题不是功能缺失，而是并发语义“半内化半外包”：

- 一部分状态靠调用者临界区保护。
- 一部分热字段靠普通对象访问。
- 一部分逻辑默认 tick/run 不并发，但 API 表面并未完全收口。

这类缝合模式对通用组件不合格，因为它既不完全 lock-free，也不完全 caller-serialized。

### 4.2 新方案

新版本只保留一种策略：

- `sched_core_tick()` 由调用者放在定时 ISR 或同一串行上下文中推进。
- 任务上下文 API 通过 `struct sched_lock_iface` 可选安装锁钩子。
- 如果钩子存在，所有任务上下文 API 统一进入和退出钩子。
- handler 与 idle hook 必须在解锁后执行，避免持锁回调。

这个约束让组件的文档、实现、测试三者重新一致：

- 没有原子字段暴露到 public header。
- 没有“只有某些 API 需要锁”的隐性例外。
- 自注销这类复杂路径也能成立，因为 handler 在锁外运行。

---

## 5. 调度与数据结构

调度器仍然保留最适合小 MCU 的基础策略：

- intrusive 单链表保存注册任务；
- 按 priority 升序插入；
- 同优先级保持 FIFO 注册顺序；
- `tick()` 只标记 pending，不执行 handler；
- `run_budget()` 按顺序消耗 ready task。

为什么不改成堆、红黑树或多级 ready bitmap：

- 目标任务数通常只有 `3~20`。
- 小 MCU 上更重要的是代码尺寸、调试可读性和零动态分配。
- 这次重写的核心是契约收口，不是把协作式调度器做成通用实时内核。

---

## 6. 控制面语义

重写后保留并强化了以下控制语义：

- `sched_task_set_phase()`：
  - 未注册时只更新 phase。
  - 已注册时丢弃当前 pending，并从 `now + phase` 重新播种。
- `sched_task_restart()`：
  - 丢弃 pending，并从 `now + phase` 重启 release 时序。
- `sched_task_disable()`：
  - 关闭任务并清掉 pending。
- `sched_task_enable()`：
  - 若旧 deadline 已过，则在任务上下文 fast-forward 到未来窗口。
  - 不制造 backlog。

这些语义的统一点是：

- 组件内部独占 release 状态；
- 调用者不再自己推断 remaining 或 next tick；
- 任何“重新对齐时间”的行为都通过显式 API 完成。

---

## 7. 查询面

旧版最大的问题之一，是调用者很容易绕过 API 直接读内部字段。

新版本改成三类官方查询：

- `sched_core_get_info()`
  - 返回 now / task_count / enabled_count / ready_count / lock_installed。
- `sched_task_get_info()`
  - 返回 period / phase / next_release / remaining / run / overrun / flags。
- `sched_core_get_task_info_at()`
  - 按调度顺序枚举任务快照。

这让 shell、diag、host test 都能只依赖正式契约，不再偷看内部结构。

---

## 8. 为什么保留 caller-owned storage

没有引入动态内存，也没有把 core/task 变成堆分配句柄，原因很简单：

- 这个组件首先是小 MCU 通用库，不是桌面框架。
- 调用者通常更愿意把调度器嵌入自己的静态生命周期对象中。
- caller-owned storage 既保住了 opaque 封装，也保住了零分配特性。

这也是本次设计和许多“opaque = malloc handle”的库最大的不同点。

---

## 9. 测试策略

新的 host 测试围绕 public contract 组织，而不是围绕内部字段组织。必须覆盖：

- storage 对齐与尺寸校验；
- opaque 初始化和错误码；
- 注册顺序与优先级/FIFO；
- budget dispatch；
- phase/restart/reseed；
- disable/enable fast-forward；
- overrun 统计；
- idle hook；
- self-unregister；
- lock hook 计数；
- 64 位域下的 `-ERANGE` 溢出拒绝。

这保证后续继续调整 `sched.c` 内部布局时，测试仍然稳定。

---

## 10. 结论

这次 `sched` 重写的核心不是“功能更多”，而是“契约更真”：

- public surface 彻底 opaque；
- 时间模型彻底自洽；
- 并发模型彻底单一；
- 查询面彻底权威；
- host test 彻底站在 public API 之上。

对 Themis 来说，这样的 `sched` 才算真正达到可独立复用的通用组件水位。
