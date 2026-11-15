# FalconFoc AT32F415 + DRV8311H 项目总结

> **当前接手入口（2026-11-15）**：请先读
> [AI-handoff-2026-11-15.md](AI-handoff-2026-11-15.md)。本摘要和后续历史章节保留过程证据；
> 当前软件状态以 RC0 基线报告为准：独立源码重建、Host `45/45`、Sanitizer `35/35`、
> Legacy/V2 ARM 构建和 `Scope all` 均通过。真实硬件安全、WCET 和电机连续运行仍未验证。

> 当前工作树：`D:/study/Software/Hollow-Gimbal`
> 历史总结基准：2026-08-30；当前软件基线见 2026-11-15 RC0 证据。
> 文档性质：项目级交接摘要；详细函数、配置和硬件推导分别见本目录其他 Code Wiki。

## 1. 项目目标

本项目是面向 12N14P 云台 BLDC 电机的单轴 FOC 固件，目标 MCU 为 AT32F415，三相栅极
驱动器为 DRV8311H。当前重点不是产品功能扩展，而是解决“低压电压开环不能流畅运行、
转子卡死或跟随失败、软件观测电流偏高”的板级根因定位问题。

当前必须区分三件事：

1. 软件内部的角度推进、SVPWM、保护和诊断路径是否正确；
2. CCR 是否通过 TMR 引脚、DRV8311H 和三相桥形成了正确的实际线电压；
3. MCU ADC/CSA 软件观测是否可信，以及真实绕组/机械系统是否正常。

第一项可以由源码、主机测试和板上 trace 部分证明；第二、三项不能由主机测试或软件 trace
替代。尤其是 CCR 寄存器正确不等于 MCU 引脚、DRV 输出和电机端线电压正确。

## 2. 当前状态

| 状态项 | 当前值 | 含义 |
| --- | --- | --- |
| `OPEN_LOOP_MATH_AND_DUTY_PATH` | `PASS` | 角度 ramp、反 Park、SVPWM、VBUS 防护和 duty 有界性已通过 |
| `OPEN_LOOP_ENCODER_ROLE` | `ENTRY_SEED_ONLY_NOT_FEEDBACK` | 编码器只影响一次性开环种子；运行后不参与持续控制 |
| `ENCODER_CALIBRATION_FRAME` | `UNVERIFIED` | 持久化 `phase_order/dir/offset/LUT` 与当前 bench 坐标系的语义兼容性未证明 |
| `TIMER_PIN_DRV_OUTPUT_PATH` | `UNVERIFIED` | CCR 正确，六路 MCU 引脚、DRV 输入和半桥物理时序未验证 |
| `PHYSICAL_LINE_TO_LINE_VOLTAGE` | `UNVERIFIED` | U-V/V-W/W-U 的平衡、相位和连续旋转未验证 |
| `MOTOR_PHYSICAL_OPERATION` | `UNVERIFIED` | 没有独立相电流、PWM/SOx 波形、机械测速和温度证据 |
| `OPEN_LOOP_ROOT_CAUSE_STATUS` | `UNRESOLVED` | 已缩小候选范围，尚未形成唯一且重复修复的物理根因 |
| `REAL_BENCH_TEST_STATUS` | `BLOCKED` | 不允许据短脉冲结果宣称产品级连续运行 |
| `ADC_MEASUREMENT_WINDOW` | `REPRODUCIBLY_TRIGGER_DEPENDENT` | CH4 compare 改变会稳定改变 MCU ADC/CSA 软件读数 |
| `PHASE_OFFSET_CANDIDATE` | `REJECTED_AS_UNREPEATED` | theta0 矩阵没有同条件可重复唯一优胜 |
| `ENCODER_DIRECTION_CANDIDATE` | `SUPPORTED_NOT_CONFIRMED` | 方向 A/B 有差异，但未在固定机械起始角度下确认 |

## 3. 证据标签规则

- `[源码确认]`：当前工作树源码或构建文件直接给出的事实。
- `[原理图确认]`：`docs/pcb docs/` 中原理图、网表或芯片资料明确支持的连接。
- `[静态推导]`：由时钟、寄存器、公式和调用链推导出的结论。
- `[无板验证]`：主机测试、CTest、静态契约或固件构建实际通过。
- `[实板测量]`：当前板通过 COM3/SWD 或板上 ADC/trace 返回的现场数据。
- `[实板未验证]`：需要独立示波器、相电流、机械测速、温度或重复物理条件才能确认的内容。

## 4. 总体架构

