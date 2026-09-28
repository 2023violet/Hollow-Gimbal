# AT32F415 + DRV8311H 电机实机测试方案

> 状态：受控执行方案；产品级连续运行与短脉冲故障定位使用不同门禁
>
> 适用对象：Board1、AT32F415、DRV8311H、2804 BLDC 云台电机
>
> 当前目标：先定位“电压开环卡死且电流偏高”，再逐步建立 I-f、编码器零点和低速闭环速度证据
>
> 编制日期：2026-08-27

## 1. 方案边界

本方案只允许进行有人值守、低能量、短时、一次只改变一个变量的实机测试。它不是“让电机多运行一会儿看结果”的操作说明，也不把串口、VOFA、PC 或调试人员的实时观察当成安全保护。

本方案的目标顺序是：

```text
最小可执行断电条件
  -> 采样/母线静态基线
  -> 固定定子矢量
  -> 低幅值电压开环扫角
  -> 低幅值 I-f
  -> 编码器原始方向与 LUT
  -> 电角度零点
  -> 低速闭环速度（仅产品级门禁解除后）
```

当前明确禁止：

- 在电压开环未能低电流、连续推进前调速度环；
- 直接使用历史的 `diag set vel -300` 作为第一组测试；
- 通过增大电压、电流或延长时间来克服卡死；
- 把 `DEFAULT_CURR_PEAK = 2.0 A` 或 DRV8311H 的内部 OCP 典型值当成电机调试安全电流；
- 在故障状态未记录、数据未冻结前清故障或自动重启。

## 2. 当前执行状态：硬门禁

### 2.1 诊断轨道与产品验收轨道分离（2026-08-28 修正）

此前版本把产品级连续运行的 G0 条件误写成了第一轮故障定位的绝对前置条件，
导致实机诊断无法开始。现分为两条轨道：

```text
PRODUCT_ACCEPTANCE_STATUS=BLOCKED
CONTROLLED_DIAGNOSTIC_STATUS=ALLOWED_WITH_MINIMUM_GUARDS
```

`PRODUCT_ACCEPTANCE_STATUS=BLOCKED` 仍然表示：没有硬件 Break/独立故障关断、
示波器死区证据、完整外部电流/温度证据之前，不允许长时间、无人值守或产品级
连续运行。它不再阻塞低压、极短时、单变量的诊断脉冲。

受控诊断的最低条件只有：

1. 操作者手边有可立即切断 12 V 的开关，异常时不等待串口回复；
2. 使用已经核验的 bench ELF，电机动作采用毫秒级短脉冲，不连续扫速；
3. 运行中只观察简单硬停信号，复杂数据在停机后读取和分析；
4. 每次只改一个变量，任何无响应、乱码、控制路径不确定都立即断电。

示波器死区/重叠、外部电流、温度和 `nFAULT` 观测是后续功率级确认与产品验收
证据，不是第一枚诊断脉冲的发现问题前置条件。没有最低条件第 1 项时，Codex
不代替操作者发送启动命令，因为软件电流读数本身可能就是待定位对象。

本项目此前已存在实板证据：2026-08-24 曾在 COM3 上以
`diag set vq 1.2 31.42` 完成约 10 s 的真实开环运行，并验证过 CSAREF 修正、
三相驱动对称性和 I-f 同步。当前任务是对齐镜像、参数和剩余异常，不是重新证明
整条功率链是否能通电。

### G0：产品级/连续运行快速停机能力

G0 是产品级、连续或高能量运行的前置门禁，当前状态为 **BLOCKED**，原因是：

- ADC ISR 已接入 `foc_fault_check_fast()`：严重相电流、VBUS、速度、温度和非法数据在继续 FOC 前锁存并关闭 PWM；
- `motor_task` 的 1 ms 任务已作为持续电流/堵转后备，但仍不能替代硬件 Break 或独立电源急停；
- 原理图中的 `nFAULT`、`nSLEEP` 没有 MCU 直接端点；
- `pwm_out_disable()` 只关闭六路 TMR1 通道，不切断 DCBUS；
- `test_motor_fault_runtime_contract`、故障上下文、启动校准、VBUS guard、动态 LPF、带电校准门禁和 FOC 开环/参数模型等 host 安全/无板目标已经通过；当前完整 host flow 已为 `38/38`，软件回归门禁通过。

因此，软件子门禁已通过，但总 G0 仍保持 `BLOCKED`；只有以下硬件/系统条件也满足后才可解除：

1. 固件已经实现并通过无板测试的快速 trip、故障锁存、快照和禁止自动重启；
2. 在固件之外有独立、已验证响应时间的硬件/电源过流急停链路，并且测试由操作者全程守在 PSU 断电开关旁。

在 G0 未满足前，本文件仍可用于准备、无板验证、静态采样和受控短脉冲诊断；
不授权无人值守或长时间带能量运行。

### G1：配置事实

带电前必须记录并确认：

- MCU 固件构建产物和源码工作树版本；
- `poles = 7`、编码器方向、相序和当前 LUT 开关状态；
- CSAREF 实际连接和电流零点；
- DRV8311H GAIN 的实际装配状态；
- VBUS 分压比例和 ADC 通道；
- TMR1 六路 PWM 映射、互补输出、死区和 ADC 触发；
- 运行命令的单位，特别是 `we` 是否为电气 `rad/s`；
- 本次测试的电压、电流、速度硬限值和自动截止时间。

任何一项只能“猜测”时，测试停在 G1。

## 3. 实机安全包线

以下数值是当前 12 V/1 A 电源背景下的临时操作包线，不是最终产品保护参数。电源限流
不是相电流限制，且必须在每次测试记录中写明。

### 3.1 第一轮电源设置

| 场景 | PSU 电压 | PSU 电流限值 | 单次最长运行 |
| --- | ---: | ---: | ---: |
| 静态采样/固定矢量 | `12.0 V` | `1.0 A` 电源能力 | `5–20 ms` 每个动作 |
| 电压开环低速旋转 | `12.0 V` | `1.0 A` 电源能力 | `400 ms` 每个动作 |
| I-f/编码器校准/速度环 | 禁止 | 禁止 | 不属于本阶段 |

PSU 的输入限流不能直接等价为相电流保护。电机绕组、电容和 PWM 功率级可能在 PSU 限流动作前产生较大的瞬态相电流，因此必须有独立的快速相电流 trip 或等效硬件保护。

### 3.2 第一轮临时停机阈值

| 量 | 警戒值 | 硬停机值 | 说明 |
| --- | ---: | ---: | --- |
| 相电流/电流矢量 | `0.75 A` | `1.00 A` | 任意相或 `sqrt(Id²+Iq²)`；超硬限值立即停 |
| 卡死电流 | `0.75 A` | — | 仅非电压模式，`|vel| < 5 rpm` 持续 `200 ms` 即停 |
| VBUS（12 V 阶段） | `10.8–13.2 V` 外为异常 | `<10.8 V` 或 `>13.2 V` | 当前 12 V/1 A 实机窗口 |
| 低速开环速度 | — | `30 rpm` 起步 | 对应约 `21.99 rad/s` 电气速度，不把 5 rpm 当旋转目标 |
| I-f `we=31.42` 阶段 | — | `60 rpm` | 预期约 `42.86 rpm`，超过即停 |
| 电机表面温度 | 快速升温 | `40 °C` 或快速升温 | 当前 NTC 未形成可靠软件保护 |

在 GAIN 尚未确认前，不能只相信换算后的安培数。若使用当前 `0.5 V/A` 假设，`1.00 A` 对应约 `621` 个 ADC counts 的偏离；快速路径仍应同时监视相对 CSAREF 中点的原始 ADC 偏差，实际阈值以已确认的 GAIN 和零点为准。若 GAIN 可能低于当前假设，禁止带电测试。

### 3.3 立即停机条件

出现以下任意情况，不等待 200 ms、3 s、VOFA 刷新或串口回复：

- 任一相电流超过硬停机值；
- 电机卡死、抖动、反复翻转并伴随电流上升；
- VBUS 超出测试包线；
- ADC、角度、速度、占空比出现 NaN/Inf、越界或不连续跳变；
- 电流命令非零但转子不动且电流持续上升；
- 驱动器有异响、异味、冒烟或异常温升；
- 电机停止后电流没有快速衰减；
- 串口、VOFA、调试器或主机脚本失联；
- 操作者无法在当前状态确认 PWM 已关闭。

软件应执行 `trip latch -> PWM disable -> 清零设定值/积分器 -> 冻结快照`；操作者同时关闭 PSU 输出。软件和人工动作谁先发生都可以，但不能等待另一个动作成功后才执行。

## 4. 日志和观测要求

### 4.1 每次测试的固定记录

每个测试建立独立目录，例如：

```text
tmp/bench/YYYYMMDD_HHMMSS/
  T00_preflight.log
  T01_adc_baseline.log
  T02_dp0.log
  T03_dp1.log
  T04_dp2.log
  T05_openloop_vq.log
  T06_if.log
  T07_encoder.log
  operator-notes.md
  result.md
```

每个文件至少记录：

