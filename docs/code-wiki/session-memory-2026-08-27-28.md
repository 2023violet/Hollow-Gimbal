# Hollow-Gimbal 调试会话记忆与交接记录

> 会话范围：2026-08-27 至 2026-08-28（本地时间）
> 项目：`D:/study/Software/Hollow-Gimbal`
> 目标：AT32F415 + DRV8311H + 2804 云台 BLDC 电机低电压开环 FOC 卡死/高电流根因定位

## 1. 这份文档的用途

这是一份可在仓库内直接查看的“显式会话记忆”和交接快照，用来保存昨晚调试中的关键事实、决策、代码修改、验证结果和下一步边界。它不是完整聊天记录，也不替代源码、原理图和测试输出；发生冲突时，优先级如下：

1. 当前源码、生成外设配置和实际测试输出；
2. `docs/pcb docs/` 中的原理图、网表、BOM 和芯片资料；
3. 本文和 Code Wiki 中的整理结论；
4. 维护日志中的历史描述。

证据标签约定：

| 标签 | 含义 |
| --- | --- |
| `[源码确认]` | 当前工作树源码、头文件、CMake 或生成代码直接支持 |
| `[原理图确认]` | Board1 原理图、网表或 BOM 可见 |
| `[静态推导]` | 根据代码、时序、公式或网络关系推导，尚未用仪器确认 |
| `[无板验证]` | 主机测试、CTest、静态脚本或交叉编译实际通过 |
| `[实板未验证]` | 需要示波器、串口、调试器、供电或电机才能证明 |

原始 Codex 会话仍保存在用户目录的 session rollout 中，当前会话 ID 为：

```text
01a03f17-924c-7b13-8246-54ce286cc661
```

原始回放路径：

```text
C:\Users\16429\.codex\sessions\2026\08\27\rollout-2026-08-27T01-29-34-01a03f17-924c-7b13-8246-54ce286cc661_01a03f1e-bfb3-7773-8d18-0835f61887c7.jsonl
```

原始回放是审计材料，不应作为日常调试入口；日常应以本文和下列 Code Wiki 为入口：

- [architecture.md](architecture.md)：整体分层、启动链和实时调用边界；
- [modules.md](modules.md)：模块职责、关键类型和接口；
- [hardware-and-drv8311h.md](hardware-and-drv8311h.md)：原理图、DRV8311H 和采样/功率级分析；
- [configuration-audit.md](configuration-audit.md)：外设配置审计；
- [motor-debugging-safety-and-procedure.md](motor-debugging-safety-and-procedure.md)：保护和停机规程；
- [motor-real-bench-test-plan.md](motor-real-bench-test-plan.md)：后续实机测试方案；
- [operations-and-validation.md](operations-and-validation.md)：命令和无板验证证据。

## 2. 最初的问题和调试原则

现场现象是：电机即使使用低电压开环命令，也不能流畅运行，经常卡死、抖动并出现偏高电流。这个现象不能直接归因于“电压不够”，因为在低电压、小命令下出现异常大电流，首先要排除坐标映射、PWM 相序、ADC rank、电流符号、offset、采样窗口、DRV 供电和功率级问题。

本轮已经固定以下原则：

- 运行中只观察预定义硬停条件，不在保护路径中做复杂分析；
- 异常时优先关闭 PWM、锁存上下文、切断电源，电机停止后再分析；
- ISR 不打印、不格式化浮点、不写 Flash、不发串口、不等待任务；
- 每次实机试验只改一个变量；
- 12 V/1 A 只是电源能力限制，不能替代相电流保护、硬件关断或人工急停；
- 主机测试只能证明软件逻辑和数学模型，不能证明实机电机能转；
- 当前不执行 I-f、编码器带电校准、`encoff` 搜索、速度闭环或长时间运行。

## 3. 板级和芯片关键事实

### 3.1 供电拓扑

```text
DCBUS
  -> DRV8311H VIN_AVDD / VM
  -> DRV8311H AVDD LDO
  -> DRV_3V3
  -> AT32F415、编码器、CSAREF、上拉网络
```

- `[原理图确认]` U4 为 `DRV8311HRRWR`，使用 H 硬件接口模式，没有软件 SPI 寄存器配置面。
- `[原理图确认]` `VIN_AVDD` 和 `VM` 接 `DCBUS`；DRV 的 AVDD LDO 生成 `DRV_3V3`。
- `[原理图确认]` `nSLEEP` 由 DCBUS/R6 网络自动拉高，不由 MCU 控制；本阶段不修改、不拉低。
- `[原理图确认]` `nFAULT` 没有 MCU 端点，不能作为软件实时故障输入；只能在可测时观察电平。
- `[静态推导]` 当前 `nFAULT` 低电平不能可靠地把 `nSLEEP` 拉到低于 VIH，因此不能把该网络当作完整的硬件故障联动。
- `[实板未验证]` DRV AVDD LDO 在当前母线和负载下的热稳定性、DRV 保护响应时间和实际功率地回路仍未测量。

### 3.2 PWM、ADC 和 SPI 映射

| 功能 | 当前配置 | 证据和边界 |
| --- | --- | --- |
| PWM 载波 | TMR1 中心对齐，ARR=3749，约 20 kHz | `[源码确认]`；载波波形和死区仍未示波器确认 |
| FOC 计算 | 每两个 ADC tick 执行一次，约 10 kHz | `[源码确认]`；真实 ISR 周期仍需仪器确认 |
| A 相 | 软件 A → TMR1 CH3 | `[源码确认]`/`[静态推导]` |
| B 相 | 软件 B → TMR1 CH2 | `[源码确认]`/`[静态推导]` |
| C 相 | 软件 C → TMR1 CH1 | `[源码确认]`/`[静态推导]` |
| ADC rank1 | VBUS → CH1 | `[源码确认]` |
| ADC rank2 | IC → CH4 | `[源码确认]` |
| ADC rank3 | IB → CH5 | `[源码确认]` |
| ADC rank4 | IA → CH6 | `[源码确认]` |
| ADC 触发 | TMR1 CH4 注入触发 | `[源码确认]` |
| SPI1 | PB3/PB4/PB5，mode 3，16 bit，MSB first，软件 CS=PA15 | `[源码确认]` |

重要映射链：

```text
ADC rank2/3/4
  -> IC/IB/IA raw
  -> offset 扣除和电流符号换算
  -> 三相电流重构
  -> Clarke/Park
  -> Id/Iq PI、解耦、反 Park
  -> SVPWM
  -> A/B/C duty
  -> CH3/CH2/CH1
```

### 3.3 当前保护阈值

