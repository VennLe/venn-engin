// ============================================================
// scene/PrefabSelfTest.cpp —— G18 预制体的无窗口自检
//
// 六组：
//   A 解析（合法 / 重复 localId / 父下标越界 / 环 / 往返逐字节）
//   B 实例化（3 份：实体互不相同、localId 相同、父子对、实例编号唯一）
//   C 传播（改母版 scale → 3 份全变）
//   D 覆盖（实例 2 覆盖 → 只有 1、3 跟着变；Apply 写回资产 + 清覆盖；
//            Apply 之后 1、3 也变成新值）
//   E 回退（Revert = 清覆盖 + 同步 → 回到母版值）
//   F 场景往返（存场景 → 读回来 → 每个成员的 prefab 标记与覆盖都还在）
//   G 从子树导出（根 + 子节点 → 2 个节点、父下标 0/-1）
//
// ⚠ 全部**不开窗口、不建 Vulkan 设备**：`AssetManager{TextureContext{}}`
//   的空设备在"没有网格 / 贴图"的场景里够用（`resolveMesh` 见到
//   `mesh:null` 直接返回 nullptr）。这和 `ScriptSelfTest` 用的是同一招。
//
// ⚠ 每条断言都问过"**别的实现会给出不同的数吗**"：
//   * 传播那条把 scale 改成 3 而不是 1 —— 1 和"没同步"看起来一样；
//   * 覆盖那条把实例值设成 5、母版改成 3，三个数**互不相等**，
//     写成"全都等于 3"或"全都等于 5"都会被抓；
//   * 往返那条比对**字节**，不是"能解析"。
// ============================================================

#include "scene/Prefab.h"

#include "assets/AssetManager.h"
#include "assets/AssetPath.h"
#include "assets/Texture.h"
#include "core/Logger.h"
#include "ecs/Components.h"
#include "scene/Scene.h"
#include "scene/SceneSerializer.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

namespace scene {

namespace {

int g_pass = 0;
int g_fail = 0;

// ⚠ detail 里**不能有空格**（验收脚本用 `detail=(\S*)` 解析）
void report(const char* name, bool ok, const std::string& detail = {}) {
    VK_LOG_INFO("PREFAB-TEST %s actual=%d expected=1 tol=0 detail=%s result=%s",
                name, ok ? 1 : 0, detail.empty() ? "-" : detail.c_str(),
                ok ? "PASS" : "FAIL");
    if (ok) ++g_pass;
    else ++g_fail;
    if (!ok) std::printf("  FAIL %s (%s)\n", name, detail.c_str());
}

// 三节点：根（带脚本）+ 子（可见性关掉）+ 子（纯分组）
const char* kPrefabText = R"JSON({
  "kind": "venn.prefab",
  "version": 1,
  "name": "crate",
  "//": "自检用夹具：根 + 两个子节点",
  "entities": [
    { "localId": "crate", "parent": -1, "name": "Crate",
      "transform": { "position": [0,0,0], "rotation": [0,0,0], "scale": [1,1,1] },
      "script": { "path": "scripts/a.vks", "enabled": true, "timeScale": 1.0 } },
    { "localId": "lid", "parent": 0, "name": "Lid",
      "transform": { "position": [0,0,1], "rotation": [0,0,0], "scale": [1,1,1] },
      "visible": false },
    { "localId": "empty", "parent": 0, "name": "Empty",
      "transform": { "position": [0,0,2], "rotation": [0,0,0], "scale": [1,1,1] },
      "folder": true }
  ]
})JSON";

float lidScaleX(scene::Scene& sc, ecs::Entity e) {
    const auto* t = sc.world().get<ecs::TransformComponent>(e);
    return t ? t->scale.x : -1.0f;
}

void setScaleX(scene::Scene& sc, ecs::Entity e, float v) {
    if (auto* t = sc.world().get<ecs::TransformComponent>(e))
        t->scale = glm::vec3(v, v, v);
}

ecs::Entity memberByLocalId(scene::Scene& sc, const PrefabInstance& inst,
                            const std::string& localId) {
    for (ecs::Entity e : inst.members) {
        if (const auto* pl = sc.world().get<ecs::PrefabLinkComponent>(e)) {
            if (pl->localId == localId) return e;
        }
    }
    return ecs::Entity{};
}

