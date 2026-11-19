#pragma once
// ============================================================
//  pptx-core C ABI：供 Python(ctypes) 等外部界面调用
//  约定：句柄不透明；字符串一律 UTF-8；跨 DLL 只传 POD 与缓冲区
// ============================================================

#ifdef __cplusplus
extern "C" {
#endif

#ifdef PPT_BUILD_DLL
#define PPT_API __declspec(dllexport)
#else
#define PPT_API __declspec(dllimport)
#endif

typedef void* PptDoc;

// 形状信息（POD，ctypes 镜像同布局）
typedef struct PptShapeInfo {
    long long id;
    int kind;                       // 0形状 1图片 2表格 3图表 4媒体 5组合
    long long x, y, cx, cy;         // EMU（页内顶层坐标）
    double rot;                     // 度
    char name[128];                 // UTF-8
    char text[1024];                // UTF-8 全文（超长截断）
    double fontsize_pt;             // 首个 run 字号，无文本为 -1
    unsigned int fill_rgb;          // 纯色填充，无填充为 0
    int italic;                     // 0/1，未知 -1
    int underline;                  // 0/1，未知 -1
    unsigned int line_rgb;          // 边框颜色，无边框为 0
    char font_name[64];             // 字体名（UTF-8），未知为空串
} PptShapeInfo;

// 打开 .pptx，失败返回 NULL（原因见 pptx_last_error）
PPT_API PptDoc pptx_open(const char* utf8_path);
PPT_API void   pptx_close(PptDoc doc);

PPT_API int pptx_slide_count(PptDoc doc);
// 幻灯片尺寸（EMU）
PPT_API void pptx_slide_size(PptDoc doc, long long* w_emu, long long* h_emu);

// 渲染一页为 RGBA8888（直通 alpha，行序自上而下）
// out_buf 由本 DLL 分配，用完必须 pptx_free_buf 释放
// 返回 0 成功；负数失败
PPT_API int pptx_render_rgba(PptDoc doc, int slide_idx, int px_w, int px_h,
                             unsigned char** out_buf, int* out_w, int* out_h);
PPT_API void pptx_free_buf(unsigned char* buf);

// ---- 形状级操作（索引为页内顶层形状次序；组内子形状归到其组） ----
PPT_API int pptx_shape_count(PptDoc doc, int slide_idx);
PPT_API int pptx_shape_get(PptDoc doc, int slide_idx, int shape_idx, PptShapeInfo* out);
// 画布坐标 -> 顶层形状索引，未命中 -1
PPT_API int pptx_hit(PptDoc doc, int slide_idx, int px, int py, int canvas_w, int canvas_h);
// 以下修改类返回 0 成功、负数失败
PPT_API int pptx_shape_move(PptDoc doc, int si, int i, long long dx_emu, long long dy_emu);
PPT_API int pptx_shape_resize(PptDoc doc, int si, int i, long long w_emu, long long h_emu);
PPT_API int pptx_shape_set_geom(PptDoc doc, int si, int i, long long x, long long y,
                                long long cx, long long cy);     // 一次改位置+尺寸
PPT_API int pptx_shape_set_rot(PptDoc doc, int si, int i, double rot_deg);
PPT_API int pptx_shape_set_text(PptDoc doc, int si, int i, const char* utf8);
PPT_API int pptx_shape_set_fontsize(PptDoc doc, int si, int i, double pt);
PPT_API int pptx_shape_set_fill(PptDoc doc, int si, int i, unsigned int rrggbb);
PPT_API int pptx_shape_set_textcolor(PptDoc doc, int si, int i, unsigned int rrggbb);
PPT_API int pptx_shape_set_align(PptDoc doc, int si, int i, int align);   // 0左 1中 2右
PPT_API int pptx_shape_set_bold(PptDoc doc, int si, int i, int bold);
PPT_API int pptx_shape_delete(PptDoc doc, int si, int i);
PPT_API int pptx_shape_duplicate(PptDoc doc, int si, int i);              // 返回新索引
PPT_API int pptx_shape_zorder(PptDoc doc, int si, int i, int dir);        // 1上移 -1下移 2置顶 -2置底
// 新增一个矩形文本形状，返回新索引
PPT_API int pptx_shape_add_rect(PptDoc doc, int si, long long x, long long y,
                                long long cx, long long cy, const char* utf8_text);
// 插入图片（自动按原图长宽比，cx/cy 传 0 表示自动），返回新索引
PPT_API int pptx_shape_add_picture(PptDoc doc, int si, const char* utf8_path,
                                   long long x, long long y, long long cx, long long cy);

// 文字样式查询：返回 0xRRGGBB，无则返回 -1
PPT_API int pptx_shape_text_color(PptDoc doc, int si, int i);
PPT_API int pptx_shape_text_align(PptDoc doc, int si, int i);   // 0左 1中 2右，未知 -1
PPT_API int pptx_shape_text_bold(PptDoc doc, int si, int i);    // 0/1，未知 -1

// 文字样式扩展：斜体 / 下划线 / 字体名
PPT_API int pptx_shape_set_italic(PptDoc doc, int si, int i, int on);
PPT_API int pptx_shape_set_underline(PptDoc doc, int si, int i, int on);
PPT_API int pptx_shape_set_fontname(PptDoc doc, int si, int i, const char* utf8_name);
PPT_API int pptx_shape_text_italic(PptDoc doc, int si, int i);
PPT_API int pptx_shape_text_underline(PptDoc doc, int si, int i);

// 边框线：rgb=0 表示无线条
PPT_API int pptx_shape_set_line(PptDoc doc, int si, int i, unsigned int rrggbb, double width_pt);
PPT_API unsigned int pptx_shape_line(PptDoc doc, int si, int i);

// 插入预设形状（prst 用 OOXML 名：rect / roundRect / ellipse / triangle /
// rightArrow / star5 / hexagon / line ...），返回新索引
PPT_API int pptx_shape_add_shape(PptDoc doc, int si, const char* prst,
                                 long long x, long long y, long long cx, long long cy,
                                 unsigned int fill_rgb, unsigned int line_rgb);

// 插入表格，返回新索引
PPT_API int pptx_shape_add_table(PptDoc doc, int si, long long x, long long y,
                                 long long cx, long long cy, int rows, int cols);

// ---- 表格单元格 ----
// 返回行数/列数，出错 -1
PPT_API int pptx_table_size(PptDoc doc, int si, int i, int* rows, int* cols);
// 画布像素坐标 -> 单元格（返回 r*cols+c，未命中 -1）
PPT_API int pptx_table_cell_at(PptDoc doc, int si, int i, int px, int py, int cw, int ch);
// 读/写单元格文字
PPT_API int pptx_table_cell_get_text(PptDoc doc, int si, int i, int r, int c, char* out, int cap);
PPT_API int pptx_table_cell_set_text(PptDoc doc, int si, int i, int r, int c, const char* utf8_text);
PPT_API int pptx_table_cell_rect(PptDoc doc, int si, int i, int r, int c, int cw, int ch,
                                 int* out_x, int* out_y, int* out_w, int* out_h);
PPT_API int pptx_table_row_add(PptDoc doc, int si, int i, int at);
PPT_API int pptx_table_row_del(PptDoc doc, int si, int i, int r);
PPT_API int pptx_table_col_add(PptDoc doc, int si, int i, int at);
PPT_API int pptx_table_col_del(PptDoc doc, int si, int i, int c);

// ---- 撤销 / 重做 ----
// begin/end 之间的多次修改合并为一步（拖拽用）
PPT_API int pptx_begin_edit(PptDoc doc);
PPT_API int pptx_end_edit(PptDoc doc);
PPT_API int pptx_undo(PptDoc doc);
PPT_API int pptx_redo(PptDoc doc);
PPT_API int pptx_can_undo(PptDoc doc);
PPT_API int pptx_can_redo(PptDoc doc);

// ---- 幻灯片级操作 ----
PPT_API int pptx_slide_add(PptDoc doc, int after_idx);      // 返回新页索引
PPT_API int pptx_slide_duplicate(PptDoc doc, int idx);
PPT_API int pptx_slide_remove(PptDoc doc, int idx);
PPT_API int pptx_slide_move(PptDoc doc, int from, int to);

// ---- 切换动画 / 入场动画 ----
// kind: none/fade/push/wipe/split/cover/uncover/cut/dissolve/blinds/checker/
//       circle/diamond/wheel/random/zoom
PPT_API int pptx_slide_transition_get(PptDoc doc, int si, char* kind, int cap,
                                      int* dur_ms, int* dir);
PPT_API int pptx_slide_transition_set(PptDoc doc, int si, const char* kind, int dur_ms, int dir);
PPT_API int pptx_transition_apply_all(PptDoc doc, const char* kind, int dur_ms, int dir);
// kind: none/appear/fade/wipe/checker/circle/diamond/dissolve/blinds/wheel/zoom
PPT_API int pptx_shape_anim_get(PptDoc doc, int si, int shape_idx, char* kind, int cap, int* dur_ms);
PPT_API int pptx_shape_anim_set(PptDoc doc, int si, int shape_idx, const char* kind, int dur_ms);
PPT_API int pptx_shape_anim_clear(PptDoc doc, int si);

// ---- 保存与导出（返回 0 成功） ----
PPT_API int pptx_save(PptDoc doc, const char* utf8_path);
PPT_API int pptx_export_html(PptDoc doc, const char* utf8_path);

// ---------------- 插件 ----------------
// 加载插件目录（传 NULL/空串用 DLL 同级的 plugins 目录），返回成功数量
PPT_API int pptx_plugins_load(const char* utf8_dir);
PPT_API int pptx_plugin_count();
// name/desc 为 UTF-8 输出缓冲
PPT_API int pptx_plugin_info(int idx, char* name, int name_cap, char* desc, int desc_cap);
// 向所有插件广播命令；返回 1=有插件处理（out 为结果文本）
PPT_API int pptx_plugin_command(const char* cmd, const char* arg, char* out, int out_cap);

// 最近一次错误（UTF-8，指针在下次错误前有效）
PPT_API const char* pptx_last_error();

#ifdef __cplusplus
}
#endif
