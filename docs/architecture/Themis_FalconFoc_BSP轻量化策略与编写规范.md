# Themis FalconFoc BSP 轻量化策略与编写规范

## 1. 文档定位

本文是 FalconFoc 当前面向小 MCU 产品的正式 BSP 架构文档。

它重点回答下面 6 个问题：

1. 为什么 FalconFoc 不适合把 Themis 的 full `device/` + `drivers/` 运行时整套搬进来。
2. 小 MCU 场景下，`platform`、`bsp/board`、`bsp/ports`、`applications` 的正确 owner 边界是什么。
3. FOC 热路径哪些必须继续直控，哪些软任务外围必须通过薄 BSP 收口。
4. BSP public API 默认应使用直接函数，还是函数指针表。
5. BSP 是否应该像 `platform` 一样继续按 `at/stm/gd/apm` 再复制一层厂商目录。
6. 后续 FalconFoc 的 BSP 代码到底应该怎么写，才能简单、直接、高效、安全、稳定、可靠。

本文不是对 Themis full `dev/drv` 路线的否定，而是 FalconFoc 在
`128 KB ROM / 32 KB RAM` 级产品约束下的正式工程裁决。

## 2. 背景与正式裁决

### 2.1 背景事实

Themis 的 full `device/` + `drivers/` 模型适合下面这类项目：

- 共享外设种类多
- 多产品复用强
- 平台治理、证据链、可观测性要求高
- MCU 资源预算相对宽松

但 FalconFoc 当前面对的典型产品约束是：

- FOC 电机控制
- OLED
- 蜂鸣器
- 电池电压检测
- 充电检测
- Shell
- 按键
- 关机倒计时
- 若干 GPIO 控制
- 蓝牙通信控制

这类产品的典型事实不是“设备种类无限扩张”，而是：

- FOC 热路径极强依赖 ADC / PWM / SPI / DMA / 中断拓扑
- 软任务外围数量有限
- ROM / RAM 预算紧
- MCU 经常落在 `128 KB / 32 KB` 级别

因此 FalconFoc 当前正式不走两条极端路线：

- 不回到 full `dev/drv`
- 也不退回全仓自由直调 vendor

### 2.2 正式裁决

FalconFoc 当前正式采用下面这条路线：

> **FOC 热路径直控，控制面和软任务外围通过薄 BSP 收口。**

展开就是：

- FOC 核心快路径：
  - 允许直接调 vendor 库
  - 允许直接碰寄存器
  - 不强行塞进 `bsp/ports`
- 软任务外围路径：
  - 通过 `bsp/board` + `bsp/ports` 收口
  - 只暴露产品真正需要的最小 northbound 动作
  - 不重新长成 `device/` / `drivers/` / `hal/`

一句话结论：

**FalconFoc 的可移植性来自把值得迁移的软任务外围隔离出来，不来自把所有硬件路径都统一抽象。**

## 3. 适用范围与不适用范围

### 3.1 本文适用的 BSP 目标

本文适用于下面这类外围：

- `bsp_console`
- `bsp_system`
- `bsp_ui_oled`
- `bsp_buzzer`
- `bsp_key_scan`
- `bsp_battery_sense`
- `bsp_charge_detect`
- `bsp_bt_comm`
- `bsp_led`
- `bsp_power_latch`
- `bsp_flash`

### 3.2 本文不适用的路径

下面这些路径不属于“薄 BSP 封装”的目标对象：

- FOC 电流环 ISR
- PWM 生成与更新
- ADC 采样序列与触发链
- DMA 耦合
- 编码器快路径
- 硬件故障关断
- 任何为了实时性必须直达 SoC 真相层的代码

这些路径继续由：

- `platform/<vendor>/<soc>/`
- `applications/periph/`
- `applications/algo/`

按当前 FalconFoc live 分层共同承担。

## 4. Owner 边界

### 4.1 `platform/<vendor>/<soc>`

拥有：

- startup
- linker
- vendor 头文件
- generated bring-up
- SoC timing truth
- Flash 几何、软中断实例、芯片 compile definition

不拥有：

- 产品任务语义
- Shell 词表
- 参数版本策略
- 板级业务命名

### 4.2 `bsp/board`

拥有：

- board facts
- 最小 pre-kernel glue
- 板级 on-chip Flash backend
- 必要的 board 级 descriptor

不拥有：

