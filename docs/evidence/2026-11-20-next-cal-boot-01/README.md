# NEXT-CAL-BOOT-01 原始证据

[验收报告](../2026-11-20-next-cal-boot-01.md) · [ZIP](raw-evidence.zip) · [SHA-256](raw-evidence.sha256)

| 目录 | 内容 |
| --- | --- |
| `baseline/` | 起始 HEAD、分支、工作树，前置证据包及相关源码冻结校验。 |
| `product/` | 两夹具两种新辨识模式的开路原始样本、审核和拒绝事件、FCAL v1、复用交互、正式 Flux 区间及取消试验。 |
| `compatibility/` | 历史 Flux/FOC、Current Startup V1、历史 FCAL、历史 Current 与 Servo 的实跑回放数据。 |
| `validation/` | 三工具链详细测试、门禁、Host 对象尺寸和锁定 ARM 构建。 |
| `reproduction/` | Bootstrap、Flux/取消、三工具链及历史兼容脚本。 |
| `source/` | 本轮关键标定、复用、观测、CLI 和测试源码快照。 |

`bootstrap-events.jsonl` 每行是一条 CLI 调用，含参数、输入及其全部 JSON 事件。`reuse-encoder-change.jsonl` 每行是一条交互命令及原生响应。负向采集 CSV 可仅含表头，表示在首次合格采样前被拒绝。ZIP 内 `manifest.sha256` 记录每个数据成员的 SHA-256。

在仓库根目录构建 Simulation 后，从已解压的归档调用下列脚本。将两个输出目录指向尚不存在的路径。

```powershell
$archive = 'D:/path/to/extracted-boot-evidence'
python -B -X utf8 "$archive/reproduction/verify-bootstrap-with-events.py" "$PWD" "$PWD/build/boot-repro/falconfoc-virtual-calibrate.exe" "$PWD/build/boot-evidence"
python -B -X utf8 "$archive/reproduction/verify-flux-and-cancel.py" "$PWD/build/boot-repro/falconfoc-virtual-calibrate.exe" "$PWD/build/boot-flux-evidence"
powershell -NoProfile -ExecutionPolicy Bypass -File "$archive/reproduction/run-test-matrix.ps1" -OutputRoot "$PWD/build/boot-matrix" -Toolchain clang
```

矩阵脚本也接受 `gcc` 和 `sanitizer`。历史回放脚本读取前序仓库证据或冻结输入，完整调用记录在报告和回归日志中。所有观测和应用回执均来自 Host。
