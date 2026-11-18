# NEXT-CODE-IMPROVE-04｜Runtime 生命周期拒绝路径 P1 修复

2026-11-17。裁决：**PASS WITH FOLLOW-UPS**。REVIEW-04 F1/P1 的两个旁路已通过正式 RED→GREEN、第二出口单独负向变异、完整回归及独立复核关闭。保留一项输出安全已满足的终态分类 P2 后续；Git 交付待用户确认。本结论限 Host 软件和 ARM 静态证据。

## 1. 基线与变更边界

工作树 `D:/study/Software/Hollow-Gimbal-vm01a`，分支 `codex/falconfoc-v2-vm01a`；开始及收口前核对本地/远端均为 `54f6e47d6a67d62684cfe84f704114c8ca7a8b6b`。已读取 AGENTS、正式纲领、REVIEW-04、直接 Runtime/Pipeline/Safety/控制器及相关正式测试。此前 REVIEW-04 的七个未提交文档/证据文件已纳入不可变快照，旧报告保留历史 BLOCK，不改写成历史 PASS。

本轮生产变更仅 `runtime/src/foc_runtime.c`（23行增、9行删）；正式测试为 `runtime/tests/test_foc_runtime_mailbox.c`、直接关联的 `runtime/tests/test_foc_motion_runtime.c` 和 `simulation/tests/test_foc_periodic_chain.c`。另增加本报告、原始证据 ZIP、核对回执，追加维护日志。不改 Core、Motion、Pipeline、Safety、默认参数、原子协议、UART、Board 或硬件权限。

## 2. 根因、最小反例与控制流

RUNNING 且已接受一次 UPDATE 后，输入同一 seq4/capture400/now401 的新鲜 ADC（valid_mask=0）。无请求时正常 Pipeline 观察把 invalid streak 加到1、confirmed INHIBIT、退出 RUNNING；原实现的 wrong-state ARM 在 Pipeline 前返回，消费时过期邮箱则完全不调用 control，两者都留下 RUNNING/旧 confirmed UPDATE/streak0。错误是屏蔽失效处置，没有产生额外 UPDATE。

原流：动作非法→直接返回；邮箱失效→直接 lifecycle_status。新流：

1. 过期/Session 不匹配邮箱的结果独立记录为 REJECTED；其动作不复制到 control input，当前普通 H0 仍执行一次 control。
2. control 在采样观察前保存动作资格，随后执行一次 guarded Pipeline；再拒绝非法生命周期动作。避免无效样本把 RUNNING 变 DISABLED 后，错误 ARM 被重新解释为合法。
3. 新鲜有效样本可以完成本来就应执行的一个 UPDATE。H0 返回值、Pipeline 状态和生命周期终态分别解释；请求拒绝不等于样本拒绝。有效禁能 SET_MODE 保留原有不执行控制的冷路径边界，非法 SET_MODE 与其他非法动作一样不能隐藏本拍观察。
4. STOP、故障与 schedule/order 的原快速安全路径保留。拒绝邮箱若普通处理观察到 STOP/FAULT，按已有取消优先级发布 CANCELLED；请求身份与完成时间不重新生成。

## 3. G1/G2：有效 RED、GREEN 与变异

最初一次 `red/` 在夹具启动时缺 Foreground 初始心跳，不是目标 RED；原日志保留但不计验收。修正夹具后，在任何生产修改前运行 `red-valid/`：编译0，CTest原生8，失败在 mailbox 正式新增 `last_request.action == INHIBIT` 断言（当时第286行）。外层脚本当时预期0因而退出1，不冒充测试运行器成功。随后 `red-final/` 用相同正式测试+不可变原生产源码重放：编译0、CTest8（预期8），再次得到同一目标失败。

```text
原实现 route0（无请求）: H0=-5 state=DISABLED action=INHIBIT updates=1 streak=1 session=1
原实现 route1（错误ARM）: H0=-5 state=RUNNING action=UPDATE updates=1 streak=0 session=0
```

首次最小修复后 `green/` mailbox 1/1、exit0。额外在独立 `mutant-expired-bypass/` 仅恢复 H0 的失效邮箱跳过 control 三元分支，保留动作资格修复：编译0、CTest8；route1 已正确 INHIBIT，但 route2（消费时过期）再次保留 UPDATE/streak0。证明测试也能独立检出第二个出口，非编译失败检出；工作树从未写入变异。

