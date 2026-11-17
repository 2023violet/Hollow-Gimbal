# FalconFOC V2｜NEXT-CODE-IMPROVE-02

日期：2026-11-16。状态：**NEXT-CODE-IMPROVE-02 PARTIAL — AWAITING PRODUCTION CHANGE APPROVAL**。

本阶段停在用户任务第六节明确规定的生产变更批准门。共享 Runtime/Sensor API、正式 H1 相位来源和 Core Rotor 实现尚未修改，不能将原型和设计完成等同于完整整改。

## 1. 输入身份和实际差异

开发树 `D:/study/Software/Hollow-Gimbal-vm01a`，分支 `codex/falconfoc-v2-vm01a`，HEAD `a198f6d4a6c61f1bb9dd5968a46e7f38ac49a3f0`。此前未提交 VM/Quality/Tuning/Release Prep/IM01 成果保留。

起始 2511 个项目文件和 2427 个 ignored 文件（322054220 字节）有 SHA-256 基线。新实验根为 `D:/study/Software/FalconFOC-code-improve-02-20261116`；包含起始 `snapshot/`、Git 状态、原型、全部日志和批准提案。

仓库内唯一受改代码文件是 `core/include/foc_core.h`，仅注释 +22/−3：角度精度域及 Clarke 平衡/重构前提。函数体、声明、结构体布局、输入拒绝行为和错误码不变。另有本报告和维护日志追加；其他既有文件与构建产物的最终核对见外部 `change-verification.json`。

完整提案：[PRODUCTION_CHANGE_PROPOSAL.md](D:/study/Software/FalconFOC-code-improve-02-20261116/PRODUCTION_CHANGE_PROPOSAL.md)。它包含两条数据链、所有权/有效性表、精确公开字段/API、兼容性、错误/首次帧/配置生命周期、回滚、测试和 ARM 安全影响，共 17 节。

## 2. P2 重新复现与 Oracle

| 实验 | 本轮重新计算的结果 |
| --- | --- |
| float(1e8)，对实际存储值的参考 | 输入表示误差 0；周期偏差 −2.78275328638 rad；1 A Park 误差 1.96789487608 A |
| 原始 double(1e8+0.25) | Float32 丢失 0.25 rad；相对原始值的最终误差 1.99703926465 A，不能靠提升已存储 float 恢复 |
| Encoder 约 −16383 圈 | 真实公开更新、同 raw=123；经生产 H1→H0/Current，误差 +0.0143116418176 rad，dq 误差 0.0143115207892 A |
| Encoder 约 −262143 圈 | 同上，误差 −0.149750858182 rad，dq 误差 0.149610969486 A |

端到端测试真实执行 ARM/START/Safety/Device 更新，H0 成功并 accepted UPDATE；连续机械位置和源时间戳保留。它是 Host 软件链反例，不是实机运行测量。

独立 Oracle 对 int64 负余数显式转数学模，再以 double 原始周期相位的 sin/cos 建立参考，不调用生产 normalize。矩阵覆盖 7 CPR、5 pole pairs、10 个正负/端点累计位置和 6 offset，共 2100 例；另有 12000 次真实 Encoder 正反方向/跨圈/回绕/reset 检查。

## 3. A/B 方案比较与选定提案

| 路径 | 7 极对同码六个转数点 | 2100 例矩阵最大误差 rad |
| --- | --- | ---: |
| 旧累计 Float32 | 随转数变化，末组约 0.14975 rad | π |
| A：复用 single-turn float | 稳定约 1.10232299e−6 rad | 0.0362816484（大 pole pairs 放大） |
| B：先做整数电周期乘模 | 稳定约 3.28188486e−7 rad | 7.65757044e−7 |

以事先固定的 Host 数值预算 `1e−5 rad`，B 矩阵 2100/2100，A 有 443 例超出；A 在 Board1 名义条件下仍是可行替代，没有被误定性为算法错误。

提案选择 B 的 Encoder 精确相位，并让现有 H1 producer 向 Runtime 发布解析后的周期电相位。这样可复用 `theta_elec_rad`，避免要求非绝对传感器和理想 Host Plant 伪造固定 CPR 的量化输入。连续机械位置继续沿原字段供 Position/Trajectory/机械遥测。

拟批准范围概要：

- Sensor Feedback 尾部新增 `uint32_t mechanical_phase_count`。
- 新增 Core `foc_rotor_phase_from_counts(config, phase_count, CPR, &phase)`，旧 Rotor API 保留。
- Runtime Sensor 尾部新增 `bool electrical_phase_valid`；motion-enabled H1 改为消费同帧 `theta_elec_rad`，保留现有位置/速度有限性、时序、会话和 Safety 检查；缺新相位不回退旧累计路径。
- Board/Host Virtual producer、实际 motion-enabled 测试 factory 与必要文档迁移。保留现有 Host 首帧角度可用、速度尚未建立时的零速假设；不把 helper 成功当作 Sensor 有效，不将 Host 源准入策略扩大到 Board。完整提案第 8.3 节给出两种合法启动顺序的实测边界。
- 不修改 Current PI/AW、Safety 实现、UART V1、Timer/ADC/SPI/PWM/MOE 或真实输出锁。

