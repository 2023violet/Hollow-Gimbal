# FalconFOC V2｜NEXT-CORE-PERF-01C Float32 热路径最终验收

## 判定

`PERF-01C PASS`

`PERF-01 FLOAT32 HOTPATH COMPLETE`

本结论只覆盖 Float32 Core 热路径的软件等价性、Host 动态回归、当前 ARM 静态产物和已有无功率测试。它不代表 AT32F415 实测周期/WCET、真实栈高水位、ADC/PWM 时序、实板安全能力或带电机验收。

本轮没有提交、推送、刷机、上电或解除 G3-PWR 输出锁。

## 范围与基线身份

```text
branch: codex/falconfoc-v2-rc0
HEAD:   46bc963f5e5849e4440d1714a845fe485d338837
```

当前工作树仍包含 PERF-01A/B 及本轮验证测试的未提交修改；Git HEAD 不能单独代表本次实际测试源码。

PERF-01A 原始 Golden 未覆盖或重生成：

```text
build/perf01a/golden-cmake.csv
SHA-256: AC74F39988D5FECBA870A58F9059E6701832683D6175EBCE02C57DFE71284A27
```

本轮新生成的优化结果单独保存为：

```text
build/perf01a/golden-optimized-perf01c.csv
SHA-256: 92C67E3A0A0026210395C58F51ADFEDE479D9D6087953E8D02089D91D57161BF
```

## 本轮实际修改

- `core/src/foc_core.c`：修正直接 sector 判定对带符号零的边界语义，使 `atan2f(-0,-0)` 和 `atan2f(-0,negative)` 的原有 sector 4 结果保持一致。没有改变 duty 计算、限幅或公共 API。
- `core/tests/test_foc_svpwm.c`：补充 `-0` 与负 alpha/负零 beta 的显式断言。
- `core/tests/test_foc_sector_reference.c`：新增独立参考实现，保留 PERF-01A 的 `atan2f` sector 语义和 duty 公式，执行定向边界、带符号零和 100,000 个确定性随机样本。
- `core/CMakeLists.txt`：将独立 sector differential test 纳入 Core CTest。
- `docs/evidence/2026-11-13-next-core-perf-01c-validation.md`：本验收记录。
- `docs/governance/Themis_工程维护日志.md`：追加本轮维护记录。

没有修改 Runtime、Pipeline、Safety、Device、Board、Protocol、Legacy 算法、Q31/Q15 或功率输出锁。

## AC01｜PERF-01B 实现与所有权复核

复核确认：

1. `foc_current_step()` 在一个控制周期的栈上创建独立 `foc_angle_cache`，一次规约并计算一次 `sinf/cosf`；Park、逆 Park、应用电压反算和抗饱和二次调制复用该周期载荷。
2. 载荷不存入全局变量，跨周期和多实例之间没有共享可变缓存。
3. `foc_angle_cache_prepare()` 先拒绝 NaN/Inf，再执行规约和三角计算；缓存中的三角值再次经过有限值检查。
4. 公开 `foc_park()`、`foc_inverse_park()` 和 `foc_svpwm()` 仍保留独立参数、指针、有限值和配置检查。
5. `foc_current_init()` 复制并校验 `foc_current_config`；可信 SVPWM 路径只由已初始化的 current context 调用，动态 VBUS、向量、duty 和有限值检查仍在 trusted entry 内。
6. 公共头文件将 `foc_current_context` 字段规定为 API 外只读。源码检索未发现初始化成功后对 `context.config` 的运行时写入路径；该 trusted path 仍依赖调用者遵守这一所有权契约，C 结构体本身没有语言级 const 防护。
7. 失败路径先清空输出，且只有最终调制成功后才提交积分器；现有 Pipeline/Runtime 测试继续验证下游 Device/Safety 拒绝时积分状态不推进。

## AC02｜独立 SVPWM sector 差分

执行：

```text
cmake --build build/perf01a/cmake --parallel 4
build/perf01a/cmake/test-foc-sector-reference.exe
```

