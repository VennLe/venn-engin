#include "LocaleFile.h"

#include "i18n/I18n.h"

#include "core/Csv.h"
#include "core/Logger.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>

namespace fs = std::filesystem;

namespace i18n {

namespace {

// 内置表 → map（只在 load / builtinOf 里建一次）
const std::map<std::string, std::string>& builtinMap() {
    static const std::map<std::string, std::string> m = [] {
        std::map<std::string, std::string> x;
        for (std::size_t i = 0; i < kZhCount; ++i) {
            if (kZh[i].en && kZh[i].zh) x[kZh[i].en] = kZh[i].zh;
        }
        return x;
    }();
    return m;
}

} // namespace

std::string LocaleEditor::builtinOf(const std::string& en) const {
    const auto& m = builtinMap();
    auto it = m.find(en);
    return it == m.end() ? std::string() : it->second;
}

bool LocaleEditor::load(const std::string& path, std::string* err) {
    m_path = path;
    m_rows.clear();

    // ---- ① 内置行（顺序 = 静态表顺序，面板看起来与 ZhTable.cpp 一致）----
    const auto& bm = builtinMap();
    for (std::size_t i = 0; i < kZhCount; ++i) {
        if (!kZh[i].en || !kZh[i].zh) continue;
        LocaleRow r;
        r.en = kZh[i].en;
        r.zh = kZh[i].zh;
        r.builtin = true;
        m_rows.push_back(std::move(r));
    }

    // ---- ② 覆盖层 ----
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        VK_LOG_INFO("I18N-CSV path=%s missing (builtin only, %zu rows)",
                    path.c_str(), m_rows.size());
        apply();
        return true;
    }
    core::CsvTable csv;
    if (!core::loadCsvFile(path, csv, err)) return false;

    const int ci = csv.columnIndex("en");
    const int cz = csv.columnIndex("zh");
    if (ci < 0 || cz < 0) {
        if (err) *err = path + ": header must contain 'en' and 'zh'";
        return false;
    }
    int applied = 0;
    for (auto& row : csv.rows) {
        const std::string en =
            static_cast<std::size_t>(ci) < row.size() ? row[ci] : std::string();
        const std::string zh =
            static_cast<std::size_t>(cz) < row.size() ? row[cz] : std::string();
        if (en.empty()) continue;
        auto it = std::find_if(m_rows.begin(), m_rows.end(),
                               [&](const LocaleRow& r) { return r.en == en; });
        if (it != m_rows.end()) {
            it->zh = zh;
            // ⚠ dirty 由"与内置值是否相同"重新算，而不是"CSV 里有这行"——
            //   CSV 里留一行与内置值完全相同的记录，看起来像覆盖、其实
            //   什么都没改；再存一次会把它清掉（这是**对的**：它确实
            //   没有信息量）。
            it->dirty = (it->zh != builtinOf(en));
        } else {
            LocaleRow r;
            r.en = en;
            r.zh = zh;
            r.builtin = false;
            r.dirty = true;   // 新增的键必须写回去，否则下次就没了
            m_rows.push_back(std::move(r));
        }
        ++applied;
    }
    VK_LOG_INFO("I18N-CSV path=%s rows=%zu overrides=%d", path.c_str(),
                m_rows.size(), applied);
    apply();
    return true;
}

bool LocaleEditor::save(std::string* err) {
    core::CsvTable csv;
    csv.header = {"en", "zh"};
    size_t n = 0;
    for (const auto& r : m_rows) {
        if (!r.dirty) continue;
        // 新增的键即使译文为空也写（"这个键我认领了，还没翻"是一个真实
        // 状态；不写的话下次加载就看不到它了）。
        csv.rows.push_back({r.en, r.zh});
        ++n;
    }
    if (!core::saveCsvFile(m_path, csv, err)) return false;
    VK_LOG_INFO("I18N-CSV-SAVE path=%s overrides=%zu", m_path.c_str(), n);
    return true;
}

bool LocaleEditor::set(const std::string& en, const std::string& zh) {
    if (en.empty()) return false;
    auto it = std::find_if(m_rows.begin(), m_rows.end(),
                           [&](const LocaleRow& r) { return r.en == en; });
    if (it == m_rows.end()) {
        LocaleRow r;
        r.en = en;
        r.zh = zh;
        r.builtin = false;
        r.dirty = true;
        m_rows.push_back(std::move(r));
    } else {
        it->zh = zh;
        it->dirty = it->builtin ? (it->zh != builtinOf(en)) : true;
    }
    apply();
    return true;
}

bool LocaleEditor::revert(const std::string& en) {
    auto it = std::find_if(m_rows.begin(), m_rows.end(),
                           [&](const LocaleRow& r) { return r.en == en; });
    if (it == m_rows.end()) return false;
    if (it->builtin) {
        it->zh = builtinOf(en);
        it->dirty = false;
    } else {
        m_rows.erase(it);
    }
    apply();
    return true;
}

