# FW-ADC-01 原始证据

[报告](../2026-11-21-fw-adc-01.md) · [ZIP](raw-evidence.zip) · [SHA-256](raw-evidence.sha256)

| 目录 | 内容 |
| --- | --- |
| `baseline/` | 开始时工作树状态与差异统计；受影响通道（Board Host、Calibration、Replay 正式过滤）的修改前构建与 CTest 日志 |
| `scenarios/` | 10 个场景的真实输出：生产 Writer 镜像、元数据、零电流见证（Host 夹具声明，非物理见证）、导入归档（含 ADC_ZERO Trace `samples.csv`）、编排器与适配器 JSON；`summary.txt` |
| `run1/` | 单次全量运行：27 步原始退出码、`verdict.json`、三工具链矩阵日志与状态、启动测试见证、五个 ARM 镜像 ELF/BIN/map、`static/` 检查、`uart/`、Legacy 预设与 Strict Scope all 日志、故障注入汇总 |
| `source/` | 跟踪文件差异、本轮新增与修改文件副本、工作树状态 |
| `scripts/capture_evidence.py` | 生成 `scenarios/` 的驱动脚本 |

ZIP 内 `manifest.sha256` 记录每个成员的 SHA-256。全部输入为 Host 合成数据，无 MCU 执行。

```powershell
bash scripts/flows/fwinteg/run_fwinteg.sh build/fwadc01/<新目录>
python -I build/fwadc01/capture_evidence.py <host build dir> build/fwadc01/<新场景目录>
```
