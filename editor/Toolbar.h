#pragma once
// ============================================================
// editor/Toolbar —— 主菜单 + 播放控制条
//
// 播放按钮是"EditorScene / RuntimeScene 分离"的操作入口：
//
//   Play  → EditorContext::play()   把编辑态整份复制成运行态并开跑
//   Pause → 冻结运行态的更新（保留全部状态）
//   Stop  → 直接丢弃运行态
//
// 因此 Stop 之后编辑态一定是你离开时的样子 —— 不需要任何回滚。
// 按钮上会把这一点直接写出来，免得用户以为"停止会重置场景"。
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
    void drawPlayControls();
    // 工具栏分组之间的竖线（比 TextUnformatted("|") 更像 UE5 的分组线）
    static void toolbarSeparator();

    void openScene(const std::string& relPath);
    void saveScene(const std::string& relPath);

    EditorContext& m_ctx;
};

} // namespace editor
