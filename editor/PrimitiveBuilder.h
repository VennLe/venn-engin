#pragma once
// ============================================================
// editor/PrimitiveBuilder —— 内置图元的"回头再改生成参数"
//
// Blender 里加完一个图元，半径 / 分段数是可以回头再改的（Add 面板），
// 改完几何重建。这里做同一件事：
//
//   MeshComponent::params  ←→  AssetManager 里那颗几何
//
// 关键点只有一个：**参数必须进缓存键**。AssetManager 是按名字缓存的，
// 生成参数不同的两颗球如果共用一个键，第二次调用会命中缓存拿回旧几何，
// 参数改了等于没改。所以 primitiveKey() 把参数编成
// "builtin/sphere/r0.5000/g48/n24" 这样的键。
//
// 副作用（有意为之）：参数相同的两颗 mesh 天然共享同一份 GPU 缓冲，
// 旧参数的那份也**不会**被释放 —— 撤销回去还能拿回同一个指针。
// 代价是编辑器会话里改参数会攒下若干份几何，几十 KB 量级，可以接受。
// ============================================================

#include "ecs/Components.h"

#include <string>

namespace assets {
class AssetManager;
}

namespace editor {

// 生成参数 → AssetManager 缓存键。浮点只保留 4 位小数：拖拽时
// 0.30000001 / 0.3 这种噪声不该被当成"两个不同的网格"。
std::string primitiveKey(const ecs::MeshPrimitiveParams& p);

// 按 params 生成（或复用）网格，把指针写回 mc.mesh。
// primitive == None（OBJ / glTF）时什么都不做。
void rebuildPrimitive(assets::AssetManager& am, ecs::MeshComponent& mc);

// Add 菜单里那几种图元的默认生成参数（和 createPrimitive 里原来的
// 硬编码默认值保持一致）。
ecs::MeshPrimitiveParams defaultPrimitiveParams(ecs::MeshPrimitive kind);

} // namespace editor
