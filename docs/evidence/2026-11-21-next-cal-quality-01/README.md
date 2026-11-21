# NEXT-CAL-QUALITY-01 原始证据

[报告](../2026-11-21-next-cal-quality-01.md) · [ZIP](raw-evidence.zip) · [SHA-256](raw-evidence.sha256)

| 目录 | 内容 |
| --- | --- |
| `baseline/` | 起始工作树文件、哈希、已复核前置证据和本轮基线。 |
| `repeatability/` | 两种电机各六次实际采集，原始 Bootstrap、训练、LUT 和闭环验证轨迹，逐次误差和交叉 Session 预测。 |
| `robustness/` | 测量副本注入、热态、电角共模与方向、J/B 负载和 Kt 比例的原生测量。 |
| `product/` 与 `timing/` | 真实 CLI 命令和事件、Quick/Full/Selective 审核应用、FCAL、三条时间轴与统计。 |
| `compatibility/` | 历史 Current、Startup、转矩边界、Servo、Flux 和 v1 兼容回放。 |
| `validation/` | 三种当前 Host 工具链、Strict Source/Legacy/Python、ARM 重建、状态及哈希。 |
| `source/` 与 `reproduction/` | 关键实现、测试、矩阵和统计脚本。 |

ZIP 内 `manifest.sha256` 记录所有数据成员。大体积历史波形在前序包中的精确原版本和本轮重跑摘要以 SHA-256 绑定。另附主要新测的直接可用 CSV/JSON/JSONL、审阅和应用回执、失败诊断与测试构建日志。

从仓库根目录运行，先构建 `build/virtual-cal`。浏览归档中的 `reproduction/` 或使用仓库测试入口，输出目录选新路径。

```powershell
$archive = 'D:/path/to/extracted-quality-evidence'
$build = "$PWD/build/virtual-cal"
python -B -X utf8 simulation/tests/test_virtual_calibration_quality.py "$build/test-foc-virtual-calibration-quality.exe" --output-dir "$PWD/build/quality-repeated"
python -B -X utf8 simulation/tests/test_virtual_calibration_product_cli.py "$build/falconfoc-virtual-calibrate.exe" --output-dir "$PWD/build/quality-product"
python -B -X utf8 "$archive/reproduction/summarize-timing.py" "$PWD/build/quality-product" "$PWD/build/quality-timing"
& "$build/test-foc-virtual-calibration-robustness.exe" --trace
& "$build/test-foc-mechanical-id-virtual.exe" --quality --trace
pwsh -NoProfile -File "$archive/reproduction/run-test-matrix.ps1" -OutputRoot "$PWD/build/quality-matrix" -Toolchain clang
```

`run-test-matrix.ps1` 也接受 `gcc` 和 `sanitizer`。所有观测、闭环和应用均在 Host 执行。报告的板级证据为离线锁定构建。
