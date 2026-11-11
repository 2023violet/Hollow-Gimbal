# FalconFoc Scripts

This repository keeps only the FalconFoc live script surface.

Current official entry points:

- quality gate: `scripts/check_themis_quality_gate.ps1`
- FalconFOC Core, G3-NP, G3-SAFE, and Runtime source contract: `scripts/gates/check_falconfoc_source_contract.ps1`
- FalconFOC G3-SAFE host project: `g3/safe/` with its independent CMake/CTest entry
- FalconFOC current-control pipeline: `runtime/` with its independent CMake/CTest entry
- host tests: `scripts/run_host_tests.ps1`
- FOC peripheral static contract: `scripts/flows/host/check_foc_peripheral_contract.ps1`
- dedicated bench firmware: CMake preset `build-fw-full-diag-carrier-bench-debug`
- minimal board smoke: `scripts/run_tiny_carrier_regression.ps1`
- diag/cal board witness: `scripts/run_diag_carrier_witness.ps1`
- shell serial probe: `scripts/flows/board/run_shell_serial_probe.ps1`
- comprehensive bench matrix runner: `scripts/flows/board/run_bench_matrix.ps1`
- open-loop root-cause probe (T1/T2/T3): `scripts/flows/board/run_openloop_probe.ps1`
- open-loop probe log analyzer (host-side): `scripts/flows/board/analyze_openloop_probe.py`
- controlled bench pulse helper: `tmp/bench/20260828_235546/run_fixed_vector_pulse.ps1`
- firmware CLI: `run.sh`, `memory.sh`
- locked V2 AT32F415 cross-build and Host Board Port test: [reference firmware](../firmware/at32f415/README.md)
- V2 binary UART protocol, shared vectors and CLI reproduction: [wire specification](../protocol/README.md) and [Host tool](../tools/falconfoc-cli/README.md)
- Keil bridge: `scripts/build_project.ps1`

Layout:

- `scripts/gates/`: canonical gate implementations
  - includes `check_cmake_smoke.ps1` for preset-first host/live/prepared-inactive configure smoke
  - `check_themis_quality_gate.ps1 -Scope falconfoc` includes the FalconFOC source-contract gate
- `scripts/flows/host/`: canonical host flows
  - includes the no-board FOC peripheral contract check; it only reads source
    and generated configuration and never connects to a debugger or board
- `scripts/flows/board/`: canonical FalconFoc board smoke flows
- `scripts/lib/`: reusable helpers
- root-level `check_*` and `run_*`: compatibility wrappers for the canonical
  paths that remain

The current live board lane is a minimal FalconFoc smoke/regression surface.
Archived product-specific probe flows are not part of the live script contract.

Bench firmware boundary:

- `fw-full-diag-carrier-bench-debug` is a dedicated AT32F415 diagnostic image;
- it keeps Shell enabled, forces `TM_FEATURE_VOFA=0`, and forces
  `FOC_ENERGIZED_CALIB_ENABLE=0`;
- build it with
  `cmake --build --preset build-fw-full-diag-carrier-bench-debug --parallel`;
- this preset does not connect to a board, enable PWM, or replace the physical
  power cutoff and hardware protection requirements.

Fixed-duty hardware isolation:

- `fw-full-diag-carrier-bench-fixed-duty-30s-debug` is a separate bench-only
  image. While `axis=running`, it writes phase duty `A/B/C=0.40/0.50/0.60` on
  every FOC tick, after measurement collection and before all FOC control paths.
- It retains PWM mapping, fast current/bus protection, the 1.5 s heartbeat
  watchdog, and the 30 s energy limit. Use
  `scripts/flows/board/run_fixed_duty_probe.ps1`; it supplies heartbeats and
  never sends `state disable`.
- `fw-full-diag-carrier-bench-raw-sixstep-30s-debug` is the matching dynamic
  hardware-isolation image. It rotates six raw `40/50/60%` phase vectors at
  1 Hz electrical without entering the FOC or encoder paths. Use
  `scripts/flows/board/run_raw_sixstep_probe.ps1`.
- `fw-full-diag-carrier-bench-raw-sine-30s-debug` is the continuous counterpart.
  It writes three 1 Hz electrical, 10%-amplitude sine duties directly, bypassing
  FOC/SVPWM, encoder, and current-control paths. Use
  `scripts/flows/board/run_raw_sine_probe.ps1`.
