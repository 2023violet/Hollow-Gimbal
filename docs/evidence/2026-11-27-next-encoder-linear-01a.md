# NEXT-ENCODER-LINEAR-01A｜绝对编码器周期性非线性补偿

2026-11-27。**PASS，C11 Sensor 与受控 Host 数据链验收通过。** FalconFOC 已可为单个 Encoder 实例显式准入周期 LUT。补偿同时进入单圈整数相位、多圈位置和速度。默认关闭的旧系统角度轨迹逐字节一致。开启补偿后，三组受控畸变的机械和电角 RMS 下降 **99.09%–99.79%**。爬行组速度 RMS 略有上升。

## 1. 已实现的接口与数据契约

- `foc_encoder_lut_validate()` 检查形状、CPR、非零 encoder/mount identity、幅值和所有环形线段。`foc_encoder_init_with_lut()` 显式启用，准入失败保留原会话并返回错误。`foc_encoder_correct_code()` 可查询当前实例的无方向校正码。普通 `foc_encoder_init()` 沿用原调用和默认行为。
- 表为调用方持有的只读 `int16_t` 数组，使用 **2 至 min(CPR, 1024)** 个等距节点，CPR 支持 **2–65536**。第 k 节点在原码坐标 k × CPR / N，最后一段回到第零节点。表的绝对偏移上限为 floor(CPR/8)，全周期映射斜率在 [0.5, 1.5]。插值按最近整码取值，正中向 +∞ 取整，允许相邻原码映射到同一码或跳一个码。
- 表描述符可在初始化后释放，数组在会话及 Reset 期间保持存活和不可变。实例保留指针、长度和表是否变化的标志。配置身份覆盖编码器及安装/标定版本。更换表或关闭补偿需要在停止使用该会话后重新初始化。
- 数据路径为 `raw_code → corrected_code → directed mechanical_phase_count → H1 → H0 → Current`。传输原码用来诊断并检查原码展开分支。校正后的有效码用来计算单圈机械角、`int64_t position_counts`、差分速度、LPF 与 Alpha-Beta 输入。整数相位等于累计有符号计数对 CPR 的非负模。位置首帧保留原质量语义，拒绝帧清除可发布码值，历史状态保留。H0 继续用既有预测路径，没有 LUT 计算。
- 单测覆盖同一 LUT 的正反方向、零补偿等价、100/101 CPR、输入与输出各自跨零、uint32 时间回绕、超出 Float32 整数精度后的长期计数、超幅/反向/首尾跳变拒绝、两实例配置隔离、丢帧和时间戳拒绝、无副作用失败和 Reset。