独立参考测试覆盖 100,015 个样本：

- 六个扇区中心、六条边界及边界相邻 Float32 值；
- 四象限、零矢量、`+0.0f/-0.0f` 组合；
- `{-1.0f,-0.0f}` 等负 alpha 带符号零边界；
- 很小的非零矢量、限幅附近矢量和 100,000 个固定种子随机有效矢量；
- 非有限输入的错误状态和安全输出。

首轮差分发现一个真实边界差异：`alpha=-0,beta=-0` 时直接判定返回 sector 6，而原 `atan2f` 语义在当前 Clang Host 上经 `-pi + 2pi` 得到 sector 4。已按原公共语义做最小修正，并将该输入加入回归。

修正后结果：

```text
test_foc_sector_reference passed samples=100015 seed=0x13579bdf
sector mismatches: 0
duty/status/saturated mismatches: 0
undefined sector 0/7: 0
```

## AC03｜Golden 数值等价性

使用未覆盖的 PERF-01A Golden 与当前优化版本独立生成结果逐行逐列对照。整数/状态字段要求精确一致；浮点字段使用预先固定的 `2e-6 * max(1, abs(old), abs(new))` 门限。

```text
records: 4126
failures: 0
discrete/status mismatches: 0
```

最大绝对误差：

| 字段 | 最大绝对误差 |
| --- | ---: |
| `measured_d` | 0 |
| `measured_q` | 3.49691098e-09 |
| `voltage_d` | 7.39999999888e-08 |
| `voltage_q` | 2.99999999842e-08 |
| `duty_a/b/c` | 0 |
| `integral_d/q` | 0 |

对绝对量级大于 `1e-6` 的非零值，最大相对误差为 `voltage_d=5.43798138e-7`；`voltage_q=7.12250700e-8`。`measured_q` 的最大相对误差只出现在接近零的值，不作为有意义的相对误差指标。

对照字段包括状态码、output enable、目标限幅、sector、saturated、Device compare、测量 d/q、应用电压 d/q、三相 duty、积分器前后状态、长时间积分、目标反转、饱和解除和非法输入失败状态。

## AC04｜抗饱和、积分器与下游提交

重新运行 Core Current、Pipeline、Runtime 和 PMSM 模型测试，覆盖正常目标、电流/电压/duty 限幅、连续饱和、目标反转、解除饱和、恢复控制和 Device/Safety 拒绝。

关键现有断言仍通过：

- `runtime/tests/test_foc_pipeline.c` 在 UPDATE、INHIBIT、Gate、STOP 和 Device 拒绝后比较积分器前后快照；
- `runtime/tests/test_foc_runtime.c` 验证停止/故障路径不提交 current context，清故障后的 START 才建立新积分边界；
- 只有最终 Device/Safety 接受 UPDATE 时，Pipeline 才提交当前控制器状态。

结果：Core CTest `8/8`、Runtime/Pipeline `6/6`、G3-NP `2/2`、G3-SAFE `1/1` 全部通过。

## AC05｜多实例、边界与确定性

现有 Core、Runtime 和 PMSM 模型测试交错运行不同配置、角度、目标和限幅，并注入单实例故障后继续驱动另一实例。固定事件流重复运行得到相同状态/轨迹指纹；本轮没有引入第二套 Runtime 模拟器。

本轮复验还得到：

```text
long replay hash=49dea950 updates=99998 due=100001 missed=0 late=0 budget=0
```

Runtime motion model 的多实例隔离、反向速度、正反位置目标和故障后 INHIBIT 均通过。

## AC06｜ASan / UBSan

工具链与环境：

```text
LLVM Clang 22.1.8
target x86_64-pc-windows-msvc
flags -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
link flags -fsanitize=address,undefined
runtime DLL path C:/Program Files/LLVM/lib/clang/22/lib/windows
```

重新配置、编译并运行六个独立 Host 工程，实际结果：

| 工程 | CTest |
| --- | ---: |
| Core | 8/8 |
| G3-NP | 2/2 |
| G3-SAFE | 1/1 |
| Runtime/Pipeline | 6/6 |
| Protocol | 1/1 |
| Board/UART/Python | 3/3 |
| 合计 | **21/21 PASS** |