- `fw-full-diag-carrier-bench-raw-sine-match-sixstep-30s-debug` repeats the same
  path at duty amplitude `0.115470`, which matches the raw-sixstep space-vector
  magnitude. Use `scripts/flows/board/run_raw_sine_match_sixstep_probe.ps1`.
- `fw-full-diag-carrier-bench-raw-sine-20-30s-debug` raises raw-sine duty amplitude
  to `0.20` while retaining the same 1 Hz and bench protection boundaries. Use
  `scripts/flows/board/run_raw_sine_20_probe.ps1`; it is a dynamic ADC/KCL
  observation probe, not a closed-loop speed-test substitute.

受控实板诊断边界：

- 当前板固定矢量、映射和触发诊断仍使用 5–20 ms 单变量脉冲；电压开环旋转不能使用
  5 ms 或 `we=3.14 rad/s` 作为有效转动证明。首轮有意义的开环参数为 30 rpm 机械速度
  （约 `21.99 rad/s` 电气速度），应使用专用 400 ms bench 镜像覆盖约一个电周期；
- `run_fixed_vector_pulse.ps1` 会发送一条已给定的 `diag set`、短暂 `state start`、
  `state disable`，最后才读取 `state/obs/fault`；它不是实时保护器；
- 运行中不打印、不写 Flash、不做校准、不发送任意连续命令；出现异常不等待脚本回复，
  应立即切断电源，停机后再保存和分析日志；
- 当前受控诊断数据只证明板上软件/ADC 观测到的短脉冲结果，不能替代独立电流、温度、
  PWM 波形和产品级 `REAL_BENCH_TEST_STATUS` 门禁。

Before any real-board action, the static no-board gate is:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File scripts/flows/host/check_foc_peripheral_contract.ps1 `
  -RepoRoot D:/study/Software/Hollow-Gimbal
```

The expected terminal marker is `FOC_PERIPHERAL_CONTRACT=PASS`.

当前 bench 诊断补充：

- `build-fw-full-diag-carrier-bench-debug` 使用 `TM_MOTOR_BENCH_RUN_BUDGET_MS=50`；
  `build-fw-full-diag-carrier-bench-200ms-debug` 用于中等窗口验证；
  `build-fw-full-diag-carrier-bench-400ms-debug` 才允许执行首轮电压开环旋转窗口，不能
  自动获得实机准入；
- bench 诊断命令包括 `diag trace clear/show` 和 `diag set theta0 <rad>`。trace
  只写 RAM，停机后读取；运行中的 `trace show` 被拒绝，不能在 ISR 中打印或写 Flash；
- `app_shell_task` 的 bench-only 栈为 2048 B。若串口无响应，应先检查调试器是否捕获
  `vApplicationStackOverflowHook`，不能把通信失联直接判定为电机过流；
- 2026-08-29 当前实板证据和日志位于
  `tmp/bench/20260829_133853_next_root_cause_final/`。V3/V4 已超过 50 mA 延长
  运行门槛，当前不执行 200 ms 实机镜像。

分层正交矩阵入口：

- 先执行只读门禁：
  `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/flows/board/run_bench_matrix.ps1 -RepoRoot D:/study/Software/Hollow-Gimbal`
- 编码器手动扫描使用 `-Mode Encoder -RunPulses`；此模式不启动 PWM，操作者在
  32 次 `obs all` 采样期间手动正反转电机轴。
- 映射、ADC 触发和相位分别使用 `-Mode Mapping|Trigger|Phase -RunPulses`，保持
  5–20 ms 脉冲；电压开环使用 `-Mode Voltage -RunPulses -BenchBudgetMs 400
  -VoltagePulseMs 400 -OpenLoopSpeedRpm 30`。脚本会拒绝使用 50/200 ms 镜像运行
  电压开环模式，且 `-RunPulses` 仍是明确的带电授权开关。
- 每个脉冲结束后脚本发送 `state disable`，随后只读取停机诊断；它不会把串口
  回复当作硬件保护器。当前状态机将受控 disable 路由到 FAULT，因此矩阵的下一
  个脉冲必须重新上电/复位并重新完成只读检查；脚本不会发送 `fault clear`、
  `state stop`、校准、参数写入或任意命令来绕过该边界。
- 该脚本不能自动操作 PSU、检测外部电流/温度或证明 PWM 波形；任何异常应由
  操作者立即切断 PSU，保存当前日志后再分析。

Current formal real-board witness lane:

- `scripts/run_diag_carrier_witness.ps1`
  - full-diag + carrier-demo board witness
  - captures `sys reboot`, boot banner, `help`, `sys status`
  - captures the live `diag/cal` runtime boundary on `COM54`

