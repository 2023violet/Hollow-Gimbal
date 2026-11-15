# FalconFoc AT32F415 Code Wiki

> 代码级知识地图、硬件配置审计与无板验证报告
>
> 当前基准：提交 `278c6911e65d7782bb58d7b0b023fdcb9f414c08`，分支 `codex/falconfoc-v2-rc0`，日期：2026-11-15。
>
> 当前交接入口：[AI-handoff-2026-11-15.md](AI-handoff-2026-11-15.md)。本文件保留较早的代码级和板级分析；与当前代码、RC0 基线证据冲突时，以当前提交、测试和基线报告为准。

## 1. 文档定位

这套 Wiki 用于回答五个问题：

1. 系统由哪些层和模块组成，启动与实时数据如何流动；
2. 关键类型、函数和命令由谁拥有，调用边界在哪里；
3. 构建、运行、参数保存和主机模拟测试如何执行；
4. Board1 原理图如何连接，DRV8311H 的硬件模式、电流采样和保护边界是什么；
5. 当前代码有哪些已经验证的事实、迁移遗留和需要继续处理的风险。

本 Wiki 只描述当前源码能证明的行为。它不替代产品需求、芯片数据手册或实板验收记录。

## 2. 范围与资料优先级

### 2.1 范围

- 当前产品边界：FalconFoc 单轴 FOC 固件；
- 当前源码平台：`platform/at/at32f415`，CPU 类：`cortex-m3-soft`；
- 当前构建系统：CMake + Ninja，固件使用 Arm GNU Toolchain；
- 当前实时框架：FreeRTOS CMSIS-RTOS2 适配层；
- 无板模拟：只使用主机编译、链接、CTest、源码级契约测试和 Board1/DRV8311H 数学模型；该轨道不证明真实电机行为。
- 实板诊断：另有 bench 固件下的低压、毫秒级、单变量短脉冲记录；它不等价于产品级连续运行验收。

### 2.2 证据标签

| 标签 | 含义 |
| --- | --- |
| `[现状]` | 当前工作树源码、配置或构建文件直接支持的事实 |
| `[基线]` | 正式架构/治理文档中的设计基线；若与当前源码冲突，不能当作当前实现证明 |
| `[验证]` | 本轮实际执行的命令及其输出 |
| `[推断]` | 根据调用链、配置或数学关系得到的分析结论，需要后续专项验证 |
| `[未验证]` | 需要实板、示波器、串口或真实负载，已被本任务明确排除 |
| `[实板测量]` | 当前板通过 COM3/SWD 或板上 ADC 返回的现场数据 |
| `[实板未验证]` | 当前没有独立仪器或重复试验支持的物理结论 |
| `[无板验证]` | 主机/CTest/静态契约实际通过的结果 |
| `[静态推导]` | 由源码、原理图、时钟和公式推导的结果 |

无板验证仍是软件回归主门禁；实板轨道只使用已经核验的 bench ELF，并受能量预算、心跳和快速保护约束。历史上已执行过受控 raw-PWM/I-f 低速运行，但带电校准仍禁止，闭环 30/60/90 rpm 仍未获准。产品级准入由独立的 `REAL_BENCH_TEST_STATUS` 控制，不能用 bench 运行数据替代。

### 2.3 资料优先级

1. 当前 `platform/at/at32f415`、`applications`、`bsp`、`subsys`、`components` 源码与 CMake；
2. 当前 `CMakePresets.json`、`run.sh`、`scripts/` 和 `tests/`；
3. `README.md` 与 `docs/` 中的正式设计文档，作为设计意图和迁移基线；
4. 维护日志中的历史板级事实，只作为历史证据，不能替代本轮无板验证。

当前资料存在明确迁移差异：正式文档多处仍写 `AT32F403A`，而当前源码、预设和本轮固件构建使用 `AT32F415`。因此本 Wiki 以当前 `AT32F415` 工作树为准，并在相关章节显式标出差异。

## 3. 导航

