# FalconFOC V2｜NEXT-RC0-CONTRACT-01 Themis-lite 历史平台契约归属审查

## 结论

`NEXT-RC0-CONTRACT-01 REVIEW COMPLETE`

当前正式状态仍为：

- `RC0 SOFTWARE BLOCKED`；
- `G3-SAFE HARDWARE NOT VERIFIED`；
- `G3-PWR LOCKED`；
- `AT32F415 WCET NOT VERIFIED`。

本轮只读核查没有修改测试脚本、平台文件、CMake、生产源码或质量门禁。

核心结论：当前 `test_falconfoc_themis_lite_contract.ps1` 不是单一契约，而是把三类内容混在一个 RC0 阻断入口中：

1. 仍然有效的 FalconFoc 产品、构建、安全和所有权断言；
2. 已迁移到 AT32F415 但脚本路径仍指向 AT32F403A 的断言；
3. AT32F403A/AT32F423 历史平台和未完成 platform timing seam 的迁移断言。

因此不能创建占位的 AT32F403A 文件，也不能简单把整份脚本注释掉。推荐下一轮将当前产品契约与历史平台迁移契约分离，再分别验收。

## A｜产品和平台归属事实

### 当前工作树事实

| 事实 | 证据 | 结论 |
| --- | --- | --- |
| 当前 live platform 目录 | 只有 `platform/at/at32f415` 存在 | AT32F415 是当前仓库唯一实现平台 |
| 当前有效 firmware presets | `CMakePresets.json` 的 `fw-base` 固定 `THEMIS_PLATFORM_SOC=at32f415` | RC0 默认平台是 AT32F415 |
| Legacy Bench | 当前 preset 输出 `at/at32f415`，CPU class `cortex-m3-soft`，FreeRTOS `ARM_CM3` | Legacy 仍是 AT32F415，不是 AT32F403A |
| V2 Reference Firmware | `firmware/at32f415`、`platform/at/at32f415` 独立 ARM 构建 | V2 仍是 AT32F415 |
| AT32F403A 目录 | `platform/at/at32f403a` 不存在 | 不属于当前可构建平台 |
| AT32F423 目录 | `platform/at/at32f423` 不存在，preset 也没有该平台 | 只能视为未来/准备中的设计目标，不能作为 RC0 当前平台 |
| 当前 Themis platform defaults | `platform/at/at32f415/ThemisPlatformDefaults.cmake` | 当前 metadata owner 是 AT32F415 |
| 当前启动源 | `platform/at/at32f415/generated/src/main.c` | 当前实际启动入口是 AT32F415 生成代码 |

### 历史证据

`test_falconfoc_themis_lite_contract.ps1` 最初在 `789a751`（2026-05-18）加入。`47c4a9e`（2026-09-28）的提交说明明确写出：

```text
test_falconfoc_themis_lite_contract.ps1 仍引用已不存在的 at32f403a 路径，
门禁在此预存失败，属多段式架构迁移欠账，与本轮改动无关。
```

当前 AT32F415 Code Wiki 的设计规范又明确规定：当前工作树和 `platform/at/at32f415` 是实现事实来源，AT32F403A 文档、stale contract 和旧生成产物必须标记为 drift/baseline，而不能默默当作当前事实。

这两项证据共同说明：AT32F403A 不是当前 Legacy 平台的“缺失文件”，而是旧 Themis-lite 基线遗留在当前 RC0 门禁中的路径与归属错误。

## B｜Themis-lite 脚本完整不匹配清单

分类含义：

- `ACTIVE`：当前产品仍必须保留；
- `LEGACY`：历史证据或旧产品仍可保留，但不应阻断当前 RC0；
- `MISMATCHED`：验证目的仍有价值，但路径、平台归属或实现边界错误；
- `STALE`：当前产品已没有有效对应对象；
- `UNKNOWN`：现有证据不足以安全决定。

