# 运行方式与无板验证

> **当前收口状态（2026-09-26）**：最新交接与实板状态见
> [AI-handoff-2026-08-31.md](AI-handoff-2026-08-31.md)。本文前半部分保留历史验证
> 记录；其中 `38/38`、旧 ELF 和旧保护阈值只代表对应日期，不得覆盖当前 `45/45`
> host 结果及“动态高占空比 ADC/CSA KCL 失配、闭环速度门禁仍阻塞”的结论。

## 1. 验证边界

本章先记录无需实板的验证：主机配置/编译/链接、CTest、固件交叉编译和静态调用链检查；
第 10 节追加当前板的受控短脉冲证据。两类证据分开统计，不能把短脉冲或板上软件电流
读数扩大解释为产品级硬件验收。

涉及任何电机实机操作时，必须同时遵守 [电机实机调试安全协议与开环排障流程](motor-debugging-safety-and-procedure.md) 和 [AT32F415 + DRV8311H 电机实机测试方案](motor-real-bench-test-plan.md)；本章的无板测试结果不能替代快速停机、母线断电和电机温升保护。

以下项目不属于无板验证范围；其中部分已在受控短脉冲轨道取得有限板上数据，但仍未形成独立物理证据：

- OpenOCD/J-Link/DAP-Link 烧录；
- COM 口收发、Shell 串口回归；
- ADC/PWM/SPI 实际波形；
- 电机连续启动、闭环稳定性、独立温升和驱动保护响应。

因此“固件构建通过”只表示源码在当前交叉工具链下可编译链接，不表示实板功能验收通过。

## 2. 环境与构建入口

### 2.1 标准入口

仓库提供：

```text
./run.sh build       构建当前默认固件
./run.sh flash       通过 OpenOCD 烧录（本轮不执行）
./run.sh all         build + flash（本轮不执行）
./run.sh memory      查看固件尺寸
```

CMake 预设：

```text
host-debug
host-relwithdebinfo
fw-tiny-carrier-minsizerel
fw-tiny-headless-minsizerel
fw-standard-carrier-minsizerel
fw-full-diag-carrier-debug
fw-full-diag-carrier-bench-debug
```

`fw-full-diag-carrier-bench-debug` 是下一阶段有人值守诊断使用的独立
AT32F415 镜像：Shell 保留，VOFA 二进制输出强制关闭，
`FOC_ENERGIZED_CALIB_ENABLE=0` 强制关闭。它只改变编译配置，不改变
硬件急停、DRV8311H 保护或 `REAL_BENCH_TEST_STATUS` 门禁。

```powershell
cmake --build --preset build-fw-full-diag-carrier-bench-debug --parallel
```

该命令只进行交叉编译，不连接调试器，不写入 Flash，不控制 `nSLEEP`，也
不使能 PWM。

### 2.2 本轮使用的隔离验证目录

为避免覆盖已有 `build/host-debug` 和固件构建结果，本轮使用新目录：

- `build/code-wiki-host`：LLVM Clang 主机配置/失败证据；
- `build/code-wiki-host-gcc`：MinGW GCC 主机验证；
- `build/code-wiki-fw`：AT32F415 交叉构建。

这些目录是构建产物，不是产品源代码；没有删除或重置已有工作树内容。

## 3. 主机模式命令与结果

### 3.1 LLVM Clang 路径

配置命令：

```powershell
cmake -S . -B build/code-wiki-host -G Ninja `
  -DCMAKE_BUILD_TYPE=Debug `
  -DTHEMIS_BUILD_FIRMWARE=OFF `
  -DTHEMIS_BUILD_HOST_TESTS=ON
```

配置成功；编译命令：

```powershell
cmake --build build/code-wiki-host --parallel 4
```

结果：未通过。`components/utils/hash_map/hash_map.c` 间接包含 `include/tmdef.h:108`，项目宏明确拒绝当前 LLVM Clang 22：

```text
error: "The current compiler is NOT supported!"
```

这不是业务测试失败，而是主机编译器白名单/工具链选择不匹配。

### 3.2 MinGW GCC 路径

配置命令：

```powershell
cmake -S . -B build/code-wiki-host-gcc -G Ninja `
  -DCMAKE_C_COMPILER=C:/mingw64/bin/gcc.exe `
  -DCMAKE_CXX_COMPILER=C:/mingw64/bin/g++.exe `
  -DCMAKE_BUILD_TYPE=Debug `
  -DTHEMIS_BUILD_FIRMWARE=OFF `
  -DTHEMIS_BUILD_HOST_TESTS=ON
```

配置成功，说明 GCC 可通过当前 `tmdef` 编译器契约。全量构建：

```powershell
cmake --build build/code-wiki-host-gcc --parallel 4
```

结果：未通过。应用测试构建使用了 `tests/CMakeLists.txt` 中的旧路径：

```text
platform/at/at32f403a/generated/inc
```

当前工作树没有该目录；随后 `applications/runtime/motor_runtime.c:78` 的 `GPIO_PINS_15` 无法从 AT32F415 vendor 头获得，应用目标停止编译。该问题属于 AT32F403A -> AT32F415 主机测试 include 迁移缺口。

## 4. 固件模式命令与结果

### 4.1 配置

```powershell
cmake --fresh -S . -B build/code-wiki-fw -G Ninja `
  "-DCMAKE_MAKE_PROGRAM=C:/Program Files (x86)/ninja/ninja.exe" `
  "-DCMAKE_TOOLCHAIN_FILE=D:/study/Software/Hollow-Gimbal/tools/cmake/arm-gcc-toolchain.cmake" `
  -DCMAKE_BUILD_TYPE=Debug `
  -DTHEMIS_BUILD_FIRMWARE=ON `
  -DTHEMIS_BUILD_HOST_TESTS=OFF `
  -DTHEMIS_FIRMWARE_TARGET_NAME=falcon-foc-at32 `
  -DTHEMIS_PLATFORM_VENDOR=at `
  -DTHEMIS_PLATFORM_SOC=at32f415
```

配置识别结果：

```text
Themis platform : at/at32f415
Themis CPU class : cortex-m3-soft
Themis linker script : .../AT32F415xB_FLASH.ld
```

### 4.2 构建

```powershell
cmake --build build/code-wiki-fw --parallel 4
```

结果：通过。生成：

```text
build/code-wiki-fw/output/falcon-foc-at32.elf
```

早期 Debug/full-diag 构建快照（历史基线，非当前最终结果）：

| 区域 | 使用量 | 容量 | 使用率 |
| --- | ---: | ---: | ---: |
| FLASH | 72,408 B | 128 KiB | 55.24% |
| RAM | 21,616 B | 32 KiB | 65.97% |

