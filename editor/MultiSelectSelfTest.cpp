// ============================================================
// editor/MultiSelectSelfTest.cpp —— 多选（#316）的无窗口自检
//
// 六组：
//   A 单选入口的零破坏（select() 之后集合恰好一项，主选中项就是它）
//   B 集合语义（Ctrl 增删 / Shift 范围 / 框选替换 / 全选跳过上锁）
//   D 支点（多选 = 世界位置 AABB 中心，不是"第一个"也不是"平均数"）
//   E 撤销（一条 MultiTransformEditCommand 回滚**全部**目标）
//   F 批量数学（MultiEditMath：增量 / 比例 / 绕支点旋转的数值）
//   C 不变式修补（成员被删 → validateSelection 剔除；主选中项跟着换人）
//
// ⚠ 每条断言都问过"**别的实现会给出不同的数吗**"：
//   · 支点那条把两个物体放在 x=0 与 x=10 —— 中心是 5。写成"取第一个"
//     得 0、"忘了除以 2"得 10、"只取最后一个"得 10，三种错法都会被抓；
//   · 范围那条 order 有 4 项、从第 1 项选到第 3 项，正确答案是 **3**；
//     差一位的实现得 2 或 4；
//   · 撤销那条给三个目标**各不相同**的 after，逐个比对位置分量。
// ============================================================

#include "MultiSelectSelfTest.h"

#include "Command.h"
#include "EditorContext.h"
#include "MultiEditMath.h"

#include "assets/AssetManager.h"
#include "assets/Texture.h"
#include "core/Logger.h"
#include "ecs/Components.h"
#include "scene/Scene.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace editor {

namespace {

int g_pass = 0;
int g_fail = 0;

// ⚠ detail 里**不能有空格**（验收脚本用 `detail=(\S*)` 解析）
void report(const char* name, bool ok, const std::string& detail = {}) {
    VK_LOG_INFO("MULTI-TEST %s actual=%d expected=1 tol=0 detail=%s result=%s",
                name, ok ? 1 : 0, detail.empty() ? "-" : detail.c_str(),
                ok ? "PASS" : "FAIL");
    if (ok) ++g_pass;
    else ++g_fail;
    if (!ok) std::printf("  FAIL %s (%s)\n", name, detail.c_str());
}

bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

} // namespace

