# NEXT-SENSOR-EST-01｜速度估计器对照与 PLL 生产准入

2026-11-27。**PASS — KEEP EXISTING ESTIMATORS。** 隔离 PLL 在静止和低速 Holdout 比现有 LPF 更安静，但加速及减速 RMS 分别恶化 **20.67% 和 21.79%**，超出预定的 15% 限值。真实 Host Velocity/Position 对照也未显示足以切换 H1 速度源的控制品质收益。PLL 的实现仅编入 Simulation 测试，生产 Sensor、H1/H0 和锁定参考固件保持基线。

## 1. 现有估计器与可复现基线

| 方法 | 本轮观察 | 样本运算与状态 |
| --- | --- | --- |
| 差分 | 量化直接放大为速度噪声，阶跃响应满足第一拍模拟样本。长多圈无新增误差。 | 从整数展开位置增量换算弧度，再除以源间隔。 |
| Variable-dt LPF | 默认 80 Hz。加减速有延迟，在长多圈仍稳定。 | 以 `h/(τ+h)` 更新缓存速度，额外 1 次 Float32 除及乘加。 |
| Alpha-Beta | 默认 α=0.85、β=0.20，普通多圈量级跟踪略快。极大圈数下由绝对 Float32 位置相减产生失真。 | 位置预测、位置残差、位置及速度修正。当采样改变时，速度修正明确用 `β/h`。 |
| 实验 PLL | 以局部相位残差避开绝对 Float32。低速平稳，动态 ramp 延迟较高。 | 按实际源间隔求一对重实极点，更新速度和相对端点相位。 |

