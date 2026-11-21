# NEXT-CAL-BOOT-01｜FalconFOC 未知电机初始化与初始电角辨识

**NEXT-CAL-BOOT-01 HOST BOOTSTRAP PASS**

**PHYSICAL ALIGNMENT NOT VERIFIED** · **PHYSICAL CALIBRATION NOT VERIFIED** · **TARGET MCU WCET NOT VERIFIED** · **G3-PWR LOCKED**

日期为 2026-11-20。Generic 与 Salient 均通过已知配置、仅 Offset 未知、极对数与方向及 Offset 均未知三条 Host 路径。开路扫描获得粗参考，既有 Encoder LUT 和 Rotor/Offset 估计器细化电角，再由既有 Calibration Application 审核、读回并验证新 Host Runtime。

[原始文件索引](2026-11-20-next-cal-boot-01/README.md) · [原始证据 ZIP](2026-11-20-next-cal-boot-01/raw-evidence.zip) · [SHA-256](2026-11-20-next-cal-boot-01/raw-evidence.sha256)

## 1. 基线与数据来源审计

工作树为 `D:/study/Software/Hollow-Gimbal-vm01a`，分支 `codex/falconfoc-v2-vm01a`，起止 HEAD `c93655efbce28f6a81fcce2e5c20ac61f45cab76`，索引为空。起始状态含 PRODUCT-02、CAL-ARCH-02、CAL-FLUX-01 的未提交实现、测试和证据。存档保留实际起始状态。ARCH-02 和 FLUX-01 的证据包及内层清单复核 176/176、181/181。ARCH-01 前置功能以 ARCH-02 工作树及其测试为复核依据。 [ARCH-02](2026-11-20-next-cal-arch-02.md) · [FLUX-01](2026-11-20-next-cal-flux-01.md)

| 环节 | 历史 Host 先验和观测 | 本轮实测初始化路径 |
| --- | --- | --- |
| 启动身份 | 原虚拟 Profile 预置极对数、原始编码器方向、ADC/编码器/相序映射及机械坐标。 | 未知模式把拟合请求中的极对数与方向清零。ADC 通道与量程、相序、编码器 CPR/有效性、采样时钟、电机/编码器/装配身份及限值仍需可信配置或观测。先取得 48 个 ADC 零偏样本，核对母线电压、独立无电流状态与安全资格。 |
| Encoder LUT、Rotor、Offset | LUT 使用理想机械轴角。Rotor 和 Offset 直接使用 `foc_virtual_motor_observe().electrical_position_rad` 作为独立理想参考。 | 新开路扫描及细化采集从各自的共时端电压与有符号轴速重建磁轴。LUT 使用独立轴角读数并单独采集 Holdout。已知 Offset 只可从身份匹配、已审核的数值配置恢复。 |
| R/L 与 Flux | 用 Plant 真实电角构造 d/q 激励和投影。端电压、ADC 和区间来自原虚拟试验，模型含理想的约束转子及固定速夹具。 | d/q 相位从合格 LUT、有向编码器计数、极对数与最终 Offset 计算。R/L 保留静止夹具；Flux 保留控速夹具、实际虚拟端电压及独立轴速验证。两种夹具仍是 Host 假设。 |
| J/B 与控制评价 | Plant 内部电磁转矩和负载作为理想虚拟见证。闭环评价用 Plant 真值作为独立 Oracle。 | J/B 通过窄接口读出虚拟等效轴转矩和已知载荷，q 激励取最终校准角。测试依旧使用独立 Oracle 检查估计误差和控制表现。 |

为保持旧证据可重复，未带 `--bootstrap` 的历史兼容台架仍可使用理想角度和机械见证。新增的两个观测模式通过独立采集接口取得轴速、端电压或解码器角度。静态回归禁止新扫描与拟合器访问 Plant 参数、状态或宽接口真实电角，并核对后续角度来自 LUT 与整数计数。电机模型中的磁链、方向、磁轴及载荷用于生成模拟传感器信号，拟合候选由观测计算。

## 2. 三条路径与开路采集

