# NEXT-MOTOR-ID-01B｜R/L 辨识采样可信度与非理想工况

2026-11-27。**PASS，生产侧观测准入与受控 Host 验证通过。** 辨识会话现在绑定采集设置、采集 ID、来源、端电压类别和作用区间。不合格观测在累积前使会话失效。窗口拟合与 PI 计算沿用 01A。

| 交付状态 | 范围 |
| --- | --- |
| IMPLEMENTED | C11 观测准入、失败原子性、逐轴结果溯源和双轴组合准入。 |
| VERIFIED ON HOST | 原 01A 轨迹等价，端电压、角度和时间错配实验、跨实例与 Reset 回归。 |
| NEEDS HARDWARE EVIDENCE | 实际端电压校准、ADC/PWM 时间关联、转子约束、电角零点、测量不确定度和目标实时性。 |
| NOT IMPLEMENTED | 真实标定或板级补偿器、运行时 PI 应用与回滚、Flash 持久化。 |

## 1. 工作树与 01A 基线

本轮从 `codex/falconfoc-v2-vm01a` 的已验收本地实现开始。本地 HEAD 与只读远端查询均为 `f9af51be41aa21ad11cb66444d6e7664ae93213d`。修改前逐文件快照了全部 11 个未提交文件，并记录 SHA-256。01A 的报告和原始证据包保留原哈希。现有两个 CMake 文件也保留原哈希。构建产物在工作树外。归档日期用于报告文件名，原始测试日志保留工作站时钟。 [01A 验收报告](2026-11-17-next-motor-id-01a.md)

01A 的三组静止 Virtual Motor 原场景在 01B 重跑后，`--trace` 的 677 行逐行与基线相同。两轴累计仍为 769 个端点、96 个窗口。原场景最大逐轴 Rs/L 误差为 **0.2724% / 0.2286%**。报告附的逐拍轨迹保留 FIT、REJECT、GAINS 和 PI 行，覆盖三组增益的接受、饱和、积分恢复与 STOP。

## 2. 从电压请求到辨识输入

| 电压层次 | 语义与准入 |
| --- | --- |
| PI 请求 | Current 计算的 d/q 请求，可能经过电压矢量限制。 |
| SVPWM 限幅后等效电压 | 调制器给定的等效矢量，反映计算和限幅。 |
| PWM 已接受且生效 | 需要区分计算、提交、接受和生效时刻；有效 duty 也只提供推算端电压的输入。 |
| 实际等效端电压 | 需要覆盖实际作用波形，包含母线、死区和器件压降影响。 |
| 可信端电压观测 | 独立测量或经独立校准可信重建的实际轴向端电压区间平均。Host 使用 Virtual Motor 的有效输入观测。 |

