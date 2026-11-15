# FalconFOC V2｜NEXT-CONFIG-01 校准参数安全准入与控制链一致性

## 判定

`NEXT-CONFIG-01 SOFTWARE PASS`

本轮把 CAL-01 的三类离线候选重新作为外部输入，在控制器初始化前执行一次性质量、范围、身份和坐标准入，生成 `foc_calibration_snapshot`，并通过显式 Board1 Host 入口把快照映射到 ADC、Sensor 和 Rotor 配置。Profile A/B 的合成输入已穿过 ADC 解码、编码器方向、Rotor 电角度换算和 H1 发布链。

这个结论只覆盖 Host/静态软件路径。真实 ADC 零点、编码器安装方向、极对数、电角度零偏、SPI/ADC 时序、功率级和电机行为仍未验证。没有写 Flash、刷机、上电、启用 PWM、真实电机校准、提交或推送。

## 实际参数数据流

```text
CAL-01 ADC / Rotor / Offset Candidates
        ↓
Quality + Range + Identity + Coordinate Validation
        ↓
foc_calibration_snapshot (all fields committed or unchanged)
        ↓
foc_board_init_with_calibration() before producer start
        ├─ foc_device_adc_config.phase_a/b/c_offset_count
        ├─ foc_encoder_config.counts_per_revolution + direction
        └─ foc_runtime_config.motion.rotor.pole_pairs + electrical_offset_rad
        ↓
Sensors → Rotor Mapping → H1 coherent slot → H0 control input
```

CAL-01 的候选计算、状态和结果质量已经存在；本轮补齐的是候选身份的应用边界、完整快照、Board1 显式装配和失败原子性。没有增加 Parameter Manager、持久化、通信消息或运行时热修改。

## 单位、身份和方向契约

| 字段 | 约定 |
| --- | --- |
| ADC raw/offset | 12-bit ADC counts；offset 直接进入 `raw - offset`，current gain 只乘一次 |
| current gain | A/count；必须与 Board1 当前 `3.3/(4095*0.5)` 完全匹配 |
| phase order | `FOC_DEVICE_PHASE_ORDER_ABC`/`ACB` 数值身份，不能跨配置复用 |
| encoder counts | 65536 raw codes/revolution；Sensor 输出连续机械 rad |
| source encoder direction | raw code 到 CAL-01 机械参考坐标的符号，来自 `expected_identity` |
| calibration direction | CAL-01 拟合的电角度相对参考机械坐标的符号 |
| Runtime Sensor direction | `source_direction × calibration_direction`，只在准入层计算一次 |
| pole pairs | 正整数，进入现有 `foc_rotor_config` |
| electrical offset | rad，归一化到 `[0, 2π)`，只影响电角度，不改变速度符号 |

现有 Core 的 Rotor 映射为正极对数乘 Runtime mechanical coordinate，因此没有把 CAL-01 `direction` 当成第二个无条件反向开关。Board1 nominal source direction 为 `-1`；Profile B 的 CAL-01 direction 为 `-1`，解析后的 Runtime Sensor direction 为 `+1`，Host 测试验证了 raw code 增大时 mechanical position 为正，Rotor 仍按 `+pole_pairs * mechanical + offset` 计算。Profile A 使用 direction `+1` 时解析结果为 Board1 原有 `-1`。

## 原子准入实现

新增 `foc_calibration_admit()` 和 `foc_calibration_snapshot`：

- ADC candidate 必须 `VALID/usable`，样本数、三相 offset、噪声 RMS 和 identity 全部通过；
- Rotor candidate 必须 `VALID/identifiable`，direction 为 ±1、pole pairs 与期望一致、residual 和 identity 通过；
- Offset candidate 必须 `VALID/usable`，方向、极对数、offset 与 Rotor 结果一致；
- 任何 NULL、NaN/Inf、非法范围、质量失败、来源身份不一致或坐标不一致都拒绝；
- 只有完整候选组全部通过，临时 snapshot 才一次性写入调用者；失败保留调用者此前的 snapshot 内容；
- `foc_calibration_snapshot_valid()` 在 Board 映射前再次检查可表示性。

`foc_board_init_with_calibration()` 只接受已接受快照，先用局部 pipeline/runtime/encoder 配置完成初始化，成功后再提交输出日志、Runtime 和 Sensor policy。默认 `foc_board_init()` 没有改变，Board1 名义参数仍为 `2048/2048/2048` counts、7 极对、0 rad、反向编码器，前馈和轨迹继续关闭。已经初始化的 encoder 或已经配置过的 calibrated encoder 会被拒绝；没有运行中热修改。

## Host Profile A/B 数据链

`firmware/at32f415/tests/test_foc_board_config.c` 覆盖：

