# FalconFOC V2｜NEXT-PERF-02 Float32 电流环热路径性能分析

日期：2026-11-14  
分支：`codex/falconfoc-v2-rc0`  
HEAD：`46bc963f5e5849e4440d1714a845fe485d338837`

## 最终判定

`NEXT-PERF-02 ANALYSIS PASS / NO CHANGE`

本轮完成了当前真实基线、AT32F415 soft-float 静态热点审计、一个低风险优化候选的 A/B 测量、Golden/数学/事务回归和完整软件门禁复验。候选可以减少典型 Current Step 的一次 `hypotf`，但完整 H0 路径没有稳定、可重复的收益，测量波动大于可确认的改善，因此已撤回本轮生产优化改动。

本轮最终没有新增生产算法差异。工作树中 `core/src/foc_current.c` 的差异均属于此前 PERF-01B/01C 等已存在的未提交工作，本轮候选已经撤销。

## AC01｜当前性能基线

基线使用当前工作树、Clang Release/Ninja、`-O3 -DNDEBUG`，沿用仓库已有 `test-foc-baseline --bench` 的固定 256 输入、预热批次、15 个有效批次和每批 200,000 次调用。当前 fresh run 的结果：

| 路径 | 最小 ns/step | 中位 ns/step | 最大 ns/step |
| --- | ---: | ---: | ---: |
| Normal Current Step | 95.325 | 112.816 | 139.8125 |
| Saturated / correction | 194.079 | 215.620 | 227.4855 |

这是 Host 相对基线，不是 AT32F415 周期或 WCET。

当前动态调用计数来自 `-O0 -g -fno-inline -finstrument-functions` 的独立计数构建，不参与计时：

| 路径 | fmodf | sinf | cosf | atan2f | sqrtf | hypotf | Park | inverse Park | SVPWM | config validate | modulate |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Normal | 1 | 1 | 1 | 0 | 3 | 1 | 2 | 1 | 1 | 0 | 1 |
| Low VBUS/vector/duty saturation | 1 | 1 | 1 | 0 | 5 | 2 | 3 | 2 | 2 | 0 | 2 |
| Saturation without correction | 1 | 1 | 1 | 0 | 3 | 1 | 2 | 1 | 1 | 0 | 1 |

当前热路径已经包含 PERF-01B/01C 的角度载荷复用、直接 sector 判定和 trusted SVPWM。`atan2f` 不再位于当前电流环路径；`hypotf` 仍用于重建电压的最终 envelope 检查。

## AC02｜AT32F415 静态热点

当前 ARM 目标保持 `Cortex-M4 + no FPU + soft-float ABI`。`arm-none-eabi-nm`/`objdump` 显示：

- `foc_runtime_h0_step -> foc_runtime_control -> foc_pipeline_step_guarded -> foc_current_step_internal`。
- `foc_current_step_internal` 正常路径调用 `foc_current_modulate` 一次；抗饱和修正路径最多两次。
- `foc_current_modulate` 调用 `foc_svpwm_trusted`，再进行 duty-to-voltage 重建和 cached Park。
- ELF 中存在 `fmodf`、`sinf`、`cosf`、`sqrtf`、`hypotf`、`__aeabi_fadd/fsub/fmul/fdiv` 等软件浮点实现；没有 `atan2f`。

当前 V2 ARM 产物：

| 项目 | 当前值 |
| --- | ---: |
| `.text` | 36,644 B |
| `.data` | 92 B |
| `.bss` | 3,544 B |
| ELF 总静态尺寸 | 40,280 B |
| `foc_current_step_internal` 符号 | `0x39a` |
| `foc_runtime_h0_step` 符号 | `0x210` |
| `foc_runtime_h1_step` 符号 | `0x378` |
| `foc_encoder_update` 符号 | `0x404` |
| `foc_runtime_control` 符号 | `0x464` |
| `foc_pipeline_step_guarded` 符号 | `0x36a` |

主要静态栈帧仍为：`foc_current_step_internal=184 B`、`foc_runtime_h0_step=96 B`、`foc_runtime_h1_step=288 B`、`foc_runtime_control=224 B`、`foc_pipeline_step_guarded=312 B`。这不是实机栈高水位或 WCET 证明。

## AC03｜唯一优化候选及数学依据

候选是对 `foc_current_modulate` 的最终电压 envelope 检查增加保守快速路径：若 `max(|alpha|, |beta|) <= 0.5 * voltage_limit`，由

