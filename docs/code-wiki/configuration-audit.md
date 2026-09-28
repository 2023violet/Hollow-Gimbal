# Board1 原理图—AT32F415 底层配置审计

> 审计日期：2026-08-29
> 审计对象：`D:/study/Software/Hollow-Gimbal` 当前工作树、`docs/pcb docs` 中的 Board1 原理图/网表/BOM、`DRV8311H` 数据手册与 AT32F415 官方参考手册。
> 首版静态审计边界：源码、网表、数据手册和主机侧数学/静态模型。本文另列 2026-08-29 的受控实板短脉冲结果；该结果不等价于产品级连续运行验收。

## 1. 结论先行

当前工程不是“所有配置都错”，而是底层映射已经形成了一条基本可工作的链路，但安全闭环和若干单一事实源仍未闭合。

### 1.1 已确认正确或基本一致的部分

- U3 `AT32F415KBU7-4` 的 PWM、ADC、SPI1、USART1 引脚与网表可以逐点对应。
- U4 `DRV8311HRRWR` 的 `MODE/SLEW/GAIN` 均由 47 kΩ 下拉，按 H 版本四电平表解码为 `6x PWM`、约 `75 V/μs`、`0.5 V/A`、典型 OCP 约 `5 A`；代码中的 `CURR_AMP_GAIN=0.5` 与之吻合。
- `PA1/PA4/PA5/PA6` 分别承担 `VBUS/IC/IB/IA`；ADC 注入序列和 `adc_read_phases()` 的 rank 映射一致。
- TMR1 使用中心对齐互补 PWM，CH1/2/3 与 A/B/C 相序的映射一致；`ARR=3749`、150 MHz 计数时钟得到约 20 kHz 载波。
- 母线分压 `100 kΩ/10 kΩ` 与 `FAC_VBUS()` 的 11:1 换算一致；SOx 的 `330 Ω + 22 pF` 也与 DRV 电流输出滤波的设计意图一致。
- ADC 采用 TMR1 CH4 触发、注入四通道、7.5 周期采样，并先做 ADC 校准，方向上已经修正了旧的自由运行采样问题。

### 1.2 确定性问题（不依赖实板即可证明）

| 等级 | 问题 | 证据结论 |
| --- | --- | --- |
| P0/P1，取决于产品接口定义 | U4 `nFAULT` 只连接到上拉、ESD 和 `nSLEEP` 电阻网络，没有 U3 端点；当前软件故障策略改由 ADC/运行量快速检查提供。 | U4 内部仍可自行关断，但 MCU 不能观察 DRV 故障原因；应用层已能对可见的相电流、VBUS、速度、温度和非法数据做软件锁存，仍没有硬件故障输入联动。 |
| P0/P1，取决于 U1/U2 用途 | 原理图的 U1/U2 连接到 `USART2_RX/TX`；启动入口注释掉 `wk_usart2_init()`，USART2 中断只清 IDLE 标志，没有 Shell/transport 消费者。 | 若 U1/U2 是产品命令或通信口，则该接口当前不可用；只有 USART1 的 PB6/PB7 测试/调试路径被实际使用。 |
| 已修正 | 原工作树曾为 `DEFAULT_CUTOFF_HZ = DEFAULT_CURR_BW_HZ * 0.1 = 25 Hz`；当前已改为 `* 5.0 = 1250 Hz`，并将采样率绑定到 10 kHz FOC step。运行时/持久化 `currbw` 现在由同一有效参数同时派生 PI 和 LPF。 | 旧值会让 Id/Iq 反馈远慢于 250 Hz 电流环；当前默认值满足 4–10 倍约束，非法/非有限 `currbw` 会被拒绝。 |
| P1，软件路径已加防护 | `foc_para_calc()` 与 `foc_ctrl_volt()` 现在先检查 `isfinite(v_bus) && v_bus > 1.0 V`，无效时清零调制量并关闭 PWM；运行时仍按 `7.4/17.0 V` 做产品欠压/过压判定。 | 零除和 NaN/Inf 传播已由软件回归测试覆盖；仍需通过实测确认 VBUS 分压、ADC 采样和故障状态迁移在真实异常下的一致性。 |

### 1.3 需要电气或版图冻结前处理的风险

- ADC 采样触发点和理论最大占空比下的低侧窗口仍需按最坏传播/建立时间验证。用当前 SVPWM 方程全角度扫掠，理想最大 duty 约 `0.8849`、rank4 结束约 `3.660 μs`，理想余量约 `2.095 μs`；同时保留 `5/6` duty 的 `0.506 μs` 作为保守下界，不能据此证明全扇区、全相电流均可靠。
- 代码把 `V_REF` 固定写成 3.3 V，而 MCU ADC 参考电源、U4 `CSAREF` 和 `DRV_3V3` 是同一供电网络；若实际电压是 3.15–3.45 V，按固定值换算会产生约 +4.76% 至 −4.35% 的比例误差。
- U4 `VIN_AVDD` 与 `VM` 均接 `DCBUS`，在 20 V 母线下 `DRV_3V3` LDO 的压差功耗必须按实际负载、电源纹波和 QFN 散热铜区核算。
- R6=3.3 kΩ 在 `DCBUS=20 V` 且 `nSLEEP` 被 D2 钳位时约耗散 84.5 mW；BOM 只说明封装/阻值，尚未证明该具体电阻的额定功率、脉冲能力和温升裕量。
- TMR1 MCU `deadtime=0` 依赖 DRV8311H 内部互锁/死区。器件级上可以成立，但当前没有 Board1 的输入、输出传播延迟、最小脉宽和版图寄生证据。

## 2. 证据和判断规则

### 2.1 资料优先级

