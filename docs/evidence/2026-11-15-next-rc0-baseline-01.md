# FalconFOC V2｜NEXT-RC0-BASELINE-01 软件候选基线

## 结论

`NEXT-RC0-BASELINE-01 PASS`

本轮证明当前 `codex/falconfoc-v2-rc0` 工作树可以脱离原有 `build` 缓存和原始工作目录，从一份独立源码快照重新配置、编译并通过 Host、Sanitizer、Legacy ARM、V2 ARM、Source contract 和全仓软件门禁。

本结论只覆盖 RC0 软件源码候选基线的可追踪性与可复现性，不代表实板安全、MCU WCET 或电机运行已经验收。

以下边界保持不变：

- `G3-SAFE HARDWARE NOT VERIFIED`
- `G3-PWR LOCKED`
- `AT32F415 WCET NOT VERIFIED`
- `MOTOR PHYSICAL OPERATION UNVERIFIED`

本轮没有修改控制算法、PI/Feedforward、Runtime、Safety、Board 参数、Timer/ADC/SPI/PWM、CPU/FPU/ABI 或输出锁；没有刷机、上电、启用 PWM、写 Flash、commit 或 push。

## 一、候选基线身份与工作树

| 项目 | 实际值 |
| --- | --- |
| 原始仓库 | `D:/study/Software/Hollow-Gimbal` |
| 分支 | `codex/falconfoc-v2-rc0` |
| Git HEAD | `46bc963f5e5849e4440d1714a845fe485d338837` |
| 原工作树状态 | 保留既有未提交成果；本轮没有 reset/clean/stash |
| 状态行数 | 基线开始时 87；加入本轮报告后 88（包含既有修改与本轮证据文件） |
| 已跟踪差异统计 | 32 files changed, 1349 insertions, 333 deletions |
| 原工作树 `git diff --check` | PASS（Git 对 LF/CRLF 的提示不影响退出码） |

Git HEAD 只用于记录身份，完整候选内容以当前工作树和下述源码清单为准。

## 二、独立源码快照与完整性

独立源码快照位于：

`D:/study/Software/Hollow-Gimbal_RC0_Verification_20261115_01`

它在原仓库之外新建，保留文件原始字节，不包含 `.git`、`.superpowers`、`.zcode`、`build*`、`work`、`Testing`、`tmp` 和 `baseline` 元数据目录，也排除被 `.gitignore` 忽略的诊断输出 `state show`。正式源输入中的 `.vscode`、`memory.sh`、`symbols.txt` 均已纳入；其中 `memory.sh` 是 Firmware CLI contract 的正式入口，不能当作可选脚本排除。

清单文件：

- [original-source-manifest.csv](D:/study/Software/Hollow-Gimbal_RC0_Verification_20261115_01/baseline/original-source-manifest.csv)
- [snapshot-source-manifest.csv](D:/study/Software/Hollow-Gimbal_RC0_Verification_20261115_01/baseline/snapshot-source-manifest.csv)

逐文件 SHA-256 对照结果：

| 指标 | 结果 |
| --- | ---: |
| 原工作树源文件 | 1660 |
| 独立快照源文件 | 1660 |
| 缺失文件 | 0 |
| 意外文件 | 0 |
| SHA-256 不一致 | 0 |
| `.vscode` 文件 | 5 |
| `memory.sh` | 已纳入，原/快照 SHA-256 均为 `B924B879319AFBC0C87EE8756129796F3618DF3784AB6A9CE9F1553A4F69E498` |

为了运行依赖 Git 根识别的脚本，另建临时验证副本：

`D:/study/Software/Hollow-Gimbal_RC0_Verification_20261115_01_build`

该副本只初始化了本地 Git 元数据并执行 `git add -A`，没有创建 commit、远程或改变原仓库 Git 状态。所有实际编译均在该副本的新建 `build` 目录完成。