| 项目 | 当前值 | 动作 |
| --- | ---: | --- |
| 预警电流 | `0.10 A` | 任务级持续计时 |
| 堵转判定 | `0.12 A` 且速度低于 `5 rpm` | 持续 `200 ms` 故障 |
| 快速过流 | `0.20 A` | ADC 快速路径立即关 PWM |
| 高电流持续 | 接近预警阈值持续 | `3 s` 任务级后备保护 |
| VBUS 有效数值下限 | `>1.0 V` 且有限 | 防止调制除零和 NaN/Inf |
| 实机 VBUS 包线 | `10.8–13.2 V` | 超出立即停机 |

电流换算当前为：

```text
I = -(raw - offset) × 3.3 / (4095 × 0.5)
1 ADC count ≈ 1.6117 mA
0.10 A ≈ 62 counts
0.12 A ≈ 75 counts
0.20 A ≈ 124 counts
```

## 4. 昨晚确认的关键软件问题

### 4.1 电流环 LPF 和带宽曾经失配

原先电流环带宽为 `250 Hz`，但 LPF 截止频率错误地为 `25 Hz`。这会让反馈明显滞后，PI 在反馈尚未跟上时继续增加电压，造成小电流指令下的瞬态过冲风险。

当前规则：

```text
cutoff_hz = curr_bandwidth × (DEFAULT_CUTOFF_HZ / DEFAULT_CURR_BW_HZ)
DEFAULT_CUTOFF_HZ = 1250 Hz
DEFAULT_CURR_BW_HZ = 250 Hz
DEFAULT_SAMPLING_HZ = 10000 Hz
```

`foc_lpf_init()` 初始化输出和系数；`foc_lpf_refresh()` 只重算系数并保留滤波历史，合法运行时 `currbw` 修改同时刷新 PI 和 LPF，非法/非有限带宽被拒绝。

### 4.2 带电校准必须在 bring-up 阶段关闭

快速过流阈值为 `0.20 A`，而历史默认带电校准激励存在 `0.30 A` 和 `1.0 A` 量级。通过提高保护阈值来迁就校准是错误方向，因此当前固件默认：

```c
#define FOC_ENERGIZED_CALIB_ENABLE (0U)
```

当前保留：

- 上电后的非带电电流偏置校准；
- 故障查询和清除；
- 只读诊断。

当前禁止：

- 电机参数带电校准；
- 编码器带电校准；
- 任何需要施加明显相电压/相电流的校准入口。

### 4.3 原有观测数据不足

此前 `obs` 不能同时关联以下数据：

```text
ADC raw -> offset -> phase current
VBUS raw -> VBUS
theta_sim/theta_elec -> we_volt_now/we_volt_ramp
vd/vq -> alpha/beta -> A/B/C duty -> sector
```

没有这组关系，就无法区分“软件内部 duty 正确但物理相序错误”和“ADC 观测已经错误”。因此本轮增加了停机后可读的故障上下文和扩展 `obs all`。

## 5. 已落地的代码实现

### 5.1 一次性故障上下文

新增 `struct motor_fault_context`，保存：

- `sequence`、第一次 `fault_code`、`axis_state`、`ctrl_mode`、`sector`；
- 三相 raw ADC、三相 offset 扣除后的电流；
- `v_bus_raw`、`v_bus`；
- `id`、`iq`；
- `theta_sim`、`theta_elec`、`we_volt_now`、`we_volt_ramp`；
- `vd`、`vq`、`v_alpha`、`v_beta`；
- `dtc_a`、`dtc_b`、`dtc_c`。

新增接口：

```c
bool motor_fault_context_read(struct motor_fault_context *out);
void motor_fault_context_clear(void);
```

行为：

1. 快速故障路径在 `pwm_out_disable()` 前锁存第一次上下文；
2. 后续故障不能覆盖第一次上下文；
3. 旧上下文不能抑制新的 `fault_code` 锁存和 PWM 关闭动作；
4. 只在启动初始化或明确 `fault clear` 时清除；
5. 上下文只在 RAM 中，不写 Flash；
6. `fault status` 在停机后导出上下文；
7. 正常 `state disable` 不清除最后一次测量/控制值，`obs all` 仍可读取。

实现入口：[motor_fault_runtime.c](../../applications/runtime/motor_fault_runtime.c) 和 [motor_fault_runtime.h](../../applications/runtime/motor_fault_runtime.h)。

### 5.2 IDLE-only 诊断写入

以下入口现在由 runtime 和 Shell 双层保护：

- `motor_runtime_diag_set_voltage()`；
- `motor_runtime_diag_set_if()`；
- `motor_runtime_diag_set_phase_volt()`；
- `motor_runtime_request_set_passthrough()`；
- `motor_runtime_diag_set_param()`，包括 `currbw`、`encoff`、保护参数和电机参数；
- `motor_runtime_diag_set_pid()`；
- `motor_runtime_diag_reset_pid()`。

在 `RUNNING` 或 `CALIB` 状态写入不会改变控制量，Shell 返回：

```text
result=error reason=idle_only
```

### 5.3 bench 固件

新增 CMake cache：

```text
THEMIS_ENABLE_BENCH_DIAG
```

新增 preset：

```text
fw-full-diag-carrier-bench-debug
build-fw-full-diag-carrier-bench-debug
```

bench 编译定义强制为：

```text
TM_BENCH_DIAG=1
TM_FEATURE_VOFA=0
FOC_ENERGIZED_CALIB_ENABLE=0
```

它保留 Shell，但关闭 VOFA 二进制输出，避免 Shell 与遥测共用 USART1 引起接收溢出。bench 固件仍不是实机安全认证固件。

## 6. 测试先行和异常修复记录

本轮遵循：

```text
先增加契约测试
→ 确认缺接口/缺门禁时失败
→ 修改最小实现
→ 专项测试
→ 全量 host/CTest
→ 静态外设契约
→ 固件构建
```

期间发现一个重要回归：新增一次性上下文后，旧测试 fixture 只清零 `motor_dev`，没有显式清除全局故障上下文，导致后续独立故障用例被旧上下文影响。随后补充了测试隔离和“旧上下文不得抑制新 fault latch”的回归测试；该测试先失败，再通过最小修复 `_latch_fault()` 完成。

这条记录说明：

- “只保存第一次上下文”与“必须继续锁存新的故障码”是两个不同约束；
- 测试 fixture 必须显式模拟 `fault clear`，不能依赖清零局部结构体；
- 生产代码不能为了让测试通过而放松故障锁存语义。

## 7. 最终无板验证证据

### 7.1 主机和静态门禁

