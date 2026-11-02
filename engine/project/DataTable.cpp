#include "DataTable.h"

#include "assets/AssetPath.h"
#include "core/Logger.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>

namespace fs = std::filesystem;

namespace project {

namespace {
const std::string kEmpty;
}

const std::string& DataTable::raw(size_t r, int c) const {
    if (r >= data.rows.size() || c < 0 ||
        static_cast<size_t>(c) >= data.rows[r].size()) {
        return kEmpty;
    }
    return data.rows[r][static_cast<size_t>(c)];
}

double DataTable::num(size_t r, int c, double def) const {
    double v = 0.0;
    if (!core::parseNumber(raw(r, c), v)) {
        // ⚠ 每张表每种列只警告一次：一张写错一格的表在脚本里每帧被读
        //   60 次，逐次警告会把日志冲垮（且没人能从中找到"哪一格"）。
        if (!raw(r, c).empty()) {
            VK_LOG_WARN("TABLE-PARSE table=%s row=%zu col=%d value='%s' "
                        "(not a number, using %.3f)",
                        name.c_str(), r, c, raw(r, c).c_str(), def);
        }
        return def;
    }
    return v;
}

std::string DataTable::str(size_t r, int c, const std::string& def) const {
    const std::string& s = raw(r, c);
    // 空串**不**回退到默认值：表里留空就是"空"，这是策划表达"没有"的
    // 唯一手段。只有越界才用默认值。
    if (r >= data.rows.size() || c < 0 ||
        static_cast<size_t>(c) >= data.rows[r].size()) {
        return def;
    }
    return s;
}

bool DataTable::flag(size_t r, int c, bool def) const {
    bool v = false;
    if (!core::parseBool(raw(r, c), v)) return def;
    return v;
}

double DataTable::num(size_t r, const char* col, double def) const {
    const int c = column(col);
    if (c < 0) {
        VK_LOG_WARN("TABLE-COL table=%s missing column '%s'", name.c_str(), col);
        return def;
    }
    return num(r, c, def);
}

std::string DataTable::str(size_t r, const char* col,
                           const std::string& def) const {
    const int c = column(col);
    if (c < 0) {
        VK_LOG_WARN("TABLE-COL table=%s missing column '%s'", name.c_str(), col);
        return def;
    }
    return str(r, c, def);
}

bool DataTable::flag(size_t r, const char* col, bool def) const {
    const int c = column(col);
    if (c < 0) return def;
    return flag(r, c, def);
}

int DataTable::rowById(const std::string& id) const {
    // 约定第一列是 id。空 id 不参与匹配 —— 空行（策划留的分隔行）
    // 不该被 `rowById("")` 意外命中。
    for (size_t r = 0; r < data.rows.size(); ++r) {
        if (!data.rows[r].empty() && data.rows[r][0] == id) {
            return static_cast<int>(r);
        }
    }
    return -1;
}

// ---------------------------------------------------------------- 集合

TableStore& TableStore::get() {
    static TableStore s;
    return s;
}

bool TableStore::loadOne(const std::string& path, std::string* err) {
    core::CsvTable csv;
    if (!core::loadCsvFile(path, csv, err)) return false;
    if (csv.header.empty()) {
        if (err) *err = path + ": empty table (no header row)";
        return false;
    }
    DataTable t;
    t.path = path;
    t.name = fs::path(path).stem().string();
    t.data = std::move(csv);

    auto it = std::find_if(m_tables.begin(), m_tables.end(),
                           [&](const DataTable& x) { return x.name == t.name; });
    if (it != m_tables.end()) *it = std::move(t);
    else m_tables.push_back(std::move(t));
    return true;
}

bool TableStore::loadAll(const std::string& dir, std::string* err) {
    m_dir = dir;
    m_tables.clear();
    std::error_code ec;
    if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) {
        // 没有配表目录 = 正常（新工程还没配表）。**不报错**：报错会让
        // "还没开始配表"看起来像"配表坏了"。
        VK_LOG_INFO("TABLE-LOAD dir=%s count=0 (no such directory)",
                    dir.c_str());
        return true;
    }
    int bad = 0;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file()) continue;
        const std::string ext = e.path().extension().string();
        // 大小写不敏感：Windows 上 `.CSV` 很常见（Excel 另存为）。
        std::string low;
        for (char c : ext) {
            low.push_back(static_cast<char>(
                std::tolower(static_cast<unsigned char>(c))));
        }
        if (low != ".csv") continue;

        std::string one;
        if (!loadOne(e.path().generic_string(), &one)) {
            ++bad;
            VK_LOG_ERROR("TABLE-LOAD failed: %s", one.c_str());
        }
    }
    VK_LOG_INFO("TABLE-LOAD dir=%s count=%zu bad=%d", dir.c_str(),
                m_tables.size(), bad);
    if (bad && err) *err = std::to_string(bad) + " table(s) failed to load";
    return true;   // 单张表坏不影响其它表
}

