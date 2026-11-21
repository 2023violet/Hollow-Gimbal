# NEXT-FW-INTEG-01 原始证据

[报告](../2026-11-21-next-fw-integ-01.md) · [ZIP](raw-evidence.zip) · [SHA-256](raw-evidence.sha256)

| 目录 | 内容 |
| --- | --- |
| `baseline/` | 211b226 `git archive` 导出树的三套 Host 矩阵日志、默认 ARM ELF/BIN/map、`main` 与适配器反汇编、静态栈、UART demo 与 PARAM_READ 响应。 |
| `final/` | 单次最终运行 `final4` 的全部输出：三套 Host 矩阵日志、三套工具链启动测试见证、skip-check 计数、四个 ARM 构建日志、`static/` 静态检查（ELF 反汇编、`nm`、`size`、`.su`、新增/删除符号、`checks.txt`、`summary.txt`）、`uart/` CLI 读回、三个 Legacy 预设构建日志、质量门禁日志、25 步退出码汇总和 BIN 哈希。 |
| `arm/` | `final4` 默认与验证镜像的 ELF/BIN/HEX/map 原件。 |
| `source/` | 源码差异、新文件副本、工作树状态、受保护路径零差异记录。 |
| `scripts/` | `run-final.sh`、`run-test-matrix.ps1`、`arm_static.py`、`uart_check.py`、`compare_prologue.py`。 |

ZIP 内 `manifest.sha256` 记录每个成员的 SHA-256。全部结果在 Host 或离线 ARM 工具链上得到，无 MCU 执行。

```powershell
# 解压后把 scripts/ 下文件放到仓库 build/fwinteg/，baseline/ 放到 build/fwinteg/evidence/baseline，从仓库根目录运行
bash build/fwinteg/run-final.sh build/fwinteg/final4
```
