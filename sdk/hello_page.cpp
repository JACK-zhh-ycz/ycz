// ============================================================
//  示例插件：页码
//  作用：导出的 HTML 放映里，每页底部中间显示 "当前页 / 总页数"
//  编译：g++ -std=c++17 -shared -DPPT_PLUGIN_EXPORTS hello_page.cpp -o hello_page.dll
//  安装：把编出来的 hello_page.dll 扔进 ycz-便携版\build\plugins\ 即可，重启程序生效
// ============================================================
#include "plugin_api.h"
#include <cstring>
#include <cstdio>

static const PptHostIf* g_host = nullptr;
static int g_total = 0;

// 必须实现：插件自我介绍
PPT_PLUGIN_API const PptPluginInfo* GetPluginInfo() {
    static PptPluginInfo info;
    memset(&info, 0, sizeof(info));
    info.abi = PPT_PLUGIN_ABI_VERSION;          // 必须填 1
    wcscpy(info.name, L"页码示例");
    wcscpy(info.author, L"自己写的");
    info.version = 1;
    wcscpy(info.desc, L"演示：给导出的 HTML 每页底部加页码；--cmd page 3 可改总页数");
    return &info;
}

// 必须实现：拿到宿主能力表
PPT_PLUGIN_API int InitPlugin(const PptHostIf* host) {
    g_host = host;
    if (g_host && g_host->slide_count) g_total = g_host->slide_count();
    if (g_host && g_host->log) g_host->log(L"页码示例插件已初始化");
    return 0;
}

PPT_PLUGIN_API void ShutdownPlugin() {}

// 可选：往 HTML <head> 里塞 CSS / JS
PPT_PLUGIN_API int HookHtmlHead(const char* in_meta, char* out, int out_cap) {
    const char* css =
        "<style>.pg-layer{position:absolute;left:0;right:0;bottom:12px;text-align:center;"
        "font-size:13px;color:#8a8a8a;letter-spacing:1px;pointer-events:none;z-index:60;}"
        "</style>\n";
    int n = (int)strlen(css);
    if (n > out_cap - 1) return 0;
    memcpy(out, css, n); out[n] = 0;
    return n;
}

// 可选：每页渲染完追加 HTML 片段，in_meta = "idx=0&total=7&title=..."
PPT_PLUGIN_API int HookHtmlSlide(const char* in_meta, char* out, int out_cap) {
    int idx = 0, total = g_total ? g_total : 1;
    const char* p = in_meta ? strstr(in_meta, "idx=") : nullptr;
    if (p) idx = atoi(p + 4);
    p = in_meta ? strstr(in_meta, "total=") : nullptr;
    if (p) total = atoi(p + 6);
    if (total <= 0) total = 1;
    char buf[256];
    snprintf(buf, sizeof(buf),
             "<div class=\"pg-layer\">%d / %d</div>\n", idx + 1, total);
    int n = (int)strlen(buf);
    if (n > out_cap - 1) return 0;
    memcpy(out, buf, n); out[n] = 0;
    return n;
}

// 可选：自定义命令（主程序 --cmd page 5，或插件面板里执行）
PPT_PLUGIN_API int HookCommand(const char* cmd, const char* arg, char* out, int out_cap) {
    if (strcmp(cmd, "page") != 0) return 0;
    if (arg && *arg) g_total = atoi(arg);
    int n = snprintf(out, out_cap - 1, "页码示例：总页数已设为 %d\n", g_total);
    if (n < 0) n = 0;
    if (n > out_cap - 1) n = out_cap - 1;
    out[n] = 0;
    return n;
}
