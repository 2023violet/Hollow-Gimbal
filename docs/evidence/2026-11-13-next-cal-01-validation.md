# FalconFOC V2｜NEXT-CAL-01 电流与电角度校准算法验证

## 判定

`NEXT-CAL-01 SOFTWARE PASS`

本轮完成独立、离线、Float32 校准候选算法和 Host 验证。结果只表示软件能够从满足契约的输入计算并验证候选参数，不表示真实 ADC 零点、编码器安装方向、极对数、电角度 offset 或真实电机已经完成标定。

本轮没有刷机、上电、启动电机、启用 PWM、写入 Flash、修改运行中的 Board/Runtime 配置、提交或推送。

## 当前数据流与参数所有权

当前实际运行数据流：

```text
Board1 ADC raw ranks
  -> foc_device_decode_sample()
       -> (raw - foc_device_adc_config.phase_a/b/c_offset_count)
       -> phase currents -> Core/Runtime

Sensors mechanical_rad
  -> Runtime foc_rotor_electrical()
       -> wrap(pole_pairs * mechanical_rad + electrical_offset_rad)
       -> H0 prediction -> Core current control
```

当前 Board1 静态名义参数仍是：

```text
phase offsets: 2048 / 2048 / 2048 counts
current gain: 3.3 / (4095 * 0.5) A/count
phase order: ABC
encoder direction: Sensor configuration, not calibration-applied
pole pairs: 7
 electrical offset: 0 rad
```

本轮 Calibration 只产生候选结果和 identity metadata：

- 三相零点候选对应 `foc_device_adc_config.phase_a/b/c_offset_count`；
- current gain、ADC full scale、phase order、encoder counts、direction、pole pairs 进入 identity；
- rotor 候选对应未来 `foc_rotor_config` 的 direction/pole-pairs/offset；
- 不执行任何结果复制、Runtime 更新或 Flash 持久化。

## 模块边界

新增独立目录：

- `calibration/include/foc_calibration.h`
- `calibration/src/foc_calibration.c`
- `calibration/tests/test_foc_calibration.c`
- `calibration/CMakeLists.txt`
- `calibration/README.md`

模块不依赖 MCU、SPI、ADC HAL、Device、Runtime、Safety、Flash 或动态内存，不在 ISR 中运行，不控制 PWM，支持多个独立实例。

校准生命周期显式区分：

```text
UNINITIALIZED -> COLLECTING -> CANDIDATE -> VALID
                                      \-> INVALID
```

失败会保留 `INVALID`，必须显式 reset；没有“误差最小就强行通过”的兜底路径。

## CAL-01A｜三相电流零点

输入包含：

- 三相 raw ADC counts；
- 必需通道 mask；
- source sequence/timestamp；
- ADC full scale；
- 外部 `zero_current_confirmed` 前提。

算法使用有界 Welford 在线均值和方差，不保存原始样本数组。拒绝：

- 未确认零电流前提；
- 通道无效或 ADC 越界；
- 重复/倒序 sequence 或 timestamp；
- 相邻输入突变；
- 样本不足；
- offset 超出物理范围；
- 噪声 RMS 超限。

确定性 Host 结果：

```text
samples=64
offset_a=2042.98 counts
offset_b=2052.02 counts
offset_c=2048.50 counts
noise_rms_a=0.826009 counts
noise_rms_b=0.826011 counts
noise_rms_c=0.503953 counts
```

这些是合成数据结果，不是 Board1 的真实零点测量。另有独立失败测试覆盖：未确认前提、突变点、无效 mask、漂移导致的噪声超限和样本质量失效。

## CAL-01B｜方向与极对数辨识

采用明确参考模型：

```text
theta_e = wrap(direction * pole_pairs * theta_m + offset)
```

仅接受带 source sequence/timestamp 的机械角度和可信电角度参考对。纯机械角度数据会返回 `UNIDENTIFIABLE`，不会猜测方向或极对数。

候选搜索范围为 `1..20` 极对数、方向 `+1/-1`。使用 circular offset、最大残差、RMS 残差和 best/second-best separation 进行判定。

确定性参考数据生成的结果：

```text
direction=-1
pole_pairs=7
offset=1.199989 rad
residual_rms=0.0004121638 rad
residual_max=0.0005190372 rad
candidate_separation=1.505348 rad
samples=40
```

失败数据覆盖：

- 机械运动跨度不足；
- 被动机械角度无电角度参考；
- sequence/timestamp 乱序；
- 非法参考数据；
- 多候选不可区分。

## CAL-01C｜电角度零偏

在 direction 和 pole pairs 已知的前提下，使用 circular mean 和 circular residual 求解 offset，避免普通算术平均在 `0/2pi` 边界失效。

确定性边界数据结果：

```text
direction=-1
pole_pairs=7
electrical_offset=6.199975 rad
residual_rms=0.0004019763 rad
residual_max=0.0005247593 rad
samples=20
```

覆盖正负 offset、0/2pi 边界、角度噪声、异常参考点、不一致参考和缺少已知方向/极对数。

## CAL-01D｜结果有效性与 identity

每个结果保留：

- ADC full scale；
- current gain；
- encoder counts/revolution；
- encoder direction；
- phase order；
- pole pairs；
- sample count；
- residual/noise；
- state/failure。