该结果同时证明当前平台生成代码、FreeRTOS ARM_CM3 port、BSP、applications、vendor 驱动和链接脚本可以在软件环境中组合链接。

## 5. 基础主机契约测试（早期基线）

由于主机全量构建在应用 include 路径处停止，本轮先单独构建不依赖该旧路径的基础 targets：

```powershell
cmake --build build/code-wiki-host-gcc --parallel 4 --target `
  test_tm_export test_tm_format test_tm_assert test_core_time_dwt `
  test_tm_runtime test_bsp_system_contract test_bsp_console_contract `
  test_tm_task_perf test_tm_log test_tm_shell test_tm_shell_task `
  test_tm_task_budget_probe_failfast
```

执行：

```powershell
ctest --test-dir build/code-wiki-host-gcc --output-on-failure -R `
  "^(test_tm_export|test_tm_format|test_tm_assert|test_core_time_dwt|test_tm_runtime|test_bsp_system_contract|test_bsp_console_contract|test_tm_task_perf|test_tm_log|test_tm_shell|test_tm_shell_task|test_tm_task_budget_probe_failfast)$"
```

结果：

| 测试 | 结果 |
| --- | --- |
| `test_tm_export` | 通过 |
| `test_tm_format` | 通过 |
| `test_tm_assert` | 通过 |
| `test_core_time_dwt` | 通过 |
| `test_tm_runtime` | 通过 |
| `test_bsp_system_contract` | 通过 |
| `test_bsp_console_contract` | 通过 |
| `test_tm_task_perf` | 通过 |
| `test_tm_log` | 通过 |
| `test_tm_shell` | 通过 |
| `test_tm_shell_task` | 通过 |
| `test_tm_task_budget_probe_failfast` | CTest 报失败；进程按设计触发断言 |

CTest 汇总为 `11/12` 通过。最后一个 target 在 CMake 中标记了 `WILL_FAIL`，但本环境下 Windows 进程异常码 `0xc0000409` 仍被 CTest 报为失败，说明“故意失败的预算探针”在当前注册/运行环境中没有被稳定折算为预期通过。

## 6. 应用主机契约测试（早期基线）

不依赖旧应用 include 路径的两个 target 单独构建：

```powershell
cmake --build build/code-wiki-host-gcc --parallel 4 --target `
  test_app_runtime_budget_contract test_foc_round_contract
ctest --test-dir build/code-wiki-host-gcc --output-on-failure -R `
  "^(test_app_runtime_budget_contract|test_foc_round_contract)$"
```

结果：

| 测试 | 结果 | 证据 |
| --- | --- | --- |
| `test_foc_round_contract` | 通过 | FOC round/控制调用契约可运行 |
| `test_app_runtime_budget_contract` | 失败 | 测试要求 `TM_SHELL_TASK_STACK_BYTES == 192*4`，当前配置为 `512*4` |

其余应用测试 target 在全量构建阶段尚未得到可执行文件，不能写成“通过”或“运行失败”。应区分：

- 编译阻塞：旧 AT32F403A include 路径导致应用目标无法完整构建；
- 已知源级缺口：`motor_fault_runtime.c` 的 `foc_fault_check()` 当前为空，而 `test_motor_fault_runtime_contract` 期待过流锁存/PWM disable；
- 未执行：FSM、store、restore、motion、calibration、Shell app 级 target 本轮没有可执行结果。

## 7. 软件-only 模拟测试矩阵

Board1/DRV8311H 的网表、strap、CSA、RC、母线分压、PWM 计时和
`nFAULT/nSLEEP` 电阻网络模型，见 [hardware-and-drv8311h.md](hardware-and-drv8311h.md)
第 13 节。本节只保留固件/主机软件测试；两者都不构成实板验收。

| 场景 | 入口/测试 | 需要证明的行为 | 本轮结果 |
| --- | --- | --- | --- |
| export 顺序与启动机制 | `test_tm_export`、固件链接 | export 注册/平台到 app 的组合合法 | 基础测试通过；固件链接通过 |
| 时间/运行时/断言 | `test_core_time_dwt`、`test_tm_runtime`、`test_tm_assert` | host 替身下基础机制契约 | 通过 |
| console/reset 边界 | `test_bsp_console_contract`、`test_bsp_system_contract` | 非硬件 host 路径返回值和边界 | 通过 |
| Shell 解析/格式 | `test_tm_shell`、`test_tm_format` | 命令框架和格式化 | 通过 |
| Shell 队列背压 | `test_tm_shell_task` | 有界队列、满队列返回 `-EAGAIN`、统计 | 通过 |
| 任务预算 fail-fast | `test_tm_task_budget_probe_failfast` | 超预算时 fail-fast | 断言如预期发生，但 CTest 状态不理想 |
| FOC round | `test_foc_round_contract` | 一轮控制数据/调用契约 | 通过 |
| Shell burst/state/store/cfg/cal/diag | 对应 `test_app_shell_*` | 产品命令与 runtime 边界 | 因旧 AT32F403A include 阻塞，未得可执行结果 |
| FSM abort/fault clear | `test_motor_fsm_*` | 状态转换、校准中止/故障清除 | 未执行；固件编译可通过不等于测试通过 |
| 参数 payload/A-B/restore | `test_foc_param_restore_contract` | CRC/slot/恢复派生状态 | 未执行；主机 Flash 几何与 F415 不同 |
| runtime snapshot/deferred save | `test_motor_runtime_contract` | 单实例、快照、save bit 消费 | 未执行；源级调用链已审查 |
| motion/ramp/trajectory | `test_foc_motion_runtime_contract` | 1 ms 输入模式和梯形轨迹 | 未执行；`POS_FILTER/VEL_FILTER/TUNING` 源码为 no-op |
| calibration runtime | `test_motor_calibration_runtime_contract` | 参数刷新和增益计算 | 未执行 |
| overcurrent protection | `test_motor_fault_runtime_contract` | 故障锁存、PWM disable | 当前源级审查可判定实现缺失；没有将未构建 target 宣称为测试结果 |

## 8. 推荐的复现顺序

1. 先用 GCC 配置主机测试，确认工具链可接受；
2. 单独运行基础 subsys/BSP targets，快速确认无板机制；
3. 运行 F415 固件构建，确认平台与链接组合；
4. 修复或显式参数化测试 CMake 的 `at32f403a` include 路径后，再构建应用 tests；
5. 单独处理 `WILL_FAIL` failfast 的 CTest 表达；
6. 将 `motor_fault_runtime.c` 的行为与故障契约测试对齐后，重新运行应用全量测试；
7. 只有在软件证据闭环后，才进入用户另行授权的实板/串口/电机验证。

## 9. 2026-08-27 安全实现后的复验结果

本节记录本轮代码变更后的新鲜命令证据；第 4 至第 8 节保留为早期审计基线，不再代表当前主机测试阻塞状态。

### 9.1 本轮新增的无板安全目标

执行：

```powershell
cmake --build build/host-debug --target `
  test_motor_fault_runtime_contract `
  test_motor_fault_context_contract `
  test_motor_runtime_contract `
  test_app_shell_diag_contract `
  test_app_shell_burst_contract `
  test_foc_calib_curr_contract `
  test_foc_vbus_guard_contract `
  test_foc_bringup_model_contract `
  test_foc_voltage_openloop_model_contract `
  test_motor_fsm_calibration_gate_contract --parallel
