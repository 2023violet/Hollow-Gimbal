# FalconFOC V2 G3-SAFE-HW01｜AT32F415 硬件安全链资源审计

- 审计日期：2026-11-10
- 工作树基线：`1f542521189d5e0ef25e64e6530608ece8d6e137`
- 范围：Board1 原理图、网表、BOM、DRV8311H 数据手册、AT32F415 生成式外设配置、厂商 SDK 和现有 Legacy 调用链的只读交叉审计。
- 本轮没有修改 `applications/`、`platform/`、`bsp/`、`subsys/`、`components/`、根 CMake、Legacy ISR、WDT 配置或板级寄存器。
- 本轮没有启动调试器、刷写固件、发送 UART 命令、使能 PWM、给板子上电或执行电机测试。

## 1. 结论等级

本记录只使用以下证据标签：

- `source-confirmed`：当前工作树源码、生成配置或网表直接给出事实。
- `host-verified`：主机测试或确定性模型已经执行并通过。
- `register-only`：SDK 或寄存器定义存在，但没有证明当前板级连接和运行时拥有者。
- `unpowered-board-verified`：已在不上功率的实板上观察到结果。
- `powered-board-verified`：已在受控带电实板上取得仪器证据。
- `not-available`：当前没有足够证据，不能把能力写入安全承诺。

HW01 的总体状态是：**资源审计完成，硬件安全链设计未放行**。

最重要的新增结论是：生成式 TMR1 初始化在应用安全状态建立以前已经打开主输出和六个通道。因此，当前工作树不能仅凭 `pwm_out_disable()` 和状态机文档宣称“复位后默认禁止功率输出”。物理桥臂在该时间窗口的真实状态仍需无功率/示波器证据确认。

## 2. 证据来源

