#pragma once
// ============================================================
// editor/CharacterWindow —— 角色详情窗口（UE5 Persona 风格 MVP）
//
// 创建第一/第三人称角色时弹出。窗口分两块，对标 UE5 的 Persona：
//
//   ① 顶部模型预览视口
//      · 复用 Renderer 的**独立预览离屏链**（第二套 HdrTarget +
//        PostProcess），与主视口同时显示、互不干扰
//      · 导航手感与主 3D 视口一致：按住右键转视角、滚轮推拉
//      · 渲染一个**独立的预览场景**：只含该角色的模型 + 一盏方向光 +
//        一个预览相机
//
//   ② 左侧属性栏（骨架 / 网格 / 动画 三块）
//      · Skeleton —— 骨骼层级树（可展开），显示关节名与父子关系
//      · Mesh     —— 当前绑定的骨骼网格模型；可"添加/切换"模型
//      · Animation—— 动画片段列表（来自 Skeleton 的 clips），
//        选中即预览播放，带播放/暂停 + 时间轴拖动
//
// 预览场景与主场景**完全隔离**：预览里摆模型、播动画不会污染编辑场景。
// "绑定到玩家"是另一个动作：把模型实例化到主编辑场景的玩家实体下，
// 并挂 SkinnedMeshComponent（蒙皮）或普通 MeshComponent（静态）。
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
    // 蒙皮模型会在根实体挂 SkinnedMeshComponent。
    std::vector<ecs::Entity> instanceModelInto(scene::Scene& scene,
                                               assets::Model* model,
                                               ecs::Entity parent);

    // ---- UI 子块 ----
    void drawPreviewViewport();
    void drawSkeletonPanel();
    void drawMeshPanel();
    void drawAnimationPanel();

    // 预览相机导航（右键转视角、滚轮推拉、中键平移）
    void handlePreviewNavigation(const glm::vec2& vpPos,
                                 const glm::vec2& vpSize, bool hovered);

    // 选一个动画 clip 并开始预览播放
    void playClip(const std::string& clipName);

    // 把当前模型绑定到主场景的玩家实体下（实例化 + 蒙皮组件）
    void bindModelToPlayer();

    EditorContext& m_ctx;
    render::Renderer& m_renderer;

    bool m_open = false;
    ecs::Entity m_player{};      // 关联的玩家实体
    std::string m_playerName;    // 玩家显示名（实体句柄失效时的兜底）

    // 当前绑定的模型（可为空：还没选模型时只显示胶囊占位）
    assets::Model* m_model = nullptr;
    std::string m_modelPath;     // 模型来源路径（Content 拖入 / 缓存键）

    // 预览场景（独立，与主场景隔离）
    scene::Scene m_previewScene;
    ecs::Entity m_previewRoot{};    // 预览场景里模型根实体
    bool m_previewDirty = true;     // 需要重建预览场景

    // 预览相机导航状态（轨道式：绕模型打转）
    float m_previewYaw = 0.6f;
    float m_previewPitch = 0.35f;
    float m_previewDistance = 3.0f;
    glm::vec3 m_previewTarget{0.0f, 0.0f, 0.9f};

    // 动画播放状态
    std::string m_currentClip;   // 空 = 静止姿势
    float m_animTime = 0.0f;
    bool m_animPlaying = false;

    // 预览视口尺寸（上一帧请求值，用于 setPreviewSize 去抖）
    uint32_t m_reqW = 0;
    uint32_t m_reqH = 0;

    // 骨架层级展开状态（按 joint 下标）
    std::vector<char> m_jointExpanded;
};

} // namespace editor
