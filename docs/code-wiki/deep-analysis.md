# 深度剖析与行为模拟

## 1. 总体判断

当前工程已经形成一条可编译的 AT32F415 产品链：平台生成代码负责芯片启动和外设，`app_main` 负责 export/RTOS 桥接，`applications` 负责 FOC、任务、FSM、Shell 和参数策略，BSP 把 Flash/console 等控制面资源收口。架构方向是清晰的，且固件交叉构建已经证明这些层可以组合到最终 ELF。

首版审计曾识别几个高影响断点：故障检查为空、主机应用测试路径仍指向旧 MCU、控制时基/滤波器注释与数值不一致、主机 Flash 几何不是 F415 几何。本轮已经补齐软件快速故障、启动零点校验、VBUS 数值防护、host include 和剩余 host 契约；硬件故障输入、门极波形和主机 Flash 几何仍未闭环。以下结论区分当前软件证据、实板只读证据和未验证物理行为。

Board1 的电源、DRV8311H 硬件 strap、采样窗口、保护链和无板电气模型单独记录在 [hardware-and-drv8311h.md](hardware-and-drv8311h.md)，本章只引用其对软件行为有直接影响的结论。

## 2. 优点与已经形成的工程能力

### 2.1 责任边界合理

- 平台和 generated code 拥有 MCU/时钟/引脚/中断事实；
- FOC 热路径可直接使用 vendor header 和寄存器相关驱动，没有引入深层通用 HAL；
- BSP 只提供板级 Flash 和薄控制面端口；
- `applications` 拥有产品状态、保存恢复、校准、Shell 和运动策略；
- Shell transport 与 app session 分离，队列有界，输出有统一格式；
- `motor_runtime_snapshot()` 为 Shell/诊断提供只读快照，减少直接触碰实时对象。

### 2.2 实时/非实时边界明确

最重要的安全边界是：校准计算和 FOC 计算可以在 ISR 推进，但 Flash 写入不能在 ISR 完成。编码器校准只置 `PARAM_SAVE_ENC`，由 `motor_safe_task` 消费；`bsp_flash` 再要求 scheduler 已运行且不在 ISR。这种拆分能避免长时间擦写阻塞控制中断。

### 2.3 存储机制具备抗掉电设计雏形

ENC/MOTOR/SYS 分组、A/B slot、magic/version/sequence/CRC 以及“相同内容跳过写入”已经形成可审查的协议骨架。应用 Shell 也将 `store save` 明确表达为 deferred，而不是假装命令返回就代表 Flash 已写完。

## 3. 高优先级风险

### 3.1 `[高/软件已补齐，硬件仍有缺口]` 故障检查与快速停机

当前证据链已经变为：

```text
ADC1_IRQHandler
  -> adc_read_phases()
  -> foc_fault_check_fast()       常数时间、无日志/等待/Flash
  -> hard fault: fault latch + pwm_out_disable() + return
  -> 无故障才继续 FOC 计算

motor_task 每 1 ms
  -> foc_fault_check()             dq/母线/速度/温度/持续规则兜底
  -> motor_fsm 在任务上下文收敛到 FAULT

tests/host/applications
  -> fault contract PASS
```

现在软件路径能够对相电流、VBUS、速度、温度和非法数据做快速 trip；任务级路径另外覆盖 `0.75 A/3 s` 持续告警和仅非电压模式下的 `0.75 A/5 rpm/200 ms` 堵转。快速路径先锁存首次 `motor_fault_context`，再锁存故障并关闭六路 PWM，不直接写 `axis_state`，避免 ISR 与 FSM 双重拥有状态。上下文仅保存到 RAM，Shell 只能在停机后读取，重复故障不会覆盖第一次数据。

仍不能由软件证明：DRV8311H 的 `nFAULT` 原因，因为该引脚没有 MCU 端点；TIM1 Break/死区的门极级响应，因为当前生成配置仍为 Break disabled、deadtime 0；以及 `pwm_out_disable()` 后 DCBUS 是否已经被外部切断。故障契约已通过 host，并不等于功率级安全验收。

### 3.2 `[高/主机编译已补齐，仍有独立契约差异]` AT32F415 迁移

当前 live source/preset/platform 和 host target 都已对齐到 `AT32F415`。本轮修复前的历史阻塞是：

```text
${CMAKE_SOURCE_DIR}/platform/at/at32f403a/generated/inc
```

该路径在当前工作树不存在，导致应用测试编译时看不到 F415 的 `GPIO_PINS_15`；本轮已改为 `platform/at/at32f415/generated/inc`，并补齐 host shim。当前 35 个 host target 均已编译并通过 CTest；早期 4 个独立契约差异已在本轮回归中闭合，详见 [operations-and-validation.md](operations-and-validation.md) 第 9 节。

正式 `README.md`、`docs/README.md`、`docs/architecture/README.md` 及多份架构/启动文档也仍写 `AT32F403A`；这与当前 F415 Wiki 形成双事实源。历史维护日志还存在 `COM54@1000000` 与最新 `COM3@115200` 的时间差，当前 generated USART1 是 115200。

**影响**：正式旧文档仍可能让新开发者误选平台目录或误解串口参数；host 编译和既有契约阻塞已解除，但平台事实源仍需后续统一。

### 3.3 `[已修正/需专项复核]` 控制时基和滤波器配置曾有数值冲突

当前数据关系：

```text
foc_config.h:
  PWM_FREQ_HZ       = 10000
  CURR_LOOP_FREQ_HZ = 10000
  DEFAULT_CURR_BW_HZ = 250
  DEFAULT_CUTOFF_HZ = DEFAULT_CURR_BW_HZ * 5.0 = 1250 Hz
  DEFAULT_SAMPLING_HZ = CURR_LOOP_FREQ_HZ = 10000

generated wk_tmr1.c:
  TMR1 ARR = 3749, dual/up-down counting
  TMR1 CH4 CCR = 3740

motor_task.c:
  ADC ISR 默认每第二个 tick 执行 current-loop；bench 每第四个 tick
```

当前代码已把默认 cutoff 修正为 `1250 Hz`，并把 LPF 离散采样率显式绑定到 FOC step；默认产品为 10 kHz，bench 为 5 kHz。`1250/250=5` 满足同一段代码声明的 4–10 倍约束。原工作树的 25 Hz 配置曾直接进入 `motor_drv.c:foc_lpf_init()`，使反馈滤波器比 PI 带宽低一个数量级。

本轮无板应力模型使用 `Rs=2.55 Ω`、`Ls=0.86 mH`、当前 PI 增益公式和一阶 RL 对象，对 `0.10 A` 指令运行 100 ms：旧 25 Hz 的电流峰值约 `0.33245 A`，当前 1250 Hz 的峰值约 `0.10000 A`。这证明旧配置具有“反馈滞后→PI继续加压→瞬态电流越过 0.20 A 快速 trip”的确定性风险机制；它解释的是 I-f/电流闭环路径，不能单独解释绕过电流 PID 的纯电压开环。

该耦合已在本计划中修复：`motor_runtime_init()` 先加载持久化 `curr_bandwidth`，对持久化 NaN/Inf/非正值回退默认值，再调用 `foc_lpf_init()`；`diag set currbw` 只接受 finite 且大于零的值，并同时重算 PI 与 Id/Iq LPF。`foc_lpf_refresh()` 保留当前滤波输出，仅更新系数；对应持久化、持久化非法值、动态修改、非法值和历史输出保持测试均已加入。

另外：

- `ENCODER_PLL_BW_HZ=500` 的正文说明约 80 Hz，宏尾部注释写约 160 Hz；
- `foc_ctrl.c` 仍称 outer loop 是 TIM6 ISR，实际平台 owner 是 TMR5 soft IRQ；
- `encoder.h`/校准注释保留 ADC ISR/20 kHz、TIM6 等历史描述。
- `foc_math.h` 将 `alpha=ωc/(ωc+fs)` 注释为 Tustin；该形式实际更接近 backward-Euler 一阶离散化，需统一公式注释和频响推导。

