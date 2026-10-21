#pragma once
// ============================================================
// ecs/Components —— 引擎内置组件（纯数据）
//
// 对 assets 类型只做前向声明：组件里存的是指针，指针不需要完整
// 类型定义。这样依赖方向保持 assets → ecs 单向，ecs 仍是叶子层。
// ============================================================

#include "ecs/Entity.h"
#include "water/WaterBody.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <string>
#include <unordered_map>
#include <vector>

namespace assets {
class Mesh;
class Material;
class Model;
class Skeleton;
class Texture;
}

namespace ecs {

// ---------------- 标识 ----------------
struct NameComponent {
    std::string name = "Entity";
};

// ---------------- 变换（局部空间）----------------
struct TransformComponent {
    glm::vec3 position{0.0f};
    glm::vec3 rotation{0.0f};  // 欧拉角（弧度），顺序 X→Y→Z
    glm::vec3 scale{1.0f};

    glm::mat4 localMatrix() const {
        glm::mat4 m(1.0f);
        m = glm::translate(m, position);
        m = glm::rotate(m, rotation.x, glm::vec3(1, 0, 0));
        m = glm::rotate(m, rotation.y, glm::vec3(0, 1, 0));
        m = glm::rotate(m, rotation.z, glm::vec3(0, 0, 1));
        m = glm::scale(m, scale);
        return m;
    }
};

// ---------------- 层级 ----------------
// 世界矩阵 = 沿父链累乘 localMatrix（由 Scene::worldMatrix 递归计算）
struct HierarchyComponent {
    EntityId parent = kInvalidEntity;
    std::vector<EntityId> children;

    // 这个节点是不是"文件夹"（纯分组节点）。
    //
    // 引擎里没有"非实体的目录"这种概念（层级在序列化时写成数组下标），
    // 所以 Hierarchy 里的"文件夹"本质上就是一个空物体 —— 但**用户看得见
    // 的差别**必须存在：树里要显示成目录图标、名字用另一种颜色。
    // 光靠"没有网格组件"猜是不可靠的（一个暂时没内容的空节点会被误判成
    // 文件夹），所以老老实实存一个显式标记。见 editor/SceneHierarchy。
    bool group = false;
};

// ---------------- 渲染 ----------------

// 内置图元的**生成参数**。只描述"这颗 mesh 是怎么生出来的"，不参与渲染。
//
// 为什么要存这个：Blender 里加完一个图元，半径 / 分段数这些生成参数是
// 可以回头再改的（改完重建几何）。这里就是同一套 —— MeshComponent 记住
// 当初用的是什么参数，Inspector 把参数显示出来、允许拖拽调整，调完按新
// 参数重新生成一颗 mesh。OBJ / glTF 没有这些参数（几何由文件决定），
// 所以 primitive 是 None。
//
// ⚠ 别和 editor::PrimitiveKind 搞混：那个是"Add 菜单里能点什么"（还包含
// PointLight / SpotLight / Empty），这个只是网格几何的生成方式。
//
// 单独拆成 struct 是为了让撤销命令能整体按值拷贝（见
// editor::PrimitiveEditCommand）—— 参数和几何必须一起回滚。
enum class MeshPrimitive : int {
    None = 0,  // 非内置图元（OBJ / glTF / 未知）
    Cube,      // size
    Plane,     // size
    Sphere,    // radius, segments, rings
    Cylinder,  // radius, segments
};

struct MeshPrimitiveParams {
    MeshPrimitive primitive = MeshPrimitive::None;
    float size = 1.0f;     // Cube / Plane：边长
    float radius = 0.5f;   // Sphere / Cylinder
    int segments = 48;     // Sphere / Cylinder：经向分段
    int rings = 24;        // Sphere：纬向分段

    bool isBuiltin() const { return primitive != MeshPrimitive::None; }
};

struct MeshComponent {
    assets::Mesh* mesh = nullptr;
    MeshPrimitiveParams params;  // 只有内置图元才有意义
};

struct MaterialComponent {
    assets::Material* material = nullptr;
};

// ---------------- 蒙皮网格（骨骼网格体）----------------
// 挂在**模型根实体**上（不是每个 SubMesh 实体）：一个骨骼网格体的所有
// primitive 共享一份骨架与动画状态。渲染层遍历带此组件的实体，按
// Skeleton::sample 采样出关节矩阵写进骨骼 SSBO，再给每个 SubMesh 的
// push.skin 填上 jointBase（见 Renderer）。
//
// 与 MeshComponent 并存：骨骼网格体的每个 primitive 仍是普通
// MeshComponent + MaterialComponent（静态路径），多一个本组件声明
// "这棵子树是蒙皮的"。jointBase 是派生数据，渲染层每帧填。
struct SkinnedMeshComponent {
    assets::Model* model = nullptr;      // 含 Skeleton 的模型
    // 当前播放的动画 clip 名（空 = 静止姿势 / rest pose）
    std::string clipName;
    float time = 0.0f;                    // 当前动画时刻（秒），由上层推进
    float speed = 1.0f;                   // 播放速度倍率
    bool playing = true;                  // 是否播放
    // 播到末尾怎么办：true → 绕回开头；false → 停在最后一帧。
    // 加它之前 time 无人推进（全仓库唯一写入点是预览窗口的滑条），
    // 所以这个语义是在 A0-3 接上时间推进时一起定下来的：
    // **默认 true**（一段 Walk 播完应该接着走，而不是定格）。
    bool loop = true;

    // 派生数据：本模型在骨骼 SSBO 里的关节矩阵起始下标（渲染层填）。
    // ⚠ 这个值是**逐实体**的。以前渲染层按 `Mesh*` 建映射
    //   （`Renderer::m_skinJointBase`），同一 FBX 实例化两次时第二个会
    //   覆盖第一个 → 多个角色串成同一个姿势。现在权威在这里。
    int jointBase = -1;

    // 派生数据：本实体的关节矩阵输出缓冲（每帧由 AnimatorSystem 重算，
    // 渲染层只负责 memcpy 进 SSBO）。长度 = 骨架关节数。
    // ⚠ 不要对它做按字节的撤销快照（它是 vector，memcpy 一个 std::vector
    //   是未定义行为）——Inspector 改动画字段走的是逐字段命令。
    std::vector<glm::mat4> jointMatrices;
};

// ---------------- 动画器（角色链 A1）----------------
//
// 与 SkinnedMeshComponent 的分工：
//   · SkinnedMeshComponent = "这个实体是骨骼网格体" + 渲染侧派生数据
//     （jointBase / jointMatrices）+ **老场景**的播放状态
//   · AnimatorComponent   = "谁来驱动它、播什么"（本组件）
//
// 为什么不直接往 SkinnedMeshComponent 里加字段：
//   A2 状态机 / A3 层与遮罩 / A4 根运动都只对"配了控制器的角色"有意义，而
//   场景里大多数蒙皮实体（角色预览窗口的私有预览场景、只播一段待机动作的
//   道具）只需要"单 clip 直放"。塞进一个组件就得给每个新字段写"空值 =
//   老行为"的分支，Inspector 也只好显示一堆用不上的控件。
//   所以：**只有真要控制器的人**才挂本组件。
//
// ⚠⚠ 权威关系（两个组件各有一份 time/speed/playing/loop，必须写清楚）：
//   · **没有** AnimatorComponent → SkinnedMeshComponent 那四项是权威，
//     行为与 A0 逐位相同（老场景 / 预览场景走的就是这条）
//   · **有** AnimatorComponent → **本组件的四项是权威**。AnimatorSystem 每帧
//     把 `time` 镜像回 `SkinnedMeshComponent::time`（那是派生量：Renderer 的
//     ANIM-SKIN 日志、控制台 `anim.time`、截图断言都读它）。镜像的写入点
//     **只有 AnimatorSystem 一处**；Inspector 在有本组件时直接绑本组件的
//     字段，不再写 sk 的那一份 —— 否则两个写入点每帧打架，症状是
//     "拖了时间轴又弹回去"，而且日志看着还挺正常。
struct AnimatorComponent {
    // 根运动的位移契约（A4 用，这里先占位 —— 字段一次加全，别分批，
    // 每批都要重跑一遍全量）。
    enum class RootMotionMode : int {
        Off = 0,        // 动画不驱动位移（"原地跑 + 代码移动"）
        FromClip = 1,   // 位移完全由动画给（过场 / 精确对齐）
        DeltaOnly = 2,  // 位移进物理、pose 保原位（脚不打滑但不离地）
    };

