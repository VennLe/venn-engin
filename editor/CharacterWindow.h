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

#include "animation/ClipEdit.h"
#include "animation/Constraint.h"
#include "animation/GaitCycle.h"
#include "animation/Pose.h"
#include "animation/RigEdit.h"
#include "animation/StrideMatch.h"
#include "animation/WeightSet.h"
#include "scene/Scene.h"

#include "ecs/Entity.h"

#include <glm/glm.hpp>

#include <imgui.h>

#include <memory>
#include <string>
#include <vector>

namespace assets {
struct Model;
struct Skeleton;
} // namespace assets

namespace render {
class Renderer;
} // namespace render

namespace editor {

class DopeSheetWindow;

class CharacterWindow {
public:
    CharacterWindow(EditorContext& ctx, render::Renderer& renderer);

    // 打开窗口并关联一个玩家实体（第一/第三人称角色）。
    void open(ecs::Entity player);

    // 每帧绘制（EditorApp::onImGui 末尾调用）。窗口关闭时自动清理预览场景。
    void draw();

    bool isOpen() const { return m_open; }
    void close();

    // 2026-10-28（R8）：把摄影表接到本窗口上。
    //
    // 为什么是"本窗口持有指针、每帧推数据"而不是摄影表自己去读文件：
    // Pose Mode 里刚打的帧还没存盘，摄影表要调的就是**内存里那份**
    // （"打完帧顺手调节奏"必须是无缝的）。反过来如果让摄影表自己读盘，
    // 作者得先存一次再切窗口，而存盘会覆盖磁盘上的版本 —— 中间任何一步
    // 出错都是不可逆的。
    void attachDopeSheet(DopeSheetWindow* d) { m_dope = d; }

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

    // ---- 预览视口的叠加层（2026-10-22）----
    //
    // 画在 ImGui 的 draw list 上、压在那张离屏图之上。为什么这几样不做成
    // 场景里的实体：
    //   · **胶囊**要"隔着模型也能看清"——实体渲染会被角色自己挡住，
    //     而 UE 的胶囊线框也是叠加层。半透明线框既是这个行业习惯，也免了
    //     透明排序那一堆事。
    //   · **gizmo** 同理：坐标轴必须永远在物体前面（选中时你要能看见它）。
    //   · 两者的代价都只是一次投影，不碰渲染管线。
    void drawPreviewOverlay(const ImVec2& imgMin, const ImVec2& imgSz);
    // 预览下方那条"选中对象"控件（位置 / 旋转 / 大小）
    void drawPreviewTransformStrip();

    // 预览里的选中对象
    enum class PreviewPick : int { None = 0, Model, Capsule };
    PreviewPick m_pick = PreviewPick::None;

    // 预览里的胶囊参数（与"生成骨骼绑定"写进玩家的那组是同一份来源）。
    // 默认按人形身高 1.8 m 的解剖比例：r = 0.16h、总高 = h。
    float m_capsuleRadius = 0.29f;
    float m_capsuleHalfHeight = 0.61f;
    // 胶囊在预览里的位置（**脚底**，与玩家实体同一口径）。默认原点 ——
    // 模型也是从原点站着，于是"胶囊套住人"这件事一眼可查。
    glm::vec3 m_capsulePos{0.0f, 0.0f, 0.0f};
    // 胶囊绕自身轴的旋转。线框看不出差别（旋转对称），但控件必须有这一项 ——
    // 用户要的是"位置 / 旋转 / 大小"三个自由度都在，缺一个会以为是没做完。
    float m_capsuleYawDeg = 0.0f;
    bool m_showCapsule = true;
    bool m_showGizmo = true;

    // 预览视口在屏幕上的矩形（overlay 与点选都要用；由
    // drawPreviewViewport 每帧写）。
    ImVec2 m_vpMin{0.0f, 0.0f};
    ImVec2 m_vpSize{0.0f, 0.0f};