**建议**：把“PWM carrier、ADC trigger、FOC step、encoder/outer step、PID dt、filter sample rate”列成单一配置表，并让宏名与实际语义分离，例如不要用 `PWM_PERIOD_S` 同时表达 FOC step 的 100 µs；同时为 `currbw` 参数恢复/修改增加 LPF 同步契约。

### 3.4 `[历史配置对照/当前已分层]` Bring-up 快速 trip 与参数校准激励

历史版本的 `FOC_FAST_CURRENT_TRIP_A=0.20 A` 会与默认 `0.30/1.00 A` 量级的带电校准激励冲突。当前临时诊断包线已重新分层为每相快速 `1.00 A`、任务级持续 `0.75 A/3 s`，且 `FOC_ENERGIZED_CALIB_ENABLE=0U` 仍在 FSM 入口拒绝带电校准；这不是通过保护阈值替代校准安全评审。后续若重新打开带电校准，必须单独建立校准专用电流/急停包线。

## 4. 中优先级风险

### 4.1 主机 Flash 替身与 F415 几何不一致

固件当前由平台元数据推导 128 KiB Flash、1 KiB sector；`motor_store.c` 的地址因此落在 `0x0801E800` 到 `0x0801FC00` 的六个 slot。主机 `tests/host/support/bsp_flash.h` 却保留 1 MiB、2 KiB sector、512 sectors。

主机测试可以证明 payload/header/CRC/A-B 选择逻辑，但不能证明：

- F415 最后 sector 地址正确；
- sector 擦除边界正确；
- slot 总大小不覆盖 linker/应用内容；
- F415 vendor flash 对齐和容量限制满足真实硬件。

### 4.2 启动加载状态被忽略，显式 `store load` 也不是事务

`motor_runtime_init()` 的默认初始化后会尝试加载 motor/enc/sys，但 `_motor_load_params()` 对各组返回状态没有向上报告，最后仍完成启动。显式 `motor_runtime_request_store_load()` 按 group 依次加载，返回 `partial`；如果前一组已经成功修改运行对象、后一组失败，当前实现没有事务回滚。

这不是一定错误：部分恢复可以是产品策略。但必须把“部分恢复是否允许、失败组是否保留默认值、已成功组是否保留”作为明确的 API 合约，否则 Shell 的 `partial=1` 只是提示而不是一致性保证。

### 4.3 编码器 blocking SPI 位于 TMR5 soft IRQ

`ENCODER_SPI_DMA_ENABLE=0`，`encoder_update()` 使用 blocking SPI；配置注释估计每次约 5 µs，但当前 soft IRQ 还负责 outer loop。主机测试无法测量中断占用，也不能证明在 5 kHz/相关时基下不会挤压更高优先级中断或破坏采样窗口。要么明确这是当前可接受的预算，要么迁移到 DMA/双缓冲，不能仅依据“能编译”判断实时性。

### 4.4 已声明但尚未实现的输入模式

`POS_FILTER`、`VEL_FILTER`、`TUNING` 在枚举和 Shell 名称体系中存在，但 `foc_process_input_task()` 的对应分支当前没有行为。调用者如果只看枚举会误以为可以使用这些模式；文档和命令帮助应明确“已声明/未实现”，或者禁止进入这些状态。

### 4.5 普通 ADC DMA API 不是当前 live path

`motor_drv.c` 中 ordinary DMA/ordinary ADC 读取实现被注释/空实现，当前电流与 VBUS 的 live 采样来自 injected group。任何依赖普通 ADC DMA 的 NTC/辅助模拟量功能都不能从当前主链推断为可用。

### 4.6 任务栈预算契约已对齐

当前 live 配置与产品预算契约一致：`TM_SHELL_TASK_STACK_BYTES=192U*4U`、`TM_APP_TASK_STACK_SHELL_BYTES=384U*4U`；FOC 任务预算由 `applications/config/motor_task_config.h` 明确定义为 `motor=512U*4U`、`safe=256U*4U`。预算 host contract 已通过。该结果只证明常量和注册边界一致，不等价于已取得真实 RTOS stack high-water mark；后者仍需运行时 profiling。

## 5. 关键行为模拟

以下是用源码调用链做的无板模拟，目的在于验证状态/调用关系，不是仿真电机物理。

### 5.1 场景 A：正常启动到 RUNNING

```text
初始：axis_state=IDLE，PWM disabled/初始钳位，默认配置已装载

state start
  -> app_shell_cmd_state
  -> motor_runtime_request_axis_action(START)
  -> motor_fsm_dispatch(START)
  -> IDLE -> RUNNING
  -> _on_running_enter()
  -> ADC1 handler 每第二 tick 执行 current-loop
  -> TMR5 handler 更新 encoder/outer-loop
```

预期：控制模式决定电压/I-f/电流/速度/位置路径，PWM 由当前环写 duty。

当前特别行为：`_action_start()` 仍只对未校准 encoder 告警，不拒绝启动；但进入驱动态后，ADC ISR 会先执行 `foc_fault_check_fast()`，任务级检查再做持续性兜底。因此“启动请求可达 RUNNING”不等于“允许带风险测量继续调制”，不过 encoder 几何/方向和功率级硬件急停仍是启动前置条件。

### 5.2 场景 B：过流事件

```text
ADC1_IRQHandler
  -> adc_read_phases() 写入较大 ia/ib/ic
  -> foc_fault_check_fast()
  -> 超过 1.00 A：capture first-fault context -> fault_code |= FAULT_OVER_CURR
  -> pwm_out_disable()
  -> return，不再执行本次 FOC
motor_task 1 ms
  -> foc_fault_check() 后备检查
  -> motor_fsm 在任务上下文收敛到 FAULT
```

该场景已由 `test_motor_fault_runtime_contract` 覆盖，并且无板目标通过；但 `pwm_out_disable()` 只关闭六路 TMR1 通道，不等价于外部电源断开或 DRV `nFAULT` 硬件反馈。

### 5.3 场景 C：校准完成后的 encoder 保存

```text
foc_calib_enc.c
  -> LUT/offset/方向计算完成
  -> motor_calibration_runtime_request_encoder_save()
  -> param_save_req |= PARAM_SAVE_ENC
  -> ADC/校准 ISR 返回，不写 Flash
motor_safe_task 每 10 ms
  -> motor_runtime_process_deferred_save()
  -> foc_param_save_enc()
  -> bsp_flash_write()
```

预期：实时中断不会执行长时间擦写；下次 `store status` 能看到 slot 状态。当前源码满足这个调用边界，但“Flash 写入成功、掉电中断后 A/B 选择正确”需要主机存储测试和后续硬件专项共同证明。

### 5.4 场景 D：Shell 队列背压

```text
DMA RX line
  -> line length <= SHELL_LINE_MAX
  -> tm_shell_task_submit_line()
  -> static queue
```

队列满时返回 `-EAGAIN`、计数失败，发送端输出有界 overflow notice；不会动态分配。`test_tm_shell_task` 和当前 app-level burst test 均在 host RTOS/运行时替身下通过；这仍只证明队列和输出预算逻辑，不证明真实串口吞吐。

### 5.5 场景 E：参数加载部分成功

```text
store load all
  -> load motor: OK，motor config 已写入 runtime
  -> load enc: EMPTY/CRC error
  -> load sys: 继续按实现处理
  -> partial = true
  -> 只在全部 requested group OK 时统一 restore/finalize
```

这会产生一个需要产品确认的状态：运行对象可能包含已成功加载的部分数据，同时派生状态可能没有完成统一 refresh。当前文档将其标为“部分恢复、无事务回滚”，不作更强的正确性承诺。

### 5.6 场景 F：运行中诊断写入

```text
diag set vq/if/dp0/currbw/PID/encoff
  -> Shell snapshot
  -> axis != IDLE ? result=error reason=idle_only
  -> 不调用 runtime setter

直接调用 motor_runtime_diag_set_* / passthrough
  -> runtime axis-state guard
  -> RUNNING/CALIB/FAULT ? false，控制量保持不变
```

