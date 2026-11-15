# FalconFOC V2｜NEXT-CONTROL-OPT-01 电流环退化根因与参数候选

日期：2026-11-14  
分支：`codex/falconfoc-v2-rc0`  
HEAD：`46bc963f5e5849e4440d1714a845fe485d338837`

## 最终判定

`NEXT-CONTROL-OPT-01 PASS（Host 研究范围）`

本轮完成了 Ld/Lq 退化复现、逐 H0 状态记录、独立电压可实现性分析、Ld/Lq 单独/组合对照、PI/Feedforward A/B、抗饱和状态观察和 Host 候选验证。

唯一保留的改进候选是：**在电机参数已经通过配置准入后，Host 研究配置启用参数一致的 d/q 电压 Feedforward**。它在固定高速、固定负载的对照中显著降低电流误差；本轮没有把它接入 Board1 默认配置，也没有修改生产代码。

## AC01｜当前控制与模型身份

当前研究名义参数：

| 参数 | 值 | 身份 |
| --- | ---: | --- |
| Rs | `0.30 Ω` | Host 合成 Plant 参数 |
| Ld/Lq | `1.0/1.5 mH` | Host 合成 Plant 参数 |
| ψf | `0.05 Wb` | Host 合成 Plant 参数 |
| 极对数 | `4` | Host 研究参数 |
| J/B | `0.002 kg·m² / 0.006 N·m·s/rad` | Host 合成机械参数 |
| VBUS | `24 V` | Host 研究条件 |
| Current Kp/Ki | `1.08/1.35 V/A`, `540/540 V/(A·s)` | 当前 Core 配置 |
| Current limit | `2.5 A` | 当前 Core 配置 |
| Voltage limit | `12 V` | 当前 Core 配置 |
| H0 | `200 µs` | 本轮研究调度 |
| H1 | `500 µs` | 本轮研究调度 |

上述 Rs、Ld、Lq、ψf、J、B、VBUS 都是 Host 假设，不是实际电机辨识结果。Feedforward 默认状态仍由调用方控制，Board1 默认并未被本轮打开。

## AC02｜退化的真实复现

研究入口为 [test_foc_robustness.c](D:/study/Software/Hollow-Gimbal/runtime/tests/test_foc_robustness.c)，构建输出为：

`build/rc0/robustness-runtime/robustness.csv`

逐 H0 记录另存为：

`build/rc0/robustness-runtime/robustness-trace.csv`

Trace 字段包括：时间戳、Plant id/iq、id/iq 目标、电角速度、PI+积分+Feedforward 请求电压、实际 d/q 施加电压、Feedforward d/q、积分状态、饱和标志、稳态电压需求和电压裕量。

关键复现结果：

| 工况 | RMSE | 饱和周期 | 首次饱和 | 最长连续饱和 | 最大请求电压 | 最小稳态裕量 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Nominal Current | `0.0201 A` | `0` | — | `0` | `10.30 V` | `+1.72 V` |
| Ld/Lq `0.5x` | `0.2908 A` | `7428` | `5116` | `7422` | `12.50 V` | `-0.071 V` |
| Ld/Lq `1.5x` | `0.2937 A` | `7433` | — | — | `12.78 V` | `-0.087 V` |

分母是约 `10,000` 个 H0 周期，研究窗口为 `4 s`。此前报告中的“约 7,400 个饱和周期”与本轮复现一致，且现在有了首次饱和、最长连续饱和和完整窗口占比。

## AC03｜物理电压可实现性

测试侧独立计算：

\[
v_d=R_si_d+L_d\frac{di_d}{dt}-\omega_eL_qi_q
\]

\[
v_q=R_si_q+L_q\frac{di_q}{dt}+\omega_e(L_di_d+\psi_f)
\]

并用稳态近似计算：

\[
v_{d,ss}=R_si_d-\omega_eL_qi_q,
\quad
v_{q,ss}=R_si_q+\omega_e(L_di_d+\psi_f)
\]

当前 Current 退化工况的目标为 `iq=1.5 A`、无负载，电机持续加速。到仿真后段，电角速度约 `231.6 rad/s`，稳态电压需求约 `12.06 V`，而软件当前有效电压边界为 `12 V`。因此至少有一部分饱和是**物理不可实现**，不是 PI 额外制造的电压。

对照验证了这一点：将同一 Ld/Lq `0.5x` Plant 固定在约 `20 rad/s`、用约 `0.33 N·m` 负载抵消电磁转矩后，稳态电压裕量恢复到 `+7.55 V`，饱和周期为 `0`，电流 RMSE 为 `0.072 A`，尾段 RMSE 为 `2.15e-4 A`。

结论：原始 0.29 A 退化由两部分组成：

1. 目标电流驱动机械速度上升，导致反电动势和电感项最终消耗完电压裕量。
2. 长时间饱和后，当前 PI 在失配 Plant 上的动态响应变差，但不是首要的物理电压不足原因。

## AC04｜Ld/Lq 单独与组合影响

当前扫描覆盖：

- Ld 单独 `0.5x/1.5x`。
- Lq 单独 `0.5x/1.5x`。
- Ld/Lq 同时 `0.5x/1.5x`。
- Rs、ψf、J、B 变化及固定种子组合。

在无速度约束的 Current 退化工况中，Ld/Lq 同时变化的 RMSE 都约 `0.29 A`，饱和比例约 `74%`，因为最终主导项是反电动势和可用电压边界。固定速度对照显示，同样 Ld/Lq 偏差并不会自动导致饱和，说明不能把全部退化归咎于电感失配。