    // 胶囊**在屏幕上**的样子：中轴两端（两个球心的屏幕坐标）+ 半径（像素）。
    // 每帧由 drawPreviewViewport 投影一次，点选直接拿它算"鼠标离线框多远"。
    //
    // 为什么不能用射线-AABB 取更近：胶囊天生比人**粗**（r = 0.16h，人正面的
    // 厚度只有 ±0.18 m），相机从正面看过来永远先撞上胶囊的 AABB —— 模型就
    // 再也点不中了。实测就是这个现象：点身体中心选到胶囊，点脚底选到模型。
    // 改成"点线框选胶囊、点身体选模型"之后两者都点得到，而且和画面一致
    // （看得见的那圈线就是能点的那个东西）。
    ImVec2 m_capScrA{0.0f, 0.0f};
    ImVec2 m_capScrB{0.0f, 0.0f};
    // 中轴 → 右母线 的屏幕偏移（像素）。透视下方向不一定水平，所以不能
    // 用"往左挪 r 个像素"当左母线 —— 相机一偏航就点空了。
    ImVec2 m_capScrOff{0.0f, 0.0f};
    float m_capScrR = 0.0f;
    bool m_capScrValid = false;

    // 胶囊线框**实际画出来的每一段**（屏幕坐标，成对存：p0,p1,p2,p3…）。
    // 由 drawPreviewOverlay 在画的时候顺手记下来，点选就量"鼠标到这串
    // 线段的最短距离"。
    //
    // 为什么不用"到中轴的垂距 ≈ r"：胶囊的 X 轴投影是斜的（默认机位下
    // off=(37,29)，垂距只有 37 px 而不是 47 px），于是线框的真实位置和
    // "中轴 ± r" 差 10 px —— 点在线上了却判定 12 px 远，选不中。画什么
    // 就量什么，是唯一不会漂的做法。
    std::vector<ImVec2> m_capScrSegs;

    // 预览相机导航（右键转视角、滚轮推拉、中键平移）
    void handlePreviewNavigation(const glm::vec2& vpPos,
                                 const glm::vec2& vpSize, bool hovered);
    // 把预览相机对准模型的包围球（工具栏的 Reset View）
    void framePreviewCamera();
    // 把给定包围盒完整装进视锥所需的相机距离（含边距）
    float previewDistanceFor(const glm::vec3& size) const;

    // 选一个动画 clip 并开始预览播放
    void playClip(const std::string& clipName);

    // ---- 动画键盘映射（P4，2026-10-22）----
    // 把某个 clip 绑到某个键上，写进**玩家实体**的 `AnimKeyMapComponent`
    // （跟着角色存盘，不是编辑器偏好 —— 理由见 Components.h 那个组件的注释）。
    void bindClipKey(const std::string& clip, int key);
    void unbindClipKey(const std::string& clip);
    // 该 clip 现在绑在哪个键上（GLFW 键码）；0 = 没绑。
    int clipKey(const std::string& clip) const;
    // "等按键"状态：非空 = 下一帧抓到的第一个键就绑给这段 clip。
    std::string m_captureClip;

    // 生成骨骼绑定：蒙皮组件 + 按模型尺寸算胶囊 + 相机眼高（写主场景）
    void generateSkeletonBinding();
    // 只把当前模型绑定到主场景的玩家实体下（实例化 + 蒙皮组件）
    void bindModelToPlayer();

    // 选中骨骼在**当前姿势**下的世界位置（预览场景的模型空间）
    bool jointWorldPosition(int joint, glm::vec3& out) const;
    // 同上但给完整世界矩阵（gizmo 需要"骨在哪、朝向哪"两样）
    bool jointWorldMatrix(int joint, glm::mat4& out) const;

    // ------------------------------------------------------------
    // Pose Mode（R7，2026-10-28）
    // ------------------------------------------------------------
    //
    // 和 Animation 页的区别：Animation 页是"播导入的片段"，Pose Mode 是
    // "自己摆姿势并打帧"。数据进 `animation::EditClip`（engine/animation/
    // 下自建结构），**不碰 assets::Skeleton** —— 后者是导入产物，且
    // engine/assets/** 在 GLOBAL_GLOBS 里，动它就是全量回归。

