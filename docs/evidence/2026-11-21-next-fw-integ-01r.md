# NEXT-FW-INTEG-01R｜Board1 校准启动集成修复与完整验收

**NEXT-FW-INTEG-01R INTEGRATION ACCEPTANCE PASS**

**NEXT-FW-INTEG-01 SOFTWARE ACCEPTANCE CLOSED**

**PHYSICAL OUTPUT NOT VERIFIED** · **TARGET MCU WCET NOT VERIFIED** · **PHYSICAL CALIBRATION NOT VERIFIED** · **G3-PWR LOCKED**

日期 2026-11-21。本轮修复 NEXT-FW-INTEG-01 评审复现的四项缺陷：Capture 端到端身份、供应 ID 截断、review_id 语义、复现脚本失败传播。物理输出锁定保持不变。全部证据属于软件与静态验证：Host 构建与测试、离线 ARM 构建与反汇编，无板卡操作。

[原始证据索引](2026-11-21-next-fw-integ-01r/README.md) · [原始证据 ZIP](2026-11-21-next-fw-integ-01r/raw-evidence.zip) · [SHA-256](2026-11-21-next-fw-integ-01r/raw-evidence.sha256)

## 0. 基线冻结

| 项 | 实测 |
| --- | --- |
| 目录 / 分支 | `D:/study/Software/Hollow-Gimbal-vm01a` / `codex/falconfoc-v2-vm01a` |
| HEAD | `aaad463a2cc9cc4fa3f73f85f46f997550811360`，与评审基线一致，分支未前移 |
| 索引 / 工作树 / 未跟踪 | 开始时为空 / 干净 / 无 |
| 历史证据 | `docs/evidence/2026-11-21-next-fw-integ-01.md` 及其 ZIP 不作修改，保留为历史证据 |
| 修复前复现源码 | `git archive aaad463` 导出到 `build/fwinteg01r/base-src`，在该树上构建 Host 库运行复现 |

范围：改动限于 Board1 采集与启动资格、Host 导入与 Locked Timing、复现脚本及测试、文档与证据。Legacy 生产源码、FOC Core、H0/H1 控制律、Safety 状态机、校准算法、FCAL v1 和功率输出策略零差异（见第 6 节）。未开启 FW-ADC-01，未连接板卡、刷写、写 MCU Flash、励磁或解锁 PWM。

## 1. A｜SRAM Capture 身份契约

**根因。** Capture V1 头部 `encoder_direction`/`pole_pairs` 原义是固定的 Board1 原始配置（−1/7）：`board_capture.py` 把它锁定为常量，并在 ADC_ZERO Trace 中把头部方向当作 `raw_direction` 写入清单。NEXT-FW-INTEG-01 让采集写入生效 Runtime 值，在同一字段上悄悄改变了含义。

**修复前复现（aaad463）。** 正式 `foc_board_boot_select()` → `foc_board_boot_capture_profile()` → 生产 C Writer 封存完整 5480 B 镜像（Host 合成行，`foc_board_capture_boot_fixture.c`）。标称镜像 ID 33（−1/7）导入退出 0；校准镜像 ID 34（+1/11，CRC 正确）导入退出 2，`RAW_IDENTITY`；`locked_timing.py --capture` 同样标称 0、校准 2。

**最小修改。**

| 信息 | 声明方 | 核对方 |
| --- | --- | --- |
| 板级硬件身份：`board_profile=1`、rank/通道、CPR 65536、原始方向 −1、相序 ABC | Writer 常量 | 导入器冻结 Board1 常量，不从镜像反读 |
| 配置区分符：V1 头部 `encoder_profile`（原恒为 1） | Writer，取自启动报告：1 标称，2 校准；其他值拒绝启动采集 | 导入器只接受 1 或 2，并须与外部声明一致 |
| 标称（1）：头部 −1/7，V1 原义 | Writer | 无声明时用冻结标称核对，旧 V1 样本和元数据格式 1 照常读入 |
| 校准（2）：头部为 Runtime 原始码→机械方向与 Runtime 极对数 | Writer，取自生效 Runtime/Encoder | 元数据格式 2 的 `configuration` 外部声明：源方向须为 Board −1，Runtime 方向须等于源×校准方向，极对数，坐标只接受 `RAW_NO_LUT`，非零 supply/review 引用；头部须与声明一致 |
| 采集来源 | `producer` 与元数据 `source` | 须一致；Host 夹具保持 HOST，Board 来源保持 `PROVENANCE_UNVERIFIED` |
| 完整性 | CRC32 | 只证明字节未变；身份另行核对 |