```

结果：上述专项目标均构建成功并运行通过；随后完整 host flow 复核为 `35/35`：

| 目标 | 覆盖内容 | 结果 |
| --- | --- | --- |
| `test_motor_fault_runtime_contract` | 快速过流幂等锁存、非法传感器、VBUS 欠压/过压、超速、3 s 持续电流、200 ms 堵转 | `PASS` |
| `test_motor_fault_context_contract` | 首次故障上下文在 PWM 禁止前锁存、重复故障不覆盖、停机后可读、显式清除 | `PASS` |
| `test_foc_calib_curr_contract` | 稳定 ADC 样本在等待后写入 offset；ADC 近满量程或噪声过大时拒绝并关闭 PWM | `PASS` |
| `test_foc_vbus_guard_contract` | 正常 VBUS 接受；0、负数、NaN、Inf 拒绝 | `PASS` |
| `test_foc_bringup_model_contract` | 10 kHz FOC step、1250/250 LPF 比例、ADC 比例和 0.10 A RL+PI+LPF 应力阶跃 | `PASS` |
| `test_motor_runtime_contract` | 持久化/动态 `currbw` 与 LPF 同步、持久化非法值回退、非法值拒绝、LPF 输出保持 | `PASS` |
| `test_app_shell_diag_contract` | RUNNING/CALIB 时禁止诊断写入并返回 `reason=idle_only` | `PASS` |
| `test_app_shell_burst_contract` | `fault status`/`obs all` 携带停机诊断信息且不超过 Shell burst 预算 | `PASS` |
| `test_motor_fsm_calibration_gate_contract` | 默认禁止电机/编码器带电校准，不进入 CALIB、不打开 PWM | `PASS` |
| `test_foc_voltage_openloop_model_contract` | 零速/角度回绕、真实电压开环/I-f、SVPWM、VBUS 和 duty 边界 | `PASS` |

### 9.2 完整 host flow

执行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File `
  scripts/flows/host/run_host_tests.ps1 `
  -RepoRoot D:/study/Software/Hollow-Gimbal
```

CMake 配置成功，当前 35 个 host target 全部编译成功；CTest 结果为 `35/35` 通过。此前 4 个失败项已逐项处理，本轮又加入了动态 LPF、故障上下文、运行态诊断门禁、带电校准门禁和 FOC 电压开环模型：

| 失败项 | 首要证据 | 本轮处理 |
| --- | --- | --- |
| `test_tm_task_budget_probe_failfast` | 故意断言以 Windows 异常码 `0xc0000409` 退出，直接运行会被 CTest 当作系统级失败 | `themis_add_host_test(WRAP_SYSTEM_FAILURE)` 包装后，`WILL_FAIL` 正确通过 |
| `test_app_runtime_budget_contract` | 栈预算测试遗留了旧的 shell/电机任务常量 | live 配置与当前产品契约对齐，测试通过 |
| `test_motor_fsm_abort_restore` | 中止路径没有清理 encoder 及其他校准子状态 | FSM 在进入 IDLE 前清理 5 个子状态，测试通过 |
| `test_app_shell_diag_contract` | host 桩使用 libc 舍入，`0.125` 的 tie 结果随平台变化 | host 桩采用与产品一致的半入规则，测试通过 |
| `test_foc_bringup_model_contract` | 旧 LPF 配置在小电流阶跃中产生明显瞬态过冲 | 当前 `fc=1250 Hz`、`fs=10 kHz` 和 ADC 比例通过确定性模型，测试通过 |
| `test_motor_runtime_contract` | LPF 初始化早于持久化参数，且 `currbw` 修改未刷新 LPF | 参数加载后初始化；合法修改同时刷新 PI/LPF，非法值拒绝，测试通过 |
| `test_motor_fsm_calibration_gate_contract` | bring-up 保护阈值低于默认带电校准激励 | 默认 `FOC_ENERGIZED_CALIB_ENABLE=0U`，FSM 在进入 CALIB 前拒绝，测试通过 |
| `test_foc_voltage_openloop_model_contract` | 电压开环需要证明真实软件数学路径而不是独立伪模型 | 直接调用 `foc_current_loop_step()`/`foc_svpwm()`，零速、I-f、VBUS/duty 边界通过 |
| `test_motor_fault_context_contract` | 故障上下文必须在关 PWM 前锁存且不能被后续故障覆盖 | 首次上下文、停机读取、显式清除和 ISR 边界通过 |

因此当前无板 host 结论是“全量编译成功、35/35 通过”；保护、停机上下文、运行态诊断门禁、校准门禁、VBUS、动态 LPF、时基/参数模型、开环数学以及原有契约目标均为 `PASS`。这仍不代表 ADC/PWM/SPI 波形或电机物理运行已经验收。

### 9.2.1 停机后诊断数据边界

快速保护路径只做有限数值检查、首次故障上下文的 RAM 锁存、`fault_code`
锁存和 `pwm_out_disable()`；不打印、不格式化浮点数、不写 Flash、不发送
串口、不等待任务。`motor_fault_context_read()` 只在电机停止后由 Shell/诊断
路径读取，`fault clear` 或启动初始化才清除该上下文。正常 `state disable`
不会抹掉最后一次测量/控制值，因此可以随后使用：

```text
fault status
obs all
```

`obs all` 包含 raw ADC、offset、换算后三相电流、VBUS、`theta_sim`、
`theta_elec`、电气速度、`vd/vq`、反 Park 电压、三相 duty 和 sector；
`fault status` 在有锁存上下文时额外显示首次故障的紧凑快照。所有输出均是
停机后的证据导出，不能反向成为实时保护条件。

下一阶段的 `diag set`（包括 `vq/vd`、`if`、`dp0/1/2`、`iq`、`vel/pos`
直通、`currbw`、PID 和 `encoff`）只允许在 `IDLE` 修改。运行中命令返回
`result=error reason=idle_only`；这条门禁同时存在于 Shell 和 runtime，防止
绕过 Shell 的调用改变控制量。

