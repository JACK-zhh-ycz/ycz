#pragma once
// ============================================================
//  基础工具：UTF-8 / UTF-16 转换、文件读写、路径处理
//  说明：为兼容 MinGW 8.1（无稳定 std::filesystem），
//        文件系统操作统一使用 Win32 宽字符 API 自行封装。
// ============================================================

#include <string>
#include <vector>
#include <cstdint>
#include <Windows.h>

namespace ppt {

// ---------- 编码转换 ----------
std::string utf16_to_utf8(const std::wstring& w);
std::wstring utf8_to_utf16(const std::string& s);

// ---------- 文件读写（二进制安全） ----------
// 成功返回 true；out 为文件全部内容
bool read_file(const std::string& utf8_path, std::string& out);
bool read_file(const std::string& utf8_path, std::vector<uint8_t>& out);
bool write_file(const std::string& utf8_path, const std::string& data);
bool write_file(const std::string& utf8_path, const std::vector<uint8_t>& data);
bool file_size_of(const std::string& utf8_path, uint64_t& sz);

// ---------- 路径处理（分隔符统一按 '/' 与 '\' 都接受） ----------
std::string dir_name(const std::string& path);
std::string base_name(const std::string& path);       // 含扩展名
std::string stem_name(const std::string& path);       // 不含扩展名
std::string extension_of(const std::string& path);    // 含点，小写
std::string join_path(const std::string& a, const std::string& b);
std::string norm_path(const std::string& p);          // 折叠 '..'、统一分隔符为 '\'
// OPC 包内部件路径：统一 '/' 分隔并折叠 '.' / '..'
std::string norm_part_path(const std::string& p);
bool path_exists(const std::string& p);
bool is_directory(const std::string& p);
bool make_dirs(const std::string& p);                 // 递归创建
// 列出目录（name 为 UTF-8，dir=true 表示子目录）
struct DirItem { std::string name; bool dir; uint64_t size; };
bool list_dir(const std::string& p, std::vector<DirItem>& out);

// ---------- 字符串小工具 ----------
std::string to_lower(std::string s);
bool starts_with(const std::string& s, const std::string& prefix);
bool ends_with(const std::string& s, const std::string& suffix);
std::string trim(const std::string& s);
void split(const std::string& s, char sep, std::vector<std::string>& out);
void replace_all(std::string& s, const std::string& from, const std::string& to);
std::string html_escape(const std::string& s);        // & < > " '
std::string attr_escape(const std::string& s);        // 同上，用于 HTML 属性
bool parse_double(const std::string& s, double& out);
long long parse_ll(const std::string& s, long long def = 0);
std::string fmt_num(double v, int digits = 2);        // 去掉多余尾 0
std::string uuid_like(uint64_t seed);                 // 稳定的伪 GUID（用于 pptx id）

// ---------- 控制台/宽字符输出辅助 ----------
void console_print(const std::string& utf8);

// ---------- CRC32（ZIP 需要） ----------
uint32_t crc32_of(const void* data, size_t len);

} // namespace ppt
