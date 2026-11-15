# FalconFOC V2｜NEXT-CODE-QUALITY-01 代码质量、Board 配置与初始化架构专项

## 结论

`NEXT-CODE-QUALITY-01 PASS`

本轮完成了 Board1 配置表达的最小可读性改进，并通过行为等价、Host、Sanitizer、质量门禁和 AT32F415 ARM 构建复验。`main.c` 初始化结构和 H0/H1 防御性检查均完成源码审查，未发现足以证明需要拆分或删除检查的事实，因此没有扩大修改范围。

本轮没有刷机、上电、启用 PWM、写 Flash、提交或推送。以下状态继续保持：

- `G3-SAFE HARDWARE NOT VERIFIED`
- `G3-PWR LOCKED`
- `AT32F415 WCET NOT VERIFIED`
- `MOTOR PHYSICAL OPERATION UNVERIFIED`

## 一、基线与范围

- 工作树：`D:/study/Software/Hollow-Gimbal`
- 分支：`codex/falconfoc-v2-rc0`
- HEAD：`46bc963f5e5849e4440d1714a845fe485d338837`
- 工作树在开工时已有多个阶段的未提交修改；本轮全部保留。
- 本轮新增或修改的本轮文件：
  - `firmware/at32f415/src/foc_board.c`
  - `docs/evidence/2026-11-14-next-code-quality-01.md`
  - `docs/governance/Themis_工程维护日志.md`

`foc_board.c` 中此前已有的 Sensor/Calibration 迁移修改属于先前阶段，本轮没有重写或回退；本轮只调整静态配置块的表达方式和注释。

## 二、实际代码修改

### Board 配置表达

将 `pipeline_config` 和 `runtime_config` 中依赖字段顺序的嵌套聚合初始化改为 designated initializer，覆盖：

- Current Core 的 `vbus_min_v`、`voltage_limit_v`、duty 边界、`max_dt_s`、d/q `Kp`、d/q `Ki`、电流限制。
- ADC 的 VBUS/电流换算、VBUS 最小值、三相零点和相序。
- PWM period 与相序。
- Safety 的命令超时、样本超时、Fast/Foreground heartbeat 超时和无效样本锁存阈值。
- H0、H1、监督、遥测的周期、最大迟到和预算。
- Rotor、prediction、velocity、position、q 轴电流限制和 trajectory enabled 标志。

所有数值、字段顺序对应的结构体布局、公共 API 和初始化生命周期均保持不变。ADC 两个换算公式使用既有 `FOC_BOARD_ADC_MAX`（值为 4095）作为唯一满量程来源，生成值保持一致。

新增注释明确：这些是 Board1 当前固件的名义默认值，不等同于已辨识的真实电机参数；转换因子和增益的单位由对应公共结构体契约定义。trajectory 仍明确为关闭，Feedforward 仍由未指定字段保持关闭。

### Magic Numbers 分类结果

| 类别 | 当前处理 | 结论 |
| --- | --- | --- |
| 数学常数 | 保持在数学实现或既有表达中 | 本轮没有把普通 `0/1` 机械宏化 |
| MCU/寄存器常量 | 使用现有 Board 枚举、宏和 vendor 标识 | 未新建 HAL 或全局宏头 |
| ADC/PWM/SPI 时间 | Board 常量或带单位字段；H0/H1/timeout/budget 改为命名字段 | 仍需实板测量确认能力边界 |
| Current PI/限幅 | 通过 `pwm`、`kp_v_per_a`、`ki_v_per_a_s`、`current_limit_a` 直接表达 | 数值是名义默认，非已辨识参数 |
| 电机物理/传感器参数 | `pole_pairs`、encoder counts、方向、LPF 等使用字段名表达 | 真实电机身份和参数来源仍未完成生产准入 |
| Safety 阈值 | 通过 `foc_safe_config` 字段表达 | 没有与普通控制参数混写 |
| 测试参数 | 未在本轮改动 | 测试假设继续由测试文件拥有 |