| 文档 | 内容 |
| --- | --- |
| [session-memory-2026-08-27-28.md](session-memory-2026-08-27-28.md) | 昨晚调试会话的显式记忆、关键决策、实现结果和后续交接入口 |
| [architecture.md](architecture.md) | 分层架构、启动链、实时调度、硬件映射和调用边界 |
| [modules.md](modules.md) | 目录/模块职责、关键类型与 API、Shell 命令、第三方选用范围 |
| [dependencies.md](dependencies.md) | CMake target、源码调用边、控制/存储数据流和依赖关系 |
| [hardware-and-drv8311h.md](hardware-and-drv8311h.md) | Board1 原理图/网表、AT32F415 硬件映射、DRV8311H 深度分析与无板电气模型 |
| [configuration-audit.md](configuration-audit.md) | 原理图—外设—调用时序的逐项配置审计、确定性问题和无板模拟测试矩阵 |
| [motor-debugging-safety-and-procedure.md](motor-debugging-safety-and-procedure.md) | 实机调试安全协议、快速停机边界、停机后日志分析与分阶段开环排障流程 |
| [motor-real-bench-test-plan.md](motor-real-bench-test-plan.md) | 带门禁的实机测试步骤、低能量参数包线、命令顺序、停机判据和失败回退决策树 |
| [operations-and-validation.md](operations-and-validation.md) | 构建运行方式、无板模拟测试矩阵和本轮实际结果 |
| [deep-analysis.md](deep-analysis.md) | 深度剖析、风险分级、行为模拟、迁移缺口与后续建议 |
| [open-loop-root-cause-deep-analysis.md](open-loop-root-cause-deep-analysis.md) | 停止新增测试后，基于同镜像实板证据完成的低压开环因果分层、反证、根因排序与结论边界 |
| [project-summary.md](project-summary.md) | 项目级架构、模块、硬件、运行方式和当前状态总结 |
| [debugging-summary.md](debugging-summary.md) | 低压开环卡死问题的实机证据、推导、候选根因和安全收口 |
| [ai-handoff-prompt.md](ai-handoff-prompt.md) | 2026-08-30 版交接提示词（历史） |
| [AI-handoff-2026-09-26.md](AI-handoff-2026-09-26.md) | **现役交接入口：根因已确认（AVDD 供电压垮）+ 修复清单 + 验证阶梯** |
| [AI-handoff-2026-11-15.md](AI-handoff-2026-11-15.md) | **当前软件 RC0 交接入口：提交、架构边界、门禁和未验证项** |
| [AI-handoff-2026-08-31.md](AI-handoff-2026-08-31.md) | 历史：攻坚全史（电流链确诊/±90° 推理/隔离镜像），被 09-26 版取代 |
| [../planning/Themis_开环根因延申验证与修复计划.md](../planning/Themis_开环根因延申验证与修复计划.md) | 本轮根因机制推导、实机确诊与修复的唯一计划入口（含预测表与结果回填区） |

## 4. 一页摘要

### 4.1 当前实现主链

```text
AT32F415 generated main
  -> wk_system/periph/debug/nvic/timebase/gpio/ADC/DMA/USART/SPI/TMR init
  -> app_main_bootstrap()
  -> bsp_init()
  -> pre-kernel export range
  -> osKernelInitialize()
  -> post-kernel export range
  -> motor_task_init()
       -> shell_task_init() [TM_SHELL_ENABLE=1 时]
       -> motor_runtime_init()/attach()
       -> motor_task + motor_safe_task
  -> osKernelStart()
```

### 4.2 实时链

```text
TMR1 PWM/ADC trigger (~20 kHz)
  -> ADC1_IRQHandler()
  -> adc_read_phases()
  -> 每两个 ADC tick 执行一次
  -> foc_current_loop_step()
  -> Clarke/Park -> Id/Iq PID -> 解耦 -> 反 Park -> SVPWM -> PWM duty

TMR5 overflow (当前平台选择的 motor soft IRQ)
  -> TMR5_GLOBAL_IRQHandler()
  -> encoder_update() [当前为阻塞 SPI 读取]
  -> foc_outer_loop_step()
  -> 速度/位置级联 PID -> iq_set

motor_task (RTOS, 1 ms)
  -> FSM、故障检查入口、输入模式/轨迹、校准完成和状态收敛

motor_safe_task (RTOS, 10 ms)
  -> 延迟参数保存、VOFA JustFloat 遥测
```

