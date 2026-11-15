# FalconFOC V2｜NEXT-TIMING-OWNER-01 时序参数所有权与控制周期一致性

## 结论

`NEXT-TIMING-OWNER-01 PASS`

本轮完成 AT32F415 Legacy 与 V2 时序事实核对，并实施最小所有权收敛：平台层现在拥有 AT32F415 timer clock、Legacy TMR1 carrier/ADC event、Legacy TMR5 soft-IRQ 和 V2 时间基准事实；Legacy 的 FOC step、velocity、position、trajectory 等仍由应用产品配置拥有，没有把控制策略误并入硬件平台事实。

没有改变任何实际控制周期、PWM/ADC/SPI/Timer 寄存器配置、FreeRTOS 调度、ISR 优先级、PI 参数、Safety、输出锁、CPU/FPU/ABI 或 V2 H0/H1 数值。

以下边界继续保持：

- `G3-SAFE HARDWARE NOT VERIFIED`
- `G3-PWR LOCKED`
- `AT32F415 WCET NOT VERIFIED`
- `MOTOR PHYSICAL OPERATION UNVERIFIED`

## 一、真实时序基线

| 时序事实/策略 | 实际值 | 权威定义 | 主要消费位置 | 归属 |
| --- | ---: | --- | --- | --- |
| AT32F415 timer input | 150 MHz | `tm_platform_foc_timing.h` | Legacy `foc_config.h`、V2 `foc_board.h` | 平台硬件事实 |
| Timer clock MHz alias | 150 MHz | `tm_platform_foc_timing.h` | Legacy compatibility expressions | 平台硬件事实 |
| Legacy TMR1 carrier/ADC event | 20 kHz | platform timing header + generated `wk_tmr.c` (`ARR=3749`, center-aligned) | Legacy ADC ISR cadence | 平台/生成外设事实 |
| Legacy FOC step | 10 kHz product / 5 kHz bench | `applications/algo/foc_config.h` + ADC divider 2/4 | `motor_task.c` `foc_tick_div` | 产品控制策略 |
| Legacy TMR5 motor soft-IRQ | 5 kHz | platform timing header + generated `wk_tmr.c` (`ARR=1999`, prescaler 14) | encoder + velocity/position outer loop | 平台/生成外设事实 |
| Legacy velocity loop | 5 kHz | `VEL_LOOP_FREQ_HZ` | `foc_outer_loop_step()` | 产品控制策略，受平台事实约束 |
| Legacy position loop | 1 kHz | `POS_LOOP_FREQ_HZ`, `POS_LOOP_DIVIDER=5` | `foc_outer_loop_step()` | 产品控制策略 |
| Legacy trajectory/input task | nominal 1 kHz | `TRAJ_TASK_FREQ_HZ`, `osDelay(1)` | `motor_task` / input path | RTOS scheduling policy；不是硬实时定时器 |
| V2 common timebase | 1 MHz / 1 µs tick | platform timing header | V2 `FOC_BOARD_US_HZ`, TMR2/TMR5 setup | V2 platform binding |
| V2 H0 | 200 µs | `FOC_BOARD_H0_US` | V2 TMR1 + ADC path | V2 Board/Runtime contract |
| V2 H1 | 500 µs | `FOC_BOARD_H1_US` | V2 TMR5 + SPI1 path | V2 Board/Runtime contract |
| V2 ADC timing window | trigger +40 µs, latest +110 µs | `main.c` local V2 timing constants | TMR1 CH4 / ADC completion | V2 peripheral transaction policy |
| V2 SPI window | send +20 µs, finish +70 µs, latest +100 µs | `main.c` local V2 timing constants | TMR5/SPI1 | V2 peripheral transaction policy |

Legacy `osDelay(1)` 只能说明任务请求约 1 ms 的调度间隔，不能证明严格 1 kHz；V2 的 H0/H1 才是硬件定时器边界。APB 75 MHz 也没有被直接当作 Timer 时钟，当前平台事实是 APB 分频后 Timer 输入 150 MHz。

## 二、重复定义与最终所有权

### 已收敛的重复事实

新增 [tm_platform_foc_timing.h](D:/study/Software/Hollow-Gimbal/platform/at/at32f415/generated/inc/tm_platform_foc_timing.h:1)，定义：

