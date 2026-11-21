# NEXT-CAL-ARCH-02｜FalconFOC 校准参数依赖管理、历史复用与安全应用

**NEXT-CAL-ARCH-02 PARAMETER REUSE & DEPENDENCY PASS**

Generic 和 Salient 均已完成新标定、显式审核及全量复用。跳过全部重新采集后，两种配置各自的 16 条 Current、Velocity、Position、Impedance 正反向闭环结果与新标定逐项一致。编码器、负载、电机、ADC 链及 Kt 参考变化均已实跑选择性重测与新实例验证。来源不明、依赖过期、审核对象变化，以及应用时原实例收到 STOP 或 FAULT，都会阻止应用。

**PHYSICAL CALIBRATION NOT VERIFIED** · **TARGET MCU WCET NOT VERIFIED** · **G3-PWR LOCKED**

[复跑入口与原始证据](2026-11-20-next-cal-arch-02/README.md) · [Host CLI](../../tools/falconfoc-cli/README.md#host-parameter-review-and-reuse)

## 1. 工作基线和 ARCH-01 门禁

工作树位于 D:/study/Software/Hollow-Gimbal-vm01a，分支为 codex/falconfoc-v2-vm01a，开工及收尾 HEAD 均为 c93655efbce28f6a81fcce2e5c20ac61f45cab76。开工暂存区为空。冻结证据列出开工已有的 PRODUCT-02 工作树状态及差异。未找到独立 ARCH-01 验收报告。旧 Calibration CTest 为 8/8，但新增门禁用例复现了完成的分项候选因缺少完整 Runtime 准入而被拒绝审核。

修正后，Workflow 把完成且合格的分项提交给原身份和 Session 的显式审核。Calibration Application 仍要求完整换相快照和准入。Host CLI 的 Rs-only 复用现有定轴 R/L 拟合和 ADC 零偏前置测量，审核输出只包含 Rs。J/B-only 使用独立虚拟等效轴转矩、速度和负载，Kt 为零，在无 Flux/Kt 阶段的情况下完成。两项审核均返回 runtime_applicable:false。Generic/Salient 的 Rs 分别为 0.599890471/0.780080080 Ω，J 分别为 0.00200005132/0.00320011936 kg·m²。完整配置的原审核指纹、FCAL 和控制结果复验一致。 [Workflow](../../calibration/WORKFLOW.md) · [门禁测试](../../calibration/tests/test_foc_calibration_workflow.c)

## 2. 参数依赖和来源

表中的依赖按本次实际测量方法确定。装配、机械参考、负载、采样链、相序、供压/磁轴/转矩参考、控制周期和限幅都有独立身份。旧 Calibration identity 用于原生准入，复用记录额外保存原始会话、Capture、分项修订号、来源类别、测量依赖标志及依赖版本。合格原始记录保留其来源。更换目标后，只有无关的原生聚合身份字段才允许投影到新 Workflow，Rotor 拟合的相对方向仍从原结果保留。

| 参数 | 本轮合同及失效依据 |
| --- | --- |
| ADC Offset | 采集链、相序和偏置温度。当前 Host 三路结果按相索引保存，采集链或相序变更需重算。 |
| Rs | 绕组、温度、电流量程及可信实际区间电压。Host 依赖 ADC 偏置和电流尺度，独立电流仪器读取的实测记录可不依赖 ADC。普通绕组 Rs 不绑定 Encoder。 |
| Ld/Lq | 绕组、相定义、定轴工作点和磁轴参考。Host 使用独立虚拟磁轴，编码器更换可保留。若来源使用已装角度映射，还须检查 LUT 和 Rotor Offset 版本。 |
| Encoder LUT | 编码器、相对安装、独立角度参考。复用要求训练和独立 Holdout 的原记录。 |
| Pole Pairs | 电机磁极和获准的电角参考。已认可的电机极对数可跨编码器保留，复合 Rotor 阶段仍重测方向并复核极对数。 |
| Rotor Offset | 电机、相序、装配、已映射的 Encoder/LUT、Pole Pairs 和电角零点参考。LUT 变化后旧零点失效。 |
| Mechanical Home | 应用机械参考与坐标。Host 使用虚拟夹具起始机械坐标，单独标识原点。真实 Home 采集和写入未验证。 |
| Flux/Kt | 电机、Pole Pairs、磁轴、ADC、实际端电压和本次端电压模型所用的 Rs、Ld/Lq。Kt 是从磁链推导的 Host 值。 |
| J/B | 轴系、负载及速度/实际转矩来源。独立等效轴转矩可保留；由 Kt×Iq 获得转矩的记录还需检查 Kt、映射和实际电流来源的版本。 |
| Current PI | 合格 R/L、控制周期、电压余量和延迟策略。任一依赖变化时按现有策略重新整定。 |
| Servo Gains | J/B、Kt、Current PI、机械坐标、周期及控制/机械限幅。独立转矩 J/B 可与新 Kt 组合，Servo 重新整定。 |
| Cogging LUT | 轴系、装配角坐标、转矩尺度及补偿限幅。仅启用时纳入组合，训练、Holdout 及依赖版本需合格。 |
| 机械限位政策 | 轴及负载、机械坐标和控制限幅，映射来源关联 Home。当前虚拟配置采用本地运动保护策略，预览显示硬机械限位是否配置。 |

模块使用固定依赖顺序和版本比较。范围可采用包含端点的区间或保守的单工作点。测试覆盖 [25,25] 的退化范围、越界、非法范围、未审核、缺失来源、缺失依赖和依赖版本变化。当前 Host 只生成虚拟测量和基于受审核测量的派生记录。外部记录、独立仪器和厂商标称有不同类别，测试证明依赖判定区别对待；虚拟 CLI 的外部参考变更返回 NEW_REFERENCE_REQUIRED，等待对应证据。25 °C 是该 Host 工位的声明点，当前 Plant 无热模型。 [依赖实现](../../simulation/src/foc_virtual_calibration_reuse.c) · [依赖专项](../../simulation/tests/test_foc_virtual_calibration_reuse.c)

## 3. 选择性复用和失效传播

| 真实 Host 改动 | 保留 | 重采或复核 |
| --- | --- | --- |
| 编码器身份、装配及一阶误差变化 | ADC、Rs、Ld/Lq、独立磁轴 Flux/Kt、独立转矩 J/B | Encoder LUT 训练/Holdout、相对方向和对应 Rotor Offset。新 LUT 和零点经过同一生产 Encoder 和 Calibration Admission 核对。 |
| 轴上负载变化，惯量 ×1.25，阻尼 ×1.15 | R/L、ADC、换相、Flux/Kt、Current PI | 新 J/B、机械限位政策与 Servo。Current PI 和旧电角零点相同，外环增益发生变化。 |
| 更换虚拟电机，保留同一采集链和机械应用 | ADC 偏置及仍相符的机架参考/限位政策 | 电机专属 R/L、极对数、换相、Flux/Kt、J/B、Current PI、Servo。旧指纹无法批准新组合。 |
| 改变 ADC 偏置及电流尺度 | LUT、方向、Rotor Offset、独立等效转矩 J/B | ADC A 偏置 2090→2101 counts。重测 R/L、Flux/Kt 并重新整定 Current/Servo。 |
| 更换 Kt 测量参考 | R/L、独立等效转矩 J/B | Flux/Kt、Servo。保留 J/B 的原会话和版本。 |

另外测试电压、磁轴和转矩来源变更，相序、机械参考、直接 Rotor Offset 和 Encoder LUT 更新，控制限幅变化，以及范围外和依赖链不完整等情形。使用独立端电压/电流测得的 Rs 可跨 ADC 变更保留。Host 经 ADC 得到的 Rs 在 ADC 或相索引变更时失效。已审核的值与其源记录一起复用。

## 4. 审核和安全应用

Host 命令顺序为 capture → review → apply。之后使用 plan 和 measure 生成新的组合。候选报告选择保留的分项、重采事件、来源、精确数值配置和审核 ID。原实例保持 DISABLED、无故障、已确认 INHIBIT，生命周期请求须结束。审核绑定会话、当前设备与参数记录、依赖版本、已预览配置及数值指纹。apply 重新核对保留记录的原会话/采集/修订号、依赖可用性、Runtime 绑定和候选指纹。重新计划、撤销依赖和 STOP/FAULT 使已有同意失效。

应用复用 Calibration Application，在独立存储完成 Runtime 和 Encoder 初始化，校验读回包括 ADC、控制增益、相序、机械坐标、限幅、电角和 LUT 内容，再运行原四模式闭环及超时故障恢复。只有全部通过才发布新的 Host 实例。初始输出回调前后复核绑定的原 Runtime。对已审核配置同步注入 STOP 或 FAULT 的回归先失败，修正后拒绝应用且新存储未初始化。测试还证明旧实例在适配器拒绝初始 INHIBIT、配置不完整、身份/角度冲突、错误审核和 stale 依赖时保持原控制配置及所有权。Host 命令由单一 Foreground 串行，实板并发时序另行验证。 [组合会话](../../simulation/src/falconfoc_virtual_calibrate_reuse.c) · [应用测试](../../control_plane/tests/test_foc_calibration_application.c)

## 5. FCAL v1 与历史证据

- 现有 v1 二进制布局及字段语义保持。两份 CAL-INTEG 归档的 36 Hz 旧包继续用原审核指纹重载，每套执行 16 条闭环试验。两份 130 Hz CQ-03 包仍能按原指纹重载，四模式读回和 16 条试验逐项相同，重新完整标定生成逐字节相同的 548 B FCAL。
- 编码器变更后的组合可以保存数值 FCAL，在相同目标身份和本地策略下显式审核并重载。重载读回及 16 条闭环结果一致。错电机/配置身份及错误审核被拒绝，原文件保留。依赖已撤销或控制限幅策略无法无损表达时拒绝导出。独立加载另一份 Cogging FCAL 的验证使用私有夹具副本，组合候选保持审核时的工况。
- **依赖历史为 Host 内存态。** v1 缺少分项来源、原会话、依赖版本和范围。旧包继续走既有数值兼容入口。读入 v1 不生成分项复用记录。跨进程扩展可使用单独版本化的 Host 元数据，绑定 FCAL SHA-256、实际 Runtime 策略、逐项来源和依赖，缺项时维持原有可信等级。当前没有存储迁移。

## 6. 回归、资源和变更

| 验证 | 观察 |
| --- | --- |
| Clang / MinGW GCC / Clang ASan+UBSan | 各 111 项，含 Core 21、Runtime 13、Calibration 8、Control Plane 5、Sensors 2、Simulation 43、Servo 7、Protocol 1、Board Host 9、Workflow 1、Replay 1。 |
| Python CLI、兼容性和工程门禁 | Python 11/11，Legacy Host 45/45，Source Contract 和 Strict Scope all 通过。 |
| 历史控制品质 | 历史 10 ms Current 正反向轨迹逐字节一致，0.12 V 双向化简案例保持安全停止及原品质 FAIL。Servo 既有冻结动态/噪声门槛继续通过，近转矩边界限幅及 0.039 N·m 超载保留历史 FAIL。Salient Startup V1 CSV 与 CQ-03 同字节，原质量评分仍退出 1。 |
| ARM Soft-float 锁定构建 | GNU 14.3.1，Flash 44,452 B / RAM 4,184 B，BIN SHA-256 78d055e1135d23a5c2d08f6db17fbf64622333f429381efa2632401eb58cbda2，与 PRODUCT-02 相同。 |

复用修改位于 Host Foreground，x64 分项元数据 120 B、13 项复用历史结构 1,640 B。组合实例及其 Encoder LUT 由 Host 调用者静态尺寸对象持有。分项审核和回调复核未改变现有 C 公共类型布局或调用签名。Host CLI 添加子选项与 JSONL 事件。Board1 资源、镜像和既有 UART V1 格式保持。

| 修改范围 | 修改原因 |
| --- | --- |
| [Workflow 头文件](../../calibration/include/foc_calibration_workflow.h)、[实现](../../calibration/src/foc_calibration_workflow.c)、[测试](../../calibration/tests/test_foc_calibration_workflow.c) | 完成分项审核，区分独立等效转矩与 Kt×Iq；保留完整应用条件。 |
| [Calibration Application](../../control_plane/src/foc_calibration_application.c)、[测试](../../control_plane/tests/test_foc_calibration_application.c) | 首次输出确认复查旧实例，早拒应用且保持目标存储。 |
| [Host 采集执行](../../simulation/src/foc_virtual_calibration.c)、[标定 Runner](../../simulation/src/falconfoc_virtual_calibrate.c)、[内部接口](../../simulation/src/falconfoc_virtual_calibrate_internal.h)、[组合会话](../../simulation/src/falconfoc_virtual_calibrate_reuse.c) | 沿用原辨识器执行选定阶段，将组合结果接入原审核与控制链。 |
| [静态依赖](../../simulation/src/foc_virtual_calibration_reuse.c)、[元数据结构](../../simulation/src/foc_virtual_calibration_reuse.h)、[CMake](../../simulation/CMakeLists.txt)、[依赖测试](../../simulation/tests/test_foc_virtual_calibration_reuse.c)、[CLI 测试](../../simulation/tests/test_virtual_calibration_reuse_cli.py)、[Python 入口](../../tools/falconfoc-cli/falconfoc_cli.py) | 把模型、显式理由、Host 控制和 FCAL 兼容接入正常工具与回归。 |
| [Workflow 文档](../../calibration/WORKFLOW.md)、[Application 文档](../../control_plane/README.md)、[Simulation README](../../simulation/README.md)、[CLI README](../../tools/falconfoc-cli/README.md)、[维护日志](../governance/Themis_工程维护日志.md) | 记录分项审核、运行边界和复现使用方式。 |

跨进程来源保存、真实独立仪器提供的数据准入、机械 Home 和物理限位测量、温漂范围、物理等效转矩验证、IRQ 时序与 WCET 仍待建立产品证据。未执行提交、推送、Flash 写入、上电或电机操作。

