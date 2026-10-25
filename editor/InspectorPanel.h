#pragma once
// ============================================================
// editor/InspectorPanel —— 选中实体的属性编辑
//
// 面板本身只做"显示 + 把改动落到组件上"，撤销记录交给两个辅助函数：
//
//   fieldEdit()    —— 连续型控件（DragFloat / SliderFloat / ColorEdit /
//                     DragFloat3）。ImGui 同一时刻只有一个 active item，
//                     所以"激活时抄旧值、失活时入栈"能把整段拖拽
//                     合并成**一条**撤销记录，不会把历史刷满。
//   discreteEdit() —— 离散型控件（Checkbox / Combo）。一次点击就是一次
//                     操作，直接用点击前后两个值入栈。
//
// 两者都走 RawBytesEditCommand（见 Command.h）：它不关心目标是 float、
// glm::vec3 还是 bool，只按字节数拷贝，所以这里不需要为每种字段写命令类。
//
// 有一个坑必须点名：**材质是共享资产**。AssetManager 按名字缓存 Material，
// 两个实体可以指向同一个 Material*。改材质因子会影响所有使用者，而且
// 它不在场景 JSON 里（只记来源描述）——所以材质编辑只能用 value 命令，
// 不能用整场景快照命令去做撤销。
// ============================================================

#include "EditorContext.h"
#include "ThumbnailCache.h"

#include "ecs/Entity.h"

#include <glm/glm.hpp>

#include <imgui.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace assets {
struct Material;
class Texture;
} // namespace assets

namespace ecs {
struct MeshComponent;
struct MeshPrimitiveParams;
} // namespace ecs

namespace editor {

class InspectorPanel {
public:
    explicit InspectorPanel(EditorContext& ctx) : m_ctx(ctx) {}

    void draw();

    // ---- 左侧页签（Inspector 的一级导航）----
    //
    // 为什么要加这一层：以前所有分区竖直堆在一条滚动条里 —— 选中一个球之后,
    // 要滚过 Transform / Visibility / Mesh / Material / Collision / Decal /
    // Reflection Probe / Particle Emitter / Light / Script 才找得到要调的那一项。
    // 改成"左边一列图标 + 右边一页内容"之后，视线只需要在一个方向上移动。
    //
    // **枚举顺序 = 图标栏自上而下的顺序 = 整数值**。下面 .cpp 里的
    // kTabs[] 表与它逐项对应，改一处必须同步改另一处（编译期不会报错，
    // 错了只会表现为"点了 A 出来 B"）。
    enum class InspTab : int {
        Object = 0,  // 物体属性：变换 / 可见性
        Tool,        // 工具：网格生成参数 / 脚本 / 增删组件
        Render,      // 渲染：灯光 / 贴花 / 反射探针
        Output,      // 输出：视口与画面的输出信息
        Scene,       // 场景：统计信息
        World,       // 世界属性：相机 / 主光
        Particles,   // 粒子系统
        Physics,     // 物理系统：碰撞体
        Material,    // 材质
        Animation,   // 动画：蒙皮与片段播放
        All,         // 全部：旧的"一长条堆叠"视图
        Count,
    };

    // 当前 / 指定页签的读写。落盘走 EditorApp 的 LayoutState（见
    // EditorLayoutIO.h 的 inspTab），所以这里是**纯状态访问**，不碰文件。
    int activeTab() const { return m_tab; }
    void setActiveTab(int t);
    static int tabCount();
    static const char* tabId(int t);          // 持久化 & env 用的稳定名
    static int tabIndexById(const char* id);  // 找不到返回 -1

    // ---- E11 自动化自检（由 Console `insp.selftest` 调用）----
    // 覆盖三件事：剪贴板 Copy→改值→Paste 往返一致；Reset to Default 回到
    // 构造默认；折叠状态按名持久化（面板级 map，与当前实体无关）。
    // 成功返回 true，out 填一行可断言的详情。
    bool selfTest(std::string& out);

private:
    void drawSceneryFallback();  // 没选中时显示相机 / 主光 / 场景信息

    // ---- 页签导航与内容派发 ----
    // drawNavBar() 自己开一个**很窄的子窗口**（宽度 = 一个方形按钮），
    // 调用方紧接着 SameLine() + BeginChild 开内容区。
    void drawNavBar();
    void drawTabContent(ecs::Entity e);
    void drawTabObject(ecs::Entity e);
    void drawTabTool(ecs::Entity e);
    void drawTabRender(ecs::Entity e);
    void drawTabOutput();
    void drawTabScene();
    void drawTabWorld(ecs::Entity e);
    void drawTabParticles(ecs::Entity e);
    void drawTabPhysics(ecs::Entity e);
    void drawTabMaterial(ecs::Entity e);
    void drawTabAnimation(ecs::Entity e);
    void drawTabAll(ecs::Entity e);

