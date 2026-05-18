# BMP Source Rules

`tools/assets/display/source_bmp/` 用来保存图片与字模的原始设计稿。

建议规则：

- 每个页面元素都保留可回溯的 BMP 源文件，不只保存最终 C 数组
- 文件名使用 `用途_尺寸_版本.bmp`
- 同一资源如有黑白反色版本，文件名显式区分
- 同一资源如对应多种屏幕布局，分别保存，不要覆盖
- 一个 BMP 只表达一个稳定资源，不把多张逻辑图片拼成一张“大总图”

推荐命名例子：

```text
logo_boot_48x24_v1.bmp
icon_heat_16x16_v2.bmp
icon_pump_16x16_v1.bmp
title_heating_40x24_v1.bmp
```

建议同时保留一份简短说明文件，记录：

- 设计用途
- 目标尺寸
- 是否反色
- 使用的软件与取模设置
- 导出日期

原则：

- `BMP` 是设计与微调入口
- `tool_exports/` 是工具原始产物归档
- `applications/resources/` 才是正式固件资源
