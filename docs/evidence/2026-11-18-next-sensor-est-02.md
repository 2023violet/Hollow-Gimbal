# NEXT-SENSOR-EST-02 | Alpha-Beta 局部残差数值精度修复

执行日期 2026-11-18（America/Los_Angeles）。工作树 `D:/study/Software/Hollow-Gimbal-vm01a`，分支 `codex/falconfoc-v2-vm01a`，本地 HEAD 和开始时只读查询的远端尖端均为 `f9af51be41aa21ad11cb66444d6e7664ae93213d`。

**软件验收 PASS。关闭 Alpha-Beta 长多圈残差精度缺陷。** 保留同一组 Alpha/Beta 增益和生产 H1 的 LPF 来源。累计跨 ±100 万机械圈时，正式 C11 测试的 Tracker 速度逐样本相同。冻结的 EST-01 爬行回放中，长多圈速度 RMS 从 165.424823 降到 0.0670448513 rad/s，等于修复后普通位置。

[原始证据 ZIP](2026-11-18-next-sensor-est-02/raw-evidence.zip) 包含 RED 和 GREEN 轨迹、前后回放、源和工作树指纹、验证命令及 ARM 产物分析。

## 正式 RED → GREEN

先向 `test-foc-sensor-reference` 添加同轨迹三原点断言，生产代码仍取上轮已验收的本地源。Clang 22.1.8 成功编译。旧代码在 `worst_origin <= origin_tolerance_rad_s` 失败，CTest 退出 8。CPR 16,384、正常方向、LUT 开启和抖动采样的首个正式检测中，旧代码的跨原点最大逐样本速度差为 634.623652 rad/s。0.9 rad/s 爬行的 RMS 在普通位置约 0.0703984 rad/s，在两个长多圈原点均为 0.9 rad/s。该轨迹的静止段用于先建立状态，冻结的 EST-01 长圈夹具使用另一套启动和运动序列，两组 RED 数值对应各自的输入。

固定的跨原点容差为 1e−6 rad/s，独立参考容差为 5e−5 rad/s + 2e−6 × |ω_ref|。Oracle 对解析轨迹独立量化，独立计算 LUT 插值、整数舍入、周期展开和方向，对期望整数增量用 Double 累加出接近零的坐标后执行 Alpha-Beta。原有 65,536 CPR 独立速度、状态和错误断言继续执行。RED 源、构建输出、测试命令、输入、断言和退出码归档。冻结断言和容差在第一次 GREEN 前后相同。最终测试用 `--trace` 导出完整结果，并加入局部偏移逐样本不变量。

| CPR | 方向 | LUT | 采样 | 爬行 RMS，三原点相同 (rad/s) | 最大 Oracle 差 (rad/s) | 最大局部偏移绝对值 (rad) |
| ---: | --- | --- | --- | ---: | ---: | ---: |
| 16,384 | 正常 | 开 | 变化 | 0.070402692 | 5.5900e−6 | 0.001554954 |
| 16,384 | 正常 | 关 | 固定 | 0.068850322 | 4.4517e−6 | 0.001597925 |
| 50,001 | 反向 | 开 | 变化 | 0.045911625 | 8.4301e−6 | 0.001524203 |
| 50,001 | 反向 | 关 | 固定 | 0.045781889 | 5.9069e−6 | 0.001592631 |

固定采样 500 µs。抖动以 500 µs 为中心，时间戳跨过 uint32 零点。每组含静止、0.9 rad/s、正负恒速、加速、减速、阶跃、反转、停止、重启、160 至 163 个重复修正码以及整圈相位过零。正常／反向配置分别验证正／负向机械相位边界。三原点的局部偏移也逐样本相同。四组全轨迹最大 |ω| 为 44.5403824 rad/s，最大 |偏移| 为 0.001597926 rad。分段真值误差与峰值见 `logs/green-test-trace.log` 的 `LOCAL` 行。表中爬行统计包含从静止启动的收敛样本。

