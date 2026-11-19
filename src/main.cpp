#include <cstdio>
#include <cstring>
#include <Windows.h>
#include <shellapi.h>

#include "base/strutil.h"
#include "core/model.h"
#include "io/pptx_io.h"
#include "html/html_render.h"
#include "plugin/plugin_host.h"

using namespace ppt;

static void usage() {
    ppt::console_print(
        "pptx-studio  —  PPT 编辑/转换工具（HTML 输出 + DLL 插件）\n\n"
        "用法:\n"
        "  pptx-studio input.pptx output.html       读 pptx，渲染为 HTML 放映文件\n"
        "  pptx-studio input.pptx                   读 pptx，输出结构信息\n"
        "  pptx-studio input.pptx --cmd 命令 [参数] 运行插件命令（如 stats / watermark 文字）\n"
        "  pptx-studio --help                       帮助\n\n"
        "选项:\n"
        "  --no-anim         关闭 HTML 动画\n"
        "  --no-notes        不包含演讲者备注\n"
        "  --size WxH        画布尺寸 (默认 1280x720)\n"
        "  --plugin-dir DIR  指定插件目录 (默认 exe 同级 plugins\\)\n\n"
        "插件:\n"
        "  接口见 src\\plugin\\plugin_api.h；示例见 plugins\\ 目录\n"
    );
}

int main() {
    int argcW = 0;
    LPWSTR* argvW = CommandLineToArgvW(GetCommandLineW(), &argcW);
    if (!argvW || argcW < 1) { usage(); return 1; }

    std::vector<std::string> args;
    for (int i = 0; i < argcW; ++i) args.push_back(utf16_to_utf8(argvW[i]));
    LocalFree(argvW);

    int argc = (int)args.size();
    if (argc < 2 || args[1] == "--help" || args[1] == "-h") {
        usage();
        return 0;
    }

    std::string input_path = args[1];
    std::string output_path;
    std::string plugin_dir;
    std::string cmd_name, cmd_arg;
    ppt::HtmlOptions opt;

    for (int i = 2; i < argc; ++i) {
        std::string a = args[i];
        if (a == "--no-anim") opt.include_anim = false;
        else if (a == "--no-notes") opt.include_notes = false;
        else if (starts_with(a, "--size")) {
            const char* v = a.c_str() + 6;
            if (*v == '=') v++;
            int w = 0, h = 0;
            if (sscanf(v, "%dx%d", &w, &h) == 2) { opt.canvas_w = w; opt.canvas_h = h; }
        } else if (a == "--plugin-dir" && i + 1 < argc) {
            plugin_dir = args[++i];
        } else if (a == "--cmd" && i + 1 < argc) {
            cmd_name = args[++i];
            if (i + 1 < argc && !starts_with(args[i + 1], "--")) cmd_arg = args[++i];
        } else if (ends_with(a, ".html") || ends_with(a, ".htm")) {
            if (output_path.empty()) output_path = a;
        }
    }

    ppt::console_print("正在加载: " + input_path);

    ppt::Presentation pres;
    std::string err;
    if (!ppt::pptx_read(input_path, pres, err)) {
        ppt::console_print("错误: " + err);
        return 1;
    }

    // ---- 插件系统 ----
    ppt::plugin_host_bind(&pres);
    if (plugin_dir.empty()) {
        wchar_t pathbuf[MAX_PATH] = {0};
        GetModuleFileNameW(nullptr, pathbuf, MAX_PATH);
        std::string exedir = dir_name(utf16_to_utf8(pathbuf));
        plugin_dir = join_path(exedir, "plugins");
    }
    int nloaded = ppt::PluginHost::instance().load_dir(plugin_dir);
    if (nloaded > 0)
        ppt::console_print("已加载 " + std::to_string(nloaded) + " 个插件");

    opt.title = ppt::stem_name(input_path);

    // ---- 插件命令模式 ----
    if (!cmd_name.empty()) {
        std::string result;
        if (ppt::PluginHost::instance().run_command(cmd_name, cmd_arg, result)) {
            if (!result.empty()) {
                if (result.back() == '\n') result.pop_back();
                ppt::console_print(result);
            }
        } else {
            ppt::console_print("没有插件处理命令: " + cmd_name);
        }
        ppt::PluginHost::instance().unload_all();
        return 0;
    }

    if (output_path.empty()) {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "解析成功: %s\n"
                 "  幻灯片: %zu 页\n"
                 "  版式:   %zu 个\n"
                 "  母版:   %zu 个\n"
                 "  主题:   %zu 个\n"
                 "  媒体:   %zu 个\n"
                 "  尺寸:   %s\n"
                 "  形状:   %zu 个\n"
                 "  字数:   %zu 词\n",
                 input_path.c_str(),
                 pres.slides.size(),
                 pres.layouts.size(),
                 pres.masters.size(),
                 pres.themes.size(),
                 pres.media.size(),
                 pres.size_label().c_str(),
                 pres.shape_count(),
                 pres.word_count());
        ppt::console_print(buf);
        ppt::PluginHost::instance().unload_all();
        return 0;
    }

    ppt::console_print("正在渲染: " + output_path);
    if (!ppt::render_to_file(output_path, pres, opt, &err)) {
        ppt::console_print("错误: " + err);
        ppt::PluginHost::instance().unload_all();
        return 1;
    }

    ppt::console_print("完成: " + output_path);
    ppt::PluginHost::instance().unload_all();
    return 0;
}