### 4.3 本轮验证摘要

| 项目 | 结果 | 解释 |
| --- | --- | --- |
| AT32F415 固件独立构建 | 通过 | 当前 bench50/200/400 FLASH 86,728/131,072 B（66.17%）、RAM 27,480/32,768 B（83.86%）；full-diag 为 77,400/21,808 B，standard 为 39,876/20,072 B，tiny 为 36,668/18,336 B |
| 主机全量构建与执行 | 45/45 通过 | CMake 配置、45 个 host target 编译和 CTest 均通过；详见 [operations-and-validation.md](operations-and-validation.md) |
| 保护/校准/VBUS 主机契约 | 通过 | `test_motor_fault_runtime_contract`、`test_foc_calib_curr_contract`、`test_foc_vbus_guard_contract` 均通过 |
| FOC 时基/电流闭环参数模型 | 通过 | `test_foc_bringup_model_contract` 已验证 10 kHz FOC step、1250/250=5 LPF 比例、ADC 比例和小电流阶跃边界；不等价于实机稳定性 |
| 历史实板只读启动复核 | 仅作历史记录 | 早期记录包含 OpenOCD verify、SWD IDLE/fault/VBUS/offset 和安全关闭 PWM；不属于本轮无板证据，也不等于驱动验收 |
| 现有板受控短脉冲 | 已记录，非产品验收 | bench 固件下 D0-D4/O0-O3 的板上 ADC、VBUS、角度、duty 和故障快照；详见 [operations-and-validation.md](operations-and-validation.md) |
| 实板电机连续运行/波形/热 | 未验证 | 没有独立 PWM 波形、外部相电流、温度和长时间运行证据 |

当前交接入口是 [AI-handoff-2026-09-26.md](AI-handoff-2026-09-26.md)。2026-09-26 深夜：电机攻坚根因确认为 **AVDD 3.3V 供电压垮**（用户万用表实测轨跌至 0.25V；驱动启动后 DRV AVDD UVLO 关断栅驱）。采样链已修复（CCR4=3740 波峰采样 + 软件触发）、SVPWM duty 钳位 [0.28,0.72]、编码器 LUT 标定跑通、闭环首次激活。唯一阻塞项是 AVDD 供电硬件修复（VIN_AVDD 改 5V），随后进入闭环整定。历史记录见 08-31 交接文档与维护日志。

具体命令、完整输出摘要和每个测试场景见 [operations-and-validation.md](operations-and-validation.md)。

### 4.4 Board1/DRV8311H 摘要

| 项目 | 当前结论 |
| --- | --- |
| U4 | `DRV8311HRRWR`，H 硬件接口版本，无 DRV SPI 寄存器面 |
| 硬件模式 | R11/R12/R13=47 kΩ 下拉，解码为 6x PWM、约 75 V/μs、0.5 V/A、OCP 约 5 A typ |
| 供电 | `VIN_AVDD` 与 `VM` 都接 `DCBUS`；U4 `AVDD` 生成 `DRV_3V3` 并供 U3/U5/CSAREF |
| 采样 | SOA/SOB/SOC 经 330 Ω/22 pF 至 U3；3.3 V、0.5 V/A 下 nominal CSA 线性范围约 ±2.8 A |
| 故障边界 | `nFAULT`、`nSLEEP` 都没有 MCU 端点；U4 硬件保护仍存在，但软件故障观测/联动不闭环 |
| 静态发现 | 当前 R6/R7/D2 网络不能把 `nFAULT=0` 拉到 `nSLEEP` 低电平；高母线下还需审查 AVDD LDO 热耗散 |

完整引脚表、保护行为、时序、热/布局边界和无板测试结果见 [hardware-and-drv8311h.md](hardware-and-drv8311h.md)。

## 5. 关键结论

