#include "project/CardSource.h"

#include "core/Logger.h"
#include "gameplay/CardTable.h"
#include "project/DataTable.h"

namespace project {

CardSourceResult loadCardDefs(std::vector<gameplay::CardDef>& out,
                              const std::string& tableName) {
    CardSourceResult r;
    r.table = tableName;

    // ⚠ 先保证扫过盘。不调的话 `find()` 永远返回 nullptr，而症状是
    //   "配表里的卡一张都不认"（`defs=0`），与"表真的不存在"无法区分。
    ensureTablesLoaded();
    TableStore& store = TableStore::get();
    r.dir = store.dir();

    DataTable* t = store.find(tableName);
    if (!t) {
        VK_LOG_INFO("CARDDEFS table=%s found=0 dir=%s defs=0 "
                    "(no such table — 卡牌会保持 known=0)",
                    tableName.c_str(), r.dir.c_str());
        out.clear();
        return r;
    }

    r.tableFound = true;
    const gameplay::CardTableResult parsed = gameplay::cardsFromCsv(t->data);
    out = parsed.defs;
    r.defs = static_cast<int>(out.size());
    r.skipped = parsed.skipped;
    r.errors = static_cast<int>(parsed.errors.size());

    VK_LOG_INFO("CARDDEFS table=%s found=1 path=%s defs=%d skipped=%d errors=%d",
                tableName.c_str(), t->path.c_str(), r.defs, r.skipped, r.errors);
    return r;
}

} // namespace project
