# FalconFOC｜项目纲领、技术原则与研发记忆基线

**版本：** V1.0（决策与交接基线）  
**整理日期：** 2026-11-17  
**项目：** FalconFOC V2 / Hollow-Gimbal  
**定位：** 产品宣言 · 架构决策 · 源码优化方法 · 开源借鉴规范 · 项目进度 · 后续执行约束  
**来源等级：** 用户明确认同的原则 + 项目历史记录 + GitHub 固定版本的只读核查  
**重要提示：** 本文不是新的生产代码验收报告，也不代表授权更改 Git、解锁硬件或发布产品。

> **FalconFOC — Portable, Deterministic, High-Performance Motor Control.**
>
> FalconFOC 致力于提供一个开放、轻量、高性能的电机控制核心，让开发者不再被特定芯片、操作系统或复杂固件框架束缚。
>
> 我们不仅追求电机能够运转，更追求精准、稳定、安全、可测量的控制品质，并通过参考固件、仿真、标定和工具链，让高质量 FOC 更容易落地。

## 0. 执行摘要：十项不得漂移的决定

1. **产品身份不变。** FalconFOC 首先是一套可独立集成的 **纯 C11、跨 MCU、无强制 RTOS 的 FOC 控制库**；不是只为 AT32F415 编写的一块板卡固件，也不是以框架规模为目标的嵌入式操作系统。
2. **性能承诺必须有证据。** High-Performance 指实际控制品质、实时性与资源效率，不等于 README 写着 20/40 kHz，不等于 Host CTest 通过。
3. **以 FalconFOC 为核心。** 外部项目提供技术原理、对照算法、测试设计和工程经验；**不以移植对方的整体固件架构替代我们的产品设计**。
4. **借鉴有边界。** 借鉴/移植均按本项目 C11、命名、API、生命周期、错误语义、状态所有权和代码风格执行；如实保留适用的开源许可与来源，不以改名掩盖派生代码。
5. **恢复逐文件精修主线。** 采用 `File-Centered, Contract-Aware Review`：一个主文件，必须看头文件、调用者、依赖、测试及跨文件契约；`REVIEW → 有证据的 IMPROVE（或 KEEP）→ AUDIT`。
6. **KEEP 是有效成果。** 没有明确缺陷、可证明收益或维护风险时不改；不为“看起来更专业”而重构、不为凑测试数量而无限审核。
7. **功能开发同等重要。** 已有基础 FOC 后，重点补齐电机参数辨识、自动整定、传感器校准、控制品质和工具体验；不会让代码审查持续挤占产品开发。
8. **保持边界。** Core 不依赖厂商头文件、RTOS、通信协议或板卡；硬件热点可以在 Port / Reference Firmware 采用直接寄存器、DMA 和厂商驱动，不强行增加厚 HAL。
9. **证据分层。** 数学单测、Host 闭环、目标 MCU 时序、实机控制与功率级安全不得互相冒名；安全许可和硬件门禁不因软件 PASS 自动解除。
10. **先读最新事实。** 任何新 Agent 先检查 `AGENTS.md`、指定分支 HEAD、未提交改动、最新报告和来源基线；旧提案、旧 README 和聊天摘要不得覆盖新事实。

## 1. 初衷：我们究竟想解决什么问题

FalconFOC 的 Legacy 固件并非完全不能运行。真正促使 V2 重构的是：固件与 AT32F415/Board1 紧耦合；FOC、FreeRTOS、Shell、硬件、产品状态互相交织；移植和首次使用成本高；历史调试/参数/状态逻辑混杂；缺少适合第三方开发者的清晰接口、示例和协议契约。

因此目标不是写出更多嵌入式胶水，而是让开发者能够 **只引入 FalconFOC Core**，在自己既有的 ISR、调度器、ADC/PWM 及编码器驱动上完成独立集成。我们的参考固件可以使用裸机 Runtime，但不能反过来要求 Core 用户照搬该 Runtime。

**三类交付物：**

- **FalconFOC Core：** 纯 C11 控制数学、电流/速度/位置环、调制和必要的控制状态，显式输入输出、多实例、无隐式外设依赖。
- **FalconFOC Reference Firmware：** 用真实板级 Port 串起 ADC/PWM/Timer/Sensor、运行时、物理安全执行与可测量时序；AT32F415 是第一款参考设备，不是永久边界。
- **FalconFOC Tools & Protocol：** 版本化 UART V1 命令/状态、主机 CLI、仿真、配置、标定和诊断；未来其他 Transport 复用语义而不污染 Core。