    // 该页签需要的组件在这个实体上没有 → 画一行"这里为什么是空的"。
    // 比直接留白好：留白会让人以为面板坏了。
    void tabEmptyHint(const char* what);

    // 相机 / 主光 / 场景统计三段，从 drawSceneryFallback 里拆出来 ——
    // 这样"没选中物体"时 World / Scene 页签仍然有内容。
    void drawCameraSection();
    void drawSunSection();
    // 渲染细节（全局）：从 render::FeatureRegistry 生成控件 —— 与 Engine
    // 面板是**同一份存储、同一套控件**（都写 paramPtr），这里只是把
    // "渲染"相关的分组挑出来放到渲染页签里，省得为了调 SSR/阴影就得开
    // 另一块面板。与选中哪个物体无关。
    void drawRenderSettingsSection();
    // 物体级渲染属性（可见性 / 投射阴影 / 逐物体 LOD 偏置）。
    void drawObjectRenderSection(ecs::Entity e);
    // ---- R7-1 物理页签的两段 ----
    // ① 世界级：整份场景的物理参数（重力/步长/求解器/休眠）+ 运行态只读统计。
    //    与"选中哪个物体"无关，所以没选中任何东西时页签也不空。
    void drawPhysicsWorldSection();
    // ② 物体级：选中实体的 RigidBodyComponent（没有就提供"添加"按钮）。
    void drawRigidBodySection(ecs::Entity e);
    // ---- M2（G07~G09）：四个玩法组件 ----
    // 四段都是"**只有组件存在时才出现**"（添加统一走 Tool 页签的
    // Add Component，与贴花 / 探针 / 粒子发射器同一条链路）——
    // 添加入口分散到每段里，迟早会出现两套默认值。
    void drawCharacterSection(ecs::Entity e);        // G07 角色控制器
    void drawTriggerSection(ecs::Entity e);          // G08 触发体
    void drawPhysicsMaterialSection(ecs::Entity e);  // G08 材质覆盖层
    void drawCameraComponentSection(ecs::Entity e);  // G09 相机组件 + 相机列表
    // 给选中实体挂/摘刚体（走 structuralEdit 进撤销栈）。
    void addRigidBodyTo(ecs::Entity e);
    void removeRigidBodyFrom(ecs::Entity e);
    void drawSceneStatsSection();
    void drawTransform(ecs::Entity e);
    void drawVisibility(ecs::Entity e);
    void drawMeshAndMaterial(ecs::Entity e);
    void drawMeshSection(ecs::Entity e);
    void drawMaterialSection(ecs::Entity e);
    // 碰撞体段落：只在实体**已经有**碰撞组件时出现（添加走视口右键菜单）。
    // 不显示"Add"按钮是有意的 —— 碰撞体的默认形状要按网格算（凸包 / 胶囊），
    // 那需要有 mesh，视口菜单能顺手把 mesh 检查掉。
    void drawCollisionSection(ecs::Entity e);
    // 延迟贴花段落（R4-4）：只在实体**已经有** DecalComponent 时出现。
    // 加组件走下面的 "Add Component → Decal"，或者场景 JSON 里直接写。
    void drawDecalSection(ecs::Entity e);
    // 反射探针段落（R5-1）：只在实体**已经有** ReflectionProbeComponent
    // 时出现。加组件走下面的 "Add Component → Reflection Probe"。
    void drawReflectionProbeSection(ecs::Entity e);
    // W4-4：地形段（尺寸 / maxHeight / 4 层材质槽 / Height scale；不做笔刷）
    void drawTerrainSection(ecs::Entity e);
    // 地形层的"材质包目录 + 刷新"（2026-10-24）。放进 drawTerrainSection
    // 的 Layer materials 上方 —— 用户说材质"一般是指定一个目录里面"，
    // 那个目录就该和材质槽挨着。
    // 返回 true = 这一次真的导入了东西（调用方据此让缩略图缓存失效）。
    bool drawMaterialSourceRow();
    // 出生点段（2026-10-24）：视角模式 / 要不要生成玩家 / 出生朝向 / 启用。
    // 只在实体**已经有** GameStartComponent 时出现。
    void drawGameStartSection(ecs::Entity e);
    // GPU 粒子发射器段落（R6-1）：只在实体**已经有**
    // ParticleEmitterComponent 时出现。曲线编辑用的是"四根滑条 = 四个
    // 控制点"（路线图明确："曲线编辑先用滑条组"）。
    void drawParticleEmitterSection(ecs::Entity e);
    // 粒子页签的两段"外壳"：
    //   · Preview —— 全局总开关 + 预览倍率（不需要选中物体）
    //   · Add —— "在世界原点生成一个粒子特效" 的按钮
    void drawParticlePreviewSection();
    void drawParticleAddSection(ecs::Entity e);
    // 在**世界原点**建一个带 Transform + ParticleEmitterComponent 的实体，
    // 选中它并打开粒子总开关（否则"加了却什么都看不见"）。
    // 返回值给自动化断言用。
    ecs::Entity addParticleEffectAtOrigin();