    // 采样链：samplePose →（Pose Mode 时）overlayEditPose → poseToJoints。
    // 结果写 m_pose / m_jointMats（+ Pose Mode 时多算一份 m_worldMats）。
    void samplePoseInto();
    // 骨架或编辑数据变过 → 重新把骨名解析成 joint 下标
    void revalidateEdit();

    void drawPosePanel();       // 左栏第 4 页：关键帧列表 + 姿态库 + 存读
    void drawPoseDetails();     // 右栏 Details 的 POSE 段：选中骨的 TRS + 打帧

    // ------------------------------------------------------------
    // Rig 页（S-A，2026-10-29）：骨架编辑（Blender 的 Armature + Edit Mode）
    // ------------------------------------------------------------
    //
    // 数据在 `animation::RigEdit` 里（engine/animation 下自建结构），
    // `assets::Skeleton` 仍然是**导入产物**、一个字节不改 —— 编辑结果是通过
    // `bake()` 生成一份**新的** Skeleton 换给 `m_model->skeleton`。
    // 这样"改坏了"随时能 revert 回导入态（`m_rigBase` 一直握着原始指针）。
    void drawRigPanel();        // 左栏 Rig 页：骨树 + 选中骨的 head/len/roll/flags
    void ensureRig();           // 懒初始化：模型/骨架换过就重新导入
    void applyRig();            // 烘焙 + 换掉预览用的骨架（revalidate 编辑轨道）
    void revertRig();           // 丢掉编辑，回到导入骨架
    void runRigScript();        // `MYVK_EDITOR_RIG_SCRIPT` 注入（与 UI 同一条通路）
    void resetRigToCurrent();   // 以**当前**骨架为基准重新导入（换模型时用）

    // ------------------------------------------------------------
    // Weights 页（S-B，2026-10-29）：顶点组 + 自动权重 + 权重绘制
    // ------------------------------------------------------------
    //
    // 数据在 `animation::WeightSet` 里（每子网格一份），`assets::Mesh` 的
    // 顶点数组仍然是**导入产物**：刷完权重才通过 `bake()` 写成四槽，再走
    // `Mesh::upload()` 重新上传 —— 全仓库唯一的上传入口，不另开后门。
    //
    // ⚠ **骨下标 = `RigEdit` 的骨下标**。`RigEdit::bake()` 输出的 Skeleton
    //   里 `joints[i]` 就是 `bones[i]`，所以两者在"没增删骨"时是同一套编号；
    //   一旦在 Rig 页增删骨，权重里的下标就会指到别的骨上（没有编译期检查、
    //   没有运行期崩溃，只有画面上"刷的权重跑到别处"）。`ensureWeights`
    //   检测到骨数变化会打 `WEIGHTS-STALE` 并重新从网格导入 —— 这是唯一
    //   能拿到的可信基线（网格里那四个槽就是 GPU 正在用的）。
    void drawWeightsPanel();
    void ensureWeights();        // 懒初始化：模型/骨架换过就重新从网格导入
    void applyWeights();         // 烘焙脏的顶点组 + 重传顶点缓冲
    void revertWeights();        // 丢掉编辑，回到网格里的原样
    void runWeightsScript();     // `MYVK_EDITOR_WEIGHTS_SCRIPT` 注入
    int  weightSetCount() const;
    int  activeWeightSet() const;              // 当前子网格下标（越界 → -1）
    void ensureWeightCaches();                 // 顶点位置 + 1-ring（选中网格变了才重建）
    void handleWeightsPaint(const ImVec2& imgMin, const ImVec2& imgSz,
                            const glm::mat4& viewProj, bool hovered);
    void drawWeightsOverlay(const ImVec2& imgMin, const ImVec2& imgSz,
                            const glm::mat4& viewProj, bool hovered);
    // 把 mesh 空间的顶点投影到屏幕（笔刷、着色带、脚本 dump 共用）
    bool projectWeightVertex(int v, const glm::mat4& viewProj,
                             const ImVec2& imgMin, const ImVec2& imgSz,
                             glm::vec2& out) const;

