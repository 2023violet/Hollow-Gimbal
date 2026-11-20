# Board1 锁定态目标时序采集

本流程用于 PRODUCT-01 的锁定态见证。实际 SWD、烧录、上电、修改调试设置、栈染色或硬件测试均须按各自的现场授权执行。记录器和时序日志没有物理输出入口。输出级独立隔离、电机三相断开和已核实 PCB/固件身份是本轮锁定台架的入场条件。

## 构建与离线检查

使用 [固件构建入口](README.md#build)。保留 toolchain 注入的 Cortex-M4/Thumb/soft-float 参数，以 `-DCMAKE_BUILD_TYPE=Debug` 为 GDB 保留 DWARF。项目目标仍使用 `-Os`。两组实板对照均开启 `-DFALCONFOC_RT02_TIMING_TRACE=ON`，分别设置 `FALCONFOC_BOARD_CAPTURE=OFF` 与 `ON`。留存 ELF、BIN、链接表、栈用量和对应 SHA-256，并核实装载与测量的是同一份构建。当前交付还提供两组 DWT OFF 的对照映像。

```powershell
cmake -S firmware/at32f415 -B <build> -G Ninja `
  -DCMAKE_TOOLCHAIN_FILE=tools/cmake/arm-gcc-toolchain.cmake `
  -DMCU_CPU=cortex-m4 -DMCU_FLOAT=soft -DMCU_FPU=none -DCMAKE_BUILD_TYPE=Debug `
  -DFALCONFOC_RT02_TIMING_TRACE=ON -DFALCONFOC_BOARD_CAPTURE=ON `
  -DFALCONFOC_BOARD_CAPTURE_BUILD_TAG=0x20261119
cmake --build <build>
```

用 `arm-none-eabi-gdb --batch -nx <elf> -ex "whatis rt_trace"` 验证本地调试符号。检查 `arm-none-eabi-nm <elf>`，`foc_board_pwm_*model` 相关 Host 模型符号不进入 ARM 映像。生产锁定的 GPIO、外设和串口路径沿用相同的既有指令。

## 授权后的最小见证

1. 在完全断电后核对实际 PCB 版本、MCU/驱动料号和丝印。记录 PWM 输入对应、可独立去能的路径、nSLEEP、nFAULT、电源树以及 Break 路由。如果板上不存在独立的可验证关断，应先由硬件负责人审查隔离夹具或 PCB 修订。实物与图纸不符时暂停。
2. 获准进行锁定态采集后，断开三相，验证蜂鸣/仪表所见隔离、输入电源限流与现场急停，记录布线和安全边界。观察复位、空闲和调试停止时的供电、六路输入、nSLEEP/nFAULT、输出端是否有意外变化。只以批准的供电和部署范围操作。出现意外输出、温升、过流、身份不符、隔离失效或异常复位则结束该轮。
3. 依据 [SRAM 采集手册](BOARD_CAPTURE.md) 保持 8 ms 窗口内连续运行，READY 后读取。用分析仪记录 CS/SCK/RX 和必要的独立参考。TMR1 比较、ADC 实际采样孔径与 PCCE 报告均须对齐见证时间。软件快照提供在可见点的代理。物理采样有效窗和实际装载与 PCCE/IRQ 系列分开证明。
4. 以匹配 ELF 的 GDB 在现场批准的前台观测点停核，避免停在 IRQ 服务或多字发布过程中。取一份日志，然后存原始输出；重跑新场景写新日志。工具拒绝异常模式和截断日志。示例命令在 GDB 已按台架流程连接后使用：

```gdb
source <repo>/tools/falconfoc-calibration-replay/locked_timing_export.gdb
set logging file locked-timing.txt
set logging enabled on
falconfoc-observe-locked-timing
set logging enabled off
```

5. 使用同场景的输出锁定、采集完整性、计时和独立仪器证据对照 Capture OFF/ON。保留 IRQ 和 H0/H1 的独立跨度、当前采样身份、前台每次服务、Runtime missed/late/budget 计数。可见的跨度可能包含抢占。RT-03 的最新单事务快照不等于全段最大值；Capture ON 冻结记录可给出窗口内 H0/H1 的观测最大值。需另行覆盖饱和、故障、拒绝与正常分支，锁定台架无法安全触达的分支明确标未覆盖。记录实际 CPU 时钟、采样率、各场景的持续时间和外部仪器误差，推导严格 WCET 时另列干扰、阻塞和上下界假设。
6. 如果获得独立的栈染色授权，以相同链接表及受控停核点记录尚未使用的 4 字节对齐栈区间。全段预置 `0xA5`，避免开始于静态区、堆或已用栈，监测期间不占用此段的其他用途；完成后只读转储该区间。报告记录最低变化地址对应的最小观测深度，尾部未变化、未探测范围和探针干扰另记。没有物理样本时采用静态资源记录。

## Host 校验

```powershell
python -B tools/falconfoc-calibration-replay/locked_timing.py locked-timing.txt --clock-hz <verified-hz> --source BOARD_DECLARED --variant CAPTURE_ON --capture board1.bin
```

OFF 组使用 `--variant CAPTURE_OFF`，没有 `--capture`。若独立得到标记栈数据，加 `--stack-dump stack.bin --stack-range <low> <painted-end> <stack-top>`。以逐文件 SHA-256 关联证据，记录场景、硬件身份、该轮封存镜像和外部仪器材料。脚本只读本地文件，Board 数据来源保留 `PROVENANCE_UNVERIFIED`。捕获完整及计时观测不授予物理安全、标定、运行或 WCET 验收状态。对原始采集和 SOFT-06 的处理仍用 [Board1 归档流程](BOARD_CAPTURE.md)。