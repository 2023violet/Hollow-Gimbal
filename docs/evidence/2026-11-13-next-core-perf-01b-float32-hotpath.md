# FalconFOC V2 NEXT-CORE-PERF-01B｜Float32 热路径精简

## 判定

`PERF-01B PASS`，在本轮限定范围内成立。

本轮只修改了 Float32 Core 热路径、必要的 Core 内部头文件、SVPWM 边界测试、PERF-01A 基线测试入口和证据文档。没有修改 Runtime、Pipeline、Safety、Device 生产逻辑、Board Port、Protocol、Legacy 算法或公共 API，也没有引入 Q31/Q15。

原始 PERF-01A Golden 文件没有覆盖或重生成：

```text
build/perf01a/golden-cmake.csv
SHA-256: AC74F39988D5FECBA870A58F9059E6701832683D6175EBCE02C57DFE71284A27
```

## 实际修改

### 1. 共享角度载荷

新增 `core/src/foc_core_internal.h`，只供 Core sibling translation units 使用，包含：

- 一次角度规约；
- 一次 `sinf/cosf`；
- Park / inverse Park 的可信内部计算；
- 只在静态配置已经由初始化/公开边界验证后使用的 `foc_svpwm_trusted()`。

公开 `foc_park()`、`foc_inverse_park()`、`foc_svpwm()` 和 `foc_current_step()` 签名没有改变。公开入口仍然执行指针、非有限值、配置、VBUS、dt 和输出边界检查。

`foc_current_step()` 在一个控制周期建立一个角度载荷，测量电流 Park、逆 Park、应用电压 Park 以及抗饱和二次调制共同复用它。当前实现只在 Park 与 inverse Park 使用同一电角度时复用；没有假设采样角度和 PWM 实际施加角度永远相同。

### 2. 无 `atan2f` sector 判定

SVPWM duty 仍由现有归一化、矢量限制、三相相量和 common-mode 注入生成。sector 元数据改为基于 alpha/beta 符号和 60° 边界的直接判定：

- 保持 sector 1～6；
- 保留零矢量及带符号零行为；
- 保留正负 alpha/beta 和边界左右语义；
- 不为遥测新增实时三角函数。

公共 `foc_svpwm_output` 结构未改变。

### 3. 静态配置校验分离

公开 `foc_svpwm()` 继续完整校验 `foc_config`。电流控制只使用 `foc_current_init()` 已经复制并验证的 context 配置，调用内部 trusted SVPWM 入口；动态 VBUS、有限值、矢量、duty、输出和抗饱和检查仍保留。

当前所有权依据是 `foc_current_context.config` 在成功初始化后由 API 私有持有，公共头文件也要求调用者将 context 字段视为只读。没有建立新的配置管理器，也没有允许运行时任意修改配置。

### 4. 抗饱和没有删除

以下路径均保留：

- duty-to-voltage 反算；
- 应用电压 d/q 重建；
- 积分器准入；
- 饱和标志；
- 二次调制；
- 错误时状态不提交。

本轮只复用了角度载荷和可信静态配置前置条件。

## Golden 等价性

对原始 PERF-01A CSV 逐行、逐列比较，共 4,126 条数据记录。比较字段包括：

- 状态码；
- output enable；
- target-limited；
- sector；
- saturated；
- Device compare 1/2/3；
- measured d/q；
- applied voltage d/q；
- 三相 duty；
- PI 积分器前后状态；
- 所有输入和配置字段。

允许门限为每个浮点字段 `2e-6 * max(1, abs(old), abs(new))`，整数和状态字段必须完全相同。

结果：

```text
rows: 4126 vs 4126
failures: 0
max measured_q difference: 3.49691098e-09
max voltage_d difference: 7.4e-08
max voltage_q difference: 3.0e-08
all integer/status fields: exact match
```

优化后的结果保存在独立文件，不覆盖旧基线：

```text
build/perf01a/golden-optimized-final.csv
```

## 动态调用结果

计数构建没有参与 Host benchmark，只用于调用计数和优化门禁。

| 路径 | PERF-01A | PERF-01B |
| --- | ---: | ---: |
| `fmodf` 正常 | 3 | 1 |
| `sinf` 正常 | 3 | 1 |
| `cosf` 正常 | 3 | 1 |
| `atan2f` 正常 | 1 | 0 |
| 静态配置校验/次 | 1 | 0 |
| `fmodf` 饱和二次调制 | 5 | 1 |
| `sinf` 饱和二次调制 | 5 | 1 |
| `cosf` 饱和二次调制 | 5 | 1 |
| `atan2f` 饱和二次调制 | 2 | 0 |
| 静态配置校验/次 | 2 | 0 |

优化后的完整计数文件为：

[counts-optimized-final.csv](D:/study/Software/Hollow-Gimbal/build/perf01a/counts-optimized-final.csv)