| 行号/对象 | 原始验证目的 | 当前事实/替代实现 | 分类 | 是否阻断 RC0 | 最小方向 |
| --- | --- | --- | --- | --- | --- |
| 150–260 `Assert-FalconFocFirmwareContracts` | 检查 full-diag/standard/tiny ELF、`.shell_cmd`、IPC、Shell-on/off 符号和 map | 当前 presets 与 `build/fw-*` 产物仍存在，Host/ARM 回归使用这些目标 | `ACTIVE` | 是 | 保留并独立运行 |
| 270–450 BSP、runtime owner、Shell、feature defaults、root CMake 边界 | 防止 app/BSP/Shell/feature ownership 回退 | 当前工作树仍有对应文件和契约 | `ACTIVE` | 是 | 保留 |
| 507、514–532 `platform\at32f403a\ThemisPlatformDefaults.cmake`，但断言要求 `AT32F415xB`、`AT32F415CBT7` | 检查 active platform metadata 和 F415 启动/Flash/CPU token | F403A 文件不存在；真实替代为 `platform/at/at32f415/ThemisPlatformDefaults.cmake`，且其 token 正是 F415 | `MISMATCHED` | 是 | 当前契约改为读取 AT32F415 metadata；保留所有 token 断言 |
| 537–570 CPU class defaults | 检查 `cortex-m3-soft`/`cortex-m4f-hard` 的 toolchain、FreeRTOS port 和 ARM_MATH owner | 两个 CPU class 文件都存在；F415 绑定 `cortex-m3-soft` | `ACTIVE` | 是 | 保留 |
| 607–621 FreeRTOS manifest、RTE、旧 `at32f403a_407.h` 禁止项 | 防止 CPU class 和旧 SoC header 回退 | 当前 manifest 使用 CPU class；旧 header 禁止断言仍有价值 | `ACTIVE` | 是 | 保留 |
| 630–637 prepared platform F415/F423 | 检查准备中的平台 metadata/timing seam | F415 metadata 存在，但 `generated/inc/tm_platform_foc_timing.h` 不存在；F423 整个平台和 timing seam 均不存在 | F415=`MISMATCHED`；F423=`UNKNOWN` | 是 | RC0 只验收 F415；F423 移到 future prepared-platform contract，不进入当前 gate |
| 661–667 F415/F423 timing header | 检查 platform-owned timer/PWM/loop timing seam | 当前 FOC 时序仍在 `applications/algo/foc_config.h`，使用 `150 MHz`、10 kHz current step、5 kHz velocity、1 kHz position；没有当前 timing header | `MISMATCHED` | 是 | 单独建立 timing migration 任务；不能靠改路径或复制旧 header 解决 |
| 684–786 禁止 `at32f403a_407.h`、公共 northbound header 禁止 vendor header | 保持 vendor header 不越过 BSP port，并阻止旧 SoC header 回退 | 当前架构仍需要这些负向断言 | `ACTIVE` | 是 | 保留 |
| 817–840 `at32f403a/generated/inc/tm_platform_foc_timing.h`、`foc_config.h` include 和 240 MHz 禁止项 | 检查旧平台 timing seam 已迁移、应用层不再拥有 SoC 时序 | 当前 F403A timing 文件不存在，F415 timing 文件也不存在；`foc_config.h` 仍直接拥有 150 MHz/loop 常量 | `MISMATCHED` | 是 | 不做路径替换；另立 F415 timing seam 迁移任务，完成后再恢复该类门禁 |
| 886–895 F403A linker script | 检查 heap/stack reserve 和取消 configure placeholder | 当前 F415 `AT32F415xB_FLASH.ld` 存在，且 `_Min_Heap_Size=0`、`_Min_Stack_Size=0x400` | `MISMATCHED` | 是 | 改为当前 F415 linker path，保留 reserve/placeholder 断言 |
| 898–964 Shell manual、transport governance、diag witness | 检查 USART1 console-first、Shell output、COM54 witness 和诊断策略 | Shell/transport/Mock/Host 契约仍有效；COM54/J-Link 是历史板证据，不是 Host RC0 必要条件 | Shell=`ACTIVE`；COM54/J-Link=`LEGACY` | Shell 是；板证据否 | 保留软件输出断言；将真实板 witness 从 RC0 software gate 分离 |
| 1005–1032 Boot docs 要求 `platform/at32f403a/generated/src/main.c`，并禁止 at32f423 等旧 owner | 检查 live boot owner 与退役任务链 | 当前真实入口是 `platform/at/at32f415/generated/src/main.c`；两个 boot doc 仍写 F403A | `MISMATCHED` | 是 | 更新 boot docs 和 contract 到 F415；保留旧任务链负向断言 |
| 1047–1049 maintenance log 要求 “FalconFoc AT32F403A Themis-lite platform/build alignment” 和 COM54/J-Link/flash witness | 确认历史维护记录存在 | 该条目确实存在于历史维护日志，但它是 2026-05 的历史证据，不是当前平台事实 | `LEGACY` | 否 | 保留历史日志；RC0 当前 gate 改验收当前 AT32F415 软件维护条目 |