| CLI 路径 | 前提与结果 |
| --- | --- |
| `--bootstrap known --load-config <v1>` | 核对 FCAL v1 长度、CRC、设备身份与换相配置，显示 `REFERENCE_REVIEW_REQUIRED`。同一 v1 审核号经显式提交后显示 `READY_WITH_TRUSTED_REFERENCE` 并通过既有应用和控制试验。全程无新扫描。 |
| `--bootstrap offset` | 两个夹具提供可信极对数与原始编码器方向。开路扫描检查其一致性，取得粗参考后用新 LUT 和独立端电压窗口拟合最终 Offset。 |
| `--bootstrap unknown` | 从空极对数与空方向开始，搜索 1–14 对极，选择唯一一致方向和粗 Offset。通过后进入同一细化流程。 |
| `--bootstrap inspect` | 核对配置和测量前提，输出缺失项和是否需要采集，无扫描与应用。 |

选择了外部虚拟机械驱动的动态磁轴参考。它将转轴有界地带动，并保持三相绕组开路和原软件 Runtime 的已确认 INHIBIT。模型以 250 µs 步进，机械目标 ±8 rad/s，绝对限值 10 rad/s，外部驱动转矩上限 0.3 N·m。四段按正、反、正、反交替，每段稳定约 280 ms，每 4 ms 共时采集 64 条，末尾制动。每步确认允许条件、相电流和信号相对母线余量。两套夹具均捕获 256 条，开路窗口由 9,600 到 2,433,600 µs 虚拟时间，确认输出 INHIBIT 且有效 UPDATE 为零。

已核对相序及极性约定时，开路 `eα,eβ` 与独立有符号机械角速度 `ωm` 给出 `atan2(-sign(ωm) × eα, sign(ωm) × eβ)`。定子命令场、真实转子磁轴和原始编码器角各有自己的坐标。初始阶段没有定子命令场，端电压参考需要可见转动、已知接线极性和足够信噪比。与静态励磁相比，该方案在已有 Host 安全边界下可直接核对机械跟随和反转一致性。绝对端电压相位偏差会进入 Offset，实际测量链需校准。

`foc_bootstrap` 检查身份、序列、采样年龄、轴速符号、编码器计数展开、半周期混叠及样本跨度。每段调用现有 `foc_calibration_rotor_finalize()` 的整数候选搜索。四段须单独通过，且极对数、方向和 Offset 重复性相符。当前粗参考门槛包括每段至少 0.70 rad 机械跨度、圆周最大残差 0.085 rad、候选间距至少 0.08 rad、四段 Offset 最大差 0.10 rad。搜索通过才标记 `COARSE_ELECTRICAL_REFERENCE`，事件中 `runtime_applicable:false`。 [C11 拟合](../../calibration/src/foc_bootstrap.c) · [Host 采样器](../../simulation/src/foc_virtual_bootstrap_capture.c)

## 3. 实测结果与方向约定

下表来自 `--bootstrap unknown` 的原生事件与 CSV。两夹具执行 `offset` 路径也通过相同判据。坐标以各自的电机、编码器、机械 Frame 与相序身份绑定，粗参考 Capture ID 为 30。

| 指标 | Generic | Salient |
| --- | ---: | ---: |
| Motor / Encoder / Frame / Phase Order ID | 201 / 101 / 310 / 0 | 202 / 102 / 310 / 1 |
| 极对数 / 原始编码器方向 / LUT 后相对方向 | 4 / +1 / +1 | 4 / −1 / +1 |
| 机械跨度最小值，rad | 1.968002 | 1.957456 |
| 粗 Offset，电角 rad | 0.356920 | 0.487001 |
| 粗参考最大圆周残差，rad | 0.049864 | 0.029258 |
| 最小候选分离，rad | 0.542877 | 0.551168 |
| 四段 Offset 最大差，rad | 0.049094 | 0.014792 |
| 最终 Offset / RMS，电角 rad | 0.369966 / 0.000230 | 0.519910 / 0.000247 |
| Encoder LUT Holdout RMS，机械 rad | 0.00006078 | 0.00006048 |
| Rs，Ω | 0.600042 | 0.779821 |
| Ld / Lq，mH | 1.200990 / 1.500557 | 1.700292 / 2.197938 |
| Flux，Wb | 0.02500480 | 0.03196543 |
| Kt，N·m/A 峰值 q 电流 | 0.15002878 | 0.19179256 |
| J，kg·m² / B，N·m·s/rad | 0.00200005 / 0.00600068 | 0.00320012 / 0.00900050 |

