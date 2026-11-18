# NEXT-CODE-IMPROVE-05｜Pipeline 冗余寄存器预拷贝优化

2026-11-17。**PASS，保留单行优化。** `foc_pipeline_submit()` 的原版与优化版在 11 条字段级事务记录上完全一致。Arm GNU 14.3.1 同条件重建后，提交函数和 Pipeline 翻译单元各减少 **14 B**，最终 V2 镜像 FLASH 减少 **12 B**，静态栈帧和 RAM 保持原值。独立复核未发现新增 P0/P1/P2。

## 1. 基线与最小差异

- 仓库 `2023violet/Hollow-Gimbal`，分支 `codex/falconfoc-v2-vm01a`，工作树 `D:/study/Software/Hollow-Gimbal-vm01a`。开始时本地 HEAD、跟踪引用和 `git ls-remote` 都指向 `69485957ff6d47de51e1027b8b3e3ee00b4d71bb`。
- 依据 [仓库约定](../../AGENTS.md)、[项目纲领](../architecture/FalconFOC_Project_Charter_and_Engineering_Memory_v1.0.md) 和 [REVIEW-05](2026-11-17-next-code-review-05.md)。正式生产差异仅位于 [foc_pipeline_submit](../../runtime/src/foc_pipeline.c#L54)。

```diff
-    struct foc_device_sim_registers candidate = mock->registers;
+    struct foc_device_sim_registers candidate;
```

原版与优化版均从同一提交检出到独立临时目录。原版源码与启动时生产文件逐字节相同，优化版与工作树中的改动逐字节相同。两边分别从新 CMake 目录配置构建。严格质量门所需的三个 Legacy 预设镜像也在隔离副本重新构建。

## 2. 完整写入与控制事务

- **UPDATE 成功。** Device Simulator 通过 C11 复合字面量整体赋值，显式设置 `bridge_gate_closed=1` 和三个 compare。遗漏的 `bridge_enabled` 由聚合初始化规则置 0。五个字段全部得到新值，校验仅查看请求和 PWM 周期。 [寄存器类型](../../g3/np/include/foc_device_sim.h#L87) · [Device 提交实现](../../g3/np/src/foc_device_sim.c#L160)
- **INHIBIT 成功。** `foc_device_sim_set_safe_registers()` 显式写 `gate_closed=1`、`bridge_enabled=0` 和三个零 compare，独立于旧寄存器内容。 [安全寄存器写入](../../g3/np/src/foc_device_sim.c#L41)
- **拒绝和失败。** `&&` 短路顺序仍为 forced rejection、Device 提交、可选 adapter。传递 `&candidate` 不读取其值，adapter 只接收原请求。只有三道条件均通过才读取 `candidate` 并写回 `mock->registers`，否则保留已提交快照并撤销本次确认。UPDATE 拒绝后的独立 INHIBIT 可在获接受时提交安全快照。
- **控制语义。** `attempts`、`last_request`、`last_inhibit_attempt`、`last_accepted`、`output_confirmed`、`updates`、`last_update` 和 `last_rejected_update` 的读写点保持原顺序。最终 Safety、STOP guard、adapter 与 PI Trial 的提交点未移动。公共 API、配置、板级输出许可和 Runtime 生命周期均无差异。 [输出准入与 PI 提交](../../runtime/src/foc_pipeline.c#L440)

## 3. 行为等价性验收

原版和优化版分别编译同一个临时探针。探针借用正式 Pipeline fixture，直接覆盖静态 submit 的非法请求入口，再通过公共 step 验证回调、PI 与恢复。五个寄存器字段、请求字段、计数、确认位、回调入口观察和两轴积分器皆按字段断言，未使用结构体内存比较。探针启用 Clang ASan+UBSan，遇到 sanitizer 错误立即失败。

| A/B 场景 | 两版实测行为 |
| --- | --- |
| 正常 UPDATE | 初始哨兵 `{7,9,11,22,33}` 被完整覆写为 `{1,0,100,200,300}`，`updates=1`，回调一次并看到旧快照。 |
| 强制拒绝 UPDATE | 寄存器保持上一快照，`updates=1`，确认位为 false，回调零次。 |
| 无效 Device 请求 | compare 超出周期，Device 拒绝。原快照与历史接受 UPDATE 保留，回调零次。 |
| 强制拒绝 INHIBIT | 寄存器仍为 `{1,0,100,200,300}`，确认位为 false，记录 INHIBIT 尝试但无回调。 |
| 正常 INHIBIT | 五字段进入 `{1,0,0,0,0}`，确认位为 true，`updates` 保持 1。 |
| adapter 拒绝 UPDATE，接受回退 INHIBIT | 两次回调依次为 UPDATE、INHIBIT，两次入口看到原已提交快照。返回 `ERR_UPDATE`，PI 保留，最终确认安全快照。 |
| adapter 同时拒绝 UPDATE 和 INHIBIT | 同样的两次回调与入口快照，返回 `ERR_INHIBIT`，原寄存器及 PI 保留，确认位为 false。 |
| STOP 与 STOP + FAULT | 各一行。均只调用一次 INHIBIT，未提交候选 PI。最终分别为 DISABLED、FAULT_LATCHED。 |
| STOP 恢复与故障清除后恢复 | 各一行。新鲜命令和新样本经 ARM/START 后再次确认 UPDATE，`updates` 从 1 增至 2。 |

两边 11 条输出记录逐字节一致，记录 SHA-256 为 `2ef3bff4de15661072fe4e0db42dbb2e4b197912162479ba2dabcb36aa89a502`。原实现与优化版均满足基准，验收采用行为等价检查。

## 4. ARM 代码与静态资源

使用 Arm GNU 14.3.1、`-std=c11 -mcpu=cortex-m4 -mthumb -mfloat-abi=soft -Os -ffunction-sections -fdata-sections -fstack-usage` 和目标原有选项。逐项核对 `compile_commands.json`，源根路径和输出对象路径归一化后，两边的编译命令相同。使用 `nm -S`、`size`、`objdump -dr`、`.su` 和链接 MAP 对照。

| 指标 | 原版 | 优化版 | 差值 |
| --- | ---: | ---: | ---: |
| `foc_pipeline_submit` text（对象与最终 ELF 符号） | 204 B | 190 B | −14 B |
| `foc_pipeline.c` 对象 text/data/bss | 2268/0/0 B | 2254/0/0 B | −14/0/0 B |
| 函数静态反汇编指令行 | 71 | 65 | −6 |
| `foc_pipeline_submit` 静态自身帧 | 48 B | 48 B | 0 |
| `step_guarded` / `emit_inhibit` / `start` / `init` 静态自身帧 | 312/48/80/408 B | 312/48/80/408 B | 0 |
| Runtime `h0_step` / Device `sim_submit` 静态自身帧 | 168/24 B | 168/24 B | 0 |
| 最终 ELF text/data/bss | 38240/92/3680 B | 38228/92/3680 B | −12/0/0 B |
| 链接区域 FLASH / RAM | 38332/3768 B | 38320/3768 B | −12/0 B |

原汇编在校验前从 `mock->registers` 读取 20 B，以 `ldmia/stmia` 和 `ldr/str` 复制到栈上；优化版移除了这组传送，保留接受后的候选写回。前导保存寄存器、24 B 局部栈分配与回收相同，`bl foc_device_sim_submit` 和可选 `blx` adapter 保留，没有新增 libcall 或栈读写。整镜像少 12 B 是因为链接 MAP `.text` 对齐填充由 22 B 增至 24 B。

优化版 BIN SHA-256 为 `fd04a5c89d20f76c3e5ba2e7078f9b40b752660e3b0359e2cc9e98c63a771b62`，原版为 `3a0260bc7519e3e830fe1e5510abe2585d62dbcbc945c97c02f72fa615bca3dc`。这些是静态资源与指令证据，WCET、实际控制节拍与栈峰值待实板测量。

## 5. 本轮正式回归

| 验证 | 本轮结果 |
| --- | --- |
| Runtime Debug（Clang 22.1.8） | 原版 **11/11**，优化版 **11/11**，含 mailbox P1 回归、Pipeline、STOP 与 Runtime 事务。 |
| Simulation Debug / Core Debug | **15/15** / **15/15**。 |
| G3-NP Device / 锁定 Board Host | **2/2** / **4/4**，含 Board 串口与 Python 用例。 |
| Runtime / Simulation ASan+UBSan | **11/11** / **15/15**。用本机 LLVM 动态库路径，ASan/UBSan 启用 `halt_on_error=1`。 |
| Source Contract | **PASS**，`82 C/H`、`9 CMake`、`9 README`。 |
| C 格式 | LLVM clang-format **22.1.8** 对 Git LF 内容格式化后逐字节一致，覆盖整个改动文件。仓库内 clang-format 12 无法解析 `BlockIndent`，Windows checkout 为 CRLF。检查在内存按仓库 `LineEnding: LF` 归一化，磁盘文件未作格式重写。 |
| Legacy firmware | `tiny`、`standard`、`full-diag` 三个 carrier 预设重新交叉构建通过，Git Bash 的 `./run.sh build` 通过。 |
| 严格 `Scope all -RequireCMakeFormatter` | **PASS**，30 个 CMake 格式检查、Mock OpenOCD CLI、平台/产物/串口契约和新鲜 Legacy Host **45/45**。首次验证驱动在启动 Git Bash 时遇 Windows 可执行文件查找错误，改用绝对路径运行后完成门禁。 |

全部数字来自本轮新执行。[原始证据包](2026-11-17-next-code-improve-05/raw-evidence.zip) 包含 A/B 探针、日志、汇编、资源对照与保护校验摘要，SHA-256 为 `84f5f559b534aea629e1ae6550f43d689a98b7fefa1a49948d958f23c1cdf71e`。

## 6. 独立复核、保护与交付停止点

独立审查者复核了实际单行差异、Device 五字段写入、候选读取被成功分支支配、回调入口的旧快照、两轴积分器、拒绝确认位及 STOP/FAULT 顺序。复核同时确认 Runtime 的预判资格、过期邮箱分离和正常 H0 处理仍由现有 mailbox 回归保护。审查结论 **PASS，保留，新增 P0/P1/P2 为 0**。审查范围是源码与本轮 Host/ARM 静态产物。

- **既有 REVIEW-05 待交付文件**为 [评审报告](2026-11-17-next-code-review-05.md) 与 [维护日志](../governance/Themis_工程维护日志.md) 的 REVIEW-05 条目。报告逐字节保留，维护日志对应前缀逐字节保留。
- **本轮 IMPROVE-05 待交付文件**为 [单行生产补丁](../../runtime/src/foc_pipeline.c#L59)、本报告、[原始证据包](2026-11-17-next-code-improve-05/raw-evidence.zip) 与同一维护日志新增条目。正式测试文件、公开接口与配置均无差异。
- **工作树保护**比较 2,427 个既有忽略文件的路径、大小、修改时间与 SHA-256，结果完全一致。Git 停在工作树，等待用户确认交付范围。

建议下一阶段转向 **Current 电流环的 R/L 辨识与 PI 整定能力**。先用固定 Virtual Motor 工况和明确的 RUNNING 评价窗口，建立阶跃、反向、受限后恢复的可重复基线，再验证独立 C11 辨识结果和候选 PI 的有效域、收敛及过冲，保留失败不应用的校准准入。已有 [SYSTEM-ANGLE-01 对照](2026-11-17-next-system-angle-01.md) 提供相位与电流质量的区分依据。本轮止于优化验收。

```text
G3-SAFE HARDWARE NOT VERIFIED
G3-PWR LOCKED
AT32F415 WCET NOT VERIFIED
MOTOR PHYSICAL OPERATION UNVERIFIED
```
