# NEXT-CAL-FLUX-01｜FalconFOC 通用磁链与 Kt 辨识集成

**NEXT-CAL-FLUX-01 REUSABLE FLUX IDENTIFICATION PASS**

Host Virtual Motor 的端电压积分和双向拟合已迁入独立 C11 估计器。两种虚拟电机共 1,024 条区间与旧版逐项一致，新模块复现了原双向估计和残差。审核后的推导 Kt 进入新 Runtime，四模式闭环、阻抗超载限流和 STOP/FAULT 已复验。

**PHYSICAL FLUX MEASUREMENT NOT VERIFIED** · **PHYSICAL TORQUE CALIBRATION NOT VERIFIED** · **TARGET MCU WCET NOT VERIFIED** · **G3-PWR LOCKED**

[复跑说明与原始证据](2026-11-20-next-cal-flux-01/README.md) · [C11 接口](../../calibration/include/foc_flux_id.h) · [Host CLI](../../tools/falconfoc-cli/README.md#host-flux-evidence)

## 1. 基线与前置验收

开工和收尾均在 D:/study/Software/Hollow-Gimbal-vm01a 的 codex/falconfoc-v2-vm01a，HEAD 为 c93655efbce28f6a81fcce2e5c20ac61f45cab76，暂存区为空。以真实工作树冻结旧算法，包含本地 PRODUCT-02 和 CAL-ARCH-02。ARCH-02 原始包 SHA-256 与清单相符。单独 ARCH-01 报告缺失，前置门禁以 ARCH-02 归档中的 Rs/J/B 分项审核红绿测试及完整配置应用回归为准。

在旧估计器的临时副本独立记录实际 αβ 端电压、两端角度、d/q 电流、速度、窗口统计和拒绝行为；旧原版与插桩版预览逐字节一致。冻结包含 11 个旧拒绝用例和可以原样重跑的源码及独立窗口复算程序。 [ARCH-02 验收](2026-11-20-next-cal-arch-02.md) · [ARCH-02 原始证据入口](2026-11-20-next-cal-arch-02/README.md)

## 2. 算法、坐标和数值复现

新估计器采用现有幅值不变 Clarke/Park，电流为相电流峰值。ωm 为机械 rad/s，ωe = p·ωm 为电角 rad/s，p 为极对数。在 q 轴上 Vq = Rs·Iq + Lq·dIq/dt + ωe·(Ld·Id + ψf)。

计算顺序保留旧实现。每区间将已生效 αβ 电压分别投影到两端角度的 q 轴并取平均，电流取匹配 ADC 两端的中点。先逐项累加 Vq − Rs·Iq − ωe·Ld·Id，再用相同窗口端点和时长减去 Lq·ΔIq/T。对窗口平均电角速度 W 和得到的平均反电势 E 求通过原点的最小二乘估计 ψf = Σ(W·E)/Σ(W²)。窗口等时，回归的隐含权重为 W²。q 电流端点保留原 double 运算顺序。整个采集不存样本数组。

Host 原路径以四档 +10、−10、+20、−20 electrical rad/s 采样，各四窗，32 个 50 µs 区间组成 1.6 ms 窗口。每套 512 个区间、513 个电流端点，时标 2,224,600 至 2,250,200 µs，综合速度 RMS 15.8113883 rad/s。第一段的请求 Vq 为 0.12 V，实际区间投影平均为 0.1199624925 V。数据源沿用观察到的实际值。

| 模型 | 组合 ψf 发布值（Wb） | 正向 / 反向拟合（Wb） | 残差 RMS（V） | 与 Plant 真值的偏差 |
| --- | ---: | --- | ---: | ---: |
| Generic | 0.0250059552 | 0.0250041327056 / 0.0250077761770 | 0.0006991222884 | +0.02382% |
| Salient | 0.03197309 | 0.0319756778030 / 0.0319705003495 | 0.0006875783490 | −0.08410% |

正反差分别为组合磁链的 0.01457% 和 0.01619%。两方向各 8 窗。与旧实现逐数匹配，包括分方向残差、组合残差、采样端点和区间 Vq。原综合速度 RMS ≥5 rad/s、ψf ∈ [0.001, 0.2] Wb、残差 ≤0.02 V 和正反差 ≤5% 保留。新通用契约显式检查各方向的窗口数、同窗口同符号、速度下限和两个速度档的间距。Host 配置每方向最少 8 窗，单窗速度绝对值底线和单向速度跨度底线皆为 1 rad/s，旧激励档和进入范围保持。

Ke = p·ψf 表示开路相对中性点的电压峰值除以机械 rad/s。Kt = 1.5·p·ψf 表示 Id=0 的永磁转矩 N·m/A_peak,q。p=4 的 Generic 得到 Ke 0.100023821、Kt 0.150035724，Salient 得到 Ke 0.127892360、Kt 0.191838533。显著凸极时完整转矩还含 1.5·p·(Ld−Lq)·Id·Iq。

## 3. 测量、来源与拒绝

- 同一采集绑定 Motor、Capture、Session、d/q 坐标、相序、极对数及其修订、R/L 修订、ADC 电流链、电压与速度参考、共用采集时钟。Host 从已通过 d/q 质量筛查的辨识结果重构 R/L 并核对实际用值。Plant 真值仅用于独立测试。
- 每个输入同时提交当前区间的 ADC 两端 d/q 值、同一区间的平均实际 Vq 和平均 ωe。电压、速度覆盖必须与电流孔径区间一致，共有端点数值相同，序列连续。固定区间时长和总采集半程上限支持 UInt32 回绕。有效电角参考、有效输出回执和端电压见证各有标志，饱和或无效数据会撤销整次采集。
- 接纳实际虚拟端电压、独立测得的物理端电压，以及明确有独立验证依据的端电压重构。单纯的控制请求或理想 PWM 换算没有端电压见证。来源标志由采集所有者声明，实机需要校验实际占空比装载、死区、电压降、母线、电流增益/偏置、角度同步及误差。间隔平均 ωe 与 Id 中点的乘积还需要足够的区间内分辨率来近似实际平均 ωe·Id。
- C11 测试用参数不同的两组独立合成数据恢复 ψf 0.042 和 0.026 Wb，极对数 4 和 11，涵盖正反和非零 Id、dIq/dt。34 种无效观测、8 种不可辨识或取消场景及有限输入导致累加溢出均拒绝发布。另测合格重构和测得电压标签，均为合成契约测试。残差 0.014625 V 时仍可因正反差 6.152% 正确拒绝。 [独立测试源码](../../calibration/tests/test_foc_flux_id.c)

| 独立数据注入 | 拟合磁链误差 | 质量裁决 |
| --- | ---: | --- |
| Vq 偏置 +25 mV | +0.32482% | 拒绝 |
| Vq 增益 +4% | +1.86201% | 通过 |
| d/q 电流偏置各 +60 mA | −0.49236% | 拒绝 |
| 速度增益 +4% | −3.89005% | 通过 |
| Rs +12%、Ld −15%、Lq +15% | +8.34071% | 拒绝 |
| 报告时间尺度 +5%，区间内部仍共时 | −2.56921% | 通过 |
| 电角零点偏差 +0.35 rad | −6.77463% | 拒绝 |
| Vq 噪声 20 mV 峰值 | −0.03135% | 通过 |

误差列包含候选被撤销时的离线拟合诊断。通过的系统偏差表明残差和双向一致性只检验给定模型和记录的自洽性。真实测量需独立验证其增益、时间和坐标。

## 4. ARCH-02 审核、复用和应用

通用输出显式标记 FLUX_FROM_MEASUREMENT 及 KT_DERIVED_FROM_FLUX。KT_FROM_INDEPENDENT_TORQUE 属于独立转矩来源，本轮通用磁链模块不签发它。现有窄的 Workflow Kt 接口保留。独立等效轴转矩测得的 J/B 可跨 Kt 重测保留，使用 Kt×Iq 的 J/B 继续依赖原 Kt 来源。

Host 保留原七阶段编号及 Measurement Trace v1。标准 CLI 额外显示 FLUX_CANDIDATE 和实际采集工况，可用 --flux-trace 导出区间。ARCH-02 的同一 Flux/Kt 记录持有磁链、导出量和原始证据，应用时复核采集/Session、R/L、ADC 和极对数修订、磁轴及电压源、结果字段、证据指纹、原 Runtime 状态以及完整数值候选。复用一致时保留原证据；更换 R/L 或使其失效会要求重测。更换 ADC 触发 R/L 和 Flux 重测。更换 Kt 参考后复用原 R/L、极对数和独立转矩 J/B。使用独立虚拟磁轴见证时，Encoder LUT 变化可继续保留磁链。

组合支持显式 review、reject 和 apply。未审核、错误审核、依赖失效、STOP/FAULT 和取消不会发布新 Runtime。既有 Calibration Application 负责完整准入和新 Runtime/Encoder 初始化。两种配置新标定、确认复用和 FCAL 重载的各 16 条四模式输出试验和原配置读回与冻结结果一致。

双向超载试验施加约 ±0.4125 N·m，超过 0.33 N·m 配置上限。Generic 记录 Iq 目标 −2.19948/+2.17207 A，按获准 Kt 算出的上限为 2.19948 A。Salient 两向均为 ±1.72020 A，达到各自上限。两种配置确认高阻 STOP 后无新增有效 UPDATE。原命令超时、锁存、受限清故障及复启用测试继续通过。

FCAL v1 仍保存既有数值和指纹，Generic/Salient 新生成的 548 B 包与原包逐字节一致。两份旧 36 Hz 包按原指纹和本地兼容策略重载。来源、方向质量、适用范围及依赖修订保留在 Host 内存或原始证据，读取 FCAL v1 不提升其可信等级。跨进程可信复用待独立版本化元数据。

## 5. 资源和全量回归

- Clang、MinGW GCC、Clang ASan/UBSan 各 **113 项**通过。Core 21、Runtime 13、Calibration 9、Control Plane 5、Sensors 2、Simulation 44、Servo 7、Protocol 1、Board Host 9、Workflow 1、Replay 1。
- Python CLI 11/11、Legacy Host 45/45、Source Contract、Strict Scope all 通过。历史 10 ms Current 双向轨迹逐字节相同，0.12 V 双向低调制试验仍安全停止，品质 FAIL 保持。Servo 冻结轨迹和门槛复验通过，历史近转矩限值及 0.039 N·m 超载品质裁决保留。Salient ±2.20 A 的 Startup V1 CSV 同字节，原评分仍失败。
- Host x64 中估计器状态 480 B、结果 216 B、输入区间 144 B。Clang -O2 用五组各 4,000 次 512 区间合成采集，估计冷路径批均值中位数约 24 µs/采集，包含资格检查和拟合。真实采集时间及 MCU WCET 需目标证据。
- ARM GNU 14.3.1 Soft-float 锁定构建复现 **44,452 B Flash / 4,184 B RAM**。BIN SHA-256 为 78d055e1135d23a5c2d08f6db17fbf64622333f429381efa2632401eb58cbda2，与前置相同。估计器符号未链接到锁定 ELF。Core、H0/H1、物理输出锁和既有七阶段/FCAL/公开类型布局均保持。

## 6. 改动与剩余实机前提

| 变更 | 用途 |
| --- | --- |
| [通用接口](../../calibration/include/foc_flux_id.h)、[实现](../../calibration/src/foc_flux_id.c)、[测试](../../calibration/tests/test_foc_flux_id.c)、[Calibration 构建](../../calibration/CMakeLists.txt) | 提取窗口积分与双向最小二乘，增加观测校验和合成负例。 |
| [Host 电气采集](../../simulation/src/foc_virtual_calibration_electrical.c)、[Host 会话](../../simulation/src/foc_virtual_calibration.c)、[私有证据接口](../../simulation/src/foc_virtual_calibration_flux.h) | 保留原激励和停止路径，把观测交给通用模块。 |
| [Host CLI](../../simulation/src/falconfoc_virtual_calibrate.c)、[私有接口](../../simulation/src/falconfoc_virtual_calibrate_internal.h)、[复用](../../simulation/src/falconfoc_virtual_calibrate_reuse.c)、[Python 入口](../../tools/falconfoc-cli/falconfoc_cli.py) | 预览来源、导出原始区间、复核依赖及证据、验证应用后负载限流。 |
| [CLI 回归](../../simulation/tests/test_virtual_calibration_cli.py)、[复用回归](../../simulation/tests/test_virtual_calibration_reuse_cli.py)、[Workflow 文档](../../calibration/WORKFLOW.md)、[维护日志](../governance/Themis_工程维护日志.md) | 锁定产品入口、原裁决和可复跑验收。 |

实机仍需要有独立误差预算的端电压和电流测量链、与 ADC 孔径对齐的磁轴和速度参考、经校验的实际 PWM 生效时间、可控的两向非饱和窗口以及独立转矩仪器。没有执行提交、推送、刷写、上电或真实电机励磁。