    // 颜色渐变带编辑器（Color over Lifetime）。Unity 里是一条带停靠点的
    // 色带，Blender 里是 Color Ramp —— 这里做成两者的共同子集：
    //   · 顶部一条**预览带**（按停靠点分段线性铺开，直接看出颜色走向）
    //   · 每行一个停靠点：颜色方块 + 位置滑条 + 删除；停靠点按 t 自动排序
    //   · 底部 "Add stop"（上限 kMaxColorStops）
    // 返回 true = 本帧改过（调用方据此打签名日志 / INS-FX-RAMP-EDIT）。
    //
    // ⚠ 撤销记的是**整条带子**（target = &ramp，sizeof(ParticleColorRamp)
    //   字节），不是单个停靠点。
    //   理由：拖完一个停靠点会 `ramp.sort()` 重排，`&stops[i]` 下次就指向
    //   另一个点了 —— 按单个点记的话，"撤销"会把**别的**点改回旧值。
    //   整条带子只有 8×(4+16)+8 ≈ 168 字节，一次快照的代价可以忽略。
    //   增删停靠点走 m_ctx.structuralEdit（那会改变停靠点个数）。
    bool drawColorRampEditor(const char* id, ecs::ParticleColorRamp& ramp);
    void drawLights(ecs::Entity e);
    void drawScript(ecs::Entity e);
    void drawAudioSource(ecs::Entity e);   // G14-7 音频源（Tool 页签）
    void drawAddRemove(ecs::Entity e);

    // 内置图元的生成参数拖完了：重建几何 + 入撤销栈
    void commitPrimitiveEdit(ecs::Entity e, const ecs::MeshComponent& before);

    // ---- E11 分区折叠 / 搜索 / 右键菜单 ----
    //
    // sectionHeader() 统一替代裸 CollapsingHeader：
    //   · 折叠状态按**分区名**记忆（m_sectionOpen），换实体不重置；
    //   · 顶部搜索词命中分区名或字段关键词才画该分区（否则整段跳过）；
    //   · 传入有效实体时，标题上挂右键菜单（Copy / Paste / Reset）。
    //   key 为折叠记忆与搜索用的稳定名（默认同 name；如 "Script (.vks)" 用 "Script"）。
    bool sectionHeader(const char* name, bool defaultOpen = false,
                       ecs::Entity e = ecs::Entity{}, const char* key = nullptr);

    // 分区标题的右键菜单（绑定 CollapsingHeader 这一项）
    void drawSectionContextMenu(const char* name, ecs::Entity e);

    // 搜索无命中提示 / INSP-SECTIONS 状态日志（去抖）
    void drawNoMatchHint();
    void logSectionState();

    // 组件剪贴板操作（POD 字节快照）。返回 true = 该分区支持此操作。
    bool copySection(const char* name, ecs::Entity e);
    bool pasteSection(const char* name, ecs::Entity e);
    bool resetSection(const char* name, ecs::Entity e);

    // 一个材质纹理槽（"小方块 + 加号"）。挨着对应的因子控件放一行，
    // 从 Content 面板把图片拖进来即可绑定；绑定后方块里放缩略图、右边
    // 显示**文件名**，再点右键可以清空。返回 true 表示本帧改过。
    //   slotId   —— 同时也是日志 tag（MAT-SLOT <slotId>）与 PushID
    //   srgb     —— 颜色贴图传 true（albedo / emissive），数据贴图传 false。
    //               这个参数同时决定缓存键的后缀，见 .cpp 里的 slotCacheKey
    //   maxWidth —— 这一行剩下的水平空间：文件名按它裁剪，避免把窗口撑出去
    //   logTag   —— 矩形日志的 tag 前缀。默认 "MAT-SLOT"（材质区），
    //               贴花区传 "DECAL-SLOT" —— 两个区的槽位名会重名
    //               （都有 albedo / normal），日志里必须能分开。
    bool drawTextureChip(const char* slotId, const char* hintText,
                         assets::Texture** slot, bool srgb, float maxWidth,
                         const char* logTag = "MAT-SLOT");