这是双层边界：Shell 提供可识别的操作反馈，runtime 防止其他调用者绕过
Shell。它只限制诊断写入，不替换正常运动请求 `vel/torque/position ramp`。

### 5.7 场景 G：故障停止后的诊断导出

```text
fault status -> motor_fault_context_read() -> 首故障摘要/快照
obs all      -> motor_runtime_snapshot()  -> 最后一次运行数据
fault clear  -> FSM clear 成功 -> context_clear()
```

因此“停止后分析”不需要在保护 ISR 中打印或写 Flash；若串口失联，硬停仍
必须发生，数据可留在 RAM 等待后续读取。`fault clear` 前必须先保存上下文，
否则会丢失本次根因证据。

## 6. 证据与结论分离

### 已由本轮软件证据证明

- AT32F415 platform target、generated code、FreeRTOS、BSP、applications 可以交叉编译并链接；
- 三种固件变体均完成最终交叉构建：full-diag 为 FLASH 57.34%、RAM 65.97%，standard 为 FLASH 30.61%、RAM 60.67%，tiny 为 FLASH 28.16%、RAM 55.37%；
- 基础 subsys/BSP/Shell host 契约中 11 个测试通过；
- FOC round contract 通过；
- GCC 主机配置通过，Clang 当前被项目白名单拒绝；
- 应用 host 全量 38 个目标均已编译并通过 CTest（最终门禁以本轮新鲜输出为准）；新增动态 LPF、故障上下文、运行态诊断门禁、带电校准门禁、无板电压开环/SVPWM 和时基/电流闭环参数模型也在同一回归中通过；
- fault runtime 的快速路径、任务级后备、启动校准验证和 VBUS 防护已由本轮 host 目标覆盖；
- 早期维护记录中的 full-diag OpenOCD 写入和实板只读复核属于历史证据，不属于本轮无板验证，也不解除当前实机门禁。

### 只能由后续软件专项证明

- host 契约已闭环：预算探针通过 CMake wrapper 表达预期断言，任务栈、FSM 中止和诊断格式均已通过回归；
- F415 Flash 几何替身、A/B 掉电/损坏 slot 组合；
- 故障清除、多故障优先级、故障快照持久化和独立电源切断；
- ISR/任务 worst-case execution time、stack high-water mark；
- TMR1/TMR5 的真实中断周期和采样相位。

### 明确未验证

- 真实串口参数、COM 设备、Shell 交互；
- PWM、ADC injected trigger、SPI 编码器信号完整性；
- 电机参数是否适配实际负载；
- 速度/位置闭环稳定性、温升、驱动器硬件保护；
- 任何“上板可运行”结论。

## 7. 建议的整改优先级

1. **P0：功率级安全闭环**：软件快速 trip 已完成；下一步是启用并测量 TIM1 Break/死区、补充独立电源/人工急停，并评估把 DRV `nFAULT` 接入 MCU 的硬件改版。
2. **P0：迁移/契约闭环**：host include 已改为 AT32F415，新增目标已纳入全量测试；后续只需保持测试注册和正式文档的单一事实源。
3. **P1：时基与滤波器单一事实源**：默认 `1250 Hz` cutoff 和运行时 `currbw` 同步已通过无板模型；仍需在后续版本统一 PWM carrier、ADC trigger、FOC step、TMR5 rate、PID dt、LPF sampling/cutoff 和 PLL 单位，并继续修订误导性注释。
4. **P1：主机存储可信度**：让 host Flash shim 使用当前 platform metadata，增加边界/擦除/损坏 slot 测试；明确 partial load 的事务语义。
5. **P1：实时预算**：为 blocking encoder SPI、FOC step、ADC handler、TMR5 handler 建立 host 可测的逻辑预算和后续板级 profiling 入口。
6. **P2：功能声明一致性**：对 no-op 输入模式和 ordinary ADC API 标记未实现，避免 Shell/API 暴露超出实际能力。
7. **P2：测试注册维护**：`WILL_FAIL` failfast 已通过 CMake wrapper 稳定表达，同步的 Shell/电机任务栈预算和诊断/FSM 契约已纳入全量回归；后续只需防止新测试重新引用旧基线。

## 8. 最终判断

当前代码不是“文档缺失导致不可理解”的状态，而是已经有较完整的分层和可追踪调用链；本 Wiki 补上了从平台入口到 FOC 输出、从 Shell 请求到 Flash commit 的知识地图。

同时，当前不能将项目标记为“电机实机验证完成”：默认滤波器和运行时同步已通过无板测试，且当前 bench 短脉冲已取得板上 ADC/角度/duty 证据，但 DRV 故障输入与 Break/死区仍没有闭环，外部电流/温度和连续旋转仍未验证。正确的工程结论是：**AT32F415 三种默认固件、bench 固件和 35 个 host 目标的软件门禁通过；低压短脉冲未复现高电流卡死并观察到角度推进；电机连续旋转、外部波形、独立电流、热和硬件故障响应仍未验证，`REAL_BENCH_TEST_STATUS=BLOCKED`。**

## 9. 2026-08-29 现有板短脉冲根因定位结果

本轮按“先复现、再停机后分析”的方式使用 bench ELF 在现有板上运行 D0-D4、O0-O3，
每轮只改变一个电压/矢量变量，使用约 50 ms 的 ADC 自动护栏。实板数据和原始日志见
[tmp/bench/20260829_actual/result.md](../../tmp/bench/20260829_actual/result.md)。

### 9.1 已观察到的事实

- `[实板测量]` 当前 ELF 已经由 OpenOCD `Verified OK` 写入；启动后 `axis=idle`、
  `fault=0`、`vbus≈11.91 V`。
- `[实板测量]` D0-D4 的板上换算相电流保持毫安级；D1/D2/D3 的软件 sector 分别为
  1/2/5，D4 的 duty 约为 `0.49/0.51/0.51`。
- `[实板测量]` O0 的静止电压开环保持 `theta=0/we=0`；O1 的正常停机快照为
  `theta≈0.01 rad`、`we≈1.28/3.14 rad/s`。
- `[实板测量]` 让 bench 护栏自然触发时，O1/O2/O3 的 `fault status` 分别锁存
  `theta≈0.075 rad`、`we≈3/3.14 rad/s`；O3 最大锁存相电流约 `60 mA`，低于
  `0.10 A` 预警和 `0.20 A` 快速阈值。
- `[源码确认]` `FAULT_COMM_ERROR (0x100)` 是 bench 预算到期的诊断停机标记，不是
  DRV `nFAULT` 反馈；当前原理图没有 MCU `nFAULT` 端点。

### 9.2 当前根因判断

这组数据没有复现“低压短脉冲高电流卡死”，也没有复现“软件角度不推进”。在
`vq≤0.40 V`、`we=3.14 rad/s`、约 50 ms 窗口内，主要观测是角度推进正确、VBUS
稳定、板上电流低。低压不产生连续机械转动只能暂列为“电压/转矩裕量不足、齿槽
转矩或机械负载”的候选，不得写成唯一根因；`OPEN_LOOP_ROOT_CAUSE_STATUS` 继续
为 `UNRESOLVED`。

本轮还发现并修正了一个 bench 诊断问题：FSM 的通用安全清理会在正常 `state disable`
后清零 `theta_sim/we_volt_now/we_volt_ramp`。bench 现在在 PWM 已关闭、PID/设定值
已清理后恢复这三个只读字段，使 O1 结果可以在停机后复核；该修复不改变默认产品
固件的清理行为。

### 9.3 下一最小动作

在没有独立波形/外部电流/温度证据的情况下，不直接放宽保护或延长连续运行。下一轮
若继续实机，只能保持 bench 镜像、同一 12 V/1 A 背景和自动护栏，先把电压开环的
短脉冲数据与外部可观察机械动作逐项对齐；任何 `>0.10 A`、VBUS 异常、失控或数据
路径不确定都立即停止。I-f、编码器校准和速度闭环仍不属于本阶段。
## 10. 2026-08-29 电压模式入口相位修复与 A/B 结果

