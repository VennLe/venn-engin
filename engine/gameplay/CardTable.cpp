#include "gameplay/CardTable.h"

#include "core/Logger.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace gameplay {

namespace {

// 列名**一处定义**：`cardsFromCsv` 按这些名字找列，自检与探针也读它。
// ⚠ 加列 = 这里加一项 + `CardDef` 加字段 + `docs/cards.md` 加一行。
const char* const kCardColumns[kCardColumnCount] = {
    "id",         "#note",  "name",        "kind",    "target",  "cost",
    "exhaust",    "unplayable", "upgradedCost", "text", "effects", nullptr,
};

std::string trimCopy(const std::string& s) {
    std::size_t a = 0;
    std::size_t b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::vector<std::string> splitOn(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) {
            out.push_back(trimCopy(cur));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(trimCopy(cur));
    return out;
}

// 数值 → 紧凑串：`6` 而不是 `6.000000`（判据要靠它做往返比对）。
std::string numToText(float v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", static_cast<double>(v));
    return std::string(buf);
}

bool hasSpace(const std::string& s) {
    for (char c : s) {
        if (std::isspace(static_cast<unsigned char>(c))) return true;
    }
    return false;
}

} // namespace

const char* cardColumnName(int index) {
    if (index < 0 || index >= kCardColumnCount) return "";
    const char* n = kCardColumns[index];
    return n ? n : "";
}

// ---------------------------------------------------------------- 效果串

bool parseCardEffect(const std::string& text, CardEffect& out, std::string* err) {
    const std::string s = trimCopy(text);
    if (s.empty()) {
        if (err) *err = "empty effect";
        return false;
    }
    // 最多 5 段：kind:target:amount:type:count。
    // 超出的段当错误 —— 静默丢掉第 6 段会让"多写了一个字段"看起来像
    // "这张牌少了一条效果"。
    const std::vector<std::string> f = splitOn(s, ':');
    if (f.size() > 5) {
        if (err) *err = "'" + s + "': too many fields (max 5)";
        return false;
    }

    CardEffect e;
    if (!parseCardEffectKind(f[0], e.kind)) {
        if (err) *err = "'" + s + "': unknown effect kind '" + f[0] + "'";
        return false;
    }
    if (f.size() > 1) e.target = f[1];
    if (f.size() > 2 && !f[2].empty()) {
        double v = 0.0;
        if (!core::parseNumber(f[2], v)) {
            if (err) *err = "'" + s + "': amount '" + f[2] + "' is not a number";
            return false;
        }
        e.amount = static_cast<float>(v);
    }
    if (f.size() > 3) e.type = f[3];
    if (f.size() > 4 && !f[4].empty()) {
        double v = 0.0;
        if (!core::parseNumber(f[4], v)) {
            if (err) *err = "'" + s + "': count '" + f[4] + "' is not a number";
            return false;
        }
        // `count <= 0` 会被当成"一条都不触发"，也就是这张牌静默失效。
        // 配表里写 0 的意图几乎一定是"先占位"，而占位的正确后果是
        // **能看出来**（这条效果不存在），所以钳到 1 并留痕在调用方。
        e.count = (v < 1.0) ? 1 : static_cast<int>(v);
    }
    out = e;
    return true;
}

bool parseCardEffects(const std::string& text, std::vector<CardEffect>& out,
                      std::string* err) {
    const std::string s = trimCopy(text);
    if (s.empty()) return true;   // 没有效果 = 合法（"这张牌只是占位"）
    for (const std::string& one : splitOn(s, '|')) {
        if (one.empty()) continue;   // `a||b` 里多写的竖线不算错
        CardEffect e;
        if (!parseCardEffect(one, e, err)) return false;
        out.push_back(e);
    }
    return true;
}

std::string formatCardEffects(const std::vector<CardEffect>& fx) {
    std::string out;
    for (std::size_t i = 0; i < fx.size(); ++i) {
        const CardEffect& e = fx[i];
        if (i) out.push_back('|');
        out += cardEffectKindName(e.kind);
        out.push_back(':');
        out += e.target;
        out.push_back(':');
        out += numToText(e.amount);
        out.push_back(':');
        out += e.type;
        out.push_back(':');
        char cnt[16];
        std::snprintf(cnt, sizeof(cnt), "%d", e.count);
        out += cnt;
    }
    return out;
}

// ---------------------------------------------------------------- 整表

bool CardTableResult::has(const std::string& id) const {
    for (const CardDef& d : defs) {
        if (d.id == id) return true;
    }
    return false;
}

CardTableResult cardsFromCsv(const core::CsvTable& t) {
    CardTableResult r;

    // 列下标预先查一遍：每行都 `columnIndex()` 的话，一张 200 行的表要
    // 重建 200 次列名映射，而列名数量固定。更重要的是——缺列能在**进入
    // 循环之前**就报出来，而不是每行报一次同样的错。
    int idx[kCardColumnCount];
    for (int i = 0; i < kCardColumnCount; ++i) {
        const char* name = cardColumnName(i);
        idx[i] = (name && name[0]) ? t.columnIndex(name) : -1;
    }
    const int iId = idx[0];
    const int iName = idx[2];
    const int iKind = idx[3];
    const int iTarget = idx[4];
    const int iCost = idx[5];
    const int iExhaust = idx[6];
    const int iUnplay = idx[7];
    const int iUpCost = idx[8];
    const int iText = idx[9];
    const int iFx = idx[10];

    if (iId < 0) {
        // 没有 id 列 = 这根本不是卡牌表。**不返回半份数据**：没有 id 的
        // 卡在牌堆里连"是哪张牌"都答不上来。
        r.errors.push_back("missing column 'id'");
        VK_LOG_ERROR("CARDTABLE missing column 'id' (cols=%zu)", t.header.size());
        return r;
    }

    auto cell = [&](const std::vector<std::string>& row, int c) -> std::string {
        if (c < 0 || static_cast<std::size_t>(c) >= row.size()) return std::string();
        return row[static_cast<std::size_t>(c)];
    };

    for (std::size_t ri = 0; ri < t.rows.size(); ++ri) {
        const std::vector<std::string>& row = t.rows[ri];
        const std::string id = trimCopy(cell(row, iId));
        if (id.empty()) {
            // 空行 / 只有注释的行：**静默跳过**。策划留空行是常态，
            // 每行报一条 WARN 会把真正的错误埋掉。
            continue;
        }
        // 行号给**人**看的那个（表头占第 1 行，所以 +2）。
        const std::size_t humanRow = ri + 2;

        if (hasSpace(id)) {
            r.errors.push_back("row " + std::to_string(humanRow) +
                               ": id '" + id + "' contains a space");
            ++r.skipped;
            continue;
        }
        if (r.has(id)) {
            // 重复 id：**保留第一份**。若让后者覆盖前者，Excel 里复制粘贴
            // 少改一格就会出现"我改了卡 A，实际生效的是卡 B"。
            r.errors.push_back("row " + std::to_string(humanRow) +
                               ": duplicate id '" + id + "' (kept the first)");
            ++r.skipped;
            continue;
        }

        CardDef d;
        d.id = id;
        d.name = cell(row, iName);
        if (d.name.empty()) d.name = id;   // 没写显示名 = 用 id，不是空字符串
        d.text = cell(row, iText);

        const std::string kindText = trimCopy(cell(row, iKind));
        if (!kindText.empty() && !parseCardKind(kindText, d.kind)) {
            r.errors.push_back("row " + std::to_string(humanRow) + " (" + id +
                               "): unknown kind '" + kindText +
                               "' (using attack)");
        }
        const std::string targetText = trimCopy(cell(row, iTarget));
        if (!targetText.empty() && !parseCardTarget(targetText, d.target)) {
            r.errors.push_back("row " + std::to_string(humanRow) + " (" + id +
                               "): unknown target '" + targetText +
                               "' (using enemy)");
        }

        const std::string costText = trimCopy(cell(row, iCost));
        if (!costText.empty()) {
            double v = 0.0;
            if (core::parseNumber(costText, v)) d.cost = static_cast<int>(v);
            else r.errors.push_back("row " + std::to_string(humanRow) + " (" +
                                    id + "): cost '" + costText +
                                    "' is not a number (using 1)");
        }

        // 旗标列：空 = 默认。`parseBool` 认 `1/0/true/false/yes/no`。
        const std::string exText = trimCopy(cell(row, iExhaust));
        if (!exText.empty()) {
            bool b = false;
            if (core::parseBool(exText, b)) d.exhaust = b;
            else r.errors.push_back("row " + std::to_string(humanRow) + " (" +
                                    id + "): exhaust '" + exText +
                                    "' is not a bool");
        }
        const std::string unText = trimCopy(cell(row, iUnplay));
        if (!unText.empty()) {
            bool b = false;
            if (core::parseBool(unText, b)) d.unplayable = b;
            else r.errors.push_back("row " + std::to_string(humanRow) + " (" +
                                    id + "): unplayable '" + unText +
                                    "' is not a bool");
        }
        const std::string upText = trimCopy(cell(row, iUpCost));
        if (!upText.empty()) {
            double v = 0.0;
            if (core::parseNumber(upText, v)) d.upgradedCost = static_cast<int>(v);
            else r.errors.push_back("row " + std::to_string(humanRow) + " (" +
                                    id + "): upgradedCost '" + upText +
                                    "' is not a number");
        }

        std::string fxErr;
        if (!parseCardEffects(cell(row, iFx), d.effects, &fxErr)) {
            // ⚠ 效果写错 → **整行跳过**（而不是"这张牌没有效果"）。
            //   静默空效果的表现是"这张牌打出去什么也不发生"，
            //   而那与"配表没生效"完全一样。
            r.errors.push_back("row " + std::to_string(humanRow) + " (" + id +
                               "): " + fxErr);
            ++r.skipped;
            continue;
        }

        r.defs.push_back(std::move(d));
    }

    for (const std::string& e : r.errors) {
        VK_LOG_WARN("CARDTABLE %s", e.c_str());
    }
    VK_LOG_INFO("CARDTABLE defs=%zu skipped=%d errors=%zu", r.defs.size(),
                r.skipped, r.errors.size());
    return r;
}

// ---------------------------------------------------------------- 自检

int runCardTableSelfTest(int* passOut) {
    int pass = 0;
    int fail = 0;
    auto check = [&](bool ok, const char* name, const std::string& detail) {
        if (ok) {
            ++pass;
            VK_LOG_INFO("GAMEPLAY-TEST %s actual=1 expected=1 tol=0 detail=%s "
                        "result=PASS",
                        name, detail.c_str());
        } else {
            ++fail;
            VK_LOG_INFO("GAMEPLAY-TEST %s actual=0 expected=1 tol=0 detail=%s "
                        "result=FAIL",
                        name, detail.c_str());
        }
    };

    // N1 效果串单条
    {
        CardEffect e;
        std::string err;
        check(parseCardEffect("damage::6:physical", e, &err) &&
                  e.kind == CardEffect::Kind::Damage && e.amount == 6.0f &&
                  e.target.empty() && e.type == "physical",
              "N1.effect-basic", err);
    }
    // N2 目标字段非空
    {
        CardEffect e;
        std::string err;
        check(parseCardEffect("status:enemy:2:vulnerable", e, &err) &&
                  e.kind == CardEffect::Kind::Status && e.target == "enemy" &&
                  e.amount == 2.0f && e.type == "vulnerable",
              "N2.effect-target", err);
    }
    // N3 空字段占位（`draw::2`：中间那个空字段才是"没有目标"）
    {
        CardEffect e;
        std::string err;
        check(parseCardEffect("draw::2", e, &err) &&
                  e.kind == CardEffect::Kind::Draw && e.amount == 2.0f,
              "N3.effect-empty-fields", err);
    }
    // N3b 反例：多写一个冒号（`draw:::2`）→ `2` 落到 **type**、amount 是空。
    //     文档第一版就是那么写的，而它的症状是"这张牌只抽 1 张"（amount 空
    //     → 0 → 钳到 1），一点也不像"效果串写错了"。这条判据钉住位置语义：
    //     哪天有人把解析改成"跳过空字段"，它会立刻红。
    {
        CardEffect e;
        std::string err;
        check(parseCardEffect("draw:::2", e, &err) && e.amount == 0.0f &&
                  e.type == "2",
              "N3b.colon-shift-trap", err);
    }
    // N4 count 字段
    {
        CardEffect e;
        std::string err;
        check(parseCardEffect("damage:enemy:4:fire:3", e, &err) && e.count == 3 &&
                  e.amount == 4.0f,
              "N4.effect-count", err);
    }
    // N5 认不出的 kind → false（**不猜**）
    {
        CardEffect e;
        std::string err;
        check(!parseCardEffect("bogus::1", e, &err) && !err.empty(),
              "N5.effect-unknown-kind", err);
    }
    // N6 数值不是数字 → false
    {
        CardEffect e;
        std::string err;
        check(!parseCardEffect("damage::abc", e, &err), "N6.effect-bad-amount", err);
    }
    // N7 字段过多 → false
    {
        CardEffect e;
        std::string err;
        check(!parseCardEffect("damage::1:physical:1:x", e, &err),
              "N7.effect-too-many", err);
    }
    // N8 多条 + 空串
    {
        std::vector<CardEffect> fx;
        std::string err;
        const bool ok = parseCardEffects("damage::6:physical|status::2:vulnerable", fx, &err);
        check(ok && fx.size() == 2 && fx[1].type == "vulnerable", "N8.effects-two",
              err);
    }
    // N9 空效果串 = 合法 + 0 条
    {
        std::vector<CardEffect> fx;
        std::string err;
        check(parseCardEffects("", fx, &err) && fx.empty(), "N9.effects-empty", err);
    }
    // N10 往返：format → parse 逐字段相等
    {
        std::vector<CardEffect> a;
        a.push_back(CardEffect{});
        a[0].kind = CardEffect::Kind::Damage;
        a[0].target = "enemy";
        a[0].amount = 4.5f;
        a[0].type = "fire";
        a[0].count = 3;
        a.push_back(CardEffect{});
        a[1].kind = CardEffect::Kind::Draw;
        a[1].amount = 2.0f;
        const std::string text = formatCardEffects(a);
        std::vector<CardEffect> b;
        std::string err;
        const bool ok = parseCardEffects(text, b, &err);
        bool same = ok && b.size() == a.size();
        for (std::size_t i = 0; same && i < a.size(); ++i) {
            same = b[i].kind == a[i].kind && b[i].target == a[i].target &&
                   b[i].amount == a[i].amount && b[i].type == a[i].type &&
                   b[i].count == a[i].count;
        }
        check(same, "N10.effects-roundtrip", text);
    }
    // N11 整表：三行好 + 一行缺 id
    {
        core::CsvTable t;
        t.header = {"id", "#note", "name", "kind", "target", "cost", "exhaust",
                    "unplayable", "upgradedCost", "text", "effects"};
        t.rows = {
            {"strike", "", "Strike", "attack", "enemy", "1", "0", "0", "", "deal 6", "damage::6:physical"},
            {"defend", "", "Defend", "skill", "self", "1", "", "", "", "", "shield::5:"},
            {"", "空行", "", "", "", "", "", "", "", "", ""},
            {"bash", "", "Bash", "attack", "enemy", "2", "1", "0", "1", "", "damage::8:physical|status::2:vulnerable"},
        };
        const CardTableResult r = cardsFromCsv(t);
        check(r.defs.size() == 3, "N11.table-rows", std::to_string(r.defs.size()));
    }
    // N12 字段值：exhaust / upgradedCost / target
    {
        core::CsvTable t;
        t.header = {"id", "name", "kind", "target", "cost", "exhaust",
                    "upgradedCost", "effects"};
        t.rows = {{"bash", "Bash", "attack", "enemy", "2", "1", "1",
                   "damage::8:physical|status::2:vulnerable"}};
        const CardTableResult r = cardsFromCsv(t);
        const bool ok = r.defs.size() == 1 && r.defs[0].exhaust &&
                        r.defs[0].upgradedCost == 1 && r.defs[0].cost == 2 &&
                        r.defs[0].effects.size() == 2;
        check(ok, "N12.table-fields", std::to_string(r.defs.size()));
    }
    // N13 名字留空 → 用 id（不是空串）
    {
        core::CsvTable t;
        t.header = {"id", "effects"};
        t.rows = {{"kick", "damage::3:physical"}};
        const CardTableResult r = cardsFromCsv(t);
        check(r.defs.size() == 1 && r.defs[0].name == "kick", "N13.table-name",
              r.defs.empty() ? "" : r.defs[0].name);
    }
    // N14 缺 id 列 → 0 条 + 1 条错误
    {
        core::CsvTable t;
        t.header = {"name", "cost"};
        t.rows = {{"X", "1"}};
        const CardTableResult r = cardsFromCsv(t);
        check(r.defs.empty() && r.errors.size() == 1, "N14.table-no-id",
              std::to_string(r.errors.size()));
    }
    // N15 重复 id → 保留第一份
    {
        core::CsvTable t;
        t.header = {"id", "cost", "effects"};
        t.rows = {{"a", "1", "damage::1:physical"}, {"a", "9", "damage::9:physical"}};
        const CardTableResult r = cardsFromCsv(t);
        check(r.defs.size() == 1 && r.defs[0].cost == 1 && r.skipped == 1,
              "N15.table-dup", std::to_string(r.defs.size()));
    }
    // N16 效果写错 → 整行跳过（不是"空效果"）
    {
        core::CsvTable t;
        t.header = {"id", "effects"};
        t.rows = {{"bad", "bogus::1"}, {"good", "damage::2:physical"}};
        const CardTableResult r = cardsFromCsv(t);
        check(r.defs.size() == 1 && r.defs[0].id == "good" && r.skipped == 1,
              "N16.table-bad-fx", std::to_string(r.defs.size()));
    }
    // N17 id 带空格 → 拒绝（文本存档按空格切字段）
    {
        core::CsvTable t;
        t.header = {"id", "effects"};
        t.rows = {{"two words", "damage::1:physical"}};
        const CardTableResult r = cardsFromCsv(t);
        check(r.defs.empty() && r.skipped == 1, "N17.table-space-id",
              std::to_string(r.defs.size()));
    }
    // N18 列名表本身：11 项且首项是 id（文档/探针读它）
    {
        check(std::strcmp(cardColumnName(0), "id") == 0 &&
                  std::strcmp(cardColumnName(kCardColumnCount - 1), "") == 0,
              "N18.column-names", cardColumnName(0));
    }

    VK_LOG_INFO("CARDTABLE-TEST pass=%d fail=%d", pass, fail);
    if (passOut) *passOut = pass;
    return fail;
}

} // namespace gameplay