- `[现状]` 分层边界总体清楚：平台负责芯片生成代码，BSP 负责板级事实和薄端口，应用层拥有 FOC 产品语义，`subsys` 提供共享机制；当前没有重新引入完整 `device/` / `drivers/` 运行时。
- `[现状]` FOC 高频路径没有被强行抽象进通用 HAL：ADC/PWM/编码器使用平台和驱动边界，符合当前软浮点 MCU 的实时约束取向。
- `[现状]` Flash 写入通过 `param_save_req` 延迟到安全任务，BSP Flash 层要求任务上下文并在 vendor 操作期间关中断，避免 ISR 直接擦写。
- `[已修复/仍需硬件证明]` ADC ISR 现在在采样后、FOC 计算前执行常数时间 `foc_fault_check_fast()`；严重电流、VBUS、速度、温度或非法数据先锁存 fault 并关闭六路 PWM，任务级 `foc_fault_check()` 再处理 3 s 持续电流和 200 ms 低速堵转后备规则。该闭环已由 host contract 覆盖，但尚未用示波器证明门极传播延迟。
- `[已修复]` AT32F415 主机测试 include 路径已同步，新增保护/校准/VBUS、故障上下文、运行态诊断门禁和 FOC 参数模型目标可构建并执行；fail-fast、栈预算、FSM 中止和诊断格式 4 个历史契约问题也已处置，当前完整 CTest 已复验为 45/45。
- `[已修复/需持续关注]` 启动电流零点校准现在先关闭桥臂、等待 100 ms，再采集 2000 个 1 ms 样本，并校验 ADC 原始范围和 Welford 标准差；失败锁存 `FAULT_CALIBRATION_FAILED`，成功才恢复零占空待机钳位。
- `[中风险]` DRV8311H 的 `nFAULT`、`nSLEEP` 仍没有 MCU 端点，TIM1 Break 仍未启用，`pwm_out_disable()` 仍不切断 DCBUS；软件快速保护不能替代独立硬件急停和电源断开。
- `[已修正]` 默认 Id/Iq cutoff 已从错误的 25 Hz 修正为 1250 Hz，并绑定 10 kHz FOC step；持久化加载在 LPF 初始化前完成，运行时合法 `currbw` 修改同时刷新 PI 与 LPF，非法/非有限值会被拒绝。
- `[确定性门控]` `FOC_ENERGIZED_CALIB_ENABLE=0U` 是默认 bring-up 配置；电机参数校准和编码器带电校准入口拒绝执行，只保留非带电电流偏置校准、故障查询/清除和只读诊断。

## 7. 2026-08-29 现有板短脉冲诊断摘要

`[实板测量]` 当前 bench ELF 已通过 OpenOCD `Verified OK` 写入板卡。使用 COM3、12 V/1 A
背景电源、20 ms 受控脉冲和约 50 ms ADC 自动能量护栏完成 D0-D4/O0-O3。`vbus` 保持
约 `11.90–11.91 V`；O1/O2/O3 的锁存电流分别约为 `-7/4/3 mA`、`-12/25/5 mA`
和 `-48/60/-2 mA`，没有触发 `FAULT_OVER_CURR`。O1 的 `theta_sim≈0.075 rad`、
`we_volt_now≈3/3.14 rad/s`，说明短脉冲内开环角度推进链已运行。

该结果只说明当前低压短脉冲没有复现高电流卡死；它不证明电机能够连续旋转，也不证明
DRV8311H 的死区、门极重叠、独立故障关断、外部相电流和温升。详细原始记录见
[2026-08-29 实有板结果](../../tmp/bench/20260829_actual/result.md)。

当前状态保持：

```text
OPEN_LOOP_SOFTWARE_PATH=PASS
MOTOR_PHYSICAL_OPERATION=UNVERIFIED
OPEN_LOOP_ROOT_CAUSE_STATUS=UNRESOLVED
REAL_BENCH_TEST_STATUS=BLOCKED
CONTROLLED_DIAGNOSTIC_STATUS=ALLOWED_WITH_MINIMUM_GUARDS
```

## 6. 2026-08-27 当前实现与证据状态

本节是本轮安全 bring-up 后的当前状态，覆盖并更新上方早期审计快照中的“故障检查为空”“主机 include 路径阻塞”等历史描述。

### 6.1 已落地的保护时序

