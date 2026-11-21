# NEXT-PRODUCT-02｜FalconFOC 软件产品成熟度与实时性能工程

**NEXT-PRODUCT-02 SOFTWARE PRODUCT MATURITY PASS**

独立 C11 Core 可安装并由第三方链接。标准 Runtime 的独立消费者通过公开接口完成控制、模式切换、停止和锁存故障。现有 Host 标定 CLI 已接通能力查询、启动轨迹和经校验 FCAL 的稳态可达域诊断。实时路径已有可复跑的 Host 开销和 ARM 静态证据，ADC 最晚允许触发与 H0 预算的组合仍需目标板实际验证。

**PHYSICAL CALIBRATION NOT VERIFIED**
**TARGET MCU WCET NOT VERIFIED**
**G3-PWR LOCKED**

[可复跑证据和校验清单](2026-11-20-next-product-02/README.md) · [独立消费者](../../examples/external-consumer/README.md) · [Host 产品工作流](../../tools/falconfoc-cli/README.md#host-capability-and-reachability-diagnostics)

## 1. 基线

开工分支为 `codex/falconfoc-v2-vm01a`，HEAD `f8bc506d6a8f1205f62a932a8fa40a513d2f4b25`，暂存区为空。冻结了实际工作树的 1,975 个路径，包含尚未提交的 CQ-02 和 CQ-03 工作。CQ-03 原始包的 SHA-256 及其验收源文件逐项匹配。目录 `g3/np` 拥有参考 Device 映射，现行 FalconFOC 平台不装配 Legacy 通用设备栈。Core 最小依赖为 ISO C11 及平台 C 数学库，源码包含公开头文件，必要状态由应用静态持有。 [项目纲领](../architecture/FalconFOC_Project_Charter_and_Engineering_Memory_v1.0.md) · [执行 ADR](../architecture/FalconFOC_BareMetal_Execution_ADR.md) · [CQ-03](2026-11-20-next-cq-03.md)

收尾时工作树 HEAD 已移至 `c93655efbce28f6a81fcce2e5c20ac61f45cab76`，该提交含 CQ-02/CQ-03 基线与本轮部分变更。最终证据以实际验证过的工作树为准，包内记录开工及收尾 HEAD，并按路径记录相对冻结基线的哈希差异。

## 2. 独立集成与公开契约

### 可执行证明

冻结基线将 `core/` 独立复制后，即使指定 `BUILD_TESTING=OFF`，CMake 仍需要树外的 G3 和 Legacy 测试源码。修复使原有测试尊重标准 `BUILD_TESTING`，正常仓库测试默认开启。新增可重定位的 `FalconFOCCore` CMake 配置和归档安装。独立验证先构建 Core-only、安装、移动安装目录，再在另一个目录 configure 消费者。链接命令只引用安装库与公共 include。

```powershell
cmake -S <core-copy> -B <sdk-build> -G Ninja -DBUILD_TESTING=OFF -DCMAKE_C_COMPILER=clang
cmake --build <sdk-build>
cmake --install <sdk-build> --prefix <sdk-prefix>
cmake -S <consumer-copy> -B <consumer-build> -G Ninja -DCMAKE_PREFIX_PATH=<sdk-prefix> -DCMAKE_C_COMPILER=clang
cmake --build <consumer-build>
ctest --test-dir <consumer-build> -R '^external-core-consumer$' -V
```

- **纯 Core。** `find_package(FalconFOCCore CONFIG REQUIRED)` / `FalconFOC::Core`。消费者提供经校验的相电流、VBUS 和编码器计数，算电角，将正反速度目标经 Velocity PI 送入 Current，验证 SVPWM 请求。调用者负责样本与命令新鲜度、STOP/FAULT、禁能确认。Host sink 接受完整请求后才提交两个 trial PI。
- **标准 Runtime。** 在隔离目录复制 `core/`、`runtime/`、`g3/`、`sensors/` 源码模块，链接 `falconfoc-pipeline` 和 `falconfoc-encoder`。从公开 API 发表带源时戳传感数据、Current、ARM/START；先确认 STOP 再切 Velocity，执行正反目标和 WDT 锁存。Runtime 拥有准入和状态机，闭锁 G3 Host Mock 确认输出事务。构建及逐步调用顺序见独立消费者 README。
- **产品工作流。** 隔离拷贝既有 Servo 和 Calibration 示例，分别验证 Control Plane＋运行链，及已测候选审核和静止应用读回。持续使用整链 Host 标定程序跑 FCAL 保存、诊断和重载。三条集成范围各自记录。

Clang 22.1.8、MinGW GCC 16.2.0 和 Clang ASan/UBSan 的外部 Core-only 各 1/1、Core＋Runtime 各 2/2。Clang 与 GCC 的外拷 Servo 7/7、Calibration 1/1。三个入口使用 C11 公共头文件，Core-only 包无需板级工程、FreeRTOS 或 UART。

| 分类 | 合同 |
| --- | --- |
| `PUBLIC_STABLE` | `foc_core.h` 的变换、SVPWM，`foc_current.h` 的 Current PI，`foc_motion.h` 的基础相位、速度与位置。Encoder 的 init/update 接收带时间戳观测。V1 wire 是独立的协议合同。这里描述已核对的源接口，消费者需匹配工具链与头文件。 |
| `PUBLIC_EXPERIMENTAL` | Runtime 模式、生命周期、G3 输出接缝及快照，标定候选、审核应用、FCAL，以及新的冷路径 PMSM API。初始化时复制配置，编码器 LUT 存储寿命由调用者保证。 |
| `INTERNAL` | Core 及 Runtime 私有头文件、邮箱布局、试算/提交和内部状态字段。 |
| `BOARD_SPECIFIC` | AT32F415 的 ADC rank、SPI/PWM/IRQ 时间和 Board1 锁定适配。 |
| `HOST_ONLY` | G3 Host Mock、Virtual Motor、FCAL v1 文件及 Python Host 工作流。 |
| `NOT_VERIFIED` | 真实采样和功率输出、物理标定、关断、目标 WCET，以及其它板卡端口。 |

## 3. 实时资源与组合预算

### 真实调用链

| 域 | 已链接的参考链路 |
| --- | --- |
| H0 | TMR1 CH4 确认时标并发起四 rank ADC；PCCE IRQ 读 rank，配对 token 并检查时间。Runtime 解码、采样及角度准入，Pipeline 域通过相位对齐、Clarke/Park、PI、逆 Park、SVPWM、限幅/抗饱和，映射 PWM 请求，最后检查 Safety/STOP。只在输出确认 UPDATE 后提交试算和回执。锁定 Board1 拒绝 UPDATE 并确认软件 INHIBIT；Host Mock 另行验证接受分支。 |
| H1 | TMR5 overflow 开始捕获，CH1 发起 SPI，SPI1 处理接收与错误，CH2 close 验证时间窗和 RX。随后 Encoder 更新连续位置、速度滤波和电角。Runtime 按已确认模式走 Velocity、Position 或 Impedance，并验证/发布 H1 目标。48 Hz D 支路由启用配置和会话控制；Board1 默认禁用 Impedance。 |
| Foreground | Board 先监督/到期遥测，再做 V1 COBS/CRC、参数、发布及响应。USART1 IRQ 每次移交字符，原始 RX 队列 256，默认每 poll 最多 24、允许 8–32。完整标定、FCAL 与 CLI 在 Host 冷路径执行。 |

### 现有预算、实测与缺口

| 阶段 | 已核对的软件时间约束 | 未实测或预算风险 |
| --- | --- | --- |
| 触发及 ADC | 200 µs 逻辑槽，CH4 名义 +40、请求最晚 +110 µs，PCCE token 至多 80 µs 且同槽。APB2/6 与四 rank 的 7.5+12.5 ADC 时钟给出 **6.4 µs** 名义末 rank。 | 实际孔径、时钟同步、IRQ 进出、PCCE 排队、采样到真实 PWM load。+110 触发后仅余 **83.6 µs** 至下一逻辑槽，已小于 H0 100 µs 入参上限，尚未计入 IRQ 和提交。 |
| H0 | 软件周期/迟到/成本分别 **200/150/100 µs**。用实际 Runtime 入口验证 100 接受、101 拒绝，delay 150/151 亦验证。 | Board 当前传入零成本占位。实际执行时间、输出 setup 和是否超期未知。 |
| SPI 及 H1 | SPI 名义 16 bit / (75 MHz÷16) 约 **3.413 µs**。CS、TX、close 名义 +0/+20/+70 µs，允许 TX +20…30、close +70…100，要求已服务 RX/hold；源年龄界 650 µs。H1 软件周期/迟到/成本 **500/150/150 µs**。 | H0 可抢占 H1，RT-03 现有 Host 模型已复现 +105 µs close 被拒。真实 IRQ 排队和捕获保持待测，H1 成本 150/151 注入只验证软件门。 |
| 抢占和前台 | CH4、PCCE 优先级 0 相互串行，TMR5/SPI 为 1，USART 为 2。H1 冲突最多重读一次。监督 200/150/100 µs，遥测 10,000/500/100 µs，合计成本上限 150 µs，V1 服务另算。 | 若把所有独立上限当作每期实际耗时，H0/H1/监督已达 130% CPU。需按各发布点实测响应、嵌套、共享状态重试、Flash/APB 等待和满速协议负载。 |

Host Windows x64 Release 用 QPC 计时，粒度 100 ns。七组批均值取中位数。冻结构建的典型 Clang/GCC 结果如下。重复运行量级相同，同时有操作系统及计时粒度影响。两个编译器各条控制路径都验证了 10,504 次有效更新，无故障，锁定拒绝和合法 H1 路径独立测量。

| Host 路径 | Clang/GCC 中位数（ns） |
| --- | ---: |
| H0 Current 常规 / 饱和 | 528 / 613；630 / 640 |
| H0 Velocity / Position | 559 / 568；516 / 536 |
| H1 Velocity / Position | 120 / 156；127 / 145 |
| H1 Impedance 0 / 48 Hz | 99 / 110；100 / 120 |
| Runtime 监督 / 监督加遥测 | 73 / 82；122 / 127 |
| Board Host 锁定 ADC 完成 / Encoder 完成 / V1 STATUS poll | 663 / 625；179 / 178；692 / 830 |

独立重复的 H0 常规约 443/455 ns，饱和约 545/564 ns。单次 QPC 调用不直接提供数 ns 分辨率，无法从这些值确定 48 Hz 过滤器的精确增量。无真实 SPI/ADC 等待、寄存器延时或目标 IRQ，Host 最大值和尾时延不用于 MCU WCET。

ARM GNU 14.3.1、Thumb Cortex-M4、Soft-float `-Os`。锁定 BIN 与 CQ-03 逐字节一致，SHA-256 `78d055e1135d23a5c2d08f6db17fbf64622333f429381efa2632401eb58cbda2`。Flash/RAM **44,452/4,184 B**，`.text/.rodata` 42,540/1,444 B，`.data/.bss` 84/3,072 B。已存在的可选 DWT trace 锁定构型 45,564/4,600 B，仅作静态编译核对。H0/H1 局部栈 168/328 B，静态正常嵌套链估算约 3,456 B，未覆盖板上实际水位及故障特例。Map、调用图、`.su` 和反汇编随包归档。

已链接热路径实际使用软件浮点加乘除、比较转换及 `fmodf/sinf/cosf/hypotf/sqrtf`。板端 Encoder 路径两次相位归一化都可进入带三角函数的缓存助手，即使调用方只取角度。没有动态分配符号；可见的原子指令重试和库内循环需在目标分析中给出响应上界。裁决 **KEEP**，先实测角度与限压步骤、IRQ/总线和 deadline 组合，再判断是否重写现有正确路径。**ARM_STATIC_COST_ANALYZED · HOST_EXECUTION_MEASURED · TARGET_WCET_NOT_VERIFIED**。

## 4. 可达域与 Virtual Motor 交叉验证

新增 `foc_pmsm_reachability_evaluate()`，在固定带符号机械速度和 Id 下计算稳态 d/q 电压、模型轴转矩、裕量及可用 Iq/转矩区间。沿用生产峰值电流与电压定义，电角速度为 `ωe = p·ωm`。

- `Vd = Rs·Id − ωe·Lq·Iq`，`Vq = Rs·Iq + ωe·(Ld·Id + λ)`。电阻耦合和反电动势分项输出，稳态电流导数为零。动态轨迹的独立分析另加 `Ld·dId/dt`、`Lq·dIq/dt`。
- `T = 1.5·p·(λ+(Ld−Lq)·Id)·Iq`。固定 Id 时对 Iq 的仿射电压线，与电流矢量和可选转矩限制求交。供电低于 ADC/Core 最小门槛给出 `BUS_MINIMUM`。
- SVPWM 按实际居中方式，窄侧对称宽度记为 `s=2·min(0.5−duty_min,duty_max−0.5)`。保守全角电压界取 `min(配置限压, VBUS/√3, s·VBUS/√3)`，最佳角的 duty 项改为 `2s·VBUS/3`。两界之间报 `INDETERMINATE/ANGLE_DEPENDENT`；非对称限幅的两正交方向用生产 SVPWM 单独复验。5% 的近界标记描述非负电压裕量，独立于闭环 trace 所用 2 mV 观测阈值。
- 模型可行、不可行与证据不足独立分类，并携带模型与审核身份。计算属于配置/诊断，未链接进板端 H0。稳态可行与达到毫秒级调节门槛分开。

用批准的 CQ-03 FCAL 在正式 Virtual Motor/Encoder/Runtime 运行 **60 组、64 个阶段**。仍使用原 130 Hz、2.5 A、2.4 V 电压上限、6 V 最小母线；固定转速用夹具承担机械反力，另跑自由加速和实际反向。涵盖 0/±0.5/±5.2/±18 rad/s，正反小/大目标，非零 Id 及反电动势方向，12/6.1/5.9 V，Plant-only Rs/Ld/Lq/λ 失配，电流矢量限幅、饱和与出口。有效输出、电角新鲜度和向量回执来自生产软件链。最后 20 ms 的独立比较用实际电压与观测速度，饱和时边界使用**实际 Id**。固定速稳态电压残差最大 **0.322 mV**，带 d/q 导数的全组最大 **1.405 mV**，饱和电流与独立边界相差至多 **4.41 mA**。另对生产 CLI 查询 **84 次**（46 可行、38 不可行），独立计算与二分电流区间一致，最大比较数值偏差约 9.86×10⁻⁸（各量自身单位）。

| 代表性工况（两极性） | 需求与轨迹 |
| --- | --- |
| Generic 18 rad/s，0.35 / 2.20 A 同向 | 约 2.010 / 3.129 V。小目标跟踪，高目标压限后约 0.991 A。 |
| Salient 18 rad/s，0.35 / 2.20 A 同向 | 约 2.578 / 4.035 V。均受 2.4 V 限制，约 0.123 / 0.120 A。反向电流可在同样转速及母线下达到。 |
| Salient 5.2 rad/s，2.20 A | 名义模型约 2.384 V，轨迹通过。Plant-only 失配需要约 2.695 V，实际只到约 1.872 A。旧包的名义可行仍正确描述所供参数，读取者必须保留不确定性。 |
| 12→6.1 V、5.9 V | 6.1 V 的 SVPWM 几何圆仍高于 2.4 V 配置限压，不能因母线标签断言同条件更小电压。四个 5.9 V 负例按准入拒绝，零 UPDATE。 |

**Salient ±2.20 A 的失败有两个阶段。** CQ-03 V1 CSV 逐字节复现，评分出口 1。命令刷新至首次 UPDATE 的 200 µs 在原调节计时之外。在 4 ms 时，实际 |Iq| 为 1.905–1.907 A，低于 1.98 A 误差带边缘，已退出最初削顶，需求稳态约 1.756 V、实际 1.769–1.776 V。相同 130 Hz 增益和电压上限的固定零速对照也需 **4.8 ms**，所以早期跨不过 4 ms 门槛由初始限压和闭环建立过程解释。自由转动到尾段速度约 ±6.904 rad/s 时，2.20 A 需要约 **2.603 V**，压到 2.4 V，实际 |Iq| 约 **1.9875 A**，这才是持续电压能力限制。原裁决保持。

## 5. 使用链、回归及退出状态

Host 使用现有 CLI 完成配置档、采集、审核、应用、四模式、诊断、安全停止、FCAL 保存和重载。新增 `--capabilities`、`--startup-trace`、`--reachability`、`--reachability-torque`。查询使用 `--load-config` 的 CRC、版本和完整身份校验，与运行审核互斥。Current 使用实际电流矢量上限，扭矩查询限定 Id=0 并使用 Impedance 的电流/转矩限值，避免把模型 d 轴切片当成可发布命令。错误 runner、启动失败、超时或非校验运行结果有 Host 诊断。通用开发者可以从公开 C11 配置和调用契约接入自己的电机，Host 校准程序本身提供两个冻结虚拟配置。

两套产品流程重跑，四模式、识别结果、超时恢复、FCAL、Startup V1 CSV 都与 CQ-03 一致。错审核 ID、错包身份、NaN 输入、互斥模式、过期会话、非法时序、拒绝后的冻结积分、低调制比的双向保护均复验。48 Hz 两个原 35 mA 噪声门槛以及 Servo-02D 原动态评估通过。历史 10 ms Current、近极限动态限幅、0.039 N·m 及 Salient ±2.20 A 都保留 FAIL。新增范围诊断从未修改历史阈值或裁决。

Clang、MinGW GCC、Clang ASan/UBSan 每套 109 项（Core 21、Runtime 13、Calibration 8、Sensors 2、Control Plane 5、Simulation 41、Servo 7、Protocol 1、Board Host 9、Workflow 1、Replay 1），Python CLI 11 项；Legacy Host 45 项、`./run.sh build`、Strict Scope all、Source Contract、ARM Soft-float 构建通过。独立消费者另见 §2。

| 验收 | 裁决 |
| --- | --- |
| `EXTERNAL_CORE_INTEGRATION_PASS` | 可移动安装前缀内的公共 Core 归档通过独立消费。 |
| `RUNTIME_INTEGRATION_PASS` | 外部源模块的公开执行及软件 STOP/FAULT 通过。 |
| `SOFTWARE_TIMING_BUDGET_DOCUMENTED` | 真实路径、预算算术和 Host 边界证据已绑定。 |
| `ARM_STATIC_RESOURCE_ANALYSIS_PASS` | 锁定构建、资源、调用、soft-float 与静态栈分析完成。 |
| `HOST_REACHABILITY_MODEL_VALIDATED` | 公开模型、Host CLI 与独立轨迹/方程匹配。 |
| `PRODUCT_WORKFLOW_PASS` | 两套 Host 使用链通过，诊断与应用分离。 |
| `REGRESSION_PASS` | 全套软件验收通过，历史品质缺口保留。 |

## 6. 文件与待交付的产品能力

| 新增或修改 | 原因 |
| --- | --- |
| [Core CMake](../../core/CMakeLists.txt)、[Core README](../../core/README.md) | 可重定位静态包、隔离构建、测试开关和模型入口。 |
| [外部消费者](../../examples/external-consumer/README.md) | CMake、Core/Runtime 示例 C 文件和可重复构建/接口清单。 |
| [模型 API](../../core/include/foc_pmsm_reachability.h)、[模型实现](../../core/src/foc_pmsm_reachability.c)、[Core 测试](../../core/tests/test_foc_pmsm_reachability.c) | 冷路径稳态方程、精确限压定义和正负向、证据不足测试。 |
| [Host Runner](../../simulation/src/falconfoc_virtual_calibrate.c)、[Simulation 构建](../../simulation/CMakeLists.txt)、[产品测试](../../simulation/tests/test_virtual_calibration_cli.py)、[Simulation README](../../simulation/README.md) | 读取复核后的产品配置，接通能力、启动和可达域。 |
| [Python CLI](../../tools/falconfoc-cli/falconfoc_cli.py)、[CLI README](../../tools/falconfoc-cli/README.md)、[维护日志](../governance/Themis_工程维护日志.md) | 复用现有入口、结构化用错诊断、记录产品使用链。 |

本阶段具备可消费的软件基础。发布前仍需为支持的编译器/平台建立干净 CI 与安装消费者回归，对公开结构做好版本/兼容性策略，验证 Runtime 未来是否需要分发产物并明确支持边界。产品端需为电机参数漂移、可达域余量和持续大电流设定可复跑门槛，补充满载协议、错误帧、参数故障/重载回归。板端还需按真实 ADC 孔径、SPI、IRQ、H0/H1 及输出 load 采集时序和物理去能证据，确认组合预算风险与目标 WCET。实板标定和允许输出另行验收。报告和原始证据保留在本地工作树，本轮未执行 Git 提交或推送，也未触碰硬件。