LUT Holdout 通过后，Rotor 与 Offset 用新电压窗口和纠正后的原始码计算。Salient 的原始反向只在有向机械计数中应用一次。两者的细化 Rotor 相对方向都为 +1；方向未通过一致性检查会拒绝下游相位。Host 扫描覆盖跨零和非线性编码器，原生测试另覆盖 14 对极、两个编码器方向、接近一圈、约 50 万及超过 4 亿累计圈的两种测试构造、32 位时钟回绕、有限扫描跨度、相邻候选混叠和高噪声。电角计算保持整数周期路径。

## 4. 与工作流、复用和安全应用的衔接

- R/L 正式分轴采集只消费最终合格电角。绕组 Rs 独立测量和分项审核仍走原功能，无需完整换相准入。分轴组合重采可以同时更新 Rs。
- FLUX-01 继续使用已接受的实际虚拟端电压区间与 d/q 电流。新区间每 50 µs 用独立轴速插值并与计数参考核对。两夹具各 512 个区间，正反向各 8 窗，Flux 拟合残差 RMS 为 0.000745 V 和 0.000676 V。磁轴身份 402 与复用依赖绑定。Kt 从该磁链推导。
- J/B 使用独立的有符号速度、等效轴转矩和已观测负载。现有 J/B 估计器及 Kt×Iq 的信任政策沿用。K/D 与闭环增益由既有整定和应用路径生成。
- `plan same`、`measure` 两夹具均保留原采集身份、Session、Capture 和 Revision，无第二次开路扫描。`plan encoder` 使 LUT、Offset、依赖这些映射的 Ld/Lq 和 Flux/Kt 进入 `REMEASURE`，复扫后得到新审核号。历史有独立角度见证的记录继续按原规则处理。参考身份、来源、质量及适用工况随分项记录保留在 Host 进程。
- 新组合 `review_id` 绑定粗参考采集摘要、Flux 证据及 FCAL 数值指纹。用数值指纹批准新组合的请求被拒绝，未创建配置文件。正确审核后由现有应用层初始化独立 Runtime 和 Encoder 并读取参数。FCAL v1 仍为 548 字节，CRC 兼容，重载以原数值审核号复核，并逐项复现相同控制结果。历史 36 Hz 和现行配置均通过兼容性试验。
- 两夹具每次审核应用执行 16 个闭环用例，覆盖四模式、双向、应用前后，另验证 STOP 和 command-timeout 恢复。130 Hz Current 最坏方向 RMS 在 Generic 为 0.060208 → 0.031219 A，在 Salient 为 0.111083 → 0.031023 A。两套应用读回 48 Hz 阻尼滤波。

## 5. 拒绝与中断证据

每个夹具实跑 14 种 CLI 拒绝情形，退出码均为 2，采集期间有效 UPDATE 均为零。以下结果来自带身份、时间戳和输出证据的原生记录。

| 条件 | 结果 |
| --- | --- |
| 相序身份错误 | `IDENTITY_MISMATCH`，在采样前拒绝。 |
| 已声明编码器方向与观测相反 | `REFERENCE_UNIDENTIFIABLE`，四段虽已采集，仍拒绝发布粗参考。 |
| 过载、极低允许电流 | `measurement_limit`。低电流用例收紧 ADC 零电流容限，拒绝在未确认测量条件下继续。 |
| 部分跟随、卡滞、编码器传输失效、反电势过弱 | `shaft_following`、`shaft_stalled`、`encoder_transport`、`weak_emf`。 |
| STOP、FAULT、超时、会话取消、身份变化、输出确认拒绝 | 原因分别为 `STOP`、`FAULT`、`TIMEOUT`、`SESSION_CANCELLED`、`IDENTITY_MISMATCH`、`INHIBIT_CONFIRMATION_LOST`。各保留 27 条诊断样本，399,850 µs 虚拟时刻结束，原输出适配器仍为高阻。FAULT 观察到 WDT 位 64。 |

