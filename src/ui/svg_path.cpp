#include "svg_path.h"

#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <algorithm>

namespace svgpath {
namespace {

const double PI = 3.14159265358979323846;

inline double rad(double deg) { return deg * PI / 180.0; }

// ---------------- 数字读取器 ----------------
struct Reader {
    const char* p;
    explicit Reader(const std::string& s) : p(s.c_str()) {}

    // SVG 分隔符：空白与逗号
    void skip_delim() {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',') ++p;
    }
    bool eof() const { return *p == '\0'; }
    // 读取一个数字；失败返回 false 且不前进
    bool num(double& out) {
        skip_delim();
        if (eof()) return false;
        char* end = nullptr;
        double v = std::strtod(p, &end);
        if (end == p) return false;
        p = end;
        out = v;
        return true;
    }
    // 是否停在命令字母上
    bool on_cmd_letter() const {
        char c = *p;
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    }
};

// ---------------- 曲线细分（输出已按 sx/sy 缩放）----------------
void cubic_into(std::vector<Pt>& out, double sx, double sy,
                double x0, double y0, double x1, double y1,
                double x2, double y2, double x3, double y3, int samples) {
    for (int i = 1; i <= samples; ++i) {
        double t = (double)i / samples, u = 1.0 - t;
        double bx = u*u*u*x0 + 3*u*u*t*x1 + 3*u*t*t*x2 + t*t*t*x3;
        double by = u*u*u*y0 + 3*u*u*t*y1 + 3*u*t*t*y2 + t*t*t*y3;
        out.push_back({bx * sx, by * sy});
    }
}

void quad_into(std::vector<Pt>& out, double sx, double sy,
               double x0, double y0, double cx, double cy,
               double x1, double y1, int samples) {
    // 升为三次后细分，保证与 C 一致的形状
    double c1x = x0 + 2.0/3.0*(cx - x0), c1y = y0 + 2.0/3.0*(cy - y0);
    double c2x = x1 + 2.0/3.0*(cx - x1), c2y = y1 + 2.0/3.0*(cy - y1);
    cubic_into(out, sx, sy, x0, y0, c1x, c1y, c2x, c2y, x1, y1, samples);
}

// ---------------- SVG 端点参数化圆弧 -> 中心参数化 ----------------
bool arc_to_center(double x0, double y0, double rx, double ry, double phi,
                   bool large, bool sweep, double x, double y,
                   double& cx, double& cy, double& th1, double& dth) {
    if (rx == 0 || ry == 0) return false;
    rx = std::fabs(rx); ry = std::fabs(ry);
    double cphi = std::cos(phi), sphi = std::sin(phi);
    double dx2 = (x0 - x) / 2.0, dy2 = (y0 - y) / 2.0;
    double x1p =  cphi*dx2 + sphi*dy2;
    double y1p = -sphi*dx2 + cphi*dy2;

    double lambda = (x1p*x1p)/(rx*rx) + (y1p*y1p)/(ry*ry);
    if (lambda > 1.0) { double k = std::sqrt(lambda); rx *= k; ry *= k; }

    double rx2 = rx*rx, ry2 = ry*ry;
    double num = rx2*y1p*y1p + ry2*x1p*x1p;
    double radic = (num == 0) ? 0.0 : (rx2*ry2 - num) / num;
    if (radic < 0) radic = 0;
    double co = std::sqrt(radic);
    if (large == sweep) co = -co;
    double cxp = co * (rx * y1p) / ry;
    double cyp = co * (-(ry * x1p) / rx);
    cx = cphi*cxp - sphi*cyp + (x0 + x) / 2.0;
    cy = sphi*cxp + cphi*cyp + (y0 + y) / 2.0;

    auto angle = [](double ux, double uy, double vx, double vy) {
        double len = std::sqrt((ux*ux+uy*uy)*(vx*vx+vy*vy));
        double a = (len == 0) ? 1 : (ux*vx + uy*vy)/len;
        if (a > 1) a = 1;
        if (a < -1) a = -1;
        double ret = std::acos(a);
        if (ux*vy - uy*vx < 0) ret = -ret;
        return ret;
    };
    th1 = angle(1, 0, (x1p - cxp)/rx, (y1p - cyp)/ry);
    double dt = angle((x1p - cxp)/rx, (y1p - cyp)/ry, (-x1p - cxp)/rx, (-y1p - cyp)/ry);
    if (!sweep && dt > 0) dt -= 2*PI;
    if (sweep && dt < 0) dt += 2*PI;
    dth = dt;
    return true;
}

// 圆弧按角度等分直接采样（端点精确，避免贝塞尔控制点在椭圆情形的偏差）
void arc_into(std::vector<Pt>& out, double sx, double sy,
              double x0, double y0, double rx, double ry, double phi_deg,
              bool large, bool sweep, double x1, double y1, int samples) {
    double phi = rad(phi_deg);
    double cx, cy, th1, dth;
    if (!arc_to_center(x0, y0, rx, ry, phi, large, sweep, x1, y1, cx, cy, th1, dth)) {
        out.push_back({x1 * sx, y1 * sy});
        return;
    }
    double cphi = std::cos(phi), sphi = std::sin(phi);
    // 按角度比例确定采样数（整圈至少 samples 个，小角度按比例减少）
    int n = (int)std::ceil(std::fabs(dth) / (2*PI) * samples);
    if (n < 2) n = 2;
    if (n > samples * 4) n = samples * 4;
    for (int i = 1; i <= n; ++i) {
        double a = th1 + dth * i / n;
        double ex = rx*std::cos(a), ey = ry*std::sin(a);
        out.push_back({(cx + ex*cphi - ey*sphi) * sx,
                       (cy + ex*sphi + ey*cphi) * sy});
    }
    if (!out.empty()) out.back() = {x1 * sx, y1 * sy};   // 终点强制精确
    else out.push_back({x1 * sx, y1 * sy});
}

// 每组的“数值个数”（不是成对数）
inline int arg_width(char up) {
    switch (up) {
    case 'M': case 'L': case 'T': return 2;
    case 'H': case 'V':           return 1;
    case 'Q':                     return 4;   // (cx,cy,x,y)
    case 'S':                     return 4;   // (x2,y2,x,y)，首控制点由反射得到
    case 'C':                     return 6;   // (x1,y1,x2,y2,x,y)
    case 'A':                     return 7;   // (rx,ry,rot,large,sweep,x,y)
    default:                      return 0;
    }
}

} // namespace