过期反例 produced=t-1500，发布=t-98（模年龄1402us≤1500），消费=t+1（1501us>1500）；在t400时跨 uint32 回绕。生产者准入成功不代表消费时仍有效。

## 4. 正式行为矩阵及恢复

mailbox 原有三个测试保留；新增 8路×4条件共32组：无请求、wrong ARM邮箱、消费过期邮箱、direct ARM、wrong SET_MODE邮箱、direct SET_MODE、direct CLEAR_FAULT、wrong START邮箱，分别组合有效采样、无效ADC、已发布invalid sensor、Foreground健康过期。

| 条件 | H0/生命周期 | Pipeline/有效输出 | UPDATE与状态 |
|---|---|---|---|
| 新鲜有效样本+非法动作 | direct或wrong-state为ERR_CONTROL；过期邮箱普通H0可OK；请求REJECTED | RUNNING/confirmed UPDATE | 恰好+1；模式/Session不变，无额外fault |
| 无效ADC+无请求/wrong ARM/过期邮箱等8路 | 错误；邮箱REJECTED | DISABLED/confirmed INHIBIT，streak1 | +0；PI d/q均不提交；Session因正常rearm边界+1 |
| 已发布invalid sensor+8路 | 错误；邮箱REJECTED | DISABLED/INHIBIT，streak1 | +0，无额外锁存fault |
| 健康过期+8路 | 错误；REJECTED或FAULT优先CANCELLED | FAULT_LATCHED/INHIBIT，现有健康fault | +0；不是请求拒绝创造fault |
| 入口STOP/FAULT+新鲜或过期ARM | ERR_STOP/CANCELLED | DISABLED或FAULT_LATCHED/INHIBIT | +0，不能重新ARM |
| 更新已经接受后才到达STOP | 已接受事务历史保留；见P2后续 | 最终DISABLED/confirmed INHIBIT | 不是STOP后又准入UPDATE |

结果检查真实 request_sequence/action/h0_us/applied_session；重复发布 ERR_ORDER 不覆盖已完成结果。恢复先拒绝旧命令，再新命令+ARM/START 后才恢复UPDATE。现有 motion_runtime 正式测试继续检查边界前旧角度不能重ARM、需要新源；旧Session发布拒绝测试保留。独立消费Session失配探针是明确的人工Session注入，用来隔离消费谓词，不声称完整重现并发交错。

完整回归暴露两项旧预期依赖被删除的旁路，作直接必要修订：

- motion_runtime 中 RUNNING 的 SET_MODE 仍必须错误返回且保持 CURRENT；与独立初始化的正常 H0 实例比较，严格要求总UPDATE=2、次数一致、PI d/q完全相同。后续STOP/重启/输出拒绝原子性断言保留。
- periodic_chain 的启动传输失败继续要求0个UPDATE和INHIBIT；拒绝ARM/START不能再隐藏坏观测，0/200/400us明确streak1/2/3，第三次按现有阈值锁存且仅锁存SAMPLE_INVALID，随后好反馈不自动重启。没有修改阈值或Safety算法。

H1拒绝候选保留旧新鲜有效源、不得刷新其时间戳的已接受合同不变；periodic_chain 正式回归仍验证旧源继续一次UPDATE及真正过期后INHIBIT。

## 5. G3：本轮真实执行结果

所有构建均在外部独立目录，不改受保护的已有build。Clang22.1.8 Debug，GCC16.2.0交叉验证Host；每条入口数量由新的 `ctest --show-only=json-v1` 读取，不靠预期数量。

| Lane | 实际CTest | 原生退出 |
|---|---:|---:|
| Core Debug Clang |15/15|0|
| Runtime Debug Clang |11/11|0|
| Simulation Debug Clang |15/15|0|
| Runtime ASan+UBSan Clang |11/11|0|
| Simulation ASan+UBSan Clang |15/15|0|
| Runtime Debug GCC |11/11|0|
| Legacy Host GCC documented runner |45/45|0|
| Source Contract |82 C/H、9 CMake、9 README|0|
| 修改C文件格式 |4/4|0|
| 严格Scope all |30 CMake格式+全部子门禁+Host45/45；[Total] PASS|0|