## 生产改动和数值域

旧计算使用绝对 Float32 累计位置 `z` 和绝对 Tracker 位置 `x`。预测为 `x + ωh`，残差为 `z − (x + ωh)`。在约 ±6.28e6 rad 时，绝对 Float32 角度的 ULP 为 0.5 rad，远大于单帧编码器位移。新状态 `tracker_local_offset_rad` 定义为上次接受时的估计位置减去实测位置。设旧偏移为 `e`，已校正、展开和定向的原有每帧弧度增量为 `d`，则：

```text
prediction = e + ωh
residual   = d − prediction
e_new      = (alpha − 1) × residual
ω_new      = ω + (beta / h) × residual
```

`(alpha − 1) × residual` 由 `prediction + alpha × residual − d` 化简，提交时即以当前测量为锚点。锚点沿用现有 `position_counts`，与时间戳、修正码和滤波结果在同一成功分支提交。Tracker 残差只用 `delta_rad` 和局部 Float32 状态。绝对 Float32 位置仍生成 `mechanical_rad` 与拒绝时的缓存读数，输出保留大行程绝对分辨率限制。没有增加累计状态、分配、依赖或生产 Double 运算。

数值边界分两层。采样间隔由 `min_dt_us`、`max_dt_us < 2³¹` 和模时间顺序限制。沿用 `max_speed_rad_s × h < π` 的相位连续性条件和原始速度门限。该物理约束作用于观测速度。Tracker 估计沿用原增益和动态范围。设 `h` 为已接受范围内的正 Float32 间隔，保守的乘积安全域在实数中为 `|ω| ≤ FLT_MAX / h`。实现以 Float32 舍入后是否有限作为可表示边界，检测最终偏移/速度即可判定。乘积、预测或残差若为 NaN/Inf，偏移必为 NaN/Inf，包括 `alpha == 1` 时的 `0 × Inf`。偏移或速度更新自身溢出同样导致拒绝。没有另设物理限幅，避免在速度阶跃、可变 dt 或合法增益下改变 Tracker 响应。注入 `FLT_MAX × 1.5 s` 的测试验证预测乘法溢出被拒绝。

第一合法帧建立位置，第二合法帧以差分速度和零偏移初始化 Tracker。Reset 清除偏移与速度。对已有非零偏移的实例，验证 Duplicate、短间隔、Timeout、Frame error、无效原码、速度越界及已锁存后的拒绝。验收断言覆盖两侧 int64 精确边界上的安全更新与下一帧溢出拒绝，以及 NaN、Inf、预测乘法溢出。拒绝保持上一接受的整数位置、时间、修正码、LPF 和 Tracker，计数按既有规则增长，反馈有效标志为假。速度越界沿用锁存，单纯数值错误沿用原状态策略。

外部调用签名、枚举、单位及结构布局兼容。调用者若直接引用公开上下文的旧内部成员名 `tracker_position_rad`，升级源码时需改名并重新编译。已搜索当前仓库全部引用，旧成员没有仓库内消费者。

## 冻结 EST-01 数值对照

以未修改的 `test-foc-sensor-est --trace` 回放。旧生产代码精确复现冻结 ZIP 中的 36,080 行输入输出、144 行速度指标、9,560 行控制记录和 8 行控制汇总。修复前后输入的时间、原码、修正码、累计计数、真值、Raw、LPF、实验 PLL 保持相同。冻结轨迹三个原点各有 1,400 个 Tracker 输出，修复后对应样本完全一致到 trace 的 9 位有效数字，正式 C11 参考另做未格式化 Float32 的精确比较。以下 RMS 与峰值均为速度误差，单位 rad/s。