- 时间、固件产物、板号、电机型号；
- PSU 设定和实际 VBUS；
- 测试命令原文和每个参数单位；
- 起止时间、自动截止时间和实际停机时间；
- 所有硬限值/警戒值；
- 停机动作是软件 trip、`state disable` 还是人工断电；
- 停机前后的原始日志文件名；
- 电机和驱动板温度/气味/外观观察；
- 单一假设、结果和下一步，不在同一轮混入多个修复。

### 4.2 最低数据通道

当前 VOFA 默认 7 通道是：

```text
ch0 dtc_a
ch1 dtc_b
ch2 dtc_c
ch3 i_q
ch4 iq_set
ch5 out_vel_rpm
ch6 out_pos_turns
```

这些通道可以观察占空比、Iq 和输出运动，但不足以单独证明电压开环是否正确。正式判定开环时还必须从冻结快照、调试器 RAM 或新增日志通道获得：

```text
raw_adc_vbus / raw_adc_ia / raw_adc_ib / raw_adc_ic
v_bus / ia / ib / ic / id / iq
vd / vq / theta_sim / theta_elec
encoder_raw / encoder_valid / omega_elec
fault_code / trip_reason / sample_counter
```

如果只有当前 7 通道，测试结果只能标记为“辅助观察”，不能标记为“电流和坐标链路通过”。

### 4.3 串口和 VOFA 的边界

当前生成 USART1 是 `115200 8N1`。历史文档中的 `COM54@1000000` 不能直接用于本轮测试；COM 口必须在设备管理器中确认，历史会话中的 `COM3` 也不能硬编码为当前事实。

文本 Shell 探针可用于只读预检：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/flows/board/run_shell_serial_probe.ps1 `
  -PortName <实际端口> -BaudRate 115200 `
  -Commands @('help', 'sys status', 'state show', 'fault status', 'obs all', 'obs enc', 'diag param show', 'diag pid all show')
```

该脚本只负责顺序发送和读取文本，不具备独立的电机保护能力。带电命令不能批量塞入脚本后无人看守。VOFA 使用 JustFloat 时必须保存原始二进制帧，不能把二进制流当作文本解析；所有二进制解析放在停机后。

## 5. 通用执行模板

每个带电测试都按照相同的 10 步执行：

1. PSU 输出关闭，确认电机机械固定、断电开关在手边；
2. 创建本次测试目录，写入测试 ID 和目标；
3. 启动只读日志，记录 `state show`、`fault status`、`obs all`、`diag param show`；
4. 确认轴处于 IDLE 或明确的可启动状态，故障锁存不能被忽略；
5. 在 IDLE 状态下设置本轮唯一变量；
6. 设定 PSU 电压和电流限值，确认实际 VBUS；
7. 发送 `state start`，使用独立秒表/硬件计时器开始计时；
8. 运行中只观察预先定义的硬限值，异常时立即执行软件 disable 和 PSU 断电；
9. 正常结束也优先发送 `state disable`，随后关闭 PSU，确认电流衰减和 `axis=fault`；
10. 先保存原始日志和快照，再清故障、复位或改变下一组参数。

### 5.1 禁止使用普通 STOP 代替紧急 DISABLE

正常 `state stop` 进入 IDLE；当前 IDLE 会重新使能 PWM 并使用 0% 占空比，具有低侧钳位/主动短路语义。紧急测试结束使用：

```text
state disable
fault status
state show
```

如果电流没有快速下降，不等待上述命令返回，直接关闭 PSU。`fault clear` 只能在电机停止、母线关闭、日志保存后执行。

## 6. 具体测试步骤

### T00：只读通信和参数预检

**目的**：确认当前运行的确实是目标 AT32F415 固件，且后续命令单位明确。

**条件**：PSU 关闭，电机不产生功率输出。

**命令顺序**：

```text
help
sys status
state show
fault status
obs all
obs enc
diag param show
diag pid all show
```

**必须记录**：

- `axis`、`ctrl`、`input`、`fault`；
- `poles` 是否为 7；
- VBUS 阈值、current peak、速度阈值；
- encoder valid、机械角、电角、速度；
- 当前 PID 限值和电流带宽。

**通过条件**：状态可读、无遗留 fault、`poles=7`、参数单位无歧义、日志文件创建成功。

**失败动作**：不接通 PSU，不进入 T01；先处理固件版本、参数恢复或串口协议问题。

### T01：母线和静态采样基线

**目的**：确认上电后母线和 CSAREF/三相采样没有在 PWM 输出前就异常。

**条件**：

- 电机已接好但轴处于禁止输出状态；
- PSU `8.0 V / 0.20 A`；
- PWM 保持 disabled 或无驱动命令；
- 只读日志持续采集。

**步骤**：

1. PSU 输出关闭状态下发送 `state disable`；
2. 接通 PSU，确认 VBUS 在 `7.0–9.0 V`；
3. 不发送 `state start` 和任何 `diag set`；
4. 读取 `obs all`/`obs enc`，并保存原始 ADC 或快照；
5. 观察 300 ms，随后关闭 PSU。

**通过条件**：

- VBUS 稳定在包线内；
- 三相采样围绕 CSAREF 中点，无明显单相偏置或持续漂移；
- 无明显母线电流、声音、温升和异味；
- PWM disabled 状态没有自行恢复。

**失败动作**：立即关闭 PSU。若静态采样就异常，不执行任何电压注入；优先回查 CSAREF、GAIN、ADC rank、采样地和功率级短路。

### T02：`dp0` 固定定子矢量

**目的**：不依赖编码器，确认一个固定定子轴能产生可解释的低电流响应。

**初始参数**：

```text
VBUS = 8.0 V
PSU current limit = 0.20 A
phase = 0
voltage = 0.10 V
dwell = 0.3 s
```

**命令顺序**：

```text
diag set dp0 0.10
state start
```

运行期间记录 `ia/ib/ic`、`id/iq`、占空比和故障快照。达到 0.3 s 或任何异常时：

```text
state disable
```

随后关闭 PSU、保存日志、确认电流衰减。

若 T02 通过，重复三组独立实验：

```text
diag set dp0 0.20
diag set dp1 0.10
diag set dp1 0.20
diag set dp2 0.10
diag set dp2 0.20
```

每一组都必须重新从安全状态开始，不能在同一个 RUNNING 会话中连续批量发送。

**通过条件**：

- 固定矢量下电流快速稳定，不持续爬升；
- `0.10 V -> 0.20 V` 的电流变化方向和比例合理；
- 三个定子轴响应没有一轴明显异常；
- 运行期间 `|I| < 0.10 A` 警戒值，且无卡死高流；
- 无异常声、明显抖动和快速升温。

**失败判定**：任意 `dp` 在 `0.10 V` 就出现 `|I| ≥ 0.12 A`、强烈抖动或静止高流，立即停止并回退到 T01。此时不允许把电压加到 0.2 V 以上。

### T03：固定矢量相位一致性

**目的**：比较 `dp0/dp1/dp2` 的三相电流响应，排除相序、相位映射和电流符号问题。

**方法**：

1. 使用 T02 中通过的最小电压；
2. 依次只测试 `dp0`、`dp1`、`dp2`；
3. 对每个轴保存触发前后快照；
4. 比较三相电流主导相、符号、幅值和 KCL 一致性；
5. 不连接闭环编码器角度，不修改 `encoff`。

**判定重点**：

- 主导相是否按理论的 120° 关系迁移；
- 电流符号是否与 Clarke/Park 约定一致；
- 某一相是否始终偏大、偏小或反号；
- 占空比输出是否与 `dp` 轴变化一致。

**失败动作**：冻结当前数据，回查 `motor_drv.c` 的 PWM 映射、ADC rank、sense-FET 符号、CSAREF 和 GAIN，不进入扫角。

### T04：低幅值电压开环固定速度

**目的**：验证电压开环的电角度推进是否连续，且转子不跟随时能被快速识别和停止。

这里的命令第二个数是电气角速度 `we_ramp`，单位为电气 `rad/s`，不是 rpm：

```text
diag set vq <电压 V> <电气角速度 rad/s>
```

第一组只使用：

```text
diag set vq 0.10 3.14
state start
```

预期同步机械速度：

```text
rpm_mech = we_elec / (2π × 7) × 60
         = 3.14 / (2π × 7) × 60
         ≈ 4.29 rpm
