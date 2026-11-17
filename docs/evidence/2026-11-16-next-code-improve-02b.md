# FalconFOC V2｜NEXT-CODE-IMPROVE-02B

日期：2026-11-16。结论：**NEXT-CODE-IMPROVE-02B PASS**，限本次获批的 motion-enabled 周期相位链整改与 Host 软件验收。

P2-02 的正常生产链已从累计 Float32 机械位置派生电相位，迁移到同帧整数周期相位。连续 Position/Trajectory 保留多圈语义。原直接角度模式、旧 Rotor API、Host 首帧准入和 Board1 输出锁保留。此结论不是所有控制品质达标、任意输入精度保证、MCU 实时性或硬件安全认证。

## 1. 授权与真实输入身份

- 用户批准文件：`C:/Users/16429/.codex/attachments/403acb2b-a33a-4367-9030-e3df3ddce292/已粘贴的文本.txt`。
- 实施与前阶段 `PRODUCTION_CHANGE_PROPOSAL.md` 第 8 节一致：Sensor uint32 字段、新 Core helper、Runtime bool 字段、Board/Virtual 与测试生产者迁移；没有扩大到 Safety、输出权限、控制参数或 UART V1。
- 工作树：`D:/study/Software/Hollow-Gimbal-vm01a`；分支 `codex/falconfoc-v2-vm01a`；HEAD `a198f6d4a6c61f1bb9dd5968a46e7f38ac49a3f0`。
- 原有 VM/Quality/Tuning/Release Prep/IM01/IM02 未提交成果是本轮输入，未用远端 RC0 覆盖。
- 新执行根：`D:/study/Software/FalconFOC-code-improve-02b-20261116`。起始保护清单含 2512 个项目文件、2427 个 ignored 文件（322054220 B）。Git 状态、diff、HEAD、主工作树状态和源文件快照已保存。

完整命令与输出在各目录的 `commands.jsonl`、`*.log`。最终差异、保护核对与回滚检查分别为 `final-stage.patch`、`change-verification-final.json`、`rollback-check-final.log`；这些都是本轮新文件。

## 2. 旧链路与新链路

```text
旧：Raw Encoder → int64 continuous counts → Float32 continuous position
    → H1 rotor mapping → H0 prediction → Current → SVPWM → adapter

新：Raw Encoder → 同一 observation 的 directed integer cycle count
    → Core (cycle_count * pole_pairs) % CPR → bounded Float32 + offset
    → H1 theta_elec_rad + electrical_phase_valid
    → 原 H1/H0 mailbox → 原 prediction → 原 Current/SVPWM → 原 adapter

并行：int64 continuous counts → continuous mechanical_rad / speed
    → 原 Position / Trajectory / mechanical telemetry
```

H1 不再从 motion-enabled `mechanical_rad` 恢复周期相位，缺少新标志时不会回退旧路径。方向由 Sensor 应用一次；极对数和 offset 由 Core 使用该 Runtime 实例的不可变配置应用一次。

## 3. 实际生产修改

| 文件 | 本轮必要修改 |
| --- | --- |
| `sensors/include/foc_encoder.h` | Feedback 尾部新增 `uint32_t mechanical_phase_count`；注明 CPR、同帧、首帧和 invalid 契约 |
| `sensors/src/foc_encoder.c` | 首个 accepted observation 与完整有效 observation 填该次 directed code；reject 的基底清零，不复用旧相位 |
| `core/include/foc_motion.h`、`core/src/foc_motion.c` | 新增 `foc_rotor_phase_from_counts(config,count,CPR,&phase)`；保留旧 Rotor API |
| `runtime/include/foc_runtime.h` | Sensor 尾部新增 `bool electrical_phase_valid`；明确有界相位与连续位置独立 |
| `runtime/src/foc_runtime.c` | 仅 H1 motion-enabled 验证/装配改用显式相位；保留时序、双槽、H0、会话和生命周期机制 |
| `firmware/at32f415/src/foc_board.c` | 同帧 Feedback、Sensor 实例 CPR、Runtime rotor config 生成相位；不改真实输出函数 |
| `simulation/src/foc_virtual_current.c` | motion-enabled 使用同帧整数来源和 controller config；motion-disabled 保留原直接角度生产者 |

其余差异是必要的测试生产者迁移、两个新增正式测试、Core/Simulation CMake 登记、API/README、阶段计划、本报告及维护日志。逐文件清单与实际增删行见外部 `change-verification-final.json` 与 `final-stage.patch`，比较基准是阶段起始快照，而非包含此前大量未提交工作的 Git HEAD。

