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
struct MeshComponent {
    assets::Mesh* mesh = nullptr;
};

struct MaterialComponent {
    assets::Material* material = nullptr;
};

struct VisibilityComponent {
    bool visible = true;      // 参与主渲染
    bool castShadow = true;   // 参与阴影贴图渲染
};

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
