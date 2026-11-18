# NEXT-CODE-REVIEW-05｜Pipeline 控制事务与代码优化评审

2026-11-17。**Runtime P2：KEEP。Pipeline：软件控制事务完整，本轮范围内未发现 P0/P1。** 推荐下一轮只删除 `foc_pipeline_submit` 中 20 B 的旧 Mock 寄存器预拷贝。ARM `-Os` 隔离编译确认该函数减少 14 B 指令，静态帧仍为 48 B。最终 Safety、STOP 准入、输出确认和 PI 提交顺序均应保留。

## 1. 源码身份与范围

- 仓库 `2023violet/Hollow-Gimbal`，分支 `codex/falconfoc-v2-vm01a`。启动前核对了工作树、本地跟踪引用和 `git ls-remote`。三者基线均为 `69485957ff6d47de51e1027b8b3e3ee00b4d71bb`，目标工作树起始无修改及未跟踪文件，复用 `D:/study/Software/Hollow-Gimbal-vm01a`。
- 依据 [AGENTS.md](../../AGENTS.md)、[项目纲领](../architecture/FalconFOC_Project_Charter_and_Engineering_Memory_v1.0.md)、[REVIEW-04](2026-11-17-next-code-review-04.md) 和 [IMPROVE-04](2026-11-17-next-code-improve-04.md)。源码复核确认前轮 P1 已关闭：动作资格在样本处理前判定，过期邮箱保留独立结果，普通 H0 仍进入 Pipeline。当前相关回归已重跑。
- 交付为本报告和维护日志。生产代码、正式测试和配置零差异。矩阵探针与两个 A/B 候选均在临时目录编译。验证限 Host 与 ARM 静态数据。

## 2. Task A｜生命周期终态 KEEP

### 实际顺序

1. Foreground 校验请求形状、源时间、`source_session` 与序号，release 发布 PUBLISHED。到期 H0 用 CAS 领取为 CLAIMED，再读取请求副本。入口 STOP、调度失败或专用恢复可在动作执行前占先。
2. H0 复核会话及请求年龄。过期请求先在局部记为 `REJECTED`，其动作不会进入 control。错误状态 ARM 在 control 内先判资格，再执行本拍正常 Pipeline。此时终态槽尚未发布。
3. `BEFORE_SUBMIT` 位于最终 Safety 决策之后、UPDATE adapter 提交之前。该处来的 STOP 由最终 guard 拒绝 UPDATE；H0 排空 STOP，结果为 `CANCELLED`。这个分支表达本拍被安全事件截断，覆盖先前局部判定。
4. `AFTER_CONTROL` 在 `foc_pipeline_step_guarded` 返回之后、Runtime 执行本拍 ARM/START 生命周期转换之前。若本拍已接受 UPDATE，STOP 随即提交 INHIBIT，已接受 UPDATE 和 PI 历史保留。这个 UPDATE 是普通控制路径，对错误或已过期的 ARM 不产生资格。若当拍是有效 START，Pipeline 此时仍在 READY，尚未执行 START，STOP 可取消该转换。
5. H0 排空 STOP 后按恢复边界推进 `motion_session`，然后发布一个终态并释放邮箱。结果带原始 `request_sequence`、`action`、`requested_mode` 和处理后的 `applied_session`。`source_session` 属于请求，终态结构没有同名字段。

