# Board1 硬件架构与 DRV8311H 深度 Wiki

> 面向当前工作树的硬件—软件对应说明。本文的目标是把 `docs/pcb docs` 中的主板原理图、网表、BOM、TI 参考设计和 DRV8311 数据手册，连接到 `AT32F415` 固件的实际 PWM、ADC、FOC 和故障边界。
>
> 本文只把无实板可以证明的内容写成“验证结论”。示波器波形、芯片温升、EMI、机械负载和真实电机运行均不在本轮证据范围内。

## 1. 证据边界与资料优先级

### 1.1 资料角色

| 资料 | 用途 | 证据等级与限制 |
| --- | --- | --- |
| [`pcb原理图.pdf`](<../pcb docs/pcb原理图.pdf>) | Board1 的人工可读连接和设计意图 | 主板事实；图上灰色/DNP 元件仍需网表/BOM交叉核对 |
| [`pcb 网表.tel`](<../pcb docs/pcb 网表.tel>) | Board1 的实际逻辑网络连接 | 本文判断引脚是否接到 MCU 时，以网表优先 |
| [`pcb bom.xlsx`](<../pcb docs/pcb bom.xlsx>) | 已列入装配的元件、值和封装 | 不能证明元件已经焊接，也不能证明 PCB 走线、焊盘和过孔 |
| [`drv8311.pdf`](<../pcb docs/drv8311.pdf>) | DRV8311 系列电气、时序、保护、布局和封装规格 | TI 数据手册；具体 H 版本以 H 接口章节和 `DRV8311HRRWR` 订货信息为准 |
| [`TI DRV8311H评估板/Project Outputs/MD045B(002)_Sch.PDF`](<../pcb docs/TI DRV8311H评估板/Project Outputs/MD045B(002)_Sch.PDF>) | TI EVM 的参考连接 | 参考设计，不是 Board1 的网表、布局或热性能证据 |
| [`2804电机参考手册.pdf`](<../pcb docs/2804电机参考手册.pdf>) | 2804 电机参数 | 电机供应商参考值，不等价于 Board1/DRV 的连续热额定值 |

### 1.2 结论标签

- `[原理图事实]`：主板 PDF 可见。
- `[网表事实]`：`pcb 网表.tel` 明确给出连接。
- `[BOM事实]`：BOM 中列出了器件和值；不代表已装配。
- `[数据手册]`：TI `drv8311.pdf` 对 DRV8311H 的规格或工作方式。
- `[代码事实]`：当前 `AT32F415` 工作树中的实现。
- `[无板验证]`：本轮主机侧静态/数学/计时模型实际通过。
- `[静态发现]`：由连接或公式确定的设计风险，不需要实板即可复现。
- `[未验证]`：需要真实 PCB、仪器、负载或温度证据，本轮明确不宣称通过。

## 2. Board1 一页架构

```text
                 DCBUS / 电机母线
                    │
       ┌────────────┼──────────────────────────────┐
       │            │                              │
  TVS D1       U4 VIN_AVDD + VM                R14/R15
  SMF15CA      DRV8311H                         100k/10k
       │            │                              │
       │      ┌─────┴─────┐                       └─> VBUS -> AT32 ADC
       │      │           │
       │    AVDD       三相半桥 OUTA/B/C ───────> 2804 电机 U/V/W
       │      │
       │  DRV_3V3 = U4 AVDD
       │      │
       │  U3 AT32F415 + U5 KTH7823 编码器
       │
       ├─> U4 nSLEEP（R6 上拉；无 MCU 控制）
       └─> U4 nFAULT（R10 上拉；无 MCU 输入）

 U3 TMR1 六路逻辑 PWM ──> U4 INHA/B/C、INLA/B/C
 U4 SOA/SOB/SOC ─330R/22pF─> U3 ADC PA6/PA5/PA4（IA/IB/IC）
 U3 SPI1 ────────────────> U5 磁编码器
 U3 USART/SWD/连接器 ───> 控制、调试和外部接口
```

### 2.1 主板组件角色

| 器件/网络 | 角色 | 当前确认 |
| --- | --- | --- |
| U3 `AT32F415KBU7-4` | 6 路 PWM、三相电流 ADC、母线 ADC、FOC、编码器 SPI、控制接口 | `[原理图事实][BOM事实]` |
| U4 `DRV8311HRRWR` | 三相三半桥、内置栅极驱动、低侧电流检测放大器、AVDD LDO、保护 | `[原理图事实][BOM事实][数据手册]` |
| U5 `KTH7823-X-N-QN16` | SPI1 磁编码器 | `[原理图事实][网表事实]` |
| `DCBUS` | 同时供 U4 的 `VIN_AVDD`、`VM`，并进入母线检测分压 | `[网表事实]` |
| `DRV_3V3` | U4 `AVDD` 输出，同时供 U3、U5、CSAREF、上拉网络 | `[网表事实]` |
| `SOA/SOB/SOC` | U4 三路低侧 CSA 输出；经 330 Ω/22 pF 送 MCU | `[网表事实][数据手册]` |
| `INH* / INL*` | U3 到 U4 的 6x PWM 控制输入 | `[网表事实]` |
| `OUTA/B/C` | U4 三个半桥输出 | U4 引脚连接已确认；外部电机铜箔/连接器未被当前网表完整描述 |
| `VBUS` | DCBUS 的 100 kΩ/10 kΩ 分压中点，送 U3 ADC | `[网表事实]` |

### 2.2 供电事实

当前网表不是“外部 3.3 V 供 U4、再由 U4 供 MCU”的分离结构，而是：

```text
DCBUS -> U4 VIN_AVDD/VM
      -> U4 AVDD LDO -> DRV_3V3
      -> U3 AT32F415、U5、U4 CSAREF、nFAULT 上拉等
```

因此，`DRV_3V3` 上的外部负载会影响 U4 的 LDO 功耗；而 `DCBUS` 电压会直接影响 LDO 热耗散。原理图注释建议 `VIN_AVDD` 使用约 5 V/VM 以降低压差，但当前网表把它与 `VM` 一起接到了 `DCBUS`。这是设计意图与实现连接之间必须保留的审查点。

## 3. U4 DRV8311H 引脚—网络对照

U4 是 `DRV8311HRRWR`，WQFN 3 mm × 3 mm、0.4 mm pitch、裸露散热焊盘为 pin 25。下面的“Board1 网络”以网表为准。

| Pin | 数据手册功能 | Board1 网络/连接 | 软件边界 |
| ---: | --- | --- | --- |
| 1 | `nFAULT`，开漏低有效故障输出 | `NFAULT`；R10=10 kΩ 拉到 `DRV_3V3`，R7=10 kΩ 接 `NSLEEP`；接 U13 ESD；没有 U3 端点 | 当前没有 MCU 故障输入、故障原因读取或日志闭环 |
| 2 | `CSAREF`，CSA 参考/供电输入 | `DRV_3V3`，与 U4 `AVDD` 同网 | 代码假定电流零点为 `V_REF/2=1.65 V` |
| 3 | `SOC`，C 相 CSA 输出 | 经 R16=330 Ω 到 `IC`，再到 U3 ADC | `adc_read_phases()` 读取 ADC injected rank 2 |
| 4 | `SOB`，B 相 CSA 输出 | 经 R9=330 Ω 到 `IB`，再到 U3 ADC | `adc_read_phases()` 读取 rank 3 |
| 5 | `SOA`，A 相 CSA 输出 | 经 R8=330 Ω 到 `IA`，再到 U3 ADC | `adc_read_phases()` 读取 rank 4 |
| 6 | `CP`，电荷泵输出 | C10=100 nF 到 `DCBUS/VM` | 由 DRV 内部生成；没有 MCU 控制 |
| 7 | `VIN_AVDD`，LDO 输入 | `DCBUS` | 不能按独立 5 V 输入理解；当前与 VM 同源 |
| 8 | `VM`，电机电源 | `DCBUS` | U4 半桥和电荷泵主电源 |
| 9 | `PGND`，功率地 | `GND` | 高 di/dt 回流与裸焊盘落地必须由 PCB 实现确认 |
| 10 | `OUTA`，A 相半桥输出 | `OUTA` | 外部电机走线/连接器未在网表中展开 |
| 11 | `OUTB`，B 相半桥输出 | `OUTB` | 同上 |
| 12 | `OUTC`，C 相半桥输出 | `OUTC` | 同上 |
| 13 | `INHC`，C 相高侧逻辑输入 | `INHC` -> U3 `INHC` | TMR1 CH1 complementary pair 的 C 相高侧输入 |
| 14 | `INHB`，B 相高侧逻辑输入 | `INHB` -> U3 `INHB` | TMR1 CH2 complementary pair 的 B 相高侧输入 |
| 15 | `INHA`，A 相高侧逻辑输入 | `INHA` -> U3 `INHA` | TMR1 CH3 complementary pair 的 A 相高侧输入 |
| 16 | `AGND`，模拟地 | `GND` | CSA/AVDD 回流；逻辑同网不代表 PCB 走线等效 |
| 17 | `AVDD`，约 3.3 V LDO 输出 | `DRV_3V3` | 给 U3/U5/CSAREF 等供电；需要 LDO 负载预算 |
| 18 | `INLA`，A 相低侧逻辑输入 | `INLA` -> U3 `INLA` | TMR1 CH3 complementary pair 的 A 相低侧输入 |
| 19 | `INLB`，B 相低侧逻辑输入 | `INLB` -> U3 `INLB` | TMR1 CH2 complementary pair 的 B 相低侧输入 |
| 20 | `INLC`，C 相低侧逻辑输入 | `INLC` -> U3 `INLC` | TMR1 CH1 complementary pair 的 C 相低侧输入 |
| 21 | `GAIN`，H 版本四电平配置 | R13=47 kΩ 到 `GND` | 固定为约 0.5 V/A |
| 22 | `SLEW`，H 版本四电平配置 | R12=47 kΩ 到 `GND` | 固定为约 75 V/μs |
| 23 | `MODE`，H 版本接口/PWM 模式 | R11=47 kΩ 到 `GND` | 固定为 6x PWM；上电/唤醒时锁存 |
| 24 | `nSLEEP`，低有效睡眠输入 | `NSLEEP`；R6=3.3 kΩ 从 `DCBUS` 上拉，R7=10 kΩ 到 `NFAULT`，D2=3.3 V 齐纳，C9=100 nF；没有 U3 端点 | 当前由母线电压自动拉高，软件不能直接睡眠/唤醒 U4 |
| 25 | `EP`，裸露散热焊盘 | `GND` | 必须焊接到 PGND/GND；底层铜、热过孔和散热面积尚未有 Board1 PCB 证据 |

