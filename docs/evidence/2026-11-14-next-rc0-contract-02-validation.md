# FalconFOC V2｜NEXT-RC0-CONTRACT-02 当前产品契约迁移与 RC0 全仓质量门禁

## 结论

`NEXT-RC0-CONTRACT-02 PASS`

`RC0 SOFTWARE GATES PASS`

本轮把当前 FalconFoc 产品契约从历史 Themis-lite 平台契约中分离，并用真实 AT32F415 工作树复验软件门禁。没有创建 AT32F403A 占位文件，没有复制平台文件伪造历史平台，也没有修改 FOC、Runtime、Safety、Device、Board 功率输出或旧历史契约脚本。

以下边界保持不变：

- `G3-SAFE HARDWARE NOT VERIFIED`
- `G3-PWR LOCKED`
- `AT32F415 WCET NOT VERIFIED`
- 未刷机、未上电、未启用 PWM、未写 Flash、未 commit、未 push。

## 1. 复验身份

| 项目 | 实际值 |
| --- | --- |
| 工作目录 | `D:/study/Software/Hollow-Gimbal` |
| 分支 | `codex/falconfoc-v2-rc0` |
| Git HEAD | `46bc963f5e5849e4440d1714a845fe485d338837` |
| 工作树 | 保留既有未提交修改和未跟踪交付；本轮没有 reset、clean、stash、commit 或 push |
| Windows Bash | 复用现有 `D:/Git/bin/bash.exe`，本轮门禁进程临时前置 `D:/Git/bin;D:/Git/cmd` |
| Bash / Git | Bash `5.3.15`，Git `2.55.0.windows.4`（ENV-01 已验证） |
| ARM 工具链 | Arm GNU Toolchain `14.3 rel1` |
| CMake / Ninja | CMake `4.4.2`，Ninja `1.13.2` |
| Clang | LLVM Clang `22.1.8` |

本轮工作树在开始时已有大量 NEXT 阶段源码、测试和证据修改；它们均被保留。新增或修改的本轮产品契约文件只包括：

- `tests/script/test_falconfoc_current_contract.ps1`
- `scripts/gates/check_themis_quality_gate.ps1`
- `docs/boot/Themis_启动与初始化流程.md`
- `docs/boot/Themis_启动链回归检查表.md`
- `docs/governance/Themis_工程维护日志.md`
- 本验证记录

历史 `tests/script/test_falconfoc_themis_lite_contract.ps1` 未修改。

## 2. 当前契约迁移

### 当前产品契约

新增 `test_falconfoc_current_contract.ps1`，保留以下仍属于当前 RC0 的检查：

- full-diag、standard、tiny AT32F415 固件 artifact、`.shell_cmd`、Shell on/off 和 IPC 符号边界；
- Root CMake、metadata helper、build axes、contracts、status report 的 owner 边界；
- AT32F415 metadata：`at32f415-hal`、`generated/src/main.c`、`startup_at32f415.s`、`AT32F415xB_FLASH.ld`、`AT32F415KBU7_4`、CMSIS/Flash header、Flash 几何和 TMR5 soft IRQ；
- CPU class 真实值：
  - `cortex-m3-soft`：`cortex-m3`、FPU `none`、ABI `soft`、`ARM_CM3`、`ARM_MATH_CM3`；
  - `cortex-m4f-hard` 参考 class：`cortex-m4`、FPU `fpv4-sp-d16`、ABI `hard`、`ARM_CM4F`、`ARM_MATH_CM4`；
- FreeRTOS manifest、RTE 旧 AT32F403A header 禁止项和公共 northbound vendor header 禁止项；
- F415 linker 的 heap/stack reserve、Flash/RAM geometry 和占位符禁止项；
- 当前 Legacy `foc_config.h` 的真实 150 MHz/5 kHz/1 kHz/1 kHz timing token；
- BSP/application/Shell owner、USART1 transport、启动桥、motor task 初始化顺序；
- Board1 `OUTPUT_LOCKED` / `UPDATE_NOT_APPLIED` 安全输出边界。

当前契约明确打印 timing seam 尚未迁移。它验证真实当前值，不宣称已经建立 platform-owned timing header，也没有把 F403A/F423 历史值带入 F415。

### Gate 入口

`check_themis_quality_gate.ps1` 的 `bsp`、`applications`、`all` scope 改为调用 current contract。历史 Themis-lite 脚本仍然保留，供历史/准备平台审计使用；它不再作为当前 RC0 的唯一阻断入口。

### 启动文档

两份 live boot 文档的当前入口统一为：

`platform/at/at32f415/generated/src/main.c`

历史 AT32F403A/F423 归属没有通过创建虚假路径修复，仍由 CONTRACT-01 的历史审计记录保留。

## 3. 负向契约测试

使用隔离临时工作树和真实 firmware artifact，基线先通过，然后逐项注入错误。原始日志保存在本地 ignored 证据目录 `build/rc0/contract-negative-logs/results.log`，不进入产品源码。

| 夹具 | 预期结果 |
| --- | --- |
| wrong CPU class | 拒绝 |
| wrong FPU/ABI | 拒绝 |
| wrong Flash geometry | 拒绝 |
| wrong linker stack | 拒绝 |
| missing tiny map artifact | 拒绝 |
| wrong Legacy timing | 拒绝 |
| wrong metadata owner path | 拒绝 |
| stale AT32F403A boot document | 拒绝 |
| old vendor header leak | 拒绝 |
| output lock changed | 拒绝 |
| IPC symbol leak | 拒绝 |
| Shell symbol leak | 拒绝 |

