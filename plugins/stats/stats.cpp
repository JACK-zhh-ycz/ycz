// 示例插件 3：统计 —— 命令型插件，--cmd stats 输出概览
#include "../api_shim.h"
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
    wcscpy(info.desc, L"--cmd stats 列出页数与每页标题概览");
    return &info;
}

PPT_PLUGIN_API int InitPlugin(const PptHostIf* host) { g_host = host; return 0; }
PPT_PLUGIN_API void ShutdownPlugin() {}

PPT_PLUGIN_API int HookCommand(const char* cmd, const char* arg, char* out, int out_cap) {
    if (strcmp(cmd, "stats") != 0) return 0;
    if (!g_host) return 0;
    int off = 0;
    int n = snprintf(out + off, out_cap - off - 1, "===== 演示文稿统计 =====\n");
    if (n > 0) off += n;
    const wchar_t* t = g_host->pres_title();
    if (t) {
        n = snprintf(out + off, out_cap - off - 1, "标题: %ls\n", t);
        if (n > 0) off += n;
    }
    int cnt = g_host->slide_count();
    n = snprintf(out + off, out_cap - off - 1, "页数: %d\n", cnt);
    if (n > 0) off += n;
    for (int i = 0; i < cnt && off < out_cap - 128; ++i) {
        const wchar_t* st = g_host->slide_title(i);
        n = snprintf(out + off, out_cap - off - 1, "  第%d页: %ls\n", i + 1, st ? st : L"(无标题)");
        if (n > 0) off += n;
    }
    out[off] = 0;
    return off;
}