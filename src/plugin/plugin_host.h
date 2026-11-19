#pragma once
// ============================================================
//  插件宿主：扫描 plugins/ 目录、加载 DLL、广播 Hook
// ============================================================

#include <string>
#include <vector>
#include "plugin_api.h"

namespace ppt {

class PluginHost {
public:
    struct Loaded {
        HMODULE mod;
        std::string path;
        std::string name;                 // UTF-8
        std::string desc;
        // Hook 指针（可空）
        int (*fn_html_head)(const char*, char*, int);
        int (*fn_html_slide)(const char*, char*, int);
        int (*fn_transition)(const char*, char*, int);
        int (*fn_command)(const char*, const char*, char*, int);
        void (*fn_shutdown)();
        int  (*fn_init)(const PptHostIf*);
    };

    static PluginHost& instance();

    // 加载目录内所有 *.dll；返回成功数量
    int load_dir(const std::string& dir_utf8);
    void unload_all();

    // ---- 广播 ----
    void html_head(const std::string& meta, std::string& out_html_fragment);
    void html_slide(const std::string& meta, std::string& out_html_fragment);
    std::string transition_override(const std::string& cur);
    // 返回 true = 某插件处理了命令，result 为其输出
    bool run_command(const std::string& cmd, const std::string& arg, std::string& result);

    const std::vector<Loaded>& loaded() const { return mods_; }
    bool empty() const { return mods_.empty(); }

private:
    PluginHost() {}
    std::vector<Loaded> mods_;
};

// 宿主能力表实现（绑定当前 Presentation，由主程序设置）
void plugin_host_bind(void* presentation_ptr);

} // namespace ppt