## 4. 数据与错误契约

| 量 | 来源 / 所有权 | 有效性与消费者 |
| --- | --- | --- |
| `position_counts` | Sensor int64，方向已应用，现有溢出防护保留 | 连续位置整数依据 |
| `mechanical_rad`、速度 | 同一 Sensor observation | Position/Trajectory/遥测；Float32 绝对分辨率仍随幅值下降 |
| `mechanical_phase_count` | 同一 directed raw code；`0 <= count < Sensor.config.CPR` | 等于累计计数的数学非负模；不是从 float 反推 |
| `theta_elec_rad` | 对同帧 count/CPR 使用 Runtime rotor config 的 Core 结果 | motion-enabled 必须有界、有限，且两个有效标志都成立 |
| `electrical_phase_valid` | Producer 对同源相位的显式准入 | 默认 false；不是数值零代表有效，也不替代 Sensor 状态 |

Helper 的 CPR 范围为 2～65536，pole pairs 为非零 uint16，count 小于 CPR。最大乘积 `65535*65535=4294836225 < UINT32_MAX`，先乘模再转 float；没有 `abs(INT64_MIN)` 或依赖负数 C 余数自动为正的运算。CPR 由 Sensor 实例拥有，无通用 Core 的 Board 常量。

Helper 清错误输出；NULL 为 −1，无效 CPR/count/poles 为 −5，非有限 offset 为 −2。有限巨大 offset 仍接受，但受原 normalize 精度域限制；本轮矩阵 offset 在 ±Float32 2π 内，不能把矩阵预算推广到任意 finite offset。

H1 保留时间、序列、原子发布规则；motion-enabled 另要求两个 valid、有限位置/速度/电速度及 θ∈[0,Float32 2π)。invalid candidate 进入既有 H0 INHIBIT 路径；无效相位不会提交 UPDATE。Motion-disabled 保留有限直接角度与精确 ADC 时间对齐，不要求新 flag。结构体 ABI 改变，所有二进制消费者须重编译。

## 5. Producer 完整盘点

- **Board**：Feedback captured_us 与 completion capture 相同；只用既有完整 valid Feedback。CPR 取 `encoder.sensor.config`，poles/offset 取 `runtime.config.motion.rotor`。首个 UNINITIALIZED 不可发布为有效周期相位；overlap、transport、timeout、continuity 错误保持 invalid/inhibit。
- **Virtual motion-enabled**：同一 Encoder update 的 count、position、speed、captured_us；CPR 来自该 Encoder，poles/offset 来自 controller Runtime。测试故意令 controller 与 Plant 参数不同，验证没有偷用 Plant 真值。
- **Virtual motion-disabled**：保留原 Plant 参数形成的直接角度及其原精度限制。这是明确的兼容分支，不宣称具有新 motion-enabled 长多圈保证。
- **Motion Runtime/System/Control01B 测试**：静止零位或原始解析坐标生成相位；原始 double 时间/位置先归约再转 float，不伪造整数码。
- **Motion Model**：理想 Plant 的 double position 在 Float32 连续位置量化之前形成独立周期相位；直接与 Runtime 对照路径共用该坐标约定。
- **Board calibration fixture**：使用真实 Feedback count/CPR。原 fixture 的 1000 µs Feedback 被标记为 0 µs；本轮改为从 UINT32_MAX−999 到 0 的同样 1000 µs 间隔，使 H1 身份正确并覆盖回绕。
- **Serial 测试 fixture**：显式声明解析静止零码来源，用实际 Runtime rotor config；不改变 Serial 生产权限。
- **Runtime 原 exact-alignment、Remote、Long、Mailbox fixtures**：motion-disabled，保留原直接角度输入，不机械设置新 flag。

原始搜索清单保存于 `consumer-inventory.txt`。没有发现必须扩大授权才能迁移的额外生产者。

## 6. 独立 Oracle 与重新复现

正式新增 `core/tests/test_foc_periodic_phase.c` 复用先前独立参考思路：对 int64 累计计数先做明确数学模，再以 double `cycle/CPR * 2π * poles + offset` 的独立 sin/cos phasor 比较圆周距离；不用生产 normalize 或生产整数电周期乘模生成预期。

7 个 CPR × 5 个 pole pairs × 10 个正负/端点计数 × 6 个 offset，共 2100 例，直接调用新的生产 helper：