`|v| <= sqrt(2) * max(|alpha|, |beta|) <= 0.7071 * voltage_limit`

可证明该向量远离 envelope，不需要调用 `hypotf`。饱和、边界和高幅值路径仍保留原有 `hypotf` 检查。该候选不改变 duty、PI、anti-windup、Safety、状态提交或公共 API。

候选的计数效果：Normal 路径 `hypotf 1 -> 0`；饱和路径保持原有 `hypotf 1/2`。候选生成的 Golden 与原基线逐行一致，`diff=0`。

## AC04｜A/B 结果与撤回决定

候选测试使用与基线相同的输入、Clang、`-O3`、计时方法和独立 build。由于 Windows Host 调度噪声明显，进行了多轮重复：

### Current Step 局部测量

5 轮、每轮 15 个有效批次的聚合中位数：

| 路径 | Before | 候选 After | 表面变化 |
| --- | ---: | ---: | ---: |
| Normal | 133.284 ns/step | 117.750 ns/step | -11.65% |
| Saturated / correction | 227.5625 ns/step | 230.6715 ns/step | +1.36% |

Normal 局部路径有改善，但饱和路径没有改善，且该结果不能代表完整 H0。

### Full H0 路径测量

使用临时 Host harness 驱动真实 Runtime H0、Pipeline、Device、Safety、H1 样本和 Foreground，20,000 次迭代，不写入仓库：

| 重复集合 | Before 中位 | 候选 After 中位 | 结论 |
| --- | ---: | ---: | --- |
| 5 次 | 528.660 ns | 564.830 ns | -6.84% |
| 10 次 | 608.025 ns | 565.965 ns | +6.92% |

两组结果方向相反，说明当前 Windows 调度噪声与 Runtime 外围成本已经淹没候选收益，不能建立可信的完整 H0 改善结论。没有目标 MCU 周期证据时，保留这项额外分支会增加维护复杂度，却无法证明产品收益。

因此已撤回候选，只保留分析和证据，不保留生产代码改动。

## AC05｜数学、Golden、事务和安全回归

- 原 PERF-01C Golden 与撤回后的最终输出逐行一致：`golden_diff=0`。
- Current PI、PMSM、Clarke/Park、逆 Park、SVPWM、Motion、Sensor 独立参考继续通过。
- PI 积分状态、饱和标志、错误输出、失败原子性、Pipeline UPDATE 拒绝语义没有改变。
- SYSTEM replay：`3f01f6e3`。
- Long replay：`49dea950`。
- 没有删除任何动态数值检查或 Safety Gate。

## AC06｜最终软件回归

| 检查 | 结果 |
| --- | --- |
| Core CTest | `14/14 PASS` |
| Sensors | `2/2 PASS` |
| Calibration | `1/1 PASS` |
| G3-NP | `2/2 PASS` |
| G3-SAFE | `1/1 PASS` |
| Runtime | `8/8 PASS` |
| Protocol | `1/1 PASS` |
| Board/UART/Python | `4/4 PASS` |
| Clang ASan/UBSan | `33/33 PASS` |
| Legacy Host Debug | `45/45 PASS` |
| Source contract | `66 C/H、8 CMake、8 README PASS` |
| C/H formatter | PASS |
| CMake formatter | `29/29 PASS` |
| `Scope falconfoc` | PASS |
| `Scope all` | PASS |
| AT32F415 ARM build | PASS |
| `git diff --check` | PASS |

正式 `Scope all` 在当前 PowerShell 进程临时前置已有 `D:\Git\bin;D:\Git\cmd` 后执行，Firmware CLI contract 也实际 PASS。没有修改永久 PATH、门禁脚本或新增依赖。

## 最终状态与后续边界

本轮没有找到值得保留的、可稳定证明的 Float32 电流环热路径优化，因此正式状态为：

`NEXT-PERF-02 ANALYSIS PASS / NO CHANGE`

下一项真正有价值的性能工作应优先取得 AT32F415 实际周期、DWT/GPIO 观测或稳定的目标平台测量，再决定是否继续优化 `foc_current_step_internal`、`foc_encoder_update` 或 `foc_runtime_h0_step`。不应把当前 Host 局部收益直接换算成 MCU WCET。

继续保持：

- `G3-SAFE HARDWARE NOT VERIFIED`
- `G3-PWR LOCKED`
- `AT32F415 WCET NOT VERIFIED`

