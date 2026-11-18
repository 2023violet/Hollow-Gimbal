# NEXT-CODE-REVIEW-04｜Runtime 逐文件深审

2026-11-17。总体裁决：**BLOCK：一项已复现P1生命周期拒绝/Safety旁路**。其余设计保留；性能候选待证，不以原子数量或函数长度作慢速结论。本轮没有修改生产代码、正式测试和配置。

## 1. 基线与纲领

工作树 `D:/study/Software/Hollow-Gimbal-vm01a`，分支 `codex/falconfoc-v2-vm01a`，起始本地/远端均为 `54f6e47d6a67d62684cfe84f704114c8ca7a8b6b`，工作树干净。读取AGENTS、Runtime合同、直接调用链与最新System Angle证据。遵循纯C11、无强制RTOS、Core可独立集成、参考Runtime不强加给Core用户的产品宗旨。

用户随后提供Downloads中的正式纲领并授权归档。已逐字节移动到 `docs/architecture/FalconFOC_Project_Charter_and_Engineering_Memory_v1.0.md`，SHA256 `648c1c6bd0f35b540fbf57a99c8ed92573d8ddc648d71caa314bd3659f646a98`，保留外部原件副本；AGENTS/Architecture README增加入口。原文f00e9a4及System Angle Pending是历史快照，未重写原件；当前54f6e47/System Angle Host PASS优先。

## 2. 路径地图和已保留合同

```text
Foreground command -> publish_reference -> inactive command slot -> release index
H1 -> schedule -> source time/sequence -> phase/speed quality -> snapshot
   -> outer PI/Trajectory trial -> inactive sensor slot -> release index
H0 -> schedule -> odd telemetry generation -> drain STOP -> claim/revalidate request
   -> control: ADC order/interval -> acquire sensor -> ADC-time prediction
   -> import heartbeat/command -> action admission -> Pipeline guarded step
   -> decode/Safety -> Current trial -> map/Safety/final STOP guard -> output submit
   -> accepted PI commit -> drain STOP -> Session boundary -> snapshot/even generation
Foreground -> supervision(snapshot private Safety) -> heartbeat -> optional telemetry
```

KEEP：H0独占Pipeline、Safety、内环Current PI与真实输出事务。H1拥有外环提案/状态，依据H0 accepted_outer_seq确认，而非擅自提交输出。STOP在入口、控制前后与最终提交门被处理；已准入UPDATE后到达STOP不能抹掉历史事务，输出视图随后失效。

KEEP：单核H0>H1>Foreground、域不重入、32位lock-free atomic的双槽/邮箱设计；不适用于并行Host线程。WRITING/PUBLISHED/CLAIMED及结果双缓冲在该合同下不构成需“修”的多线程数据竞争。snapshot使用原子payload和奇偶代际，pending STOP在读窗内；clear_result明确独立于该代际。

KEEP：机械/电单位和独立周期相位；H0以ADC sample_tick而非执行now计算年龄。H1拒绝未来/重复/过期候选保留旧新鲜源；原源到期必须INHIBIT。此次ADC旁路与该已批准合同不同，不能混淆。恢复保留水印，需要新命令/观测及ARM/START，clear不直接RUNNING。

## 3. F1 — P1 / BLOCK：拒绝生命周期请求跳过本拍失效处理

位置：`runtime/src/foc_runtime.c:582` 的action_valid早退，及 `:1188` 的失效邮箱直接返回lifecycle_status。拒绝动作本身正确，但这两条出口分别在Pipeline前或整个control前跳过当前样本的观察/Safety。

最小配对反例复用正式test_foc_runtime.c启动fixture，301us已接受一次UPDATE；400us发布合法新角度，给seq4/capture400/now401的ADC，valid_mask=0：

| 条件 | H0返回 | 状态 | 有效输出 | invalid streak | updates |
|---|---:|---|---|---:|---:|
| 无生命周期请求 | -5 | DISABLED | confirmed INHIBIT | 1 | 1 |
| 302us发布新鲜ARM，当前已RUNNING | -5 | RUNNING | confirmed UPDATE | 0 | 1 |
| 302us接受ARM，401us消费时已过期 | -5 | RUNNING | confirmed UPDATE | 0 | 1 |

过期例produced_us=UINT32_MAX-1099：发布时模年龄1402us合法，消费时1501us超过fixture1500us超时。三个分支相同ADC、角度、命令/初态。last_sample仍300；**没有新增UPDATE**，错误是旧的输出仍生效而即时invalid处理被压掉。不是任意运行错误都需清除所有历史状态，也不是H1旧有效源策略问题。

独立审查者发现后，主审在本轮不可变快照重新编译/运行确认同样结果。探针自身exit0表示三条观察成功；行为不满足预期安全合同，不能把它记作软件验收PASS。源码/命令/退出码及原始输出在附带证据中。

