# FalconFOC V2｜NEXT-RC0-SW-01B 剩余 CMake 门禁修复与 RC0 软件复验

## 判定

`NEXT-RC0-SW-01B BLOCKED`

本轮按授权只格式化了 `g3/safe/CMakeLists.txt`。单文件格式、G3-SAFE 构建、完整 V2 Host/Sanitizer/Board/Runtime 回归、SYSTEM-01 回放和 V2 ARM 构建均通过。

全仓 CMake 只读扫描随后确认仍有 4 个未授权的历史格式失败文件，因此 `Scope all` 仍未通过。当前不能报告 `RC0 SOFTWARE GATES PASS`。

本轮没有修改生产控制算法、Runtime、Safety、Board 功率输出、Protocol 或测试预期；没有 reset、clean、stash、commit、push、刷机、上电、启用 PWM 或写 Flash。

## A｜工作树与边界

| 项目 | 结果 |
| --- | --- |
| 分支 | `codex/falconfoc-v2-rc0` |
| HEAD | `46bc963f5e5849e4440d1714a845fe485d338837` |
| 工作树 | 保留此前全部已修改和未跟踪成果 |
| 已保留的前轮修复 | `g3/np/CMakeLists.txt` 格式修复仍在 |
| 本轮允许修改 | 仅 `g3/safe/CMakeLists.txt` 的 cmake-format 排版 |
| formatter | `cmakelang/cmake-format 0.6.13` |

## B｜g3/safe 最小格式修复

修复前后只发生括号换行、参数换行、缩进和排版变化。目标名、源文件、编译选项、链接关系、构建条件和测试注册没有改变。

| 文件 | 修复前 SHA-256 | 修复后 SHA-256 |
| --- | --- | --- |
| `g3/safe/CMakeLists.txt` | `B01FA78240008E602C734B765BA1CB47D57BD69BA51F2FFB06CAA93971D32757` | `A8300D5610B241E83E8CAEF3B65A6F2784CF0E29BA9F5C645CD5D98874A6A603` |

验证结果：

- changed/untracked CMake formatter：PASS；
- G3-SAFE 独立 Debug 构建：PASS；
- G3-SAFE CTest：`1/1 PASS`；
- `git diff --check`：PASS。

## C｜Scope all 全量 CMake 扫描

使用仓库现有 `.cmake-format` 配置，对 `Scope all` 纳入的 29 个实际存在的 CMake 文件逐一执行 `cmake-format --check`，而不是在首个失败处停止。

### 修复前

失败文件共 5 个：

1. `g3/safe/CMakeLists.txt`
2. `tests/CMakeLists.txt`
3. `cmake/ThemisBuildAxes.cmake`
4. `cmake/ThemisContracts.cmake`
5. `platform/at/at32f415/ThemisPlatformDefaults.cmake`

原始扫描记录：

`build/rc0/software-gate-01b/cmake-format-scan-before-preserved.log`

### 修复后

`g3/safe/CMakeLists.txt` 已通过，剩余失败文件共 4 个：

1. `tests/CMakeLists.txt`
2. `cmake/ThemisBuildAxes.cmake`
3. `cmake/ThemisContracts.cmake`
4. `platform/at/at32f415/ThemisPlatformDefaults.cmake`

完整扫描记录：

`build/rc0/software-gate-01b/cmake-format-scan-after.log`

这些文件不在本轮授权范围内，没有自动修改。正式 `Scope all` 当前在按路径顺序检查到 `tests/CMakeLists.txt` 时停止，原始门禁失败为：

```text
cmake-format --check failed: D:\study\Software\Hollow-Gimbal\tests\CMakeLists.txt
```

## D｜质量门禁结果

| 门禁 | 结果 |
| --- | --- |
| Source contract | PASS，`61 C/H、8 CMake、8 README` |
| C/H formatter | PASS |
| changed/untracked CMake formatter | PASS，含 `g3/safe` |
| `Scope falconfoc` | PASS |
| `Scope all` | **BLOCKED**，4 个历史 CMake 格式文件 |
| `git diff --check` | PASS |

`Scope falconfoc` 与 `Scope all` 均实际执行过。没有把专项门禁 PASS 替代全仓门禁 PASS。

## E｜软件回归矩阵

新鲜构建根：`build/rc0/software-gate-01b`

### Host Debug / 组件

使用 MinGW GCC/G++ `16.2.0`：

| 构建 | CTest |
| --- | --- |
| Host Debug | `45/45 PASS` |
| Core | `10/10 PASS` |
| Sensors | `1/1 PASS` |
| Calibration | `1/1 PASS` |
| G3-NP | `2/2 PASS` |
| G3-SAFE | `1/1 PASS` |
| Runtime/Pipeline | `8/8 PASS` |
| Protocol | `1/1 PASS` |
| Board/UART/Python | `4/4 PASS` |

### Clang ASan/UBSan

使用 Clang `22.1.8` 与：

```text
-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
```

所有可在 Host 执行的 V2 C 模块均重新配置、编译和运行：

