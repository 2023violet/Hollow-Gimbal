# NEXT-ADC-HW-01A 原始证据

[报告](../2026-11-21-next-adc-hw-01a.md) · [ZIP](raw-evidence.zip) · [SHA-256](raw-evidence.sha256)

| 目录 | 内容 |
| --- | --- |
| `offline/` | 25 个离线场景：生产 Writer 镜像、元数据、声明式见证（非物理）、导入归档、编排器与 `replay.py` 原始输出、Attestation 文件；`summary.txt` |
| `run1/` | 单次全量回归：27 步原始退出码、`verdict.json`、三工具链矩阵日志、五个 ARM 镜像、`static/`、`uart/`、Strict Scope all 与 Legacy 日志、故障注入汇总 |
| `build/` | 本轮 Host 工具构建日志与回归控制台输出 |
| `scripts/verify_offline.py` | 离线场景驱动 |

ZIP 内 `manifest.sha256` 记录每个成员的 SHA-256。全部为 Host 软件证据，无硬件访问。

```powershell
python -I scripts/verify_offline.py <firmware host build> <replay build> <new output dir>
bash scripts/flows/fwinteg/run_fwinteg.sh build/adchw01a/<新目录>
```
