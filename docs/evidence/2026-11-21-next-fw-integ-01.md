# NEXT-FW-INTEG-01｜Board1 基础校准快照接入与锁定态启动验证

**NEXT-FW-INTEG-01 LOCKED FIRMWARE INTEGRATION PASS**

**PHYSICAL OUTPUT NOT VERIFIED** · **TARGET MCU WCET NOT VERIFIED** · **G3-PWR LOCKED**

日期 2026-11-21。AT32F415 Board1 的正式 `main()` 现在通过构建期只读供应接口选择启动配置，并经 UART PARAM_READ 读回生效值与校准输入结果。默认构建无外部输入，启动完整标称配置。另有一份显式命名的锁定 ARM 软件验证镜像，经同一接口提供合成快照。当前没有经过确认的真实板卡参数包，本轮证明的是固件消费能力，不证明任何板卡或电机参数。

[原始证据索引](2026-11-21-next-fw-integ-01/README.md) · [原始证据 ZIP](2026-11-21-next-fw-integ-01/raw-evidence.zip) · [SHA-256](2026-11-21-next-fw-integ-01/raw-evidence.sha256)

## 1. 基线冻结

工作树 `D:/study/Software/Hollow-Gimbal-vm01a`，分支 `codex/falconfoc-v2-vm01a`，HEAD `211b226c4ca870c90bf9b9048d112bfce2365ce8`，与观测基线一致。暂存区为空，起始无已修改或未跟踪文件。RC0 `a198f6d` 是 HEAD 祖先（`git merge-base --is-ancestor` 退出 0）。本轮没有执行 Reset、Checkout、拉取、合并、提交或推送。

首次在工作树内跑基线时，Sanitizer 一轮与源码编辑重叠，该轮结果作废。正式基线改用 `git archive 211b226` 导出的独立源码树（`build/fwinteg/base-src`），全部重跑：

| 基线项 | 实测结果（211b226 导出树） |
| --- | --- |
| Clang / MinGW GCC / Clang ASan+UBSan Host | 各 120/120：Core 21、Runtime 13、Calibration 10、Control Plane 5、Sensors 2、Simulation 50、Servo 7、Protocol 1、Board Host 9、Calibration Workflow 1、Replay 1 |
| 默认 ARM（Cortex-M4 / soft / FPU none） | BIN SHA-256 `78d055e1135d23a5c2d08f6db17fbf64622333f429381efa2632401eb58cbda2`，与 CAL-BOOT-01 记录逐字节一致。Flash 44,452 B，RAM 4,184 B |
| 锁定入口 | `main` 反汇编：`cpsid i` → TMR1 时钟/复位 → PA7–10、PB0–1 `GPIO_MODE_ANALOG` → 时钟 → `foc_board_init` → ADC |
| UART（Host 模拟器 + CLI demo） | HELLO/CAPABILITY/STATUS/FAULT/TELEMETRY = OK，ARM = OUTPUT_LOCKED，SAVE = NOT_SUPPORTED，STOP = ACCEPTED；PARAM_READ 16=7、17=0.0，32/38 = INVALID_ARGUMENT |

上轮评审参考数字 Board 9/9、Calibration 10/10、Runtime 13/13、Control Plane 5/5、Protocol 1/1 与实测一致。Simulation 实测 50 项，较 CAL-BOOT-01 记载的 46 项多 4 项，以本轮实测为准。

## 2. 实际调用链

```
Reset_Handler → main()
  __disable_irq()
  foc_at32_lock_output()            TMR1 复位（MOE、通道清零），六个桥臂输入脚 analog
  foc_at32_clock_init()             仅 Flash 等待周期，不擦写
  foc_at32_boot()
    foc_board_calibration_expectation()   CMake 编译期常量：可接纳供应 ID、预期极对数
    foc_board_calibration_supply()        默认 NULL；验证镜像返回 .rodata 合成记录
    foc_board_boot_select()
      ├ supply == NULL → foc_board_init()                   NOMINAL + NOT_AVAILABLE
      ├ foc_board_boot_qualify()  格式 → 供应身份 → 坐标 → 预期极对数
      │    → foc_calibration_admit(供应候选, Board 身份+预期极对数, 供应限值)
      │    → 重算快照与供应快照逐位一致 → Board 身份 → 方向乘积 → Board 窗口
      ├ 通过 → foc_board_init_with_calibration()            CALIBRATED + ACCEPTED
      └ 拒绝/失败 → 清零 Runtime/输出/Encoder → foc_board_init()   NOMINAL + REJECTED(+原因)
                    标称也失败 → 清零，无生效配置，返回错误
  失败 → for(;;) __WFI()            无 ADC/SPI/定时器/UART/IRQ 生产者
  成功 → foc_at32_adc_init → spi → timers → foc_serial_init
         → foc_serial_attach_boot(foc_board_boot_serial_readback, &boot) → uart → NVIC → 计数器 → __enable_irq
```