### 9.3 AT32F415 固件构建

仓库的 `run.sh` 是 Bash 入口，而当前 Windows 环境没有 `bash` 命令；使用同一 CMake 预设完成等价构建：

```powershell
cmake --preset fw-full-diag-carrier-debug
cmake --build --preset build-fw-full-diag-carrier-debug --parallel

cmake --preset fw-standard-carrier-minsizerel
cmake --build --preset build-fw-standard-carrier-minsizerel --parallel

cmake --preset fw-tiny-carrier-minsizerel
cmake --build --preset build-fw-tiny-carrier-minsizerel --parallel

cmake --build --preset build-fw-full-diag-carrier-bench-debug --parallel
```

三种默认变体和专用 bench 变体均 `PASS`。本轮新鲜复核的固件资源为：默认 full-diag `FLASH 77,128/131,072 B（58.84%）`、`RAM 21,720/32,768 B（66.28%）`；standard `FLASH 39,876 B（30.42%）`、`RAM 19,984 B（60.99%）`；tiny `FLASH 36,668 B（27.98%）`、`RAM 18,248 B（55.69%）`；bench full-diag `FLASH 76,740 B（58.55%）`、`RAM 21,320 B（65.06%）`。bench 通过编译定义关闭 VOFA 并强制关闭带电校准。

### 9.4 历史实板只读复核（不作为本轮无板证据）

以下内容来自此前维护记录，仅用于保留证据链；本计划的当前执行没有重复这些板级动作，也不以它们解除 `REAL_BENCH_TEST_STATUS`。

在 host 安全目标和固件构建通过后，使用 CMSIS-DAP/OpenOCD 对当前 AT32F415 板执行了以下无旋转动作：

1. 通过 `program <full-diag ELF> verify reset exit` 写入并校验；OpenOCD 返回 `Programming Finished`、`Verified OK`、退出码 `0`。
2. 复位运行约 4 s，等待启动电流校准结束后 halt，SWD 读到 `axis_state=0`、`fault_code=0`、`v_bus=11.8783884 V`，offset 为 `2051.73975 / 2051.6189 / 2047.52905`。
3. 通过调试器调用生产代码 `pwm_out_disable()`，立即和 300 ms 后均读到 TIM1 `CCTRL=0x1000`；轴状态仍为 0、fault 仍为 0，未发送任何启动命令。

串口只读尝试收到约 `15,347` 字节后出现 `<RxBuff Overflow>`，原因是 USART1 同时输出 VOFA 二进制流；本轮不把该串口结果当作状态或保护证据。没有执行 `state start`、电压开环、I-f、闭环、校准命令，也没有做示波器、热像或转速验证。

### 9.5 当前结论

软件层已经具备“快速 trip + 任务级后备 + 默认拒绝带电校准 + 无效 VBUS 不调制”的最小 bring-up 闭环，默认 Id/Iq LPF 和运行时同步也已通过无板契约；历史实板只读记录不替代本轮无板门禁。由于 DRV8311H `nFAULT/nSLEEP` 没有 MCU 端点、TIM1 Break/死区仍未形成可验证安全闭环、PWM 禁能不切断 DCBUS，当前必须保持：

```text
REAL_BENCH_TEST_STATUS=BLOCKED
```

### 21.4 ADC 触发点重复对照

`[实板测量]` bench400、`vq=1.00 V`、`we=21.9911 rad/s`、`enc_dir=0` 条件下，
`adc_trig=512` 重复两次的软件电流峰值约为 `0.012/0.011/0.014 A` 和
`0.012/0.012/0.015 A`；`adc_trig=3740` 重复两次约为
`0.270/0.312/0.262 A` 和 `0.275/0.276/0.201 A`。VBUS 约 `11.87..11.93 V`，
编码器机械角尾段变化均小于 `0.001 rad`，每项 trace 约 2000 条。

`[静态推导]` 结果支持 ADC/CSA 软件观测对 TMR1 CH4 compare 敏感，优先检查低侧
CSA 建立时间、中心对齐 PWM 相位和 ADC 注入完成时序；不能把 512 认定为有效采样点，
也不能把软件电流认定为独立相电流。四次测试在 `state disable` 后因名义等待叠加
串口往返跨过 400 ms bench 预算而出现 `FAULT_COMM_ERROR (0x100)`，后续以 trace
样本数判断实际窗口，不以该通信故障作为功率级故障。

原始结果：[`ADC trigger follow-up`](../../tmp/bench/20260830_095925_adc_trigger_followup/result.md)。

### 9.6 可复现安全检查命令

```powershell
cmake --build build/host-debug --target `
  test_motor_fault_runtime_contract `
  test_foc_calib_curr_contract `
  test_foc_vbus_guard_contract --parallel

powershell -NoProfile -ExecutionPolicy Bypass -File `
  scripts/flows/host/run_host_tests.ps1 `
  -RepoRoot D:/study/Software/Hollow-Gimbal

cmake --build --preset build-fw-full-diag-carrier-debug --parallel

powershell -NoProfile -ExecutionPolicy Bypass -File `
  scripts/flows/host/check_foc_peripheral_contract.ps1 `
  -RepoRoot D:/study/Software/Hollow-Gimbal

ctest --preset test-host-debug --output-on-failure