### 3.1 关键连接的确定性结论

1. `[网表事实]` U4 是硬件接口 H 版本，不存在 DRV SPI 控制线；U3 的 `SPI1` 只连接 U5 编码器。
2. `[网表事实]` U4 的 `nFAULT` 没有接到 U3；即使 U4 拉低故障线，MCU 也无法知道故障发生或原因。
3. `[网表事实]` U4 的 `nSLEEP` 没有接到 U3；U4 不是由固件 GPIO 使能，而是由 `DCBUS/R6` 网络决定唤醒电平。
4. `[网表事实]` `CSAREF` 与 `AVDD` 同为 `DRV_3V3`；C8=100 nF 位于共享 `DRV_3V3` 网，但网表中没有单独命名的 `CSAREF` 专用电容。
5. `[BOM事实]` 原理图上可见灰色 `C23=100 nF`，但它不在当前网表和 BOM 中，应视为 DNP/未装配候选，不能把它当作已存在的 CSAREF 去耦。

## 4. DRV8311 家族与 H 版本定位

### 4.1 芯片内部组成

DRV8311 把以下功能集成在一个三相驱动器中：

- 三个内置 N-MOS 半桥，输出为 `OUTA/OUTB/OUTC`；
- 高低侧栅极驱动器和电荷泵；
- 三路低侧 sense-FET 电流检测放大器；不需要外部三相分流电阻；
- `VIN_AVDD` 输入、`AVDD` 约 3.3 V LDO；
- VM/VIN/AVDD/CP 欠压、过流、过热关断等保护；
- H 版本的硬件四电平 `GAIN/SLEW/MODE` 配置和 `nFAULT/nSLEEP` 控制。

同一数据手册还覆盖 S、P 版本。S 版本使用 SPI 寄存器，P 版本使用 tSPI/额外配置脚；这些接口不属于当前 `DRV8311HRRWR` Board1 实现。不要把数据手册后半段 S/P 的寄存器能力当成 H 版本可用能力。

### 4.2 H 版本最重要的限制

| 能力 | H 版本当前结论 |
| --- | --- |
| SPI 配置/读诊断寄存器 | 不支持；H 没有 `nSCS/SCLK/SDI/SDO` |
| OCP 阈值 | 由 `MODE` 四电平选择约 5 A 或 9 A 典型值；不能由软件动态设置 |
| CSA 增益 | 由 `GAIN` 四电平固定为 0.25/0.5/1/2 V/A 之一 |
| 输出压摆率 | 由 `SLEW` 四电平固定为约 35/75/180/230 V/μs 之一 |
| PWM 方式 | 由 `MODE` 选择 6x 或 3x；当前固定 6x |
| 故障原因 | 只有开漏 `nFAULT`，H 没有可读故障寄存器 |
| 故障恢复 | H 版本保护通常固定快速自动重试约 5 ms；不是软件清除锁存模型 |

## 5. H 版本四电平配置与 Board1 解码

四电平输入不是普通的“高/低 GPIO”。芯片内部存在上拉/下拉网络，外部状态可为接地、约 47 kΩ 接地、悬空或接 AVDD。数据手册给出的典型配置如下：

| 外部配置 | GAIN | SLEW | MODE | OCP 典型阈值 |
| --- | ---: | ---: | --- | ---: |
| 接 AGND | 0.25 V/A | 35 V/μs | 6x PWM | 9 A |
| 47 kΩ ±5% 接 AGND | 0.5 V/A | 75 V/μs | 6x PWM | 5 A |
| Hi-Z | 1 V/A | 180 V/μs | 3x PWM | 9 A |
| 接 AVDD | 2 V/A | 230 V/μs | 3x PWM | 5 A |

Board1 的 R11/R12/R13 均为 47 kΩ 下拉，因此确定为：

```text
MODE = 6x PWM
SLEW = 约 75 V/μs
GAIN = 0.5 V/A
OCP  = 约 5 A 典型值
```

`MODE` 在上电或唤醒阶段锁存。设计上不能在 U4 已经 awake 时改变这些 strap；当前 Board1 是固定硬件配置，软件没有改变它们的路径。

## 6. 6x PWM 工作模型

在 H 版本 6x PWM 中，每个半桥有独立的高侧输入 `INH` 和低侧输入 `INL`：

| `INL` | `INH` | 半桥输出状态 |
| ---: | ---: | --- |
| 0 | 0 | Hi-Z |
| 0 | 1 | 高侧导通，输出为 H |
| 1 | 0 | 低侧导通，输出为 L |
| 1 | 1 | Hi-Z |

这意味着：

- 两个输入都为 1 不是“高低侧同时导通”，而是 Hi-Z；
- 两个输入都为 0 也为 Hi-Z；
- H 版本直接接受 MCU 的六路互补逻辑，不需要驱动器内部的 3x PWM 生成器；
- 具体相电压还取决于电机反电动势、续流路径、电流方向和死区，不能只用逻辑表宣称 OUT 波形。

当前固件的相位映射不是 `A=CH1, B=CH2, C=CH3` 的直觉顺序，而是：

| 电机相 | `PHASE_ORDER_ABC` 下的 TMR1 通道 | U4 输入 |
| --- | --- | --- |
| A | CH3 + CH3C | `INHA/INLA` |
| B | CH2 + CH2C | `INHB/INLB` |
| C | CH1 + CH1C | `INHC/INLC` |

来源是 [`applications/periph/motor_drv.c`](../../applications/periph/motor_drv.c) 的 `pwm_out_set_duty()`。`PHASE_ORDER_ACB` 会交换 B/C 的 duty 映射，用于相序适配。

### 6.1 死区与驱动器内部握手

当前 [`wk_tmr.c`](../../platform/at/at32f415/generated/src/wk_tmr.c) 把 AT32F415 TMR1 `deadtime` 配为 0。H 版本本身会检测栅极状态并阻止上下管直通；在 VM=12 V、47 kΩ SLEW 配置附近，数据手册给出内部握手等效延迟约 450 ns 典型、760 ns 最大值。

这说明当前设计的安全层次是：

```text
MCU 六路逻辑互补输出
  -> DRV8311H 内部握手/防直通
  -> 半桥栅极
```

`deadtime=0` 可以解释为把死区保护交给驱动器，但不能仅凭源码证明所有边界条件下的开关波形、最小脉冲和 EMI 都满足要求。若后续要提高可验证性，应把 TMR1 的逻辑输出、驱动器传播延迟和最小脉冲约束作为同一时序预算。

在 47 kΩ SLEW 配置、VM 约 12 V 的条件下，H 版本输入到功率输出的传播延迟典型约 650 ns、最大约 1.1 μs；不同四电平 SLEW 会改变该延迟。数据手册还给出最快压摆配置下约 500 ns 的最小脉冲约束。它们是 U4 的器件级时序，不是 AT32 GPIO 的边沿时间，也不是 Board1 的最终 EMI 结论。

