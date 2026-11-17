# FalconFOC V2｜NEXT-CONTROL-TUNING-01 Host 整定研究证据

## 结论

- 阶段：`NEXT-CONTROL-TUNING-01`
- 结果：`NEXT-CONTROL-TUNING-01 RESEARCH COMPLETE`
- Host 研究结论：`IMPROVEMENT_CONFIRMED`
- 冻结 Holdout：`HOLDOUT_PASS`
- 研究候选不是生产固件参数，也不是真实电机最终整定结果。
- 真实硬件、AT32F415 WCET、物理关断、Board1 控制品质和真实电机参数辨识仍未验证。

## 1. 身份与边界

| 项目 | 实际值 |
|---|---|
| 工作树 | `D:\study\Software\Hollow-Gimbal-vm01a` |
| 独立工作树 | `D:\study\Software\Hollow-Gimbal-vm01a` |
| 分支 | `codex/falconfoc-v2-vm01a` |
| 原始基线 | `a198f6d4a6c61f1bb9dd5968a46e7f38ac49a3f0` |
| 研究入口 | `tools/falconfoc-cli/control_tuning.py` |
| 机器结果 | `build/control-tuning-01/study.json` |
| ARM BIN | `37,864 B` |
| ARM SHA-256 | `BDEC4D2706049080FF758EE308ADDC1304294DFD5805F05A0CAE7D56D4191806` |

本轮只改变 Host Virtual device 在 Runtime 初始化前复制的研究配置。没有修改生产 Current/Velocity/Position 默认值、Board1 参数、Runtime Safety、生命周期邮箱、UART V1、Feedforward 准入、真实输出提交路径或 ARM 代码路径。主工作树仍保持独立；没有 commit、push、merge、reset、clean、stash、刷机、上电或运行真实电机。

## 2. AC00 基线复核

`GENERIC_RESEARCH_MOTOR` 名义模型保持原值：

| 参数 | 名义值 |
|---|---:|
| Rs | `0.6 Ω` |
| Ld / Lq | `0.0012 / 0.0015 H` |
| ψf | `0.025 Wb` |
| 极对数 | `4` |
| VBUS | `12 V` |
| H0 / H1 | `200 / 500 µs` |
| Current Kp(d/q) | `1.08 / 1.35 V/A` |
| Current Ki(d/q) | `540 / 540 V/(A·s)` |
| Velocity Kp/Ki | `0.18 / 1.4` |
| Position Kp | `4.0 s⁻¹` |
| 电流/电压限制 | `2.5 A / 2.4 V` |

历史 Current 10 ms 证据重新得到：Full RMSE `0.152791132 A`、RUNNING RMSE `0.089168894 A`、RUNNING Tail RMSE `0.005696917 A`。名义长时域结果也保持：Velocity 100 ms 最终速度 `2.61577222 rad/s`，Position 2 s 最终误差 `0.0049254 rad`。

## 3. 实验设计与验收 Oracle

实验沿用同一套 Host Virtual device、生产 Runtime H0/H1、Current PI、Velocity PI、Position P、SVPWM、ADC/Encoder 解码和平均值 PMSM Plant。研究参数只在初始化前写入 `foc_virtual_current_profile`，不在运行中修改控制器积分状态。

搜索顺序固定为：

```text
Current 小网格 → 固定 Current 后 Velocity 小网格 → 固定内环后 Position 小网格
→ 冻结候选 → Holdout → 精确长时域复核
```

硬约束先于性能排序：

- `INTEGRATION_PASS`，无最终 Fault。
- 至少一个有效 RUNNING UPDATE。
- 指标有限，不能出现 NaN/Inf。
- 饱和持续时间不超过观察窗口的 80%。
- STOP 后的最终 `DISABLED` 不被误判为运行失败；运行证据来自状态限定的有效 UPDATE 窗口。

可行候选再按 `RUNNING Tail RMSE`、Final Error、Settling Time、Saturation 和 Overshoot 比较。结果保留 `FEASIBLE/INFEASIBLE` 与 `CONTROL_QUALITY_PASS/FAIL/EXPECTED_DEGRADED` 两套语义。

## 4. Current 搜索

