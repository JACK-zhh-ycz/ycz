#include "xmldom.h"
#include "../base/strutil.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>

namespace ppt {

// ==================== 节点方法 ====================

const char* XmlNode::attr(const std::string& key) const {
    for (const auto& kv : attrs) if (kv.first == key) return kv.second.c_str();
    return nullptr;
}

std::string XmlNode::get(const std::string& key, const std::string& def) const {
    const char* v = attr(key);
    return v ? std::string(v) : def;
}

long long XmlNode::get_num(const std::string& key, long long def) const {
    const char* v = attr(key);
    return v ? parse_ll(v, def) : def;
}

double XmlNode::get_dbl(const std::string& key, double def) const {
    const char* v = attr(key);
    double d = def;
    if (v) parse_double(v, d);
    return d;
}

void XmlNode::set(const std::string& key, const std::string& val) {
    for (auto& kv : attrs) if (kv.first == key) { kv.second = val; return; }
    attrs.emplace_back(key, val);
}

void XmlNode::set_num(const std::string& key, long long val) { set(key, std::to_string(val)); }

void XmlNode::remove_attr(const std::string& key) {
    for (size_t i = 0; i < attrs.size(); ++i) {
        if (attrs[i].first == key) { attrs.erase(attrs.begin() + i); return; }
    }
}

// 名称匹配：完全相等，或忽略命名空间前缀后相等（"r:embed" == "embed"）
static bool name_eq(const std::string& node, const std::string& want) {
    if (node == want) return true;
    size_t c = node.find(':');
    if (c != std::string::npos && node.compare(c + 1, std::string::npos, want) == 0) return true;
    return false;
}

XmlPtr XmlNode::first(const std::string& name) const {
    for (const XmlPtr& c : children) if (name_eq(c->tag, name)) return c;
    return nullptr;
}

std::vector<XmlPtr> XmlNode::all(const std::string& name) const {
    std::vector<XmlPtr> v;
    for (const XmlPtr& c : children) if (name_eq(c->tag, name)) v.push_back(c);
    return v;
}

static void collect_deep(const XmlNode* n, const std::string& name, int depth, int maxd,
                         std::vector<XmlPtr>& out) {
    if (depth > maxd) return;
    for (const XmlPtr& c : n->children) {
        if (name_eq(c->tag, name)) out.push_back(c);
        collect_deep(c.get(), name, depth + 1, maxd, out);
    }
}

XmlPtr XmlNode::first_deep(const std::string& name, int max_depth) const {
    std::vector<XmlPtr> v;
    collect_deep(this, name, 1, max_depth, v);
    return v.empty() ? nullptr : v[0];
}

std::vector<XmlPtr> XmlNode::all_deep(const std::string& name, int max_depth) const {
    std::vector<XmlPtr> v;
    collect_deep(this, name, 1, max_depth, v);
    return v;
}

XmlPtr XmlNode::path(const std::string& slash_sep) const {
    std::vector<std::string> segs;
    split(slash_sep, '/', segs);
    XmlPtr cur = nullptr;
    const std::vector<XmlPtr>* kids = &children;   // 起点是 this 的子节点
    for (const std::string& s : segs) {
        if (s.empty()) continue;
        XmlPtr nx;
        for (const XmlPtr& c : *kids) {
            if (name_eq(c->tag, s)) { nx = c; break; }
        }
        if (!nx) return nullptr;
        cur = nx;
        kids = &cur->children;
    }
    return cur;    // 若最后一段命中则返回，否则返回根自身
}

XmlPtr XmlNode::make(const std::string& tag_name) {
    XmlPtr n = std::make_shared<XmlNode>();
    n->tag = tag_name;
    return n;
}

XmlPtr XmlNode::add_child(const std::string& tag_name) {
    XmlPtr n = make(tag_name);
    return add_child(n);
}

XmlPtr XmlNode::add_child(XmlPtr node) {
    node->parent = this;
    children.push_back(node);
    return node;
}

void XmlNode::set_text(const std::string& t) { text = t; }

void XmlNode::remove_child(const std::string& tag_name) {
    for (size_t i = 0; i < children.size(); ++i) {
        if (name_eq(children[i]->tag, tag_name)) { children.erase(children.begin() + i); return; }
    }
}

std::string XmlNode::child_text(const std::string& name, const std::string& def) const {
    XmlPtr c = first(name);
    if (!c) return def;
    return c->text;
}

std::string XmlNode::child_attr(const std::string& name, const std::string& attr,
                                const std::string& def) const {
    XmlPtr c = first(name);
    if (!c) return def;
    return c->get(attr, def);
}

// ==================== 序列化 ====================

std::string xml_escape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            case '\'': o += "&apos;"; break;
            default: o += c;
        }
    }
    return o;
}

