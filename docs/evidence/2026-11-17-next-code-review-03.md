# NEXT-CODE-REVIEW-03｜foc_motion 深度审查

日期：2026-11-17。方法：File-Centered, Contract-Aware Review V2。

**最终结论：IMPROVE / P2。没有确认的 P0/P1，没有 BLOCK。**

保留现有预测算法作为默认名义软件域的实现，优先明确误差契约；不建议现在把生产预测器替换为 Double。Velocity PI 的条件积分、单次状态提交以及 Position P 的连续多圈职责可 KEEP。需要补足预测的数值工作域和计算溢出时的状态不提交测试。PowerShell 问题独立保持 **OPEN — NEXT-QUALITY-GATE-01**。

本轮没有更改生产源码、正式测试、Golden、增益、Safety、相位来源或硬件配置。用户同一条消息新增“每轮完成后提交推送”，覆盖附件中 Commit/Push 禁令：只读审查先完成并核验，随后以独立的交付步骤提交已有成果和新增审查文档。Git 身份改变属于这项明确授权，不能继续声称交付后 HEAD 未变。

## 1. 实际源码、Git 身份和范围

- 审查工作树：`D:/study/Software/Hollow-Gimbal-vm01a`；分支 `codex/falconfoc-v2-vm01a`。
- 审查起始 HEAD：`a198f6d4a6c61f1bb9dd5968a46e7f38ac49a3f0`。实际对象是含 VM/Quality/Tuning/Release Prep/相位整改未提交成果的工作树，不是 HEAD 单独代表的 RC0。
- 新隔离目录：`D:/study/Software/FalconFOC-code-review-03-20261117`，源快照 2516 文件，已有 ignored 产物 2427 文件 / 322054220 B。
- 当前 2516 文件的完整内容清单与上一轮 AUDIT-02B 起始清单相同，旧全面回归证据仍对应同一生产源码；本轮新增执行结果单独列示，未将旧 PASS 冒充新运行。
- 主工作树 `D:/study/Software/Hollow-Gimbal` 保持干净。origin 为 `https://github.com/2023violet/Hollow-Gimbal.git`；开始时远端尚无 VM 分支，RC0 分支指向上述 a198f6d。
- 已按真实路径阅读 AGENTS、CR01/CI01、CR02、IM02B、AUDIT02B 和有关 Core/Runtime/Sensor/Calibration/Board/Virtual 源码、接口、测试、CMake 与契约。此前 P2-A 是本轮主审对象，P2-B 不修复。

源身份、状态、未跟踪文件、完整 diff、不可变快照和原产物哈希保留在隔离目录。所有编译和变异在新目录运行，无 clean/reset/stash/自动格式化/旧 build 覆盖。

## 2. 完整函数地图

