# FalconFoc Docs

`docs/` is the formal documentation root for the current FalconFoc AT32
repository.

Read in this order:

1. [architecture/README.md](./architecture/README.md)
2. [boot/Themis_启动与初始化流程.md](./boot/Themis_%E5%90%AF%E5%8A%A8%E4%B8%8E%E5%88%9D%E5%A7%8B%E5%8C%96%E6%B5%81%E7%A8%8B.md)
3. [boot/Themis_启动链回归检查表.md](./boot/Themis_%E5%90%AF%E5%8A%A8%E9%93%BE%E5%9B%9E%E5%BD%92%E6%A3%80%E6%9F%A5%E8%A1%A8.md)
4. [application/README.md](./application/README.md)
5. [governance/Themis_Transport与Shell硬化说明.md](./governance/Themis_Transport%E4%B8%8EShell%E7%A1%AC%E5%8C%96%E8%AF%B4%E6%98%8E.md)
6. [governance/Themis_Shell与诊断输出规范.md](./governance/Themis_Shell%E4%B8%8E%E8%AF%8A%E6%96%AD%E8%BE%93%E5%87%BA%E8%A7%84%E8%8C%83.md)
7. [governance/Themis_工程维护日志.md](./governance/Themis_%E5%B7%A5%E7%A8%8B%E7%BB%B4%E6%8A%A4%E6%97%A5%E5%BF%97.md)

Current live scope:

- FalconFoc no-`dev/drv` architecture
- `platform/at/at32f403a` ownership
- `bsp/board + bsp/ports` structure
- startup chain through `applications/app_main.c`
- shell transport and session split
- current build, test, and board-debug entry points

Current non-live scope:

- retired product semantics
- superseded runtime guidance
- historical planning and review material
