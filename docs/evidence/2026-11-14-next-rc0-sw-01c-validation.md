# FalconFOC V2｜NEXT-RC0-SW-01C CMake 格式债务收口与 RC0 软件验收

## 判定

`NEXT-RC0-SW-01C BLOCKED`

本轮已完成授权的 4 个 CMake 文件纯格式修复，动态枚举的 29 个 `Scope all` CMake 文件全部通过格式检查，`Scope falconfoc` 通过，完整 Host/V2/Legacy/Sanitizer/系统回归也通过。

正式 `Scope all` 继续在既有的 `test_firmware_cli_contract.ps1` 阶段失败，原因是当前 Windows 环境没有可用 Bash。没有发现 Git Bash，WSL 也没有已安装发行版；本轮没有安装新依赖、修改门禁脚本或跳过该检查。因此 `Scope all` 仍为真实 BLOCKED，不能报告 `RC0 SOFTWARE GATES PASS`。

本轮没有修改生产算法、Runtime、Safety、Board 功率输出、Protocol、测试语义或 `NDEBUG` 条件；没有 reset、clean、stash、commit、push、刷机、上电、启用 PWM 或写 Flash。

## A｜基线与工作树

| 项目 | 结果 |
| --- | --- |
| 分支 | `codex/falconfoc-v2-rc0` |
| HEAD | `46bc963f5e5849e4440d1714a845fe485d338837` |
| 新鲜构建根 | `build/rc0/software-gate-01c` |
| cmake-format | `cmakelang/cmake-format 0.6.13` |
| 工作树 | 保留此前完整未提交成果；最终状态保存于 `build/rc0/software-gate-01c/git-status-final.txt` |
| `git diff --check` | PASS |

上一轮 `g3/np/CMakeLists.txt` 与 `g3/safe/CMakeLists.txt` 格式修复保持不变，未撤回或重新修改。

## B｜四个授权文件的格式修复

修改前后 token 序列逐文件一致，只发生空格、缩进、换行、括号和参数排版变化。目标名、命令、参数值、条件、宏、链接、源文件和测试注册均未改变。

| 文件 | 修复前 SHA-256 | 修复后 SHA-256 | 语义 token |
| --- | --- | --- | --- |
| `tests/CMakeLists.txt` | `5CB018A25BE52C2A28EE4927D399758F2111DB8ED0F0C89A0DC50C6C97E6E5BE` | `3BA8DF9E73F3DD983C6CA0765F649F2A977FF9F269C79EB7F39D6979A4FC5BAD` | `1737 = 1737` |
| `cmake/ThemisBuildAxes.cmake` | `A24AA2F35249F179888AE718A8C1E5F6FA312E6C4D7D8457B8A6BAF12733FC9D` | `A67F7CF6062EBCCE39B30179AE0543D34CFA0962544F167ADDB22654325DCE85` | `543 = 543` |
| `cmake/ThemisContracts.cmake` | `ABCECC7C7B80EEB96E9724FA3BA63E4FCEF297834F82E0C066489F7BD130A814` | `6986DA3AC92332C52B767C6CF76A78CA52651A418DE2F5CA2981B1B28D15FE49` | `1176 = 1176` |
| `platform/at/at32f415/ThemisPlatformDefaults.cmake` | `5011C241E70346B5F44261DC52479338AE84B70A7F96D90C8520196D41818922` | `AC86F0808DBFC6E67A656231170610AF4F1424A57E54B18AB011E1654AF3CF25` | `58 = 58` |

四个文件的单文件 formatter 检查均为 `exit=0`，`git diff --check` 为 `exit=0`。

## C｜全量 CMake 扫描

扫描器从当前仓库的 `scripts/gates/check_cmake_quality.ps1` 动态读取 `relativePaths`，并递归加入当前平台默认和 CPU 默认 CMake 文件，而不是硬编码旧的文件列表。

```text
cmake-format version: 0.6.13
scanned: 29
passed:  29
failed:  0
exit:    0
```

完整原始记录：

`build/rc0/software-gate-01c/cmake-format-scan-after.log`

## D｜正式质量门禁

| 门禁 | 结果 |
| --- | --- |
| Source contract | PASS，`61 C/H、8 CMake、8 README` |
| C/H formatter | PASS |
| CMake formatter | PASS，changed/untracked 与全量扫描均通过 |
| `Scope falconfoc` | PASS |
| `git diff --check` | PASS |
| `Scope all` | **BLOCKED**，`test_firmware_cli_contract.ps1` 缺少 Bash |

正式 `Scope all` 的原始失败：

```text
Bash is required to execute the firmware CLI contract smoke.
At tests/script/test_firmware_cli_contract.ps1:117
```

环境核查结果：没有 `bash` 命令、没有常见 Git Bash 路径，WSL 只有系统组件且没有已安装发行版。没有改变门禁定义，也没有用弱化脚本替代正式 `Scope all`。

## E｜新鲜软件回归

### Host Debug 与组件

