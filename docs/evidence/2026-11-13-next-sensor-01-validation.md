# FalconFOC V2｜NEXT-SENSOR-01 磁编码器反馈、速度估计与滤波验证

## 判定

`NEXT-SENSOR-01 SOFTWARE PASS`

本结论覆盖独立 Sensor 算法、Board1 软件适配、Host 数值/异常测试、Runtime 回归、sanitizer 和 AT32F415 静态构建。它不代表真实编码器电气波形、MCU 实测周期、实际噪声、SPI 时序余量、真实电机控制质量或 G3-SAFE/G3-PWR 通过。

本轮没有刷机、上电、启用 PWM、提交或推送。

## 当前真实调用路径

Board1 的传感器调用链现在是：

```text
TMR5_GLOBAL_IRQHandler
  -> foc_board_encoder_begin()
  -> SPI 完成/错误或 TMR5 结束窗口
  -> foc_board_encoder_complete()
       -> foc_encoder_update(captured_us, raw_code, transport, &feedback)
       -> foc_runtime_h1_step(&sensor, completion_us, cost_us)
            -> H1 双槽发布 foc_runtime_sensor
            -> H0 在 ADC 源时间消费机械位置并执行既有电角度预测
```

`captured_us` 是 CS/事务开始时的源时间，`now_us` 只用于 Board 传输完成窗口和 Runtime 调度。Sensor 不使用 H1 处理时间替代源时间，也不访问 SPI、DMA、IRQ、GPIO、ADC、PWM 或 Safety。

H0 继续使用 Runtime 的源角度、机械位置和预测策略。滤波后的机械速度不会直接替换 H0 的源角度；Board1 只将 LPF 速度发布到既有 `foc_runtime_sensor.mechanical_rad_s`，raw 和 alpha-beta 速度留在 Board 本地反馈诊断中。

## 实际修改

新增：

- `sensors/include/foc_encoder.h`
- `sensors/src/foc_encoder.c`
- `sensors/tests/test_foc_encoder.c`
- `sensors/CMakeLists.txt`
- `sensors/README.md`

适配：

- `firmware/at32f415/include/foc_board.h`
- `firmware/at32f415/src/foc_board.c`
- `firmware/at32f415/tests/test_foc_board.c`
- `firmware/at32f415/CMakeLists.txt`
- `firmware/at32f415/README.md`
- `scripts/gates/check_falconfoc_source_contract.ps1`
- `scripts/format_themis.ps1`
- `build/rc0/run-sanitizer.ps1`（本地验证脚本）
- `docs/governance/Themis_工程维护日志.md`

没有修改 Core 热路径、Runtime 公共结构、Safety 状态机、UART/Protocol、Legacy、Q31/Q15 或功率输出锁。

## Sensor 算法契约

### 角度与连续位置

- 首版配置支持 `65536` 个原始码、正向/反向安装方向、最大可信机械速度、最小/最大源采样间隔。
- 连续位置使用 `int64_t position_counts` 累计，只有输出边界转换为 Float32，避免多圈状态在 Float32 内部直接累计造成更早的精度损失。
- 使用有界模差处理 `65535 -> 0`、`0 -> 65535`、正反向和多圈。
- 时间戳使用 uint32 无符号差值；跨回绕的合法差值可接受，重复时间戳、半范围以上的倒序和超大间隔拒绝。
- 若 `max_speed * dt >= pi`，相邻单圈角度无法唯一确定经过的圈数，返回并锁存 `DISCONTINUITY`，不猜测位置。
- 精确半圈跳变、速度超过上限、连续性失效均保持锁存；下一帧合法不能自动恢复，必须显式 `foc_encoder_reset()`。

### 速度与滤波

- `raw_velocity_rad_s = delta_rad / actual_dt_s`。
- LPF 使用后向欧拉：`alpha = dt / (tau + dt)`，`tau = 1/(2*pi*cutoff)`；`dt` 来自每一帧源时间戳。
- alpha-beta 跟踪器使用位置预测、残差、alpha 位置修正和 beta/dt 速度修正，初始化、失效和 reset 语义明确。
- 无效传输、无效码、重复/倒序时间戳、非法间隔、连续性错误均不更新最后有效位置，也不推进任一滤波器状态。
- 质量结果区分 `UNINITIALIZED`、`VALID`、`TIMEOUT`、`TRANSPORT_ERROR`、`DUPLICATE`、`TIME_ORDER_ERROR`、`DISCONTINUITY`、`SPEED_OUT_OF_RANGE`、`INVALID_SAMPLE`、`INVALID_INTERVAL` 和 `NUMERIC_ERROR`。

### Board1 选择

Board1 配置：