```text
AT32F415 generated main
  -> 时钟/GPIO/ADC/DMA/USART/SPI/TMR/NVIC 初始化
  -> applications/app_main.c
  -> bsp_init()
  -> export pre-kernel / post-kernel bring-up
  -> motor_task_init()
       -> motor_runtime_init()/attach()
       -> shell_task_init()
       -> motor_task / motor_safe_task / vofa_task
  -> osKernelStart()
```

层级职责如下：

| 层 | 目录 | 职责边界 |
| --- | --- | --- |
| 平台 | `platform/at/at32f415/` | 厂商启动、链接脚本、生成外设初始化、MCU 头文件 |
| 板级 | `bsp/board/` | 板级事实、最小启动胶水、Flash 后端 |
| 薄端口 | `bsp/ports/` | 控制面串口、复位等功能型端口；不包裹 FOC 热路径 |
| 产品应用 | `applications/` | FOC、运行时策略、FSM、校准门禁、Shell、保护和参数策略 |
| 可复用组件 | `components/` | PID、滤波、协议、通用工具和 VOFA 组件 |
| 框架机制 | `subsys/` | RTOS/任务、Shell、日志、时间和导出机制 |

FOC 热路径允许直接使用 AT32F415 厂商寄存器、生成代码和硬件耦合函数；软任务和控制
面硬件访问保持薄端口，不重新引入完整 `device/` / `drivers/` 运行时。

## 5. 实时数据链

```text
TMR1 中心对齐 PWM/CH4 ADC trigger 约 20 kHz
  -> ADC1_IRQHandler()
  -> adc_read_phases()
  -> 快速 fault 检查
  -> bench 每 4 个 ADC tick / 产品按当前配置执行 FOC
  -> CTRL_MODE_VOLT: theta_sim + vd/vq（电流只观测/保护）
     或闭环模式: Clarke/Park -> LPF -> Id/Iq PI -> 解耦
  -> 反 Park -> SVPWM -> A/B/C duty -> TMR1 CH3/CH2/CH1

TMR5 soft IRQ
  -> encoder_update() -> SPI1 读取编码器
  -> foc_outer_loop_step() -> 速度/位置环

motor_task 1 ms
  -> FSM、输入模式、故障收敛、校准状态

motor_safe_task 10 ms
  -> 延迟参数保存、受限诊断/遥测
```

当前 bench 镜像的 FOC 计算约为 5 kHz，`PWM_DT=200 us`；产品数学基准为 10 kHz。
这两个概念不能和 TMR1 约 20 kHz 的载波频率混为一谈。

## 6. 主要模块和关键文件

| 模块 | 关键文件 | 主要职责和调用边界 |
| --- | --- | --- |
| FOC 数学 | `applications/algo/foc_core.c`、`foc_math.h` | Clarke/反 Clarke、Park/反 Park、SVPWM；由 FOC 控制层调用，不处理 Shell/Flash |
| FOC 参数 | `applications/algo/foc_config.h` | 采样时基、电机参数、保护包线和默认控制参数的事实入口 |
| 电流/PWM 驱动 | `applications/periph/motor_drv.c/.h` | ADC raw 到 IA/IB/IC、电流符号/offset、VBUS 检查、ABC 到 TMR1 通道映射、PWM enable/disable |
| FOC 控制 | `applications/periph/foc_ctrl.c/.h` | 电流环、纯电压模式、I-f/闭环入口、角度推进和 SVPWM 调用；属于热路径 |
| 编码器 | `applications/periph/encoder.c/.h` | SPI1 编码器读取、机械/电角度和方向；当前 SPI 读取位于 soft IRQ，需关注阻塞边界 |
| 运行时 | `applications/runtime/motor_runtime.c/.h` | 参数加载、控制模式入口、IDLE-only 诊断写入、停机快照和 bench 入口 |
| 参数存储 | `applications/runtime/motor_store.c/.h` | 保存/恢复电机和编码器 payload；当前编码器校准来源只校验结构版本与 CRC |
| 运动命令 | `applications/runtime/foc_motion_runtime.c/.h` | 电压/相电压/I-f 入口、theta seed、速度 ramp 和运行时命令边界 |
| 故障 | `applications/runtime/motor_fault_runtime.c/.h` | 快速/任务级保护、first-fault context、bench trace；ISR 只做常数时间 RAM 操作 |
| FSM | `applications/tasks/motor_fsm.c/.h` | IDLE/RUNNING/FAULT/CALIB 状态迁移；带电校准门禁在这里生效 |
| FOC 任务 | `applications/tasks/motor_task.c` | ADC 回调、FOC 调度分频、任务状态和安全停机调用 |
| Shell | `applications/shell/app_shell_cmd_*.c` | 只读诊断、受控 bench 命令和停机后 trace 导出，不应阻塞保护路径 |
| 外设事实 | `platform/at/at32f415/generated/src/wk_*.c` | 生成时钟、ADC、TMR、SPI、USART 等初始化；重配置后应重新跑静态契约 |