    // 骨架来源。空 → 回退用 SkinnedMeshComponent::model（同一个实体的那
    // 一份）—— 这样"只有一个模型"的常见情况不用填两遍。
    assets::Model* model = nullptr;

    // 控制器资产（A2 起是 `assets/animators/*.anim.json`）。
    // **空 = 单 clip 直放模式**（A1 就是这一种），此时只用 defaultClip。
    std::string controllerPath;

    // 单 clip 直放模式播哪一段。**只在 clipName 还空着时补一次**
    // （见下面的 appliedDefaultClip）。
    std::string defaultClip;

    // ---- 权威播放状态（有本组件时以这四个为准）----
    float time = 0.0f;      // 当前时刻（秒）
    float speed = 1.0f;     // 播放速度倍率
    bool playing = true;
    bool loop = true;

    RootMotionMode rootMotion = RootMotionMode::Off;

    // ==================================================================
    // 运行时（**不序列化**）
    // ==================================================================

    // A2 的状态机游标：当前状态下标 + 在该状态里待了多久。
    // -1 = 还没初始化（A1 的单 clip 模式一直是 -1）。
    int stateIndex = -1;
    float stateTime = 0.0f;

    // defaultClip 只补一次：补过之后把 clipName 清空 = 真的看静止姿势。
    // 没有这个标记的话，系统会每帧把 clipName 重新填成 defaultClip ——
    // Inspector 上那句 "empty = rest pose" 就成了谎话（点不掉的控件比
    // 没有更糟）。
    bool appliedDefaultClip = false;

    // ---- 参数黑板（A2 的状态机从这里读）----
    // 三张表分开存而不是一个 variant：脚本侧 setFloat / setBool / setTrigger
    // 本来也是三个内建函数，分开就不必在每次读的时候做类型判断。
    // trigger 的语义是**一次性待消费事件**（true = 还没被转移吃掉）。
    // A2 会按控制器资产里的参数声明初始化它们，所以这三张表不落盘。
    std::unordered_map<std::string, float> floatParams;
    std::unordered_map<std::string, bool> boolParams;
    std::unordered_map<std::string, bool> triggerParams;
};

// ---------------- 实体 transform 关键帧（编辑器 Animation 模式）----------------
//
// 和上面的 `AnimatorComponent` **不是一回事**，两个都要有：
//
//   AnimatorComponent  播的是**骨骼动画片段**（导入的 clip / 状态机）。
//                      它动的是 joints，实体自己的 transform 很可能一点不变；
//                      数据来自 `.anim.json` 资产，实体上只存播放状态。
//
//   AnimationClipComponent  动的是**实体自身的 transform**（升降平台、
//                      平移门、旋转的机关）。没有骨架、没有 clip 资产、
//                      不需要蒙皮 —— 就是几组"时刻 → TRS"，直接存在场景里。
//
// 为什么不塞进 AnimatorComponent 里加个 `keys` 字段：这两者的时间轴语义
// 不同（骨骼动画的时间是 clip 内时间，这里的是一段独立的轨道），混在一起
// 之后播放器要判断"这次到底该驱动哪一层"，而 Inspector 上一个组件会同时
// 出现两套互斥的参数。
//
// ⚠ 关键帧是**场景数据**，必须进序列化（`je["animClip"]`）。第一版把
//   它们放在 `AnimationEditor::m_keys` 里，于是存盘再开全没了 —— 那一版的
//   注释还写着"not saved with the scene yet"，这才是真正的缺口。
struct TransformKey {
    float t = 0.0f;
    glm::vec3 position{0.0f};
    glm::vec3 rotation{0.0f};   // 欧拉角（弧度），与 TransformComponent 同序
    glm::vec3 scale{1.0f};
};

struct AnimationClipComponent {
    // 按 t 升序（写入时排序，读档时也排一次 —— 手改 JSON 的顺序不能信）
    std::vector<TransformKey> keys;

    // 时间轴总长（秒）。**不强制等于最后一帧的时刻**：留着尾部空白是常见
    // 做法（"播完停一会儿再循环"），所以它是独立的字段而不是 keys.back().t。
    float duration = 4.0f;
    bool loop = true;

    // 场景一打开就播。编辑器里默认关掉 —— 否则一进场景所有带轨道的物体
    // 都在动，根本没法摆平面。
    bool autoplay = false;

    // ---- 运行时（不参与"权威状态"，但存盘）----
    // 播放头存盘是为了"存盘那一刻播到哪，重开就接在哪"。这跟 AnimatorComponent
    // 的 time 一样。注意 `playing` **不存**：重开场景时是否在播由 autoplay
    // 决定，不该由"上次退出时刚好在播"决定。
    float time = 0.0f;
    bool playing = false;
};

// ---------------- 碰撞体 ----------------
//
// 碰撞体是**独立于渲染网格**的一块凸几何：默认按网格生成（凸包紧紧包住
// 模型 / 胶囊套住最长轴），之后用 W/E/R 手柄单独摆它 —— 这就是"碰撞框
// 可以调，但创建时刚好包裹住物体"的实现方式。
//
// 几何存在**自己的局部空间、且已经按自身中心居中**，位置/旋转/缩放分开
// 存。居中这一步很关键：否则缩放手柄会把碰撞体从实体原点往外拉，而不是
// 绕它自己放大。
//
// 凸包顶点是**派生数据**（由网格顶点经 QuickHull 算出），不进场景 JSON：
// 读盘时按 mesh 重新生成。和网格 / 材质"只存来源、读时重建"是同一条思路。
enum class ColliderShape : int {
    ConvexHull = 0,  // 按网格几何求凸包（QuickHull）
    Capsule = 1,     // 胶囊体（轴 = 碰撞体局部 +Z）
};

struct CollisionComponent {
    ColliderShape shape = ColliderShape::Capsule;

    // 碰撞体相对实体的 TRS（默认 = 刚好包裹物体）
    glm::vec3 position{0.0f};
    glm::vec3 rotation{0.0f};  // 欧拉角（弧度），顺序 X→Y→Z，与 Transform 一致
    glm::vec3 scale{1.0f};

    // 胶囊参数（shape == Capsule 时有效）
    float capsuleRadius = 0.5f;
    float capsuleHalfHeight = 0.5f;  // 圆柱段半高，不含两端半球

    // 凸包顶点（碰撞体局部空间，已居中）。派生数据，见上面的说明。
    std::vector<glm::vec3> hullPoints;
    // 凸包的边（索引进 hullPoints；无向、已去重）。
    // 只有**线框绘制**要用它：凸包的面是三角形，直接画面等于画一团糊，
    // 而画边才是"碰撞框"该有的样子。同样是派生数据。
    std::vector<glm::uvec2> hullEdges;

    // 参与"不能互相穿过"的求解。关掉就是纯触发器 —— 仍然可视化、
    // 仍然能被相机挡住，但不会推开别的物体。
    bool solid = true;

    // 画不画碰撞框线框（Inspector 里可切）。编辑中（W/E/R 调整）时
    // 无论开关都强制显示，否则调整时看不见自己在动什么。
    bool showWireframe = true;

