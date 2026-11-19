#pragma once
// ============================================================
//  PPT 核心数据模型（场景图 / Scene Graph）
//  - 结构对齐 OOML(PresentationML)，但做了对渲染友好的整理
//  - 所有几何量使用 EMU（与 pptx 内部一致，1 pt = 12700 EMU）
//  - 单位/颜色工具见 units.h
// ============================================================

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <cstdint>

#include "units.h"

namespace ppt {

// ---------------- 形状类型 ----------------
enum class ShapeKind {
    Rect, RoundedRect, Ellipse, Triangle, RightTriangle, Parallelogram, Trapezoid,
    Diamond, Pentagon, Hexagon, Chevron, Arrow, Star, Line, Connector, Freeform,
    Group, Picture, ContentPlaceHolder, PlaceHolder, Table, Chart, Media, OLE, Note, Unknown
};

// ---------------- 占位符类型 ----------------
enum class PhType {
    None, Title, Body, CenterTitle, SubTitle, SlideTitle,
    Object, Chart, Table, Pic, Media, ClipArt,
    Date, Footer, SlideNumber, VertTitle, HorzTitle, Header, Other
};

// ---------------- 填充 ----------------
enum class FillKind { None, Solid, Gradient, Pattern, Blip, Group, NoFill, Inherit };
enum class GradKind { Linear, Radial, Path };
enum class FillMode { Rectangle, Stretch, Tile, Center, Frame };

struct GradientStop { double pos = 0; ARGB color; double alpha = 100; std::string scheme; };

struct Fill {
    FillKind kind = FillKind::Inherit;
    ARGB solid;
    std::string solid_scheme;                          // solidFill 来自 schemeClr 时的名称

    GradKind grad_kind = GradKind::Linear;
    double ang = 0;                                  // 线性渐变角度：0 = 向上（OOXML 约定）
    std::vector<GradientStop> stops;

    std::string blip_part;                           // 媒体部件名，如 "ppt/media/image1.png"
    double blip_alpha = 100;
    FillMode fill_mode = FillMode::Rectangle;
    int tile_ratio = 0;                              // 0 表示不缩放

    std::string prst_pat;                            // "lgUp"/"pct50" 等
    ARGB pat_bg, pat_fg;

    bool is_inherit() const { return kind == FillKind::Inherit; }
    bool is_none() const { return kind == FillKind::None || kind == FillKind::NoFill; }
};

// ---------------- 线条 ----------------
enum class LineCap { Flat, Square, Round, Triangle };
enum class DashType { Solid, SquareDot, RoundDot, Dash, DashDot, LongDash,
                      LongDashDot, LongDashDotDot };

struct LineArrow { std::string prst = "none"; double w = 2; double l = 2; };

struct Line {
    bool explicit_none = false;
    bool inherit = true;
    double w = 0;                                    // EMU
    ARGB color;
    std::string color_scheme;                         // 主题色引用名（accent1 等）
    DashType dash = DashType::Solid;
    LineCap cap = LineCap::Flat;
    int join = 2;                                    // 0 round 1 bevel 2 miter
    LineArrow head, tail;
    bool has_head = false, has_tail = false;

