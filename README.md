# ycz

一个零第三方依赖的 PPTX 编辑器：C++ 内核 + Python 界面，插件可扩展，可导出带脚本与媒体的 HTML 放映。

仓库地址：https://github.com/JACK-zhh-ycz/ycz

![截图](docs/screenshot.png)

## 中文

**ycz** 是一个轻量的演示文稿编辑器。C++ 内核负责解析、渲染与写回 `.pptx`，Python 界面负责交互，插件以 DLL 的形式在运行时加载。目标不是替代 PowerPoint，而是提供一个**内核固定、行为可换、能把演示带走**的工具。

### 功能

- 打开 / 新建 / 保存 / 另存 `.pptx`，撤销 80 步
- 文字：字体、字号、颜色、粗斜下划线、对齐、画面内就地编辑、超长自动缩小
- 形状：45 个预设（基本 / 线条箭头 / 立体 / 流程图），拖动、缩放、旋转、多选、对齐分布、吸附参考线
- 表格：Excel 风格网格与表头、整表拖动、单元格输入、增删行列、导入导出 Excel
- 媒体：图片、音视频、网页快照、嵌入外部程序，每个形状可挂 JS 脚本
- 动画：17 种切换动画 + 形状入场动画
- 导出：单文件 HTML 放映，放映时可翻页、全屏、画笔批注
- 插件：水印、页码、进度条、网页嵌入，均可由 `build/plugins/` 下的 DLL 增删

### 构建

需要 Windows 10/11 x64、MinGW-w64（x86_64）和 PowerShell。

```powershell
powershell -ExecutionPolicy RemoteSigned -File build.ps1
```

产物：

```
build\pptx-core.dll     内核（Python 通过 ctypes 调用）
build\plugins\*.dll     示例插件
build\pptx-tool.exe     可选命令行工具（加 -WithExe）
```

### 运行

```powershell
pip install customtkinter pillow
python src\pyui\editor.py
```

`src\pyui\core.py` 会在 `src\pyui`、`build\`、`..\build` 中查找 `pptx-core.dll`，保持 `src\` 与 `build\` 同级即可。

### 插件

一个 DLL 就是一个插件，放进 `build\plugins\` 后重启生效，删掉即卸载。最少只需导出两个函数：

```cpp
const PptPluginInfo* GetPluginInfo();
int InitPlugin(const PptHostIf* host);
```

可选钩子：`HookHtmlHead`、`HookHtmlSlide`、`HookTransition`、`HookCommand`。编译方式与字段定义见 [sdk/](sdk/)。

### 已知问题

- 整页渲染约 200 ms/帧，拖动时已改为只画选框，但尚无渲染缓存与脏矩形
- 单文件 exe 启动需解包，约 2–4 秒
- 表格暂不支持合并单元格、列宽拖拽、公式
- 文字排版为手算换行 + 自动缩小，行距与段距控制较粗
- 仅支持 Windows；未签名 exe 可能被杀软误报

### 关于 AI 参与

本项目由 **Qwen** 与 **Doubao** 两个模型先后协作完成：早期版本由 **Qwen** 搭建底层与界面骨架，后续功能由 **Doubao** 迭代实现。人类作者负责需求定义、交互设计、逐轮验收、缺陷定位与发布维护。

项目以 MIT 协议开源，欢迎任何人修改、分发、商用。

## English

**ycz** is a lightweight presentation editor. A C++ core parses, renders and writes back `.pptx` files; a Python UI handles interaction; plugins load as DLLs at runtime. It is not meant to replace PowerPoint, but to be a tool where **the core stays fixed, behavior stays swappable, and a presentation can travel with you**.

### Features

- Open / new / save / save-as `.pptx`, 80-step undo
- Text: font, size, color, bold/italic/underline, alignment, in-place editing, auto shrink for overflow
- Shapes: 45 presets (basic / lines and arrows / 3D / flowchart), drag, resize, rotate, multi-select, align and distribute, snapping guides
- Tables: Excel-style grid and header, whole-table drag, cell editing, add/remove rows and columns, Excel import and export
- Media: images, video, audio, web snapshots, embedded external programs, per-shape JavaScript
- Animation: 17 slide transitions plus shape entrance animations
- Export: single-file HTML slideshow with paging, fullscreen and a pen layer
- Plugins: watermark, page numbers, progress bar, web embed, all addable or removable as DLLs under `build/plugins/`

### Build

Requires Windows 10/11 x64, MinGW-w64 (x86_64) and PowerShell.

```powershell
powershell -ExecutionPolicy RemoteSigned -File build.ps1
```

Output:

```
build\pptx-core.dll     kernel (loaded by Python through ctypes)
build\plugins\*.dll     sample plugins
build\pptx-tool.exe     optional CLI (pass -WithExe)
```

### Run

```powershell
pip install customtkinter pillow
python src\pyui\editor.py
```

`src\pyui\core.py` looks for `pptx-core.dll` in `src\pyui`, `build\` and `..\build`, so keep `src\` and `build\` side by side.

### Plugins

One DLL is one plugin: drop it into `build\plugins\`, restart to load, delete to unload. The minimum is two exports:

```cpp
const PptPluginInfo* GetPluginInfo();
int InitPlugin(const PptHostIf* host);
```

Optional hooks: `HookHtmlHead`, `HookHtmlSlide`, `HookTransition`, `HookCommand`. See [sdk/](sdk/) for the ABI and build command.

### Known issues

- Full-page rendering takes about 200 ms per frame; during drags only the selection box is painted, but there is no render cache or dirty-rectangle yet
- The single-file exe unpacks at startup, roughly 2–4 seconds
- Tables do not support merged cells, column resizing or formulas
- Text layout is manual line breaking plus auto shrink; line and paragraph spacing controls are coarse
- Windows only; unsigned exe files may trigger antivirus false positives

### About AI involvement

This project was built by two models in sequence: **Qwen** created the early foundation and UI skeleton, and **Doubao** implemented the later feature work. The human author defined requirements, designed interactions, reviewed and tested every round, located defects and handles releases.

Released under the MIT license: modify, distribute and use commercially as you like.

> AI 翻译

## License

MIT
