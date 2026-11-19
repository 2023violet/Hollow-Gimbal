# ARCH-001｜FalconFOC V2 实时执行架构与变更规则

- **日期** 2026-11-19
- **状态** 架构变更规则生效。方案 B 为 V2 Reference Firmware 的评审基线。硬件路径与性能参数待验收。
- **适用范围** FalconFOC V2 的 Core、Runtime 和 Reference Firmware 边界。Legacy 实测作为有独立出处的历史证据。 [项目纲领](FalconFOC_Project_Charter_and_Engineering_Memory_v1.0.md)

## 决定与证据

**FalconFOC 的架构必须由产品目标、成熟工程方法和可验证证据共同决定，不能随着每一次讨论的方向反复变化。**

产品目标确定应满足的接口、控制品质和安全约束。成熟工程提供可比较的实现和验收方法。可复现证据决定取舍及适用范围。评审应明确当前实现、已采纳的架构方向、候选参数和待证假设。新讨论可以提出验证任务，变更现役裁决走下方的 Architecture Change Gate。

| 决策层 | 当前裁决 |
| --- | --- |
| 产品架构 | 保留独立的纯 C11 Core，供第三方在自己的执行环境集成。Core 不引入强制 RTOS、板卡或厂商依赖。Reference Firmware 和 Tools & Protocol 保有各自边界。 |
| 控制执行 | 采用方案 B 作为基线。保留 H0/H1/Foreground 的状态所有权与既有 Runtime、Safety 事务，以 PWM 周期驱动的采样到输出时间链作为 Reference Firmware 的验证目标。物理时间约束优先于调度框架偏好。 |
| 硬件实现 | Board Port 管理 AT32F415 的触发、采样窗口、SPI、IRQ、计时与输出生效。定时器和 IRQ 数量、触发路径及 IRQ/DMA 取舍由测量和外设约束决定。 |

| 执行域 | 所有权 |
| --- | --- |
| H0 | 消费带源时间和身份的电流观测，执行角度对齐、Current PI、SVPWM 与最终输出 Guard。H0 不阻塞等待 SPI 或 Foreground。 |
| H1 | 管理编码器采样和连续机械状态，运行已选外环并发布有时间和会话身份的目标。 |
| Foreground | 负责监督、UART V1、配置、诊断和标定编排，经现有命令入口提交请求。现有 `sched` 可作为后台周期服务候选，等待可证明的需求。它不参与 H0/H1 调度。 |
| 物理保护 | 目标板须能在 H0 停止或超期时完成必要关断。Break、Gate Disable、Fault 和 Watchdog 的有效性需专项实测。 |

TI C2000 FCL 和 MSPM0 的参考实现用于对照采样至 PWM 生效的延迟、采样拓扑与验收方法。实际工程对照须写明芯片、采样配置、调制和专用外设能力。[TI C2000 FCL 参考设计](https://www.ti.com/tool/TIDM-02007) · [TI MSPM0 Sensored FOC 测量接口](https://software-dl.ti.com/msp430/esd/MSPM0-SDK/2_09_00_01/docs/english/middleware/motor_control_pmsm_sensored_foc/doxygen/mspm0g3507-doc-srcs-api-guide/html/measure_8h.html)

## 基线与待验收项

- **V2 源码基线 `34417392`。** Board1 锁定参考镜像中，TMR1 CH4 比较 IRQ 发起软件触发的 ADC 抢占组转换，ADC 完成进入 H0。TMR2 提供微秒时基，TMR5/SPI1 负责编码器时序，USART1 提供命令链路。当前无已验收的真实 PWM 生效时间。[Board1 合同](../../firmware/at32f415/README.md) · [V2 固定提交](https://github.com/2023violet/Hollow-Gimbal/commit/34417392cd94932058e659442881a4bcc73eb695)
- **Legacy 反例 `a03ee4b`。** 该配置以 20 kHz 中心对齐 PWM 产生约 40 kHz 候选边沿。ADC 服务后重新武装使空闲时每两个边沿采样一次，运行态约每三个边沿一次，记录约 13.35 kHz。它用于检验 V2 的漏触发检测能力，V2 需独立取证。[Legacy 实测记录](https://github.com/2023violet/Hollow-Gimbal/commit/a03ee4b758ab0c14de4d93a88ca6e0623aefccbd)
- **性能候选。** `20/20 kHz`（PWM/电流环）优先验证，`10/10 kHz` 是备选。采样窗口、ADC 完成至 PWM 装载的截止期、传感器年龄、H0/H1 抢占、目标板 WCET 和栈余量仍待证。Host、目标 MCU 锁定测量、实机闭环与功率级安全分别验收。

先用 `NEXT-ARCH-00` 建立可失败的正常采样与 Legacy 式漏边沿反例，并核对 AT32F415 硬件触发路由和勘误。随后按讨论路线推进 RT-01（时间链）、RT-02（抢占与 WCET）、INTEG-01（复用 SOFT-01～06 完成 Host 用户链）、RT-03（锁定板端测量）、PRODUCT-01（经物理安全验收与授权的实机闭环）。此处记录评审顺序，各阶段按自己的证据和授权推进。

## Architecture Change Gate

适用于 ISR、Timer、ADC/PWM 时序、H0/H1/Foreground 所有权、Safety 提交边界及公共 Runtime 契约的重大调整。开始接口或实现变更前，在 ADR 中记录下列信息。原 ADR 保留历史，由新决定显式标注替代关系。

1. **问题与指标。** 写明现役决定、具体缺口或收益、受影响的合同与量化验收阈值。
2. **证据与反例。** 固定代码提交、配置、工具链、负载和复现输入，注明是源码、厂商参考、Host、锁定板端或实机证据，并列出仍待验证的假设。
3. **成熟方案。** 记录可采纳的工程方法、选型约束和许可条件。对照保持同一控制律、输入、有效频率、采样和装载时刻、输出 Guard、故障场景及测量口径。
4. **决策与复验。** 记录日期、范围、选择、理由和批准依据。覆盖时间、资源、公开接口、状态事务、Safety 与用户使用链；写清回退阈值及可恢复的已验证基线。

`KEEP` 是正式结论。具体局部缺口可裁决为 `IMPROVE`。`REPLACE` 需要公平对照证明替代方案解决了明确缺口，或在守住既有合同的前提下产生足以承担迁移成本的净收益。缺少关键证据或授权时记录 `BLOCKED` 及解除条件。若实证指出现有 H0/H1 跨域成本无法满足截止期而集中执行可以满足，则复审方案 A。若两种执行结构都受限于 AT32F415 的采样或计算能力，则复审方案 C（频率或参考 MCU 选型）。物理关断证据不足时，功率输出集成保持阻断。