- `TM_PLATFORM_TMR_CLK_HZ = 150000000U`
- `TM_PLATFORM_TMR_CLK_MHZ = 150U`
- `TM_PLATFORM_LEGACY_TMR1_CARRIER_HZ = 20000U`
- `TM_PLATFORM_LEGACY_TMR5_SOFTIRQ_HZ = 5000U`
- `TM_PLATFORM_V2_TIMEBASE_HZ = 1000000U`

Legacy [foc_config.h](D:/study/Software/Hollow-Gimbal/applications/algo/foc_config.h:90) 不再直接拥有 150 MHz/150 MHz-in-MHz 数值，而是引用平台别名。V2 [foc_board.h](D:/study/Software/Hollow-Gimbal/firmware/at32f415/include/foc_board.h:33) 也引用同一平台时钟和时间基准。V2 standalone CMake 将平台 generated include 目录加入 Board target；Legacy Host contract 同样导出该目录，因此生产构建与 Host 测试消费同一个真实文件。

`TM_MOTOR_BENCH_ADC_TICK_HZ` 也已改为引用 `TM_PLATFORM_LEGACY_TMR1_CARRIER_HZ`，消除 bench 配置中第二份 20 kHz 平台事实。

### 明确保留的应用层参数

以下参数没有迁移，因为它们表达产品策略，而不是芯片时钟事实：

- `PWM_FREQ_HZ`：Legacy 软件 FOC step cadence，默认 10 kHz，bench 5 kHz；它不是 TMR1 carrier。
- `VEL_LOOP_FREQ_HZ`：5 kHz velocity policy；通过编译期检查与平台 TMR5 soft-IRQ 对齐。
- `POS_LOOP_FREQ_HZ`：1 kHz position policy；由外环每 5 次执行一次得到。
- `TRAJ_TASK_FREQ_HZ`：1 kHz 任务名义 cadence；实际受 FreeRTOS 调度影响。
- V2 `FOC_BOARD_H0_US`/`FOC_BOARD_H1_US`：V2 独立 Board/Runtime contract，不能与 Legacy TMR1/TMR5 事实宏强行合并。
- Safety timeout、ADC/SPI transaction window 和 foreground budget：属于执行域安全/事务策略，不随平台时钟自动改写。

### 编译期一致性约束

`foc_config.h` 新增检查：

- timer Hz/MHz 两个别名必须一致；
- Legacy FOC step 必须整除平台 TMR1 carrier；
- Legacy velocity cadence 必须等于平台 TMR5 soft-IRQ cadence。

这些检查只拒绝配置漂移，不改变运行时数值或调度。

## 三、实际修改文件

- `platform/at/at32f415/generated/inc/tm_platform_foc_timing.h`：新增平台 timing seam。
- `applications/algo/foc_config.h`：引用平台 timer facts，保留产品控制 cadence，并增加编译期一致性检查；文件原有宏表布局使用 `clang-format off` 保持，不做无关重排。
- `applications/config/motor_task_config.h`：bench ADC tick 引用平台 carrier，保留原值 20 kHz。
- `firmware/at32f415/include/foc_board.h`：V2 Board timer/timebase 由平台事实派生。
- `firmware/at32f415/CMakeLists.txt`：V2 Board target 消费平台 generated include。
- `cmake/ThemisContracts.cmake`：Host test contract 消费当前 AT32F415 platform timing include。
- `tests/script/test_falconfoc_current_contract.ps1`：当前 RC0 contract 改为检查真实 F415 timing seam，并保留 Legacy control cadence 断言。
- `firmware/at32f415/README.md`：记录 Legacy/V2 timing ownership split。
- `docs/governance/Themis_工程维护日志.md`：追加本轮维护记录。

没有修改生成的 `wk_tmr.c`、Legacy Timer/ADC/SPI 寄存器配置、V2 `main.c`、FreeRTOS 调度或任何实际周期。

## 四、行为等价与产物对照

### V2