DataTable* TableStore::find(const std::string& name) {
    for (auto& t : m_tables) {
        if (t.name == name) return &t;
    }
    return nullptr;
}

bool TableStore::save(DataTable& t, std::string* err) {
    if (t.path.empty()) t.path = (fs::path(m_dir) / (t.name + ".csv"))
                                      .generic_string();
    return core::saveCsvFile(t.path, t.data, err);
}

// ---------------------------------------------------------------- 自检

// ---------------------------------------------------------------- 按 key 查

bool splitTableKey(const std::string& key, std::string* table, std::string* row,
                   std::string* col) {
    // ⚠ 用**第一个**点切表、**最后一个**点切列，不用 `find` 两次相同的位置：
    //   行 id 里带点是常态（`goblin.chief`），而表名/列名带点才是异常。
    const std::size_t first = key.find('.');
    if (first == std::string::npos) return false;
    const std::size_t last = key.rfind('.');
    if (last == first) return false;      // 只有一个点 = 缺行或缺列
    if (first == 0 || last + 1 >= key.size()) return false;

    if (table) *table = key.substr(0, first);
    if (row) *row = key.substr(first + 1, last - first - 1);
    if (col) *col = key.substr(last + 1);

    // 中间段为空（`"cards..cost"`）是写错了，不是"行 id 是空串"。
    if (row && row->empty()) return false;
    return true;
}

int ensureTablesLoaded() {
    // `dir()` 非空 = 已经扫过一次（`loadAll` 会记住它）。用这个而不是
    // 一个 static bool：编辑器里 DataTablePanel 可能先扫过一遍，
    // 这时再扫一次会白白重读磁盘（而且会把"运行时改过的单元格"覆盖回去）。
    TableStore& store = TableStore::get();
    if (!store.dir().empty()) return static_cast<int>(store.all().size());
    std::string err;
    store.loadAll(assets::resolveAssetPath("data"), &err);
    if (!err.empty()) {
        VK_LOG_WARN("TABLE-LOAD err=%s", err.c_str());
    }
    VK_LOG_INFO("TABLE-LOAD dir=%s tables=%d", store.dir().c_str(),
                static_cast<int>(store.all().size()));
    return static_cast<int>(store.all().size());
}

double tableNum(const std::string& key) {
    std::string table, row, col;
    if (!splitTableKey(key, &table, &row, &col)) return 0.0;
    DataTable* t = TableStore::get().find(table);
    if (!t) return 0.0;
    const int r = t->rowById(row);
    if (r < 0) return 0.0;
    return t->num(static_cast<std::size_t>(r), col.c_str(), 0.0);
}

std::string tableStr(const std::string& key) {
    std::string table, row, col;
    if (!splitTableKey(key, &table, &row, &col)) return std::string();
    DataTable* t = TableStore::get().find(table);
    if (!t) return std::string();
    const int r = t->rowById(row);
    if (r < 0) return std::string();
    return t->str(static_cast<std::size_t>(r), col.c_str(), std::string());
}

bool tableHas(const std::string& key) {
    std::string table, row, col;
    if (!splitTableKey(key, &table, &row, &col)) return false;
    DataTable* t = TableStore::get().find(table);
    if (!t) return false;
    const int r = t->rowById(row);
    if (r < 0) return false;
    // 列不存在也算"这一格没有" —— `column()` 返回 -1。
    return t->column(col) >= 0;
}

int tableRows(const std::string& tableName) {
    DataTable* t = TableStore::get().find(tableName);
    if (!t) return -1;
    return static_cast<int>(t->rows());
}