    bool is_none() const { return explicit_none; }
};

// ---------------- 效果 ----------------
struct Shadow {
    bool on = false;
    double blur_rad = 0;                             // EMU
    double dist = 0;                                 // EMU
    double dir = 0;                                  // 角度，0 = 右，顺时针（OOXML 约定）
    double scale_h = 100, scale_v = 100;
    ARGB color;
    std::string color_scheme;                         // 主题色引用名（accent1 等）
    double alpha = 40;
};

struct Reflection {
    bool on = false;
    double start_pos = 0, end_pos = 35;
    double start_alpha = 30, end_alpha = 0;
    double dist = 0, dir = 0;
    double blur = 0;
    double size_pct = 100;
    int tilt = 0;
};

struct Effect {
    double blur_rad = 0;
    bool soft_edge = false; double soft_rad = 0;
    bool glow = false; double glow_rad = 0; ARGB glow_color; double glow_alpha = 100;
    std::string glow_scheme;                         // 发光颜色的主题引用
    bool bevel = false; double bevel_w = 0, bevel_h = 0; std::string bevel_prst = "circle";
    Shadow shadow;
    Reflection reflect;
    bool any() const {
        return blur_rad > 0 || soft_edge || glow || bevel || shadow.on || reflect.on;
    }
};

// ---------------- 3D ----------------
struct Scene3D {
    bool on = false;
    std::string camera = "orthographicFront";
    std::string light_rig = "threePt";
    int bg = 0;
    double fov = 55, dist = 9600000;
};
struct Shape3D {
    bool on = false;
    std::string prst_material = "warmMatte";
    std::string bevel;
    double extrusion = 0;
    ARGB extrusion_color;
    ARGB face_color;
};

// ---------------- 文本 ----------------
enum class AlignL { Left, Center, Right, Justify, Distributed, JustifyLow };
enum class AutoFit { None, Normal, Shape, NoShrink };
enum class VAlign { Top, Center, Bottom };
enum class BulletKind { None, Char, AutoNum, Blip };

struct ParaProps {
    bool lvl_set = false; int lvl = 0;
    bool algn_set = false; AlignL algn = AlignL::Left;
    bool marL_set = false; long long marL = 0, marR = 0, indent = 0;
    bool spcbef_set = false, spcaft_set = false; long long spcbef = 0, spcaft = 0;
    double lnspc_pct = 100; bool lnspc_set = false; long long lnspc_pts = 0;
    bool rtl = false; bool defTabSz_set = false; long long defTabSz = 0;
};

struct BulletProps {
    BulletKind kind = BulletKind::None;
    std::string char_;                               // "•" 等
    std::string font;                                // Arial / Wingdings
    double size_pct = 100;
    std::string color_scheme;                        // "tx1"/"accent1"...
    ARGB color; bool color_set = false;
    std::string blip_part;
    double marL = -228600, indent = -228600;
    bool autofit_num = false; std::string num_fmt = "1."; std::string num_arabic = "arabicPeriod";
};

struct TabStop { long long pos = 0; int algn = 0; };

struct LineProps {
    bool spc_pct_set = false; double spc_pct = 100;
    bool spc_pts_set = false; long long spc_pts = 0;
};

struct RunProps {
    bool sz_set = false; double sz = 1800;           // 百分之一磅
    bool b_set = false; bool b = false;
    bool i_set = false; bool i = false;
    bool u_set = false; std::string u = "none";
    bool strike = false;
    bool baseline_set = false; int baseline = 0;
    double spc = 0; bool spc_set = false;             // 字距（pt）
    ARGB color; bool color_set = false;
    std::string color_scheme;                        // 主题色引用
    std::string latin, ea, cs;
    std::string hyperlink_part;                      // 外部 URL
    std::string action_part;                         // 跳转到某张幻灯片（slide part name）
    bool hyperlink_is_ext = false;
    std::string link_target;                         // URL 或 "#slide-N"

    bool has_link() const { return !link_target.empty(); }
};

struct TextRun {
    std::string text;                                // UTF-8
    RunProps props;
};

struct TextParagraph {
    ParaProps props;
    BulletProps bullet;
    std::vector<TabStop> tabs;
    std::vector<TextRun> runs;
    bool endParaRPr_set = false;
    RunProps end_props;
    bool para_default_set = false;                    // <a:pPr><a:defRPr>
    RunProps para_default;

    std::string plain_text() const;
    void set_plain_text(const std::string& t);        // 单 run 替换，保留首 run 样式
    const RunProps* first_props() const;
};

struct TextBody {
    std::vector<TextParagraph> paras;
    AutoFit autofit = AutoFit::None;
    VAlign anchor = VAlign::Top;
    bool anchor_set = false;
    long long lIns = 91440, tIns = 45720, rIns = 91440, bIns = 45720;
    bool vert = false;                                // 竖排
    std::string vert_dir;                             // "horz"/"eaVert"/"vert270"...
    bool wrap = true;
    bool rtl = false;
    bool normAutofit_font_scaled = false;
    double fontScale = 100;                           // 百分比（pptx 存 100000 基准）
    double lnSpcReduction = 0;

    std::string all_text() const;                     // 段落间 \n
    void set_all_text(const std::string& t);          // 按 \n 拆段
    bool empty() const { return paras.empty(); }
    void ensure_first();
    TextParagraph& add_para();
    TextParagraph& insert_para(size_t idx);
    void remove_para(size_t idx);
    void move_para(size_t from, size_t to);
    TextRun& add_run(TextParagraph& p);
    void clear() { paras.clear(); }

