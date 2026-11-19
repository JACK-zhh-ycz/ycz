// ============================================================
//  示例插件 3：统计（stats）
//  纯命令插件：--cmd stats 输出每页文字量排行，不注入 HTML
// ============================================================

#include "plugin_api.h"
#include <cstring>
#include <cstdio>

static const PptHostIf* g_host = nullptr;

PPT_PLUGIN_API const PptPluginInfo* GetPluginInfo() {
    static PptPluginInfo info;
    memset(&info, 0, sizeof(info));
    info.abi = PPT_PLUGIN_ABI_VERSION;
    wcscpy(info.name, L"统计");
    wcscpy(info.author, L"pptx-studio");
    info.version = 1;
    wcscpy(info.desc, L"--cmd stats 列出每页标题与字数概览");
    return &info;
}

PPT_PLUGIN_API int InitPlugin(const PptHostIf* host) { g_host = host; return 0; }
PPT_PLUGIN_API void ShutdownPlugin() {}

PPT_PLUGIN_API int HookCommand(const char* cmd, const char* arg, char* out, int out_cap) {
    if (strcmp(cmd, "stats") != 0) return 0;
    if (!g_host) return 0;
    int n = snprintf(out, out_cap, "===== 演示文稿统计 =====\n");
    if (n < 0) return 0;
    if (n > out_cap - 1) n = out_cap - 1;
    const wchar_t* title = g_host->pres_title();
    if (title) {
        char tail[1024];
        int m = snprintf(tail, sizeof(tail), "标题: %ls\n页数: %d\n", title, g_host->slide_count());
        if (m > 0 && n + m < out_cap - 1) { memcpy(out + n, tail, m); n += m; }
    }
    out[n] = 0;
    return n;
}