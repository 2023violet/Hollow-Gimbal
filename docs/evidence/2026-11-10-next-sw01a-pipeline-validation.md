# FalconFOC V2 NEXT-SW01A 纵向闭环验证

基线为本地工作树 `1f542521189d5e0ef25e64e6530608ece8d6e137`，含此前未提交的 Core、G3 和质量门禁。本轮新增独立 Host 目标 `falconfoc-pipeline`。基线在新目录复跑 Core `4/4`、G3-NP `2/2`、G3-SAFE `1/1`。先加入启动、最终拒绝和恢复断言，首次构建以缺少 `foc_pipeline.h` 失败。实现后同组测试通过。

## 事务与恢复边界

- Device 解码原始 ADC 并保留源时间和序列。纯解码器接受零序列和零时间。原 G3-NP 电压开环模拟器仍独立拒绝零序列，回归测试覆盖两种行为。
- Pipeline 复用 Safety 的模序和模时间判断。只有半范围内前进的序列和新鲜的源时间能进入控制。命令按原始发布时间计龄。流水线保留命令及 ADC 的顺序记录，包含已消费的无效 ADC，故障清除不抹除重放边界。
- 首个无效样本立即发出 INHIBIT，并撤销运行许可。连续无效阈值由 Safety 决定，重放同一无效样本不增加次数。重新使能需要恢复边界之后的新命令和有效样本，以及明确的 ARM 和 START。START 在新会话中复位 PI。
- 先在副本上算电流 PI，再执行 Device 映射、最终 Safety Gate 和闭门 Mock 提交。只有完整 UPDATE 接受后提交副本。Mock 若拒绝 UPDATE，Pipeline 尝试 INHIBIT。若 INHIBIT 也失败，保留已知寄存器元数据并标记输出未确认。历史 UPDATE 仅供诊断。
- 单执行者负责所有调用。测试在映射之后注入时间流逝、显式停止或故障，检查最终 Gate 拒绝。Host 模型的 INHIBIT 阶段采用停止仿真，板上关断电流、异步抢占和控制周期之外的监督需要独立验证。

## AC01 至 AC10

| 验收 | 本轮断言 |
| --- | --- |
| AC01 | 初始化、未 ARM 和 READY 都保持 Mock INHIBIT。START 后第一份新样本才允许 UPDATE |
| AC02 | ACB 原始 ADC 和已确认的 CH1/CH2/CH3 compare 驱动真实闭环。模型正向、反向和联合跟踪 |
| AC03 | Gate 前的命令或样本超时、Device 拒绝、Core 拒绝、Mock 拒绝均保留正式积分。备用 INHIBIT 失败标记为未确认 |
| AC04 | 第一次无效立即禁止，重放不计数，新有效样本不能自动恢复，第三份连续无效样本锁存 |
| AC05 | 普通命令不能覆盖锁存，清除回 DISABLED，旧 START、旧序列和恢复前的源时间被拒绝，新 START 复位积分 |
| AC06 | 0 合法，`UINT32_MAX → 0` 合法，重复、旧序列、未来、半范围和超时被拒绝，重放不续期 |
| AC07 | 初检后注入 INHIBIT 或故障会覆盖候选 UPDATE。已接受 UPDATE 后的显式 INHIBIT 清空当前 Mock compare。非法单通道 compare 不改变任何通道 |
| AC08–AC09 | 正反目标实例各自运行，故障只作用于目标实例。同输入重放的 PI、Safety 和 compare 一致 |
| AC10 | Core、G3-NP、G3-SAFE、Legacy Host、Legacy Bench 均复跑通过 |

## 模型结果

固定转子，电角 `0.37 rad`、`Rs=0.5 Ω`、`Ld=1 mH`、`Lq=1.5 mH`、`VBUS=12 V`、电压矢量上限 `2 V`、目标电流上限 `5 A`。`Kp_d/q=(1, 1.5) V/A`，`Ki_d/q=(500, 500) V/(A·s)`。ADC 分辨率 `1 mA/count`，Mock PWM 周期 `6000 counts`，步长 `100 µs`。模型以固定转子的 R/L 零阶保持更新公式积分。每级 `600` 步，末尾 `100` 步最大轴误差门限为 `10 mA`。

| 目标 d/q，A | 最终模型 d/q，A | 末段最大轴误差 | 累计接受 UPDATE |
| --- | --- | --- | ---: |
| `(0, 1)` | `(0.000232, 1.000051)` | `0.415 mA` | `600` |
| `(0, -1)` | `(-0.000054, -0.999794)` | `0.417 mA` | `1200` |
| `(0.5, -0.75)` | `(0.500029, -0.750102)` | `0.274 mA` | `1800` |

第 `1801` 次运行态调用在最终 Gate 前注入 INHIBIT，Mock 确认 INHIBIT，三路当前 compare 清零，累计接受 UPDATE 与模型步数均保持 `1800`，正式积分不变。

## 本地命令与结果

| 验证 | 执行命令与结果 |
| --- | --- |
| 独立构建 | `cmake -S runtime -B build/sw01a-runtime -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Debug`。同样配置 `core`、`g3/np`、`g3/safe` 到 `build/sw01a-core`、`build/sw01a-np`、`build/sw01a-safe` |
| CTest | 对四个目录分别执行 `cmake --build <dir> --parallel` 与 `ctest --test-dir <dir> --output-on-failure`。Pipeline `1/1`、Core `4/4`、G3-NP `2/2`、G3-SAFE `1/1` |
| 模型输出 | `./build/sw01a-runtime/test-foc-pipeline.exe`，三个目标和最终 INHIBIT 均通过 |
| Legacy Host | `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/flows/host/run_host_tests.ps1 -RepoRoot D:/study/Software/Hollow-Gimbal`，`45/45` |
| Legacy Bench | `cmake --preset fw-full-diag-carrier-bench-debug -B build/sw01a-bench`，`cmake --build build/sw01a-bench --parallel`，完整编译并链接 |
| 格式 | `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/format_themis.ps1 -Scope c -Mode check`，`python -m cmakelang.format --check -c .cmake-format runtime/CMakeLists.txt`，均通过 |
| 专项门禁 | `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/gates/check_themis_quality_gate.ps1 -Scope falconfoc`，通过。源契约扫描 `20` 个 C/H、`4` 个 CMake、`4` 个 README，包含 `runtime/`。CMake structural checks 通过 |
| 全仓门禁 | 同一脚本 `-Scope all`。源契约通过，全仓在既有根 `CMakeLists.txt` 严格格式处停止 |
| Firmware CLI | 在子进程 PATH 加入本地 Git Bash 后运行 `tests/script/test_firmware_cli_contract.ps1`，模拟合约通过 |
| 工作树 | `git diff --check` 和本轮受影响的未跟踪文件逐个 `git diff --no-index --check`，无空白错误。根 CMake 和 Legacy 执行路径无源码变更 |

Bench 构建仅提供 Legacy 编译回归。NEXT-SW01A 没有 MCU 时间、资源占用或物理功率输出测量。
