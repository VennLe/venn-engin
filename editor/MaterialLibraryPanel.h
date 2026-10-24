#pragma once
// ============================================================
// editor/MaterialLibraryPanel —— 材质仓库面板（2026-10-24）
//
// 素材来自 `assets/materials/library.json`（由
// `tools/gen_material_library.py` 从 `venn-resource` 的 glTF 材质包生成）。
// 引擎侧的索引在 `assets/MaterialLibrary`，本面板只负责画出来 + 派发动作。
//
// ------------------------------------------------------------
// 这个面板存在的两个理由（都是用户原话）
// ------------------------------------------------------------
//   ① "打造出的材质仓库，可以在模式中用作笔刷，给地形区域手动加上纹理贴图"
//      → 选中条目点 "Use as terrain brush"：把该材质绑到地形的第 N 层，
//        切到刷层权重工具，之后在视口里拖就是刷这块纹理。
//   ② "将来可以除了地形地面，还可以给各种其他的静态网格体在属性栏中绑定
//      对应的材质信息"
//      → 选中条目点 "Apply to selection"：把 `MaterialComponent::material`
//        换成这份 .vmat（含模型根下所有子网格）。
//
// ------------------------------------------------------------
// 为什么缩略图不复用 ContentBrowser
// ------------------------------------------------------------
// ContentBrowser 是按**目录**浏览的，材质仓库是按**分类 + 语义**摆的货架
// （"地面"/"混凝土"），还带着 glTF 里读出来的 roughness / metallic 这些
// 元信息。共用一份网格布局代码只会让两边都长出一堆 if。
// 缩略图本身倒是共用同一个 `ThumbnailCache` —— 那才是真正该复用的一层。
// ============================================================

#include "EditorContext.h"

#include "ThumbnailCache.h"

#include <string>

namespace assets {
struct MaterialLibraryEntry;
}

namespace editor {

class LandscapeEditor;

class MaterialLibraryPanel {
public:
    explicit MaterialLibraryPanel(EditorContext& ctx) : m_ctx(ctx) {}

    // 地形笔刷那一半能力要靠它（"用这个材质刷地形"）。
    // nullptr = 还没有地形编辑器（理论上不会，它是 EditorApp 常驻成员），
    // 那个按钮就置灰。
    void attachLandscape(LandscapeEditor* land) { m_land = land; }

    void draw();

private:
    // 一张卡片（缩略图 + 名字 + 元信息）。点击 = 选中。
    void drawCard(const assets::MaterialLibraryEntry& e, float cellW);
    // 详情区 + 两个动作按钮
    void drawDetails(const assets::MaterialLibraryEntry& e);
    // 把材质赋给选中实体（含其下所有带 MaterialComponent 的子网格）。
    // 返回受影响的组件数；0 = 没选中东西 / 选中项没有材质槽。
    int applyToSelection(const assets::MaterialLibraryEntry& e);

    EditorContext& m_ctx;
    LandscapeEditor* m_land = nullptr;

    ThumbnailCache m_thumbs;

    char m_filter[64] = "";
    // -1 = 全部分类；否则是 categories() 的下标
    int m_category = -1;
    // 选中的条目 id（空 = 还没选）
    std::string m_selected;
    // 上次见到 `EditorContext::materialLibraryRevision()` 的值（见 draw()）
    int m_libRevision = -1;
};

} // namespace editor