### 2026-08-30 previous AT32F415/COM3 evidence

本轮实际使用的 bench400 镜像为：

```text
build/fw-full-diag-carrier-bench-400ms-debug/output/falcon-foc-at32.elf
SHA256=25A38A987BE93B48C05526B8A6839F697AE618AACFD740C8517AAA661CA65922
```

`[实板测量]` 当前板端串口为 `COM3/115200 8N1`，预检和复位安全状态均通过。完整
分层日志在 `tmp/bench/20260830_074936_post_snapshot_fix/`；其中 36 组 PWM/ADC
组合、CH4 compare 扫描、theta0 对照和 15/20/30 rpm 电压开环记录均按镜像身份单独
保存。

`[实板未验证]` `run_bench_matrix.ps1` 只负责白名单串口命令、窗口计时和停机后日志，
不是实时过流器，也不控制 PSU。它不能证明 PWM/DRV 波形、SOx 真实采样孔径、独立
相电流或热稳定性；异常时必须立即人工断电。

当前实机状态仍为：

```text
OPEN_LOOP_ROOT_CAUSE_STATUS=UNRESOLVED
MOTOR_PHYSICAL_OPERATION=UNVERIFIED
REAL_BENCH_TEST_STATUS=BLOCKED
```

### 2026-08-30 bench 方向实验补充

`scripts/flows/board/run_bench_matrix.ps1` 的 bench 白名单包含
`diag set enc_dir 0|1`，但该命令只用于当前 bench 镜像的 RAM 方向 A/B 实验：只能在
IDLE 设置，不能写入产品参数，不能在 RUNNING 中修改。相关 Shell 契约和 runner 白名单
测试均通过。

本轮 bench400 镜像：

```text
build/fw-full-diag-carrier-bench-400ms-debug/output/falcon-foc-at32.elf
SHA256=59B78D648511926F8AD47213CA35E49C0D536697BE814B1B77AF64F84E949A03
```

`[实板测量]` `enc_dir` A/B 与 `vq=0.25/1.00 V、we=21.9911 rad/s` 短脉冲结果已保存
到 `tmp/bench/20260830_091746_encoder_trace_freeze_validation/`。运行期间脚本/探针不做
在线复杂分析；异常仍由操作者立即断 PSU，停机后读取 trace。没有独立相电流、温度和
PWM 波形时，脚本输出不能被解释为产品级运行证明。

## 2026-08-30 完整实机矩阵记录

`run_bench_matrix.ps1` 的命令白名单和 bench400 镜像用于本轮实机分层测试。实际另以
OpenOCD reset + COM3 短窗口执行了以下矩阵，并将原始串口输出保存在各自目录：

- 9 个 ADC trigger 点：`512..3740`，`dp0=0.05 V`；
- 36 组 `6 PWM map × 6 ADC map`；
- `dp0/dp1/dp2` 正负六向固定矢量；
- `vq=0.25..2.00 V`、`we=21.9911 rad/s` 的电压梯度；
- 1 V 静态相位、正反向动态和 `enc_dir=0/1` A/B。

所有矩阵均在 bench400 镜像 `59B78D648511926F8AD47213CA35E49C0D536697BE814B1B77AF64F84E949A03`
下执行；没有校准、参数写入、`state stop` 或 3 s 连续运行。低幅矩阵没有发现唯一
PWM/ADC map，动态触发点对软件 ADC/CSA 观测仍敏感；脚本结果不能替代 PWM/SOx 波形、
独立相电流、温度或机械测速。

完整索引：`tmp/bench/20260830_103200_full_hardware_root_cause/result.md`。

### 动态 ADC trigger 边界复测（2026-08-30）

在 `vq=1.00 V`、`we=21.9911 rad/s` 下对 `CH4 compare=1874/2300/2812/3740` 各
重复两次。`1874` 软件峰值约 `0.011..0.017 A`，`2300` 约 `0.256..0.259 A`（B 相），
`2812/3740` 的 A/B 峰值约 `0.237..0.264 A`；VBUS 约 `11.87..11.93 V`，无 fault。
这确认 `ADC_MEASUREMENT_WINDOW=REPRODUCIBLY_TRIGGER_DEPENDENT`，但只代表 MCU
ADC/CSA 观测相关性，不代表独立相电流或物理采样窗口已验证。日志位于：
`tmp/bench/20260830_104326_dynamic_trigger_boundary_repeats_1V_30rpm/`。