| 候选 | Kp | Ki | Full RMSE (A) | Tail RMSE (A) | Final Error (A) | Settling (µs) | Score | 约束 |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| baseline | 名义 | 名义 | 0.152791 | 0.005697 | 0.185183 | 4500 | 2.3588 | FEASIBLE |
| current-c1 | 0.90 | 450 | 0.160005 | 0.003266 | 0.184130 | 5000 | 2.3740 | FEASIBLE |
| current-c2 | 1.08 | 540 | 0.155853 | 0.003126 | 0.184406 | 4500 | 2.3351 | FEASIBLE |
| current-c3 | 1.26 | 630 | 0.152713 | 0.003036 | 0.184521 | 4500 | 2.3473 | FEASIBLE |

`current-c1` 被较慢且 Full RMSE 更高的结果淘汰；`current-c2` 的综合 Score 略低，但在 Pareto 维度被 `current-c3` 以同样 Settling 和更低 Tail RMSE 支配，因此不作为最终候选。Current 改善幅度很小，不能单独把它描述为生产收益。

## 5. Velocity 搜索

搜索使用选定 Current 候选，保留 100 ms 长时域。

| 候选 | Kp | Ki | RUNNING Tail RMSE (rad/s) | Final Error (rad/s) | Settling (µs) | Score | 约束 |
|---|---:|---:|---:|---:|---:|---:|---|
| velocity-baseline | 0.18 | 1.40 | 0.591312 | 0.378045 | 95500 | 2.8937 | FEASIBLE |
| velocity-c1 | 0.135 | 1.05 | 0.946396 | 0.725827 | — | 4.3444 | FEASIBLE |
| velocity-c2 | 0.18 | 1.40 | 0.591312 | 0.378045 | 95500 | 2.8937 | FEASIBLE |
| velocity-c3 | 0.225 | 1.75 | 0.338711 | 0.145466 | 79500 | 1.7634 | FEASIBLE |

低增益候选没有触发安全故障，但跟踪变慢；`velocity-c3` 在相同电压/电流限制下改善误差和调节时间，因此被保留。

## 6. Position 搜索

搜索使用选定 Current/Velocity 候选，先用 200 ms 固定研究窗口筛选。

| 候选 | Kp (s⁻¹) | RUNNING Tail RMSE (rad) | Final Error (rad) | Settling (µs) | Score | 约束 |
|---|---:|---:|---:|---:|---:|---|
| position-baseline | 4.0 | 0.532458 | 0.453249 | — | 20.7141 | FEASIBLE |
| position-c1 | 3.0 | 0.635269 | 0.569509 | — | 25.0956 | FEASIBLE |
| position-c2 | 4.0 | 0.532458 | 0.453249 | — | 20.7141 | FEASIBLE |
| position-c3 | 5.0 | 0.438211 | 0.349110 | — | 16.7464 | FEASIBLE |

`position-c1` 过慢；`position-c3` 在筛选窗口中改善误差，且没有增加饱和或 Fault。

## 7. 冻结候选与精确长时域复核

冻结 Host 研究候选：

```text
current_kp = 1.26       current_ki = 630.0
velocity_kp = 0.225     velocity_ki = 1.75
position_kp = 5.0
```

Current Kp/Ki 是对 d/q 两轴的标量 Host 覆盖；电流限制 `2.5 A`、电压限制 `2.4 V`、VBUS `12 V`、H0/H1 周期和电机模型保持名义值。

| 场景 | 名义结果 | 冻结候选结果 | 变化 |
|---|---:|---:|---:|
| Velocity 100 ms Tail RMSE | 0.592805 | 0.338711 | 下降 42.8% |
| Velocity Settling | 95500 µs | 79500 µs | 缩短 16000 µs |
| Velocity 最终速度 | 2.615772 | 2.852833 rad/s | 更接近 3 rad/s |
| Position 2 s Tail RMSE | 0.016434 | 0.006548 | 下降 60.2% |
| Position Settling | 1383000 µs | 1215500 µs | 缩短 167500 µs |
| Position 最终误差 | 0.004925 | 0.001579 rad | 下降 0.003346 rad |
| Current 10 ms Full RMSE | 0.152791 | 0.152713 A | 轻微下降 |
| Current 10 ms Tail RMSE | 0.005697 | 0.003036 A | 下降 46.7% |

两个长时域场景均为研究阈值下的 `CONTROL_QUALITY_PASS`；这只证明 Host 模型中的候选收益，不能推广到 Board1。

## 8. Holdout 与参数扰动

候选冻结后没有改变搜索范围，执行未参与选择的目标方向、负载、VBUS 和惯量条件：