    // 归一化：给没有显式属性的 run 继承段落/默认样式
    void normalize_defaults(double default_sz_pt, const ARGB& default_color,
                            const std::string& default_font);
};

// ---------------- 表格 ----------------
struct TableCell {
    std::map<std::string, std::string> props_raw;     // 极少数字段兜底
    Fill fill;
    std::vector<Line> borders;                        // 顺序 l r t b 的显式定义
    bool has_border[4] = {false, false, false, false};
    ARGB border_color[4];
    double border_w[4] = {0, 0, 0, 0};
    DashType border_dash[4] = {DashType::Solid, DashType::Solid, DashType::Solid, DashType::Solid};
    TextBody text;
    VAlign anchor = VAlign::Top; bool anchor_set = false;
    bool merge_h = false, merge_v = false;
    long long span_h = 1, span_v = 1;
    long long marL = 0, marR = 0, marT = 0, marB = 0;
    bool mar_set = false;
    int dir = 0;
};

struct Table {
    std::vector<long long> cols;                      // EMU
    std::vector<long long> rows;                      // EMU
    std::vector<std::vector<TableCell>> cells;        // [r][c]
    bool first_row = true, first_col = false, band_row = false, band_col = false;
    std::string style_id;                             // tableStyleId GUID
    std::map<std::string, std::string> table_props_raw;
    // 表格样式（来自 tableStyles.xml 或显式覆盖）
    std::map<std::string, Fill> style_fills;          // key: "whole"/"firstRow"/"band1"...

    void resize(size_t nr, size_t nc);
    size_t nrows() const { return rows.size(); }
    size_t ncols() const { return cols.size(); }
    long long total_w() const;
    long long total_h() const;
};

// ---------------- 形状 ----------------
struct Shape {
    long long id = 0;
    std::string name = "形状";
    ShapeKind kind = ShapeKind::Rect;
    std::string prst_geom = "rect";                   // OOXML 预设几何名
    bool is_place_holder = false;
    std::string ph_idx;                               // 原始 idx 字符串
    PhType ph_type = PhType::None;
    std::string ph_type_raw;

    long long x = 0, y = 0, cx = 0, cy = 0;           // EMU（group 内为子坐标空间）
    double rot = 0;                                   // 度，顺时针
    bool flip_h = false, flip_v = false;
    bool hidden = false;
    std::string alt_text;

    Fill fill;
    Line line;
    Effect fx;
    Scene3D scene3d;
    Shape3D shape3d;
    std::string style_ref;                            // <p:style> 引用（accent1 等）
    std::string style_ref_ln;
    std::string style_ref_fill;
    std::string style_ref_fx;
    double effect_dag_depth = 0;
    bool no_group = false;

    TextBody text;
    bool has_text = false;
    std::shared_ptr<Table> table;
    bool tx_rot = false;                              // 文字方向旋转

    std::vector<std::shared_ptr<Shape>> children;     // group

    std::string pic_part;                             // 图片部件名
    std::string pic_ext = "png";
    long long pic_crop_l = 0, pic_crop_t = 0, pic_crop_r = 0, pic_crop_b = 0;
    std::string pic_prst = "";
    bool pic_fill_rect = false;
    double src_rect_l = 0, src_rect_t = 0, src_rect_r = 0, src_rect_b = 0;   // 0..1
    int src_tile_ratio = 0;
    std::string chart_part;                            // ppt/charts/chartN.xml
    std::string media_part;                            // 音视频部件名
    std::string media_kind;                           // "video"/"audio"
    std::string post_media_part;                       // 封面帧
    bool media_auto_play = false; bool media_loop = false; bool media_hide = false;
    std::string hyperlink;                            // 形状级点击跳转 URL
    std::string hyperlink_slide;                      // 形状级跳转到幻灯片 part
    std::string action_callout;                       // customShow / hlinkClick 其它信息

    // 自由路径（custGeom）
    std::vector<std::string> path_cmds;
    std::string svg_path;                             // custGeom 转出的归一化 0..1 SVG path

    // 预设几何 adjust 值（a:avLst/gd，键 "adj"/"adj1"...，值已除以 100000）
    std::map<std::string, double> prst_adj;