**不是当前目标：** 首版支持所有电机/所有电流采样拓扑/所有无感算法；为当前仅有 USART1 外部链路的 Board1 强行引入 CAN-FD/EtherCAT；把硬件实测条件当作可移植的默认常量；通过增加通用设备框架取代直接且确定的高频热路径。

## 2. 产品价值观与技术不可变条件

**FalconFOC 的架构必须由产品目标、成熟工程方法和可验证证据共同决定，不能随着每一次讨论的方向反复变化。** 重大变更以证据和复验条件形成正式 ADR，由后续裁决显式替代。 [架构变更规则](FalconFOC_BareMetal_Execution_ADR.md#architecture-change-gate)

| 追求 | 必须落实为具体工程条件 | 不接受的替代说法 |
|---|---|---|
| Portable / 可移植 | 第三方可不使用我们 Runtime；第二不同 MCU/采样组合实际移植；板卡常量局部化 | “Host GCC 能编译，所以通用” |
| Deterministic / 确定性 | 显式采集/执行/提交时间、最坏路径、状态事务、超时和故障语义 | “大部分运行时间足够快” |
| High-Performance / 高性能 | 电流、速度、位置误差与带宽；过冲/稳态/抗扰；WCET、抖动、Flash/RAM/栈 | “控制频率高、算法很多” |
| Open / 开放 | 简明 API、示例、测试、移植说明、源码许可/版权归属明确 | “代码公开即生态成熟” |
| Lean / 轻量 | 只引入必要依赖；静态实例、无必须的动态内存与线程；可选择功能 | “抽象越多越好” |
| Reliable / 可靠 | 明确错误路径、积分器提交、STOP/FAULT/INHIBIT、恢复边界、可复现证据 | “软件 PASS 即功率级安全” |

**公共约定：** 物理量使用 SI 单位（A、V、rad、rad/s、s），机械角与电角分开；三相电流极性、Clarke/Park 定义、相序、角度零点必须统一；样本源时间和输出生效时间不可混淆；状态由实例/指定执行域拥有；错误结果不得留下半更新状态。

## 3. 当前架构：保持解耦，但不为抽象付费

```text
Host / CLI / UART V1
        ↓   版本化控制语义
Foreground → 请求/会话 → Runtime（H0 / H1 / 监督）
                         ↓         ↑
              Current / Motion / Trajectory Core
                         ↑         ↑
                Sensor / Calibration
                         ↓
                 Device / Safety / Port
                         ↓
         Host Virtual Motor   或   AT32F415 Board1
```

- **H0**：电流采样、角度对齐、Current、SVPWM、最终输出请求；拥有高速控制事务及功率输出提交边界。
- **H1**：传感器观测、速度/位置外环、校准数据的有效发布；不得越过 H0 的真实输出提交权。
- **Foreground**：命令、协议、状态快照、诊断和非高频服务；不能直接修改 H0 已拥有的安全状态。
- **Core**：不获取 MCU 时钟、不读寄存器、不打印日志；输入、状态、输出显式；必要时使用内部受控快速路径，但不能绕过动态安全检查。
- **Port/Board**：GPIO/PWM/ADC/DMA/SPI、采样拓扑、电流增益/偏置、逆变器保护及真正的物理输出权限。

**重点保护的跨文件契约：** 新命令不得绕过 Session；STOP 优先于 UPDATE；故障锁存与清除不能自动解锁；传感器有效性与捕获时间属于同一个观测；输出请求被拒绝时不能提交电流 PI 状态；多圈机械位置不得退化成大 Float32 相位累加。

## 4. 最新可确认的开发阶段与证据边界

**仓库：** `https://github.com/2023violet/Hollow-Gimbal`  
**目标分支：** `codex/falconfoc-v2-vm01a`  
**本次只读核对远端 HEAD：** `f00e9a414d4de2e4d7a82cab47ebbeef6d54179b`（2026-11-17）  
**外部参考：** `cyberbeast0829/robot_mc`，`CBA03_v4.2.89` = `2f1fb61ec081eb7a42ab8436ba8f1504fc529da7`。

| 领域 | 现状 / 证据 | 裁决 |
|---|---|---|
| 独立 C11 Core | Clarke/Park、SVPWM、Current PI、Velocity PI、Position P、Trajectory；Host CTest | 软件基础已建立 |
| Sensor 与 Runtime | Encoder 计数/多圈位置/差分+LPF+Alpha-Beta、H0/H1/Foreground、输出事务 | 已实现，继续逐文件优化 |
| 仿真 | Host Virtual Motor、Current/Velocity/Position/Load/Fault、CLI 与可复现 CSV/JSON | 集成已证明，不等于控制品质合格 |
| 当前 Control Quality | 历史 `NEXT-VM-01d`：Current `CONTROL_QUALITY_FAIL`；其他运动场景 `EXPECTED_DEGRADED` | 必须保留问题标签，需核对新基线再判当前表现 |
| QUALITY-GATE-01 | PowerShell 参数转发修复、Strict/CI gate 通过，远端 `6b151a1` | PASS，已关闭，不重复修复 |
| MOTION-CONTRACT-01 | 四类错误变异检出、74,684 样本域内预测回归，生产 Motion 算法未变 | PASS，远端提交 `f00e9a4` |
| SYSTEM-ANGLE-01 | 用户已交 Codex 执行 | **进行中/待结果**；本次无验收报告，不得记 PASS |
| 真实 AT32F415 | 独立参考固件与构建/资源记录存在 | 没有 AT32F415 WCET、真实电机品质与物理输出验收 |
| 正式开源发布 | 软件准备、Host 外部消费者证据存在 | 根许可证选择与第三方组件来源核查未最终关闭 |

**永久保留的硬件状态标记：**

```text
G3-SAFE HARDWARE NOT VERIFIED
G3-PWR LOCKED
AT32F415 WCET NOT VERIFIED
MOTOR PHYSICAL OPERATION UNVERIFIED
```

除非出现明确的新硬件测试报告与授权，否则不得擅自修改上述状态。Host 模型的 `physical_applied=false` 不等同于真实桥臂已关断。2026-11-15 的 AI handoff 属于旧 RC0 交接入口，最新分支/报告优先；仓库 README 中尚未更新的 `Scope all` 环境阻断描述也不能推翻 11-17 新验证。

## 5. 历史过程：哪些工作值得保留，哪里曾经偏航

- **2026-11-07 / 11-11：** 决定从单板 Legacy 固件转向可移植 C11 Core、参考固件与工具生态。曾明确要求：先做好真实电流闭环、速度/位置和 Host 验证，避免过早扩张 Runtime/硬件审查。
- **2026-11-15 前后：** 软件 RC0、Virtual Motor/Runtime/Serial/Calibration/Host Research 已建立重要基础，但 Control Quality 仍有不合格或降级标签。
- **2026-11-16～17：** 逐文件 `NEXT-CODE-REVIEW / IMPROVE / AUDIT` 聚焦 Current 和 Motion/电相位；缺陷修复、数值合同、严格 Gate 均形成可追溯证据。
- **2026-11-17：** 用户纠正：数学审查/质量门禁属于工程附加保障，不能无限延伸为开发主线。`NEXT-SYSTEM-ANGLE-01` 已在 Codex 执行，正常收口即可；不应据此再衍生下一轮纯数学专项。
- **2026-11-17：** 研究 CyberBeast `robot_mc` 后明确：已有成熟基础 FOC，但电机辨识、自动整定、编码器线性化、阻抗控制、齿槽补偿、惯量辨识及真实效率数据仍明显不足。

**纠偏原则：** 不否定既有验证成果；它们是质量底座。今后每个开发周期优先产出有价值的生产能力或有依据的实现质量改进，测试与文档以该改进所需的程度为限。避免“审查→增加测试→再审查”的无终点循环。

## 6. 外部参考：学习成熟技术，不丢失 FalconFOC 身份

| 参考 | 主要学习内容 | 明确不照搬 |
|---|---|---|
| SguanFOC | 算法模块化、有感/无感与扩展方向 | 不以算法数量当成熟度，不一次引入所有观测器 |
| SimpleFOC | 易用 API、跨 MCU 使用体验、资料与示例 | 不绑定 Arduino/C++ 抽象模式 |
| moteus | 完整电机使用流程、标定、协议和主机工具 | 不将库绑定其具体硬件和协议 |
| ODrive | 级联电流/速度/位置、运动模式、参数整定经验 | 不复制整个固件和配置框架 |
| CyberBeast robot_mc | 实际关节驱动固件、40 kHz ISR 目标、ADC/SPI DMA 重叠、查表三角、R/L→PI、编码器非线性校准、力控与负载等工程经验 | 不复制 N32H473 全局状态、板级驱动与产品特定硬件时序；40 kHz 不是 FalconFOC 的已达指标 |

**来源事实：** `robot_mc` README 声明基于 Ben Katz Mini Cheetah `motorcontrol`，包括部分 ODrive MIT 衍生算法；根许可证为 MIT，`Drivers/`、CMSIS、生成外设文件仍有独立版权条款。对方也吸收成熟开源成果，这证明合理的再实现与许可合规并不矛盾。

### 外部代码纳入标准（必须）

1. **先分类来源：** 通用公开公式/技术思想、独立重写、直接复制或改编、供应商 SDK/生成文件，分别记录。
2. **先审许可证：** 按具体文件核查来源、许可兼容、版权声明、NOTICE 需求及历史上游；根目录 MIT 不覆盖所有下游文件。
3. **按 FalconFOC 契约实现：** 纯 C11、显式单位与时间、模块独立、实例拥有状态、无强制 RTOS、错误原子性、既有代码风格，不能为了迁移方便污染 Core。
4. **不做“换名伪原创”：** 重命名函数/变量不改变派生代码身份；需要归属时保留归属。真正独立实现应源于原理与独立设计/测试，而非逐行同义替换。
5. **通过比较性验证：** 关注输出、误差、边界、资源及目标 MCU 上的 WCET；不能仅凭外部工程声称高频运行就直接选择。
6. **未提供证据则不合入：** 候选优化需有明确目标、反例或基准、最小差异、回退与许可结论。

**FalconFOC 自有代码标准高于“命名不同”。** 独立 API、时间与状态契约、代码风格、独立测试和证据归属共同构成技术身份。

## 7. 逐文件深审的正式工作制度

**固定方法：** `File-Centered, Contract-Aware Review`。每轮以一个关键主文件为中心，读完相关头文件、调用者、被调用函数、正式测试、现有证据及跨文件契约。不是按字母顺序清理仓库，也不是每个文件必须改动。

**任务顺序：**

1. **BASELINE：** 检查 Git HEAD、工作树、已有未提交内容、构建入口、历史审计；不误判已完成问题为新缺陷。
2. **REVIEW：** 写出函数地图、前置/后置条件、错误路径、状态所有权、时间语义、API 边界、依赖、并发、资源与测试 Oracle；标记 `DOCUMENTED / IMPLEMENTED / INFERRED / UNSPECIFIED`。
3. **EVIDENCE：** 对怀疑的问题先提供可复现反例、剖析报告或同条件性能数据。把“可能慢/代码长/风格不喜欢”与确认缺陷分开。
4. **DECISION：** `KEEP`（现实现合理）、`IMPROVE`（有明确净收益）、`BLOCK`（确认关键错误）、`NEEDS EVIDENCE`（缺哪项证据、怎么取得、谁承担）。
5. **IMPROVE（必要时）：** 最小修改；凡改了 API、参数语义、状态事务或跨文件约定，必须同步修改其所有必要调用点和测试，不留半成品。
6. **AUDIT：** 独立视角核查差异、回归、负向变异、性能/资源数据、许可与回退；合格后收口。没有改动的 KEEP 同样是完结项。

### IMPROVE 的准入条件

必须同时说明：**缺陷或收益证据、影响范围和最小修改方案、验证与回退方法**。纯重命名、无收益的泛化、拆分长函数、无限加注释不自动构成 IMPROVE。性能优化至少比较功能等价、错误路径、Flash/RAM/stack、每周期执行代价与最坏情况；目标 MCU 数据缺失时标记 `NEEDS EVIDENCE`。

### 评审停止条件与跨模块风险

- 每个公开 API 的关键契约和状态/错误路径已复原，关键失效模式有针对性测试或已存在证据；不要求人为凑固定数量的“问题”。
- 问题位于跨文件契约时，归属到契约所有者并关联必要模块，不强行在单文件内消化。
- `KEEP` 不能是未阅读源码的橡皮图章；`IMPROVE` 也不能是为证明工作量而制造重构。
- 不因为修改了实现内部细节就自动要求全仓库重新审查；只有行为/接口/时序/所有权契约发生实质变化时扩大受影响回归范围。

### 与产品能力开发的关系

- **轨道 A：质量精修。** 逐文件深入评审/有证据改进，守护独立 Core、确定性、安全、可移植性。
- **轨道 B：控制能力。** 电机辨识、自动整定、传感器策略、控制品质、调试体验等按用户价值推进；每个新增能力仍接受同等代码质量标准。
- 两轨共享相同的模块边界和测试，而不是分别造两套框架。性能专项应服务真实热路径瓶颈或闭环性能问题。

## 8. 当前已发现的具体优化研究点（不是缺陷裁决）

| 文件/路径 | 已见事实 | 合理的下一步 | 注意事项 |
|---|---|---|---|
| `core/src/foc_core_internal.h` | 单拍角度缓存已复用 sin/cos，缓存准备仍含 `fmodf + sinf + cosf` | 在 Soft-float/ARM 工具链上隔离测量并评估受控 LUT/CORDIC/近似方案 | 改变相位误差、归约域、ROM 表容量必须独立验收 |
| `core/src/foc_current.c` | 已有矢量限幅、真实 duty 反推、`hypotf` 校验及饱和时可能二次调制 | 比较常态与饱和最坏路径，关联 Current Quality，确认是否值得优化 | 不能删掉反算和保护来换速度 |
| `runtime/src/foc_runtime.c` | 存在 H0/H1 所有权、双槽、原子快照、生命周期、STOP 和会话逻辑 | 作为下一项逐文件深审主文件候选，审 CPU/栈/状态重复及复杂性 | 不能简单压缩代码而破坏 STOP/UPDATE 顺序 |
| `runtime/src/foc_pipeline.c` | 拥有电流事务、输出适配、安全准入 | 与 Runtime 配对审查状态提交和拒绝路径 | 保留最终输出许可 |
| `sensors/src/foc_encoder.c` | 整数计数、差分、LPF、Alpha-Beta 同时存在 | 测实际选用速度源、低速行为、计算成本；评估 PLL/LUT 价值 | 不照搬 robot_mc 的齿比和硬件时序 |
| `calibration/src/foc_calibration.c` | 有偏置、转子、角度等候选会话；不直接应用结果 | 逐步扩展 R/L、磁链/Kt 辨识与验证 | 每类标定独立契约、失败不应用 |
| AT32F415 Board/Port | MCU 无硬件 FPU；当前参考时序参数是声明而非 WCET | `-mfloat-abi=soft` 下链接 libm/Flash/RAM/栈/周期实测 | 不把 `robot_mc` M4F 的 40 kHz 宣称移植后可达 |

上述项目是**候选与待证事项**。一切性能数字都必须绑定具体 MCU、编译器/选项、控制路径、参数域和硬件证据。

## 9. 研发路线与推荐先后（非自动授权）

| 阶段 | 目标交付 | 验收依据 | 先决关系 |
|---|---|---|---|
| 当前 | 收口 Codex 已开始的 `NEXT-SYSTEM-ANGLE-01` | 实际报告和源码差异；不再连锁启动纯数学专项 | 用户已在执行 |
| A1 | 恢复 `NEXT-CODE` 逐文件深审，候选 `foc_runtime.c` | KEEP 或有证据的最小改善；H0/H1/STOP 完整回归 | 先核实际最新基线 |
| A2 | Current 热路径真实性能基线 | GCC/Clang Host 对照 + ARM Soft-float 代码/周期/ROM/RAM，区分常态与饱和 | 无测量先不换三角函数 |
| B1 | Current Control Quality 问题归因与必要改进 | 相同 Host Profile 下的电流阶跃、限制/饱和、稳态/恢复；不修改 Board 默认增益伪造 PASS | 保持测试条件一致 |
| B2 | 电机 R/L 识别及 PI 自动整定 API | 独立模型与有效参数域、可回退的校准候选、必要防回归 | 控制律与采样契约清楚 |
| B3 | Encoder 非线性 LUT、速度 PLL 等按证据选型 | 定量低速抖动、动态滞后、ROM/CPU 消耗、失效恢复 | 可在不动 Core 公共接口下扩展 |
| B4 | 磁链/Kt、惯量识别、阻抗/齿槽补偿 | 专项场景和真实电机应用证据 | 基础标定与控制品质成熟 |
| 平台线 | 第二 MCU/采样组合、参考固件时序、上手体验 | 外部使用者可复现、真实 WCET/栈/故障与安全门禁 | 需独立硬件授权 |
| 长期 | 弱磁、MTPA、HFI、SMO、无感切换 | 对应模型与采样可观测性、热与故障边界 | 不是 V2 当前阻断项 |

**路径决策规则：** 如下一轮的实测发现最严重的问题是 Current Quality 或 Soft-float 时延，可以调整 A1/A2/B1 顺序。排序由实际问题和产品收益决定，不由文件名单决定；若当前 `SYSTEM-ANGLE-01` 发现 P0/P1，先处理实质缺陷。

## 10. 面向未来 Agent 的执行协议

**启动任何新任务前：**

1. 读本项目纲领和 `AGENTS.md`；读取指定分支最新 Git SHA、工作树保护状态、最新任务证据及直接相关的源文件。
2. 单句说清这轮属于 **新增产品能力、现有代码精修、构建/安全缺陷修复或实验研究**；必须指出结果怎样帮助 FalconFOC 变成更好的 FOC 库。
3. 提供可验证成功条件和边界；修改只围绕目标及正确性所需的调用点，保留已有通过的契约。
4. 优先复用现有 Core/Runtime/Sensor/Simulation 测试和实验设施，不为单次任务建立新的大型框架。
5. 区分 **已验证事实 / 假设 / 建议 / 仍待确认**；不要把上次聊天中的候选计划写成已经交付。
6. 修改前保护用户代码及构建产物；涉及删除、历史改写、合并、推送、许可/安全权限、刷机上电等不可逆或共享状态操作时先取明确授权。
7. 结束时报告：实际修改哪些生产能力、是否实现正向收益、哪些测试实际重跑、失败/降级状态是否保留、硬件状态是否改变、下一阶段是否有必要。
8. 如果只有测试或文档增量，要明确说明它为何直接解决了现实问题；不得把它包装成控制性能提升。

**避免偏离的自检问题：** 这次是在写 FOC 代码、提高可量化的控制品质/效率，还是只增加审查材料？如果只是在后者上循环，应停止并重新选择收益更大的任务。

## 11. 状态优先级、更新机制和记忆存档

### 11.1 信息来源优先级

1. **最新源码和 Git 事实：** 当前分支、HEAD、diff、正式测试结果与新验收报告。
2. **用户明确批准的产品原则与约束：** 本文件的纲领部分；其变化需明确新决定，不得被 Agent 推测覆盖。
3. **正式架构/质量报告：** 采用与当前源码同一基线的报告；旧报告只能解释历史。
4. **聊天历史与项目记忆：** 用于还原初衷和决策背景，不能代替代码事实。
5. **外部项目与技术博客：** 提供学习素材，不能直接证明 FalconFOC 性能。

这里要区分两类冲突：**Git 事实**决定“现在代码做了什么”；**用户已确认的产品原则**决定“是否允许朝该方向做”。两者不互相替代；当发生实质冲突时必须暂停并重新裁决，而非自动选择一方。

### 11.2 本文维护方式

- 本版为 `v1.0`；以后原则变化使用新版本并写 `Decision ID / 日期 / 旧决定 / 新决定 / 理由 / 影响模块 / 授权依据`。
- 普通开发进度不重写产品纲领；仅更新“实际 HEAD、模块状态、当前任务、未关闭风险、下一步”快照。
- `NEXT-SYSTEM-ANGLE-01` 的最终结果在新证据出现之前保持 Pending；对尚未完成的工作不可预先宣称 PASS。
- 将此文档的短版摘要用作新 Agent 的上下文入口，定期与仓库现状对齐。
- 本文件保存产品原则。具体 V2 执行基线和版本化变更过程见 [ARCH-001](FalconFOC_BareMetal_Execution_ADR.md)。

### 11.3 已达成的原则性共识与仍需做的决定

| 性质 | 内容 |
|---|---|
| **已确认** | 上述英文/中文产品宣言；纯 C11/无强制 RTOS/跨 MCU；FalconFOC 独立代码风格和架构；逐文件深审 + 有证据才改；成熟开源技术可合法借鉴；不能无限扩张数学审查 |
| **当前事实** | 分支 `f00e9a4`；Motion 与 QG 测试项关闭；Host Core/Runtime/Simulation 存在；硬件与 Control Quality 仍有限制 |
| **执行中** | `NEXT-SYSTEM-ANGLE-01`，由 Codex 在本次讨论时执行，未收到最终验收记录 |
| **建议但未批准为固定顺序** | 下一文件优先审 `foc_runtime.c`；Current 实时热路径分析；R/L 辨识与自动整定、Encoder PLL/LUT、阻抗/齿槽/惯量等能力分期 |
| **需要独立决策** | FalconFOC 正式根许可证、第三方代码清单/NOTICE、第二参考平台选择、实机上电/输出解锁和发布范围 |

## 12. 给新对话 / Codex 的极简恢复指令

> 你正在开发 FalconFOC V2，不是普通单板固件。先确认 `2023violet/Hollow-Gimbal` 的 `codex/falconfoc-v2-vm01a` 最新 HEAD、工作树及 AGENTS.md，并将产品宗旨固定为：Portable、Deterministic、High-Performance、Open、Lean。Core 为可独立使用的纯 C11 FOC 控制库，AT32F415 只是参考目标；当前板只有 UART。恢复 `File-Centered, Contract-Aware Review`：一个主文件 + 契约上下游，先 REVIEW，只有证据充分才 IMPROVE，最后独立 AUDIT；KEEP 合法。既要逐文件精修，也要持续补齐辨识、自动整定、编码器与控制品质等真实产品能力。学习 robot_mc/ODrive/moteus/SguanFOC/SimpleFOC，但必须按 FalconFOC 自身 API、状态/时间契约与代码标准实现；改编代码须遵守原版权与许可证。不要为了凑审查、数学测试或代码行数而偏航。Host PASS 不代表实际 MCU WCET、电机控制品质或功率级安全。`NEXT-SYSTEM-ANGLE-01` 当前仅记录为执行中，结果待验；不要擅自推送 Git、刷机或解锁 PWM。

## 13. 主要证据与资料入口

1. FalconFOC 代码与当前分支：`https://github.com/2023violet/Hollow-Gimbal/tree/codex/falconfoc-v2-vm01a`
2. 本次固定 HEAD：`https://github.com/2023violet/Hollow-Gimbal/commit/f00e9a414d4de2e4d7a82cab47ebbeef6d54179b`
3. 项目目标早期设计：[`docs/superpowers/specs/2026-11-07-falconfoc-v2-platform-design.md`](../superpowers/specs/2026-11-07-falconfoc-v2-platform-design.md)与用户 11-11 产品重定义记录；早期属于 Proposal，非最新验收。
4. Code Review 03：`https://github.com/2023violet/Hollow-Gimbal/blob/f00e9a4/docs/evidence/2026-11-17-next-code-review-03.md`
5. Quality Gate 01：`https://github.com/2023violet/Hollow-Gimbal/blob/f00e9a4/docs/evidence/2026-11-17-next-quality-gate-01.md`
6. Motion Contract 01：`https://github.com/2023violet/Hollow-Gimbal/blob/f00e9a4/docs/evidence/2026-11-17-next-motion-contract-01.md`
7. Host Virtual 验收：`https://github.com/2023violet/Hollow-Gimbal/blob/f00e9a4/docs/evidence/2026-11-16-next-vm-01d.md`
8. `robot_mc` 固定版本：`https://github.com/cyberbeast0829/robot_mc/tree/2f1fb61ec081eb7a42ab8436ba8f1504fc529da7`
9. `robot_mc` 根 README 与许可证：`https://github.com/cyberbeast0829/robot_mc/blob/2f1fb61ec081eb7a42ab8436ba8f1504fc529da7/README.md`、`https://github.com/cyberbeast0829/robot_mc/blob/2f1fb61ec081eb7a42ab8436ba8f1504fc529da7/LICENSE`
10. Mini Cheetah 上游：`https://github.com/bgkatz/motorcontrol`；其他参考：`https://github.com/simplefoc/Arduino-FOC`、`https://github.com/mjbots/moteus`、`https://github.com/odriverobotics/ODrive`。

---

**结论：** FalconFOC 现在处于“可移植控制核心和 Host 软件集成基础已建立，向专业电机控制算法、性能与可用性成熟化迈进”的阶段。我们的标准不是功能列表比谁长，而是每项控制能力都经过正确的工程设计、可复现的验证和明确的适用边界；同时，让真正高质量的 FOC 更容易被第三方开发者使用。