```text
max_abs_periodic_phase_error_rad = 7.65757044441e-7
max_error_case = counts:9223372036854775807, CPR:1000,
                 poles:1, offset:-0.709999979
sample_count = 2100
oracle_source = int64 mathematical modulo + independent double sin/cos phasor
budget = 1e-5 rad (Host numerical acceptance only)
```

Sensor 正式测试另覆盖 24000 次正反方向、跨圈、反向行程、时间回绕更新，以及首帧、duplicate、transport failure、reset 的字段/状态约束。INT64_MAX/MIN 溢出边界用明确标记的内部状态单元测试验证：拒绝更新、保留原计数/前帧、invalid 且整数相位清零；这不冒充走到该极限的真实连续序列。既有 Sensor Reference 保留。

旧 Encoder→H1→H0/Current 反例在本轮先运行并退出 1：约 −16383 圈为 +0.0143116418176 rad，约 −262143 圈为 −0.149750858182 rad。该 RED 与新 API/字段缺失的测试日志均保留。

## 7. 正式新链端到端

新增 `simulation/tests/test_foc_periodic_chain.c` 使用合法公开 Encoder 更新：两个方向各 16777216 次，包含微秒回绕。六个同码里程点、两个 offset/电流方向共 24 个快照，实际进入 Core helper→H1→H0→Current PI→SVPWM→生产 Virtual Output Adapter。H1 输入、capture/seq/raw、H0 从 measured dq 反解的使用相位、Duty 和 accepted UPDATE 全部输出到 `behaviors-closeout/phase-trace.log`。固定 ADC 电流为平衡 1/−0.5/−0.5 A；最终夹具 VBUS=12 V 与该 Virtual profile/adapter 一致。

| 指标 | 实测 |
| --- | ---: |
| 24 组 H1 最大圆周相位误差 | 3.28188485858e−7 rad |
| 24 组 H0 measured dq 最大向量误差 | 3.30374200523e−7 A（1 A 平衡输入） |
| 约 −16383 与 −262143 圈、同码 raw=123、零 offset | 均约 3.28188486e−7 rad，不随累计圈数增长 |
| Position 相同连续值与源时间戳 | 保留 |
| 正/负电流参考与接受 UPDATE | 保留；真实 Virtual adapter 确认 |

Sensor 在长序列中连续更新；Runtime 在各里程点建立新的正常 ARM/START 会话。此测试不是连续数小时的多域闭环运行，也不是实机证据。

额外完整 Virtual Trajectory 控制到 ±12.566371 rad，各运行 6 s：测得 −12.5809441 / +12.5811357 rad，Plant 与反馈差小于 0.01 rad，均 RUNNING、无 fault、29996 个 UPDATE。保持正负多圈目标，未把位置截断成单圈。

## 8. 首帧、无效输入和生命周期

- 正常 Host 顺序：0/200/400 µs 无 UPDATE，600 µs 第一个 UPDATE；500 µs Encoder 第二帧已建立速度。
- 提前排队 ARM/START：400 µs 第一个 UPDATE，Encoder 速度尚未建立，沿用既有零速假设；600 µs 第二个 UPDATE。
- 两者均不伪造 Encoder speed_valid；此 Host-only 边界不传播到 Board。
- 启动边界 STOP 或显式 FAULT 在 400 µs 前置：均无 UPDATE，分别 DISABLED / FAULT_LATCHED。
- 首次 transport failure 后提前 ARM/START：起始/迁移后的独立执行输出逐字节相同，始终 DISABLED、零 UPDATE；1000 µs 速度恢复也不会自动启动。
- 缺失 phase flag、source invalid 配有限旧相位、NaN/负/上界 θ、非有限位置/速度、电速度溢出均拒绝，UPDATE 不增加、输出 INHIBIT。
- 旧 Session、STOP 优先级、FAULT 锁存、超时、模式切换、请求身份和 UPDATE 拒绝后的 PI/Trajectory 不提交，由迁移后的 Runtime/Serial/Virtual 既有行为用例继续覆盖。

新增首帧传输测试初稿误以为 400 µs 必然 fault；源码与起始/新实现对比表明，被拒绝的 START 在样本观察之前返回，invalid streak 为 1/1/2，随后有效帧重置。已据实纠正新测试契约为保持 DISABLED/INHIBIT，不改 Safety 或阈值；失败日志与 `boot-failure-comparison/` 保留。

## 9. 六类正式生产变异

最终源码副本的正常基线通过；六个变异全部编译成功，再由相应行为断言退出 1。具体替换和输出在 `mutations-final/<case>/mutations.json`、`behavior.log`，汇总 `mutations-final/results.json`。

