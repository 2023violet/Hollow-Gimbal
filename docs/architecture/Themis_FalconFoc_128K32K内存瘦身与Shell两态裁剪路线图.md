# FalconFoc 128K/32K 内存瘦身与 Shell 两态裁剪路线图

## 1. 目标与范围

本文固定 FalconFoc 面向 `128K ROM + 32K RAM` 量产约束下的内存裁决口径和落地路线。

本轮约束不变：

- 不修改 FOC 核心算法精度路径
- 不删除 `docs/governance` 既有规范体系
- 不调整 FreeRTOS `ucHeap`，仍固定 `10 * 1024 B`
- 优先砍预分配和冗余，不砍核心功能
- 优先编译期裁剪，不依赖运行态动态分配

## 2. 已冻结的技术裁决

### 2.1 AT32F415 低 RAM 主因

`AT32F415KBU7_4` 的 RAM 低于 `AT32F403A/AT32F423`，主因不是 FOC 算法，也不是 `platform/at` 驱动“大幅更省”，而是 linker reserve 差异：

- `F403/F423` 当前 linker reserve: `0x1000 + 0x1000 = 8192 B`
- `F415` 当前 linker reserve: `0x200 + 0x400 = 1536 B`
- 单项可解释差异: `6656 B`

因此：

- `6656 B` 差异应直接归因于 `._user_heap_stack`
- 剩余约 `2.0 ~ 2.2 KB` 才属于真实静态 RAM 差异
- 剩余差异应继续按控制面配置漂移和产物复建复核，不归因于 FOC 核心、vendor driver、缺失外设，或 FPU save area 本身

### 2.2 四个排查维度的裁决口径

- `platform/at` 驱动全局对象不是主因，预估影响 `< 0.5 KB`
- 编译优化级别不是主因，linker reserve 才是主因
- F415 缺少某些外设没有形成 `KB` 级静态 Buffer 收益，预估 `< 0.3 KB`
- `M4` vs `M4F` 的静态 RAM 影响不直接来自 FPU save area，而是体现在默认 stack / link policy 上

### 2.3 Shell、mempool、spsc_queue 的裁决

- `mempool + spsc_queue` 仅从 Shell TX 路径退出，不做全仓退役
- Shell 只保留“编译期开 / 编译期关”两态
- 不再保留 `product / factory / diag` 三套 Shell 运行形态
- `TM_SHELL_ENABLE=0` 时，Shell transport、session、命令表、banner、TX staging 均不进入最终镜像

## 3. 证据口径

### 3.1 正式基线

当前发布真值以用户提供的正式基线为准：

- `AT32F403ACGT7 (1M/96K)`: `FLASH 77588 B`, `RAM 29760 B`
- `AT32F423CCU7 (256K/48K)`: `FLASH 77036 B`, `RAM 29712 B`
- `AT32F415KBU7_4 (128K/32K)`: `FLASH 73108 B`, `RAM 20992 B`

Shell 税以用户正式基线为准：

- `F403`: `FLASH +7632 B`, `RAM +6168 B`
- `F423`: `FLASH +7704 B`, `RAM +6168 B`
- `F415`: `FLASH +7960 B`, `RAM +6176 B`

### 3.2 本地产物注意事项

当前仓库源码与部分本地 ELF / `.map` 曾出现 Shell 路径漂移迹象：

- 源码已切换为 `ring_buffer + 单 queue` 路径
- 某些历史本地产物符号视图仍可能停留在旧 `mempool + spsc_queue` 时代

因此本地证据只作为支持性证据。正式裁决和数字冻结仍以用户正式基线为准，本地产物必须重建复核后才能作为归档证据。

### 3.3 文档发布前必须归档的 6 组证据

必须重建并归档下列 6 组数据：

- `F403 shell off`
- `F403 shell on`
- `F423 shell off`
- `F423 shell on`
- `F415 shell off`
- `F415 shell on`

每组必须至少保留：

- `.map`
- `memory_report.txt`
- `arm-none-eabi-size -A` 输出

正式附录必须单列：

- `data + bss`
- `.bss`
- `._user_heap_stack`
- Top BSS Symbols

## 4. 已落地的代码收口

本仓库当前已按该路线完成第一轮结构性收口：

- 新增显式 feature 宏：
  - `TM_FEATURE_SHELL`
  - `TM_FEATURE_VOFA`
  - `TM_FEATURE_IPC`
  - `TM_FEATURE_TASK_BUDGET`
  - `TM_FEATURE_VERBOSE_LOG`
