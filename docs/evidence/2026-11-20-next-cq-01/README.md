# NEXT-CQ-01 原始证据

[验收报告](../2026-11-20-next-cq-01.md) · [原始证据压缩包](raw-evidence.zip) · [压缩包 SHA-256](raw-evidence.sha256)

| 压缩包路径 | 内容 |
| --- | --- |
| `baseline-manifest.json`、`cq-working-tree-delta.json` | 变更前工作树文件哈希、HEAD、暂存状态，以及本轮验证过的代码和文档改动。 |
| `baseline-evidence/`、`attribution/` | 冻结源码重建后的历史 Current、Servo、两套 36 Hz FCAL 文件，以及初轮组合 Oracle 和单因素对照。 |
| `reviewed-results/`、`reviewed-current-cycle-summary.json` | 最终两套 130 Hz 应用/重载，兼容 36 Hz 文件重载，每组 A–F 逐 H0 观测及三个扰动工况。最终 C 仅替换用于整定的 R/L。 |
| `discrete-rl-model.json` | 单轴离散模型的输入、极点、采样和更新裕量。 |
| `historical-current/` | 原 CSV、状态及穿线记录，低压缺陷红绿、GDB、回归源和复跑方式，Current Step 计时与 ARM 静态资源。 |
| `servo/` | 原始 Servo Holdout、冻结门槛、噪声和过载的逐项消融、独立 Servo-02D 和跨模式对照。 |
| `build-and-regression/`、`arm-bin-sha256.txt` | 多工具链、Python、Legacy、Strict、Source Contract 和锁定构建日志，包含以冻结数据复现收益门槛失败的记录。 |
| `diagnostic-startup/` | Windows 环境初次重试缺 PATH 的 ASan/Bash 启动诊断。补齐运行时后通过，最终日志见构建目录。 |
| `sha256-manifest.json` | 压缩包内每个证据文件的 SHA-256。 |

在 VM01A 工作树根目录执行以下命令。将 `$evidence` 指向解压目录，将 `$results` 指向尚未创建的输出目录。Python 3 用于验证两个配置的逐周期时序、限压和重载。

```powershell
cmake -S simulation -B build/cq01-replay -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=clang
cmake --build build/cq01-replay --target falconfoc-virtual-calibrate
& "$evidence/reproduce-current-study.ps1" -Runner (Resolve-Path 'build/cq01-replay/falconfoc-virtual-calibrate.exe') -OutputDirectory $results -BaselineDirectory "$evidence/baseline-evidence"
python "$evidence/analyze-current-cycles.py" "$results"
```

历史 Current 和 Servo 复跑分别见包内 `historical-current/reproduction/README.md` 和 `servo/README.md`。`check-frozen-current-benefit.py` 检查变更前已经保存的 36 Hz 输出，Generic 按预期触发断言。
