// ============================================================
// scene/SceneIoSelfTest.cpp —— G16-5 三段拆分的验收（`Game.exe --selftest sceneio`）
//
// 这个文件要回答的问题只有两个：
//
//   1. ①② 两段是不是**真的**能在没有 AssetManager 的情况下跑完？
//      ——「签名里没有它」是设计判据；这里把它**跑起来**是运行时判据。
//         （`parseSceneDocument` 只吃 const json&，`instantiateDocument`
//          连 assets 名字都不出现 —— 加参数的那一刻这个文件就编不过。）
//
//   2. 三段加起来与原来那个"一把梭"的 `sceneFromJson` **逐字段一致**吗？
//      —— 拆分最容易出的错不是崩，而是"漏了一段"：拆完之后 Prefab 的
//        父子没了、或某个组件只在某条路上生效。这类错误在画面上静默。
//
// ⚠ 所有断言都问过"别的实现会给出不同的数吗"：
//   * `parse_keeps_raw` 抓"解析时只挑自己认识的字段，raw 被截断"——
//     那会让 ③ 段看不到 mesh/skinned，画面全空而日志全对；
//   * `stage_equals_oneshot` 抓"父子只在一段里做了"；
//   * `parse_fail_keeps_scene` 抓"版本校验挪到 clear 之后"——
//     这条是 G16-2 留下的债，拆分段最容易顺手把它搬错位置。
// ============================================================

#include "scene/SceneIoSelfTest.h"

#include "assets/AssetManager.h"
#include "assets/Texture.h"
#include "core/Logger.h"
#include "ecs/Components.h"
#include "ecs/World.h"
#include "scene/Scene.h"
#include "scene/SceneDocument.h"
#include "scene/SceneSerializer.h"

#include <nlohmann/json.hpp>

#include <string>
#include <unordered_set>
#include <vector>

namespace scene {

namespace {

using json = nlohmann::json;

int g_pass = 0;
int g_fail = 0;

// ⚠ detail 里**不能有空格**：验收脚本用 `detail=(\S*)` 解析（全仓自检的硬约定）。
void report(const char* name, double actual, double expected, double tol = 0.0) {
    const bool ok = (actual - expected) <= tol && (expected - actual) <= tol;
    VK_LOG_INFO("SCENEIO-TEST %s actual=%.6f expected=%.6f tol=%.6f result=%s", name, actual,
                expected, tol, ok ? "PASS" : "FAIL");
    if (ok) ++g_pass;
    else ++g_fail;
}

void reportFlag(const char* name, bool ok, const std::string& detail = {}) {
    VK_LOG_INFO("SCENEIO-TEST %s actual=%d expected=1 tol=0 detail=%s result=%s", name, ok ? 1 : 0,
                detail.c_str(), ok ? "PASS" : "FAIL");
    if (ok) ++g_pass;
    else ++g_fail;
}

// ---------------- 夹具 ----------------
// 六个实体，每一个都在钉一条不同的路径：
//   0 Root      folder + guid，是 1 和 3 的父
//   1 Child     parent 下标（文件内引用）
//   2 Grand     parent 下标，且父在**前面**（下标小于自己）
//   3 ByGuid    parentGuid（跨文件引用那条路）
//   4 Nameless  完全没有 guid → 走"补发"分支
//   5 LostParent parentGuid 指向本文件里**不存在**的 guid → 必须独立存在、不 fail
const char* kFixture = R"({
  "version": 2,
  "camera": { "target": [0, 0, 0], "yaw": 0.5, "pitch": 0.25, "distance": 9.0, "fov": 50 },
  "physics": { "enabled": true, "gravity": [0, 0, -19.6] },
  "entities": [
    { "name": "Root", "guid": "00000000000000a1", "folder": true,
      "transform": { "position": [0, 0, 0] } },
    { "name": "Child", "guid": "00000000000000a2", "parent": 0,
      "transform": { "position": [1, 2, 3] } },
    { "name": "Grand", "guid": "00000000000000a3", "parent": 1,
      "transform": { "position": [0, 0, 1] } },
    { "name": "ByGuid", "guid": "00000000000000a4", "parentGuid": "00000000000000a1",
      "transform": { "position": [5, 0, 0] } },
    { "name": "Nameless" },
    { "name": "LostParent", "guid": "00000000000000a6",
      "parentGuid": "00000000000000ff",
      "transform": { "position": [0, 0, 9] } }
  ]
})";

constexpr ecs::EntityGuid kGuidRoot = 0xa1ull;
constexpr ecs::EntityGuid kGuidChild = 0xa2ull;
constexpr ecs::EntityGuid kGuidByGuid = 0xa4ull;