size_t LocaleEditor::missingCount() const {
    size_t n = 0;
    for (const auto& r : m_rows) {
        if (r.zh.empty()) ++n;
    }
    return n;
}

size_t LocaleEditor::dirtyCount() const {
    size_t n = 0;
    for (const auto& r : m_rows) {
        if (r.dirty) ++n;
    }
    return n;
}

void LocaleEditor::apply() {
    // ⚠ 必须**先清再刷**：`addTranslation` 是"后写覆盖"，但撤销一条覆盖
    //   （revert）没法用"再写一次旧值"表达 —— 旧值就是内置值，写进去等于
    //   没撤销。清空重放是唯一能让"撤销"真的生效的做法。
    //   代价：其它模块（模组 / 脚本）如果也调过 `addTranslation`，会一起
    //   被清掉。目前全仓只有这里调它（`grep addTranslation` 可证），
    //   将来真有人用，这里要改成"按 key 撤销"。
    clearAddedTranslations();
    size_t n = 0;
    for (const auto& r : m_rows) {
        if (!r.dirty) continue;
        addTranslation(r.en.c_str(), r.zh.c_str());
        ++n;
    }
    if (n) VK_LOG_INFO("I18N-OVERRIDE applied=%zu", n);
}

// ---------------------------------------------------------------- 自检

int runLocaleSelfTest() {
    int pass = 0;
    int fail = 0;
    auto check = [&](bool ok, const char* name, const std::string& detail) {
        if (ok) {
            ++pass;
            std::printf("LOCALE-TEST PASS %s\n", name);
        } else {
            ++fail;
            std::printf("LOCALE-TEST FAIL %s (%s)\n", name, detail.c_str());
        }
    };

    std::error_code ec;
    const fs::path base = fs::temp_directory_path(ec) / "venn_locale_test";
    fs::remove_all(base, ec);
    fs::create_directories(base, ec);
    const std::string p = (base / "zh.csv").generic_string();

    // ---- L1 没有 CSV → 只有内置行，不报错 ----
    LocaleEditor e;
    std::string err;
    check(e.load(p, &err), "L1 load-missing-ok", err);
    check(e.count() == kZhCount, "L1b builtin-rows",
          std::to_string(e.count()));

    // 找一个内置键来改
    std::string key;
    std::string bz;
    for (size_t i = 0; i < e.count(); ++i) {
        if (e.row(i).builtin && !e.row(i).en.empty()) {
            key = e.row(i).en;
            bz = e.row(i).zh;
            break;
        }
    }
    check(!key.empty(), "L1c found-a-builtin-key", "");

    // ---- L2 改一条 → dirty，apply 之后 tr() 真的变 ----
    check(e.set(key, "改了"), "L2 set-ok", "");
    check(e.dirtyCount() == 1, "L2b dirty-one",
          std::to_string(e.dirtyCount()));
    setLanguage(1);   // 中文
    check(std::string(tr(key.c_str())) == "改了", "L2c tr-applied",
          tr(key.c_str()));

    // ---- L3 只写覆盖（内置的其它行不许进 CSV）----
    check(e.save(&err), "L3 save-ok", err);
    {
        core::CsvTable csv;
        check(core::loadCsvFile(p, csv, &err), "L3b csv-readable", err);
        check(csv.rows.size() == 1, "L3c only-overrides-written",
              std::to_string(csv.rows.size()));
        check(csv.rows.size() == 1 && csv.rows[0][0] == key,
              "L3d the-right-row", csv.rows.empty() ? "" : csv.rows[0][0]);
    }

    // ---- L4 撤销 → 回到内置值，且 dirty 归零 ----
    check(e.revert(key), "L4 revert-ok", "");
    check(e.dirtyCount() == 0, "L4b dirty-cleared",
          std::to_string(e.dirtyCount()));
    check(std::string(tr(key.c_str())) == bz, "L4c tr-back-to-builtin",
          tr(key.c_str()));

    // ---- L5 重新加载 + 新增键 ----
    check(e.set("a brand new key", "全新的键"), "L5 add-new-key", "");
    check(e.save(&err), "L5b save-new", err);
    LocaleEditor e2;
    check(e2.load(p, &err), "L5c reload", err);
    check(e2.count() == kZhCount + 1, "L5d one-extra-row",
          std::to_string(e2.count()));
    check(e2.dirtyCount() == 1, "L5e new-key-persisted",
          std::to_string(e2.dirtyCount()));

    clearAddedTranslations();
    setLanguage(0);
    fs::remove_all(base, ec);
    std::printf("LOCALE-TEST SUMMARY pass=%d fail=%d\n", pass, fail);
    return fail;
}

} // namespace i18n