### 10.1 根因假设与代码路径

`[静态推导]` 电压开环默认控制模式为 `CTRL_MODE_VOLT`。旧路径在
`_motor_clear_setpoints()` 中将 `theta_sim` 清零，而
`foc_cmd_voltage_set()` 只有在“从其他控制模式切入电压模式”时才同步角度。
因此，若系统已经处于默认电压模式，第一次写入 `vq` 不会执行相位种子同步。
启动时 `theta_sim` 可能为 `0`，而编码器电角度为另一相位，存在初始转矩方向
错误或卡死的风险。

第一版修复曾读取 `motor->meas_data.theta_elec`，但该字段在 IDLE 阶段可能还是
旧缓存；`[实板测量]` A1 仍出现 `theta_sim≈0` 而编码器电角度约 `3.34 rad`。
最终修复改为读取实时编码器状态 `motor->enc_pri.state.angle_elec_rad`，并在
电机处于 IDLE 或真正从其他模式切入 VOLT 时执行：

```text
theta_sim = wrap_0_2pi(enc_pri.state.angle_elec_rad)
we_volt_now = 0
```

运行中的内部电压更新不重复重置角度；固定相电压命令仍使用显式 phase 角度，
不被编码器角度覆盖。`motor_runtime_snapshot()` 同步改为报告实时编码器角度，
避免将停机诊断中的旧 `meas_data.theta_elec` 误认为当前角度。

### 10.2 证据链

- `[主机测试]` 新增/扩展 `test_foc_motion_runtime_contract`，覆盖默认 VOLT
  模式、IDLE 入口、角度 wrap、I-f 入口、RUNNING 禁止改参和固定相电压边界；
  测试先因 `theta_sim` 未同步失败，修复后通过。
- `[主机测试]` `motor_runtime_snapshot` 和 Shell burst fixture 先因实时编码器角度
  语义变化失败，更新断言后通过。
- `[无板验证]` host flow 与独立 CTest 均为 `38/38 PASS`，外设静态契约为
  `FOC_PERIPHERAL_CONTRACT=PASS`。
- `[实板测量]` 修复前同一 `diag set vq 0.10 3.14` 入口读数为
  `theta_sim≈0`、`theta_elec≈3.35 rad`；最终镜像 A1 读数为
  `theta_sim≈3.21`、`theta_elec≈3.20 rad`，`we_volt_now=0`，相位误差约
  `0.01 rad`。
- `[实板测量]` 最终镜像在 `vq=0.10/0.20/0.30/0.40 V`、`we=3.14 rad/s`、
  每次约 `20 ms` 的正向短脉冲，以及 `vq=0.10 V` 的反向短脉冲中，均为
  `fault=0`、`VBUS≈11.91 V`；板上 ADC/CSA 换算电流未超过 `0.10 A`，未触发
  `0.20 A` 快速过流。

### 10.3 当前结论边界

相位入口缺陷已经被源码、主机回归和 A1 实板入口数据共同确认并修复；它是一个
真实的软件缺陷，但由于修复前后的 `20 ms` 短脉冲均未复现原始“高电流卡死”现象，
不能把它单独写成整机唯一主根因。连续运行、外部相电流、互补 PWM 死区/重叠、
采样窗口、DRV 温升和机械转矩裕量仍未得到实板证据。

当前状态保持：

```text
PHASE_ENTRY_ALIGNMENT=CONFIRMED
OPEN_LOOP_SOFTWARE_PATH=PASS
MOTOR_PHYSICAL_OPERATION=UNVERIFIED
OPEN_LOOP_ROOT_CAUSE_STATUS=UNRESOLVED
REAL_BENCH_TEST_STATUS=BLOCKED
CONTROLLED_DIAGNOSTIC_STATUS=ALLOWED_WITH_MINIMUM_GUARDS
```

最终镜像为
`build/fw-full-diag-carrier-bench-debug/output/falcon-foc-at32.elf`，SHA256 为
`F0402BADC248FEB2C7EEB3F149DF321B6A3189B3C65125AB876A5E83064F04B1`；对应实测
日志见 `tmp/bench/20260829_phase_seed_final/`。

## 17. 2026-08-29 当前规范证据快照

本节是当前工作树和最后一轮 COM3 实板记录的规范入口。前文带有旧日期、旧镜像
哈希或旧样本数量的段落保留为历史过程记录，不覆盖本节的最新状态。

```text
PHASE_ENTRY_ALIGNMENT=CONFIRMED
OPEN_LOOP_SOFTWARE_PATH=PASS
MOTOR_PHYSICAL_OPERATION=UNVERIFIED
OPEN_LOOP_ROOT_CAUSE_STATUS=UNRESOLVED
REAL_BENCH_TEST_STATUS=BLOCKED
```

## 23. 2026-08-30 完整正交实机矩阵

`[实板测量]` 在同一 bench400 镜像下完成了 9 点 ADC trigger、36 组 PWM/ADC map、
六向固定矢量、1 V 静态相位矩阵、1 V/30 rpm 正反向动态测试、`theta0=0/+π/2`
动态 A/B 和 `enc_dir=0/1` 对照。所有测试都在每项前 reset、短窗口运行、停机后读取，
未执行校准、参数写入或 `state stop`。

### 23.1 已排除的低幅软件映射现象

`[实板测量]` 低幅 `dp0=+0.05 V` 下，9 个 trigger 点的软件峰值约 `0.009..0.023 A`；
36 组 `6×6` PWM/ADC 映射峰值约 `0.008..0.028 A`，均无 fault、VBUS 均约
`11.88..11.93 V`。默认映射下 `dp0/dp1/dp2` 正负六向也均无单相饱和，峰值约
`0.011..0.028 A`。

因此当前证据没有显示某一个 PWM map 或 ADC map 是唯一正常项，也没有显示低幅固定矢量
存在稳定的单相失效。该结论只适用于 MCU ADC/CSA 软件观测，不替代真实 INH/INL、SOx
和绕组电流测量。

### 23.2 电压梯度和相位 A/B

`[实板测量]` 使用正确的 30 rpm 电气速度命令 `we=21.9911 rad/s`，`vq=0.25..2.00 V`
时软件电流总体随电压增加，最高观测约 `0.468/0.450/0.065 A`；VBUS 没有明显下陷。
软件 `theta_sim` 和 `we_volt_now` 正常推进，但编码器没有稳定跟随软件场。

`[实板测量]` 1 V 静态相位矩阵中不同 `theta0` 会改变软件电流和编码器电角变化，说明
相位关系确实值得观察；但 `theta0=0` 与 `+π/2` 各两次动态复测的电流和编码器尾差
没有稳定的同条件改善，因此不确认相位种子为根因。`enc_dir=1` 两次与 `enc_dir=0`
历史结果相比也没有形成稳定机械改善，产品默认方向不改。

### 23.3 采样窗口结论

`[实板测量][静态推导]` 在此前 `vq=1.00 V,we=21.9911 rad/s` 对照中，
`adc_trig=512` 两次软件峰值约 `0.011..0.015 A`，`adc_trig=3740` 两次约
`0.201..0.312 A`；而本轮低幅固定矢量的 9 点扫描差异很小。这说明触发敏感性依赖
动态功率状态，优先候选仍是低侧 CSA 建立时间、中心对齐 PWM 相位、ADC 注入完成时序
或采样孔径，不能简单将 512 写成默认正确值。

### 23.4 当前归因

当前最强证据是“软件电流观测存在动态触发点依赖”；这不是“物理相电流已确认异常”。
“软件旋转场正常而机械未稳定跟随”仍可由真实电压/转矩裕量、机械负载、电机参数、
编码器相位/方向或功率级响应共同解释，现有矩阵尚未形成唯一主根因。

完整汇总：[`20260830_103200_full_hardware_root_cause/result.md`](../../tmp/bench/20260830_103200_full_hardware_root_cause/result.md)。