V1 布局、128 B 头、5480 B 镜像和既有字段含义不变；没有采用破坏性格式变更。电角 Offset 不进入头部：当前唯一消费者 ADC_ZERO Trace 不需要它；Trace 清单的 `raw_direction` 改为恒取 Board 原始方向 −1。`supply_id`/`review_id` 在声明中只作引用。源码：`firmware/at32f415/include/foc_board_capture.h`、`src/foc_board_capture.c`、`src/foc_board.c`、`src/foc_board_boot.c`、`tools/falconfoc-calibration-replay/board_capture.py`、`locked_timing.py`，合同见 `firmware/at32f415/BOARD_CAPTURE.md#configuration-identity`。

**同条件验证（修复后，同一夹具、同一命令，外加声明）。**

| 输入 | 导入 | Locked Timing |
| --- | --- | --- |
| 标称镜像 + 旧元数据格式 1 | 0，`FROZEN_NOMINAL` | 0 |
| 校准镜像 + 声明 | 0，`DECLARED_CONFIGURATION` | 0，capture 34 完整 |
| 校准镜像无声明 | 2，`CONFIGURATION_REQUIRED` | 2 |
| 修复前写出的校准镜像（profile 1 却带 +1/11），无声明 | 2，`RAW_IDENTITY` | 2 |
| 同上，带校准声明 | 2，`CONFIGURATION_MISMATCH` | 2 |

修复后标称镜像与修复前 Writer 产物逐字节相同（SHA-256 `bcb1b53c…`），旧 V1 兼容由此成立；校准镜像只在 `encoder_profile`（1→2）和 CRC 上不同。

**端到端测试 `test-foc-capture-boot-import`（8 个用例，CTest 注册，三工具链运行）。** 两种真实启动选择实例经生产 Writer、生产导入、`locked_timing.py` 正常入口读通；校准 Trace 清单为 `raw_direction −1 / pole_pairs 11`。负例：无声明（导入、describe、Locked Timing 均 `CONFIGURATION_REQUIRED`）；标称错贴校准、校准错贴标称（导入与 Locked Timing 均 `CONFIGURATION_MISMATCH`）；声明极对数 12、方向 −1、源方向 +1、坐标 UNKNOWN/LUT_CORRECTED、乘积不自洽、review/supply 为 0、校准方向 0；**重新计算 CRC 后**改极对数 12、方向 −1、CPR 4096、相序 1、`encoder_profile` 3 均 `RAW_IDENTITY`，改 `encoder_profile` 1 为 `CONFIGURATION_MISMATCH`，标称镜像改成 +1/11 为 `RAW_IDENTITY`；未重算 CRC 为 `CRC_MISMATCH`；版本 2 `RAW_VERSION`；截断 `RAW_SIZE`；元数据 Capture ID 不符与混合采集行 `CAPTURE_ID_MISMATCH`；来源冲突（导入与 Locked Timing）`SOURCE_MISMATCH`；格式 1 携带声明 `METADATA_INVALID`。既有 `test-foc-capture-import`（旧 V1 夹具）与 `test-foc-board-capture`（新增未声明区分符 0/3 拒绝启动）保持通过。

## 2. B｜供应 ID 截断

**修复前复现。** `0x156464331` 配置 0、构建 0、启动测试 0（被窄化为 `0x56464331` 后接纳）；`0x100000000` 配置与构建 0（窄化为 0）。

**最小修改。** `firmware/at32f415/CMakeLists.txt` 在任何 C 转换前对原文本去掉 `0x` 与前导零后限定 1–8 位十六进制（非零 uint32），极对数限定 1–64；`foc_board_expectation.c` 以 `ULL` 字面量接收，对未窄化值静态断言 `≤ 0xFFFFFFFF`，验证供应要求非零，NONE 构建要求为 0（保留零值哨兵）。

**同条件验证。**