ARM 反汇编证实两组镜像 `main` 的调用顺序为 `... system_core_clock_update → foc_board_calibration_expectation → foc_board_calibration_supply → foc_board_boot_select → (cmp r0,#0; bne → wfi) → gpio_init(ADC 引脚) → adc_reset ...`。锁定前导 75 条指令在归一化文本偏移与栈帧偏移后与基线完全相同，字面量池中 CRM 外设号、GPIOA/GPIOB 基址、Flash 寄存器和 150 MHz 校验常量逐字相同。

## 3. 参数来源、资格与读回

| 字段 | 来源 | 资格检查 | 读回（PARAM_READ） |
| --- | --- | --- | --- |
| 三相 ADC Offset | 供应 ADC 候选 → Board 重算准入 | 准入限值、Board 窗口 1536–2560 count、有限值；ADC 满量程 4095 与增益 3.3/(4095×0.5) A/count 由 Board 事实提供 | 38–40，取自生效 `runtime.pipeline.adc_config` |
| Encoder 方向 | 供应 Rotor 方向 × Board 原始方向 | 原始源方向必须等于 Board −1；Runtime 方向 = 源方向 × 校准方向（准入、Board 选择、`foc_board_init_with_calibration` 三处独立检查） | 42 Runtime、43 源、44 校准（1=+1，2=−1，0=无），取自生效 Encoder 策略 |
| CPR | Board 事实 65536 | 候选身份 CPR 必须一致 | 41 |
| 极对数 | Rotor 候选 | 预期值来自 CMake（默认 7，验证镜像 11），与候选无关；1–64 | 16，取自生效 Runtime |
| Rotor Electrical Offset | Offset 候选 | 与 Rotor 粗值差 ≤ 准入残差；[0, 2π) | 17，取自生效 Runtime |
| 相序 | Board 事实 ABC | 候选身份相序必须一致 | — |
| 编码器坐标 | 供应声明 | 仅 `RAW_NO_LUT`；LUT 修正或未声明一律拒绝 | — |
| 供应 / 审核身份 | 供应记录 | 非零且等于 CMake 期望；`accepted=true` 本身不被信任，快照由候选重算 | 36、37 |
| 结果 | 启动选择 | — | 32 生效配置、33 校准输入、34 拒绝原因、35 准入失败细分 |
| 物理电流测量资格 | — | 本镜像恒为否 | 45 = 0；TELEMETRY 有效位保持 0 |

R/L、Flux、Kt、J/B、PI 和 LUT 不进入板端启动，板端不解析 FCAL。ELF 中没有 FCAL、估计器（`*_collect`/`*_finalize`）或 Flash 编程/擦除符号。现有字段能证明的是配置一致性：供应的 Board 身份、CPR、相序、方向和极对数与本构建期望一致。它们不能证明实物电机或板卡身份。

验证镜像的合成供应（`src/foc_board_supply_verification.c`，供应 ID `0x56464331`，审核 ID `0x01210001`）：Offset 2031.5/2063.25/2017.75 count，校准方向 −1，Runtime 方向 +1，极对数 11，电角 Offset 1.2509766 rad。审核链：`test_audit_chain` 用现有 CAL-01 估计器从合成样本重新得到候选（Offset 2031.50012/2063.25/2017.75，Rotor −1/11/1.250004 rad，Offset 1.250000 rad），供应候选与之在 0.001 count / 0.002 rad 内一致；Board 由供应候选重算的快照与供应快照逐位一致；Board 接纳后读回与 Runtime 实际消费一致。

## 4. 集成验证（Host，生产实现）

`test-foc-board-boot` 链接与验证镜像相同的供应和期望翻译单元；`test-foc-board-boot-default` 链接默认镜像的那两个单元。两者均调用正式 `foc_board_boot_select()`。