    glm::mat4 localMatrix() const {
        glm::mat4 m(1.0f);
        m = glm::translate(m, position);
        m = glm::rotate(m, rotation.x, glm::vec3(1, 0, 0));
        m = glm::rotate(m, rotation.y, glm::vec3(0, 1, 0));
        m = glm::rotate(m, rotation.z, glm::vec3(0, 0, 1));
        m = glm::scale(m, scale);
        return m;
    }
};

// ---------------- 刚体（R7-1）----------------
//
// CollisionComponent 只描述"有个碰撞体"（给编辑器的碰撞预览 + 位置分离用，
// 没有动力学）。本组件才是"这东西**受物理支配**"：Play 时编辑器会按它给
// 每个实体在 physics::PhysicsWorld 里建一个刚体，每帧把解算出来的变换写回
// TransformComponent；Stop 时整个运行态被丢弃，于是天然回滚。
//
// ⚠ 只有 Play 态参与仿真（与相机/脚本/玩家的处理方式一致）。编辑态摆好的
//   位置永远是"初始条件"，不会被物理改掉 —— 这是 ECS 存档、撤销、序列化
//   全都依赖的前提。

// 刚体的运动类型。名字对应 physics::BodyType。
enum class PhysicsBodyType : int {
    Static = 0,     // 永不动（墙 / 地板），逆质量 = 0
    Kinematic = 1,  // 由速度驱动（平台 / 门），不受重力与冲量影响
    Dynamic = 2,    // 完整动力学
};

// 碰撞形状。Auto = 按实体的 CollisionComponent / 网格包围盒推断，
// 这样"先加网格再加刚体"不用手填尺寸。
enum class PhysicsShapeType : int {
    Auto = 0,
    Sphere = 1,
    Box = 2,
    Capsule = 3,
    ConvexHull = 4,
};

struct RigidBodyComponent {
    PhysicsBodyType bodyType = PhysicsBodyType::Dynamic;
    PhysicsShapeType shape = PhysicsShapeType::Auto;

    // ---- 显式形状尺寸（shape != Auto 时生效）----
    float radius = 0.5f;             // Sphere / Capsule
    float halfHeight = 0.5f;         // Capsule 的圆柱段半高
    glm::vec3 halfExtents{0.5f};     // Box

    // ---- 质量 ----
    // mass <= 0 → 按 density × 体积算（默认路径：改尺寸时质量自动跟着变）。
    float mass = 0.0f;
    float density = 500.0f;

    // ---- 材质 ----
    float staticFriction = 0.6f;
    float dynamicFriction = 0.4f;
    float restitution = 0.0f;        // 弹性：0 = 完全非弹性
    float linearDamping = 0.05f;
    float angularDamping = 0.05f;

    // ---- 行为开关 ----
    bool useGravity = true;
    bool allowSleep = true;

    // ---- 初速度（Play 开始的那一刻施加，随后就交给求解器）----
    glm::vec3 initialVelocity{0.0f};
    glm::vec3 initialAngularVelocity{0.0f};
};

// ---------------- 角色控制器（G07）----------------
//
// 与 RigidBodyComponent 的 Dynamic 体是**两种东西**：
//   · 刚体：被力/冲量驱动，台阶会卡、斜坡会滑 —— 适合箱子、碎块
//   · 角色：位置由 physics::stepCharacter 直接决定（运动学），能迈台阶、
//     能贴地、不会反弹 —— 适合玩家 / NPC
//
// ⚠ 与 PlayerComponent 的关系：PlayerComponent 是编辑器 Play 态那套"便携
//   移动"（它有自己的 WASD 代码路径）。两者**互斥**；同时挂时角色控制器
//   优先（Inspector 里会提示）。
struct CharacterControllerComponent {
    // 尺寸：position 是胶囊**中心**，总高 = 2 × (halfHeight + radius)
    float radius = 0.35f;
    float halfHeight = 0.55f;

    // 行走
    float stepHeight = 0.35f;
    float slopeLimitDeg = 46.0f;
    bool groundSnap = true;
    float groundSnapDistance = 0.30f;

    // 运动
    float gravityScale = 1.0f;
    float maxSpeed = 4.5f;
    float sprintMultiplier = 2.0f;
    float jumpSpeed = 5.0f;
    float acceleration = 40.0f;
    float airControl = 0.25f;
    float turnSpeed = 12.0f;      // 转身插值速度（1/s）
    float pushStrength = 1.0f;

    bool enabled = true;

    // ---- 运行态（不序列化）----
    glm::vec3 velocity{0.0f};
    glm::vec3 position{0.0f};     // 胶囊中心（每帧由控制器写回）
    float bodyYaw = 0.0f;         // 当前朝向（平滑转身用）
    bool onGround = false;
    float groundNormalZ = 1.0f;
    bool started = false;         // 本轮 Play 的第一帧（初始化 bodyYaw）
};

// ---------------- 触发体（G08）----------------
//
// 挂在**已经有 CollisionComponent** 的实体上：那个碰撞体的几何就是触发
// 体积。语义与 `CollisionComponent::solid=false` 的区别是**有没有事件**：
//   · solid=false       —— 只可视化、不推开、不报事件
//   · TriggerComponent  —— 不推开，但每帧产出 Enter / Stay / Exit
//
// 为什么要一个独立组件而不是复用 solid 标志：事件是有成本的（每帧对每个
// 触发体做一次重叠测试），必须能"只为真的需要事件的体积付这笔钱"。
struct TriggerComponent {
    bool enabled = true;
    // 只报告带这些层的实体（位掩码，0xFFFFFFFF = 全收）。
    unsigned mask = 0xFFFFFFFFu;
    // Stay 事件每帧都派发是有意义的（"站在火里持续掉血"），但它也是
    // 事件量的主要来源 —— 不需要时可以关掉，只留 Enter / Exit。
    bool reportStay = true;
};

// ---------------- 物理材质（G08）----------------
//
// 与 RigidBodyComponent 里那四个字段的关系：本组件是**覆盖层**。
// 没有它 → 行为与 G08 之前逐字段相同（这是硬约束：既有物理用例的轨迹
// 一位都不许变）；有它 → 用这里的值。
//
// 为什么拆出来而不是从 RigidBody 里删字段：摩擦/弹性属于"表面属性"，
// 一个箱子换材质不该动它的质量与初速度；而 RigidBody 的生命周期更长
//（Play 开始时就烘成 physics::RigidBody 了）。覆盖层的写法让两者解耦，
// 又不用改任何既有场景。
struct PhysicsMaterialComponent {
    float staticFriction = 0.6f;
    float dynamicFriction = 0.4f;
    float restitution = 0.0f;
    float density = 500.0f;
    // ⚠ 这里**没有** restitutionThreshold：弹性速度阈值是
    //   physics::Settings 上的**世界级**参数（Solver 用它统一判断"这一下
    //   算不算撞"），按物体分开需要改求解器契约。别在这儿放一个吃不到的
    //   字段 —— "界面上能调、实际没接线"比没有更糟。
};

// ---------------- 相机组件（G09）----------------
//
// 一台相机 = 一个实体 + 本组件。`scene::Camera` 仍是**渲染用的那一台**；
// 本组件决定"谁来当它、怎么摆"。
//
// 优先级：每帧挑 priority 最大的启用相机。同分时取**实体 id 较小**的
//（确定性：不能因为遍历顺序变了就换镜头）。
//
// ⚠ 两种用法，别混：
//   · **跟随相机（第三人称）** → 挂在**角色实体**上（与
//     CharacterControllerComponent 同一个实体）。姿态由 PlayerController
//     算好交给 CameraSystem；本组件的 fov / 跟随参数就是给它的。
//   · **固定机位（过场 / 备用镜头）** → 独立实体，位置与朝向取它的世界
//     矩阵，fov 取本组件（见 CameraSystem::poseFromEntity）。
//
// ⚠ 这里**故意没有** nearClip / farClip / orthographic / orthoHeight /
//   damping：渲染端目前用的是全局近远平面（改它要动投影矩阵与深度精度，
//   near 一变所有像素断言都得重定），正交相机则整条渲染路径都还没写。
//   "界面上能调、实际没接线"比没有更糟 —— 等真接线时再加回来。
struct CameraComponent {
    float fov = 45.0f;