    // ---- 约束栈（S-C）----
    // 数据是 `animation::ConstraintStack`（骨下标 = **骨架 joint 下标**），
    // Rig 页里挂在"当前选中骨"下面。这里只做编辑与存取，"顺序有意义"这件事
    // 由 `ConstraintStack::evaluate` 负责，UI 不重复实现。
    void drawConstraintsSection();
    void runConstraintScript();          // `MYVK_EDITOR_CONSTRAINT_SCRIPT` 注入
    void pruneConstraints();             // 换模型/改骨架之后丢掉越界条目
    int  constraintBoneOfSel() const;    // 当前选中骨 → skeleton joint 下标（-1）
    void drawConstraintTargetCombo(const char* id, int& joint, float width);
    bool saveConstraintsTo(const std::string& path);
    bool loadConstraintsFrom(const std::string& path);

    void drawBoneGizmo(const ImVec2& imgMin, const ImVec2& imgSz,
                       const glm::mat4& viewProj);    // 返回 true = 这一下被 gizmo 吃掉了，调用方不要再做点选
    bool handleBoneGizmoInput(const ImVec2& imgMin, const ImVec2& imgSz,
                              const glm::mat4& viewProj);
    void handlePoseKeys();

    // 给选中骨打一帧（rot/loc/scale 三个通道分别可选）
    void keySelectedBone(bool rot, bool loc, bool scale);
    // 把某根骨的当前局部 TRS 写成关键帧（keySelectedBone 的实现体）
    int  keyJoint(const std::string& jointName,
                  const animation::Pose& pose, int joint, float time,
                  bool rot, bool loc, bool scale);

    // 把当前 m_jointMats 的原始字节压成一个 FNV-1a 摘要打进日志。
    // 验收脚本靠它做"存盘前后 / 两个进程之间逐位一致"的比对 ——
    // 63 根骨的矩阵逐个数出来是 4KB 日志，摘要是一行。
    void logPoseDigest(const char* tag);

    bool saveEdit(const std::string& path);
    bool loadEdit(const std::string& path);

    // 验收脚本注入（与 MYVK_EDITOR_CURVED_SCRIPT 同一个套路）：
    // 走**和 UI 完全同一条**数据通路，不是另一套实现。
    void runPoseScript();

    // gizmo 拖拽状态
    int m_boneDragAxis = -1;             // 0/1/2 = X/Y/Z；-1 = 空闲
    glm::vec2 m_boneDragStart{0.0f, 0.0f};
    glm::vec2 m_boneDragTangent{0.0f, 0.0f};
    float m_boneDragAccum = 0.0f;        // 累计角度（弧度）
    float m_boneDragApplied = 0.0f;      // 已经写进 pose 的角度
    glm::quat m_boneDragOrig{1.0f, 0.0f, 0.0f, 0.0f};
    glm::quat m_boneDragParentWorld{1.0f, 0.0f, 0.0f, 0.0f};
    int m_gizmoHoverAxis = -1;           // 鼠标压着哪个环（画的时候高亮）
    bool m_gizmoConsumed = false;        // 这一帧的左键被 gizmo 吃掉了吗
    bool m_showBoneGizmo = true;         // 骨骼旋转环的显示开关
    // 下一帧是否把左栏切到 Pose 页。开 Pose Mode 时置上 —— 用户按了
    // "Pose Mode"，左栏还停在 Skeleton 页是很怪的（那一页跟摆姿势无关）。
    bool m_focusPoseTab = false;