| 用例 | 实测 |
| --- | --- |
| 默认构建 | `NOMINAL_CONFIGURATION` + `CALIBRATION_NOT_AVAILABLE`。ADC 配置与 `foc_board_init()` 结果 `memcmp` 相同，极对数 7、Offset 0、Encoder 未设显式策略 |
| 合成供应接纳 | `CALIBRATED_CONFIGURATION` + `ACCEPTED`；Runtime ADC Offset、极对数 11、电角 1.2509766、Encoder 策略 +1、采集元数据（CPR 65536、方向 +1、极对数 11、相序 0）全部来自生效配置 |
| Board ADC 端口 | `foc_board_adc_begin/complete` 送入 rank {1241, 2032, 2063, 2018}，H0 已调用（`runtime.adc.seq=0`，`h0_called`），以 Runtime 实际 `adc_config` 解码为 +0.5/−0.25/+0.25 count × 增益；标称 Offset 下同一 rank 会读成约 −26 mA |
| Board Encoder 端口 | `foc_board_encoder_begin/complete`，原始码 0→1：校准后机械 +0.00009587 rad、电角 1.2520312 = wrap(11×2π/65536 + 1.2509766)；Sensor 实例方向等于策略方向 |
| 反向验证输出 | 同一原始码在标称策略下机械 −0.00009587 rad、电角 6.2825146；若方向错误而沿用校准 Rotor，电角将为 1.2499219，测试断言实际值与之相差 >1e-3 rad |
| UART（两种启动） | 32–45、16、17 与 Runtime/Encoder 实际值逐位相同；写 38、写 16 返回 NOT_SUPPORTED 且值不变；重放 = REPLAY，错会话 = BAD_SESSION；ARM/START = OUTPUT_LOCKED，PARAM_SAVE = NOT_SUPPORTED；TELEMETRY 有效位 0；STOP = ACCEPTED，`stop_requests` 由 H0 清零；无故障时 FAULT_CLEAR = BAD_STATE |
| 锁定 RUNNING 不可持续（实测） | 经 UART 发布当前目标后用 Safety 公开生命周期 API 强制进入 RUNNING：首个 H0 计算出合法 UPDATE，Board 适配器拒绝（`declined_updates=1`），Pipeline 退回 DISABLED 且不锁存故障；之后 3.4 ms（超过 2.5 ms 命令超时）仍无 COMMAND_TIMEOUT、无更多 UPDATE、输出为软件 INHIBIT。因此锁定镜像中真实的单独命令超时无法累积 |
| 故障清除由 H0 消费（两种启动） | 基于上一行的实测，测试经公开 Safety API `foc_safe_latch_fault()` 在正式选择的实例上锁存该故障，H0 发出软件 INHIBIT 后：UART FAULT_CLEAR = ACCEPTED，`clear_result` = PENDING 且 Safety 仍为 FAULT_LATCHED（前台只登记请求）；随后两个 H0 周期内 H0 复核并应用，`clear_result` = APPLIED，状态 DISABLED，故障位清零；FAULT_READ 报 DISABLED + CLEAR_APPLIED，输出仍为软件 INHIBIT。UART、Runtime 准入与 H0 判定均为生产代码 |
| 锁定输出 | 两种启动、ADC/Encoder/UART 全程 `declined_updates=0`、`pipeline.mock.updates=0`；Board 适配器直接收到由 `foc_device_map_output` 生成的合法 UPDATE 时返回 false，记 `UPDATE_NOT_APPLIED`，`declined_updates=1` |
| CLI（真实 Python 客户端 + 模拟器） | 默认模拟器：32=1、33=1、38–40=2048.0、42=2、45=0，写 38 = NOT_SUPPORTED，原 demo 8 条响应与基线逐字段相同（随机序号除外）。验证模拟器：16=11、17=1.2509765625、32=2、33=2、36=1447445297(0x56464331)、38–40=2031.5/2063.25/2017.75、42=1、43=2、44=2、45=0，ARM/START = OUTPUT_LOCKED |

### 拒绝与故障用例

每例均以全新零值实例调用正式选择，断言 `NOMINAL_CONFIGURATION` + `REJECTED`、指定原因、Runtime 为完整标称值、Encoder 无显式策略、输出日志为软件 INHIBIT，再经 UART 读回原因。