- Shell profile 改为纯两态：
  - `TM_SHELL_ENABLE=1`
  - `TM_SHELL_ENABLE=0`
- `app_shell_task` 去掉 backlog，只保留单静态 queue
- `TM_SHELL_TASK_QUEUE_DEPTH` 固定默认值 `3`
- `shell_task.c` 的 TX staging 改为 `1024 B ring_buffer`
- `mempool/spsc_queue` 不再承担 Shell TX 路径
- `vofa_task` 与 `subsys/shell` / `subsys/ipc` / `tm_task_budget_probe` 进入显式 feature gate
- linker reserve 改为通过 CMake 生成脚本注入：
  - `shell off`: `_Min_Heap_Size = 0`, `_Min_Stack_Size = 0x400`
  - `shell on`: `_Min_Heap_Size = 0x200`, `_Min_Stack_Size = 0x400`

## 5. 优化路线图

### 5.1 P0 必做

1. 统一 linker reserve 策略

- 目标：所有产品镜像停止沿用模板默认 `0x1000 + 0x1000`
- 现口径：
  - `shell off`: heap `0`, stack `0x400`
  - `shell on`: heap `0x200`, stack `0x400`
- 预估收益：
  - `F403/F423`: `RAM -6.5 KB`
  - `F415`: 主要是口径统一，额外收益接近 `0`
- 叠加关系：不可与“同一镜像下 reserve 已收紧”的收益重复相加

2. Shell 纯编译期开关

- 目标：Shell 关闭时 RAM 税和 FLASH 税均为 `0`
- 范围：
  - `shell_task`
  - `app_shell_task`
  - `applications/shell/*`
  - `subsys/shell/tm_shell.c`
  - Shell 命令导出段
- 预估收益：
  - 相对 Shell 开启镜像：`FLASH -7.6 ~ 8.0 KB`, `RAM -6.1 ~ 6.2 KB`
- 叠加关系：与“TX ring_buffer”“单队列”不可简单线性相加，因为二者本身属于 Shell 税内部收缩

3. Shell TX 路径切换到 `ring_buffer`

- 目标：用单 FIFO 替代 `4096 B mempool + 128 B ptr queue`
- 当前策略：
  - `1024 B ring_buffer`
  - 溢出时丢弃新输出
  - 仅累加诊断计数
- 预估收益：
  - `RAM -3.0 ~ 3.2 KB`
  - `FLASH 0 ~ +0.5 KB`
- 叠加关系：可与“Shell 开启但保留诊断镜像”场景叠加；与“Shell 全关”不可重复相加

4. `app_shell_task` 去双重排队

- 目标：删除 `_tm_shell_task_backlog`
- 保留：
  - queue depth `3`
  - `RX DMA 256 B`
  - `line buffer 128 B`
- 预估收益：
  - `RAM -0.9 ~ 1.0 KB`
  - `FLASH` 基本持平
- 叠加关系：可与“TX ring_buffer”叠加

5. 显式 feature 宏解耦控制面模块

- 目标：不再用 `TM_DEVDRV_PROFILE_*` 隐式控制 Shell / VOFA / IPC / 日志
- `128K/32K` 默认建议：
  - `SHELL=0`
  - `VOFA=0`
  - `TASK_BUDGET=0`
  - `VERBOSE_LOG=0`
  - `IPC=0`，除非 `CANopen/RS485` 最终证明必须依赖 `tm_ipc`
- 预估收益：
  - `FLASH -4 ~ 7 KB`
  - `RAM -2.5 KB`
  - 运行时 heap 压力再降 `2 ~ 3 KB`
- 叠加关系：与 LTO、轻量 libc 有重叠，禁止简单线性相加

### 5.2 P1 建议

1. Shell `strtof` / 重量级浮点格式化替换

- 目标：诊断输入输出不再拉入 `strtod` / `malloc` / locale / stdio 大链路
- 范围：只影响 shell 诊断输入输出，不碰 FOC 计算精度
- 预估收益：
  - `FLASH -5 ~ 8 KB`
  - `RAM -0.7 KB`
- 附带收益：允许 `shell on` 工程镜像进一步把 linker heap reserve 降到 `0`

2. 产品构建固定走 `Release` / `MinSizeRel`

- 必开项：
  - `-ffunction-sections`
  - `-fdata-sections`
  - `--gc-sections`
  - `LTO`