    // ---- 撤销辅助（定义在头里，模板需要可见）----
    //
    // 只在编辑态记录历史。原因不只是"运行态的编辑反正会被丢弃"：
    // Stop 会 clear() 运行态世界，命令里存的目标指针当场变成悬垂指针，
    // 之后再按撤销就是往已释放内存里写。所以运行时一律不记录。
    template <typename T>
    void fieldEdit(EditorContext& ctx, const char* name, T* target,
                   const T& before) {
        if (!ctx.isEditing()) {
            m_armTarget = nullptr;
            return;
        }
        if (ImGui::IsItemActivated()) {
            m_armTarget = target;
            m_armSize = sizeof(T);
            m_armName = name;
            m_armBefore.assign(
                reinterpret_cast<const std::uint8_t*>(&before),
                reinterpret_cast<const std::uint8_t*>(&before) + sizeof(T));
        }
        if (m_armTarget != target || m_armSize != sizeof(T)) return;
        if (!ImGui::IsItemDeactivatedAfterEdit()) return;

        if (m_armBefore.size() == sizeof(T) &&
            std::memcmp(m_armBefore.data(), target, sizeof(T)) != 0) {
            ctx.commands().pushAlreadyApplied(
                std::make_unique<RawBytesEditCommand>(
                    m_armName, target, m_armBefore.data(), target, sizeof(T)));
            ctx.setStatus(m_armName);
            ctx.dirty() = true;
        }
        m_armTarget = nullptr;
    }

    template <typename T>
    void discreteEdit(EditorContext& ctx, const char* name, T* target,
                      const T& before) {
        if (!ctx.isEditing()) return;
        if (std::memcmp(&before, target, sizeof(T)) == 0) return;
        ctx.commands().pushAlreadyApplied(std::make_unique<RawBytesEditCommand>(
            name, target, &before, target, sizeof(T)));
        ctx.setStatus(name);
        ctx.dirty() = true;
    }

    EditorContext& m_ctx;

    // 材质纹理槽的缩略图。和 ContentBrowser 共用同一份实现（同一张 GPU
    // 纹理 + ImGui 句柄的生命周期管理），见 ThumbnailCache.h。
    ThumbnailCache m_thumbs;

    // 正在拖拽的控件（ImGui 同时只有一个 active item，单槽位足够）
    const void* m_armTarget = nullptr;
    std::size_t m_armSize = 0;
    std::string m_armName;
    std::vector<std::uint8_t> m_armBefore;

    // 脚本路径的输入缓冲（跟着选中项走）
    ecs::Entity m_scriptBufFor{};
    char m_scriptBuf[256] = {0};

    // 音频素材路径的输入缓冲（同上：换实体就重灌）。
    // `AudioSourceComponent::clip` 是 std::string，InputText 绑不了它。
    ecs::Entity m_audioBufFor{};
    char m_audioBuf[256] = {0};

    // 动画片段名的输入缓冲（同上：换实体就重灌）。
    // clipName 是 std::string，InputText 不能直接绑它，必须有个 char 缓冲。
    ecs::Entity m_skinnedBufFor{};
    char m_skinnedBuf[128] = {0};

    // Animator 的两个路径输入缓冲（A1）：`controllerPath` / `defaultClip`
    // 同样是 std::string。分两个 For 而不是共用一个 —— 同一个实体上两个
    // 框各自的"换实体重灌"时机是独立的，共用一个槽会让先填的那个每帧
    // 被另一个覆盖（症状：打着字突然跳回原值）。
    ecs::Entity m_animCtrlBufFor{};
    char m_animCtrlBuf[256] = {0};
    ecs::Entity m_animDefBufFor{};
    char m_animDefBuf[128] = {0};
    // 弹簧骨的前缀列表（逗号分隔）。与上面两个同模式：buffer 跟着实体走，
    // 换选中目标时重新灌一次，避免"在 A 上打的字出现在 B 上"。
    ecs::Entity m_springPfxBufFor{};
    char m_springPfxBuf[256] = {0};

    // 两骨 IK 的三个骨名（第八刀）。共用**一个** For 标记：三个框属于同
    // 一组，换实体时一起重灌；分开三个 For 没有任何收益，只是三个字段。
    ecs::Entity m_ikBufFor{};
    char m_ikRootBuf[64] = {0};
    char m_ikMidBuf[64] = {0};
    char m_ikEndBuf[64] = {0};