| 输入 | 拒绝原因 / 准入细分 |
| --- | --- |
| ADC 候选 NaN | ADMISSION / ADC_RANGE |
| 供应快照 NaN（候选正常） | SNAPSHOT_MISMATCH |
| Offset 3000（供应放宽限值至 4095，准入通过） | BOARD_RANGE |
| CPR 4096、原始源方向 +1、相序 1、ADC 增益 ×2、极对数 0、极对数 7（候选与快照同改） | ADMISSION / ADC_IDENTITY（各一例） |
| 期望极对数 7 而供应为 11 | ADMISSION / ADC_IDENTITY |
| 期望极对数 0 | BOARD_IDENTITY |
| 供应快照 Runtime 方向改为 −1（方向乘积冲突） | SNAPSHOT_MISMATCH；同一快照直接送 `foc_board_init_with_calibration` 返回 ERR_CONFIG，Runtime/输出/Encoder 不变 |
| Offset 候选校准方向与 Rotor 不一致 | ADMISSION / OFFSET_IDENTITY |
| `accepted=true` 但 ADC 候选不可用 | ADMISSION / ADC_QUALITY |
| 供应 ID 不符；供应 ID 0 | SUPPLY_IDENTITY |
| 坐标 LUT_CORRECTED；坐标 UNKNOWN | COORDINATE |
| 格式错 | FORMAT |
| 默认期望遇到任何供应 | SUPPLY_IDENTITY |
| 校准初始化提交后报告失败（Host 注入） | INITIALIZATION，候选被清除后重建完整标称配置，无校准残留 |
| 校准与标称都失败 | 返回错误，生效配置 NONE，`runtime_ready=false`，Runtime 未初始化且 `output_arg=NULL`，`foc_board_boot_read`/采集元数据/串口读回适配器均拒绝；附上该报告的串口读 32 返回 INVALID_ARGUMENT |
| Encoder 已启动 | 选择前拒绝，Runtime 保持哨兵值未被写入 |

初始化失败注入通过 `foc_board_boot_select_with()` 的步骤表完成，注入函数先调用真实 `foc_board_init_with_calibration()`/`foc_board_init()` 再报告失败，被测选择与清除逻辑为生产实现。ARM `main` 只调用 `foc_board_boot_select()`，其默认步骤表固定为两个真实初始化函数。

## 5. 软件复现

最终工作树由 `bash build/fwinteg/run-final.sh build/fwinteg/final4` 在全新构建目录执行（全部用例就位后的完整重跑）。脚本对每步单独记录退出码，并在同一次运行内：用各通道自己的构建产物直接运行两个启动测试并保存输出（Sanitizer 通道在同一步设置 ASan 运行库）；统计全部 CTest 结果行，要求全部为 Passed 且 Not Run/Skipped/Failed 计数为 0；用本次构建的两个 ARM 镜像执行第 6 节的静态检查（`arm_static.py`）；用本次构建的两个 Host 模拟器经真实 Python CLI 执行第 4 节的 UART 读回检查（`uart_check.py`）。每项检查不符即退出非零。

| 验证 | 实测结果 | 退出码 |
| --- | --- | --- |
| Clang Debug Host | 122/122：Core 21、Runtime 13、Calibration 10、Control Plane 5（独立计数）、Sensors 2、Simulation 50、Servo 7、Protocol 1、Board Host 11、Calibration Workflow 1、Replay 1 | 0 |
| MinGW GCC 16.2 Debug Host | 122/122，分项同上 | 0 |
| Clang ASan+UBSan（`halt_on_error=1`） | 122/122，分项同上 | 0 |
| Board Host 11 项 | 原 9 项 + `test-foc-board-boot` + `test-foc-board-boot-default`；`test-foc-python` 含新增读回断言 | — |
| 同一次运行内三套工具链直接运行两个启动测试（6 步） | `final/witness-*.txt`。三套输出逐行一致，均含：接纳供应、估计器审核链、反向验证、两种启动的 FAULT_CLEAR → PENDING → H0 APPLIED → DISABLED、锁定 RUNNING 1 次 UPDATE 被拒且 3.4 ms 后无超时 | 0 ×6 |
| Legacy 预设 full-diag Debug、standard/tiny MinSizeRel | 构建成功，供门禁读取 ELF/map | 0 |
| `check_themis_quality_gate.ps1 -Scope all -RequireCMakeFormatter -Summary` | `[Total] PASS`。Source Contract：C/H 208、CMake 15、README 15；Current Contract PASS；Legacy Host 45/45；cmake-format 强制检查通过 | 0 |
| 跳过与失败（脚本内 `skip-check` 步） | CTest 结果行 366，Passed 366，Not Run/Skipped/Failed/Exception/Timeout 0（3 × 122） | 0 |