## 7. 电源、唤醒和状态机

### 7.1 推荐电源范围与绝对边界

以下是数据手册级别，不是 Board1 的实际承诺：

| 项目 | 推荐工作范围/典型值 | 绝对或保护边界 |
| --- | --- | --- |
| `VM` | 3–20 V | 绝对最大约 24 V |
| `VIN_AVDD` | 3–20 V | 绝对最大约 24 V |
| `CSAREF` | 2–3.6 V | 约 -0.3–4 V |
| `AVDD` | 约 3.15–3.45 V（输入条件满足时） | 约 -0.3–4 V |
| 逻辑输入 | `VIH` 至少约 1.65 V；`VIL` 不超过约 0.6 V | 约 -0.3–6 V |
| 输出电流 | 推荐峰值 5 A | 绝对峰值也不能当连续热额定值 |
| 结温 | 环境/器件规格通常以 -40–125 °C 为工作条件 | 绝对最大结温约 150 °C |

AT32F415 的 3.3 V GPIO 电平满足 U4 的逻辑高电平门限；这只证明电平兼容，不证明高速边沿、过冲、地弹噪声和布局裕量。

### 7.2 上电、睡眠和唤醒时间

- `nSLEEP=0` 进入睡眠，半桥输出 Hi-Z，相关模拟/电荷泵功能关闭。
- 典型唤醒时间约 1–3 ms；CSA 唤醒约 30–50 μs。
- `nSLEEP` 保持低约 15–50 μs 可以复位故障，而不必保持完整睡眠；数据手册给出的复位脉冲范围约 10–65 μs。
- 睡眠转换时间约 100–200 μs；内部电源启停期间 `nFAULT` 可能保持低。
- H 版本没有 MCU 直接操作 `nSLEEP` 的连接，因此上述时序在当前代码中没有 GPIO 状态机承载。

### 7.2.1 电源监控窗口

以下数值是数据手册中的典型监控点和去毛刺时间，用于理解 U4 何时允许功率级工作；不是 Board1 的电源纹波验收指标。

| 监控项 | 上升/下降典型阈值 | 去毛刺/说明 |
| --- | --- | --- |
| `VM` UVLO | 约 2.7 V / 2.6 V | 约 7.5 μs；恢复后重新允许工作 |
| `VIN_AVDD` UVLO | 约 2.7 V / 2.6 V | 约 4 μs |
| `AVDD` UVLO | 约 2.0 V / 1.8 V | 内部 LDO 输出异常时关闭相关功能 |
| `CP` UVLO（相对 VM） | 约 2.3 V / 2.2 V | 约 0.5 μs；栅极驱动不能正常工作时保护 |
| `CSAREF` UV | 约 1.8 V 附近 | H 版本该项检测禁用；参考电压仍必须满足 CSA 输出要求 |

`CSAREF` 推荐 2–3.6 V，输入电流在 3.0 V 附近约 1.7–3 mA；TI 推荐其附近放置 0.1 μF X5R/X7R 到 AGND。TI MD045B EVM 用 `R22=0 Ω` 将 VREF 接 3V3，并配 100 nF 电容；该做法可以作为连接意图参考，但不能替代 Board1 的 C23/共享 C8 装配确认。

### 7.3 Board1 的 nSLEEP/nFAULT 静态行为

当前网络可以简化为：

```text
DCBUS -- R6 3.3k --+-- nSLEEP
                   |
                 R7 10k
                   |
nFAULT(open-drain) +---- U4 pin 1

nFAULT -- R10 10k -- DRV_3V3
nSLEEP -- D2 3.3 V 齐纳 -- GND
nSLEEP -- C9 100 nF -- GND
```

当 U4 因故障把 `nFAULT` 拉低时，R6 仍从 DCBUS 向 `nSLEEP` 提供较强上拉，D2 还会把该节点钳在约 3.3 V。无板电阻网络模型得到：

| DCBUS | `nFAULT=0` 时 `nSLEEP` 模型值 | 结论 |
| ---: | ---: | --- |
| 7.4 V | 约 3.3 V（D2 钳位） | 高于 DRV `VIH`，不进入睡眠 |
| 12 V | 约 3.3 V（D2 钳位） | 高于 DRV `VIH`，不进入睡眠 |
| 16 V | 约 3.3 V（D2 钳位） | 高于 DRV `VIH`，不进入睡眠 |
| 20 V | 约 3.3 V（D2 钳位） | 高于 DRV `VIH`，不进入睡眠；R6/D2 电流约 5.06 mA |

结论是 `[静态发现]`：如果设计意图是“U4 故障后由 `nFAULT` 联动拉低 `nSLEEP`，形成更强的外部关断”，当前 R6/R7/D2 比值不能实现该意图。在 U4 内部 OCP/OTSD 动作时，半桥仍会由芯片自身关闭并按 H 版本策略自动重试；但外部 nSLEEP fail-safe 和 MCU 故障感知都没有形成闭环。

## 8. 三相电流检测链

### 8.1 芯片公式与符号

DRV8311H 的低侧 sense-FET/CSA 输出可用下式理解：

```text
SOx = CSAREF / 2 ± GCSA × IOUTx
```

其中数据手册的 `IOUTx` 方向定义是“流入驱动器低侧端子”的电流。它与当前 FOC 使用的电机相电流正方向相反，因此 [`motor_drv.c`](../../applications/periph/motor_drv.c) 对 A/B/C 三路都显式取负号：

```c
motor->meas_data.c = -FAC_CURR(ic_raw - ic_offset);
motor->meas_data.b = -FAC_CURR(ib_raw - ib_offset);
motor->meas_data.a = -FAC_CURR(ia_raw - ia_offset);
```

这不是增益修正，而是坐标/电流正方向约定修正；如果删除负号，Clarke/Park 链的 `iq` 极性会与施加电压方向相反。

### 8.2 Board1 的模拟量预算

当前连接和代码常数为：

```text
CSAREF ≈ V_REF = 3.3 V
GCSA = 0.5 V/A
ADC = 12 bit, full scale = 4095
zero current ≈ 1.65 V ≈ ADC 2047.5
```

因此：

| 量 | 计算值 |
| --- | ---: |
| ADC 电压分辨率 | 3.3/4095 = 0.805861 mV/count |
| 电流分辨率 | 0.805861 mV / 0.5 V/A = 1.611722 mA/count |
| CSA 线性输出范围 | 0.25–3.05 V |
| CSA 线性范围对应电流 | 约 -2.8–+2.8 A |
| 12 bit ADC 理想数字范围 | 约 -3.299–+3.299 A |
| 实际优先限制 | CSA 输出线性范围，约 ±2.8 A |

电机手册给出的额定电流为 0.5 A、最大电流为 2 A；2 A 在上述 nominal CSA 模拟范围内，也与当前软件 `i_peak=2 A` 对齐。但这不等于 2 A 连续运行的热验收，也不等于 OCP 会在 2 A 附近精确动作。

### 8.2.1 CSA 误差、动态和参考电压要求

除分辨率外，CSA 还受以下数据手册指标影响：

- 四档增益为 0.25/0.5/1/2 V/A；增益误差随测试条件约为 ±4%/±5%，全工作范围的保证值更宽，约为 ±5.5%/±7%；
- 三相增益匹配约 ±5%；
- 在 0.5 V/A 配置下，输入等效偏置可达约 ±50 mA，温漂约 ±150 μA/°C；
- 在约 30 pF 负载下，CSA 输出稳定时间约不超过 1 μs；
- 输出线性范围为 `0.25 V … CSAREF-0.25 V`，所以增益/参考电压共同决定可用电流窗口；
- 零电流的理论输出是 `CSAREF/2`，实际还会叠加 U4 偏置、AGND/PGND 电位差和开关噪声。

因此当前软件只使用每相零点校准和固定 `0.5 V/A` 增益，属于 nominal 模型；它没有消除增益误差、相间误差、温漂或 cross-phase correction。若产品需要低速小电流精度，应把这些误差纳入校准/误差预算，而不是继续提高 ADC 显示小数位数。

### 8.3 MCU ADC 排列

当前平台生成代码的注入组为：

| ADC injected rank | 通道/引脚 | 信号 | `motor_drv.c` 读取含义 |
| ---: | --- | --- | --- |
| 1 | ADC CH1 / PA1 | `VBUS` | 母线电压原始码 |
| 2 | ADC CH4 / PA4 | `IC` | C 相电流原始码 |
| 3 | ADC CH5 / PA5 | `IB` | B 相电流原始码 |
| 4 | ADC CH6 / PA6 | `IA` | A 相电流原始码 |