### 其他硬编码平台 token

脚本中的 `at32f403a_407.h`、`at32f415.h`、`at32f423.h` 出现在公共头文件的**禁止项**中。这些不应被误判为当前平台路径要求：它们是在验证 vendor/CMSIS 头没有穿过 northbound public contract，属于 `ACTIVE` 负向断言。

## C｜哪些内容仍然必须保留

即使历史 AT32F403A 归属被移出当前 RC0，也不能删除以下质量约束：

- 当前 firmware preset 的 full-diag/standard/tiny 产物、Shell on/off、IPC 禁止和 `.shell_cmd` 断言；
- root CMake 与 `ThemisPlatformMetadata`、`ThemisBuildAxes`、`ThemisContracts`、`ThemisStatusReport` 的 owner 边界；
- CPU class 负责 `mcpu/mfpu/mfloat-abi/FreeRTOS port/ARM_MATH_*` 的契约；
- 当前 AT32F415 metadata 的 MCU、ABI、Flash 几何、startup、linker、CMSIS 和 motor soft IRQ token；
- BSP public header 不暴露 vendor/CMSIS header；
- 旧 AT32F403A header、旧任务 owner、旧 northbound vendor 依赖的负向断言；
- USART1 console-first、Shell 输出、参数存储安全边界和 output lock；
- Firmware CLI smoke 的 Mock OpenOCD 隔离；
- Host、Sanitizer、Runtime replay、ARM build 和 `git diff --check`。

## D｜唯一推荐的最小修复方向

推荐 **方向 C：当前产品与历史 Themis-lite 平台契约分离**，并在分离后对仍有一一对应关系的检查做路径迁移。理由是：

1. AT32F403A 没有当前实现目录、preset 或活动平台 owner；
2. 当前 Legacy 和 V2 都明确使用 AT32F415；
3. 仅将 `at32f403a` 字符串替换为 `at32f415` 会在 timing header、boot docs、`foc_config.h` 和 F423 prepared platform 处继续失败，并可能把 240 MHz 旧事实错误带入 F415；
4. 删除整份 Themis-lite contract 会丢失大量仍然有效的产品、安全、构建和所有权断言。

推荐的最终形态是两个明确的验证面：

### 当前 RC0 FalconFoc contract

只针对当前产品边界：

- AT32F415 platform metadata、startup、linker、Flash geometry、CPU class；
- 当前 boot owner 和应用启动桥；
- 当前 shell/transport/feature/Flash ownership；
- current artifact symbols and negative checks；
- Host/ASan/ARM/CLI/Scope all 质量门禁。

### Historical/Prepared platform audit

单独保存历史或未来平台要求：