// ==================== 解析 ====================
std::vector<PathCmd> parse_path(const std::string& d) {
    std::vector<PathCmd> cmds;
    Reader rd(d);
    char cmd = 0;
    double cx = 0, cy = 0;        // 当前点
    double sx = 0, sy = 0;        // 子路径起点
    double c2x = 0, c2y = 0;      // 上一条 C/S 的第二控制点
    double q2x = 0, q2y = 0;      // 上一条 Q/T 的控制点
    char prev = 0;

    while (!rd.eof()) {
        rd.skip_delim();
        if (rd.eof()) break;
        if (rd.on_cmd_letter()) { cmd = *rd.p; ++rd.p; }
        else if (cmd == 0) break;          // 开头没有命令字母
        if (cmd == 0) break;

        bool rel = (cmd >= 'a' && cmd <= 'z');
        char up = rel ? (char)(cmd - 'a' + 'A') : cmd;

        if (up == 'Z') {
            PathCmd zc; zc.type = 'Z';
            cmds.push_back(zc);
            cx = sx; cy = sy;
            prev = 'Z';
            continue;
        }

        // A 的参数是 7 个（rx ry rot large sweep x y）
        int width = (up == 'A') ? 7 : arg_width(up);
        if (width == 0) break;             // 未知命令，停止防止死循环

        // 先读该命令后面的全部数字
        std::vector<double> nums;
        {
            double v;
            while (rd.num(v)) {
                nums.push_back(v);
                if (rd.on_cmd_letter()) break;   // 下一个字母意味着新命令
            }
        }
        if ((int)nums.size() < width) { prev = up; continue; }   // 参数不足：丢弃该命令

        // 按参数宽度切组；SVG 规定 M 的后续组按 L 处理
        for (size_t base = 0; base + width <= nums.size(); base += width) {
            PathCmd pc;
            pc.type = (base == 0) ? up : (up == 'M' ? 'L' : up);

            if (pc.type == 'H') {
                double x = rel ? cx + nums[base] : nums[base];
                pc.pts.push_back(x);
                cx = x;
            } else if (pc.type == 'V') {
                double y = rel ? cy + nums[base] : nums[base];
                pc.pts.push_back(y);
                cy = y;
            } else if (pc.type == 'A') {
                double rx = std::fabs(nums[base]), ry = std::fabs(nums[base+1]);
                double rot = nums[base+2];
                bool large = nums[base+3] != 0, sweep = nums[base+4] != 0;
                double ex = nums[base+5], ey = nums[base+6];
                if (rel) { ex += cx; ey += cy; }
                pc.pts.push_back(rx); pc.pts.push_back(ry); pc.pts.push_back(rot);
                pc.pts.push_back(large ? 1 : 0); pc.pts.push_back(sweep ? 1 : 0);
                pc.pts.push_back(ex); pc.pts.push_back(ey);
                cx = ex; cy = ey;
            } else {   // 2 或 3 对坐标的命令
                // 逐参数转绝对坐标
                std::vector<double> a(nums.begin() + base, nums.begin() + base + width);
                for (int i = 0; i < width; ++i) if (rel) a[i] += (i % 2 == 0) ? cx : cy;

                if (pc.type == 'C') {
                    for (int i = 0; i < width; ++i) pc.pts.push_back(a[i]);
                    c2x = a[2]; c2y = a[3];
                    cx = a[4]; cy = a[5];
                } else if (pc.type == 'S') {
                    double p1x, p1y;
                    if (prev == 'C' || prev == 'S') { p1x = 2*cx - c2x; p1y = 2*cy - c2y; }
                    else { p1x = cx; p1y = cy; }
                    pc.pts.push_back(p1x); pc.pts.push_back(p1y);
                    pc.pts.push_back(a[0]); pc.pts.push_back(a[1]);
                    pc.pts.push_back(a[2]); pc.pts.push_back(a[3]);
                    c2x = a[0]; c2y = a[1];
                    cx = a[2]; cy = a[3];
                } else if (pc.type == 'Q') {
                    for (int i = 0; i < width; ++i) pc.pts.push_back(a[i]);
                    q2x = a[0]; q2y = a[1];
                    cx = a[2]; cy = a[3];
                } else if (pc.type == 'T') {
                    double qx, qy;
                    if (prev == 'Q' || prev == 'T') { qx = 2*cx - q2x; qy = 2*cy - q2y; }
                    else { qx = cx; qy = cy; }
                    pc.pts.push_back(qx); pc.pts.push_back(qy);
                    pc.pts.push_back(a[0]); pc.pts.push_back(a[1]);
                    q2x = qx; q2y = qy;
                    cx = a[0]; cy = a[1];
                } else {   // M / L
                    for (int i = 0; i < width; ++i) pc.pts.push_back(a[i]);
                    if (pc.type == 'M') { sx = a[0]; sy = a[1]; }
                    cx = a[0]; cy = a[1];
                }
            }
            cmds.push_back(pc);
            prev = pc.type;
        }
    }
    return cmds;
}