    int priority = 0;
    bool enabled = true;

    // ---- 跟随（运行态 PlayerController 也用它）----
    // 注视点相对**胶囊中心**。0.76 ≈ 头顶往下 0.14（1.8 m 高的胶囊）。
    // ⚠ 是相对胶囊中心、不是相对实体原点（原点在脚底）：跟随相机每帧
    //   拿的是物理状态里的胶囊中心，少一次换算是少一处错。
    glm::vec3 targetOffset{0.0f, 0.0f, 0.76f};
    float distance = 4.0f;                      // 第三人称臂长
    float minDistance = 0.6f;
    bool collideWithWorld = true;                // 臂长遇墙缩短

    // ---- 震动（**默认 0 = 关**：关掉时相机状态逐位不变）----
    float shakeAmplitude = 0.0f;   // 米
    float shakeFrequency = 24.0f;  // Hz
    // 运行态：外部通过 CameraSystem::addShake() 累加，随时间衰减
    float trauma = 0.0f;
};

struct VisibilityComponent {
    bool visible = true;      // 参与主渲染
    bool castShadow = true;   // 参与阴影贴图渲染
};

// ---------------- 延迟贴花（DBuffer，R4-4）----------------
//
// 贴花 = 一个**盒体体积** + 两张贴图。渲染时画的是盒体几何（单位立方体 ×
// 世界矩阵），但片元不采盒体自己的表面，而是：
//   ① 从深度图 texelFetch 重建该像素的**场景表面世界坐标**；
//   ② 乘 worldToLocal 投到贴花局部空间；
//   ③ 落在单位立方体内 → 采样贴花贴图，结果写进 DBuffer（三张：反照率 /
//      世界法线 / 粗糙度-金属度），再由前向 Pass 按 coverage 混合进去。
//
// 为什么绕这一圈而不干脆把贴图画在盒体表面上：贴花要"贴着"任意形状的
// 表面（地面标记 / 弹孔 / 墙上的涂鸦），贴着盒体面只能得到一个悬空的方盒。
// 这个 pass 也没有深度附件 —— 遮挡关系完全由"重建出的场景点是否在盒内"
// 决定（见 DecalBuffer 的说明）。
//
// 位置 / 朝向 / 尺寸都取自实体的 TransformComponent：
//   · position / rotation → 盒体中心与朝向
//   · scale × size         → 盒体的三个半轴长度
// 于是"移动贴花"与"移动物体"共用同一套机制（含父链累乘、撤销、序列化）。
struct DecalComponent {
    // 盒体尺寸（米，沿**实体局部轴**）。默认 1 m 立方体。
    glm::vec3 size{1.0f, 1.0f, 1.0f};
    // 不透明度：乘在贴花 albedo 的 alpha 上，也就是 DBuffer 里 coverage
    // 的上限。0 = 完全看不见（但 pass 照跑，用于对照实验）。
    float opacity = 1.0f;

    // 贴花颜色 = albedo.rgb × tint。让同一张贴图能当多种标记用。
    glm::vec3 tint{1.0f};

    // 贴花自己的 PBR 参数。**覆盖**被贴表面的对应量（按 coverage 加权）：
    // 弹孔该比周围墙面更粗糙、油渍该更有金属感，都靠它。
    float roughness = 0.7f;
    float metallic = 0.0f;
    // 法线强度：0 = 贴花不影响法线（只用场景几何法线），1 = 贴图原样。
    float normalScale = 1.0f;

    // 距离淡出（米，相机到**贴花中心**的距离）。
    // fadeEnd <= fadeStart 时这一路关闭（默认关闭 = 不做距离淡出）。
    float fadeStart = 0.0f;
    float fadeEnd = 0.0f;

    // 边缘软淡出（占半轴的比例：0 = 硬边，0.15 = 最后 15% 逐渐透明）。
    // 硬边会在盒体边界上留下一条可见的直线接缝。
    float edgeFade = 0.15f;

    bool enabled = true;

    // 贴图。为空时用缺省值（albedo → 白、normal → 平坦法线），
    // 也就是"只有一个纯色方块"，仍然是个能用的贴花。
    //   albedo —— 颜色贴图（srgb=true）
    //   normal —— 切线空间法线（srgb=false）
    assets::Texture* albedo = nullptr;
    assets::Texture* normal = nullptr;
};

// ---------------- 玩家（运行态由 WASD + 鼠标驱动）----------------
//
// Player 实体 = 玩家的"脚"。第一人称只是一个空节点（+ 胶囊碰撞体，
// 编辑态能看见占位线框，运行态不穿墙）；第三人称在它下面挂一套程序化
// 卡通小人（见 EditorScene::createPlayer 的 rig）。
//
// 相机语义（对齐市面游戏的手感差异）：
//   第一人称 —— 鼠标 = 转动**视野**（yaw/pitch），WASD 沿视线走；
//   第三人称 —— 鼠标 = **环绕角色**的轨道相机（camYaw/camPitch），
//               WASD 沿相机朝向走、角色平滑转身朝移动方向。
enum class PlayerMode : int { FirstPerson = 0, ThirdPerson = 1 };

struct PlayerComponent {
    PlayerMode mode = PlayerMode::FirstPerson;

    float eyeHeight = 1.7f;     // 第一人称眼睛离"脚"的高度
    float moveSpeed = 3.0f;     // m/s（按住 Shift ×2）
    float lookSpeed = 0.0025f;  // 鼠标灵敏度：弧度/像素

    // **视线仰角**：弧度，**正 = 抬头**（鼠标往上 → 抬头，两种模式一致）。
    // yaw 与 Camera 同约定：yaw=0 朝 +X，yaw 增大向右转。
    float yaw = 0.0f;
    float pitch = 0.0f;

    // 第三人称轨道相机（轨道方位角复用上面的 yaw）。Camera 的 pitch 是
    // "相机在目标上方的俯视角"，与视线仰角**反号**，所以驱动相机时传
    // -pitch（见 EditorApp::runPlayers）。创建第三人称玩家时 pitch 初始
    // 给 -0.35 → 视线略微向下 → 相机落在角色上方俯视。
    float camDistance = 3.5f;   // 相机离角色（注视点）的距离
    float turnSpeed = 12.0f;    // 角色朝移动方向转身的插值速度（1/s）