int runDataTableSelfTest() {
    int pass = 0;
    int fail = 0;
    auto check = [&](bool ok, const char* name, const std::string& detail) {
        if (ok) {
            ++pass;
            std::printf("TABLE-TEST PASS %s\n", name);
        } else {
            ++fail;
            std::printf("TABLE-TEST FAIL %s (%s)\n", name, detail.c_str());
        }
    };

    std::error_code ec;
    const fs::path base = fs::temp_directory_path(ec) / "venn_table_test";
    fs::remove_all(base, ec);
    fs::create_directories(base, ec);

    // ---- G1 基本解析：引号里的逗号 / 换行 / 转义引号 ----
    {
        core::CsvTable t;
        std::string err;
        const std::string text =
            "id,name,hp,note\r\n"
            "slime,\"Slime, green\",12,\"says \"\"hi\"\"\"\r\n"
            "boss,\"Big\r\nBoss\",900,\r\n";
        check(core::parseCsv(text, t, &err), "G1 parse-quoted", err);
        check(t.header.size() == 4 && t.header[1] == "name",
              "G1b header", std::to_string(t.header.size()));
        check(t.rows.size() == 2, "G1c two-rows", std::to_string(t.rows.size()));
        check(t.rows[0][1] == "Slime, green", "G1d comma-in-quotes",
              t.rows[0][1]);
        check(t.rows[0][3] == "says \"hi\"", "G1e escaped-quote", t.rows[0][3]);
        check(t.rows[1][1] == "Big\r\nBoss", "G1f newline-in-quotes",
              t.rows[1][1]);
    }

    // ---- G2 短行补齐（手写表常见）----
    {
        core::CsvTable t;
        std::string err;
        core::parseCsv("a,b,c\n1,2\n", t, &err);
        check(t.rows.size() == 1 && t.rows[0].size() == 3,
              "G2 short-row-padded",
              std::to_string(t.rows.empty() ? 0 : t.rows[0].size()));
    }

    // ---- G3 BOM 与末尾空行 ----
    {
        core::CsvTable t;
        std::string err;
        core::parseCsv("\xEF\xBB\xBFid,v\nx,1\n\n", t, &err);
        check(t.header.size() == 2 && t.header[0] == "id", "G3 bom-stripped",
              t.header.empty() ? "" : t.header[0]);
        check(t.rows.size() == 1, "G3b trailing-blank-skipped",
              std::to_string(t.rows.size()));
    }

    // ---- G4 未闭合引号 → 失败并报行号 ----
    {
        core::CsvTable t;
        std::string err;
        const bool ok = core::parseCsv("a\n\"oops\n", t, &err);
        check(!ok && err.find("line") != std::string::npos,
              "G4 unterminated-quote-detected", err);
    }

    // ---- G5 写→读 往返（含需要加引号的字段）----
    {
        core::CsvTable a;
        a.header = {"id", "text"};
        a.rows = {{"x", "a,b"}, {"y", "say \"hi\""}, {"z", "line\nbreak"}};
        const std::string text = core::writeCsv(a);
        core::CsvTable b;
        std::string err;
        check(core::parseCsv(text, b, &err), "G5 roundtrip-parse", err);
        bool same = b.rows.size() == a.rows.size();
        for (size_t i = 0; same && i < a.rows.size(); ++i) {
            same = b.rows[i] == a.rows[i];
        }
        check(same, "G5b roundtrip-identical", text);
    }

    // ---- G6 取值：数值 / 布尔 / 越界 / 按 id ----
    {
        DataTable t;
        t.name = "t";
        t.data.header = {"id", "#note", "hp", "alive"};
        t.data.rows = {{"slime", "starter", "12.5", "1"},
                       {"boss", "", "900", "no"}};
        check(t.rowById("boss") == 1, "G6 row-by-id",
              std::to_string(t.rowById("boss")));
        check(t.rowById("nope") == -1, "G6b row-by-id-miss", "");
        check(t.num(0, "hp") == 12.5, "G6c num", std::to_string(t.num(0, "hp")));
        check(t.flag(0, "alive") && !t.flag(1, "alive"), "G6d flags", "");
        // 越界 → 默认值，不崩
        check(t.num(99, "hp", -1.0) == -1.0, "G6e out-of-range-default", "");
        // 空单元格：str 返回空串（**不**回退默认值）
        check(t.str(1, "#note", "DEF").empty(), "G6f empty-cell-is-empty",
              t.str(1, "#note", "DEF"));
    }

    fs::remove_all(base, ec);
    std::printf("TABLE-TEST SUMMARY pass=%d fail=%d\n", pass, fail);
    return fail;
}

} // namespace project
