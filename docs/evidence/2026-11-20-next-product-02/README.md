# NEXT-PRODUCT-02 原始证据与复跑入口

[验收报告](../2026-11-20-next-product-02.md) · [原始证据](raw-evidence.zip) · [SHA-256](raw-evidence.sha256)

| 包内目录 | 内容 |
| --- | --- |
| `baseline/` | 冻结工作树和收尾提交身份、验证源文件快照、CQ-03 一致性、独立构建和可达域入口的基线失败证据。 |
| `consumer/` | 三工具链的外构建、真实命令及链接清单、移动安装前缀、外拷 Servo/Calibration。 |
| `timing/` | 可执行 Host 计时和软件时间边界、两次测量、ARM map/反汇编/.su、调用及栈分析。 |
| `reachability/` | 独立 Virtual Motor 夹具、signed 逐拍、Python 方程及电流区间、84 次生产 CLI、CQ-03 Salient 原 CSV。 |
| `workflow/` | 正常 CLI 能力、审核、应用、诊断、STOP/恢复、两套 FCAL 和重载，另含错误输入与身份。 |
| `validation/` | 工具链矩阵、Source Contract、Strict、Board Host、Legacy、最新锁定 ARM 和历史质量复跑。 |
| `cq03/startup/` | 两套参考 FCAL、CQ-03 评分器、四模式事件和启动轨迹，用于独立复跑和比较。 |

从 [外部消费者 README](../../../examples/external-consumer/README.md) 复制公开的最小构建命令，归档的 `consumer/SOURCE-LAYOUT.md` 记录独立证据布局。使用 `timing/run.py <repo> <fresh-output>` 重建 ARM 与 Host 计时，完整环境见 Timing README。使用 `reachability/README.md` 重跑 Plant 观测和 CLI 数学核查。若使用当前源码复跑历史夹具，保留工作树中 CQ-03 的控制基线并核对控制输出包的哈希。

C11 Host 标定程序和生产 Python wrapper 可以使用如下命令重跑。输出目录须新建，参数为完整路径。

```powershell
cmake -S simulation -B build/product02 -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Debug
cmake --build build/product02 --target falconfoc-virtual-calibrate
python -B -X utf8 <evidence>/workflow/verify-workflow.py <repo> <runner> <evidence>/cq03/startup <fresh-output>
```

`baseline/working-tree-delta.json` 存档改动前后哈希。`sha256-manifest.json` 为压缩包内逐文件校验。Host 软确认、ARM 静态证据及实机验收仍按报告分开记录。
