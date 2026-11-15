# FalconFOC V2｜NEXT-CONTROL-ROBUST-01 控制稳定性与鲁棒性研究

日期：2026-11-14  
分支：`codex/falconfoc-v2-rc0`  
HEAD：`46bc963f5e5849e4440d1714a845fe485d338837`

## 最终判定

`NEXT-CONTROL-ROBUST-01 PASS（Host 数学/仿真研究范围）`

本轮新增一个小型、确定性的 Host 研究入口 [test_foc_robustness.c](D:/study/Software/Hollow-Gimbal/runtime/tests/test_foc_robustness.c)，使用生产 Float32 Current、Velocity、Position、Trajectory 和 Feedforward 接口，测试侧独立实现 double PMSM 电气/机械模型与 RK4 积分。没有修改生产控制算法、Board 默认参数、Runtime/Safety 行为或硬件路径。

这个 PASS 只代表定义的 Host 研究范围完成，不代表真实电机参数已经辨识，也不代表 AT32F415 的实际延迟、WCET、功率级稳定性或带载运行已经验证。

## 控制结构与执行契约

```text
Position Target
      ↓
Trajectory Planner（可选）
      ↓
Position P / Velocity Reference
      ↓                 H1 = 500 us
Velocity PI             ↓
      ↓ iq target       ↓ electrical angle/speed snapshot
      └──────────────→ H0 = 200 us
                         ↓
              Current PI + optional PMSM feedforward
                         ↓
                 voltage limit / anti-windup
                         ↓
                       SVPWM
                         ↓
                 independent double RK4 PMSM
                         ↓
              encoder quantization / delay / noise
```

研究入口每 `100 us` 推进一次时间基准，在 `200 us` 边界执行 H0，在 `500 us` 边界执行 H1。H0 只消费最近一次 H1 结果；传感器延迟从测试侧历史状态中取样，不把 Host 函数执行时间当作 ADC/PWM 延迟。

## 参数与来源

| 参数 | 研究名义值 | 来源/身份 | 说明 |
| --- | ---: | --- | --- |
| Rs | `0.30 Ω` | Host 合成模型 | 不是实测值 |
| Ld | `1.0 mH` | Host 合成模型 | 扫描 `0.5x/1.5x` |
| Lq | `1.5 mH` | Host 合成模型 | 扫描 `0.5x/1.5x` |
| ψf | `0.05 Wb` | Host 合成模型 | 扫描 `0.8x/1.2x` |
| 极对数 | `4` | 研究模型 | 不代表 Board1 已校准结果 |
| J | `0.002 kg·m²` | Host 合成模型 | 扫描 `0.25x/4x` |
| B | `0.006 N·m·s/rad` | Host 合成模型 | 组合工况改变 |
| Current Kp/Ki | `{1.08,1.35}` / `{540,540}` | 当前 Core 配置 | 本轮不修改 |
| Current limit | `2.5 A` | 当前 Core 配置 | 本轮不修改 |
| Voltage limit | `12 V` | 当前 Core 配置 | VBUS 研究值 `24 V` |
| Velocity Kp/Ki | `0.18 / 1.4` | 当前 Runtime 研究配置 | 本轮不修改 |
| Position Kp | `4.0 s⁻¹` | 当前 Runtime 研究配置 | 本轮不修改 |
| 速度限制 | `12 rad/s` | 当前 Runtime 研究配置 | 本轮不修改 |
| H0/H1 | `200 / 500 us` | 本轮研究契约 | 不是实测 ISR 时序 |

这些值中 Rs、Ld、Lq、ψf、J、B、VBUS 和负载均属于 Host 假设；当前仓库没有可以把它们标记为真实电机测量值的证据。

## 独立 PMSM 模型

测试侧使用：

\[
T_e=\frac32p[\psi_f i_q+(L_d-L_q)i_di_q]
\]