## 7. Board1/DRV8311H 硬件事实

### 7.1 供电和模式

```text
DCBUS -> DRV8311H VIN_AVDD/VM
      -> DRV8311H AVDD LDO -> DRV_3V3
      -> AT32F415、编码器、CSAREF、上拉网络
```

`[原理图确认]` U4 为 DRV8311H 硬件接口版本，没有可由 MCU 读取的驱动器 SPI 配置面。
MODE、GAIN、SLEW 等由电阻 strap 决定：当前 47 kΩ 下拉对应 6x PWM、约 75 V/us、
`0.5 V/A` 电流检测增益，OCP 典型值约 5 A。驱动器内部保护不能替代软件和人工断电。

### 7.2 MCU 映射

| 功能 | 当前事实 |
| --- | --- |
| 三相 PWM | 软件 A→TMR1 CH3，B→CH2，C→CH1 |
| ADC 注入组 | rank1 VBUS=CH1，rank2 IC=CH4，rank3 IB=CH5，rank4 IA=CH6 |
| 电流输出 | DRV SOA/SOB/SOC 经板级 RC/CSA 路径进入 ADC |
| VBUS | PA1，原理图分压 100 kΩ/10 kΩ |
| 编码器 SPI1 | PB3/PB4/PB5，SPI mode 3，16-bit，MSB first，软件片选 PA15 |
| TMR1 | 中心对齐，ARR=3749，CH4 为 ADC 触发，默认 CCR4=3740 |
| 保护脚 | `nFAULT` 没有 MCU 端点；`nSLEEP` 由板级网络自动拉高，不是 MCU 可控 API |
| PWM 安全配置 | TMR1 deadtime=0（MCU 无额外死区）、Break 未启用、自动输出未启用；DRV 内部仍有 VGS 握手和约 450–760 ns 输出死区，物理重叠仍未验证 |

### 7.3 不能越权解释的内容

`[实板未验证]` 板上软件 trace 不能证明真实 INH/INL 波形、死区、SOx 建立时间、
绕组相电流、DRV 热稳定、机械转速或 EMI 条件。12 V/1 A 只限制电源输出能力，不是
独立相电流保护，也不是人工急停。

`[原理图/网表确认]` MCU 到 DRV 的六路 INH/INL 网络有明确两端连接，但网表中的
`OUTA/OUTB/OUTC` 每个只列出 U4 自身引脚，没有列出电机连接器或焊盘组件端点。这不能
直接证明实板断路，却意味着设计资料没有闭合证明 DRV 输出到电机端的连续性和顺序。

`[源码/手册确认]` 当前 TMR1 中心对齐 PWM 还启用了 `IS2` 从模式 RESET，同一配置中的
TMR3 overflow 是其同步来源。该实现与同源参考树的独立 TMR1 配置不同；CCR 正确仍不能
替代六路实际引脚波形证明。

## 8. 保护和校准策略

当前产品/bench 共用的主要数值：

```text
FOC_FAST_CURRENT_TRIP_A = 1.00 A    每相快速绝对值保护
FOC_WARN_CURRENT_A      = 0.75 A    任务级持续高电流包线
FOC_STALL_CURRENT_A     = 0.75 A    仅非电压模式堵转后备
FOC_STALL_SPEED_RPM     = 5 rpm     仅闭环堵转判据，不是开环目标速度
FOC_STALL_TRIP_MS       = 200 ms
FOC_WARNING_TRIP_MS      = 3000 ms  最大容忍边界，不要求等待到期
FOC_ENERGIZED_CALIB_ENABLE = 0      bring-up 默认禁止带电电机/编码器校准
```

`[源码确认][实板测量]` 禁止新校准不等于忽略旧校准。板上曾读到
`enc_valid=1、mech_offset=-0.124 rad、pole_pairs=7`，启动仍会恢复 Flash 中的
`phase_order/dir/mech_offset/LUT`。该 payload 当前没有电机/板号、PWM map 或校准算法来源绑定；
bench map/方向 override 还可能在运行时拆开这一耦合元组。