- FOC 热路径控制
- 产品 task / shell 语义
- 第二套 platform
- 复制 vendor driver stack

### 4.3 `bsp/ports`

拥有：

- 面向控制面的薄功能端口
- console / reset / OLED / buzzer / key / battery / charge / 蓝牙等软任务外围的最小 northbound 动作

不拥有：

- 通用 `hal/`
- 设备注册表
- vtable 森林
- “未来也许会复用”的泛化总线框架
- 产品策略和业务状态机

### 4.4 `applications/*`

拥有：

- 产品语义
- FOC 运行时编排
- 状态机
- Shell 命令词表
- 参数版本、恢复、保存策略
- 声光提示策略
- 电量阈值与关机策略
- 蓝牙协议与产品命令语义

## 5. 两类硬件路径必须分开理解

### 5.1 A 类：FOC 核心路径

典型对象：

- ADC
- PWM
- SPI
- DMA
- 编码器
- 保护中断

规则：

- 不进 `bsp/ports`
- 不为了“看起来整洁”多加一层函数壳
- 允许 vendor API 和寄存器直控

原因：

**这里的耦合是业务本质，不是架构失控。**

### 5.2 B 类：软任务外围路径

典型对象：

- OLED
- buzzer
- key
- battery
- charge detect
- shell console
- 蓝牙通信
- board Flash
- power latch / reset / LED

规则：

- 通过 `bsp/board` 或 `bsp/ports` 收口
- northbound 只给上层真正需要的动作
- 不提供“完整外设类 API”

## 6. BSP 对各类外围真正应该拥有的语义

| 外设 | BSP 应拥有 | BSP 不应拥有 |
| --- | --- | --- |
| OLED | 复位脚、DC/CS、底层发送、必要的总线 gate | 页面栈、字体策略、UI 业务语义 |
| 蜂鸣器 | 开关、频率、占空比、原始使能 | 提示音优先级、旋律、产品事件映射 |
| 按键 | 原始电平或位图采样 | click/long/repeat 解释、产品动作绑定 |
| 电池检测 | ADC 原始采样、必要的标定换算 | 电量分级、低电策略、关机倒计时 |
| 充电检测 | GPIO/ADC 原始状态 | 充电状态机、提示策略 |
| 蓝牙通信 | transport 收发、复位、使能脚 | 配对流程、产品协议、命令语义 |
| Shell console | DMA/IRQ transport、收发桥 | 命令词表、banner、输出策略 |
| Flash | 读写擦后端、几何事实 | 参数 schema、版本迁移、保存时机 |

正式原则：

> **BSP 只拥有板级真值与原始动作，不拥有产品解释。**

## 7. Public API 的正式裁决：默认使用直接函数，不默认上函数指针表

### 7.1 默认路线

FalconFoc BSP public API 的默认路线固定为：

- 直接函数
- 明确的参数
- 明确的返回值
- 单文件静态 owner 状态

推荐形态例如：

```c
int  bsp_buzzer_init(void);
int  bsp_buzzer_set(bool on);
int  bsp_buzzer_set_tone(uint16_t freq_hz, uint16_t duty_permille);

int  bsp_battery_sense_sample_mv(uint16_t *mv);
bool bsp_charge_detect_is_present(void);

int  bsp_ui_oled_write_cmd(const uint8_t *buf, uint16_t len);
int  bsp_ui_oled_write_data(const uint8_t *buf, uint16_t len);
```

### 7.2 为什么不默认上函数指针表

原因很直接：

1. FalconFoc 当前是单产品、单镜像、单活动板级基线思路。
2. 直接函数最省 ROM、RAM、调试复杂度和阅读成本。
3. 大多数小外设并没有“同镜像下多后端热切换”的真实需求。
4. 很多函数指针抽象最后只是在为“也许未来会换芯片”提前交长期税。

当前正式结论是：

> **“将来可能换芯片”不足以成为今天在 BSP public API 上铺函数指针表的理由。**

### 7.3 什么时候才允许引入函数指针或 ops

只有下面几类情况成立时，才建议引入 callback / ops / function pointer：

1. `components/` 里存在真正可复用的控制器或协议层，需要后端注入。
   - 例如 SSD13xx 显示控制器组件需要注入 `write_cmd / write_data / delay / reset`
2. 同一镜像里确实要支持多个可替换后端同时共存。
3. 某个共享总线 gate 需要显式事务 begin/end contract。
4. host test 需要的注入能力，已经超出“同名 stub 文件”可以简单承担的范围。