int runMultiSelectSelfTest() {
    VK_LOG_INFO("=== Multi-select self-test (#316) ===");

    assets::AssetManager assets{assets::TextureContext{}};
    EditorContext ctx(assets);
    scene::Scene& sc = ctx.editorScene();
    ecs::World& w = sc.world();

    // 四个物体，位置刻意不同（支点要能分辨 5 / 0 / 10）
    const ecs::Entity a = sc.createObject("A");
    const ecs::Entity b = sc.createObject("B");
    const ecs::Entity c = sc.createObject("C");
    const ecs::Entity d = sc.createObject("D");
    w.get<ecs::TransformComponent>(a)->position = glm::vec3(0.0f);
    w.get<ecs::TransformComponent>(b)->position = glm::vec3(10.0f, 0.0f, 0.0f);
    w.get<ecs::TransformComponent>(c)->position = glm::vec3(10.0f, 0.0f, 10.0f);
    w.get<ecs::TransformComponent>(d)->position = glm::vec3(0.0f, 0.0f, 10.0f);

    // ---------------------------------------------------------- A 零破坏
    ctx.select(a);
    report("single_set_size1", ctx.selectionCount() == 1 &&
                                   ctx.selection().id == a.id &&
                                   ctx.isSelected(a) && !ctx.isSelected(b),
           "n=" + std::to_string(ctx.selectionCount()));
    report("single_not_multi", !ctx.multiSelected());

    // ---------------------------------------------------------- B 集合语义
    ctx.selectToggle(b);
    report("toggle_add", ctx.selectionCount() == 2 && ctx.selection().id == b.id,
           "n=" + std::to_string(ctx.selectionCount()));

    ctx.selectToggle(b);
    report("toggle_remove", ctx.selectionCount() == 1 &&
                                ctx.selection().id == a.id && !ctx.isSelected(b),
           "n=" + std::to_string(ctx.selectionCount()));

    ctx.selectToggle(a);
    report("toggle_remove_last", ctx.selectionCount() == 0 &&
                                     !ctx.hasSelection(),
           "n=" + std::to_string(ctx.selectionCount()));

    // Shift 范围：order = [A,B,C,D]，从 A 选到 C → 3 项，主选中项 = C
    const std::vector<ecs::Entity> order{a, b, c, d};
    ctx.select(a);
    ctx.selectRange(c, order);
    report("range_a_to_c",
           ctx.selectionCount() == 3 && ctx.selection().id == c.id &&
               ctx.isSelected(a) && ctx.isSelected(b) && ctx.isSelected(c) &&
               !ctx.isSelected(d),
           "n=" + std::to_string(ctx.selectionCount()));

    // 反向：锚点现在是 C，选到 A → 还是同样那三项
    ctx.selectRange(a, order);
    report("range_reverse", ctx.selectionCount() == 3 &&
                                ctx.selection().id == a.id &&
                                !ctx.isSelected(d),
           "n=" + std::to_string(ctx.selectionCount()));

    // 框选：替换整个集合；旧主选中项还在里面就留着
    ctx.select(b);
    ctx.selectMany({a, b, c});
    report("many_keeps_primary",
           ctx.selectionCount() == 3 && ctx.selection().id == b.id,
           "n=" + std::to_string(ctx.selectionCount()));

    ctx.selectMany({c, d});
    report("many_replaces", ctx.selectionCount() == 2 &&
                                ctx.selection().id == c.id && !ctx.isSelected(a),
           "n=" + std::to_string(ctx.selectionCount()));

    // 全选跳过上锁的（自带地面）
    w.add<ecs::LockedComponent>(d);
    ctx.selectAll();
    report("all_skips_locked",
           ctx.selectionCount() == 3 && !ctx.isSelected(d) && ctx.isSelected(a),
           "n=" + std::to_string(ctx.selectionCount()));
    w.remove<ecs::LockedComponent>(d);

    // ---------------------------------------------------------- D 支点
    ctx.select(a);
    report("pivot_single", near(ctx.selectionPivot().x, 0.0f),
           "x=" + std::to_string(ctx.selectionPivot().x));

    ctx.selectMany({a, b});   // (0,0,0) 与 (10,0,0) → 中心 (5,0,0)
    const glm::vec3 p = ctx.selectionPivot();
    report("pivot_box_center", near(p.x, 5.0f) && near(p.y, 0.0f),
           "x=" + std::to_string(p.x));

    // ---------------------------------------------------------- E 撤销
    {
        // 三个目标，各自的 after 不同 —— 逐个比才能抓住"只写了第一个"
        std::vector<MultiTransformEditCommand::Item> items;
        const ecs::Entity targets[3] = {a, b, d};
        for (int i = 0; i < 3; ++i) {
            MultiTransformEditCommand::Item it;
            it.entity = targets[i];
            it.before = TransformSnapshot::capture(sc, targets[i]);
            it.after = it.before;
            it.after.position += glm::vec3(static_cast<float>(i + 1), 0.0f, 0.0f);
            items.push_back(it);
        }
        // 模拟"已经拖完"：先把 after 写进场景，再压栈
        for (const auto& it : items) it.after.apply(sc, it.entity);
        ctx.commands().pushAlreadyApplied(
            std::make_unique<MultiTransformEditCommand>("Move 3 objects", &sc,
                                                        items));
        const bool applied =
            near(w.get<ecs::TransformComponent>(a)->position.x, 1.0f) &&
            near(w.get<ecs::TransformComponent>(b)->position.x, 12.0f) &&
            near(w.get<ecs::TransformComponent>(d)->position.x, 3.0f);
        report("multi_cmd_applied", applied,
               "ax=" + std::to_string(w.get<ecs::TransformComponent>(a)->position.x));

        ctx.undo();
        const bool undone =
            near(w.get<ecs::TransformComponent>(a)->position.x, 0.0f) &&
            near(w.get<ecs::TransformComponent>(b)->position.x, 10.0f) &&
            near(w.get<ecs::TransformComponent>(d)->position.x, 0.0f);
        report("multi_cmd_undo_all", undone,
               "bx=" + std::to_string(w.get<ecs::TransformComponent>(b)->position.x));

        ctx.redo();
        const bool redone =
            near(w.get<ecs::TransformComponent>(a)->position.x, 1.0f) &&
            near(w.get<ecs::TransformComponent>(b)->position.x, 12.0f) &&
            near(w.get<ecs::TransformComponent>(d)->position.x, 3.0f);
        report("multi_cmd_redo_all", redone);
    }

    // ---------------------------------------------------------- F 批量数学
    //
    // Inspector 的"混合值"编辑与 gizmo 的多选旋转都落在这三个纯函数上
    // （见 MultiEditMath.h）。它们错了的话，界面上看起来一切正常：数字
    // 会变、物体也会动，只是变的方向 / 幅度不对 —— 只能靠数值断言抓。
    {
        // 位置·一致：prior = 共同值 3 → 拖到 5 = **设成** 5
        const glm::vec3 p1 = batchAdd(glm::vec3(3.0f), glm::vec3(5.0f),
                                      glm::vec3(3.0f));
        report("batch_add_uniform", near(p1.x, 5.0f) && near(p1.y, 5.0f),
               "x=" + std::to_string(p1.x));

        // 位置·混合：prior = 0 → 拖到 1 = 每个**加** 1
        const glm::vec3 p2 = batchAdd(glm::vec3(0.0f, 10.0f, 10.0f),
                                      glm::vec3(1.0f), glm::vec3(0.0f));
        report("batch_add_mixed", near(p2.x, 1.0f) && near(p2.y, 11.0f) &&
                                      near(p2.z, 11.0f),
               "y=" + std::to_string(p2.y));

        // 缩放·一致：prior = 2 → 拖到 3 = **设成** 3（不是 ×1.5）
        const glm::vec3 s1 = batchMul(glm::vec3(2.0f), glm::vec3(3.0f),
                                      glm::vec3(2.0f));
        report("batch_mul_uniform", near(s1.x, 3.0f),
               "x=" + std::to_string(s1.x));

        // 缩放·混合：prior = 1 → 拖到 2 = 各自 **×2**（2/4/1 → 4/8/2）
        const glm::vec3 s2 = batchMul(glm::vec3(2.0f, 4.0f, 1.0f),
                                      glm::vec3(2.0f), glm::vec3(1.0f));
        report("batch_mul_mixed", near(s2.x, 4.0f) && near(s2.y, 8.0f) &&
                                      near(s2.z, 2.0f),
               "y=" + std::to_string(s2.y));

        // 缩放·prior 有 0：那一轴必须原样不动（除以 0 → inf → 整棵树 NaN）
        const glm::vec3 s3 = batchMul(glm::vec3(1.0f, 1.0f, 0.0f),
                                      glm::vec3(3.0f, 3.0f, 3.0f),
                                      glm::vec3(0.0f, 1.0f, 1.0f));
        report("batch_mul_zero_prior",
               std::isfinite(s3.x) && near(s3.x, 1.0f) && near(s3.y, 3.0f),
               "x=" + std::to_string(s3.x));

        // 绕支点旋转 180°（绕 Z）：支点 (5,0,0)、物体在 (0,0,0) → (10,0,0)。
        // 忘了公转得 (0,0,0)；忘了减支点得 (-5,0,0)（"绕原点转"）。
        const glm::mat3 D = glm::mat3(
            glm::rotate(glm::mat4(1.0f), glm::radians(180.0f),
                        glm::vec3(0.0f, 0.0f, 1.0f)));
        const glm::vec3 o = orbitPosition(glm::vec3(0.0f), glm::vec3(5.0f, 0, 0), D);
        report("orbit_180", near(o.x, 10.0f) && near(o.y, 0.0f),
               "x=" + std::to_string(o.x));
    }

    // ---------------------------------------------------------- C 不变式修补
    //
    // 放在最后：这一组会**真的销毁实体**，前面几组还要用它们。
    ctx.selectMany({a, b});
    w.destroy(b);
    ctx.validateSelection();
    report("validate_drops_dead",
           ctx.selectionCount() == 1 && ctx.isSelected(a) &&
               ctx.selection().id == a.id,
           "n=" + std::to_string(ctx.selectionCount()));

    // 主选中项被删、集合里还有人 → 主选中项换成集合里的（不能悬空）。
    // 用 select + selectToggle 拼出"集合 {A,C}、主选中项 = C"——
    // 直接 selectMany({a,c}) 不行：旧主选中项 A 还在名单里，它会被保留。
    ctx.select(a);
    ctx.selectToggle(c);
    report("primary_is_c", ctx.selectionCount() == 2 && ctx.selection().id == c.id,
           "n=" + std::to_string(ctx.selectionCount()));
    w.destroy(c);
    ctx.validateSelection();
    report("validate_rebinds_primary",
           ctx.selectionCount() == 1 && ctx.hasSelection() &&
               ctx.selection().id == a.id,
           "n=" + std::to_string(ctx.selectionCount()));

    // 收尾：清空选择集之后不变式仍然成立（主选中项跟着失效）
    ctx.clearSelection();
    report("clear_all", ctx.selectionCount() == 0 && !ctx.hasSelection());

    VK_LOG_INFO("MULTI-TOTAL pass=%d fail=%d", g_pass, g_fail);
    std::printf("MULTI-TOTAL pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail;
}

} // namespace editor