`adc_read_phases()` 根据 `PHASE_ORDER_ABC/ACB` 解释 rank 2/3，并用零点校准值换算电流。A 相不依赖 KCL 重构，而是实际读取 rank 4；文件注释仍保留了 KCL/两相重构背景，不能把注释当作当前实际实现。

### 8.4 SOx RC 滤波

Board1 为每路输出配置 330 Ω 串联、22 pF 对地：

```text
tau = 330 Ω × 22 pF = 7.26 ns
fc  = 1 / (2πRC) ≈ 21.922 MHz
```

相对于 20 kHz PWM，频率比约 1096 倍；从“不会明显拖慢 20 kHz 基波”的静态角度看有很大余量。真正的噪声抑制仍取决于电容位置、ADC 采样孔径、开关节点耦合和地回路；当前没有 Board1 PCB 版图，不能仅凭阻容值确认布局正确。

### 8.5 采样窗口是电流链的关键边界

DRV8311H 的低侧 sense-FET 只有在低侧导通的时间窗口内才提供有效的相电流信息。当前生成代码已经把 ADC 触发从自由运行改为 TMR1 CH4：

```text
TMR1 CH4 compare -> ADC1 injected group -> ADC1_IRQHandler
                         -> adc_read_phases()
```

但“触发源同步”不等于“每个占空比、每个电流方向都采到了有效窗口”。还要同时考虑：

- 当前半桥 `INL/INH` 状态和相电流方向；
- DRV 传播延迟，H 版本在 VM=12 V 时约 650 ns 典型、最高约 1.1 μs（47 kΩ SLEW）；
- ADC 注入组四通道转换总时长；
- TMR1 CH4 在中心对齐计数上下行时的实际触发次数和相位；
- 低侧导通时间过短时的最小采样窗口。

所以当前可以写成 `[代码事实]`“ADC 已绑定 TMR1 CH4”，但不能写成 `[未验证]`“所有 SVPWM 扇区都已正确采样”。

## 9. 保护机制完整模型

### 9.1 芯片内部保护

| 保护/事件 | H 版本行为 | 当前主板/软件可见性 |
| --- | --- | --- |
| VM 欠压/上电复位 | 半桥、泵和逻辑被禁止；电压恢复后重新启动 | U4 内部处理；软件无专用状态输入 |
| `VIN_AVDD` 欠压 | 输出 Hi-Z，`nFAULT` 低，H 版本约 5 ms 自动重试 | 只有芯片动作，MCU 不读 `nFAULT` |
| `AVDD` 欠压 | 同类关闭/重试 | `DRV_3V3` 供 U3，若严重会同时影响 MCU |
| 电荷泵欠压 | 栅极驱动关闭，按 H 保护策略自动重试 | 无 MCU 诊断寄存器 |
| 低侧/高侧 OCP | 模式选择约 5 A/9 A；固定 blank/deglitch；输出 Hi-Z、`nFAULT` 低；约 5 ms 自动重试 | 当前 47 kΩ strap 选约 5 A 典型；不能软件调节/读取原因 |
| `CSAREF` 欠压 | H 版本该项保护禁用 | CSAREF 错误时 CSA 可能输出无效，但不一定触发 nFAULT |
| OTW | 典型约 178 °C；H 版本默认不通过 `nFAULT` 报告，也不自动限流 | 软件无法得到 U4 温度；不能依赖 OTW 做产品策略 |
| OTSD | 典型约 190 °C；关断半桥/电荷泵、`nFAULT` 低；H 版本约 5 ms 自动重试 | 绝对最大结温约 150 °C，不能把 OTSD 当工作目标 |
| `nSLEEP=0` | 进入睡眠、输出 Hi-Z | 当前无 MCU GPIO 控制 |

OCP blanking 约 0.2 μs、去毛刺约 1 μs；这些是驱动器内部故障判定时间，不是整个板级短路保护响应，也不是示波器上可直接等同的关断延迟。

### 9.2 nFAULT 的真实含义

`nFAULT` 是开漏、低有效、外部上拉的“有故障/状态不正常”线，不提供 H 版本详细原因编码。当前 R10=10 kΩ 拉到 `DRV_3V3` 的静态电流和逻辑电平合理，但由于没有连接到 U3：

- MCU 不能在 1 ms `motor_task` 中读取它；
- 软件不能区分 OCP、欠压、OTSD 或启动过渡；
- 软件不能对 U4 做按原因分类的清故障或重启；
- MCU 仍不能从该引脚得到故障原因或完成硬件睡眠控制；但当前应用已在 ADC ISR 和 1 ms 任务中对可见的相电流、母线、速度、温度和非法数据执行软件故障策略。

因此当前系统存在“双层但不闭环”的保护结构：

```text
DRV8311H 内部硬件保护 -> 直接关闭功率级 -> 固定自动重试
                         （MCU 不可观测）

应用层 fault policy     -> ADC fast check -> fault latch -> PWM disable
                         -> 1 ms fallback -> FSM FAULT
```

## 10. 热、功率与封装

### 10.1 数据手册热参数

DRV8311H RRW WQFN 的典型封装热参数包括：

- `RθJA` 约 42.6 °C/W；
- `RθJC(bot)` 约 4.8 °C/W；
- `RθJB` 约 15.7 °C/W。

这些参数依赖 JEDEC/参考铜区条件，不能直接替代 Board1 的实际 PCB 热阻。

器件级绝对边界还包括：逻辑输入约 -0.3–6 V、`CSAREF/AVDD` 约 -0.3–4 V、输出不超过约 `VM+1 V` 且不超过 24 V 的器件边界、HBM ESD 约 ±1500 V、CDM ESD 约 ±750 V。绝对最大值用于避免损坏，不是可持续设计目标。

数据手册给出的主要估算关系可以写成：

```text
P_FET ≈ 3 × I_RMS² × RDS(on)
P_LDO ≈ (VIN_AVDD - AVDD) × IAVDD
Tj    ≈ Ta + P_total × RθJA
```

在 VM≥6 V、室温附近，三相上下管总导通电阻可按约 210 mΩ 典型、250 mΩ 最大值理解；结温升高后电阻上升。以 0.5 A 每相 RMS 粗估，`P_FET≈0.158 W`；若 2 A 是三相每相 RMS 而非瞬时峰值，导通项粗估已经约 `2.52 W`，这说明“2 A 在 CSA 模拟范围内”不能被误读为“芯片可无条件连续承受 2 A”。

当前主板把 U4 的 AVDD LDO 作为 `DRV_3V3` 来源，因此仅 LDO 项就可能显著：

| DCBUS/VIN_AVDD | 外部 `IAVDD` | `P_LDO` 粗估 |
| ---: | ---: | ---: |
| 7.4 V | 20 mA | 0.082 W |
| 12 V | 50 mA | 0.435 W |
| 16 V | 100 mA | 1.270 W |
| 20 V | 100 mA | 1.670 W |

上表未包含开关损耗、体二极管损耗、短路重试、温度导致的 RDS 增长和 Board1 实际铜区影响；它的用途是识别 LDO 热预算风险，而不是给出实板结温。

### 10.2 封装/布局必须满足的条件

TI 数据手册和 EVM 共同强调：

- 裸露散热焊盘 pin 25 必须完整焊接并接 PGND；
- 裸焊盘下方需要热过孔、连续铜区和低阻回流；
- VM、VIN_AVDD、AVDD、CP、CSAREF 去耦电容应尽量靠近 U4 对应引脚；
- OUTA/B/C 和功率地需要宽、短、低寄生电感路径；
- 高 di/dt 的功率回路远离 SOA/SOB/SOC、CSAREF、ADC 和编码器 SPI；
- AGND/PGND 的连接方式、单点/整面策略必须结合实际版图确定。

当前资料没有 Board1 的 `PcbDoc`、Gerber、层叠、铜厚、U4 焊盘/过孔和走线宽度证据。TI EVM 的 PCB 文件只能说明推荐实现，不能证明 Board1 达到同样热性能或 EMI 性能。

## 11. 母线检测与电机参数的对应

### 11.1 母线分压

主板为 DCBUS 配置 `R14=100 kΩ` 上臂、`R15=10 kΩ` 下臂，`VBUS` 送 PA1；代码用 `VIN_R1=10`、`VIN_R2=100` 表达 11:1 比值：

```text
VBUS_ADC = DCBUS × 10 / (100 + 10)
FAC_VBUS(code) = code × (3.3/4095) × 11
ADC 满量程对应母线约 36.3 V
DCBUS=20 V 时 ADC 码约 2256
```

R14||R15 的 Thevenin 电阻约 9.09 kΩ，与 C17=100 nF 形成约 175 Hz 的低通量级。这个分压能覆盖当前软件 7.4–17 V 的欠压/过压阈值，也不会在 20 V 瞬态时接近 12 bit 满量程；实际采样噪声和 ADC 输入采样时间仍需板级验证。