```text
ADC1_IRQHandler
  -> adc_read_phases()
  -> foc_fault_check_fast()       O(1)，不打印/不阻塞/不写 Flash
       -> hard fault: fault_code |= reason; pwm_out_disable(); return
  -> fault_code != 0 ? return : half-div / foc_current_loop_step()

motor_task (1 ms)
  -> foc_fault_check()             任务级兜底
       -> 0.75 A 持续 3000 ms -> FAULT_OVER_CURR
       -> 非电压模式：>=0.75 A 且 <=5 rpm 持续 200 ms -> FAULT_OVER_CURR
  -> motor_fsm 在任务上下文完成 FAULT 状态迁移
```

当前快速过流门限为每相绝对值 `1.00 A`；配置的 `i_peak` 只能把它进一步降低，不能用默认的 `2.0 A` 绕过 bring-up 门限。VBUS 数值防护拒绝 `NaN/Inf/<=1.0 V`，并在无效时清零调制量、关闭 PWM；产品欠压/过压仍由运行时阈值 `7.4/17.0 V` 判定。

### 6.2 启动零点校准边界

`foc_calib_curr()` 的采样窗口为：PWM 六路关闭 → 等待 `100 ms` → 以 `1 ms` 间隔采集 `2000` 个原始 ADC 样本 → 每相均值和标准差验证。均值必须落在 `64..4031`，标准差不得超过 `16 counts`；任一相失败即锁存校准失败并保持 PWM 关闭。全部通过后才写入三相 offset，并恢复零占空 IDLE 钳位。该策略针对现场已复现的“原始 ADC≈2050 但旧 offset 造成假电流安培级”问题。

### 6.3 验证结论边界

- 无板：快速保护、启动偏置校准、VBUS、动态 LPF、故障上下文、运行态诊断门禁、带电校准门禁、开环模型和完整 host flow 均通过；38 个目标全部编译并通过 CTest。fail-fast 测试通过 CMake wrapper 正确表达预期断言，栈预算、FSM 中止和诊断格式契约已与当前实现对齐。
- 固件：`fw-full-diag-carrier-debug`、`fw-standard-carrier-minsizerel`、`fw-tiny-carrier-minsizerel` 均完成无板交叉构建；本轮没有执行 OpenOCD 写入。早期 `Verified OK` 记录仅作为历史证据保存。
- 实板：复位等待启动校准后，SWD 读到 `axis_state=0`、`fault_code=0`、`v_bus≈11.878 V`、三相 offset `≈2051.74/2051.62/2047.53`；随后通过调试器调用同一 `pwm_out_disable()`，300 ms 后仍读到 TIM1 `CCTRL=0x1000`，且轴仍非驱动态。
- 未证明：电机旋转能力、开环同步、门极死区/Break 波形、DRV 热稳定性、独立断电响应时间、完整串口日志链。串口本轮因 VOFA 二进制流导致接收溢出，只能作为通道问题记录，不能作为保护证据。

### 6.4 本计划新增实现

- `foc_lpf_refresh()` 只重算 Id/Iq LPF 系数并保留历史输出；`motor_runtime_init()` 在参数加载完成后初始化 LPF，持久化非法带宽先回退默认值，`currbw` 修改会同时更新 PI 和 LPF。
- `motor_runtime_diag_set_*()` 对 `currbw` 的 `0`、负数、NaN、Inf 请求返回拒绝，不改变当前带宽、PI 或 LPF。
- `FOC_ENERGIZED_CALIB_ENABLE` 默认值为 `0U`。FSM 在进入电机/编码器带电校准动作前拒绝请求，因此不会进入 `AXIS_STATE_CALIB`，不会调用校准准备或打开 PWM；既有“开启宏=1”中止恢复测试继续保留。
- `test_foc_voltage_openloop_model_contract` 使用真实 `foc_current_loop_step()`、`foc_ctrl_volt()` 与 `foc_svpwm()` 验证零速、连续角度、角度回绕、I-f 一致性、VBUS 无效防护、矢量限幅和 duty `[0,1]` 边界；它只证明主机数学路径。
- `scripts/flows/host/check_foc_peripheral_contract.ps1` 读取生成式时钟、NVIC、TMR1、ADC、SPI 以及 `motor_drv.c`/`foc_ctrl.c` 映射事实，成功输出 `FOC_PERIPHERAL_CONTRACT=PASS`，不连接板卡。
- `motor_fault_context` 在快速 trip 关闭 PWM 前锁存首故障的 raw ADC、电流、VBUS、角度、FOC 和 SVPWM 数据；`fault status` 只在停机后导出，`fault clear` 才清除。
- `diag set` 和 runtime 诊断 setter 统一只允许 `IDLE`；运行中写入返回 `result=error reason=idle_only`，不会改变电压、I-f、相电压、带宽、PID 或 encoder offset。
- `fw-full-diag-carrier-bench-debug` 是专用诊断构建入口，保留 Shell、关闭 VOFA、强制关闭带电校准；另有 200 ms/400 ms 受限窗口镜像，构建通过不等于实机准入。