```text
counts_per_revolution = 65536
方向 = REVERSED
max_speed = 1000 rad/s
min_dt = 1 us
max_dt = 2500 us
LPF cutoff = 80 Hz
alpha-beta = alpha 0.85, beta 0.20
```

综合确定性数据集结果，alpha-beta 在本地合成阶跃、反向和加速误差上略优于 LPF；但真实编码器的电气噪声、SPI 延迟和采样抖动尚未测量，因此 Board1 先选择更简单、相位行为更容易界定的 LPF 作为现有 Runtime 默认速度，alpha-beta 只作为可比较诊断输出，不宣称已获得真实传感器优势。

## 确定性算法数据集

当前 Host 测试使用固定序列：静止量化噪声、低速匀速、4 rad/s 匀速、8 rad/s 阶跃、加速、正反转、单次传输丢帧、uint32 时间戳回绕、重复/倒序时间戳、半圈歧义、速度越界、非法码和两个独立实例。

本轮测试输出：

```text
encoder dataset samples=2000
raw_rms=0.02669964
lpf_rms=0.02669964
tracker_rms=0.02668464
context_bytes=80
feedback_bytes=48

encoder metrics
static_rms_raw=0.271071
static_rms_lpf=0.03487715
static_rms_tracker=0.03200987
step_delay_lpf=11 frames
step_delay_tracker=9 frames
reverse_delay_lpf=14 frames
reverse_delay_tracker=11 frames
accel_rmse_lpf=0.03382908
accel_rmse_tracker=0.02845867
low_rms_raw=0.008252417
low_rms_lpf=0.008252417
low_rms_tracker=0.008193034
drop_recovery_abs_error=0.02669954 rad/s
```

这些数值只比较算法对合成输入的响应，不是 Board1 的噪声、延迟、执行周期或真实电机性能。

## AC01–AC11｜独立算法与状态

| 验收项 | 结果 | 证据 |
| --- | --- | --- |
| AC01 模块独立构建 | PASS | `cmake -S sensors -B build/sensors -G Ninja`；无 MCU/SPI/RTOS/Runtime 依赖 |
| AC02 单圈角度 | PASS | 0、满量程、中间码、方向反转和边界转换测试 |
| AC03 跨圈/连续位置 | PASS | 65535/0 跨界、正反向、多圈和 int64 累计测试 |
| AC04 时间戳 | PASS | uint32 回绕、重复、倒序、半范围和最大间隔测试 |
| AC05 连续性失效 | PASS | 速度越界、半圈别名、超大间隔锁存和显式 reset 测试 |
| AC06 原始速度 | PASS | 实际源 `dt` 与独立角度差分结果对照 |
| AC07 LPF | PASS | 静止、低速、阶跃、反向、变 dt、丢帧和失效不推进测试 |
| AC08 alpha-beta | PASS | 位置预测、残差修正、正反向、加速、失效和 reset 测试 |
| AC09 算法对照 | PASS | raw/LPF/alpha-beta RMS、阶跃延迟、反向恢复、加速 RMSE 和丢帧恢复统计 |
| AC10 状态隔离 | PASS | 65536 正向实例与 4096 反向实例交错运行，状态互不污染 |
| AC11 失败状态 | PASS | 非法配置、非法码、重复/倒序 dt、transport timeout/frame error、NaN/非有限保护路径 |

## AC12–AC13｜Board1 与 Runtime 集成

Board1 仍保留：

- SPI 事务启动；
- SPI 完成/错误和 TMR5 结束窗口；
- 原始 16 位码获取；
- CS 捕获时间；
- H1 `foc_runtime_sensor` 发布；
- ARM/START `OUTPUT_LOCKED`、UPDATE `NOT_APPLIED`、INHIBIT 软件确认。

Board1 Host 结果：

```text
Board/UART/Python: 3/3 PASS
```

Board 测试额外确认：

- 首帧只有位置参考，`quality=UNINITIALIZED`，不产生可信速度；
- 第二个有效帧产生 `VALID` 反馈；
- `mechanical_rad_s` 等于 Sensor 的 LPF 输出；
- transport 错误、超时和连续性错误仍请求 Runtime 禁止；
- 原有输出锁和 UPDATE 拒绝行为不变。

Runtime/Pipeline 结果：

```text
Runtime/Pipeline: 6/6 PASS
G3-NP: 2/2 PASS
G3-SAFE: 1/1 PASS
```

H0/H1 时间预测、运动控制、下游 UPDATE 拒绝后的状态提交、故障禁止和既有 PMSM 模型均未回归。

## AC14｜ASan / UBSan

使用 LLVM Clang `22.1.8`、Windows `x86_64-pc-windows-msvc`，测试进程启用：

```text
-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
```

实际结果：