### 11.2 2804 电机参考值

供应商手册给出的主要参数：

- 12N14P、7 对极、星形连接；
- 工作约 7.4–16 V，标称 12 V，瞬态最高约 20 V；
- 线间电阻约 5.1 Ω，相电阻约 2.55 Ω；
- 额定电流 0.5 A，最大电流 2 A；
- `KV≈220 RPM/V`，12 V 理想转速约 2600 RPM；
- 相电感约 0.86 mH，磁链约 0.0035 Wb；
- 转子惯量约 `3.7e-6 kg·m²`，供应商标称扭矩约 0.03 N·m。

当前 `foc_config.h` 的默认极对数、Rs、Ls、flux、惯量、额定/峰值电流与这些参考值对齐。它们是 FOC 初值和无板模型输入，不替代装配后的电机识别、负载测试和热限制。

## 12. AT32F415 软件对应与调用边界

本节只说明职责、选用文件和调用边界，不展开 vendor/CMSIS 每个函数。

| 文件 | 选用原因/职责 | 与 DRV8311H 的边界 |
| --- | --- | --- |
| [`platform/at/at32f415/generated/inc/at32f415_wk_config.h`](../../platform/at/at32f415/generated/inc/at32f415_wk_config.h) | 定义 `VBUS`、六路 `INH/INL`、SPI1 CS 等 GPIO 事实 | 不承载 DRV 故障策略 |
| [`platform/at/at32f415/generated/src/wk_tmr.c`](../../platform/at/at32f415/generated/src/wk_tmr.c) | 配置 TMR1 中心对齐、CH1/2/3 互补 PWM、CH4 ADC 触发、MCU deadtime=0 | 只提供硬件时序；不解释电流是否处于有效低侧窗口 |
| [`platform/at/at32f415/generated/src/wk_adc.c`](../../platform/at/at32f415/generated/src/wk_adc.c) | 配置 PA1/PA4/PA5/PA6 的注入采样和 TMR1 CH4 触发 | 识别“低侧 CSA 需 PWM 同步采样”；不做故障判断 |
| [`applications/periph/motor_drv.c`](../../applications/periph/motor_drv.c) | 连接 FOC duty、TMR1 通道、ADC 原始码、CSA 正负号和相序 | 是 U4 的应用侧适配边界；当前没有 nFAULT/nSLEEP API |
| [`applications/algo/foc_config.h`](../../applications/algo/foc_config.h) | 定义 3.3 V、0.5 V/A、ADC 量程、分压、FOC/电机初值 | 数值假设必须与硬件 strap/网表保持一致 |
| [`applications/tasks/motor_task.c`](../../applications/tasks/motor_task.c) | ADC ISR 读相电流；每二次 ADC tick 执行一次 10 kHz FOC；软 IRQ 更新编码器/外环 | 不读取 U4 nFAULT，不控制 nSLEEP |
| [`applications/runtime/motor_fault_runtime.c`](../../applications/runtime/motor_fault_runtime.c) | 应用层故障策略拥有者 | `foc_fault_check_fast()` 位于 ADC 快速边界，`foc_fault_check()` 负责 1 ms 后备与持续判定；不包含 DRV `nFAULT` 输入 |
| [`applications/periph/encoder.c`](../../applications/periph/encoder.c) | 通过 SPI1 驱动 U5 编码器 | SPI1 不属于 DRV8311H 控制面 |
| [`bsp/board`](../../bsp/board) / [`bsp/ports`](../../bsp/ports) | 板级启动、Flash 和薄控制面端口 | 不应被扩展为第二个 DRV/FOC HAL |

### 12.1 当前实时链

```text
TMR1 中心对齐 PWM（20 kHz）
  -> CH4 触发 ADC1 注入组
  -> ADC1_IRQHandler()
  -> adc_read_phases()
  -> 默认产品每二次中断执行一次 foc_current_loop_step()（10 kHz）
  -> bench 镜像每四次中断执行一次 foc_current_loop_step()（5 kHz）
  -> Clarke/Park/PID/SVPWM
  -> pwm_out_set_duty()
  -> U4 INH/INL
  -> DRV8311H 六路半桥
```

```text
TMR5 motor soft IRQ（约 5 kHz）
  -> encoder_update()
  -> foc_outer_loop_step()
  -> iq_set
```

`ADC1_IRQHandler()` 的默认产品路径每两个采样执行一次完整 FOC，是因为 AT32F415 无 FPU、
soft-float 当前完整电流环接近 20 kHz 的单周期预算；这解释了“PWM/采样 20 kHz、FOC
计算 10 kHz”同时存在。专用 bench 镜像进一步每四个采样执行一次，使用 5 kHz FOC step
和 `PWM_DT=200 us`，给 Shell 和软浮点 ISR 留出更大余量。`foc_config.h` 中
`PWM_FREQ_HZ` 是软件 FOC step 名称，不是 TMR1 实际载波频率；两者必须分开解释。

`[实板测量]` 当前 bench 短脉冲结果显示：在 `vq=0.10–0.40 V`、`we=3.14 rad/s`、
约 50 ms 自动护栏内，VBUS 约 `11.90–11.91 V`，锁存的板上换算相电流最大约
`60 mA`，没有触发 `0.20 A` 快速过流。该数据来自 ADC/CSA 软件读数，不证明外部
相电流、实际门极波形或热稳定性；原始日志见 [实有板短脉冲结果](../../tmp/bench/20260829_actual/result.md)。

## 13. 无实板静态/数学模拟测试

本节使用主机 Python 浮点/整数公式和网表解析做确定性验证，结果只覆盖无板模型；
2026-08-29 的实板短脉冲证据另列于第 18 节，不与本节的无板 PASS 混合。

### 13.1 测试矩阵

| 用例 | 输入 | 结果 | 证明范围 |
| --- | --- | --- | --- |
| 网表关键连通性 | U4 pin、MCU PWM/ADC、供电网 | `PASS` | 证明文本网表中的逻辑连接；不证明焊接/走线 |
| H strap 解码 | R11/R12/R13=47 kΩ 到 GND | `PASS` | `6x PWM / 75 V/μs / 0.5 V/A / OCP 5 A typ` |
| CSA 零点与极性 | `I_motor=-2,-0.5,0,+0.5,+2 A` | `PASS` | 3.3 V、0.5 V/A、软件负号下可往返恢复 |
| CSA 线性窗口 | 0.25–3.05 V | `PASS` | nominal ±2.8 A，不证明动态线性 |
| ADC 分辨率 | 12 bit、3.3 V | `PASS` | 1.611722 mA/count |
| SOx RC | 330 Ω + 22 pF | `PASS` | τ=7.26 ns、fc≈21.922 MHz |
| 母线分压 | 100 kΩ/10 kΩ、20 V | `PASS` | 11:1，20 V 约 2256 code，满量程约 36.3 V |
| TMR1 静态计数 | 150 MHz、ARR=3749、CCR4=3740 | `PASS` | 中心对齐载波约 20 kHz；下数比较事件为峰值后 60 ns |
| SVPWM 全角度静态扫掠 | 当前 `foc_svpwm()`、归一化幅值 `2/3` | `PASS/边界提示` | 理想最大 duty≈0.8849；低侧窗口≈5.755 μs，rank4 结束≈3.660 μs，理想余量≈2.095 μs；不含传播/建立/噪声 |
| Id/Iq 参数应力模型 | `Rs=2.55 Ω`、`Ls=0.86 mH`、250 Hz PI、0.10 A step | `PASS/已修正` | `fc=25 Hz` 旧值峰值≈0.33245 A；`fc=1250 Hz` 当前值峰值≈0.10000 A |
| nFAULT→nSLEEP | R6/R7/D2 网络、7.4–20 V | `STATIC_FINDING` | nFAULT 低时 nSLEEP 仍约 3.3 V，不能形成预期 sleep 联动 |
| MCU 故障可见性 | 网表查找 U3 与 NFAULT | `STATIC_FINDING` | 无 U3 端点；软件不能观察 DRV 故障 |
| 热粗估 | RθJA、RDS、LDO 公式 | `PASS/风险提示` | 仅识别功耗量级；不宣称结温或散热通过 |

### 13.2 电流通道往返结果

采用 `SO=1.65 V + 0.5 V/A × I_driver`、`I_motor=-I_driver` 和 3.3 V/4095 ADC 模型：