    // 瞬态姿态：摆出来但**还没打帧**的那几根骨。
    // 见 .cpp 里 Pose Mode 段头注释的"为什么要 ①"。
    std::vector<char> m_touched;
    std::vector<glm::vec3> m_touchedT;
    std::vector<glm::quat> m_touchedR;
    std::vector<glm::vec3> m_touchedS;
    void clearTransient();
    // 保证三个瞬态数组长度 == n（骨架/模型换过之后要对齐）
    void ensureTransient(int n);
    // 第一次碰第 j 根骨：先把**三个通道的当前值**都抄一份再交给调用方改。
    //
    // ⚠ 少了这一步就会出这个 bug（冒烟时实测到的）：`setrot` 只改旋转，
    //   但"已触碰"是**按骨**记的、不是按通道记的 —— 于是平移/缩放被
    //   初始值（0 / 1）顶掉，角色的右臂在 t=1.0 时整根缩到原点。
    //   抄当前值之后，"没改的那两个通道"就等于它本来该有的样子。
    void touchBone(int j);

    bool m_poseMode = false;
    animation::EditClip m_edit;
    // ---- 步态生成器（2026-10-30）----
    // 参数与上一次的报告都留在窗口上：读者按「生成」之后要看到**毫米数**，
    // 而不是"看起来像在走"。报告里的 rig 也留着 —— 面板要显示哪些角色
    // 在这具骨架上没找到（缺 Neck / Toe 的骨架照样能生成，只是少一段动作）。
    animation::GaitParams m_gait;
    animation::GaitReport m_gaitReport;
    bool m_gaitRan = false;
    // 生成一次行走循环写进 m_edit。面板按钮与脚本 `gait:` 共用（见 .cpp）。
    bool generateWalk();

    // ---- 步幅匹配（A4，2026-10-30）----
    // 量**模型里某一段**的自然步速，并打一行 `CHAR-STRIDE`。
    // 面板的 STRIDE MATCH 段与脚本 `stride[:clip]` 共用这个入口 ——
    // 量出来的数就是 `Locomotion` 每帧会用来算播放速率的那个数，
    // 所以"面板上显示的 0.598"和"Play 里用的"不可能不是同一个。
    bool measureStride(const std::string& clipName);
    animation::StrideInfo m_strideInfo;
    std::string m_strideClip;   // 上次量的是哪一段（空 = 还没量过）
    // 摄影表的绑定（R8）。为空 = 编辑器没建它（只在测试/裁剪构建里）。
    DopeSheetWindow* m_dope = nullptr;
    // 摄影表标题里显示的"哪份片段"（bind 时算一次，指针要活到 bind 之后）
    std::string m_dopeLabel;
    // 把 m_edit / 骨架 / 播放头 / 脏标记推给摄影表；退出 Pose Mode 时解绑。
    // 每帧调用（在 draw 里），bind() 自己会去重，不会刷日志。
    void syncDopeSheet();

    // ---- Rig 页状态（S-A）----
    animation::RigEdit m_rig;                        // 可编辑骨架（作者改的就是它）
    std::shared_ptr<assets::Skeleton> m_rigBase;     // 导入骨架（烘焙基准 + revert 用）
    assets::Skeleton m_rigSkel;                      // 最近一次烘焙结果
    std::vector<glm::mat4> m_rigWorld;               // 静止世界矩阵（画骨用）
    bool m_rigActive = false;    // 打开过 Rig 页（= m_rig 已经有内容）
    bool m_rigDirty = false;     // 需要重烘焙
    bool m_showRigBones = true;  // 视口里画骨
    int m_rigSel = -1;           // 选中的骨下标（RigEdit 里的）
    int m_rigPendingApply = 0;   // >0 = 这帧结束要 apply（UI 批量写时只烘焙一次）
    // 下一帧是否把左栏切到 Rig 页。验收脚本一定要切 —— 页签内容只有在
    // **被选中**时才走绘制，不切的话锚点一个都不会出现（R7 的 m_focusPoseTab
    // 是同一个问题）。
    bool m_focusRigTab = false;
    char m_rigPathBuf[512] = {0};
    bool m_rigScriptRan = false;