仍存在的参数来源缺口是物理事实问题，而不是命名问题：`Rs/Ld/Lq/ψf` 等真实 PMSM 参数还没有进入 Board1 的生产配置来源链；这与此前 Feedforward admission 的 Host-only 结论一致。本轮不伪造来源，也不自动开启 Feedforward。

## 三、main.c 启动与所有权审查

当前源码中的真实启动顺序为：

```text
main
 ├─ __disable_irq
 ├─ foc_at32_lock_output
 ├─ foc_at32_clock_init
 ├─ foc_board_init(runtime, output, 0)
 ├─ foc_at32_adc_init
 ├─ foc_at32_spi_init
 ├─ foc_at32_timers_init
 ├─ foc_serial_init + foc_at32_uart_init
 ├─ foc_at32_irq_enable
 ├─ enable TMR2/TMR5/TMR1 counters
 ├─ __enable_irq
 └─ foreground loop
     ├─ foc_runtime_foreground_poll
     ├─ foc_serial_poll
     └─ UART TX interrupt admission
```

IRQ 所有权如下：

- `TMR1_CH_IRQHandler` 负责 H0 触发窗口和 ADC acquisition token。
- `ADC1_IRQHandler` 读取四个 ADC rank，提交 Board ADC 完成事件并进入 H0。
- `TMR5_GLOBAL_IRQHandler` 负责 H1 编码器事务的开始、发送窗口和完成窗口。
- `SPI1_IRQHandler` 只更新 SPI phase/word/rx timestamp；H1 完成在 TMR5 事件中收口。
- `USART1_IRQHandler` 只操作 UART ring buffer。
- `runtime`、`output`、`adc`、`encoder`、SPI phase/word/timestamp 和 UART/Serial 均为 `main.c` 私有静态状态，与对应 ISR 生命周期一致。

平台层职责是时钟、GPIO、ADC、SPI、Timer、NVIC 和 IRQ 绑定；Board 层负责样本/输出边界；Runtime/应用层负责状态机、调度、Safety 和前台协议。当前 `main.c` 的静态 helper 已经按这些职责形成可读边界，且输出锁在任何外设启动前执行。

没有发现重复初始化或顺序错误。把外设函数移到多个文件会移动私有 IRQ 状态和初始化顺序，当前没有量化收益证据，反而会增加跨文件生命周期耦合。因此本轮 `main.c` 结论为 `ANALYSIS PASS / NO CHANGE`；只有当后续增加第二个 Board 或真实硬件提交链时，才建议以独立 `NEXT-BOOT-ARCH-01` 评审拆分。

## 四、防御性编程与热路径审计

审查对象：`foc_current_step_internal`、`foc_encoder_update`、`foc_runtime_h0_step`、`foc_runtime_h1_step`。

| 检查类型 | 频率/位置 | 防护目的 | 结论 |
| --- | --- | --- | --- |
| `dt`、VBUS、有限值、目标限幅 | H0 Current step | 拒绝动态非法 ADC/时间/命令并保持事务原子性 | 必须保留 |
| 电压/电流积分、SVPWM 和 anti-windup 的提交条件 | H0 Current step | 防止部分状态提交或无效输出 | 必须保留 |
| 角度、样本时间、transport、连续性和速度范围 | H1 Sensor update | 防止旧帧、丢帧、回绕和异常速度污染反馈 | 必须保留 |
| H0 heartbeat、Safety Gate、STOP/FAULT/INHIBIT 优先级 | H0 | 保证故障和停止请求优先于 UPDATE | 必须保留 |
| H1 snapshot 的时间、序列、有限值和目标有效性 | H1 | 防止旧快照和不一致外环目标进入 H0 | 必须保留 |
| 初始化静态配置校验 | init 路径 | 阻止非法配置进入热路径 | 保留；已由 init 契约承担 |

