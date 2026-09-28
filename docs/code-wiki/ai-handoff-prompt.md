# AI 接手提示词：AT32F415 + DRV8311H 低压开环根因定位

下面内容可直接复制给下一位 AI。它描述的是当前工作树和最后一次实测事实，不要把
历史计划、旧镜像或旧哈希当作当前状态。

```text
你正在接手项目 D:/study/Software/Hollow-Gimbal。

任务：继续定位 AT32F415 + DRV8311H + 12N14P 云台电机的低压电压开环卡死/失步和
MCU 软件观测电流偏高问题。当前算法、SVPWM 马鞍波、duty 和 CCR 已基本确认正确；首要问题是
查清 CCR 之后的六路 PWM 引脚、DRV8311H、三相桥、PCB/接线和绕组是否真正形成完整三相线电压。
不能继续盲目调 PI、编码器、保护阈值，也不能把软件 trace 当成物理电流或物理 PWM。

当前状态必须保持：
OPEN_LOOP_MATH_AND_DUTY_PATH=PASS
OPEN_LOOP_ENCODER_ROLE=ENTRY_SEED_ONLY_NOT_FEEDBACK
ENCODER_CALIBRATION_FRAME=UNVERIFIED
TIMER_PIN_DRV_OUTPUT_PATH=UNVERIFIED
PHYSICAL_LINE_TO_LINE_VOLTAGE=UNVERIFIED
MOTOR_PHYSICAL_OPERATION=UNVERIFIED
OPEN_LOOP_ROOT_CAUSE_STATUS=UNRESOLVED
REAL_BENCH_TEST_STATUS=BLOCKED
ADC_MEASUREMENT_WINDOW=REPRODUCIBLY_TRIGGER_DEPENDENT
PHASE_OFFSET_CANDIDATE=REJECTED_AS_UNREPEATED
ENCODER_DIRECTION_CANDIDATE=SUPPORTED_NOT_CONFIRMED

一、当前硬件和软件事实

1. MCU 是 AT32F415，当前源码事实来源为 platform/at/at32f415/generated/。
2. DRV8311H 为硬件接口版本；nFAULT 没有 MCU 端点，nSLEEP 由板级网络自动拉高，
   不要新增或假设 nFAULT/nSLEEP 软件 API。
3. TMR1 为中心对齐 PWM，ARR=3749，CH4 为 ADC trigger，默认 CCR4=3740；TMR1
   deadtime=0 表示 MCU 没有额外插入死区，不表示 DRV 功率级没有死区。DRV8311H 在当前
   strap 下有内部 VGS 握手和约 450–760 ns 输出死区；Break 未启用、自动输出未启用，
   真实重叠仍未验证。不要把软件构建通过写成波形安全证明。
4. PWM 映射：软件 A -> TMR1 CH3，B -> CH2，C -> CH1。
5. ADC 注入序列：rank1 VBUS=CH1，rank2 IC=CH4，rank3 IB=CH5，rank4 IA=CH6。
6. SPI1 编码器：PB3/PB4/PB5，mode 3，16-bit，MSB first，软件片选 PA15。
7. DRV GAIN strap 按 0.5 V/A 换算，CSAREF 接 DRV_3V3；软件电流仍不是独立相电流。
8. 电机是 12N14P，7 对极；正确 30 rpm 目标是 we=21.9911 rad/s。5 rpm 只属于
   闭环堵转判据，不是开环旋转速度目标。
9. 保护：每相快速 1.00 A，任务级 0.75 A；非电压模式堵转 0.75 A 且 <=5 rpm 持续
   200 ms；0.75 A/3 s 是最大容忍边界，不要求等待。12 V/1 A 不是相电流保护。
10. bring-up 默认 FOC_ENERGIZED_CALIB_ENABLE=0；禁止带电电机校准、编码器校准、
    encoff 搜索、I-f、速度闭环和长时间运行。
11. 纯 CTRL_MODE_VOLT 只在 IDLE/切入模式时使用 theta_elec 一次性播种 theta_sim；运行后
    theta_sim 由 we_volt_now*dt 独立推进。固定相电压命令完全绕过编码器。编码器校准风险影响
    首次种子和未来闭环，但不是纯开环持续失败的直接反馈根因。
12. 板上不是“无校准默认状态”：历史只读日志显示 enc_valid=1、mech_offset=-0.124 rad、
    pole_pairs=7。启动会恢复 phase_order/dir/mech_offset/LUT 的完整 payload；当前存储只校验
    版本和 CRC，没有绑定电机、板号、PWM map 或校准算法来源。
13. bench PWM map 会绕过 payload 中的 phase_order；diag set enc_dir 只改方向，不同步
    变换或失效 offset/LUT。因此既有 map、enc_dir 和 theta0 A/B 不能当作完整校准坐标的
    闭环校准证明；也不能据此把编码器提升为纯开环主根因。
14. 校准代码以 target-measured 生成均值，LUT 在运行时相加，而 mech_offset 在运行时
    被相减，存在待证伪的符号一致性嫌疑。历史 -0.124 rad 在 7 对极下使加/减两种消费方式
    相差约 1.736 rad_e；该量级吻合不是唯一根因证明，禁止直接取反 offset。
15. 当前 TMR1 为中心对齐 ARR=3749，还启用 IS2 从模式 RESET；TMR3 ARR=7499 并以 overflow
    提供同步触发。该配置与同源参考树的独立 TMR1、ARR=7499、deadtime=15 不同。它是引脚
    时序候选，不是已确认错误，禁止未经证据直接照搬参考配置。
16. 网表中 INHA/INLA...INHC/INLC 均有 MCU 和 DRV 两端；OUTA/OUTB/OUTC 每个只列出 U4
    自身引脚，没有电机连接器/焊盘组件端点。这不证明实板断路，但说明输出连续性和顺序没有
    被设计资料闭合证明。

二、当前唯一指定实机镜像

ELF:
build/fw-full-diag-carrier-bench-400ms-debug/output/falcon-foc-at32.elf
SHA256:
59B78D648511926F8AD47213CA35E49C0D536697BE814B1B77AF64F84E949A03
编译宏：
TM_BENCH_DIAG=1
TM_FEATURE_VOFA=0
FOC_ENERGIZED_CALIB_ENABLE=0
TM_MOTOR_BENCH_RUN_BUDGET_MS=400
串口：COM3，115200 8N1；供电背景：12 V/1 A。

如果哈希不一致，先重新构建和记录；刷写只能使用显式 ELF 和 OpenOCD verify，禁止使用
裸 run.sh flash。当前工作树很脏，必须保留所有用户/生成文件，禁止 git reset、checkout、
clean、删除、批量覆盖或自动提交。

三、已取得的最新实机证据

1. 动态 ADC trigger：vq=1.00 V、we=21.9911 rad/s 时，CH4 compare 1874 的软件峰值
   约 0.013/0.017/0.011 A，2300 约 0.058/0.259/0.020 A，2812/3740 的 A/B 约
   0.24..0.26 A；各点重复两次，VBUS 11.87..11.93 V，无 fault。
2. 静态隔离：把 we 固定为 0 后，1874/2300/2812/3740 仍分别出现低读数、2300 B 相
   转折、2812/3740 A/B 高读数。说明 trigger 相关性不以旋转场为必要条件。
3. 固定矢量 vq 梯度（we=0、trigger=3740）：0.25/0.50/0.75/1.00/1.50/2.00 V 的
   软件峰值依次约为 0.024/0.018/0.043、0.078/0.037/0.112、0.188/0.191/0.022、
   0.186/0.163/0.265、0.254/0.277/0.041、0.430/0.408/0.628 A；无 fault。
4. 30 rpm 参数下完整电压窗口：1.0 V 首轮/重复、1.5 V、2.0 V 的软件峰值约为
   0.257/0.256/0.033、0.273/0.277/0.233、0.407/0.435/0.420、0.586/0.633/0.578 A；
   软件场角推进，encoder 只部分跟随。
5. 2.5 V、we=21.9911 rad/s、约 350 ms：峰值约 0.680/0.754/0.650 A，VBUS 11.83..11.91 V，
   theta_sim 约 1.11 rad，theta_elec 约 4.68 rad，we_now 约 16.72 rad/s，无 fault。
   已接近 0.75 A 预警包线，禁止继续升压或延长时间。
6. 2 V trigger A/B：1874 的峰值约 0.579/0.376/0.432 A，3740 约 0.539/0.577/0.157 A；
   两者包络接近但相分配不同，不能把 1874 选择为“安全正确窗口”。
7. 纯 CTRL_MODE_VOLT 的 duty 不使用电流 PI。ADC trigger 依赖是确定的测量可信度问题，
   但当前调用链不支持把它写成开环不转的直接运动根因。
8. 低幅 0.05 V 命令仅占 12 V 母线约 0.42%，小于或接近 DRV 内部 deadtime/传播延迟的
   周期占比，早期低幅 map 矩阵不具备充分相序区分力。
9. 健康三相的完整排列只改变旋转方向或相位原点；所有 PWM map 都不能获得稳定旋转时，
   单纯“相序错误”不是足够解释，更应怀疑缺相、互补时序、弱桥臂、接线或绕组。
10. 静态 1 V 固定矢量中曾观察到近似 KCL 自洽的两相电流，说明部分功率链能导电；它没有
    覆盖完整电周期和六个扇区，不能证明三只桥臂都健康。

原始日志：
tmp/bench/20260830_103200_full_hardware_root_cause/
tmp/bench/20260830_105417_static_trigger_boundary_1V/
tmp/bench/20260830_111200_static_voltage_gradient/
tmp/bench/20260830_112000_dynamic_trigger_compare_2V_30rpm/
tmp/bench/20260830_112600_voltage_2p5V_30rpm/

四、必须遵守的执行纪律

1. 当前默认策略是不再新增实机扫描。先读源码、数据手册、网表和既有日志，审计 CCR 之后的
   定时器/引脚/DRV/功率链；若物理证据缺失，只记录为未验证，不要用编码器或 ADC 猜测补齐。
2. 第一次新数据前不要改 motor_drv.c、foc_ctrl.c、foc_config.h、PWM 相序、ADC rank、
   编码器方向、保护阈值或电机参数；若必须修复，先加失败 host 测试，再最小改单变量，
   然后重新跑 host、CTest、外设契约和固件构建。
3. 每项实机测试前 SWD reset，确认 axis=idle、fault=0、duty=0/0/0；只设置一个变量，
   运行短窗口；正常停机用 state disable，紧急异常直接切 PSU，不使用 state stop。
4. 禁止发送 cal motor、cal enc、if、encoff、diag param、diag pid 和任意未列入白名单的
   启动命令。运行中不打印、不写 Flash、不做复杂在线分析。
5. 任一相电流达到 1.00 A、接近 0.75 A 且无下降趋势、VBUS 超出 10.8..13.2 V、
   ADC/角度/速度/duty 异常、卡死/尖叫/剧烈抖动、快速发热/异味、串口失联或无法确认
   PWM 关闭，立即切 PSU；停机后再保存 fault/trace 并分析。
6. 没有示波器、独立相电流、机械测速和温度时，不得宣称 PWM、死区、SOx、DRV 热稳定、
   实际绕组电流、30 rpm 机械转速或产品级连续运行已验证。

五、建议接手后的最小动作

1. 读取 docs/code-wiki/project-summary.md、debugging-summary.md、hardware-and-drv8311h.md、
   open-loop-root-cause-deep-analysis.md 和本提示词；读取上述最新日志，不重跑已完成矩阵。
2. 记录 git status --short --branch 和当前 ELF SHA256；不清理工作树。
3. 对照 foc_ctrl.c、motor_drv.c、wk_tmr.c、AT32F415 TMR 手册、DRV8311H 真值表和 PCB 网表，
   建立边界真值表：theta/duty/CCR -> TMR CHx/CHxC -> INHx/INLx -> OUTx -> 电机端。特别审计
   TMR1 的 IS2/TMR3 RESET 同步与 CCR buffer/互补输出时序，不直接修改配置。
4. 独立保留 ADC 测量链问题：对照 motor_task.c、foc_ctrl.c、wk_tmr.c、wk_adc.c 和 DRV
   数据手册解释 1874->2300 的 PWM 状态边界；不要把低读数点直接选为正确窗口。
5. 编码器校准 producer/store/consumer 不变量作为未来闭环专项保留，不得再将其列为纯开环
   当前首要根因，也不得直接取反 offset。
6. 本轮原则上不做 COM3、刷写或 PWM 动作。只有用户重新授权实机验证时，最有信息量的缺失
   证据才是 MCU 六路 PWM、DRV 输入/输出和 U-V/V-W/W-U 实际线电压，而不是更多 theta0/map 扫描。
7. 只有在同一最小命令相同条件重复两次、唯一主根因成立、单变量修复后再次两次改善，
   且 host/CTest/静态契约/固件构建全部通过时，才可写 OPEN_LOOP_ROOT_CAUSE_STATUS=IDENTIFIED。
   否则保持 UNRESOLVED 和 REAL_BENCH_TEST_STATUS=BLOCKED。
```

## 交接文档索引

- [`project-summary.md`](project-summary.md)：项目、架构、硬件、运行方式和当前状态。
- [`debugging-summary.md`](debugging-summary.md)：调试时间线、数据、推导、候选根因和安全收口。
- [`hardware-and-drv8311h.md`](hardware-and-drv8311h.md)：原理图和 DRV8311H 深度资料。
- [`open-loop-root-cause-deep-analysis.md`](open-loop-root-cause-deep-analysis.md)：基于既有证据的反证、校准坐标和时序推导。
- [`motor-real-bench-test-plan.md`](motor-real-bench-test-plan.md)：详细实机测试门禁和矩阵。
