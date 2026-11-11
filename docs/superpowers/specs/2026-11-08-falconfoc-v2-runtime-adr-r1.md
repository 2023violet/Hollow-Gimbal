# FalconFOC V2 Runtime ADR R1

> 状态：Proposal，配合 FalconFOC V2 平台重构设计使用。
>
> 日期：2026-11-08

## 1. 决策摘要

V2 reference runtime 采用混合执行模型：

```text
硬实时中断域
    -> ADC/PWM 采样和电流控制
    -> 传感器时间边界
    -> 快速故障处理
    -> PWM 安全提交

软实时前台域
    -> UART 字节流消费
    -> 协议解析
    -> Device Supervisor
    -> 静态协作式调度
    -> 参数和校准事务
    -> 遥测和日志

物理安全域
    -> 硬件保护
    -> 软件禁止输出锁存
    -> 独立看门狗
    -> 失联和超时恢复
```

当前 Legacy 固件继续使用已验证的 FreeRTOS 配置。V2 Core 不依赖 FreeRTOS。V2 reference runtime 默认采用本 ADR 的裸机混合模型。未来可以增加 FreeRTOS runtime adapter，但不能改变 Core、Protocol 和 Device service 的语义。

## 2. 已确认前提

- 当前 AT32F415 固件可正常构建和运行。
- 当前 FreeRTOS `ARM_CM3` port 用于无 FPU Cortex-M4 是合理的，不以 CPU 名称相同作为 port 选择条件。
- 当前 `cortex-m3-soft` 是 Legacy 的兼容构建配置，不在 G1 中强制替换。
- 当前 Board1 外部通信以 USART 为准。
- 当前 USART1 已使用 DMA RX/TX 和 IDLE 中断。
- 当前 TMR1、TMR3、TMR5 已经参与 PWM、采样同步和电机软中断路径。
- 当前 TMR2 没有进入生成式初始化链。
- 当前编码器默认使用阻塞式 SPI 读取，存在无界等待循环。
- 当前 TMR1 Break 没有启用，不能把软件 fault port 名称当成完整硬件故障闭环。

## 3. 执行域

### 3.1 硬实时中断域

硬实时域负责：

- ADC injected sample capture
- 快速母线和相电流有效性检查
- 电流环
- 角度和调制计算
- PWM output update
- 快速过流和非法数据处理
- 禁止输出锁存的最终复核

硬实时域禁止：

- UART 协议解析
- 文本格式化
- Flash 擦写
- 阻塞 SPI
- 阻塞 DMA 等待
- malloc/free
- 不受上界控制的循环
- 等待前台域发布数据

### 3.2 软实时前台域

前台域运行在主循环中，负责：

- UART RX ring drain
- 有界协议解析
- Device request 校验
- 命令邮箱发布
- 状态机推进
- 速度和位置慢环服务
- 校准事务
- 参数保存事务
- 遥测发送
- trace 管理
- supervisor heartbeat

前台域中的每个 service 必须有最大处理工作量。单次循环不能无限消费输入，也不能为了完成一个后台事务持续占用 CPU。

### 3.3 物理安全域

物理安全域独立于前台主循环，包含：

- DRV 或定时器硬件保护
- 软件快速故障锁存
- 独立看门狗
- fast tick heartbeat
- foreground heartbeat
- command freshness
- ADC sample freshness
- 输出禁止状态

看门狗只有在 fast tick 和 foreground supervisor 都在规定窗口内推进时才允许喂狗。单独的主循环运行不能证明控制链健康。

## 4. 当前 Reference Device 的资源决策

以下是 V2 的首选资源分配。它们在 G1 资源审计前属于候选锁定项，实施人员不得自行改用其他定时器或 DMA 通道。

| 资源 | 首选职责 | 触发或执行位置 | 锁定条件 |
| --- | --- | --- | --- |
| TMR1 | 20 kHz PWM 和 CC4 采样定位 | 硬件加短 ISR | 保留当前 PWM 语义 |
| ADC1 | injected sample、快速检查和 fast tick 输入 | ADC ISR | 样本有效性可观测 |
| TMR5 | 5 kHz 传感器时间边界和外环时间边界 | TMR5 ISR | 不允许阻塞 SPI |
| TMR2 | 1 MHz 单调时间源候选 | 硬件计数 | 通过芯片和板级资源审计 |
| SysTick | 1 ms 前台唤醒标志 | 极短 ISR | 不承载服务逻辑 |
| USART1 | UART Protocol v1 | DMA + 前台 | 连续流和溢出可检测 |
| SPI1 | 编码器事务 | 非阻塞 DMA 候选 | DMA 通道不与 ADC/UART 冲突 |
| WDT | 独立复位保护 | 硬件 | 喂狗条件由 supervisor 统一控制 |