当前状态机和保护结论仍受以下硬件事实限制：DRV8311H `nFAULT/nSLEEP` 当前没有 MCU 端点，TMR1 Break 关闭、deadtime 为 0，`pwm_out_disable()` 不切断 DCBUS。12 V/1 A 只能限制电源输出能力，不能替代物理急停、独立硬件关断或驱动器保护。因此：

```text
REAL_BENCH_TEST_STATUS=BLOCKED
```

## 2026-08-30 当前标准 SVPWM 与实机矩阵收口

本节是当前证据链的最新补充，完整原始记录见
[`tmp/bench/20260830_074936_post_snapshot_fix/result.md`](../../tmp/bench/20260830_074936_post_snapshot_fix/result.md)。

- `[源码确认][无板验证]` `foc_svpwm()` 已统一为矢量保持的标准逆 Clarke/common-mode 实现；主机模型、全量 host/CTest 和外设静态契约通过。
- `[源码确认]` 停机后的 `motor_runtime_snapshot()` 不再沿用最后一次 FOC 的陈旧速度/位置缓存，而是读取当前 encoder PLL 状态，避免把停止后的诊断值误读为真实转速。
- `[实板测量]` 当前 bench400 镜像 `SHA256=25A38A987BE93B48C05526B8A6839F697AE618AACFD740C8517AAA661CA65922` 已显式刷写并通过预检；复位后 `axis=idle`、`fault=0`、`duty=0/0/0`、VBUS 约 `11.91 V`。
- `[实板测量]` 36 组 PWM/ADC 映射组合均完成且没有稳定唯一优胜；一次 map2 的旋转响应在两次重复中未复现，因此默认 map0 不变。
- `[实板测量]` CH4 compare 扫描显示软件电流随采样点明显变化：低 compare 约 `0.01 A`，靠近 `2300..3740` 时约 `0.27 A`；这是低侧 CSA/采样窗口候选证据，不是独立相电流证明。
- `[实板测量]` `2 V / 20..30 rpm` 未建立稳定同步；`3 V / 30 rpm` 一次触碰约 `1 A` 软件快速边界并锁存首故障；`2 V / 15 rpm` 两次表现不一致，不能据此确认速度阈值或单一根因。
- `[实板测量]` 相位偏置扫描没有可重复优胜项，未修改编码器零点、相序或电机参数。

最新状态仍为：

```text
SVPWM_SOFTWARE_MODEL=PASS
ADC_MEASUREMENT_WINDOW=ANOMALOUS_OR_STRONGLY_SENSITIVE
PWM_ADC_MAPPING=NO_UNIQUE_WINNER
PHASE_OFFSET_CANDIDATE=REJECTED
OPEN_LOOP_SYNCHRONIZATION=NOT_DEMONSTRATED
OPEN_LOOP_SOFTWARE_PATH=PASS
MOTOR_PHYSICAL_OPERATION=UNVERIFIED
OPEN_LOOP_ROOT_CAUSE_STATUS=UNRESOLVED
REAL_BENCH_TEST_STATUS=BLOCKED
```

`[实板未验证]` 本轮仍没有人工手动编码器整圈记录、PWM/DRV 引脚示波器、独立相电流或温度数据，不能将软件 trace、短脉冲或构建结果写成产品级电机运行证明。

## 8. 2026-08-30 分层正交诊断实现快照

本节是当前工作树关于下一轮低压开环根因定位能力的规范入口，覆盖前文历史记录中未展开的
bench-only trace、PWM/ADC 映射矩阵、ADC 触发位置扫描和编码器手动轨迹。

### 8.1 已实现的 bench 观测面