- ELF SHA-256：`51D7428FF2414FA4623DCE5086A6CD8D4F69058DFC6405B317DC28875E281551`
- BIN SHA-256：`9806ADBFA3282450527F40BB97CB1DA75FAFE41FB405F1126114187C6393B029`
- HEX SHA-256：`B80C263E6B07CE976E2BF1AEDED2737EED536C7224369091DC0919CAA48C51AE`
- MAP SHA-256：`BDC648824C427C873259638A30EB2FAE0C59E1598EBEA3C2C43E0C29CF6516B3`
- `.text=36,644 B`、`.data=92 B`、`.bss=3,544 B`、Flash `36,736 B`、RAM `3,632 B`。

V2 产物、资源和主要链接布局完全保持。

### Legacy 三种固件 preset

| Preset | ELF SHA-256 | BIN SHA-256 | HEX SHA-256 | text/data/bss |
| --- | --- | --- | --- | --- |
| tiny carrier MinSizeRel | `D14D0B954733DCB0B499254927301EAB14B9E474518AE2CD78E6B0579B53745E` | `D7D049C03DDD2378DEFE39B1E516EB6FF0973FF01818658DB58D9F9C683C37BD` | `DEE2EEC96B4BB4F9591E119FEAEEC3B45F612CD4614637C62B8C1E74A372D83A` | `37372/128/18368` |
| standard carrier MinSizeRel | `258BAA189100B0379297F5CA9F47B0DD840AC41022B178C8BBC2C5BCAE7C3F20` | `7A23BB4370CBA6139266EA6DF5358E43D2B200F438FFE7359C114167AE963A78` | `F50AD30A428F10F98D0B256D9AB6AF658249E9CDB30B49A1D49ADDDD66D7FAFB` | `40596/128/20104` |
| full-diag carrier Debug | `AF7D4F3942772BA89619C12413A6973CE189CAC8A2CC4F1B67E9326B6AFAAF56` | `698E14395C7D3876589BBFFDAE6EF509E10BFE061EA6ABF26F8533521ED079E9` | `A588EB2BB0F96112CD482182A7A3BC45F32E1E8A2DCA8C8381307797B9208152` | `79576/132/21840` |

Tiny and standard ELF/BIN/HEX are byte-identical to the pre-change baseline. Full-diag BIN/HEX and section sizes remain identical; its ELF hash changes because this Debug artifact records the modified include/macro source and debug metadata. No executable or Flash image section changed.

### Runtime replay and output boundary

- SYSTEM replay：`3f01f6e3`
- Long replay：`49dea950`
- V2 Board1 output lock remains unchanged.
- Feedforward remains disabled by default.
- No PWM enable or physical output path was introduced.

## 五、回归与门禁

- Host Debug：`45/45 PASS`
- Core：`14/14 PASS`
- Sensors：`2/2 PASS`
- Calibration：`1/1 PASS`
- Runtime/Pipeline：`10/10 PASS`
- G3-NP：`2/2 PASS`
- G3-SAFE：`1/1 PASS`
- Protocol：`1/1 PASS`
- Board/UART/Python：`4/4 PASS`
- Clang ASan/UBSan：`35/35 PASS`
- Legacy tiny/standard/full-diag ARM builds：PASS
- V2 AT32F415 ARM build：PASS
- Source contract：`68 C/H、8 CMake、8 README PASS`
- CMake formatter：`29/29 PASS`
- C/H formatter：PASS；legacy macro tables are explicitly formatter-protected to avoid unrelated reflow
- `Scope falconfoc`：PASS
- `Scope all`：PASS
- `git diff --check`：PASS

The final quality gate was rerun after the Host contract include fix. The first attempt correctly exposed that some historical Host targets lacked the new platform include directory; after adding that directory to `themis_host_test_contract`, all 45 Host tests passed.

## 六、仍需真实硬件测量的事实

- 150 MHz oscillator/PLL accuracy and timer-clock tolerance。
- TMR1/TMR5/ADC/SPI interrupt latency and jitter。
- Actual FOC step execution time, TMR5 outer-loop execution time and stack high-water.
- ADC aperture, CSA settling and SPI transaction behavior on the assembled board。
- `osDelay(1)` actual task wake-up jitter；它仍不能被称为硬实时 1 kHz。
- V2 H0/H1 observed period and hardware safety response。

本轮只是时序事实与所有权收敛，不是实板实时性或功率级安全验收。

