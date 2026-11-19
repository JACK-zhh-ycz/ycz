// ============================================================
//  示例插件 2：进度条（progress）
//  在 HTML 底部注入随翻页移动的进度条
// ============================================================

#include "plugin_api.h"
#include <cstring>
#include <cstdio>

static const PptHostIf* g_host = nullptr;

PPT_PLUGIN_API const PptPluginInfo* GetPluginInfo() {
    static PptPluginInfo info;
    memset(&info, 0, sizeof(info));
    info.abi = PPT_PLUGIN_ABI_VERSION;
    wcscpy(info.name, L"进度条");
    wcscpy(info.author, L"pptx-studio");
    info.version = 1;
    wcscpy(info.desc, L"放映时底部显示翻页进度条");
    return &info;
}

PPT_PLUGIN_API int InitPlugin(const PptHostIf* host) { g_host = host; return 0; }
PPT_PLUGIN_API void ShutdownPlugin() {}

PPT_PLUGIN_API int HookHtmlHead(const char* in_meta, char* out, int out_cap) {
    const char* css =
        "<style>#pgbar{position:fixed;left:0;bottom:0;height:4px;background:#5b9bd5;"
        "width:0;transition:width .25s ease;z-index:999}"
        "#pgbar span{position:absolute;right:6px;top:-18px;font-size:11px;color:#8ab4e8}</style>\n";
    int n = (int)strlen(css);
    if (n > out_cap - 1) return 0;
    memcpy(out, css, n);
    out[n] = 0;
    return n;
}

PPT_PLUGIN_API int HookHtmlSlide(const char* in_meta, char* out, int out_cap) {
    // 只往最后一页里放一次全量进度条脚本（简化：注入到每页但用 id 去重）
    const char* js =
        "<script>function pgbarFix(){var t=document.querySelectorAll('.slide').length;"
        "if(!document.getElementById('pgbar')){var d=document.createElement('div');"
        "d.id='pgbar';d.innerHTML='<span></span>';document.body.appendChild(d);}"
        "var el=document.getElementById('pgbar');el.style.width=((cur+1)/t*100)+'%';"
        "el.querySelector('span').textContent=(cur+1)+'/'+t;}"
        "var _o=window.show;window.show=function(i){if(_o)_o(i);pgbarFix();};</script>\n";
    int n = (int)strlen(js);
    if (n > out_cap - 1) return 0;
    memcpy(out, js, n);
    out[n] = 0;
    return n;
}