| 冻结工况 | LPF RMS | 旧 Alpha-Beta RMS（普通 / −100 万 / +100 万） | 新 Alpha-Beta RMS（三原点） | 旧 Alpha-Beta 峰值（普通 / 任一长圈） | 新 Alpha-Beta 峰值（三原点） |
| --- | ---: | --- | ---: | --- | ---: |
| 0.9 rad/s 爬行 | 0.06359015 | 0.06704663 / 165.424823 / 165.424823 | 0.06704485 | 0.199098 / 197.532434 | 0.199010 |
| 恒加速 | 0.24591500 | 0.20945873 / 159.387057 / 159.387057 | 0.20946030 | 0.345326 / 205.594934 | 0.345273 |
| 恒速 | 0.06734464 | 0.06857843 / 358.266134 / 359.840379 | 0.06858692 | 0.222826 / 601.079712 | 0.222742 |

| 普通位置，冻结 Holdout/LUT 开启 | LPF RMS | 旧 Alpha-Beta RMS | 新 Alpha-Beta RMS | 旧 → 新 T90 (µs) |
| --- | ---: | ---: | ---: | --- |
| 静止/抖动 | 0.131578 | 0.133414 | 0.133380 | — |
| 爬行 | 0.059700 | 0.063557 | 0.063560 | — |
| 加速 | 0.339096 | 0.287686 | 0.287691 | — |
| 减速 | 0.447543 | 0.375982 | 0.376002 | — |
| 阶跃 | 0.070537 | 0.068879 | 0.068951 | 4,486 → 4,486 |
| 反向 | 0.075619 | 0.078260 | 0.078242 | 4,486 → 4,486 |
| 停止 | 7.39e−7 | 5.46e−4 | 2.37e−8 | — |

普通轨迹全部分段最大 RMS 差 0.000834606 rad/s，最大峰值差 0.000834433 rad/s，均出现在 LUT 关闭的停止段，修复后误差降低。所有原统计中的 T90 一致，包括 LUT 关闭的阶跃和反向。重复码仍有量化和启动瞬态，冻结长圈爬行的新峰值回到普通位置的 0.199010 rad/s。实验 PLL 的冻结裁决仍为 `KEEP_EXISTING_ESTIMATORS`，加速和减速准入仍失败。完整 LPF/Alpha-Beta/PLL 数值见 `data/dynamic-metrics.csv`。

## 控制链与原专题回归

回放 Virtual Encoder → Sensor → H1 → H0 → Pipeline → Current 的 Velocity/Position 研究链。A/B 的 LPF 臂和 D 的实验 PLL 臂前后 7,170 条控制记录和 6 条汇总完全一致。隔离的 Host C 臂使用 Alpha-Beta，仍在每种模式恰好接受 1,195 个 UPDATE，Velocity/Position 饱和计数分别为 252/6，首次解除饱和时间为 400/200 µs。H0 电角 RMS 分别为 0.00072414 → 0.00072457 rad 和 0.00077388 → 0.00077654 rad。生产 Board1 与默认 H1 路由保持 LPF，测试 C/D 的选择在原有 Host 专用入口中执行。

每个有效控制周期正好一次 UPDATE。STOP 和最终候选失效时 INHIBIT，故障与锁存标志相同。原链的角预测断言和带 Session、过期观测及候选拒绝的 Runtime 11 项回归通过。System Angle 全 2,588 行（含表头）与基线相同。Encoder LUT 01A 线性化 11 行，01B Holdout 369 行与原采集导出 80,276 行，Motor-ID 01A 轨迹 677 行和 01B 质量 36 行，均逐行相同。

## AT32F415 Cortex-M4 soft-float

Arm GNU Toolchain 14.3.Rel1，独立对象用 `-std=c11 -mcpu=cortex-m4 -mthumb -mfloat-abi=soft -Os -ffunction-sections -fdata-sections -fstack-usage` 和全警告。锁定镜像对比 EST-01 已验收的同工具链产物。对象测量取本次 32 路径保护快照与当前 Sensor 源独立重编译。

