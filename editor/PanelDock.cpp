// ============================================================
// editor/PanelDock.cpp —— 托管提示的唯一一份存储
//
// 定义在这里而不是头文件里：`static` 局部变量塞进 inline 函数会在**每个
// 编译单元各来一份**（链接器把它们合成一个函数体，但静态变量仍是每个 TU
// 一份）—— 于是 EditorApp 写的那个和面板读的那个不是同一个对象，表现就是
// "标签页永远不生效"。这是 header-only 全局状态最经典的坑。
// ============================================================

#include "PanelDock.h"

namespace editor {

DockRect& dockHintRect() {
    static DockRect r;
    return r;
}

bool& dockHintActive() {
    static bool active = false;
    return active;
}

} // namespace editor
