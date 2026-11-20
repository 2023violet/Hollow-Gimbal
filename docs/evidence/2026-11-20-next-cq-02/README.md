# NEXT-CQ-02 原始证据与复跑

[验收报告](../2026-11-20-next-cq-02.md) · [原始证据](raw-evidence.zip) · [负载诊断](load-diagnostics.zip) · [SHA-256](sha256.txt)

| 路径 | 内容 |
| --- | --- |
| `baseline-manifest.json`、`cq02-working-tree-delta.json` | 变更前实际工作树、逐文件哈希、HEAD、暂存区、与最终工作树的差异。 |
| `baseline/` | 冻结源码重建后运行的历史 Current、Servo 和双配置 130 Hz，含原始数据、命令所用参数和日志。 |
| `impedance/` | 同一原 Holdout 的 0/48 Hz 生产链回放、原门槛红绿验证，以及 ±3/±12 count 独立探针。 |
| `dynamics/`、`servo-frozen-evaluator/` | 冻结 Servo-02D 判据、两个 48 Hz 实现 A/B 的原始试验和判定、跨模式 A/B。 |
| `current/` | 逐拍 ADC、H0/H1、PI、电压、接收与实际输出。含独立解析与数值模型、冻结与当前版本复跑。 |
| `calibration/` | 两配置已审核的应用、FCAL 重载、兼容旧包、完整 H0 轨迹和汇总。 |
| `performance/`、`validation/` | H1 Release Host、锁定 ARM 对照和哈希、三工具链、Board Host、Legacy 和严格门禁。 |
| 独立 `load-diagnostics.zip` | 负载配对、静态近边界、两极性超载、卸载、STOP/FAULT 的完整 H0/H1 公开路径诊断、脚本和逐文件哈希。 |

在工作树根目录执行正式 Runtime 阻尼验收。

```powershell
cmake -S simulation -B build/cq02 -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=clang
cmake --build build/cq02
ctest --test-dir build/cq02 -R '^(test-foc-impedance-runtime-damping|test-foc-runtime-impedance|test-foc-virtual-calibration-cli)$' --output-on-failure
& ./build/cq02/test-foc-impedance-robustness.exe --trace
& ./build/cq02/test-foc-impedance-robustness.exe --runtime-damping --trace
```

旁路复跑仍输出原噪声、超载裁决。48 Hz 运行以同一输入和门槛验证可行原用例。冻结分析脚本 `impedance/check-impedance-holdout.py` 在原旁路轨迹上因两个噪声项失败，在 48 Hz 上成功。

解压后按 `current/README.md` 复现三点 Current 与冷启动模型。动态试验用 `dynamics/reproduce-cq02-dynamics.py --help`，将 Servo 冻结证据目录指向解压后的 `servo-frozen-evaluator/`。传入 `--fixtures` 可选择 CQ-02 的冻结源码路径。仅滤测量对照的隔离 Runtime 源码随包保存，证据中其生产路径对照使用同一份冻结 evaluator。完整负载压缩包内有 `README.md` 和 `run.ps1`，包含正反向、近边界及卸载的复现方式。

H1 计时驱动的 CMake 和 C 源在 `performance/h1-bench-src/`。使用 Release Clang 和 Ninja，传入源码绝对路径 `-DREPO=<repo>`、冻结 `-DHAS_FILTER=0 -DRUNS=9` 或本轮 `-DHAS_FILTER=1 -DRUNS=18`。计时仅包住 H1 测试入口，真实 H0 更新与 Foreground 发生在计时窗口之后。ARM 参考固件依仓库构建说明使用 Cortex-M4 Soft-float，归档 ELF 段统计及符号尺寸。
