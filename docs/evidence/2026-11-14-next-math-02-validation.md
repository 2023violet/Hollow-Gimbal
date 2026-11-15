# FalconFOC V2｜NEXT-MATH-02 独立数学验证

日期：2026-11-14  
分支：`codex/falconfoc-v2-rc0`  
基线 HEAD：`46bc963f5e5849e4440d1714a845fe485d338837`

## 结论

`NEXT-MATH-02 PASS（Host 独立数学验证范围）`

本轮没有修改生产控制算法、Runtime、Safety、Board、Protocol 或主程序。新增内容只包括独立 Host 参考测试、测试注册和本证据记录。通过并不等于 AT32F415 的 WCET、真实 ADC/PWM 时序或功率级行为已经验证。

## 独立参考覆盖

| 领域 | 参考方法 | 样本/覆盖 | 结果 |
| --- | --- | ---: | --- |
| Clarke、Park、逆 Park、SVPWM | 测试侧 double 重算；不调用生产变换作为期望值 | 10,000 变换 + 10,000 SVPWM | PASS；Park 最大绝对误差 d=`4.54459617e-5`、q=`4.26009622e-5`，逆 Park alpha/beta=`3.81469727e-6` |
| d/q Current PI | double 反馈变换、目标矢量限幅、比例项、积分试算和事务失败状态 | 4,096 个确定性轨迹，另有目标限幅边界 | PASS；`target_limited=1`、非法 `dt` 不提交状态 |
| PMSM/前馈 | 独立 double d/q 方程与四阶 Runge–Kutta PMSM 电流模型；比较生产前馈入口的符号和幅值 | 5,000 个正反电气速度、凸极参数和目标组合 | PASS；最大方程误差 `4.34276863e-6`，RK4 平衡点最大漂移 `4.4408921e-16` |
| 速度/位置/轨迹 | double 转子映射、速度 PI 抗饱和、位置 P 限幅和轨迹边界重算 | 5,000 个确定性随机输入 | PASS |
| 磁编码器 | 解析常速轨迹、16 位量化、unwrap、原始速度、LPF、alpha-beta tracker 和时间拒绝 | 5,000 个有效样本，包含环绕和时间间隔拒绝 | PASS；tracker 最大误差 `0.000827040791` |
| Float32 精度 | 生产 Float32 与 double 参考的误差统计；区分计算顺序/角度规约舍入和公式错误 | 全角度/大角度 API 输入、象限和边界样本 | PASS；当前 H0 在角度缓存中规约到 `[0, 2π)` 后使用三角函数 |

`test_foc_math_reference` 输出中的 `failures=1` 是专门验证非法 VBUS 的一次预期拒绝，不是失败样本。

## 新增文件与构建入口

- `core/tests/test_foc_current_reference.c`：独立 double Current PI 参考，覆盖测量 d/q、目标限幅、PI 积分和失败原子性。
- `core/tests/test_foc_pmsm_reference.c`：独立 PMSM d/q 微分方程、RK4 积分和前馈符号参考。
- `core/tests/test_foc_motion_reference.c`：独立速度/位置/轨迹参考（此前已加入，本轮纳入统一验收）。
- `sensors/tests/test_foc_sensor_reference.c`：独立编码器/速度估计/滤波参考（此前已加入，本轮纳入统一验收）。
- `core/CMakeLists.txt`：注册 `test-foc-current-reference` 和 `test-foc-pmsm-reference`。
- `sensors/CMakeLists.txt`：注册 `test-foc-sensor-reference`。

生产控制实现没有被测试替代；测试侧仅通过公共 API 取得实际结果，再用独立方程计算预期值。

## 实际验证

### Core 与 Sensors

```text
cmake -S core -B build/rc0/engineering-core -G Ninja
cmake --build build/rc0/engineering-core --parallel 4
ctest --test-dir build/rc0/engineering-core --output-on-failure
```

结果：Core `14/14 PASS`。

```text
cmake -S sensors -B build/rc0/engineering-sensors -G Ninja
cmake --build build/rc0/engineering-sensors --parallel 4
ctest --test-dir build/rc0/engineering-sensors --output-on-failure
```

结果：Sensors `2/2 PASS`。

### 全工程 Clang ASan/UBSan

```text
powershell -NoProfile -ExecutionPolicy Bypass -File build\rc0\run-sanitizer.ps1
```

结果：Core `14/14`、Sensors `2/2`、Calibration `1/1`、G3-NP `2/2`、G3-SAFE `1/1`、Runtime `8/8`、Protocol `1/1`、Board `4/4`，合计 `33/33 PASS`。未出现 ASan 或 UBSan 诊断。

### 既有回归与质量门禁

- Host Debug：`45/45 PASS`。
- SYSTEM replay：`3f01f6e3`，`updates=96`，`h0_due=103`，`h1_due=41`。
- Long replay：`49dea950`，`updates=99998`，`due=100001`，`missed=0`，`late=0`，`budget=0`。
- Source contract：`66 C/H`、`8 CMake`、`8 README`，PASS。
- C/H formatter：PASS。
- `Scope falconfoc`：PASS。
- `Scope all`：在当前 PowerShell 临时前置现有 `D:\Git\bin;D:\Git\cmd` 后 PASS；Firmware CLI contract PASS。
- `git diff --check`：PASS（仅报告既有 LF/CRLF 转换提示，无 diff 错误）。

### AT32F415 构建与静态边界

```text
cmake --build build/rc0/baseline-arm --parallel 4
```

结果：PASS。当前 V2 ELF `arm-none-eabi-size`：`.text=36,644 B`、`.data=92 B`、`.bss=3,544 B`、总计 `40,280 B`；独立测试目标未链接进该 ARM 固件。

当前主要符号尺寸：`foc_current_step_internal=0x39a`、`foc_runtime_h0_step=0x210`、`foc_runtime_h1_step=0x378`、`foc_encoder_update=0x404`、`foc_runtime_control=0x464`、`foc_pipeline_step_guarded=0x36a`。这些是静态符号信息，不是实测 WCET。

本轮 ARM 产物 SHA-256：

```text
ELF  51D7428FF2414FA4623DCE5086A6CD8D4F69058DFC6405B317DC28875E281551
BIN  9806ADBFA3282450527F40BB97CB1DA75FAFE41FB405F1126114187C6393B029
HEX  B80C263E6B07CE976E2BF1AEDED2737EED536C7224369091DC0919CAA48C51AE
MAP  BDC648824C427C873259638A30EB2FAE0C59E1598EBEA3C2C43E0C29CF6516B3
```

## 性能与后续边界

本轮没有优化热路径，没有把 Host 时间换算成 MCU 周期，也没有宣称 WCET 合格。当前最值得后续单独测量的热点仍是：

1. `foc_current_step_internal`：角度缓存、Float32 三角函数、限幅和反 Park/SVPWM 链。
2. `foc_encoder_update`：浮点速度、LPF 和 tracker 更新。
3. `foc_runtime_h0_step`：控制入口、前馈/轨迹门禁和 Pipeline 事务。

`G3-SAFE HARDWARE NOT VERIFIED`、`G3-PWR LOCKED`、`AT32F415 WCET NOT VERIFIED` 继续保持。

## 状态

`NEXT-MATH-02 PASS`（独立 Host 数学和数值验证范围）。本轮完成后停止，不自动进入算法优化、定点后端、传感器硬件接入或实板运行。