// ==================== 展平 ====================
std::vector<Polyline> flatten(const std::vector<PathCmd>& cmds,
                              double sx, double sy, int samples) {
    std::vector<Polyline> polys;
    if (samples < 1) samples = 1;
    double cx = 0, cy = 0;
    size_t cur = SIZE_MAX;          // 当前子路径下标

    auto start_poly = [&]() {
        polys.push_back(Polyline{});
        cur = polys.size() - 1;
        polys[cur].pts.push_back({cx * sx, cy * sy});
    };

    for (const auto& cmd : cmds) {
        const std::vector<double>& v = cmd.pts;
        switch (cmd.type) {
        case 'M':
            if (v.size() >= 2) { cx = v[0]; cy = v[1]; }
            start_poly();
            break;
        case 'L':
            for (size_t p = 0; p + 1 < v.size(); p += 2) {
                cx = v[p]; cy = v[p+1];
                if (cur == SIZE_MAX) start_poly();
                else polys[cur].pts.push_back({cx * sx, cy * sy});
            }
            break;
        case 'H':
            for (size_t p = 0; p < v.size(); ++p) {
                cx = v[p];
                if (cur == SIZE_MAX) start_poly();
                else polys[cur].pts.push_back({cx * sx, cy * sy});
            }
            break;
        case 'V':
            for (size_t p = 0; p < v.size(); ++p) {
                cy = v[p];
                if (cur == SIZE_MAX) start_poly();
                else polys[cur].pts.push_back({cx * sx, cy * sy});
            }
            break;
        case 'C': case 'S':
            for (size_t p = 0; p + 5 < v.size(); p += 6) {
                if (cur == SIZE_MAX) start_poly();
                cubic_into(polys[cur].pts, sx, sy, cx, cy,
                           v[p], v[p+1], v[p+2], v[p+3], v[p+4], v[p+5], samples);
                cx = v[p+4]; cy = v[p+5];
            }
            break;
        case 'Q': case 'T':
            for (size_t p = 0; p + 3 < v.size(); p += 4) {
                if (cur == SIZE_MAX) start_poly();
                quad_into(polys[cur].pts, sx, sy, cx, cy,
                          v[p], v[p+1], v[p+2], v[p+3], samples);
                cx = v[p+2]; cy = v[p+3];
            }
            break;
        case 'A':
            for (size_t p = 0; p + 6 < v.size(); p += 7) {
                if (cur == SIZE_MAX) start_poly();
                arc_into(polys[cur].pts, sx, sy, cx, cy, v[p], v[p+1], v[p+2],
                         v[p+3] != 0, v[p+4] != 0, v[p+5], v[p+6], samples);
                cx = v[p+5]; cy = v[p+6];
            }
            break;
        case 'Z':
            if (cur != SIZE_MAX && !polys[cur].pts.empty()) {
                polys[cur].closed = true;
                polys[cur].pts.push_back(polys[cur].pts.front());
            }
            break;
        default: break;
        }
    }
    return polys;
}

