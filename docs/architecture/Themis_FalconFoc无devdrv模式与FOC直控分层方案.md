# Themis FalconFoc 无 `dev/drv` 模式与 FOC 直控分层方案

## 1. 文档定位

本文是 FalconFoc 当前正式架构裁决文档。

它只回答四件事：

1. 为什么 FalconFoc 不回到 full `device/` + `drivers`
2. 为什么 FalconFoc 也不能退化成全仓自由直调 vendor
3. 当前 `platform -> bsp -> applications` 的正式分层是什么
4. 这套分层如何直接落到当前仓库

## 2. 正式裁决

FalconFoc 当前正式采用：

- FOC 核心路径直控
- 软任务外围按功能做薄 `port`
- 只在真实共享时加最小 glue
- `event bits + command latch + snapshot` 作为层间通信合同

同时正式否决两条路线：

- 回到 full `device/` / `drivers`
- 把整仓退回全局自由直调 vendor

一句话结论：

**FalconFoc 的实时性来自 FOC 核心路径不抽象；FalconFoc 的可维护性来自把可迁移部分隔离出来，而不是强行把所有路径统一抽象。**

## 3. 背景约束

当前约束不是“语义统一优先”，而是：

- FOC 热路径必须保实时
- 资源预算必须可控
- 代码边界必须能长期维护
- MCU 迁移必须聚焦在值得迁移的部分

当前 live 仓库事实：

- live platform owner: `platform/at/at32f403a`
- live board owner: `bsp/board`
- live control-plane port owner: `bsp/ports`
- live product owner: `applications/`
- FalconFoc 当前不装配 `device/` / `drivers/` runtime

## 4. 路线对比

| 路线 | 结论 | 原因 |
| --- | --- | --- |
| full `dev/drv` | 否决 | 统一但 ROI 错，对当前 FOC 的 ROM、RAM、调用链和维护成本都不合适 |
| 全局自由直调 vendor | 否决 | bring-up 快，但软任务外围会失控，迁移成本会扩散到应用层 |
| FOC 直控 + 功能薄 `port` | 采用 | 同时满足实时性、边界、迁移收敛和当前资源约束 |

## 5. 正式分层设计

```text
applications/algo + applications/periph
    -> FOC core path

applications/tasks + applications/shell
    -> runtime orchestration / state machine / shell / diagnostics

bsp/board + bsp/ports
    -> board fact / board glue / thin functional ports

platform/at/at32f403a
    -> SoC baseline / vendor / generated / startup / linker
```

### 5.1 `platform/at/at32f403a`

职责：

- 芯片基线
- vendor / generated / startup / linker / `wk_*`

禁止：

- 产品参数语义
- shell 语义
- board 业务命名

### 5.2 `bsp/board`

职责：

- board pre-kernel glue
- 板级事实
- 必要的 on-chip flash backend

禁止：

- 复制 vendor driver
- 承载产品 service
- 演化成第二套 platform

### 5.3 `bsp/ports`

职责：

- 控制面硬件薄封装
- 为 shell、reset、OLED、LED、按键、battery sense 一类软任务外围提供最小 northbound 动作

禁止：

- 进入 FOC 热路径
- 建通用 HAL
- 建 vtable、设备表、三层 wrapper

### 5.4 `applications/algo` 与 `applications/periph`

职责：

- FOC 算法
- FOC 强耦合硬件路径

正式允许：

- 直接调 vendor API
- 必要时直接调寄存器

### 5.5 `applications/tasks` 与 `applications/shell`

职责：

- task cadence
- state machine
- shell / diagnostics
- 参数保存触发
- 外围编排

禁止：

- 把软任务外围路径继续散落成 vendor include 污染面

## 6. 两类硬件路径

### 6.1 A 类：FOC 核心路径

典型内容：

- 电流环 ISR
- PWM
- ADC trigger / sampling sequence
- DMA
- 编码器快路径
- 硬件故障关断

规则：

- 不进 `port`
- 可直接调 vendor
- 可直接写寄存器

原因：

**这里的耦合不是技术债，而是 FOC 业务本质。**

### 6.2 B 类：软任务外围路径

典型内容：

- console
- telemetry
- OLED
- LED
- buzzer
- key scan
- battery sense
- charge detect
- 参数 flash

规则：

- 按功能做薄 `port`
- 共享时才加最小 glue
- 不建通用 HAL，不建 vtable，不建三层目录

## 7. `port` 设计口径

推荐命名：

- `console_port`
- `system_port`
- `param_flash_port`
- `ui_oled_port`
- `led_port`
- `buzzer_port`
- `key_scan_port`
- `battery_sense_port`
- `charge_detect_port`

文件形态：

