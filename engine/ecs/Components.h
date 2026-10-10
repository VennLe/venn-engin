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
};

struct SpotLightComponent {
    glm::vec3 color{1.0f, 0.96f, 0.90f};
    float intensity = 30.0f;
    float range = 9.0f;
    glm::vec3 direction{0.0f, -1.0f, 0.0f};  // 传播方向（从灯指向被照物）
    float innerAngle = 0.28f;  // 内锥半角（弧度）：此角度内全亮
    float outerAngle = 0.52f;  // 外锥半角（弧度）：此角度外为 0
    bool enabled = true;
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