- `[源码确认]` FOC 完成一次计算后，在 `TM_BENCH_DIAG=1` 下写入 64 条 RAM-only
  定点采样记录。每条记录包含 TMR1 `CVAL/PR/CCR1..4`、控制寄存器和状态寄存器、ADC
  注入 raw 值、三相 mA、电源 mV、角度/速度、`vd/vq`、三相 duty、sector、轴状态和
  fault code。写满后只保留最新 64 条，非 bench 固件不分配该环形缓冲。
- `[源码确认]` 编码器另有 32 条 RAM-only 轨迹，记录 SPI raw word、修正计数、机械/电
  角度、速度、方向、累计计数和 SPI/PLL 错误标志。该轨迹用于 PWM 禁止时手动正反转扫查。
- `[源码确认]` 诊断命令均有状态边界：`trace clear`、map/trigger 写入只能在 IDLE，
  `trace show/sample` 和 encoder trace 读取只能在停止后执行；运行中读取统一返回
  `result=error reason=stopped_only`。trace 不写 Flash、不在 ISR 打印、不发送串口。

### 8.2 正交实验变量

- `[源码确认]` bench PWM map 0 为默认硬件事实 `A→CH3、B→CH2、C→CH1`，另提供全部
  6 种排列；ADC map 0 为 `A→rank4、B→rank3、C→rank2`，另提供全部 6 种排列。
  映射只保存在 RAM，重启恢复默认，不改变产品 `PHASE_ORDER`、PID、保护阈值或编码器参数。
- `[源码确认]` ADC 触发默认 `TMR1 CH4 compare=3740`，bench 运行时只允许
  `256..3740` 的预设范围；每条 trace 保存 CVAL、计数方向、CH4 compare 和 ADC 状态。
  该变量只能做软件相关性实验，不能替代示波器确认真实采样窗口。
- `[源码确认]` bench 固件固定 `TM_FEATURE_VOFA=0`、`FOC_ENERGIZED_CALIB_ENABLE=0`，
  默认运行预算为 50 ms，并提供只用于后续门禁通过后的 200 ms/400 ms 构建预设；产品镜像不继承
  bench 映射、trace、early-trip 或 budget override。

### 8.3 运行入口与证据边界

推荐先执行只读预检：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File scripts/flows/board/run_bench_matrix.ps1 `
  -RepoRoot D:/study/Software/Hollow-Gimbal `
  -Mode Preflight -PortName COM3
```

需要进行短脉冲时必须显式加入 `-RunPulses`；跨多个脉冲还必须使用
`-WaitForPowerCycle`，因为当前受控 `state disable` 会进入 FAULT，脚本不会发送
`fault clear` 或 `state stop` 来绕过安全边界。脚本只发送白名单命令，不自动控制 PSU。

`[无板验证]` 本轮 host/CTest 均为 `38/38 PASS`，外设静态契约为
`FOC_PERIPHERAL_CONTRACT=PASS`，bench50、bench200、bench400、full-diag、standard、tiny 均构建
通过。`[实板未验证]` 新增矩阵没有因为构建通过而自动获得物理准入；实际互补 PWM 重叠、
死区、ADC 模拟建立时间、独立相电流、DRV8311H 热状态和产品级连续机械运行仍未验证。

本轮当前构建身份为：bench50 ELF SHA256=`A80994B415782E4F3CAB45A59510F9F014984149963FBC88BCAC8F864B21825A`；
bench200 ELF SHA256=`9BB77862DEB793F68ED1D085CB21E5C7933712D1538967821AADA890F5D4DA41`；
bench400 ELF SHA256=`38B6036012D9BCEB5849A5B5260BF75E5C151481FEC9CE8F5F5854563D576285`。
历史实板日志中的 `F040...`、`3969...` 等哈希只属于各自生成时的旧镜像，不能与本轮
新增 trace/mapping 能力混用。

当前状态继续保持：

```text
OPEN_LOOP_SOFTWARE_PATH=PASS
MOTOR_PHYSICAL_OPERATION=UNVERIFIED
OPEN_LOOP_ROOT_CAUSE_STATUS=UNRESOLVED
REAL_BENCH_TEST_STATUS=BLOCKED
```