```

每次运行 0.5 s；若没有连续跟随或电流上升，立即停。通过后按以下顺序每次重新上电/启动：

| 测试 | `Vq` | `we` 电气 rad/s | 预期机械 rpm | 最高允许电压 |
| --- | ---: | ---: | ---: | ---: |
| T04-A | `0.10 V` | `3.14` | `4.29` | — |
| T04-B | `0.20 V` | `3.14` | `4.29` | — |
| T04-C | `0.40 V` | `3.14` | `4.29` | 第一轮上限 |
| T04-D | 通过后才允许 | `6.28` | `8.57` | 保持 `0.40 V` 以下 |
| T04-E | 通过后才允许 | `12.56` | `17.14` | 保持 `0.40 V` 以下 |

T04-D/T04-E 不是必做项；只要 T04-A/T04-B 已经显示卡死高流，就停止整组，不进入更高速度。

**通过条件**：

- `theta_sim` 连续推进，没有跳变或停滞；
- 三相占空比连续变化，没有打满、跳变或失真；
- 转子能够连续跟随，速度方向与 `we` 符号一致；
- 电流低于 `0.10 A` 警戒值，且未出现 200 ms 卡死判据；
- 0.5 s 运行结束后电流能快速下降。

**失败解释**：

- 固定矢量通过、扫角失败：优先查角度推进、SVPWM 电压归一化、载波/ADC 时序和相序；
- `Vq` 增大后才失败：优先查电流换算、采样窗口、母线值和电压饱和；
- 低速能转、高速卡死：优先查电压余量、反电动势前馈和开环加速度；
- 一开始就卡死高流：回到 T02/T03，不调编码器零点。

### T05：低幅值 I-f

**前置条件**：T04 至少完成 T04-A/T04-B，且电流、占空比和停机快照均合格。

I-f 命令格式为：

```text
diag set if <Iq 目标 A> <电气角速度 rad/s>
```

第一组：

```text
diag set if 0.05 3.14
state start
```

通过后逐步执行：

| 测试 | `Iq` 目标 | `we` 电气 rad/s | 预期机械 rpm |
| --- | ---: | ---: | ---: |
| T05-A | `0.05 A` | `3.14` | `4.29` |
| T05-B | `0.10 A` | `3.14` | `4.29` |
| T05-C | `0.10 A` | `6.28` | `8.57` |
| T05-D | `0.10 A` | `12.56` | `17.14` |
| T05-E | 通过后才允许 | `31.42` | `42.86` |

每一组最长 1 s。`T05-E` 只有在低速多次通过后才允许，不能把历史记录中的“300 Hz”作为输入。`we=31.42 rad/s` 与 7 对极对应约 42.86 rpm。

**通过条件**：

- `i_q` 跟随目标且不打满；
- 三相电流没有明显偏置、反号或单相异常；
- 电气速度与机械速度满足 7 对极换算；
- 运行平滑、无持续卡死和快速温升。

**诊断意义**：I-f 通过只能证明“电流环 + 开环角度推进 + 功率级”组合能工作，不能证明编码器电角度零点正确，也不能直接授权闭环速度。

### T06：编码器原始角度和方向

**条件**：PSU 关闭、PWM disabled、机械手动缓慢转动。

**命令**：

```text
obs enc
```

重复读取并保存：

- 原始/机械角是否连续回绕；
- 正向手动旋转时角度符号；
- 一机械圈内电角度是否完成约 7 个周期；
- encoder valid 是否稳定；
- 是否存在固定跳变、非线性突变或读数卡死。

**LUT 对照**：

1. 先记录启用当前 LUT 的结果；
2. 再以不改变其他参数的方式记录原始线性角结果；
3. 比较 LUT 是否引入空间角度扭曲；
4. 不在电机带电时直接反复修改 LUT 和 `encoff`。

**失败动作**：停止所有闭环实验，优先处理 SPI mode、CS、读数超时、方向和 LUT；编码器原始角度不可信时，`encoff` 试验没有意义。

### T07：电角度零点小步长搜索

**前置条件**：T04、T05、T06 通过；G0 仍需有效；电机使用低压、低速、低电流包线。

`encoff` 的输入单位是电气 rad，且实现是累加到当前 offset。每次测试必须从同一个已记录基线开始，不能连续累加后丢失实际偏移量。

搜索步长：

```text
15° = 0.261799 rad electrical
```

建议顺序：

```text
0
+0.261799
-0.261799
+0.523599
-0.523599
+0.785398
-0.785398
```

每一个 offset 都需要：

1. PSU 关闭；
2. 恢复同一基线参数；
3. 在 IDLE 状态设置 `diag param encoff <delta_rad>`；
4. 低速命令，例如 `diag set vel 5`，其中 `5` 是输出机械 rpm；
5. `state start`，运行不超过 0.5 s；
6. 观察是否出现“转子吸附、Iq 打满、速度不跟随”；
7. 立即 `state disable`、关闭 PSU、保存数据。

**通过条件**：

- 在某个 offset 附近，低速正向能连续启动；
- `Iq` 不持续饱和；
- 转子不吸附在静态矢量；
- 正向和反向小速度命令都能得到可解释的响应；
- 同一 offset 重复测试结果一致。

**失败解释**：如果正负 `90°` 只产生相同的卡死结果，说明简单 offset 可能发生自抵消，应停止继续扩大 offset，回到方向、LUT 和几何锚点分析。

### T08：低速闭环速度确认

**前置条件**：T07 在同一基线下重复通过至少 3 次。

只允许从极低速度开始：

```text
diag set vel 5
state start
```

通过后再执行：

```text
diag set vel -5
```

每次运行最长 0.5 s，速度硬限值保持在 `20 rpm`。不允许直接使用 `±300 rpm`，也不允许在方向未确认时增加速度。

**通过条件**：

- 正反向都能启动和停止；
- 速度误差收敛，Iq 不打满；
- 无静态吸附、剧烈抖动和明显发热；
- 同一参数重复 3 次结果一致；
- 日志和快照可以在停机后完整解析。

T08 通过后才讨论速度环带宽、PID 和 12 V 包线；不通过时不修改多个控制参数。

## 7. 每阶段失败后的决策树

| 失败位置 | 立即回退 | 首要假设 | 禁止动作 |
| --- | --- | --- | --- |
| T01 静态采样异常 | T00/T01 | CSAREF、GAIN、ADC rank、采样地或硬件短路 | 不加电压、不启动 PWM |
| T02 固定轴高流 | T01 | 电流符号、PWM 映射、相线、CSA 零点 | 不加大电压“顶过去” |
| T03 三轴不对称 | T02/T03 | 相序、互补通道、ADC 相序、sense 极性 | 不进入编码器校准 |
| T04 低速扫角卡死 | T03/T04 | SVPWM 角度推进、载波/ADC 时序、开环电压路径 | 不调速度 PID |
| T04 高电压才卡死 | T04 低级别 | 电流换算、采样窗口、电压饱和、反电动势 | 不提高电源限流 |
| T05 I-f 失败 | T04 | 电流环、PI、采样延迟、current sign | 不修改 encoff |
| T06 编码器失败 | T06 | SPI、CS、mode、方向、LUT | 不跑闭环 |
| T07 零点搜索失败 | T06/T07 | 方向/LUT/几何关系而非单一 offset | 不连续累加 offset |
| T08 闭环吸附高流 | T07 | 零点、坐标方向或速度反馈符号 | 不继续运行 3 s |

每次失败只写一个主假设。修正前先补充能区分该假设的最小证据，再修改一个变量并重复对应测试。

## 8. 停机后的分析顺序

电机已停止、PSU 已关闭后，按以下顺序分析，不在带电状态中深挖：

1. 确认停机时间、PWM disabled、母线电流衰减；
2. 检查是否存在硬件损伤或异常温升；
3. 解析触发前窗口，而不是只看最终一帧；
4. 先看 raw ADC 和 CSAREF，再看换算电流；
5. 再看相电流 KCL、Clarke/Park、`Id/Iq` 和电流符号；
6. 再看 `theta_sim`、`theta_elec`、占空比和 SVPWM sector；
7. 最后看速度、位置和 PID 输出；
8. 形成单一根因假设和下一轮最小测试；
9. 保存报告后，才允许清 fault 或修改参数。

## 9. 结果判定格式

每个测试必须使用以下结果之一：

- `PASS`：达到本阶段全部通过条件，允许进入下一阶段；
- `FAIL_SAFE_TRIP`：保护或人工急停触发，必须停机分析；
- `FAIL_DIAGNOSTIC`：未达到目标但未触发危险状态，保留数据并回退；
- `BLOCKED`：安全门禁、数据通道、单位或硬件事实不足，禁止运行；
- `INVALID`：命令批量发送、阈值未记录、日志丢失或变量同时改变，结果作废。

不得使用“感觉差不多”“运行起来了”“没有马上烧坏”等非证据结论替代结果状态。

## 10. 相关文件

- [电机实机调试安全协议与开环排障流程](motor-debugging-safety-and-procedure.md)
- [原理图—AT32F415 配置审计](configuration-audit.md)
- [Board1 与 DRV8311H 深度分析](hardware-and-drv8311h.md)
- [运行方式与无板验证](operations-and-validation.md)
- [`app_shell_cmd_state.c`](../../applications/shell/app_shell_cmd_state.c)：`state start/stop/disable`
- [`app_shell_cmd_diag.c`](../../applications/shell/app_shell_cmd_diag.c)：`diag set/param/pid`
- [`app_shell_cmd_obs.c`](../../applications/shell/app_shell_cmd_obs.c)：只读观测命令
- [`app_shell_cmd_fault.c`](../../applications/shell/app_shell_cmd_fault.c)：故障状态与清除
- [`foc_motion_runtime.c`](../../applications/runtime/foc_motion_runtime.c)：电压开环/I-f/定子轴入口
- [`motor_fault_runtime.c`](../../applications/runtime/motor_fault_runtime.c)：当前故障策略缺口
- [`motor_fsm.c`](../../applications/tasks/motor_fsm.c)：FAULT、PWM 关闭和状态边界
- [`motor_drv.c`](../../applications/periph/motor_drv.c)：PWM/ADC 硬件耦合入口
- [`foc_config.h`](../../applications/algo/foc_config.h)：电机、母线、采样和环路参数
- [`run_shell_serial_probe.ps1`](../../scripts/flows/board/run_shell_serial_probe.ps1)：只读/受控 Shell 文本探针

本方案是实机测试的操作门禁和实验记录模板。当前固件已经具备经 host 验证的快速 trip、任务级后备和故障锁存，但这不替代硬件 Break、独立电源急停或操作者的立即断电动作。任何实机测试结果必须附带原始日志、停机原因和测试条件。

## 11. 2026-08-27 软件门禁复核后的当前状态

### 11.1 已通过的 G0 子项

- `test_motor_fault_runtime_contract`、`test_foc_calib_curr_contract`、`test_foc_vbus_guard_contract` 和 `test_foc_bringup_model_contract` 已在无板环境通过。
- AT32F415 的 full-diag、standard、tiny 三个固件变体均构建通过。
- 早期记录中的 full-diag OpenOCD 写入/校验、SWD `axis_state=0`、`fault_code=0`、`v_bus≈11.88 V` 和三相 offset 仅是历史只读证据；本轮不重复、不依赖这些动作。
- 最终通过 `pwm_out_disable()` 关闭六路 PWM；立即及 300 ms 后 TIM1 `CCTRL=0x1000` 保持不变，未发送任何电机启动命令。

### 11.2 G0 仍未解除的项目

G0 仍是 `BLOCKED`，原因不是软件构建，而是功率级安全证据尚不完整：

- 本轮完整 host flow 的 35 个目标全部编译成功并通过 CTest（`35/35`）；软件回归门禁已通过，但这不关闭功率级硬件安全门禁。
- DRV8311H 的 `nFAULT`、`nSLEEP` 在当前网表没有 MCU 端点；软件不能观测驱动器故障，也不能由 MCU 控制硬件睡眠。
- AT32F415 TIM1 当前生成配置仍为 `brk_enable=FALSE`、`deadtime=0`，没有示波器证据证明互补门极不会直通。
- `pwm_out_disable()` 只关闭定时器六路输出，不切断 `DCBUS`；12 V / 1 A 是电源输入限制，不等于相电流限制。
- 串口同时承载 Shell 和 VOFA 二进制流，本轮出现 `<RxBuff Overflow>`；实时保护不允许依赖串口分析。

### 11.3 下一次允许执行的最小步骤（受控诊断轨道）

在 G0 的产品级硬件项目完成前，不执行长时间或无人值守驱动动作；在最低人工
断电条件具备时，允许按以下顺序执行受控短脉冲：

1. 物理确认电机三相、编码器和供电极性；电机保持机械自由且旁边无可卷入物。
2. 上电后只读取静态量：VBUS、三相 raw ADC、offset、温度、`axis_state`、`fault_code`；不发送 `state start`。
3. 若 raw/offset/换算电流不在 T01 门限内，立即执行安全停机并断开 12 V，再在停机后分析日志。
4. 先执行 D0 零电压基线，再执行 D1/D2/D3 的 `0.05 V`、不超过 `20 ms`
   固定矢量；每次重新上电并停机后读取数据，不批量连续发送。
5. 只有 D0-D3 均无异常，才允许 `0.10 V` 固定矢量或 O0/O1 最低速开环脉冲。
6. 死区/重叠、电流和温度仪器证据继续记录为后续确认项，不在此处伪造为已验证。

本轮此前的实板动作属于“刷写 + 上电校准 + SWD 只读/安全关闭”，不属于 T01
通过，也不构成电机开环或闭环验收；历史 2026-08-24 的开环运行证据另行保留，
不能被本次只读结果覆盖。

## 12. 本计划执行时的独立准入结论

当前仅完成无板软件实现与验证，实机准入不因 host/CTest/固件构建通过而自动解除：

```text
REAL_BENCH_TEST_STATUS=BLOCKED
```

在状态解除前，不执行本文件面向产品的连续运行、I-f、编码器校准、速度闭环或长时间 PWM。允许的例外是第 13 节定义的 bench 短脉冲故障定位：它必须使用已核验 bench ELF、自动运行预算和停机后导出，且不解除产品级状态。`FOC_ENERGIZED_CALIB_ENABLE=0U` 时，`cal motor`/`cal enc` 必须被拒绝；电流偏置校准之外的带电校准不属于当前 bring-up 流程。

解除状态必须同时有：DRV8311H `nFAULT` 的 MCU/独立监控路径、`nSLEEP` 状态一致性、TMR1 Break 或等效硬件快速关断、互补 PWM 死区的配置与示波器确认、可触达物理断电开关、机械固定、示波器/电流/温度测量和白名单命令。12 V/1 A 只能限制电源输出能力，不能替代上述保护。

硬停仍遵守不可延迟的时序：

```text
检测异常
→ 立即关闭 PWM
→ 锁存故障上下文
→ 禁止继续发送驱动命令
→ 切断电源/使能
→ 等待电机完全停止
→ 保存并分析日志
```

## 13. 2026-08-28 现有板开环卡死根因定位阶段

本节是上一阶段实现后的执行顺序。它不解除第 12 节的产品级 `BLOCKED` 状态；
但在最低人工断电条件满足后，可由现场负责人按本节执行受控短脉冲诊断。
本阶段不修改 PCB、不控制 `nSLEEP`、不执行带电电机/编码器校准，也不使用
外部可编程设备。12 V/1 A 仅是电源背景，不能替代相电流保护或人工断电。

### 13.1 上电前和静态阶段

断电时记录三组绕组电阻、相线到 GND、`DRV_3V3-GND`、编码器 SPI/CS 和
连接器相序；三组绕组应同数量级，不能有开路或明显短路。上电但不接电机时
使用 12 V/1 A，只读读取 `sys status`、`state show`、`fault status`、
`obs all`、`diag param show`、`diag pid all show`，不发送 `state start`、
`cal motor`、`cal enc`、`if` 或 PID/电压写命令。

静态记录必须包含 DCBUS/VBUS、`DRV_3V3`、`CSAREF`、`nSLEEP`、可测的
`nFAULT`、三相 raw/offset、换算电流、轴状态、故障码和 PWM 禁止状态。电机
重新接入后保持 PWM 禁止并重复一次。任一静态量不合格，立即断电并停在
诊断阶段。

### 13.2 固定矢量阶段

每组都从断电/重新上电/静态读取开始，只在 `IDLE` 设置一个变量；运行中
`diag set` 写入会被 Shell 和 runtime 同时拒绝，并返回
`result=error reason=idle_only`。顺序如下：

说明：下表是完整诊断计划上限；当前 bench 镜像还增加了约 `50 ms` 的 ADC ISR 自动
运行护栏，因此本轮实机执行采用 `20 ms` 脉冲或让护栏到期，不会达到下表的产品级
持续时间。

| 测试 | 命令 | 最长时间 | 目的 |
| --- | --- | ---: | --- |
| D0 | `diag set dp0 0.00` | 100 ms | 零电压/PWM 基线 |
| D1 | `diag set dp0 0.10` | 300 ms | A 轴固定矢量 |
| D2 | `diag set dp1 0.10` | 300 ms | B 轴固定矢量 |
| D3 | `diag set dp2 0.10` | 300 ms | C 轴固定矢量 |
| D4 | `diag set dp0 0.20` | 300 ms | D1-D3 全通过后的增量测试 |

每组结束使用 `state disable`（异常时不等待回复，直接切断 PSU），然后
读取 `fault status`、`state show`、`obs all`。D1-D3 的三个电流幅值应同
数量级；`0.10 V/2.55 Ω` 约为 `0.039 A` 的静态 RL 量级。任何相超过
`1.00 A` 均为立即硬停；闭环模式下超过 `0.75 A` 且低于 5 rpm 持续 200 ms
才触发堵转判据，电压开环不使用该判据，不继续增加电压。

### 13.3 电压开环阶段

仅在 T00/T01 和 D0-D3 全通过后执行，使用专用 bench 固件：

```powershell
cmake --build --preset build-fw-full-diag-carrier-bench-400ms-debug --parallel
```

该镜像保持 Shell，关闭 VOFA 二进制输出，并强制保持
`FOC_ENERGIZED_CALIB_ENABLE=0`。测试矩阵如下，命令第二个参数是电气
`rad/s`，不是机械 rpm：

| 测试 | 命令 | 最长时间 | 目标 |
| --- | --- | ---: | --- |
| O0 | `diag set vq 0.10 0.00` | 100 ms | 固定 q 轴矢量 |
| O1 | `diag set vq 0.25 21.99` | 400 ms | 30 rpm 机械速度起步 |
| O2 | `diag set vq 0.50 21.99` | 400 ms | 提高电压，不提高速度 |
| O3 | `diag set vq 0.75 21.99` | 400 ms | 额定电流附近的受控诊断 |
| O4 | `diag set vq 1.00 21.99` | 400 ms | 仅 O1-O3 无异常后执行 |

7 对极换算为 `rpm_mech = we_elec/(2π×7)×60`，所以 `21.99 rad/s` 约为
`30 rpm`。O1-O4 必须观察 `theta_sim` 连续且归一化、
`we_volt_now` 受控逼近 `we_volt_ramp`、duty 有界且有限、VBUS 在
`10.8–13.2 V`、电流保持低于 `0.75 A` 持续保护区，并且没有卡死、尖叫、快速升温。
由于 `DEFAULT_VOLT_WE_ACCEL=60 rad/s²`，从零到 `21.99 rad/s` 约需 `367 ms`，
400 ms 主要是启动过渡窗口，不是稳态旋转验收；需要更长观察时必须另行批准受限
bench-long 镜像。
电压开环不应使用 5 rpm 作为旋转目标；低压不转本身不能判为软件失败。若角度、duty、VBUS 和电流均正常，应把
结论暂记为电压/转矩裕量、机械负载或电机参数待验证。

当前已执行的短脉冲结果见第 15 节。bench 预算到期时的 `FAULT_COMM_ERROR` 是
诊断自动停机，不是 DRV8311H 故障或过流结论。

### 13.4 统一日志与停机分析

每组创建 `tmp/bench/YYYYMMDD_HHMMSS/`，至少保存
`preflight.log`、`T00_static.log`、`D0_zero_vector.log`、
`D1_dp0.log`、`D2_dp1.log`、`D3_dp2.log`、`O0_vq_static.log`、
`O1_vq_3p14.log`、`fault_context.txt`、`operator-notes.md` 和 `result.md`。
`fault status` 的上下文是首次故障的 RAM 快照，包含 raw ADC、换算电流、
VBUS、角度、`vd/vq`、SVPWM duty/sector；不要在保存前执行 `fault clear`。

运行中只看预先定义的硬停条件；详细数据关系在电机停止、PSU 断开后分析。
每次只保留一个主根因假设：软件坐标/相序、ADC 电流链、DRV 供电、采样
时序或机械转矩不足，不能在同一轮同时改变多个参数。

本节完成前和完成后均保持产品级状态：

```text
OPEN_LOOP_SOFTWARE_PATH=PASS
MOTOR_PHYSICAL_OPERATION=UNVERIFIED
REAL_BENCH_TEST_STATUS=BLOCKED
```

## 14. 2026-08-28 只读 T01 现场执行记录

本次执行仅完成 Task 1 无板门禁和 COM3 只读查询，没有烧录、没有发送
`state start`、`diag set`、校准、PID 写入或 `fault clear`。主机测试为
`33/33 PASS`，独立 CTest 为 `33/33 PASS`，外设契约为 `PASS`，bench
固件构建为 `PASS`；bench ELF SHA256 为
`A919731EED1FFE548DE285E82C743F35002D3B0E2A5FB0DE7448C3D2733831FD`。

COM3 的只读响应显示：`vbus≈11.91 V`、`axis=fault`、
`fault=0x00000080`、故障名为 `calibration-failed`、`iq=0`、`id=0`、
速度为 `0 rpm`。当前 Shell 没有板上 build-id/hash 查询接口，因此不能
证明响应镜像就是最新 bench ELF；延长等待后的 `obs all` 仍缺少当前源码
应输出的 `phase/amps/ctrl/svpwm` 行，`fault status` 也缺少 context 行，
这是镜像不匹配的强指示，但仍不能排除串口丢包/溢出。`diag debug show`
返回 `bad_arg/rc=-4`，经源码核对该命令本来就不属于当前 Shell 命令面，
不能据此判断镜像过旧。串口文本还存在部分乱码，因此 raw ADC、offset、
三相电流、duty、sector 和物理 PWM 状态均未形成有效证据。

本次结果不通过 T01，不进入 D0；G0 仍为 `BLOCKED`。详细原始记录见：

- [T01 结果](../../tmp/bench/2026-08-28_224839/result.md)
- [只读串口记录](../../tmp/bench/2026-08-28_224839/T01_static.log)
- [故障上下文记录](../../tmp/bench/2026-08-28_224839/fault_context.txt)

## 15. 2026-08-29 现有板短脉冲实测结果

本节是第 13 节受控诊断轨道的实际执行记录，专门回应“必须先完成完整 G0 才能发现
问题”的不适用情况。当前没有把示波器、独立电流/温度仪器当作第一枚诊断脉冲的硬
阻塞；而是依赖 bench 固件的 ADC 快速过流、VBUS/非法数据检查、约 50 ms 自动预算、
故障锁存和停机后 Shell 导出。产品级连续运行仍保持 `BLOCKED`。

### 15.1 固件与现场条件

- `[实板测量]` 使用 ELF `build/fw-full-diag-carrier-bench-debug/output/falcon-foc-at32.elf`，OpenOCD 返回 `Verified OK`。
- `[实板测量]` 首轮 D0-D4/O0-O3 ELF SHA256：`CDE7AC7D8044719D4CF4E2650C298C566D4C3CF2A72DE439D8119AA13BE3B4BC`。
- `[实板测量]` 最终重刷并复测 O1 的 ELF SHA256：`325A464AA838DCB7344657E2917953DD52515D72AD7ED0B40B5AF91D5C5BEE36`。
- `[源码确认]` `TM_BENCH_DIAG=1`、`TM_FEATURE_VOFA=0`、`FOC_ENERGIZED_CALIB_ENABLE=0`；bench FOC step 为 5 kHz，TMR1/ADC 载波约 20 kHz。
- `[实板测量]` COM3/115200 返回启动状态 `axis=idle`、`fault=0`、`vbus≈11.91 V`。
- `[实板未验证]` 没有独立 PWM 波形、外部相电流、温度、死区/重叠和 DRV `nFAULT` MCU 反馈证据。

### 15.2 已执行矩阵

| 测试 | 命令/脉冲 | 板上结果 | 判定 |
| --- | --- | --- | --- |
| D0 | `diag set dp0 0.00`，20 ms | `fault=0`，duty `0.5/0.5/0.5`，电流约 `1/-5/0 mA` | PASS |
| D1 | `diag set dp0 0.05`，20 ms | `fault=0`，sector 1，电流约 `3/0/-2 mA` | PASS |
| D2 | `diag set dp1 0.05`，20 ms | `fault=0`，sector 2，电流约 `1/-4/3 mA` | PASS |
| D3 | `diag set dp2 0.05`，20 ms | `fault=0`，sector 5，电流约 `1/1/3 mA` | PASS |
| D4 | `diag set dp0 0.20`，20 ms | `fault=0`，duty `0.49/0.51/0.51`，电流约 `1/2/1 mA` | PASS |
| O0 | `diag set vq 0.10 0.00`，20 ms | `theta=0`、`we=0`，电流约 `1/-4/5 mA` | PASS |
| O1 | `diag set vq 0.10 3.14`，20 ms | `theta≈0.01`，`we≈1.28/3.14`，电流约 `4/1/-6 mA` | PASS |
| O1-G | 同命令，80 ms 请求 | 50 ms 护栏锁存 `theta≈0.075`、`we≈3/3.14`，电流约 `-7/4/3 mA` | PASS |
| O2-G | `diag set vq 0.20 3.14`，80 ms 请求 | 锁存电流约 `-12/25/5 mA` | PASS |
| O3-G | `diag set vq 0.40 3.14`，80 ms 请求 | 锁存电流约 `-48/60/-2 mA`，duty `0.503/0.481/0.519` | PASS |
| O1-R | 最终镜像，`diag set vq 0.10 3.14`，20 ms | `fault=0`，`theta≈0.01`，`we≈1.32/3.14`，电流约 `1/2/1 mA` | PASS |

O1-G/O2-G/O3-G 的 `fault=0x00000100` 是 bench 运行预算到期的
`FAULT_COMM_ERROR`，不是过流或 DRV 硬件故障。完整记录见
[2026-08-29 result.md](../../tmp/bench/20260829_actual/result.md)。

### 15.3 停机后判断

当前低压短脉冲的真实板上数据没有复现“高电流卡死”：VBUS 约 `11.90–11.91 V`，
最大锁存软件换算电流约 `60 mA`，O1 的 `theta_sim/we_volt_now` 已按目标推进。
这排除了“当前 bench 镜像在短脉冲内完全不执行电压开环”的假设，但不能排除以下
尚未覆盖的情况：

- 运行时间延长后电流积累或热异常；
- 外部实际相电流与 ADC/CSA 换算不一致；
- 物理互补 PWM 重叠、死区不足或采样窗口在某些扇区失效；
- `0.10–0.40 V` 的电压不足以克服静摩擦、齿槽转矩或实际负载。

因此本阶段不写入唯一根因确认，继续保持：

```text
OPEN_LOOP_SOFTWARE_PATH=PASS
MOTOR_PHYSICAL_OPERATION=UNVERIFIED
OPEN_LOOP_ROOT_CAUSE_STATUS=UNRESOLVED
REAL_BENCH_TEST_STATUS=BLOCKED
CONTROLLED_DIAGNOSTIC_STATUS=ALLOWED_WITH_MINIMUM_GUARDS
```
## 16. 2026-08-29 电压入口相位 A/B 与最终短脉冲结果

### 16.1 本轮执行边界

本轮按“先取证、后改单变量”的原则执行。由于示波器、外部相电流和温度测量
无法作为当前第一轮诊断的前置条件，采用已验证的 bench 固件、`12 V/1 A`、
每次约 `20 ms` 的短脉冲和固件自动护栏；操作者保持可直接断电。该流程只用于
发现软件入口和板级响应差异，不解除产品级
`REAL_BENCH_TEST_STATUS=BLOCKED`，也不证明连续运行安全。

### 16.2 相位入口修复

`[源码确认]` 默认 `ctrl_mode=CTRL_MODE_VOLT` 时，旧实现可能在第一次 `vq` 写入
前保持 `theta_sim=0`；而实时编码器电角度已经处于约 `3.3 rad`。最小修复位于
`applications/runtime/foc_motion_runtime.c`：在 IDLE 写入电压命令，或真正从其他
控制模式切入 VOLT 时，将 `theta_sim` 同步为
`enc_pri.state.angle_elec_rad` 的 `[0,2π)` 包络值，并清零 `we_volt_now`。运行中
更新电压命令不重复同步，固定相电压路径仍使用显式 phase 角度。

第一版读取 `meas_data.theta_elec` 的修复被实板 A1 证伪为缓存不足；改用编码器
实时状态后才获得有效对齐。`motor_runtime_snapshot()` 也改为报告实时编码器角度，
确保停机后诊断数据与入口状态一致。

### 16.3 A/B 实测矩阵

最终 bench ELF：
`build/fw-full-diag-carrier-bench-debug/output/falcon-foc-at32.elf`；SHA256：
`F0402BADC248FEB2C7EEB3F149DF321B6A3189B3C65125AB876A5E83064F04B1`。
编译定义为 `TM_BENCH_DIAG=1`、`TM_FEATURE_VOFA=0`、
`FOC_ENERGIZED_CALIB_ENABLE=0`，并已由 OpenOCD 返回 `Verified OK`。

| 测试 | 实测结果 | 判断 |
|---|---|---|
| 修复前 A1：`diag set vq 0.10 3.14`，不启动 | `theta_sim≈0`，`theta_elec≈3.35 rad` | 相位入口缺陷复现 |
| 修复后 A1：同命令，不启动 | `theta_sim≈3.21`，`theta_elec≈3.20 rad`，`we_now=0` | 相位误差约 `0.01 rad`，通过 |
| A2：`vq=0.10`、`we=+3.14`、20 ms | `fault=0`，电流约 `2/-4/3 mA`，速度约 `-0.21 rpm` | 通过 |
| A3：`vq=0.10`、`we=-3.14`、20 ms | `fault=0`，电流约 `-3/-3/-2 mA`，速度约 `0.20 rpm` | 通过 |
| V2/V3/V4：`vq=0.20/0.30/0.40`、`we=+3.14`、20 ms | `fault=0`，最大板上换算电流约 `6 mA`，VBUS≈`11.91 V` | 受控窗口通过 |

上述电流是 MCU ADC/CSA 换算值，不是独立外部相电流测量。最终测试后已执行
`fault clear`，板处于 `axis=idle`、`fault=0`，未留下持续 PWM 请求。

### 16.4 阶段结论

`[实板测量]` 相位入口缺陷已被修复后 A1 直接确认；`[实板测量]` 同时表明，
修复前后的超短脉冲均未重现原始高电流卡死，因此不能写成唯一整机根因。
连续运行、机械转矩裕量、外部电流、PWM 死区/重叠、采样窗口和温升仍未验证。

```text
PHASE_ENTRY_ALIGNMENT=CONFIRMED
OPEN_LOOP_SOFTWARE_PATH=PASS
MOTOR_PHYSICAL_OPERATION=UNVERIFIED
OPEN_LOOP_ROOT_CAUSE_STATUS=UNRESOLVED
REAL_BENCH_TEST_STATUS=BLOCKED
CONTROLLED_DIAGNOSTIC_STATUS=ALLOWED_WITH_MINIMUM_GUARDS
```

原始记录位于：

- `tmp/bench/20260829_phase_seed_baseline/`
- `tmp/bench/20260829_phase_seed_after_fix/`
- `tmp/bench/20260829_phase_seed_final/`

下一轮若继续实机，只能针对“相位已对齐但物理仍不转”的单一假设取证，不能同时
修改相序、采样、PI、保护阈值或电机参数。

## 17. 2026-08-29 当前轮次执行结果与准入状态

本节是当前实施轮次的规范结果。此前章节中的旧 ELF 哈希和旧日志目录是历史记录，
不代表当前板上镜像。

### 17.1 镜像、串口与保护边界

- `[实板测量]` 当前使用 ELF：
  `build/fw-full-diag-carrier-bench-debug/output/falcon-foc-at32.elf`。
- `[无板验证]` 当前 SHA256：
  `3969B23E8DB62BCD590B49D5FE2F63574944C3281CDF5A00D2A3CCE3931AD8AE`。
- `[实板测量]` F0–F6、theta0 和 V1–V4 的 pulse 矩阵是在同一源代码的上一轮
  bench ELF（SHA256 `7EBC7C00CB4E8225372B9E774B761CCB1A561E7A9CE96F7C208F8E88C201801E`）
  上完成；随后仅因 host 宏头文件可见性修正而重建/刷写 `3969...`，刷写后只做了
  A0 只读复核，没有重复发送电机脉冲。
- `[无板验证]` 编译定义：`TM_BENCH_DIAG=1`、`TM_FEATURE_VOFA=0`、
  `FOC_ENERGIZED_CALIB_ENABLE=0`、`TM_MOTOR_BENCH_RUN_BUDGET_MS=50`。
- `[实板测量]` 串口为 COM3/115200 8N1，电源设定为 12 V/1 A，软件 VBUS 读数约
  `11.87–11.92 V`。
- `[源码确认]` bench app-shell 任务栈为 2048 B，默认 bench 自动运行护栏仍为
  50 ms；200 ms 镜像只完成构建，没有用于本轮实机。

### 17.2 已执行的最小矩阵

1. A0 上电只读：`axis=idle`、`fault=0`，PWM 保持安全状态，VBUS 正常。
2. A1 相位入口：`diag set theta0 0.0000`、`diag set vq 0.10 3.14`，未启动时
   `theta_sim≈3.12`、`theta_elec≈3.11`，`phase_error≈0.01 rad`，`we_volt_now=0`。
3. A2/A3 正反向 20 ms：`vq=0.10 V`、`we=±3.14 rad/s`，均 `fault=0`，软件
   电流峰值保持毫安级。
4. F0–F6 固定矢量：`dp0/dp1/dp2=0、±0.10 V`，三轴峰值同数量级，未发现单轴
   饱和或持续高电流。
5. theta0 偏置：五个有效 offset 没有可重复的优胜结果；`-3.1416` 因越过严格
   `-π` 边界被正确拒绝，随后以 `-3.14159` 完成有效测试。
6. V1–V4：`vq=0.10/0.20/0.30/0.40 V`、`we=3.14 rad/s`、每次约 20 ms。V3/V4
   的软件 trace 峰值已经超过 50 mA，因此停止，不进入 bench-long 实机窗口。

### 17.3 结论与下一门禁

`[实板测量]` 本轮没有复现原始高电流卡死，不能把相位入口缺陷写成唯一物理根因。
`[实板未验证]` 外部相电流、PWM 波形和死区/重叠、采样窗口、DRV/电机温升、手动
编码器整圈方向扫查和连续机械运行仍未验证。最终状态保持：

```text
PHASE_ENTRY_ALIGNMENT=CONFIRMED
OPEN_LOOP_SOFTWARE_PATH=PASS
MOTOR_PHYSICAL_OPERATION=UNVERIFIED
OPEN_LOOP_ROOT_CAUSE_STATUS=UNRESOLVED
REAL_BENCH_TEST_STATUS=BLOCKED
```

不得因为 F0–F6 或 V1–V4 通过就进入 I-f、编码器带电校准、速度闭环或长时间运行。
完整原始记录位于 `tmp/bench/20260829_133853_next_root_cause_final/`；其中
`result.md` 是本轮汇总，早期失败和中间日志保持不删除。

## 18. 2026-08-30 分层正交矩阵执行规范

本节覆盖下一轮“尽量一次性区分根因”的实际执行入口。它不是产品级连续运行批准，且不
改变现有 `REAL_BENCH_TEST_STATUS=BLOCKED`。

### 18.1 镜像、白名单和停机边界

`[源码确认]` 只使用显式 bench ELF，并在运行前核对 `TM_BENCH_DIAG=1`、
`TM_FEATURE_VOFA=0`、`FOC_ENERGIZED_CALIB_ENABLE=0`。`run_bench_matrix.ps1` 的
`Preflight` 只读；Mapping/Trigger/Phase/Voltage/All 必须显式传入 `-RunPulses`。
脚本拒绝校准、I-f、`encoff`、参数/PID 写入、`fault clear` 和 `state stop`。

当前 `state disable` 的安全收敛会进入 FAULT，所以多脉冲执行必须使用
`-WaitForPowerCycle`，由操作者重新上电/复位并让脚本再次确认 `axis=idle`。串口失联、
数据异常或任何硬停条件都停止矩阵，不等待回复，不自动进入下一项。

### 18.2 固定执行顺序

```text
Preflight
  -> Encoder (PWM disabled, manual forward/reverse)
  -> ADC static (motor disconnected/connected)
  -> Mapping (6 PWM × 6 ADC, only after baseline)
  -> Trigger (9 CH4 compare points)
  -> Phase (9 theta0 points)
  -> Voltage (0.05/0.10/0.15/0.20 V, +/-3.14 rad/s)
