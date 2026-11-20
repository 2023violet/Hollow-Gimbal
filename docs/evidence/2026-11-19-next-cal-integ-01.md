# NEXT-CAL-INTEG-01｜FalconFOC Host 电机自动标定与 FOC 集成

2026-11-19

**CAL-INTEG-01 SOFTWARE END-TO-END PASS**

**PHYSICAL CALIBRATION NOT VERIFIED**

基线为 `D:/study/Software/Hollow-Gimbal-vm01a`、`codex/falconfoc-v2-vm01a`、`0d6ca149612f097992e597cc8d8cdf323c30dd56`。起始工作树干净，HEAD 与本地远端跟踪引用一致。交付保存在工作树。本轮验证使用 Host 和静态交叉编译。 [原始运行记录、采样、配置与回归](2026-11-19-next-cal-integ-01/raw-evidence.zip)

## 1. 基线缺口与交付

| 基线状态 | 起始能力与缺口 | 当前交付 |
| --- | --- | --- |
| IMPLEMENTED | ADC 零偏、单轴 R/L、Rotor、Offset、Encoder LUT、J/B 和 K/D 估计器。 | 保留估计器和原生质量判据。两种虚拟配置用独立可观测量走同一路径。 |
| INTEGRATED | Workflow 管样本、身份、资格和审核。Application 将 ADC、Rotor、LUT、合格 Kt 装入新的 Host Mock 实例。 | 应用扩展为初始化期绑定现有 Virtual Motor 输出适配器。配置和 Encoder 读回后，运行实际 H0/H1/Pipeline/Safety。 |
| ESTIMATOR_ONLY | 已有 `foc_motor_id_make_model()` 和 `foc_motor_pi_design()`。工作流只保存一轴，PI 候选未进入控制。 | 可选双轴契约 保存两条结果、采集和时间证据。审核通过后安装独立 d/q Kp、共享 Ki、速度 PI、位置 Kp、Kt；K/D 经正式 Impedance 命令消费。 |
| MISSING | 主动采集、磁链、齿槽补偿、统一命令、完整的 Host 保存与重载。 | 前台采集会话、磁链拟合、可选 Host 齿槽托管、`falconfoc-cli calibrate` 和 FCAL v1。 |
| BLOCKED | Board1 的独立关断、物理 PWM 装载、真实测量和 Flash 应用。 | 锁定镜像复建字节一致，设备层的实板限制接续 PRODUCT-01。 |

[PMSM 虚拟采集会话](../../simulation/src/foc_virtual_calibration.c) · [工作流合同](../../calibration/WORKFLOW.md) · [显式应用与重载准入](../../control_plane/include/foc_calibration_application.h) · [Board1 准入](2026-11-19-next-product-01.md)

前台持有一份禁止输出的原 Runtime 和一台独立模拟试验电机。正常顺序为 ADC 零偏、LUT 训练和独立 Holdout、Rotor、Offset、d 轴、q 轴、Flux、J/B、K/D 与预览。可选齿槽扫描使用同一会话的排他检查和独立时间预算。STOP、状态/身份变化或到期时，采集在下一次受控激励前停止，撤销 Workflow 及独立磁链/Kt 资格。测试在 d 轴第 2 个端点发 STOP，最后保留的采集长度为 2，未继续提交下一端点。旧 Runtime 的参数与输出所有权独立于新实例。

## 2. 采集质量与模型真值对照

原始 ADC 由 Plant 电流映射到 ADC Rank，再叠加偏置、量化和一计数的确定性扰动。48 个独立零电流观测给出 A/B/C **2090、2030、2024 count**，代替原 Runtime 的 **2048、2048、2048 count**。LUT 使用 512 条训练和 512 条反向独立 Holdout，保留 32 个节点。两配置均由成对相位观测判得 4 极对，Encoder 的机械正向在原始映射和工作流相对方向合成后进入新实例。Salient 使用反向原始码和 ACB 采样顺序，另有非零 B/C 电流的 Rank 与解码检查。虚拟适配器按规范相占空比驱动模型，实际比较通道接线仍属于板级见证。

