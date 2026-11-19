#include "plugin_host.h"
#include "../base/strutil.h"
#include "../core/model.h"

#include <cstdio>

namespace ppt {

// ---------------- 当前绑定的演示文稿 ----------------
static Presentation* g_pres = nullptr;

void plugin_host_bind(void* presentation_ptr) {
    g_pres = (Presentation*)presentation_ptr;
}

// ---------------- 宿主能力表（静态缓冲） ----------------
static wchar_t g_title_buf[512];

static void host_log(const wchar_t* msg) {
    if (!msg) return;
    std::string s = utf16_to_utf8(msg);
    console_print("[插件] " + s);
}

static int host_slide_count() {
    return g_pres ? (int)g_pres->slides.size() : 0;
}

static const wchar_t* host_slide_title(int idx) {
    if (!g_pres || idx < 0 || idx >= (int)g_pres->slides.size()) return nullptr;
    std::string t = g_pres->slides[idx]->title_text();
    if (t.empty()) t = g_pres->slides[idx]->name;
    std::wstring w = utf8_to_utf16(t);
    size_t n = w.size() < 511 ? w.size() : 511;
    for (size_t i = 0; i < n; ++i) g_title_buf[i] = w[i];
    g_title_buf[n] = 0;
    return g_title_buf;
}

static const wchar_t* host_pres_title() {
    std::string t = g_pres ? g_pres->src_path : std::string();
    std::wstring w = utf8_to_utf16(stem_name(t));
    size_t n = w.size() < 511 ? w.size() : 511;
    for (size_t i = 0; i < n; ++i) g_title_buf[i] = w[i];
    g_title_buf[n] = 0;
    return g_title_buf;
}

static long long host_w_emu() { return g_pres ? g_pres->slide_w : 12192000; }
static long long host_h_emu() { return g_pres ? g_pres->slide_h : 6858000; }

static PptHostIf make_host_if() {
    PptHostIf h;
    h.size = (int)sizeof(PptHostIf);
    h.log = host_log;
    h.slide_count = host_slide_count;
    h.slide_title = host_slide_title;
    h.pres_title = host_pres_title;
    h.slide_w_emu = host_w_emu;
    h.slide_h_emu = host_h_emu;
    return h;
}

// ---------------- 加载 ----------------
PluginHost& PluginHost::instance() {
    static PluginHost inst;
    return inst;
}

int PluginHost::load_dir(const std::string& dir_utf8) {
    std::vector<DirItem> items;
    if (!list_dir(dir_utf8, items)) return 0;
    // 必须是静态的：插件会保存这个指针，栈变量出了函数就失效
    static PptHostIf g_host_if;
    g_host_if = make_host_if();
    int ok = 0;
    for (const DirItem& it : items) {
        if (it.dir) continue;
        if (extension_of(it.name) != ".dll") continue;
        std::string full = join_path(dir_utf8, it.name);
        std::wstring wpath = utf8_to_utf16(full);
        HMODULE mod = LoadLibraryW(wpath.c_str());
        if (!mod) continue;

        auto info_fn = (const PptPluginInfo* (*)())GetProcAddress(mod, "GetPluginInfo");
        if (!info_fn) { FreeLibrary(mod); continue; }
        const PptPluginInfo* info = info_fn();
        if (!info || info->abi != PPT_PLUGIN_ABI_VERSION) { FreeLibrary(mod); continue; }

        auto init_fn = (int (*)(const PptHostIf*))GetProcAddress(mod, "InitPlugin");
        if (!init_fn || init_fn(&g_host_if) != 0) { FreeLibrary(mod); continue; }

        Loaded L;
        L.mod = mod;
        L.path = full;
        L.name = utf16_to_utf8(info->name);
        L.desc = utf16_to_utf8(info->desc);
        L.fn_html_head   = (int (*)(const char*, char*, int))GetProcAddress(mod, "HookHtmlHead");
        L.fn_html_slide  = (int (*)(const char*, char*, int))GetProcAddress(mod, "HookHtmlSlide");
        L.fn_transition  = (int (*)(const char*, char*, int))GetProcAddress(mod, "HookTransition");
        L.fn_command     = (int (*)(const char*, const char*, char*, int))GetProcAddress(mod, "HookCommand");
        L.fn_shutdown    = (void (*)())GetProcAddress(mod, "ShutdownPlugin");
        L.fn_init        = init_fn;
        mods_.push_back(L);
        console_print("已加载插件: " + L.name + " (" + L.desc + ")");
        ++ok;
    }
    return ok;
}

void PluginHost::unload_all() {
    for (Loaded& L : mods_) {
        if (L.fn_shutdown) L.fn_shutdown();
        if (L.mod) FreeLibrary(L.mod);
    }
    mods_.clear();
}

// ---------------- 广播 ----------------
static std::string call_buf(int (*fn)(const char*, char*, int), const std::string& in) {
    if (!fn) return std::string();
    char buf[16384];
    buf[0] = 0;
    int n = fn(in.c_str(), buf, (int)sizeof(buf) - 1);
    if (n <= 0) return std::string();
    if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
    return std::string(buf, n);
}

void PluginHost::html_head(const std::string& meta, std::string& out_fragment) {
    for (Loaded& L : mods_) {
        std::string s = call_buf(L.fn_html_head, meta);
        if (!s.empty()) out_fragment += s;
    }
}

void PluginHost::html_slide(const std::string& meta, std::string& out_fragment) {
    for (Loaded& L : mods_) {
        std::string s = call_buf(L.fn_html_slide, meta);
        if (!s.empty()) out_fragment += s;
    }
}

std::string PluginHost::transition_override(const std::string& cur) {
    std::string best = cur;
    for (Loaded& L : mods_) {
        std::string s = call_buf(L.fn_transition, cur);
        if (!s.empty()) best = s;
    }
    return best;
}

bool PluginHost::run_command(const std::string& cmd, const std::string& arg, std::string& result) {
    bool handled = false;
    for (Loaded& L : mods_) {
        if (!L.fn_command) continue;
        char buf[8192];
        buf[0] = 0;
        std::string packed = cmd + "\n" + arg;
        int n = L.fn_command(cmd.c_str(), arg.c_str(), buf, (int)sizeof(buf) - 1);
        if (n > 0) {
            handled = true;
            result += std::string(buf, n);
        }
    }
    return handled;
}

} // namespace ppt