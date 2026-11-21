# NEXT-CAL-ARCH-02 原始证据

[验收报告](../2026-11-20-next-cal-arch-02.md) · [原始证据 ZIP](raw-evidence.zip) · [SHA-256](raw-evidence.sha256)

| 包内目录 | 内容 |
| --- | --- |
| baseline/ | 开工 HEAD、工作树、已有 PRODUCT-02 差异和 ARCH-01 门禁红绿结果。 |
| product/ | CLI 复跑脚本和每条命令的真实 JSONL 结果。 |
| fcal/ | 36 Hz 与 130 Hz 归档 FCAL、数值兼容验证和旧 CLI 的 CQ-03 字节一致性对照。 |
| validation/ | 三工具链模块日志、最终专项复跑、Python CLI、源合同、Strict、应用中断红绿和 ARM 锁定构建。 |
| historical/ | Current 原始对照及 0.12 V 签名复跑，Servo 冻结联合/轴级试验与评分。 |
| source/ | 本轮代码及文档的最终校验快照。 |

从 VM01A 工作树运行以下命令。证据 ZIP 解压到 `<evidence>`，使用新的构建和输出路径。

~~~powershell
cmake -S simulation -B <host-build> -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Debug
cmake --build <host-build>
ctest --test-dir <host-build> -R 'test-foc-virtual-calibration-(cli|reuse|reuse-cli)$' --output-on-failure
python -B -X utf8 <evidence>/product/verify-reuse-with-events.py <host-build>/falconfoc-virtual-calibrate.exe <fresh-output>
python -B -X utf8 <evidence>/fcal/verify-historical-fcal.py <host-build>/falconfoc-virtual-calibrate.exe <fresh-fcal-output>
~~~

历史 Servo 脚本以及完整输入位于 CQ-03 的原始证据包。Current 脚本使用包内的冻结 CSV/JSON 以及同目录的新 after 目录。工具链和 ARM 参数记录在验证日志和命令清单中。sha256-manifest.json 记录包内各文件的 SHA-256。 [CQ-03 证据](../2026-11-20-next-cq-03/README.md)

