# FalconFOC V2 HW02-A｜复位安全测试准备审计

- 审计日期：2026-11-10
- 状态：**批准只读准备，未执行实板供电与波形观测**。
- 关联阶段：G3-SAFE-HW01 资源审计复核。
- 工作树基线：`1f542521189d5e0ef25e64e6530608ece8d6e137`
- 本记录没有修改 Legacy ISR、TMR1、ADC、WDT/WWDT 配置、板级寄存器或根 CMake。
- 本记录没有启动调试器、刷写固件、连接 DCBUS、注入外部 3.3 V、使能 PWM 或执行电机测试。

## 1. HW02-A 的边界

HW02-A 只负责把后续复位波形测试所需的电源、寄存器、引脚和测量前提写成可审查的准备记录。它不产生实板安全结论。

HW02-B 才负责在新的单独授权下观察 `NRST`、六路 `INH/INL`、`nSLEEP`、`nFAULT` 和最终禁止动作。HW02-B 当前没有授权。

## 2. Board1 电源路径

当前网表和硬件 Wiki 给出的电源关系是：

```text
DCBUS
  ├─> U4 VIN_AVDD
  ├─> U4 VM
  └─> VBUS 分压 -> U3 ADC

U4 AVDD LDO
  └─> DRV_3V3
       ├─> U3 AT32F415 VDD 网络
       ├─> U5 编码器
       ├─> U4 CSAREF
       └─> R10 / nFAULT 上拉及其他 3.3 V 负载
```

### 2.1 测试前必须确认的事实

| 项目 | 当前静态结论 | 证据等级 | HW02-A 要求 |
| --- | --- | --- | --- |
| U4 `VIN_AVDD` | 与 DCBUS 同网 | `source-confirmed` | 记录允许的 DCBUS 测试范围 |
| U4 `VM` | 与 DCBUS 同网 | `source-confirmed` | 明确电机母线是否保持未接 |
| U4 `AVDD` | 由 U4 内部 LDO 产生 | `source-confirmed` | 测量 DRV_3V3 与 U3 VDD 的依赖 |
| U3 供电 | 进入 DRV_3V3 网络 | `source-confirmed` | 不假设 DCBUS 断开后 MCU 仍可运行 |
| 外部 3.3 V 注入 | 可能向 U4、CSAREF、nFAULT、nSLEEP 反向供电 | `not-available` | 未完成反向供电审查前禁止注入 |
| 仅断开电机 | 不等于功率级断电 | `source-confirmed` | 测试计划必须区分“无电机”和“无 DCBUS” |

### 2.2 测试安全前提

HW02-B 之前必须书面确认以下条件：

1. DCBUS 由什么电源提供，限流和最大电压是多少。
2. 电机连接器是否完全断开，OUTA/OUTB/OUTC 是否有外部负载。
3. U3、U4、U5 和调试器之间是否存在额外供电路径。
4. 示波器探头地、逻辑分析仪地和板级功率地如何连接。
5. 外部 3.3 V 是否会通过 U3 VDD、CSAREF、nFAULT 或 nSLEEP 反向供电。
6. 在什么状态下可以停止测试，谁负责物理断电。

## 3. TMR1 启动寄存器矩阵

### 3.1 当前生成式配置

| 资源 | 当前配置 | 代码位置 |
| --- | --- | --- |
| CH1/2/3 output state | `TRUE` | `wk_tmr.c` 的 `tmr_output_struct.oc_output_state` |
| CH1C/2C/3C output state | `TRUE` | `wk_tmr.c` 的 `tmr_output_struct.occ_output_state` |
| CH1/2/3 CCR | `0` | `tmr_channel_value_set()` |
| CH4 CCR | `3740` | ADC 触发边界 |
| 主输出 `TMR1->brk_bit.oen` | `TRUE` | `tmr_output_enable(TMR1, TRUE)` |
| 计数器 | `TRUE` | `tmr_counter_enable(TMR1, TRUE)` |
| Break `brken` | `FALSE` | `tmr_brkdt_config()` 输入 |
| 自动输出 `aoen` | `FALSE` | `tmr_brkdt_config()` 输入 |
| Break 极性 | `TMR_BRK_INPUT_ACTIVE_LOW` | `tmr_brkdt_config()` 输入 |
| deadtime | `0` | `tmr_brkdt_config()` 输入 |
| CH1/2/3 主/互补极性 | `ACTIVE_HIGH` | `tmr_output_channel_config()` 输入 |
| 主/互补 idle state | `FALSE` | `tmr_output_channel_config()` 输入 |

### 3.2 必须从寄存器推导的字段

HW02-A 不得只记录 API 参数，还必须列出最终寄存器字段：

- `TMR1->brk_bit.oen`
- `TMR1->brk_bit.aoen`
- `TMR1->brk_bit.brken`
- `TMR1->cctrl_bit.c1en/c1cen`
- `TMR1->cctrl_bit.c2en/c2cen`
- `TMR1->cctrl_bit.c3en/c3cen`
- `TMR1->cctrl_bit.c1p/c1cp`
- `TMR1->cctrl_bit.c2p/c2cp`
- `TMR1->cctrl_bit.c3p/c3cp`
- CH1/2/3 比较模式字段和对应的 `C1ORAW/C2ORAW/C3ORAW` 输出解释
- `TMR1->pr`、`c1dt`、`c2dt`、`c3dt` 和计数方向

