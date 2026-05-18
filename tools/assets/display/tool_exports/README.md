# 取模工具导出规则

`tools/assets/display/tool_exports/` 用来保存字模软件、取模软件直接导出的原始文本或 C 数组。

这里的价值不在于“拿来直接编译”，而在于：

- 保留导出时的原始格式，方便问题回溯
- 记录工具参数，避免后续忘记当时的取模方向和字节顺序
- 作为脚本化清洗和转换的输入样本

当前正式子目录：

- `tools/assets/display/tool_exports/legacy_arrays/`
  - 保存历史项目直接沉淀下来的数组与头文件
  - 作为转换脚本和资源审查的输入样本

## 1. 为什么有些工具会多输出 3 个字节

很多工具会在像素 payload 前面附带 3 个辅助字节，例如：

```c
0x28, 0x18, 0x05
```

它们通常表示：

- `0x28` -> 宽度 `40`
- `0x18` -> 高度 `24`
- `0x05` -> stride `5`

这类字节是工具导出元数据，不是正式像素 payload。

## 2. Themis 的正式处理规则

### 2.1 在 `tool_exports/`

- 允许保留这 3 个字节
- 允许保留工具附带的注释和说明
- 允许保留不同工具的不同导出风格

### 2.2 在 `applications/resources/`

- 不再直接保留这类工具头
- 宽、高、stride 必须收口到 `struct mono_bitmap`
- `data[]` 里只保留纯像素字节

也就是说：

- 原始导出可以保留“工具味”
- 正式运行时资源必须收口成 Themis 自己的运行时契约

## 3. 当前推荐转换方式

当前最小工具链是：

```powershell
python scripts/mono_asset_codegen.py bitmap `
  --input tools/assets/display/tool_exports/example.txt `
  --output applications/resources/images/img_demo.c `
  --symbol display_img_demo `
  --width 40 `
  --height 24 `
  --stride 5 `
  --format page_vlsb `
  --pctolcd-header
```

这个脚本会：

- 解析工具导出的 `0x..` 十六进制字节
- 可选剥离 `<width,height,stride>` 三字节工具头
- 校验最终 payload 长度
- 生成符合 `mono_res.h` 的正式 `mono_bitmap` 资源文件

### 3.1 当输入文件里有多个数组时

如果同一个文件里同时存在多个数组，例如：

- `ascii_font`
- `ascii_font5x10`
- `ascii_font6x12`

就必须显式使用：

```powershell
--array-symbol <数组名>
```

脚本会只解析指定数组的初始化体，而不是把整个文件里的所有 `0x..` 一起吞进去。

### 3.2 固定字宽字体数组现在也支持直接转换

这轮已经新增 `font` 子命令，适合处理这类历史数组：

```c
const uint8_t ascii_font5x10[95][10] = { ... };
```

示例：

```powershell
python scripts/mono_asset_codegen.py font `
  --input "tools/assets/display/tool_exports/legacy_arrays/ascii_font.c" `
  --output applications/resources/fonts/font_ascii_5x10.c `
  --symbol display_font_ascii_5x10 `
  --array-symbol ascii_font5x10 `
  --width 5 `
  --height 10 `
  --stride 5 `
  --format page_vlsb `
  --glyph-count 95 `
  --first-codepoint 32 `
  --advance 6 `
  --fallback-codepoint '?'
```

如果要一次性重建正式运行时 ASCII 字库，优先使用：

```powershell
python scripts/generate_display_fonts.py
python scripts/generate_display_fonts.py --check
```

它会生成：

- glyph bitmap blob
- `mono_glyph[]`
- `mono_font`

也就是 `mono_canvas_draw_text()` 可直接消费的正式资源。

## 4. 当前正式建议

- 所有工具导出先归档到这里
- 不要把工具原始导出直接搬进 `applications/resources/`
- 如果遇到新的工具格式，先在这里保留样本，再补转换规则
- 如果资源已经进入正式运行时目录，对应的测试和文档也要同步补齐
