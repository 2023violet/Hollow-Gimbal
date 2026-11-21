# NEXT-CAL-FLUX-01 原始证据

[验收报告](../2026-11-20-next-cal-flux-01.md) · [原始证据 ZIP](raw-evidence.zip) · [SHA-256](raw-evidence.sha256)

| 包内目录 | 内容 |
| --- | --- |
| baseline/ | 开工工作树、前置证据校验、旧算法的独立源码快照/插桩/复现包，以及本轮源文件前后哈希。 |
| parity/ | 1,024 个新生产 CLI 区间、方向估计、四模式审批/重载事件、数值包和双向限流记录。 |
| reuse/ | 实际组合命令、来源身份、显式拒绝和依赖撤销、重采及闭环 JSONL。 |
| fcal/ | 36 Hz 及 130 Hz 旧包复验，Startup V1 轨迹与原评分。 |
| validation/ | 三工具链回归、独立数值及灵敏度输出、负例红绿、Python CLI、Strict 和 ARM 资源。 |
| historical/ | 历史 Current 对照和低调制保护，Servo 冻结复跑与门槛。 |
| source/ | 当前实现、测试和相关文档的校验快照。 |

在 VM01A 工作树根目录运行。以下路径中的尖括号为待替换项，证据包与输出路径需提前解压或选用新的路径。

~~~powershell
cmake -S simulation -B <host-build> -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Debug
cmake --build <host-build>
ctest --test-dir <host-build> -R '^test-foc-(flux-id|virtual-calibration-(electrical|cli|reuse-cli))$' -V
python -B -X utf8 <evidence>/parity/verify-flux-parity.py <repo> <host-build>/falconfoc-virtual-calibrate.exe <evidence>/baseline/flux01a-baseline.zip <fresh-parity-output>
python -B -X utf8 <evidence>/reuse/verify-reuse-with-events.py <host-build>/falconfoc-virtual-calibrate.exe <fresh-reuse-output>
~~~

旧算法复现入口在内层 baseline/flux01a-baseline.zip 的 README.md 内。新生产 CLI 原始区间使用实际端电压源，旧包还含各端点 αβ 电压、角度和独立复算。sha256-manifest.json 校验外层各文件。 [CLI 源字段](../../../tools/falconfoc-cli/README.md#host-flux-evidence)

