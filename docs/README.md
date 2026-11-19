# FalconFoc Docs

`docs/` is the formal documentation root for the current FalconFoc AT32
repository.

Current handoff entry: [code-wiki/AI-handoff-2026-11-15.md](./code-wiki/AI-handoff-2026-11-15.md). It is the current software status entry; dated debugging and review documents remain historical evidence.

Current V2 release-preparation entry: [evidence/2026-11-16-next-release-prep-01.md](./evidence/2026-11-16-next-release-prep-01.md). It records the independent Core consumer, source inventory, dependency boundary, license review, test matrix, and release blockers.

The [Servo Quickstart](./servo/quickstart.md) covers the standalone Host Servo example and external consumer builds.

Read in this order:

1. [code-wiki/README.md](./code-wiki/README.md) — 当前 AT32F415 源码地图、Board1 硬件理解与无板验证
2. [code-wiki/hardware-and-drv8311h.md](./code-wiki/hardware-and-drv8311h.md) — Board1 原理图、DRV8311H 与 AT32F415 硬件对应
- [code-wiki/project-summary.md](./code-wiki/project-summary.md) — 项目整体架构、模块职责、运行方式和当前状态总结
- [code-wiki/debugging-summary.md](./code-wiki/debugging-summary.md) — 低压开环问题的调试证据、推导和根因状态
- [code-wiki/open-loop-root-cause-deep-analysis.md](./code-wiki/open-loop-root-cause-deep-analysis.md) — 不新增测试条件下，对既有实板证据进行因果分层、反证和根因排序
- [code-wiki/ai-handoff-prompt.md](./code-wiki/ai-handoff-prompt.md) — 下一位 AI 的完整接手提示词
- [code-wiki/session-memory-2026-08-27-28.md](./code-wiki/session-memory-2026-08-27-28.md) — 昨晚关键调试记忆、决策、验证证据和后续交接入口
- [code-wiki/configuration-audit.md](./code-wiki/configuration-audit.md) — 原理图—外设—调用时序配置审计
- [code-wiki/motor-debugging-safety-and-procedure.md](./code-wiki/motor-debugging-safety-and-procedure.md) — 实机调试安全协议、快速停机与开环排障流程
- [code-wiki/motor-real-bench-test-plan.md](./code-wiki/motor-real-bench-test-plan.md) — 带门禁的实机测试步骤与低能量开环排障方案
3. [architecture/README.md](./architecture/README.md)
4. [boot/Themis_启动与初始化流程.md](./boot/Themis_%E5%90%AF%E5%8A%A8%E4%B8%8E%E5%88%9D%E5%A7%8B%E5%8C%96%E6%B5%81%E7%A8%8B.md)
5. [boot/Themis_启动链回归检查表.md](./boot/Themis_%E5%90%AF%E5%8A%A8%E9%93%BE%E5%9B%9E%E5%BD%92%E6%A3%80%E6%9F%A5%E8%A1%A8.md)
6. [application/README.md](./application/README.md)
7. [governance/Themis_Transport与Shell硬化说明.md](./governance/Themis_Transport%E4%B8%8EShell%E7%A1%AC%E5%8C%96%E8%AF%B4%E6%98%8E.md)
8. [governance/Themis_Shell与诊断输出规范.md](./governance/Themis_Shell%E4%B8%8E%E8%AF%8A%E6%96%AD%E8%BE%93%E5%87%BA%E8%A7%84%E8%8C%83.md)
9. [governance/Themis_工程维护日志.md](./governance/Themis_%E5%B7%A5%E7%A8%8B%E7%BB%B4%E6%8A%A4%E6%97%A5%E5%BF%97.md)

Current live scope:

- FalconFoc no-`dev/drv` architecture
- `platform/at/at32f415` ownership in the current source tree
- current-source Code Wiki and software-only validation
- platform / SoC migration guidance
- FalconFoc CMake build constitution and new-product landing workflow
- small-MCU BSP light-wrapper policy and writing contract
- `bsp/board + bsp/ports` structure
- live BSP module naming is `bsp_xxx`, not `*_port`
- startup chain through `applications/app_main.c`
- shell transport and session split
- current build, test, and board-debug entry points

Current non-live scope:

- retired product semantics
- superseded runtime guidance
- historical planning and review material
