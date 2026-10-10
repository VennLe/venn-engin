#pragma once
// ============================================================
// editor/EditorScene —— 场景内容的"工厂"
//
// 这里放的都是"往场景里放东西"的操作，被几处共用：
//   · Toolbar / SceneHierarchy 的"新建物体"菜单
//   · ContentBrowser 双击 / 拖进视口来实例化模型
//   · EditorApp 启动时搭出的那张"白纸"（resetToEmptyScene）
//
// 编辑器**不**依赖任何游戏示例 —— 引擎与应用层是分开的两块。
//
// ⚠ 这里**故意没有任何"内置示例场景"**。
//   2026-10-09 之前 venn 启动会搭出一整套演示内容（室内房间 + 展台 +
//   PBR 采样球 + 玻璃 + 24 盏局部光源 + 挂脚本的动画物体），并按用户要求
//   连同独立的 Sandbox 示例程序一起从项目里删掉了。启动即空场景，
//   想要什么自己在编辑器里搭。
// ============================================================

#include "ecs/Entity.h"

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace assets {
class AssetManager;
}
namespace scene {
class Scene;
}

namespace editor {

class PickingSystem;  // 贴地要用它算网格包围盒（只前向声明，不引入头文件）

// 可以一键放进场景的东西
enum class PrimitiveKind : int {
    Cube = 0,
    Sphere = 1,
    Plane = 2,
    PointLight = 3,
    SpotLight = 4,
    Empty = 5,  // 纯空节点，用来做父级分组（Hierarchy 里的"文件夹"）
    Cylinder = 6,
};

const char* primitiveKindName(PrimitiveKind k);

// 创建基本物体（网格 + 材质 / 灯光组件），返回新实体
ecs::Entity createPrimitive(scene::Scene& scene, assets::AssetManager& assets,
                            PrimitiveKind kind,
                            const glm::vec3& position = glm::vec3(0.0f));

// 创建基本物体并"落在地面上"：网格类把**底部**贴到 groundSpot.z 所在的
// 水平面（中心再抬半高），灯光抬到半空。Shift+A 添加菜单用 —— 从菜单里
// 加出来的东西应该稳稳站在鼠标指的那块地上，而不是半截埋进地里。
ecs::Entity createPrimitiveOnGround(scene::Scene& scene,
                                    assets::AssetManager& assets,
                                    PrimitiveKind kind,
                                    const glm::vec3& groundSpot);

// ---- 特殊"灯光"条目（不落在 PrimitiveKind 里，语义各不相同）----

// Sunlight：把**场景级太阳**（scene.light()，有阴影、影响全局的那盏）
// 配置成一套日照预设，返回太阳实体（选中它 → Inspector 显示太阳参数）。
// 场景里没有太阳时会被懒创建；已有太阳时直接覆写成预设值。
ecs::Entity createSunlight(scene::Scene& scene);

// Directional Light：**实体级**方向光（补光）。无阴影、无衰减，
// 可以往场景里摆任意多盏；与 Sunlight 的区别见 createSunlight。
ecs::Entity createDirectionalLight(scene::Scene& scene,
                                   const glm::vec3& position = glm::vec3(0.0f));

// ------------------------------------------------------------ 模型实例化
//
// 导入一个模型文件（.gltf / .glb / .obj）会建出**一棵小树**而不是一堆散件：
//
//     root（文件名，无网格，只当"整体把手"）
//       ├── submesh 0
//       ├── submesh 1
//       └── ...
//
// 为什么要有 root：几何体自己的局部变换来自 glTF 节点（可能带偏移/旋转），
// 想整体挪动或"底部贴地"就得动每一个子节点 —— 有一个空根节点就只需要动它
// 一个，而且 Hierarchy 里也干净（一行，不是十几行散件）。
struct ImportResult {
    ecs::Entity root{};                 // 根节点（失败时为无效句柄）
    std::vector<ecs::Entity> entities;  // 实际带网格的子节点
    bool ok() const { return root.valid(); }
    int count() const { return static_cast<int>(entities.size()); }
};

ImportResult instantiateModel(scene::Scene& scene, assets::AssetManager& assets,
                              const std::string& modelRelativePath,
                              const glm::vec3& position);

// 把导入的整棵物体"底部刚好贴住地面"（默认 y = 0 的栅格面），
// 水平方向把包围盒中心对到 target.xz 上。
// 需要 PickingSystem 来算联合包围盒（网格顶点在 CPU 侧有保留）。
// 拿不到包围盒（网格无 CPU 数据）时只做平移，不贴地 —— 宁可放不准，
// 也不要因为算错而把物体塞到地底下。
void alignImportToGround(scene::Scene& scene, PickingSystem& picking,
                         const ImportResult& imported, const glm::vec3& target);

// 把场景清成"一张白纸"：一块**不透明地面**（一个普通平面实体，顶面正好落在
// z = 0）+ 一盏方向光（否则全黑）+ 默认相机机位。编辑器启动 / File → New
// Scene 都走它。
//
// 为什么要有地面：新场景一打开就有"实在的落脚点"——物体放上去看得见接缝、
// 阴影有地方落，和 UE5 新建关卡自带 Floor 是同一个手感。
// 地面与栅格是**叠加**关系而不是二选一：栅格 pass 排在不透明之后，且顶点
// 阶段把深度往近平面推了 1e-6（NDC），所以栅格线稳稳画在地面上面。
// 地面是普通实体（可选中 / 可移动 / 可删 / 会写进场景 JSON）。
void resetToEmptyScene(scene::Scene& scene, assets::AssetManager& assets);

} // namespace editor
