# FalconFOC V2｜NEXT-RC0-SW-01 全仓软件门禁与 RC0 复验

## 判定

`NEXT-RC0-SW-01 BLOCKED`

本轮完成了新构建目录中的 Host、Sanitizer、Legacy、V2 ARM 和 FalconFOC 专项复验，并关闭了已知的 `g3/np/CMakeLists.txt` 格式问题。`AC11` 动态检测实际通过；`Scope falconfoc`、源代码契约、C/H 与受影响 CMake formatter 也通过。

`Scope all` 在继续检查时暴露出另一个未修改的既有文件 `g3/safe/CMakeLists.txt` 的 `cmake-format --check` 失败。本轮任务明确只允许修复 `g3/np/CMakeLists.txt`，因此没有扩大修改范围。全仓门禁仍未闭合，RC0 软件状态保持 BLOCKED。

本轮没有修改控制算法、Runtime、Safety、Board、Protocol、默认校准参数或功率输出路径；没有刷机、上电、启用 PWM、写 Flash、提交或推送。

## A｜基线与环境

| 项目 | 结果 |
| --- | --- |
| 分支 | `codex/falconfoc-v2-rc0` |
| Git HEAD | `46bc963f5e5849e4440d1714a845fe485d338837` |
| 新鲜构建根 | `build/rc0/software-gate-01` |
| Host Debug 工具链 | MinGW GCC/G++ `16.2.0` |
| Sanitizer 工具链 | Clang `22.1.8`，`-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer` |
| CMake formatter | `cmakelang/cmake-format 0.6.13` |
| ARM 工具链 | Arm GNU Toolchain `14.3.1` |
| ARM ABI | ARM ELF32，Cortex-M4 项目配置，soft-float ABI，FPU none |

默认 Clang Host Debug 曾在既有 Legacy `tmdef.h` 编译器白名单处失败；随后使用仓库已支持的 MinGW GCC 完成规范 Host Debug 回归。Clang sanitizer 构建和运行使用独立的 LLVM runtime 路径，未通过关闭 sanitizer 绕过检测。

## B｜已知 CMake 阻断的最小修复

只修改 `g3/np/CMakeLists.txt` 的 cmake-format 布局。修复前后语义保持一致：目标名称、源文件、编译选项、链接依赖、构建条件和测试注册均未改变。

| 文件 | 修复前 SHA-256 | 修复后 SHA-256 |
| --- | --- | --- |
| `g3/np/CMakeLists.txt` | `8CF6AF7363FDCE3F836069B51E84B7F254FB67D7D486459479AC43546BCB7415` | `68AC235F1D78B63950E961E0A3A972A849FB939592F584EC3018E0430AE7D3B5` |

独立构建 `build/rc0/g3-np-format-check` 的 `CTest` 结果为 `2/2 PASS`。

重新执行 `Scope all` 后，首个剩余失败为：

```text
cmake-format --check failed: D:\study\Software\Hollow-Gimbal\g3\safe\CMakeLists.txt
```

`g3/safe/CMakeLists.txt` 在本轮没有工作树差异；该问题与 `g3/np` 修复无关，按任务边界记录为新的既有阻断，不自动修复。

## C｜Host 与 Sanitizer 验收

| 构建/测试 | 结果 |
| --- | --- |
| Host Debug MinGW fresh build | PASS |
| Host Debug CTest | `45/45 PASS` |
| Host RelWithDebInfo MinGW build | PASS |
| Host RelWithDebInfo CTest | `44/45 PASS`；唯一失败为 `test_tm_task_budget_probe_failfast`，RelWithDebInfo 定义 `NDEBUG` 后不执行该 Debug-only assert，属于配置差异，不是本轮代码缺陷 |
| G3-NP fresh build/CTest | `2/2 PASS` |
| Clang ASan/UBSan Core | `10/10 PASS` |
| Clang ASan/UBSan Sensors | `1/1 PASS` |
| Clang ASan/UBSan Calibration | `1/1 PASS` |
| Clang ASan/UBSan G3-NP | `2/2 PASS` |
| Clang ASan/UBSan G3-SAFE | `1/1 PASS` |
| Clang ASan/UBSan Runtime | `8/8 PASS` |
| Clang ASan/UBSan Protocol | `1/1 PASS` |
| Clang ASan/UBSan Board | `4/4 PASS` |
| ASan/UBSan 合计 | `28/28 PASS`，无 sanitizer 诊断 |

Sanitizer 运行时使用 LLVM 的 Windows runtime 路径；直接运行和 `ctest` 均在该环境下通过。覆盖范围为当前所有可在 Host 执行的 V2 C 代码：Core、Sensors、Calibration、Device、Safety、Runtime/Pipeline、Protocol 和 Board Port Host 测试。

## D｜确定性系统回放

当前 fresh sanitizer Runtime 可执行文件的输出：

```text
system replay hash=3f01f6e3 updates=96 h0_due=103 h1_due=41 outer_seq=40 session=3
long replay hash=49dea950 updates=99998 due=100001 missed=0 late=0 budget=0
```

Runtime `CTest` 为 `8/8 PASS`；系统回放与长时间回放无漏期、迟到或预算超限。

## E｜Legacy 与 V2 ARM 构建

Legacy Bench fresh 构建 `build/rc0/software-gate-01/legacy-bench` 通过，生成：