但即便如此，也应遵守：

- `ops` 优先放在 `components/` 或 private implementation 边界
- 不把 `applications -> bsp` 的主 northbound API 默认做成 vtable

## 8. BSP 不应该重新长成“简化版驱动层”

下面这些写法，FalconFoc 当前应明确视为反模式：

- `bsp_gpio_write(pin, level)`
- `bsp_spi_transfer(bus, tx, rx, len)`
- `bsp_adc_read_channel(ch, *raw)`
- `bsp_uart_open(name, cfg)`
- `bsp_periph_register(...)`
- `bsp_device_find(...)`

原因不是这些函数“不能工作”，而是：

- 它们会把 `bsp/` 推回“外设分类抽象层”
- 上层开始依赖“通用外设名词”，而不是产品真正动作
- 最终重新长成弱化版 `dev/drv`

FalconFoc 当前 BSP 要追求的是：

- `bsp_buzzer_set()`
- `bsp_battery_sense_sample_mv()`
- `bsp_charge_detect_is_present()`
- `bsp_power_latch_release()`
- `bsp_console_write_dma()`

也就是：

**按功能动作命名，不按外设门类复刻。**

## 9. BSP 目录策略：默认不按 `at/stm/gd/apm` 再复制一层

### 9.1 正式结论

FalconFoc 当前不建议在 `bsp/` 下再做一层：

```text
bsp/
├─ at/
├─ stm/
├─ gd/
└─ apm/
```

原因如下：

1. vendor / SoC 真相已经有正式 owner：
   - `platform/<vendor>/<soc>/`
2. `bsp/` 的本质是 board-owned layer，不是 vendor-owned layer。
3. 同一块板子换芯片时，真正变化的是 southbound implementation 和 board truth，不是 northbound 业务语义。
4. 如果 `bsp/` 再按 vendor 复制一层，通常会把同一份 board 语义拆成多套平行目录，维护成本更高。

### 9.2 FalconFoc 推荐目录

当前推荐长期保持：

```text
bsp/
├─ bsp_config.h
├─ board/
│  ├─ bsp_init.h
│  ├─ bsp_init.c
│  ├─ bsp_flash.h
│  └─ bsp_flash.c
└─ ports/
   ├─ bsp_console.h
   ├─ bsp_console.c
   ├─ bsp_system.h
   ├─ bsp_system.c
   ├─ bsp_ui_oled.h
   ├─ bsp_ui_oled.c
   ├─ bsp_buzzer.h
   ├─ bsp_buzzer.c
   ├─ bsp_key_scan.h
   ├─ bsp_key_scan.c
   ├─ bsp_battery_sense.h
   ├─ bsp_battery_sense.c
   ├─ bsp_charge_detect.h
   ├─ bsp_charge_detect.c
   ├─ bsp_bt_comm.h
   ├─ bsp_bt_comm.c
   ├─ bsp_led.h
   ├─ bsp_led.c
   ├─ bsp_power_latch.h
   └─ bsp_power_latch.c
```

### 9.3 什么时候才考虑在 BSP 里引入实现分支

只有下面条件同时出现时，才考虑在 BSP 内做 build-selectable implementation 分支：

- 同一仓库需要长期并行维护多个真实板级实现
- 同一份 `bsp_xxx.h` 必须对应多套 `.c`
- `platform` 选择已经不足以表达 board 差异

即便如此，也优先按 **board / variant** 分，而不是按 vendor 分。

## 10. 换芯片时，到底是改 `platform`、改 `bsp`，还是整包替换

### 10.1 正式原则

不要用“换芯片就替换整个 BSP”作为默认动作。

正确做法是按 owner 粒度替换：

| 变化类型 | 主要改动 owner | 尽量保持稳定的部分 |
| --- | --- | --- |
| 只换 SoC / vendor 库 | `platform/<vendor>/<soc>` + 受影响的 `bsp_xxx.c` | `applications/*` 和大多数 `bsp_xxx.h` |
| 板级引脚 / 外设实例变化 | `bsp_config.h` + 受影响的 `bsp/board` / `bsp_xxx.c` | 上层调用方式 |
| 产品新增外围 | 新增对应 `bsp_xxx.h/.c` + 上层产品逻辑 | 既有 BSP 契约 |
| FOC 拓扑变化 | `platform` + `applications/periph` | 软任务 BSP 契约 |

