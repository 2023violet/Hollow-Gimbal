# FalconFOC V2｜NEXT-ENGINEERING-01 独立数学验证、热路径审计与结构评审

## 判定

`NEXT-ENGINEERING-01 PARTIAL PASS`

本轮完成了独立 Core 数学参考测试、Float32/ARM 热路径审计、防御性检查分类、V2 `main.c` 职责审查和完整 Host/ARM/sanitizer 回归。

没有发现已被独立参考确认的控制公式错误，也没有实施未经测量的算法优化或删除 Safety/事务检查。由于当前独立参考覆盖主要集中在 Clarke/Park/逆 Park/SVPWM，Current PI、PMSM 外环、Trajectory 和 Sensor 仍保留需要继续扩充的独立 oracle；V2 `main.c` 也没有在本轮强行拆分，因为当前安全锁、静态 IRQ 状态和初始化失败路径还没有足够低风险的行为等价证明。

因此本轮不能报告完整 `PASS`，但已经达到可以进入下一轮“针对具体函数的、带实测证据的优化”状态。

边界保持：

- `RC0 SOFTWARE GATES PASS`
- `G3-SAFE HARDWARE NOT VERIFIED`
- `G3-PWR LOCKED`
- `AT32F415 WCET NOT VERIFIED`
- 未刷机、未上电、未启用 PWM、未写 Flash、未 commit、未 push。

## 1. 基线和实际改动

工作树：`codex/falconfoc-v2-rc0`，HEAD `46bc963f5e5849e4440d1714a845fe485d338837`。既有未提交修改和未跟踪文件全部保留。

本轮新增的最小验证改动：

- `core/tests/test_foc_math_reference.c`
  - 独立 double 参考实现，不调用生产私有 helper 生成期望值；
  - 覆盖 Clarke、Park、逆 Park、SVPWM、非法 VBUS 安全输出；
  - 10,000 个确定性变换样本、10,000 个确定性 SVPWM 样本和失败样本；
- `core/CMakeLists.txt`
  - 将 `test-foc-math-reference` 纳入 Core CTest；
- 本证据文档和维护日志。

没有修改：

- `firmware/at32f415/src/main.c`；
- FOC 生产算法、PI、抗饱和、前馈、Runtime、Safety、Device、Board、Protocol；
- MCU、FPU、ABI、ADC、SPI、Timer、NVIC 或输出锁。

## 2. 独立数学参考验证

### 2.1 Clarke

当前代码和参考公式：

```text
alpha = a
beta  = (b - c) / sqrt(3)
```

单位保持输入单位，适用于相电流和相电压。当前实现是 amplitude-invariant Clarke，依赖物理相序和 `a/b/c` 正方向正确。

独立验证覆盖：随机三相输入、正负值、零序输入、边界有限值和非有限失败。

### 2.2 Park / 逆 Park

当前约定：

```text
d = alpha*cos(theta) + beta*sin(theta)
q = beta*cos(theta) - alpha*sin(theta)

alpha = d*cos(theta) - q*sin(theta)
beta  = d*sin(theta) + q*cos(theta)
```

单位保持不变；`theta` 为电角度 rad，正方向由编码器/转子映射决定。独立参考以 double `sin/cos` 计算期望值，生产实现以 Float32 `fmodf/sinf/cosf` 计算。

结果：10,000 个确定性样本通过。最大绝对差：

| 字段 | 最大绝对差 |
| --- | ---: |
| Park d | `4.54459617e-5` |
| Park q | `4.26009622e-5` |
| 逆 Park alpha | `3.81469727e-6` |
| 逆 Park beta | `3.81469727e-6` |

Park 的较大误差只出现在大角度规约与 Float32 三角函数和 double 参考不同的样本；没有发现符号、轴或周期错误。参考测试使用 `1e-4 * max(1, |actual|, |expected|)` 的明确 Float32/double 交叉验证门限，并保留最大误差输出，不能解释为逐位一致。

### 2.3 SVPWM

当前实现的数学步骤：

```text
alpha_n = alpha / VBUS
beta_n  = beta / VBUS
m       = sqrt(alpha_n² + beta_n²)
limit   = min(voltage_limit / VBUS, 2/3)
```

若 `m > limit`，按 `limit/m` 缩放矢量；然后：

```text
phase_a = alpha_n
phase_b = -0.5*alpha_n + sqrt(3)/2*beta_n
phase_c = -0.5*alpha_n - sqrt(3)/2*beta_n
common  = 0.5*(max(phase) + min(phase))
duty_i  = 0.5 + phase_i - common
```