| 变异 | 检出 |
| --- | --- |
| H1 故意用累计 Float32 位置覆盖新相位 | 长多圈 phase/dq 精度断言失败 |
| Sensor 初始方向及 delta 方向反转 | 真实 Sensor directed position/phase 行为断言失败 |
| Core 忽略 pole pairs | 独立 phase Oracle 失败 |
| Core 忽略 offset | 独立 phase Oracle 失败 |
| Virtual 将当前相位标为旧 500 µs capture | 同帧捕获身份断言失败 |
| H1 忽略 source invalid，继续消费旧有限相位 | H1/H0 错误成功，UPDATE 1→2，被 INHIBIT/不增 UPDATE 断言检出 |

没有将编译失败、超时或崩溃当作变异检出。这次变异作用于正式生产源副本，区别于 IM02 前阶段的 Host-only staging 原型。

## 10. 完整回归与 CLI

| 集合 | 本轮实际结果 |
| --- | --- |
| Core（Math Reference、Current、Motion、Trajectory 等） | 15/15 |
| Sensors / Sensor Reference | 2/2 |
| Runtime | 11/11；最终 Simulation 集合再次包含全部 11 项 |
| Simulation | 15/15 |
| Clang ASan/UBSan | Core 15/15、Sensors 2/2、Simulation（含 Runtime）15/15、Board 4/4；无 sanitizer 报告 |
| Board Host | 4/4，含更新后的 Board 首帧/相位/锁定检查 |
| Python/C CLI | 11/11，无 skip |
| Legacy Host | 45/45，新 GCC cache |
| Source Contract | 81 C/H、9 CMake、9 README |
| Formatter、CMake Quality/Format、Scope falconfoc、Scope all | 全部退出 0 |
| ARM 固件 | 新目录完整编译链接成功 |

完整 Current C 场景包含正反电流、UPDATE 拒绝、STOP、回放；Motion C 场景包含速度/位置/负载/多圈/反向/模式/故障。Python Current、Velocity、Position、Load、Fault 五种工作流均 `INTEGRATION_PASS`，各有新 CSV/JSON。

**控制品质没有被重新定义为全 PASS。** 同参数起始源码与新实现的 CLI 对比：Current 短窗口仍 `CONTROL_QUALITY_FAIL`；Velocity 为 `CONTROL_QUALITY_PASS`；Position/Load 为 `EXPECTED_DEGRADED`；Fault 为 `NOT_MEASURED`。标签完全一致，除 Position 的微小数值变化外各列测量相同；Position running-tail RMSE 0.198718117→0.198706419，final error 0.10129814→0.10130303 rad。没有放宽阈值或整定参数。

Fault 场景于 5000 µs 发现 Sensor/H1/H0 失效并确认软件 INHIBIT，5400 µs fault latch，最后 UPDATE 为 4800 µs；INHIBIT 后 UPDATE 为 0。均为 Host 事件时间，不是物理关断延迟。

门禁在新本地 clone 运行，叠加本轮工作树；所需 Legacy ELF/MAP 为哈希验证后的历史拷贝，不冒称新 Legacy ARM 构建。短专用 TEMP 及 Bash PATH 解决环境约束，未改门禁。最初 Formatter 在新增 Board 测试后的换行格式上失败，格式化该受改文件后通过；历史失败输出保留。

## 11. ARM BIN、Flash 和 RAM

GNU 14.3.1，MinSizeRel、cortex-m4、soft-float，与原目标一致。另在新目录完整重建阶段起始源码，确实再次得到原 37864 B 和原 SHA，排除了历史源漂移。

| 项目 | 起始 | 新生产实现 | 增量 |
| --- | ---: | ---: | ---: |
| BIN / Flash | 37864 B | 38304 B | +440 B |
| `.text` 原始 section | 36044 B | 36484 B | +440 B |
| `.rodata` | 1352 B | 1352 B | 0 |
| `.data` | 84 B | 84 B | 0 |
| `.bss` | 2636 B | 2656 B | +20 B |
| GNU size 汇总 bss（包含 heap/stack reserve） | 3664 B | 3680 B | +16 B |

`.bss` 实际增加 20 B；`._user_heap_stack` 对齐填充从 1028→1024 B，所以 GNU 汇总和链接 RAM used 增量为 16 B，不混淆两个口径。链接 RAM used 为 3752→3768 B。