本轮曾经发现初始快照排除了 `memory.sh` 和 `.vscode`，导致 CLI contract 在独立目录中真实失败；补回正式输入后重新生成清单并重跑门禁，最终快照不再有该遗漏。这一过程作为基线证据保留，而不是通过跳过合约掩盖。

## 三、独立构建与测试

完整命令记录见 [build-commands.txt](D:/study/Software/Hollow-Gimbal_RC0_Verification_20261115_01/baseline/build-commands.txt)，工具链见 [toolchain-versions.txt](D:/study/Software/Hollow-Gimbal_RC0_Verification_20261115_01/baseline/toolchain-versions.txt)。最终快照的门禁日志保存在 [scope-all.log](D:/study/Software/Hollow-Gimbal_RC0_Verification_20261115_01/baseline/scope-all.log)、[scope-falconfoc.log](D:/study/Software/Hollow-Gimbal_RC0_Verification_20261115_01/baseline/scope-falconfoc.log)、[firmware-cli-contract.log](D:/study/Software/Hollow-Gimbal_RC0_Verification_20261115_01/baseline/firmware-cli-contract.log) 和 [source-contract.log](D:/study/Software/Hollow-Gimbal_RC0_Verification_20261115_01/baseline/source-contract.log)。核心结果如下：

| 验证项 | 独立结果 |
| --- | --- |
| Host Debug（MinGW GCC，干净配置） | 45/45 PASS |
| Clang ASan/UBSan | 35/35 PASS（Core 14、Sensors 2、Calibration 1、Runtime 10、NP 2、SAFE 1、Protocol 1、Board 4） |
| Legacy tiny carrier MinSizeRel | ARM 构建 PASS |
| Legacy standard carrier MinSizeRel | ARM 构建 PASS |
| Legacy full-diag carrier Debug | ARM 构建 PASS |
| V2 AT32F415 ARM | 构建 PASS |
| FalconFoc source contract | 68 C/H、8 CMake、8 README PASS |
| `Scope falconfoc` | PASS |
| `Scope all` | PASS |
| CMake formatter | 29/29 PASS（由 `Scope all` 实际执行） |
| `git diff --check` | PASS |
| Firmware CLI contract | PASS，使用隔离 Mock OpenOCD |
| SYSTEM replay | `3f01f6e3` |
| Long replay | `49dea950` |

独立 Host 首次使用系统默认 Clang 22 配置时，`tmdef.h:108` 按项目约束拒绝了不支持的编译器；这属于已记录的工具链选择问题，不是源码缺陷。删除临时验证副本中的旧 Host 缓存后，使用 `C:/mingw64/bin/gcc.exe` 和 `g++.exe` 从零配置，Host 45/45 及 `Scope all` 均通过。原工作树没有被清理或改写。

Sanitizer 使用 Clang 22，编译和最终链接均启用：

`-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer`

没有关闭 sanitizer，也没有在 ARM 固件上使用 Host sanitizer 选项。

## 四、独立固件产物与对照

### V2 AT32F415

独立产物位于验证副本 `build/v2-arm`。使用 `cortex-m4`、`FPU=none`、`FLOAT=soft`，保持当前 V2 ABI 约束。

| 产物 | 字节数 | 独立 SHA-256 | 对照结论 |
| --- | ---: | --- | --- |
| ELF | 65,824 | `51D7428FF2414FA4623DCE5086A6CD8D4F69058DFC6405B317DC28875E281551` | 与原工作树 V2 ELF 一致 |
| BIN | 36,736 | `9806ADBFA3282450527F40BB97CB1DA75FAFE41FB405F1126114187C6393B029` | 与原工作树 V2 BIN 一致 |
| HEX | 103,423 | `B80C263E6B07CE976E2BF1AEDED2737EED536C7224369091DC0919CAA48C51AE` | 与原工作树 V2 HEX 一致 |
| MAP | 240,450 | `BD3B81359E348163DBF05F9DA52B267259757CAF9C22CEE7508D54695717C1CA` | 与原 MAP 的绝对构建路径文本不同；不影响可加载内容 |