- 预估收益：
  - `FLASH -4 ~ 8 KB`
  - `RAM 0 ~ -0.5 KB`
- 口径：所有正式数字必须以产品构建而非 Debug 构建为准

### 5.3 P2 远期

1. `STM32G431` 适配前置约束

- FOC 热路径继续直连寄存器 / LL
- 控制面只允许薄 `bsp/ports`
- 禁止 CubeMX 默认 `defaultTask`
- 禁止默认 CMSIS queue / 大模板 `freertos.c`
- 所有 HAL handle、DMA staging buffer、CMSIS queue 必须进入 RAM 预算表并接受 Top-BSS 审计

## 6. 128K/32K 目标预算

量产验收使用 `shell off` 口径，内部预算固定为：

- `FLASH <= 120 KB`
- `RAM <= 30 KB`

这样对外仍满足：

- `FLASH <= 128 KB`
- `RAM <= 32 KB`

并保留量产安全余量。

## 7. STM32G431 适配预警

### 7.1 HAL + LL 的典型 RAM 通胀点

- 多组 `HAL_*HandleTypeDef`
- 每外设一套 DMA staging buffer
- CubeMX 默认 RTOS 对象
- CMSIS queue / semaphore / timer 控制块
- `freertos.c` 默认任务模板和默认栈

### 7.2 预防性架构建议

- FOC ISR 热路径不经 HAL
- 控制面访问只经薄 `bsp/ports`
- 所有 RTOS 对象必须由产品 owner 显式声明，而不是交给 CubeMX 默认模板
- 每个适配仓库都必须保留 Top 30 BSS 审计表
- 一旦出现 `HAL handle + DMA buffer + CMSIS queue > 1 KB`，优先砍控制面预分配，不动 FOC 核心

## 8. 验收标准

### 8.1 根因验收

把 `._user_heap_stack` 单列后：

- `F415` 与 `F403/F423` 的真实静态 RAM 差异必须收敛到约 `2.2 KB` 内
- 若仍超出该范围，继续追查控制面配置漂移
- 不允许回退为“算法或 vendor driver 是主因”的表述

### 8.2 Shell 两态验收

`shell off` 镜像中不得出现以下符号：

- `shell_task`
- `app_shell_task`
- `tm_shell`
- `__tx_mempool_buf`
- `__tx_queue_buf`
- `_tm_shell_task_backlog`

`shell on` 镜像的新增 RAM 税目标：

- `<= 2.5 KB`

### 8.3 STM32G431 适配验收

Top 30 BSS 符号中，不得出现：

- 多组 `HAL_*HandleTypeDef`
- 多组 DMA staging buffer
- CubeMX 默认 RTOS 对象

累计超过 `1 KB` 的失控预分配。

## 9. 需要用户补充的证据

- `AT32F415`、`AT32F403`、`AT32F423` 各自 Shell 开 / 关状态的 `.map`
- `./run.sh memory` 输出的模块级明细
- `arm-none-eabi-size -A` 的完整输出
- Top BSS symbols 清单

## 10. 2026-05-19 Live Update

- FalconFoc 产品 feature owner 已收回到 `include/config/tm_core_config.h`
  - `TM_FEATURE_IPC=0`
  - `TM_FEATURE_VOFA=0`
  - `TM_FEATURE_TASK_BUDGET=0`
  - `TM_FEATURE_VERBOSE_LOG=0`
  - `TM_FEATURE_SHELL` 由 profile 派生
- shared manifest 回归 TEC 基线
  - `subsys/CMakeLists.txt` 常规纳入 `tm_ipc.c`、`tm_shell.c`、`tm_task_budget_probe.c`
  - `applications/CMakeLists.txt` 常规纳入 `shell/*.c`、`tasks/*.c`
- shell-off 改为源码级自裁，不再依赖 CMake 排除源码
  - 不生成 `SHELL_CMD_EXPORT(...)`
  - 不保留 `_shell_cmd_*`
- IPC 默认镜像依赖 `TM_IPC_ENABLE=0` 自裁，不再通过 shared CMake 排除
- linker reserve owner 改回平台脚本固定预算
  - `_Min_Heap_Size = 0`
  - `_Min_Stack_Size = 0x400`
- shell decimal parser 已替换 `strtof()` 路径
  - 只接受十进制格式
  - 拒绝 `1e3`、`nan`、`inf`、hex-float 和尾随垃圾字符