### 10.2 真正应该被“保护”的资产是什么

换芯片时真正值得保护的，不是某一份 `.c` 文件，而是：

- 上层调用语义
- `bsp_xxx.h` 的 northbound contract
- `bsp_config.h` 的 board truth 组织方式
- `platform -> bsp -> applications` 的 owner 顺序

因此：

> **迁移时应该优先稳定 northbound 头文件，重写 southbound `.c` 与 platform owner。**

### 10.3 一句话回答“要不要整个替换 BSP”

答案是：

- **默认不要整包替换**
- **按受影响的 board / BSP module owner 重写**
- **只要产品语义没变，就尽量保持 `bsp_xxx.h` 不变**

## 11. BSP public header 的书写规范

### 11.1 头文件必须像 consumer contract

`bsp/ports/*.h` 和 `bsp/board/*.h` 必须做到：

- 调用者不看 `.c` 也知道怎么用
- 明确 ownership / lifecycle / context
- 明确 blocking / timeout / failure 语义

### 11.2 public header 必须保持“北向纯净”

正式要求：

- 不在 public header 里泄漏 vendor MCU 头
- 不在 public header 里泄漏寄存器名
- 不在 public header 里泄漏 generated 结构体
- 只 include 当前 public declaration 真正需要的头

如果 public declaration 不需要 vendor 类型，就不应该把 vendor 头 include 进来。

### 11.3 API 分组建议

推荐按下面阶段组织：

- `Lifecycle`
- `Configuration / Binding`
- `Runtime Action / Runtime Sample`
- `Snapshot / Diag`

而不是把几十条声明平铺到底。

## 12. `.c` 实现文件的书写规范

### 12.1 include 规则

`.c` 文件顺序固定为：

1. 自己的头文件
2. 同域契约头
3. platform seam / vendor 头
4. 标准库头

### 12.2 vendor 头只进 `.c`

正式要求：

- `CMSIS_device_header`
- `wk_*`
- vendor Flash/UART/SPI/GPIO 头

这类内容默认只进入 `.c`，不进入 `.h`。

### 12.3 why-comment 要补在真正复杂的位置

特别是下面这些位置必须说明白：

- flash 写擦为什么只能 `idle-only`
- 为什么某条路径默认 `task-context only`
- 总线 lock / gate 的持有顺序
- rollback / recovery 逆序释放
- 为什么某个端口只暴露最小动作而不暴露完整总线 API

## 13. 返回值、错误码与 `errno` 口径

### 13.1 正式返回值规则

FalconFoc BSP public API 按当前仓库规范执行：

- 状态型函数：`int`
- 纯判断：`bool`
- 长度 / 计数型函数：优先 `ssize_t`，受现有接口约束时至少保持负 `errno` 语义
- 成功：`0` 或非负长度
- 失败：负 `errno`

### 13.2 不再自造 Public Error Codes

正式禁止继续在 BSP public contract 里长期扩散：

- `BSP_XXX_OK`
- `BSP_XXX_ERR_RANGE`
- `BSP_XXX_ERR_ALIGN`
- `BSP_XXX_ERR_OP`

这类“第二套错误码体系”。

后续统一收口到：

- `-EINVAL`
- `-ENODEV`
- `-EBUSY`
- `-EIO`
- `-ETIMEDOUT`
- `-ERANGE`
- `-ENOMEM`

等标准负 `errno` 词表。

### 13.3 `<errno.h>` 与 `tm_errno.h` 的关系

正式建议：

- northbound 契约按 `<errno.h>` 语义表达
- 如果工具链 `errno` 覆盖不完整，则通过仓库的 `tm_errno.h` 做兼容
- 但无论实现细节如何，**对上层暴露的都仍然是负 `errno` 语义**

也就是说：

> **允许用 `tm_errno.h` 做兼容，不允许重新发明一套 BSP 私有错误号。**

## 14. `bsp_config.h` 应该放什么，不应该放什么

### 14.1 应该放的内容

`bsp_config.h` 当前应只放 compile-time board truth，例如：

- board name
- 默认 console / telemetry 资源名
- OLED 地址或极性
- buzzer / LED / charge pin 极性
- ADC 分压系数相关 board fact
- 默认 feature-on-board truth

### 14.2 不应该放的内容

禁止继续往里塞：