    // group 子坐标空间（chOff/chExt）
    long long ch_off_x = 0, ch_off_y = 0, ch_ext_cx = 0, ch_ext_cy = 0;

    // p:style 引用（fillRef/lnRef/effectRef 的 idx；scheme 名存 style_ref_*）
    int fill_ref_idx = 0, ln_ref_idx = 0, eff_ref_idx = 0;

    // 原始 XML 兜底（保真用）
    bool raw_preserve = false;                        // true 时按 raw_xml 原样输出
    std::string raw_xml;

    // ---- 编辑标记（步骤C：DOM 手术写回时按位更新对应节点）----
    bool ed_geom = false;    // xfrm 被改（move/resize/rot）
    bool ed_text = false;    // txBody 被改（text/fontsize）
    bool ed_fill = false;    // 填充被改
    bool ed_line = false;    // 边框线被改
    bool ed_table = false;   // 表格单元格文字被改（需要整段重写 a:tbl）

    bool is_group() const { return kind == ShapeKind::Group; }
    bool is_pic() const { return kind == ShapeKind::Picture; }
    bool is_table() const { return kind == ShapeKind::Table || (bool)table; }
    bool is_chart() const { return kind == ShapeKind::Chart || !chart_part.empty(); }
    bool is_media() const { return !media_part.empty(); }
    bool has_click_action() const { return !hyperlink.empty() || !hyperlink_slide.empty(); }

    std::string kind_label() const;                   // 中文可读名
    double area() const { return (double)cx * (double)cy; }
};

using ShapePtr = std::shared_ptr<Shape>;

// ---------------- 动画 / 切换 ----------------
enum class AnimKind { Fade, FlyIn, Zoom, Wipe, Dissolve, Split, RandomBars,
                      Appear, Float, RiseUp, Swivel, Bounce, Wheel, Other };
enum class AnimEffect { Entrance, Emphasis, Exit, MotionPath };
enum class TrgKind { OnClick, WithPrevious, AfterPrevious };

struct AnimNode {
    AnimEffect effect = AnimEffect::Entrance;
    AnimKind kind = AnimKind::Fade;
    std::string raw_prst;                             // pptx presetID / presetSubtype 记录
    long long target_id = 0;                          // sp id
    TrgKind trg = TrgKind::OnClick;
    int seq = 0;                                      // 主序列编号
    long long delay_ms = 0;
    long long dur_ms = 500;
    std::string from, to;                             // 触发参数（文本串即可）
    int dir = 0;                                      // 1=up 2=down 3=left 4=right
    double rot = 0;
    std::string fill;                                 // "freeze"/"remove"
    int auto_reverse = 0;
    int repeat = 0;
    bool has_dim_color = false; ARGB dim_color;
    std::string motion_path;                          // "M 0 0 L 100 0" 归一化路径
};

struct Transition {
    std::string adv_click = "1";                      // "1"/"0"
    std::string adv_time = "0";
    long long dur_ms = 0;
    std::string kind = "none";                        // fade/push/wipe/split/...
    int dir = 0;
    bool thru_blk = false;
    long long auto_adv_ms = 0;                        // 0 = 不自动
    int sound = 0;                                     // 0=none
    std::string sound_part;                            // 音效媒体部件
    bool raw_fallback = false;
    std::vector<AnimNode> seq;                         // 本页动画
};

// ---------------- 版式 / 母版 / 主题 ----------------
struct ThemeColors {
    ARGB dk1, lt1, dk2, lt2;
    ARGB accent[6];
    ARGB hlink, folhlink;
    std::string dk1_name = "tx1", lt1_name = "bg1", dk2_name = "tx2", lt2_name = "bg2";
    std::string scheme_name = "office";
    // 名称 "tx1"/"bg1"/"accent1"... -> 颜色
    bool resolve(const std::string& name, ARGB& out) const;
};

struct ThemeFonts {
    std::string major_latin = "Calibri Light", minor_latin = "Calibri";
    std::string major_ea, minor_ea, major_cs, minor_cs;
};

struct ThemeEffect {
    std::vector<Shadow> shadows;                      // 4 档
    std::vector<Reflection> reflections;              // 4 档
    std::vector<std::pair<std::string, std::vector<std::pair<double, ARGB>>>> gradients;  // 3 档
};

struct Theme {
    std::string name = "Office 主题";
    ThemeColors colors;
    ThemeFonts fonts;
    ThemeEffect effects;
    std::vector<std::pair<std::string, Fill>> fmt_scheme_fill;   // 背景填充样式 4 档
};

struct SlideLayoutDef {
    long long id = 0;                                 // 运行时 id
    std::string name = "空白";
    std::string part;                                 // ppt/slideLayouts/slideLayoutN.xml
    std::string master_part;                          // 所属母版部件（由 rels 解析）
    std::string type = "obj";                         // "blank"/"titleOnly"/...
    std::vector<ShapePtr> shapes;
    std::vector<std::pair<std::string, std::string>> bg_props;   // raw 兜底
    bool show_master_bg = true;
    Fill bg_fill;
};

struct SlideMasterDef {
    long long id = 0;
    std::string name = "Office 主题母版";
    std::string part;
    std::vector<ShapePtr> shapes;
    std::vector<std::pair<long long, std::string>> layout_refs;  // id -> part
    Fill bg_fill;
    bool has_bg = false;
    std::string date_fmt, footer_txt;
    long long num_pos_x = 0, num_pos_y = 0, num_w = 0, num_h = 0;
    bool show_num = true;
};

// ---------------- 幻灯片 ----------------
struct SlideBg {
    bool from_master = true;
    Fill fill;
    bool override_master = false;                     // masterClrMapOvr
    std::string clr_map;
};

struct Slide {
    long long id = 0;
    std::string name = "幻灯片";                       // 备注用名称
    std::string part;                                 // ppt/slides/slideN.xml
    std::vector<ShapePtr> shapes;
    std::string notes;                                // 演讲者备注正文
    std::string notes_part;
    std::string layout_part;
    long long layout_id = 0;
    Transition trans;
    SlideBg bg;
    std::string show_master_sp = "1";                 // 母版形状是否显示
    // 隐藏页 / 节
    bool hidden = false;
    std::string section;