| 来源 | 用途 | 当前证据等级 |
| --- | --- | --- |
| [`docs/pcb docs/pcb原理图.pdf`](<../pcb docs/pcb原理图.pdf>) | Board1 主原理图和 U4 连接意图 | `source-confirmed` |
| [`docs/pcb docs/pcb 网表.tel`](<../pcb docs/pcb 网表.tel>) | `INH*`、`INL*`、`nFAULT`、`nSLEEP`、`SOA/SOB/SOC`、`VBUS` 的实际逻辑连接 | `source-confirmed` |
| [`docs/pcb docs/pcb bom.xlsx`](<../pcb docs/pcb bom.xlsx>) | R6/R7/R10/R11/R12/R13、D2 等装配候选 | `source-confirmed`，不等于已焊接 |
| [`docs/pcb docs/drv8311.pdf`](<../pcb docs/drv8311.pdf>) | DRV8311H 接口、保护、6x PWM、CSA 和时序 | `source-confirmed` |
| [`docs/code-wiki/hardware-and-drv8311h.md`](../code-wiki/hardware-and-drv8311h.md) | 已完成的原理图、网表、BOM、数据手册和代码对应整理 | `source-confirmed`，无板结论仍受原文边界约束 |
| [`platform/at/at32f415/generated/src/main.c`](../../platform/at/at32f415/generated/src/main.c) | 低层初始化顺序 | `source-confirmed` |
| [`platform/at/at32f415/generated/src/wk_tmr.c`](../../platform/at/at32f415/generated/src/wk_tmr.c) | TMR1 六路 PWM、CH4、Break 和 deadtime 配置 | `source-confirmed` |
| [`platform/at/at32f415/generated/inc/at32f415_conf.h`](../../platform/at/at32f415/generated/inc/at32f415_conf.h) | WDT/WWDT 模块编译开关 | `source-confirmed` |
| [`platform/at/at32f415/vendor/drivers/inc/at32f415_wdt.h`](../../platform/at/at32f415/vendor/drivers/inc/at32f415_wdt.h) | WDT SDK 能力和寄存器字段 | `register-only` |
| [`platform/at/at32f415/vendor/drivers/inc/at32f415_wwdt.h`](../../platform/at/at32f415/vendor/drivers/inc/at32f415_wwdt.h) | WWDT SDK 能力、窗口和 PCLK1 分频 | `register-only` |
| [`applications/periph/motor_drv.c`](../../applications/periph/motor_drv.c) | `pwm_out_enable()`、`pwm_out_disable()` 和相序映射 | `source-confirmed` |
| [AT32F415 官方 Reference Manual](https://www.arterytek.com/download/RM/RM_AT32F415_V2.05_EN.pdf) | WDT 的 LICK 时钟、WWDT 主时钟、系统复位和 TMR1 Break 复用 | `source-confirmed` |
| [AT32F415 官方 Datasheet](https://arterytek.com/download/DS/DS_AT32F415_V2.02_EN.pdf) | QFN32 PA6 的 `TMR1_BRK` 复用和封装引脚定义 | `source-confirmed` |

## 3. Board1 功率级拓扑

### 3.1 DRV8311H 的 MCU 连接

网表确认 U4 `DRV8311HRRWR` 的六个逻辑输入全部连接到 AT32F415：

| DRV8311H 输入 | Board1 网络 | AT32F415 当前定义 |
| --- | --- | --- |
| `INHA` / `INLA` | `INHA` / `INLA` | TMR1 CH3 / CH3C |
| `INHB` / `INLB` | `INHB` / `INLB` | TMR1 CH2 / CH2C |
| `INHC` / `INLC` | `INHC` / `INLC` | TMR1 CH1 / CH1C |

`at32f415_wk_config.h` 还记录了原理图网络和定时器通道交叉的板级事实。后续安全代码不能把 `CH1=A、CH2=B、CH3=C` 当作默认假设。

AT32F415 官方 Datasheet 将 QFN32 的 PA6 列为 `TMR1_BRK` 复用脚。当前生成配置把 PA6 定义为 `IA`，网表也把 U3.12 接到 IA 电流采样网络。因此，即使芯片内部有 TMR1 Break 功能，当前 Board1 也不能在保留 A 相电流采样的同时把 DRV `nFAULT` 接到这个 Break 输入。要获得这条路径，需要改板或放弃该 ADC 输入并重新证明采样链。

### 3.2 驱动器故障和睡眠引脚没有 MCU 端点

网表给出：

- `NFAULT` 只连接 U4 pin 1、R10 上拉到 `DRV_3V3`、R7 和 ESD 器件。它没有连接到 U3 任意 GPIO、ADC、EXTI 或 TMR1 Break 输入。
- `NSLEEP` 只连接 U4 pin 24、DCBUS 经 R6 的上拉、R7、D2 和 C9。它没有连接到 U3 GPIO。
- U4 H 版本没有可由当前 `SPI1` 读取的诊断寄存器。当前 `SPI1` 的板级拥有者是编码器 U5。

因此当前 Board1 的软件安全监督无法读取 DRV 内部 OCP、UVLO、OTSD 或启动过渡，也无法由 MCU 主动把 U4 拉入睡眠。DRV 内部保护仍然存在，但属于 MCU 不可观测的外部保护层。

R7 确实把 `NFAULT` 与 `NSLEEP` 连接在同一电阻网络中，因此不能把两条网络描述成完全电气隔离。现有静态模型显示，R6=3.3 kΩ 的 DCBUS 上拉和 D2 钳位可能压过 R7=10 kΩ 的下拉作用。当前仍不能仅凭网表证明 `nFAULT=0` 会把 `nSLEEP` 拉到 DRV8311H 的 Sleep 阈值，也不能把这条通路写成已经闭合的外部关断路径。必须在明确供电条件后计算并测量节点电压。

结论：`nFAULT` 和 `nSLEEP` 当前均为 `not-available` 的软件安全输入/执行路径。R7 通路属于待电气复核的板级关系，不是 MCU 可观测输入，也不是已经证明的独立关断路径。PA6 的 TMR1_BRK 复用被 IA 采样占用，不能把 SDK 的 Break 位字段当成当前板上的独立故障关断路径。

### 3.3 Board1 电源依赖

网表显示 `DCBUS` 同时进入 U4 的 `VIN_AVDD`、`VM` 和母线检测分压。U4 的 `AVDD` 输出形成 `DRV_3V3`，而 `DRV_3V3` 又连接 U3、U5、CSAREF 和故障上拉网络。因此，完全断开 DCBUS 时不能假设 U3 仍然能够启动。

外部单独注入 3.3 V 也不能直接作为 HW02 测量前提。必须先审查向 U4 AVDD、CSAREF、nFAULT、nSLEEP 和其他电源网络反向供电的路径。没有完成这项审查前，HW02 只允许完成文档和计算准备。

## 4. TMR1 PWM 和启动时序审计

### 4.1 生成式 TMR1 配置

`wk_tmr1_init()` 当前执行的关键动作如下：

| 配置项 | 当前值 | 证据 |
| --- | --- | --- |
| 计数模式 | `TMR_COUNT_TWO_WAY_1` | `source-confirmed` |
| 周期 | `ARR=3749` | `source-confirmed` |
| CH1/2/3 主输出 | `oc_output_state=TRUE` | `source-confirmed` |
| CH1C/2C/3C 互补输出 | `occ_output_state=TRUE` | `source-confirmed` |
| CH1/2/3 比较值 | 初始化为 `0` | `source-confirmed` |
| TMR1 主输出使能 | `tmr_output_enable(TMR1, TRUE)` | `source-confirmed` |
| TMR1 计数器 | `tmr_counter_enable(TMR1, TRUE)` | `source-confirmed` |
| Break | `brk_enable=FALSE` | `source-confirmed` |
| 自动输出 | `auto_output_enable=FALSE` | `source-confirmed` |
| MCU deadtime | `deadtime=0` | `source-confirmed` |
| CH4 | `CCR4=3740`，用于 CC4 软件触发 ADC | `source-confirmed` |

AT32F415 SDK 确实暴露 `brken`、`brkv`、`aoen`、`oen` 和 `brkif` 字段，但这只证明定时器寄存器和驱动 API 存在。当前生成配置没有启用 Break，也没有证明 Board1 有外部 Break 信号接入。

### 4.2 复位到应用的时间窗口

`generated/src/main.c` 的顺序是：

```text
wk_gpio_config()
-> wk_adc1_init()
-> DMA/USART/SPI 初始化
-> wk_tmr1_init()
-> wk_tmr5_init()
-> wk_tmr3_init()
-> app_main_bootstrap()
```

`wk_gpio_config()` 当前只显式初始化 SPI1 CS。六个 PWM 引脚是在 `wk_tmr1_init()` 中切换为复用输出。此时 TMR1 的主输出、六个通道和计数器已经打开，随后才进入 `app_main_bootstrap()`、RTOS 初始化、`motor_task_init()` 和最终的 `motor_fsm_init()`。

这得到一个必须单独处理的 P0：

> 当前源码没有证明从 MCU 复位释放到应用安全状态建立期间，六个 DRV 输入始终处于禁止状态。

这不是说实板一定已经产生危险功率输出。比较值为零、DRV8311H 逻辑、复位引脚默认态和桥臂波形仍需实测。可以确定的是，应用层 `pwm_out_disable()` 尚未成为复位后的第一道硬件门禁。

### 4.3 “禁止输出”和“零占空比”是不同动作

`applications/periph/motor_drv.c` 中：

- `pwm_out_disable()` 将 CH1、CH1C、CH2、CH2C、CH3、CH3C 的 channel enable 位清零。
- `pwm_out_enable()` 先把 CH1/2/3 比较值写为零，再重新打开六个 channel enable 位。

现有 `motor_fsm.c` 将产品 IDLE 的零占空比解释为低侧夹紧/主动短路，将 FAULT 的 `pwm_out_disable()` 解释为桥臂 Hi-Z。这个语义必须被 G3-SAFE 保留并显式化：

| 安全动作 | 允许的硬件映射 | 用途 |
| --- | --- | --- |
| `INHIBIT` | 关闭六个通道，必要时再清除 TMR1 主输出使能 | 上电、故障、样本无效、最终提交失败 |
| `ZERO_DUTY` | 保持通道启用，比较值为零 | 仅在明确需要主动制动且已验证功率级行为时使用 |
| `ACTIVE_BRAKE` | 专门的制动策略 | 不能由 `INHIBIT` 隐式替代 |

HW01 的默认安全动作确定为 `INHIBIT`。不能用 `ZERO_DUTY` 代替桥臂禁止，也不能把 mock 中的零 compare 值当成物理输出关闭证明。

### 4.4 `CCR=0` 的输出状态仍需解析

当前源码能够确认三个比较寄存器被写成零、主输出和互补输出被打开，但不能只凭 `CCR=0` 直接断言 DRV 输入组合。最终输入电平还取决于 TMR1 的比较输出模式、主/互补极性、中心对齐计数方向、通道使能和主输出使能。

DRV8311H 6x PWM 真值表中，`INH=0、INL=1` 是低侧导通，`INH=0、INL=0` 和 `INH=1、INL=1` 才是 Hi-Z。HW02-A 必须先从 TMR1 寄存器字段推导 `CCR=0` 对应的实际六路逻辑组合，HW02-B 才能用 NRST 和六路输入波形确认是否存在低边常导通、主动制动或其他非禁止状态。

## 5. WDT/WWDT 资源审计

### 5.1 当前配置事实

`generated/inc/at32f415_conf.h` 对 `WDT_MODULE_ENABLED` 和 `WWDT_MODULE_ENABLED` 都保持注释状态。生成式启动代码没有调用 `wdt_enable()`、`wdt_counter_reload()`、`wwdt_enable()` 或对应的时钟/复位初始化。当前 Legacy 路径也没有发现一个已拥有的硬件看门狗运行时。

因此 G3-SAFE R2 的 host `feed permission` 仍然只是软件决定，不是硬件看门狗能力。

### 5.2 SDK 能力

AT32F415 SDK 提供两类候选：

- WDT：12 位 reload、4 到 256 分频、独立的 enable/reload API，并有 WDT reset flag。
- WWDT：PCLK1/4096 到 PCLK1/32768 分频、7 位 counter/window、可选 reload interrupt，并有 WWDT reset flag。

官方 AT32F415 Reference Manual 已确认 WDT 由 LICK 驱动。LICK 是 30–60 kHz 的低速内部 RC，启用 watchdog 时会被强制打开。官方资料还说明 WWDT 由主时钟驱动。由此可以把“WDT 比 WWDT 更适合作为独立监督兜底”从经验判断提升为器件级设计依据，但当前工作树仍没有完成运行时配置和复位测试。

以下数据仍缺失：

- LICK 频率误差范围和目标 WDT 时钟容差。
- 最小、最大和目标 reset timeout 的计算与实测值。
- 调试暂停位对 WDT/WWDT 的影响。
- 复位期间 TMR1 主输出、GPIO 复用和 DRV 输入的默认状态。
- WDT reset flag 的采集、保留和上报路径。

### 5.3 HW01 的预选方向

第一版 Runtime 预选 **WDT**，而不是 WWDT。官方资料已确认 WDT 使用独立于系统主时钟的 LICK，而 WWDT 使用主时钟。WDT 没有窗口服务约束，能把“两个执行域都健康才允许喂狗”保持为上层监督策略，同时降低前台调度抖动造成误复位的风险。WWDT 仅在后续器件限制或产品策略明确要求时作为备选。

这仍然不是已经批准的寄存器修改。只有完成 LICK 频率容差、timeout 计算、复位默认态检查和无功率实板复位测试后，才能把 WDT 选型从 `register-only` 改成 `source-confirmed` 或 `unpowered-board-verified`。官方 Reference Manual 还规定 WDT/WWDT/软件复位会把大部分寄存器恢复为复位值，因此 TMR1 和 GPIO 的复位后状态必须单独验证。

## 6. 安全时序链

当前 Legacy 快速链是：

```text
TMR1 CC4
  -> 软件触发 ADC injected group
  -> ADC1_IRQHandler
  -> 读取 VBUS/三相 CSA
  -> fast fault check
  -> FOC 计算
  -> 写入 TMR1 CCR1/2/3
```

`CCR4=3740`、中心对齐计数和 ADC 注入序列的静态关系已在硬件 Wiki 中记录，但当前仍缺少以下可复现的物理时序证据：

1. CC4 边沿到 ADC 采样孔径的实际延迟。
2. ADC 转换完成到 H0 gate 判断的延迟。
3. H0 gate 通过后到 CCR shadow/live 更新的延迟。
4. 最终发生新故障时，TMR1 通道是否能在同一载波周期被禁止。
5. `pwm_out_disable()` 到 INH/INL、OUTA/B/C 真实状态变化的延迟。
6. DRV8311H 传播延迟、CSA 建立时间和最小脉冲约束对上述窗口的影响。

这些值不能由 host Core、G3-NP 或 G3-SAFE 测试代替。HW01 只冻结测量接口和验收顺序，不宣称已经通过。

## 7. 资源和安全决策矩阵

| 安全资源 | 当前结论 | 证据等级 | HW01 决策 | 进入硬件集成前必须补齐 |
| --- | --- | --- | --- | --- |
| TMR1 CH1/2/3 + complementary | 已配置，且启动时打开 | `source-confirmed` | 保留 Legacy 映射，禁止自行重排 | 复位到应用的禁止窗口、INH/INL 波形 |
| TMR1 Break | SDK 字段存在，QFN32 PA6 可复用，但 PA6 当前承担 IA 采样，DRV `nFAULT` 未接入 | `register-only` + `not-available` | 不得声明当前板存在硬件 Break 保护 | 明确改板级 BK 输入并重做 IA 采样资源审计 |
| 软件 channel disable | `pwm_out_disable()` 存在 | `source-confirmed` | 作为 H0 最终 gate 的候选动作 | 证明写入顺序、影子寄存器和实际关闭延迟 |
| TMR1 主输出使能 | `oen` 可用，当前初始化为 TRUE | `source-confirmed` | 评估为第二道软件关断，不替代硬件保护 | 在最终 gate 中定义是否同时清除 |
| DRV `nFAULT` | 板上有线，但不进 MCU | `source-confirmed` | 软件不能依赖它做故障闭环 | 版次修改或增加独立外部安全路径 |
| DRV `nSLEEP` | 板上有网络，但不进 MCU，nFAULT 联动不成立 | `source-confirmed` | 不能当 MCU 睡眠/急停接口 | 重新设计硬件关断网络或独立 gate |
| AT32 WDT | SDK 有 API，当前未使能；官方资料确认 LICK 30–60 kHz 时钟 | `register-only`，时钟源 `source-confirmed` | 第一版首选候选 | 频率容差、timeout、复位原因和无功率测试 |
| AT32 WWDT | SDK 有 API，依赖 PCLK1 和 window | `register-only` | 保留备选，不在 HW01 直接启用 | 只有 WDT 不满足约束时再评估 |
| 复位默认 GPIO/TMR1 状态 | 当前仓库没有完整 default-state 证据 | `not-available` | P0，不能签署启动安全 | 无功率复位捕获和芯片手册核对 |
| ADC/FOC 到 PWM 时序 | 源码链存在，物理延迟未测 | `source-confirmed` | 只允许离线模型和无功率验证 | 示波器/逻辑分析仪时序证据 |

## 8. HW01 P0 门禁

在以下三项关闭以前，不得进入 G3-SAFE 硬件安全验收，更不得进入 G3-PWR：

1. **复位启动门禁**：TMR1 在复位释放、GPIO 复用、应用启动和安全状态建立全过程中保持 `INHIBIT`，或者有等效的独立硬件门禁。
2. **故障关断门禁**：当前 Board1 没有 MCU 可见 `nFAULT`，而 QFN32 PA6 的 TMR1 Break 复用又被 IA 采样占用，生成配置也未启用 Break。必须明确这是产品边界，或提供修订板/外部硬件路径，不能把驱动器内部自动重试当成 MCU 可验证安全闭环。
3. **看门狗门禁**：冻结 WDT/WWDT 的器件级时钟、timeout、复位原因、复位期间输出状态和 feed 权限交接。Host `feed permission` 不能单独升级为硬件 watchdog 通过。

## 9. 下一轮只读工作和允许范围

### 允许

- 补齐 AT32F415 器件手册和官方示例的 WDT/WWDT 时钟、复位和 debug 行为证据。
- 生成 TMR1/ADC/PWM 时序图和寄存器级最终 gate 映射。
- 在不接功率母线的情况下观察复位释放、PWM 引脚、TMR1 状态和 WDT reset flag。
- 用 Host Mock 注入“最终安全检查后立即发生故障”的提交竞争。
- 为修订板定义 `nFAULT`、`nSLEEP`、TMR1 Break 或独立 Gate 的网络要求。
- 明确 DCBUS、DRV_3V3、U3 VDD、CSAREF、nFAULT 和 nSLEEP 的供电测量点及外部注入禁令。

### 不允许

- 修改 Legacy ISR、`wk_tmr.c`、`wk_adc.c`、WDT/WWDT 配置或 board register。
- 在当前 Board1 上声明已经存在硬件 Break、MCU 故障输入或硬件急停。
- 刷写固件、使能功率 PWM、接入电机母线、执行带电故障注入或进行 G3-PWR。

## 10. 验收状态

| 阶段 | 状态 |
| --- | --- |
| G2 Core | 保持既有本地验收状态 |
| G3-NP | 保持既有本地验收状态 |
| G3-SAFE R2 Host 策略 | 设计通过，独立源码复核仍按既有状态跟踪 |
| G3-SAFE-HW01 资源审计 | **完成，条件不满足硬件安全放行** |
| HW02-A 复位安全测试准备 | **下一阶段，批准只读准备** |
| HW02-B 实板供电与波形观测 | **待单独授权** |
| G3-SAFE 硬件安全验收 | **NOT PASSED** |
| G3-PWR | **LOCKED** |

本记录的安全判断以当前工作树和已入库硬件资料为准。它没有把历史带电记录、host 测试或生成配置的存在，扩大解释成当前板级安全能力。