| Holdout | 条件 | 控制品质 | 硬约束 |
|---|---|---|---|
| Current reverse | `-0.4 A`, 10 ms | `CONTROL_QUALITY_FAIL` | FEASIBLE |
| Velocity load/perturbed | `-3 rad/s`, `0.02 N·m`, VBUS `10 V`, inertia `1.2×` | `CONTROL_QUALITY_PASS` | FEASIBLE |
| Position reverse/perturbed | `-2 rad`, VBUS `10 V`, inertia `1.2×` | `CONTROL_QUALITY_PASS` | FEASIBLE |

Current reverse 的短观察窗口仍然没有达到控制品质阈值，保留为失败证据，没有通过调低阈值或删除样本来包装结果。整体 Holdout 硬约束为 `HOLDOUT_PASS`；控制品质状态仍按场景单独解释。

## 9. 饱和、安全和淘汰边界

本轮所有记录候选均通过 Host 硬约束，没有候选触发持续饱和或 Fault。低增益 Current/Velocity 候选因误差与调节时间更差而被 Pareto/Score 淘汰；高增益候选没有被当作无条件最优，仍需在更多负载、VBUS 和电机参数范围内继续验证。

所有虚拟输出仍满足 `physical_applied=false`。Board1 的 `foc_board_output_submit()`、PWM/MOE 锁定、Serial `LOCKED` 默认策略和 ARM 安全构建没有被整定研究绕过。

## 10. 机器结果与复现

完整机器结果：

- `build/control-tuning-01/study.json`
- `build/control-tuning-01/optimization/*.json` / `*.csv`
- `build/control-tuning-01/holdout/*.json` / `*.csv`
- `build/control-tuning-01/confirmation/*.json` / `*.csv`

`study.json` 的 `final_candidates[]` 按 `profile_id`、`candidate_id`、Current/Velocity/Position 参数、`training_metrics`、`holdout_metrics`、`safety_result` 和 `selection_reason` 保存最终候选的机器可读身份。

复现命令：

```powershell
cmake --build build/vm01a-simulation --parallel 4
python tools/falconfoc-cli/control_tuning.py `
  --simulator build/vm01a-simulation/falconfoc-virtual-device.exe `
  --output-dir build/control-tuning-01
```

单个冻结候选也可以直接通过 CLI 复现，例如：

```powershell
python tools/falconfoc-cli/falconfoc_cli.py `
  --simulator build/vm01a-simulation/falconfoc-virtual-device.exe `
  virtual-demo --virtual-scenario velocity --horizon long `
  --duration-us 100000 --reference 3.0 --profile-id selected-long-velocity `
  --current-kp 1.26 --current-ki 630 --velocity-kp 0.225 --velocity-ki 1.75 `
  --position-kp 5.0 --json build/control-tuning-01/replay-selected-velocity.json
```

本轮没有引入绘图库；CSV/JSON 是机器可读取交付物，曲线可由现有数据后处理生成，不影响控制器或研究结论。

## 11. 回归与 ARM

整定研究后执行并保留以下门禁：

- Simulation CTest：`14/14 PASS`
- Clang ASan/UBSan Simulation：`14/14 PASS`
- Runtime：`11/11 PASS`（`build/vm01a-runtime-final`）
- Board Host：`4/4 PASS`
- Python/C CLI 与整定选择器：`11/11 PASS`（含 3 个整定选择器测试）
- Legacy Host GCC：`45/45 PASS`
- Source Contract、CMake Quality、Formatter、`Scope falconfoc`：PASS
- `Scope all`：`SCOPE_ALL_ENVIRONMENT_BLOCKED`（Firmware CLI contract 需要当前环境缺少的 Bash；没有绕过或伪造 PASS）
- V2 ARM BIN：`37,864 B`，SHA-256 `BDEC4D2706049080FF758EE308ADDC1304294DFD5805F05A0CAE7D56D4191806`，与本轮前一致

## 12. 最小下一步

保留本候选作为 Host Research Profile，继续扩大独立工况矩阵，优先验证反向 Current、低 VBUS、负载施加/移除和更多 Rs/Ld/Lq/ψf 扰动。只有在真实电机参数辨识、目标 MCU 周期/栈/抖动测量和硬件安全准入完成后，才讨论是否提出生产参数变更；本报告不授权这些动作。

当前边界继续保持：`G3-SAFE HARDWARE NOT VERIFIED`、`G3-PWR LOCKED`、`AT32F415 WCET NOT VERIFIED`、`MOTOR PHYSICAL OPERATION UNVERIFIED`。
