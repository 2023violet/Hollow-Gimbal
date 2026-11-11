# FalconFOC V2 NEXT-SW01B Host Runtime 验证

工作树基线为 `1f542521189d5e0ef25e64e6530608ece8d6e137`，包含此前未提交的 Core、G3 和 NEXT-SW01A。新目录基线复跑为 Pipeline `1/1`、Core `4/4`、G3-NP `2/2`、G3-SAFE `1/1`。首先注册抢占和停滞测试，构建因缺少 `foc_runtime.h` 失败，接入运行时后转绿。第二条回归先复现了前台完成监督后，H0 在遥测期间接收致命故障，前台仍报告许可的情况。加入返回前发布代次复核后，故障注入通过。

## 所有权与调度

| 对象 | 唯一写入者 | 读取者 | 交接点与条件 |
| --- | --- | --- | --- |
| 传感器快照 | H1 | H0 | H1 填非发布槽再 release 索引，H0 acquire 复制。H1 不能抢占 H0 |
| 电流命令 | Foreground | H0 | 同一槽规则，保留源序号和时间戳。前台不能抢占 H0 |
| Pipeline PI、Safety 主状态、Mock | H0 | H0 | H0 内串行。前台用发布快照进行局部 Safety 推演 |
| Fast 心跳、输出和 H0 诊断 | H0 | Foreground | 全部共享字段为原子，代次在 H0 写期间为奇数。一轮复制代次变化则丢弃 |
| Foreground 心跳 | 完成监督的前台路径 | H0 | 监督结束后发布时间。H0 每个新时间只导入一次 |
| 独立禁止请求 | 各请求来源原子合并 | H0 和前台输出视图 | OR 合并，H0 exchange 取走。最终 Gate 后检查 pending。仍待确认时，输出视图为未确认 INHIBIT |

优先级为 `H0 > H1 > Foreground`，禁止同域重入，Host 使用单线程嵌套调用模拟抢占。遥测复制窗口内需少于 `2^31` 次 H0 发布。Runtime 要求 32 位无锁字原子。目标 MCU 的原子指令和中断实现需独立证明。

H0、H1、监督和遥测各有固定周期、最大迟到与模拟预算。逾期时直接推进到下一未来截止时间。迟到从最早尚未处理的截止时间计算。调度测试跳过一次 `100 µs` 名义周期，H0 只执行一次，实际 ADC `dt=200 µs`，q 轴积分增加 `0.1 V`。超过 H0 预算或监督迟到时，分别产生 Fast 或 Foreground 故障。遥测因合计预算不足可丢弃，监督仍完成。

角度源时间必须等于 ADC 源时间并满足角度年龄上限。闭环使用 H0/H1 同为 `100 µs` 的 Host 配置。更低的 H1 更新率下，未对齐的 H0 周期拒绝 UPDATE。超过年龄、重新收到角度或清除故障均不会自动恢复运行。

## AC01～AC12

| 验收 | 结果与断言 |
| --- | --- |
| AC01 | H1 角度、H0 ADC、独立前台监督串起正负 q 轴闭环，模型只消费已确认 compare |
| AC02 | H0 抢占写入一半的 H1，读取旧完整槽，因与新 ADC 未对齐而禁止 |
| AC03 | H0 抢占写入一半的命令，当周期保留旧的两轴目标，完整发布后的下一周期才使用新命令 |
| AC04 | 遥测复制被两个 H0 发布抢占，返回 `ERR_BUSY` 且调用方原快照保持原值 |
| AC05 | H0 停滞时前台发出 Fast 心跳故障及禁止，视图为未确认 INHIBIT，主状态尚待 H0 处理 |
| AC06 | 前台停滞超过窗口，H0 拒绝 UPDATE。普通故障已确认禁止且监督健康时仍可获 WDT 许可。致命复位和发布代次变化时拒绝 |
| AC07 | H1 停滞导致角度不匹配及过期，首份异常即停，后续新角度不会自动恢复 |
| AC08 | 跳过历史截止时间、真实 `dt` 积分、H0 超预算、监督迟到和遥测丢弃均有断言 |
| AC09 | 写入、读出、H0 获取输入及最终 Gate 后的提交边界注入禁止。门前拒绝时积分保持原值。已接受 UPDATE 后的独立禁止撤销当前输出，保留先前已接受事务 |
| AC10 | `UINT32_MAX → 0` 的时间和序列可用，零合法，重复源时间、未来和恰好半范围拒绝 |
| AC11 | 两次完整事件重放的三相输出序列指纹、最终 compare、故障、漏期、PI 和模型电流一致 |
| AC12 | Pipeline AC01～AC10、Core、G3-NP、G3-SAFE、Legacy Host 和 Legacy Bench 全部复跑通过 |