状态保持：

```text
ADC_MEASUREMENT_WINDOW=ANOMALOUS_OR_STRONGLY_SENSITIVE
PHASE_OFFSET_CANDIDATE=REJECTED_AS_UNREPEATED
ENCODER_DIRECTION_CANDIDATE=SUPPORTED_NOT_CONFIRMED
OPEN_LOOP_SOFTWARE_PATH=PASS
MOTOR_PHYSICAL_OPERATION=UNVERIFIED
OPEN_LOOP_ROOT_CAUSE_STATUS=UNRESOLVED
REAL_BENCH_TEST_STATUS=BLOCKED
```

## 25. 2026-08-30 静态 trigger 隔离与完整低速窗口

`[实板测量]` 将 `we` 固定为 `0` 后，`adc_trig=1874/2300/2812/3740` 仍复现动态扫描
中的软件电流转折：1874 的峰值约为 `0.012..0.016 A`，2300 开始出现约 `0.262 A`
的 B 相读数，2812 和 3740 的 A/B 读数约为 `0.241..0.264 A`；每个点重复两次，
VBUS 保持 `11.87..11.93 V`，无 fault。这说明触发相关性不以旋转场为必要条件，
应优先从固定 PWM 低侧导通、DRV8311H SOx/CSA 建立、注入序列和 ADC 采样相位解释。

`[实板测量]` 在 `we=21.9911 rad/s`（7 对极约 30 rpm）和约 375 ms 有效窗口下，
`vq=1.0/1.5/2.0 V` 的软件峰值分别约为 `0.257/0.256/0.033 A`、
`0.407/0.435/0.420 A`、`0.586/0.633/0.578 A`。软件 `theta_sim` 持续推进，
但 `theta_elec` 只部分跟随；VBUS 没有同步塌陷，且未触发 1 A 软件快速保护。
1.0 V 首轮与重复轮的起始角不同，不能将峰值差异解释成根因。

这组结果把问题分成两个未闭环层面：

1. `ADC_MEASUREMENT_WINDOW=REPRODUCIBLY_TRIGGER_DEPENDENT`：这是 MCU ADC/CSA
   换算观测与 CH4 compare 的可重复关系，不是独立相电流证明。
2. 电压开环存在场角与编码器角的滑差：可能是转矩裕量、机械/参数、功率级响应或
   采样观测共同作用，现有数据不足以选出唯一物理根因。

完整日志：[`static trigger boundary`](../../tmp/bench/20260830_105417_static_trigger_boundary_1V/)、
[`1 V`](../../tmp/bench/20260830_105810_voltage_full_window_1V_30rpm/)、
[`1 V repeat`](../../tmp/bench/20260830_105937_voltage_full_window_1V_30rpm_repeat/)、
[`1.5 V`](../../tmp/bench/20260830_110024_voltage_full_window_1p5V_30rpm/)、
[`2 V`](../../tmp/bench/20260830_110109_voltage_full_window_2V_30rpm/)。

`[实板未验证]` 尚无 SOx/PWM 波形、独立相电流、温度和机械测速，因此不把软件峰值
当作绕组实际电流，不把 2 V 窗口当作电机运行通过，也不修改默认 trigger、ADC rank、
相序、编码器方向或保护阈值。状态仍为：

```text
ADC_MEASUREMENT_WINDOW=REPRODUCIBLY_TRIGGER_DEPENDENT
OPEN_LOOP_SOFTWARE_PATH=PASS
MOTOR_PHYSICAL_OPERATION=UNVERIFIED
OPEN_LOOP_ROOT_CAUSE_STATUS=UNRESOLVED
REAL_BENCH_TEST_STATUS=BLOCKED
```

## 22. 2026-08-30 ADC trigger 重复对照

`[实板测量]` 使用 bench400 镜像 `59B78D648511926F8AD47213CA35E49C0D536697BE814B1B77AF64F84E949A03`，
固定 `vq=1.00 V`、`we=21.9911 rad/s`、`enc_dir=0` 和约 350 ms 名义窗口，分别重复
`adc_trig=512`、`adc_trig=3740` 两次。结果如下：

| CH4 compare | 两次软件电流峰值 A | raw ADC 特征 | 编码器机械角变化 |
|---:|---|---|---:|
| 512 | `0.012/0.011/0.014`、`0.012/0.012/0.015` | 约 `2042..2058`，接近 offset | 约 `0.0001..0.0004 rad` |
| 3740 | `0.270/0.312/0.262`、`0.275/0.276/0.201` | IA/IB/IC 约 `1882..2221`、`1888..2244`、`1922..2067` | 约 `-0.0002..+0.0002 rad` |

两组 VBUS 均约 `11.87..11.93 V`，trace 各约 2000 条，按约 5 kHz 处理频率折算为
约 400 ms。名义 350 ms 等待叠加串口往返越过 bench400 预算，四次在 `state disable`
之后出现 `FAULT_COMM_ERROR (0x100)`；没有出现 `FAULT_OVER_CURR`。因此该故障属于测试
编排/预算窗口边界，不能作为 DRV 或电机过流根因。

`[实板测量][静态推导]` 触发点差异在两次重复中均保持，说明 MCU ADC/CSA 软件观测对
TMR1 CH4 compare 位置高度敏感，优先候选是低侧 CSA 建立时间、中心对齐计数相位、ADC
注入组完成时序或采样孔径。该结论不等同于“512 正确、3740 错误”：512 可能只是读到
掩盖真实电流的中心/无效窗口，3740 的数值也不是独立相电流计读数。

`[实板未验证]` 两组测试的编码器机械角几乎不变，仍不能证明转子跟随 30 rpm，也没有
获得真实 PWM/SOx 波形、独立相电流或温度证据。原始结果：
[`20260830_095925_adc_trigger_followup/result.md`](../../tmp/bench/20260830_095925_adc_trigger_followup/result.md)。

本轮不修改默认 ADC 触发点、ADC rank、相序、FOC 参数或保护阈值。当前状态：

```text
ENCODER_DIRECTION_CANDIDATE=SUPPORTED_NOT_CONFIRMED
ADC_MEASUREMENT_WINDOW=ANOMALOUS_OR_STRONGLY_SENSITIVE
OPEN_LOOP_SOFTWARE_PATH=PASS
MOTOR_PHYSICAL_OPERATION=UNVERIFIED
OPEN_LOOP_ROOT_CAUSE_STATUS=UNRESOLVED
REAL_BENCH_TEST_STATUS=BLOCKED
```

## 24. 2026-08-30 动态 ADC trigger 全曲线与边界复测

### 24.1 测试条件和实际窗口

`[实板测量]` 使用同一 bench400 镜像，固定 `pwm_map=0`、`adc_map=0`、`enc_dir=0`、
`theta0=0`、`vq=1.00 V`、`we=21.9911 rad/s`，先扫描 `CH4 compare=512..3740`，
再对 `1874/2300/2812/3740` 各重复两次。每项前复位，运行后发送 `state disable`，
停机后读取 `fault/obs/trace`，最终复位得到 `axis=idle`、`fault=0`、`duty=0/0/0`。

bench 镜像的 FOC trace 名义频率约为 `20 kHz / 4 = 5 kHz`；因此约 `1000` 个样本
对应约 `200 ms` 的软件观测窗口。该换算不等价于示波器的精确带电时间，也不能代替
人工断电时序记录。

### 24.2 观测结果

| CH4 compare | samples | 软件峰值 ia/ib/ic (A) | VBUS (V) |
| ---: | ---: | --- | --- |
| 512 | 1005 | 0.011/0.010/0.014 | 11.87–11.92 |
| 1024 | 1005 | 0.013/0.013/0.012 | 11.88–11.92 |
| 1536 | 1003 | 0.013/0.013/0.012 | 11.88–11.91 |
| 1874 | 1002 | 0.013/0.017/0.011 | 11.89–11.93 |
| 2300 | 1000 | 0.058/0.259/0.020 | 11.87–11.91 |
| 2812 | 999 | 0.241/0.251/0.032 | 11.89–11.93 |
| 3200 | 1007 | 0.243/0.258/0.027 | 11.87–11.93 |
| 3500 | 1004 | 0.241/0.256/0.030 | 11.90–11.93 |
| 3740 | 1003 | 0.243/0.259/0.029 | 11.89–11.92 |

