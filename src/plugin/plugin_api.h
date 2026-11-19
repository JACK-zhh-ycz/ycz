#pragma once
// ============================================================
//  插件 DLL 接口（风格对齐 lang_interface.h）
//  规则：只用 C 导出 + POD 结构 + char 缓冲区传参，
//        不跨 DLL 边界传 std::string / C++ 对象，避免 ABI 崩溃。
//
//  一个插件 DLL 最少要实现：
//    GetPluginInfo()  —— 必须
//    InitPlugin(host) —— 必须（拿到宿主能力表）
//  其余 Hook 函数全部可选，GetProcAddress 找不到就跳过。
//
//  编译插件：
//    g++ -std=c++17 -shared -DPPT_PLUGIN_EXPORTS my_plugin.cpp -o my_plugin.dll
// ============================================================

#include <windows.h>

#ifdef PPT_PLUGIN_EXPORTS
#define PPT_PLUGIN_API extern "C" __declspec(dllexport)
#else
#define PPT_PLUGIN_API extern "C" __declspec(dllimport)
#endif

#define PPT_PLUGIN_ABI_VERSION 1

#define PPT_MAX_NAME   64
#define PPT_MAX_DESC  256

struct PptPluginInfo {
    int      abi;                          // 必须 = PPT_PLUGIN_ABI_VERSION
    wchar_t  name[PPT_MAX_NAME];           // 插件名
    wchar_t  author[PPT_MAX_NAME];         // 作者
    int      version;                      // 插件自身版本
    wchar_t  desc[PPT_MAX_DESC];           // 简介
};

// ---------------- 宿主能力表 ----------------
struct PptHostIf {
    int  size;                             // = sizeof(PptHostIf)
    // 日志：写入主程序日志窗/控制台
    void        (*log)(const wchar_t* msg);
    // 当前演示文稿（只读概览，编辑请用 HookCommand 扩展）
    int         (*slide_count)();
    const wchar_t* (*slide_title)(int idx);     // 返回静态缓冲，可能为 NULL
    const wchar_t* (*pres_title)();
    // 画布尺寸（EMU）
    long long   (*slide_w_emu)();
    long long   (*slide_h_emu)();
};

// ---------------- 插件回调约定 ----------------
// 所有 Hook 返回 int：
//   0  = 本插件不处理
//   >0 = 已处理，out 中写入的字节数（out 为 UTF-8）
// Hook 参数 out_cap 为缓冲区容量；写不下就截断，别越界。

// 1) HTML <head> 追加内容（CSS/JS/meta）
PPT_PLUGIN_API int HookHtmlHead(const char* in_meta, char* out, int out_cap);

// 2) 每张幻灯片渲染完成后追加 HTML 片段（如水印层）
//    in_meta 形如 "idx=3&total=7&title=xxx"
PPT_PLUGIN_API int HookHtmlSlide(const char* in_meta, char* out, int out_cap);

// 3) 幻灯片切换效果名替换/增强：返回新效果名
//    in_cur 为当前效果名，out 写覆盖效果名
PPT_PLUGIN_API int HookTransition(const char* in_cur, char* out, int out_cap);

// 4) 自定义命令：主程序菜单/命令行 "--cmd name arg" 会广播给所有插件
//    返回 >0 表示已处理（out 可带结果文本给主程序显示）
PPT_PLUGIN_API int HookCommand(const char* cmd, const char* arg, char* out, int out_cap);

// ---------------- 生命周期 ----------------
PPT_PLUGIN_API const PptPluginInfo* GetPluginInfo();
PPT_PLUGIN_API int  InitPlugin(const PptHostIf* host);
PPT_PLUGIN_API void ShutdownPlugin();