独立审查找到近半圈整数展开的 P1。CPR 100、2 节点时，可构造真实正向位移略低于半圈的情况，量化后选择错误的反向短弧。修订增加两项控制。对变化表，原码半圈或原码与校正码短弧符号冲突时拒绝。又为校正码的半圈保留 2.5 counts 裕量，覆盖两个端点各 0.75 count 量化映射误差与 0.5 count 输出取整。两个定向回归分别捕获原码和校正码短弧冲突、两者同向但同时落入错误分支，均由失败转为通过。变化表可能因此保守拒绝接近半圈的物理有效帧。常量及零表保持原分支策略。 [公开接口](../../sensors/include/foc_encoder.h) · [Sensor 集成说明](../../sensors/README.md#optional-periodic-linearization)

设计借鉴 CyberBeast 的周期 LUT 插值思路。FalconFOC 的表格式、CPR/身份绑定、准入、插值舍入与连续性保护均独立实现。 [robot_mc 固定版参考](https://github.com/cyberbeast0829/robot_mc/blob/2f1fb61ec081eb7a42ab8436ba8f1504fc529da7/Core/Src/position_sensor.c#L116-L120)

## 2. Host 验证与效果

Host 在虚拟编码器量化前，向原码坐标添加 `A1*sin(θ+0.41) + A3*sin(3θ−0.23)`。量化走现有 Virtual Motor 接口，真实机械角和 ADC 电流来自同一 Plant 状态。表的 64 节点由独立双精度二分反演连续模型生成，与被测整数插值无共用求值代码。Plant 的机械状态和 α/β 电流由夹具规定，统一使用 1 A α 轴电流，使三臂的电角和 d/q 差异可归因于编码器。实跑虚拟编码器发布、H1 整数电角、H0 预测、Virtual ADC、Current Park 和更新接受；记录范围为实际数据链上的误差，电机自主闭环动态在本轮未测。

- A 为理想编码器、关闭表。B 为注入畸变、关闭表。C 为完全相同畸变、启用匹配表。D 是单正弦工况的额外反证，对理想编码器启用原畸变的表。
- 单正弦工况 25 rad/s、CPR 65,536、4 极对和 0.035 rad 基波。谐波反向工况 −40 rad/s、反向配置、CPR 50,001、7 极对。爬行工况 +0.9 rad/s、CPR 16,384、11 极对。后两组使用 0.028 rad 基波及 0.011 rad 三次谐波。
- 每次经过约 1.1 圈。H0 基准 200 µs。H1 基准 500 µs，按固定偏移 0/7/19/3/13 µs 改变采样时刻。有效电角预测年龄最高 500 µs。三组每臂分别有 1,378、859、38,393 个被纳入指标的 H0 更新，均按期接受且无故障或拒绝。各臂均有两个环绕穿越。
- 角度以环形最短差计算。机械量来自实际整数位置，电角由测得 d/q 反解并对照独立 Plant 电角，电流以同一真值角度下的 Park 结果作参考。速度 RMS 排除起始 20 ms 的滤波建立区间。 [虚拟量化路径](../../simulation/src/foc_virtual_current.c#L134) · [控制链验收](../../simulation/tests/test_foc_periodic_chain.c#L1150)

| 工况 | 机械 RMS A / B / C，mrad | 机械峰值 B → C，mrad | 电角 RMS A / B / C，mrad | 电角峰值 B → C，mrad |
| --- | ---: | ---: | ---: | ---: |
| 单正弦 | 0.0276 / 24.574 / 0.0510 | 35.025 → 0.137 | 0.121 / 98.501 / 0.222 | 140.196 → 0.626 |
| 谐波反向 | 0.0361 / 21.636 / 0.0924 | 35.626 → 0.275 | 0.283 / 152.169 / 0.687 | 250.890 → 2.103 |
| 爬行 | 0.1107 / 21.649 / 0.1857 | 35.796 → 0.593 | 1.362 / 238.157 / 2.178 | 394.187 → 7.140 |

| 工况 | 机械 / 电角 RMS 降幅 | 原始速度 RMS B → C，rad/s | LPF 速度 RMS B → C，rad/s | d/q 电流 RMS B → C，mA |
| --- | ---: | ---: | ---: | ---: |
| 单正弦 | 99.79% / 99.77% | 0.62157 → 0.11792 | 0.61650 → 0.01804 | 70.156 / 69.058 → 0.158 / 0.157 |
| 谐波反向 | 99.57% / 99.55% | 1.24366 → 0.17208 | 1.21003 → 0.02896 | 106.030 / 108.793 → 0.485 / 0.486 |
| 爬行 | 99.14% / 99.09% | 0.29216 → 0.31855 | 0.05638 → 0.06016 | 166.516 / 168.905 → 1.510 / 1.570 |

三个 C 臂的接缝位移最大误差为 **0.055 mrad**，无虚假整圈跳变。两组较高速度的速度扰动下降，爬行原始和 LPF RMS 分别增加 **9.03%** 和 **6.70%**。低速少码步进时，校正后重复码、跳码和整码舍入影响差分速度，即使整体角度误差下降。D 的机械和电角 RMS 达到 **24.504 / 98.215 mrad**，说明形状合法的表若用在不匹配的传感器条件下可以降低质量。以上幅值为研究夹具参数。

## 3. AT32F415 Soft-float 静态代价

Arm GNU 14.3.1，Cortex-M4 Thumb，`-Os -mfloat-abi=soft -fstack-usage`，同参数比较。统计为编译和链接结果。

| 资源 | 基线 | 本轮 | 增量 |
| --- | ---: | ---: | ---: |
| Encoder 实例 RAM | 80 B | 96 B | +16 B，启用与关闭相同 |
| Feedback 结构 | 52 B | 56 B | +4 B |
| Encoder 翻译单元 text | 1,724 B | 2,206 B | +482 B，含新冷路径公共 API |
| update 符号 text | 1,032 B | 1,088 B | +56 B，另外调用共享子函数 |
| update / init / reset 静态自身栈 | 72 / 56 / 56 B | 88 / 56 / 72 B | +16 / 0 / +16 B |
| 锁定 V2 固件 Flash / RAM | 38,320 / 3,768 B | 38,520 / 3,784 B | +200 / +16 B，Board1 仍关闭 LUT |

ARM 对象的浮点辅助调用集合一致，无新增 libm 依赖。启用路径每次插值两次表读，一次 UDIV 和一次 SDIV、乘法/乘减、范围归一化及歧义检查，跟节点数无关。关闭路径通过空表分支直接取原码。LUT helper 的静态自身栈为 16 B，短弧 helper 为 8 B。栈帧是逐函数数据，MCU 完整调用峰值和最坏时延待证。

独立 ARM 链接夹具中，本轮关闭时 text 为 3,434 B，启用的链接固定开销为 240 B，再加每节点 2 B 的只读表。16、64、128、1024 节点的表占用分别为 32、128、256、2,048 B。常量表可放 ROM，实例无需再分配表 RAM。描述符在 ARM 上为 16 B，调用方可在初始化后让其退出生命周期。冷路径准入为 O(N)，每帧为 O(1)。锁定固件 BIN 哈希由 `fd04a5c89d20f76c3e5ba2e7078f9b40b752660e3b0359e2cc9e98c63a771b62` 更新为 `736a0eada7e43439e9d981f2649e144785eccdc50b7c01fa61f3fc7e5ee3d5b4`。

## 4. 回归、工作树和后续

- Core **15/15**、Sensors Clang 与 GNU 各 **2/2**、Runtime **11/11**、Simulation **18/18**、Calibration **2/2**。Sensors 和 Simulation ASan/UBSan 分别 **2/2、18/18**。
- C/H 格式、Source Contract（86 C/H、9 CMake、9 README）和严格 `Scope all -RequireCMakeFormatter`（30 项格式、Legacy Host **45/45**）通过。Legacy 三份预设及 `run.sh build`、锁定 V2 ARM 构建通过。
- 关闭 LUT 的旧系统电角测试 **2,587 条**数据逐字节一致。Motor-ID 的 **677 行**拟合和 PI 轨迹与 01B 归档一致。
- 独立复核确认近半圈 P1 已修复，GNU 短枚举的两项 Sensors 用例与控制链用例均通过，最终无 P0/P1/P2。 [原始数据和日志](2026-11-27-next-encoder-linear-01a/raw-evidence.zip)

工作树基线为 `codex/falconfoc-v2-vm01a` 的 `f9af51be41aa21ad11cb66444d6e7664ae93213d`，本地与只读远端查询一致。开工前快照 13 个待提交文件并登记哈希。12 项保持原哈希，维护日志保留全部原字节和新增条目。未暂存、提交或推送。

本轮修改 8 个现有文件。

- `sensors/include/foc_encoder.h`、`sensors/src/foc_encoder.c`、`sensors/tests/test_foc_encoder.c`、`sensors/README.md`。
- `simulation/include/foc_virtual_current.h`、`simulation/src/foc_virtual_current.c`、`simulation/tests/test_foc_periodic_chain.c`。
- `docs/governance/Themis_工程维护日志.md`。

新增本报告和证据 ZIP。从 HEAD 起合计 **22 个**待提交路径，完整清单在证据 ZIP 的 `verification.json`。旧 Motor-ID 两轮交付继续随同工作树保留。

下一阶段优先开发 LUT 标定生成与独立验收。用独立参考角覆盖整圈和多速度，留出一组数据验证插值误差与速度纹波，隔离电角零点、装配变化和重复标定误差，再决定是否调整速度估计。当前结果支持优先取得可追溯的 LUT，爬行速度的量化结果还不足以决定引入 PLL。实机参数、传感器传输时序与目标耗时仍待证。本轮止于 Host 和目标静态验证。

```text
G3-SAFE HARDWARE NOT VERIFIED
G3-PWR LOCKED
AT32F415 WCET NOT VERIFIED
MOTOR PHYSICAL OPERATION UNVERIFIED
```