| 参数 | Generic 真值 → 估计 | Salient 真值 → 估计 |
| --- | ---: | ---: |
| Rs Ω | 0.600000 → 0.600018 | 0.780000 → 0.780003 |
| Ld mH | 1.200000 → 1.200988 | 1.700000 → 1.700147 |
| Lq mH | 1.500000 → 1.500658 | 2.200000 → 2.198496 |
| 磁链 λ Wb | 0.0250000 → 0.0250060 | 0.0320000 → 0.0319731 |
| Ke V/(机械 rad/s) | 0.100000 → 0.100024 | 0.128000 → 0.127892 |
| Kt N·m/峰值 q 轴 A | 0.150000 → 0.150036 | 0.192000 → 0.191839 |
| J kg·m² | 0.00200000 → 0.00200005 | 0.00320000 → 0.00320012 |
| B N·m·s/rad | 0.00600000 → 0.00600067 | 0.00900000 → 0.00900052 |
| 电角零点 rad | 0.370000 → 0.369966 | 0.520000 → 0.520006 |
| Encoder LUT Holdout RMS rad | 0.00005893 | 0.00006091 |

- **R/L。** 独立轴约束在虚拟积分器全步生效。每组均输出 769 个实测端点。Capture 6 为 2,147,400–2,185,800 µs，Capture 7 为 2,186,000–2,224,400 µs。每个有效电压覆盖上一 ADC 孔径至本孔径。Generic d/q 拟合残差 2.47/2.60 mV，Salient 3.21/3.97 mV，各归一化激励度均超过 0.9999。独立 Rs 一致后才形成联合模型。
- **磁链。** 通过两个幅值、正反电角速度、观察端电压和实际虚拟 ADC 电流求得 513 个端点的反电势拟合。残差为 Generic 0.699 mV、Salient 0.688 mV。Ke 采用相对中性点的相峰值电压，`Ke=p·λ`。Kt 采用 `Id=0` 时的每峰值 q 轴安培磁体转矩，`Kt=1.5·p·λ`。Kt 来源为独立于 J/B 的磁链采集，转矩仪的等效轴转矩仅用于 J/B。
- **J/B。** 电机轴坐标下实际正反加减速，采集等效电磁转矩、有符号负载和带量化噪声的速度。负载含正弦项和平滑库仑摩擦。Generic 和 Salient 分别取得 0.290 mN·m 和 0.478 mN·m 的等效转矩残差。缺少合格参考或转矩来源时拒绝。这里 J/B 对应电机轴等效惯量和拖曳，减速器输出轴与负载反射需另外辨识。
- **齿槽。** 32 节点固定容量。正反等速扫描消去方向相关阻力，用跨圈同相位样本辨识线性负载趋势。错位网格 Holdout 对照。统一 Generic 入口的转矩 RMS 从 8.485 mN·m 降到 0.345 mN·m。独立专项的 ±1.4 rad/s 试验中，速度 RMS 从 0.5070/0.5051 降到 0.02747/0.02769 rad/s，净轴转矩纹波 RMS 从 6.456/6.290 降到 0.614/0.612 mN·m。错误身份、错误坐标、无效 LUT 和越界方向走现有无补偿动作。阻抗补偿和显式前馈共享原有上限。

## 3. 审核、配置与正式控制见证

`CANDIDATE_PREVIEW` 呈现拟合量和增益。`--approve-candidate <review_id>` 审核同一 FCAL 规范化负载的 CRC32 指纹。失配在初始化前拒绝。载入包可先检视，审核后按本地采样、延迟和限幅策略重算 PI、速度、位置与 K/D，并核实保存值。CRC32 用于数据完整性和操作审核标识。成功控制后才保存新文件，输出明确区分 Host 保存和 MCU Flash。FCAL v1 具有版本、Motor/Encoder/Frame/采集身份、参数范围、独立采集 ID 和两个 LUT。UART V1、Measurement Trace v1 与生产 Config Store Schema 2 沿用各自格式。

