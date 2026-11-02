#pragma once

#include <string>
#include <vector>

namespace core {

// 一张 CSV 表：第一行是表头，其余是数据行。
//
// 为什么要有它（而不是各面板自己 `split(',')`）：配表（C1）与本地化（C2）
// 是同一种文件格式的两种用途，**解析器只能有一份**。分开写的话，
// 引号里的逗号这种边角一定是一边对一个错 —— 而错的那一半会把一句
// 带逗号的台词从中间劈开，界面上看起来只是"多了一列"。
struct CsvTable {
    std::vector<std::string> header;
    std::vector<std::vector<std::string>> rows;

    int columnIndex(const std::string& name) const;
    size_t count() const { return rows.size(); }
};

// RFC 4180 的实用子集：
//   · 双引号包裹的字段可以含逗号 / 换行；
//   · `""` 表示一个字面双引号；
//   · CRLF / LF / CR 都当行尾；
//   · 开头 3 字节 BOM 会被吃掉（Excel 导出的 UTF-8 CSV 一定带它，
//     不吃的话第一个列名会变成 "\xEF\xBB\xBFen" —— 永远匹配不上）。
// 解析失败（引号没闭合）返回 false 并给出**行号**。
bool parseCsv(const std::string& text, CsvTable& out, std::string* err);

// 写回：行尾统一 CRLF（Excel 打开不乱行），含逗号/引号/换行的字段自动加引号。
std::string writeCsv(const CsvTable& t);

bool loadCsvFile(const std::string& path, CsvTable& out, std::string* err);
// 原子写（tmp → rename），与 `project::saveGameFile` 同一套路：
// 直接截断目标再写，半途失败会留下半截表 —— 那比"没保存"糟得多。
bool saveCsvFile(const std::string& path, const CsvTable& t, std::string* err);

// 数值/布尔的宽松解析：整串必须吃掉（`12abc` 用 atoi 会静默得到 12）。
bool parseNumber(const std::string& s, double& out);
bool parseBool(const std::string& s, bool& out);

} // namespace core