- 产品业务策略
- Shell 命令语义
- 电量阈值策略
- 声光提示策略
- “为了以后通用”而先放进去的运行态开关

## 15. 共享资源与总线的正式口径

只有“真实共享”时才允许加 glue。

允许的形态例如：

- `ui_spi_gate_lock() / ui_spi_gate_unlock()`
- `bsp_ui_oled_begin() / bsp_ui_oled_end()`
- `bsp_bt_comm_rx_start()`

不允许的形态例如：

- 先造一个 `spi_manager`
- 再造一个 `device registry`
- 再造一个 `bus object`
- 最后只是为了给 OLED 发几字节

当前原则固定为：

> **共享只在真实共享时成立，不为猜测中的复用预付框架税。**

## 16. BSP 的上下文与阻塞规则

### 16.1 默认上下文

除非 public header 明确写出 `ISR-safe`，否则一律按：

- `task-context only`

理解。

### 16.2 慢路径必须显式说明

尤其是下面这些路径，必须在头文件或文档里说明：

- Flash 擦写
- OLED 大块刷屏
- 蓝牙阻塞发送
- 电池采样平均与滤波
- 任何带 timeout / busy wait 的路径

### 16.3 与 FOC 实时性的关系

如果某个 BSP 端口会长时间阻塞或会关中断：

- 只能在安全任务或 idle 态执行
- 不能在 FOC 运行关键阶段随意调用
- 不能让 shell 命令绕开产品状态机直接进入高风险慢路径

## 17. 推荐模板

### 17.1 头文件模板方向

```c
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int  bsp_buzzer_init(void);
int  bsp_buzzer_set(bool on);
int  bsp_buzzer_set_tone(uint16_t freq_hz, uint16_t duty_permille);
void bsp_buzzer_deinit(void);

#ifdef __cplusplus
}
#endif
```

### 17.2 实现文件模板方向

```c
#include "bsp_buzzer.h"
#include "bsp_config.h"

#ifndef CMSIS_device_header
#error "bsp_buzzer.c requires CMSIS_device_header from platform compile definitions."
#endif

#include CMSIS_device_header

int bsp_buzzer_init(void)
{
    /* board-local bring-up */
    return 0;
}

int bsp_buzzer_set(bool on)
{
    /* write board-owned pin or timer gate */
    return 0;
}
```

模板重点不是长得多复杂，而是：

- `vendor` 头只留在 `.c`
- northbound API 保持小而稳定
- 参数和错误码讲清楚

## 18. 当前 FalconFoc 仓库的直接整改方向

当前仓库后续新增或重写 BSP 文件时，应统一按下面口径执行：

1. `bsp_console`、`bsp_system` 继续作为薄 `port` owner，不往里倒灌产品语义。
   其中 `bsp_system` 现已作为正式样板收口为 `int bsp_system_reset(void)` + `0 / -errno` + `ISR reject`。
2. `bsp_flash` 继续保留 board-owned backend 身份，但 public contract 收口到负 `errno` 语义。
3. 若 public header 不需要 vendor 类型，就移除不必要的 vendor 头暴露。
4. 新增外围统一采用 `bsp_xxx.h/.c` 命名，不新增 `hal/`。
5. OLED、按键、蜂鸣器、电池、充电、蓝牙等外围按“动作级 contract”建口，不按“外设类 API”建口。
6. host stub 继续只验证 northbound contract，不验证 vendor 细节。

## 19. 最终口径

FalconFoc 当前对 BSP 的正式判断可以压成下面 8 条：

1. 小 MCU FalconFoc 不回 full `dev/drv`，也不退回全仓自由直调。
2. FOC 热路径继续直控 vendor 或寄存器。
3. 软任务外围通过 `bsp/board` + `bsp/ports` 收口。
4. BSP public API 默认使用直接函数，不默认上函数指针表。
5. `platform` 按 vendor / SoC 分；`bsp` 默认不按 vendor 再复制目录。
6. 换芯片时优先稳定 `bsp_xxx.h`，重写受影响的 `.c`、`bsp_config.h` 和 `platform` owner。
7. BSP public contract 统一收口到负 `errno`，不再自造第二套 public error code。
8. BSP 只拥有板级真值与原始动作，不拥有产品解释与业务策略。

一句话总结：

> **FalconFoc 的薄 BSP，不是把 Themis 做简陋，而是把抽象只放在真正值得抽象的地方。**