| 函数 / 行 | 责任及前置条件 | 成功后置 / 修改 | 失败语义 | 时间、并发和消费者 |
| --- | --- | --- | --- | --- |
| [foc_motion_clamp:22](../../core/src/foc_motion.c#L22) | 私有对称限幅；调用点已确认正有限 limit 和合法状态 | 返回 [-L,L]；无状态 | 无独立 status；不负责修复损坏的 context | 无时钟；Velocity/Position 内部 |
| [foc_rotor_phase_from_counts:29](../../core/src/foc_motion.c#L29) | directed uint32 count、CPR 2..65536、非零 uint16 poles、有限 offset、有效存储 | 整数乘/模后 float 缩放，电相位有界；只写输出 | NULL/config/nonfinite；有输出时先清零 | 无状态；Board/Virtual producer，未改已接受的数据链 |
| [foc_rotor_electrical:60](../../core/src/foc_motion.c#L60) | finite 连续机械位置/速度、非零 poles/有限 offset | 周期电相位及 p·机械速度；只写输出 | 先清输出；非法配置/非有限或速度乘法溢出拒绝 | 无时间同步能力；保留兼容 API、研究模型使用 |
| [foc_rotor_predict:91](../../core/src/foc_motion.c#L91) | 同时刻 finite θ/ω；horizon<2³¹；非负有限 a/error；caller 已检查正向 age | constant-speed 外推并归约；只写结果 | NULL/nonfinite/config/horizon/model-budget 错误，输出清零 | age 为 µs；不读时钟；H0 align、init 验证 |
| [foc_velocity_init:124](../../core/src/foc_motion.c#L124) | 非负有限 Kp/Ki；正有限 max_dt；有效 context | 最后完整写入 config、I=0、initialized | 验证失败不改旧 context | 单 owner；Runtime init 或 trial fixture |
| [foc_velocity_reset:143](../../core/src/foc_motion.c#L143) | initialized context | 只清 I，不改配置 | NULL/uninitialized 拒绝 | 调用者串行化；H1 新 session/非运行态 |
| [foc_velocity_step:155](../../core/src/foc_motion.c#L155) | 合法 initialized/未被外部损坏的 context；finite ref/measured/limit；0<dt≤max_dt、L>0 | enabled 时 \|I\|、\|iq\|≤L，最后提交 I；disabled 冻结 I、结果 disabled | 输出先清零；输入/运算失败不提交任何积分 | dt 是实际外环秒数；支持 caller-owned trial copy；H1 外环或独立研究 |
| [foc_position_step:215](../../core/src/foc_motion.c#L215) | 非负有限 Kp、正有限 Vlimit、有限连续 ref/measured | v=clip(Kp(ref−measured),±Vlimit)，只写输出 | 输出先清零；NULL/config/nonfinite/乘减溢出拒绝 | 无持久状态、dt 或 enable；Runtime/Trajectory 组合者负责生命周期 |

整个文件没有可变全局、动态分配、时钟、硬件 I/O 或跨实例状态。所有有状态操作依赖一个 owner；stateless 函数可在独立输入/输出、只读配置下使用。不能推断同一个 Velocity context 可以被多个线程无同步地修改。

## 3. API 契约复原：声明与实现分开

| 项目 | DOCUMENTED | IMPLEMENTED / INFERRED | UNSPECIFIED 或边界 |
| --- | --- | --- | --- |
| 周期计数 helper | 来源先应用方向、整数模先于 float、成功不授予 source/output 权限 | count·p 最大 4294836225，uint32 不溢出；归约后整数可精确转 float | 任意巨大有限 offset 的数学精度不保证；沿用已接受限制 |
| Legacy Rotor | 连续机械坐标到电坐标；Position 保留全部机械圈 | 先用 float 周期归约，再 p·phase；速度乘法有限性检查 | 不恢复调用前已丢失的机械位置精度；不是新的整数来源路径 |
| Prediction | 恒速模型、µs、horizon、`0.5*a*dt²` contribution；source uncertainty 属于 caller | float dt、乘加和 float 周期归约；max_error 只比较模型项 | 没有公共全 float 域数值误差保证；没有 Sensor/输入量化/所有误差之和预算 |
| Velocity PI | Kp A/(rad/s)，Ki A/rad，dt s，live q limit；disabled 冻结；新 session reset | 计算局部 trial，按可达电流边界限制积分增长，最后一次提交 | 不负责获得实际电机电流/电压裕量、参数热修改或损坏内存恢复 |
| Position P | 连续位置差，输出受限机械 rad/s；无状态 | 不调用 wrap，不追踪历史；先拒绝非有限计算，再限幅 | 没有独立 enable；Kp=0 不代表硬件停机或忽略非法输入；无轨迹/加速度保证 |

Legacy Rotor 中最终 normalize 的返回值未单独传播：前面已把 phase/offset 归约且 p≤65535，中间结果有限，不存在仅靠合法有限输入把该末次加乘变成 Inf 的路径。没有把这一写法当作风格缺陷整改。

## 4. H0 预测反例和误差来源

独立探针从当前源码新编译，复现 `{horizon=400,a_max=2000,max_error=.0003f}`，θ=0.35820722579956055f、ωe=65535000f、age=400 µs：status=OK，输出 0.90759086608886719，最短圆周数值误差 **0.001514806475911627 rad**。输出失败时的 0 不可被当成合法零相位。

参考定义：以实际存储的 float θ、ω 为精确输入、整数 µs 为精确时间，先做 double 参考；为接近全 float 输入边界的测试额外用 110 位 Decimal 和高精度数学 2π，避免双精度大数取余本身失真。所有误差按圆周距离计算。double 候选不是 Oracle。

历史输入的分解（带符号乘加误差，归约/三角行给绝对圆周误差）：

| 来源 | 本例值 rad | 含义 |
| --- | ---: | --- |
| 模型误差 | 0（构造的真实模型恒速） | 允许的加速度模型贡献约 0.00016；这是检查值，不是本例实际加速 |
| 输入表示误差 | 按已存储输入定义为 0 | 65535000 可精确表示；不能替假想的原始物理速度补精度 |
| float dt 转换 | -0.0006622213417 | ω·(float_dt−精确 dt) |
| float 乘法舍入 | +0.0006622213405 | 本例近乎抵消上一项；不能假定其他输入也抵消 |
| 相位累加舍入 | -0.0007853507996 | 向约 26214 rad 的增量加小初相位时丢失低位 |
| 周期归约 / float 2π | 0.0007294556764 | 大增量含许多圈，float 周期常数偏差被放大 |
| sin/cos→atan2 观测 | 8.56e-10 | 不构成本例主导项；predictor 返回的 θ 本身不由 sin/cos 反算 |

四类误差不能混用：实际运动模型未知、输入量化已发生、计算舍入可测、Sensor/时间测量误差由上游决定。即使 Double 完全修复计算项，原始速度 65535001 若先存为 65535000，在 400 µs 内已经产生 0.0004 rad 的输入表示差；它不会被下游 Double 恢复。

[motion-numerical-oracle.json](2026-11-17-next-code-review-03/motion-numerical-oracle.json) 保存汇总、最坏输入、代表性逐项分解；完整 61233 行预测记录在 numerical2/prediction-results.csv，每行包含要求的 θ、ω、age、生产/参考相位、圆周误差、模型项和 status；完整策略输入另存 inputs.txt。`model_error_budget_rad` 列是由 a 和精确 age 算得的模型贡献，配置允许阈值见对应输入，不代表总误差。

## 5. 实际软件工作域与生命周期

| 来源 | 默认 Board1 | 默认 Host Virtual | 公共 Core / Runtime |
| --- | --- | --- | --- |
| poles | 7 | 4 | Core uint16 非零；H1 只要求转换后 finite |
| Encoder raw speed limit | 1000 mechanical rad/s | 1000 | Core 不限定物理速度；H1 不额外 clamp measured speed |
| Runtime 使用的速度 | 同帧 filtered LPF velocity×p | 同左，首帧原策略零速 | caller 提供 finite θ/ω 或 finite p·机械速度 |
| 名义电速度 | 约 ±7000 rad/s | 约 ±4000 rad/s | 允许远大于名义范围，finite 不等于精度受保证 |
| H0 / H1 | 200 / 500 µs | 200 / 500 µs | 配置驱动 |
| angle timeout | 650 µs | 600 µs | freshness 独立检查 |
| prediction horizon | 650 µs | 400 µs | <2³¹ µs |
| max a / model contribution limit | 2000 rad/s² / .00065 rad | 2000 / .002 | 非负有限；只检查模型项 |

原始速度先受 Encoder max_speed 检查；LPF 从合法速度初始化后为其加权平滑，舍入带来微小 ULP 余量。Runtime 不用可能超调的 tracker speed，而用 filtered_velocity。以上是软件准入/配置事实，**不是电机能达到此转速或电压范围的物理证明**。

Board 校准 admission/snapshot 的 identity 将 poles 限至 1..1000，且 identity 与 snapshot 必须一致；Board init 只在 Sensor 尚未初始化时绑定。故不能把默认 p=7 当成所有合法 Board 配置：校准准入包络可到约 |ωe|=1e6。公共 Runtime 配置还允许 p=65535，历史极端输入属于该数值允许范围，但不属于默认 Board/经当前校准准入的 p 范围。

H1 完整发布 bounded θ、同帧速度/位置/time/seq；H0 计算 `ADC.sample_tick−sensor.capture` 的正向模差，先检查 source freshness、未来时间和预测 horizon。默认软件域按全部 0..650 µs 测，不以理想调度相位替代实际 IRQ/SPI 延迟；真实正常/最坏 age 分布需要硬件 trace。

正常 H0 路径一次 align/predict；timeout-clear readiness 可能先做一次检查，随后 control 再 align，故该恢复路径可能两次。init 另有 age=0 的配置验证。Current 会再次准备电角度及三角缓存；“每次 H0 只发生一次归约/三角计算”并不成立。

启动保留 Host 首帧零速策略，Board 首帧仍需完整 Feedback；STOP、FAULT、旧会话、future/过期 source、invalid flag 和发布不同步遵守原 Runtime/Safety/rearm 规则。时间回绕在 <半范围合同下处理。缺相位不回退累计机械 float。现有 Runtime/Simulation 模式切换、STOP、sensor fault、时间回绕、旧 session、preemption/失效路径本轮回归执行，未改已接受的整数相位链。

## 6. A/B/C 对照、收益与建议契约

A 为原生产实现；B 为先把 float 增量 fmodf 后再与初相位相加；C 使用 double dt/乘加/数学周期归约，最后转回 float。B/C 仅在外部数学原型中，复制相同验证规则，没有进入产品构建。

| 实测域 | 样本 | A 最大误差 rad | B 最大误差 rad | C 最大误差 rad |
| --- | ---: | ---: | ---: | ---: |
| Board 默认软件包络 | 20000 | 8.983257844e-7 | 8.983257844e-7 | 2.383651729e-7 |
| Virtual 默认软件包络 | 20000 | 5.294064120e-7 | 5.294064120e-7 | 2.384185791e-7 |
| 校准准入包络，\|ω\|≤1e6、age≤650 | 20000 | 9.300888123e-5 | 7.306829514e-5 | 2.384180000e-7 |
| 历史 65535000 / 400 µs | 单点 | .001514806476 | .000729455676 | 1.403358985e-8 |

此外：1216 组零/正/负/小/中/极高/近 float 边界速度和 0/τ/负相位网格，10 个更长/更大输入，7 个 horizon/model/config admission 边界。public_grid 极端值中 C 最大误差仍约 3.13 rad，说明 Double 不是“任意有限 float 的精确周期相位”方案；在某些原实现因 float 溢出拒绝的输入下 C 会返回有限结果，连成功/失败范围都可能扩大，不能无评估直接替换。

候选 B 在某些输入反而更差：历史速度 age=650 µs 时 A 误差约 1.75e-5，B 约 .00118528。原累加和周期偏差偶然抵消被 B 改变，所以不能用一个改善单点证明 B 单调优于 A。

建议先定义三层合同，而非改算法：

1. 数值准入：当前 finite/horizon/错误清输出语义照实描述，不把 OK 当成精度认证。
2. 精度工作域：对 bounded |θ|≤float(2π)、|ωe|≤7000、age≤650 µs，在 binary32 最近舍入、正确 fmodf、无 fast-math 前提下，采用保守 **2e-6 rad 计算误差预算**作为建议验收域；本轮随机集支持它。解析逐项界也支持：dt 项约≤2.2e-7、增量舍入≤2.4e-7、加法≤4.8e-7、归约/负角补偿量级<5e-7，总计<2e-6；不含输入量化和 Sensor 误差。此为审查建议，未擅自修改公共 API 承诺。
3. 系统精度：单独叠加模型贡献、source 相位/速度估计和采样时间误差。默认 Board 最大模型项 .0004225，数值项远小于该量；一个 Encoder LSB 对应电相位约 .00067112 rad，不能忽视源量化而只优化百万分之一 rad 的计算项。此 LSB 只是量纲估计，不是实测完整传感器误差。

是否值得改：**默认域先保留 A、补合同和独立测试。** 若产品明确需要更高电速度、更长 age 或更严总误差，在该明确域比较 C 与其他有界增量方案，再做目标 MCU 时序/全固件资源验证；目前没有证明默认 Board 数值失败，也没有足够证据支持热路径算法迁移。

## 7. Velocity PI：不变量、抗饱和与恢复

设 E=ref−measured、P=Kp·E、I0=clip(Iold,±L)、ΔI=Ki·dt·E。原实现只允许朝 actuator 可用边界前进的积分增量，最后再对 I、P+I 和输出作检查/限幅。独立 Oracle 用“增量在 I0 到边界之间的区间投影”重算，未调用生产 helper 或原分支作为期望。

可证明的不变量：

- 初始化/Reset 后 I=0；每次成功 enabled step 后 |I|≤L、|iq|≤L，且输出 enabled。
- 当 P+I0 已在 +L 外，正积分增量不能继续增加 I；负方向对称。跨越边界时只接纳可支持的增量，允许回到有效区间。
- 每次调用都是“局部计算→完整校验→最后一次 context.integral_a 写入”；失败状态不产生部分提交。72 项外部错误检查包括 Ki/乘加溢出，而不仅是入参 NaN；普通及 ASan/UBSan 均通过。
- disabled 先验证输入，合法时输出清零/disabled，积分冻结；L 变小但 disabled 时 I 可能仍大于新的 L，这是文档指定的 freeze。下次 enabled 先 clamp；不能声称 disabled 也始终满足新 live limit。

20000 组独立 PI 状态/增益/dt/限幅输入，包括零增益、limit 收缩、disabled、正负误差与有限合法历史，最大 I/iq 数值差约 **7.11004529e-7 A**，未发现不变量违反。非有限/非法 dt/limit、uninitialized、init 失败、overflow、reset 等实测见 motion_errors.c。大合法增益若计算溢出会返回错误，不能把“配置 finite”升级为“一切 finite 输入必成功”。

反向饱和并不意味着无条件将 I 清零：若 ΔI 会加深当前另一侧饱和，冻结是既有条件积分策略；它不保证任意反转轨迹的最短恢复时间。原随机独立 reference 能检出错误的负分支边界（变异 index=317），基本纯 I 测例则不能覆盖这一全部策略。

Runtime 在 H1 的 outer_velocity 副本上 step，记录 trial；下一 H1 仅在同 session、合法样本、RUNNING 和 H0 确认了对应 outer seq 时才提交，UPDATE 拒绝不得提交 inner/outer trial。STOP/模式切换使 session 边界生效，H1 下次观察到新 session/非运行态时 reset，历史 I 不被重新启动沿用。它不是 H0 STOP 当场就写 H1 的 I；所有权仍分离。相关事务断言本轮实际执行。

限制：Velocity anti-windup 使用 q **请求限幅**，不直接知道电压饱和后的实际可实现电流。Current 饱和、负载和电机参数下的整体闭环稳定/最优性需单独模型/硬件证据。现有 PMSM 模型的正反速度、负载上/下和位置场景本轮通过，但研究模型结果不能覆盖所有电机。

## 8. Position P：连续机械位置与职责

Position 无内部状态，计算的是连续 ref−measured，不取周期余数。正/负多圈、跨零、快速目标跳变均按同一差值处理，输出速度限幅；它不规划加速度、模式和目标历史。启用/停机由 Runtime，轨迹由 foc_trajectory 负责，分工合理，无需因函数短长重分架构。

20021 组独立输入：10000 个大范围位置、10000 个小误差/未饱和位置、21 个零/正负多圈/巨大坐标边界。最大输出差 **9.519280866e-7 rad/s**，均满足 Vlimit。错误地单圈化差值的变异被两个正式 Motion 测试抓住；本轮 Simulation ASan 的正负多圈轨迹亦运行。

大绝对位置仍受输入 Float32 分辨率影响：1e8 与 1e8+1 在 float 中可能相同，Position 得到的误差就是 0。Integer 电相位整改并没有解决这一机械坐标表示问题；Double 内部减法也不能恢复已经相同的两个输入。此为已声明范围边界，不是本轮发现的错误截圈。

Current 饱和时，Position 只能保证速度目标被限制，不能保证实际速度/位置可达。不能从 stateless P 公式正确推导出任意大目标、母线或负载下的跟踪品质。无 enable 字段、Kp=0 或 limited 标志都不代表物理停机。

## 9. 状态事务与错误路径矩阵

| 类别 | Rotor / Prediction | Velocity | Position |
| --- | --- | --- | --- |
| NULL 输出 | ERR_NULL，无可用结果 | ERR_NULL，context 不动 | ERR_NULL |
| NULL config/context/sample/input | 输出清零、ERR_NULL | 输出清零；state 不动 | 输出清零、ERR_NULL |
| 未初始化 | stateless，不适用 | step/reset 拒绝；init 成功才建立状态 | stateless，不适用 |
| 非有限配置/输入 | 拒绝；预测非有限 dt 不存在，age 是整数 | dt 非有限属于 INVALID_DT；其他输入 NONFINITE | NONFINITE |
| 非法范围 | poles/CPR/count、负预测预算/半范围 horizon 拒绝 | Kp/Ki<0、max_dt≤0、L≤0、dt 越界拒绝 | Kp<0、Vlimit≤0 拒绝 |
| 期限 / 模型不足 | horizon 优先，随后模型项；输出 0 不可用 | 不处理时钟，实际 dt 由调用者给出 | 无 dt |
| 运算溢出 | 电速度乘法、预测乘加/normalize 可拒绝 | trial/requested/target 检查后才提交 I | 差/乘法非有限时拒绝，不能用 clamp 掩盖 |
| disabled | 无该字段，source/Safety 由调用者 | 先验证再冻结状态、清结果 | 无该字段，由 Runtime 决定调用 |
| 恢复 | 新合法 source/time/session 由 Runtime 管理 | reset 新 session，配置保留 | 无状态需要恢复 |

错误路径探针 72 检查实际通过。没有以损坏 context、外部热改只读配置、无效存储或 output/context 非法重叠制造反例。初始化失败保留已有合法实例；stateless 结果失败清零不能被当成成功。

## 10. 现有测试有效性矩阵

| 测试 | 实际证据 | 局限 |
| --- | --- | --- |
| core/test_foc_motion | 方向/单位、期限/模型预算、PI 基本积分/限幅/disabled、NaN 状态保持、多圈 Position | 预测 truth 使用与生产相同的 float dt/fmodf；不能证明高增量精度；未覆盖 arithmetic-overflow 状态不提交 |
| core/test_foc_motion_reference | 5000 double 方程参考，PI 复杂限幅/负恢复、Position，另有 Trajectory bound | **没有调用 foc_rotor_predict**；不能按文件名把它算作独立预测 Oracle；PI 策略参考仍共享分支假设 |
| runtime/test_foc_motion_runtime | source age、输出事务、session/STOP、invalid/missing phase、outer trial ACK | 主要 fixture 是受控输入，不替代 Sensor 物理误差或穷尽调度 |
| runtime/test_foc_motion_model | 正反速度、负载、Position 的真实 production Controller/Runtime＋独立 PMSM | 模型和配置受限，不能认证实际电机工作域 |
| Core periodic phase / Simulation periodic chain | 整数模、同帧、长计数、H0/dq、回绕 | 验证 source 相位，不等于覆盖任意速度预测数值域 |
| Simulation virtual current / control01b / system / mailbox | 正负轨迹、模式/故障/生命周期组合、抢占 hook | 没有实机并发/WCET、真实栈或实际桥关断证据 |
| 本轮外部 Oracle | 61233 预测、20000 PI、20021 Position、72 错误检查和六种目标变异 | 非穷举；不是永久产品门禁，本轮不擅自加入正式测试 |

实测回归：Core Debug 15/15、Runtime Debug 11/11、Core ASan/UBSan 15/15、Simulation ASan/UBSan 15/15（含 Runtime）。这些 lane 互有嵌套，不能相加成独立证明数量。全量 Scope all/Legacy/Board/ARM 固件在上一轮对完全相同 2516 源文件已测；本轮未重跑，未冒充本轮新 PASS。

## 11. 六类变异与独立反例

以下全部修改私有生产源副本、成功编译，正式源码/测试/Golden 未变。0=错误变异仍被该测试放过，1=测试检出。

| 错误假设 / 变异 | 正式 motion | 正式 motion_reference | 独立反证 |
| --- | ---: | ---: | --- |
| 预测速度符号反转 | 1 | 0 | production phase 不符恒速正向参考 |
| µs→s 错为 ×1e-3 | 1 | 0 | 原应成功的 horizon 内输入错误失败 |
| 忽略加速度预算 | 1 | 0 | 7000 µs 原应 ERR_PREDICTION，却成功 |
| 只在计算错误分支提交 trial | 0 | 0 | **全部 Core 15/15 仍通过；外部溢出原子性断言失败** |
| 反向饱和边界用错号 | 0 | 1 | double PI reference 在 index=317 失败 |
| Position 差错误归约为单圈 | 1 | 1 | 正负多圈目标断言与 double reference 失败 |

现有生产实现通过外部原子性断言，所以第四项是**测试有效性缺口，而非当前生产 partial commit 缺陷**。最小反例：合法初始化 `{Kp=0,Ki=FLT_MAX,max_dt=1}`，I=.25，input `{ref=2,measured=0,dt=1,L=2,enabled=true}`；trial overflow，函数应 NONFINITE、原 I 不变、输出 disabled/零。错误分支如果先写 I=Inf，现有 Core 全绿仍成立，独立探针明确失败。

早期驱动假设“reference 测试也检查预测”不成立，实际 PASS 已保留。另一较宽的无条件提前提交变异会改变正常 saturated 标志，正式 basic 测试已能抓住；不能把它的失败当作错误分支原子性已覆盖。因此改用任务明确要求的错误分支 fault model。负恢复也如实记录为 basic 漏检、reference 检出。所有早期目录保留，不删除失败日志、不更改正式断言、不把编译失败算作检出。

探针：[motion_errors.c](2026-11-17-next-code-review-03/motion_errors.c.txt)。从新目录以当前 Core include、foc_core.c、foc_motion.c 编译并执行即可重复；源和结果不需要连接硬件。

## 12. 性能、资源与可移植性

**STATIC ANALYSIS / HOST MEASUREMENT / ARM COMPILE EVIDENCE**，没有 MCU WCET VERIFIED。

新 ARM GNU 14.3.1 `-Os -mcpu=cortex-m4 -mthumb -mfloat-abi=soft` object：

| 函数 | 自身 text B | 自身静态 frame B |
| --- | ---: | ---: |
| counts helper | 124 | 32 |
| Legacy Rotor | 148 | 40 |
| production predict A | 288 | 40 |
| Velocity init/reset/step | 200 / 26 / 576 | 48 / 0 / 48 |
| Position P | 256 | 32 |
| B 主函数 / C 主函数 | 68 / 224 | 24 / 32 |
| B/C 共用验证 helper | 264 | 32 |

B/C 的自身 frame/text 不能遗漏共用 validate、normalize/libm 或嵌套调用。Core normalize 在该构建调用 angle cache prepare，其中有 fmodf、sinf、cosf；新 helper/H1、H0 predict/Current 的计算分布保持原样。C 增加路径上的 `__aeabi_*d*`、fmod(double)、float/double 转换；该参考构建使用 Soft-float，未采用硬件浮点指令。

Host Windows Clang 22.1.8 -O2，QPC，三轮各每候选一百万调用，中位数 A=30.50 ns、B=34.60 ns、C=11.27 ns。C 没有调用会额外计算 unused sin/cos 的公共 normalize，因此这既包含精度改变也包含调用结构差异；不能解释为 Double 在 Soft-float MCU 上更快。

独立最小 ARM 链接探针 `.text/.rodata`：A=6356/996、B=6400/996、C=5484/32 B。该最小程序只保留一个预测入口，C 不保留 float trig 链，所以数字反而更小；真实固件 Current 已经需要 trig，不能据此宣称整个固件能减少 1836 B。它也不是已经完成的全固件替换资源比较。nosys 既有链接警告保留。

实现复杂度/API：A 不动生产且无新增成本；B 额外 fmodf、没有普遍数值优势；C 增加 double 计算与失败范围变化，收益明确于扩展有限域，成本/精度准入必须单独评估。未对任何候选作目标周期、抢占 jitter、最坏栈或故障关闭延迟承诺。

## 13. 跨文件契约登记

| Contract Owner / Implementation Owner | Affected Callers / Tests | Evidence / Suggested Next Action |
| --- | --- | --- |
| Core Prediction 合同 / foc_motion.c；Runtime 负责 source age 和配置 | H0 align/init，motion/motion_reference/runtime model | max_error 只控制模型 contribution；明确数值域/源误差分账，禁止把 OK 解释为系统总误差认证 |
| Sensor 来源 / Encoder＋Board/Virtual glue | H1 producer、Current phase，periodic chain/Sensors | directed 整数计数与 CPR/rotor config 同源；KEEP 已接受链，不重新修改 |
| Velocity 状态 / Core PI；Runtime 外环 ACK/session owner | H1 prepare_outer、H0 accepted_outer，motion_runtime | 局部 trial＋下游确认后提交、session reset；正式测试补 arithmetic error atomicity |
| Position/Trajectory / Core stateless P＋trajectory＋Runtime | 多圈/位置/速度模式，model/control01b/virtual current | 不 wrap机械差；外环限速与轨迹加速度分工 KEEP；饱和场景品质另测 |
| 质量门禁 / scripts/gates | RequireCMakeFormatter、CI=true | **OPEN — NEXT-QUALITY-GATE-01**，上一审计已复现且源码未变，本轮不修复或宣称 strict PASS |

## 14. KEEP 项及保留方论证

- Integer source 与连续机械位置分离、旧 Rotor API 保留：关闭已批准的累计位置退化，同时维持位置语义；本轮无理由重开这次迁移。
- 默认域恒速预测 A：计算误差小于建议 2e-6 的证据充足，当前主要 uncertainty 还包括模型/Sensor；尚无正常数值工作域失败反例，替换成本收益不足。
- Velocity 条件积分和一次最终提交：量纲、限幅、不加深饱和、disabled 冻结和错误不提交均有源码与独立反证；无需照搬 Current PI 或引入新 AW 框架。
- Position stateless continuous P：职责明确，无单圈截断；轨迹和模式由现有 owner 完成。不因函数长短或 libm 调用数量做改写。
- Runtime 的 H1 trial/H0 output ACK、session、STOP/FAULT、单核优先级和 Board 输出双锁：原代码和本轮回归支持保留；不扩大到任意 Host 多线程或真实功率输出。

## 15. IMPROVE 项与支持修改方论证

### CR03-F01｜P2｜预测数值工作域与误差分账不完整

位置：foc_motion.c:91/117/121、foc_motion.h prediction contract、Core/Runtime README。合法类型反例输出误差 .001515 而仍成功，默认/校准/公共 API 范围明显不同。现有 contribution 描述没有承诺总误差，因此**不将它认定为已违反模型预算的算法错误**；问题是不能据此验收公共 API 的全域精度。

最小整改：文档明确模型项与计算项/输入/Sensor 项，声明支持的精度域和域外行为；补独立 stored-input Oracle。暂保留 A。关闭条件是域内解析界与边界/随机误差测量一致、域外不产生虚假保证，实际产品精度目标可追溯。若要求支持更高域，另评 C/其他候选和 MCU 代价。风险：过度缩域可能不必要地改变兼容性；仅修契约不会提升真实测量品质。

### CR03-F02｜P3｜计算错误状态事务缺少永久负向 Oracle

位置：core/tests/test_foc_motion.c 的 Velocity 错误测试、core/tests/test_foc_motion_reference.c；生产 foc_motion.c:193/211 的正确事务目前可 KEEP。错误分支提交 Inf 的变异通过全部 Core 15/15，独立探针失败，证明现有 PASS 没有覆盖该契约。

最小整改：把本轮 Ki/计算溢出后 context 完整保持、输出清零的负向检查迁入现有 Motion 测试；同时让 prediction 拥有真正独立的 double/高精度参考。关闭条件：正确实现通过、错误分支 commit 变异失败、三个预测方向/单位/模型变异均失败；不改 PI 算法、不放宽容差。风险：不能把“新增测试更充分”写成原生产 PI 已坏。

### CR03-F03｜P2｜OPEN — NEXT-QUALITY-GATE-01

与算法无关的已有问题。严格 Formatter/CI 的 PowerShell SwitchParameter 转发失败在 AUDIT02B 已确认，当前 gate 文件相同。保留独立 owner 和复现条件，本轮不修改；关闭需 strict/non-strict/CI 三路实际调用 Formatter 并正确传播失败。默认 Scope all 的历史成功不关闭它。

## 16. BLOCK 项

**无确认 BLOCK，无 P0/P1。**没有找到当前生产 Velocity 部分提交、Position 单圈截断、整数来源回退或新的 Board 输出解锁路径。当前证据不足以称软件零缺陷，也不以这一点制造阻断。

## 17. NEEDS EVIDENCE 项及取证条件

- **P2，系统误差和物理工作域**：需实际 motor/Encoder/clock 标定与产品误差预算，分开相位量化、速度估计、时间误差、加速度假设；取真实 raw/time/LPF/θ/ω trace 对照参考。名义 max_speed、最大 poles 不等于可运行转速；软件数值研究不能关闭此项。
- **P2，MCU 时序/栈/候选代价**：输出保持锁定，在独立授权和硬件条件具备后测 H0 正常与恢复路径的周期、jitter、IRQ 嵌套和栈高水位；若选 Double，还需同一固件/工况前后完整链接与 timing。不能用 Host ns、最小链接或单函数 .su 关闭。
- **P2，内外环共同饱和时的控制品质**：先固定电机模型/母线/负载/command 域与 RMSE/恢复/积分限值，再跑 holdout/极限电压扰动；需要实机时另经硬件准入。现有电流限幅 AW 只保证请求/状态有界，不保证所有负载下可达或最优。

## 18. 最多五项建议、只读收尾与交付

1. P2：先收口 Prediction 的模型/数值/源误差合同和默认/扩展工作域；保留 A。
2. P3：在单独测试整改中迁入 arithmetic-overflow 原子性负向检查与独立预测 Oracle；保留本轮六种反例。
3. P2：独立执行 NEXT-QUALITY-GATE-01，只修 switch 转发并覆盖 strict/CI；本轮状态 OPEN。
4. NEEDS EVIDENCE/P2：在考虑改变热路径前，获取目标 MCU 周期、栈、实际 age 和候选全固件资源证据。
5. NEEDS EVIDENCE/P2：按真实产品目标验证电压饱和/负载下的内外环品质；不能凭代数有界宣称控制最优。

审查阶段只读实测：2516→2516 项目文件、2427→2427 ignored 产物，无新增/缺失/内容变化；Git HEAD/分支/status/diff 和主工作树 status 未变，见 [readonly-verification.json](2026-11-17-next-code-review-03/readonly-verification.json)。这份 JSON 的时间范围截至授权提交交付之前。之后仅新增交付文档/证据、按 AGENTS 追加维护记录，并提交起始已存在的成果；原生产/正式测试/构建/Golden 字节保持。提交/推送的最终 SHA 和交付期差异由独立 publication-verification.json 记录，不能与只读阶段混写。

核心交付：[motion-numerical-oracle.json](2026-11-17-next-code-review-03/motion-numerical-oracle.json)、[motion-test-matrix.json](2026-11-17-next-code-review-03/motion-test-matrix.json)、本报告、只读验证。原始源码副本、CSV、编译/运行日志、最小 ARM 链接 map、.su、nm、反汇编均在新隔离目录。Git 仅交付审查文档、摘要和可重复探针，不把 build 缓存/整套副本塞入仓库。完整 raw CSV 留在外部证据目录，位置和 SHA-256 写入摘要。

最终 **IMPROVE / P2**：本轮完成审查，不自动修复、不进入下一文件；仅执行用户新增的 Git 提交推送交付。

提交交付期另发现：此前未跟踪的 `docs/evidence/2026-11-16-next-control-quality-01.md` 在第 187 行有多余 EOF 空行，首次纳入 index 后 `git diff --cached --check` 返回 2。为保留历史 evidence 字节，本轮不修剪该文件；这项已有文档空白问题与 Motion 代码结论分开记录，未把 cached whitespace 检查声明为通过。初次发布保护断言还因 Git 中文路径引用方式不一致触发，统一状态读取方式后确认原文件未变；两次保护退出均保留日志。

```text
G3-SAFE HARDWARE NOT VERIFIED
G3-PWR LOCKED
AT32F415 WCET NOT VERIFIED
MOTOR PHYSICAL OPERATION UNVERIFIED
```