1. `bsp/bsp_config.h`
2. `bsp/ports/<name>_port.h`
3. `bsp/ports/<name>_port.c`
4. host stub

规则：

- vendor 头只进 `.c`
- `port.h` 只暴露当前产品真正需要的动作
- board fact 进 `bsp_config.h`
- host 测试看 northbound contract，不看 vendor 细节

### 7.1 完整落地模板

示例：`led_port`

`bsp/ports/led_port.h`

```c
#pragma once

#include <stdint.h>

void led_port_init(void);
void led_port_set(uint8_t on);
```

`bsp/ports/led_port.c`

```c
#include "led_port.h"
#include "bsp_config.h"
#include "at32f403a_407.h"

void led_port_init(void)
{
    /* vendor init or generated binding */
}

void led_port_set(uint8_t on)
{
    if (on != 0U) {
        /* set pin */
    } else {
        /* clear pin */
    }
}
```

host stub：

```c
#include "led_port.h"

static uint8_t g_led_state;

void led_port_init(void)
{
    g_led_state = 0U;
}

void led_port_set(uint8_t on)
{
    g_led_state = (on != 0U) ? 1U : 0U;
}
```

要点：

- `board fact` 留在 `bsp_config.h`
- vendor include 只留在 `.c`
- 上层永远只看功能动作，不看 GPIO 细节

## 8. 参数与启动链设计

### 8.1 参数 owner

参数系统应拆成：

- flash 读写和扇区动作：`bsp/board` 或参数 flash port owner
- 参数默认值、版本升级、恢复策略：`applications/algo/foc_param.*`
- 保存触发时机：task / state machine owner

禁止：

- ISR 里写 flash
- shell 绕过参数模块直接改 slot

### 8.2 启动链 owner

正式链路：

```text
platform generated main
    -> chip-level raw bring-up
    -> applications/app_main
    -> bsp/board/bsp_init
    -> export/init
    -> task bring-up
    -> motor runtime
```

规则：

- 芯片基线 bring-up 在下
- 产品 orchestration 在上

## 9. 层间通信设计

正式采用：

- `event bits`
- `command latch`
- `snapshot`

### 9.1 `event bits`

方向：

- `ISR -> task`

用途：

- fault 锁存
- calib done
- stop done
- dirty 标志

### 9.2 `command latch`

方向：

- `task -> ISR`

用途：

- start
- stop
- clear fault
- enter calib
- abort calib

### 9.3 `snapshot`

方向：

- `ISR -> task / shell / diag`

用途：

- 电流
- 电压
- 角度
- 速度
- fault bits
- 轴状态

### 9.4 明确禁止

- 高频 ISR 持续灌普通 queue
- 用散落 `volatile` 全局变量充当完整协议
- 在 ISR 中做 flash 保存或复杂策略判断

## 10. 共享资源规则

只有真实共享时才允许加 glue。

允许形态：

- `spi_bus_gate_lock() / spi_bus_gate_unlock()`
- `ui_spi_begin() / ui_spi_end()`
- `console_channel_write()`

禁止：

- 总线对象层层包装
- 动态设备注册
- 以“以后可能共享”为由先建 manager

## 11. MCU 迁移口径

FalconFoc 的 MCU 迁移能力来自：

- FOC 核心路径承认需要重写
- 软任务外围路径尽量收敛在 `bsp/ports`

错误做法是：

- 为了减少未来重写，先在今天把 FOC 核心路径抽象成慢接口

## 12. 当前仓库目标目录

```text
bsp/
├─ bsp_config.h
├─ board/
│  ├─ bsp_init.h/.c
│  └─ bsp_flash.h/.c
└─ ports/
   ├─ console_port.h/.c
   ├─ system_port.h/.c
   ├─ param_flash_port.h/.c
   ├─ ui_oled_port.h/.c
   ├─ led_port.h/.c
   ├─ buzzer_port.h/.c
   ├─ key_scan_port.h/.c
   ├─ battery_sense_port.h/.c
   └─ charge_detect_port.h/.c
```

命名规则：

- 用 `port`
- 不用 `hal`

## 13. 当前仓库收口要求

当前仓库必须满足：

1. FOC 核心快路径不被强行塞进 `port`
2. `applications/tasks` 与 `applications/shell` 不再散落无关 vendor include
3. 新增软任务外围只按功能级 `port` 接入
4. 参数保存不在 ISR 中执行
5. live 文档、脚本、测试、配置只描述 FalconFoc 自己

## 14. 最终口径

1. FOC 核心路径直控
2. 软任务外围按功能级 `port` 收口
3. 共享只在真实共享时成立
4. `ISR / task / state machine` 通过 `event bits + command latch + snapshot` 协作
5. `platform` 是芯片真相层，`bsp` 是板级 glue 层，`applications` 是产品 owner 层