没有 AddressSanitizer、UndefinedBehaviorSanitizer 或 runtime error 诊断。sanitizer 只用于 Host 测试进程，没有写入 ARM 生产构建选项；它不能替代并发、真实栈高水位或功率级验证。

## AC07｜AT32F415 ARM 资源与调用链

重新构建：

```text
cmake --build build/rc0/baseline-arm --parallel 4
```

Arm GNU Toolchain `14.3.1`，目标 `cortex-m4 / Thumb-2 / soft-float / no FPU`。ELF、BIN、HEX、MAP 均生成。

当前产物：

| 产物 | 大小 | SHA-256 |
| --- | ---: | --- |
| `falconfoc-v2-at32f415.elf` | 64,540 B | `41B2C2A99B80CA88FC28ACFCDD5B00F7C069FF4E7A77AA1A6E238ADFBB77BBE9` |
| `falconfoc-v2-at32f415.bin` | 33,292 B | `E504F60F92DA9BDD57089BE362A1AED3843293078766708202CE4C678E1743A7` |
| `falconfoc-v2-at32f415.hex` | 93,740 B | `B359BCE5457BCBCA8F55CF12E1D98347F411CDFE12A08F7D9FDFE5AD5F0B7715` |
| `falconfoc-v2-at32f415.map` | 217,115 B | `73EA4F9CBDFBBEDC9EB4B43F39E566690228E39C063F1E524597D0DB6B7EE41F` |

链接器资源：FLASH `33,292/131,072 B`，RAM `3,376/32,768 B`。

| section | 当前大小 |
| --- | ---: |
| `.text` | 31,556 B |
| `.rodata` | 1,268 B |
| `.data` | 84 B |
| `.bss` | 2,268 B |

相对 PERF-01A，`.text` 减少 `420 B`，`.rodata` 减少 `56 B`，静态 RAM 不变。相对 PERF-01B 本次边界修正使 `.text` 比此前产物少 `4 B`；这不是新的算法优化指标。

关键静态栈帧：`foc_current_step=160 B`、`foc_current_modulate=56 B`、`foc_svpwm_trusted=48 B`、`foc_park_cached=32 B`。反汇编确认 H0 路径为 `foc_current_step -> foc_current_modulate -> foc_svpwm_trusted`，并使用 cached Park helper；优化 ELF 中没有 `atan2f` 或 `__ieee754_atan2f` 符号。`fmodf/sinf/cosf/sqrtf/hypotf` 仍存在于公共 API、角度缓存或其他路径，不能仅凭整份 ELF 符号存在判定热路径失败。

这些是单函数静态栈帧和可见调用链证据，当前没有真实运行栈高水位或 MCU WCET 证据。

## AC08｜Host 性能与动态调用

保持 PERF-01A/B 的 256 个固定输入、15 个有效批次、每批 200,000 次调用、Release 构建和 QueryPerformanceCounter 方法。本轮新测量：

| 路径 | PERF-01A 中位数 | PERF-01B 参考 | PERF-01C 复测 | 相对 A |
| --- | ---: | ---: | ---: | ---: |
| 正常 | 141.2125 ns | 88.7070 ns | 90.5050 ns | -35.91% |
| 饱和/修正 | 257.9015 ns | 177.7580 ns | 179.5810 ns | -30.37% |

本轮复测批次范围：正常 `88.7555–98.4820 ns`，饱和/修正 `176.0070–186.6780 ns`。相对 PERF-01B 的小幅回升属于同机 Host 批次离散；没有出现相对 PERF-01A 的性能回退。

动态调用计数与 PERF-01B 完全一致，计数文件 `build/perf01a/counts-perf01c.csv` SHA-256 为 `273D092E950EF5E314ED75BDC50215F23BEF4CEB9D512D7E06ECC35F6BDE1CEE`：