1. Board1 的逻辑网表 `pcb 网表.tel`；用于判断“实际有哪些网络、哪些管脚确实相连”。
2. Board1 原理图 `pcb原理图.pdf`；用于理解设计意图和电源/接口分组。
3. BOM `pcb bom.xlsx`；用于判断元件是否列入装配清单，但不证明已经焊接或 PCB 走线正确。
4. U4 的 TI 数据手册 `drv8311.pdf`；用于 H 版本模式、时序、电流采样、保护和封装边界。
5. 当前 `platform/at/at32f415` 生成代码和 `applications`；用于判断 MCU 寄存器配置、调用边界和实际运行路径。
6. AT32F415 官方参考手册；用于解释中心对齐计数、互补输出、ADC 触发和 ADC 转换时序。可参见 [AT32F415 官方参考手册](https://www.arterytek.com/download/RM/RM_AT32F415_V2.05_EN.pdf)。

### 2.2 结论标签

| 标签 | 含义 |
| --- | --- |
| `[网表事实]` | 可在当前 `pcb 网表.tel` 中直接找到。 |
| `[代码事实]` | 可在当前源码/生成配置中直接找到。 |
| `[计算验证]` | 由网表和代码常数在主机侧重算得到。 |
| `[确定性问题]` | 由静态连接或调用链即可证明，不需要上板。 |
| `[风险]` | 已发现边界，但是否在产品条件下失效还需要更多电气、版图或动态证据。 |
| `[未验证]` | 本任务明确不使用实板，因此不能宣称波形、热、EMI、电机或通信实际通过。 |

## 3. 原理图到 MCU/DRV 的总数据流

```text
DCBUS
 ├─> U4 VM / VIN_AVDD
 ├─> D1 TVS
 ├─> R14/R15 -> VBUS -> U3 PA1 / ADC1_IN1
 └─> R6 -> NSLEEP；D2 钳位；R7 连接 NFAULT

U4 AVDD -> DRV_3V3
 ├─> U3 电源与 ADC 参考供电
 ├─> U5 电源
 ├─> U4 CSAREF
 └─> R10 -> NFAULT 上拉

U3 TMR1 CH1/2/3 + complementary
 └─> INHC/INHB/INHA、INLC/INLB/INLA -> U4

U4 SOA/SOB/SOC
 └─> 330 Ω + 22 pF -> IA/IB/IC -> U3 PA6/PA5/PA4

TMR1 CH4 compare
 └─> ADC1 injected trigger -> VBUS, IC, IB, IA -> ADC1_IRQHandler

U3 SPI1
 └─> PB3/PB4/PB5 + PA15 CS -> U5 编码器
```

这个图能说明信号被如何传递，但不能替代 PCB 版图。当前资料没有 Board1 的 PcbDoc、Gerber、层叠、铜厚、U4 焊盘热过孔和走线证据，因此功率回路、AGND/PGND 回流、SOx 串扰和散热仍是 `[未验证]`。

## 4. U4 电源、四电平和故障网络

### 4.1 电源连接

网表显示：

- `DCBUS` 同时连接 U4 pin 7 `VIN_AVDD` 和 pin 8 `VM`；
- U4 pin 17 `AVDD` 输出 `DRV_3V3`；
- U4 pin 2 `CSAREF` 也在 `DRV_3V3` 网；
- U4 pin 6 `CP` 由 C10=100 nF 接回 `DCBUS/VM`；
- VM 侧有 C11/C12=10 μF 和 C13=100 nF；AVDD 侧有 C7=4.7 μF，另有共享 100 nF 去耦；
- U4 pin 9 `PGND`、pin 16 `AGND` 和裸焊盘 pin 25 都进入逻辑 `GND` 网。

从器件功能看，`VIN_AVDD=VM=DCBUS` 在推荐输入范围内是允许的；真正的代价是 AVDD LDO 要承受完整的母线压差：

```text
P_LDO ≈ (DCBUS - 3.3 V) × I_DRV_3V3
```

例如仅作量级计算：

| DCBUS | 假设 DRV_3V3 负载 | LDO 粗估功耗 |
| ---: | ---: | ---: |
| 12 V | 20 mA | 0.174 W |
| 20 V | 20 mA | 0.334 W |
| 20 V | 50 mA | 0.835 W |
| 20 V | 100 mA | 1.670 W |

这些数值不是结温结论。实际还要加入 MCU/U5/ESD/上拉电流、开关损耗、封装热阻、铜区和环境温度。数据手册的 `AVDD` 最大供电能力不是“可以无条件把任何外部负载接在 AVDD 上”的许可。

### 4.2 H 版本四电平解码

R11/R12/R13 都是 47 kΩ 对 `GND`：

| U4 管脚 | 网表 | 实际 strap | H 版本解码 | 代码对应 |
| ---: | --- | --- | --- | --- |
| 21 | GAIN | 47 kΩ 下拉 | 0.5 V/A | `CURR_AMP_GAIN=0.5f` |
| 22 | SLEW | 47 kΩ 下拉 | 75 V/μs | 无寄存器代码；固定硬件 |
| 23 | MODE | 47 kΩ 下拉 | 6x PWM、OCP 约 5 A typ | TMR1 提供六路逻辑 PWM |

这些输入不是普通运行时 GPIO。H 版本会在上电/唤醒时锁存模式，因此软件不能通过当前网表改变它们；不能将 DRV8311S/P 的 SPI/tSPI 寄存器能力套用到当前 U4。

### 4.3 nFAULT/nSLEEP 的等效电路和实际结果

网表给出的网络是：

```text
DCBUS -- R6 3.3k --+-- NSLEEP -- U4 pin24
                   |
                   +-- R7 10k -- NFAULT -- U4 pin1
                   |
                   +-- D2 3.3V 齐纳 -- GND
                   +-- C9 100nF -- GND

NFAULT -- R10 10k -- DRV_3V3
```

静态推导：

1. `nFAULT` 是 U4 的开漏低有效输出，R10 上拉到 `DRV_3V3`，这一部分连接方式正确。
2. U4 把 `nFAULT` 拉低时，R7 只是给 `nSLEEP` 增加一个到低电位的 10 kΩ 支路。
3. R6=3.3 kΩ 从 DCBUS 上拉，且 D2 会把 `nSLEEP` 钳在约 3.3 V；因此在 7.4–20 V 工作母线范围内，`nSLEEP` 仍高于 U4 的逻辑 `VIH`，不会因为 `nFAULT=0` 自动进入睡眠。
4. 当前没有 U3 GPIO 连接到这两个网络，所以固件既不能主动拉低 `nSLEEP` 做复位/休眠，也不能读取 `nFAULT`。

因此，“U4 内部保护会动作”与“系统软件知道并锁存故障”是两件事。当前前者可能存在，后者静态上不成立。

在 `DCBUS=20 V`、`nSLEEP≈3.3 V` 时，R6 的计算为：

```text
I_R6 ≈ (20 - 3.3) / 3.3k ≈ 5.06 mA
P_R6 ≈ I_R6² × 3.3k ≈ 84.5 mW
```

这要求检查 R6 的实际功率等级和封装温升；R6 是 0402/高功率特殊型号还是普通 1/16 W 料号，当前 BOM 证据不足以关闭该风险。

## 5. TMR1 六路 PWM 与 DRV 输入边界

### 5.1 引脚和相序对应

生成头文件与网表的对应关系为：

| 电机相 | U4 高侧输入 | U4 低侧输入 | U3 管脚 | TMR1 通道 |
| --- | --- | --- | --- | --- |
| A | `INHA` | `INLA` | PA10 / PB1 | CH3 / CH3C |
| B | `INHB` | `INLB` | PA9 / PB0 | CH2 / CH2C |
| C | `INHC` | `INLC` | PA8 / PA7 | CH1 / CH1C |

网表和生成配置的方向一致：

- `INHA/INLA` 进入 U4 pin 15/pin 18；
- `INHB/INLB` 进入 U4 pin 14/pin 19；
- `INHC/INLC` 进入 U4 pin 13/pin 20；
- `motor_drv.c` 在 `PHASE_ORDER_ABC` 下把 `dtc_a/dtc_b/dtc_c` 写入 CH3/CH2/CH1。

因此，代码中的“逻辑 A/B/C”和 U4 的物理相位命名没有明显交换错误。若实际电机线束或编码器零点方向不同，仍需通过相位/电角度试验确认，不能只凭 MCU 管脚表证明闭环方向正确。

### 5.2 TMR1 计数和载波

当前配置：

```text
系统时钟       = 150 MHz
APB1/APB2      = 75 MHz
定时器时钟     = 150 MHz（APB 分频非 1 时，定时器时钟倍频）
TMR1 DIV       = 0
TMR1 PR/ARR    = 3749
计数模式       = TMR_COUNT_TWO_WAY_1（中心对齐上下计数）
```

主机侧计算：

```text
T_carrier ≈ 2 × 3749 / 150 MHz = 49.9867 μs
f_carrier ≈ 20.005 kHz
```

AT32F415 参考手册说明中心对齐模式 1 的计数器上下交替计数，通道事件只在下数阶段置位相应通道标志；PWM 模式 A 的主输出与互补输出再经通道极性、互补使能和硬件输出使能组合。当前 CH1/2/3 均配置主输出和互补输出、两侧 active-high，形成给 DRV 的互补逻辑对。参考：[AT32F415 官方参考手册](https://www.arterytek.com/download/RM/RM_AT32F415_V2.05_EN.pdf)。

### 5.3 MCU deadtime=0 的边界

`wk_tmr.c` 把 TMR1 break 关闭、`deadtime=0`、TMR1 OEN 打开；U4 的 H 版本负责自己的功率级握手和保护。这样配置并非语法错误，但它把上下管互锁的绝大部分责任交给 U4：

- 优点是不会在 MCU deadtime 和 DRV 内部死区之间重复叠加过大死区；
- 风险是不能将 `deadtime=0` 解释成“整个板级没有直通风险”；
- 当前 `nFAULT` 没有进入 TMR1 BRK，TMR1 break 也被关闭，所以不存在 MCU 侧异步硬件刹车路径；
- 仍需检查驱动传播延迟、最小脉宽、输入毛刺、复位瞬间 GPIO 状态和 PCB 走线寄生。

### 5.4 CH4 ADC 触发点的真实方向

CH4 配置为 `PWM_MODE_A`、CCR4=3740，使用 `TMR1_CH4` 作为 ADC 注入触发源。因为中心对齐下数从 `3749` 回到 `3740` 只经过 9 个 150 MHz 计数：

```text
9 / 150 MHz = 60 ns
```

所以该事件是“峰值之后约 60 ns”（下数阶段），不是生成文件旧注释中写的“峰值之前 60 ns”。这是注释方向错误，不改变当前比较值，但会影响后续采样窗口推导，已在本文按实际方向记录。

## 6. ADC 注入序列、电流采样和母线换算

### 6.1 通道和 rank 对照

| 注入 rank | ADC 通道 | MCU 管脚 | 网络 | `adc_read_phases()` 用途 |
| ---: | ---: | --- | --- | --- |
| 1 | ADC1_IN1 | PA1 | VBUS | 母线电压 |
| 2 | ADC1_IN4 | PA4 | IC/SOC | ABC 时 C 相 |
| 3 | ADC1_IN5 | PA5 | IB/SOB | ABC 时 B 相 |
| 4 | ADC1_IN6 | PA6 | IA/SOA | A 相 |

`PHASE_ORDER_ABC` 时，软件读 rank2/3/4 为 C/B/A；`PHASE_ORDER_ACB` 时交换 B/C。这与网表中的 `SOC->IC`、`SOB->IB`、`SOA->IA` 对应。

ADC 配置还满足：

- 7.5 ADC 周期采样；
- 注入长度 4；
- 非重复注入组；
- TMR1 CH4 外部触发；
- PCCE 中断；
- ADC 使能后执行校准；
- 普通 ADC DMA 路径当前没有被应用使用。

### 6.2 ADC 时钟和转换时间

系统代码把 APB2 配置为 75 MHz，ADC 分频为 4，因此：

```text
f_ADCCLK = 75 MHz / 4 = 18.75 MHz
```

AT32F415 手册给出的单次转换时间为 `采样周期 + 12.5 周期`。因此 7.5 周期采样时：

```text
单通道总时间 = (7.5 + 12.5) / 18.75 MHz ≈ 1.0667 μs
四通道组时间 = 4 × 1.0667 μs ≈ 4.2667 μs
```

这一配置满足 ADC 时钟低于手册上限，且 20 kHz 触发周期约 50 μs，转换组不会跨越到下一个 PWM 周期。

### 6.3 低侧采样窗口的保守模型

DRV8311H 的 CSA 是低侧 sense-FET 采样，SOx 不是任意时刻都代表有效相电流。当前 SVPWM 在 `foc_core.c` 中将归一化电压矢量幅值限制为 `2/3`，理论上最大相占空比约为 `5/6`。在最不利的最大占空比估算下：

```text
CCR_max ≈ 3749 × 5/6 ≈ 3124.17
峰值到低侧窗口边缘 ≈ (3749 - 3124.17) / 150 MHz
                         ≈ 4.1656 μs
```

CH4 触发在峰值后 0.06 μs，四个 rank 顺序转换时，rank4 的采样孔径约为：

```text
rank4 开始 ≈ 0.06 + 3 × 1.0667 = 3.260 μs
rank4 结束 ≈ 3.260 + 7.5/18.75 MHz = 3.660 μs
```

相对于上述低侧窗口边缘，保守孔径余量约：

```text
4.1656 - 3.660 ≈ 0.506 μs
```

这得到一个重要但有限的结论：

- `[计算验证]` 触发点没有明显晚到下一个 PWM 周期，rank4 的采样孔径在理想最大占空比模型中仍落在约 4.17 μs 的窗口前；
- `[风险]` 余量小于 DRV 47 kΩ SLEW 下典型约 650 ns 的输入到功率输出传播延迟量级，且四相值是顺序采样、不是同一时刻采样；
- `[未验证]` 不能因此宣称所有 SVPWM 扇区、相电流方向、占空比和电流变化率下都已满足 CSA 建立时间；还需要至少有全扇区模拟波形模型或实测 SOx/ADC 触发波形。

### 6.4 CSA 模拟量和软件量程

U4 `GAIN=0.5 V/A`，`CSAREF≈3.3 V`，CSA 典型关系为：

```text
SOx ≈ CSAREF/2 ± 0.5 × IOUTx
```

输出线性边界按约 `0.25 V … 3.05 V` 估算，则 nominal 电流窗口是：

```text
I_nominal ≈ (3.05 - 0.25) / (2 × 0.5) = ±2.8 A
```

12 bit ADC 的单 LSB 和当前代码比例为：

```text
ADC_LSB     = 3.3 / 4095 ≈ 0.8059 mV
CURRENT_LSB = ADC_LSB / 0.5 ≈ 1.6117 mA/count
```

ADC 满量程相对于软件中心 2047.5 码的数字窗口约 `±3.30 A`，所以先撞到 DRV CSA 线性边界，而不是 ADC 数字满量程。默认 `DEFAULT_CURR_PEAK=2 A` 位于 nominal 模拟窗口内，但这不等价于硬件 2 A 保护；硬件 H strap 的 OCP 典型值约 5 A，软件过流策略当前为空。

### 6.5 固定 3.3 V 参考误差

网表把 U3 电源、U4 `CSAREF` 和 `DRV_3V3` 放在同一网络，代码却把 `V_REF` 固定为 3.3 V。若该 LDO 实际在 3.15–3.45 V 范围内：

```text
换算误差近似 = 3.3 / V_actual - 1
```

结果为：

| 实际 DRV_3V3 | 固定 3.3 V 公式的比例误差 |
| ---: | ---: |
| 3.15 V | +4.76% |
| 3.30 V | 0 |
| 3.45 V | −4.35% |

这不是 PA 管脚配置错误，而是 ADC/CSAREF 的参考建模问题。若需要绝对电流/母线精度，应选择校准、测量实际参考、使用内部基准测量或接受明确的误差预算；若只需要相对电流控制，应在控制参数和校准文档中明确这一假设。

### 6.6 SOx RC 和母线分压 RC

每路 SOx 是 330 Ω 串联、22 pF 对地：

```text
τ_SOx = 330 × 22 pF = 7.26 ns
f_c   ≈ 21.92 MHz
```

相对于 20 kHz 载波约有 1096 倍频率余量，静态值不会把 20 kHz 基波滤掉；实际抗噪能力仍取决于电容位置、ADC 输入孔径、功率地回流和开关节点耦合。

VBUS 分压是 R14=100 kΩ、R15=10 kΩ，C17=100 nF：

```text
VBUS_ADC = DCBUS × 10 / 110
FAC_VBUS(code) = code × 3.3/4095 × 11
ADC 满量程母线 ≈ 36.3 V
τ_VBUS ≈ (100k || 10k) × 100 nF ≈ 0.909 ms
f_c_VBUS ≈ 175 Hz
```

该 RC 有利于抑制 PWM 纹波，但会延迟母线快速变化；因此不能把当前 17 V/7.4 V 软件阈值解释为纳秒级硬件保护。

## 7. SPI1 编码器时序和中断边界

### 7.1 电气对应

网表和代码对应：

- U5 `SPI1_SCK` -> U3 PB3；
- U5 `SPI1_MISO` -> U3 PB4；
- U5 `SPI1_MOSI` -> U3 PB5；
- U5 `SPI1_CS` -> U3 PA15；
- 代码配置 16 bit、MSB first、SPI mode 3、软件 CS；
- `encoder_init()` 与 `encoder_update()` 使用 PA15 拉低/拉高包围一次阻塞传输。

因此 MCU 侧的 SPI 资源选择与网表一致。当前 `ENCODER_SPI_DMA_ENABLE=0`，生成的 DMA1 CH3/CH4 仅保留初始化函数，不构成实际 DMA 编码器传输。

### 7.2 真实运行频率和旧注释

TMR5 配置 `ARR=1999`、预分频寄存器 14，即除以 15：

```text
f_TMR5_IRQ = 150 MHz / (15 × 2000) = 5 kHz
```

`motor_task.c` 的 TMR5 中断中先执行 `encoder_update()`，再执行外环；因此 PLL 的采样率实际由 `VEL_LOOP_FREQ_HZ=5 kHz` 承担，而不是 encoder.c 旧注释写的 20 kHz。PLL 中 `dt=1/sample_rate_hz` 的实现与 5 kHz 参数是自洽的，旧注释需要统一。

SPI 传输在软中断中轮询等待，没有超时：

```c
while (TDBE == RESET) { }
while (RDBF == RESET) { }
```

在 SPI、时钟和外设始终正常的假设下，16 bit 传输时间远小于 200 μs 的 5 kHz 周期；但一旦 SPI 外设异常或总线状态不满足，ISR 可能无限等待。这是代码级实时风险，不能由原理图连接正确性消除。

## 8. USART1/USART2 接口核对

### 8.1 USART1：内部路径自洽

代码配置 USART1 PB6/PB7、115200 8N1、DMA1 CH1/CH2、IDLE 中断；`bsp_console.c` 运行时也明确把 USART1 和 DMA1 CH1/CH2 作为 console transport。因此 USART1 的生成配置和应用调用边界是闭合的。

### 8.2 USART2：原理图接口与软件路径断裂

原理图/网表显示：

- U1/U2 的连接器引出 `USART2_RX/TX`、`DCBUS` 和 `GND`；
- U3 PA2/PA3 是 USART2 TX/RX 网络；
- U14/U15 是该网络的 ESD 器件。

当前代码却有三层断点：

1. `main.c` 中 `wk_usart2_init()` 被注释掉；
2. USART2 的时钟虽然开启，但 GPIO、波特率、收发器使能和外设使能没有被启动调用；
3. `USART2_IRQHandler()` 即使被触发也只清 IDLE 标志，没有把数据交给 Shell、console 或协议层。

所以必须先决定产品事实：

- 如果 U1/U2 是正式命令/通信口，应该把 USART2 初始化、DMA 缓冲、IRQ 消费者和协议绑定纳入同一个 owner；
- 如果 U1/U2 是预留接口，应在文档和 BOM/原理图说明中明确“未启用”，避免误把 USART1 测试点当成产品串口；
- 不能只取消一行注释就宣称 USART2 可用，因为中断消费和 DMA 所有权仍未闭合。

## 9. 故障、启动和保护路径

### 9.1 当前真正发生的路径

```text
ADC1_IRQHandler
  -> adc_read_phases()
  -> foc_fault_check_fast()       常数时间快速门
  -> 严重电流/非法传感器/VBUS/速度/温度
  -> fault_code 锁存 + pwm_out_disable() + 退出本次 FOC

每 1 ms motor_task
  -> foc_fault_check(&_motor_ins)
  -> 3 s 警戒电流或 200 ms 低速堵转后备
  -> fault_code 锁存并在任务上下文收敛到 FAULT

若其他代码主动派发 EV_FAULT_TRIP
  -> motor_fsm
  -> axis_state=FAULT
  -> pwm_out_disable()
  -> PID/FOC 状态清零
```

FSM 的 FAULT 进入动作本身是有序的；当前软件 fault producer 已覆盖可见的相电流、VBUS、速度、温度和非法数据。当前 U4 的 `nFAULT` 没有 MCU 端点，TMR1 break 也关闭，意味着：

- 硬件 DRV OCP/UVLO/OTSD 可能自行关断功率级；
- MCU 仍不知道 DRV 内部故障来源，不能把硬件 nFAULT 原因纳入软件故障报告；
- 软件快速保护和任务级后备已经运行，但不能替代硬件 Break、独立电源急停和 DCBUS 断开。

### 9.2 零 VBUS 的数值路径

启动时 `motor_runtime_init()` 把轴设为 IDLE；IDLE 下 ADC ISR 只采样，不执行完整 FOC。收到 START 后 `_on_running_enter()` 把轴设为 RUNNING，但下一次进入完整电流环前会先经过快速故障检查。`foc_para_calc()` 和 `foc_ctrl_volt()` 还对 `v_bus` 做数值有效性检查：

```text
foc_current_loop_step()
  -> foc_para_calc()
  -> isfinite(v_bus) && v_bus > 1.0f
  -> invalid: zero modulation + pwm_out_disable()
  -> valid: inv_vbus = 1.0f / meas_data.v_bus
  -> foc_svpwm()
```

这仍不是“启动前母线窗口门禁”：软件数值保护负责阻止零除、NaN/Inf 和无效调制，产品欠压/过压由运行时阈值处理。该路径已由无板 VBUS contract 覆盖，不需要实板即可复现边界。

### 9.3 温度和 NTC

源码存在 NTC 表格/换算帮助函数，但当前 Board1 网表没有 NTC ADC 网络，`adc_read_ordinary()` 的调用在安全任务中被注释；U4 H 版本的 OTW 默认不进入 MCU 的可读软件路径。因此：

- `motor_temp_thr`、`driver_temp_thr` 是配置字段；
- 当前没有证据证明它们会收到有效测量值；
- 不能把温度阈值存在当作温度保护已实现。

### 9.4 历史 Bring-up 门限冲突与当前分层包线

历史版本的 ADC 快速路径在 `AXIS_STATE_CALIB` 和 `AXIS_STATE_RUNNING` 都生效，且每相绝对电流达到 `FOC_FAST_CURRENT_TRIP_A=0.20 A` 即锁存并关闭 PWM；这会与参数校准默认激励冲突：

| 路径 | 默认电流 | 与快速门限的关系 |
| --- | ---: | --- |
| 编码器 I-f 拖动 | `CALIB_ENC_IF_IQ=0.30 A` | 历史值高于 `0.20 A`，会触发快速保护 |
| Rs 校准 | `CALIB_RS_CURR_MAX=1.00 A` | 历史值高于 `0.20 A`，不能在历史 bring-up 门限下执行 |
| 磁链校准 | `CALIB_FLUX_IQ_A=1.00 A` | 历史值高于 `0.20 A`，不能在历史 bring-up 门限下执行 |
| 惯量校准 | `CALIB_JS_IQ_AMP=1.00 A` | 历史值高于 `0.20 A`，不能在历史 bring-up 门限下执行 |

当前工程的确定性结论是：`FOC_FAST_CURRENT_TRIP_A=1.00 A`、任务级持续阈值为 `0.75 A/3 s`，而 `FOC_ENERGIZED_CALIB_ENABLE=0U` 仍禁止带电校准。提高或关闭保护不能替代校准专用的电流包线、硬件急停和安全评审；在这些条件具备前，校准命令仍视为 `BLOCKED`。

## 10. 配置/命名不一致清单

| 位置 | 实际值 | 注释/设计意图 | 影响 |
| --- | ---: | --- | --- |
| `foc_config.h:90` 与 TMR1 | `PWM_FREQ_HZ=10 kHz` 是 FOC step cadence；TMR1 实际约 20 kHz | 多处仍按 PWM 频率命名/描述 | 后续人员可能用错 `PWM_DT`、校准计数或载波周期 |
| `foc_config.h:286` | 当前 `DEFAULT_CUTOFF_HZ=1250 Hz`，为 `DEFAULT_CURR_BW_HZ=250 Hz` 的 5 倍 | 原工作树曾为 25 Hz，注释曾写 2000 Hz | 原数值问题已修正；持久化或运行时修改 `currbw` 后，LPF 仍需同步刷新 |
| `foc_config.h:253` | `DEFAULT_CURR_BW_HZ=250 Hz` | 同一段范围写 500–3000 Hz | 参数范围、实值和历史经验没有单一事实源 |
| `foc_config.h:287` | `DEFAULT_SAMPLING_HZ=CURR_LOOP_FREQ_HZ=10 kHz` | 当前已绑定 FOC step | 运行时保持不变；旧的 20 kHz 注释已不再代表当前值 |
| `foc_math.h:306` | `alpha=ωc/(ωc+fs)` | 注释称 Tustin | 实际是 backward-Euler 形式；不影响当前参数结论，但应统一算法注释与离散频响推导 |
| `encoder.c:38`、encoder API 注释 | 实际 TMR5/PLL 5 kHz | 旧注释写 20 kHz、ADC ISR | 不直接改变当前 `dt`，但会误导实时预算和调参 |
| `wk_tmr.c:165–168` | CH4 事件为峰值后约 60 ns | 旧注释写“峰值前” | 会误导采样孔径分析 |
| `wk_tmr.c:207–244` | TMR3 CH4 仍配置 | ADC 已切到 TMR1 CH4，未见应用消费者 | 生成资源和实际 ADC source of truth 不一致，增加维护歧义 |

| `wk_usart.c`/`main.c` | USART2 生成函数存在但未调用 | U1/U2 原理图接口容易被理解为可用接口 | 接口功能断裂 |

## 11. 后续处理建议（区分已完成与未完成项）

1. **先确定产品通信口。** 明确 U1/U2 是否正式接口；若是，补齐 USART2 的初始化、DMA 缓冲、IRQ 消费和 Shell/协议 owner；若不是，在原理图/Wiki 标明预留。
2. **闭合 DRV 故障边界。** 决定 nFAULT 是否进 MCU GPIO/外部逻辑，决定 nSLEEP 是否由硬件 fail-safe 或 MCU 控制，补齐 fault latch、故障记录和清除条件；不要把 U4 自动重试当作产品故障策略。
3. **冻结时钟命名。** 分开定义 `CARRIER_FREQ_HZ≈20 kHz`、`ADC_TRIGGER_FREQ_HZ≈20 kHz`、`FOC_STEP_FREQ_HZ=10 kHz`、`OUTER_LOOP_FREQ_HZ=5 kHz`，让 `PWM_DT` 只属于真正使用它的控制周期。
4. **保持 Id/Iq 滤波器同步。** 默认 `1250 Hz / 250 Hz = 5` 已通过无板应力模型；后续 `currbw` 从参数恢复或 Shell 修改时，应同步更新滤波器 cutoff，避免 PI 增益和反馈滤波器再次分叉。
5. **验证 ADC 采样窗口。** 在主机侧做全六扇区、占空比、相序和电流方向模型；至少覆盖最大 duty、最小脉宽、DRV 传播延迟、ADC rank 顺序和异常 VBUS。
6. **处理参考与热预算。** 明确 `DRV_3V3/CSAREF/ADC VREF` 的测量或校准策略，检查 20 V 下 R6 功率、AVDD LDO 负载、QFN EP/AGND/PGND 布局。
7. **消除无效资源和阻塞边界。** 评估删除/关闭已不用的 TMR3、USART2 DMA CH3/4 生成资源，给阻塞 SPI 读取增加可验证的超时/故障返回策略；这些是后续代码变更，不在本审计中直接实施。

## 12. 无实板模拟测试矩阵

以下测试均不需要实板，可以作为后续 CI 或主机测试的最小集合：

| 场景 | 输入 | 应验证的性质 |
| --- | --- | --- |
| 网表—代码 pin map | 解析 `pcb 网表.tel` 与生成头文件 | `VBUS/IA/IB/IC`、六路 PWM、SPI1、USART1/2 的端点一致性 |
| DRV strap 解码 | R11/R12/R13=47 kΩ | `6x PWM / 75 V/μs / 0.5 V/A / OCP≈5 A typ` |
| ADC 量化回算 | ±2 A、零点、满量程码 | SO 电压、ADC code、软件电流的往返误差与符号 |
| VBUS 分压 | 7.4/12/16/20/36.3 V | ADC 码、阈值、36.3 V 满量程和 0.909 ms RC |
| PWM 定时 | 150 MHz、PR3749、CCR4=3740 | 20.005 kHz、峰值后 60 ns、互补通道映射 |
| 采样孔径 | 全扇区 duty、rank 顺序、传播延迟参数 | 所有 rank 是否处于有效低侧窗口，异常时是否显式失败 |
| fault 网络 | 7.4–20 V、nFAULT=0 | nSLEEP 是否仍高于 VIH；当前预期结果是“不能联动拉低” |
| 零 VBUS 防护 | `v_bus=0`、极低值、NaN/异常 ADC | FOC 不产生 Inf/NaN duty，并进入可观测 fault 状态 |
| USART 接口契约 | U1/U2/USART2 数据流模型 | 初始化、DMA、IRQ、协议消费者是否全部存在 |
| 状态机 | IDLE/RUNNING/FAULT/clear | fault producer、PWM disable、重启门控和清除条件闭合 |

本轮已完成的主机侧静态/数学结果摘要：

```text
STATIC_NETLIST=PASS
DRV_STRAPS=PASS mode=6x_PWM slew_typ=75V/us gain=0.5V/A ocp_typ=5A
CURRENT_SCALE=PASS lsb=1.611722mA/count analog_range=±2.800A
CSA_RC=PASS fc=21.922MHz
VBUS_DIVIDER=PASS full_scale=36.30V code_at_20V=2256.2
PWM_MODEL=PASS carrier≈20.005kHz trigger_after_peak=60ns
ADC_TIMING=PASS f_adc=18.75MHz channel=1.0667us group=4.2667us
FAULT_PATH=STATIC_FINDING nFAULT_low_does_not_pull_nSLEEP_below_VIH
NO_BOARD=PASS no physical waveform/thermal/EMI/motor claim
```

本轮实际执行的无板测试结果：

- `cmake --preset fw-tiny-carrier-minsizerel` + `cmake --build --preset build-fw-tiny-carrier-minsizerel --parallel`：通过；当前产物为 `FLASH 36,668/131,072 B`、`RAM 18,248/32,768 B`。
- 完整 `run_host_tests.ps1`：当前注册目标已扩展为 38 个，全部编译并通过 CTest（最终门禁以本轮新鲜输出为准）；包括 fault/calibration/VBUS、故障上下文、运行态诊断门禁、动态 LPF、带电校准门禁、FOC 电压开环/SVPWM、时基/比例与电流闭环参数应力模型、任务预算、FSM 中止和诊断格式契约。
- AT32F415 full-diag、standard、tiny 三种默认固件以及 bench full-diag 均构建通过；本轮新鲜构建的默认 full-diag 为 FLASH `77,400/131,072 B`、RAM `21,808/32,768 B`，standard 为 `39,876 B/20,072 B`，tiny 为 `36,668 B/18,336 B`，bench full-diag 为 `86,728 B/27,480 B`。
- Clang 22 仍被 `include/tmdef.h:108` 的项目编译器白名单拒绝；这是工具链选择限制，不是业务代码回归。主机验证使用 MinGW GCC 完成。

### 12.1 电流闭环参数应力模型

为避免把“比例宏通过”误认为“控制环稳定”，本轮又加入了一个纯主机、确定性、不可接触硬件的简化模型：`Rs=2.55 Ω`、`Ls=0.86 mH`、`curr_bw=250 Hz`、`Ts=100 μs`，由当前 PI 公式驱动一阶 RL 电流对象，再把反馈经过当前 Id/Iq LPF。该模型不包含反电动势、机械负载、PWM 离散调制、死区、采样噪声和 DRV 非线性，只用于比较滤波器配置的相对风险。

```text
current[n+1] = current[n] + (V[n] - Rs × current[n]) / Ls × Ts
filtered[n] = filtered[n-1] + alpha × (current[n] - filtered[n-1])
alpha = 2πfc / (2πfc + fs)
```

对 `0.10 A` 小电流阶跃运行 `100 ms`：

| 配置 | LPF alpha | 模型峰值电流 | 100 ms 末端反馈 |
| --- | ---: | ---: | ---: |
| 原错误值 `fc=25 Hz` | `0.015465` | `0.33245 A` | `0.10000 A` |
| 当前修正值 `fc=1250 Hz` | `0.439901` | `0.10000 A` | `0.10000 A` |

因此旧值的主要危险不是最终稳态误差，而是反馈延迟导致 PI 在反馈尚未跟上时继续增加电压；它可以解释 I-f/电流闭环路径中“指令很小但瞬态电流过大”的高风险机制。纯 `CTRL_MODE_VOLT` 路径绕过 Id/Iq PID，因此该模型不能单独解释纯电压开环的卡死；纯电压路径仍需另外检查电角度、相序、编码器零点和实际门极/采样波形。

## 13. 本轮不能关闭的事项

- Board1 实际 PCB 层叠、铜厚、走线、U4 裸焊盘、热过孔、功率地和模拟地回流；
- `DRV_3V3`、`CSAREF`、`nSLEEP`、`nFAULT` 的上电/故障波形；
- SOA/SOB/SOC 的开关噪声、建立时间和全扇区有效采样；
- U4 的真实结温、AVDD LDO 温升、短路/过流自动重试热循环；
- U5 `KTH7823-X-N-QN16` 的 SPI mode、最大时钟、时序裕量和真实磁角度；
- 电机相序、编码器电角度、FOC 电流环闭环稳定性、转矩、振动和机械负载；
- USART2 连接器上的真实电平、线缆、ESD 和协议兼容性；
- TVS D1 的浪涌能量、母线插拔和 EMI 合规。

上述项目只能在补充 PCB/器件资料或执行实物测试后关闭。本 Wiki 不把静态/数学 PASS 扩大解释为实板验收通过。

## 14. 直接证据索引

| 证据 | 关注点 |
| --- | --- |
| [`pcb原理图.pdf`](<../pcb docs/pcb原理图.pdf>) | Board1 三页电源、U4、MCU、连接器和外围连接 |
| [`pcb 网表.tel`](<../pcb docs/pcb 网表.tel>) | 当前逻辑网络；重点为 `DCBUS`、`DRV_3V3`、`NFAULT`、`NSLEEP`、`SOA/B/C`、`INH*/INL*`、`VBUS` |
| [`pcb bom.xlsx`](<../pcb docs/pcb bom.xlsx>) | 元件阻值、容量、封装和装配清单 |
| [`drv8311.pdf`](<../pcb docs/drv8311.pdf>) | DRV8311H 的四电平、PWM、CSA、保护、时序和布局要求 |
| [`at32f415_wk_config.h`](../../platform/at/at32f415/generated/inc/at32f415_wk_config.h) | AT32F415 生成 pin 绑定 |
| [`wk_adc.c`](../../platform/at/at32f415/generated/src/wk_adc.c) | ADC 注入序列、采样时间、TMR1 CH4 触发 |
| [`wk_tmr.c`](../../platform/at/at32f415/generated/src/wk_tmr.c) | TMR1/TMR3/TMR5 计数、PWM、CH4 触发、break/deadtime |
| [`at32f415_wk_config.c`](../../platform/at/at32f415/generated/src/at32f415_wk_config.c) | 系统/外设时钟、NVIC 优先级 |
| [`main.c`](../../platform/at/at32f415/generated/src/main.c) | 启动初始化调用顺序，尤其 USART2 被注释 |
| [`wk_spi.c`](../../platform/at/at32f415/generated/src/wk_spi.c) | SPI1 GPIO、mode3、16 bit、软件 CS |
| [`wk_usart.c`](../../platform/at/at32f415/generated/src/wk_usart.c) | USART1/2 生成配置 |
| [`bsp_console.c`](../../bsp/ports/bsp_console.c) | 实际 USART1 console owner |
| [`motor_drv.c`](../../applications/periph/motor_drv.c) | PWM duty、ADC rank、相序、电流符号和比例换算 |
| [`motor_task.c`](../../applications/tasks/motor_task.c) | ADC ISR、5 kHz TMR5 soft IRQ、故障入口 |
| [`foc_ctrl.c`](../../applications/periph/foc_ctrl.c) | `v_bus`、FOC step、动态电压限制 |
| [`foc_core.c`](../../applications/algo/foc_core.c) | SVPWM 2/3 矢量限制和 duty 生成 |
| [`motor_fault_runtime.c`](../../applications/runtime/motor_fault_runtime.c) | 任务级故障策略入口；快速 ISR 保护由 `foc_fault_check_fast()` 配合使用 |
| [`check_foc_peripheral_contract.ps1`](../../scripts/flows/host/check_foc_peripheral_contract.ps1) | 只读检查生成外设配置、pin map、FOC 映射和 VBUS 防护 |

## 15. 计划实施后的闭环结论

### 15.1 运行时带宽契约

`motor_runtime_init()` 的顺序现在是：默认配置/控制器初始化 → `_motor_load_params()` → `foc_lpf_init()`。持久化的有效 `curr_bandwidth` 会参与首次 LPF 系数计算；持久化值为 NaN、Inf 或非正数时，runtime 先恢复编译期默认值，再进行 PI/LPF 派生。运行时 `diag set currbw` 先检查 finite 且大于零，再依次更新 PI 增益和 `foc_lpf_refresh()`；刷新只改 alpha，不清零 `lpf.output`。无板契约覆盖持久化带宽、持久化非法值回退、合法动态修改、非法值拒绝和输出保持。

### 15.2 带电校准门禁

`FOC_ENERGIZED_CALIB_ENABLE` 默认定义为 `0U`。在该配置下，`motor_fsm` 的 `EV_CMD_CALIB_MOTOR` 和 `EV_CMD_CALIB_ENC` action 在进入 `AXIS_STATE_CALIB` 之前返回拒绝，不能调用 `motor_calib_prepare_entry()`、编码器带电初始化或 PWM 使能；启动后的非带电电流偏置校准仍保留。测试构建通过 `FOC_ENERGIZED_CALIB_ENABLE=1` 保留原有中止恢复覆盖，但这不改变默认 bring-up 固件的门禁状态。

### 15.3 无板电压开环验证范围

`test_foc_voltage_openloop_model_contract` 使用 `100 us` 确定性步长和真实软件函数，覆盖零速度角度保持、恒速角度推进与 `[0,2π)` 回绕、SVPWM 矢量限幅、合法/非法 VBUS、I-f 角度一致性、绕过/调用电流 PI 的模式边界，以及 A/B/C 到 CH3/CH2/CH1 映射。该测试证明的是数学和调用边界，不证明采样窗口、死区、门极波形、DRV 保护或电机转动。

### 15.4 当前实机准入状态

```text
REAL_BENCH_TEST_STATUS=BLOCKED
```

解除条件仍包括：`nFAULT` MCU/独立监控路径、`nSLEEP` 软件状态一致性、TMR1 Break 或等效硬件快速关断、互补 PWM 死区示波器确认、物理断电开关、机械固定、电流/温度/示波器设备、非阻塞保护路径、VOFA 限流和白名单命令。12 V/1 A 不能替代其中任何一项。

### 15.5 2026-08-29 现有板短脉冲结果

本轮把产品级 G0 与第一轮故障发现分离：使用 bench 镜像的约 50 ms ADC 自动护栏执行
现有板 D0-D4/O0-O3，不执行带电校准、I-f、速度闭环或长时间运行。

`[实板测量]` 当前 bench ELF 经 OpenOCD `Verified OK` 写入；COM3 启动状态为
`axis=idle`、`fault=0`、`vbus≈11.91 V`。在 `vq=0.10–0.40 V`、`we=3.14 rad/s`
的短脉冲中，锁存的板上换算相电流最大约 `60 mA`，没有触发 `0.20 A` 快速过流；
O1 的 `theta_sim≈0.075 rad`、`we_volt_now≈3/3.14 rad/s`，说明开环软件角度在
实板短脉冲中推进。

`[实板未验证]` 独立 PWM 波形、互补死区/重叠、外部相电流、DRV 结温、绕组温升、
连续旋转和所有扇区采样窗口仍未验证。因此当前只能写成“短脉冲未复现高电流卡死；
低压不转暂列电压/转矩裕量候选”，不能写成唯一根因。

详细数据见 [`tmp/bench/20260829_actual/result.md`](../../tmp/bench/20260829_actual/result.md)。

## 16. 2026-08-30 正交诊断配置契约

`[源码确认]` 当前 bench 配置和产品配置必须分开解释：

| 项目 | bench | 产品默认 |
| --- | --- | --- |
| `TM_BENCH_DIAG` | `1` | 不定义/关闭 |
| `TM_FEATURE_VOFA` | 强制 `0` | 按产品 preset |
| `FOC_ENERGIZED_CALIB_ENABLE` | 强制 `0` | 按产品当前安全策略 |
| `TM_MOTOR_BENCH_RUN_BUDGET_MS` | 50/200/400（仅 bench） | 不继承 |
| PWM/ADC map override | RAM-only | 不存在 |
| FOC/encoder trace | RAM-only | 不分配 |
| ADC trigger override | IDLE-only | 默认生成值 3740 |

`[源码确认]` map0 是当前已确认的默认映射：PWM `A/B/C→CH3/CH2/CH1`，ADC
`A/B/C→rank4/rank3/rank2`。6 种排列只用于诊断筛选，不能被写入 Flash 或当作产品相序。
触发 compare 只接受 `256..3740`，越界和运行中写入均拒绝。

`[无板验证]` `test_motor_bench_trace_contract`、`test_motor_bench_mapping_contract`、
`test_motor_bench_trigger_contract`、`test_encoder_bench_trace_contract` 及相关 Shell/
runtime/FOC 测试均已纳入 38 个 host 目标；host flow、CTest 和静态外设契约均通过。

`[实板未验证]` 这些契约只能证明寄存器字段、软件表和状态边界的一致性，不能证明 TMR1
引脚波形、DRV8311H 传播/死区、ADC 模拟采样窗口或三相物理电流。

## 17. 2026-08-30 开环保护和测试时基修正

`[源码确认]` 当前保护值为快速每相 `1.00 A`、持续 `0.75 A/3 s`；闭环堵转为
`0.75 A/200 ms` 且 `<=5 rpm`，只对非 `CTRL_MODE_VOLT` 生效。bench 快速 ADC 路径不
再把预警值连续 3 个样本直接转换为过流故障，避免正常开环启动电流被误判，同时保留
快速绝对停机和持续电流停机。

`[资料确认][静态推导]` 电机为 7 对极、0.5 A 额定/2.0 A 最大；30 rpm 机械速度对应
约 `21.99 rad/s` 电气速度，一个电周期约 `285.7 ms`。因此 5 ms 和 `3.14 rad/s`
只能作为固定矢量或软件链路取证，不能作为连续旋转测试。新增
`fw-full-diag-carrier-bench-400ms-debug`，Voltage 模式默认使用 400 ms 观察窗口；
映射/触发/相位模式仍保持 5–20 ms。

## 18. 2026-08-30 实板采样点相关性补充

原始记录：[`tmp/bench/20260830_074936_post_snapshot_fix/`](../../tmp/bench/20260830_074936_post_snapshot_fix/)。

`[实板测量]` 在当前标准 SVPWM、`1 V/30 rpm`、相同 map0/adc0 条件下，CH4 compare 扫描得到以下量级：

| CH4 compare | 软件三相峰值量级 | 解释边界 |
| ---: | --- | --- |
| 512、1024、1536、1874 | 约 `0.01 A` | 可能处于低侧 sense/CSA 不可见或未建立窗口 |
| 2300 | 约 `0.286/0.012/0.128 A` | 开始出现明显相不平衡/窗口相关性 |
| 2812、3200、3500、3740 | 约 `0.27 A` 两相量级 | 接近载波峰值，软件观测明显增大 |

`[静态推导]` 该趋势与 DRV8311H 低侧 sense-FET 的导通条件相符，说明不能用任意 CH4 点的单次软件电流去代表真实相电流。它同时提示当前生成配置的采样点仍需用 SOx 波形和 ADC 触发关系验证；仅凭 trace 无法证明所有 sector、占空比和电流方向均落在有效窗口。

`[实板测量]` 36 组 PWM/ADC map 没有稳定唯一优胜，单次 map2 旋转响应未在两次重复中复现；因此没有将实验 map 写入产品默认。相位 offset 矩阵也没有重复优胜，不修改编码器零点。

`[实板未验证]` 仍未完成人工 encoder 整圈、INH/INL/OUT/SOx 示波器、独立相电流和温升测量。当前结论只能是：`ADC_MEASUREMENT_WINDOW=ANOMALOUS_OR_STRONGLY_SENSITIVE`，`OPEN_LOOP_ROOT_CAUSE_STATUS=UNRESOLVED`。
