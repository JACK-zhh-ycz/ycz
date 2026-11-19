#pragma once
// ============================================================
//  ZIP 读写（纯手写，不依赖 zlib）
//  - read ：解析 central directory，支持 stored(0) 与 deflate(8)
//  - write ：统一用 stored，产出 PowerPoint / WPS 可直接打开的包
//  路径名一律使用 '/' 分隔（OOXML 要求）
// ============================================================

#include <string>
#include <vector>
#include <cstdint>
#include <map>

namespace ppt {

class ZipArchive {
public:
    struct Entry {
        std::string name;
        std::vector<uint8_t> data;
    };

    ZipArchive() {}

    // ---- 解包 ----
    bool open(const std::vector<uint8_t>& blob, std::string& err);
    bool open_file(const std::string& path, std::string& err);

    bool has(const std::string& name) const;
    bool get(const std::string& name, std::string& out) const;
    bool get(const std::string& name, std::vector<uint8_t>& out) const;
    std::vector<std::string> names() const;
    const std::map<std::string, size_t>& index() const { return index_; }

    // ---- 打包 ----
    void add(const std::string& name, const std::string& data);
    void add(const std::string& name, const std::vector<uint8_t>& data);
    bool remove(const std::string& name);
    void clear() { entries_.clear(); index_.clear(); }

    bool save(const std::string& path, std::string& err) const;
    std::vector<uint8_t> build() const;

    size_t count() const { return entries_.size(); }

private:
    std::vector<Entry> entries_;
    std::map<std::string, size_t> index_;
};

} // namespace ppt