使用 MinGW GCC/G++ `16.2.0`，全部在 `build/rc0/software-gate-01c` 重新配置、编译和运行：

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

使用 Clang `22.1.8` 和：

```text
-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
```

重新覆盖全部 Host V2 C 模块：

```text
Core 10/10
Sensors 1/1
Calibration 1/1
G3-NP 2/2
G3-SAFE 1/1
Runtime 8/8
Protocol 1/1
Board 4/4
总计 28/28 PASS
```

没有 ASan/UBSan 诊断。

## F｜RelWithDebInfo 契约

本轮重新构建并运行了 RelWithDebInfo：

```text
Host RelWithDebInfo build: PASS
Host RelWithDebInfo CTest: 44/45
```

唯一失败仍是 `test_tm_task_budget_probe_failfast`。它由 `WILL_FAIL TRUE` 和 `WRAP_SYSTEM_FAILURE` 注册，依赖 Debug 断言产生非零退出；`cmake/ThemisContracts.cmake` 对 RelWithDebInfo 注入 `NDEBUG`，所以断言不触发、进程正常退出，CTest 将预期失败报告为失败。

本轮没有修改测试、没有删除 `NDEBUG`、没有关闭 fail-fast。当前仓库正式 Host 验收入口仍是 `host-debug -> build-host-debug -> test-host-debug`，因此保留该限制，不把 `44/45` 伪报为 PASS。

## G｜SYSTEM-01 确定性回放

新鲜 Sanitizer Runtime 可执行文件输出：

```text
system replay hash=3f01f6e3 updates=96 h0_due=103 h1_due=41 outer_seq=40 session=3
long replay hash=49dea950 updates=99998 due=100001 missed=0 late=0 budget=0
```

Trajectory、Velocity PI、电角速度前馈、Current PI、SVPWM、Device、Safety、Host Mock、UPDATE 拒绝、WDT Fault Latch、INHIBIT 和会话隔离测试均保持通过，两个指纹与前轮一致。

## H｜AT32F415 ARM 与 Legacy 产物

ARM 工具链为 Arm GNU Toolchain `14.3.1`，目标仍为 Cortex-M4 项目配置、soft-float ABI、FPU none。

```text
.text   34916 B
.rodata 1352 B
.data   84 B
.bss    2520 B
Flash   36736 B
RAM     3632 B
```

主要静态栈帧：

```text
foc_current_step_internal  184 B
foc_pipeline_step_guarded 312 B
foc_runtime_control       224 B
foc_runtime_h1_step       288 B
foc_runtime_h0_step        96 B
foc_board_output_submit    96 B
foc_encoder_update          72 B
```

V2 产物 SHA-256：

```text
falconfoc-v2-at32f415.bin  9806ADBFA3282450527F40BB97CB1DA75FAFE41FB405F1126114187C6393B029
falconfoc-v2-at32f415.elf  51D7428FF2414FA4623DCE5086A6CD8D4F69058DFC6405B317DC28875E281551
falconfoc-v2-at32f415.hex  B80C263E6B07CE976E2BF1AEDED2737EED536C7224369091DC0919CAA48C51AE
falconfoc-v2-at32f415.map  BDC648824C427C873259638A30EB2FAE0C59E1598EBEA3C2C43E0C29CF6516B3
```

Legacy Bench 重新构建通过，链接布局保持：`.text=0x12810`、`.rodata=0x3d08`、`.data=0x80`、`.bss=0x6780`，Flash `92144 B`、RAM `27 KB`。Legacy ELF/BIN/HEX 指纹因 `__DATE__`/`__TIME__` 在跨日构建时变化，不能作为 CMake 语义变化证据；代码 section 大小和布局未变。

## I｜安全边界复核

Board Host 配置测试继续通过，确认：

- 默认 ADC offset、极对数、电角度 offset 未改变；
- 校准候选仍需显式准入；
- Feedforward 和 Trajectory 默认关闭；
- Board1 `UPDATE` 仍不会应用到真实功率级；
- 没有新增 PWM 解锁路径；
- G3-SAFE 硬件门禁仍未通过；
- G3-PWR 仍锁定。

本轮没有真实 MCU WCET、运行栈高水位、ADC/PWM 采样窗口、功率级响应或实机电机证据。

## J｜最终状态

当前正式阻断：

1. `Scope all` 的 Firmware CLI contract 需要 Bash，而当前环境没有 Bash 或已安装 WSL 发行版；
2. RelWithDebInfo 的 `test_tm_task_budget_probe_failfast` 保持真实 `44/45`，按当前正式 RC0 Host Debug 契约记录为已知配置限制，不伪报 PASS。

因此保持：

- `NEXT-RC0-SW-01C BLOCKED`；
- `RC0 SOFTWARE BLOCKED`；
- `G3-SAFE HARDWARE NOT VERIFIED`；
- `G3-PWR LOCKED`；
- `AT32F415 WCET NOT VERIFIED`。

/* ---------------------------------------- end of file ----------------------------------------- */