初次广回归 Runtime10/11、Simulation13/15 的旧预期失败日志同样保留；修订后的全新构建目录才是上述最终结果。ASan/UBSan未报告诊断。

Strict gate 在从54f6e47本地clone的隔离Git副本运行，叠加本轮完整C差异和受保护文档。使用短TEMP、Bash路径和预先由`-CompilerPath C:/mingw64/bin/gcc.exe`配置的Host缓存。实际命令：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/gates/check_themis_quality_gate.ps1 -RepoRoot <isolated-clone> -Scope all -RequireCMakeFormatter -Summary
```

Current Contract 使用从原build逐字节复制的6个历史Legacy ELF/MAP，SHA在证据内；不是本轮重建Legacy或V2固件。不能把Gate或ARM单对象编译说成完整ARM镜像重建。命令完整argv/cwd/UTC/native exit/log SHA保存在ZIP的commands.jsonl。

## 6. G4：独立复核与未决

独立审查者 `/root/improve04_review` 只读核对源码、测试、有效RED并运行私有探针；确认两处旁路、单次Pipeline、动作预判、错误后无重解释、PI事务、身份和STOP安全。随后另行复核两处旧断言修订，确认是契约纠正、非放宽。未发现新P0/P1，生产补丁SHA与复核时一致。完整摘要和probe源/输出随ZIP归档。

**P2后续：已准入UPDATE之后的非锁存STOP与请求终态分类。** AFTER_CONTROL注入STOP，wrong ARM结果REJECTED/H0=-5，过期ARM结果REJECTED/H0=0；两者最终均DISABLED、confirmed INHIBIT、Session增长，没有旧有效UPDATE残留。成功Pipeline先返回、提交后STOP不抹去历史事务是既有边界；本轮新进入的拒绝组合使终态分类更值得明确。后续用最小正式交错测试收口REJECTED/CANCELLED含义；不把该输出安全已满足的分类问题夸大为新P1，也不声称已修复。

单核 H0>H1>Foreground、域不重入和32位lock-free前提不变；尚无任意并行Host线程证明。目标WCET/中断时序/实际关断/栈水位仍需硬件证据。

## 7. RAM、Flash及路径成本

本轮分别重新编译原与新Runtime翻译单元，Arm GNU14.3、C11、cortex-m4/thumb、soft-float、-Os、-fstack-usage：对象text6484→6512B（+28B），data/bss均0；ARM Runtime实例1236B、Host64实例1296B，公共布局未改。H0静态帧168B、control224B、H1 328B前后相同。

这不是最终链接Flash差值、完整调用链栈上界或WCET。没有增加动态分配、锁、原子操作或数据复制；普通每拍仍只有一个guarded Pipeline。被拒绝动作/邮箱现在需要执行原本应有的采样/Safety/正常控制成本；有效SET_MODE冷路径保留。未声称性能加速，也没有将Host耗时换算成AT32F415中断时间。

## 8. 保护、Git与下一步

新增代码/测试SHA、原七文件保护、维护日志只追加、2546份原始源快照、2427个既有忽略产物（322054220B）和主工作树均在verification.json核对。原REVIEW报告/zip/receipt/纲领/入口保持字节不变；维护日志保留原完整字节前缀。未commit/push/reset/clean/rebase，未改其他分支。所有未提交路径与历史/本轮归属列入回执；Git须用户明确批准。

原F1/P1可正式关闭：测试先失败，修复后通过，两出口均能被回归检出，独立复核成立。实际质量收益是把生命周期拒绝与本拍采样/Safety分离，避免错误请求屏蔽输出失效处理，同时保留正常周期与错误原子性。可以在下一次授权后恢复普通逐文件精修；优先完成本报告P2终态分类的小测试/契约收口，之后评审 `foc_pipeline.c` 的事务边界。H1窄快照/遥测优化仍只有候选证据，不直接实施。本轮到此停止。

```text
G3-SAFE HARDWARE NOT VERIFIED
G3-PWR LOCKED
AT32F415 WCET NOT VERIFIED
MOTOR PHYSICAL OPERATION UNVERIFIED
```