`arm-none-eabi-size`：

`text=36644, data=92, bss=3544, dec=40280`

V2 参考资源仍为 Flash `36,736 B`、RAM `3,632 B`；其中 `._user_heap_stack=1,024 B` 为链接脚本保留栈空间。`.text + .data + .bss` 没有被误当成 Flash 占用。

### Legacy 三种构建

| Preset | text/data/bss | ELF SHA-256 | BIN SHA-256 | HEX SHA-256 |
| --- | --- | --- | --- | --- |
| tiny carrier MinSizeRel | `37372/128/18368` | `D14D0B954733DCB0B499254927301EAB14B9E474518AE2CD78E6B0579B53745E` | `D7D049C03DDD2378DEFE39B1E516EB6FF0973FF01818658DB58D9F9C683C37BD` | `DEE2EEC96B4BB4F9591E119FEAEEC3B45F612CD4614637C62B8C1E74A372D83A` |
| standard carrier MinSizeRel | `40596/128/20104` | `258BAA189100B0379297F5CA9F47B0DD840AC41022B178C8BBC2C5BCAE7C3F20` | `7A23BB4370CBA6139266EA6DF5358E43D2B200F438FFE7359C114167AE963A78` | `F50AD30A428F10F98D0B256D9AB6AF658249E9CDB30B49A1D49ADDDD66D7FAFB` |
| full-diag carrier Debug | `79576/132/21840` | `F9123BA3692109753E5F9EDAB1BCF2C3EC8AB3FC94C290C72771C64A935CA3E4` | `CDA3356722B1CCA6786FBF360B270E0E13FA8F1D22459CB8EA6261ADAE54FD0C` | `99AD3EE360E59DE6F877FD84DF07C15777FBA04EB4645904756F86C5ACF2ECA5` |

tiny 和 standard 的 ELF/BIN/HEX 与原工作树逐字节一致。full-diag 的可加载 section 尺寸和 BIN 长度一致，但 Debug 镜像包含 `__DATE__`/`__TIME__` 字符串；独立构建日期/时间不同，BIN 仅在这些字符串位置发生差异（偏移 `68992`～`69009`），没有发现指令或控制常量差异。full-diag ELF/MAP 还会包含构建目录和调试路径元数据，因此不要求跨目录逐字节一致。

## 五、隐藏依赖审查

- 新增平台时序头 `platform/at/at32f415/generated/inc/tm_platform_foc_timing.h` 已进入源码快照；Source contract、Legacy Host、Legacy ARM 和 V2 ARM 均实际消费/构建通过。
- CMake target、include directory、compile definition、toolchain、linker script、startup、generated source 均来自快照；没有从原仓库 `build` 读取源输入。
- Git 依赖仅用于脚本的 repository-root/source-contract 识别；验证副本使用独立本地 index，未伪造历史提交，也未把 Git HEAD 当作源码清单。
- 合法环境依赖为现有 Git Bash、MinGW GCC、LLVM/Clang、ARM GCC、CMake、Ninja 和 Python；本轮没有安装新依赖。
- 未发现需要手工生成且未记录的生产源码步骤。

## 六、冻结判断

当前候选已经满足软件源码基线冻结条件：源输入完整且逐文件一致，独立目录从零构建通过，Host/Sanitizer/Legacy/V2/门禁/replay 结果均可追踪，V2 可加载 BIN/HEX 与原构建一致，唯一观察到的 Legacy Debug 差异是可解释的日期/时间元数据。

因此建议将当前工作树作为 **RC0 软件源码提交候选** 冻结，后续如需修改应从新的分支或新的候选基线开始，不能把本轮验证副本或旧 build 目录当作源码来源。

这不是产品发布批准：真实 AT32F415 时序、栈高水位、G3-SAFE 硬件链、功率级行为和电机实机性能仍需独立准入。