git diff --check
```

### 9.7 本计划的无板执行禁令

第 9 节是无板验证禁令；第 10 节是当前板的受控短脉冲记录。产品级连续运行仍禁止 OpenOCD 无边界写入、校准命令、闭环和长时间 PWM；短脉冲必须使用已核验 bench ELF、自动运行预算和停机后日志导出。任何高电流、异常 VBUS、失控速度或数据路径不确定，都必须先关闭 PWM/切断电源，再在停止后分析。

## 10. 2026-08-29 现有板受控短脉冲验证

### 10.1 目的与方法

针对“低压电压开环卡死且电流偏高”的问题，本轮不等待产品级 G0 所要求的示波器、
外部相电流和独立温度证据，而是先执行可重复的最短实机诊断。使用专用 bench ELF，
每轮先 SWD `reset run` 回到安全 IDLE，只设置一个变量，再启动脉冲；bench ADC ISR
有约 50 ms 自动运行上限，超时只做 RAM 故障锁存和 PWM disable。所有复杂字段在
停机后通过 COM3 Shell 读取。

### 10.2 现场配置

| 项目 | 值 | 证据 |
| --- | --- | --- |
| MCU/调试器 | AT32F415，CMSIS-DAP/DAPLink，SWD `Cortex-M4 r0p1` | `[实板测量]` |
| 串口 | COM3，115200 8N1 | `[实板测量]` |
| ELF | `build/fw-full-diag-carrier-bench-debug/output/falcon-foc-at32.elf` | `[实板测量]` |
| 首轮 D0-D4/O0-O3 ELF SHA256 | `CDE7AC7D8044719D4CF4E2650C298C566D4C3CF2A72DE439D8119AA13BE3B4BC` | `[实板测量]` |
| 最终重刷及 O1 复测 ELF SHA256 | `325A464AA838DCB7344657E2917953DD52515D72AD7ED0B40B5AF91D5C5BEE36` | `[实板测量]` |
| 编译定义 | `TM_BENCH_DIAG=1`、`TM_FEATURE_VOFA=0`、`FOC_ENERGIZED_CALIB_ENABLE=0` | `[源码确认]` |
| 供电背景 | 12 V / 1 A，板上 VBUS 约 11.90–11.91 V | `[实板测量]` |
| FOC step | bench 5 kHz，`PWM_DT=200 us`；TMR1/ADC 载波仍约 20 kHz | `[静态推导]` |

### 10.3 实测结果

| 测试 | 命令 | 主要结果 | 判定 |
| --- | --- | --- | --- |
| D0 | `diag set dp0 0.00`，20 ms | `fault=0`，duty `0.5/0.5/0.5`，电流约 `1/-5/0 mA` | PASS |
| D1 | `diag set dp0 0.05`，20 ms | `fault=0`，sector 1，电流约 `3/0/-2 mA` | PASS |
| D2 | `diag set dp1 0.05`，20 ms | `fault=0`，sector 2，电流约 `1/-4/3 mA` | PASS |
| D3 | `diag set dp2 0.05`，20 ms | `fault=0`，sector 5，电流约 `1/1/3 mA` | PASS |
| D4 | `diag set dp0 0.20`，20 ms | `fault=0`，duty `0.49/0.51/0.51`，电流约 `1/2/1 mA` | PASS |
| O0 | `diag set vq 0.10 0.00`，20 ms | `theta=0`、`we=0`，电流约 `1/-4/5 mA` | PASS |
| O1 | `diag set vq 0.10 3.14`，20 ms | `theta≈0.01`，`we≈1.28/3.14 rad/s`，电流约 `4/1/-6 mA` | PASS |
| O1-G | 同命令，80 ms 请求 | 50 ms 自动护栏，锁存 `theta≈0.075`、`we≈3/3.14`、电流约 `-7/4/3 mA` | PASS |
| O2-G | `diag set vq 0.20 3.14`，80 ms 请求 | 锁存电流约 `-12/25/5 mA`，`d≈0.501/0.49/0.51` | PASS |
| O3-G | `diag set vq 0.40 3.14`，80 ms 请求 | 锁存电流约 `-48/60/-2 mA`，`d≈0.503/0.481/0.519` | PASS |
| O1-R | 最终镜像，`diag set vq 0.10 3.14`，20 ms | `fault=0`，`theta≈0.01`，`we≈1.32/3.14`，电流约 `1/2/1 mA` | PASS |

`O1-G/O2-G/O3-G` 的 `fault=0x00000100` 是 bench 运行预算到期的
`FAULT_COMM_ERROR`，不是 DRV `nFAULT` 或 `FAULT_OVER_CURR`。当前结果显示：

- `[实板测量]` 在 `vq≤0.40 V`、约 50 ms 内，板上换算相电流没有超过 `0.10 A`，
  没有复现本轮短脉冲高电流卡死；
- `[实板测量]` O1 的 `theta_sim` 和 `we_volt_now` 按斜坡推进，未复现软件角度不动；
- `[实板测量]` D1/D2/D3 的 sector 发生变化，但没有外部探头，不能把它扩大为物理
  相序和互补波形证明；
- `[实板未验证]` 外部相电流峰值、死区/重叠、DRV 温升、采样窗口和连续机械旋转仍未验证。

### 10.4 停机后结论

当前唯一主根因不能写成“已确认”。本轮“低压不转”只有以下候选解释：

```text
电压/转矩裕量不足、齿槽转矩、静摩擦、机械负载或电机参数不匹配
```

它们需要更长但仍受控的实机证据才能区分；不能通过提高电压、关闭保护、启动 I-f
或调速度环来替代证据。当前状态保持：

```text
OPEN_LOOP_SOFTWARE_PATH=PASS
MOTOR_PHYSICAL_OPERATION=UNVERIFIED
OPEN_LOOP_ROOT_CAUSE_STATUS=UNRESOLVED
REAL_BENCH_TEST_STATUS=BLOCKED
CONTROLLED_DIAGNOSTIC_STATUS=ALLOWED_WITH_MINIMUM_GUARDS
```

原始记录：[`tmp/bench/20260829_actual/result.md`](../../tmp/bench/20260829_actual/result.md)。
## 11. 2026-08-29 相位入口 A/B 验证

### 11.1 软件入口

`[源码确认]` 电压模式默认已激活时，第一次 `vq` 命令现在也会在 IDLE 入口同步
`theta_sim` 与实时编码器电角度，并将 `we_volt_now` 清零；运行中的电压更新不
重复播种角度。该边界由 `test_foc_motion_runtime_contract` 覆盖。快照接口使用
实时 `enc_pri.state.angle_elec_rad`，不再使用可能未刷新的测量缓存。

### 11.2 实板证据

- `[实板测量]` 修复前 A1：`theta_sim≈0`、`theta_elec≈3.35 rad`。
- `[实板测量]` 修复后 A1：`theta_sim≈3.21`、`theta_elec≈3.20 rad`，相位误差约
  `0.01 rad`，`we_volt_now=0`。
- `[实板测量]` A2/A3 正反向 `vq=0.10 V`、`we=±3.14 rad/s`、20 ms 均为
  `fault=0`；V2/V3/V4 的 `vq=0.20/0.30/0.40 V` 短脉冲也未触发过流，VBUS
  约 `11.91 V`。
- `[实板未验证]` 以上电流为 ADC/CSA 软件换算值，尚无独立相电流、示波器死区/
  重叠、采样窗口、DRV 温升和连续机械运行证据。

### 11.3 验证状态

```text
PHASE_ENTRY_ALIGNMENT=CONFIRMED
OPEN_LOOP_SOFTWARE_PATH=PASS
MOTOR_PHYSICAL_OPERATION=UNVERIFIED
OPEN_LOOP_ROOT_CAUSE_STATUS=UNRESOLVED
REAL_BENCH_TEST_STATUS=BLOCKED
CONTROLLED_DIAGNOSTIC_STATUS=ALLOWED_WITH_MINIMUM_GUARDS
```

对应 bench 镜像为
`build/fw-full-diag-carrier-bench-debug/output/falcon-foc-at32.elf`，SHA256 为
`F0402BADC248FEB2C7EEB3F149DF321B6A3189B3C65125AB876A5E83064F04B1`；日志目录为
`tmp/bench/20260829_phase_seed_final/`。相位缺陷已经确认，但由于 A/B 短脉冲未
复现原始高电流卡死，整机唯一根因仍未确认。

## 12. 2026-08-29 当前执行证据

### 12.1 软件门禁与构建

`[无板验证]` 当前主机测试和独立 CTest 均为 `36/36 PASS`；外设静态检查输出
`FOC_PERIPHERAL_CONTRACT=PASS`。50 ms bench、200 ms bench-long、full-diag、
standard、tiny 五个目标均构建成功；其中 200 ms 目标只用于验证编译定义和链接，
本轮没有刷写或实机运行。

当前 50 ms bench 镜像：

```text
ELF=D:/study/Software/Hollow-Gimbal/build/fw-full-diag-carrier-bench-debug/output/falcon-foc-at32.elf
SHA256=3969B23E8DB62BCD590B49D5FE2F63574944C3281CDF5A00D2A3CCE3931AD8AE
TM_BENCH_DIAG=1
TM_FEATURE_VOFA=0
FOC_ENERGIZED_CALIB_ENABLE=0
TM_MOTOR_BENCH_RUN_BUDGET_MS=50
TM_APP_TASK_STACK_SHELL_BYTES=2048
```

矩阵实测日志对应上一轮已验证的 bench ELF（SHA256
`7EBC7C00CB4E8225372B9E774B761CCB1A561E7A9CE96F7C208F8E88C201801E`）；随后因
host 宏头文件可见性修正重建并刷写当前 `3969...` 镜像，刷写后只进行 A0 只读
复核，没有重复执行电机脉冲。

`[无板验证]` 50 ms bench 资源为 FLASH `79648/131072 B`、RAM `21472/32768 B`；
200 ms 镜像资源相同。full-diag 为 FLASH `77184 B`、RAM `21728 B`，standard
为 `39892 B/19992 B`，tiny 为 `36668 B/18256 B`。这些只说明当前工具链能构建
链接，不是硬件运行证明。

### 12.2 当前板上短脉冲结果

`[实板测量]` COM3/115200 8N1 下，A0 上电静态为 `axis=idle`、`fault=0`、VBUS
约 `11.89–11.92 V`。A1 读取到 `theta_sim≈3.12`、`theta_elec≈3.11`、
`phase_error≈0.01 rad`。F0–F6 固定矢量峰值约 `0.010–0.031 A`；theta0 有效矩阵
没有可重复优胜 offset；V1–V4 峰值约为 `0.027/0.019/0.016 A`、
`0.036/0.032/0.019 A`、`0.056/0.045/0.016 A`、`0.067/0.056/0.016 A`。
所有值均为 MCU ADC/CSA 软件换算。

V3/V4 超过 50 mA 延长运行门槛，因而没有进入 200 ms 实机测试。没有复现高电流
卡死，且没有获得外部相电流、PWM 死区/重叠、采样窗口、温升或真实机械旋转证据。
最终已回到 `axis=idle`、`fault=0`；当前仍为：

```text
OPEN_LOOP_SOFTWARE_PATH=PASS
MOTOR_PHYSICAL_OPERATION=UNVERIFIED
OPEN_LOOP_ROOT_CAUSE_STATUS=UNRESOLVED
REAL_BENCH_TEST_STATUS=BLOCKED
```

### 21.7 2026-08-30 静态触发隔离与完整电压窗口

`[实板测量]` 在 `we=0、vq=1.00 V` 下，`adc_trig=1874/2300/2812/3740` 的两次重复
仍分别表现为约 `0.01 A` 低读数、2300 的约 `0.26 A` B 相转折、以及 2812/3740
的约 `0.24..0.26 A` A/B 高读数。VBUS 保持 `11.87..11.93 V`，无 fault，停机后
复位安全。该证据将 `ADC_MEASUREMENT_WINDOW` 更新为
`REPRODUCIBLY_TRIGGER_DEPENDENT`，但不是独立相电流证明。

`[实板测量]` 在 `we=21.9911 rad/s`、约 375 ms 有效窗口下，1.0 V 首轮/重复、1.5 V、
2.0 V 的软件峰值分别为 `0.257/0.256/0.033 A`、`0.273/0.277/0.233 A`、
`0.407/0.435/0.420 A`、`0.586/0.633/0.578 A`。软件场角连续推进，编码器角度只
部分跟随，四轮无软件 fault；这只能把“场角滑差”列为事实，不能选出采样、功率、
转矩或机械因素中的唯一根因。

原始数据：[`static trigger boundary`](../../tmp/bench/20260830_105417_static_trigger_boundary_1V/)、
[`1 V`](../../tmp/bench/20260830_105810_voltage_full_window_1V_30rpm/)、
[`1 V repeat`](../../tmp/bench/20260830_105937_voltage_full_window_1V_30rpm_repeat/)、
[`1.5 V`](../../tmp/bench/20260830_110024_voltage_full_window_1p5V_30rpm/)、
[`2 V`](../../tmp/bench/20260830_110109_voltage_full_window_2V_30rpm/)。

最终状态仍为：`OPEN_LOOP_SOFTWARE_PATH=PASS`、`MOTOR_PHYSICAL_OPERATION=UNVERIFIED`、
`OPEN_LOOP_ROOT_CAUSE_STATUS=UNRESOLVED`、`REAL_BENCH_TEST_STATUS=BLOCKED`。

### 21.6 2026-08-30 动态 trigger 曲线验证

`[实板测量]` bench400 镜像在 `vq=1.00 V`、`we=21.9911 rad/s` 下完成了 9 个
`CH4 compare` 点，并对转折点各做两次复测。`1874` 的软件电流峰值保持在
`0.011..0.017 A`，`2300` 为 `0.058..0.090/0.256..0.259/0.017..0.022 A`，
`2812` 与 `3740` 的 A/B 峰值稳定在约 `0.237..0.264 A`。VBUS 始终在
`11.87..11.93 V`，没有软件故障。

因此当前运行验证标签为：

```text
ADC_MEASUREMENT_WINDOW=REPRODUCIBLY_TRIGGER_DEPENDENT
```

该标签只表示板上 ADC/CSA 读数随 CH4 compare 可重复变化。下一步若要把候选收敛为
采样窗口根因，仍需要停机后结合 SOx、PWM 相位和独立相电流证据；不得直接使用低
读数点替换默认配置。原始记录：
[`dynamic trigger boundary repeats`](../../tmp/bench/20260830_104326_dynamic_trigger_boundary_repeats_1V_30rpm/)。

### 21.5 2026-08-30 完整实机矩阵复盘

`[实板测量]` 本轮在 bench400 镜像下完成 9 点 ADC trigger、36 组 PWM/ADC 映射、
六向固定矢量、1 V 静态相位、`0.25..2.00 V`/30 rpm 参数电压梯度、正反向和
`enc_dir=0/1` 对照。低幅固定矢量的软件峰值约 `0.008..0.028 A`，没有唯一 map
优胜；电压提高到 2 V 时软件峰值最高约 `0.468/0.450/0.065 A`，VBUS 约
`11.86..11.95 V`，软件场推进正常但编码器未形成稳定机械跟随。

`[静态推导]` 动态测试中 CH4 compare 仍表现出明显的软件电流观测差异，优先候选为
采样窗口/CSA 建立时序；相位种子 A/B 和 encoder direction A/B 没有达到同条件重复改善，
不改变默认配置。完整汇总：[`full hardware root cause`](../../tmp/bench/20260830_103200_full_hardware_root_cause/result.md)。

本轮结论不能扩展为真实 PWM、SOx、独立相电流、温度、死区或产品级运行验证。最终状态：

```text
ADC_MEASUREMENT_WINDOW=ANOMALOUS_OR_STRONGLY_SENSITIVE
PHASE_OFFSET_CANDIDATE=REJECTED_AS_UNREPEATED
ENCODER_DIRECTION_CANDIDATE=SUPPORTED_NOT_CONFIRMED
OPEN_LOOP_SOFTWARE_PATH=PASS
MOTOR_PHYSICAL_OPERATION=UNVERIFIED
OPEN_LOOP_ROOT_CAUSE_STATUS=UNRESOLVED
REAL_BENCH_TEST_STATUS=BLOCKED
```

原始日志：`tmp/bench/20260829_133853_next_root_cause_final/`。其中
`fault_context.txt` 明确记录本轮没有 first-fault context；`result.md` 记录了
每组命令、单位、停机方式和单一未确认根因假设。

## 13. 2026-08-30 分层正交诊断运行方式

### 13.1 只读预检

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File scripts/flows/board/run_bench_matrix.ps1 `
  -RepoRoot D:/study/Software/Hollow-Gimbal `
  -Mode Preflight -PortName COM3 -BaudRate 115200
```

