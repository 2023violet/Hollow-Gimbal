# FalconFOC V2｜NEXT-CONTROL-01 Float32 电流环前馈与动态验证

## 判定

`NEXT-CONTROL-01 ALGORITHM PASS / INTEGRATION BLOCKED`

算法、Core 可选入口、抗饱和组合、Host PMSM 模型和软件回归均通过；正式 Runtime/Board 集成保持阻断，因为当前 H0 到 `foc_current_input` 的公共边界只提供电角度，不提供与 ADC 电流采样同一时刻、经过质量确认的电角速度。没有虚构速度接入 Board1，也没有修改 Runtime 调度、Safety、Board 或功率输出。

本轮没有刷机、上电、启用 PWM、写入 Flash、提交或推送。

## 现有控制数学与数据流核查

当前 PI 输出单位是 V：`Kp [V/A] * current error [A] + integral [V]`。PI context 的 `integral_v` 保存 d/q 轴电压贡献，最终经 SVPWM、VBUS/电压向量限制、duty 限制和电压反算后进行积分器准入。

当前 Park 约定：

```text
d = alpha*cos(theta) + beta*sin(theta)
q = beta*cos(theta) - alpha*sin(theta)
```

现有 Runtime 数据流为：

```text
H1 mechanical position/speed
  -> foc_rotor_electrical()
       -> electrical angle + electrical speed
  -> H1 sensor slot
  -> H0 angle prediction
  -> Pipeline current input currently carries theta_elec_rad, phase currents, VBUS, dt
  -> foc_current_step()
```

H1 的 `foc_electrical_state.velocity_rad_s` 是电角速度，但当前 H0/Pipeline 到 `foc_current_input` 的接口没有携带这个同一控制时刻的速度。将机械速度重新乘极对数也不能自动解决源时间对齐和预测时刻问题，因此本轮不把它隐式接入前馈。

## 前馈模型与符号

本轮实现的候选模型是：

```text
vd_ff = -omega_e * Lq * iq_target
vq_ff =  omega_e * (Ld * id_target + psi_f)
```

参数单位：

- `Ld`, `Lq`: H；
- `psi_f`: Wb；
- `omega_e`: signed electrical rad/s；
- `id/iq`: A；
- 输出 d/q 电压：V。

正向 `omega_e=100 rad/s`、`Ld=0.001 H`、`Lq=0.0015 H`、`psi_f=0.05 Wb`、`id=1 A`、`iq=2 A` 的独立结果：

```text
vd_ff = -0.3 V
vq_ff =  5.1 V
```

反向速度符号同时反转两项；零速度时两项均为零。公式与当前 Park、电流正方向和 PMSM d/q 电压模型一致。

## 实际代码修改

新增到 `core/include/foc_current.h` 的公共边界：

- `foc_current_feedforward_model`；
- `foc_current_feedforward_context`；
- `foc_current_feedforward_output`；
- `foc_current_feedforward_init/reset/compute()`；
- `foc_current_step_with_feedforward()`。

`foc_current_step()` 保持原入口，内部走原有无前馈路径。前馈关闭 context 的 `foc_current_step_with_feedforward()` 与原 `foc_current_step()` 对相同输入逐项一致。

`foc_current_step_with_feedforward()` 在目标电流限幅后计算前馈，并将：

```text
PI proportional + PI integral + feedforward
```

作为总请求送入原有 `foc_current_modulate()`。前馈没有绕过 SVPWM 或 VBUS/duty 限制。

抗饱和时积分器准入使用：

```text
applied_voltage - proportional_voltage - feedforward_voltage
```

因此最终可用电压中的前馈部分不会被错误计入 PI 积分器。失败时输出清零，积分器不提交。

## 配置与失败语义

- 前馈关闭时允许空模型，不强制填写电机参数；
- 前馈启用时要求 `Ld > 0`、`Lq > 0`、`psi_f >= 0` 且全部有限；
- 启用时 `omega_e`、目标电流必须有限；
- 非法模型、NaN/Inf、未初始化 context 返回确定错误；
- 前馈关闭不执行补偿乘法，也不改变旧状态事务。

## 数值和状态测试

新增 `core/tests/test_foc_feedforward.c`，覆盖正负速度、零速度、正负 d/q 目标、各向同性/异性电感、磁链项、非法模型、NaN/Inf、前馈关闭逐项等价、电压限幅、积分器抗饱和和失败不提交。`test_invalid_speed_is_transactional()` 明确验证非法 `omega_e` 会清零输出且不提交控制器状态。

旧 PERF-01A Golden 使用当前无前馈入口重新生成到独立文件，对照结果：

```text
records=4126
failures=0
max_abs_float_difference=7.39999999888e-08
```

原始 Golden 没有被覆盖。

## PMSM 模型对照

新增 Host 合成 PMSM 电气模型，显式包含：

```text
vd = Rs*id + Ld*did/dt - omega_e*Lq*iq
vq = Rs*iq + Lq*diq/dt + omega_e*(Ld*id + psi_f)
```

控制器、PI 参数、VBUS、dt、电压限幅和目标轨迹保持一致。模型使用最终反算 d/q 电压作为实际施加电压。

### 正向恒速 `omega_e=+100 rad/s`

```text
PI only: RMSE d=1.4291e-07, q=9.536743e-07, saturated=0
PI + FF: RMSE d=3.491163e-08, q=0, saturated=0
```

### 反向恒速 `omega_e=-100 rad/s`

```text
PI only: RMSE d=1.16216e-07, q=9.536743e-07, saturated=0
PI + FF: RMSE d=2.06767e-08, q=0, saturated=0
```

### 参数失配

将 `Ld +20%`、`Lq -20%`、`psi_f -20%` 后：