- Profile A：Board1 名义 ADC offset、7 极对、0 rad，解析后 Sensor direction 为 `-1`；
- Profile B：CAL-01 合成候选值 `(2042.98, 2052.02, 2048.50)` counts、direction `-1`、7 极对、1.2 rad，解析后 Sensor direction 为 `+1`；
- 相同 raw ADC 输入经过真实 `foc_device_decode_sample()` 后得到不同但可计算的三相电流，gain 没有重复应用；
- 真实 `foc_encoder_update()` 方向化输出经过 `foc_rotor_electrical()`，电角度与独立 `wrap(7*mechanical + offset)` 参考一致；
- 结果进入 Runtime H1 sensor slot，角度和速度来自同一发布快照；
- 默认 Board1 初始化、软件 INHIBIT 和 UPDATE 锁定行为保持不变；
- 已初始化 encoder、未接受快照和身份不匹配都会拒绝，旧输出/encoder 状态保持。

## 回归结果

| 检查 | 结果 |
| --- | ---: |
| Calibration CTest | `1/1 PASS` |
| Board/UART/Python Host | `4/4 PASS`（新增 `test-foc-board-config`） |
| Core | `10/10 PASS` |
| Sensors | `1/1 PASS` |
| Runtime/Pipeline | `7/7 PASS` |
| G3-NP | `2/2 PASS` |
| G3-SAFE | `1/1 PASS` |
| Protocol | `1/1 PASS` |
| Legacy Host | `45/45 PASS` |
| Legacy Bench | 构建通过 |
| Clang ASan/UBSan | `27/27 PASS`，无 sanitizer 诊断 |
| Source contract | `60 C/H、8 CMake、8 README PASS` |
| C/H formatter | PASS |
| 受影响 CMake formatter | PASS |
| `Scope falconfoc` | PASS |
| `git diff --check` | PASS |
| `Scope all` | BLOCKED，既有 `g3/np/CMakeLists.txt` cmake-format debt |

`Scope all` 的既有阻断没有在本轮修改；它不属于配置准入代码失败，但全仓质量门禁仍不能标记为 PASS。

## AT32F415 静态构建

命令使用 Arm GNU Toolchain `14.3.1`、`cortex-m4`、`cortex-m3-soft` CPU class、soft-float、`FPU=none`。V2 独立固件构建通过：

```text
.text   = 34,916 B
.rodata = 1,352 B
.data   = 84 B
.bss    = 2,520 B
Flash   = 36,736 B
RAM     = 3,632 B
```

新增入口的静态栈帧为 `foc_board_init_with_calibration = 376 B`；准入函数 `foc_calibration_admit = 168 B`，snapshot validator `24 B`。当前 `main` 仍调用默认 `foc_board_init()`，链接器垃圾回收后最终 ELF 没有链接 `foc_calibration_*` 或 `foc_board_init_with_calibration` 符号，因此默认 H0/H1 热路径没有新增准入调用。

产物 SHA-256：

```text
ELF  51D7428FF2414FA4623DCE5086A6CD8D4F69058DFC6405B317DC28875E281551
BIN  9806ADBFA3282450527F40BB97CB1DA75FAFE41FB405F1126114187C6393B029
HEX  B80C263E6B07CE976E2BF1AEDED2737EED536C7224369091DC0919CAA48C51AE
MAP  BDC648824C427C873259638A30EB2FAE0C59E1598EBEA3C2C43E0C29CF6516B3
```

## 未验证边界与停止点

- 合成 Profile B 不是实际设备的 ADC 零点、编码器安装方向、极对数或电角度零偏；
- 没有证明物理设备身份与软件 identity 是同一台电机；
- 没有真实 SPI/ADC 时序、滤波群延迟、MCU WCET、功率级响应或 WDT 证据；
- `foc_board_init_with_calibration()` 是显式初始化入口，未接入默认启动流程，也不实现在线参数更新、Flash 保存或 UART 校准命令；
- `G3-SAFE HARDWARE NOT VERIFIED` 和 `G3-PWR LOCKED` 保持不变；
- RC0 全仓软件状态仍受既有 `Scope all` 格式债务影响。

本轮完成后停止，不自动进入 Flash 参数存储、UART 校准协议或真实硬件应用。

## 可复核命令

```powershell
cmake -S calibration -B build/config-calibration -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/config-calibration --parallel 4
ctest --test-dir build/config-calibration --output-on-failure
cmake -S firmware/at32f415 -B build/config-board -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/config-board --parallel 4
ctest --test-dir build/config-board --output-on-failure
powershell -NoProfile -ExecutionPolicy Bypass -File build/rc0/run-sanitizer.ps1
cmake --build build/config-arm --parallel 4
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/gates/check_falconfoc_source_contract.ps1 -RepoRoot D:/study/Software/Hollow-Gimbal
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/format_themis.ps1 -Scope c -Mode check
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/format_themis.ps1 -Scope cmake -Mode check
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/gates/check_themis_quality_gate.ps1 -RepoRoot D:/study/Software/Hollow-Gimbal -Scope falconfoc
```

/* ---------------------------------------- end of file ----------------------------------------- */
