// 示例插件 1：水印 —— 每页右下角注入半透明水印
#include "../api_shim.h"
#include <cstring>
#include <cstdio>

static const PptHostIf* g_host = nullptr;
static char g_text[256] = "PPTX-STUDIO";

PPT_PLUGIN_API const PptPluginInfo* GetPluginInfo() {
    static PptPluginInfo info;
    memset(&info, 0, sizeof(info));
    info.abi = PPT_PLUGIN_ABI_VERSION;
    wcscpy(info.name, L"水印");
    wcscpy(info.author, L"pptx-studio");
    info.version = 1;
    wcscpy(info.desc, L"为导出的 HTML 每页加右下角水印；--cmd watermark 文字 可改");
    return &info;
}

PPT_PLUGIN_API int InitPlugin(const PptHostIf* host) {
    g_host = host;
    if (g_host && g_host->log) g_host->log(L"水印插件已初始化");
    return 0;
}

PPT_PLUGIN_API void ShutdownPlugin() {}

PPT_PLUGIN_API int HookHtmlHead(const char* in_meta, char* out, int out_cap) {
    const char* css =
        "<style>.wm-layer{position:absolute;right:14px;bottom:10px;"
        "font-size:12px;color:rgba(128,128,128,.55);pointer-events:none;"
        "letter-spacing:1px;z-index:50;}</style>\n";
    int n = (int)strlen(css);
    if (n > out_cap - 1) return 0;
    memcpy(out, css, n); out[n] = 0;
    return n;
}

PPT_PLUGIN_API int HookHtmlSlide(const char* in_meta, char* out, int out_cap) {
    char buf[512];
    snprintf(buf, sizeof(buf), "<div class=\"wm-layer\">%s</div>\n", g_text);
    int n = (int)strlen(buf);
    if (n > out_cap - 1) return 0;
    memcpy(out, buf, n); out[n] = 0;
    return n;
}

PPT_PLUGIN_API int HookCommand(const char* cmd, const char* arg, char* out, int out_cap) {
    if (strcmp(cmd, "watermark") != 0) return 0;
    if (arg && *arg) { strncpy(g_text, arg, sizeof(g_text) - 1); g_text[sizeof(g_text) - 1] = 0; }
    int n = snprintf(out, out_cap - 1, "水印文字已设为: %s\n", g_text);
    if (n < 0) n = 0;
    if (n > out_cap - 1) n = out_cap - 1;
    out[n] = 0;
    return n;
}