同一生产 Encoder 每帧只做一次 LUT、方向和整数展开运算。四估计器共用同一原始码、同一 Sensor 反馈、同一 LUT 和源时间戳。研究 CPR=16,384，使用 01B 从采集 38/39 生成的 64 节点表，保持 80 Hz LPF 与既有 Alpha-Beta 增益。运动真值为独立分段运动学，原码通过现有 Virtual Motor 量化器，使用与 01A/01B 一致的基波与三次谐波扰动。静止在物理角恒定时叠加 ±1 码的 LCG 噪声。训练种子 718，Holdout 种子 2257，使用不同方向配置和轨迹。固定间隔 500 µs，Holdout 源间隔 477–524 µs。时间原点靠近 uint32 回绕。 [现有 Sensor](../../sensors/src/foc_encoder.c#L368) · [统一观察入口](../../simulation/tests/test_foc_sensor_est.c#L170)

静止和等速 RMS 去掉每段前 25 ms，加减速 RMS 纳入整段。T90 为进入目标阶跃幅值 10% 的误差带后保持至段末，且最终连续区间至少 5 ms。峰值、符号偏差、全部工况与未稳定的记录见 `data/estimator-summary.csv`，T90=0 表示未建立该指标。基线在加入候选实现前记录，最终 `--baseline` 输出逐行一致。

## 2. 候选模型与预注册筛选

设 `u` 为 Encoder 对本区间的差分速度，`e` 为估计位置减去该位置观测的局部误差，`v` 为 PLL 速度，`Ω` 为自然角频率。区间内以线性观测插值，连续误差方程为 `e'=v−u−2Ωe`、`v'=−Ω²e`。阻尼比为 1。用后向欧拉，令 `x=Ωh`、`r=1/(1+x)`。

```text
e_next = r² * (e + h * (v - u))
v_next = v - Ω*x*e_next
```

固定步长的重极点为 `r`，随 dt 更新。用 `(e,(v−u)/Ω)` 归一化后，各步的齐次更新是同一临界阻尼矩阵的函数。该结论用于说明离散方程，实际抖动和动态误差由 Holdout 测量。`Ω=800 rad/s` 的连续速度响应 −3 dB 约 82 Hz。公平性以实测 T90 衡量。相位使用 Sensor 已选择的短弧，在局部端点坐标中处理零点，无累计 Float32 角或二次取模。系统保持既有 Encoder 相位输入。实验仅向 H1 提供速度。候选要求有限有效报告，限制速度和相位残差，要求 `Ωh≤2.5`，IEEE Float32 更新每拍与独立 Double 2×2 隐式方程求解比较通过。 [实验内核](../../simulation/tests/foc_sensor_pll_experiment.c)

网格固定为 600/800/1000/1200 rad/s。训练要求正向与反向 T90 为 LPF 的 80%–125%，静止 RMS ≤110%。合格候选中以爬行 RMS 最低者入 Holdout。

| Ω rad/s | 正向 / 反向 T90，ms | 训练爬行 RMS，rad/s | 训练公平性 |
| ---: | ---: | ---: | --- |
| 600 | 7.5 / 7.5 | 0.02547 | 超过响应延迟上限 |
| 800 | 6.0 / 6.0 | 0.03654 | 合格，选中 |
| 1000 | 5.0 / 5.0 | 0.04691 | 合格 |
| 1200 | 4.0 / 4.0 | 0.05653 | 低于公平响应下限 |

Holdout 固定要求爬行 RMS 至少改善 15%，静止 ≤110%，加减速 ≤115%，阶跃和反向 T90 ≤125%，高速 RMS ≤125%。闭环门槛为 Velocity/Position 跟踪、H0 总角、d/q 联合 RMS ≤同 LUT LPF 的 105%，UPDATE 接受和 STOP/FAULT 满足相同语义。实验静态成本预算为 32 B 实例、1,200 B 对象 text、无新增 libm。门槛存于 `preregistered-policy.json`，仅用训练选择 800，Holdout 保持这一候选。

## 3. Holdout、低速纹波和长多圈

| 工况 | 差分 RMS | LPF RMS | Alpha-Beta RMS | PLL RMS | PLL 相对 LPF |
| --- | ---: | ---: | ---: | ---: | ---: |
| 静止 ±1 码，rad/s | 0.86127 | 0.13158 | 0.13341 | 0.06035 | −54.14% |
| 0.9 rad/s 爬行 | 0.31259 | 0.05970 | 0.06356 | 0.03499 | −41.39% |
| +150 rad/s² 加速 | 0.46102 | 0.33910 | 0.28769 | 0.40917 | +20.67%，未通过 |
| −28 到 0 rad/s 的负向减速 | 0.41561 | 0.44754 | 0.37598 | 0.54508 | +21.79%，未通过 |
| −240 rad/s 稳态 | 0.43189 | 0.06910 | 0.07123 | 0.03365 | −51.30% |
| 正向阶跃 / 反向 T90，ms | 0.477 / 0.477 | 5.516 / 5.516 | 4.486 / 4.486 | 6.000 / 6.000 | +0.484 ms |

同一爬行轨迹的 13,999 个相邻变化，LUT 关闭时重复码 **0**、跨 ≥2 码 **2,430**，开启后为 **139** 和 **2,526**。LPF RMS 为 **0.05549 → 0.05970 rad/s**，PLL 为 **0.03471 → 0.03499 rad/s**。早先 01B 的分解说明主要噪声来自输入码量化与输出整码舍入，周期拟合余差项较小。此轮的相同原码对照显示 LUT 会改变码值步进分布，估计器决定位移差分噪声的带宽和延迟。在试验区间内，平稳工况收益可重复，不能抵消 ramp 未通过。 [01B 量化分析](2026-11-27-next-encoder-linear-01b.md#5-低速速度纹波及量化归因)

只给整数状态注入 **±1,000,000 圈**，其局部角度、采样和速度真值保持相同。原始码、校正码、源时间戳、差分、LPF 和 PLL 的逐点值一致。Alpha-Beta 爬行 RMS 从近原点的 **0.06705** 增至 **165.42 rad/s**，峰值 **197.53 rad/s**。绝对角约 6.28×10⁶ rad 时 Float32 间距约 0.5 rad，远超爬行每拍变化。误差一部分进入预测残差，并被 β/h 带到速度。当前默认 LPF 源规避了此问题。长圈数测试模拟既有整数状态，未声称 Host 实时跑过一百万圈。

## 4. Sensor 失效和实际 Host 控制链

独立复核发现实验 PLL 可拒绝 Sensor 已接受的过长间隔，导致下一短增量若被接收会从旧 PLL 端点错误续算。定向测试复现后修正，PLL 只在成功时提交 `captured_us` 与动态状态。后续区间必须从这个端点出发并复现 Sensor 的模块化 dt，端点缺口要求显式 reset/reseed。普通重复和超时不推进 Sensor 和 PLL，下一完整间隔恢复。试验中超时后 500 µs 重新有可用速度，使用完整 1,000 µs 差分。过期帧锁存连续性丢失，Sensor 与 PLL 重置后第二帧 500 µs 重新播种。无效速度、非有限输入和未播种位置均不作为有效 PLL 反馈。

四臂为 A 无 LUT+LPF、B LUT+LPF、C LUT+Alpha-Beta、D LUT+实验 PLL。Host 专用 H1 适配器提交同一观测的整数相位和源时间戳，速度来自所选方法。随后运行真实 `foc_runtime_h1_step()`、实际虚拟 Plant 推进、H0、Pipeline 提交和 Current。重建同拍 ADC 的 Double Park 结果验证 Current 确实消费该预测角。各臂的研究电机、增益、命令、负载和故障时间相同。反馈差异产生的真实 Plant 轨迹变化保留。 [Host 控制链](../../simulation/tests/test_foc_sensor_est.c#L625)

每次 240 ms。H0 200 µs，H1 500 µs。控制电压限 1.2 V，40–90 ms 大目标制造饱和。110–160 ms 注入 0.010 N·m 同一负载。240 ms 对 Velocity 执行 STOP，对 Position 注入同一帧错误。下表预测速度项是固定同一 H1 相位、替换为采样时 Plant 真速度所形成的 H0 预测差。总电角直接对实际 H0 时刻 Plant 真值，d/q 对相同 ADC 在真值转子角下的结果。

| 模式 / 臂 | Plant 跟踪 RMS | 速度反馈 RMS，rad/s | H0 总电角 RMS，mrad | 预测速度项 RMS，mrad | d/q RMS，mA |
| --- | ---: | ---: | ---: | ---: | ---: |
| Velocity / A | 8.29245 rad/s | 0.17753 | 93.816 | 0.174 | 44.043 / 1.677 |
| Velocity / B | 8.30975 rad/s | 0.13595 | 0.738 | 0.133 | 0.500 / 0.005 |
| Velocity / C | 8.30999 rad/s | 0.11935 | 0.724 | 0.117 | 0.495 / 0.005 |
| Velocity / D | 8.30795 rad/s | 0.15311 | 0.719 | 0.150 | 0.484 / 0.005 |
| Position / A | 0.93452 rad | 0.15241 | 76.606 | 0.149 | 38.313 / 1.477 |
| Position / B | 0.93701 rad | 0.12265 | 0.741 | 0.120 | 0.498 / 0.001 |
| Position / C | 0.93695 rad | 0.10976 | 0.774 | 0.108 | 0.496 / 0.001 |
| Position / D | 0.93711 rad | 0.13684 | 0.749 | 0.134 | 0.478 / 0.001 |

每臂 **1,195 个接受 UPDATE、0 个拒绝**。Velocity 各臂有 252 个饱和更新，高目标释放后首个未饱和采样在 400 µs。Position 各臂是 6 个和 200 µs。轨迹可查看恢复阶段的后续状态。STOP/FAULT 从 240 ms 起均确认 INHIBIT，之后无接受 UPDATE，最终分别为 DISABLED/FAULT_LATCHED。闭环门槛通过。D 的低速静态增益未成为持续跟踪的改善，速度预测误差分量在两组都略增。生产继续使用原 LPF 发布。实测转矩品质仍待独立验证。

## 5. Cortex-M4 Soft-float 静态资源与正式回归

ARM GNU 14.3.1，`-Os -mcpu=cortex-m4 -mthumb -mfloat-abi=soft -fstack-usage`。现有 Encoder 对象 **2,206 B text**，Encoder 状态 **96 B**，反馈 **56 B**，update 自身静态栈 **88 B**。实验 PLL 独立对象 **700 B text**、data/bss 0，状态 **24 B**，init/step 自身静态栈 **24/48 B**。实验 step 反汇编包含 **8** 个乘、**1** 个除、2 个加、2 个减及 1 个整数转 Float32 的辅助调用点，另有比较和有限性分支。不同分支执行的调用集合有区别。无新增 libm 或 Double 快路径依赖。Double 仅在 Host 数值参考与指标计算。真实 MCU WCET 未测。生产无新增 Flash/RAM/计算开销，锁定固件仍为 **38,520 B Flash / 3,784 B RAM**，BIN SHA-256 `736a0eada7e43439e9d981f2649e144785eccdc50b7c01fa61f3fc7e5ee3d5b4`。

- Sensors **2/2**、Calibration **5/5**、Core **15/15**、Runtime **11/11**、Simulation Clang/GNU 各 **22/22**。Sensors 与 Simulation ASan/UBSan **2/2** 和 **22/22**。严格 Scope all、30 项 CMake 格式、Source Contract、三份 Legacy 预设、`run.sh build`、Legacy Host **45/45**、锁定参考固件构建均通过。
- 独立 GNU 短枚举定向 CTest **1/1**，端点续算 P2 已由 RED 转 GREEN，最终无 P0/P1/P2。证据包保留 36,080 条 Sensor 输入口径、9,560 条受控 Host H0 更新、固定策略和 ARM 反汇编。 [原始证据](2026-11-27-next-sensor-est-01/raw-evidence.zip)
- 编码器 01B 研究的 **369 行**、01A 系统角度 **2,587 条**记录和 LUT 对照 10 行复现。Motor-ID **677 行**轨迹和 **35 条**质量记录复现。

## 6. Git 范围与下一步

本地与只读远端基线为分支 `codex/falconfoc-v2-vm01a`、HEAD `f9af51be41aa21ad11cb66444d6e7664ae93213d`。开工前快照 27 个已有路径，仅改动 `simulation/CMakeLists.txt` 的测试注册及 `docs/governance/Themis_工程维护日志.md` 的新条目。新增 `simulation/tests/foc_sensor_pll_experiment.h`、`simulation/tests/foc_sensor_pll_experiment.c`、`simulation/tests/test_foc_sensor_est.c` 及报告和证据 ZIP。从 HEAD 起 **32 个路径待提交**，原始哈希及准确清单存于 ZIP 的 `verification.json`。本轮止于本地工作树交付。

优先完善速度估计的局部坐标。先给 Alpha-Beta 建立整数增量或局部残差的等价实现，对照正常圈数、百万圈、丢帧和实际源 jitter。保留默认 LPF，分别量化爬行噪声与加减速滞后，再选择 Velocity/Position 的反馈源。专业伺服验收继续要求独立参考速度、带负载闭环工况和目标时序证据。

```text
G3-SAFE HARDWARE NOT VERIFIED
G3-PWR LOCKED
AT32F415 WCET NOT VERIFIED
MOTOR PHYSICAL OPERATION UNVERIFIED
```