    // ---- 运行态派生状态（不序列化）----
    float walkPhase = 0.0f;     // 四肢摆动相位
    float bodyYaw = 0.0f;       // 第三人称角色当前朝向（平滑转身用）
    bool  started = false;      // 本轮 Play 的第一帧（初始化 bodyYaw 等）
};

// 第三人称小人的肢体标记：运行态 walk 动画按 kind 摆动对应的肢体。
// 部件本体就是普通实体（网格 + 材质 + 父子变换），序列化天然支持。
struct PlayerLimbComponent {
    enum Kind : int { Body = 0, Head = 1, ArmL = 2, ArmR = 3,
                      LegL = 4, LegR = 5 };
    int kind = Body;
};

// ---------------- 编辑器锁定（纯标记）----------------
// 上锁的实体：**视口点选会跳过它**，选中了也不给变换手柄 —— 也就是
// "看得见、但动不了"。
//
// 引擎自带的**地面**就是靠它固定住的：地面 + 栅格是编辑器的参照物，
// 不该被平移 / 缩放 / 旋转（用户明确要求），否则"参考系"本身就会跑掉。
//
// 只影响编辑器交互：渲染、脚本、将来的物理都不看它。
struct LockedComponent {};

// ---------------- 光照 ----------------
struct DirectionalLightComponent {
    glm::vec3 direction{-0.5f, -1.0f, -0.3f};  // 传播方向（指向被照物）
    glm::vec3 color{1.0f, 0.98f, 0.95f};
    float intensity = 1.0f;
    bool castsShadow = true;
    // 程序化环境光（环境 IBL 近似）的强度倍率。
    // 室内场景把它压到 0.1~0.3，局部光源才有存在感；
    // 室外开阔场景保持 1.0。
    float ambientScale = 1.0f;
};

// ---------------- 局部光源（分簇前向渲染用）----------------
// 刻意**不**存位置：位置取自实体 TransformComponent 的世界矩阵平移。
// 这样"移动灯"和"移动物体"共用同一套机制（含父链累乘），
// 动画、序列化都不用为灯单独开一条路。
//
// intensity 是 HDR 光强（可以远大于 1）：
// 距离衰减 d² 会把它压下来，室内点光常用 5~50 这个量级。
// range 必须尽量贴合真实影响范围 —— 分簇剔除的效果完全取决于它。

struct PointLightComponent {
    glm::vec3 color{1.0f, 0.95f, 0.88f};
    float intensity = 12.0f;
    float range = 5.0f;  // 影响半径（超过这个距离贡献为 0）
    bool enabled = true;
    // 是否投射阴影。**默认关**：点光要走立方体贴图的 6 个面，
    // 一层 1024² 的深度图 × 6，是聚光灯的 6 倍代价。想要影子
    // 的室内点光手动打开即可（层预算见 render::kMaxLocalShadowLayers）。
    bool castsShadow = false;
};

struct SpotLightComponent {
    glm::vec3 color{1.0f, 0.96f, 0.90f};
    float intensity = 30.0f;
    float range = 9.0f;
    glm::vec3 direction{0.0f, -1.0f, 0.0f};  // 传播方向（从灯指向被照物）
    float innerAngle = 0.28f;  // 内锥半角（弧度）：此角度内全亮
    float outerAngle = 0.52f;  // 外锥半角（弧度）：此角度外为 0
    bool enabled = true;
    // 射灯只占 **1 层**（沿锥轴的透视投影），代价很低，所以默认开
    bool castsShadow = true;

    // ---------------- R3-3 光源修饰 ----------------
    // 三者共享灯的局部正交基（由 direction 定义），所以只有射灯支持。
    //
    // 矩形面光源尺寸（米）。任一为 0 = 点状（默认，= 原行为）。
    // 非 0 时按"矩形发射体"求值：漫反射用最近点、高光用 UE4 式球光
    // 代表点 → 高光变柔、接触处不再过曝。
    float areaWidth = 0.0f;
    float areaHeight = 0.0f;
    // IES 光域网：内建 "ies:narrow" / "ies:wide" / "ies:batwing"，
    // 或运行时加载的 .ies 文件名。空 = 不用。
    std::string iesProfile;
    // light function（投影贴图 / cookie）：内建 "cookie:grid" /
    // "cookie:rings" / "cookie:bars"。空 = 不用。
    std::string lightFunction;
};

// ---------------- 脚本 ----------------
// 一个实体最多挂一个脚本文件（.vks）。
//
// 关键约定：脚本**只在运行态求值** —— 编辑器模式下不执行任何游戏逻辑，
// 这是"编辑态 / 运行态场景分离"的一部分（见 editor/EditorContext）。
// 脚本可以改本实体的 Transform / 灯光 / 材质，改动只发生在运行态副本上，
// 因此 Stop 之后编辑态场景永远是你离开时的样子。
struct ScriptComponent {
    std::string path;          // 脚本路径（相对资产根，或绝对路径）
    bool enabled = true;
    float timeScale = 1.0f;    // 脚本内部 time / dt 的缩放
};

// ---------------- 音频（G14）----------------
//
// ⚠ 组件是**配置**，不是"一个正在播放的声音"（docs/audio_roadmap.md §3.10）。
//   真正的声部活在 `audio::Mixer` 的 64 个槽里，由 `playOnAwake` 申请、
//   播完释放。200 个实体各挂一个 AudioSource 也不会变成 200 路混音。
//
// ⚠ `bus` 用 int 而不是 `audio::BusId`：`ecs` 是叶子层，不应该认识
//   `engine/audio`（反过来更不行）。数值对应 1=Music / 2=SFX / 3=Voice。
struct AudioSourceComponent {
    // 相对资产根的路径（"audio/click.wav"）。空 = 不播。
    std::string clip;

    bool playOnAwake = true;
    bool loop = false;
    // 3D 空间化。关掉 = 增益恒为 volume，与距离无关（2D 音效 / UI 音）。
    bool spatial = true;
    float volume = 1.0f;
    float pitch = 1.0f;
    // 距离衰减参数（公式见 audio/Mixer.h 的 spatialGain）
    float minDist = 1.0f;
    float maxDist = 50.0f;
    float rolloff = 1.0f;
    int bus = 2;           // 1=Music 2=SFX 3=Voice
    // 长音乐用流式（3 s 环形缓冲）；音效一律 false
    bool streaming = false;
};

// 听者标记。挂在相机实体上；**没有就用 `activeScene().camera()`**
// （运行时与编辑器 Play 都成立 —— 那里一定有相机）。
struct AudioListenerComponent {
    bool enabled = true;
};

// ---------------- 反射探针（R5-1）----------------
//
// 一个探针 = 场景里的一个**盒体**（沿实体局部轴）。渲染器从盒体中心把
// 周围环境烘成一张立方体贴图，之后任何落在这个盒体内的表面，其镜面
// 反射都从这张图里取 —— 于是背对窗口的墙/地板/金属球也能反射出屋子
// 里的东西（那是 SSR 按定义做不到的：屏幕里没有的东西它反射不了）。
//
// 盒体尺寸直接复用实体的 Transform（位置 / 旋转 / 缩放）：
//   extents 是**半轴长度**，实体的 scale 会再乘上去（所以把一个探针
//   整体放大就是把它的作用范围放大，与直觉一致）。
struct ReflectionProbeComponent {
    // 盒体半轴（米，沿实体局部轴）。默认 10×10×5 —— 一个房间的尺度。
    // 探针的**作用范围**就是这个盒子：只有盒体内部的表面才会采样它。
    glm::vec3 extents{10.0f, 10.0f, 5.0f};

    // 反射强度倍率。1 = 与 IBL 同强度；0 = 关了（但捕获照做，用于对照）。
    float intensity = 1.0f;

    // 盒投影修正。开（默认）= 把反射方向先打到盒体壁上再采样 ——
    // 贴近单面墙时会得到正确的透视（关掉的话整个房间的反射会被压成
    // 一个方向的天空，贴到平面上像一张贴纸）。
    bool boxProjection = true;

    // 边界混合距离（米）：从盒体边界往里 blend 这么宽的区域里，
    // 探针的权重线性淡出到 0。硬边会在两个相邻探针之间留下可见接缝。
    float blendDistance = 1.0f;