| 输入 | 修复前 配置/构建/测试 | 修复后 |
| --- | --- | --- |
| `0x56464331` | 0/0/0 | 0/0/0，接纳 |
| `0x0056464331`（前导零） | — | 0/0/0 |
| `0x156464331` | 0/0/0（别名接纳） | 配置 1 |
| `0x100000000` | 0/0（窄化为 0） | 配置 1 |
| `0xFFFFFFFF` | 0/0 | 0/0，探针：期望 `0xffffffff` ≠ 供应 → NOMINAL + REJECTED，原因 2 |
| `0x0` | 0/0 | 配置 1 |
| 空值、`0xZZ`、十进制、极对数 0、65、`011` | 空/非十六进制/0 为配置 1；65 编译失败 | 全部配置 1 |
| 绕过 CMake 直接编译 `0x156464331ULL`、`0x100000000ULL`、验证构建 `0ULL`、NONE 构建非零 | — | 均编译退出 1，对应静态断言消息 |

## 3. C｜review_id 语义

**修复前复现。** 验证供应复制后置 `review_id = 0`，`foc_board_boot_qualify` 返回 NONE，启动选择得到 CALIBRATED + ACCEPTED。

**最小修改。** `foc_board_boot_qualify` 在供应身份之后要求 `review_id != 0`，新增拒绝原因 `FOC_BOARD_REJECTION_REVIEW_REFERENCE = 10`（追加，既有 0–9 与 UART 字段、帧不变）。准入重算与快照比较保留。`review_id` 只是审核材料引用；独立身份匹配只覆盖 `supply_id` 与期望极对数。头文件、`protocol/README.md`（ID 34/37 说明）、固件 README 明确它不被匹配、不做签名验证、不证明物理来源。NEXT-FW-INTEG-01 报告第 3 节"供应 / 审核身份 … 非零且等于 CMake 期望"的表述对审核 ID 不成立，以本节为准。

**同条件验证。** 同一程序修复后：拒绝原因 10，NOMINAL + REJECTED。`test-foc-board-boot` 新增：零审核 ID → 原因 10 且 UART 读回 10；供应 ID 错而审核 ID 有效 → SUPPLY_IDENTITY；审核 ID 有效但 Rotor 残差 0.5 → ADMISSION/ROTOR_MAPPING；审核 ID 有效但快照电角 +0.01 → SNAPSHOT_MISMATCH；另一个非零审核 ID 仍被接纳（证明不匹配审核 ID）；UART ID 37 标称读回 0、接纳后读回 `0x01210001`。

## 4. D｜复现脚本失败传播

**修复前复现。** 证据 ZIP 中 `scripts/run-final.sh`（SHA-256 `3fe07732…`，与清单一致）在 `pwsh`/`cmake`/`python` 全部立即失败时，25 个子步骤记录 exit 1，顶层退出 0。其 skip-check 只比较发现数与通过数，用例被删除时两者同时下降不会报错。

**最小修改。** 新的受控入口 `scripts/flows/fwinteg/`（跟踪文件，替代忽略目录中的旧脚本）：

| 文件 | 作用 |
| --- | --- |
| `run_fwinteg.sh` | 从仓库根运行；输出目录必须不存在；每步记录原始退出码、从不中断；顶层退出码等于 verdict 的退出码 |
| `run_matrix.ps1` | 单工具链矩阵；每个套件都运行，逐套件记录 configure/build/ctest 原始退出码与日志结束标记 |
| `expected_tests.tsv` | 冻结用例清单（123 项，三工具链相同），逐名核对 |
| `fwinteg_verdict.py` | 分类判定：步骤、各工具链清单（缺失、新增、重复、失败、跳过、未执行、日志截断）、见证、ARM、静态、UART、Strict Scope all、Legacy Host、故障注入 |
| `arm_static.py` | 五个 ARM 镜像各与同选项的 211b226 基线比较 |
| `uart_check.py` | 真实 CLI 对两个模拟器的读回与锁定策略 |
| `fwinteg_stub.py` / `test_fault_injection.py` | `--stub` 时以同格式合成产物，`FWINTEG_INJECT` 注入故障，从顶层进程验证汇总逻辑 |

依赖：CMake ≥3.25、Ninja、Clang（含 ASan/UBSan 运行库）、MinGW GCC、Arm GNU Toolchain、Python 3、pwsh、git、cmakelang。命令：`bash scripts/flows/fwinteg/run_fwinteg.sh <新输出目录>`。基线 ARM 镜像与 UART 基线由脚本自行从 `git archive 211b226` 构建，不依赖忽略目录。

**同条件验证（顶层故障注入，19 例）。**

