#pragma once
// ============================================================
// editor/NavPanel —— 导航操作台（Authoring ▸ Navigation，C8 导航）
//
// 为什么必须有一个面板（而不是只有 `nav.*` 命令）：
//   "界面点不到 = 没交付" 是这个项目写在纪律里的判据。导航这件事如果没有
//   一个能**烘一张网格、看一眼形状、试一条路径、给选中的实体挂个目标**
//   的地方，那它就只是"引擎里有个类"，作者做关卡时根本用不上。
//
// 分工：
//   · 这个面板：烘 / 清 / 打印形状 / 试路径 / 给选中实体挂代理 + 设目标；
//   · Inspector：**不**加导航段。理由同 CardsPanel —— 代理只有九个字段，
//     塞进 Inspector 就是在 14 个页签上面再加半屏，而"烘一张网格看看"
//     这件事本来就不属于"某个实体"。
//
// ⚠ 面板**不持有场景、也不持有物理世界**。烘网格要射线，而"射线从哪来"
//   是宿主的事（编辑器用的是当前编辑场景临时建的物理世界，运行时用的是
//   已经在跑的那个）—— 所以烘的动作通过 `BakeFn` 注入。
//   这条边界不是洁癖：它让"按钮"和"命令"走**同一个**函数，判据才能验到。
// ============================================================

#include "ecs/Entity.h"
#include "nav/NavGrid.h"
#include "nav/NavSystem.h"

#include <functional>
#include <string>

namespace scene {
class Scene;
}

namespace editor {

class NavPanel {
public:
    // 烘一张网格并装进 sys。返回一行状态（成功/失败都由它带出去）。
    // 注入而不是自己调物理，理由见文件头。
    using BakeFn = std::function<std::string(const nav::NavGridDesc&, nav::NavSystem&)>;

    // 网格资产的存/取（`assets/<rel>`）。同样注入：路径解析在宿主那边，
    // 面板不认识"工程根目录"这种概念。
    //   save = true  → 把 sys 里的网格写到 `rel`
    //   save = false → 从 `rel` 读进 sys（读失败保留原网格）
    using GridIoFn = std::function<std::string(bool save, const std::string& rel,
                                               nav::NavSystem& sys)>;

    void draw(nav::NavSystem& sys, scene::Scene& scene, ecs::Entity selected, const BakeFn& bake,
              const GridIoFn& io);

    // ---- 命令入口（与按钮**同一条路**）----
    std::string bakeInto(nav::NavSystem& sys, const BakeFn& bake);
    std::string saveGrid(nav::NavSystem& sys, const GridIoFn& io);
    std::string loadGrid(nav::NavSystem& sys, const GridIoFn& io);
    std::string stateLine(const nav::NavSystem& sys) const;
    std::string ascii(const nav::NavSystem& sys) const;
    // 试一条路径：`from` → `to`，返回一行结果（`NAV-PATH …`）。
    // 没有网格时返回以 `NAV-PATH grid=0` 开头的一行 —— 不返回空串，
    // 否则调用方分不清"没网格"和"函数没被调到"。
    std::string pathTest(const nav::NavSystem& sys, const glm::vec3& from, const glm::vec3& to) const;
    // 给实体挂代理（已有就只改目标）。
    std::string attachAgent(scene::Scene& scene, ecs::Entity e, const glm::vec3& goal);
    std::string stopAgent(scene::Scene& scene, nav::NavSystem& sys, ecs::Entity e);
    std::string agentLine(scene::Scene& scene, ecs::Entity e) const;

    const nav::NavGridDesc& desc() const { return m_desc; }
    const std::string& status() const { return m_status; }

private:
    nav::NavGridDesc m_desc;
    // 试路径的两个端点（面板上可编辑；命令 `nav.path` 不走这两个字段）
    glm::vec3 m_from{-8.0f, 0.0f, 0.0f};
    glm::vec3 m_to{8.0f, 0.0f, 0.0f};
    // 网格资产路径（相对 `assets/`）。默认放在 `nav/` 下，与 `data/` 同一条
    // 约定：内容资产按类别分目录。
    char m_gridPath[96] = "nav/main.vnav.json";
    std::string m_status;
};

} // namespace editor