    bool enabled = true;
};

// ---------------- GPU 粒子发射器（R6-1）----------------
//
// 一个发射器 = 场景里的一个**点 / 盒体 / 球 / 锥**，它按 rate 持续
// 往外喷粒子；粒子在 GPU 上模拟（年龄 / 速度 / 重力 / 阻尼 / 曲线），
// 由一个 instanced billboard pass 画出来。
//
// ---- 为什么粒子数据不在组件里 ----
// 组件是"发射器的配置"，不是"粒子"。粒子（位置/速度/年龄）活在
// render::ParticleSystem 的 GPU 池子里，每帧被 compute 推进一步 ——
// 上万颗粒子不可能每帧从 CPU 塞回去（那正是本任务要消掉的瓶颈）。
// 所以这里只有**参数**，没有运行时状态；序列化也只需要存参数。
//
// ---- 位置 / 朝向 ----
// 与贴花 / 探针同一条约定：发射器的位置与朝向直接吃实体的 Transform
// （含父链累乘），所以移动发射器 = 用同一套 gizmo / 撤销 / 层级。
//
// ---- 为什么曲线是 4 个滑条而不是曲线编辑器 ----
// 路线图明确"曲线编辑先用滑条组"。4 个控制点在 t = 0、1/3、2/3、1 处
// 线性插值 —— 足够表达"先大后小""先亮后灭"这两类最常见的形状，
// 而它的存储就是一个 vec4，序列化 / 撤销 / GPU 上传全都是现成的。
// 真正的曲线编辑器（贝塞尔 + 关键帧）留给后续的粒子编辑器任务。
//
// ---- 模块划分照抄 Unity Shuriken ----
// 本组件与 Inspector 面板都按 Unity 的模块切分（Main / Emission / Shape /
// Velocity over Lifetime / Limit Velocity / Force over Lifetime / Color over
// Lifetime / Size over Lifetime / Rotation over Lifetime / Noise / Renderer），
// 这样"在 Unity 里会调粒子的人"打开面板就能直接上手 —— 参数名、单位、
// 分组顺序都刻意对齐。Blender 那边（Emitter / Physics / Render 三大块）
// 的差别主要是曲线编辑体验，本引擎用 4 点滑条曲线 + 渐变色带表达。
//
// ---- 每个模块都有 enabled 开关，默认 = 老行为 ----
// 新增模块全部默认**关**（或默认值等价于"不产生影响"），所以：
//   · 老场景加载进来逐位一致（验收"无粒子组件 = 基线不变"的延伸）；
//   · 冻结帧验证的旧断言全部继续成立。
enum class ParticleShape : int {
    Point = 0,       // 从一个点发射
    Box,             // 在盒体体积内均匀分布
    Sphere,          // 在球体内均匀分布（半径 = shapeRadius）
    Hemisphere,      // 上半球
    Cone,            // 沿实体局部 +Z 的锥体（半径 = shapeRadius，角 = spread）
    Circle,          // 底面圆盘（等价于厚度 = 0 的 Cone 底面）
    Edge,            // 沿一条线段（长度 = 2 × shapeRadius，沿局部 X）
};

// Unity 的 "Emit From"：形状的体积 / 边缘 / 壳。
enum class ParticleEmitFrom : int {
    Volume = 0,  // 实心体积内均匀（半径按 radiusThickness 加权）
    Edge,        // 只在边缘（Box 的棱 / Cone 的底圈）
    Shell,       // 只在表面上（球壳 / 盒面）
};

enum class ParticleBlend : int {
    Additive = 0,  // 加色（火 / 火花 / 魔法）—— 默认
    Alpha,         // 普通 alpha 混合（烟 / 水花）
};

// Unity 的 Renderer > Render Mode。
enum class ParticleRenderMode : int {
    Billboard = 0,           // 永远正对相机（默认）
    StretchedBillboard,      // 长轴沿速度方向拉伸
    HorizontalBillboard,     // 躺平（正对世界 Z 轴）—— 地面环形冲击波
    VerticalBillboard,       // 竖立但绕世界 Z 轴锁定 —— 火柱、瀑布
};

// Unity 的 Renderer > Sort Mode。加色混合与顺序无关，只有 Alpha 需要它。
enum class ParticleSortMode : int {
    None = 0,          // 槽位顺序（最快，加色用这个）
    ByDistance,        // 远的先画（Alpha 的正确顺序）
    OldestFirst,       // 老的先画
    YoungestFirst,     // 新的先画
};

// Unity 的 Main > Simulation Space。
enum class ParticleSimulationSpace : int {
    Local = 0,  // 发射器移动时粒子跟着走（火把、车尾灯）
    World,      // 发射出去的粒子留在世界里（烟雾、篝火）
};

// 颜色渐变的停靠点上限。8 个足够表达火焰（暗红→橙→黄→白→烟）与
// 爆炸（白→黄→橙→红→黑）这类典型色带，而 GPU 侧就是 8 个 vec4。
static constexpr int kMaxColorStops = 8;

// 一次性爆发的上限（Unity 的 Bursts 是个可增删的数组，这里固定 4 条）。
static constexpr int kMaxBursts = 4;

// 粒子的**贴图形状**（2026-10-18）。
//
// 为什么是"程序化形状"而不是贴图资产：本引擎的粒子 pass 是**一次间接绘制
// 画完所有发射器**（紧凑表 + 顶点拉取），描述符集里也没有贴图槽 —— 想要
// "每个发射器一张 PNG"就得加一个 sampler2DArray（8 层）+ 图像数组上传，
// 那是另一摊工程。这里先用片元里的距离场做出 8 种轮廓：默认的 0 号与
// 改动前**逐像素相同**（同样是 (1-r²)² 的圆盘羽化），其余几种给
// 烟 / 火花 / 光环 / 雨丝 / 雪花 / 光晕各自的边缘，视觉差异一眼可见。
//
// ⚠ 枚举值是**序列化契约**：只能在末尾追加，不能插入/重排。
enum class ParticleSprite : int {
    Disc = 0,      // 默认：软边圆盘（改动前的外观）
    Glow = 1,      // 更软的外晕 + 亮芯（魔法、闪电辉光）
    Spark = 2,     // 四角星芒（火花 / 闪光）
    Smoke = 3,     // 带噪声的团（烟、尘土）
    Ring = 4,      // 空心环（冲击波、涟漪）
    Streak = 5,    // 竖长条（雨丝、拖尾）
    Flake = 6,     // 六角雪花
    Count = 7,
};

// 颜色渐变的第 i 个停靠点：xyz = 线性 HDR 颜色，w = 位置 t ∈ [0,1]。
// 存成 vec4 而不是 (float t, vec3 rgb) 是为了让 std430 的数组步长
// 恰好是 16 字节 —— 与 GPU 那边的 `vec4 ramp[8]` 逐字对齐。
struct ParticleColorRamp {
    int count = 3;
    glm::vec4 stops[kMaxColorStops] = {
        glm::vec4(1.00f, 0.75f, 0.25f, 0.0f),   // t = 0：橙
        glm::vec4(1.00f, 0.35f, 0.05f, 0.5f),   // t = 0.5：红
        glm::vec4(0.25f, 0.04f, 0.02f, 1.0f),   // t = 1：暗红
        glm::vec4(0.0f), glm::vec4(0.0f), glm::vec4(0.0f), glm::vec4(0.0f),
        glm::vec4(0.0f),
    };
    // 停靠点必须按 t 递增（求值时只做一次线性扫描，不做排序）。
    // 面板在拖动停靠点时调用；插入排序，最多 8 个元素。
    void sort() {
        if (count < 2) return;
        if (count > kMaxColorStops) count = kMaxColorStops;
        for (int i = 1; i < count; ++i) {
            const glm::vec4 key = stops[i];
            int j = i - 1;
            while (j >= 0 && stops[j].w > key.w) {
                stops[j + 1] = stops[j];
                --j;
            }
            stops[j + 1] = key;
        }
    }
    // 求值：t 落在哪一段，段内线性插值。
    // t 在第一个停靠点之前 → 取第一个；在最后一个之后 → 取最后一个。
    glm::vec3 eval(float t) const {
        if (count <= 0) return glm::vec3(1.0f);
        if (count == 1) return glm::vec3(stops[0]);
        const float u = glm::clamp(t, 0.0f, 1.0f);
        if (u <= stops[0].w) return glm::vec3(stops[0]);
        for (int i = 1; i < count; ++i) {
            if (u <= stops[i].w) {
                const float span = stops[i].w - stops[i - 1].w;
                const float f =
                    span > 1e-6f ? (u - stops[i - 1].w) / span : 0.0f;
                return glm::mix(glm::vec3(stops[i - 1]), glm::vec3(stops[i]), f);
            }
        }
        return glm::vec3(stops[count - 1]);
    }
};

// Unity 的 Bursts 数组的一项。
struct ParticleBurst {
    // 相对发射器开始的时刻（秒）。0 = 立刻。
    float time = 0.0f;
    // 这一波发多少颗。
    int count = 0;
    // 重复几次（1 = 只发一次）。0 或 1 都表示一次。
    int cycles = 1;
    // 两次之间的间隔（秒）。cycles > 1 时才有意义。
    float interval = 0.0f;
};

struct ParticleEmitterComponent {
    // ======================================================================
    //  Main / Playback（Unity 的 "Particle System" 主模块）
    // ======================================================================
    // 每秒发射数。真实发射数是**累积**的：rate=120 在 60fps 下
    // 每帧发 2 颗，rate=90 则是"发 1 颗、停 1 颗、发 1 颗……"。
    // 累积器在 GPU 侧按"每个槽位自己的相位"实现（见 particle_sim.comp），
    // 所以任何帧率下发射都是均匀的、且完全确定（给定帧号就能算出状态）。
    float rate = 120.0f;
    // 每移动 1 米额外发射多少颗（Unity: Rate over Distance）。
    // 与 rate 叠加：静止时只有 rate 起作用，快速移动时轨迹会"拉丝"。
    // 0 = 关（默认，等价于老行为）。
    float rateOverDistance = 0.0f;