阻断范围：RUNNING中生命周期拒绝与本拍失效叠加的正确性验收；不得以当前Host回归绿色宣称这一流程完整。不改硬件权限，不触发任何真实电机动作。

下一轮最小修改：仅Runtime两个拒绝出口及直接Runtime/mailbox测试。先将此反例变成正式RED，要求wrong-state/expired/old-session与invalid ADC、失效角度、健康过期的组合仍执行失效处置；再选择“拒绝请求但继续NONE动作的正常H0观察”或“拒绝时显式INHIBIT”的统一语义。优先维持新鲜有效样本的正常服务，错误动作不进入生命周期执行；STOP/FAULT优先和会话边界不变。验证无请求、有效请求、取消/拒绝身份、输出拒绝后PI、故障恢复及回绕；Runtime/Simulation和ARM资源回归。回退到54f6e47的该最小补丁；回退只恢复已知缺陷基线，不代表问题关闭。

## 4. 资源与性能证据

工具：Clang22.1.8 Host Debug；Arm GNU14.3，C11，cortex-m4/thumb、soft-float、-Os、-fstack-usage，独立编译未改的Runtime翻译单元。CPU/float设置与当前参考固件CMake一致；没有新链接完整ARM固件，不是板上测量。

| 类型sizeof | Host64 B | ARM ABI B |
|---|---:|---:|
| Runtime |1296|1236|
| Pipeline |384|372|
| Runtime config |144|132|
| snapshot |112|108|
| sensor slot |64|64|
| command slot |20|20|
| lifecycle request/result |20/36|16/28|

ARM Runtime中sensor双槽128B、命令双槽40B、生命周期请求/结果32/56B、遥测payload92B。尺寸非内存泄漏；Mock/journal属于参考集成成本，Core不强制依赖它。runtime.o text6484B/data0/bss0是单对象，实例由调用者分配，依赖库/GC/最终镜像另计，不能当固件总Flash/RAM。

静态每函数栈：init600B、H1 328B、H0 168B、control224B、copy_snapshot184B、publish_snapshot104B、Foreground240B。H1调用copy时仅这两帧已涉及512B；不是完整调用链上界，更未含异常现场、抢占、库函数、MSP/PSP高水位。冷init的大局部对象不应当成每拍开销。

热点事实：

- 每次完成H0服务发布23个payload原子字及status。ARM实际23次循环各前后DMB，status前后另2个，发布结尾sequence的LDREX/STREX前后再2个：发布函数该路径50次DMB执行，尚未计H0入口等；不能据此宣称浪费多少us。正常/STOP/FAULT完成服务共享该发布成本；未到期无STOP的IDLE早退不发布。
- H1完整snapshot读取28次原子load（含两次代际、status、pending、独立clear），但prepare_outer只用flags/state/mode/session/outer_seq及pending。专用同代际私有视图可研究28→8次load；需要证明含STOP一致性，不能直接去掉屏障。当前H1周期500us时为2000次/s的潜在简化对象；H0 200us对应5000次/s是配置频率，非实测能力。
- schedule跳过旧deadline为常数工作量，一次整数除法，M4对象有硬件指令支持；没有积压while循环。H0将ADC interval转换一次秒，预测年龄另转换一次，两者时间域不同，不应直接合并。
- Runtime对象依赖__aeabi_ui2f/fmul/fcmp、fminf/fmaxf；sinf/cosf/fmodf等在下游Core/Predictor，不能把所有libm成本都算在此文件。参数校验的init冷路径与动态finite/age/Safety检查必须分开；本轮无证据删动态检查。

最多三项优先事项：F1立即IMPROVE并阻断验收；H1窄快照与遥测发布成本分别NEEDS EVIDENCE。后两者先做功能等价、目标周期/栈/代码差异对照，达标才准入IMPROVE；无收益保留原实现。此次未实施候选或声称已获得速度收益。头文件lifecycle_request_pending的0/1/2注释落后于实现0/1/2/3，可随F1修复澄清，不单开重构专项。

## 5. 可移植性与CyberBeast有限对照

Runtime无vendor/RTOS/HAL调用，但具有明确单核优先级、32位lock-free和Host Mock组成假设；这些属于参考Runtime接缝，不是Core跨MCU承诺。不要加厚HAL、移植全局设备框架或把uint32 lock-free假设推广到任意MCU。