ARM 符号基线（V2 ELF）为：`foc_current_step_internal=0x39a`、`foc_runtime_h0_step=0x210`、`foc_runtime_h1_step=0x378`、`foc_encoder_update=0x404`、`foc_runtime_control=0x464`、`foc_pipeline_step_guarded=0x36a`。本轮没有发现能够由上游契约充分证明、且删除后不改变失败语义的重复检查，因此没有删除条件判断。源码中条件数量不能直接当作 MCU 周期成本；实际 WCET 仍需目标 MCU 测量。

## 五、行为等价证据

- `test-foc-board-config`：`1/1 PASS`。
- Board/UART/Python：`4/4 PASS`。
- ARM V2：`text=36,644 B`、`data=92 B`、`bss=3,544 B`、总静态使用 `40,280 B`；Flash `36,736 B`、RAM `3,632 B`。
- 重新构建后的 ELF/BIN/HEX/MAP SHA-256 与本轮基线一致：
  - ELF `51D7428FF2414FA4623DCE5086A6CD8D4F69058DFC6405B317DC28875E281551`
  - BIN `9806ADBFA3282450527F40BB97CB1DA75FAFE41FB405F1126114187C6393B029`
  - HEX `B80C263E6B07CE976E2BF1AEDED2737EED536C7224369091DC0919CAA48C51AE`
  - MAP `BDC648824C427C873259638A30EB2FAE0C59E1598EBEA3C2C43E0C29CF6516B3`
- `SYSTEM replay = 3f01f6e3`。
- `Long replay = 49dea950`。
- 初始化顺序、输出锁、默认 Feedforward 关闭、默认 Trajectory 关闭、Safety 阈值、H0/H1 周期和 Board1 UPDATE 拒绝行为均未改动。

SHA-256 和 section/resource 未变化，支持“本轮只改变源码可读性、没有改变目标代码或链接布局”的判断；这不等同于真实功率级或实际电机行为已经验证。

## 六、回归结果

- Host Debug：`45/45 PASS`。
- Core：`14/14 PASS`。
- Sensors：`2/2 PASS`。
- Calibration：`1/1 PASS`。
- Runtime/Pipeline：`10/10 PASS`。
- G3-NP：`2/2 PASS`。
- G3-SAFE：`1/1 PASS`。
- Protocol：`1/1 PASS`。
- Board/UART/Python：`4/4 PASS`。
- Clang ASan/UBSan：`35/35 PASS`。Windows CTest 运行时在当前 PowerShell 进程临时加入已有 LLVM runtime 目录后执行，未修改工程或系统环境。
- Legacy Bench：构建通过；Legacy Host Debug 已包含在 `45/45` 回归中。
- C/H formatter：通过；`foc_board.c` 经 clang-format 后通过。
- Source contract：`68 C/H、8 CMake、8 README PASS`。
- CMake formatter：`29/29 PASS`。
- `Scope falconfoc`：PASS。
- `Scope all`：PASS。
- `git diff --check`：PASS（仅保留仓库现有 CRLF/LF 提示）。

## 七、仍未解决的问题与最多三个后续任务

1. 真实 PMSM 参数来源、motor identity/version 与生产 Feedforward 准入仍需独立任务，不应把 Board 名义值当作辨识结果。
2. `main.c` 只有在出现第二个真实 Board、功率提交链或新的启动生命周期证据时，才值得启动 `NEXT-BOOT-ARCH-01`；当前拆分没有已证明收益。
3. 需要在真实 AT32F415 目标上完成 H0/H1 入口延迟、执行时间、栈高水位和故障响应测量；当前只具备静态 ELF 与 Host 证据。

## 八、边界声明

本轮通过的是代码可读性、配置表达和软件回归门禁。它没有证明 `G3-SAFE` 的硬件关闭时延、独立 WDT、桥臂默认状态、AT32F415 WCET 或任何电机实际运行性能。