\[
J\dot{\omega}_m=T_e-T_L-B\omega_m
\]

电气状态使用：

\[
\dot{i}_d=\frac{v_d-R_si_d+\omega_eL_qi_q}{L_d}
\]

\[
\dot{i}_q=\frac{v_q-R_si_q-\omega_e(L_di_d+\psi_f)}{L_q}
\]

每个 H0 周期使用独立 double RK4；生产控制器仍然使用现有 Float32 API。测试侧没有复制生产 PI、Park、SVPWM 或 Runtime 实现作为期望值。

模型没有包含逆变器死区、开关器件压降、PWM 更新延迟、ADC 放大器误差、采样保持、机械齿槽、摩擦非线性或功率级保护动作。这些是后续硬件验证问题。

## 理论稳定性参考

在不考虑交叉耦合、限幅和延迟的局部 RL 近似下：

\[
G_i(s)=\frac1{Ls+R}
\]

当前 PI 的连续时间近似特征式为：

\[
Ls^2+(R+K_p)s+K_i=0
\]

名义 d 轴约为 `ωn=735 rad/s、ζ=0.94`，q 轴约为 `ωn=600 rad/s、ζ=0.92`。速度环在理想电流环、`K_t=1.5pψf=0.30 N·m/A` 的近似下约为 `ωn=14.5 rad/s、ζ≈1.03`。

这些是单工作点、连续时间、无延迟和无饱和的局部分析，不能推广为完整离散闭环稳定性证明。实际结果必须结合 H0/H1 采样、量化、延迟、饱和和 anti-windup 仿真解释。

## 22 个确定性工况结果

CSV 输出保存在忽略的构建证据目录：`build/rc0/robustness-runtime/robustness.csv`。每行包含 RMSE、末四分之一时间段 RMSE、峰值误差、最大 iq、电压幅值、转矩 RMS/峰值、饱和周期、拒绝样本和稳定标志。

代表性结果如下：

| 工况 | RMSE | 尾段 RMSE | 最大 iq | 饱和周期 | 结论 |
| --- | ---: | ---: | ---: | ---: | --- |
| Current nominal | `0.0201 A` | `0.00112 A` | `1.04 A` | `0` | 稳定 |
| Velocity nominal | `0.765 rad/s` | `5.0e-6` | `1.47 A` | `0` | 启动瞬态后稳定 |
| Velocity + load `0.08 N·m` | `0.851` | `9.75e-6` | `1.49 A` | `0` | 负载扰动可恢复 |
| Position direct | `0.522 rad` | `0.00257 rad` | `1.47 A` | `0` | 稳定 |
| Position + trajectory | `1.706 rad` | `0.0140 rad` | `0.31 A` | `0` | 轨迹更保守，尾段误差较大 |
| Ld/Lq `0.5x` | `0.291 A` | `0.337 A` | `1.49 A` | `7428` | 明显电流退化/饱和 |
| Ld/Lq `1.5x` | `0.294 A` | `0.339 A` | `1.64 A` | `7433` | 明显电流退化/饱和 |
| J `4x` | `1.498` | `0.0123` | `1.55 A` | `0` | 速度尾段变差 |
| Rs/Lq/J 组合 | `1.694` | `0.0195` | `1.72 A` | `0` | 当前外环鲁棒性较差 |
| 角度偏差 `0.03 rad` | `0.801` | `9.11e-6` | `1.47 A` | `0` | 本模型下速度尾段仍稳定 |
| 速度噪声 `0.25 rad/s` | `0.801` | `0.0176` | `1.47 A` | `0` | 尾段误差明显增加 |
| 传感器延迟 `400 us` | `0.795` | `1.00e-5` | `1.49 A` | `0` | 本模型下仍稳定 |
| 丢样 + Feedforward | `0.786` | `1.26e-5` | `1.51 A` | `0` | 444 个 H1 样本跳过，研究模型保持旧目标 |
| 随机组合 B | `1.149 A` | `1.185 A` | `1.50 A` | `9436` | 电流环严重饱和，未发散 |

