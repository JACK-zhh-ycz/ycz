#include "strutil.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <cwctype>

namespace ppt {

// ==================== 编码转换 ====================

std::string utf16_to_utf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                        &out[0], n, nullptr, nullptr);
    return out;
}

std::wstring utf8_to_utf16(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n);
    return out;
}

// ==================== 文件读写 ====================

bool read_file(const std::string& utf8_path, std::vector<uint8_t>& out) {
    out.clear();
    std::wstring wp = utf8_to_utf16(utf8_path);
    HANDLE h = CreateFileW(wp.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER li{};
    if (!GetFileSizeEx(h, &li)) { CloseHandle(h); return false; }
    uint64_t sz = (uint64_t)li.QuadPart;
    if (sz > (1ull << 31)) { CloseHandle(h); return false; }   // 上限 2GB
    out.resize((size_t)sz);
    size_t done = 0;
    while (done < out.size()) {
        DWORD got = 0;
        UINT32 chunk = (UINT32)std::min<uint64_t>(out.size() - done, 1u << 20);
        if (!ReadFile(h, out.data() + done, chunk, &got, nullptr) || got == 0) break;
        done += got;
    }
    CloseHandle(h);
    if (done != out.size()) { out.clear(); return false; }
    return true;
}

bool read_file(const std::string& utf8_path, std::string& out) {
    std::vector<uint8_t> buf;
    if (!read_file(utf8_path, buf)) return false;
    out.assign(buf.begin(), buf.end());
    return true;
}

static bool write_binary(const std::string& utf8_path, const void* data, size_t len) {
    std::wstring wp = utf8_to_utf16(utf8_path);
    HANDLE h = CreateFileW(wp.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    size_t done = 0;
    while (done < len) {
        DWORD put = 0;
        UINT32 chunk = (UINT32)std::min<size_t>(len - done, 1u << 20);
        if (!WriteFile(h, (const uint8_t*)data + done, chunk, &put, nullptr) || put == 0) {
            CloseHandle(h);
            return false;
        }
        done += put;
    }
    CloseHandle(h);
    return true;
}

bool write_file(const std::string& utf8_path, const std::string& data) {
    std::string dir = dir_name(utf8_path);
    if (!dir.empty() && !make_dirs(dir)) return false;
    return write_binary(utf8_path, data.data(), data.size());
}

bool write_file(const std::string& utf8_path, const std::vector<uint8_t>& data) {
    std::string dir = dir_name(utf8_path);
    if (!dir.empty() && !make_dirs(dir)) return false;
    return write_binary(utf8_path, data.data(), data.size());
}

bool file_size_of(const std::string& utf8_path, uint64_t& sz) {
    std::wstring wp = utf8_to_utf16(utf8_path);
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(wp.c_str(), GetFileExInfoStandard, &fad)) return false;
    ULARGE_INTEGER u{};
    u.LowPart = fad.nFileSizeLow;
    u.HighPart = fad.nFileSizeHigh;
    sz = u.QuadPart;
    return true;
}

// ==================== 路径处理 ====================

std::string dir_name(const std::string& path) {
    size_t p = path.find_last_of("/\\");
    if (p == std::string::npos) return std::string();
    if (p == 0) return path[0] == '/' ? std::string("/") : std::string();
    return path.substr(0, p);
}

std::string base_name(const std::string& path) {
    size_t p = path.find_last_of("/\\");
    return (p == std::string::npos) ? path : path.substr(p + 1);
}

std::string stem_name(const std::string& path) {
    std::string b = base_name(path);
    size_t d = b.find_last_of('.');
    return (d == std::string::npos || d == 0) ? b : b.substr(0, d);
}

std::string extension_of(const std::string& path) {
    std::string b = base_name(path);
    size_t d = b.find_last_of('.');
    if (d == std::string::npos || d + 1 >= b.size()) return std::string();
    return to_lower(b.substr(d));
}

std::string join_path(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    char last = a[a.size() - 1];
    if (last == '/' || last == '\\') return a + b;
    if (b[0] == '/' || b[0] == '\\') return a + b;
    return a + "\\" + b;
}

std::string norm_path(const std::string& p) {
    std::vector<std::string> parts;
    std::vector<std::string> raw;
    split(p, '/', raw);
    // '/' 未命中时按 '\' 再切一次
    if (raw.size() < 2) { split(p, '\\', raw); }
    bool abs = false;
    for (size_t i = 0; i < raw.size(); ++i) {
        std::string seg = raw[i];
        if (i == 0 && seg.empty() && raw.size() > 1 && (p[0] == '/' || p[1] == ':')) abs = true;
        if (i == 0 && seg.size() >= 2 && seg[1] == ':') { parts.push_back(seg); abs = true; continue; }
        if (seg.empty() || seg == ".") continue;
        if (seg == "..") {
            if (parts.size() > 1 && parts.back() != "..") parts.pop_back();
            else if (!abs) parts.push_back(seg);
            continue;
        }
        parts.push_back(seg);
    }
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += "\\";
        out += parts[i];
    }
    if (out.empty()) out = abs ? "\\" : std::string(".");
    return out;
}

// OPC 包内部件路径：统一 '/' 分隔，折叠 '.' 与 '..'
std::string norm_part_path(const std::string& p) {
    std::vector<std::string> raw, parts;
    std::string q = p;
    for (char& c : q) if (c == '\\') c = '/';
    split(q, '/', raw);
    for (const std::string& seg : raw) {
        if (seg.empty() || seg == ".") continue;
        if (seg == "..") { if (!parts.empty()) parts.pop_back(); continue; }
        parts.push_back(seg);
    }
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) { if (i) out += "/"; out += parts[i]; }
    return out;
}