```text
Core 10/10
Sensors 1/1
Calibration 1/1
G3-NP 2/2
G3-SAFE 1/1
Runtime 8/8
Protocol 1/1
Board 4/4
合计 28/28 PASS
```

没有 ASan/UBSan 诊断。

### SYSTEM-01 重放

```text
system replay hash=3f01f6e3 updates=96 h0_due=103 h1_due=41 outer_seq=40 session=3
long replay hash=49dea950 updates=99998 due=100001 missed=0 late=0 budget=0
```

两个指纹和前一轮一致。

## F｜RelWithDebInfo 44/45 调查

本轮没有修改测试。调查结果如下：

- `tests/CMakeLists.txt` 将 `test_tm_task_budget_probe_failfast` 注册为 `WILL_FAIL TRUE`，并启用 `WRAP_SYSTEM_FAILURE`；
- 测试源会注册超过 `TM_TASK_BUDGET_PROBE_MAX_TASKS` 的任务，预期触发 fail-fast；
- `cmake/ThemisContracts.cmake` 对 `RelWithDebInfo` 明确注入 `NDEBUG`；
- 因此 RelWithDebInfo 中标准 `assert` 不触发，测试返回 0，CTest 会把“预期失败”报告为失败；
- Debug 配置中断言触发，现有 Host Debug 仍为 `45/45 PASS`。

当前仓库正式 Host 验收入口在规范中明确为：

```text
host-debug -> build-host-debug -> test-host-debug
```

`host-relwithdebinfo` 在 preset contract 中要求存在并可配置，但当前正式 RC0 验收记录没有把 RelWithDebInfo 的 `test-host-relwithdebinfo` 作为 45/45 产品门禁。故本轮如实记录：

```text
Host RelWithDebInfo build: PASS
Host RelWithDebInfo CTest: 44/45
```

这不是本轮修改造成的代码失败，也没有将其写成 `45/45 PASS`。如果未来把所有构建配置的 CTest 全通过提升为 RC0 必需条件，应另立验收决策并单独处理该 Debug-only fail-fast 契约。

## G｜ARM 与 Legacy 构建

### V2 AT32F415

ARM 构建使用 Arm GNU Toolchain `14.3.1`，Cortex-M4 项目配置、soft-float ABI、FPU none。与前一轮 V2 产物一致：

```text
.text   34916 B
.rodata 1352 B
.data   84 B
.bss    2520 B
Flash   36736 B
RAM     3632 B
```

关键静态栈帧：

```text
foc_current_step_internal  184 B
foc_pipeline_step_guarded 312 B
foc_runtime_control       224 B
foc_runtime_h0_step        96 B
foc_runtime_h1_step       288 B
foc_board_output_submit    96 B
foc_encoder_update          72 B
```

ARM artifact SHA-256 与前一轮相同：

```text
falconfoc-v2-at32f415.bin  9806ADBFA3282450527F40BB97CB1DA75FAFE41FB405F1126114187C6393B029
falconfoc-v2-at32f415.elf  51D7428FF2414FA4623DCE5086A6CD8D4F69058DFC6405B317DC28875E281551
falconfoc-v2-at32f415.hex  B80C263E6B07CE976E2BF1AEDED2737EED536C7224369091DC0919CAA48C51AE
falconfoc-v2-at32f415.map  BDC648824C427C873259638A30EB2FAE0C59E1598EBEA3C2C43E0C29CF6516B3
```

### Legacy Bench

Legacy Bench fresh build通过，`.text/.rodata/.data/.bss` 地址和大小与前一轮一致：`.text=0x12810`、`.rodata=0x3d08`、`.data=0x80`、`.bss=0x6780`，Flash `92144 B`、RAM `27 KB`。

ELF/BIN/HEX 的字节指纹发生变化，原因是 Legacy Shell banner 使用 `__DATE__` 与 `__TIME__`：前一轮包含 `Nov 13 2026 23:38:40`，本轮包含 `Nov 14 2026 00:03:26`。这改变了构建时间字符串和调试字符串，不是 `g3/safe` CMake 排版改变了控制逻辑；代码 section 大小和链接布局保持一致。

本轮没有真实 MCU WCET、栈高水位、ADC/PWM 采样窗口或功率级响应证据。

## H｜最终状态与后续准入

本轮应保持：

- `NEXT-RC0-SW-01B BLOCKED`；
- `RC0 SOFTWARE BLOCKED`；
- `G3-SAFE HARDWARE NOT VERIFIED`；
- `G3-PWR LOCKED`；
- `AT32F415 WCET NOT VERIFIED`。

要关闭本轮唯一软件门禁阻断，需要单独授权修复以下 4 个 CMake 文件，或由项目负责人明确接受这些全仓格式债务并改变 `Scope all` 的验收规则：

- `tests/CMakeLists.txt`；
- `cmake/ThemisBuildAxes.cmake`；
- `cmake/ThemisContracts.cmake`；
- `platform/at/at32f415/ThemisPlatformDefaults.cmake`。

在该决策前，不扩大修改范围。

/* ---------------------------------------- end of file ----------------------------------------- */