// 实例根上的第一条覆盖路径（没有 → 空串）。断言里用它，可以区分"记了 1 条"
// 与"记了 1 条**位置**" —— 只数个数的话，记错成 scale 也照样绿。
std::string firstOverride(Scene& sc, std::uint32_t instance) {
    std::string out;
    sc.world().each<ecs::PrefabLinkComponent>(
        [&](ecs::Entity, const ecs::PrefabLinkComponent& pl) {
            if (pl.instance == instance && pl.root && !pl.overrides.empty() &&
                out.empty())
                out = pl.overrides.front();
        });
    return out;
}

void testParse() {
    PrefabAsset a;
    const bool ok = parsePrefab(kPrefabText, a);
    report("prefab_parse_ok", ok && a.nodes.size() == 3,
           "nodes=" + std::to_string(a.nodes.size()));
    report("prefab_parse_parents",
           a.nodes.size() == 3 && a.nodes[0].parentIndex == -1 &&
               a.nodes[1].parentIndex == 0 && a.nodes[2].parentIndex == 0,
           "p0=" + std::to_string(a.nodes.size() == 3 ? a.nodes[0].parentIndex : -9));
    report("prefab_parse_comment",
           a.comment.find("自检用夹具") != std::string::npos);
    report("prefab_parse_find", a.find("lid") != nullptr && a.indexOf("nope") < 0);

    // 往返：toJson → parse → toJson **逐字节**相同（少一个字段就露）
    const std::string t1 = prefabToJson(a);
    PrefabAsset b;
    const bool ok2 = parsePrefab(t1, b);
    const std::string t2 = prefabToJson(b);
    report("prefab_roundtrip_bytes", ok2 && t1 == t2,
           "b1=" + std::to_string(t1.size()) + " b2=" + std::to_string(t2.size()));

    // 负向：重复 localId
    {
        PrefabAsset bad;
        const bool r = parsePrefab(
            R"({"version":1,"entities":[{"localId":"a","parent":-1},"
               R"({"localId":"a","parent":0}]})", bad);
        report("prefab_reject_dup_localid", !r);
    }
    // 负向：父下标越界
    {
        PrefabAsset bad;
        const bool r = parsePrefab(
            R"({"version":1,"entities":[{"localId":"a","parent":-1},"
               R"({"localId":"b","parent":7}]})", bad);
        report("prefab_reject_bad_parent", !r);
    }
    // 负向：环（b→a，a→b）
    {
        PrefabAsset bad;
        const bool r = parsePrefab(
            R"({"version":1,"entities":[{"localId":"a","parent":1},"
               R"({"localId":"b","parent":0}]})", bad);
        report("prefab_reject_cycle", !r);
    }
    // 负向：localId 带 `/`（会毁掉覆盖路径的解析）
    {
        PrefabAsset bad;
        const bool r = parsePrefab(
            R"({"version":1,"entities":[{"localId":"a/b","parent":-1}]})", bad);
        report("prefab_reject_slash_localid", !r);
    }
    // 负向：空 entities
    {
        PrefabAsset bad;
        report("prefab_reject_empty", !parsePrefab(R"({"version":1,"entities":[]})", bad));
    }
}