| Runtime 使用项 | Generic 应用读回 | Salient 应用读回 |
| --- | ---: | ---: |
| Current Kp d/q，V/A | 0.271657 / 0.339441 | 0.384564 / 0.497288 |
| Current Ki，V/(A·s) | 135.720779 | 176.432526 |
| Velocity Kp / Ki | 0.365252 / 4.812311 | 0.460195 / 6.021955 |
| Position Kp，s⁻¹ | 3.600000 | 3.600000 |
| Impedance 命令 K / D | 0.450012 / 0.042001 | 0.720027 / 0.067802 |
| Rotor 与 ADC | 4 极对，0.369966 rad，2090/2030/2024 | 4 极对，0.520006 rad，2090/2030/2024 |
| Encoder / 齿槽 | Encoder 32 点，齿槽 32 点 | Encoder 32 点，齿槽关闭 |

应用时电压和电流限制仍归原 Pipeline。设计策略使用 36 Hz 电流带宽、360 µs 最坏采样周期、260 µs 更新延迟及保守电压和初始误差余量。36 Hz 是这两个 Host 候选的设计值。速度和位置外环用 J/B、Kt、H1 周期和回路带宽分离约束验证。Rs/Ld/Lq 在此进入 PI 设计，λ/Ke 作为诊断与持久化数据、λ 派生的 Kt 用于整定和阻抗转矩换算。J/B 通过增益和 K/D 生效。真实电机适用参数和设备质量仍待测。

初始化使用现有虚拟 Plant/Runtime 边界，不修改已启动的实例。候选使用原 Runtime 的可信配置作为基底，应用回调在 Runtime 初始化前绑定。故障试验故意停止更新命令，见证 `COMMAND_TIMEOUT`、INHIBIT、新会话清除、显式 ARM/START、反向目标恢复，最后 STOP 无新增 UPDATE。两组从 8 个已确认 UPDATE 进入故障/清除，再重臂至 11 个，最终停止。保存和重载的四模式双向对照逐项一致。

## 4. 控制品质及反例

下表为 Generic 使用相同扰动模型时的最后 25% 误差 RMS。Current、Velocity、Position 和 Impedance 分别使用 A、rad/s、rad、rad。每个方向都做未标定/标定两次独立启动，JSON 同步保存整段 RMS、超调、最后超差时刻、饱和数、故障和 STOP。

| 模式 | 负向标定前 → 后 | 正向标定前 → 后 |
| --- | ---: | ---: |
| Current ±0.45 A，60 ms | 0.044185 → 0.019907 | 0.005356 → 0.022678 |
| Velocity ±1.5 rad/s，700 ms | 0.324788 → 0.003355 | 0.463563 → 0.003083 |
| Position ±0.25 rad，1.4 s | 0.003618 → 0.003447 | 0.005219 → 0.003401 |
| Impedance ±0.18 rad，1 s | 0.022696 → 0.000128 | 0.034381 → 0.000394 |

两个新试验配置在声明包线内均为 `CONTROL_QUALITY_PASS`，四模式正反试验的标定后饱和计数为零。改善不均匀。Generic 正向 Current 的整段 RMS 从 0.02892 A 增至 0.08310 A，低带宽候选使短窗瞬态指标退化。正向 Position 最后超差时刻从 0.277 s 延至 0.630 s，尾段误差与超调在该对照下降。Salient 正反结果和所有参考窗口见 JSON。

历史工况按原参数与阈值重跑。**10 ms Current 仍为 `CONTROL_QUALITY_FAIL`**，整窗 RMS 0.152791 A，RUNNING 最后阶段 RMS 0.005697 A。Servo-01C Holdout 仍为 **7 PASS、3 QUALITY_FAIL、1 个预期时序拒绝**。标称/高阻尼的两项 q 请求纹波超限，以及 0.25 A 电流限制下 0.039 N·m 的持续负载不可达，均保留原失败标签。 [旧 Current 口径](2026-11-16-next-control-quality-01.md) · [旧 Servo 口径](2026-11-18-next-servo-01c.md)

## 5. 回归、成本与复现