    // ---- 约束栈状态（S-C）----
    animation::ConstraintStack m_constraints;   // 骨下标 = 骨架 joint 下标
    animation::ConstraintEvalResult m_conStats; // 上一帧求值的账（面板上显示）
    int m_conSel = -1;                 // 选中的约束（栈里的全局下标）
    bool m_conDirty = false;           // 有未落盘的编辑（标题上打个 *）
    bool m_showConstraints = true;     // 视口里画"被约束的骨"高亮
    char m_conPathBuf[512] = {0};
    bool m_conScriptRan = false;
    // 校验和：模型换了之后骨数/顺序可能变，用它决定要不要 prune
    int m_conBoundJoints = -1;

    // ---- Weights 页状态（S-B）----
    // 每子网格一份顶点组；`m_weightsActive[i]` = 这一份有内容（网格非空）。
    std::vector<animation::WeightSet> m_weights;
    std::vector<char> m_weightsActive;
    std::vector<char> m_weightsDirty;      // 这一份需要烘焙 + 重传
    std::vector<glm::vec3> m_weightPos;    // 选中网格的顶点位置（**网格空间**）
    std::vector<glm::vec3> m_weightNrm;    // 法线（Front Faces Only 用）
    std::vector<int> m_weightRingOff;      // 1-ring（CSR），平滑用
    std::vector<int> m_weightRingIdx;
    int m_weightCacheSet = -1;             // 缓存对应哪个子网格（-1 = 无效）
    std::vector<char> m_deformMask;        // 骨的 deform 标志（约 binding 用）
    int m_weightsRigBones = -1;            // 导入时的骨数（检测增删骨后的错位）
    bool m_weightsInited = false;
    int m_weightSel = 0;                   // 当前子网格
    bool m_showWeights = false;            // 视口里画权重色带
    // 刷子（Blender 的 Weight Paint 那一排）
    int m_brushMode = 0;                   // 0 Add / 1 Subtract / 2 Set
    float m_brushWeight = 0.5f;            // 一次刷多少（Add/Subtract 是增量）
    float m_brushRadiusPx = 46.0f;         // 屏幕像素半径（和 Blender 同口径）
    float m_strengthScale = 1.0f;          // 中心 → 边缘的衰减强度
    bool m_brushAutoNormalize = true;      // 刷完立刻归一化
    bool m_brushXMirror = false;           // 镜像到对侧顶点（骨名 .L/.R 互换）
    bool m_brushFrontFaces = true;         // 只刷朝向相机的面（免得刷到背面）
    int m_brushHits = 0;                   // 上一笔刷到几个顶点（HUD 显示）
    bool m_weightsTabOpen = false;         // 这一帧 Weights 页被绘制了吗
    bool m_focusWeightsTab = false;
    char m_weightsPathBuf[512] = {0};
    bool m_weightsScriptRan = false;
    // 自动权重参数（面板上可调）
    animation::AutoWeightParams m_autoParams;
    // 顶点色带的屏幕坐标缓存（每帧重算太贵；只算"有权重"的前 N 个）
    int m_weightHudVerts = 0;
    // 名字→下标解析结果（只在 m_editDirty 时重算）
    // ⚠ S-D 起是 `ResolvedTrack`（轨道 + 缓动表 + 预烘曲线），不再是裸的
    //   `assets::AnimTrack`。没有缓动的轨道内部 `hasEase == false`，
    //   采样直接转发 `assets::sampleTrack` —— 行为与 R7 逐位相同。
    std::vector<animation::ResolvedTrack> m_editResolved;
    animation::ResolveStats m_editStats;
    bool m_editDirty = true;
    std::string m_editPath;
    char m_editPathBuf[512] = {0};
    char m_poseNameBuf[64] = {0};
    // 三段式的中间结果。Details 的 TRS 编辑器与 gizmo 都读它。
    animation::Pose m_pose;
    // 每骨世界矩阵（**只在 Pose Mode 下算**：多一次 FK，非 Pose Mode 不花）
    std::vector<glm::mat4> m_worldMats;
    bool m_scriptRan = false;

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
