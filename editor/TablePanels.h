#pragma once

#include "i18n/LocaleFile.h"
#include "project/DataTable.h"

#include <string>
#include <vector>

namespace editor {

// ---------------------------------------------------------------- 配表（C1）
//
// 面板本身只做三件事：选表 / 改格子 / 存盘。**不做类型编辑**（没有类型行，
// 见 `project::DataTable` 的头注释），也**不做导入导出**（CSV 就是交换格式）。
class DataTablePanel {
public:
    void draw();

    // 命令用的四个入口（与按钮走同一条路 —— 否则"能点不能存"）。
    bool saveCurrent(std::string* msg);
    bool reloadAll(std::string* msg);
    bool setCell(int row, int col, const std::string& value, std::string* msg);
    std::string cellText(int row, int col) const;

    int selectedTable() const { return m_sel; }
    // 按名字选表（命令用）。**没有它**的话命令只能按下标选，而下标会随
    // 文件名排序变化 —— 判据里写死下标等于写死一个不稳定量。
    bool selectTable(const std::string& name);
    const std::vector<project::DataTable>& tables() const;

private:
    void ensureLoaded();

    bool m_loaded = false;
    int m_sel = 0;
    std::string m_status;
    std::string m_dir;
};

// ---------------------------------------------------------------- 本地化（C2）
class LocalizationPanel {
public:
    void draw();

    bool save(std::string* msg);
    bool reload(std::string* msg);
    bool setRow(int index, const std::string& zh, std::string* msg);
    std::string valueOf(int index) const;
    size_t rowCount() const { return m_ed.count(); }

    i18n::LocaleEditor& editor() { return m_ed; }

private:
    void ensureLoaded();

    bool m_loaded = false;
    i18n::LocaleEditor m_ed;
    std::string m_status;
    char m_filter[96] = {};
    bool m_onlyMissing = false;
    bool m_onlyDirty = false;
    int m_selected = -1;
    // 过滤后的行下标（每帧重算；`m_selected` 用的是**全表**下标，
    // 所以过滤条件一变不会"选到别人身上"）
    std::vector<int> m_view;
};

} // namespace editor