    // ---- 播放控制 ----
    // 发射时长（秒）。**0 = 无限**（默认）。
    // > 0 且 looping 时，每 duration 秒重新开始一轮（含 burst）；
    // > 0 且非 looping 时，duration 之后停止发射（已有粒子继续活完）。
    float duration = 0.0f;
    bool looping = true;
    // 预热：模拟在"第 0 秒"之前先跑一整轮，所以第一帧看到的就是
    // 稳态（Unity 的 Prewarm）。**冻结帧验证不要开** —— 它会让
    // "第一次模拟"不再是第 1 步。默认关。
    bool prewarm = false;
    // 启动延迟（秒）：这时刻之前不发射。默认 0。
    float startDelay = 0.0f;

    // ---- 生命周期 ----
    float lifetime = 1.8f;           // 基础寿命（秒）
    float lifetimeVariance = 0.2f;   // 寿命随机比例（0..1，± 这个比例）
    float startSpeed = 2.5f;         // 初速（米/秒）
    float speedVariance = 0.3f;      // 速度随机比例（0..1）
    float spread = 25.0f;            // 方向扩散半角（度），Cone 也用它

    // ======================================================================
    //  Emission（一次性爆发）
    // ======================================================================
    ParticleBurst bursts[kMaxBursts];
    // 兼容字段：等价于 bursts[0]（time = 0，cycles = 1）。
    // 序列化时若只有 "burst" 就填进 bursts[0]。
    // ⚠ 不直接用它驱动 GPU（统一走 bursts 数组），只是老的读写入口。
    // 面板显示的是 bursts 数组。

    // ======================================================================
    //  Shape
    // ======================================================================
    ParticleShape shape = ParticleShape::Point;
    glm::vec3 shapeExtents{0.5f, 0.5f, 0.5f};  // Box 的**半轴**（米）
    float shapeRadius = 0.5f;                  // Sphere / Cone / Circle / Edge
    // 体积厚度：1 = 实心（老行为），0 = 只有壳/边缘。
    // Unity 面板上叫 "Radius Thickness"。
    float radiusThickness = 1.0f;
    // 绕局部 +Z 的角度范围（度）。360 = 整圈（默认），180 = 只在半边。
    float arc = 360.0f;
    // 形状自身绕局部 +Z 的旋转（度）。把"半边锥"转到想要的方向。
    float shapeRotation = 0.0f;
    ParticleEmitFrom emitFrom = ParticleEmitFrom::Volume;

    // ======================================================================
    //  Velocity over Lifetime（Unity 的模块之一，默认关）
    // ======================================================================
    bool velOverLifeEnabled = false;
    // 三个轴向的 4 点曲线（加在速度上，米/秒）
    glm::vec4 velOverLifeX{0.0f};
    glm::vec4 velOverLifeY{0.0f};
    glm::vec4 velOverLifeZ{0.0f};
    // 曲线的坐标系：false = 发射器局部（默认），true = 世界。
    bool velOverLifeWorldSpace = false;
    // 绕三轴的轨道角速度（度/秒）—— 火旋、龙卷风的基础。
    glm::vec3 orbital{0.0f};
    float orbitalOffset = 0.0f;   // 轨道半径偏移（米）
    // 径向速度（米/秒）：正 = 从中心向外，负 = 向内聚拢。
    float radial = 0.0f;
    // 速度倍率曲线（Unity 的 Speed Modifier）：乘在 |v| 上。
    // 0,0,0,0 = 关（默认）。
    glm::vec4 speedModifier{0.0f};

    // ======================================================================
    //  Limit Velocity over Lifetime
    // ======================================================================
    bool limitVelEnabled = false;
    float limitSpeed = 1.0f;      // 速度上限（米/秒）
    float limitDampen = 0.0f;     // 超过上限时每秒损失多少比例（0..1）

    // ======================================================================
    //  Force over Lifetime
    // ======================================================================
    bool forceEnabled = false;
    glm::vec3 force{0.0f};        // 恒定加速度（米/秒²），与重力叠加
    float forceRandomize = 0.0f;  // 逐粒子随机化比例（0..1）

    // ======================================================================
    //  Rotation over Lifetime
    // ======================================================================
    float startRotation = 0.0f;            // 初始旋转（度）
    float startRotationVariance = 180.0f;  // 初始旋转随机半幅（度）
    float angularVelocity = 0.0f;          // 角速度（度/秒）
    float angularVelocityVariance = 0.0f;  // 角速度随机比例（0..1）

    // ======================================================================
    //  Noise
    // ======================================================================
    bool noiseEnabled = false;
    float noiseStrength = 1.0f;    // 扰动强度（米/秒²）
    float noiseFrequency = 0.5f;   // 空间频率（越小越"大团"）
    float noiseScrollSpeed = 0.0f; // 噪声场自身的滚动（0 = 冻结）
    float noiseDamping = 0.0f;     // 阻尼（Unity: Damping）
    int noiseOctaves = 1;          // 倍频层数（1..4）

    // ======================================================================
    //  Physics（老字段，保留）
    // ======================================================================
    // 重力（米/秒²）。Z-up 坐标系 → 默认 (0,0,-9.8)。
    glm::vec3 gravity{0.0f, 0.0f, -9.8f};
    // 每秒阻尼系数（>0 = 速度按 exp(-drag*t) 衰减）。
    // 0 = 真空（粒子只会被重力/初速支配）。
    float drag = 0.15f;
    // 模拟空间：Local = 粒子跟着发射器走；World = 留在世界里。
    ParticleSimulationSpace simulationSpace = ParticleSimulationSpace::World;

    // ======================================================================
    //  Size over Lifetime / Color over Lifetime
    // ======================================================================
    // 尺寸倍率：乘在 startSize 上。默认"先大后小"是火焰的典型形状。
    glm::vec4 sizeOverLife{0.35f, 1.0f, 0.75f, 0.0f};
    // 不透明度：默认"快速涨到 1、末段归零"。
    glm::vec4 alphaOverLife{0.25f, 1.0f, 0.7f, 0.0f};
    // 颜色渐变（默认就是老的"橙→红→暗红"三段）。
    ParticleColorRamp colorRamp;