- AT32F403A baseline 文档和 Themis-lite 历史 witness；
- AT32F423 prepared-platform 设计和缺失实现；
- platform-owned timing seam 的迁移目标；
- 真实 COM54/J-Link/flash witness。

该 audit 可以失败、记录 drift 或等待平台实现，但不应阻断已经明确选择 AT32F415 的 RC0 软件门禁。

## E｜精确实施计划（本轮不执行）

下一轮获得批准后，最小改动集合应为：

1. 新增 `tests/script/test_falconfoc_current_contract.ps1`：从现有 Themis-lite 脚本提取并保留当前有效的 artifact、ownership、feature、CPU-class、F415 metadata、negative-header、Shell、Flash 和启动链断言。
2. 修改 `scripts/gates/check_themis_quality_gate.ps1`：在 `Scope all` 中调用 current contract；保留旧 Themis-lite 脚本为显式 historical audit，不让它作为当前 RC0 的唯一阻断入口。
3. 更新两个 `docs/boot/*.md`：将 live boot source 从 `platform/at/at32f403a/generated/src/main.c` 对齐到 `platform/at/at32f415/generated/src/main.c`，保留历史路径只作为迁移记录。
4. 在 current contract 中将 platform defaults 和 linker 检查指向 `platform/at/at32f415`，保留 F415 token、Flash geometry、`_Min_Heap_Size=0`、`_Min_Stack_Size=0x400` 等真实断言。
5. 将 maintenance-log selector 从历史 AT32F403A 标题改为一个当前 AT32F415 RC0 maintenance marker；旧日志条目继续保留，不删除历史证据。
6. 将 F423 和 timing seam 检查移入独立 prepared-platform/timing audit；不复制 F403A header，不创建假平台目录。
7. 在实施前单独决定 `foc_config.h` 的时序 owner：如果要继续要求 platform timing seam，必须另开生产迁移任务，按真实 150 MHz/F415 参数建立并验证；不能在本轮 contract 修复中改默认时序。

本计划明确**不修改**：

- FOC 算法、Runtime、Safety、Device、Board PWM/ADC；
- CPU/FPU/ABI、Flash 地址或 linker 参数；
- F415 vendor/generated 文件；
- RelWithDebInfo `NDEBUG` 或 fail-fast 测试语义；
- 真实硬件安全门禁。

## F｜后续实施验收矩阵

获得修复授权后，必须重新证明：

- current contract 独立 PASS；
- historical/prepared audit 的失败与归属可分开观察；
- 反向测试仍能拒绝旧 vendor header、旧 boot owner、Shell/IPС 泄漏和错误 feature ownership；
- Firmware CLI smoke 使用 Mock OpenOCD 通过；
- CMake formatter `29/29 PASS`；
- Host Debug `45/45`；
- Clang ASan/UBSan `28/28`；
- SYSTEM replay `3f01f6e3`；
- Long replay `49dea950`；
- V2 AT32F415 ARM build、资源和主要静态栈帧保持；
- `Scope falconfoc PASS`；
- 正式 `Scope all PASS`；
- Board1 output lock、默认校准值、Feedforward/Trajectory 默认关闭保持不变。

## G｜需要单独批准的变更

以下不是本轮只读审查的默认授权：


1. 修改 `check_themis_quality_gate.ps1` 的 `Scope all` 合约入口；
2. 新增 current contract 并将旧 Themis-lite contract 降为 historical audit；
3. 修改 boot docs 和 maintenance contract 的当前平台口径；
4. 建立 F415 platform timing seam 或移动 `foc_config.h` 时序 owner；
5. 改变 RC0 必需门禁的范围。

这些变化会改变治理边界，必须在实施前单独批准。不能用创建占位 AT32F403A 文件、复制 F415 文件伪装 F403A、注释掉失败检查或直接返回 PASS 的方式解决。

/* ---------------------------------------- end of file ----------------------------------------- */