全部 25 步退出码为 0（`final/exit-codes.txt`）。基线 120 项全部保留且仍通过，新增 2 项。没有删除测试、放松既有断言或吞掉退出码。四个 ARM BIN 与此前各次运行逐字节相同。

## 6. ARM 构建与静态证据

以下均为 Arm GNU Toolchain 14.3.Rel1、Cortex-M4 / soft-float / FPU none 的构建与静态证据，无 MCU 执行。表中检查由 `arm_static.py` 在 `final4` 运行内对本次镜像逐项执行，结果见 `final/static/summary.txt` 与各镜像的 `checks.txt`。

| 镜像 | BIN SHA-256 | Flash / RAM |
| --- | --- | --- |
| 基线 211b226 默认 | `78d055e1135d23a5c2d08f6db17fbf64622333f429381efa2632401eb58cbda2` | 44,452 B / 4,184 B |
| 本轮默认 `falconfoc-v2-at32f415` | `d6466d74984522598a626c4a7a9db702597853a631967d6baebc0c447b17647b` | 48,312 B / 4,280 B |
| 本轮验证 `falconfoc-v2-at32f415-calibration-verification` | `9c39d55552270b00f627b851fbf2a17a4fa353f821b9a1615a2f97eb24f83488` | 48,592 B / 4,280 B |
| 默认 + `FALCONFOC_BOARD_CAPTURE=ON`（标签 7） | `5c31792e97a716f62015439191b0dd2c53dd78f92b114e59fa9c4bf8d7825c85` | 49,544 B / 9,792 B |
| 默认 + `FALCONFOC_RT02_TIMING_TRACE=ON` | `cd8d6514661215ed1244aec112a2f38fee7d8e817f4419ab0fc1b4dcb3cd440b` | 49,432 B / 4,696 B |

默认和验证镜像在另一组全新目录各重建一次，BIN 哈希逐字节相同。默认镜像相对基线 Flash +3,860 B，主要来自正式入口可达的准入重算、选择和读回代码；RAM +96 B 为启动报告与串口读回指针。验证镜像只多 280 B 的 `.rodata` 合成记录。两镜像都通过 `stack-budget.ld` 的 4 KiB 栈余量断言，链接日志保留既有 newlib 空 syscall 与 RWX LOAD 段警告。采集与 RT-02 追踪选项构建仅作编译和链接证据。

新增符号（两镜像相同，验证镜像另有 `foc_board_verification_supply`）：`foc_board_boot_select`、`foc_board_boot_select_with`、`foc_board_boot_qualify`、`foc_board_boot_scrub`、`foc_board_boot_offset_qualified`、`foc_board_boot_read`、`foc_board_boot_serial_readback`、`foc_board_boot_default_ops`、`foc_board_calibration_supply`、`foc_board_calibration_expectation`、`foc_board_calibration_identity`、`foc_board_encoder_policy`、`foc_board_init_with_calibration`、`foc_calibration_admit`、`foc_calibration_snapshot_valid`、`foc_calibration_identity_matches`、`foc_calibration_identity_static_matches`、`foc_calibration_identity_valid`、`foc_calibration_wrap_2pi`、`foc_serial_attach_boot`、`boot`。无删除符号。RC0 审计中被裁剪的 `foc_board_init_with_calibration` 与 `foc_calibration_admit` 现在由正式入口可达并保留在 ELF。

| 静态检查 | 结果 |
| --- | --- |
| `main` 锁定前导 | 75 条指令与基线归一化后相同，字面量 CRM/GPIO/Flash 常量逐字相同（`final/static/*/checks.txt` 的 `prologue_identical: PASS`） |
| 选择位置 | `foc_board_boot_select` 位于 `system_core_clock_update` 之后、ADC GPIO/`adc_reset` 之前；返回非零跳转 `wfi` 循环 |
| `foc_board_output_submit` | 98 行反汇编与基线仅一个字面量池地址不同（`pipeline_config` 位置），UPDATE 拒绝与 INHIBIT 软件确认逻辑不变；默认与验证镜像相同 |
| UART 拒绝 | `foc_serial_init` 仍选择 LOCKED 策略，ARM/START/SAVE 分发源码未改；Host 与 CLI 对两种启动均实测 OUTPUT_LOCKED/NOT_SUPPORTED |
| Flash 写入 | 两镜像无 Flash 编程/擦除符号，启动只设置读等待周期 |
| 静态栈（`.su`） | 最深启动链 `main` 64 → `foc_board_boot_select` 24 → `_with` 96 → `foc_board_init_with_calibration` 448 → `foc_runtime_init` 664 → `foc_pipeline_init` 408，约 1.7 KiB 加末端调用；资格链 `main`→`qualify` 160 → `foc_calibration_admit` 168 约 0.5 KiB。基线链 `main` 56 → `foc_board_init` 144 → `foc_runtime_init` 664 → `foc_pipeline_init` 408。在 IRQ 开启前执行，不与中断嵌套叠加。实测栈高水位未测 |

