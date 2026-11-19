#pragma once
// ============================================================
//  GDI+ 渲染抽象层：Presentation -> GDI+ 绘制
//  与 html_render 共用同一份模型与 RenderTheme 解析结果，
//  保证编辑器画布所见即所得与 HTML 导出一致。
// ============================================================

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include <string>
#include <map>

#include "../core/model.h"

namespace ppt {

// 画布坐标换算上下文
struct DrawCtx {
    long long sw = 12192000, sh = 6858000;      // 幻灯片 EMU
    double cw = 960, ch = 540;                  // 目标画布像素
    const RenderTheme* rt = nullptr;
    int slide_idx = 0, slide_total = 1;
    std::string date_str;

    double kx() const { return sw ? cw / (double)sw : 1.0; }
    double ky() const { return sh ? ch / (double)sh : 1.0; }
    double pxW(long long e) const { return e * kx(); }
    double pxH(long long e) const { return e * ky(); }
    double pt2px(double pt) const { return sh ? pt * 12700.0 / (double)sh * ch : pt * 1.333; }
};

Gdiplus::Color argb_of(const ARGB& c);

// 媒体图片缓存：从 Presentation::media 的字节流建 GDI+ Image
class ImageCache {
public:
    explicit ImageCache(const Presentation& p) : pres_(p) {}
    ~ImageCache();
    Gdiplus::Image* get(const std::string& part);       // 失败返回 nullptr
    ImageCache(const ImageCache&) = delete;
    ImageCache& operator=(const ImageCache&) = delete;
private:
    struct Ent { IStream* st; Gdiplus::Image* img; bool ok; };
    const Presentation& pres_;
    std::map<std::string, Ent> map_;
};

// 形状解析后的画布像素包围盒（含占位符几何继承）
struct ResolvedBox {
    double x = 0, y = 0, w = 0, h = 0;
    double rot = 0;
    bool valid = false;
};
ResolvedBox resolve_box(const Shape& s, const RenderTheme& rt, const DrawCtx& ctx);

// 绘制一整页：背景色/填充 + 母版版式装饰层 + 本页形状
void draw_slide(Gdiplus::Graphics& g, const Presentation& pres, const Slide& sl,
                DrawCtx& ctx, ImageCache& imgs, bool with_bg_layer = true);

// 命中测试：返回画布坐标 (px,py) 处最上层形状（自 z 序顶向下），未命中返回 nullptr
ShapePtr hit_test_slide(const Presentation& pres, const Slide& sl,
                        double px, double py, const RenderTheme& rt,
                        double cw, double ch);

// 把当前页渲染为 PNG 文件（验证/缩略图用）
bool render_slide_png(const Presentation& pres, size_t slide_idx, int w, int h,
                      const std::string& out_path, std::string& err);

} // namespace ppt
