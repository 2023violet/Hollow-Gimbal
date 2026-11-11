# FalconFOC V2 NEXT-FOC-01 电流闭环验证

本地工作树基于 `1f542521189d5e0ef25e64e6530608ece8d6e137`，含本轮开始前已有的 G2、G3 和规范整改。证据范围为 Host 数值测试、模型闭环、Legacy 回归和 Bench 编译。

## 实现与故障行为

- `foc_current_init/reset/step` 管理独立的 d/q PI 状态。采用 `I_trial = I_prev + Ki * dt_s * error`，输出请求为 `Kp * error + I_trial`。使用 G2 的 Clarke、Park、逆 Park 与 SVPWM。
- 对目标电流和电压各施加保方向的合成矢量限幅。电压圆形上限取配置值与 `VBUS / sqrt(3)` 的较小值，SVPWM 继续执行配置的逐相占空比限制。
- 抗饱和使用由最终占空比和 VBUS 还原的理想等效电压，限制本周期积分增量，允许朝退饱和方向积分。增量改变时重新计算一次调制。`Ki=0` 的积分保持为零。
- 计算失败时整份输出清零，积分状态保留。极端母线电压与电压上限比值可能使浮点占空比的分辨率不足。若还原电压越过上限且超出浮点容差，返回 `FOC_STATUS_ERR_VOLTAGE_LIMIT`。
- 先将新测试加入 CMake，构建因缺少 `foc_current.h` 失败。实现后，另用 `VBUS=1e8 V` 和 `5 V` 上限复现占空比舍入越限，确认断言先失败，再补最终电压检查。既有 G2 测试预期保持原值。

## 固定转子闭环结果

模型初始电流为零，电角度固定为 `0.37 rad`，电角速度为零。参数为 `Rs=0.5 Ω`、`Ld=1 mH`、`Lq=1.5 mH`、`VBUS=12 V`、电压矢量上限 `2 V`、电流目标矢量上限 `50 A`。`Kp_d/q=(1, 1.5) V/A`，`Ki_d/q=(500, 500) V/(A·s)`。模型从最终三相占空比减去浮动中点，再乘以母线电压并转换到 d/q。每步使用零阶保持精确式 `i_next = exp(-R*dt/L)*i + (1 - exp(-R*dt/L))*v/R`。

每级目标持续 `60 ms`。末尾 `10 ms` 窗口中，两轴绝对误差的最大值必须不超过 `0.010 A`，即以 `1 A` 目标阶跃为基准的 `1%` 模型测试门限。各合法步长均执行以下四级目标。

| d/q 目标 | 结果 |
| --- | --- |
| `(0, 1) A` | 从零跟踪正 q 轴，未饱和 |
| `(0.5, -1) A` | d 轴上升，q 轴反向，两轴均收敛 |
| `(5, 8) A` | 目标超出 `2 V` 下的稳态能力。全程受限，最终约为 `(1.897986, 3.521030) A` |
| `(-0.5, -0.75) A` | 退出长期饱和后两轴回到可达目标 |

| 步长 | 可达目标窗口最大误差，日志保留六位小数 | 恢复阶段报告饱和的周期数 |
| --- | ---: | ---: |
| `50 µs` | `0.000001 A` | `36/1200` |
| `100 µs` | `0.000000 A` | `19/600` |
| `200 µs` | `0.000000 A` | `10/300` |
| `1 ms` | `0.000001 A` | `2/60` |

模型每周期独立还原电压并断言矢量模长不超过 `2.00002 V`。另用占空比范围 `[0.4, 0.6]` 执行 `3000` 次持续饱和，验证 d 轴积分停在 `1.6 V`，目标反向时先退到 `1.1 V`，继续反向时受限于 `-1.6 V`。

## 构建与验收

在仓库根目录执行。三个独立工程使用 Clang，Bench 使用 ARM GNU `14.3.1` 与 `cortex-m3-soft`。

| 范围 | 命令或检查 | 本轮结果 |
| --- | --- | --- |
| Core | `cmake -S core -B build/core-next-foc01 -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Debug`，`cmake --build build/core-next-foc01 --parallel`，`ctest --test-dir build/core-next-foc01 --output-on-failure` | 实现前 `3/3`，实现后 `4/4`。闭环数据由 `./build/core-next-foc01/test-foc-current.exe` 输出 |
| G3-NP | `cmake -S g3/np -B build/g3-np`，`cmake --build build/g3-np --parallel`，`ctest --test-dir build/g3-np --output-on-failure` | `2/2` |
| G3-SAFE | `cmake -S g3/safe -B build/g3-safe`，`cmake --build build/g3-safe --parallel`，`ctest --test-dir build/g3-safe --output-on-failure` | `1/1` Host 契约测试 |
| Legacy Host | `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/flows/host/run_host_tests.ps1 -RepoRoot D:/study/Software/Hollow-Gimbal` | `45/45` |
| Legacy Bench | `cmake --preset fw-full-diag-carrier-bench-debug -B build/next-foc01-bench`，`cmake --build build/next-foc01-bench --parallel` | 全新目录完整编译并链接。BIN 为 `92,144 B`，与此前本地 Bench BIN 的 `11` 处字节差异均在 Shell 编译日期及时间字符串 |
| 源码与 CMake | `clang-format --dry-run --Werror` 检查本轮四个 C/H 文件。`python -m cmakelang.format --check -c .cmake-format core/CMakeLists.txt` | 通过。使用本机 clang-format `22.1.8`，仓库捆绑版本无法解析现有 `BlockIndent` 配置 |
| FalconFOC 专项门禁 | `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/gates/check_themis_quality_gate.ps1 -Scope falconfoc` | 通过 |
| 全仓门禁 | 同上，使用 `-Scope all` | 未闭合。FalconFOC 源码契约通过后，既有根 `CMakeLists.txt` 严格格式检查失败 |
| Firmware CLI 合约 | 在子进程 PATH 加入本机 Git Bash 后运行 `powershell -NoProfile -ExecutionPolicy Bypass -File tests/script/test_firmware_cli_contract.ps1` | 模拟合约通过。OpenOCD 调用由脚本桩记录 |
| 工作树检查 | `git diff --check`，新增文件逐个执行 `git diff --no-index --check` | 无空白错误 |

| 任务验收项 | 本地证据 |
| --- | --- |
| AC01–AC02 | 独立 C11 编译及双轴比例、积分、联合测试 |
| AC03–AC05 | 长时饱和、反向退出、共享电压包络、逐相占空比限制、四种 `dt_s` 等时积分 |
| AC06–AC08 | 非法输入后的状态重放、独立实例、由最终占空比驱动的固定转子模型 |
| AC09–AC10 | G2、G3 和 Legacy 回归，FalconFOC 专项及严格格式检查 |
| AC11–AC12 | 变更限于 Core 与本轮文档。实板带宽、电流波形、WCET、V2 Flash/RAM 占用和硬件安全仍无本轮证据 |
