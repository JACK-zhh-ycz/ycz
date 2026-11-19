# 贡献指南

## 先跑起来

```powershell
powershell -ExecutionPolicy RemoteSigned -File build.ps1     # 编内核和示例插件
pip install customtkinter pillow
python src\pyui\editor.py
```

## 改动前

1. 先读 `src/core/model.h`——它是中枢，几何统一用 EMU（1pt = 12700 EMU）
2. 改 PPTX 写回相关代码前，务必确认"未编辑部件逐字节照抄"这条约束没被破坏
   （`src/io/pptx_io.cpp`，原文优先 + `ed_geom/ed_text/ed_fill/ed_line` 标记）
3. 新增内核接口：先加 `src/api/pptx_api.h` 的 C ABI，再在 `src/pyui/core.py` 里包一层，最后才碰界面

## 提交

- 一个 PR 只做一件事，标题写清楚改了什么
- 带上能说明行为变化的测试（`test_*.py`，无需 GUI 的优先）
- C++ 改动注明是否影响 PPTX 兼容性
- 不要提交 `build/`、`dist/`、`*.dll`、`*.exe`

## 代码风格

- C++：C++17，`snake_case` 函数、`PascalCase` 类型；只用 Win32/GDI+，不引入第三方库
- Python：PEP 8 基本遵守，界面字符串用中文
- 注释写"为什么"，不写"做了什么"

## 插件贡献

插件放 `plugins/<名字>/`，源码 + 自带一份 `plugin_api.h`，然后在 `build.ps1` 的 `$plugins` 列表里登记。

## 关于 AI 生成代码

本项目大量代码由 AI 生成。提交 PR 时不需要隐瞒这一点，但请确认你**真的读过、编译过、跑过**你提交的代码——评审标准是"能不能跑"，不是"谁写的"。