| 命令 | 结果 |
| --- | --- |
| `run_host_tests.ps1 -RepoRoot D:/study/Software/Hollow-Gimbal` | `33/33 PASS` |
| `ctest --preset test-host-debug --output-on-failure` | `33/33 PASS` |
| `check_foc_peripheral_contract.ps1` | `FOC_PERIPHERAL_CONTRACT=PASS` |
| Markdown 相对链接检查 | `PASS`，检查 13 个文档 |
| 本轮相关文件尾随空格检查 | `PASS` |
| 本轮相关 tracked 文件定向 `git diff --check` | `PASS` |

### 7.2 固件构建

| 变体 | FLASH | RAM | 结果 |
| --- | ---: | ---: | --- |
| full-diag | `77,352/131,072 B`（59.01%） | `21,720/32,768 B`（66.28%） | PASS |
| standard | `40,356/131,072 B`（30.79%） | `19,984/32,768 B`（60.99%） | PASS |
| tiny | `37,132/131,072 B`（28.33%） | `18,248/32,768 B`（55.69%） | PASS |
| bench full-diag | `76,732/131,072 B`（58.54%） | `21,328/32,768 B`（65.09%） | PASS |

bench ELF 的 `arm-none-eabi-nm` 检查未发现 `vofa_` 符号，且 `compile_commands.json` 包含三个强制安全定义。

### 7.3 全局差异检查的边界

全局 `git diff --check` 仍会报告既有/构建生成文件中的格式问题，例如：

- `build/CMakeFiles/CMakeConfigureLog.yaml`；
- `build/output/falcon-foc-at32.map`；
- `platform/at/at32f415/generated/src/main.c`；
- `work/project/` 下的生成/备份文件。

本轮没有为了消除这些既有问题而修改、清理、回退或覆盖用户文件；本轮相关代码、脚本和文档已经单独检查通过。

## 8. 当前结论

当前可以确认：

```text
OPEN_LOOP_SOFTWARE_PATH=PASS
```

这里的 `PASS` 只表示：故障保护、诊断上下文、IDLE-only 门禁、LPF/PI 同步、带电校准门禁、FOC 电压开环数学路径、SVPWM 边界、VBUS 数值防护、外设静态契约和无板构建测试通过。

当前不能确认：

```text
MOTOR_PHYSICAL_OPERATION=UNVERIFIED
REAL_BENCH_TEST_STATUS=BLOCKED
```

## 21.5 2026-08-30 完整实机矩阵

`[实板测量]` 本轮完成 9 点 ADC trigger、36 组 PWM/ADC map、六向固定矢量、1 V
静态相位矩阵、`vq=0.25..2.00 V` 的 30 rpm 参数电压梯度、正反向动态、
`theta0=0/+π/2` 动态 A/B 和 `enc_dir=0/1` 对照。每项均在前置 reset 后短时运行，
停机后读取 trace，未执行校准、参数写入或 `state stop`。

- 低幅固定矢量下 trigger 9 点约 `0.009..0.023 A`，36 组 map 约 `0.008..0.028 A`，
  六向约 `0.011..0.028 A`；没有唯一 map 优胜或单相饱和。
- `we=21.9911 rad/s`（约 30 rpm 机械目标）下电压升至 2 V，软件峰值最高约
  `0.468/0.450/0.065 A`，VBUS 稳定，软件 theta/ramp 正常，但编码器未稳定跟随。
- 动态高电压时 `adc_trig=512` 与 `3740` 的软件电流差异仍明显；低幅静态扫描则差异较小，
  采样窗口/CSA 建立时间候选保留。
- 1 V 相位和 encoder direction A/B 没有形成同条件、可重复的唯一机械改善，产品默认配置不改。

完整汇总：[`full hardware root cause`](../../tmp/bench/20260830_103200_full_hardware_root_cause/result.md)。
当前仍不能把软件 trace 解释为独立相电流，也不能宣称 PWM/SOx 波形、死区、温度或产品级
连续运行已验证。

```text
ADC_MEASUREMENT_WINDOW=ANOMALOUS_OR_STRONGLY_SENSITIVE
PHASE_OFFSET_CANDIDATE=REJECTED_AS_UNREPEATED
ENCODER_DIRECTION_CANDIDATE=SUPPORTED_NOT_CONFIRMED
OPEN_LOOP_SOFTWARE_PATH=PASS
MOTOR_PHYSICAL_OPERATION=UNVERIFIED
OPEN_LOOP_ROOT_CAUSE_STATUS=UNRESOLVED
REAL_BENCH_TEST_STATUS=BLOCKED
```

## 23. 2026-08-30 后续实机证据：静态 trigger 与 1–2 V 窗口

`[实板测量]` 为区分“旋转场问题”和“采样窗口问题”，将动态扫描的 `we` 设为零，
`adc_trig=1874/2300/2812/3740` 仍重复得到约 `0.01 A`、`0.26 A` B 相转折和
`0.24..0.26 A` A/B 平台。VBUS 约 `11.87..11.93 V`，无 fault。结论是触发相关性
在固定矢量/固定 PWM 条件下也存在，不能只用机械卡死解释。

随后使用 `we=21.9911 rad/s` 和约 375 ms bench400 窗口测试 1.0 V（含重复）、1.5 V、
2.0 V。软件峰值分别约为 `0.257/0.256/0.033 A`、`0.273/0.277/0.233 A`、
`0.407/0.435/0.420 A`、`0.586/0.633/0.578 A`；软件场角推进而 encoder 角度仅
部分跟随。没有软件 fault，所有轮次结束后均回到 `axis=idle、fault=0、duty=0/0/0`。

这批数据只新增两点记忆：

- `ADC_MEASUREMENT_WINDOW=REPRODUCIBLY_TRIGGER_DEPENDENT` 在 `we=0` 仍成立；
- 电压从 1.0 V 增加到 2.0 V 会提高软件观测电流，但尚未得到稳定 30 rpm 机械跟随。

不得将低 compare 低读数当作正确采样窗口，也不得将软件峰值当作独立相电流。状态
继续保持 `OPEN_LOOP_ROOT_CAUSE_STATUS=UNRESOLVED`、`MOTOR_PHYSICAL_OPERATION=UNVERIFIED`、
`REAL_BENCH_TEST_STATUS=BLOCKED`。原始目录：[`static`](../../tmp/bench/20260830_105417_static_trigger_boundary_1V/)、
[`voltage 1 V`](../../tmp/bench/20260830_105810_voltage_full_window_1V_30rpm/)、
[`voltage 1.5 V`](../../tmp/bench/20260830_110024_voltage_full_window_1p5V_30rpm/)、
[`voltage 2 V`](../../tmp/bench/20260830_110109_voltage_full_window_2V_30rpm/)。