| 指标 | 前 | 后 | 差 |
| --- | ---: | ---: | ---: |
| Encoder Context | 96 B | 96 B | 0 |
| Encoder Feedback | 56 B | 56 B | 0 |
| Encoder 对象 text | 2,206 B | 2,214 B | +8 B |
| Encoder 对象 data / bss | 0 / 0 B | 0 / 0 B | 0 |
| `foc_encoder_update` text | 1,088 B | 1,096 B | +8 B |
| `foc_encoder_update` 静态栈 | 88 B | 88 B | 0 |
| 锁定镜像 Flash | 38,520 B | 38,528 B | +8 B |
| 锁定镜像 RAM（链接器） | 3,784 B | 3,784 B | 0 |

反汇编中 `foc_encoder_update` 有 398 → 401 个静态指令位置（含一处新增对齐 NOP）。Tracker 分支的算术调用用一次 `fsub` 替换一次 `fadd`，乘除与有限性检查的静态调用数相同。新增的普通指令用于常数装载和局部状态取值，已播种且通过更新路径的 Tracker 算术块由 24 条增至 26 条（均调用 8 次 helper），共同的成功后检与提交尾段均为 99 条。完整反汇编保留所有分支，静态分析不测执行周期。两版 `l2f` 和 `ui2f` 分别均为 4 和 3 个静态调用点。整数展开、加法、溢出比较与转型开销沿用原路径，绝对位置的转换仍供反馈使用。Tracker 残差读取已计算的 `delta_rad`，没有新增 64 位运算或 Double helper。链接镜像二进制 SHA-256 为 `1c82f9971ba3145c7c7f6e63961e70a1cada0613beb4396a0d0bd6e5d1f89947`。

## 回归、复核及文件归属

| 验证 | 结果 |
| --- | --- |
| Clang Debug 正式 Host 模块 | Sensors 2/2、Core 15/15、Runtime 11/11、Calibration 5/5、Simulation 22/22 |
| GCC Debug Simulation | 22/22 |
| Clang ASan/UBSan | Sensors 2/2、Core 15/15、Runtime 11/11、Calibration 5/5、Simulation 22/22 |
| 独立锁定固件 Host | Board1、Board Config、Serial、Python CLI，共 4/4 |
| Firmware 与严格门禁 | 锁定 ARM 构建、三固件预设、`./run.sh build`、Source Contract（92 个 C/H、9 个 CMake、9 个 README）、CMake 格式 30 项、严格 `Scope all -RequireCMakeFormatter`（含正式 Host 45/45）通过 |
| 修改文件格式及 Git 空白检查 | 4 个 C/H 的 clang-format 和 git diff --check 通过 |
| 独立只读复核 | 无 P0/P1。1 项 P2 文档澄清已关闭，见数值域的观测上界与可表示上界，无未关闭 P2 |

门禁和旧固件构建在独立临时副本上执行，按基线提交叠加当前代码。各模块、锁定固件、Host 回放和 ARM 对照使用当前工作树的源码及隔离构建目录。本轮生产变更只有 `sensors/src/foc_encoder.c` 和 `sensors/include/foc_encoder.h`。测试变更位于 `sensors/tests/test_foc_encoder.c` 和 `sensors/tests/test_foc_sensor_reference.c`。后者是本轮新出现的已跟踪修改。维护日志只追加本轮条目，报告和原始证据为本轮新路径。原 32 个待提交路径中，除本轮追加维护日志和修改三个 Sensor 文件外，其余 28 个原路径字节级保留，最终共有 35 个待提交路径。历史研究源码、LUT/辨识代码及报告 ZIP 均保留。无暂存、提交或推送。

Sensor 专项可按当前软件范围结束，下一阶段可单独开启 SERVO-01，并沿用生产 LPF 及冻结的估计器验收基线。此轮止于 EST-02。

G3-SAFE HARDWARE NOT VERIFIED  
G3-PWR LOCKED  
AT32F415 WCET NOT VERIFIED  
MOTOR PHYSICAL OPERATION UNVERIFIED

本轮只有软件和静态产物证据，没有刷机、上电、Flash 写入或真实 PWM 操作。