    // ---- 回写保真（步骤C）----
    std::string raw_xml;                  // 原始 slide XML 全文；空 = 新建页
    bool touched = false;                 // 本页被编辑过，需要手术
    std::vector<long long> removed_ids;   // 被删除的顶层形状 id，手术时从 raw 中移除

    ShapePtr find_by_id(long long sid) const;
    ShapePtr find_deep(long long sid) const;
    ShapePtr find_by_name(const std::string& nm) const;
    size_t next_shape_id() const;
    void add_shape(ShapePtr sp) { shapes.push_back(sp); }
    bool remove_shape(long long sid);
    void normalize_ids(long long start = 2);
    void recompute_max_id();
    long long max_id() const;
    ShapePtr title_shape() const;
    std::string title_text() const;
};

using SlidePtr = std::shared_ptr<Slide>;

// ---------------- 演示文稿 ----------------
class Presentation {
public:
    long long slide_w = 12192000;                     // EMU (13.333in)
    long long slide_h = 6858000;                      // EMU (7.5in)
    std::string aspect = "16:9";
    std::string title, author, last_modified_by, subject, comments;
    std::string category, keywords = "";
    std::string company;
    std::string created_utc, modified_utc;            // ISO 字符串
    long long revision = 1;

    std::vector<SlidePtr> slides;
    std::vector<SlideLayoutDef> layouts;
    std::vector<SlideMasterDef> masters;
    std::vector<Theme> themes;

    Theme* theme(size_t idx = 0);
    const Theme* theme(size_t idx = 0) const;
    SlideLayoutDef* layout_by_part(const std::string& part);
    const SlideLayoutDef* layout_by_part(const std::string& part) const;
    SlideLayoutDef* layout_by_id(long long id);
    SlideMasterDef* master_by_part(const std::string& part);
    const SlideMasterDef* master_by_part(const std::string& part) const;
    long long add_layout_def(SlideLayoutDef d);
    long long add_master_def(SlideMasterDef d);

