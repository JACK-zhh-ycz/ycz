#pragma once
// ============================================================
//  轻量 XML DOM（手写解析，支持命名空间前缀、属性、文本、注释）
//  OOXML 的属性名带前缀（如 a:latin / r:embed），因此这里
//  **保留原始前缀**，不做命名空间归一化匹配。
// ============================================================

#include <string>
#include <vector>
#include <memory>
#include <map>

namespace ppt {

class XmlNode;
using XmlPtr = std::shared_ptr<XmlNode>;

class XmlNode {
public:
    std::string tag;                       // 含前缀，如 "p:sp"
    std::vector<std::pair<std::string, std::string>> attrs;
    std::string text;                      // 直接子文本拼接
    std::vector<XmlPtr> children;
    XmlNode* parent = nullptr;

    // ---- 属性 ----
    const char* attr(const std::string& key) const;
    std::string get(const std::string& key, const std::string& def = std::string()) const;
    long long get_num(const std::string& key, long long def = 0) const;
    double get_dbl(const std::string& key, double def = 0) const;
    bool has_attr(const std::string& key) const { return attr(key) != nullptr; }
    void set(const std::string& key, const std::string& val);
    void set_num(const std::string& key, long long val);
    void remove_attr(const std::string& key);

    // ---- 子节点 ----
    // name 支持 "a:rPr" 精确匹配，也支持忽略前缀匹配（后缀比较）
    XmlPtr first(const std::string& name) const;
    XmlPtr first_deep(const std::string& name, int max_depth = 12) const;
    std::vector<XmlPtr> all(const std::string& name) const;
    std::vector<XmlPtr> all_deep(const std::string& name, int max_depth = 12) const;
    bool has(const std::string& name) const { return first(name) != nullptr; }
    std::string child_text(const std::string& name, const std::string& def = std::string()) const;
    // 取第一个匹配子节点的某属性值（找不到返回 def）
    std::string child_attr(const std::string& name, const std::string& attr,
                           const std::string& def = std::string()) const;

    // ---- 构造 ----
    static XmlPtr make(const std::string& tag_name);
    XmlPtr add_child(const std::string& tag_name);
    XmlPtr add_child(XmlPtr node);
    void set_text(const std::string& t);
    void remove_child(const std::string& tag_name);

    // ---- 路径式查找： "p:cSld/p:spTree/p:sp" ----
    XmlPtr path(const std::string& slash_sep) const;

    // ---- 序列化 ----
    std::string to_xml(bool with_decl = true) const;
};

// 解析入口
XmlPtr xml_parse(const std::string& data, std::string& err);
XmlPtr xml_parse_file(const std::string& path, std::string& err);

// 属性值转义（内部使用，公开供生成 XML 的模块复用）
std::string xml_escape(const std::string& s);
// 文本节点转义（只处理 & < >）
std::string xml_escape_text(const std::string& s);

} // namespace ppt