## 21.4 2026-08-30 ADC trigger follow-up

`[实板测量]` bench400 镜像 `59B78D648511926F8AD47213CA35E49C0D536697BE814B1B77AF64F84E949A03`
下，固定 `vq=1.00 V`、`we=21.9911 rad/s`、`enc_dir=0`，将 `adc_trig=512` 和
`adc_trig=3740` 各重复两次。512 的软件电流峰值约为 `0.012/0.011/0.014 A` 和
`0.012/0.012/0.015 A`；3740 约为 `0.270/0.312/0.262 A` 和
`0.275/0.276/0.201 A`。VBUS 保持 `11.87..11.93 V`，编码器机械角尾段变化近零，
每项 trace 约 2000 条。

`[实板测量][静态推导]` 触发点差异具有重复性，优先保留“低侧 CSA 建立时间、中心对齐
PWM 相位、ADC 注入完成时序或采样孔径”候选。512 不能直接认定为正确采样点，可能只是
掩盖真实电流的中心/无效窗口；3740 的软件电流也不是独立相电流计读数。

四次测试均因名义等待加串口往返跨过 bench400 预算，在 `state disable` 后出现
`FAULT_COMM_ERROR (0x100)`，没有出现 `FAULT_OVER_CURR`。这是测试编排/时间窗口边界，
后续以 trace 实际样本数判断运行时长。原始结果：
[`ADC trigger follow-up`](../../tmp/bench/20260830_095925_adc_trigger_followup/result.md)。

当前状态：

```text
ENCODER_DIRECTION_CANDIDATE=SUPPORTED_NOT_CONFIRMED
ADC_MEASUREMENT_WINDOW=ANOMALOUS_OR_STRONGLY_SENSITIVE
OPEN_LOOP_SOFTWARE_PATH=PASS
MOTOR_PHYSICAL_OPERATION=UNVERIFIED
OPEN_LOOP_ROOT_CAUSE_STATUS=UNRESOLVED
REAL_BENCH_TEST_STATUS=BLOCKED
```

## 20. 2026-08-30 标准 SVPWM 与当前实机矩阵证据

- `[无板验证]` 当前标准 SVPWM 以逆 Clarke/common-mode 方式保持 alpha/beta 矢量；专项模型、host flow、独立 CTest 和外设契约均通过。停机 snapshot 也已修复为从当前 encoder PLL 状态派生速度/位置，排除了此前停机后的陈旧 `-334 rpm` 诊断误读。
- `[实板测量]` bench400 镜像 `25A38A987BE93B48C05526B8A6839F697AE618AACFD740C8517AAA661CA65922` 已显式刷写并通过预检；复位安全状态为 `axis=idle`、`fault=0`、duty 零、VBUS 约 `11.91 V`。
- `[实板测量]` 36 组 PWM/ADC 映射组合无稳定唯一优胜；map2 的一次旋转响应在两次重复中消失，未改变默认 map0。theta0 矩阵同样无可重复优胜，未改变编码器零点。
- `[实板测量][静态推导]` CH4 compare 从 `512..1874` 到 `2300..3740` 时软件电流从约 `0.01 A` 量级升到约 `0.27 A` 量级，提示低侧 CSA/采样窗口相关性；无 SOx 波形和独立电流，不能将其写成真实相电流定量证明。
- `[实板测量]` 2 V/20–30 rpm 多轮未稳定同步，3 V 一轮触发约 1 A 软件快速边界；2 V/15 rpm 两次一轮部分跟随、一轮不跟随，故“低速已通过”不成立。
- `[实板未验证]` 本轮没有人工手动编码器整圈、示波器、独立相电流或温度数据；因此 `OPEN_LOOP_ROOT_CAUSE_STATUS=UNRESOLVED`、`MOTOR_PHYSICAL_OPERATION=UNVERIFIED`、`REAL_BENCH_TEST_STATUS=BLOCKED` 保持不变。

完整结果：[`tmp/bench/20260830_074936_post_snapshot_fix/result.md`](../../tmp/bench/20260830_074936_post_snapshot_fix/result.md)。

## 19. 2026-08-30 当前最终镜像的首个正交诊断脉冲

- `[实板测量]` bench50 ELF 已用显式 OpenOCD 命令刷写并返回 `Verified OK`；当前
  SHA256 为 `7CFBDED0B667192AA6410788C41B0CD2BCF3D724D3F48848E1B5E76E4D139B9C`。
- `[实板测量]` Preflight 已确认 `cmd=map action=show result=ok`、
  `cmd=enc_trace action=show result=ok`、`axis=idle`、`fault=0`。无 PWM 静态数据为
  VBUS 约 `11.91 V`、duty `0/0/0`、软件电流 `-0.002/-0.001/-0.012 A`。
- `[实板测量]` 编码器采集命令完成并保留 SPI 样本，但本轮没有可确认的人工整圈动作，
  所以方向、极对数和一机械圈七电周期仍未验证。
- `[实板测量]` 首个映射样本为 `pwm=0`、`adc=0`、`adc_trig=3740`、`dp0=+0.05 V`，
  脚本名义 5 ms；停机 trace 电流峰值 `0.019/0.012/0.013 A`、VBUS
  `11.89-11.92 V`、duty `0.498/0.502/0.502`。
- `[实板测量]` 板端锁存 `FAULT_COMM_ERROR (0x100)` 并进入 FAULT；fault context
  为 `theta_sim=0`、`theta_elec≈0.67`、`vd=0.05 V`，没有过流证据。
- `[静态推导]` trace 保留 250 条 FOC 样本，与 bench50 的 50 ms 能量护栏一致；名义
  5 ms 不等于实际带电窗口，`state disable` 的响应时序需单独分析。
- `[实板未验证]` 完整 6×6 映射、触发扫描、相位扫描、人工整圈编码器验证、PWM 波形、
  独立相电流、温度和产品级连续运行均未完成。

本轮状态不升级：

```text
OPEN_LOOP_SOFTWARE_PATH=PASS
MOTOR_PHYSICAL_OPERATION=UNVERIFIED
OPEN_LOOP_ROOT_CAUSE_STATUS=UNRESOLVED
REAL_BENCH_TEST_STATUS=BLOCKED
```

仍未被无板测试证明的项目：