```text
旧 SHA-256: BDEC4D2706049080FF758EE308ADDC1304294DFD5805F05A0CAE7D56D4191806
新 SHA-256: 73EE73C4FB1570417DF7842520CCA8BA07204C86FE644D837F00DBDDB7EA9E89
```

符号变化解释：new helper +124 B；Board encoder complete +44；Encoder update +4；H1 +280；受新结构布局影响而源逻辑未改的 runtime_control/H0 各 −4，函数大小净 +444；`.text` fill 从 26→22 B，得到最终 +440。未改 `.rodata` 或 `.data`。完整 symbols/disassembly/map 在 `arm/`，对比在 `resources/`。

## 12. 结构体和静态栈

| ARM sizeof | 起始 | 新值 |
| --- | ---: | ---: |
| Runtime Sensor | 24 | 28 |
| Encoder Feedback | 48 | 52 |
| Board Encoder | 184 | 192 |
| Runtime | 1224 | 1236 |

最终生产头文件编译得到这些大小；ELF 各有一个 Runtime、一个 Board Encoder 静态实例，实际合计 +20 B，符合前阶段估计。

`.su` 函数自身静态 frame：Board encoder complete 56→64 B；H1 288→328 B；H0 168 B 不变；新 helper 32 B；旧 rotor/predict 各 40 B。新整数模生成 UDIV/MLS，无新增 int64 除法。新 helper 两次 normalize 仍有 fmodf/sinf/cosf 标准库成本；H1 使用同帧机械速度乘 poles，不增加配置缓存/同步机制。

这些不是完整调用链峰值、IRQ 嵌套栈高水位或 WCET。链接仍有基线同样的 nosys `_read/_write/_close/_lseek` 与 RWX segment warning，保留日志，没有屏蔽或声称零 warning。

## 13. 安全隔离与独立审查

Board/Serial Host 行为检查仍证明：默认 LOCKED，ARM/START 为 OUTPUT_LOCKED，`foc_board_output_submit()` 拒绝 UPDATE，`FOC_BOARD_UPDATE_NOT_APPLIED`，`physical_applied=false`。V1 codec/layout、Target Policy、生命周期优先级、Current/Velocity PI、Position P、Feedforward 准入、Safety 及真实 Timer/ADC/SPI/PWM/MOE 不在本轮修改中。

独立审查以阶段快照差异审查，确认一项 P2：motion-disabled Virtual 用未配置的 rotor 会把初相位 .37 变为 0。新增回归先 RED，再恢复该模式原生产者；完整最终回归 GREEN。没有其它确认的 P0/P1/P2、没有 deferred minor。审查者列出的精度域、首帧、连续位置分辨率和证据范围限制全部保留，没有据此扩大实现。

## 14. 历史保护与回滚

不 clean-first、不删除构建目录、不 reset/clean/stash/rebase、不覆盖旧 CSV/JSON/Golden/evidence。新增测试使用新源码副本和输出目录；旧项目/ignored 产物逐文件保护结果见 `change-verification-final.json`。主工作树保持干净，HEAD 未变；维护日志只追加。

回滚以本轮 `final-stage.patch` 的逆向应用为单位，涵盖生产者/消费者/ABI 和新增测试文档，不单独回退某一个 flag。已对当前工作树执行 `git apply --reverse --check`，只检查而不实际回滚；执行前须重新确认阶段文件哈希没有后续改动，之后重编所有消费者。不得 reset 到 HEAD，以免丢失此前未提交成果。首版 patch 被 Python 以 CRLF 写出，导致部分 LF 源文件的检查失败；最终 patch 明确以 LF 字节写出并检查通过，早期失败文件/日志保留，不用于回滚。

## 15. 最终边界和停止点

本轮获批正常链的 P2-02 已关闭；P2-01/Clarke 前阶段文档保留。Legacy Rotor API、motion-disabled direct-angle Virtual 来源的大输入精度限制仍存在，这是明确保留的兼容路径，不当作任意来源自动高精度。Float32 多圈 Position 的分辨率、Host 更强首帧 readiness、默认控制品质不足、MCU 栈峰值/WCET 等不由本轮解决。

保留：

```text
G3-SAFE HARDWARE NOT VERIFIED
G3-PWR LOCKED
AT32F415 WCET NOT VERIFIED
MOTOR PHYSICAL OPERATION UNVERIFIED
```

最终结论 **NEXT-CODE-IMPROVE-02B PASS**，仅指授权范围的相位来源整改、兼容与软件验证。没有 commit、push、merge、发布、刷机、上电或真实电机操作；完成后停止。