扫描只操作 Plant 副本，受保护的原实例无有效 UPDATE。资格和重复性失败清空粗候选，细化阶段失败不会退回使用粗 Offset。在 `ELECTRICAL_D` 或 `FLUX` 后另注入 Workflow 取消，即使附上正常审核号，也返回 `CALIBRATION_CANCELLED`，未发布组合预览、应用、闭环或 FCAL 文件。

## 6. 全量回归与资源

| 验证 | 实际结果 |
| --- | --- |
| Clang Debug、MinGW GCC Debug、Clang ASan/UBSan | 各 116/116。每套含 Core 21、Runtime 13、Calibration 10、Control Plane 5、Sensors 2、Simulation 46、Servo 7、Protocol 1、Board Host 9、示例 1、回放 1。 |
| Python Host CLI | 11/11，配置两个本地模拟器，零跳过。 |
| Strict Scope all、Source Contract、Legacy Host | Strict PASS，Legacy Host 45/45，Source Contract 检查 C/H 196 项、CMake 15 项、README 15 项。 |
| 历史冻结回放 | FLUX-01 独立理想参考的每电机 512 区间、正反向拟合、16 个闭环和 FCAL 哈希逐项匹配。历史 Current 正反样本逐字节复现且维持全窗 FAIL，低压对照仍 FAIL。Servo 冻结数据重放通过 48 Hz 正式质量门槛。Current Startup V1 的原 CSV 哈希一致，Generic 退出码 0，Salient 原失败退出码 1。CQ-03 的近边界 0.03675 N·m 动态超差与旧 0.039 N·m 超载裁决沿用原证据。 |
| ARM Soft-float 锁定构建 | BIN SHA-256 `78d055e1135d23a5c2d08f6db17fbf64622333f429381efa2632401eb58cbda2`，与前置镜像逐字节匹配，Flash 44,452 B，RAM 4,184 B。ELF 无新增 Bootstrap 或 Flux 估计器符号。链接日志保留原 newlib 空 syscall 和 RWX 段警告。 |

Windows x64 Host 实测 `sizeof` 为采样结构 72 B、固定采集对象 18,444 B、粗拟合结果 80 B、复用 Rotor 上下文 1,404 B。一次最多存 256 条扫描样本，逐段复用 Rotor 上下文。这些数值是单对象容量，目标板总栈、水位和 WCET 仍待实测。估计器位于 Foreground，板级锁定产物保持既有构建路径。 [CQ-03 冻结裁决](2026-11-20-next-cq-03.md)

## 7. 复现与实机前提

在仓库根目录用当前 CMake、Ninja、Clang、Python 工具执行。每次选择一个新输出目录，采样和 FCAL 导出采用独占创建。

```powershell
cmake -S simulation -B build/boot-repro -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=clang
cmake --build build/boot-repro
ctest --test-dir build/boot-repro --output-on-failure
python -B tools/falconfoc-cli/falconfoc_cli.py calibrate --simulator build/boot-repro/falconfoc-virtual-calibrate.exe --bootstrap unknown --boot-trace boot-repro.csv
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/gates/check_themis_quality_gate.ps1 -RepoRoot $PWD -Scope all -RequireCMakeFormatter -Summary
cmake -S firmware/at32f415 -B build/boot-arm-locked -G Ninja -DCMAKE_TOOLCHAIN_FILE="$PWD/tools/cmake/arm-gcc-toolchain.cmake" -DFALCONFOC_BOARD_CAPTURE=OFF -DFALCONFOC_RT02_TIMING_TRACE=OFF
cmake --build build/boot-arm-locked
```

归档提供两套 Host CLI 原始 CSV、事件、审核前后 FCAL v1、重放脚本、构建和回归日志。开路方案依赖具备独立外部轴驱动、失效停机、可信机械方向、端电压共模和量程校准及共时触发的实物台架。静止主动对齐、真实物理电压及轴角测量链仍待验证。本轮无板端上电、烧录、解锁、写 Flash 或实物转动，无提交或推送。 [CLI 说明](../../tools/falconfoc-cli/README.md#host-commutation-bootstrap) · [归档复现指南](2026-11-20-next-cal-boot-01/README.md)