### 3.3 `CCR=0` 不等于 `INHIBIT`

DRV8311H 6x PWM 的输入真值表是：

| INH | INL | 半桥状态 |
| ---: | ---: | --- |
| 0 | 0 | Hi-Z |
| 1 | 0 | 高边导通 |
| 0 | 1 | 低边导通 |
| 1 | 1 | Hi-Z |

当前源码只能证明 `CCR=0` 且通道被打开。它不能单独证明实际输出是 `00`、`01` 还是其他组合。HW02-A 必须完成以下推导：

1. TMR1 PWM mode A 在中心对齐上数和下数阶段的 `C1ORAW` 行为。
2. 主输出和互补输出的极性处理。
3. `CCR=0` 时主输出与互补输出的组合。
4. `oen`、channel enable 和 idle state 对最终 GPIO 电平的影响。
5. 该组合映射到 DRV8311H 后是 Hi-Z、低边导通、高边导通还是过渡状态。

如果推导无法排除低边持续导通或主动制动，HW02-B 必须把它列为首要观测目标。

## 4. nFAULT、nSLEEP 与 R7 通路

网表给出：

```text
NFAULT = U4.1 + R7.2 + R10.1 + U13.2
NSLEEP = U4.24 + R7.1 + R6.1 + D2.1 + C9.2 + U12.2
R7     = 10 kΩ
R6     = 3.3 kΩ，另一端接 DCBUS
R10    = 10 kΩ，连接 NFAULT 与 DRV_3V3
```

### 4.1 当前可确认的边界

- `nFAULT` 与 `nSLEEP` 不是完全电气隔离，R7 提供电阻通路。
- `nFAULT` 没有 MCU 采集端点。
- `nSLEEP` 没有 MCU 控制端点。
- R7 通路能否把 `nSLEEP` 拉到 DRV Sleep 阈值，取决于 DCBUS、D2 钳位、电阻容差、节点负载和 DRV 输入电流。
- DRV 内部故障自动重试不等于软件故障锁存。

### 4.2 HW02-A 计算输出

基于当前 nominal 电阻值和 D2 约 3.3 V 钳位的静态近似，可以先得到以下计算结果。该表没有代替实测，也没有把 DRV 输入漏电、D2 动态特性和电阻容差隐藏掉。

| 条件 | DCBUS | nFAULT | nSLEEP 静态近似 | R6 电流 | D2/R7 处理 |
| --- | ---: | ---: | ---: | ---: | --- |
| 上电正常 | 7.4–20 V | 高 | 约 3.3 V，受 D2/DRV_3V3 条件影响 | 待测 | nFAULT 通过 R10 上拉 |
| nFAULT 拉低 | 7.4 V | 低 | 约 3.3 V | 约 1.24 mA | 约 0.33 mA 经 R7，余量进入 D2 |
| nFAULT 拉低 | 12 V | 低 | 约 3.3 V | 约 2.64 mA | 约 0.33 mA 经 R7，余量进入 D2 |
| nFAULT 拉低 | 16 V | 低 | 约 3.3 V | 约 3.85 mA | 约 0.33 mA 经 R7，余量进入 D2 |
| nFAULT 拉低 | 20 V | 低 | 约 3.3 V | 约 5.06 mA | 约 0.33 mA 经 R7，余量进入 D2 |

这个静态模型的关键结论是，R7 的确参与了故障电流分配，但在 D2 正常钳位并且 R6 接入 DCBUS 的假设下，nFAULT 拉低不自动意味着 nSLEEP 低于 Sleep 阈值。不能把 R7 通路写成硬件关断，也不能把它写成完全无效。下一步仍需补充 DRV nSLEEP 输入阈值、D2 实际钳位、容差和节点波形。

## 5. HW02-B 的前置条件

HW02-B 只有在以下条件全部完成后才可单独申请：

- HW02-A 电源路径和反向供电审查通过。
- TMR1 寄存器矩阵和 `CCR=0` 逻辑推导完成。
- 示波器/逻辑分析仪探头与接地方案通过审查。
- DCBUS 限流、电机断开和物理断电动作明确。
- 测量点包含 `NRST`、六路 `INH/INL`、`DRV_3V3`、U3 VDD、`nSLEEP` 和 `nFAULT`。
- WDT 不混入第一次启动波形测试。WDT 需要单独的配置和复位测试授权。

## 6. 当前判定

| 项目 | 状态 |
| --- | --- |
| HW01 资源与源码审计 | 接受，补充 R7、电源依赖和 `CCR=0` 不确定性 |
| HW02-A 只读准备 | **进行中，已批准** |
| HW02-B 实板供电与波形观测 | **待单独授权** |
| WDT 实际复位测试 | **未授权** |
| G3-SAFE 最终硬件验收 | **NOT PASSED** |
| G3-PWR | **LOCKED** |