### 静态触发隔离与完整 30 rpm 参数窗口（2026-08-30）

追加实机证据位于：
`tmp/bench/20260830_105417_static_trigger_boundary_1V/`、
`tmp/bench/20260830_105810_voltage_full_window_1V_30rpm/`、
`tmp/bench/20260830_105937_voltage_full_window_1V_30rpm_repeat/`、
`tmp/bench/20260830_110024_voltage_full_window_1p5V_30rpm/`、
`tmp/bench/20260830_110109_voltage_full_window_2V_30rpm/`。

`[实板测量]` 将 `we` 固定为零后，CH4 compare 转折仍可重复；在约 375 ms 的
`we=21.9911 rad/s` 窗口中，1.0/1.5/2.0 V 软件峰值总体升高，但 encoder 未稳定
跟随软件场。该结果只更新 `ADC_MEASUREMENT_WINDOW=REPRODUCIBLY_TRIGGER_DEPENDENT`，
不证明真实相电流、SOx/PWM 波形、机械转速或热稳定；状态仍为
`OPEN_LOOP_ROOT_CAUSE_STATUS=UNRESOLVED`、`REAL_BENCH_TEST_STATUS=BLOCKED`。

### 2026-08-31 SpeedStep（90 rpm 闭环短时验证）

`flows/board/run_openloop_probe.ps1 -Mode SpeedStep -RunPulses` 是仅用于
`TM_BENCH_DIAG=1`、30 s bench ELF 的闭环速度阶跃 runner。它不改变已确认的采样
配对、相序或保护阈值：每个机械方向各三轮，以 `0.50 A`、`we=±4 rad/s` 的 I-f
起转 2.5 s，交接至同方向 30 rpm，再依次验证 30/60/90 rpm（每档 5 s，200 ms
采样）。运行态阶跃由 `diag velstep <rpm>` 完成，只有 I-f→VEL 已交接的 RUNNING
轴才会接受，且仅更新 `target_vel`。

输出目录为 `tmp/bench/<timestamp>_speed_step/`，含 `preflight.log`
（ELF SHA-256）、逐样本/分级 CSV、每轮串口日志、人工电源表观察表和 `result.md`。
软件峰值达到 1.4 A、VBUS 不在 10.8–13.2 V、fault、串口失联或操作者观察到异常时，
必须立即切断 12 V；失败后只能 SWD 复位或物理断电，禁止 `fault clear` / `state stop`。
无示波器时，这个 lane 只能验收串口/母线行为，不能区分编码器磁路误差与功率级相位畸变。

### 2026-09-01 AutonomousDiag 与安全恢复

`run_openloop_probe.ps1 -Mode AutonomousDiag` 只使用 SWD reset，自动写入 ADC 基线、
复零、map-trigger、位置与 SpeedStep 产物；不会提示人工输入，不会发送 `fault clear` 或
`state stop`。它烧录并 verify 30 s bench ELF；OpenOCD 必须收到正斜杠 ELF 路径，避免
Windows 反斜杠被命令解释为转义。

现行 `obs all` 三相电流是 `amps : i=a/b/c`，runner 同时兼容该格式和历史 `[amps]`；
idle `diag adc zero` 等待上限为 5 s。速度失败后只能 SWD reset 后重新预检。

`Invoke-ProbePulse` 对 `DurationMs <= 400` 必须先保持完整指定时间再 disable；此前
100 ms proof 会立即停机，相关历史 map/position 日志不可作为实机证明。SpeedStep 的
心跳在每个采样点非阻塞写入，下一条 `obs all` 回执必须包含 keepalive 成功。

### 2026-09-01 CCR4=2700 default and autonomous restart

The bench default ADC trigger is `2700`, including TMR1 initialization and map reset. Board runners
use that value for normal proof modes; `TriggerScan` may still explicitly test other values, including
`3740`, as comparison points. `SpeedStep` now resolves the board back to idle through the configured
SWD gate before its ADC baseline, so a preceding `state disable` fault-safe state does not require a
manual `fault clear` or power cycle.

Live evidence is in `tmp/bench/20260901_trigger2700_adcproof/`,
`tmp/bench/20260901_trigger2700_triggerscan/`, and
`tmp/bench/20260901_133000_adc_then_speed/`. The static proof passes at 2700; the -30 rpm stage still
aborts at high duty because phase-current KCL no longer closes. Do not run 60/90 rpm until a dynamic
sampling fix has waveform evidence.