代码定位见 [领取与结果写入](../../runtime/src/foc_runtime.c#L422)、[最终 guard](../../runtime/src/foc_runtime.c#L342)、[控制与应用顺序](../../runtime/src/foc_runtime.c#L584)、[H0 终态发布](../../runtime/src/foc_runtime.c#L1140) 和 [公共结果定义](../../runtime/include/foc_runtime.h#L101)。

### 定序矩阵

临时探针复用 [正式 mailbox fixture](../../runtime/tests/test_foc_runtime_mailbox.c#L33)。RUNNING 组先积累 1 次接受 UPDATE，在 401 µs 服务下一拍。过期组发布时在租期内，H0 消费时年龄 1501 µs，超过 1500 µs。有效 START 组从 READY 在 200 µs 执行。STOP 都不带锁存故障。

| 请求和注入点 | H0 / 生命周期结果 | Δ UPDATE | 最终确认动作 / Safety | Motion Session |
| --- | --- | ---: | --- | --- |
| RUNNING 的 ARM，无 STOP | `ERR_CONTROL / REJECTED` | +1 | UPDATE / RUNNING | 0 → 0 |
| 消费时过期 ARM，无 STOP | `OK / REJECTED` | +1 | UPDATE / RUNNING | 0 → 0 |
| 错误状态 ARM，`BEFORE_SUBMIT` STOP | `ERR_STOP / CANCELLED` | +0 | INHIBIT / DISABLED | 0 → 1 |
| 过期 ARM，`BEFORE_SUBMIT` STOP | `ERR_STOP / CANCELLED` | +0 | INHIBIT / DISABLED | 0 → 1 |
| 错误状态 ARM，`AFTER_CONTROL` STOP | `ERR_CONTROL / REJECTED` | +1 | INHIBIT / DISABLED | 0 → 1 |
| 过期 ARM，`AFTER_CONTROL` STOP | `OK / REJECTED` | +1 | INHIBIT / DISABLED | 0 → 1 |
| READY 的有效 START，无 STOP | `IDLE / APPLIED` | +0 | INHIBIT / RUNNING | 0 → 0 |
| READY 的有效 START，`AFTER_CONTROL` STOP | `ERR_STOP / CANCELLED` | +0 | INHIBIT / DISABLED | 0 → 1 |

8/8 断言通过。各行请求序号为 41，动作及请求模式一致，源会话在邮箱槽中保持 0，结果会话与 H0 终态相符。Hook 内结果槽仍未切换，完成后仅切换一次。重复发布原请求被拒绝，下一拍无请求的 H0 服务未再次发布终态。所有行输出确认均为真，STOP 行锁存位为 0。

**裁决依据。** `REJECTED` 表示请求自身未获准，`CANCELLED` 表示 STOP 或故障阻断当拍准入或尚可发生的转换。两个后置 STOP 反例中的 ARM 在 STOP 到达前已经没有资格。后置 STOP 改变当前输出和会话，保留该 ARM 的拒绝归因。有效 START 对照证明同一个 Hook 能取消仍待应用的请求。保持当前枚举与实现，报告记录这个区分。

## 3. Task B｜Pipeline 控制事务

`Runtime 源观测与 ADC 时间差 → Pipeline 水印及恢复边界 → Device ADC 解码 → Safety 样本与初检 → Current Trial → SVPWM → Device 三相 compare → Safety 最终许可 → STOP guard → Device 试提交与 adapter 应答 → 正式 PI 提交`。

- **输入。** 样本序号与源时刻按模 2³² 半区间排序。新鲜但无效的 ADC/角度观测先消耗 Pipeline 水印，再递增 Safety 的 invalid streak，立即尝试 INHIBIT。重复、未来、过期和越恢复边界的拒绝与 streak 语义有区别。样本角度必须对应 ADC 源时刻；Runtime 负责 H1 对齐。命令有独立水印和租期，派生电流目标必须绑定当前命令序号与源时间，不能续租。启用前馈时，电角速度来自同一已验证 H1 快照。
- **Trial 与提交。** `current` 的 52 B 副本容纳候选 d/q 积分器。Current 局部结果成功后才映射三相 compare，最终 Safety 和 guard 通过后才进入输出提交。`foc_device_sim_submit` 验证请求并在局部生成寄存器候选，再由同步 adapter 确认；两者成功后才提交寄存器快照、UPDATE 计数、正式 PI 和测量。试算失败、映射失败、最终 gate 拒绝、guard 拒绝或 adapter 拒绝均不提交候选 PI。命令和样本水印、Safety 观测及诊断历史可以前进，它们的职责是防重放和报告失效。
- **STOP 与 FAULT。** 入口锁存故障短路试算，最终 gate 仍能看到延时、STOP 和故障注入。Runtime guard 是独立 STOP 的软件准入点。晚于准入的 STOP 由 H0 排空并应用 INHIBIT，避免把历史 UPDATE 误当作当前有效输出。显式 STOP 即使携带错误时间也会尝试 INHIBIT。若 adapter 拒绝 INHIBIT，保留历史寄存器供诊断，同时将 `output_confirmed` 清零并返回 `ERR_INHIBIT`。`last_reason` 保留触发原因。
- **恢复。** ARM 需要 confirmed INHIBIT 和恢复边界之后的新鲜命令、样本；START 再检查可用性与心跳并清零积分器，下一新拍才可 UPDATE。故障清除要求确认禁止输出，由 Safety 校验锁存类别，再建立新边界。INIT 用完整局部对象校验及请求初始 INHIBIT 后才覆盖调用者对象。初始化和停止的确认范围由 adapter 声明。
- **时间与物理范围。** Pipeline 用调用者传入时间和 `before_gate.delay_us`，不自行取时。Board/Runtime 普通调用的该延时为零，因此存在两个逻辑检查点；实际计算到输出生效的时差、守卫至适配器之间的物理事件及外设关断延时均需要目标证据。Board1 adapter 拒绝 UPDATE，只确认软件 INHIBIT。真实 Port 仍需证明完整三相提交、STOP/FAULT 仲裁和失败后的硬件状态。

主要实现见 [Pipeline](../../runtime/src/foc_pipeline.c#L350)、[Current](../../core/src/foc_current.c#L223)、[Device 映射](../../g3/np/src/foc_device.c#L203)、[Safety 输出门](../../g3/safe/src/foc_safe.c#L419) 和 [锁定 Board 输出](../../firmware/at32f415/src/foc_board.c#L370)。

### 主文件逐函数裁决

| `foc_pipeline.c` 定义行 | 函数 | 裁决与契约 |
| ---: | --- | --- |
| 23 | `foc_pipeline_accept_time` | KEEP。记录非回退执行时间，接受相同时间；时间有效不意味着请求或样本有效。 |
| 33 | `foc_pipeline_after_recovery` | KEEP。要求生产时刻严格晚于恢复边界。 |
| 42 | `foc_pipeline_input_newer` | KEEP。同时检查序号和生产时间顺序，保留跨回绕语义。 |
| 54 | `foc_pipeline_submit` | **IMPROVE**。本地验证、adapter 确认和 journal 提交顺序正确；删除成功分支不会使用的 20 B 旧寄存器预拷贝。 |
| 85 | `foc_pipeline_require_rearm` | KEEP。建立恢复边界，撤销 READY/RUNNING，保留已锁存故障。 |
| 95 | `foc_pipeline_emit_inhibit` | KEEP。产生规范 INHIBIT，清反馈，区分触发原因与确认失败，即使 PWM 配置错误仍构建禁止动作。 |
| 110 | `foc_pipeline_stop` | KEEP。活跃会话新建恢复边界，再统一提交 INHIBIT。 |
| 119 | `foc_pipeline_output_inhibited` | KEEP。确认位和动作必须同时成立。 |
| 125 | `foc_pipeline_ready_inputs` | KEEP。ARM/START 的独立新鲜度、有效性和恢复准入。 |
| 145 | `foc_pipeline_observe_sample` | KEEP。水印挡住重放，Device/Safety 处理新观测与无效 streak。 |
| 174 | `foc_pipeline_feedforward_feedback_valid` | KEEP。对启用策略检查源年龄与有限电角状态。`source_seq` 是诊断身份，Pipeline 接受的为调用者保证的相干快照。 |
| 191 | `foc_pipeline_init` | KEEP。完整验证配置和初始 INHIBIT 应答后发布实例；冷路径大帧单列。 |
| 227 | `foc_pipeline_set_command` | KEEP。拒绝无效命令而不替换已接受命令或续租。 |
| 259 | `foc_pipeline_arm` | KEEP。只从 DISABLED 进入 READY，且需要当前可确认的禁止输出。 |
| 277 | `foc_pipeline_start` | KEEP。READY 到 RUNNING 之前复核健康并事务式复位 PI。 |
| 302 | `foc_pipeline_request_inhibit` | KEEP。时间出错不阻断显式 STOP，锁存位按请求处理。 |
| 325 | `foc_pipeline_clear_fault` | KEEP。确认 INHIBIT 后清可恢复故障，建立新鲜输入边界。 |
| 345 | `foc_pipeline_step` | KEEP。无 Runtime 依赖的串行入口。 |
| 350 | `foc_pipeline_step_guarded` | KEEP 主事务。最终 gate 内的同时间健康重复检查列为候选；提交点和 STOP guard 位置保留。 |

## 4. 执行效率与资源

### 路径成本

| 路径 | 主要工作与已见成本 |
| --- | --- |
| 正常 UPDATE | 每拍 Device ADC 配置复核、掩码及相序换算，样本/Safety 时间检查，52 B Current Trial，角度缓存、Clarke/Park、PI、一次调制与实际 duty 反算，三相 compare、最终健康/动态请求验证、输出 journal。ADC 配置包含 6 个浮点有限性检查；PWM 配置、周期及相序检查较小。反汇编已见三相共用一次 period 整数转浮点，无需在源码添加另一份缓存。 |
| 饱和 | 按向量限幅分支进入 `sqrtf`，按实际 duty 反算电压，抗积分饱和修改候选时可再调制一次。调制含 `hypotf`，第二次调制复用同一次角度缓存。 |
| 输入早拒绝 / 锁存故障 | 不进入 Current；必要时仍进行 `supervisor_tick` 和 INHIBIT，保留锁存与拒绝诊断。 |
| 最终门/STOP 拒绝 | 已付出 Current 与映射成本，丢弃 Trial，仅提交 INHIBIT。入口 STOP 比末端 STOP 更早截断。 |
| 输出拒绝 | 尝试一次 UPDATE 后再尝试 INHIBIT；包含 adapter 的两次独立应答，失败时一致地撤销输出确认。 |

**最主要的预期计算成本是 Current/SVPWM 的 Soft-float 浮点链。** Arm GNU 14.3.1、`-Os -mcpu=cortex-m4 -mthumb -mfloat-abi=soft` 的实际镜像反汇编显示 Current 角缓存调用 `fmodf / sinf / cosf`，调制含 `hypotf`，向量和 SVPWM 路径包含 `sqrtf`，大量 Float32 运算和比较落为 `__aeabi_f*` 调用。实际走哪条 libm 分支取决于输入，快速路径与饱和最坏路径的占比尚无目标周期数据。Host Clang 22.1.8 和 MinGW GCC 16.2.0 各通过同一 Runtime 套件，CTest 耗时包含进程与仿真，未作为可比每拍周期数据。保持现有角缓存、反算、限幅和抗饱和。

普通健康 UPDATE 在时刻 T 运行一次 Supervisor，最终 gate 在其决策时刻调用 Supervisor，再直接重复调用一次 `foc_safe_current_health_faults`。最后两次在同一个时刻、同一个执行域，对成功 RUNNING 路径读取相同健康输入。前后两个逻辑检查点仍有独立价值，尤其是延迟、晚到 STOP 或 FAULT 的路径。分层 `isfinite`、占空比与 compare 检查面对的对象不同，现有 API 和错误原子性要求保留它们。

### 对象、静态帧与整镜像

| 对象 `sizeof` | Host64 B | ARM B |
| --- | ---: | ---: |
| Pipeline / Runtime | 384 / 1296 | 372 / 1236 |
| Current context trial / Mock journal | 52 / 132 | 52 / 132 |
| Mock registers / PWM request / Safety decision | 20 / 32 / 40 | 20 / 32 / 40 |
| Measurement / Current input / Current output / `foc_output` | 24 / 32 / 44 / 36 | 24 / 32 / 44 / 36 |
| Pipeline input | 88 | 76 |

ARM 基线 `.su` 自身帧：`foc_pipeline_step_guarded` 312 B，`submit` 48 B，`emit_inhibit` 48 B，`start` 80 B，`init` 408 B（启动冷路径）；Device decode/map 48/56 B，Safety gate 32 B，Current 内部 step/modulate 184/56 B。`observe_sample` 等被内联，无独立帧。局部对象的尺寸不能直接相加计算函数帧。Pipeline 使用调用者静态/自动存储，本控制链源码没有动态分配。

按反汇编中实际 `bl` 嵌套，举例计算 `ADC1_IRQHandler → Board ADC complete → H0 → Runtime control → Pipeline → Current step wrapper → Current internal → modulate → SVPWM` 的已知静态 C 帧和为 **1,096 B**。其中 Pipeline 312 B、Runtime 两帧 168 + 224 B。此和未包含更深 libm、硬件异常栈帧及前台/H1 被抢占时已有的栈，不能作为整机上界。厂商 linker 的 1 KiB 保留项与额外 `stack-budget.ld` 的 4 KiB 静态间距断言是两种链接口径，二者都不测量栈高水位。

新鲜锁定参考固件为 Flash **38,332 B**、RAM **3,768 B**。`size` 为 `text=38240 data=92 bss=3680`，RAM 采用链接器区域统计，包含保留栈空间。BIN SHA-256 为 `3A0260BC7519E3E830FE1E5510ABE2585D62DBCBC945C97C02F72FA615BCA3DC`。ARM 配置直接使用目标自带的 `-fstack-usage`；最初通过 `CMAKE_C_FLAGS` 覆盖该变量导致 CPU 架构标志丢失并触发 UART 锁自由静态断言，废弃该构建，重新使用原构建选项通过。汇编、`.su`、链接数据均来自纠正后的配置。

## 5. 可移植性与固定参考对照

- **Portable。** Pipeline 公共 API 由调用者持有实例，显式传递 ADC、时间和 adapter，无 MCU、RTOS、UART 或 Runtime 链接依赖。独立实例测试通过。Core 可单独集成，H0/H1 是参考 Runtime 的执行假设。Pipeline 仍暴露 `foc_device_sim.h` 与 Mock journal，Device 目前固定相 A→CH3，支持 ABC/ACB，异构采样和 PWM 布线需要 Port/Device 适配；本轮没有第二 MCU 验证。
- **Deterministic。** 单实例串行 owner、半区间时间、水印、同步 guard 与同步有界 adapter 定义了可重放的软件顺序。adapter 必须遵守借用请求、禁止阻塞和重入的契约，提交确认属于适配器声明范围。目标执行时间和外设响应有独立验收。
- **High-Performance。** 已有单拍角缓存、有限分支、可选择功能和静态资源基线。Soft-float 最坏路径、实际抖动及电机闭环品质仍待各自证据。Simulation CTest 通过维持现有软件合同，本轮没有修订控制品质判据。
- **`robot_mc` 对照。** 只读获取并核对了 `2f1fb61ec081eb7a42ab8436ba8f1504fc529da7`。N32H473CE/M4F 的 `ATIM1_UP_IRQHandler` 先启动编码器 SPI DMA，再做 ADC 取样和故障检查，随后收帧、PLL、FSM 控制与通讯，分段 timing 宏提供直接测量位置。`analog_sample` 对三路注入 ADC 仍含完成等待，`set_dtc` 按相序直写三相比较，变换使用查表 sin/cos。适合借鉴的是外设传输与独立工作重叠、以及按阶段测最坏路径。其 40 kHz 与全局 `controller`、硬件浮点、板级 ADC/编码器拓扑都属于该实现。核对根 MIT 及版权声明，未引入任何外部代码；后续如需引入具体实现，按实际文件核对来源和许可。 [固定 README](https://github.com/cyberbeast0829/robot_mc/blob/2f1fb61ec081eb7a42ab8436ba8f1504fc529da7/README.md) · [ISR](https://github.com/cyberbeast0829/robot_mc/blob/2f1fb61ec081eb7a42ab8436ba8f1504fc529da7/Core/Src/n32h47x_48x_it.c#L229) · [FOC 实现](https://github.com/cyberbeast0829/robot_mc/blob/2f1fb61ec081eb7a42ab8436ba8f1504fc529da7/Core/Src/foc.c#L23) · [MIT 许可](https://github.com/cyberbeast0829/robot_mc/blob/2f1fb61ec081eb7a42ab8436ba8f1504fc529da7/LICENSE)

## 6. 生产改进候选

### 1. IMPROVE｜删除 Mock 候选的旧寄存器预拷贝

- **位置与依据。** [Pipeline submit](../../runtime/src/foc_pipeline.c#L54) 目前从旧寄存器复制 20 B 至 `candidate`。在所有返回 OK 的分支，[Device sim submit](../../g3/np/src/foc_device_sim.c#L160) 都完整写入 gate、enable 与三个 compare 字段；失败和强制拒绝分支不读取候选值。基线 ARM 反汇编实际保留了旧寄存器的读写指令。
- **最小改法。** 只去掉 `candidate = mock->registers` 里的初始化，保留局部候选和所有短路顺序。影响所有 Pipeline 调用者，包括直接集成、Runtime、Simulation 和 Board1。`candidate` 只能在本地和 adapter 都接受后用于写回；保留故障回退、`output_confirmed`、历史 UPDATE 和 PI 的提交时点。
- **证据与验收。** 隔离 A/B 中 `foc_pipeline_submit` **204 → 190 B**，Pipeline 对象文件 `text` **2268 → 2254 B**，该函数静态帧 **48 → 48 B**，另行链接的现有 Pipeline 和 Runtime 主测试通过。正式变更时复跑 Device Mock、Pipeline/Runtime、Simulation 和锁定 Board Host，覆盖成功、两类拒绝及确认状态，重建 ARM 核查符号、栈和整镜像。ARM 静态证据足够决策这项局部删冗，目标周期收益仍待测。值得下一轮先做，回退仅恢复这一初始化。

### 2. NEEDS EVIDENCE｜收敛最终 Safety gate 内同一时刻的重复健康检查

- **位置与最小方向。** [Safety gate](../../g3/safe/src/foc_safe.c#L419) 成功 RUNNING 分支可复用刚完成的 `supervisor_tick` 结论，保留 DISABLED/READY 的心跳原因位和早退故障分支。隔离候选同时把相同拒绝分支内的第二次请求有效性检查合为一次。变化涉及所有 `foc_safe_gate_output` 调用者，保留最终 gate 本身、有效样本、动态请求、锁存及 guard 顺序。
- **收益与取舍。** 隔离候选通过已有 Pipeline、Runtime、SAFE 合约测试。ARM 对象 `text` 1288 → 1266 B，编译器内联原 180 B 请求校验助手，gate 自身符号 190 → 348 B，静态帧 **32 → 40 B**。有可证明的重复调用，当前还未确定运行周期收益与栈取舍，暂不和候选 1 混合落地。进一步验证应比较五种 Safety 状态、双心跳/超时、畸形请求、时间回绕与晚 STOP 的状态和原因位等价，再用同条件 ARM 数据核对代价。

52 B Current Trial、每拍 Device 配置校验和 Mock journal 继续 KEEP。缩窄 Trial 需要在 Core 与 Pipeline 间重新定义候选状态，跳过配置校验会改变跨层责任，裁剪 journal 会波及 Runtime 确认/诊断读者。当前证据不足以支持这些改动。**下一次生产补丁只做候选 1**，保持依赖和接口范围。

## 7. 本轮验证和停止点

| 验证 | 结果 |
| --- | --- |
| Runtime Debug，Clang 22.1.8 / GCC 16.2.0 | 各 11/11，含 mailbox、Pipeline 拒绝、STOP、重放与故障 |
| Simulation Debug，Clang 22.1.8 | 15/15，含以上 Runtime 用例及 4 个 Virtual Motor / Sensor / Current / Periodic Chain 用例 |
| 复用 mailbox fixture 的临时交错探针 | 8/8，覆盖资格、准入前后 STOP、身份、会话和发布一次 |
| Arm GNU 14.3.1 锁定参考固件 | 正常选项构建通过，验证 Soft-float 汇编、`.su`、`sizeof` 和链接资源 |
| 临时 A/B | submit 候选 2 个既有套件通过；gate 候选 3 个既有套件通过。候选未合入 |

可通过 `cmake -S runtime -B <runtime-build> -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=clang`（`simulation` 同理），再 `cmake --build <build>`、`ctest --test-dir <build> --output-on-failure` 复跑基线。ARM 使用 [参考固件构建入口](../../firmware/at32f415/README.md#build)，用 `compile_commands.json` 确认编译选项，`arm-none-eabi-objdump -d` 与 `.su` 对照调用和自身帧。A/B 按候选描述在临时源副本上独立改一处，用相同 ARM flags 编译并对比 `size` 与 `nm -S`；Host 将该对象优先链接到原有 Runtime 静态库和现有测试入口。

结论限本次源码与软件验证。Git 无提交或推送，本轮止于评审。

```text
G3-SAFE HARDWARE NOT VERIFIED
G3-PWR LOCKED
AT32F415 WCET NOT VERIFIED
MOTOR PHYSICAL OPERATION UNVERIFIED
```
