# FalconFoc 项目目录结构速查

## 1. 当前 live 目录骨架

```text
FalconFoc_AT32/
├── applications/
├── bsp/
│   ├── board/
│   └── ports/
├── components/
├── docs/
├── include/
├── kernel/
├── platform/
│   └── at/at32f403a/
├── scripts/
├── subsys/
├── tests/
├── tools/
└── work/
```

## 2. 当前 owner 解释

- `applications/`
  - 产品 owner
  - 启动桥、任务、shell、参数、FOC 应用侧逻辑
- `bsp/board/`
  - board facts、最小 pre-kernel glue、flash backend
- `bsp/ports/`
  - control-plane 硬件薄封装
- `components/`
  - 可复用模块
- `include/`
  - 公共头和仓库级配置入口
- `kernel/`
  - FreeRTOS / CMSIS-RTOS2 集成
- `platform/at/at32f403a/`
  - 芯片真相层
- `scripts/`
  - live 构建、验证、board smoke 和门禁入口
- `subsys/`
  - 共享机制
- `tests/`
  - live host/script contract tests
- `work/`
  - 图形化工具工作区，不纳入当前重构 owner

## 3. 关键边界

- FalconFoc 当前不使用 Themis `device/` / `drivers/` runtime
- FOC 热路径可直接使用 vendor 或寄存器
- 软任务外设通过 `bsp/ports` 收口
- `platform/at/at32f403a` 是唯一 live SoC owner