边界重复结果：

| compare | repeat 1 峰值 ia/ib/ic (A) | repeat 2 峰值 ia/ib/ic (A) |
| ---: | --- | --- |
| 1874 | 0.012/0.015/0.015 | 0.013/0.017/0.011 |
| 2300 | 0.058/0.256/0.022 | 0.090/0.259/0.017 |
| 2812 | 0.239/0.248/0.029 | 0.237/0.252/0.029 |
| 3740 | 0.242/0.264/0.030 | 0.243/0.254/0.029 |

八次边界测试均为 `fault=0`、`active=0`。`theta_sim` 约从 `3.40 rad` 推进到
`4.60 rad`，`we_volt_now` 尾值约 `12 rad/s`，而 `theta_elec` 只在约 `3.40..3.42 rad`
附近变化。VBUS 没有随高读数同步下陷。

### 24.3 底层推导

`[源码确认]` TMR1 使用 `150 MHz` 计时基准、`ARR=3749`、中心对齐双向计数；一个
计数 tick 约为 `6.667 ns`，半个载波计数区间约 `25 us`。因此 `CCR4=3740` 是接近
峰值的比较点，`CCR4=1874` 约在半区间中点，`CCR4=2300` 和 `2812` 位于更靠近
峰值的不同时间位置。中心对齐下还要区分上数/下数事件；trace 中同时记录了 `CVAL`、
计数方向和 `CCR4`，所以 compare 值本身不能单独描述完整采样相位。

`[源码确认]` ADC 注入组按 `VBUS→IC→IB→IA` 四个 rank 顺序转换，所有通道采样时间
为 `7.5 cycles`。一次触发并不是三个相电流的同时采样；CH4 compare 改变后，四个
转换相对于 PWM 边沿的时间全部改变。

`[原理图确认]` DRV8311H 的 SOA/SOB/SOC 来自低侧电流检测路径，CSAREF 接
`DRV_3V3`，当前 GAIN 配置按 `0.5 V/A` 换算。若触发处于低侧检测无效窗口、CSA 尚未
建立或开关瞬态附近，MCU 读数可能被压低或出现相间不对称；这与本轮从 `1874` 到
`2300` 的突变相容，但仍只是候选解释。

### 24.4 结论边界

`[实板测量][静态推导]` 本轮首次把“trigger 敏感”提升为可重复的整段曲线：`1874`
及以下读数低，`2300` 进入高读数，`2812..3740` 保持高读数。当前应使用：

```text
ADC_MEASUREMENT_WINDOW=REPRODUCIBLY_TRIGGER_DEPENDENT
```

这证明 MCU ADC/CSA 软件观测与 TMR1 CH4 compare 存在稳定相关性，不证明 `1874` 是
正确采样点，也不证明 `3740` 的 `0.24..0.26 A` 就是独立仪器测得的真实相电流。
不得依据低读数点直接修改默认 trigger、ADC rank、相序或保护阈值。

`[实板未验证]` 仍缺少 PWM/互补门极波形、SOA/SOB/SOC 波形、独立相电流、机械测速
和温度证据。因此当前唯一物理根因仍未确认：

```text
PHASE_OFFSET_CANDIDATE=REJECTED_AS_UNREPEATED
ENCODER_DIRECTION_CANDIDATE=SUPPORTED_NOT_CONFIRMED
OPEN_LOOP_SOFTWARE_PATH=PASS
MOTOR_PHYSICAL_OPERATION=UNVERIFIED
OPEN_LOOP_ROOT_CAUSE_STATUS=UNRESOLVED
REAL_BENCH_TEST_STATUS=BLOCKED
```

原始数据：[`full hardware root cause`](../../tmp/bench/20260830_103200_full_hardware_root_cause/result.md)、
[`boundary repeats`](../../tmp/bench/20260830_104326_dynamic_trigger_boundary_repeats_1V_30rpm/)。

## 20. 2026-08-30 标准 SVPWM 与正交实机矩阵结果

完整原始记录：[`tmp/bench/20260830_074936_post_snapshot_fix/result.md`](../../tmp/bench/20260830_074936_post_snapshot_fix/result.md)。

### 20.1 先区分软件修复与物理证据

`[源码确认][无板验证]` 当前 `foc_svpwm()` 将 `v_alpha/v_beta` 按 `inv_vbus` 归一化，在幅值超过 `2/3` 时缩放，再由逆 Clarke 和居中的 common-mode 生成三相 duty。主机契约按 duty 反推 alpha/beta，覆盖六个象限、过限幅、低幅和非法 VBUS，确认数学向量没有被旧的六扇区逐段公式破坏。该结论只覆盖软件数学和调用边界。

`[源码确认][无板验证]` 另修复了停机快照语义：FOC 停止后，encoder PLL 仍由 TMR5 更新，而 `meas_data.out_vel_rpm/out_pos_turns` 不再刷新；snapshot 现在在非 RUNNING 状态从当前 encoder 状态派生输出速度/位置。此前出现的停机后 `-334 rpm` 是陈旧缓存，不能作为物理速度证据。

### 20.2 实板观测

`[实板测量]` bench400 镜像显式刷写验证通过，预检和复位后静态状态正常：VBUS 约 `11.91 V`、软件电流约 `0.012/0.001/0.007 A`、duty 为零、map0/adc0/CH4=3740。36 组 PWM/ADC map 组合均完成，没有出现可重复的唯一正常组合。一次 map2 看似跟随，但两次重复不成立，因此不能用实验 map 替代默认硬件映射。

`[实板测量][静态推导]` 在 `1 V/30 rpm` 下，CH4 compare `512..1874` 读到的软件峰值约 `0.01 A`，`2300..3740` 约 `0.27 A`；固定 `0.5 V/we=0` 时各点约 `0.009..0.027 A`。这与 DRV8311H 低侧 sense-FET 只在相应导通窗口提供有效 SOx 信息的模型一致，说明 ADC 数值对触发位置非常敏感；但没有 SOx/触发波形和独立电流，不能把它等同为真实相电流，也不能仅凭此判定“采样窗口就是唯一根因”。

`[实板测量]` 当前标准 SVPWM 下，`2 V/20..30 rpm` 多轮运行均未建立稳定编码器跟随，软件场角持续推进，软件电流峰值约 `0.54..0.68 A`。`3 V/30 rpm` 一轮峰值接近 `0.9 A`，另一轮首故障上下文记录 `0.281/-1.003/0.740 A` 并触发 `FAULT_OVER_CURR`。`2 V/15 rpm` 两次均无故障，但一次场角约 `2.16 rad`、编码器约 `1.83 rad`，另一次场角约 `2.20 rad`、编码器仅约 `0.65 rad`，因此“低速可稳定跟随”没有重复成立。

### 20.3 当前根因排序和边界

当前可保留的主假设是“电压源开环的转矩/同步裕量不足，且板上电流观测受低侧 CSA 采样窗口影响”；这是候选组合，不是唯一根因。PWM map、ADC map 和 theta0 没有出现可重复优胜，不能修改相序、ADC rank 或编码器零点来迎合单次结果。

下一项真正有判别力的证据仍是 PWM 禁止时的人工 encoder 正/反向整圈扫描，以及必要时同时观察 INH/INL、OUT、SOA/SOB/SOC 和独立相电流。没有这些证据时，必须保持：

```text
OPEN_LOOP_SYNCHRONIZATION=NOT_DEMONSTRATED
OPEN_LOOP_ROOT_CAUSE_STATUS=UNRESOLVED
MOTOR_PHYSICAL_OPERATION=UNVERIFIED
REAL_BENCH_TEST_STATUS=BLOCKED
```