G1 必须输出资源矩阵，至少包含：

- timer 实例和位宽
- timer clock source
- IRQ vector
- DMA channel/request mapping
- ADC/PWM/encoder 资源冲突
- 中断优先级
- 可用 GPIO 和外部故障输入
- WDT 类型和复位行为

如果 TMR2 或 SPI DMA 候选与真实资源冲突，必须新增 ADR 修订记录。不能在实现时静默切换到 TMR3、TMR11 或其他 DMA 通道。

## 5. Fast tick 时序

### 5.1 TMR1 CC4 ISR

职责只包括：

1. 检查并清除 CC4 标志。
2. 触发 ADC injected group。
3. 记录采样触发序号和时间戳。
4. 检查上一组 ADC 是否仍未完成。
5. 在连续未完成时设置 sample overrun 标志。

CC4 ISR 不执行 FOC 算法，不等待 ADC，不等待 DMA，不重新执行协议逻辑。

### 5.2 ADC ISR

职责顺序固定为：

1. 读取 ADC injected results。
2. 记录转换完成时间戳和序号。
3. 检查采样窗口、数据范围和 freshness。
4. 执行快速过流、过压和欠压检查。
5. 如果输出禁止锁存有效，立即写安全输出并返回。
6. 按 fast loop divider 执行 `foc_fast_tick()`。
7. 在 PWM 更新窗口提交占空比。
8. 更新 fast heartbeat、WCET 和 overrun 统计。

fast tick 必须使用完整的 `sample_contract` 和 `timing_contract`。当样本过期、漏样或执行超时超过规定次数时，进入安全降级或 fault latch。

### 5.3 TMR5 ISR

TMR5 只作为 5 kHz 的传感器和外环时间边界。

首选行为：

1. 检查上一笔编码器 DMA 事务是否完成。
2. 发布上一笔完整编码器样本，或标记样本无效。
3. 发起下一笔非阻塞 SPI DMA 事务。
4. 使用最近一笔完整且未过期的传感器样本运行外环。
5. 更新 sensor heartbeat 和 outer-loop WCET。

禁止在 TMR5 ISR 中调用任何无限等待的 SPI 读函数。当前 `encoder_update()` 的阻塞路径只能留在 Legacy 或被改造成有界非阻塞事务后才能进入 V2。[encoder.c](D:/study/Software/Hollow-Gimbal/applications/periph/encoder.c:91)

## 6. 前台超级循环

主循环的固定顺序为：

```c
for (;;) {
    now = monotonic_time_us();

    service_safety_events();
    uart_rx_drain_bounded();
    protocol_dispatch_bounded();
    device_supervisor_step();
    scheduler_poll(now);
    telemetry_tx_service_bounded();
    storage_service_if_safe();
    supervisor_publish_heartbeat();
    idle_until_interrupt_if_possible();
}
```

每个 service 必须满足：

- 有最大处理帧数或最大字节数
- 有最大执行时间
- 可以提前返回
- 不等待外设状态改变
- 不在运行态执行 Flash 擦写
- 不直接修改 fast control context

如果任何 service 超过预算，记录 overrun 并继续下一轮。不能通过扩大单次预算掩盖系统性过载。

## 7. 静态协作调度器

调度器使用静态任务表，不创建线程，不创建动态任务，不提供抢占。

```c
struct runtime_job {
    uint32_t period_us;
    uint32_t deadline_us;
    uint32_t budget_us;
    uint32_t next_due_us;
    uint8_t  priority;
    bool (*run)(uint32_t now_us, uint32_t budget_us);
};
```

第一版 job 表建议：

| Job | 周期 | 最大工作 |
| --- | --- | --- |
| Device supervisor | 1 ms | 一次状态推进 |
| UART protocol | 每轮 + 1 ms 保护周期 | 固定帧数和字节数 |
| Calibration | 1 ms | 一个状态机 step |
| Storage | 1 ms | 一个非阻塞事务 step |
| Telemetry | 10 ms | 固定发送预算 |
| Diagnostics | 10 ms | 固定快照读取 |
| Health monitor | 1 ms | heartbeat 和 deadline 检查 |

调度器不负责 fast current loop，也不负责硬件 fault。