预检核对显式 bench ELF、SHA256、编译定义和串口只读响应；还要求板端返回
`cmd=map action=show result=ok` 与 `cmd=enc_trace action=show result=ok`，避免旧镜像被
误当成当前诊断镜像。预检不刷写、不启动 PWM、不控制 PSU。

### 13.2 矩阵运行

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File scripts/flows/board/run_bench_matrix.ps1 `
  -RepoRoot D:/study/Software/Hollow-Gimbal `
  -Mode Mapping -PortName COM3 -RunPulses -WaitForPowerCycle
```

可选模式为 `Encoder`、`Mapping`、`Trigger`、`Phase`、`Voltage` 和 `All`。`Encoder` 只
做 PWM 禁止下的手动扫查；其余模式是 5–20 ms 单变量脉冲。`-RunPulses` 是显式带电授权，
而 `-WaitForPowerCycle` 只解决受控 disable 后的 FAULT 复位边界，不表示脚本拥有硬件急停
能力。串口失联或异常时脚本立即停止，不进入下一项。

### 13.3 结果解释

`[无板验证]` host/CTest 38/38、静态契约 PASS 和五种固件构建 PASS 证明实现可编译、
可测试、配置边界一致。`[实板未验证]` 当前轮只使用新镜像完成了预检和一个首脉冲，尚未
完成自动矩阵，因此旧实板日志不能与新 trace 能力混合；新脚本生成的结果必须记录独立
ELF SHA256。软件 trace 不
等价于物理 PWM、外部电流计、温度或产品运行证明。

上一轮无板构建身份：bench50 为
`7CFBDED0B667192AA6410788C41B0CD2BCF3D724D3F48848E1B5E76E4D139B9C`，bench200 为
`7440A83EE8306FEF75F8B259754ECF75ED3521ED3012185476FDB7A33EDA68AD`；它们对应上一轮
构建/验证记录，旧实板日志中的其他 ELF 哈希也只作历史证据。当前重建身份见本页第 14 节
及 Code Wiki 首页。

## 14. 有意义的电压开环验证入口（2026-08-30）

### 14.1 保护与速度

`[源码确认]` 当前保护配置为快速每相 `1.00 A`、持续 `0.75 A/3 s`；闭环堵转
`0.75 A/200 ms` 只在非电压模式执行。这样不会把开环固定矢量的有意零速误判成堵转，
又保留绝对快速关断和持续高电流保护。

`[静态推导]` 7 对极、30 rpm 对应 `we=21.99 rad/s`，电周期约 `285.7 ms`。因此
5 ms/`3.14 rad/s` 只能证明极短软件响应，不能证明旋转；Voltage 模式必须使用
bench400 和 `VoltagePulseMs=400`，映射/触发/相位模式才使用 5–20 ms。
当前 `DEFAULT_VOLT_WE_ACCEL=60 rad/s²` 时，达到 `21.99 rad/s` 约需 `367 ms`，
所以 400 ms 仍主要是启动过渡窗口；它不能替代稳态或产品级连续运行验证。

### 14.2 运行命令

```powershell
cmake --build --preset build-fw-full-diag-carrier-bench-400ms-debug --parallel