### 17.1 bench 基础设施异常与修复

`[实板测量]` 第一轮新诊断命令无响应时，调试器捕获的 PC 位于
`vApplicationStackOverflowHook`，任务名为 `app_shell_task`；调用链经过
`vTaskSwitchContext`/`PendSV_Handler`。这不是 DRV8311H 过流，也不是电机卡死证据。

`[无板验证]` 先增加 bench 栈契约并观察到专项测试失败，再仅对 bench 将
`TM_APP_TASK_STACK_SHELL_BYTES` 从 1536 B 提高到 2048 B。修复后专项测试、全量
host/CTest 和 bench 构建通过，串口 trace、theta0 和固定矢量命令恢复响应。产品
full-diag、standard、tiny 的栈配置没有继承该 bench-only 变更。

### 17.2 当前实板推导链

`[实板测量]` 栈修复后的 F0–F6 固定矢量均在约 20 ms 窗口完成，三相峰值约
`0.010–0.031 A`，没有单轴长期异常或 `FAULT_OVER_CURR`。这使“固定矢量就已经
存在单相 ADC/PWM/绕组灾难”的假设暂不成立，但电流仍只是板上 ADC/CSA 换算值。

`[实板测量]` theta0 矩阵使用 `-3.14159/-1.5708/0/+1.5708/+3.14159 rad`，峰值
约 `0.009–0.028 A`，没有可重复的优胜偏置。严格边界输入 `-3.1416` 被拒绝，
因为它略小于精确 `-π`；这属于命令参数边界，不是电机故障。

`[实板测量]` V1–V4 使用 `vq=0.10/0.20/0.30/0.40 V`、`we=3.14 rad/s`、
约 20 ms。峰值分别约为 `0.027/0.019/0.016 A`、`0.036/0.032/0.019 A`、
`0.056/0.045/0.016 A`、`0.067/0.056/0.016 A`；VBUS 保持约 `11.88–11.91 V`，
未触发故障。V3/V4 已超过当前拟定的 50 mA 延长窗口准入线，所以没有刷写或执行
200 ms 实机镜像。

这些结果确认相位入口软件路径在板上被执行，但没有复现用户报告的“高电流卡死”。
当前不能从“theta_sim 正常”直接推导机械旋转已验证，也不能在没有外部仪器时把
软件电流当作独立相电流。下一步必须先补充可区分机械转矩、实际功率波形和测量链
可信度的证据，不能用提高电压、放宽阈值或开启 I-f 替代证据。

本轮原始记录：`tmp/bench/20260829_133853_next_root_cause_final/`。

## 18. 2026-08-30 分层正交诊断的推导与验收边界

### 18.1 为什么必须正交改变变量

`[静态推导]` 低压开环卡死至少有五条互相可伪装的路径：控制角度错误、PWM channel
错配、ADC rank 错配、采样窗口不在 CSA 稳定区间、以及功率级/机械转矩不足。若同时改
相序、角度、PI 和电机参数，任何改善都不能归因。本轮把变量拆成：

```text
编码器静态 -> ADC 零点 -> PWM map -> ADC map -> ADC trigger -> theta0 -> vq/we
```

每一层只改变一个实验量，并在停止后读取同一组 `raw/current/VBUS/theta/duty/fault`
字段，才能形成最小可比较证据。

### 18.2 trace 能回答和不能回答的问题

- `[源码确认]` TMR1 `CVAL`、`CCR1..4`、ADC 状态和 FOC 定点字段来自同一次软件 trace；可
  用于判断软件计算是否连续、map setter 是否落到预期寄存器和 ADC 读数是否随触发点变化。
- `[无板验证]` host tests 覆盖 64 条环形覆盖、定点边界、map/trigger 合法性、停止读取
  门禁、角度/duty/VBUS 数学边界和产品编译隔离。
- `[实板未验证]` trace 不采样 MCU 引脚，不知道 DRV8311H 实际高低侧栅极状态，不知道
  电机端瞬时相电流，也不能证明 `deadtime=0` 在物理器件上安全。

因此诊断结果必须分开写成“软件观察通过/物理操作未验证”，不能把 trace 中的 `ia_mA`
当作独立电流计结果。

### 18.3 当前实现后的实验决策树

1. 无 PWM 时 raw-offset 异常：停在 CSA/CSAREF/ADC/供电层。
2. 固定矢量只有一个 PWM map 正常：停在 channel/相序层。
3. PWM map 正常而只有一个 ADC map 平衡：停在 rank/电流解释层。
4. current 随 CH4 compare 变化：停在采样窗口/CSA 建立层。
5. 固定矢量正常、相位偏置存在稳定优胜：才考虑电角度零点。
6. theta/duty 连续且电流低但转子不跟随：转入电压/转矩/机械/参数假设，不放宽保护。

当前仍未完成这套新矩阵的完整物理结果；首个脉冲不能让最终状态从 `UNRESOLVED` 改为
`IDENTIFIED`。下一轮脚本预检若发现板上没有 `diag map show` 或 encoder trace marker，
应归类为“镜像不匹配”，不能归类为硬件故障。

### 18.4 2026-08-30 当前镜像首个映射脉冲

- `[实板测量]` 显式刷写 bench50 ELF
  `build/fw-full-diag-carrier-bench-debug/output/falcon-foc-at32.elf`，SHA256 为
  `7CFBDED0B667192AA6410788C41B0CD2BCF3D724D3F48848E1B5E76E4D139B9C`；只读预检
  返回 `axis=idle`、`fault=0`，并确认 `map`/`enc_trace` 命令存在。
- `[实板测量]` 无 PWM 静态读数为 VBUS 约 `11.91 V`，duty `0/0/0`，三相软件电流
  `-0.002/-0.001/-0.012 A`，没有静态过流证据。
- `[实板测量]` 首个 `pwm=0, adc=0, adc_trig=3740, dp0=0.05 V` 脉冲的 trace
  记录 duty `0.498/0.502/0.502`、VBUS `11.89-11.92 V`、软件电流峰值
  `0.019/0.012/0.013 A`，未触发过流。
- `[实板测量]` 同一脉冲停机后锁存 `FAULT_COMM_ERROR (0x100)`；trace 保留 `250`
  个 FOC 样本，首故障上下文为 `theta_sim=0`、`theta_elec≈0.67`、`vd=0.05 V`。
- `[静态推导]` 250 个样本与 bench50 的 50 ms 能量护栏相符，说明脚本名义上的
  5 ms 脉冲不能直接等同于板端实际带电时长；`state disable` 未在护栏触发前完成
  的可能性必须单独处理。这是诊断执行链的停机时序问题，不足以解释电机物理卡死。
- `[实板未验证]` 本轮没有完成完整 6×6 映射、触发矩阵、相位矩阵、机械整圈编码器
  扫描、PWM 引脚波形、独立相电流或温度测量，因此不能升级根因状态。

当前状态保持：

```text
OPEN_LOOP_SOFTWARE_PATH=PASS
MOTOR_PHYSICAL_OPERATION=UNVERIFIED
OPEN_LOOP_ROOT_CAUSE_STATUS=UNRESOLVED
REAL_BENCH_TEST_STATUS=BLOCKED
```

## 19. 2026-08-30 开环测试包线与故障判据校正

### 19.1 保护阈值的依据

`[原理图确认]` DRV8311H 的 GAIN 配置为 `0.5 V/A`，CSAREF 由 `DRV_3V3` 提供；
`[资料确认]` 2804 电机参考资料给出 `0.5 A` 额定电流和 `2.0 A` 最大电流。由此，
原先 `0.20 A` 快速、`0.10 A` 预警和 `0.12 A` 堵转阈值会在正常的启动和软件测量
噪声范围内过早打断有意义的开环验证。

当前代码采用临时、可回溯的诊断包线：