// ============================================================
// A 组：parseSceneDocument（纯 CPU，无 Scene、无 AssetManager）
// ============================================================
void groupParse() {
    json root = json::parse(kFixture, nullptr, false);
    reportFlag("parse_fixture_is_valid_json", !root.is_discarded());

    SceneDocument doc;
    std::string err;
    const bool ok = parseSceneDocument(root, doc, err);

    reportFlag("parse_ok", ok, err);
    report("parse_entity_count", static_cast<double>(doc.entities.size()), 6.0);
    report("parse_format_version", static_cast<double>(doc.formatVersion), 2.0);

    // guid 必须**从文件读**，而不是像"补发"分支那样留 0。
    reportFlag("parse_guid_from_file",
               doc.entities.size() > 1 && doc.entities[0].guid == kGuidRoot &&
                   doc.entities[1].guid == kGuidChild,
               doc.entities.size() > 1 ? ecs::guidToHex(doc.entities[1].guid) : "");

    // 没有 guid 字段 → kInvalidGuid（不是"随便给一个"）。
    reportFlag("parse_guid_absent_is_invalid",
               doc.entities.size() > 4 && doc.entities[4].guid == ecs::kInvalidGuid);

    // 下标型父引用
    report("parse_parent_index",
           doc.entities.size() > 2 ? static_cast<double>(doc.entities[2].parentIndex) : -999.0,
           1.0);
    // guid 型父引用：parentGuid 落定，parentIndex 保持 -1（两条路互斥）
    reportFlag("parse_parent_guid",
               doc.entities.size() > 3 && doc.entities[3].parentGuid == kGuidRoot &&
                   doc.entities[3].parentIndex == -1);

    // 全局块：白名单里有的必须被搬进 globals
    reportFlag("parse_globals_camera", doc.globals.contains("camera"));
    reportFlag("parse_globals_physics", doc.globals.contains("physics"));
    // 反过来：`entities` **不能**混进 globals（混进去 documentToJson 会写重）
    reportFlag("parse_globals_excludes_entities", !doc.globals.contains("entities"));

    // raw 必须原样保留 —— ③ 段就是靠它拿到 mesh / skinned / 动画那些块的。
    // ⚠ 这条抓的是"解析时只挑自己认识的字段"：那种实现里 raw 会变空，
    //   于是所有实体都解析成空物体，而 parse 自己的断言全绿。
    reportFlag("parse_keeps_raw",
               doc.entities.size() > 1 && doc.entities[1].raw.is_object() &&
                   doc.entities[1].raw.contains("transform"));

    // 只有相机/光照、没有 entities 的老文件也算成功
    {
        json emptyDoc;
        emptyDoc["version"] = 2;
        emptyDoc["camera"] = json::object();
        SceneDocument e2;
        std::string e2err;
        const bool ok2 = parseSceneDocument(emptyDoc, e2, e2err);
        reportFlag("parse_no_entities_ok", ok2 && !e2.hasEntities,
                   std::string("ok=") + (ok2 ? "1" : "0"));
    }

    // 版本：比我们新 → 拒绝；比我们旧 → 照读
    {
        json future;
        future["version"] = kSceneFormatVersion + 1;
        future["entities"] = json::array();
        SceneDocument f;
        std::string ferr;
        const bool fok = parseSceneDocument(future, f, ferr);
        reportFlag("parse_reject_newer_version", !fok && !ferr.empty(),
                   std::string("err=") + (ferr.empty() ? "empty" : "set"));

        json legacy;
        legacy["version"] = 1;
        legacy["entities"] = json::array();
        SceneDocument l;
        std::string lerr;
        const bool lok = parseSceneDocument(legacy, l, lerr);
        reportFlag("parse_accept_v1", lok && l.formatVersion == 1);
    }

    // root 不是 object → 必须报错而不是崩在 `root["entities"]`
    {
        json arr = json::array();
        SceneDocument a;
        std::string aerr;
        reportFlag("parse_reject_non_object", !parseSceneDocument(arr, a, aerr) && !aerr.empty());
    }

    // 非 object 的实体槽位：**保留占位**，不能压缩
    {
        json mixed;
        mixed["version"] = 2;
        mixed["entities"] = json::array({42, json{{"name", "Real"}}});
        SceneDocument m;
        std::string merr;
        const bool mok = parseSceneDocument(mixed, m, merr);
        reportFlag("parse_keeps_invalid_slot",
                   mok && m.entities.size() == 2 && !m.entities[0].raw.is_object() &&
                       m.entities[1].name == "Real");
    }
}