```text
PI + mismatched FF: RMSE d=3.518598e-08, q=4.768372e-07
saturated=0, max_error=2.999999 A
```

该测试工况中 PI 本身已经足够强，参数失配没有造成明显恶化；这不能推广为所有电机和所有速度。前馈对电机参数误差敏感，真实参数和实时速度质量仍是正式启用的前置条件。

### 饱和与恢复

在 `omega_e=500 rad/s`、VBUS 24 V、voltage limit 4 V 的高反电动势工况：

```text
500/500 cycles saturated
final integral_q=0
```

前馈和 PI 一起限幅，积分器没有无界增长；随后关闭目标并将速度置零，积分器恢复到有限范围。

## Host 性能

同一固定输入、200,000 次控制调用、相同 Clang Host 构建条件：

```text
feedforward disabled: 0.160 us/step
feedforward enabled: 0.205 us/step
```

补充重复运行的观测范围为关闭 `0.140--0.145 us/step`、启用 `0.150--0.165 us/step`；Windows `clock()` 分辨率和调度会造成抖动，因此保留上面的同条件记录作为本轮报告值。这些是当前 Windows Host 的相对测量，不是 AT32F415 周期或 WCET。前馈关闭仍执行原路径；启用后增加有限乘加和一次可选计算，未增加三角函数、动态内存或全局共享状态。

## AC01–AC18

| AC | 结果 | 证据 |
| --- | --- | --- |
| AC01 前馈关闭与旧版一致 | PASS | 独立 Golden 4126/4126，0 failures |
| AC02 零速度前馈为零 | PASS | 独立公式测试 |
| AC03/04 正反电角速度 | PASS | 正负速度公式和 PMSM 模型 |
| AC05/06 d/q 目标 | PASS | 正负 d/q 目标组合 |
| AC07/08 各向同性/异性电感 | PASS | `Ld=Lq`、`Ld!=Lq` 参数覆盖 |
| AC09 磁链项 | PASS | q 轴反电动势公式测试 |
| AC10 正负组合 | PASS | 独立方程和模型测试 |
| AC11/12 非法参数与 NaN/Inf | PASS | 初始化和计算拒绝 |
| AC13/14 电压/duty 限幅 | PASS | 低母线、高速饱和模型 |
| AC15 抗饱和恢复 | PASS | 500 周期饱和、速度/目标恢复 |
| AC16 多实例 | PASS | 独立 controller/feedforward context |
| AC17 失败状态不提交 | PASS | 输出清零、PI state unchanged |
| AC18 速度无效/过期拒绝 | PASS | enabled path 对 NaN/非有限 omega 拒绝 |

## Sanitizer 与完整回归

Host Clang ASan/UBSan 实际结果：

```text
Core         9/9 PASS
Sensors      1/1 PASS
Calibration  1/1 PASS
G3-NP        2/2 PASS
G3-SAFE      1/1 PASS
Runtime      6/6 PASS
Protocol     1/1 PASS
Board        3/3 PASS
Total       24/24 PASS
```

其余软件回归：

- Core CTest：`9/9 PASS`；
- Runtime/Pipeline：`6/6 PASS`；
- Sensors：`1/1 PASS`；
- Calibration：`1/1 PASS`；
- G3-NP：`2/2 PASS`；
- G3-SAFE：`1/1 PASS`；
- Board/UART/Python：`3/3 PASS`；
- Protocol：`1/1 PASS`；
- Legacy Host：`45/45 PASS`；
- Legacy Bench：PASS；
- FalconFoc source contract、C/H formatter、scoped quality gate、`git diff --check`：PASS。

`Scope all` 只读复核仍在既有 `g3/np/CMakeLists.txt` 的 cmake-format 差异处失败；本轮没有格式化该历史文件，也没有把全仓门禁失败包装成 NEXT-CONTROL-01 通过。新增和本轮修改的 standalone CMake 文件已单独通过严格 formatter 检查。

## ARM 资源

AT32F415 V2 构建通过。由于前馈 API 被保留为可选 Core 入口、没有从 Runtime/Board 调用，生产输出锁和现有 H0/H1 数据流未改变。

当前 ARM 产物：

```text
.text=33436 B
.rodata=1312 B
.data=84 B
.bss=2392 B
linker RAM=3504 B
```

相对 Sensor-01 构建：`.text +360 B`，静态 RAM 不变。关键静态栈帧：

```text
foc_current_step_internal = 184 B
foc_current_modulate      = 56 B
foc_current_step wrapper   = 16 B
foc_svpwm_trusted          = 48 B
```

Host 与 ARM 静态数据都不能替代真实 MCU WCET、H0 时序或栈高水位测量。

## 集成状态与未验证边界

算法和 Core 可选入口已经通过，但正式 Runtime/Board 集成保持阻断：

- H0 的 `foc_current_input` 没有携带与 ADC 采样同一时刻的可信电角速度；
- H1 的电气速度位于传感器发布/预测边界，尚未定义 H0 消费时刻的速度快照契约；
- 本轮没有重复应用编码器方向，也没有使用 CAL-01 合成参数覆盖 Board1；
- 没有真实电机参数、真实磁链、电感、电阻、速度延迟或参数温漂证据；
- 没有真实功率输出、刷机、上电或电机运行。

因此本轮最终状态不是完整 `SOFTWARE PASS`，而是：

`NEXT-CONTROL-01 ALGORITHM PASS / INTEGRATION BLOCKED`

后续若要正式启用，需要单独定义同一 ADC 控制时刻的电角速度快照、质量/超时语义、参数匹配和 Runtime 事务边界，再进行独立集成评审。

/* ---------------------------------------- end of file ----------------------------------------- */