| 路径 | `fmodf` | `sinf` | `cosf` | `atan2f` | 配置校验 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 正常 | 1 | 1 | 1 | 0 | 0 |
| 饱和二次调制 | 1 | 1 | 1 | 0 | 0 |

Host 时间和调用计数只用于相对软件对照，不能换算为 AT32F415 周期或 WCET。

## AC09｜PMSM 动态模型回归

`test-foc-motion-model`、`test-foc-motion-runtime` 和 Runtime 长事件流重新运行通过。关键复测值：

| 场景 | 最终位置/速度 | 稳定时间 | 超调 | 限幅次数 |
| --- | --- | ---: | ---: | ---: |
| 正向速度 `+10 rad/s` | `pos=13.713761, speed=10.000168` | `0.4543 s` | `1.073327` | 1 |
| 反向速度 `-10 rad/s` | `pos=22.652746, speed=-10.000023` | `0.5134 s` | `1.433323` | 2 |
| 正向位置 `+7 rad` | `pos=6.999540, speed=0.001799` | `1.2569 s` | `0` | 4 |
| 反向位置 `-3 rad` | `pos=-2.998761, speed=-0.004988` | `1.5079 s` | `0` | 4 |

Runtime motion replay、故障注入、下游拒绝和多实例断言均保持通过；本轮没有改变控制模型指标或阈值。

## AC10｜工程与完整回归

| 检查 | 结果 |
| --- | ---: |
| Core CTest | `8/8 PASS` |
| Runtime / Pipeline | `6/6 PASS` |
| G3-NP | `2/2 PASS` |
| G3-SAFE | `1/1 PASS` |
| Board / UART / Python | `3/3 PASS` |
| Protocol | `1/1 PASS` |
| Legacy Host | `45/45 PASS` |
| Legacy Bench | PASS |
| V2 AT32F415 ELF/BIN/HEX/MAP | PASS |
| FalconFOC source contract | PASS，48 个 C/H |
| C/H formatter | PASS |
| FalconFOC scoped quality gate | PASS |
| `git diff --check` | PASS |

完整 `Scope all` 已实际执行，但仍在 `core/CMakeLists.txt` 的 `cmake-format --check` 处停止。该阻断属于全仓 CMake 格式债务，本轮没有为了全仓门禁去重构或格式化无关文件；它不影响本轮 Core 数值、性能、sanitizer、ARM 和 FalconFOC scoped gate 结论。

## 未解决边界与停止点

- 没有 AT32F415 实际控制周期、WCET 或真实栈高水位证据；
- 没有 ADC 采样窗口、SPI 事务或 PWM 提交时延的实板证据；
- 没有执行刷机、上电、功率输出或电机运行；
- 没有替换 Legacy 快速 sin/cos；
- 没有引入 Q31/Q15；
- 全仓 `Scope all` 的 CMake formatter debt 仍单独开放，不在本轮性能优化范围内。

PERF-01 Float32 热路径优化阶段到此结束。本轮不自动进入 `NEXT-SENSOR-01`、校准开发、Legacy 数学替换或新的性能微优化。

## 可复核命令

```powershell
cmake --build build/perf01a/cmake --parallel 4
ctest --test-dir build/perf01a/cmake --output-on-failure
build/perf01a/cmake/test-foc-baseline.exe --golden
build/perf01a/cmake/test-foc-baseline.exe --bench
build/perf01a/cmake/test-foc-sector-reference.exe
powershell -NoProfile -ExecutionPolicy Bypass -File build/rc0/run-sanitizer.ps1
cmake --build build/rc0/baseline-arm --parallel 4
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/flows/host/run_host_tests.ps1 -RepoRoot D:/study/Software/Hollow-Gimbal
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/gates/check_falconfoc_source_contract.ps1 -RepoRoot D:/study/Software/Hollow-Gimbal
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/format_themis.ps1 -Scope c -Mode check
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/gates/check_themis_quality_gate.ps1 -RepoRoot D:/study/Software/Hollow-Gimbal -Scope falconfoc
```

/* ---------------------------------------- end of file ----------------------------------------- */