## 8. 数据所有权和并发规则

### 8.0 H0/H1 控制状态所有权

H0 是实时电流控制域，H1 是传感器与运动外环域。

H1 独占写入：

- 编码器和传感器状态
- 速度环状态
- 位置环状态
- 校准状态
- 外环目标生成状态

H1 只发布不可变的 `validated_motion_target`，其中包含 `iq_target` 或其他合法内环目标、时间戳、有效期和质量标记。

H0 独占写入：

- 电流环积分器
- 电流环滤波器状态
- 调制状态
- PWM 输出状态
- 快速故障状态

H0 在控制周期边界消费 H1 的完整目标快照。H0 不直接访问 H1 的内部控制状态，H1 不直接修改 H0 的积分器或调制状态。

### 8.1 高频控制命令

使用单槽 latest-wins mailbox：

- 普通 setpoint 只保留最新一份。
- 旧目标不排队。
- 每份命令带发布序号和 freshness deadline。
- fast domain 只在控制边界读取完整快照。

### 8.2 安全命令

使用独立优先级锁存通道：

- `DISABLE`
- `FAULT_TRIP`
- `COMMAND_TIMEOUT`
- `ADC_INVALID`
- `OUTPUT_INHIBIT`

安全通道不能因为普通 mailbox 或遥测队列满而阻塞。

### 8.3 参数和校准

参数写入、擦除和校准请求使用有界 FIFO。队列满时返回 `BUSY`，不覆盖旧请求。

`RUNNING` 状态禁止 Flash 擦写。最终 Flash 写入点必须再次检查：

- 轴状态
- 输出使能状态
- fault latch
- scheduler context
- 电源状态

### 8.4 复合快照

ISR 和前台之间的复合数据使用双缓冲和发布索引：

1. 写入者写 inactive buffer。
2. 写入完整后执行 release barrier。
3. 发布 index。
4. 读取者读取 index 后执行 acquire barrier。
5. 读取者只读取已发布 buffer。

禁止读写同一个复合结构的部分字段。

### 8.5 三类数据交换规则

| 数据方向 | 机制 | 一致性规则 | 失败动作 |
| --- | --- | --- | --- |
| 前台 → H0 | 单写者双缓冲 `validated_motion_target` | H0 只在控制边界读取已发布版本 | 过期则进入命令超时策略 |
| H1 → H0 | 单写者快照 `sensor_snapshot` | H0 只消费完整且未过期样本 | 样本无效则保持安全目标或 fault |
| H0 → 前台 | 一致性复制 `telemetry_snapshot` | 前台读取前后检查 sequence，失败则丢弃本次非关键遥测 | 不重试，不阻塞 H0 |

H0 不等待前台读取。前台不允许通过关闭中断或自旋锁阻塞 H0。遥测样本可以丢失，安全事件和禁止输出状态不能丢失。

## 9. 状态机和输出语义

运动状态与功率输出状态分开。

### 9.1 运动状态

```text
BOOT
  -> IDLE
  -> CALIBRATION
  -> READY
  -> RUNNING
  -> STOPPING
  -> FAULT
```

### 9.2 功率输出状态

```text
DISABLED
  -> ENABLE_PENDING
  -> ENABLED
  -> INHIBITED
  -> DISABLED
```

命令语义固定：

- `STOP` 只改变运动目标和运动状态，不等同于断功率。
- `DISABLE` 设置禁止输出锁存并关闭功率输出。
- `BRAKE` 是独立能力，Board1 在完成物理验证前不开放。
- `FAULT_CLEAR` 清除条件满足后只回到 `DISABLED` 或 `IDLE`，绝不自动恢复 `RUNNING`。

### 9.3 失败动作

| 失败 | 动作 |
| --- | --- |
| 命令过期 | fast domain 设置禁止输出或进入预定义安全状态 |
| ADC 连续无效 | 停止使用旧数据，达到次数后 fault latch |
| fast deadline 超时 | 关闭输出或进入限制模式，记录 overrun |
| foreground 卡死 | supervisor heartbeat 不更新，独立 WDT 复位 |
| UART RX 溢出 | 丢弃当前不完整帧，记录 transport fault，保留安全命令通道 |
| Flash 写入失败 | 保留旧记录，报告持久化错误，不改变运行态安全状态 |

## 10. 初始频率目标

以下是 Reference Device 的初始目标，不是已经完成的实机验收：

