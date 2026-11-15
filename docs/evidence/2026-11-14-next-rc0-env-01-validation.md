# FalconFOC V2｜NEXT-RC0-ENV-01 Bash 环境接通与 RC0 软件门禁复验

## 判定

`NEXT-RC0-ENV-01 BLOCKED`

本轮没有新增依赖，也没有修改生产源码、CMake、门禁脚本或测试语义。已复用仓库所在机器已有的 Git for Windows Bash，Firmware CLI contract smoke 实际通过，正式 `Scope all` 越过了 Bash 阻断，但随后在既有 `test_falconfoc_themis_lite_contract.ps1` 处失败：脚本读取不存在的 `platform/at32f403a/ThemisPlatformDefaults.cmake`。

因此当前软件门禁仍未闭合，不能报告 `RC0 SOFTWARE GATES PASS`。

## A｜Bash 环境来源

发现的现有 Git for Windows：

```text
Git:  D:\Git\cmd\git.exe
Git:  2.55.0.windows.4
Bash: D:\Git\bin\bash.exe
Bash: GNU bash 5.3.15(1)-release (x86_64-pc-cygwin)
```

验证通过：

- `bash -lc` 可执行；
- `git`、`cmake`、`ninja`、`cygpath` 均可解析；
- `cygpath -w` 能将 `/d/study/Software/Hollow-Gimbal` 转回 Windows 路径；
- 只在当前 PowerShell 子进程临时前置 `D:\Git\bin;D:\Git\cmd`；
- 没有修改用户或系统 PATH；
- 没有安装、下载或新增依赖；
- 没有修改全局 Git 配置。

## B｜Firmware CLI contract smoke 安全复核

检查文件：`tests/script/test_firmware_cli_contract.ps1`

确认该 smoke 使用隔离 Mock：

- 在系统临时目录创建随机 `themis_cli_contract_*` 目录；
- 创建 Mock `cmake`、`ninja`、`arm-none-eabi-gcc`、`arm-none-eabi-size`、`arm-none-eabi-nm`、`cygpath` 和 `openocd.exe`；
- 通过 `THEMIS_OPENOCD_BIN` 指向临时 Mock OpenOCD；
- 通过临时 PATH 让 `run.sh` 只能调用这些 Mock 工具；
- 脚本检查 `run.sh all` 的 OpenOCD 调用次数和参数；
- 脚本检查 `run.sh repair` 不触发 OpenOCD；
- 脚本在 `finally` 中清理临时目录和仓库内随机 `build_cli_contract_*` 目录。

本轮实际执行：

```text
PowerShell temporary PATH = D:/Git/bin;D:/Git/cmd;...
powershell -NoProfile -ExecutionPolicy Bypass -File tests/script/test_firmware_cli_contract.ps1 -RepoRoot D:/study/Software/Hollow-Gimbal
```

实际结果：

```text
[test_firmware_cli_contract] all assertions passed.
CLI_SMOKE_EXIT=0
```

执行后未留下 `build_cli_contract_*` 目录，没有接触真实 OpenOCD、调试器、Flash 或板卡。

## C｜正式 Scope all

重新执行了仓库正式入口，并保持临时 Bash PATH：

| 门禁 | 结果 |
| --- | --- |
| `Scope falconfoc` | PASS |
| CMake formatter | PASS，动态扫描 `29/29` |
| Source contract | PASS，`61 C/H、8 CMake、8 README` |
| C/H formatter | PASS |
| CMake presets/platform/smoke | PASS |
| Firmware CLI contract | PASS |
| CMake cache guard | PASS |
| Transport/Shell hardening | PASS |
| `Scope all` | **BLOCKED** |

正式 `Scope all` 的下一处失败：

```text
Exception calling "ReadAllText" with "2" argument(s):
Could not find a part of the path
'D:\study\Software\Hollow-Gimbal\platform\at32f403a\ThemisPlatformDefaults.cmake'.

At tests/script/test_falconfoc_themis_lite_contract.ps1:20
```

脚本中对应的固定读取位置为 `tests/script/test_falconfoc_themis_lite_contract.ps1:507`。当前仓库实际存在的同类默认文件只有：

```text
platform/at/at32f415/ThemisPlatformDefaults.cmake
```

本轮没有伪造 AT32F403A 文件、没有修改 Lite contract、没有把缺失文件归类为环境 PASS，也没有扩大修改范围。

## D｜本轮软件复验状态

当前 turn 的新鲜构建根为 `build/rc0/software-gate-01c`：

| 项目 | 实际结果 |
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
| Clang ASan/UBSan | `28/28 PASS`，无诊断 |
| SYSTEM replay | `3f01f6e3` |
| Long replay | `49dea950` |
| Legacy Bench | PASS |
| V2 AT32F415 ARM | PASS |
| `git diff --check` | PASS |

RelWithDebInfo 独立结果保持：

```text
build: PASS
CTest: 44/45
```

唯一失败仍是 `test_tm_task_budget_probe_failfast` 在 `NDEBUG` 下不触发 Debug-only 断言。本轮没有修改 `NDEBUG` 或测试语义。

## E｜当前结论

Bash 环境阻断已经解决，Firmware CLI smoke 已通过；当前正式剩余软件阻断是：

1. `test_falconfoc_themis_lite_contract.ps1` 依赖缺失的 `platform/at32f403a/ThemisPlatformDefaults.cmake`；
2. RelWithDebInfo 的 Debug-only fail-fast 测试仍为真实 `44/45`，按既有 Host Debug RC0 契约独立记录。

因此保持：

- `NEXT-RC0-ENV-01 BLOCKED`；
- `RC0 SOFTWARE BLOCKED`；
- `G3-SAFE HARDWARE NOT VERIFIED`；
- `G3-PWR LOCKED`；
- `AT32F415 WCET NOT VERIFIED`。

本轮没有刷机、上电、启用 PWM、写 Flash、提交或推送。

/* ---------------------------------------- end of file ----------------------------------------- */
