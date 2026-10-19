#pragma once
// ============================================================
// editor/DebugRects —— 转发壳（2026-10-19 启动器 M1 起）
//
// 实现已下沉到 `engine/ui/DebugRects.h`，因为启动器是**另一个进程**，
// 但它的验收脚本要用同一套锚点格式。这里只留一行 using，让编辑器的
// 几十处 `editor::logRect(...)` / 裸 `logRect(...)` 一个字都不用改。
//
// 细则（为什么只在矩形变化时打、被裁掉的控件照样有 rect、脚本取最后
// 一条）全部在原头文件的注释里 —— 见 `engine/ui/DebugRects.h`。
// ============================================================

#include "ui/DebugRects.h"

namespace editor {

using ui::logRect;

} // namespace editor
