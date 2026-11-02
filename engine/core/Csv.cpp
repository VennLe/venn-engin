#include "Csv.h"

#include "core/Logger.h"

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace core {

int CsvTable::columnIndex(const std::string& name) const {
    for (size_t i = 0; i < header.size(); ++i) {
        if (header[i] == name) return static_cast<int>(i);
    }
    return -1;
}

namespace {

// 一个字段的读取：返回 false = 引号没闭合（行号由调用方补）。
bool readField(const std::string& s, size_t& i, std::string& out) {
    out.clear();
    if (i < s.size() && s[i] == '"') {
        ++i;  // 跳过开引号
        while (i < s.size()) {
            if (s[i] == '"') {
                if (i + 1 < s.size() && s[i + 1] == '"') {
                    out.push_back('"');
                    i += 2;
                    continue;
                }
                ++i;  // 闭引号
                return true;
            }
            out.push_back(s[i++]);
        }
        return false;  // 走到末尾还没闭引号
    }
    while (i < s.size() && s[i] != ',' && s[i] != '\n' && s[i] != '\r') {
        out.push_back(s[i++]);
    }
    return true;
}

} // namespace

bool parseCsv(const std::string& text, CsvTable& out, std::string* err) {
    out.header.clear();
    out.rows.clear();

    size_t i = 0;
    // BOM
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        i = 3;
    }

    int line = 1;
    while (i < text.size()) {
        std::vector<std::string> row;
        // ---- 读一整行（引号里的换行不算行尾）----
        for (;;) {
            if (i >= text.size()) break;
            std::string f;
            if (!readField(text, i, f)) {
                if (err)
                    *err = "unterminated quote at line " + std::to_string(line);
                return false;
            }
            row.push_back(f);
            if (i < text.size() && text[i] == ',') {
                ++i;
                continue;
            }
            if (i < text.size() && text[i] == '\r') ++i;
            if (i < text.size() && text[i] == '\n') ++i;
            break;
        }

        // **空行跳过**：Excel 常在末尾留一个空行，把它当成"一行空数据"
        // 会让配表多出一行全空的记录（那行没有 id，查起来像数据丢了）。
        const bool allEmpty = row.size() == 1 && row[0].empty();
        if (!allEmpty) {
            if (out.header.empty()) out.header = row;
            else out.rows.push_back(row);
        }
        ++line;
    }

    // 补齐：短行补空串（手写表常见）。不补的话 `cell(r, c)` 会越界。
    for (auto& r : out.rows) {
        if (r.size() < out.header.size()) r.resize(out.header.size());
    }
    return true;
}

namespace {

bool needsQuote(const std::string& s) {
    return s.find_first_of(",\"\r\n") != std::string::npos;
}

void emitField(std::string& out, const std::string& s) {
    if (!needsQuote(s)) {
        out += s;
        return;
    }
    out.push_back('"');
    for (char c : s) {
        if (c == '"') out += "\"\"";
        else out.push_back(c);
    }
    out.push_back('"');
}

} // namespace

std::string writeCsv(const CsvTable& t) {
    std::string out;
    for (size_t c = 0; c < t.header.size(); ++c) {
        if (c) out.push_back(',');
        emitField(out, t.header[c]);
    }
    out += "\r\n";
    for (const auto& r : t.rows) {
        for (size_t c = 0; c < t.header.size(); ++c) {
            if (c) out.push_back(',');
            emitField(out, c < r.size() ? r[c] : std::string());
        }
        out += "\r\n";
    }
    return out;
}

bool loadCsvFile(const std::string& path, CsvTable& out, std::string* err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        if (err) *err = "cannot open " + path;
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string text = ss.str();
    if (!parseCsv(text, out, err)) {
        if (err) *err = path + ": " + *err;
        return false;
    }
    return true;
}

bool saveCsvFile(const std::string& path, const CsvTable& t, std::string* err) {
    const std::string text = writeCsv(t);
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            if (err) *err = "cannot open " + tmp + " for writing";
            return false;
        }
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        f.flush();
        if (!f) {
            if (err) *err = "write failed: " + tmp;
            std::error_code ec;
            fs::remove(tmp, ec);
            return false;
        }
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        // 跨卷 / 目标被占用：退化成"删了再改名"（与 saveGameFile 同）。
        fs::remove(path, ec);
        ec.clear();
        fs::rename(tmp, path, ec);
    }
    if (ec) {
        if (err) *err = "rename failed: " + ec.message();
        std::error_code e2;
        fs::remove(tmp, e2);
        return false;
    }
    return true;
}

bool parseNumber(const std::string& s, double& out) {
    if (s.empty()) return false;
    errno = 0;
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    // 整串必须吃掉：`12abc` 用 atof 会静默得到 12（同 `project::setField`
    // 与 `ui2::parseUiAction` 的那两处判断）。
    if (!end || *end != '\0' || end == s.c_str()) return false;
    out = v;
    return true;
}

bool parseBool(const std::string& s, bool& out) {
    if (s.empty()) return false;
    std::string t;
    t.reserve(s.size());
    for (char c : s) t.push_back(static_cast<char>(std::tolower(
        static_cast<unsigned char>(c))));
    if (t == "1" || t == "true" || t == "yes" || t == "on") {
        out = true;
        return true;
    }
    if (t == "0" || t == "false" || t == "no" || t == "off") {
        out = false;
        return true;
    }
    // 数值也当布尔（表里常写 1 / 0 之外的 2、-1）
    double n = 0.0;
    if (parseNumber(t, n)) {
        out = n != 0.0;
        return true;
    }
    return false;
}

} // namespace core
