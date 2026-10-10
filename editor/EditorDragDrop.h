#pragma once
// ============================================================
// editor/EditorDragDrop —— 拖拽载荷的类型标识
//
// ImGui 的拖拽靠**字符串**匹配载荷类型（SetDragDropPayload 的 type 与
// AcceptDragDropPayload 必须逐字节相同）。原来是四处字面量散落在
// ContentBrowser / ViewportPanel / SceneHierarchy 里，改一处漏一处，
// 所以收敛到这里。
//
// 载荷内容约定（这个比类型串更容易踩错，一并写在这儿）：
//   kAsset  —— 以 '\0' 结尾的 **UTF-8 字符串**，值是**相对资产根的路径**
//              （assets::makeAssetRelative）。接收端直接当 const char* 用。
//   kEntity —— 一个 ecs::EntityId 的裸字节拷贝。
//
// 注意 ImGui 给 type 的上限是 32 字节（imgui.h 里 SetDragDropPayload 的
// IM_ASSERT(strlen(type) < 32)），这两个都远没到。
// ============================================================

namespace editor {
namespace drag {

inline constexpr char kAsset[] = "EDITOR_ASSET";    // 资产相对路径
inline constexpr char kEntity[] = "EDITOR_ENTITY";  // ecs::EntityId

} // namespace drag
} // namespace editor
