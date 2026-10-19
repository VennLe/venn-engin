#pragma once
// ============================================================
// editor/CharacterWindow —— 角色详情窗口（UE5 Persona 风格）
//
// 创建第一/第三人称角色时弹出。布局对标 UE5 的 Persona 编辑器：
//
//   ┌─────────────────────────────────────────────────────────────┐
//   │ Character: <名字>            [shading] [bones] [grid] [reset]│
//   ├────────────┬──────────────────────────────┬─────────────────┤
//   │ Skeleton   │                              │ Details         │
//   │ Mesh       │        预览视口               │  · Asset        │
//   │ Animation  │   （独立离屏链，可导航）      │  · Skeleton     │
//   │            │                              │  · Bone         │
//   ├────────────┴──────────────────────────────┴─────────────────┤
//   │ ▶/⏸ ⏹ ⟲  速度 [1.0]  ────时间轴────  0.00/1.00s            │
//   └─────────────────────────────────────────────────────────────┘
//
// 三块内容：
//
//   ① 预览视口 —— 复用 Renderer 的**独立预览离屏链**（第二套 HdrTarget +
//      PostProcess），与主视口同时显示、互不干扰。导航手感与主 3D 视口
//      一致：右键转视角、中键平移、滚轮推拉。
//   ② 左栏三页 —— Skeleton（骨骼树 + 骨骼搜索）/ Mesh（模型来源与绑定）/
//      Animation（片段列表）。
//   ③ 右栏 Details —— UE 的 Asset Details：当前资产、骨架统计、**选中骨骼
//      的静止/动画世界位置**，以及两个动作按钮（生成骨骼绑定 / 绑定到玩家）。
//
// 预览场景与主场景**完全隔离**：预览里摆模型、播动画不会污染编辑场景。
// "生成骨骼绑定"才会写主场景 —— 它把模型实例化到玩家实体下并挂
// SkinnedMeshComponent，同时**按模型包围盒**算出胶囊尺寸与相机眼高，
// 这是"创建角色"真正缺的那一步（不然新角色只有一具空胶囊）。
// ============================================================

#include "EditorContext.h"

#include "scene/Scene.h"

#include "ecs/Entity.h"

#include <glm/glm.hpp>

#include <imgui.h>

#include <memory>
#include <string>
#include <vector>

namespace assets {
class Model;
class Skeleton;
} // namespace assets

namespace render {
class Renderer;
} // namespace render

namespace editor {

class CharacterWindow {
public:
    CharacterWindow(EditorContext& ctx, render::Renderer& renderer);

    // 打开窗口并关联一个玩家实体（第一/第三人称角色）。
    void open(ecs::Entity player);

    // 每帧绘制（EditorApp::onImGui 末尾调用）。窗口关闭时自动清理预览场景。
    void draw();

    bool isOpen() const { return m_open; }
    void close();

private:
    // ---- 预览场景 ----
    // 懒构建：第一次打开时把当前绑定的模型实例化进预览场景。
    void rebuildPreviewScene();
    // 把模型（SubMesh 列表）实例化进目标场景的指定父节点下，返回实体列表。
    // 实际实现转发给 `engine/scene/ModelInstance`（全仓库唯一的装配入口）。
    // autoPlay：预览场景传 false（姿势由底部时间轴滑条驱动），
    //           绑到主场景的玩家实体上传 true（绑上去就该动）。
    std::vector<ecs::Entity> instanceModelInto(scene::Scene& scene,
                                               assets::Model* model,
                                               ecs::Entity parent,
                                               bool autoPlay);
    // 把"当前模型 + 当前 clip"绑到预览场景的根上
    void syncPreviewSkinning();

    // ---- UI 子块 ----
    void drawToolbar();          // 预览区顶部的工具条（着色 / 骨骼 / 网格）
    void drawPreviewViewport();
    void drawSkeletonPanel();
    void drawMeshPanel();
    void drawAnimationPanel();
    void drawTransportBar();     // 底部播放条（播放 / 循环 / 速度 / 时间轴）
    void drawDetailsPanel();     // 右栏 Details

    // 预览相机导航（右键转视角、滚轮推拉、中键平移）
    void handlePreviewNavigation(const glm::vec2& vpPos,
                                 const glm::vec2& vpSize, bool hovered);
    // 把预览相机对准模型的包围球（工具栏的 Reset View）
    void framePreviewCamera();
    // 把给定包围盒完整装进视锥所需的相机距离（含边距）
    float previewDistanceFor(const glm::vec3& size) const;

    // 选一个动画 clip 并开始预览播放
    void playClip(const std::string& clipName);

    // 生成骨骼绑定：蒙皮组件 + 按模型尺寸算胶囊 + 相机眼高（写主场景）
    void generateSkeletonBinding();
    // 只把当前模型绑定到主场景的玩家实体下（实例化 + 蒙皮组件）
    void bindModelToPlayer();

    // 选中骨骼在**当前姿势**下的世界位置（预览场景的模型空间）
    bool jointWorldPosition(int joint, glm::vec3& out) const;

    EditorContext& m_ctx;
    render::Renderer& m_renderer;

    bool m_open = false;
    ecs::Entity m_player{};      // 关联的玩家实体
    std::string m_playerName;    // 玩家显示名（实体句柄失效时的兜底）

    // 当前绑定的模型（可为空：还没选模型时只显示胶囊占位）
    assets::Model* m_model = nullptr;
    std::string m_modelPath;     // 模型来源路径（Content 拖入 / 缓存键）

    // 模型的**世界包围盒**（预览场景里，含各 SubMesh 的变换）。
    //
    // ⚠ 不能用 Model::center / Model::radius 取景：那是**包围球**，而且这个
    //   字段在部分 FBX 上根本没被填（实测 anime_girl 的 center = (0,0,0)、
    //   radius = 0.822，而模型实际站在 z ∈ [0, 1.556]）——相机于是盯着脚底、
    //   把人拍成"从下往上"的半截。自己按 SubMesh 的 bounds × transform 累
    //   一个 AABB 最稳，取景与"眼高 / 胶囊"都从它算。
    glm::vec3 m_boundsMin{0.0f};
    glm::vec3 m_boundsMax{0.0f};
    bool m_boundsValid = false;
    void computeModelBounds();

    // 预览场景（独立，与主场景隔离）
    scene::Scene m_previewScene;
    ecs::Entity m_previewRoot{};    // 预览场景里模型根实体
    bool m_previewDirty = true;     // 需要重建预览场景

    // 预览相机导航状态（轨道式：绕模型打转）
    float m_previewYaw = 0.6f;
    float m_previewPitch = 0.35f;
    float m_previewDistance = 3.0f;
    float m_previewFov = 45.0f;
    glm::vec3 m_previewTarget{0.0f, 0.0f, 0.9f};

    // 动画播放状态
    std::string m_currentClip;   // 空 = 静止姿势
    float m_animTime = 0.0f;
    bool m_animPlaying = false;
    bool m_animLoop = true;
    float m_animSpeed = 1.0f;

    // 选中的骨骼（Details 用）与骨骼搜索
    int m_selectedJoint = -1;
    char m_boneSearch[64] = {0};

    // 预览视口尺寸（上一帧请求值，用于 setPreviewSize 去抖）
    uint32_t m_reqW = 0;
    uint32_t m_reqH = 0;

    // 骨架层级展开状态（按 joint 下标）
    std::vector<char> m_jointExpanded;
    // 当前姿势的 joint 矩阵缓存（Details 里算骨骼世界位置用）
    mutable std::vector<glm::mat4> m_jointMats;
};

} // namespace editor
