# FalconFOC V2 NEXT-FOC-02 有感外环验证

工作树基线为 `1f542521189d5e0ef25e64e6530608ece8d6e137`，包含此前未提交的 NEXT-SW01A/SW01B。实施前用新目录复跑 Core `4/4`、Runtime `2/2`、G3-NP `2/2`、G3-SAFE `1/1`。先注册 `test-foc-motion`，构建以缺少 `foc_motion.h` 失败。完成独立算法后 Core `5/5`，随后用直接 Core 与 Mock 输出完成动态模型门限，再接入 Runtime。门限沿用直接模型运行前在测试中设定的值。

## 物理和事务契约

- 机械零点、传感器方向、相电流极性由调用方建立。转子位置用连续机械 rad，速度用机械 rad/s，转矩用 N·m。正 q 电流在与 ADC 相序匹配的 Park 约定下产生正向转矩。电角度为 `wrap(p * θm + offset)`，电角速度为 `p * ωm`。仅电角度归一化，多圈位置误差保持连续。单圈编码器的解圈由反馈生产者完成。
- H0 先用 R2 模时间判断源样本新鲜度及 ADC 时间关系，再调用恒速预测。最大外推误差贡献按 `0.5 * |αe|max * dt²` 计算。超时间、超误差、未来样本、非法速度及低质量数据拒绝正常 UPDATE。源时间保留在 H1 快照，H0 另发布预测间隔。机械反馈关闭时沿用 SW01B 的精确源时间对齐。
- 速度 PI 使用真实相邻 H1 采样间隔和对称 q 电流上限，抑制推动饱和的积分并允许反向解饱和，电流限值下降时收紧既有积分。位置 P 使用连续机械位置，速度目标受限。外环模式使用 `Id=0`。
- H0 单独管理模式、会话和内环，H1 单独管理外环。H1 将角度、目标、源命令序号/时间、模式及会话写入同一非发布槽，完成后切换索引。H0 经 Mock 接受后发布该 H1 样本序号的确认。H1 下次才提交匹配提案。同一目标被多个 H0 周期接受仍只提交一次外环积分。拒绝、停机或模式变更使旧提案失效。前台发布源命令，派生目标从不延长源有效期。
- 模式只在 DISABLED 且 Mock 确认 INHIBIT 时切换。新会话要求新源命令、边界后的机械传感器样本以及 ARM/START。H1 观察到边界后复位外环，H0 在 START 复位内环。Pipeline 可选派生目标与已经观测的源身份必须相符，原电流和电压开环入口继续有效。

## 模型、门限和结果

主模型使用 `p=4`、`Rs=0.6 Ω`、`Ld=1.2 mH`、`Lq=1.5 mH`、`ψf=0.025 Wb`、`J=0.002 kg·m²`、`B=0.006 N·m·s/rad`、电角偏置 `0.37 rad`。这些是合成参数。母线 `12 V`，电压矢量限 `2.4 V`，q 电流限 `2.5 A`，速度目标限 `12 rad/s`。ADC `2 mA/count`，PWM 周期 `6000 counts`，电压断言额外容差 `4 mV` 用于 compare 量化。

内环 `Kp_d/q=(1.08, 1.35) V/A`，`Ki_d/q=(540, 540) V/(A·s)`。速度 PI `Kp=0.18 A/(rad/s)`、`Ki=1.4 A/rad`，位置 `Kp=4 s⁻¹`。H0 `100 µs`、H1 `500 µs`、监督 `1 ms`、遥测 `10 ms`。预测窗口 `400 µs`、样本年龄 `600 µs`、电加速度假设 `2000 rad/s²`、外推误差预算 `0.0003 rad`。

幅值不变 Clarke/Park 下用 `3/2` 转矩及功率系数。测试先核对瞬时能量平衡及零输入静止。动态方程含 d/q 交叉耦合、反电势、惯量、粘性阻尼和负载。RK4 按 `100 µs` 步进，约为最小电气时间常数 `2 ms` 的 5%。每步从最终确认的三相 compare 和母线还原 α/β 电压，步内按转子角度更新 dq。INHIBIT 时停止模型推进。速度、负载增减和反转共 `5.4 s`，位置共 `5.2 s`，每组独立启动。

测试前以电气时间常数、速度闭环极点约 `10 rad/s`、位置增益 `4 s⁻¹` 和 `12 rad/s` 速度限确定门限。速度阶跃 10–90% 时间 ≤`0.65 s`，超调 ≤`2.4 rad/s`，稳定 ≤`1.10 s`。负载增减峰值速度偏差 ≤`2.0 rad/s`，恢复 ≤`0.95 s`。位置阶跃 10–90% 分别 ≤`1.40 s`、`1.55 s`，超调 ≤`1 rad`，稳定 ≤`2.10 s`。稳定时间取最后一次超出速度 `0.30 rad/s` 或位置 `0.08 rad` 容差的时刻。末 `150 ms` 的绝对误差门限为速度 `0.15 rad/s`、位置 `0.035 rad`。