    // ======================================================================
    //  Renderer
    // ======================================================================
    float startSize = 0.18f;                  // 基础尺寸（米，billboard 边长）
    // 贴图形状（见 ParticleSprite）。默认 Disc = 与改动前逐像素相同。
    ParticleSprite sprite = ParticleSprite::Disc;
    ParticleBlend blend = ParticleBlend::Additive;
    ParticleRenderMode renderMode = ParticleRenderMode::Billboard;
    ParticleSortMode sortMode = ParticleSortMode::None;
    // 拉伸系数：0 = 纯 billboard（永远正对相机）；>0 = 沿速度方向拉伸
    // （拉伸长度 = 速度 × 这个系数，雨 / 火星 / 快速移动的碎屑）。
    // ⚠ renderMode = StretchedBillboard 时才生效。
    float stretchScale = 0.0f;
    // 自发光强度：粒子不参与场景光照（它是自发光贴片），亮度靠它。
    float emissive = 2.5f;

    // ======================================================================
    //  Soft particles（本引擎特有，Unity 里是粒子着色器的软混合选项）
    // ======================================================================
    // 开 = 粒子靠近不透明表面时按深度差淡出（消除"硬切在墙里"的接缝）。
    // 关 = 只做硬深度遮挡。两者都不需要深度写入（见 particle.frag）。
    bool softParticles = true;
    float softDistance = 0.5f;  // 淡化距离（米）

    bool enabled = true;
};

// ============================================================
// ScriptVarsComponent —— 脚本的**逐实体黑板**（S2）
//
// `setAttr(e, "hp", 80)` 写的地方。为什么是组件而不是"脚本实例里的一个
// map"：
//   · 黑板按**实体**索引，而脚本实例按"实体 + 脚本路径"索引。一个实体上
//     可以挂两个脚本，它们必须看到同一份 hp；反过来实体没挂脚本时
//     Inspector 也该能读出 hp（S5）。
//   · 实体被删掉时组件随之消失 —— 不会像实例表那样留下悬垂条目。
//
// ⚠ 这个组件**不进存档**（运行期状态）。要"跨存档保住的东西"用
//   `worldAttr` → `Scene::scriptVars`，两者刻意分开：逐实体黑板是
//   "这一局的血量"，世界黑板是"这关的进度"。
// ⚠ 只存 double。vec3 请拆成 x/y/z 三键 —— 黑板是玩法用的，不是通用
//   数据容器，类型越少越不容易在脚本里搞错。
// ============================================================
struct ScriptVarsComponent {
    std::unordered_map<std::string, double> values;
};

// ============================================================
// TerrainComponent —— 地形（W4）
//
// 一个地形实体 = 一份 `.vterrain` 资产 + 一个变换。**没有网格组件**：
// 几何完全由资产里的高度图与分块 LOD 生成，渲染器自己管顶点缓冲
// （见 render/TerrainPass.h），物理层拿同一份高度当高度场
// （见 physics/Heightfield.h）。
//
// 为什么 heightScale 放在组件上而不是只放资产里：
//   同一个 `hills.vterrain` 可以摆两座"同一形状、不同起伏"的山。
//   ⚠ 渲染与物理**必须都读这一个值**：物理侧用 heightAt 得到的高度与
//     渲染侧顶点位移的高度一旦不同，角色就会悬空或陷进地里 —— 而画面
//     完全正常（只是"贴地不准"），极难定位。
// ============================================================
struct TerrainComponent {
    // 相对 assets/ 的 `.vterrain` 路径，例如 "terrain/hills/terrain.vterrain"
    std::string source;
    bool visible = true;
    // 高度倍率：乘在 `.vterrain` 的 maxHeight 上。1.0 = 原样。
    float heightScale = 1.0f;
    // 4 层材质 id 覆盖（空串 = 用 `.vterrain` 里写的）。
    // 改它需要重建地形实例（贴图与层参数 UBO 都在实例里），
    // 所以把 dirty 置位即可 —— 渲染层下一帧会重建。
    std::string layerOverrides[4];
    bool dirty = false;

    // ---- 地形笔刷专用的轻量脏标记 ----
    //
    // 为什么不能复用上面的 dirty：dirty 走的是"销毁实例 → 重读资产 → 重建
    // 顶点/纹理/sampler/描述符"，一次 ~10 ms 且必须等 GPU 空闲。凭空刷地形
    // 每 80 ms 触发一次，几秒钟就会把设备拖死（实测 VK_ERROR_DEVICE_LOST）。
    //
    // 而 terrain.vert 里高度是**在顶点着色器里采样高度图**加出来的
    // （pos.z += texture(heightMap, uv).r * maxHeight），顶点本身是平坦格子 ——
    // 所以改高度只需要把那张 R32 图原地重传一遍：VBO、sampler、描述符、
    // 分块全部不动。264 KB 的数据量，不重建任何对象。
    bool heightsDirty = false;   // 高度图变了（刷高/降低/平滑/压平）
    bool splatDirty = false;     // 权重图变了（层绘制）
    // 挖洞遮罩变了（Hole 笔刷）。单独一位而不是"跟着 heightsDirty 一起刷"：
    // 遮罩只有 66 KB，但它走的是**另一张图**，混进高度那条路径会让
    // "只改了高度"的笔画多传一次遮罩（12 Hz 下是纯浪费）。
    bool holesDirty = false;

    // ---- 内存直传通道（2026-10-21）----
    //
    // 上面三个 dirty 位只说明"要重传"，但没说明"用哪份数据"。原来的实现是
    // 让渲染层**回磁盘再读一遍**（TerrainPass::refreshSurface → loadTerrain）：
    // 编辑器先写盘、渲染层再读盘，一次刷新 2 MB 磁盘 I/O，跟着笔刷每 80 ms
    // 来一遍 —— 这就是"刷地形延迟很大"的主因。
    //
    // 现在编辑器把当前这份资产（内存里的 vector）挂在这三个指针上，渲染层
    // 直接拿它上传，不碰磁盘。指针的**生命周期由编辑器保证**（LandscapeEditor
    // 的 m_asset 在它自己的存活期内一直有效）；场景卸载 / 换地形时编辑器会把
    // 它们清成 nullptr，渲染层见 nullptr 就退回读盘 —— 所以"忘了清"只会变慢，
    // 不会读到野指针以外的行为（而不是崩在别处）。
    const std::vector<float>* liveHeights = nullptr;
    const std::vector<std::uint8_t>* liveSplat = nullptr;
    const std::vector<std::uint8_t>* liveHoles = nullptr;
};

// ============================================================
// WaterComponent —— 一块水体（2026-10-21 第五轮）
//
// 一个实体 = 一块水体。和地形不同，水体**没有网格组件也没有资产**：
//   · 几何是渲染层现生成的（圆形径向网格 / 矩形方格），尺寸变了就重生成；
//   · 参数全在 `water::WaterBody` 里（浅/深水色、折射率、流速、波高…
//     见 water/WaterBody.h 的注释），存盘时写进场景 JSON。
//
// 为什么水面高度（level）在 body 里而不是用实体的 position.z：
//   水位是**水面**的位置，而实体位置是这块水体的"锚点"。两者在
//   "拖出一个水塘"时并不一致 —— 锚点跟着鼠标落在 xz 上，水位来自
//   当时命中的地表高度。把它分开存，后续调水位不会把整块水体挪走。
// ============================================================
struct WaterComponent {
    water::WaterBody body = water::makeDefault();
    bool visible = true;
};

} // namespace ecs