| 工程 | CTest |
| --- | ---: |
| Core | 8/8 |
| Sensors | 1/1 |
| G3-NP | 2/2 |
| G3-SAFE | 1/1 |
| Runtime/Pipeline | 6/6 |
| Protocol | 1/1 |
| Board/UART/Python | 3/3 |
| 合计 | **22/22 PASS** |

没有 AddressSanitizer、UndefinedBehaviorSanitizer 或 runtime error 诊断。ARM 生产构建没有使用 sanitizer 选项。

## AC15｜AT32F415 ARM 资源

重新构建目标 `falconfoc-v2-at32f415`，Arm GNU Toolchain `14.3.1`，`cortex-m4 / Thumb-2 / soft-float / no FPU`。

| 产物 | 大小 | SHA-256 |
| --- | ---: | --- |
| ELF | 64,928 B | `45EAC2379ECC33DF39618AAF1057AFC12D919A6F76B3F94F333E285D75F18734` |
| BIN | 34,856 B | `8B9DFFF63EE0A6CD1AE61B01766A796BDE72A8B473FACB589ADC1D16C3EEA5F6` |
| HEX | 98,129 B | `B2FA6E2A620297D1EF61989BF52D310227161680534D7020DEF3403A090405FE` |
| MAP | 218,468 B | `F1C51DBD51D2182ED6B3BE6D8982346E24F85F953D1370CEAC9D6FE8CCA0800B` |

当前 section 与链接器统计：

| 项目 | Sensor 接入后 |
| --- | ---: |
| `.text` | 33,076 B |
| `.rodata` | 1,312 B |
| `.data` | 84 B |
| `.bss` | 2,392 B |
| 链接器 RAM | 3,504 B |
| FLASH used | 34,856 B |

相对 PERF-01C 的无 Sensor 产物，新增 Sensor/Board1 适配约为：`.text +1,520 B`、`.rodata +44 B`、`.bss +124 B`、链接器 RAM `+128 B`、Flash `+1,564 B`。

静态栈帧：

```text
foc_encoder_update 72 B
foc_encoder_init 56 B
foc_encoder_reset 56 B
foc_board_encoder_complete 56 B
foc_board_encoder_begin 16 B
```

这些是静态 `.su` 帧大小，不是 H1 实测栈高水位；Host benchmark 和 ARM 静态尺寸也不能换算为 MCU WCET。

## AC16｜完整工程回归

| 检查 | 结果 |
| --- | ---: |
| Sensor CTest | `1/1 PASS` |
| Core CTest | `8/8 PASS` |
| Runtime/Pipeline | `6/6 PASS` |
| G3-NP | `2/2 PASS` |
| G3-SAFE | `1/1 PASS` |
| Board/UART/Python | `3/3 PASS` |
| Protocol | `1/1 PASS` |
| Legacy Host | `45/45 PASS` |
| Legacy Bench | PASS |
| V2 AT32F415 ELF/BIN/HEX/MAP | PASS |
| FalconFOC source contract | PASS，51 个 C/H、7 个 CMake、7 个 README |
| C/H formatter | PASS，包含 sensors/ |
| FalconFOC scoped quality gate | PASS |
| `git diff --check` | PASS |

全仓 `Scope all` 仍保留此前的 CMake formatter debt，不作为本轮 Sensor 专项门禁；本轮没有格式化无关 Legacy CMake 文件。

## 未解决边界与停止点

- 实际编码器电气协议、CS/SCK/MISO 波形、帧延迟和采样抖动未在实板测量；
- 16 位编码器绝对零点和电角度 offset 仍未校准；
- 真实 MCU H1 周期、栈高水位和长期噪声未验证；
- 没有带电机运行、刷机、上电或 PWM 使能；
- α-β 估计器尚未被强制作为真实产品默认，只保留为可比较诊断输出；
- 本轮不自动进入 `NEXT-CAL-01`。

Sensor 软件阶段到此停止，等待独立的校准和真实硬件授权。

## 可复核命令

```powershell
cmake -S sensors -B build/sensors -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/sensors --parallel 4
ctest --test-dir build/sensors --output-on-failure
cmake --build build/perf01a/board --parallel 4
ctest --test-dir build/perf01a/board --output-on-failure
powershell -NoProfile -ExecutionPolicy Bypass -File build/rc0/run-sanitizer.ps1
cmake --build build/rc0/baseline-arm --parallel 4
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/flows/host/run_host_tests.ps1 -RepoRoot D:/study/Software/Hollow-Gimbal
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/gates/check_falconfoc_source_contract.ps1 -RepoRoot D:/study/Software/Hollow-Gimbal
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/format_themis.ps1 -Scope c -Mode check
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/gates/check_themis_quality_gate.ps1 -RepoRoot D:/study/Software/Hollow-Gimbal -Scope falconfoc
```

/* ---------------------------------------- end of file ----------------------------------------- */