| 层级 | 当前值 | 作用 |
| --- | ---: | --- |
| 每相快速绝对停机 | `1.00 A` | ADC 快速路径首个有效样本触发，仍优先关 PWM |
| 持续电流停机 | `0.75 A / 3000 ms` | 任务路径保护持续高电流，适用于包括开环在内的运行模式 |
| 闭环堵转停机 | `0.75 A / 200 ms` 且 `<=5 rpm` | 仅闭环控制模式；不适用于电压开环固定矢量或旋转矢量 |

`[静态推导]` `1.00 A` 是额定值的 2 倍且低于电机资料的 2 A 最大值；它不是 DRV
硬件 OCP 的替代，也不是电机热安全认证。持久化的更小 `i_peak` 仍会进一步降低快速
门限。bench 快速路径不再把 `0.75 A` 预警电流在 3 个 ADC 样本后伪装成过流，绝对
快速停机和 3 s 持续停机仍保留。

### 19.2 开环速度和测试时长

`[静态推导]` 该电机为 7 对极，机械转速与电气角速度的换算为：

```text
we_elec = 2π × 7 × rpm_mech / 60
rpm_mech = we_elec × 60 / (2π × 7)
```

因此 `we=3.14 rad/s` 只有约 `4.29 rpm` 机械速度；在 5 ms 内角度只变化约
`0.016 rad`，不能作为“电机连续旋转”的测试。当前 bench 入口将首轮有意义的低速
候选固定为 `30 rpm`，对应约 `21.99 rad/s`，并将电压开环窗口固定为至少 `400 ms`，
覆盖一个约 `285.7 ms` 的电周期。固定矢量、映射和触发实验仍可使用 5–20 ms，因为它们
验证的是电气响应和采样关系，不是机械转速。

`[静态推导]` 当前 `DEFAULT_VOLT_WE_ACCEL=60 rad/s²`，从零加速到
`21.99 rad/s` 约需 `367 ms`；因此 400 ms 窗口主要覆盖启动过渡，并不等于已经达到
稳态旋转。若后续需要观察稳态，只能在前级短脉冲无异常后另行使用受限 bench-long
窗口，不能把 400 ms 结果表述为产品级连续运行证明。

脚本因此区分：

```text
Mapping/Trigger/Phase: 5–20 ms + bench50/bench200
Voltage: 30 rpm default, 21.99 rad/s, 400 ms + bench400
```

`[实板未验证]` 400 ms 镜像只是获得足够观测时间，不代表已经验证电机能跟随、实际
PWM 波形、死区、外部相电流或热稳定性；在当前代码验证完成前不能据此延长连续运行。

### 19.3 当前首个实机脉冲的边界

`[实板测量]` 2026-08-30 首个 `dp0=0.05 V` 脉冲在软件 trace 中只有约
`0.019/0.012/0.013 A` 峰值，VBUS 约 `11.89–11.92 V`，但最终进入 `FAULT_COMM_ERROR`
并保留约 250 个样本；这与 bench50 的 50 ms 护栏一致。该事件说明名义 5 ms 与板端
停止时序不等价，不能解释为电机过流或 DRV 故障，也不能作为完整矩阵证据。

当前状态不升级：

```text
OPEN_LOOP_SOFTWARE_PATH=PASS
MOTOR_PHYSICAL_OPERATION=UNVERIFIED
OPEN_LOOP_ROOT_CAUSE_STATUS=UNRESOLVED
REAL_BENCH_TEST_STATUS=BLOCKED
```

## 21. 2026-08-30 当前镜像方向 A/B 与 30 rpm 电压窗口

### 21.1 诊断边界

`[源码确认]` bench-only `diag set enc_dir 0|1` 只在 IDLE 状态修改 RAM 中的
`enc_pri.config->dir`，不写 Flash，不修改 `DEFAULT_DIR`、相序、PID、保护阈值或电机
参数。`DIR_CW` 使用原始计数，`DIR_CCW` 对编码器计数镜像；产品默认方向保持不变。

`[源码确认]` `state disable` 后 encoder trace 被冻结，FOC trace 记录的是最后一段
运行窗口，encoder trace 记录的是停机后保留的最后 encoder 样本。两者不能按相同时间戳
直接拼接，但可以分别判断“运行期控制场”和“停机后编码器尾段”。

### 21.2 实板证据

本轮当前镜像：

```text
ELF: build/fw-full-diag-carrier-bench-400ms-debug/output/falcon-foc-at32.elf
SHA256: 59B78D648511926F8AD47213CA35E49C0D536697BE814B1B77AF64F84E949A03
TM_BENCH_DIAG=1, TM_FEATURE_VOFA=0, FOC_ENERGIZED_CALIB_ENABLE=0
TM_MOTOR_BENCH_RUN_BUDGET_MS=400
```

`[实板测量]` 静止 `vq=0.50 V, we=0` 的四轮方向 A/B 中，CW (`enc_dir=0`) 的峰值为
`0.017/0.039/0.020 A`、`0.110/0.054/0.095 A`，CCW (`enc_dir=1`) 的峰值为
`0.085/0.123/0.098 A`、`0.117/0.126/0.077 A`。VBUS 约 `11.88–11.93 V`，没有
软件故障。

`[实板测量]` 正确速度定义下 `we=21.9911 rad/s`（7 对极约 30 rpm）进行 350 ms
短脉冲：

| 方向 | `vq` | 软件 trace 峰值 `ia/ib/ic` | `theta_sim` | `theta_elec` | 观察 |
| --- | ---: | --- | --- | --- | --- |
| CW | 0.25 V | 0.051/0.062/0.037 A | 4.399→4.616 | 4.173→4.161 | 场推进，encoder 尾段近似静止 |
| CCW | 0.25 V | 0.070/0.059/0.037 A | 1.890→2.105 | 4.047→3.758 | 场推进，encoder 有小幅变化 |
| CW | 1.00 V | 0.286/0.272/0.190 A | 2.741→5.179 | 2.758→4.497 | 无故障，未证明机械同步 |
| CCW | 1.00 V | 0.275/0.289/0.259 A | 0.515→3.103 | 0.502→0.616 | 无故障，末段仅小幅跟随 |

上述电流是 MCU ADC/CSA 换算值，不是独立相电流计读数。方向组之间的物理起始角度未
用机械夹具锁定，因此 CW 电流较低、CCW 电流较高只能作为候选信号，不能据此修改
产品默认方向。

### 21.3 根因判断

当前证据支持以下分层结论：

1. `[无板验证]` FOC 角度推进、SVPWM duty 边界和 30 rpm 电气速度单位路径能够运行。
2. `[实板测量]` 0.25 V 下电流较低但未观察到可靠机械跟随；1.00 V 下电流上升到
   `0.19–0.29 A`，仍未获得稳定 30 rpm 证据。低压不转不能直接归因于软件角度错误。
3. `[实板测量]` 方向切换改变了电流和 encoder 尾段，但没有在相同机械位置下形成两次
   同条件的唯一优胜；`ENCODER_DIRECTION_CANDIDATE=SUPPORTED_NOT_CONFIRMED`。
4. `[实板测量][静态推导]` 既有 CH4 compare 扫描仍显示软件电流随触发位置明显变化，
   因而 ADC/CSA 建立窗口是独立候选，不能与方向候选合并为单一根因。
5. `[实板未验证]` 没有实际 PWM 引脚波形、死区/重叠、SOx 模拟波形、独立相电流、
   温度、机械转速和手动 encoder 整圈基准；物理电机运行仍未闭环证明。

本轮不执行软件默认方向、相序、ADC rank、FOC 参数或保护阈值修改。当前状态：

```text
ENCODER_DIRECTION_CANDIDATE=SUPPORTED_NOT_CONFIRMED
ADC_MEASUREMENT_WINDOW=ANOMALOUS_OR_STRONGLY_SENSITIVE
OPEN_LOOP_SOFTWARE_PATH=PASS
MOTOR_PHYSICAL_OPERATION=UNVERIFIED
OPEN_LOOP_ROOT_CAUSE_STATUS=UNRESOLVED
REAL_BENCH_TEST_STATUS=BLOCKED
```