bool path_exists(const std::string& p) {
    DWORD a = GetFileAttributesW(utf8_to_utf16(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES;
}

bool is_directory(const std::string& p) {
    DWORD a = GetFileAttributesW(utf8_to_utf16(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool make_dirs(const std::string& p) {
    if (p.empty() || is_directory(p)) return is_directory(p);
    std::wstring wp = utf8_to_utf16(p);
    // 逐级创建
    size_t start = 0;
    if (wp.size() >= 2 && wp[1] == L':') start = 3;
    for (size_t i = start; i <= wp.size(); ++i) {
        if (i != wp.size() && wp[i] != L'\\' && wp[i] != L'/') continue;
        std::wstring sub = wp.substr(0, i);
        if (sub.empty()) continue;
        DWORD a = GetFileAttributesW(sub.c_str());
        if (a != INVALID_FILE_ATTRIBUTES) {
            if (!(a & FILE_ATTRIBUTE_DIRECTORY)) return false;
            continue;
        }
        if (!CreateDirectoryW(sub.c_str(), nullptr)) {
            DWORD e = GetLastError();
            if (e != ERROR_ALREADY_EXISTS) return false;
        }
    }
    return is_directory(p);
}

bool list_dir(const std::string& p, std::vector<DirItem>& out) {
    out.clear();
    std::wstring wp = utf8_to_utf16(join_path(p, "*"));
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(wp.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        DirItem it;
        it.dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        ULARGE_INTEGER u{}; u.LowPart = fd.nFileSizeLow; u.HighPart = fd.nFileSizeHigh;
        it.size = u.QuadPart;
        it.name = utf16_to_utf8(n);
        out.push_back(std::move(it));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end(), [](const DirItem& a, const DirItem& b) {
        if (a.dir != b.dir) return a.dir > b.dir;
        return to_lower(a.name) < to_lower(b.name);
    });
    return true;
}

// ==================== 字符串小工具 ====================

std::string to_lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

bool starts_with(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') ++a;
    while (b > a && (unsigned char)s[b - 1] <= ' ') --b;
    return s.substr(a, b - a);
}

void split(const std::string& s, char sep, std::vector<std::string>& out) {
    std::string cur;
    for (char c : s) {
        if (c == sep) { out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    out.push_back(cur);
}

void replace_all(std::string& s, const std::string& from, const std::string& to) {
    if (from.empty()) return;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
}

std::string html_escape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            case '\'': o += "&#39;"; break;
            default: o += c;
        }
    }
    return o;
}

std::string attr_escape(const std::string& s) { return html_escape(s); }

bool parse_double(const std::string& s, double& out) {
    std::string t = trim(s);
    if (t.empty()) return false;
    errno = 0;
    char* end = nullptr;
    double v = strtod(t.c_str(), &end);
    if (end == t.c_str()) return false;
    out = v;
    return true;
}

long long parse_ll(const std::string& s, long long def) {
    std::string t = trim(s);
    if (t.empty()) return def;
    errno = 0;
    char* end = nullptr;
    long long v = strtoll(t.c_str(), &end, 10);
    if (end == t.c_str()) return def;
    return v;
}

std::string fmt_num(double v, int digits) {
    if (v != v) v = 0;                       // NaN
    char buf[64];
    snprintf(buf, sizeof(buf), "%.*f", digits, v);
    std::string s = buf;
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    if (s == "-0") s = "0";
    return s;
}

std::string uuid_like(uint64_t seed) {
    // 简单确定性 8-4-4-4-12 形式，用作 pptx 内部 id / rId 之外的稳定标识
    uint64_t a = seed * 0x9E3779B97F4A7C15ull >> 16;
    uint64_t b = (seed ^ 0xC2B2AE3D27D4EB4Full) * 0x165667B19E3779F9ull >> 16;
    char buf[64];
    snprintf(buf, sizeof(buf),
             "%08llx-%04llx-%04llx-%04llx-%012llx",
             (unsigned long long)(a & 0xFFFFFFFFull),
             (unsigned long long)((a >> 32) & 0xFFFFull),
             (unsigned long long)(0x4000ull | ((b >> 48) & 0x0FFFull)),
             (unsigned long long)(0x8000ull | ((b >> 32) & 0x3FFFull)),
             (unsigned long long)(b & 0xFFFFFFFFFFFFull));
    return std::string(buf);
}

void console_print(const std::string& utf8) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    std::wstring w = utf8_to_utf16(utf8);
    DWORD put = 0;
    if (h && h != INVALID_HANDLE_VALUE && !w.empty()) {
        if (WriteConsoleW(h, w.data(), (DWORD)w.size(), &put, nullptr) && put == w.size()) return;
    }
    fputs(utf8.c_str(), stdout);
}

// ==================== CRC32 ====================

static uint32_t g_crc[256];
static bool g_crc_ready = false;

static void crc_init() {
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        g_crc[i] = c;
    }
    g_crc_ready = true;
}

uint32_t crc32_of(const void* data, size_t len) {
    if (!g_crc_ready) crc_init();
    const uint8_t* p = (const uint8_t*)data;
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) c = g_crc[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

} // namespace ppt