    // 媒体与原始部件（pptx 往返保真）
    std::map<std::string, std::vector<uint8_t>> media;         // "ppt/media/image1.png" -> bytes
    std::map<std::string, std::string> raw_parts;              // 未识别部件原样保留
    // 步骤C：除 ppt/media/* 外全部 zip 条目的原文快照（xml/rels/二进制内嵌件）
    // 未编辑的部件写出时从这里逐字节回写
    std::map<std::string, std::string> orig_parts;
    struct Rel {
        std::string id;                                 // rId3
        std::string type;                               // 完整关系类型 URI
        std::string target;                             // ../media/image1.png 或 URL
        std::string mode;                               // "External" / ""
    };
    // "ppt/slides/_rels/slide1.xml.rels" -> 关系列表
    std::map<std::string, std::vector<Rel>> rels;
    std::map<std::string, std::map<std::string, std::string>> content_types_defaults;
    std::map<std::string, std::map<std::string, std::string>> content_types_overrides;
    std::vector<std::string> entry_order;                     // 原 zip 顺序
    std::string src_path;
    std::string package_kind = "pptx";                        // "pptx" / "html" / "native"
    bool dirty = false;
    std::string last_error;
    std::string notes_slide_master_part;

    // ---- 幻灯片操作 ----
    SlidePtr add_slide(const SlidePtr& after = nullptr);
    SlidePtr duplicate_slide(size_t idx);
    bool remove_slide(size_t idx);
    bool move_slide(size_t from, size_t to);
    void swap_slides(size_t a, size_t b);
    SlidePtr slide(size_t i);
    size_t index_of(const std::string& part) const;
    size_t index_of(const SlidePtr& s) const;
    std::string rel_part_for(const std::string& part) const;   // part -> _rels 路径
    std::string resolve_target(const std::string& base_part, const std::string& rels_key,
                               const std::string& target) const;
    std::string find_rel(const std::string& part, const std::string& rid, std::string& mode) const;
    std::string add_media(const std::string& ext, const std::vector<uint8_t>& bytes,
                          std::string& part_out);
    std::string add_rel(const std::string& part, const std::string& type_tail,
                        const std::string& target, bool external, const std::string& rid_hint = "");
    void drop_rel(const std::string& part, const std::string& rid);
    long long max_shape_id() const;
    void ensure_ids_unique();

    // ---- 尺寸 ----
    double aspect_ratio() const { return slide_h ? (double)slide_w / (double)slide_h : 1.777; }
    void set_size_by_aspect(const std::string& ratio, long long w_emu_default = 12192000);
    std::string size_label() const;                     // "13.33 x 7.50 英寸 (16:9)"

    // ---- 文本/统计 ----
    size_t shape_count() const;
    size_t word_count() const;

    Presentation();
};

// 深拷贝一个形状（含 table / children）
ShapePtr clone_shape(const ShapePtr& src, long long new_id = 0);

// ---------------- 渲染解析（占位符继承 / 主题色） ----------------
struct ResolvedRun {
    double sz_pt = 18;
    bool b = false, i = false, u = false, strike = false;
    ARGB color;
    std::string font;                                 // 拉丁字体
    std::string font_ea;                              // 东亚字体
    std::string link;
};

struct RenderTheme {
    const Theme* theme = nullptr;
    const SlideLayoutDef* layout = nullptr;
    const SlideMasterDef* master = nullptr;
    const Presentation* pres = nullptr;
    const Slide* slide = nullptr;

    ARGB scheme_color(const std::string& name, const ARGB& def) const;
    std::string font_latin(bool major) const;
    std::string font_ea(bool major) const;
    // 解析一个形状的最终填充（含 style/theme/占位继承）
    Fill resolve_fill(const Shape& s) const;
    Line resolve_line(const Shape& s) const;
    // 解析效果（阴影/发光颜色引用落地）
    Effect resolve_effects(const Shape& s) const;
    // 占位符几何继承
    void resolve_geometry(Shape& s) const;
    // 只读版本：返回继承到的几何，成功返回 true
    bool placeholder_geometry(const Shape& s, long long& x, long long& y,
                              long long& cx, long long& cy) const;
    // 段落/文本运行默认继承
    ResolvedRun resolve_run(const Shape& s, const TextParagraph& p, const TextRun& r) const;
    ARGB background_of(const Slide& sl, bool with_master = true) const;
    Fill background_fill(const Slide& sl) const;
};

RenderTheme make_render_theme(const Presentation& p, const Slide& s);

} // namespace ppt