| 验证 | 结果 |
| --- | --- |
| Clang 22.1.8、MinGW GCC 16.2、Clang ASan/UBSan | 每条工具链均通过 Core 20/20、Runtime 13/13、Calibration 8/8、Sensors 2/2、Control Plane 5/5、Simulation 40/40、Servo 自身 7/7、Protocol 1/1、锁定 Board Host 9/9、旧 Workflow 示例 1/1、Measurement Trace 回放 1/1。独立工程与 Simulation 内嵌套测试有重叠。 |
| 失败路径 | 覆盖无审核、不同 review_id、不同电机、CRC/版本不符、重算 CRC 后的 NaN 和非法外环 Ki、已初始化存储、适配器拒绝 INHIBIT、两轴数据不全、端点/孔径不符、会话撤销、STOP/FAULT、超时、无合格参考、错误或失效的 LUT、激励不足。旧 Trace 回放测试覆盖异步孔径拒绝。 |
| Python CLI | 标准库 unittest 11/11，配置了锁定串口模拟器和 Virtual Device。新 CLI 的认证、两配置采集/复载、保存及负向路径额外由 Simulation CTest 执行。 |
| Source Contract 与严格 `Scope all -RequireCMakeFormatter` | 175 个 C/H、15 个 CMake、15 个 README 通过。严格总门禁通过，含 MinGW Legacy Host 45/45。独立 Legacy `./run.sh build` 通过。 |
| ARM GNU 14.3.1 soft-float | Portable Workflow 和 Application 均以 Cortex-M4 Thumb、soft-float、`-Werror` 独立编译。默认锁定参考镜像 Flash 43,836 B / RAM 4,176 B，BIN SHA-256 `C8648A7E6EBB92C02C60ACF277CDF9F1E9634AC15967965BDA7A0D2181D15C5E`，与基线字节一致。链接仍发出已有 syscall stub 与 RWX LOAD 提示。 |

新增和变更 C/H/Python 的产品行数为 4,306 新增、90 删除，测试为 1,545 新增、1 删除。Clang x64 会话 `sizeof` 为 **10,048 B**，Virtual Current 驱动为 **2,840 B**，Host Runtime 的齿槽配置增加 168 B。两个 FCAL 包均为 **548 B**。采集、拟合、文件编解码和审核在 Host 前台执行，齿槽控制只在 Host H1 做定长表双点插值与限幅。ARM 产物保持既有二进制，资源数据不用于推定 WCET。

从仓库根使用 [CLI 构建和三步命令](../../tools/falconfoc-cli/README.md#host-motor-calibration-and-foc)。运行普通 Simulation 构建后以 `ctest --test-dir build/virtual-cal -R '^test-foc-virtual-calibration-cli$' --output-on-failure` 复验。负向会话与电气测试名为 `test-foc-virtual-calibration-session` 和 `test-foc-virtual-calibration-electrical`。证据包包含实际 d/q 端点 CSV、两份配置与应用/恢复 JSON、取消和错误审核日志、历史品质结果、工具链日志与逐文件 `sha256.json`。压缩包 SHA-256 为 `b574e30bd82de42fa24901979d4e06a93440f29359d5f8099d1686e99fb6e1b7`。

复核发现并处理了已启动实例可能被通用 initializer 清零、审核与预览未绑定、重载外环增益仅做范围检查和 STOP 后继续收集整段的问题。最终用冷存储准入、候选指纹、按本地策略复算增益和逐激励检查覆盖上述路径。回放脚本首次使用了错误的 CTest 名称过滤器，改用真实注册目标后，三个工具链各通过 1/1。

## 6. 当前产品边界

- 统一流程支持两个声明了原始相序和机械身份的 Host 电机配置。任意新电机的探测策略、未知机械参考、跨负载和机构坐标鲁棒性需要新的可辨识性与品质验证。
- 主动采集使用共有虚拟时钟和可控参考。试验中的时间拒绝和原协议回放有软件覆盖，实际 ADC aperture、端电压/轴转矩测量、误差预算与外部参考资格需真实来源。
- 历史短窗 Current 和 Servo 噪声/执行器余量仍是需解决的品质问题。已实现的 Host 存储、参数准入和控制消费为后续迭代保留可复现比较路径。
- Board1 功率输出和 Flash 应用保持硬件准入。产品仍为 `G3-SAFE HARDWARE NOT VERIFIED`、`G3-PWR LOCKED`、`AT32F415 WCET NOT VERIFIED`、`MOTOR PHYSICAL OPERATION UNVERIFIED`、`REAL BOARD CAPTURE NOT VERIFIED`、`REAL DATA REPLAY NOT VERIFIED`。