// ==================== 包围盒 ====================
Bounds polyline_bounds(const std::vector<Polyline>& polys) {
    Bounds b;
    bool any = false;
    for (const auto& pl : polys) {
        for (const auto& pt : pl.pts) {
            if (!any) { b.xmin = b.xmax = pt.x; b.ymin = b.ymax = pt.y; any = true; }
            else {
                if (pt.x < b.xmin) b.xmin = pt.x;
                if (pt.x > b.xmax) b.xmax = pt.x;
                if (pt.y < b.ymin) b.ymin = pt.y;
                if (pt.y > b.ymax) b.ymax = pt.y;
            }
        }
    }
    b.valid = any;
    return b;
}

// ==================== 命中测试（奇偶填充规则） ====================
bool point_in_polylines(const std::vector<Polyline>& polys, double x, double y) {
    bool inside = false;
    for (const auto& pl : polys) {
        const auto& pts = pl.pts;
        size_t n = pts.size();
        if (n < 3) continue;
        for (size_t i = 0, j = n - 1; i < n; j = i++) {
            double yi = pts[i].y, yj = pts[j].y;
            if ((yi > y) != (yj > y)) {
                double xint = (pts[j].x - pts[i].x) * (y - yi) / (yj - yi) + pts[i].x;
                if (x < xint) inside = !inside;
            }
        }
    }
    return inside;
}

// ==================== 到轮廓最短距离 ====================
namespace {
double dist_seg(double px, double py, double ax, double ay, double bx, double by) {
    double dx = bx - ax, dy = by - ay;
    double l2 = dx*dx + dy*dy;
    double t = (l2 == 0) ? 0 : ((px - ax)*dx + (py - ay)*dy) / l2;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    double qx = ax + t*dx, qy = ay + t*dy;
    return std::sqrt((px-qx)*(px-qx) + (py-qy)*(py-qy));
}
} // namespace

double distance_to_polylines(const std::vector<Polyline>& polys, double x, double y) {
    double best = 1e18;
    for (const auto& pl : polys) {
        const auto& pts = pl.pts;
        for (size_t i = 0; i + 1 < pts.size(); ++i)
            best = std::min(best, dist_seg(x, y, pts[i].x, pts[i].y, pts[i+1].x, pts[i+1].y));
    }
    return best;
}

} // namespace svgpath
