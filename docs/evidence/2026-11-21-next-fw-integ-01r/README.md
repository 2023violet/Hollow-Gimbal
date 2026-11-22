# NEXT-FW-INTEG-01R 原始证据

[报告](../2026-11-21-next-fw-integ-01r.md) · [ZIP](raw-evidence.zip) · [SHA-256](raw-evidence.sha256)

| 目录 | 内容 |
| --- | --- |
| `repro-before/A–D` | 在 `git archive aaad463` 导出树上的修复前复现：Capture 镜像与导入/Locked Timing 输出、供应 ID 配置/构建/测试日志与汇总、`review_id = 0` 探针、旧 `run-final.sh`（哈希与原清单一致）在全部失败桩下顶层退出 0 的日志 |
| `verify-after/A–C` | 修复后同条件验证：同一夹具与命令的导入/Locked Timing 结果、新旧镜像逐字节比较、供应 ID 输入矩阵与接纳探针、C 静态断言编译日志、`review_id` 探针 |
| `run2/` | 单次全量真实运行：`steps.tsv`（27 步原始退出码）、`verdict.json`/`verdict.txt`、三工具链矩阵日志与状态、启动测试见证、五个 ARM 镜像 ELF/BIN/HEX/map、`static/`（逐镜像检查、`main`/选择/资格/适配器反汇编、`nm`、`size`、`.su`、符号增删）、`uart/`、Legacy 预设与质量门禁日志、`fault-injection/`（19 例顶层故障注入）、211b226 同选项基线 ELF 与 UART 基线 |
| `source/` | 跟踪文件差异、新文件副本、工作树状态、受保护路径零差异记录 |
| `scripts/` | `scripts/flows/fwinteg/` 的全部文件 |

ZIP 内 `manifest.sha256` 记录每个成员的 SHA-256。全部结果来自 Host 或离线 ARM 工具链，无 MCU 执行。

```powershell
# 从仓库根目录（检出本轮提交）运行；输出目录必须不存在
bash scripts/flows/fwinteg/run_fwinteg.sh build/fwinteg01r/<新目录>
```

脚本自行从 `git archive 211b226` 构建同选项基线，不依赖 `build/` 中的忽略文件。依赖：CMake ≥3.25、Ninja、Clang（含 ASan/UBSan 运行库）、MinGW GCC、Arm GNU Toolchain、Python 3、pwsh、git、cmakelang。