- 实际 PWM 互补波形、极性、重叠和死区；
- ADC 注入采样窗口和采样相位；
- DRV8311H `nFAULT` 实际保护原因和传播延迟；
- `nSLEEP` 网络在故障时的真实电平；
- DRV AVDD LDO、功率地和高频 EMI；
- 电机相序、绕组、齿槽转矩、机械负载和低速同步；
- 真实电流、温升、转速和持续运行稳定性。

## 9. 后续恢复调试时的唯一入口

在 `REAL_BENCH_TEST_STATUS` 解除前，不执行电机驱动动作。解除前必须先具备：

1. `nFAULT` 的 MCU 或独立硬件监控路径；
2. `nSLEEP` 状态一致性证据；
3. TMR1 Break 或等效硬件快速关断；
4. 互补 PWM 死区的配置、芯片手册和示波器三方证据；
5. 可触达物理断电开关；
6. 电机机械固定、示波器、电流和温度测量；
7. 无 VOFA 溢出的 bench 固件和白名单命令。

获准后严格从以下顺序开始，不跳步：

```text
T00/T01 静态检查
  -> D0 零电压基线
  -> D1/D2/D3 0.10 V 固定矢量
  -> D4 0.20 V（仅前三项通过后）
  -> O0 0.10 V / 0 rad/s
  -> O1 0.10 V / 3.14 rad/s
  -> O2 0.20 V / 3.14 rad/s
  -> O3 0.40 V / 3.14 rad/s（仅 O0/O1 通过后）
```

任何异常的硬停顺序：

```text
发现异常
→ 立即切断 PSU
→ 不等待串口回复
→ 禁止继续发送命令
→ 等待电机完全停止
→ 保留 fault context，不执行 fault clear
→ 保存日志和示波器/电源快照
→ 停止后分析
```

硬停条件包括任一相超过 `0.20 A`、`0.12 A` 且低于 `5 rpm` 持续 `200 ms`、VBUS 超出 `10.8–13.2 V`、ADC/角度/duty 非有限或越界、卡死/尖叫/快速发热、`nFAULT` 有效、PWM 波形异常或保护路径失联。

## 10. 交接时最重要的三句话

1. 低压开环卡死不能先靠提高电压或放宽保护阈值解决，必须先用 raw ADC、offset、duty、角度和 VBUS 证据区分软件映射、测量链、功率级和机械转矩问题。
2. 任何异常状态先停机和断电，复杂分析放到电机停止后；`12 V/1 A` 不是相电流保护。
3. 当前软件路径已通过无板门禁，但实际电机运行仍未验收，`REAL_BENCH_TEST_STATUS=BLOCKED` 必须保持。

## 11. 2026-08-28 现有板只读 T01 结果

Task 1 已重新执行并通过：host/CTest 均为 `33/33 PASS`，外设静态契约为
`FOC_PERIPHERAL_CONTRACT=PASS`，bench ELF 构建通过，且编译定义仍为
`TM_BENCH_DIAG=1`、`TM_FEATURE_VOFA=0`、`FOC_ENERGIZED_CALIB_ENABLE=0`。

随后只对识别出的 USB 串口 `COM3` 发送了以下只读命令：

```text
sys status
state show
fault status
obs all
diag param show
diag pid all show
diag debug show
```

现场返回的可识别事实是：`vbus≈11.91 V`、`axis=fault`、
`fault=0x00000080 calibration-failed`、`iq=0`、`id=0`、速度为 `0 rpm`。
串口输出部分乱码，`diag debug show` 返回 `bad_arg/rc=-4`；源码核对表明
该命令本来就不属于当前 Shell 命令面，因此不能据此判断板上镜像过旧。
由于当前 Shell 没有板上 build-id/hash 查询接口，bench ELF 仍未被证明已
烧录；延长等待后的 `obs all` 仍缺少当前源码应输出的
`phase/amps/ctrl/svpwm` 行，`fault status` 也缺少 context 行，这是镜像
不匹配的强指示，但不能排除串口丢包/溢出。raw ADC/offset/三相电流、
duty/sector、DRV 电源和物理 PWM 均未验证。

本次没有发送 `state start`、`diag set`、任何校准或 `fault clear`。由于
G0 所需的人工急停、示波器死区/重叠、电流和温度证据仍缺失，本次执行在
T01 停止，不进入 D0，状态保持：

```text
OPEN_LOOP_SOFTWARE_PATH=PASS
MOTOR_PHYSICAL_OPERATION=UNVERIFIED
REAL_BENCH_TEST_STATUS=BLOCKED
OPEN_LOOP_ROOT_CAUSE_STATUS=UNRESOLVED
```

本次现场记录：

- [T01 结果](../../tmp/bench/2026-08-28_224839/result.md)
- [只读串口记录](../../tmp/bench/2026-08-28_224839/T01_static.log)
- [故障上下文记录](../../tmp/bench/2026-08-28_224839/fault_context.txt)

## 12. 2026-08-28 对实机诊断门禁的修正

前一版执行计划把 G0 的产品级条件（示波器死区/重叠、独立电流与温度测量、
完整硬件急停证据）误当成第一轮故障发现的绝对前置条件。这会让调试在没有得到
任何新物理数据前就停止，不能达到“实机运行以发现问题”的目的。

现明确分离：

```text
PRODUCT_ACCEPTANCE_STATUS=BLOCKED
CONTROLLED_DIAGNOSTIC_STATUS=ALLOWED_WITH_MINIMUM_GUARDS
```

历史日志已经保留了真实板证据：2026-08-24 曾通过
`diag set vq 1.2 31.42` 完成约 10 s 的电压开环运行，并验证了 CSAREF 修正、
三相驱动对称性和 I-f 同步。因此下一轮不再重复“是否能通电”的假设验证，而是
先确认 bench 镜像确实已烧录，再用低压、毫秒级、单变量固定矢量获取新的 raw ADC、
电流、VBUS、角度和 duty 数据。

第一轮诊断不以示波器或外部温度/电流仪器作为阻塞条件，但必须有人手能立即切断
12 V，不能无人值守或等待串口回复；复杂分析全部在停机后进行。诊断顺序固定为：

```text
确认 bench 镜像
→ 只读静态基线
→ D0: dp0=0.00 V，≤20 ms
→ D1/D2/D3: dp0/dp1/dp2=0.05 V，各≤20 ms
→ 停机后读取 fault/obs
→ 只有前三轴都正常才提高到 0.10 V 或 O0/O1
```

如果软件电流读数、串口输出或 PWM 状态无法确认，不把它判定为“安全”，而是
立即断电并把该轮结果标为数据通道失败。示波器死区/重叠、外部电流、温度和
`nFAULT` 仍是后续功率级确认及产品级验收证据。

## 13. 2026-08-28 bench FSM PWM 边界修正