最后执行 duty 限制并设置 `saturated`。Sector 只作为元数据，不改变 duty 核心生成。

独立 double 参考覆盖：四象限、低/高电压、矢量限幅、duty 限幅、随机有效矢量和非法 VBUS。

结果：`10,000/10,000` 通过；duty 与 saturation 无差异，sector 只在排除数学边界的随机样本上比较，已有带符号零边界测试继续通过。

### 2.4 Current PI 与前馈

当前离散模型：

```text
error      = target_dq - measured_dq
proportion = Kp * error
integral'  = integral + Ki * dt * error
request    = proportion + integral + feedforward
```

前馈：

```text
vd_ff = -omega_e * Lq * iq_target
vq_ff =  omega_e * (Ld * id_target + psi_f)
```

单位分别为 A、V、s、H、Wb、rad/s。当前实现先进行电流目标和 VBUS/电压矢量限制，再通过 SVPWM 反算实际 `applied_voltage_dq`，饱和时用：

```text
applied_voltage - proportional - feedforward
```

作为积分器准入边界，并在最终调制成功后提交积分器。

现有测试覆盖：正负误差、低 VBUS、电压和 duty 限制、抗饱和、目标反转、饱和解除、前馈开关、Ld/Lq 不同、速度正负、参数失配和失败不提交。

本轮没有新增独立 double PI oracle。需要明确的下一步是把积分器试算、矢量限幅和实际电压反算拆成独立参考模型；在此之前不改写 PI 或抗饱和实现。

### 2.5 Rotor mapping、Velocity、Position、Trajectory

当前基本关系：

```text
omega_e = pole_pairs * omega_mechanical
theta_e = wrap(pole_pairs * theta_mechanical + electrical_offset)
```

速度 PI 使用显式 `dt`，位置 P 输出速度参考，Trajectory 使用速度上限、加速度上限和制动速度：

```text
v_brake = sqrt(2 * a_max * abs(position_error))
v_target = sign(error) * min(v_max, v_brake)
```

现有 Host/PMSM/Runtime 测试覆盖正反速度、正反位置、轨迹限制、多实例、故障拒绝和不规则事件流；本轮没有新增独立 double 外环 oracle，也没有修改控制模型指标。该区域仍是独立数学验证盲区。

### 2.6 Sensor

当前 Sensor 已实现：整数累计解圈、方向映射、半圈边界、uint32 时间戳模差、dt 限制、最大速度、LPF、alpha-beta tracker 和异常样本锁存。现有 Sensor 测试覆盖回绕、反向、丢帧、无效时间戳、连续性丢失和滤波确定性。

本轮没有新增独立 double Sensor oracle；后续应独立重算 unwrap、LPF alpha 和 tracker 轨迹，以避免测试只复用生产公式。

## 3. ARM 热路径审计

当前 V2 ARM ELF：`.text=34,916 B`、`.rodata=1,352 B`、`.data=84 B`、`.bss=2,520 B`、保留栈 `1,024 B`，Flash BIN `36,736 B`。目标为 Cortex-M4、Thumb-2、soft-float、无 FPU。

### 3.1 H0 Current

`foc_current_step_internal` 静态大小约 `922 B`，静态栈帧 `184 B`。反汇编确认一个控制周期包含：

- `fmodf` 一次；
- `sinf` 一次；
- `cosf` 一次；
- `sqrtf` 经 current vector limit；
- `hypotf` 经调制电压检查；
- 多次 `__aeabi_*` 软件浮点调用；
- Park/逆 Park、Clarke、SVPWM、抗饱和二次调制；
- 饱和修正路径可能再次进入 `foc_current_modulate`。

这条路径每约 200 µs 运行一次，是当前最高优先级热点。PERF-01 已完成 sin/cos 复用和 sector 去 `atan2f`，本轮没有重复做同类优化。当前仍没有目标 MCU 周期实测，不能把 ARM 指令数量换算成 WCET。

### 3.2 H0 Runtime/Safety

`foc_runtime_h0_step` 静态大小约 `528 B`、静态栈帧 `96 B`，会执行 schedule、时间戳、angle age、Device decode、Safety 状态和 `foc_runtime_control`。这些检查大多在故障、时间有效性和状态所有权上有明确作用，不应按代码长度批量删除。

### 3.3 H1 Sensor / Motion