| 电机约定电流 | 预期 SO 电压 | ADC 理想码 | 固件恢复值 |
| ---: | ---: | ---: | ---: |
| -2.00 A | 2.650 V | 3288.4 | -2.0000 A |
| -0.50 A | 1.900 V | 2357.7 | -0.5000 A |
| 0 A | 1.650 V | 2047.5 | -0.0000 A |
| +0.50 A | 1.400 V | 1737.3 | +0.5000 A |
| +2.00 A | 0.650 V | 806.6 | +2.0000 A |

该测试只验证公式、零点、增益和负号的一致性；它不能验证采样时刻、放大器偏置漂移、地弹、PWM 开关噪声或电机 KCL。

### 13.3 模拟测试的负责任边界

本轮 `[无板验证]` 明确没有证明：

- `OUTA/B/C` 的真实电压波形或上下管互锁波形；
- ADC 是否在所有 SVPWM 扇区和占空比下位于低侧有效采样窗口；
- U4 裸焊盘、功率回路、AGND/PGND、SOx RC 的实际 PCB 布局；
- DRV 结温、AVDD LDO 温升、短路自动重试热循环；
- 2804 的真实转速、转矩、振动、编码器相位或闭环稳定性；
- ESD、TVS、母线浪涌和 EMI 合规；
- nFAULT 在真实故障下的边沿、电压和 D2/R6 的脉冲能量。

## 14. 深度审查结论与优先级

### P0：必须在产品安全闭环前处理

1. **DRV 故障不可观测。** `nFAULT` 没有 MCU 端点，H 版本又没有故障寄存器；新增的应用 fault policy 只能依据 ADC/运行量间接 trip，不能知道 U4 内部故障原因。产品仍需明确“只依赖 U4 硬件保护”是否可接受，并补充 MCU 可观测/关断路径或独立急停策略。
2. **nFAULT→nSLEEP 联动不成立。** R6=3.3 kΩ 的 DCBUS 上拉强于 R7=10 kΩ，D2 又钳位到约 3.3 V；无板模型已证明工作母线范围内 `nFAULT=0` 仍高于 `VIH`。应先确认这是有意的“只提示、不睡眠”网络，还是阻容/齐纳选值错误。该硬件问题不会被新增的软件 ADC fault policy 自动修复。

### P1：必须在电气/热设计冻结前处理

3. **VIN_AVDD 与 VM 共接 DCBUS 的 LDO 功耗。** 这让 7.4–20 V 母线压差直接落在 U4 的 AVDD LDO；需要给 U3、U5、ESD/上拉等 `DRV_3V3` 负载做最坏值预算，并结合 Board1 铜区计算结温。
4. **CSAREF 去耦与共享电源。** 数据手册推荐 CSAREF 附近独立 100 nF；当前网表只有共享 `DRV_3V3` 去耦，C23 又不在 BOM/网表。需确认装配意图和实际摆放。
5. **低侧采样窗口。** 当前方程的全角度静态扫掠给出理想 duty/孔径边界，但还缺少基于真实占空比更新、相序、电流方向、传播延迟、CSA 建立时间和 ADC 转换时长的全扇区证明。
6. **Board1 版图证据缺失。** 当前没有 Board1 PcbDoc/Gerber/stackup，不能完成 QFN 裸焊盘、功率回路、地回流、SOx/CSAREF 隔离和热铜审查。

### P2：应在控制参数冻结前处理

7. **单一频率事实源。** TMR1 实际载波约 20 kHz，完整 FOC 每 2 次 ADC 执行约 10 kHz，而配置宏 `PWM_FREQ_HZ=10000` 的命名与注释容易引起误读；应在后续维护中把 carrier、ADC sample、FOC step 分成独立命名。
8. **CSA 误差与交叉相校正。** 当前软件只减每相零点，没有使用数据手册给出的 cross-phase correction 系数；在精密低速/正弦 FOC 场景应评估是否需要标定矩阵。
9. **保护阈值层级。** 软件默认峰值 2 A、DRV OCP 约 5 A typ、芯片推荐输出峰值 5 A 是三种不同语义；必须在产品文档中区分“控制限幅”“应用故障阈值”“最后一道硬件保护”。
10. **电流反馈参数同步。** 默认 `fc=1250 Hz` 已通过无板 RL+PI+LPF 模型，但持久化或 Shell 修改 `currbw` 后，当前代码只重算 PI 增益，尚未同步重建 Id/Iq LPF；应补充参数变更契约。

## 15. 复核清单

### 只读/无板阶段已完成

- [x] 主板原理图 3 页已读。
- [x] 主板网表关键网络已解析并与原理图交叉核对。
- [x] BOM 两个 sheet 已读取，C23 的网表/BOM 差异已标出。
- [x] TI DRV8311 数据手册关键页已读取：引脚、规格、H strap、PWM、CSA、保护、时序、布局、封装。
- [x] TI MD045B EVM 与 Board1 已分离，未把 EVM 连接当成 Board1 事实。
- [x] AT32F415 PWM、ADC、FOC、编码器和 fault runtime 边界已对照源码。
- [x] 公式、分压、RC、PWM 计时、nFAULT/nSLEEP 电阻网络已做主机侧确定性模拟。

### 有实物/版图后才可关闭

- [ ] 检查 U4 pin25 焊盘、热过孔和 PGND 回流。
- [ ] 示波器同时观察 TMR1 INH/INL、OUTA/B/C、SOx 和 ADC 触发点。
- [ ] 逐扇区验证低侧采样窗口和电流方向符号。
- [ ] 测量 `DRV_3V3`、`CSAREF`、`nSLEEP`、`nFAULT` 的上电/故障波形。
- [ ] 用最坏 DCBUS、外部 3V3 负载和目标相电流做热测试。
- [ ] 触发短路/过流/欠压/睡眠恢复，记录 H 版本固定自动重试行为。
- [ ] 验证电机相序、编码器电角度、FOC 电流环和机械负载稳定性。

## 16. 直接来源定位

- Board1 主原理图：[`pcb原理图.pdf`](<../pcb docs/pcb原理图.pdf>)，第 1–3 页。
- Board1 逻辑网表：[`pcb 网表.tel`](<../pcb docs/pcb 网表.tel>)，重点网络 `DCBUS`、`DRV_3V3`、`NFAULT`、`NSLEEP`、`SOA/SOB/SOC`、`INH*/INL*`、`VBUS`。
- Board1 BOM：[`pcb bom.xlsx`](<../pcb docs/pcb bom.xlsx>)，`bomģ��` 和 `Field����`。
- DRV8311 数据手册：[`drv8311.pdf`](<../pcb docs/drv8311.pdf>)；引脚/框图约第 3–6、17–23 页，电气规格约第 7–14 页，H 配置/PWM 约第 22、29 页，CSA/保护约第 31–46 页，布局/应用约第 75–85 页，订货/封装约第 87–93 页。
- TI EVM：[`MD045B(002)_Sch.PDF`](<../pcb docs/TI DRV8311H评估板/Project Outputs/MD045B(002)_Sch.PDF>)，第 1–3 页；只作参考设计。
- 电机：[`2804电机参考手册.pdf`](<../pcb docs/2804电机参考手册.pdf>)，第 2–4 页。

## 17. 本计划的配置契约与证据矩阵

本节把当前代码和原理图结论收敛为可重复的无板检查。脚本入口是
[`check_foc_peripheral_contract.ps1`](../../scripts/flows/host/check_foc_peripheral_contract.ps1)，只读生成源码、生成 pin header、FOC 硬件映射和 VBUS 数值防护；成功标记为 `FOC_PERIPHERAL_CONTRACT=PASS`。

| 对象 | 当前结论 | 证据类型 |
| --- | --- | --- |
| `at32f415_wk_config.c` | 150 MHz 系统/计时基准；APB1/APB2 `/2`；ADC1 priority 5、TMR5 6、USART1 7、USART2 8 | 源码确认 |
| `wk_tmr.c` | TMR1 双向中心对齐、ARR=3749；CH1/2/3 主/互补 PWM；CH4=3740 作为 ADC 触发；Break 关闭、deadtime=0、自动输出关闭 | 源码确认 |
| `wk_adc.c` + `at32f415_wk_config.h` | PA1/VBUS，PA4/PA5/PA6=IC/IB/IA；rank1/2/3/4=CH1/4/5/6；4 个注入通道、7.5 cycles、TMR1 CH4 触发 | 源码确认 |
| `wk_spi.c` + 生成 pin header | PB3/PB4/PB5=SPI1 SCK/MISO/MOSI；mode 3、16 bit、MSB first、软件 CS；PA15 为编码器 CS | 源码确认 |
| `motor_drv.c` | ABC→CH3/CH2/CH1；ADC rank→IC/IB/IA；offset 扣除后按 DRV8311H sense-FET 符号取负 | 源码确认 |
| `foc_ctrl.c` | VBUS finite 且大于 1 V 后才计算 `1/v_bus`；非法值清零调制量并关闭 PWM | 源码确认/主机验证 |
| U4 `nFAULT`/`nSLEEP` | 原理图/网表可见上拉与 RC 网络，但没有 MCU 端点；不能声明软件故障反馈或 GPIO 睡眠控制闭合 | 原理图确认 |
| U4 MODE/SLEW/GAIN | 47 kΩ 下拉静态解码为 6x PWM、约 75 V/μs、0.5 V/A，OCP 约 5 A typ | 原理图确认/数据手册 |
| TMR1 Break/deadtime | 当前生成配置是 `FALSE/0`，尚未有示波器或独立硬件关断证据 | 源码确认/实板未验证 |

