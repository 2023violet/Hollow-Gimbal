# FalconFoc Architecture Docs

`docs/architecture/` records the active platform, build, startup, and boundary
rules for the current FalconFoc Themis-lite repository.

Read these first:

1. [Themis_FalconFoc无devdrv模式与FOC直控分层方案.md](./Themis_FalconFoc%E6%97%A0devdrv%E6%A8%A1%E5%BC%8F%E4%B8%8EFOC%E7%9B%B4%E6%8E%A7%E5%88%86%E5%B1%82%E6%96%B9%E6%A1%88.md)
2. [Themis_CMake构建架构说明.md](./Themis_CMake%E6%9E%84%E5%BB%BA%E6%9E%B6%E6%9E%84%E8%AF%B4%E6%98%8E.md)
3. [Themis_BSP层定位、边界与优化路线图.md](./Themis_BSP%E5%B1%82%E5%AE%9A%E4%BD%8D%E3%80%81%E8%BE%B9%E7%95%8C%E4%B8%8E%E4%BC%98%E5%8C%96%E8%B7%AF%E7%BA%BF%E5%9B%BE.md)
4. [Themis_Export机制与阶段设计.md](./Themis_Export%E6%9C%BA%E5%88%B6%E4%B8%8E%E9%98%B6%E6%AE%B5%E8%AE%BE%E8%AE%A1.md)
5. [Themis_参数持久化存储设计说明.md](./Themis_%E5%8F%82%E6%95%B0%E6%8C%81%E4%B9%85%E5%8C%96%E5%AD%98%E5%82%A8%E8%AE%BE%E8%AE%A1%E8%AF%B4%E6%98%8E.md)
6. [Themis_FalconFoc_128K32K内存瘦身与Shell两态裁剪路线图.md](./Themis_FalconFoc_128K32K%E5%86%85%E5%AD%98%E7%98%A6%E8%BA%AB%E4%B8%8EShell%E4%B8%A4%E6%80%81%E8%A3%81%E5%89%AA%E8%B7%AF%E7%BA%BF%E5%9B%BE.md)

Current architecture facts:

- live platform owner: `platform/at/at32f403a`
- live board owner: `bsp/board`
- live thin control-plane port owner: `bsp/ports`
- product runtime does not assemble Themis `device/` or `drivers/`
- FOC hot path stays direct-to-library or direct-to-register when needed
- shell is split into transport owner `shell_task` and session owner
  `app_shell_task`

Use this directory for:

- layering and ownership decisions
- build-system contracts
- BSP and startup boundary rules
- export and startup staging rules
- parameter save/recover/version strategy

Do not use this directory as a live owner for:

- retired product semantics
- deprecated implementation guidance
- historical benchmark or review narratives