```

每项实验使用 5–20 ms 脉冲，单变量、独立日志；V3/V4 或任何超过预警门限的情况不继续
延长。每项停机后再读取 trace/fault，复杂分析不放在运行窗口。

### 18.3 验收条件

- `[源码确认]` trace 必须能读到最新 64 条 FOC sample，encoder trace 必须能读到最新
  32 条 SPI/角度 sample；未出现对应 bench marker 时拒绝运行矩阵。
- `[静态推导]` 软件 map/trigger 的优胜只能缩小假设集合；必须结合 raw ADC、板上 VBUS、
  机械响应和必要的外部仪器证据。
- `[实板未验证]` 没有独立 PWM、相电流和温度测量时，不能宣称死区、实际功率波形、
  DRV 热安全或产品级连续运行通过。

最终只有在最小命令可复现两次、修复后同条件改善两次、且 host/CTest/静态契约/固件构建
全部通过时，才允许写 `OPEN_LOOP_ROOT_CAUSE_STATUS=IDENTIFIED`。

### 18.4 当前镜像执行门禁记录（2026-08-30）

`[实板测量]` bench50 镜像已显式刷写并通过 OpenOCD `Verified OK`；Preflight 确认
`map`、`enc_trace`、`axis=idle` 和 `fault=0`。无 PWM 静态 VBUS 约 `11.91 V`，duty
为 `0/0/0`，软件电流低于预警阈值。

`[实板测量]` 首个 map0/adc0、CH4 compare `3740`、`dp0=0.05 V`、名义 5 ms 的脉冲
未出现软件过流，trace duty 约 `0.498/0.502/0.502`，电流峰值约
`0.019/0.012/0.013 A`；但板端最终锁存 `FAULT_COMM_ERROR (0x100)`，保留 250 条
FOC 样本后进入 FAULT。脚本因此在下一项前停止，不能将该轮视为完整矩阵。

`[静态推导]` 250 条样本与 50 ms bench 护栏一致，当前必须先完成物理 power-cycle/
reset 边界并单独解决 stop-latency 证据，再继续映射/触发/相位测试。不得用
`fault clear` 或提高脉冲时间绕过该门禁。

当前轮次日志：
`tmp/bench/20260830_012451_comprehensive_root_cause/`。

## 19. 2026-08-30 开环有效测试参数修正

本节覆盖前文仍使用 `0.20/0.10/0.12 A` 和 `we=3.14 rad/s` 的旧测试表述；旧数值保留
为历史记录，下一轮执行以本节和源码当前值为准。

### 19.1 当前保护包线

`[源码确认]` 当前临时诊断包线为：每相快速绝对停机 `1.00 A`；任务级持续电流
`0.75 A` 持续 `3000 ms`；闭环堵转 `0.75 A` 且 `<=5 rpm` 持续 `200 ms`。堵转判定
在 `CTRL_MODE_VOLT` 下明确关闭，电压开环只使用快速绝对电流、VBUS/数据有效性和
持续电流保护。该包线基于电机资料的 `0.5 A` 额定/`2.0 A` 最大值，是 bring-up
诊断临时值，不是产品最终热安全结论。

### 19.2 正确的开环旋转窗口

该电机有 7 对极：

```text
we_elec = 2π × 7 × rpm_mech / 60
30 rpm  -> 约 21.99 rad/s
一个电周期 -> 约 285.7 ms
```

因此 `we=3.14 rad/s` 只相当于约 `4.29 rpm`，而 5 ms 只推进约 `0.016 rad`，不能
作为机械旋转验收。固定矢量/映射/触发仍使用 5–20 ms；电压开环使用专用 400 ms
镜像和 30 rpm 默认速度：

```powershell
cmake --build --preset build-fw-full-diag-carrier-bench-400ms-debug --parallel

