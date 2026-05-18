# Themis 显示源资产工作区

`tools/assets/display/` 是 Themis 显示资源的源资产工作区，不是正式运行时资源目录。

这里保存的是：

- 原始 BMP 设计稿
- 字模工具或取模工具的原始导出
- 历史项目遗留的字体/图片数组
- 资源设计说明与导入规则

这里不负责：

- 直接参与当前固件构建
- 作为应用层正式 `#include` 入口
- 承担运行时渲染 ABI

## 1. 当前目录定位

```text
tools/assets/display/
|-- README.md
|-- 我们的疑问.md
|-- source_bmp/
|-- tool_exports/
`-- tool_exports/legacy_arrays/
```

### 1.1 `source_bmp/`

- 保存设计阶段的 BMP 原稿
- 方便反复微调、重新导出和版本追溯

### 1.2 `tool_exports/`

- 保存字模软件或取模软件直接导出的原始文本或数组
- 允许保留工具头信息、注释和导出参数
- 是后续脚本化清洗的输入区

### 1.3 `tool_exports/legacy_arrays/`

- `ascii_font.*`、`mod_font.*` 当前保留为历史资产档案
- 可以作为参考来源
- 不应再直接复制进运行时链路
- 物理位置已经收口到 `tools/assets/display/tool_exports/legacy_arrays/`，避免仓库主目录继续承担运行时入口误导

## 2. 与运行时目录的关系

Themis 正式运行时资源统一收口在 `applications/resources/`：

- `components/display/mono_canvas/` 负责资源消费 ABI
- `applications/resources/` 负责正式字体和图片
- `tools/assets/display/` 负责源资产沉淀、追溯与再加工

这样做的意义是：

1. 不把设计资产、历史数组和运行时代码混在一起
2. 让应用层正式拥有资源入口，而不是让组件层背业务资源
3. 让“源资产”和“正式固件资源”长期共存，既方便迭代，也方便审计

## 3. 当前正式工作流

```text
设计稿 BMP
    -> tools/assets/display/source_bmp/
工具原始导出
    -> tools/assets/display/tool_exports/
规范化转换
    -> scripts/mono_asset_codegen.py
正式运行时资源
    -> applications/resources/
```

推荐流程：

1. 在 `source_bmp/` 保留原始设计稿
2. 在工具中导出原始数组，归档到 `tool_exports/`
3. 对导出结果做规范化处理
   - 图片优先用 `scripts/mono_asset_codegen.py bitmap`
   - 固定字宽字库优先用 `scripts/mono_asset_codegen.py font`
4. 生成或整理为 `mono_bitmap` / `mono_font` 兼容的运行时资源
5. 放入 `applications/resources/`
6. 运行 `mono_asset_codegen.py index` 更新统一导出头
7. 更新资源声明与测试

## 4. 当前正式原则

- `tools/assets/display/` 中的文件默认不是“能直接进固件”的正式资源
- 工具导出的冗余头信息应在转换阶段消化，不应直接带入运行时 payload
- 如果运行时资源需要追溯来源，应在 `tools/assets/display/` 中保留原稿和导出记录，而不是把历史注释塞满正式资源文件
- 后续即使引入更多字体、中文字库、多套主题，也继续坚持“源资产工作区”和“运行时目录”分离
