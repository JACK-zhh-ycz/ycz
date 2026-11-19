// 插件：网页/HTML 嵌入 —— 导出 HTML 放映时把本地网页（含图片/图表等）
// 以 iframe 形式加载到指定幻灯片上，位置按页宽页高的百分比给。
//
// 用法（编辑器“插件”窗口里，命令 + 参数）：
//   web                    查看当前嵌入列表
//   web 1 D:\demo\report.html 10 10 80 60
//       第1页，left10% top10% 宽80% 高60%
//   webdel 1               删除第1页的嵌入
//   web clear              清空全部
//
// 只影响导出的 HTML（编辑画布里的“插入网页”另一套：截图成图片）。
#include "../api_shim.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <windows.h>

static const PptHostIf* g_host = nullptr;

struct Embed {
    int slide;                  // 1 基
    std::string url;            // file:///...
    double x, y, w, h;          // 百分比
};
static Embed g_embeds[16];
static int g_count = 0;

PPT_PLUGIN_API const PptPluginInfo* GetPluginInfo() {
    static PptPluginInfo info;
    memset(&info, 0, sizeof(info));
    info.abi = PPT_PLUGIN_ABI_VERSION;
    wcscpy(info.name, L"网页嵌入");
    wcscpy(info.author, L"pptx-studio");
    info.version = 1;
    wcscpy(info.desc, L"把本地 HTML/网页以 iframe 加载到导出的放映页上；命令 web 查看用法");
    return &info;
}

PPT_PLUGIN_API int InitPlugin(const PptHostIf* host) {
    g_host = host;
    if (g_host && g_host->log) g_host->log(L"网页嵌入插件已初始化");
    return 0;
}

PPT_PLUGIN_API void ShutdownPlugin() {}

// UTF-8 百分号编码（中文路径也能用）
static std::string pct_encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (c >= 128 || c == ' ' || c == '#' || c == '%' || c == '?' || c == '&' || c == '+') {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        } else {
            out += (char)c;
        }
    }
    return out;
}

// 本地路径 -> file:/// URL（Python 侧传进来的一般已经是绝对路径）
static std::string to_file_url(const std::string& path) {
    std::string p = path;
    for (char& c : p) if (c == '\\') c = '/';
    if (p.size() > 2 && p[0] == '/' && p[2] == ':') p = p.substr(1);   // /D:/ -> D:/
    if (p.size() > 1 && p[1] == ':') return "file:///" + pct_encode(p);
    return "file:///" + pct_encode(p);                 // 相对路径：按浏览器当前目录解析
}

static int find_embed(int slide) {
    for (int i = 0; i < g_count; ++i) if (g_embeds[i].slide == slide) return i;
    return -1;
}

PPT_PLUGIN_API int HookCommand(const char* cmd, const char* arg, char* out, int out_cap) {
    if (!cmd) return 0;
    if (strcmp(cmd, "web") != 0 && strcmp(cmd, "webdel") != 0) return 0;
    std::string text;

    if (strcmp(cmd, "webdel") == 0) {
        int s = arg ? atoi(arg) : 0;
        int i = find_embed(s);
        if (i < 0) text = "该页没有嵌入\n";
        else {
            for (int k = i; k + 1 < g_count; ++k) g_embeds[k] = g_embeds[k + 1];
            --g_count;
            text = "已删除\n";
        }
    } else if (!arg || !*arg) {
        text = "用法: web 页码 文件.html 左% 上% 宽% 高%\n";
        text += "      webdel 页码    /    web clear\n";
        for (int i = 0; i < g_count; ++i) {
            char b[512];
            snprintf(b, sizeof b, "  第%d页 %s  [%.0f%%,%.0f%% %.0fx%.0f%%]\n", g_embeds[i].slide,
                     g_embeds[i].url.c_str(), g_embeds[i].x, g_embeds[i].y,
                     g_embeds[i].w, g_embeds[i].h);
            text += b;
        }
    } else if (strncmp(arg, "clear", 5) == 0) {
        g_count = 0;
        text = "已清空全部网页嵌入\n";
    } else {
        // 行: 页码 路径 左 上 宽 高（路径不允许带空格）
        char buf[1024];
        strncpy(buf, arg, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
        char* tok[8] = {nullptr};
        int nt = 0;
        char* p = strtok(buf, " \t");
        while (p && nt < 8) { tok[nt++] = p; p = strtok(nullptr, " \t"); }
        if (nt < 6) {
            text = "参数不足：web 页码 文件.html 左% 上% 宽% 高%\n";
        } else {
            int slide = atoi(tok[0]);
            std::string url = to_file_url(tok[1]);
            if (url.empty()) { text = "路径无法解析\n"; }
            else {
                double v[4];
                for (int i = 0; i < 4; ++i) v[i] = atof(tok[2 + i]);
                int idx = find_embed(slide);
                if (idx < 0) {
                    if (g_count >= 16) { text = "最多 16 个嵌入\n"; }
                    else { idx = g_count++; g_embeds[idx].slide = slide; }
                }
                if (idx >= 0) {
                    g_embeds[idx].url = url;
                    g_embeds[idx].x = v[0]; g_embeds[idx].y = v[1];
                    g_embeds[idx].w = v[2]; g_embeds[idx].h = v[3];
                    char b[256];
                    snprintf(b, sizeof b, "第%d页已嵌入: %s\n", slide, url.c_str());
                    text = b;
                }
            }
        }
    }
    int n = (int)text.size();
    if (n > out_cap - 1) n = out_cap - 1;
    memcpy(out, text.data(), n);
    out[n] = 0;
    return n;
}

PPT_PLUGIN_API int HookHtmlHead(const char* in_meta, char* out, int out_cap) {
    const char* css =
        "<style>.pt-webembed{background:#fff;overflow:hidden;border-radius:4px;"
        "box-shadow:0 6px 26px rgba(0,0,0,.35);z-index:800}"
        ".pt-webembed iframe{width:100%;height:100%;border:0;display:block}</style>\n";
    int n = (int)strlen(css);
    if (n > out_cap - 1) return 0;
    memcpy(out, css, n); out[n] = 0;
    return n;
}

// in_meta: idx=3&total=7&title=xxx （idx 从 0 开始，插件对外用 1 基页码）
PPT_PLUGIN_API int HookHtmlSlide(const char* in_meta, char* out, int out_cap) {
    if (!in_meta || g_count == 0) return 0;
    int idx = 0;
    const char* p = strstr(in_meta, "idx=");
    if (p) idx = atoi(p + 4) + 1;          // 转成 1 基，和用户输入一致
    int k = find_embed(idx);
    if (k < 0) return 0;

    const Embed& e = g_embeds[k];
    std::string html =
        "<div class=\"pt-webembed\" style=\"position:absolute;left:" + std::to_string(e.x) +
        "%;top:" + std::to_string(e.y) + "%;width:" + std::to_string(e.w) +
        "%;height:" + std::to_string(e.h) +
        "%\"><iframe src=\"" + e.url + "\" loading=\"eager\" allow=\"fullscreen\"></iframe></div>\n";
    int n = (int)html.size();
    if (n > out_cap - 1) return 0;
    memcpy(out, html.data(), n); out[n] = 0;
    return n;
}
