# NEXT-MOTOR-ID-01A｜R/L 参数辨识与模型驱动 PI 整定

2026-11-17。**PASS，软件能力与受控 Host 验证通过。** FalconFOC 可从观测生成独立的 d/q 轴 R/L 拟合结果，质量通过后可组合 Rs/Ld/Lq 并生成 PI 候选。三个 Virtual Motor 参数组中，最大逐轴 Rs 误差 **0.2725%**，最大电感误差 **0.2287%**。当前固定增益和所有生产控制事务保持原值。

## 1. 交付与源码身份

- 分支 `codex/falconfoc-v2-vm01a`，开始时本地与远端均为 `f9af51be41aa21ad11cb66444d6e7664ae93213d`，工作树无未提交文件。构建、回归和数据输出使用隔离目录。
- [公开 API](../../calibration/include/foc_motor_id.h) 提供 `init/push/finish/reset`。caller-owned 实例在每次完成的窗口累积六个回归乘积，无样本堆分配、全局可变状态或保留输入指针。复用 `foc_calibration_state` 和 `foc_calibration_failure`，新 API 有明确操作返回码。
- `foc_motor_id_make_model()` 组合同来源且 ID 各异的两轴有效候选，验证 Rs 相对差后取均值。调用方保留两份记录并确保同一电机与测量配置。每份记录含来源、轴、采集 ID、首尾序号/时间、样本数、窗口数、激励、残差和拒绝原因。
- `foc_motor_pi_design()` 返回待评估增益，调用方显式把它写入离线 `foc_current_config`。新模块进入独立 Calibration 库和 Simulation 测试。Board1 镜像依然使用原配置及链接输入。 [调用和物理边界](../../calibration/README.md#standstill-motor-rl-and-pi-design)

## 2. 物理模型与辨识方法

现有 Virtual Motor 已实现 `vd = Rs*id + Ld*d(id)/dt − ωe*Lq*iq`、`vq = Rs*iq + Lq*d(iq)/dt + ωe*(Ld*id + ψf)`。ωe 是带符号电角速度，坐标和单位与 Current 一致。逐轴捕获要求固定电角轴、零观测电角速度，以及采集方对整个区间的独立静止确认。 [Plant 方程](../../simulation/src/foc_virtual_motor.c#L61)

每个不重叠窗口拟合 `mean(v) = Rs*mean(i) + L*(i_end−i_start)/T`。电流用实际时间间隔做梯形积分，电压输入是刚结束区间的实际平均**端电压**。窗口拉长了电流变化的观测基线，避免逐点对噪声求导。估计经双精度二列归一化最小二乘求解，参数和增益通过检测后以 Float32 发布。

准入使用逐点有效性和限幅标记、物理信号范围、严格连续序号、单调有界时间差、完整窗口、绝对 RMS 激励、归一化 Gram 行列式 ≥0.02、物理参数域，以及发布系数的含数值余量 RMS 电压残差。缺样、间隔异常、近零或共线激励、饱和、端量无效、参数超界和过大残差均有失败路径。无效会话保留诊断且参数归零，可显式 reset。累计最多 65,535 个端点。

独立复核发现初版的一个 P1。对于固定步长 ZOH 的精确 RL 端点，梯形近似会得到 `L_fit/L = (x/2)coth(x/2)`，其中 `x = Rs*Δt/L`。即使残差接近零，`x=4` 时 L 也会被高估约 107.5%。配置现在强制 `max_Δt*max_Rs/min_L ≤ 0.3`，在固定步长 ZOH 模型下把这项偏差限制在约 0.75%。存在抖动、噪声或区间内快速变化时仍需自己的采集证据。复核也收紧了极小残差的数值处理和极端 PI `Ki*Ts` 的可表示性。定向回归与最终独立复核均通过，最终无 P0/P1/P2 发现。

## 3. Virtual Motor 参数恢复与拒绝

估计器只收到已有 RK4 Plant 的电压与电流观测。每轴接受 769 个端点、96 个八区间窗口，供电研究配置为 12 V，实际轴电压在 −1.13 至 +1.11 V。d 轴单独激励保持标称磁链和 iq=0。q 轴软件试验设磁链为零、id=0，PM 和磁阻转矩均为零，实测虚拟速度和位移始终为零。另一组自由转子试验保留磁链，实现真实非零速度拒绝。该 q 轴数据是明确可辨识的纯软件实验。

| 场景与轴 | Rs 真值 / 拟合 Ω | L 真值 / 拟合 mH | Rs / L 误差 | 残差 mV |
| --- | ---: | ---: | ---: | ---: |
| 50 µs 名义 d | 0.600 / 0.600000 | 1.200 / 1.200063 | 0.0000% / 0.0052% | 0.00074 |
| 50 µs 名义 q | 0.600 / 0.600000 | 1.500 / 1.500050 | 0.0000% / 0.0033% | 0.00074 |
| 80 µs 扰动 d | 0.420 / 0.420586 | 0.850 / 0.850797 | 0.1395% / 0.0938% | 2.470 |
| 80 µs 扰动 q | 0.420 / 0.421144 | 2.000 / 2.001524 | 0.2724% / 0.0762% | 4.547 |
| 120 µs 扰动 d | 0.920 / 0.921131 | 2.000 / 2.002958 | 0.1229% / 0.1479% | 3.070 |
| 120 µs 扰动 q | 0.920 / 0.920830 | 0.950 / 0.952172 | 0.0902% / 0.2286% | 2.399 |

扰动组使用实际 Virtual ADC 量化通路，量程配置为 2 mA/count，注入有界 ±1 mA 电流测量噪声、4 mV 电压量化和 ±4 mV 电压测量噪声，并改变观测步长 ±12 µs。三组拟合的激励行列式为 0.99685–0.99996，两轴 Rs 通过 2% 一致性门槛。报告的误差限于这三个受控点。

| 失败实验 | 结果 |
| --- | --- |
| 零激励，769 个端点 / 96 窗口 | `UNIDENTIFIABLE`，无可用参数。 |
| 注入饱和标记 | 在第 40 个已接收端点之后拒绝，`PRECONDITION`，无可用参数。 |
| 带磁链的自由转子 q 激励 | 第一段后的电角速度约 0.00027565 rad/s，`PRECONDITION`，无可用参数。 |

单测另外覆盖非有限/无效 I/V，时间和序号缺口、uint32 回绕、同一指数形成的共线数据、估计负值和超范围、异常残差、超严残差约束、不完整尾窗口、不同采集来源、两轴 Rs 分歧、独立实例和 reset。 [Calibration 测试](../../calibration/tests/test_foc_motor_id.c) · [Virtual Motor 测试](../../simulation/tests/test_foc_motor_id_virtual.c)

## 4. PI 候选与固定电流品质基线

理想 RL 零极点抵消给出 `wc=2πfc`、`Kp_d/q=L_d/q*wc`、`Ki=Rs*wc`。PI 设计拒绝高于 `wc*Ts=0.3` 或 `Rs*Ts/min(L)=0.3` 的请求，且要求 `wc*(delay+Ts/2)≤π/6`。同时校验三个增益和设计采样周期下的实际 Float32 `Ki*Ts`，并检查 `(max(Kp_d,Kp_q)+Ki*Ts)*initial_error` 的电压余量。这匹配 Current 在同一拍累加积分的实现，实际稳定裕度和饱和后品质由闭环验证。 [Current 实现](../../core/src/foc_current.c#L307)

原有 10 ms Current 工况完整保留并重新运行。Full RMSE **0.152791132 A**、RUNNING RMSE **0.089168894 A**、RUNNING Tail RMSE **0.005696917 A**，历史 `CONTROL_QUALITY_FAIL` 仍成立。新增压力工况专门测试反向和饱和释放，继续调用既有 `_virtual_metrics`，RUNNING 指标只纳入 state=RUNNING 且 action=UPDATE 的行。 [原品质窗口](2026-11-16-next-control-quality-01.md) · [原整定研究](2026-11-16-next-control-tuning-01.md)

压力工况三组共享研究电机初态、200 µs H0、1.2 V 控制上限、正向 `(Id,Iq)=(0.10,0.45) A`、反向 `(-0.10,-0.45) A`、饱和 `(0.10,2.35) A`、释放 `(0,0.15) A`。前台按原接口刷新指令。设计请求 150 Hz，Ts 和最大更新延迟各 200 µs，声明初始误差 0.65 A，电压余量 1.2 V。Virtual Motor 在 H0 接受后立即使用新输出，200 µs 是设计预留。压力阶跃超出声明的初始误差域，用于考查抗饱和和恢复。

| 增益组 | Kp_d V/A | Kp_q V/A | Ki V/(A·s) | 理论首拍需求 |
| --- | ---: | ---: | ---: | ---: |
| 固定 | 1.0800 | 1.3500 | 540.0000 | — |
| 模型真值 | 1.130973 | 1.413717 | 565.4867 | 0.992429 V |
| 模型辨识 | 1.131032 | 1.413764 | 565.4867 | 0.992460 V |

| 段 / 增益 | RUNNING RMSE A | Tail RMSE A | 峰值误差 A | 超调 A | 调节 µs | 饱和 µs |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 正向 / 固定 | 0.110135 | 0.006251 | 0.450000 | 0 | 1800 | 0 |
| 正向 / 真值 | 0.107830 | 0.006113 | 0.450000 | 0 | 1800 | 0 |
| 正向 / 辨识 | 0.107828 | 0.006112 | 0.450000 | 0 | 1800 | 0 |
| 反向 / 固定 | 0.217292 | 0.006653 | 0.893724 | 0 | 2400 | 0 |
| 反向 / 真值 | 0.212889 | 0.006445 | 0.894069 | 0 | 2400 | 0 |
| 反向 / 辨识 | 0.212885 | 0.006443 | 0.894069 | 0 | 2400 | 0 |
| 饱和 / 固定 | 1.376656 | 0.622171 | 2.793455 | 0 | — | 7000 |
| 饱和 / 真值 | 1.376559 | 0.621923 | 2.793497 | 0 | — | 7000 |
| 饱和 / 辨识 | 1.376559 | 0.621923 | 2.793498 | 0 | — | 7000 |
| 恢复 / 固定 | 0.290684 | 0.003487 | 1.640132 | 1.640132 | 2200 | 400 |
| 恢复 / 真值 | 0.288961 | 0.003189 | 1.640373 | 1.640373 | 2000 | 600 |
| 恢复 / 辨识 | 0.288953 | 0.003207 | 1.640371 | 1.640371 | 2000 | 600 |

误差指标为 Iq。恢复段的超调是上一个高目标保留的初始电流相对新低目标之差。按原 ±0.1 A 容差，正向、反向与恢复段三组均在段内调节，饱和段三组均 `CONTROL_QUALITY_FAIL`，保留这一结果。各段接受 UPDATE 分别为 50/50/35/85，共 220 次、拒绝 0 次。STOP 在 44,600 µs 确认 INHIBIT，进入 DISABLED、故障码 0，UPDATE 计数保持 220。

完整压力窗口 RMSE 依次为 0.587725/0.586904/0.586902 A，RUNNING RMSE 为 0.589610/0.588785/0.588783 A。辨识候选与模型真值在该轨迹上接近，恢复调节快一拍，但比固定增益多一拍饱和，峰值变化也不全有利。原始轨迹记录 d/q 积分电压、状态与输出确认。两轴积分在记录中均为有限值。固定、真值、辨识三组的 q 轴积分在饱和段末分别为 0.43236/0.39618/0.39615 V，恢复段末分别为 0.17038/0.17011/0.17016 V。本结果支持在该模型和域内使用模型生成候选，真实带宽和更广工况的质量仍需验证。

## 5. 资源、回归与来源

| 验证 | 实际结果 |
| --- | --- |
| Core Debug | 15/15。 |
| Calibration Debug | Clang / GNU 各 2/2。 |
| Simulation Debug | 18/18，含 Runtime 11 个原用例、4 个原 Simulation 用例和新纳入的 3 个 Calibration / Virtual ID 用例。 |
| Calibration / Simulation ASan+UBSan | 2/2、18/18，错误立即退出。 |
| 格式与 Source Contract | C/H 格式通过，Source Contract 86 C/H、9 CMake、9 README。 |
| 严格 `Scope all -RequireCMakeFormatter` | 30 个 CMake 格式检查和全部软件门禁通过，独立 Legacy Host 45/45。四份 Legacy 构建均通过（3 个 carrier 预设及 `run.sh build`）。 |
| 独立审查 | 初稿的采样辨识 P1 通过定向 RED→GREEN 关闭，修订后 GNU 定向用例 2/2，最终无 actionable P0/P1/P2。 |

Arm GNU 14.3.1 `-Os -mcpu=cortex-m4 -mthumb -mfloat-abi=soft -fstack-usage` 单独编译新增翻译单元，`text/data/bss = 4018/0/0 B`。ARM 下单轴 context **216 B**，config **60 B**、result **64 B**。静态自身帧 init **112 B**、push **72 B**、finish **136 B**、make_model **56 B**、PI design **88 B**。目标静态对象有 `sqrt` 一个标准 libm 符号及 `__aeabi_d*` 软件双精度帮助函数，未分配堆。对象 text 数值未计入额外链接的通用数学帮助代码，整个辨识与设计是冷路径。锁定 V2 参考镜像仍为 **38,320 B Flash / 3,768 B RAM**，BIN SHA-256 `fd04a5c89d20f76c3e5ba2e7078f9b40b752660e3b0359e2cc9e98c63a771b62` 与基线一致。

对照 CyberBeast `robot_mc` 固定版 `2f1fb61ec081eb7a42ab8436ba8f1504fc529da7`。借鉴了 R/L 驱动增益、区分 d/q 轴，以及注意电压裁剪和采样归属的工程思路。该项目标注其测量例程源自 ODrive。FalconFOC 本轮的窗积分拟合、公共 API、回归与连续 PI 输出是独立实现，没有移入 CyberBeast 或 ODrive 代码。 [固定版 R/L 来源](https://github.com/cyberbeast0829/robot_mc/blob/2f1fb61ec081eb7a42ab8436ba8f1504fc529da7/Core/Src/calibration.c#L321) · [固定版 PI retune](https://github.com/cyberbeast0829/robot_mc/blob/2f1fb61ec081eb7a42ab8436ba8f1504fc529da7/Core/Src/foc.c#L197)

[原始证据包](2026-11-17-next-motor-id-01a/raw-evidence.zip) 保存本轮原始轨迹、完整 JSON 指标、失败回归、ARM 数据与门禁日志。用 [Calibration](../../calibration/README.md#build-and-test) 和 [Simulation](../../simulation/README.md) 的独立构建入口可复跑。`test-foc-motor-id-virtual --trace` 可重建逐拍数据。解压后，在仓库根目录运行 `python <证据目录>/reproduction/metrics-adapter.py <证据目录> <test-foc-motor-id-virtual 可执行文件路径>`，适配脚本调用现有 `_virtual_metrics` 重新生成 CSV 和 JSON。

## 6. 范围与下一步

下一阶段优先完善辨识鲁棒性和采集证据。需要确认真实转子约束及电角轴对齐、与 ADC 同一时基的端电压和生效延迟、逆变器压降与死区、采样孔径及电流噪声，并用重复捕获检查温度漂移与磁饱和。先在可确认域内比较估计偏差和实测电流环，再单独设计候选应用、失败回滚及持久化策略。

本轮 Git 差异共 11 文件。

- 新增 `calibration/include/foc_motor_id.h`、`calibration/src/foc_motor_id.c`、`calibration/tests/test_foc_motor_id.c`、`simulation/tests/test_foc_motor_id_virtual.c`。
- 修改 `calibration/CMakeLists.txt`、`calibration/README.md`、`simulation/CMakeLists.txt`、`simulation/README.md`，追加 `docs/governance/Themis_工程维护日志.md`。
- 新增本报告和 `docs/evidence/2026-11-17-next-motor-id-01a/raw-evidence.zip`。

本轮止于工作树交付。未提交、推送、刷机、上电或开展真实 PWM 激励。

```text
G3-SAFE HARDWARE NOT VERIFIED
G3-PWR LOCKED
AT32F415 WCET NOT VERIFIED
MOTOR PHYSICAL OPERATION UNVERIFIED
```