| 功能 | 初始目标 | 执行域 |
| --- | --- | --- |
| PWM | 20 kHz | 硬件 |
| 电流控制 | 5 kHz | ADC/FOC ISR |
| 传感器时间边界 | 5 kHz | TMR5 ISR |
| 速度环 | 1 kHz | 确定性控制时间边界 |
| 位置环 | 500 Hz | 确定性控制时间边界 |
| Device Supervisor | 1 kHz | 前台 |
| 默认遥测 | 不超过 100 Hz | 前台 |
| UART RX | DMA 事件加有界轮询 | DMA/前台 |

验收记录必须同时包含：

- WCET
- 最大抖动
- 漏样次数
- sample-to-PWM 延迟
- deadline miss
- UART 过载行为
- fault response latency

平均频率接近目标不能替代最坏情况验收。

## 11. G0/G1/G2 运行时门禁

### G0

- Legacy 可重建
- 当前 FreeRTOS 配置和板级时序不改
- 记录当前测试和镜像

### G1

- 完成 TMR、ADC、DMA、SPI、USART、WDT 资源矩阵
- 确认 TMR2 是否可作为单调时间源
- 确认 SPI DMA 通道是否可用
- 测量当前 ISR、ADC 注入和 TMR5 周期
- 记录当前 encoder 阻塞路径
- 完成 UART 连续流和 RX 溢出实验设计
- 不强制迁移 CPU class 或 FreeRTOS port

### G2

- Core 可以在主机编译
- 最小 Core 支持电压开环和限幅
- Core 不依赖 FreeRTOS、UART 或 board headers
- Runtime API 使用显式时间、sample 和 output contract

### G3

- 当前板完成 ADC → Core → PWM 最小纵向链
- fast path 不依赖前台
- timing、sample freshness 和 output inhibit 可观测

### G3-SAFE 前置门禁

G3 在任何真实 PWM 输出或电机带电测试前，必须先具备以下最小安全链：

- 上电默认禁止输出
- 功率输出使能守卫
- 电压和电流限制
- 独立计时的运行超时
- 快速 ADC/母线/非法数据检查
- 立即禁止输出路径
- 命令过期处理
- fast heartbeat
- foreground heartbeat
- 独立看门狗监督条件

G3-SAFE 未通过时，只允许完成无功率输出的编译、寄存器、采样和 Core 模拟验证。不能进入带电机的运行测试。

### G4 调度门禁

前台调度器必须满足：

- 每轮最大执行预算可测量
- 每个 job 有最大允许迟到时间
- 高优先级服务有单轮工作上限
- 低优先级 job 具有饥饿计数
- 过期 job 按规则跳过或降级
- 调度超限生成可观测事件
- UART 满负载不能无限延迟 storage、health 和 safety service

## 12. 禁止实施人员自行决定的事项

1. 不替换 Legacy CPU class、FreeRTOS port 或已验证板级时序。
2. 不把 FOC 电流环放入超级循环。
3. 不把协议解析放入控制 ISR。
4. 不把所有 service 放入 SysTick ISR。
5. 不允许 SPI、UART 或 DMA 等待进入新的实时链。
6. 不允许 `RUNNING` 状态执行 Flash 擦写。
7. 不允许改变 TMR2、TMR5、DMA 和 SPI 资源分配而不更新 ADR。
8. 不允许在缺少物理测量时更改 Board1 相序、ADC 采样点、deadtime 或 BRAKE 语义。
9. 不允许把平均频率、host test 或软件 trace 写成物理输出验收。

## 13. G0/G1 audit status

The 2026-11-08 read-only audit recorded the following:

- Legacy baseline HEAD: `1f542521189d5e0ef25e64e6530608ece8d6e137`.
- Host CTest: `45/45 PASS`.
- AT32F415 bench debug build: PASS with the existing `cortex-m3-soft` and FreeRTOS Legacy configuration.
- TMR2: candidate monotonic-time source, not allocated.
- SPI1 DMA: SDK request identifiers exist, but current generated DMA1 channels 1-4 are UART-owned. Channels 5-7 remain candidates pending request-priority and interrupt audit.
- WDT/WWDT: current generated configuration does not enable a hardware watchdog module. G3-SAFE remains blocked until watchdog selection and reset behavior are explicit.
- Encoder SPI: current active path contains blocking status polling. It is not accepted as the V2 H1 execution path.
- No powered-board action occurred.

The evidence records are in `docs/evidence/2026-11-08-*.md`. These findings do not authorize resource allocation or Legacy source changes.
