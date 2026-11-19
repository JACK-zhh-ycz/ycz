# ycz 插件 SDK

插件就是一个 **dll**。放到 `build/plugins/` 下，主程序启动时自动扫描加载；删掉即卸载，不用登记。

## 目录

```
sdk/
├─ plugin_api.h         插件 ABI 头文件（C 接口 + POD 结构）
├─ hello_page.cpp       可直接编译的示例：给导出的 HTML 加页码
├─ 编译示例插件.bat      双击编译并复制到 ..\build\plugins\
```

## 最小插件

```cpp
#include "plugin_api.h"

PPT_PLUGIN_API const PptPluginInfo* GetPluginInfo() {
    static PptPluginInfo info;
    memset(&info, 0, sizeof(info));
    info.abi = PPT_PLUGIN_ABI_VERSION;   // 固定 1
    wcscpy(info.name, L"我的插件");
    wcscpy(info.author, L"你的名字");
    info.version = 1;
    wcscpy(info.desc, L"一句话说明");
    return &info;
}

PPT_PLUGIN_API int InitPlugin(const PptHostIf* host) {
    if (host && host->log) host->log(L"我的插件已初始化");
    return 0;
}
```

只要这两个函数，插件就能被识别并出现在插件面板里。

## 可选钩子

| 函数 | 时机 | 用途 |
|---|---|---|
| `HookHtmlHead(meta, out, cap)` | 导出 HTML 时 | 往 `<head>` 加 CSS / JS |
| `HookHtmlSlide(meta, out, cap)` | 每页渲染完 | 追加 HTML；`meta` = `idx=0&total=7&title=...` |
| `HookTransition(cur, out, cap)` | 导出时 | 改切换效果名 |
| `HookCommand(cmd, arg, out, cap)` | `--cmd` 命令 | 自定义命令，命令名自己定 |

返回 `0` = 不处理；`>0` = 已处理，值为写入 `out` 的字节数（UTF-8）。`out_cap` 是容量，写不下就返回 0，**绝不越界**。

## 宿主能力表 `PptHostIf`

```
void log(const wchar_t* msg)
int  slide_count()
const wchar_t* slide_title(int idx)
const wchar_t* pres_title()
long long slide_w_emu() / slide_h_emu()
```

只读；要改文档内容请走 `HookCommand` 自定义命令。

## 编译

需要 **x86_64 的 MinGW-w64 g++**（必须与主程序同架构）：

```bash
g++ -std=c++17 -O2 -shared -DPPT_PLUGIN_EXPORTS hello_page.cpp \
    -o hello_page.dll -static-libgcc -static-libstdc++
```

- `-DPPT_PLUGIN_EXPORTS`：切换成 `dllexport`，漏了会"加载成功但找不到函数"
- `-static-libgcc -static-libstdc++`：静态链接运行库，换机器也能加载

Windows 下也可直接双击 `编译示例插件.bat`。

## 调试

- 在 `InitPlugin` 里 `host->log(L"...")`，启动日志会打印
- 插件加载失败不会让主程序崩溃，只是列表里没有它
- dll 被主程序占用时无法覆盖，改插件先关程序