### 17.1 保护边界的工程含义

当前软件快速过流值为 `1.00 A`，持续电流值为 `0.75 A` 持续 `3 s`；堵转规则为仅非电压模式下 `>=0.75 A` 且速度 `<=5 rpm` 持续 `200 ms`。这些是基于电机 `0.5 A` 额定/`2.0 A` 最大值的临时应用诊断阈值，不是 DRV8311H 的硬件 OCP 阈值；`12 V/1 A` 电源也只是限制电源输出能力，不能替代物理急停、独立硬件关断或驱动器保护。

当前 `FOC_ENERGIZED_CALIB_ENABLE=0U`，所以电机参数校准和编码器带电校准不能作为硬件配置验证手段执行；保留的只有不施加明显相电压/相电流的启动电流偏置校准。任何 `nFAULT`、VBUS、波形、温升、采样窗口或电机转矩结论仍必须标为实板未验证，不能由本脚本的 PASS 推导出来。

```text
REAL_BENCH_TEST_STATUS=BLOCKED
```

## 18. 2026-08-29 现有板短脉冲测量补充

为避免产品级 G0 条件阻塞第一轮故障发现，本轮使用专用 bench 镜像做低压、单变量、
毫秒级诊断。该轨道不执行带电校准、I-f、闭环或长时间运行；详细原始日志见
[实有板结果](../../tmp/bench/20260829_actual/result.md)。

`[实板测量]` bench ELF 经 OpenOCD `Verified OK` 写入，COM3 返回的启动状态为
`axis=idle`、`fault=0`、`vbus≈11.91 V`。D0-D4 固定/零矢量以及 O0-O3 低压开环
短脉冲中，VBUS 保持约 `11.90–11.91 V`，板上 ADC/CSA 换算的锁存相电流最大约
`60 mA`，没有触发 `FAULT_OVER_CURR`。O1 的 `theta_sim≈0.075 rad`、
`we_volt_now≈3/3.14 rad/s`，duty 约 `0.501/0.495/0.505`，说明软件开环角度
推进和短脉冲 SVPWM 计算已在实板运行。

`[源码确认]` bench 自动护栏约 50 ms 到期时以 `FAULT_COMM_ERROR` 关闭 PWM；该故障码
表示诊断预算到期，不能解释为 DRV 硬件故障。`[实板测量]` 通过该护栏的 `fault status`
读取到首故障上下文，证明停机后数据导出链可用。

这些结果不能证明：

- 物理 OUTA/OUTB/OUTC 相序和 TMR1 互补门极没有重叠；
- ADC 采样窗口在所有扇区都有足够建立时间；
- 独立相电流、DRV 结温、绕组温升和长时间稳定性；
- 低压下电机一定能克服齿槽/静摩擦并连续旋转。

因此当前分类为：低压短脉冲未复现高电流卡死；低压不转仍只有“电压/转矩裕量不足”
候选解释，唯一根因未确认。产品状态继续为：

```text
OPEN_LOOP_SOFTWARE_PATH=PASS
MOTOR_PHYSICAL_OPERATION=UNVERIFIED
OPEN_LOOP_ROOT_CAUSE_STATUS=UNRESOLVED
REAL_BENCH_TEST_STATUS=BLOCKED
CONTROLLED_DIAGNOSTIC_STATUS=ALLOWED_WITH_MINIMUM_GUARDS
```

## 19. 2026-08-30 分层正交实机诊断接口

`[原理图确认]` 当前默认硬件事实仍为 `A→TMR1 CH3、B→CH2、C→CH1`，ADC 注入序列仍为
`VBUS→IC→IB→IA`，即 `A→rank4、B→rank3、C→rank2`。本轮没有修改 PCB、`nSLEEP`、
`nFAULT`、默认相序、死区或保护阈值。

`[源码确认]` 为区分软件映射和物理连接，bench-only 提供：

- 6 种 PWM channel permutation；
- 6 种 ADC rank permutation；
- TMR1 CH4 compare 的合法扫描范围 `256..3740`，默认 `3740`；
- 每次 FOC 完成后的 64 条定点 TMR1/ADC/FOC ring sample；
- 32 条 SPI 编码器手动扫描 sample。

映射实验默认从 map0 开始，所有变量只在 IDLE 设置，重启后恢复默认。脚本矩阵必须逐层
通过：编码器静态、ADC 基线、固定矢量、PWM 映射、ADC 映射、触发位置、相位偏置，最后才
是低速旋转矢量。任何一个层级失败都不得用后续提高电压来掩盖。

`[静态推导]` 如果只有一个 PWM map 产生对称软件/板上电流，优先怀疑 channel/物理相序；
如果只有一个 ADC map 使三相电流平衡，优先怀疑 rank 解释；如果电流随 CH4 compare 明显
变化，优先怀疑 CSA 建立时间或采样位置。但这些是筛选假设，不是只靠软件 trace 得出的
物理定论。

`[实板未验证]` 当前仍没有新增矩阵对应的独立示波器波形、外部相电流、DRV 温度或连续机械
运行证据。12 V/1 A 不能替代人工急停、硬件故障关断和热/波形验收；`nFAULT/nSLEEP` 无
MCU 端点的事实也没有因新增 bench 命令而改变。

## 20. 2026-08-30 当前实板电流观测与功率响应

原始记录见 [`tmp/bench/20260830_074936_post_snapshot_fix/result.md`](../../tmp/bench/20260830_074936_post_snapshot_fix/result.md)。

`[实板测量]` bench400 显式刷写后，复位静态快照显示 VBUS 约 `11.91 V`、duty 为 `0/0/0`、软件相电流约 `0.012/0.001/0.007 A`。低压固定/旋转命令能够改变 ADC raw 和软件电流，说明控制器到功率级至少存在可观测响应；这不是对 DRV 输出波形或绕组实际电流的完整证明。

`[实板测量][静态推导]` CH4 compare 扫描的差异具有方向性：低 compare 时软件读数接近零，compare 接近载波峰值后软件读数达到约 `0.27 A` 量级。DRV8311H 的 SOx 来自低侧 sense-FET，因而该现象优先指向“触发时刻所处的低侧导通/CSA 建立窗口”，而不是立即指向 PWM 相序。当前 CH4=3740 仍是源码默认值；实验没有修改产品生成配置。

`[实板测量]` 2 V/20–30 rpm 时 software trace 电流峰值约 `0.54..0.68 A`，3 V 时一轮达到约 `0.9 A`，另一轮触发 `1 A` 快速保护；VBUS 约 `11.84..11.93 V`，未见同步母线塌陷。由于没有外部电流探头，必须把这些数值称为“ADC/CSA 软件观测值”。

`[实板未验证]` 下列硬件事实仍缺实测闭环：

- 三路 INH/INL 到 DRV8311H 的实际引脚波形、内部传播和互补重叠；
- SOA/SOB/SOC 的模拟波形、建立时间和真实采样孔径；
- DRV 温度、绕组温升、独立相电流和长时间机械稳定性；
- 编码器人工正/反向整圈与 7 对极的实板记录。

因此不能把“VBUS 稳定、软件电流有限、软件 duty 合法”解释为 DRV8311H 或电机已通过产品级验收。

## 21. 2026-08-30 TMR1/ADC trace 的硬件解释边界

`[实板测量]` 当前 bench400 trace 样本中可同时看到 `TMR1 CVAL`、`CCR1..4`、ADC 状态
和软件电流。例如一组停止前样本记录 `CCR1/2/3=1610/2143/1753`、`CCR4=3740`、
`CVAL≈1972`、`ADC sts=0x0A`。这证明软件快照能观察到“触发配置、FOC 输出和 ADC 完成
状态”在同一记录中的关系。

`[静态推导]` 在中心对齐计数下，`CCR4=3740` 接近 `ARR=3749`；ADC 是四通道注入
序列，软件 trace 记录的是转换完成/FOC 处理时的 `CVAL`，不等于示波器看到的实际
采样孔径。已有 compare 扫描在高 compare 区间出现软件电流约 `0.27 A` 量级、低
compare 区间约 `0.01 A` 量级的变化，因此优先保留“低侧 CSA 建立/采样相位”候选。