    // 骨骼挂点的宿主名 + 骨名。宿主名比骨名长（实体名可能是
    // "alicia (imported)" 这种），所以给 128。
    ecs::Entity m_attachBufFor{};
    char m_attachHostBuf[128] = {0};
    char m_attachBoneBuf[64] = {0};

    // Mesh / Light 面板"这一帧到底画了哪些参数"的签名。
    //
    // 用途有两个：一是**验证**（tools/verify_mesh_params.py 靠这行日志断言
    // "Cube 只有 Size、Sphere 多出 Segments/Rings"这类结构性差异，比去截图
    // 里认控件靠谱得多）；二是排查"为什么这个属性没出现"。
    // 只在签名变化时打一行，不会刷屏 —— 和 logRect 的去抖是同一个思路。
    std::string m_lastMeshParamSig;
    std::string m_lastLightParamSig;
    // 贴花段同理：DECAL-PARAMS labels=... 只在签名变化时打一行。
    std::string m_lastDecalParamSig;
    // 探针段同理（PROBE-PARAMS）。
    std::string m_lastProbeParamSig;
    // 粒子发射器段同理（FX-PARAMS）—— 验证脚本靠它断言"这个分段真的有
    // 这些控件"（比去截图里认控件靠谱）。
    std::string m_lastFxParamSig;
    // 粒子面板的一键"全展开 / 全折叠"。
    //   -1 = 不强制（常态）；0 = 本帧强制全折叠；1 = 本帧强制全展开。
    // 为什么需要这个瞬态标志：ImGui 的 SetNextItemOpen(cond = Appearing)
    // 只在节点**第一次出现**时生效，之后状态归 ImGui 自己的 storage 管，
    // 所以"一键全展开"必须用 cond = Always 强推一帧 —— 而 Always 每帧都
    // 推就会让用户点不动那些折叠标题。测完这一帧立刻复位。
    int m_fxForceModules = -1;

    // ---- 左侧页签状态 ----
    // 当前页签（`InspTab` 的整数值）。切页签**不动** m_sectionOpen ——
    // 折叠状态是按分区名记的，所以来回翻页签既不会丢折叠，也不需要重建。
    int m_tab = static_cast<int>(InspTab::Object);
    // INSP-TAB 日志的去抖签名（页签名 + 选中的实体）。只在变化时打一行，
    // 供 tools/verify_inspector_tabs.py 断言"点了哪个页签"。
    std::string m_lastTabSig;
    // INS-TAB-EMPTY 的去抖签名（"<页签>/<缺什么组件>"），见 tabEmptyHint。
    std::string m_lastEmptySig;

    // ---- E11 分区折叠 / 搜索 / 剪贴板 ----
    // 折叠状态**按分区名**持久化：面板级成员，切实体、切场景都保留。
    // 值的权威来源仍是 ImGui 自己的 window storage（用户点击时同步回来），
    // 这里存一份是为了"第一帧施加初始值"与日志可断言。
    std::unordered_map<std::string, bool> m_sectionOpen;
    // 组件搜索词：非空时只画命中的分区（分区名或字段关键词）
    char m_search[64] = {0};
    // Render 页签里渲染特性块的过滤词。**和 m_search 分开**：m_search 过滤
    // 的是"有哪些分区"，这里是"分区里有哪些参数"，两件事混在一起会让
    // "搜 subsurf 结果把整个 Rendering 分区藏了"。
    char m_renderFilter[64] = {0};
    // 组件剪贴板：分区名 + 组件字节快照（Copy/Paste Properties）
    std::string m_clipSection;
    std::vector<std::uint8_t> m_clipBytes;
    int m_clipKind = 0;               // 快照对应的组件类型（见 PodKind）
    int m_sectionsDrawn = 0;          // 本帧画出的分区数（搜索"无命中"提示用）
    // INSP-SECTIONS 日志去抖签名
    std::string m_lastSectionSig;

    // ---- 地形层材质：材质包目录导入（2026-10-24）----
    // 用户原话："那里的地形材质一般是指定一个目录里面，将对应的材质信息
    // 按照之前 E:\code\cpp\venn-resource 中的材质信息的形式去自动添加，
    // 这个通过一个刷新按钮去找到最新的材质信息"
    //
    // 目录与档位存在 `assets/materials/library.source.json`（loadMaterial
    // SourceSettings），这里只是 UI 的镜像 —— 首次画这一段时读一次。
    char m_matSrcDir[512] = "";
    int m_matSrcTier = 0;             // kMatSrcTiers 的下标
    bool m_matSrcLoaded = false;      // 是否已经从设置文件读过
    std::string m_matSrcStatus;       // 上次导入的结果（灰色小字）
};

} // namespace editor