powershell -NoProfile -ExecutionPolicy Bypass `
  -File scripts/flows/board/run_bench_matrix.ps1 `
  -RepoRoot D:/study/Software/Hollow-Gimbal `
  -Mode Voltage -RunPulses -WaitForPowerCycle `
  -BenchBudgetMs 400 -VoltagePulseMs 400 -OpenLoopSpeedRpm 30
```

脚本会核对对应 ELF 的编译宏和预算，不接受用 bench50/bench200 冒充 Voltage 镜像。
实机运行仍保持 `REAL_BENCH_TEST_STATUS=BLOCKED`，且结果只代表当前板上软件/ADC
观测；PWM 引脚、外部相电流、DRV 温升和产品级连续运行仍需独立证据。

### 13.4 当前镜像首个实机脉冲证据

`[实板测量]` 2026-08-30 使用 bench50 SHA256
`7CFBDED0B667192AA6410788C41B0CD2BCF3D724D3F48848E1B5E76E4D139B9C` 完成显式刷写、
Preflight 和一项 map0/adc0 低压脉冲。Preflight 通过；静态 VBUS 约 `11.91 V`，duty
为零，软件电流未越过预警。

`[实板测量]` 首脉冲的 trace 记录 duty `0.498/0.502/0.502`、电流峰值
`0.019/0.012/0.013 A`、VBUS `11.89-11.92 V`，但最终锁存 `FAULT_COMM_ERROR`，并
保留 250 条 FOC 样本。当前 runner 因 power-cycle 门禁在下一项前停止。

`[静态推导]` 这表明名义 `PulseMs=5` 与板端实际停机边界不等价；必须先经过物理
power-cycle/reset 并解决 stop-latency，不能继续自动矩阵或把这一项当成根因确认。
完整日志位于 `tmp/bench/20260830_012451_comprehensive_root_cause/`；状态仍是
`OPEN_LOOP_ROOT_CAUSE_STATUS=UNRESOLVED`、`REAL_BENCH_TEST_STATUS=BLOCKED`。

## 15. 2026-08-30 当前标准 SVPWM 实机矩阵收口

本轮使用 bench400 ELF：

```text
build/fw-full-diag-carrier-bench-400ms-debug/output/falcon-foc-at32.elf
SHA256=25A38A987BE93B48C05526B8A6839F697AE618AACFD740C8517AAA661CA65922
```

`[无板验证]` host/CTest 均为 `38/38 PASS`，外设静态契约为 `FOC_PERIPHERAL_CONTRACT=PASS`，bench50/200/400、full-diag、standard、tiny 构建通过。`[实板测量]` 当前镜像经过显式 OpenOCD 验证，COM3 预检通过并返回 `axis=idle`、`fault=0`、`duty=0/0/0`。

### 15.1 实板结果摘要

- `[实板测量]` 36 组 PWM/ADC 组合均执行完成，没有唯一可重复的 PWM 或 ADC map 优胜。
- `[实板测量]` CH4 compare 对软件电流读数有显著影响；该现象作为采样窗口/CSA 建立候选，不能等同真实相电流。
- `[实板测量]` `2 V/20..30 rpm` 没有稳定同步；`3 V/30 rpm` 一次触发约 `1 A` 软件快速保护；`2 V/15 rpm` 两次结果不一致。
- `[实板测量]` theta0 矩阵没有稳定优胜，因此没有修改相序、编码器零点或电机参数。
- `[实板未验证]` 尚未完成人工 encoder 整圈、PWM/DRV/SOx 波形、独立相电流和温升证据。

完整原始串口日志与分项证据见 [`tmp/bench/20260830_074936_post_snapshot_fix/`](../../tmp/bench/20260830_074936_post_snapshot_fix/)。

当前状态：

```text
SVPWM_SOFTWARE_MODEL=PASS
OPEN_LOOP_SYNCHRONIZATION=NOT_DEMONSTRATED
OPEN_LOOP_ROOT_CAUSE_STATUS=UNRESOLVED
MOTOR_PHYSICAL_OPERATION=UNVERIFIED
REAL_BENCH_TEST_STATUS=BLOCKED
```

## 21. 2026-08-30 停机后方向 A/B 与 30 rpm 复测

`[实板测量]` 当前使用的唯一镜像为 bench400：
`build/fw-full-diag-carrier-bench-400ms-debug/output/falcon-foc-at32.elf`，SHA256 为
`59B78D648511926F8AD47213CA35E49C0D536697BE814B1B77AF64F84E949A03`。编译定义为
`TM_BENCH_DIAG=1`、`TM_FEATURE_VOFA=0`、`FOC_ENERGIZED_CALIB_ENABLE=0`、
`TM_MOTOR_BENCH_RUN_BUDGET_MS=400`。

### 21.1 诊断命令与停机语义

`[无板验证]` Shell 契约和脚本白名单已覆盖 `diag set enc_dir 0|1`：只有 IDLE 可写，
非法值和 RUNNING 写入均拒绝。该命令只作本轮 RAM 实验，重启后恢复默认/持久化配置。

`[源码确认]` 正常 `state disable` 会关闭 PWM 并进入 FAULT 状态，即使故障码仍为零；
这不是 `FAULT_OVER_CURR`。停机后 encoder trace 冻结，允许读取最后一段运行尾迹；异常时
仍必须先人工断 PSU，不能等待 Shell 回复。

### 21.2 实际结果

| 实验 | 运行窗口 | 软件电流峰值 | VBUS | 结果 |
| --- | ---: | --- | --- | --- |
| `enc_dir=0/1` 静态 `vq=0.50 V,we=0` | 约 25 ms | CW `0.017..0.110 A`；CCW `0.077..0.126 A` | `11.88..11.93 V` | 无故障，位置依赖明显 |
| `enc_dir=0` 动态 `vq=0.25 V,we=21.9911` | 350 ms | `0.037..0.062 A` | `11.89..11.93 V` | 软件场推进，未证明旋转 |
| `enc_dir=1` 动态 `vq=0.25 V,we=21.9911` | 350 ms | `0.037..0.070 A` | `11.88..11.92 V` | 软件场推进，未证明旋转 |
| `enc_dir=0` 动态 `vq=1.00 V,we=21.9911` | 350 ms | `0.190..0.286 A` | `11.88..11.92 V` | 未触发快速保护，未建立同步 |
| `enc_dir=1` 动态 `vq=1.00 V,we=21.9911` | 350 ms | `0.259..0.289 A` | `11.87..11.92 V` | 未触发快速保护，未建立同步 |

原始日志位于 [`tmp/bench/20260830_091746_encoder_trace_freeze_validation/`](../../tmp/bench/20260830_091746_encoder_trace_freeze_validation/)。

### 21.3 当前验证状态

- `[实板测量]` 方向切换有影响，但受物理起始角度未固定、无独立电流计和无机械测速
  限制，方向只标记为 `SUPPORTED_NOT_CONFIRMED`。
- `[实板测量]` `theta_sim` 在 30 rpm 电气命令下持续变化；encoder 末段只小幅变化，
  所以“软件旋转场正常”与“转子达到 30 rpm”必须分开记录。
- `[实板测量][静态推导]` CH4 compare 采样扫描的电流敏感性仍是独立的 ADC/CSA 窗口
  候选；不能因为方向 A/B 有差异就覆盖这个证据。
- `[实板未验证]` PWM、死区/重叠、真实 SOx 采样窗口、独立相电流、温度和产品级连续
  运行仍未验证。

最终状态保持：

```text
OPEN_LOOP_SOFTWARE_PATH=PASS
MOTOR_PHYSICAL_OPERATION=UNVERIFIED
OPEN_LOOP_ROOT_CAUSE_STATUS=UNRESOLVED
REAL_BENCH_TEST_STATUS=BLOCKED
```