参考固定为[robot_mc 2f1fb61](https://github.com/cyberbeast0829/robot_mc/tree/2f1fb61ec081eb7a42ab8436ba8f1504fc529da7)，本轮clone读回一致：

- [n32h47x_48x_it.c](https://github.com/cyberbeast0829/robot_mc/blob/2f1fb61ec081eb7a42ab8436ba8f1504fc529da7/Core/Src/n32h47x_48x_it.c#L231) 先启动encoder DMA，再读ADC/查fault，之后收encoder/run_fsm，CAN每拍最多两帧；具名ISR分段计时而非只报频率。
- [motion.c](https://github.com/cyberbeast0829/robot_mc/blob/2f1fb61ec081eb7a42ab8436ba8f1504fc529da7/Core/Src/motion.c#L1) 外环按控制ISR计数分频，内环保持同ISR。借鉴显式频率/采样相位和有界冷工作，而非强制FalconFOC变成40kHz。
- [main.c](https://github.com/cyberbeast0829/robot_mc/blob/2f1fb61ec081eb7a42ab8436ba8f1504fc529da7/Core/Src/main.c#L583) 协议慢服务/slow FSM放主循环；[fsm.c](https://github.com/cyberbeast0829/robot_mc/blob/2f1fb61ec081eb7a42ab8436ba8f1504fc529da7/Core/Src/fsm.c#L72) 状态exit/enter再控制。可学习热点分段与安全优先组织；它的N32寄存器、全局controller/state、FPU/ADC/SPI时序及40kHz声明不构成FalconFOC指标。

仅做源码对照，不复制外部代码；若未来采纳实现，按纲领逐文件审许可/归属。

## 6. 本轮验证、未决与停止点

本轮新建隔离目录运行Runtime11/11、Simulation15/15（含最新System Angle）均exit0，ARM编译/资源探针exit0；新增私有反例揭示正式测试未覆盖的F1。没有把测试绿等同于全路径正确。未执行新Scope all或板上测量，生产和正式测试未改；此前Gate PASS仍是此前证据，不重复记为本轮。

NEEDS EVIDENCE：生命周期sequence/time回绕及发布后H0抢占使session失效的专门组合；H1在snapshot后被H0抢占的外环PI/Trajectory确认账目；目标栈水位/WCET/中断抖动与故障到物理关断。先修F1，不衍生泛化数学专项。KEEP不代表这些证据已齐。

审查附带一次性外部探针和原始日志，不改正式测试。受影响功能维持BLOCK，本轮不自动实施修复，不提交推送，不清理旧成果。档案移动按用户单独指令执行，与生产审查只读边界区分。

```text
G3-SAFE HARDWARE NOT VERIFIED
G3-PWR LOCKED
AT32F415 WCET NOT VERIFIED
MOTOR PHYSICAL OPERATION UNVERIFIED
```

## 附：逐函数地图

| 函数（省略foc_runtime_） | 定义行 | 执行域/结论 |
|---|---:|---|
| float_word | 75 | 浮点位编码 |
| word_float | 83 | 浮点位解码 |
| lane_valid | 90 | 冷路径配置准入 |
| schedule | 97 | 所有域定时/跳拍/预算 |
| hook | 127 | 受控抢占测试缝 |
| publish_snapshot | 135 | H0每次有效服务的23字原子发布 |
| copy_snapshot | 176 | H1/Foreground有界读与STOP遮罩 |
| drain_stop | 239 | H0排空合并STOP |
| import_foreground | 253 | H0消费已完成心跳 |
| import_command | 271 | H0命令水印/派发 |
| outer_mode | 289 | 模式分类 |
| set_mode | 294 | 冷路径禁能下切模式 |
| align_angle | 315 | H0源新鲜度/ADC对齐 |
| admit_update | 342 | 提交前STOP最终门 |
| timeout_clear_ready | 351 | 冷路径timeout恢复准入 |
| action_valid | 386 | 状态/动作组合准入，关联F1 |
| lifecycle | 394 | Pipeline生命周期派发 |
| lifecycle_shape_valid | 409 | 生产者请求形状 |
| lifecycle_write_result | 422 | H0发布终态结果 |
| lifecycle_claim | 447 | H0原子领取 |
| lifecycle_applied | 468 | 检查动作后置条件 |
| lifecycle_request_current | 486 | H0复核Session/年龄 |
| lifecycle_finish | 501 | 先结果后IDLE |
| control | 517 | H0主控制事务，关联F1 |
| publish_foreground | 638 | 完成监督后发布时间 |
| supervise | 644 | Foreground私有Safety视图 |
| prepare_outer | 697 | H1外环提案/确认提交 |
| publish_reference | 806 | Foreground双槽命令发布 |
| init | 837 | 冷路径局部验证/实例初始化 |
| publish_command | 920 | CURRENT准入 |
| publish_motion_command | 939 | 外环命令准入 |
| publish_lifecycle_request | 953 | Foreground请求发布 |
| read_lifecycle_result | 1016 | Foreground按真实身份读取 |
| h1_step | 1056 | H1观测准入/源发布 |
| h0_step | 1108 | H0 STOP/生命周期/事务/Session |
| foreground_poll | 1224 | 监督优先/可丢遥测 |
| request_inhibit | 1275 | 独立原子OR停机请求 |
| revoke_command | 1282 | 先STOP后撤销命令 |
| request_timeout_clear | 1291 | 受限恢复邮箱 |
| read_snapshot | 1309 | 公共有界快照入口 |