所有 22 个工况均为有限状态，`unstable_cases=0`。这不是“所有参数都安全”的证明；它只说明当前选定的扫描点没有触发测试定义的发散条件。

## 关键判断

### Current PI

名义模型下电流环响应正常，但 Ld/Lq 变化到 `0.5x/1.5x` 时，电流 RMSE 从 `0.0201 A` 上升到约 `0.29 A`，并出现约 `7,400` 个饱和周期。当前 Kp/Ki 对电感不确定性并不鲁棒，电压限制和 anti-windup 成为主导非线性。

### Velocity/Position

名义速度环的尾段误差很小；J 增大到 `4x` 后尾段 RMSE 约 `0.0123`，Rs/Lq/J 组合约 `0.0195`。直接 Position P 的尾段误差约 `0.00257 rad`，启用 Trajectory 后约 `0.0140 rad`，说明当前轨迹约束更保守，但在该模型中降低了位置收敛速度。

### Sensor 与延迟

固定角度偏差和 `400 us` 延迟在当前低负载速度案例中没有引起发散，但速度噪声的尾段误差明显增大到 `0.0176`。这不能归结为单纯滤波不足：量化、噪声、延迟和外环积分分别作用于不同路径。本轮的丢样工况采用“研究模型保持上一 H1 目标”的假设，不替代 Runtime Safety 的真实失效语义。

## 最多三项优化候选

1. **Current PI 参数随辨识的 Rs/Ld/Lq 调度。** 以目标离散带宽为约束，按 `Kp≈Lωc-R`、`Ki≈Rωc` 形成候选表，并在电压限幅和 anti-windup 下重新验证。当前没有足够真实电机参数，不能直接改 Board1 默认值。
2. **大惯量/负载下的速度环鲁棒整定。** 对 J `0.25x～4x`、负载阶跃和 Rs/Lq 组合重新选择外环带宽或加入受限的负载/加速度前馈；必须先证明电流环裕量，不应单独提高速度 PI 增益。
3. **Position/Trajectory 交接策略。** 当前 Trajectory 的尾段位置误差大于直接 Position P。应研究“轨迹速度上限、位置误差门限、速度 PI 积分清零/冻结”的交接契约，再决定是否调整轨迹参数；本轮不改默认轨迹配置。

这些是 Host 候选，不是实机部署参数。没有 J、B、负载和真实 ψf 的测量证据前，不能把它们写入 Board1 默认配置。

## 回归与边界

- 新增 Runtime CTest：`9/9 PASS`，包括 `test-foc-robustness`。
- Clang ASan/UBSan：Core `14/14`、Sensors `2/2`、Calibration `1/1`、G3-NP `2/2`、G3-SAFE `1/1`、Runtime `9/9`、Protocol `1/1`、Board `4/4`，合计 `34/34 PASS`。
- Host Debug：`45/45 PASS`。
- SYSTEM replay：`3f01f6e3`。
- Long replay：`49dea950`。
- Source contract：`67 C/H、8 CMake、8 README PASS`。
- C/H formatter、CMake formatter `29/29`、`Scope falconfoc`、临时接通已有 Bash 后的 `Scope all`、`git diff --check`：PASS。
- V2 AT32F415 ARM 构建：PASS；本轮研究测试未链接进 ARM 固件，ARM 资源无解释变化。

本轮没有修改 FOC 数学、PI 参数、Runtime/Safety FSM、Board 输出、ADC/SPI/PWM/Timer、CPU/FPU/ABI，也没有刷机、上电、运行电机或写 Flash。

仍保持：`G3-SAFE HARDWARE NOT VERIFIED`、`G3-PWR LOCKED`、`AT32F415 WCET NOT VERIFIED`、`MOTOR PHYSICAL OPERATION UNVERIFIED`。