`[原理图确认]` DRV8311H 的 SOA/SOB/SOC 来自低侧电流检测路径，CSAREF 接到
`DRV_3V3`，GAIN strap 对应 `0.5 V/A`；`nFAULT` 没有 MCU 端点，`nSLEEP` 由板级网络
保持高电平而非软件控制。新增 `enc_dir` 和 trace 命令没有改变这些硬件边界。

`[实板未验证]` 没有示波器探头时仍不能判断：真实 INH/INL 是否重叠、deadtime=0 是否
在功率级安全、SOx 何时建立、软件电流是否等于绕组相电流、DRV 是否热稳定。当前软件
trace 只能作为板上 ADC/控制关系证据，不能替代这些物理测量。

## 22. 2026-08-30 ADC 触发位置重复测量

`[实板测量]` bench400 在 `vq=1.00 V`、`we=21.9911 rad/s`、`enc_dir=0` 条件下，
对 TMR1 CH4 compare `512` 和 `3740` 各重复两次。512 的软件电流峰值约为
`0.012/0.011/0.014 A`、`0.012/0.012/0.015 A`；3740 的峰值约为
`0.270/0.312/0.262 A`、`0.275/0.276/0.201 A`。512 的 raw ADC 大致停留在
`2042..2058`，3740 则出现 IA/IB/IC 约 `1882..2221`、`1888..2244`、`1922..2067`
的变化。VBUS 全程约 `11.87..11.93 V`，编码器机械角尾段变化接近零。

`[静态推导]` 该重复差异说明 MCU 侧 ADC/CSA 观测强烈依赖触发位置，下一步应优先核对
DRV8311H 低侧 SOA/SOB/SOC 建立时间、PWM 中心对齐的计数相位、ADC 注入序列完成时序
和采样孔径。`adc_trig=512` 不能被直接写成“正确窗口”，因为它可能只是读到掩盖真实
相电流的中心/无效值；3740 的软件换算值也不能代替独立相电流测量。

四次测试在 `state disable` 后出现 `FAULT_COMM_ERROR (0x100)`，原因是名义 350 ms 加
串口往返跨过 bench400 的 400 ms 预算；未出现 `FAULT_OVER_CURR`。应将其记录为测试
编排/时间窗口问题，而不是 DRV 过流结论。原始日志：
[`ADC trigger follow-up`](../../tmp/bench/20260830_095925_adc_trigger_followup/result.md)。

`[实板未验证]` 没有 SOx/PWM 示波器、独立相电流和温度数据，仍不能判断真实采样孔径、
死区/重叠、功率级电流或 DRV 热稳定性。

## 23. 2026-08-30 完整实机矩阵的硬件边界

`[实板测量]` 低幅 trigger、PWM/ADC map、固定矢量和 30 rpm 参数电压梯度均完成；
低幅 map 矩阵没有出现唯一软件正常映射，VBUS 在约 `11.86..11.95 V`，软件场可以
推进到约 `15 rad/s` 的尾值，但编码器没有稳定显示机械跟随。1 V 动态 `adc_trig=512`
与 `3740` 的软件电流差异仍然明显，而 1 V 固定矢量 trigger 扫描差异很小，说明
采样窗口候选与动态开关状态相关。

`[静态推导]` 结合原理图中 SOA/SOB/SOC 的低侧检测路径、CSAREF 接 `DRV_3V3` 和
当前 `GAIN=0.5 V/A`，下一步若要把候选收敛到 DRV/采样窗口，必须在停机后补充真实
SOx、PWM 互补输出和独立相电流证据。软件 trace 的 raw ADC、CVAL、CCR4 和换算电流
只能证明 MCU 侧观测关系，不能证明 DRV 输出电流或采样孔径。

完整实机汇总：[`full hardware root cause`](../../tmp/bench/20260830_103200_full_hardware_root_cause/result.md)。

`[实板未验证]` 当前仍不能宣称 PWM 波形、死区/重叠、SOx 建立时间、DRV 热稳定、
独立相电流、机械转速或产品级连续运行已经验证；不修改默认 ADC trigger、相序、
encoder direction 或保护阈值。

## 24. 2026-08-30 动态 CH4 compare 转折的硬件解释

`[实板测量]` 同一 bench400 镜像、`vq=1.00 V`、`we=21.9911 rad/s` 下，动态
`CH4 compare` 从 `512` 扫到 `3740` 的软件电流曲线在 `1874→2300` 处出现转折，
并在 `2812..3740` 保持高读数。对 `1874/2300/2812/3740` 各做两次，峰值分别为：

| compare | repeat 1 ia/ib/ic (A) | repeat 2 ia/ib/ic (A) |
| ---: | --- | --- |
| 1874 | 0.012/0.015/0.015 | 0.013/0.017/0.011 |
| 2300 | 0.058/0.256/0.022 | 0.090/0.259/0.017 |
| 2812 | 0.239/0.248/0.029 | 0.237/0.252/0.029 |
| 3740 | 0.242/0.264/0.030 | 0.243/0.254/0.029 |

VBUS 保持 `11.87..11.93 V`，无软件 fault。该结果把当前硬件—软件接口状态标记为：

```text
ADC_MEASUREMENT_WINDOW=REPRODUCIBLY_TRIGGER_DEPENDENT
```

`[静态推导]` TMR1 `150 MHz/ARR=3749` 的计数 tick 约 `6.667 ns`，中心对齐半周期
约 `25 us`；`3740` 接近半周期端点，`1874` 约在中点，`2300/2812` 处于两者之间。
由于中心对齐会出现上数/下数事件，必须同时看 trace 的 `CVAL`、方向、`CCR4` 和
ADC 状态。ADC 四个 injected rank 还是顺序转换，CH4 compare 改变会改变 IC/IB/IA
相对于 PWM 边沿的实际采样时间。

`[原理图确认][静态推导]` DRV8311H 的 SOA/SOB/SOC 是低侧 sense-FET 观测路径，
CSAREF 接 `DRV_3V3`，GAIN 按 `0.5 V/A` 换算。故“低读数”可能是低侧/CSA/ADC
窗口无效或尚未建立，而不是实际电流较小；“高读数”也仍需独立电流确认。不得把
`1874` 作为正确默认点，不得用这组软件读数替代 SOx/PWM 示波器和独立电流测量。

完整证据：[`full hardware root cause`](../../tmp/bench/20260830_103200_full_hardware_root_cause/result.md)、
[`boundary repeats`](../../tmp/bench/20260830_104326_dynamic_trigger_boundary_repeats_1V_30rpm/)。

## 25. 2026-08-30 触发相位静态隔离与功率响应

`[原理图确认]` DRV8311H 的 SOA/SOB/SOC 来自低侧电流检测路径，CSAREF 接 `DRV_3V3`，
当前 GAIN strap 按 `0.5 V/A` 换算。`[实板测量]` 即使 `we=0`，改变 TMR1 CH4 compare
仍会在软件 ADC/CSA 观测上产生稳定转折：1874 约 `0.01 A`，2300 约 `0.26 A` 的
B 相读数，2812/3740 约 `0.24..0.26 A` 的 A/B 读数。该结果优先指向 SOx/CSA 建立、
中心对齐 PWM 导通相位、注入组顺序和 ADC 孔径的联合问题，而不是单纯旋转场速度。

`[静态推导]` `CCR4=3740` 接近 `ARR=3749`，`1874` 位于中心附近；四个 injected rank
仍按 `VBUS→IC→IB→IA` 顺序完成。compare 改变时，不仅改变触发事件位置，也改变各
顺序通道相对于低侧导通边沿的时间关系。由于当前 trace 记录的是 MCU 侧转换完成时的
软件快照，不能由此直接求出真实 SOx 建立时间。

`[实板测量]` 1.0–2.0 V、约 375 ms 的电压窗口中，软件峰值随电压总体增加到约
`0.633 A`，VBUS 仍约 `11.84..11.93 V`，但 encoder 没有稳定跟随软件场。没有独立
相电流、PWM/SOx 波形和机械测速时，功率级、转矩裕量、机械负载和测量链仍不能唯一
区分；不得把高/低软件读数分别等同为真实高/低绕组电流。

完整证据：[`static trigger boundary`](../../tmp/bench/20260830_105417_static_trigger_boundary_1V/)、
[`2 V window`](../../tmp/bench/20260830_110109_voltage_full_window_2V_30rpm/)。
状态保持 `ADC_MEASUREMENT_WINDOW=REPRODUCIBLY_TRIGGER_DEPENDENT`、
`OPEN_LOOP_ROOT_CAUSE_STATUS=UNRESOLVED`、`MOTOR_PHYSICAL_OPERATION=UNVERIFIED`。