## AC05｜PI 与 Feedforward A/B

### PI 候选

采用 `Kp=Lωc`、`Ki=Rωc` 作为 Host 研究起点，使用 `ωc=1000 rad/s`：

- Ld/Lq `0.5x` 候选：`Kp={0.5,0.75}`、`Ki={300,300}`。
- Ld/Lq `1.5x` 候选：`Kp={1.5,2.25}`、`Ki={300,300}`。

结果：

| 工况 | 当前默认 | PI 候选 | 判断 |
| --- | ---: | ---: | --- |
| Ld/Lq `0.5x`，无速度约束 RMSE | `0.2908 A` | `0.3223 A` | 变差 |
| Ld/Lq `0.5x`，固定速度 RMSE | `0.0720 A` | `0.1196 A` | 变差 |
| Ld/Lq `0.5x`，固定速度饱和 | `0` | `0` | 无收益 |
| Ld/Lq `1.5x`，无速度约束 RMSE | — | `0.2930 A` | 仍受物理电压限制 |

因此不建议按连续时间公式直接改写当前离散 PI；它没有在代表性失配工况中证明收益。

### Feedforward 对照

固定初始速度 `20 rad/s`、负载 `0.30 N·m`、目标 `iq=1 A`，相同 Plant、相同 PI、相同窗口：

| 对照 | RMSE | 尾段 RMSE | 最大电压 | 最大 Feedforward 电压 | 饱和 |
| --- | ---: | ---: | ---: | ---: | ---: |
| Feedforward OFF | `0.0702 A` | `1.62e-4 A` | `4.46 V` | `0` | `0` |
| 参数一致 Feedforward ON | `0.0132 A` | `1.09e-6 A` | `5.46 V` | `4.00 V` | `0` |
| 错误模型 Feedforward ON | `0.0214 A` | `2.87e-5 A` | `4.66 V` | `3.20 V` | `0` |

在该工况中，参数一致 Feedforward 是唯一得到明显、可重复收益的候选；它降低了稳态电流误差，但提高了电压请求，仍必须经过电压裕量和参数准入检查。

## AC06｜抗饱和专项判断

逐 H0 trace 显示：

- 未饱和时积分器按 `Ki·dt·error` 试算。
- Ld/Lq 退化进入饱和后，积分器不再无限增长，`integral_abs_max` 在当前长饱和路径约 `11.9 V`。
- 饱和期间实际施加电压被限制在边界附近；请求电压继续超过边界，符合物理不可实现工况。
- `test_foc_current`、Feedforward saturation 和 Runtime UPDATE rejection 现有事务测试继续通过。
- 本轮没有发现积分器绕过 `applied voltage` 反馈而错误提交的证据。

因此当前 anti-windup 没有被证明存在数学错误；退化主要来自持续物理饱和和参数/工况失配。长时间饱和下的积分幅值仍然较大，属于后续可独立研究的调参风险，不能在本轮直接重写。

## AC07｜保留的唯一优化候选

**Host 参数候选：经过校准参数准入后启用一致的 PMSM d/q Feedforward。**

证据：固定高速/负载对照中 RMSE `0.0702 -> 0.0132 A`，尾段 RMSE `1.62e-4 -> 1.09e-6 A`，没有新增饱和；错误模型仍有改善但明显弱于一致模型。

代价与约束：

- 需要可接受的 Ld/Lq/ψf 和可信电气速度。
- Feedforward 会增加电压请求，必须继续经过现有 SVPWM/电压限幅。
- 参数未辨识时不能直接打开 Board1 默认配置。
- 本轮只验证 Host 配置，没有接入生产 Runtime，也没有改默认开关。

PI 增益调度候选被否决：两组代表性失配中均未改善，甚至加重 RMSE 或饱和。

## 回归结果

- Robustness CTest：`9/9 PASS`，研究入口报告 `31` 个场景、`unstable_cases=0`。
- Core：`14/14 PASS`。
- Sensors：`2/2 PASS`。
- Calibration：`1/1 PASS`。
- G3-NP：`2/2 PASS`。
- G3-SAFE：`1/1 PASS`。
- Runtime Sanitizer：`9/9 PASS`。
- Protocol：`1/1 PASS`。
- Board/UART/Python：`4/4 PASS`。
- Clang ASan/UBSan：总计 `34/34 PASS`。
- Host Debug：`45/45 PASS`。
- SYSTEM replay：`3f01f6e3`。
- Long replay：`49dea950`。
- Source contract：`67 C/H、8 CMake、8 README PASS`。
- CMake formatter：`29/29 PASS`。
- `Scope falconfoc`、`Scope all`、`git diff --check`：PASS。
- AT32F415 ARM 构建：PASS；研究测试未链接进 ARM 固件，资源无解释变化。

本轮只修改 Host 研究测试、CMake 测试注册、证据和维护日志。没有修改生产 Current PI、Feedforward、Safety FSM、Runtime 事务、Board 默认参数或任何硬件路径。

## 最终建议

下一步唯一优先事项是：在真实电机参数能够通过校准准入之前，继续保持 Feedforward 关闭；完成参数身份、Ld/Lq/ψf 和电气速度质量契约后，再对 Feedforward 做独立 Runtime 接入评审。不要先改 PI，也不要把当前 Host 候选直接写入 Board1 默认配置。

继续保持：`G3-SAFE HARDWARE NOT VERIFIED`、`G3-PWR LOCKED`、`AT32F415 WCET NOT VERIFIED`、`MOTOR PHYSICAL OPERATION UNVERIFIED`。