| 函数 | 静态大小 | 静态栈帧 | 主要成本 |
| --- | ---: | ---: | --- |
| `foc_encoder_update` | `1,028 B` | `72 B` | Float 除法、模差、LPF、tracker、多个质量分支 |
| `foc_runtime_h1_step` | `888 B` | `288 B` | snapshot、时间戳、rotor mapping、trajectory、velocity/position PI |
| `foc_trajectory_velocity_step` | `196 B` | 32 B | clamp、乘法和有限值判断 |
| `foc_trajectory_position_step` | `280 B` | 40 B | `sqrtf` 制动速度、clamp、有限值判断 |

H1 频率低于 H0，但 `foc_encoder_update` 的 float division 和 `foc_trajectory_position_step` 的 `sqrtf` 是可量化候选。未取得实际 MCU cycle counter 前，不进行近似替换。

### 3.4 H0/H1 调用与浮点符号

当前 ELF 含 `fmodf`、`sinf`、`cosf`、`sqrtf`、`hypotf`、`fminf`、`fmaxf` 和软件浮点库；没有 `atan2f`。公共 API 和非 H0 路径仍保留这些符号，不能根据整份 ELF 是否含某符号直接判定热路径失败。

## 4. 防御性编程分类

### A｜初始化阶段必要检查

- `foc_current_init`：PWM config、PI gain、current limit、duty center 合法性；只执行初始化，保留。
- `foc_encoder_init`：counts、方向、dt、速度上限、滤波参数；只执行初始化，保留。
- `foc_trajectory_init`：速度/加速度/dt/tolerance；只执行初始化，保留。
- `foc_board_init`：Runtime/Pipeline/Safety 配置；初始化失败必须禁止 producer，保留。

### B｜每周期必要动态检查

- ADC/VBUS/dt/angle 的 finite 和范围；防止非法样本进入 Core。
- Sensor timestamp freshness、连续性和速度上限；防止错误编码器样本推进状态。
- Safety heartbeat、command timeout、fault latch 和 output gate；属于安全契约，不优化删除。
- Device/Safety 最终提交前检查；防止 PI 状态在输出拒绝后推进。

### C｜可证明的重复检查

- Public `foc_svpwm()` 的完整 config validation 与 current context 内 trusted path 的配置复核已经分开；这是当前唯一有明确所有权契约的重复检查优化。
- Current angle cache 在同一周期内复用三角函数；不是跨周期共享状态，保留事务边界。
- `foc_runtime_h0_step` 中多个 timestamp 检查看似重复，但分别对应 ADC、angle 和 sensor source time，不能凭代码相邻就删除。

### D｜暂未证明可删除

没有发现可以在本轮安全删除的防御性检查。编译器已经优化的检查必须通过反汇编证明；当前软件浮点和 `isfinite` 调用成本真实存在，但 Safety/错误语义的收益更高。

## 5. V2 `main.c` 结构审查

当前文件职责可以画成：

```text
main
  ├─ output lock / clock / ADC / SPI / timers / UART / NVIC init
  ├─ runtime + serial lifetime
  └─ foreground polling

IRQ entry points
  ├─ TMR1_CH_IRQHandler -> ADC trigger
  ├─ ADC1_IRQHandler -> ADC read -> H0
  ├─ TMR5_GLOBAL_IRQHandler -> SPI transaction window -> H1
  ├─ SPI1_IRQHandler -> receive word
  └─ USART1_IRQHandler -> UART ring
```

当前 `main.c` 虽然较长，但平台初始化和 IRQ 逻辑已通过静态 helper 分区，应用 Runtime 不拥有寄存器。更激进的拆分需要迁移 `runtime`、`output`、`adc`、`encoder`、`spi_phase`、`spi_word`、`spi_rx_us` 和 `uart` 的静态所有权；如果仅把函数搬到另一个 C 文件，容易改变链接可见性、初始化失败路径或中断共享状态。

本轮没有实施拆分，原因是：

1. 当前没有行为问题或测量证据证明文件拆分会改善实时性；
2. 初始化失败路径必须保持“关中断、输出锁、失败后 WFI”；
3. IRQ 入口名称和静态事务状态需要逐项二进制/寄存器等价复核；
4. 将平台函数抽到新文件不能减少 H0 的指令成本。

后续若实施，只建议把“无共享状态的纯初始化函数”分组到 `foc_at32_platform.c`，将 IRQ 事务状态显式封装为一个平台上下文，并先做符号、启动顺序和失败路径差分。当前不具备必要性，故不执行。

