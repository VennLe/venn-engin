#pragma once
// ============================================================
// runtime/GameFlags —— **兼容垫片**（2026-11-02）
//
// 真身搬到 `engine/scene/GameFlags.h` 了，理由写在那份文件头：
// 编辑器 Play 态也是宿主，而 `editor/` 分层上不许 include `runtime/`，
// 所以类必须落在两边都能看见的 `engine/` 里。
//
// 这里只留一行别名，让 `RuntimeApp` 那几处 `GameFlags m_flags;` /
// `m_flags.set(...)` 一个字都不用改。**新代码请直接 include
// `scene/GameFlags.h` 并写 `scene::GameFlags`** —— 别再用这个别名，
// 否则下一个人还会以为旗标住在 runtime。
// ============================================================

#include "scene/GameFlags.h"

namespace runtime {

using GameFlags = scene::GameFlags;

} // namespace runtime