源码审计发现，通用 FSM 的初始 `IDLE` enter 回调会调用 `pwm_out_enable()`，而
`RUNNING` enter 回调不调用它。这使 bench 上电前的桥状态和显式 `state start`
边界不清晰。新增 bench-only host contract 并先观察到旧实现失败后，bench 构建
现在改为：IDLE 进入时保持 PWM disabled，只有 `EV_CMD_START` 接受后才 enable；
进入 FAULT 仍再次 disable。默认产品固件的 0% 主动低侧钳位行为不变。

这项修正只改善诊断边界，不构成死区、nFAULT、采样窗口或电机运行的实板证明。

## 14. 2026-08-29 实有板低压短脉冲诊断

- 根据现场反馈，确认示波器死区/重叠、独立相电流、温度和完整人工急停证据不应阻塞第一轮“运行以发现问题”的短脉冲诊断；将产品级 `REAL_BENCH_TEST_STATUS=BLOCKED` 与 `CONTROLLED_DIAGNOSTIC_STATUS=ALLOWED_WITH_MINIMUM_GUARDS` 分开。没有把短脉冲结果写成产品验收。
- bench 固件在前一轮已加入约 50 ms ADC ISR 自动运行护栏；本轮通过 OpenOCD 将明确 ELF 写入当前板并收到 `Verified OK`。当前 ELF SHA256 为 `CDE7AC7D8044719D4CF4E2650C298C566D4C3CF2A72DE439D8119AA13BE3B4BC`，编译定义为 `TM_BENCH_DIAG=1`、`TM_FEATURE_VOFA=0`、`FOC_ENERGIZED_CALIB_ENABLE=0`。
- COM3/115200 实测启动状态为 `axis=idle`、`fault=0`、`vbus≈11.91 V`。D0-D4 固定/零矢量 20 ms 和 O0-O3 低压开环短脉冲均未触发 `FAULT_OVER_CURR`；`vq=0.40 V`、`we=3.14 rad/s` 的 O3 自动护栏快照最大板上换算相电流约 `-48/60/-2 mA`，低于 `0.10 A` 预警与 `0.20 A` 快速阈值。
- O1 `diag set vq 0.10 3.14` 实测：20 ms 正常停机后 `theta_sim≈0.01`、`we_volt_now≈1.28/3.14 rad/s`；让 50 ms bench 护栏自然触发时，`fault status` 锁存 `theta≈0.075`、`we≈3/3.14`、duty 约 `0.501/0.495/0.505`。这证明软件角度推进在实板短脉冲中执行，但不证明连续机械旋转。
- 实测发现正常 `state disable` 后 `motor_calib_prepare_entry()` 会清零 `theta_sim/we_volt_now/we_volt_ramp`，导致停机 `obs all` 丢失开环运动信息。新增 bench-only 快照保留契约，测试先失败后通过；PWM 已关闭、PID/设定值仍清理，默认产品固件行为不变。修复后重新构建并刷写 bench ELF，O1 正常停机可读到角度和速度斜坡。
- 当前停机后结论：`vq≤0.40 V`、约 50 ms 内没有复现高电流卡死，也没有复现软件角度不推进；低压不转暂列“电压/转矩裕量、齿槽转矩、静摩擦、机械负载或电机参数”候选，唯一根因仍未确认。外部相电流、实际 PWM 波形、死区/重叠、DRV 温升和长时间运行仍是 `[实板未验证]`。
- 详细记录：`tmp/bench/20260829_actual/`，其中 `result.md` 汇总 D0-D4/O0-O3，原始日志保留每次 Shell 返回。当前状态继续为 `OPEN_LOOP_SOFTWARE_PATH=PASS`、`MOTOR_PHYSICAL_OPERATION=UNVERIFIED`、`OPEN_LOOP_ROOT_CAUSE_STATUS=UNRESOLVED`、`REAL_BENCH_TEST_STATUS=BLOCKED`。

补充镜像身份复核：注释调整改变了带调试信息 ELF 的哈希，因此将当前 bench ELF
重新通过 OpenOCD `Verified OK` 写入板上，最终 SHA256 为
`325A464AA838DCB7344657E2917953DD52515D72AD7ED0B40B5AF91D5C5BEE36`。同一
`diag set vq 0.10 3.14`、20 ms O1 复测返回 `fault=0`、`vbus≈11.91 V`、板上
电流约 `1/2/1 mA`、`theta≈0.01`、`we≈1.32/3.14`；最终镜像与首轮短脉冲行为一致。
## 15. 2026-08-29 电压模式入口相位 A/B 修复

本轮执行了“相位入口假设”的最小代码—实板闭环，过程与结论如下：

- `[静态推导]` 默认 `ctrl_mode=CTRL_MODE_VOLT`，旧路径清零 `theta_sim` 后，
  `foc_cmd_voltage_set()` 因模式没有发生切换而不播种编码器角度。首次 `vq` 写入
  前可能得到 `theta_sim≈0`、`theta_elec≈3.35 rad`。
- `[主机测试]` 扩展 `test_foc_motion_runtime_contract`，先观察到入口同步断言失败，
  再实现 IDLE/模式切入同步、边界 wrap、I-f 同入口及运行态不跳角，专项测试和
  全量 host/CTest 均通过，最终为 `35/35 PASS`。
- `[实板测量]` 第一版读取 `meas_data.theta_elec` 仍无法在 IDLE 入口对齐；这说明
  测量缓存不是可靠的入口事实。最终改为读取
  `enc_pri.state.angle_elec_rad`，并把运行时快照的 `theta_elec` 也改为实时编码器
  状态。
- `[实板测量]` 最终 bench ELF 在 A1 入口读到 `theta_sim≈3.21`、
  `theta_elec≈3.20 rad`、`we_volt_now=0`，误差约 `0.01 rad`；A2/A3 正反向
  `20 ms` 短脉冲及 `vq=0.20/0.30/0.40 V` 梯度均 `fault=0`，VBUS 约 `11.91 V`，
  板上 ADC/CSA 换算电流未超过 `0.10 A`。
- `[实板未验证]` 修复前后短脉冲均未复现原始高电流卡死，因此只能确认“相位入口
  缺陷真实存在且已修复”，不能确认它是整机唯一主根因。外部相电流、实际 PWM
  死区/重叠、采样窗口、DRV 温升和连续机械运行仍未验证。

最终镜像：

```text
ELF=build/fw-full-diag-carrier-bench-debug/output/falcon-foc-at32.elf
SHA256=F0402BADC248FEB2C7EEB3F149DF321B6A3189B3C65125AB876A5E83064F04B1
TM_BENCH_DIAG=1
TM_FEATURE_VOFA=0
FOC_ENERGIZED_CALIB_ENABLE=0
```