## Sector 边界验证

新增测试覆盖：

- 0°、60°、120°、180°、240°、300°、360°；
- 边界前后 Float32 值；
- alpha/beta 四象限；
- 零矢量；
- `+0/-0` 的 alpha/beta 组合；
- 很小有效矢量；
- 矢量和 duty 限幅。

SVPWM 原有测试和新增边界测试全部通过。Golden 中所有 sector 字段也逐项精确相同。

## Host benchmark

保持 PERF-01A 相同输入、Release 构建、Windows QueryPerformanceCounter、256 个固定输入、15 个有效批次和每批 200,000 次调用。

| 路径 | PERF-01A 中位数 | PERF-01B 中位数 | 差异 |
| --- | ---: | ---: | ---: |
| 正常 | 141.2125 ns/step | 88.7070 ns/step | -37.18% |
| 饱和/修正候选 | 257.9015 ns/step | 177.7580 ns/step | -31.08% |

原始和优化后 benchmark：

- [benchmark.csv](D:/study/Software/Hollow-Gimbal/build/perf01a/benchmark.csv)
- [benchmark-optimized-final.csv](D:/study/Software/Hollow-Gimbal/build/perf01a/benchmark-optimized-final.csv)

Host 数据只说明当前 Windows Host 的相对变化，不能转换成 AT32F415 周期或 WCET。

## ARM 资源对照

优化前后 AT32F415 目标仍为 `cortex-m4 / soft-float / no FPU`。

| 项目 | PERF-01A | PERF-01B | 差异 |
| --- | ---: | ---: | ---: |
| `.text` | 31,976 B | 31,560 B | -416 B |
| `.rodata` | 1,324 B | 1,268 B | -56 B |
| `.data` | 84 B | 84 B | 0 |
| `.bss` | 2,268 B | 2,268 B | 0 |
| Linker RAM | 3,376 B | 3,376 B | 0 |
| BIN SHA-256 | `B2743F...763EB7` | `1BFC52...C40343` | changed as expected |

关键栈帧：

| 函数 | PERF-01A | PERF-01B |
| --- | ---: | ---: |
| `foc_current_step` | 144 B | 160 B |
| `foc_current_modulate` | 56 B | 56 B |
| `foc_svpwm` / trusted path | 56 B | 48 B |
| Public `foc_park` | 32 B | 48 B |
| Public `foc_inverse_park` | 32 B | 40 B |

`foc_current_step` 的栈帧增加 16 B，来自本周期角度载荷和内部临时状态；总体 Flash 减少且 RAM 静态统计不变。该栈变化已记录，不能解释为已经通过真实栈高水位验收。

优化后 ELF 中没有 `atan2f` / `__ieee754_atan2f` 符号；H0 当前电流路径调用的是 `foc_current_step -> foc_current_modulate -> foc_svpwm_trusted` 和 cached Park helpers。整份 ELF 仍保留 `sinf/cosf/fmodf/sqrtf/hypotf`，因为公共 API、角度缓存、其他路径和数学库实现仍需要它们。

优化后的静态产物：

- [arm-size-optimized.txt](D:/study/Software/Hollow-Gimbal/build/perf01a/arm-size-optimized.txt)
- [arm-symbols-hot-optimized.txt](D:/study/Software/Hollow-Gimbal/build/perf01a/arm-symbols-hot-optimized.txt)
- [arm-disassembly-optimized.txt](D:/study/Software/Hollow-Gimbal/build/perf01a/arm-disassembly-optimized.txt)

## 回归结果

| 检查 | 结果 |
| --- | ---: |
| Core CTest | 7/7 PASS |
| Runtime / Pipeline | 6/6 PASS |
| G3-NP | 2/2 PASS |
| G3-SAFE | 1/1 PASS |
| Board / UART / Python | 3/3 PASS |
| Protocol | 1/1 PASS |
| Legacy Host | 45/45 PASS |
| Legacy Bench | PASS |
| V2 AT32F415 独立构建 | PASS |
| FalconFOC source contract | PASS，47 个 C/H 文件 |
| C/H formatter | PASS |
| FalconFOC scoped quality gate | PASS |
| `git diff --check` | PASS |

全仓 `Scope all` 仍受 PERF-01A 之前记录的既有 CMake formatter debt 阻断；本轮没有改动那些授权范围外的文件。

## 未解决边界

- 没有 AT32F415 实际周期或 WCET 证据；
- 没有真实 ADC/PWM 采样时刻偏差证据；
- 没有证明电流采样角度和 PWM 实际施加角度未来永远相同；
- 没有修改或替换 Legacy 快速三角函数；
- 没有引入 Q31/Q15；
- 没有执行任何刷机、上电或功率输出。

本轮完成后停止，不自动进入 PERF-01C、Legacy 数学替换、Q31 或传感器开发。
