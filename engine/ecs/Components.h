#pragma once
// ============================================================
// ecs/Components —— 引擎内置组件（纯数据）
//
// 对 assets 类型只做前向声明：组件里存的是指针，指针不需要完整
// 类型定义。这样依赖方向保持 assets → ecs 单向，ecs 仍是叶子层。
// ============================================================

#include "ecs/Entity.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <string>
#include <vector>

namespace assets {
class Mesh;
class Material;
class Model;
class Skeleton;
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

    // 派生数据：本模型在骨骼 SSBO 里的关节矩阵起始下标（渲染层填）
    int jointBase = -1;
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

struct VisibilityComponent {
    bool visible = true;      // 参与主渲染
    bool castShadow = true;   // 参与阴影贴图渲染
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

} // namespace ecs