## 7. 源码差异

| 文件 | 变更 |
| --- | --- |
| `firmware/at32f415/include/foc_board_boot.h`、`src/foc_board_boot.c` | 新增：供应记录、期望、报告、资格检查、选择、清除、读回、采集元数据和串口读回适配器 |
| `src/foc_board_supply_none.c`、`src/foc_board_expectation.c` | 新增：默认无供应；CMake 注入的期望 |
| `src/foc_board_supply_verification.c` | 新增：合成验证供应，仅验证构建和 Host 测试链接 |
| `src/main.c` | `foc_board_init` 换为 `foc_at32_boot()`；附加串口读回；采集元数据改用生效配置 |
| `src/foc_board.c`、`include/foc_board.h` | 新增 `foc_board_calibration_identity()`、`foc_board_encoder_policy()`；`foc_board_init_with_calibration()` 增加方向乘积检查 |
| `src/foc_serial.c`、`include/foc_serial.h` | PARAM_READ 只读 ID 32–45，经回调读取，串口适配器不依赖 Board |
| `CMakeLists.txt` | `FALCONFOC_BOARD_CALIBRATION_SUPPLY`（NONE/VERIFICATION）、两套供应库、验证镜像命名、两个新测试、验证模拟器 |
| `tests/test_foc_board_boot*.c`、`tests/foc_board_boot_test.h`、`tests/foc_serial_sim.c` | 新测试与夹具；模拟器改走正式选择 |
| `tools/falconfoc-cli/test_falconfoc_cli.py` | 追加读回与只读拒绝断言 |
| `protocol/README.md`、`firmware/at32f415/README.md`、维护日志 | 读回表、启动选择与验证镜像说明 |

`applications/`、`bsp/`、`platform/`、`subsys/`、`components/`、`kernel/`、`calibration/`、`core/`、`runtime/`、`g3/`、`sensors/`、`control_plane/`、`simulation/` 与协议源码零差异。Legacy 生产源码、FCAL v1、Safety 门限和功率寄存器策略未改。已有测试和断言未删改；新增的 0.001 count 容差只用于本轮新测试中估计器与供应候选的比对。

## 8. 未验收项与边界

- 无板卡操作：未上电、未烧录、未写 MCU Flash、未解锁 PWM、未励磁。ARM 结果仅为构建与静态证据。
- 合成供应是测试数据，不代表任何实物参数。真实参数包、其来源审核与物理设备身份比对仍待提供。
- 物理电流测量资格、真实 ADC 零点、编码器方向与电角零点的物理正确性未验证。
- 启动选择在 IRQ 之前执行，静态栈估计见第 6 节；目标 MCU 执行时间、实测栈高水位与 WCET 未测。
- 锁定态没有能自然产生单独 COMMAND_TIMEOUT 的运行路径，这一点由 `test_locked_running_cannot_persist` 实测证明，是输出锁优先于接入的直接结果。故障清除的 H0 消费在正式选择的实例上完整验证；被清除的故障由公开 Safety API 锁存，而非由运行中的命令超时产生。

## 9. 复现

从仓库根目录运行（需要 CMake、Ninja、Clang、MinGW GCC、Arm GNU Toolchain 14.3、Python 3、pwsh、cmakelang）：

```powershell
bash build/fwinteg/run-final.sh build/fwinteg/final4
```

`run-final.sh`、`run-test-matrix.ps1`、`arm_static.py`、`uart_check.py` 与 `compare_prologue.py` 的副本在原始证据 ZIP 的 `scripts/` 下；连同 `baseline/` 一起放回 `build/fwinteg/`（基线目录放到 `build/fwinteg/evidence/baseline`）后运行，输出目录可取任意新目录。本报告的全部最终数字、见证输出、静态检查与 UART 读回均来自同一次 `final4` 运行。单独构建验证镜像见 [固件 README](../../firmware/at32f415/README.md#startup-calibration-selection)。