powershell -NoProfile -ExecutionPolicy Bypass `
  -File scripts/flows/board/run_bench_matrix.ps1 `
  -RepoRoot D:/study/Software/Hollow-Gimbal `
  -Mode Voltage -PortName COM3 -BaudRate 115200 -RunPulses -WaitForPowerCycle `
  -BenchBudgetMs 400 -VoltagePulseMs 400 -OpenLoopSpeedRpm 30
```

脚本会拒绝用 50/200 ms 镜像执行 Voltage 模式；电压梯度为 `0.25/0.50/0.75/1.00 V`，
每次只改变电压且每项独立 power-cycle。`[实板未验证]` 这只是获得足够电气角度和
机械响应观测的测试窗口，不代表该电压梯度已经安全通过；任一快速停机、持续高电流、
VBUS 越界、卡死、尖叫或快速发热都必须立即断电。

### 19.3 状态与证据边界

当前 bench50 首脉冲实际保留约 250 个样本并以 `FAULT_COMM_ERROR` 结束，说明旧的
5 ms 名义脉冲受到 50 ms 护栏/串口停机时序影响。新的 bench400 构建和实际板上运行
必须使用新的 ELF SHA256 单独记录，不能与旧镜像日志混合。没有独立相电流、示波器和
温度证据时，只能报告软件/板上 ADC 观测结果，不能宣称电机物理运行或产品级连续运行
已验证。

## 20. 2026-08-30 当前矩阵执行结果与门禁状态

本轮完整原始记录位于 [`tmp/bench/20260830_074936_post_snapshot_fix/result.md`](../../tmp/bench/20260830_074936_post_snapshot_fix/result.md)。

### 已执行

- `[无板验证]` 当前标准 SVPWM 通过 host/CTest、静态外设契约和 bench400 固件构建。
- `[实板测量]` bench400 镜像显式刷写验证通过；预检、复位和无 PWM 静态状态正常。
- `[实板测量]` PWM/ADC 36 组合矩阵全部完成；没有可重复的单一 map 优胜。
- `[实板测量]` ADC CH4 compare 扫描完成；软件电流对 compare 明显敏感。
- `[实板测量]` 1.2 V/2 V/3 V 电压和 15/20/30 rpm 低速短时对照完成；2 V/20–30 rpm 未建立稳定同步，3 V 触碰 1 A 软件快速边界。
- `[实板测量]` theta0 旋转/静态对照完成；没有可重复相位优胜项。
- `[源码确认]` 停机输出速度/位置快照的陈旧缓存问题已增加测试并修复。

### 未满足的准入项

`[实板未验证]` 尚未有人工 encoder 正/反向整圈记录、PWM/DRV/SOx 示波器、独立相电流、温度或真实机械连续运行证据。因此不得写入 `OPEN_LOOP_ROOT_CAUSE_STATUS=IDENTIFIED`。

当前状态：

```text
SVPWM_SOFTWARE_MODEL=PASS
ADC_MEASUREMENT_WINDOW=ANOMALOUS_OR_STRONGLY_SENSITIVE
PWM_ADC_MAPPING=NO_UNIQUE_WINNER
PHASE_OFFSET_CANDIDATE=REJECTED
OPEN_LOOP_SYNCHRONIZATION=NOT_DEMONSTRATED
OPEN_LOOP_ROOT_CAUSE_STATUS=UNRESOLVED
MOTOR_PHYSICAL_OPERATION=UNVERIFIED
REAL_BENCH_TEST_STATUS=BLOCKED
```

后续不得以提高电压、放宽保护、修改相序/offset 或延长连续时间替代缺失证据；下一项最小判别动作是 PWM 禁止的 encoder 手动正向与反向整圈，然后再决定是否需要 SOx/门极/独立电流测量。

## 21. 2026-08-30 当前镜像方向 A/B 与有效速度窗口执行记录

### 21.1 镜像身份

`[实板测量]` 本轮后续试验使用：

```text
ELF: build/fw-full-diag-carrier-bench-400ms-debug/output/falcon-foc-at32.elf
SHA256: 59B78D648511926F8AD47213CA35E49C0D536697BE814B1B77AF64F84E949A03
TM_BENCH_DIAG=1
TM_FEATURE_VOFA=0
FOC_ENERGIZED_CALIB_ENABLE=0
TM_MOTOR_BENCH_RUN_BUDGET_MS=400
```

Shell 契约测试和 bench runner 白名单已补充 `diag set enc_dir 0|1`。它只用于 bench RAM
实验，不能改变产品默认方向；每次试验先 OpenOCD reset，再在 IDLE 设置方向、theta0 和
电压命令，最后正常 `state disable` 并在停机后读取 trace。

### 21.2 已执行矩阵

- `[实板测量]` `vq=0.50 V,we=0` 静态方向 A/B 各两次：CW 峰值范围约
  `0.017..0.110 A`，CCW 峰值范围约 `0.077..0.126 A`；VBUS 稳定、无故障。
- `[实板测量]` `vq=0.25 V,we=21.9911 rad/s`、350 ms，CW/CCW 各一轮：软件场
  连续推进，电流峰值约 `0.037..0.070 A`，encoder 尾段变化很小。
- `[实板测量]` `vq=1.00 V,we=21.9911 rad/s`、350 ms，CW/CCW 各一轮：软件电流
  峰值约 `0.190..0.289 A`，未触发 `1.00 A` 快速保护，但没有独立证据证明 30 rpm
  机械同步。

日志：[`tmp/bench/20260830_091746_encoder_trace_freeze_validation/result.md`](../../tmp/bench/20260830_091746_encoder_trace_freeze_validation/result.md)。

### 21.3 判定与后续边界

`[实板测量]` `enc_dir=0` 与 `enc_dir=1` 存在电流/encoder 尾段差异，但测试间未固定
同一机械起始角度，不能满足“同一最小命令、同一条件重复两次改善”的根因确认要求。
因此：

```text
ENCODER_DIRECTION_CANDIDATE=SUPPORTED_NOT_CONFIRMED
OPEN_LOOP_ROOT_CAUSE_STATUS=UNRESOLVED
```

`[实板测量][静态推导]` 既有 ADC CH4 compare 敏感性仍单独保留为采样窗口/CSA 建立候选。
在没有 PWM/SOx 波形、独立相电流和温度证据前，不能将软件 trace 数值解释为物理相电流，
也不能把 1 V、350 ms 的短脉冲解释为产品级运行通过。

### 21.4 采样触发点重复结果

`[实板测量]` 使用 bench400 镜像、`vq=1.00 V`、`we=21.9911 rad/s`、`enc_dir=0`，
对 `adc_trig=512` 和 `adc_trig=3740` 各重复两次。512 的软件电流峰值分别约为
`0.012/0.011/0.014 A`、`0.012/0.012/0.015 A`；3740 分别约为
`0.270/0.312/0.262 A`、`0.275/0.276/0.201 A`。VBUS 保持约 `11.87..11.93 V`，
编码器机械角尾段变化约在 `-0.0002..+0.0004 rad`，每次 trace 约 2000 条。

`[实板测量][静态推导]` 两个触发点的重复差异使“ADC/CSA 观测随 CH4 compare 改变”
成为高优先级候选，重点是低侧 CSA 建立时间、中心对齐计数相位、ADC 注入组完成时序
和采样孔径。该结果不证明 512 是正确点；512 可能只是掩盖真实相电流的无效窗口，
3740 的软件电流也不是独立相电流计读数。

四次测试均因名义 350 ms 加串口往返跨过 bench400 的 400 ms 预算，在 `state disable`
后出现 `FAULT_COMM_ERROR (0x100)`，未出现 `FAULT_OVER_CURR`。这是测试编排/时间窗口
问题，后续必须用 trace 实际样本数判断运行时长。原始数据：
[`ADC trigger follow-up`](../../tmp/bench/20260830_095925_adc_trigger_followup/result.md)。

`[实板未验证]` 编码器机械角几乎没有变化，且无 PWM/SOx 波形、独立相电流和温度证据，
所以不能把本轮结果解释为电机已经达到 30 rpm，也不能据此修改默认触发点或提高电压。

### 21.5 2026-08-30 完整正交实机矩阵结果

`[实板测量]` 本轮实际完成：9 个 ADC trigger 点、36 组 PWM/ADC map、`dp0/dp1/dp2`
正负六向固定矢量、1 V 静态相位矩阵、`vq=0.25..2.00 V` 的 30 rpm 参数电压梯度、
正反向动态、`theta0=0/+π/2` A/B 和 `enc_dir=0/1` 对照。每项均先复位并在停机后读取
trace；没有校准、参数写入、`state stop` 或长时间运行。

结果摘要：

- `[实板测量]` 低幅固定矢量下，trigger 9 点软件峰值约 `0.009..0.023 A`；36 组
  map 约 `0.008..0.028 A`；六向固定矢量约 `0.011..0.028 A`，无唯一 map 优胜、
  无单相饱和和无 fault。
- `[实板测量]` 正确的 30 rpm 命令为 `we=21.9911 rad/s`。`vq=0.25..2.00 V` 时软件
  电流总体随电压增加，2 V 最高约 `0.468/0.450/0.065 A`，VBUS 约 `11.86..11.95 V`；
  `theta_sim`/`we_volt_now` 正常推进，但编码器没有稳定跟随软件场。
- `[实板测量]` 1 V 相位矩阵中不同 `theta0` 会影响电流和编码器尾差，但 `0` 与 `+π/2`
  动态各重复两次没有稳定改善；相位偏置候选拒绝。`enc_dir=1` 两次也没有相对于
  `enc_dir=0` 形成稳定改善，默认方向不改。
- `[实板测量][静态推导]` 动态 1 V 对照仍显示 `adc_trig=512` 约 `0.011..0.015 A`、
  `3740` 约 `0.201..0.312 A`，而低幅静态扫描差异很小；因此采样窗口/CSA 建立候选
  仍然存在，但不能把 512 认定为有效采样点。

完整数据索引：[`full hardware root cause`](../../tmp/bench/20260830_103200_full_hardware_root_cause/result.md)。
当前结论仍是：`OPEN_LOOP_SOFTWARE_PATH=PASS`、`MOTOR_PHYSICAL_OPERATION=UNVERIFIED`、
`OPEN_LOOP_ROOT_CAUSE_STATUS=UNRESOLVED`、`REAL_BENCH_TEST_STATUS=BLOCKED`。没有 PWM/SOx
波形、独立相电流、温度和机械测速证据，不能把软件 trace 当作产品级物理验证。

### 21.6 2026-08-30 动态 trigger 转折复测

`[实板测量]` 在 `vq=1.00 V`、`we=21.9911 rad/s`、默认 PWM/ADC map 下完成了
`512..3740` 的完整动态 trigger 曲线，并对 `1874/2300/2812/3740` 各重复两次。
结果显示：`1874` 两次峰值约 `0.012..0.017 A`，`2300` 两次约
`0.058/0.256/0.022 A` 和 `0.090/0.259/0.017 A`，`2812` 两次约
`0.237..0.252 A`（A/B），`3740` 两次约 `0.243..0.264 A`（A/B）。所有测试
均无 fault，VBUS 约 `11.87..11.93 V`。

这使采样窗口状态从“强敏感”升级为：

```text
ADC_MEASUREMENT_WINDOW=REPRODUCIBLY_TRIGGER_DEPENDENT
```

但该状态只描述 MCU ADC/CSA 观测，不等于真实相电流已知。当前禁止把低读数的
`1874` 当作“安全采样点”，禁止把高读数直接当作独立电流计结论，也禁止在没有
SOx/PWM/独立电流证据时修改默认 trigger。完整原始日志见：
[`dynamic trigger boundary repeats`](../../tmp/bench/20260830_104326_dynamic_trigger_boundary_repeats_1V_30rpm/)。

### 21.7 2026-08-30 静态 trigger 隔离与 1–2 V 完整窗口

`[实板测量]` 将 `we=0` 固定后，`adc_trig=1874/2300/2812/3740` 各重复两次，仍得到
低读数、转折、高读数平台三段形态：1874 约 `0.012..0.016 A`，2300 的 B 相约
`0.262..0.264 A`，2812/3740 的 A/B 约 `0.241..0.264 A`；VBUS `11.87..11.93 V`，
无 fault。该项已排除“旋转速度是触发转折必要条件”，但仍不能证明正确采样窗口。

`[实板测量]` 在 `we=21.9911 rad/s`、bench400 约 375 ms 有效窗口下完成 1.0 V 首轮、
1.0 V 重复、1.5 V 和 2.0 V：

| vq | 软件峰值 ia/ib/ic | theta_sim | theta_elec | VBUS |
| ---: | --- | --- | --- | --- |
| 1.00 V | 0.257/0.256/0.033 A | 3.395→6.115 | 3.398→4.789 | 11.88–11.93 V |
| 1.00 V repeat | 0.273/0.277/0.233 A | 4.807→1.240 | 4.807→5.751 | 11.87–11.92 V |
| 1.50 V | 0.407/0.435/0.420 A | 5.703→2.143 | 5.710→0.571 | 11.85–11.91 V |
| 2.00 V | 0.586/0.633/0.578 A | 0.468→3.220 | 0.462→2.244 | 11.84–11.91 V |

四项均无软件 fault，软件场角推进正常但未形成稳定机械同步。电流仍是 MCU
ADC/CSA 换算值；没有独立相电流和测速时，结果只能支持“存在滑差”，不能在采样链、
转矩裕量、机械负载、参数和功率级之间作唯一选择。

原始日志：[`static trigger boundary`](../../tmp/bench/20260830_105417_static_trigger_boundary_1V/)、
[`1 V`](../../tmp/bench/20260830_105810_voltage_full_window_1V_30rpm/)、
[`1 V repeat`](../../tmp/bench/20260830_105937_voltage_full_window_1V_30rpm_repeat/)、
[`1.5 V`](../../tmp/bench/20260830_110024_voltage_full_window_1p5V_30rpm/)、
[`2 V`](../../tmp/bench/20260830_110109_voltage_full_window_2V_30rpm/)。

当前准入结论不变：`ADC_MEASUREMENT_WINDOW=REPRODUCIBLY_TRIGGER_DEPENDENT`，
`OPEN_LOOP_ROOT_CAUSE_STATUS=UNRESOLVED`，`MOTOR_PHYSICAL_OPERATION=UNVERIFIED`，
`REAL_BENCH_TEST_STATUS=BLOCKED`。不得因 1874 的低读数选择默认触发点，也不得因 2 V
未触发软件故障而进入长时间运行。
