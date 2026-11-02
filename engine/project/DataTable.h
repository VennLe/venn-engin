#pragma once

#include "core/Csv.h"

#include <string>
#include <vector>

namespace project {

// 一张配表 = `assets/data/<name>.csv`。
//
// 为什么是 CSV 而不是 JSON：策划要能用 Excel / WPS 打开、能交给不懂代码的人、
// 能在 diff 里一行一行看。JSON 表在这三件事上全输。
//
// 列约定：
//   · 第一行 = 列名（英文标识符，**不翻译**）；
//   · 列名里带 `#` 前缀的列（例如 `#note`）是**注释列**：加载时保留、界面上
//     显示、但不参与任何类型检查 —— 策划常要写"这列为什么是这个值"。
//   · 没有类型行。类型由**访问函数**决定（`num` / `str` / `flag`）——
//     多一行类型声明就多一处要同步的地方，而 CSV 里那一行策划经常忘改。
//     类型不对时返回默认值并**记一次警告**（不抛异常：一张表里有一格写错
//     不该让整个场景加载失败）。
struct DataTable {
    std::string name;   // 文件名去掉 .csv
    std::string path;   // 绝对路径
    core::CsvTable data;

    int column(const std::string& col) const { return data.columnIndex(col); }
    size_t rows() const { return data.count(); }
    size_t cols() const { return data.header.size(); }

    const std::string& raw(size_t r, int c) const;
    double num(size_t r, int c, double def = 0.0) const;
    std::string str(size_t r, int c, const std::string& def = {}) const;
    bool flag(size_t r, int c, bool def = false) const;

    // 按列名取（配表里最常用的写法）。列不存在时记一次警告并返回默认值。
    double num(size_t r, const char* col, double def = 0.0) const;
    std::string str(size_t r, const char* col, const std::string& def = {}) const;
    bool flag(size_t r, const char* col, bool def = false) const;

    // 第一列当 id：按 id 找行，找不到返回 -1。
    // ⚠ 配表的**主要用法**是这个 —— "按名字取一行的某个字段"，
    //   而不是"按下标"。下标会随策划插入一行全体错位。
    int rowById(const std::string& id) const;
};

// 全进程的表集合（单例）。
//
// 为什么允许单例：配表是**只读的进程级数据**，脚本内建函数（`tableNum`）
// 也在查它 —— 把它顺着 ScriptRuntime 一路传下去要改四五个签名，而收益是
// "可以有两个表集合"，那个能力没人需要。
class TableStore {
public:
    static TableStore& get();

    // 扫描目录下所有 `.csv`（非递归）。不存在该目录 = 成功 + 0 张表
    // （新工程没有配表是正常状态，不该报错）。
    bool loadAll(const std::string& dir, std::string* err);
    bool loadOne(const std::string& path, std::string* err);

    DataTable* find(const std::string& name);
    const std::vector<DataTable>& all() const { return m_tables; }
    std::vector<DataTable>& allMutable() { return m_tables; }

    // 落盘（原子写）。成功后同步 `path`。
    bool save(DataTable& t, std::string* err);

    std::string dir() const { return m_dir; }

private:
    std::vector<DataTable> m_tables;
    std::string m_dir;
};

// ---------------------------------------------------------------- 按 key 查
//
// `"表.行.列"` 的一条字符串寻址。**脚本内建 `tableNum/tableStr/tableHas`
// 唯一的实现就在这里** —— 两处各写一份切分逻辑的话，"行 id 带点"这种
// 边角一定是一边对一边错，而错的那一边症状是"这个敌人查不到数值"。
//
// 切分规则：**第一个点**切表名、**最后一个点**切列名，中间全是行 id。
//   `"cards.strike.cost"`        → cards / strike / cost
//   `"cards.goblin.chief.cost"`  → cards / goblin.chief / cost
// 少于两个点 → false（这一格不存在）。
//
// ⚠ 找不到时**不记日志**：脚本可能每帧查一次，记日志会刷屏；而且
//   "找不到"与"值是 0"的区分由 `tableHas()` 负责 —— 那才是诊断入口。
bool splitTableKey(const std::string& key, std::string* table, std::string* row,
                   std::string* col);

// 保证配表**已经扫描过至少一次**（只有第一次真的扫盘）。返回加载到的表数。
//
// ⚠⚠ 编辑器与运行时都必须在装好脚本服务**之前**调它。不调的话
//   `tableNum` 会静默返回 0 / `tableStr` 返回 ""，而"表一个都没加载"
//   与"表里那一格就是 0"在脚本里长得一模一样 —— 症状是
//   "配表里的数值全是 0"，日志一行错都没有。
//   （`tableHas` 是给作者自查用的，但它不该是唯一的防线。）
int ensureTablesLoaded();

double tableNum(const std::string& key);            // 找不到 → 0
std::string tableStr(const std::string& key);       // 找不到 → ""
bool tableHas(const std::string& key);              // 表/行/列任一不存在 → false
int tableRows(const std::string& tableName);        // 表不存在 → **-1**

// 自检（无窗口）。G1~G6，见 .cpp。
int runDataTableSelfTest();

} // namespace project