结果：`NEGATIVE_TESTS_PASS=12`；current contract baseline `PASS`。这证明新增检查器不仅接受当前文件，也能拒绝典型错误；不代表它替代了公共 API 语义代码评审。

## 4. 软件门禁结果

以下结果均为本轮实际执行，不沿用旧日志冒充本轮结果：

| 门禁 | 结果 |
| --- | --- |
| current contract standalone | PASS |
| `Scope falconfoc` | PASS |
| `Scope bsp` | PASS |
| `Scope applications` | PASS |
| `Scope all` | PASS |
| CMake formatter | `29/29 PASS` |
| FalconFoc source contract | PASS，61 C/H、8 CMake、8 README |
| Firmware CLI contract | PASS，使用 Mock cmake/ninja/OpenOCD，未接触真实设备 |
| Host Debug | `45/45 PASS` |
| Clang ASan/UBSan | `28/28 PASS`，10 Core + 1 Sensors + 1 Calibration + 2 G3-NP + 1 G3-SAFE + 8 Runtime + 1 Protocol + 4 Board |
| SYSTEM replay | `3f01f6e3`，updates `96`、H0 due `103`、H1 due `41`、outer seq `40`、session `3` |
| Long replay | `49dea950`，updates `99998`、due `100001`、missed `0`、late `0`、budget `0` |
| Legacy Bench | PASS |
| `git diff --check` | PASS；Git 仅报告既有 LF/CRLF warning，无 diff 错误 |

`RelWithDebInfo` 的历史 Debug-only fail-fast 行为仍按既有契约记录为 `44/45`，没有修改 `NDEBUG`、测试语义或用关闭测试制造通过；它不属于本轮产品契约迁移的阻断项。

## 5. AT32F415 构建

### Legacy presets

本轮实际重新构建：

```text
cmake --build --preset build-fw-tiny-carrier-minsizerel --parallel
cmake --build --preset build-fw-standard-carrier-minsizerel --parallel
cmake --build --preset build-fw-full-diag-carrier-debug --parallel
```

三个 preset 均使用 `at/at32f415` 和 `cortex-m3-soft`，均生成 ELF/BIN/HEX/MAP；没有修改 Flash、RAM、CPU、FPU 或 ABI 参数。

### V2 Reference Firmware

重新构建：

```text
cmake --build build/rc0/baseline-arm --parallel 4
```

结果：PASS。链接器报告：

- `.text = 36,644 B`
- `.rodata = 1,352 B`
- `.data = 92 B`
- `.bss = 3,544 B`
- Flash 使用 `36,736 B / 128 KB`
- RAM 使用 `3,632 B / 32 KB`
- ELF `ELF32 ARM`，EABI flags 标记 `soft-float ABI`

产物 SHA-256：

| 产物 | SHA-256 |
| --- | --- |
| `falconfoc-v2-at32f415.bin` | `9806ADBFA3282450527F40BB97CB1DA75FAFE41FB405F1126114187C6393B029` |
| `falconfoc-v2-at32f415.elf` | `51D7428FF2414FA4623DCE5086A6CD8D4F69058DFC6405B317DC28875E281551` |
| `falconfoc-v2-at32f415.hex` | `B80C263E6B07CE976E2BF1AEDED2737EED536C7224369091DC0919CAA48C51AE` |
| `falconfoc-v2-at32f415.map` | `BDC648824C427C873259638A30EB2FAE0C59E1598EBEA3C2C43E0C29CF6516B3` |

主要静态栈帧（`.su`，单位 B）：

| 函数 | 栈帧 |
| --- | ---: |
| `foc_current_step_internal` | 184 |
| `foc_pipeline_step_guarded` | 312 |
| `foc_runtime_control` | 224 |
| `foc_runtime_h1_step` | 288 |
| `foc_runtime_h0_step` | 96 |
| `foc_board_output_submit` | 96 |
| `foc_encoder_update` | 72 |

这些是静态 ARM 编译产物和 `.su` 证据，不是目标 MCU 实测 WCET、IRQ 截止时间或功率级安全证据。

## 6. 本轮未解决项

- platform-owned timing seam 仍需独立迁移任务；当前契约只验证真实 F415 Legacy timing，不改变其 owner。
- AT32F415 真实 MCU WCET、实板 WDT、PWM 禁止路径和 G3-SAFE 硬件能力仍未验证。
- G3-PWR 仍锁定，不能把 Host/ARM 构建结果表述为带电机运行通过。
- 历史 `test_falconfoc_themis_lite_contract.ps1` 仍可能在单独历史审计中报告 F403A/F423 drift；这不再混入当前 RC0 软件 gate。

## 7. 最终状态

`NEXT-RC0-CONTRACT-02 PASS`

`RC0 SOFTWARE GATES PASS`

`G3-SAFE HARDWARE NOT VERIFIED`

`G3-PWR LOCKED`

`AT32F415 WCET NOT VERIFIED`

/* ---------------------------------------- end of file ----------------------------------------- */