std::string xml_escape_text(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 4);
    for (char c : s) {
        if (c == '&') o += "&amp;";
        else if (c == '<') o += "&lt;";
        else if (c == '>') o += "&gt;";
        else if (c == '\r') { /* 丢弃 */ }
        else o += c;
    }
    return o;
}

static void write_node(const XmlNode* n, std::string& out) {
    out += '<';
    out += n->tag;
    for (const auto& kv : n->attrs) {
        out += ' ';
        out += kv.first;
        out += "=\"";
        out += xml_escape(kv.second);
        out += '"';
    }
    bool empty_text = n->text.empty();
    if (n->children.empty() && empty_text) { out += "/>"; return; }
    out += '>';
    if (!empty_text) out += xml_escape_text(n->text);
    for (const XmlPtr& c : n->children) write_node(c.get(), out);
    out += "</";
    out += n->tag;
    out += '>';
    out += '\n';
}

std::string XmlNode::to_xml(bool with_decl) const {
    std::string out;
    if (with_decl) out += "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n";
    write_node(this, out);
    return out;
}

// ==================== 解析器 ====================
namespace {

struct Parser {
    const char* s;
    size_t n, i;
    std::string err;
    int depth_guard;

    Parser(const char* d, size_t len) : s(d), n(len), i(0), depth_guard(0) {}

    bool eof() const { return i >= n; }
    char cur() const { return i < n ? s[i] : '\0'; }

    void skip_ws() { while (!eof() && (unsigned char)s[i] <= ' ') ++i; }

    bool match(const char* lit) {
        size_t l = strlen(lit);
        if (i + l > n) return false;
        if (memcmp(s + i, lit, l) != 0) return false;
        i += l;
        return true;
    }

    std::string read_name() {
        size_t start = i;
        while (!eof()) {
            char c = s[i];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '=' || c == '>' ||
                c == '/' || c == '<' || (unsigned char)c <= 0x20) break;
            ++i;
        }
        return std::string(s + start, i - start);
    }

    std::string read_quoted() {
        char q = s[i];
        if (q != '"' && q != '\'') { err = "属性值缺少引号"; return std::string(); }
        ++i;
        size_t start = i;
        while (!eof() && s[i] != q) ++i;
        std::string raw(s + start, i - start);
        if (eof()) { err = "属性值引号未闭合"; return raw; }
        ++i;
        return decode_entities(raw);
    }

    static std::string decode_entities(const std::string& in) {
        if (in.find('&') == std::string::npos) return in;
        std::string o;
        o.reserve(in.size());
        for (size_t k = 0; k < in.size(); ++k) {
            if (in[k] != '&') { o += in[k]; continue; }
            size_t sc = in.find(';', k);
            if (sc == std::string::npos || sc - k > 10) { o += in[k]; continue; }
            std::string ent = in.substr(k + 1, sc - k - 1);
            bool done = false;
            if (ent == "amp") { o += '&'; done = true; }
            else if (ent == "lt") { o += '<'; done = true; }
            else if (ent == "gt") { o += '>'; done = true; }
            else if (ent == "quot") { o += '"'; done = true; }
            else if (ent == "apos") { o += '\''; done = true; }
            else if (ent.size() > 1 && ent[0] == '#') {
                long code = (ent[1] == 'x' || ent[1] == 'X')
                                ? strtol(ent.c_str() + 2, nullptr, 16)
                                : strtol(ent.c_str() + 1, nullptr, 10);
                if (code > 0 && code < 0x110000) { o += utf32_to_utf8((uint32_t)code); done = true; }
            }
            if (done) k = sc;
            else { o += in[k]; }
        }
        return o;
    }

