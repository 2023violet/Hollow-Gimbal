# NEXT-CQ-03 原始证据与复跑

[验收报告](../2026-11-20-next-cq-03.md) · [Host 控制品质 V1](../../servo/host-control-quality-v1.md) · [原始证据包](raw-evidence.zip) · [负载诊断](load-diagnostics.zip) · [包 SHA-256](sha256.txt)

| 路径 | 内容 |
| --- | --- |
| `baseline/` | Git 历史、reflog、完整工作树冻结哈希、CQ-02 一致性、历史 Current 和 48 Hz 复跑。 |
| `startup/` | 两配置应用/重载原始 JSONL、Startup V1 逐 H0 CSV、逐场景评分与 FCAL。 |
| `candidates/` | 预设判据、A/B/C 逐周期及负向场景、额外 2.20 A 对照、隔离实现和复跑脚本、ARM 资源证据。 |
| `servo/` | 原冻结 02D 回放、验收摘要、Velocity/Position 和 Sensor Fault。 |
| `validation/` | 构建及回归矩阵、Board Host、Legacy、ARM 段和哈希。 |
| `load-diagnostics.zip` | 正反向近转矩与超载、逐拍比较、解析模型、STOP/FAULT 和复跑。 |
| `sha256-manifest.json` | 原始包内每份资料的 SHA-256。 |

可从 VM01A 根目录重建 Startup V1。使用 Clang、CMake、Ninja 与 Python 3。

```powershell
cmake -S simulation -B build/cq03 -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=clang
cmake --build build/cq03
ctest --test-dir build/cq03 -R 'test-foc-virtual-calibration-cli' --output-on-failure
$exe = Resolve-Path 'build/cq03/falconfoc-virtual-calibrate.exe'
& $exe --profile-id GENERIC_RESEARCH_MOTOR --cogging
# 将 $reviewId 设为上述 CANDIDATE_PREVIEW 的 review_id。
& $exe --profile-id GENERIC_RESEARCH_MOTOR --cogging --approve-candidate $reviewId --cq-startup-csv startup-generic.csv
python simulation/tests/current_startup_quality.py GENERIC_RESEARCH_MOTOR startup-generic.csv
```

Salient 使用 `CALIBRATION_SALIENT_MOTOR` 的独立审核。它的 V1 评分退出码 1 保留大阶跃失效。按 `candidates/README.md` 复现隔离候选，按负载附包的 README 复跑 signed load harness。旧 10 ms、48 Hz 与已归档的全部历史证据见 [CQ-02](../2026-11-20-next-cq-02/README.md)。本轮报告的候选策略是 Host 隔离试验。

重跑冻结动态判定时，用同一 Clang Simulation 构建生成 `test-foc-impedance-robustness` 和 `falconfoc-virtual-motion`。执行包内 `servo/reproduction/run-frozen-dynamics.py <repo> <build> <fresh-output>`，脚本会复用附包中的原夹具和评估器，核对两个逐周期回放的哈希，并复跑 Velocity/Position 对照。
