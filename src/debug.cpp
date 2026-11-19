#include <cstdio>
#include "base/zip.h"
#include "base/strutil.h"
#include "core/model.h"
#include "io/pptx_io.h"

using namespace ppt;

int main() {
    std::string err;
    
    // 直接测试 ZIP 读取
    ZipArchive zip;
    if (!zip.open_file("D:\\Desktop\\code\\Make使用说明.pptx", err)) {
        printf("ZIP FAIL: %s\n", err.c_str());
        return 1;
    }
    printf("ZIP entries: %zu\n", zip.count());
    for (const std::string& nm : zip.names()) {
        if (ends_with(nm, ".rels")) printf("  RELS: %s\n", nm.c_str());
    }

    // 完整读取
    Presentation pres;
    if (!pptx_read("D:\\Desktop\\code\\Make使用说明.pptx", pres, err)) {
        printf("READ FAIL: %s\n", err.c_str());
        return 1;
    }
    printf("Slides: %zu\n", pres.slides.size());
    printf("Rels keys: %zu\n", pres.rels.size());
    for (const auto& kv : pres.rels) {
        printf("  REL '%s' => %zu entries\n", kv.first.c_str(), kv.second.size());
    }
    return 0;
}