    // 数字字符引用 -> UTF-8（输入文档本身是 UTF-8，直接拼字节即可）
    static std::string utf32_to_utf8(uint32_t cp) {
        std::string o;
        if (cp < 0x80) {
            o += (char)cp;
        } else if (cp < 0x800) {
            o += (char)(0xC0 | (cp >> 6));
            o += (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            o += (char)(0xE0 | (cp >> 12));
            o += (char)(0x80 | ((cp >> 6) & 0x3F));
            o += (char)(0x80 | (cp & 0x3F));
        } else {
            o += (char)(0xF0 | (cp >> 18));
            o += (char)(0x80 | ((cp >> 12) & 0x3F));
            o += (char)(0x80 | ((cp >> 6) & 0x3F));
            o += (char)(0x80 | (cp & 0x3F));
        }
        return o;
    }

    // 主循环：解析一个元素，返回节点
    XmlPtr parse_element(int depth) {
        if (depth > 200) { err = "嵌套过深"; return nullptr; }
        if (cur() != '<') { err = "期望 '<'"; return nullptr; }
        ++i;
        std::string tag = read_name();
        if (tag.empty()) { err = "标签名为空"; return nullptr; }
        if (tag[0] == '?') { err = "非法处理指令"; return nullptr; }
        XmlPtr node = XmlNode::make(tag);

        for (;;) {
            skip_ws();
            if (eof()) { err = "文件在标签内截断"; return nullptr; }
            if (cur() == '/') {
                if (!match("/>")) { err = "期望 '/>'"; return nullptr; }
                return node;
            }
            if (cur() == '>') { ++i; break; }
            std::string key = read_name();
            if (key.empty()) { err = "属性名为空"; return nullptr; }
            skip_ws();
            if (cur() != '=') { err = "属性缺少 '=': " + key; return nullptr; }
            ++i;
            skip_ws();
            std::string val = read_quoted();
            if (!err.empty()) return nullptr;
            node->attrs.emplace_back(key, val);
        }

        // 内容
        std::string pending_text;
        auto flush_text = [&]() {
            if (!pending_text.empty()) {
                if (!node->text.empty()) node->text += pending_text;
                else node->text = pending_text;
                pending_text.clear();
            }
        };

        while (!eof()) {
            if (cur() == '<') {
                if (i + 1 < n && s[i + 1] == '/') {
                    // 结束标签
                    i += 2;
                    std::string en = read_name();
                    skip_ws();
                    if (!match(">")) { err = "结束标签未闭合"; return nullptr; }
                    if (en != tag) { err = "标签不匹配: " + tag + " vs " + en; return nullptr; }
                    flush_text();
                    return node;
                }
                if (match("<!--")) {                       // 注释
                    size_t end = 0;
                    for (size_t k = i; k + 2 < n; ++k) {
                        if (s[k] == '-' && s[k + 1] == '-' && s[k + 2] == '>') { end = k; break; }
                    }
                    if (!end) { err = "注释未闭合"; return nullptr; }
                    i = end + 3;
                    continue;
                }
                if (match("<![CDATA[")) {
                    size_t end = 0;
                    for (size_t k = i; k + 2 < n; ++k) {
                        if (s[k] == ']' && s[k + 1] == ']' && s[k + 2] == '>') { end = k; break; }
                    }
                    if (!end) { err = "CDATA 未闭合"; return nullptr; }
                    pending_text += std::string(s + i, end - i);
                    i = end + 3;
                    continue;
                }
                if (match("<?")) {                          // 处理指令
                    size_t end = 0;
                    for (size_t k = i; k + 1 < n; ++k) {
                        if (s[k] == '?' && s[k + 1] == '>') { end = k; break; }
                    }
                    if (!end) { err = "PI 未闭合"; return nullptr; }
                    i = end + 2;
                    continue;
                }
                flush_text();
                XmlPtr child = parse_element(depth + 1);
                if (!child) return nullptr;
                node->add_child(child);
            } else {
                size_t start = i;
                while (!eof() && s[i] != '<') ++i;
                pending_text += decode_entities(std::string(s + start, i - start));
            }
        }
        err = "文档未闭合: " + tag;
        return nullptr;
    }
};

} // namespace

XmlPtr xml_parse(const std::string& data, std::string& err) {
    const char* p = data.c_str();
    size_t n = data.size();
    size_t i = 0;
    // 跳过 BOM
    if (n >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) i = 3;
    // 跳过 XML 声明 / DOCTYPE / 顶层注释
    while (i < n) {
        while (i < n && (unsigned char)p[i] <= ' ') ++i;
        if (i + 5 <= n && memcmp(p + i, "<?xml", 5) == 0) {
            size_t end = std::string(p + i, n - i).find("?>");
            if (end == std::string::npos) { err = "XML 声明未闭合"; return nullptr; }
            i += end + 2;
            continue;
        }
        if (i + 4 <= n && memcmp(p + i, "<!--", 4) == 0) {
            size_t end = std::string(p + i, n - i).find("-->");
            if (end == std::string::npos) { err = "注释未闭合"; return nullptr; }
            i += end + 3;
            continue;
        }
        if (i + 2 <= n && memcmp(p + i, "<!", 2) == 0) {
            size_t end = std::string(p + i, n - i).find('>');
            if (end == std::string::npos) { err = "DOCTYPE 未闭合"; return nullptr; }
            i += end + 1;
            continue;
        }
        break;
    }
    if (i >= n) { err = "空文档"; return nullptr; }

    Parser ps(p + i, n - i);
    XmlPtr root = ps.parse_element(0);
    if (!root) { err = ps.err.empty() ? std::string("XML 解析失败") : ("XML 解析失败: " + ps.err); return nullptr; }
    return root;
}

XmlPtr xml_parse_file(const std::string& path, std::string& err) {
    std::string s;
    if (!read_file(path, s)) { err = "无法读取 XML: " + path; return nullptr; }
    return xml_parse(s, err);
}

} // namespace ppt
