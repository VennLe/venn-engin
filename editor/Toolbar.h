#pragma once
// ============================================================
// editor/Toolbar —— 主菜单 + 撤销/重做条
//
// 历史上这条工具栏塞过 Save/New/手柄模式/播放按钮；现在全部移走：
//   · Save / New       → File 菜单（Ctrl+S / Ctrl+N 一直都在）
//   · Move/Rotate/Scale → 视口工具条（W / E / R），工具栏里是重复入口
//   · Fullscreen       → F / F11 快捷键 + View 菜单
//   · Run / Play/Pause/Stop → 视口正下方那一行播放条
//     （Run=运行游戏；Play/Pause/Stop=编辑态脚本动画，见 EditorContext）
// 工具栏只剩右对齐的 Undo / Redo。
// ============================================================

#include "EditorContext.h"

namespace editor {

class Toolbar {
public:
    explicit Toolbar(EditorContext& ctx) : m_ctx(ctx) {}

    void draw();

    // 文件操作（Ctrl+N 由 EditorApp 转发到这里）
    void newScene();

private:
    void drawMenuBar();
    void drawBar();
    // 工具栏分组之间的竖线（比 TextUnformatted("|") 更像 UE5 的分组线）
    static void toolbarSeparator();

    void openScene(const std::string& relPath);
    void saveScene(const std::string& relPath);

    EditorContext& m_ctx;
};

} // namespace editor