| Runtime 场景 | 最终反馈 | 10–90% | 超调或扰动峰值 | 稳定或恢复 | 末段最大误差 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 静止 → `+10 rad/s` | `10.000168 rad/s` | `0.1012 s` | `1.073327 rad/s` | `0.4543 s` | `0.000269 rad/s` |
| `+10 rad/s` 加 `0.09 N·m` | `9.999651 rad/s` | — | `1.863264 rad/s` | `0.3613 s` | `0.000486 rad/s` |
| `+10 rad/s` 移除负载 | `10.000272 rad/s` | — | `1.863379 rad/s` | `0.3613 s` | `0.000387 rad/s` |
| `+10 → −10 rad/s` | `−10.000023 rad/s` | `0.1210 s` | `1.433323 rad/s` | `0.5134 s` | `0.000171 rad/s` |
| `0 → +7 rad` | `6.999540 rad` | `0.5397 s` | `0 rad` | `1.2569 s` | `0.000836 rad` |
| `+7 → −3 rad` | `−2.998761 rad` | `0.6946 s` | `0 rad` | `1.5079 s` | `0.002235 rad` |

负载增加时，稳态 q 目标和实际电流由约 `0.400 A` 升至 `1.000 A`，卸载后回到 `0.400 A`。反转期间 q 目标达到 `2.5 A` 限幅，量化 compare 还原的最大电压为 `2.401062 V`。同一速度与负载序列重放两次，三相 compare 轨迹指纹 `884b005e`，终态位置、速度、电流、两级积分和故障相同。两实例交错运行 `1.2 s`，主模型 `p=4` 达 `4.000127 rad/s`，第二模型 `p=3` 达 `−3.999896 rad/s`。第二模型用 `Rs=0.7 Ω`、`Ld=1.4 mH`、`Lq=1.7 mH`、`ψf=0.028 Wb`、`J=0.0016 kg·m²`、`B=0.0045 N·m·s/rad` 和偏置 `−0.21 rad`。主模型锁存后，第二实例仍接受 UPDATE。

实跑时在本地 `build/foc02-runtime/motion-trace.txt` 保存输出，共 `112` 条时间序列记录，包含每级首步及其后每 `0.1 s` 的数据。`trace` 行字段为模式、阶段、阶段时间、参考、位置、速度、q 目标、q 实际、负载、电压。一次包含输出记录的 Host 进程墙钟时间约 `782 ms`，与 MCU 单周期执行预算分别计量。

## 验收与复现

| 验收 | 本轮证据 |
| --- | --- |
| AC01–AC02 | 坐标、正反方向、偏置和极对数，匀速及加速外推，回绕、未来、非法速度和超窗拒绝 |
| AC03–AC05 | 独立速度 PI 的 P/I、实际 dt、抗饱和、反向恢复、降限、冻结及复位，连续位置 P 与速度限幅 |
| AC06–AC08 | 独立和 Runtime 动态闭环按预设门限验证正反速度、位置保持及负载增减 |
| AC09–AC10 | 三次模式迁移、RUNNING 拒绝、旧命令及旧角度拒绝、H0 拒绝不提交外环、H1 断流后保持禁止 |
| AC11–AC12 | 两机交错、确定性重放及故障隔离 |
| AC13–AC15 | Pipeline、Runtime、Core、G3-NP、G3-SAFE、Legacy Host 与 Legacy Bench 复跑 |

| 检查 | 命令与结果 |
| --- | --- |
| Core 与 Runtime | `cmake -S core -B build/foc02-core -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Debug`，同法配置 `runtime` 到 `build/foc02-runtime`。分别 `cmake --build <dir> --parallel` 与 `ctest --test-dir <dir> --output-on-failure`。Core `5/5`、Runtime `4/4` |
| 模型轨迹 | `./build/foc02-runtime/test-foc-motion-model.exe`，可将 stdout 保存到本地文件 |
| G3-NP / G3-SAFE | 同法独立配置、编译并运行 CTest，`2/2`、`1/1` |
| Legacy Host | `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/flows/host/run_host_tests.ps1 -RepoRoot D:/study/Software/Hollow-Gimbal`，`45/45` |
| Legacy Bench | `cmake --preset fw-full-diag-carrier-bench-debug -B build/foc02-bench` 与 `cmake --build build/foc02-bench --parallel`，完整编译和链接通过 |
| 规范 | `scripts/format_themis.ps1 -Scope c -Mode check`，`python -m cmakelang.format --check -c .cmake-format core/CMakeLists.txt runtime/CMakeLists.txt`，通过 |
| 专项 | `scripts/gates/check_themis_quality_gate.ps1 -Scope falconfoc`，通过。扫描 `29` 个 C/H、`4` 个 CMake、`4` 个 README |
| 全仓 | `scripts/gates/check_themis_quality_gate.ps1 -Scope all`，源码契约通过后停止于既有根 CMake 格式差异 |
| Firmware CLI | 本机 Git Bash 下单独运行 `tests/script/test_firmware_cli_contract.ps1`，模拟合约通过 |
| 工作树 | `git diff --check` 及逐文件 `git diff --no-index --check` 无空白错误，含未跟踪 Core、Runtime 和本记录。Legacy 源码与根 CMake 无差异 |

模型使用合成且可信的连续机械反馈。实际编码器的解圈、速度估计、误差与加速度范围、MCU 时序和功率级物理行为仍需独立证据。Bench 只验证 Legacy 构建。