`foc_calibration_identity_matches()` 对全部 identity 字段进行匹配。测试确认 pole pairs、方向或其他配置变化都会拒绝旧结果；校准结果不会因为瞬时传输异常自动被删除，也不会自动应用。

## AC01–AC12｜算法与边界验收

| AC | 结果 | 证据 |
| --- | --- | --- |
| AC01 Calibration 独立构建 | PASS | `cmake -S calibration -B build/calibration -G Ninja` |
| AC02 三相偏置正常估计 | PASS | 64 样本、三相不同偏置、在线均值/噪声统计 |
| AC03 噪声/漂移/异常/样本不足 | PASS | 白噪声、突变、慢漂移、样本不足失败测试 |
| AC04 ADC 非法输入与质量拒绝 | PASS | invalid mask、越界、未确认零电流、乱序测试 |
| AC05 编码器方向辨识 | PASS | 明确机械/电角参考模型，direction=-1 |
| AC06 极对数与不可辨识 | PASS | pole_pairs=7；被动角度/跨度不足返回不可辨识 |
| AC07 电角度零偏 | PASS | offset=6.199975 rad，圆周残差通过 |
| AC08 角度回绕与圆周残差 | PASS | `0/2pi` 边界和异常参考点测试 |
| AC09 校准状态与配置匹配 | PASS | state/failure/identity matching 测试 |
| AC10 多实例隔离 | PASS | 两个 Rotor context 交错、独立失败状态 |
| AC11 非法数据不破坏已确认状态 | PASS | failed collect 不提交新的候选；reset 才重新开始 |
| AC12 Board1 参数映射验证 | PASS | Host identity/字段映射契约验证，未自动覆盖 Board1 静态配置 |

## AC13–AC16｜回归与工程门禁

本轮重新验证：

- Calibration CTest：`1/1 PASS`；
- Sensor CTest：`1/1 PASS`；
- Core：`8/8 PASS`；
- Runtime/Pipeline：`6/6 PASS`；
- G3-NP：`2/2 PASS`；
- G3-SAFE：`1/1 PASS`；
- Board/UART/Python：`3/3 PASS`；
- Protocol：`1/1 PASS`；
- Legacy Host：`45/45 PASS`；
- Legacy Bench：PASS；
- FalconFOC source contract：54 个 C/H、8 个 CMake、8 个 README；
- C/H formatter：PASS，包含 calibration/ 和 sensors/；
- FalconFOC scoped quality gate：PASS；
- `git diff --check`：PASS。

## AC14｜ASan / UBSan

Clang sanitizer 真实执行，配置为：

```text
LLVM Clang 22.1.8
-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
```

实际结果：

```text
Core         8/8 PASS
Sensors      1/1 PASS
Calibration  1/1 PASS
G3-NP        2/2 PASS
G3-SAFE      1/1 PASS
Runtime      6/6 PASS
Protocol     1/1 PASS
Board        3/3 PASS
Total       23/23 PASS
```

无 AddressSanitizer、UndefinedBehaviorSanitizer 或 runtime error 诊断。

## AC15｜ARM 构建与资源

AT32F415 V2 固件重新构建通过，且生产固件没有自动链接或应用校准结果。Calibration 源文件另外用 Arm GNU Toolchain `14.3.1`、`cortex-m4 / Thumb-2 / soft-float` 完成编译检查：

```text
calibration_foc_calibration.o
text=4100 B, data=0 B, bss=0 B
最大静态函数栈帧=104 B（foc_calibration_adc_init / rotor_init）
```

该对象是独立离线算法编译证据，不代表已进入运行固件或 H0/H1 实时路径。

现有 V2 ARM 固件仍保持 Sensor-01 产物：

```text
.text=33076 B
.rodata=1312 B
.data=84 B
.bss=2392 B
linker RAM=3504 B
```

## 未解决边界与停止点

- 真实 ADC 零点未测量；
- 零电流前提仍由外部硬件/安全流程负责；
- 实际磁编码器安装方向未确认；
- 真实极对数未辨识；
- 真实电角度 offset 未标定；
- 没有真实定子电压对齐或转子运动流程；
- 没有写入 Flash 或应用运行配置；
- 没有实际 MCU 校准执行时间验证；
- G3-SAFE 硬件门禁未通过；
- G3-PWR 继续锁定。

本轮完成后停止，不自动进入真实硬件校准或下一阶段。

## 可复核命令

```powershell
cmake -S calibration -B build/calibration -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/calibration --parallel 4
ctest --test-dir build/calibration --output-on-failure
powershell -NoProfile -ExecutionPolicy Bypass -File build/rc0/run-sanitizer.ps1
cmake --build build/rc0/baseline-arm --parallel 4
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/flows/host/run_host_tests.ps1 -RepoRoot D:/study/Software/Hollow-Gimbal
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/gates/check_falconfoc_source_contract.ps1 -RepoRoot D:/study/Software/Hollow-Gimbal
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/format_themis.ps1 -Scope c -Mode check
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/gates/check_themis_quality_gate.ps1 -RepoRoot D:/study/Software/Hollow-Gimbal -Scope falconfoc
```

/* ---------------------------------------- end of file ----------------------------------------- */