当前状态：

```text
PHASE_ENTRY_ALIGNMENT=CONFIRMED
OPEN_LOOP_SOFTWARE_PATH=PASS
MOTOR_PHYSICAL_OPERATION=UNVERIFIED
OPEN_LOOP_ROOT_CAUSE_STATUS=UNRESOLVED
REAL_BENCH_TEST_STATUS=BLOCKED
CONTROLLED_DIAGNOSTIC_STATUS=ALLOWED_WITH_MINIMUM_GUARDS
```

日志目录：`tmp/bench/20260829_phase_seed_baseline/`、
`tmp/bench/20260829_phase_seed_after_fix/`、
`tmp/bench/20260829_phase_seed_final/`。

## 17. 2026-08-29 下一阶段低压开环根因定位最终记录

本节是当前工作树和最新实板记录的交接事实；前文旧镜像哈希和旧目录保留为历史
过程，不覆盖本节。

### 17.1 镜像与基础设施修复

`[无板验证]` 当前最终使用的 50 ms bench ELF 为
`build/fw-full-diag-carrier-bench-debug/output/falcon-foc-at32.elf`，SHA256 为
`3969B23E8DB62BCD590B49D5FE2F63574944C3281CDF5A00D2A3CCE3931AD8AE`。编译定义为
`TM_BENCH_DIAG=1`、`TM_FEATURE_VOFA=0`、`FOC_ENERGIZED_CALIB_ENABLE=0`、
`TM_MOTOR_BENCH_RUN_BUDGET_MS=50`，bench app-shell 栈为 2048 B。

本节的 F0–F6、theta0 和 V1–V4 pulse 矩阵对应上一轮 bench ELF
`7EBC7C00CB4E8225372B9E774B761CCB1A561E7A9CE96F7C208F8E88C201801E`；随后仅因
host 宏头文件可见性修正重建/刷写为当前 `3969...`，刷写后只做 A0 只读复核，
没有重复发送电机脉冲。

`[实板测量]` 首轮命令无响应通过调试器定位到 FreeRTOS `app_shell_task` 栈溢出，
不是电机或 DRV 故障。先增加 host 栈契约使其失败，再做 bench-only 栈增量并重建、
刷写；修复后所有 Shell/trace 命令恢复响应。该缺陷和修复不能解释原始电机卡死。

### 17.2 实际测试结果

- `[实板测量]` A0：`axis=idle`、`fault=0`、VBUS 约 `11.89–11.92 V`，无静态
  软件电流异常。
- `[实板测量]` A1：`diag set theta0 0.0000` 后设置 `vq=0.10 V,we=3.14 rad/s`
  但不启动，`theta_sim≈3.12`、`theta_elec≈3.11`、误差约 `0.01 rad`、
  `we_volt_now=0`，相位入口保持对齐。
- `[实板测量]` F0–F6：`dp0/dp1/dp2=0、±0.10 V`，每次约 20 ms；峰值约
  `0.010–0.031 A`，三轴同数量级，无故障。
- `[实板测量]` theta0 有效值 `-3.14159/-1.5708/0/+1.5708/+3.14159` 没有
  可重复优胜；`-3.1416` 仅因略小于严格 `-π` 被拒绝。
- `[实板测量]` V1–V4 为 `vq=0.10/0.20/0.30/0.40 V,we=3.14 rad/s`，每次约
  20 ms，软件 trace 峰值分别约 `0.027/0.019/0.016 A`、`0.036/0.032/0.019 A`、
  `0.056/0.045/0.016 A`、`0.067/0.056/0.016 A`，VBUS 约 `11.88–11.91 V`。

V3/V4 已超过 50 mA 延长运行门槛，故未执行 200 ms 实机镜像。最后一次测试后已
回到 `axis=idle`、`fault=0`。`obs` 保留最后一次控制计算快照，不能把停机后的
旧 duty 当作物理 PWM 输出证明。

### 17.3 证据边界与下一步

`[实板未验证]` 本轮没有独立相电流、示波器 PWM/死区/重叠、温度、手动编码器整圈
扫查或连续机械运行数据；所有电流来自 MCU ADC/CSA。原始高电流卡死在本轮 20 ms
窗口内没有复现，theta0 也没有显示可重复改善，因此不能写入唯一物理根因。

```text
PHASE_ENTRY_ALIGNMENT=CONFIRMED
OPEN_LOOP_SOFTWARE_PATH=PASS
MOTOR_PHYSICAL_OPERATION=UNVERIFIED
OPEN_LOOP_ROOT_CAUSE_STATUS=UNRESOLVED
REAL_BENCH_TEST_STATUS=BLOCKED
```

当前日志主目录：`tmp/bench/20260829_133853_next_root_cause_final/`。其中
`result.md`、`fault_context.txt`、`operator-notes.md` 是本轮停机后交接入口；
早期无响应、无效命令和中间测试日志均保留，不得删除。

## 18. 2026-08-30 分层正交诊断实现记忆

这是当前会话后续工作的仓库内记忆入口，不覆盖前文已记录的历史实板结果。

- `[源码确认]` 新增 64 条 FOC 定点 RAM ring、32 条编码器 RAM ring；均在 bench 编译时
  才分配，停机后通过 Shell 读取，不能在运行中打印或写 Flash。
- `[源码确认]` bench map0 与原理图/生成配置事实一致：PWM `A/B/C→CH3/CH2/CH1`，ADC
  `A/B/C→rank4/rank3/rank2`；另有 6 种 PWM、6 种 ADC 排列和 `CH4 compare=256..3740`
  的运行时实验入口，默认 3740。
- `[源码确认]` `run_bench_matrix.ps1` 只允许白名单命令，主动脉冲必须 `-RunPulses`；
  `state disable` 后由于当前状态机进入 FAULT，多脉冲必须人工 power-cycle 并传
  `-WaitForPowerCycle`。脚本不发送 `state stop`、`fault clear`、校准、I-f 或参数写入。
- `[无板验证]` 本轮实现后 host/CTest 为 `38/38 PASS`，外设契约为
  `FOC_PERIPHERAL_CONTRACT=PASS`；bench50、bench200、full-diag、standard、tiny 构建
  均通过。
- `[无板验证]` 当前构建身份为 bench50
  `7CFBDED0B667192AA6410788C41B0CD2BCF3D724D3F48848E1B5E76E4D139B9C`、bench200
  `7440A83EE8306FEF75F8B259754ECF75ED3521ED3012185476FDB7A33EDA68AD`；历史实板日志
  的旧哈希不能与本轮新增诊断能力混用。