// ============================================================
// B 组：instantiateDocument（纯 CPU，有 Scene，**没有** AssetManager）
// ============================================================
void groupInstantiate() {
    json root = json::parse(kFixture, nullptr, false);
    SceneDocument doc;
    std::string err;
    if (!parseSceneDocument(root, doc, err)) {
        reportFlag("instantiate_precondition_parse", false, err);
        return;
    }

    Scene s;
    // 先塞三个"上一次打开的场景"，验证 instantiate 会清干净
    for (int i = 0; i < 3; ++i) s.createObject("old" + std::to_string(i));
    const size_t before = s.world().entityCount();

    std::vector<ecs::Entity> created;
    std::unordered_map<ecs::EntityGuid, ecs::Entity> guidMap;
    DocumentStats st;
    instantiateDocument(s, doc, created, guidMap, st);

    reportFlag("instantiate_precondition_had_old", before == 3, std::to_string(before));
    report("instantiate_entity_count", static_cast<double>(created.size()), 6.0);
    report("instantiate_stats_entities", static_cast<double>(st.entities), 6.0);
    // 文件里只有一个实体没有 guid（Nameless）
    report("instantiate_guid_assigned", static_cast<double>(st.guidAssigned), 1.0);
    report("instantiate_guid_dup", static_cast<double>(st.guidDup), 0.0);

    // 旧世界必须被清掉（否则"打开第二个场景"会叠加）。
    //
    // ⚠ 判据必须按**名字**，不能写死 `entityCount() == 6`：
    //   `instantiateDocument` 里那句 `applyLight(scene.light(), …)` 会顺手建一个
    //   "DirectionalLight" 实体（`Scene::light()` 找不到方向光组件就自己造一个）。
    //   写死 6 的话这条断言永远红在 7，而**代码是对的** —— 期望错了。
    {
        int oldLeft = 0;
        for (int i = 0; i < 3; ++i) {
            if (s.find("old" + std::to_string(i)).valid()) ++oldLeft;
        }
        report("instantiate_clears_old", static_cast<double>(oldLeft), 0.0);
    }

    // guid 落定 + 反查表
    const ecs::Entity child = s.world().find(kGuidChild);
    reportFlag("instantiate_guid_applied", child.valid());
    report("instantiate_guidmap_size", static_cast<double>(guidMap.size()), 6.0);
    reportFlag("instantiate_guidmap_hits_root",
               s.world().find(kGuidRoot) == guidMap[kGuidRoot]);

    // 下标父引用：Grand(2) 的父是 Child(1)
    if (created.size() > 2 && created[2].valid() && created[1].valid()) {
        const auto* h = s.world().get<ecs::HierarchyComponent>(created[2]);
        reportFlag("instantiate_parent_by_index", h && h->parent == created[1].id,
                   h ? std::to_string(h->parent) : "nohier");
    } else {
        reportFlag("instantiate_parent_by_index", false, "missing");
    }

    // guid 父引用：ByGuid(3) 的父是 Root(0)
    if (created.size() > 3 && created[3].valid() && created[0].valid()) {
        const auto* h = s.world().get<ecs::HierarchyComponent>(created[3]);
        reportFlag("instantiate_parent_by_guid", h && h->parent == created[0].id,
                   h ? std::to_string(h->parent) : "nohier");
    } else {
        reportFlag("instantiate_parent_by_guid", false, "missing");
    }

    // 父 guid 不存在 → 实体照建、父为空（**不是**加载失败）
    if (created.size() > 5 && created[5].valid()) {
        const auto* h = s.world().get<ecs::HierarchyComponent>(created[5]);
        reportFlag("instantiate_parent_missing_safe", h && h->parent == ecs::kInvalidEntity);
    } else {
        reportFlag("instantiate_parent_missing_safe", false, "missing");
    }

    // 外壳：transform 必须落上（它是 entityApplyShell 的活儿，属 ② 段）
    if (created.size() > 1 && created[1].valid()) {
        const auto* t = s.world().get<ecs::TransformComponent>(created[1]);
        reportFlag("instantiate_transform",
                   t && t->position.x == 1.0f && t->position.y == 2.0f &&
                       t->position.z == 3.0f,
                   t ? "x=set" : "notransform");
    } else {
        reportFlag("instantiate_transform", false, "missing");
    }

    // 文件夹标记（写在 raw 里，② 段负责读回来）
    if (created.size() > 0 && created[0].valid()) {
        const auto* h = s.world().get<ecs::HierarchyComponent>(created[0]);
        reportFlag("instantiate_folder", h && h->group);
    } else {
        reportFlag("instantiate_folder", false, "missing");
    }

    // 全局块：相机必须被应用（② 段而不是 ③ 段）
    reportFlag("instantiate_camera_applied", s.camera().yaw() == 0.5f,
               std::to_string(s.camera().yaw()));
}