## 6. Magic Numbers / 配置可读性

本轮只审查，不改变数值。主要发现：

| 位置 | 数值类别 | 当前判断 |
| --- | --- | --- |
| `foc_board.h` | H0 `200 us`、H1 `500 us`、150 MHz、1 µs epoch | 有字段名和注释，属于 Board1 policy；后续应与 platform timing seam 统一 |
| `foc_board.c` | ADC frame `80 us`、encoder frame `100 us`、gap `2500 us` | 有宏名，但应补充来源/测量状态 |
| `foc_board.c` | PI、position、velocity、trajectory 数值 | 属于名义参考配置，不能表述为真实电机已验证 |
| `foc_safe` 配置 | timeout、heartbeat、invalid threshold | 属于 Safety 门槛，必须保留来源和单位，不应散落替换 |
| `foc_encoder.c` | 2π、π、half-range | 数学常量，当前宏可读且不应过度抽象 |
| `main.c` | ADC/SPI 时间窗、poll limit、IRQ flags | 硬件事务常量，建议未来集中在 Board timing contract，但本轮不移动 |

没有批量把 `0U/1U/2.0f` 抽成宏，也没有改变 PI、ADC、SPI、Timer 或安全阈值。

## 7. 回归结果

本轮实际通过：

| 检查 | 结果 |
| --- | --- |
| 独立数学参考 | `test-foc-math-reference` PASS，变换/ SVPWM 各 10,000 样本 |
| Core CTest | `11/11 PASS` |
| Clang ASan/UBSan | Core `11/11`，全工程合计 `29/29 PASS` |
| Host Debug | `45/45 PASS` |
| V2 ARM build | PASS；ELF/BIN/HEX/MAP 生成 |
| Legacy tiny/standard/full-diag build | PASS |
| Source contract | PASS，62 C/H、8 CMake、8 README |
| C/H formatter | PASS |
| `Scope falconfoc` | PASS |
| `Scope all` | PASS |
| `git diff --check` | PASS，仅既有 LF/CRLF warning |

SYSTEM replay 和 long replay 仍保持 RC0 证据中的 `3f01f6e3` 与 `49dea950`；本轮没有修改 Runtime，因此没有重生成新的 replay 语义。

## 8. 下一阶段最值得优化的三个函数

按“实际热度 × 可量化收益 × 正确性风险”排序：

1. `foc_current_step_internal`
   - H0 每周期运行；软件浮点 `fmodf/sinf/cosf/sqrtf/hypotf` 和可能的二次调制最集中。
   - 下一步必须使用 DWT/目标板 cycle counter 或等价 ARM 测量，先验证三角函数和重复 `isfinite` 的真实占比。
   - 不能直接替换近似函数，不能删除 anti-windup 或 Safety 检查。
2. `foc_encoder_update`
   - H1 每周期运行，包含多次 float division、LPF、tracker 和质量分类。
   - 先测有效样本/异常样本两条路径，再决定是否缓存常量或改用乘法倒数；必须保持 timestamp wrap 和 invalid 状态语义。
3. `foc_runtime_h0_step`
   - H0 外层包含时间戳、Device decode、Safety gate、heartbeat、publication 和事务提交。
   - 只有在调用图和故障注入覆盖后，才能考虑把初始化期检查从可信热路径移出；不得合并或删除安全检查。

Trajectory position 的 `sqrtf` 是 P2 候选，当前 H1 频率和正确性风险低于上述三项。

## 9. 未解决事实和停止点

- 没有 AT32F415 真实 cycle count、WCET 或运行栈高水位；
- 没有真实 ADC/SPI/PWM 事务时延和采样窗口测量；
- 没有实施 `main.c` 文件拆分，因此没有新的初始化二进制差分；
- Current PI、外环、Trajectory、Sensor 的独立 double oracle 尚未完整建立；
- 没有替换 Legacy sin/cos、没有引入 Q31/Q15、没有修改控制模型或硬件时序。

## 最终状态

`NEXT-ENGINEERING-01 PARTIAL PASS`

原因不是发现了已确认的控制错误，而是独立参考覆盖和安全的主程序结构重构证据仍不完整。下一轮应优先完成目标板 cycle measurement 和 Current PI/外环独立 oracle，再决定是否实施任何热路径改动。

/* ---------------------------------------- end of file ----------------------------------------- */