| 用例 | 顶层退出码 | 首个发现 |
| --- | --- | --- |
| control | 0 | 无（全部 27 步完成且为 0） |
| clang-fail | 1 | steps: matrix-clang exit 1 |
| gcc-fail | 1 | steps: matrix-gcc exit 1 |
| sanitizer-fail | 1 | steps: matrix-sanitizer exit 1 |
| partial-run | 1 | matrix-gcc: suite tools-falconfoc-calibration-replay configure/build/ctest = not recorded |
| not-run | 1 | matrix-sanitizer: runtime/test-foc-control01b Not Run |
| skipped | 1 | matrix-clang: sensors/test-foc-encoder Skipped |
| removed-test | 1 | matrix-clang: calibration/test-foc-bootstrap expected but not reported |
| duplicate-test | 1 | matrix-gcc: protocol/test-foc-protocol reported 2 times |
| unexpected-test | 1 | matrix-clang: core/test-foc-not-in-inventory not in the expected inventory |
| truncated-log | 1 | matrix-sanitizer: suite simulation log truncated |
| missing-log | 1 | matrix-clang: suite protocol log missing |
| arm-build-fail | 1 | steps: arm-verification-capture-build exit 1 |
| static-fail | 1 | arm-static: lacks 'ALL STATIC CHECKS PASS' |
| uart-fail | 1 | steps: uart-check exit 1 |
| gate-fail | 1 | quality-gate: lacks '[quality_gate] all requested checks passed.' |
| legacy-fail | 1 | legacy-host: lacks '100% tests passed out of 45' |
| empty-step | 1 | steps: uart-check exit 127 |
| skip-step | 1 | steps: required step uart-check did not run |

汇总：`FAULT INJECTION PASS`。负例全部非零，全绿对照组在全部必需步骤完成后才为 0。`static-fail` 与 `gate-fail` 让步骤谎报退出 0，仅靠内容检查捕获；`removed-test` 让发现数与通过数同时下降，由逐名清单捕获；`skip-step` 让必需步骤完全不运行。该表来自 `run2` 内的 `fault-injection` 步骤，与桩模式预演结果一致。

首次真实运行 `run1` 证明新入口会在真实问题上失败：矩阵步骤日志为空（`run_matrix.ps1` 未写 stdout）、Capture/追踪镜像与非同选项基线比较导致前导不一致，顶层退出 1。修复为逐套件 stdout 摘要和同选项基线（211b226 的 Capture/追踪构建与本轮镜像前导 75 条指令一致）；唯一的符号移除 `foc_board_capture_default_profile` 来自 NEXT-FW-INTEG-01 的采集元数据修复，作为有理由的已知移除写入检查。verdict 中类别前缀误判（"arm" 命中 "arm-static"）改为精确匹配。

## 5. 全量回归（`run2`）

`bash scripts/flows/fwinteg/run_fwinteg.sh build/fwinteg01r/run2`，全新输出目录，顶层退出 **0**，`verdict.json` 0 项发现，27 个步骤原始退出码全部为 0。

| 类别 | 结果 |
| --- | --- |
| Clang Debug | 期望 123，发现 123，通过 123，失败/跳过/未执行/缺失/新增/重复均 0；11 个套件 configure/build/ctest 均 0 |
| MinGW GCC Debug | 同上 123/123 |
| Clang ASan+UBSan（`halt_on_error=1`） | 同上 123/123 |
| 套件分项 | Core 21、Runtime 13、Calibration 10、Control Plane 5（独立计数）、Sensors 2、Simulation 50、Servo 7、Protocol 1、Board1/UART/Capture Import/Locked Timing 12、Calibration Workflow 1、Replay 1 |
| 相对前阶段 122 的增量 | +1：`firmware-at32f415/test-foc-capture-boot-import`（第 1 节）。其余 122 个名称与前阶段逐名相同 |
| 启动测试见证（三工具链，同次运行） | 接纳供应、估计器审核链、反向验证、两种启动 FAULT_CLEAR 由 H0 应用、锁定 RUNNING 不可持续、默认启动 NOMINAL + NOT_AVAILABLE |
| Legacy 三组预设 | full-diag Debug、standard/tiny MinSizeRel 构建 0 |
| Strict Scope all（`-RequireCMakeFormatter -Summary`） | `[Total] PASS`；Source Contract C/H 209；cmake-format 强制检查通过 |
| Legacy Host | 45/45 |
| UART（真实 CLI） | 默认 demo 与 211b226 基线逐字段相同；两模拟器 ID 16–17、32–46 期望值、只读拒绝、ARM/START/SAVE/STOP 全部符合 |
| 故障注入 | 19 例正确（第 4 节） |