这仍是进入编码器闭环前必须修复的软件风险，但不是纯电压开环持续不转的首要直接根因：
`CTRL_MODE_VOLT` 只在命令入口用编码器角度播种一次，随后 `theta_sim` 独立推进；固定相矢量
完全绕过编码器。

电压开环测试的有效速度目标不能使用 5 rpm 作为“能否旋转”的目标。电机为 7 对极：

```text
we_elec = 2π × 7 × rpm_mech / 60
30 rpm  -> 21.9911 rad/s
60 rpm  -> 43.9823 rad/s
```

## 9. 验证和运行入口

无板门禁：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/flows/host/run_host_tests.ps1 -RepoRoot D:/study/Software/Hollow-Gimbal
ctest --preset test-host-debug --output-on-failure
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/flows/host/check_foc_peripheral_contract.ps1 -RepoRoot D:/study/Software/Hollow-Gimbal
```

当前已记录的无板结果：`HOST_TESTS=38/38 PASS`、`CTEST=38/38 PASS`、
`FOC_PERIPHERAL_CONTRACT=PASS`、`BENCH_MATRIX_CONTRACT=PASS`；bench50/200/400、
full-diag、standard、tiny 均构建成功，Flash/RAM 未超过链接脚本限制。

当前实机 bench400 镜像：

```text
ELF: build/fw-full-diag-carrier-bench-400ms-debug/output/falcon-foc-at32.elf
SHA256: 59B78D648511926F8AD47213CA35E49C0D536697BE814B1B77AF64F84E949A03
TM_BENCH_DIAG=1
TM_FEATURE_VOFA=0
FOC_ENERGIZED_CALIB_ENABLE=0
TM_MOTOR_BENCH_RUN_BUDGET_MS=400
```

实机命令只允许使用 bench 白名单、单变量、短窗口和停机后读取。禁止在本问题阶段执行
带电校准、`encoff` 搜索、I-f、速度闭环、3 s 连续运行或任意产品启动命令。

## 10. 当前主要风险和下一步

1. `[最高优先级]` 正确的软件马鞍波和 CCR 尚未被证明等价于电机端正确三相线电压。当前最强
   直接根因类别是 CCR 之后的缺相/输出不对称：TMR 引脚、DRV 输入/半桥、PCB/焊接、电机引线
   或绕组均在边界内，现有日志不能细分。
2. `[高优先级]` 当前 TMR1 由 TMR3 overflow 通过 IS2 复位同步，与同源参考树不同。该差异可能
   影响真实引脚周期和互补时序，但尚未证明为具体根因，不能直接照搬参考配置。
3. `[高优先级]` ADC/CSA 软件观测对 CH4 compare 有可重复依赖；必须联合分析中心对齐 PWM
   状态、低侧导通窗口、DRV CSA 建立时间和四 rank 顺序。该问题会破坏电流可信度，但纯
   `CTRL_MODE_VOLT` 的 duty 不由电流 PI 生成，不能把它单独写成开环不转的直接根因。
4. `[启动风险]` 当前开环在播种后立即加速，没有独立的无编码器静态对齐保持阶段。只有实际三相
   输出已证明正确时，才应把根因转向对齐、拉入斜坡、摩擦和机械负载。
5. `[闭环前风险]` 编码器 payload 来源和 offset 符号仍需端到端不变量验证，但不能再覆盖当前
   纯开环功率输出问题，也不能直接取反 offset 或清空 Flash。
6. `[结构风险]` TMR1 未增加 MCU deadtime、Break 关闭、`nFAULT` 无 MCU 端点、`nSLEEP`
   不可控；DRV 内部死区削弱“零死区必然直通”的推断，但仍不构成产品级安全闭环。
7. `[流程风险]` `state disable` 的安全状态机结果可能显示 `axis=fault` 但 fault code 为
   0；这是当前停机语义，不应误判为 DRV 过流。每项测试结束后应 SWD reset 回到 IDLE。

原始实测索引：

- [`完整矩阵汇总`](../../tmp/bench/20260830_103200_full_hardware_root_cause/result.md)
- [`静态 trigger 隔离`](../../tmp/bench/20260830_105417_static_trigger_boundary_1V/)
- [`固定矢量电压梯度`](../../tmp/bench/20260830_111200_static_voltage_gradient/)
- [`2 V trigger A/B`](../../tmp/bench/20260830_112000_dynamic_trigger_compare_2V_30rpm/)
- [`2.5 V 电压窗口`](../../tmp/bench/20260830_112600_voltage_2p5V_30rpm/)
- [`既有证据深度根因分析`](open-loop-root-cause-deep-analysis.md)