上述均为**待批准变更**，本轮没有实施。

独立复核补充发现：公开 API 在首次 run_until(0) 前排队 ARM、200 µs 前排队 START，可于 400 µs 提交首个虚拟 UPDATE，而完整速度反馈在 500 µs 才建立。正常调用顺序的首次 UPDATE 则在 600 µs。已撤回提案初稿中普遍的“首个 UPDATE 使用完整有效帧”承诺；此次相位整改保持既有 Host 准入行为，更强的启动/reset/readiness 规则需独立设计。未证明这是 P0/P1 或真实输出缺陷，不能将该限制隐藏在正常演示的 PASS 中。观测源码与日志位于外部 `startup_early_probe.c`、`startup-check/early-output.log`。

## 4. 公共精度和 Clarke 文档

已写入 `foc_core.h`：在 binary32、最近舍入、正确 fmodf remainder、无 fast-math 条件下，`|θ|<=6.2831854820251465f` 的归约圆周误差相对实际存储输入≤`5e−7 rad`。解析界约 `4.1326414e−7`，1000009 点与边界检查最大观察值 `4.13264139109e−7`。

该界不包含原始值转 float、传感器量化、trig 或后续向量算术误差。域外 finite 输入仍被接受，但没有此精度保证。未添加拒绝范围或新错误码。

Clarke 文档明确 `a+b+c=0` 的平衡或约定重构输入；当前 reduced transform 不检查 balance，也不自动去零序。数学公式保持原样。

## 5. 变异检出及证据限制

六种独立变异均在新副本内被行为测试检出，编译成功后测试退出 1：累计 float 回退、Encoder 方向反转、忽略 poles、忽略 offset、混合捕获时间、Sensor invalid 后继续消费相位。原型基线正常退出 0。

其中时间/invalid 守卫基线经真实旧 Runtime 验证 INHIBIT、不增 UPDATE；去掉守卫会使 UPDATE 从 1 增为 2，测试拒绝。它们是 Host-only 组装原型层的证据；**尚未证明正式新 Runtime 来源已经满足这些性质**。获批接入后必须迁入并重跑正式端到端变异。

日志：`experiments/red*`、`experiments/candidates*`、`experiments/binding*`、`mutations/results.json`。

## 6. 本轮实际回归

| 集合 | 结果 |
| --- | --- |
| Core / Core Clang ASan、UBSan | 14/14、14/14 |
| Sensors（含独立 Reference） | 2/2 |
| Runtime / Simulation | 11/11、14/14 |
| Board Host / Python、C CLI | 4/4、11/11，无 skip |
| Current、Velocity、Position、Fault | 由上述对应 Host 测试覆盖，通过 |
| Source Contract | 79 C/H、9 CMake、9 README |
| Formatter / Scope falconfoc / Scope all | 均退出 0；Scope all 含 Legacy Host 45/45、Firmware CLI mock contract |

构建和门禁在新 snapshot/本地 clone 中运行。门禁只读取保留并逐字节复制的历史 Legacy ELF/MAP，不把它称为新 ARM 重建。本轮未正式构建新 ARM 固件，因为生产修改未批准。

首次出现过独立测试 enum 编译错误、CLI export 新目录缺失、头文件末尾换行、过长 TEMP 导致 CMake warning；都保留失败日志，修正外部夹具/文档格式/专用 TEMP 后重跑成功，未修改门禁、跳过用例或覆盖旧证据。完整日志和原因见外部提案第 13 节。

## 7. 资源、安全及剩余事项

整数候选只需 uint32 乘/模；在现有 CPR≤65536、pole_pairs≤65535 下乘积不溢出。ARM 静态编译为 udiv+mls，无 int64 除法 helper。原型 B text=160 B、自身静态 frame=48 B，含速度输出；不是拟议相位专用函数的最终 Flash/栈开销。

拟议目标 ABI：Runtime sensor 24→28 B；Feedback 48→52 B；Board encoder 184→192 B；Runtime 预计1224→1236 B。当前各一个静态实例合计预计 +20 B，最终 ARM 链接/调用链栈/WCET仍待获批测定。Host sizeof 不可替代这些 ARM 数值。

生产 P2-02 仍存在；P2-01 文档及独立验证已完成，永久 CI 的迁入包含在后续批准范围。P3 文档已补。现有 ARM BIN 保持 37864 B / `BDEC4D2706049080FF758EE308ADDC1304294DFD5805F05A0CAE7D56D4191806`；Board1 LOCKED、ARM/START OUTPUT_LOCKED、UPDATE REJECTED、physical_applied=false 保持。

硬件安全、AT32F415 WCET、真实栈高水位和真实电机运行仍未验证。没有 commit、push、合并、发布或硬件操作。本轮在批准门停止；下一步仅在明确批准外部提案第 8 节后实施。