ARM 镜像（Cortex-M4 / soft-float / FPU none，构建与静态证据）：

| 镜像 | BIN SHA-256 | Flash / RAM |
| --- | --- | --- |
| 默认锁定 | `c03591eaeda8a3fd5752b2a4e453b94d13618fe6351388621e65b476eec2375e` | 48,324 B / 4,280 B |
| 合成校准 | `bfc890fdad3b763fa2b5957faac87281c5fb116d3b4feeec051419f8e8ce6509` | 48,604 B / 4,280 B |
| 默认 + Capture | `c83dfab646f03ff3f61a2fdd698afb099ad92cdf1d3da0b71ba4bc1ba12f0d6d` | 49,592 B / 9,792 B |
| 合成校准 + Capture | `ecdae3d44b28626851e38254347f6c975488e800b4b38d15cbea384a99d62f04` | 49,872 B / 9,792 B |
| 默认 + RT-02 追踪（附加） | `f73549065bb3506735a551da208ffabffec836fe3fe7b8e89f19e59a3968dd47` | 49,444 B / 4,696 B |

相对 `aaad463`：默认镜像 Flash +12 B（review_id 检查），RAM 不变；静态栈 `foc_board_boot_qualify` 160 B、`foc_board_boot_select_with` 96 B、`main` 64 B 不变，Capture 构建的 `foc_board_capture_start` 48 → 56 B（区分符校验）。

## 6. 安全边界与源码范围

`arm_static.py` 在 `run2` 内对五个镜像逐项检查，每个镜像与同选项的 211b226 基线比较（`static/summary.txt`）：

| 检查 | 默认 | 合成校准 | 默认 + Capture | 校准 + Capture | 追踪 |
| --- | --- | --- | --- | --- | --- |
| `main` 锁定前导（TMR1 复位、六脚 analog）与基线相同 | PASS | PASS | PASS | PASS | PASS |
| 启动选择位于时钟之后、ADC 初始化之前，失败跳 `wfi` | PASS | PASS | PASS | PASS | PASS |
| Board 适配器 UPDATE 拒绝代码与基线相同（仅字面量地址） | PASS | PASS | PASS | PASS | PASS |
| 无 Flash 擦写符号 | PASS | PASS | PASS | PASS | PASS |
| 合成供应只在校准镜像 | PASS | PASS | PASS | PASS | PASS |
| 校准初始化可达 | PASS | PASS | PASS | PASS | PASS |
| 无未解释的符号移除 | PASS | PASS | PASS（`foc_board_capture_default_profile`，已解释） | PASS（同左） | PASS |

运行行为（Host，生产实现）：ARM/START = OUTPUT_LOCKED、PARAM_SAVE = NOT_SUPPORTED（两种启动与 CLI）；STOP 由 H0 消费、FAULT_CLEAR 由 H0 应用且输出保持软件 INHIBIT；正常启动链有效 UPDATE 为 0，适配器直接收到合法 UPDATE 仍拒绝；以上由 `test-foc-board-boot` 在三工具链执行。

源码范围：`git diff --stat` 共 17 个跟踪文件改动，另有新文件 `foc_board_capture_boot_fixture.c`、`test_board_capture_boot.py`、`scripts/flows/fwinteg/*` 与本报告。`applications/`、`bsp/`、`platform/`、`subsys/`、`components/`、`kernel/`、`calibration/`、`core/`、`runtime/`、`g3/`、`sensors/`、`control_plane/`、`simulation/`、`examples/` 与协议源码零差异；FCAL v1、Safety 状态机、H0/H1 控制律与功率输出策略未改。

## 7. 未验收项

- 无板卡操作；物理输出、物理校准、目标 MCU WCET 与栈高水位未验证。
- 合成供应与 Host 夹具行是测试数据，不代表任何实物参数或实测采集。
- 校准采集的外部声明由调用方提供；导入器核对其自洽与镜像一致，不能证明声明本身的物理来源。
- `review_id` 只是引用，没有独立审核期望或签名。

## 8. 复现

```powershell
bash scripts/flows/fwinteg/run_fwinteg.sh build/fwinteg01r/run2
```

任意新输出目录均可；目录已存在时脚本拒绝运行。修复前复现命令与输出在原始证据 `repro-before/`，修复后同条件验证在 `verify-after/`。