- `[实板未验证]` 当前最终镜像已完成首个物理脉冲，但新增正交矩阵尚未完成；历史日志
  必须按其原 ELF SHA256 单独引用。死区/重叠、实际采样孔径、独立相电流、DRV 热稳定性
  和连续机械运行仍未验证。

状态不要提前升级：

```text
OPEN_LOOP_SOFTWARE_PATH=PASS
MOTOR_PHYSICAL_OPERATION=UNVERIFIED
OPEN_LOOP_ROOT_CAUSE_STATUS=UNRESOLVED
REAL_BENCH_TEST_STATUS=BLOCKED
```

## 19. 2026-08-30 开环测试目标校正与实现结果

- `[实板反馈]` 当前用户确认：`5 rpm` 不能作为电压开环旋转测试目标，旧的
  `0.20 A/0.10 A/0.12 A` 包线也会在正常开环启动电流范围内过早触发，必须区分
  绝对快速停机、持续电流保护和闭环堵转判定。
- `[原理图确认][资料确认]` DRV8311H GAIN 为 `0.5 V/A`，2804 电机资料给出
  `0.5 A` 额定、`2.0 A` 最大、7 对极；当前临时诊断值改为快速 `1.00 A`、持续
  `0.75 A/3 s`，闭环堵转 `0.75 A/200 ms`。
- `[源码确认]` `CTRL_MODE_VOLT` 已排除闭环堵转检测；bench 快速路径不再因超过预警
  值的 3 个样本直接锁存过流，快速绝对阈值和任务级 3 s 持续保护保持有效。
- `[静态推导]` 30 rpm 机械速度对应约 `21.99 rad/s` 电气速度，电周期约 `285.7 ms`；
  5 ms/`3.14 rad/s` 仅产生很小角度变化，因此新增 bench400 作为电压开环的首个有效
  观测窗口，固定矢量/映射/触发仍使用 5–20 ms。
- `[无板验证待执行]` 已新增 400 ms bench CMake preset、运行时长契约和脚本的
  `-BenchBudgetMs/-VoltagePulseMs/-OpenLoopSpeedRpm` 参数；实际 bench400 构建、刷写
  和实机运行必须使用独立 ELF SHA256，不能混用旧 bench50 日志。

状态保持：

```text
OPEN_LOOP_SOFTWARE_PATH=PASS
MOTOR_PHYSICAL_OPERATION=UNVERIFIED
OPEN_LOOP_ROOT_CAUSE_STATUS=UNRESOLVED
REAL_BENCH_TEST_STATUS=BLOCKED
```

## 21. 2026-08-30 当前镜像方向 A/B 与 30 rpm 实测记忆

- `[源码确认]` 当前 bench-only `diag set enc_dir 0|1` 只修改 RAM 方向，IDLE-only，重启
  恢复默认/持久化方向；产品 `DEFAULT_DIR` 未改。`state disable` 后 encoder trace 冻结，
  停机读取不再被后续 encoder 更新覆盖。
- `[实板测量]` 当前唯一镜像是 bench400，SHA256 为
  `59B78D648511926F8AD47213CA35E49C0D536697BE814B1B77AF64F84E949A03`。静止
  `vq=0.50 V,we=0` 的 CW/CCW 各两次均无故障，CCW 电流通常更高，但没有同一机械位置
  基准，方向差异只能标记为候选。
- `[实板测量]` 在正确的 `we=21.9911 rad/s`（约 30 rpm）下，0.25 V/350 ms 的
  两方向峰值约 `0.037..0.070 A`；1.00 V/350 ms 的两方向峰值约 `0.190..0.289 A`。
  VBUS 保持约 `11.87..11.93 V`，没有触发 1 A 快速故障；软件 theta 连续推进，但没有
  可靠机械同步证据。
- `[实板测量][静态推导]` 既有 CH4 compare 扫描的电流敏感性仍是独立 ADC/CSA 采样窗口
  候选，不能被方向 A/B 结果覆盖。
- `[实板未验证]` 仍缺手动 encoder 整圈、PWM/死区/SOx 波形、独立相电流、温度和机械
  转速，因此根因状态不升级。

当前状态：

```text
ENCODER_DIRECTION_CANDIDATE=SUPPORTED_NOT_CONFIRMED
OPEN_LOOP_SOFTWARE_PATH=PASS
MOTOR_PHYSICAL_OPERATION=UNVERIFIED
OPEN_LOOP_ROOT_CAUSE_STATUS=UNRESOLVED
REAL_BENCH_TEST_STATUS=BLOCKED
```

## 22. 2026-08-30 动态 ADC trigger 全曲线记忆

`[实板测量]` 在同一 bench400 ELF（SHA256 `59B78D648511926F8AD47213CA35E49C0D536697BE814B1B77AF64F84E949A03`）
下，固定 `vq=1.00 V`、`we=21.9911 rad/s`，完成 CH4 compare 9 点动态扫描：
`512/1024/1536/1874` 的软件峰值约 `0.01..0.017 A`；`2300` 约
`0.058/0.259/0.020 A`；`2812/3200/3500/3740` 的 A/B 峰值约
`0.24..0.26 A`。VBUS 约 `11.87..11.93 V`。

`[实板测量]` `1874/2300/2812/3740` 各重复两次，结果分别保持低、转折、高、高；
八次均 `fault=0`、`active=0`，结束后复位为 `axis=idle`、`duty=0/0/0`。因此新的
记忆锚点是：

```text
ADC_MEASUREMENT_WINDOW=REPRODUCIBLY_TRIGGER_DEPENDENT
```

这仍然是 MCU ADC/CSA 观测证据，不是独立相电流或 SOx 波形证据。不能把 `1874`
写成正确采样点，不能把 `3740` 读数写成真实相电流，不能修改默认 trigger、ADC rank、
相序或保护阈值。完整记录：[`boundary repeats`](../../tmp/bench/20260830_104326_dynamic_trigger_boundary_repeats_1V_30rpm/)。

最终状态仍为：

```text
PHASE_OFFSET_CANDIDATE=REJECTED_AS_UNREPEATED
ENCODER_DIRECTION_CANDIDATE=SUPPORTED_NOT_CONFIRMED
OPEN_LOOP_SOFTWARE_PATH=PASS
MOTOR_PHYSICAL_OPERATION=UNVERIFIED
OPEN_LOOP_ROOT_CAUSE_STATUS=UNRESOLVED
REAL_BENCH_TEST_STATUS=BLOCKED
```