01A 的辨识实验直接调用 `foc_virtual_motor_step()`，赋给样本的电压来自向 RK4 Plant 传入的固定轴电压。标称组用 Plant 电流端点，扰动组用 Virtual ADC、Device 解码和 Clarke，并对该有效电压观测加量化和噪声。辨识环节没有经过 PI、SVPWM 或 PWM 事务。另一个用于 PI 对比的闭环场景确实经过 Current、Runtime 和虚拟 duty 应用，它与开环辨识采样分属两条路径。 [Current 实现](../../core/src/foc_current.c) · [Virtual Motor](../../simulation/src/foc_virtual_motor.c#L177) · [Virtual Current](../../simulation/src/foc_virtual_current.c#L60)

## 3. 生产采样契约

- **身份。** 每样本提交 `setup_id`、`capture_id` 和采集来源。非零 setup ID 指向电机、固定 d/q 坐标与采集校准。每次新采集使用独立 ID，由采集方管理跨 Reset 和仍在途的数据。身份变化触发 `INPUT_CHANGED`。
- **时间。** 电流时间为 ADC 采样孔径时刻。电压携带实际覆盖的半开区间 `[begin,end)`，必须逐端匹配 `[上一个 ADC 孔径, 这个 ADC 孔径)`。连续序号、同一单调微秒时基、原最小和最大间隔继续适用。检查支持 uint32 回绕，时基重启和整周期以上空档由采集方排除。错配触发 `TIME_ORDER`。初始端点不积分电压。
- **电压。** 所有有效区间都需确认输出在区间内已生效，电压来源与会话配置匹配。Virtual 只接收虚拟端电压类别，Measured 接收实测或独立校准类别。请求、电压作用未确认、已知限幅或来源缺失使会话失效。结果的 `voltage_source` 是配置策略，也是所有已准入区间的类别。
- **物理前提。** `frame_confirmed` 表示采集方已验证电压与电流使用同一固定、对齐且符号一致的 d/q 坐标。`stationary_confirmed` 代表采集方对全区间静止的证据，另外要求观测电角速度为零。实测应把证据绑定到 setup ID。单个布尔字段和零速读数都无法独立证明机械锁止或角度正确。
- **发布与 PI。** 新检查先于全部累积。错误后可读取诊断和已接收计数，R/L 均为零，Reset 开始新采集。双轴组合要求有效 d/q、同 setup、同来源、同端电压类别和不同 capture ID，再检查两轴 Rs 一致性。调用方随模型保留两份原始结果，包含时间边界、残差、激励与 provenance。直接输入已知模型的 PI 接口保留，仍由调用方对参数可信度负责。

新增字段属于 C 接口布局变更，01A 调用方需重新编译并供应观测元数据。拟合保留窗口积分、激励秩、残差、物理边界及采样分辨率限制。 [公开 API](../../calibration/include/foc_motor_id.h) · [集成说明](../../calibration/README.md#standstill-motor-rl-and-pi-design)

## 4. 非理想端电压与重复性

使用已有 RK4 PMSM、Virtual ADC 和 Device 采样链。研究电机 Rs = 0.6 Ω、Ld = 1.2 mH、Lq = 1.5 mH，12 V 母线，80 μs 采样加有界 ±12 μs 抖动。测试用可配置有效电压增益，A 中为 1，B 和 C 为 0.8，同一激励，B 和 C 每次读取同一套电流端点。它代表一个有意设置的线性电压传递误差。当前件不为产生这项偏差而修改 Virtual Motor 的生产接口或默认值。观测使用 2 mA/count 的 ADC 配置、±1 mA 电流噪声、4 mV 电压量化、±4 mV 电压测量噪声。每种工况用三组种子 123、2027、5179 独立复跑两个轴。接受规则为每轴 769 端点、96 窗口，Rs 和 L 误差均小于 2%，沿用 18 mV 残差及 0.02 激励门槛。误差门槛只用于可知真值的 Host 测试。

| 场景 | usable 数 | Rs 误差范围 | Ld 误差范围 | Lq 误差范围 | 残差 mV |
| --- | ---: | ---: | ---: | ---: | ---: |
| A，理想接收电压 | 6/6 | +0.118% 至 +0.165% | +0.057% 至 +0.105% | +0.038% 至 +0.092% | 2.849–3.589 |
| B，非理想 Plant，按未验证类别提交请求电压 | 0/6 | — | — | — | 未拟合 |
| C，非理想 Plant，可信有效端电压 | 6/6 | −0.043% 至 +0.021% | −0.035% 至 +0.026% | −0.041% 至 +0.026% | 2.340–3.128 |
| 反证，B 的请求电压故意误标为可信端电压 | 6/6 | +24.944% 至 +25.036% | +24.973% 至 +25.031% | +24.963% 至 +25.031% | 2.737–3.650 |

C 组重复采集的 d/q Rs 峰峰跨度分别是 0.174 mΩ、0.270 mΩ，Ld/Lq 跨度分别是 0.725 μH、1.008 μH。每个成功拟合的窗口激励行列式为 0.99978–0.99988。B 组确认拒绝实际提交的类别 0，输出 CSV 的 `configured_voltage_source` 保留会话预期的类别 1。错误贴标签时，拟合接近平凡缩放关系 `v_request = v_terminal / 0.8`，拟合能以 L 和 Rs 同比放大来吸收系统误差，残差仍小。时序和标签的一致性检查无法判断来源是否说真话。

## 5. 转子、坐标、时间和失效验证

- **轴与符号。** Plant 使用 `vd = cos(θ)*vα + sin(θ)*vβ`，`vq = cos(θ)*vβ − sin(θ)*vα`，电角速度为极对数乘机械速度。Current 使用同旋转约定。角度实验用两个各自静止的 RK4 单轴轨迹组成固定角度的电气观测，再对电流和电压一起旋转。该夹具隔离了角度混叠，真实自由转子的同时 d/q 激励仍可能产生磁阻转矩。
- **角度。** 正确对齐的 d/q 拟合为 Rs 0.600787/0.601094 Ω、Ld/Lq 1.201261/1.501601 mH。已知对齐无证据时在首个作用区间拒绝。误称已对齐的 0.1 rad d 轴观测仍在 14.422 mV 残差下给出 0.595037 Ω、1.204428 mH，误差 −0.827% 和 +0.369%。误称已对齐的 0.4 rad 观测产生 52.328 mV 残差，拒绝并清零。两个角度为检测能力样例。
- **时序。** 六个早移 5 μs 的配对均在第 17 个有效端点后拒绝，其中已完成 2 个窗口且参数为零。单测还让时间范围跨 uint32 回绕，分别改变区间起点和终点。
- **其他拒绝。** 复用 01A 的自由转子 q 轴实验，保留永磁磁链后首个区间电角速度约 0.00027565 rad/s，当场拒绝。零激励、恒定电流与单指数共线三个不可辨识夹具均拒绝。源码级用例覆盖来源/会话切换、输出未确认、帧和静止预条件、有效数据后拒绝、统计量不变、Reset 后重获完整候选及多实例隔离。上述拒绝率是对指定测试夹具的计数。
- **PI。** 窗关闭、R/L 最小二乘和 PI 函数体与 01A 相同。合法采集命中原来的增益值和 220 次已接受 UPDATE、0 次拒绝、STOP 后 DISABLED。受控反例使两轴模型组合失败，随后 PI 接口拒绝清零模型。原负面结论保留，模型候选恢复达标快 200 μs，同时多饱和 200 μs。候选由调用方显式评估，生产 Current、Runtime、Pipeline、Safety 和 Board1 固定参数保持原样。

## 6. ARM Soft-float 与回归

同一 Arm GNU 14.3.1、Cortex-M4 Thumb、C11、`-Os -mfloat-abi=soft -fstack-usage` 编译比较。

| 指标 | 01A | 01B | 增量 |
| --- | ---: | ---: | ---: |
| 单轴 context | 216 B | 224 B | +8 B |
| 单样本结构 | 24 B | 52 B | +28 B，采集方提供 |
| config / result | 60 / 64 B | 64 / 72 B | +4 / +8 B |
| 对象 text / data / bss | 4,018 / 0 / 0 B | 4,150 / 0 / 0 B | +132 B text |
| 静态帧 init / push | 112 / 72 B | 120 / 64 B | +8 / −8 B |
| 静态帧 finish / make_model / PI | 136 / 56 / 88 B | 136 / 56 / 88 B | 0 B |

新增准入为整型、布尔检查和字段传递。ARM 对象中的 Double Soft-float 辅助集合与 `sqrt` 保持一致，编译器新增对 `memcpy` 的引用。资源增量限于标定冷路径和调用方携带的样本元数据，实际采集缓冲区大小由调用方确定。静态帧是函数自身，未计完整调用链。锁定 V2 参考固件仍为 **38,320 B Flash / 3,768 B RAM**，BIN SHA-256 仍为 `fd04a5c89d20f76c3e5ba2e7078f9b40b752660e3b0359e2cc9e98c63a771b62`。

- 新用例在旧累积实现上复现异会话样本被接受，更新准入后通过。
- Debug 实跑 Core 15/15、Calibration Clang 和 GNU 各 2/2、Simulation 18/18、Runtime 11/11、G3-NP Device 2/2。Calibration 和 Simulation ASan/UBSan 分别 2/2 和 18/18。
- C/H 格式、Source Contract（86 C/H、9 CMake、9 README）、强制 CMake 格式的 `Scope all`（30 个格式检查、Legacy Host 45/45）通过。三份 Legacy carrier 预设及 `run.sh build` 通过。
- 独立复核无 P0/P1。配置电压类别的命名、采集 ID/共享时钟及溯源文档、Host 端电压增益参数三项 P2 已修正。复核在隔离 GNU Debug 构建中通过定向 2/2，最终无 P0/P1/P2。
- `test-foc-motor-id-virtual --quality` 复跑 35 条观测资格结果，同种子的两次输出完全相同。`--trace` 复跑旧流程。原始数据、保护清单、资源日志与验收日志见证据包。 [原始证据包](2026-11-27-next-motor-id-01b/raw-evidence.zip)

## 7. 硬件证据与待提交范围

下一阶段优先取得同步采样证据。需要在独立授权的台架验收中确认转子约束与位移观测、电角零点和相序、ADC 采样孔径与 PWM 实际生效边界、经过温度与电流方向扫描的端电压测量或校准重建，以及在不同条件下重复辨识的误差。参数应用、回滚与保存应在这些证据之后单独验收。

本轮相对 01A 修改 7 文件。

- `calibration/include/foc_motor_id.h`、`calibration/src/foc_motor_id.c`、`calibration/tests/test_foc_motor_id.c`、`calibration/README.md`。
- `simulation/tests/test_foc_motor_id_virtual.c`、`simulation/README.md`。
- `docs/governance/Themis_工程维护日志.md`。

新增 2 文件，为本报告与 `docs/evidence/2026-11-27-next-motor-id-01b/raw-evidence.zip`。从 HEAD 起的完整待提交范围共 13 文件，还包括保持原哈希的 01A `calibration/CMakeLists.txt`、`simulation/CMakeLists.txt`、`docs/evidence/2026-11-17-next-motor-id-01a.md` 和 `docs/evidence/2026-11-17-next-motor-id-01a/raw-evidence.zip`，以及前述 9 个本轮路径。工作止于本地工作树交付。

```text
G3-SAFE HARDWARE NOT VERIFIED
G3-PWR LOCKED
AT32F415 WCET NOT VERIFIED
MOTOR PHYSICAL OPERATION UNVERIFIED
```
