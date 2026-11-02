#pragma once
// ============================================================
// editor/PostVolumePanel —— 后处理体积（Post-Process Volume）
//
// ---- 为什么需要它 ----
//   `engine/scene/PostVolume.{h,cpp}` 从 R1-2/R1-3 起就是完整的：AABB +
//   优先级 + 过渡带 + `kNoOverride` 哨兵，`SceneSerializer` 落盘，
//   `Renderer` 每帧按相机位置求值混进 `CameraEffects`。
//   但在 2026-11-02 之前 `grep -rln PostVolume editor/` **命中 0** ——
//   它不是实体，Hierarchy 里也没有，于是**开发者无法在编辑器里放一个**。
//   想"进洞穴变暗 / 角色特写加景深"只能改 118 个**全局** `post.*` 参数，
//   一改全场。这一屏补的就是这个洞。
//
// ------------------------------------------------------------
// 四条设计约束
// ------------------------------------------------------------
// ① **真值在场景里**（`scene.postVolumes()`），面板不持副本。
//    与 EnvironmentPanel 同一条纪律：面板只是视图，Renderer 每帧自己
//    去场景里取。做"本地副本 + 同步按钮"一定会分叉，症状是
//    "面板显示景深 5 米、画面没有景深"。
//
// ② **"不覆盖"必须是显式的**。浮点项用 `kNoOverride(-1)` 表示，
//    面板上画成**未勾选**；取消勾选写回 -1，**不是写 0**。
//    0 是一个有效值（强度 0 = 关），两者混淆会让"关掉覆盖"变成
//    "把效果关掉" —— 而用户想表达的是前者。
//
// ③ **必须能看到"这个体积现在生效吗"**。体积最常见的事故不是参数错，
//    是**相机根本不在里面**（或落在过渡带边缘只生效 40%）。所以面板底部
//    常驻一段实时读数：相机坐标 + 每个体积的权重 + 最终混合值。
//    没有这一段，用户只能靠"改一下看画面变没变"来二分。
//
// ④ **新建的体积默认落在相机脚下**（4 米半边长），而不是原点 ±1。
//    原点 ±1 在大多数场景里离相机 10 米开外 —— 建完什么都看不到，
//    用户第一反应是"这个面板坏了"。
//
// ------------------------------------------------------------
// 自动化锚点（`tools/verify_post_volume.py` 用）
// ------------------------------------------------------------
//   PV-RECT              面板内容区矩形
//   PV-ITEM-<i>          列表第 i 行
//   PV-NEW / PV-DUP / PV-DEL
//   PV-NAME / PV-PRIORITY / PV-BLEND
//   PV-MIN / PV-MAX
//   PV-OVR-<FIELD>-CHECK / -VAL     浮点覆盖（DOF-FOCUS / DOF-APERTURE /
//                                   DOF-MAXR / MOTION / CHROMA / VIGNETTE /
//                                   CG-LUT / EXPOSURE）
//   PV-OVR-<FIELD>-TRI              int 三态（DOF / MOTION / CHROMA / VIGNETTE）
//   PV-LIVE                        实时读数那一行
// 日志锚点：
//   PV-CREATE / PV-DELETE / PV-DUP  结构变更（带 count=N）
//   PV-EVAL                         求值结果（`pv.eval` 命令与面板共用）
// ============================================================

#include "scene/PostVolume.h"

#include <glm/glm.hpp>

#include <string>

namespace scene {
class Scene;
}

namespace editor {

class EditorContext;

// ---- 结构操作（面板按钮与 `pv.add` / `pv.remove` 命令**共用这三个**）----
// 分开写两套的后果是"命令能建、按钮不能建"，而判据只覆盖其中一条。
int postVolumeAdd(scene::Scene& sc, const std::string& name,
                  const glm::vec3& center, const glm::vec3& halfSize);
bool postVolumeRemove(scene::Scene& sc, int index);
// 给日志/命令行用的一行摘要（不翻译：它是调试输出，不是界面文字）。
std::string postVolumeDescribe(const scene::PostVolume& v);

class PostVolumePanel {
public:
    // camPos = 视口相机世界位置（生效权重的判据点）。
    void draw(EditorContext& ctx, const glm::vec3& camPos);

    int selected() const { return m_sel; }

private:
    // 选中下标。**不存名字**：名字可以改、可以重名，而下标只在
    // 增删时变 —— 增删两条路径都会把它钳回合法范围。
    int m_sel = -1;
    // 删除要二次确认（一个体积可能配了十几个覆盖值，误删很难重建）。
    int m_confirmDelete = -1;

    // 名字输入框的缓冲：ImGui 的 InputText 要一块稳定内存。
    // `m_nameFor` 记住"这块缓冲现在属于哪个下标"，换了对象才重灌 ——
    // 否则每帧重灌会让用户打字打到一半被覆盖掉。
    char m_nameBuf[96] = {};
    int m_nameFor = -2;
};

} // namespace editor