## 闭环与重放结果

复用 SW01A 固定转子 ACB 夹具。`Rs=0.5 Ω`、`Ld=1 mH`、`Lq=1.5 mH`、固定电角 `0.37 rad`、母线 `12 V`、电压上限 `2 V`、ADC 分辨率 `1 mA/count`、PWM 周期 `6000 counts`。`Kp_d/q=(1, 1.5) V/A`，`Ki_d/q=(500, 500) V/(A·s)`。H0 与 H1 均为 `100 µs`，前台监督 `1 ms`、遥测 `10 ms`。每级 600 步，末尾 100 步的最大轴误差门限为 `10 mA`。

| d/q 目标 | 最终 q 轴电流 | 窗口最大轴误差 |
| --- | --- | --- |
| `(0, 1) A` | `1.000051 A` | `0.415 mA` |
| `(0, -1) A` | `-0.999794 A` | `0.417 mA` |

每次运行累计接受 `1200` 个 UPDATE。最后注入过迟 H0 触发，漏期计数为 `1`，Mock 确认 INHIBIT，积分和模型均未推进，轨迹指纹为 `85428585`。旧 Pipeline 夹具三个阶段的输出和 SW01A 记录一致。

## 实际构建与门禁

| 项目 | 命令与结果 |
| --- | --- |
| Runtime 与 Pipeline | `cmake -S runtime -B build/sw01b-runtime -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Debug`，`cmake --build build/sw01b-runtime --parallel`，`ctest --test-dir build/sw01b-runtime --output-on-failure`，`2/2` |
| 模型输出 | `./build/sw01b-runtime/test-foc-runtime.exe`，`./build/sw01b-runtime/test-foc-pipeline.exe`，均通过 |
| 独立回归 | `cmake --build <dir> --parallel` 和 `ctest --test-dir <dir> --output-on-failure`，`build/sw01b-core` `4/4`，`build/sw01b-np` `2/2`，`build/sw01b-safe` `1/1` |
| Legacy Host | `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/flows/host/run_host_tests.ps1 -RepoRoot D:/study/Software/Hollow-Gimbal`，`45/45` |
| Legacy Bench | `cmake --preset fw-full-diag-carrier-bench-debug -B build/sw01b-bench`，`cmake --build build/sw01b-bench --parallel`，全新目录编译链接通过 |
| 格式 | `scripts/format_themis.ps1 -Scope c -Mode check`，`python -m cmakelang.format --check -c .cmake-format runtime/CMakeLists.txt`，通过 |
| 专项 | `scripts/gates/check_themis_quality_gate.ps1 -Scope falconfoc`，通过。覆盖 `24` 个 C/H、`4` 个 CMake、`4` 个 README |
| 全仓 | `scripts/gates/check_themis_quality_gate.ps1 -Scope all`，源契约通过后停在既有根 CMake 严格格式差异 |
| Firmware CLI | 使用本机 Git Bash 单独运行 `tests/script/test_firmware_cli_contract.ps1`，模拟合约通过 |
| 工作树 | `git diff --check` 和逐文件 `git diff --no-index --check` 无空白错误，含未跟踪 Runtime 文件和本记录。Legacy 源码与根 CMake 无差异 |

Bench 是 Legacy 固件编译回归。Host 运行、确定性抢占、模拟预算和 Mock 输出均无本轮目标 MCU 实测结论。