// ============================================================
// C 组：等价性 + 位置不变量（拆分段最容易出错的两种）
// ============================================================
void groupEquivalence() {
    assets::AssetManager am{assets::TextureContext{}};  // device == nullptr 就够

    const char* plain = R"({
      "version": 2,
      "entities": [
        { "name": "A", "guid": "00000000000000b1", "folder": true,
          "transform": { "position": [1, 0, 0] } },
        { "name": "B", "guid": "00000000000000b2", "parent": 0,
          "transform": { "position": [2, 0, 0] } },
        { "name": "C", "guid": "00000000000000b3", "parentGuid": "00000000000000b1",
          "transform": { "position": [3, 0, 0] } }
      ]
    })";

    // ---- 一把梭 ----
    Scene oneShot;
    const SceneIoResult r1 = sceneFromJson(oneShot, am, plain);

    // ---- 三段 ----
    json root = json::parse(plain, nullptr, false);
    SceneDocument doc;
    std::string err;
    const bool pok = parseSceneDocument(root, doc, err);

    Scene staged;
    std::vector<ecs::Entity> created;
    std::unordered_map<ecs::EntityGuid, ecs::Entity> guidMap;
    DocumentStats st;
    if (pok) instantiateDocument(staged, doc, created, guidMap, st);
    if (pok) resolveDocumentAssets(staged, am, doc, created, st);

    reportFlag("equiv_both_ok", r1.ok && pok,
               std::string("one=") + (r1.ok ? "1" : "0") + ",stage=" + (pok ? "1" : "0"));
    reportFlag("equiv_entity_count", oneShot.world().entityCount() == staged.world().entityCount(),
               std::to_string(oneShot.world().entityCount()) + "/" +
                   std::to_string(staged.world().entityCount()));

    // 逐 guid 比对 + **父子形状**比对 —— 只比数量的话"父子全丢"也能过。
    //
    // "形状相同"的判据不能是"两边的 parent id 相等"：id 是槽位索引，
    // 两个场景的分配顺序虽然一样，但拿 id 比等于在测"槽位分配"，不是在测
    // 父子。所以按**角色**比：B 和 C 的父都必须恰好是那份文件里的 A。
    int guidBad = 0;
    int parentBad = 0;
    {
        const ecs::Entity aA = oneShot.world().find(0xb1ull);
        const ecs::Entity bA = staged.world().find(0xb1ull);
        if (!aA.valid() || !bA.valid()) ++guidBad;
        else {
            for (ecs::EntityGuid g : {0xb2ull, 0xb3ull}) {
                const ecs::Entity a = oneShot.world().find(g);
                const ecs::Entity b = staged.world().find(g);
                if (!a.valid() || !b.valid()) {
                    ++guidBad;
                    continue;
                }
                const auto* ha = oneShot.world().get<ecs::HierarchyComponent>(a);
                const auto* hb = staged.world().get<ecs::HierarchyComponent>(b);
                const bool okA = ha && ha->parent == aA.id;
                const bool okB = hb && hb->parent == bA.id;
                if (okA != okB || !okA) ++parentBad;
            }
        }
    }
    report("equiv_guid_lookup", static_cast<double>(guidBad), 0.0);
    report("equiv_parent_shape", static_cast<double>(parentBad), 0.0);

    // ---- 版本校验必须在 clear 之前（G16-2 的债，拆分段最容易搬错）----
    {
        Scene keep;
        keep.createObject("k0");
        keep.createObject("k1");
        const size_t n0 = keep.world().entityCount();
        json future;
        future["version"] = kSceneFormatVersion + 1;
        future["entities"] = json::array();
        const SceneIoResult rf = sceneFromJson(keep, am, future.dump());
        reportFlag("parse_fail_keeps_scene",
                   !rf.ok && keep.world().entityCount() == n0,
                   std::string("ok=") + (rf.ok ? "1" : "0") + ",n=" +
                       std::to_string(keep.world().entityCount()));
    }

    // ---- 三段读回来的场景，再写出去必须与一次性读回来的**逐字节**相同 ----
    // ⚠ 只判长度会漏：两条路径都不写某个字段时长度一样但内容不同（G16 踩过）。
    {
        std::string ja, jb;
        sceneToJson(oneShot, ja);
        sceneToJson(staged, jb);
        reportFlag("equiv_json_bytes", !ja.empty() && ja == jb,
                   std::to_string(ja.size()) + "/" + std::to_string(jb.size()));
        reportFlag("equiv_json_nonempty", ja.size() > 200, std::to_string(ja.size()));
    }
}

}  // namespace

int runSceneIoSelfTest() {
    g_pass = 0;
    g_fail = 0;

    VK_LOG_INFO("== selftest: sceneio (G16-5 three-stage split) ==");
    groupParse();
    groupInstantiate();
    groupEquivalence();

    VK_LOG_INFO("SCENEIO-TOTAL pass=%d fail=%d", g_pass, g_fail);
    return g_fail;
}

}  // namespace scene