void testMain() {
    PrefabAsset asset;
    if (!parsePrefab(kPrefabText, asset)) {
        report("prefab_fixture_parses", false);
        return;
    }
    asset.path = "prefabs/__selftest_crate.vprefab.json";

    scene::Scene sc;
    assets::AssetManager am{assets::TextureContext{}};

    PrefabInstance i1, i2, i3;
    const bool o1 = instantiatePrefab(sc, am, asset, {0, 0, 0}, i1, nullptr);
    const bool o2 = instantiatePrefab(sc, am, asset, {5, 0, 0}, i2, nullptr);
    const bool o3 = instantiatePrefab(sc, am, asset, {10, 0, 0}, i3, nullptr);
    report("prefab_three_instances", o1 && o2 && o3 &&
                                        i1.members.size() == 3 &&
                                        i2.members.size() == 3 &&
                                        i3.members.size() == 3);
    if (!o1 || !o2 || !o3) return;

    // ---- B 实例化 ----
    report("prefab_instance_ids_unique",
           i1.id != i2.id && i2.id != i3.id && i1.id != i3.id,
           std::to_string(i1.id) + "/" + std::to_string(i2.id) + "/" +
               std::to_string(i3.id));

    std::unordered_set<ecs::EntityId> ids;
    for (const PrefabInstance* p : {&i1, &i2, &i3})
        for (ecs::Entity e : p->members) ids.insert(e.id);
    report("prefab_no_entity_share", ids.size() == 9,
           "unique=" + std::to_string(ids.size()));

    // 同一 localId 在三份里都指同一个节点，但成员实体不同
    const ecs::Entity lid1 = memberByLocalId(sc, i1, "lid");
    const ecs::Entity lid2 = memberByLocalId(sc, i2, "lid");
    const ecs::Entity lid3 = memberByLocalId(sc, i3, "lid");
    report("prefab_localid_stable",
           lid1.valid() && lid2.valid() && lid3.valid() &&
               lid1.id != lid2.id && lid2.id != lid3.id);

    report("prefab_root_lookup",
           prefabInstanceRoot(sc, lid2) == i2.members.front() &&
               prefabInstanceRoot(sc, i2.members.front()) == i2.members.front());

    // 父子：lid 的父是 i2 的根
    const auto* h2 = sc.world().get<ecs::HierarchyComponent>(lid2);
    report("prefab_hierarchy", h2 && h2->parent == i2.members.front().id,
           h2 ? std::to_string(h2->parent) : "no-hierarchy");

    // 名字去重（3 份 "Crate" 不能全叫同一个名字）
    const std::string* n1 = sc.world().name(i1.members.front());
    const std::string* n2 = sc.world().name(i2.members.front());
    report("prefab_names_unique",
           n1 && n2 && *n1 == "Crate" && *n2 == "Crate 2",
           (n1 ? *n1 : "?") + std::string("/") + (n2 ? *n2 : "?"));

    // 落点只作用于根，子节点保持相对
    const auto* rt2 = sc.world().get<ecs::TransformComponent>(i2.members.front());
    const auto* lt2 = sc.world().get<ecs::TransformComponent>(lid2);
    report("prefab_place_at_root",
           rt2 && lt2 && std::fabs(rt2->position.x - 5.0f) < 1e-4f &&
               std::fabs(lt2->position.z - 1.0f) < 1e-4f);

    // 落点 ≠ 母版位置 → 必须记一条覆盖。否则改母版时实例会自己跳回母版
    // 坐标（"我摆好的东西跑掉了"）。夹具里母版根位置是 (0,0,0)：
    //   i1 摆在 (0,0,0) = 与母版相同 → 0 条；i2 摆在 (5,0,0) → 恰好 1 条。
    report("prefab_place_records_override",
           countOverrides(sc, i1.id) == 0 && countOverrides(sc, i2.id) == 1 &&
               firstOverride(sc, i2.id) == "crate/transform.position",
           "i1=" + std::to_string(countOverrides(sc, i1.id)) + " i2=" +
               std::to_string(countOverrides(sc, i2.id)) + " p=" +
               (firstOverride(sc, i2.id).empty() ? "-"
                                                 : firstOverride(sc, i2.id)));

    // ---- C 传播：改母版 scale 2 → 三份全变 ----
    asset.nodes[1].entity["transform"]["scale"] = {2, 2, 2};
    const PrefabSyncResult s1 = syncInstances(sc, am, asset);
    report("prefab_source_edit_propagates",
           std::fabs(lidScaleX(sc, lid1) - 2.0f) < 1e-4f &&
               std::fabs(lidScaleX(sc, lid2) - 2.0f) < 1e-4f &&
               std::fabs(lidScaleX(sc, lid3) - 2.0f) < 1e-4f,
           std::to_string(lidScaleX(sc, lid1)) + "/" +
               std::to_string(lidScaleX(sc, lid2)) + "/" +
               std::to_string(lidScaleX(sc, lid3)));
    report("prefab_sync_counts", s1.instances == 3 && s1.members == 9 &&
                                     s1.applied == 9 && s1.failed == 0,
           "inst=" + std::to_string(s1.instances) + " mem=" +
               std::to_string(s1.members) + " app=" + std::to_string(s1.applied));

    // ---- D 覆盖：实例 2 的 lid.scale 被改过 → 不再跟随 ----
    // i2 身上已有 1 条（落点 position），再记一条 scale 应为 2 条。
    const bool rec = recordOverride(sc, lid2, "transform", "scale");
    report("prefab_override_recorded",
           rec && countOverrides(sc, i2.id) == 2 && countOverrides(sc, i1.id) == 0,
           "i2=" + std::to_string(countOverrides(sc, i2.id)) + " i1=" +
               std::to_string(countOverrides(sc, i1.id)));
    setScaleX(sc, lid2, 5.0f);   // 模拟"用户拖了实例 2 的盖子"

    asset.nodes[1].entity["transform"]["scale"] = {3, 3, 3};   // 母版再改一次
    syncInstances(sc, am, asset);
    report("prefab_override_blocks",
           std::fabs(lidScaleX(sc, lid1) - 3.0f) < 1e-4f &&
               std::fabs(lidScaleX(sc, lid3) - 3.0f) < 1e-4f &&
               std::fabs(lidScaleX(sc, lid2) - 5.0f) < 1e-4f,
           std::to_string(lidScaleX(sc, lid1)) + "/" +
               std::to_string(lidScaleX(sc, lid2)) + "/" +
               std::to_string(lidScaleX(sc, lid3)));

    // ---- D2 可见性也是"字段"（同一套覆盖机制）----
    // 夹具里 lid 是 visible=false。把实例 1 的盖子打开并记一条覆盖，再同步：
    // 应该**只有实例 1 变可见**（母版仍是 false）。这条挡的是"母版一同步，
    // 我手动隐藏/显示的东西就被悄悄改回去"。
    {
        if (auto* v1 = sc.world().get<ecs::VisibilityComponent>(lid1))
            v1->visible = true;
        recordOverride(sc, lid1, "visible");
        syncInstances(sc, am, asset);
        const auto* v1b = sc.world().get<ecs::VisibilityComponent>(lid1);
        const auto* v2b = sc.world().get<ecs::VisibilityComponent>(lid2);
        const bool v1on = v1b && v1b->visible;
        const bool v2on = v2b && v2b->visible;
        report("prefab_visible_override", v1on && !v2on,
               std::string("v1=") + (v1on ? "1" : "0") + " v2=" +
                   (v2on ? "1" : "0"));
        clearInstanceOverrides(sc, i1.id);   // 后面 F 组要干净的计数
    }

    // Apply：把实例 2 的覆盖值写回资产，并清掉它的覆盖
    // 两条路径（position + lid.scale）都应被写回，然后一起清空。
    int cleared = 0;
    const int wrote = applyInstanceToAsset(sc, asset, i2.id, &cleared);
    const auto& jscale = asset.nodes[1].entity["transform"]["scale"];
    const auto& jpos   = asset.nodes[0].entity["transform"]["position"];
    report("prefab_apply_writes_asset",
           wrote == 2 && cleared == 2 && countOverrides(sc, i2.id) == 0 &&
               std::fabs(jscale[0].get<float>() - 5.0f) < 1e-4f &&
               std::fabs(jpos[0].get<float>() - 5.0f) < 1e-4f,
           "wrote=" + std::to_string(wrote) + " cleared=" +
               std::to_string(cleared) + " v=" + jscale.dump());

    // Apply 之后同步 → 另外两份也变成 5（这是"Apply 到底生效没有"的判据）
    syncInstances(sc, am, asset);
    report("prefab_apply_propagates_others",
           std::fabs(lidScaleX(sc, lid1) - 5.0f) < 1e-4f &&
               std::fabs(lidScaleX(sc, lid3) - 5.0f) < 1e-4f,
           std::to_string(lidScaleX(sc, lid1)) + "/" +
               std::to_string(lidScaleX(sc, lid3)));

    // ---- E 回退：清覆盖 + 同步 → 回到母版值 ----
    recordOverride(sc, lid3, "transform", "scale");
    setScaleX(sc, lid3, 9.0f);
    clearInstanceOverrides(sc, i3.id);
    syncInstances(sc, am, asset);
    report("prefab_revert_restores",
           std::fabs(lidScaleX(sc, lid3) - 5.0f) < 1e-4f &&
               countOverrides(sc, i3.id) == 0,
           std::to_string(lidScaleX(sc, lid3)));

    // ---- F 场景往返：prefab 标记与覆盖都要活下来 ----
    recordOverride(sc, lid1, "transform", "position");
    report("prefab_override_position", countOverrides(sc, i1.id) == 1);

    std::string text;
    const SceneIoResult wr = sceneToJson(sc, text);
    report("prefab_scene_saved", wr.ok && wr.skipped == 0,
           "skipped=" + std::to_string(wr.skipped));

    scene::Scene sc2;
    assets::AssetManager am2{assets::TextureContext{}};
    const SceneIoResult rd = sceneFromJson(sc2, am2, text);
    int members2 = 0, roots2 = 0, overrides2 = 0, missing = 0;
    sc2.world().each<ecs::PrefabLinkComponent>(
        [&](ecs::Entity e, const ecs::PrefabLinkComponent& pl) {
            ++members2;
            if (pl.root) {
                ++roots2;
                overrides2 += static_cast<int>(pl.overrides.size());
            }
            if (pl.prefabPath != asset.path) ++missing;
            if (!sc2.world().get<ecs::TransformComponent>(e)) ++missing;
        });
    report("prefab_scene_roundtrip_members",
           rd.ok && members2 == 9 && roots2 == 3 && missing == 0,
           "mem=" + std::to_string(members2) + " roots=" +
               std::to_string(roots2) + " bad=" + std::to_string(missing));
    report("prefab_scene_roundtrip_overrides", overrides2 == 1,
           "overrides=" + std::to_string(overrides2));

    // ---- G 从子树导出 ----
    scene::Scene sc3;
    const ecs::Entity rootE = sc3.createObject("GroupRoot");
    const ecs::Entity childE = sc3.createObject("GroupChild");
    sc3.setParent(childE, rootE);
    if (auto* t = sc3.world().get<ecs::TransformComponent>(childE))
        t->position = {0, 0, 3};
    PrefabAsset ex;
    std::string exErr;
    const bool eok = prefabFromSubtree(sc3, rootE, ex, &exErr);
    report("prefab_extract_subtree",
           eok && ex.nodes.size() == 2 && ex.nodes[0].localId == "GroupRoot" &&
               ex.nodes[0].parentIndex == -1 && ex.nodes[1].parentIndex == 0,
           "err=" + exErr + " n=" + std::to_string(ex.nodes.size()));
    if (eok && ex.nodes.size() == 2) {
        report("prefab_extract_keeps_transform",
               std::fabs(ex.nodes[1].entity["transform"]["position"][2]
                             .get<float>() - 3.0f) < 1e-4f);
        // 导出的东西本身必须能被解析回来（否则"导出"就是一次性的）
        PrefabAsset again;
        report("prefab_extract_roundtrip",
               parsePrefab(prefabToJson(ex), again) && again.nodes.size() == 2);
    }

    // ---- H 文件往返（真写盘 + 读回，自检不留垃圾） ----
    {
        asset.path = "prefabs/__selftest_tmp.vprefab.json";
        const std::string full = assets::resolveAssetPath(asset.path);
        std::error_code ec;
        std::filesystem::create_directories(
            std::filesystem::path(full).parent_path(), ec);
        std::string serr;
        const bool wroteFile = savePrefab(asset, serr);
        PrefabAsset back;
        const bool readBack = loadPrefab(asset.path, back);
        report("prefab_file_roundtrip",
               wroteFile && readBack && back.nodes.size() == 3 &&
                   back.name == asset.name,
               serr);
        if (readBack && back.nodes.size() == 3) {
            const auto& s = back.nodes[1].entity["transform"]["scale"];
            report("prefab_file_keeps_values",
                   std::fabs(s[0].get<float>() - 5.0f) < 1e-4f, s.dump());
        }
        std::filesystem::remove(full, ec);
    }
}

} // namespace

int runPrefabSelfTest() {
    g_pass = 0;
    g_fail = 0;
    std::printf("== selftest: prefab ==\n");
    testParse();
    testMain();
    VK_LOG_INFO("PREFAB-TOTAL pass=%d fail=%d", g_pass, g_fail);
    std::printf("PREFAB-TOTAL pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

} // namespace scene