| 文件 | 大小 | SHA-256 |
| --- | ---: | --- |
| `falcon-foc-at32.bin` | 92,144 | `9D15570165378404DECD7357D69D61D098F8D3F263874459BF0BDDAFA072FB0B` |
| `falcon-foc-at32.elf` | 1,476,344 | `C320EC0E6965D19709681631CA4629E5EB31E172DBB56E0CBCEA71E315891E54` |
| `falcon-foc-at32.hex` | 259,262 | `82BD8AD1BF4EF0649FBE71B635962AFD6ECB96073C66C82BD122DE685646F6F0` |
| `falcon-foc-at32.map` | 1,564,366 | `FE82B708CB2DE39692C312AFDA24354ECDF0467D92B5D06485DDDACD94D59FFF` |

V2 独立 ARM 构建 `build/rc0/software-gate-01/v2-arm` 通过，ELF 的关键 section 为：

```text
.text   34916 B
.rodata 1352 B
.data   84 B
.bss    2520 B
Flash   36736 B
RAM     3632 B
```

V2 artifacts：

| 文件 | 大小 | SHA-256 |
| --- | ---: | --- |
| `falconfoc-v2-at32f415.bin` | 36,736 | `9806ADBFA3282450527F40BB97CB1DA75FAFE41FB405F1126114187C6393B029` |
| `falconfoc-v2-at32f415.elf` | 65,824 | `51D7428FF2414FA4623DCE5086A6CD8D4F69058DFC6405B317DC28875E281551` |
| `falconfoc-v2-at32f415.hex` | 103,423 | `B80C263E6B07CE976E2BF1AEDED2737EED536C7224369091DC0919CAA48C51AE` |
| `falconfoc-v2-at32f415.map` | 230,771 | `BDC648824C427C873259638A30EB2FAE0C59E1598EBEA3C2C43E0C29CF6516B3` |

ELF header确认 ARM ELF32 与 soft-float ABI。Board1 代码仍保持 `ARM/START -> OUTPUT_LOCKED`、非零 PWM 请求返回 `UPDATE_NOT_APPLIED` 的软件锁定行为；没有新增功率输出解锁路径。

没有真实 MCU 执行时间、WCET、栈高水位、ADC/PWM 采样窗口或功率级响应证据。

## F｜质量门禁

| 门禁 | 结果 |
| --- | --- |
| Source contract | `61 C/H、8 CMake、8 README PASS` |
| C/H formatter | PASS |
| CMake formatter（changed/untracked scope） | PASS |
| `Scope falconfoc` | PASS |
| `git diff --check` | PASS（仅有 Git 换行提示） |
| `Scope all` | **BLOCKED**：`g3/safe/CMakeLists.txt` cmake-format |

## G｜修改范围与剩余阻断

本轮实际生产文件修改只有 `g3/np/CMakeLists.txt` 的格式调整；本证据文件和维护日志为记录性修改。工作树中其他文件属于此前已授权阶段的既有修改，本轮保留不覆盖。

RC0 不能宣布软件 PASS 的唯一当前门禁阻断是 `Scope all` 未通过。下一步应单独授权 `g3/safe/CMakeLists.txt` 的格式修复，或明确接受该既有格式债务后重新定义全仓门禁；在此之前保持：

- `NEXT-RC0-SW-01 BLOCKED`；
- `RC0 SOFTWARE BLOCKED`；
- `G3-SAFE HARDWARE NOT VERIFIED`；
- `G3-PWR LOCKED`。

## 可复核命令

```powershell
cmake -S g3/np -B build/rc0/g3-np-format-check -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/rc0/g3-np-format-check --parallel 4
ctest --test-dir build/rc0/g3-np-format-check --output-on-failure
cmake --preset host-debug -B build/rc0/software-gate-01/host-debug-mingw -DCMAKE_C_COMPILER=C:/mingw64/bin/gcc.exe -DCMAKE_CXX_COMPILER=C:/mingw64/bin/g++.exe
cmake --build build/rc0/software-gate-01/host-debug-mingw --parallel 4
ctest --test-dir build/rc0/software-gate-01/host-debug-mingw --output-on-failure
ctest --test-dir build/rc0/software-gate-01/san-core --output-on-failure
ctest --test-dir build/rc0/software-gate-01/san-sensors --output-on-failure
ctest --test-dir build/rc0/software-gate-01/san-calibration --output-on-failure
ctest --test-dir build/rc0/software-gate-01/san-np --output-on-failure
ctest --test-dir build/rc0/software-gate-01/san-safe --output-on-failure
ctest --test-dir build/rc0/software-gate-01/san-runtime --output-on-failure
ctest --test-dir build/rc0/software-gate-01/san-protocol --output-on-failure
ctest --test-dir build/rc0/software-gate-01/san-board --output-on-failure
cmake --build build/rc0/software-gate-01/legacy-bench --parallel 4
cmake --build build/rc0/software-gate-01/v2-arm --parallel 4
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/gates/check_falconfoc_source_contract.ps1 -RepoRoot D:/study/Software/Hollow-Gimbal
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/format_themis.ps1 -Scope c -Mode check
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/format_themis.ps1 -Scope cmake -Mode check
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/gates/check_themis_quality_gate.ps1 -RepoRoot D:/study/Software/Hollow-Gimbal -Scope falconfoc
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/gates/check_themis_quality_gate.ps1 -RepoRoot D:/study/Software/Hollow-Gimbal -Scope all
```

/* ---------------------------------------- end of file ----------------------------------------- */
