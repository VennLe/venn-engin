// ============================================================
// ecs/GuidSelfTest.cpp —— G16-1 的验收（`Game.exe --selftest guid`）
//
// 只测 World 层的身份不变量，**不测场景加载**（那是 G16-2 的场景级往返，
// 要 fixture 与 `SceneSerializer`；本文件可以完全不需要 GPU）。
//
// ⚠ 每条断言都问过一句："**别的实现会给出不同的数吗**"。
//   最典型的一组是 ⑤/⑨：它们专门抓 `setGuid` 里"忘了先摘旧键"这个 bug ——
//   忘了的话 `m_byGuid` 里留下一串幽灵键，而**其它所有断言都仍然通过**，
//   只有"数一数能命中几个"能看出来（⑨ 期望 1，漏写会是 100）。
// ============================================================

#include "ecs/GuidSelfTest.h"

#include "assets/AssetManager.h"
#include "assets/Texture.h"
#include "core/Logger.h"
#include "ecs/World.h"
#include "scene/Scene.h"
#include "scene/SceneSerializer.h"

#include <string>
#include <unordered_set>
#include <vector>

namespace ecs {

namespace {

int g_pass = 0;
int g_fail = 0;

// ⚠ detail 里**不能有空格**：验收脚本用 `detail=(\S*)` 解析（全仓自检的硬约定）。
void report(const char* name, double actual, double expected, double tol) {
    const bool ok = (actual - expected) <= tol && (expected - actual) <= tol;
    VK_LOG_INFO("GUID-TEST %s actual=%.6f expected=%.6f tol=%.6f result=%s", name, actual,
                expected, tol, ok ? "PASS" : "FAIL");
    if (ok) ++g_pass;
    else ++g_fail;
}

void reportFlag(const char* name, bool ok, const std::string& detail = {}) {
    VK_LOG_INFO("GUID-TEST %s actual=%d expected=1 tol=0 detail=%s result=%s", name, ok ? 1 : 0,
                detail.c_str(), ok ? "PASS" : "FAIL");
    if (ok) ++g_pass;
    else ++g_fail;
}

// ---------------- ① 分配：不重复 / 不返回 0 ----------------
void groupAllocate() {
    World w;

    std::unordered_set<EntityGuid> seen;
    seen.reserve(100000);
    for (int i = 0; i < 100000; ++i) seen.insert(w.allocateGuid());
    report("guid_unique_100k", static_cast<double>(seen.size()), 100000.0, 0.0);

    // 0 保留给"无身份"。若实现直接 `return rng()`，撞上 0 的概率是 2^-64 ——
    // 100 万次几乎测不出来；这条真正防的是"实现里写成 `g % 0xFFFF` 之类的窄化"。
    int zeros = 0;
    for (int i = 0; i < 1000000; ++i) {
        if (w.allocateGuid() == kInvalidGuid) ++zeros;
    }
    report("guid_never_zero", static_cast<double>(zeros), 0.0, 0.0);
}

// ---------------- ② 反查：find(guid(e)) == e ----------------
void groupLookup() {
    World w;
    std::vector<Entity> es;
    es.reserve(1000);
    for (int i = 0; i < 1000; ++i) es.push_back(w.create("e" + std::to_string(i)));

    int bad = 0;
    for (const Entity e : es) {
        if (w.find(w.guid(e)) != e) ++bad;
    }
    report("guid_find_roundtrip", static_cast<double>(bad), 0.0, 0.0);

    // 边界：find(0) 必须给无效句柄，**不能**"撞上第一个 guid 为 0 的实体"。
    reportFlag("guid_find_zero_empty", !w.find(kInvalidGuid).valid());

    // 无效句柄取 guid 应为 0（而不是越界读 m_records）。
    reportFlag("guid_invalid_entity_zero", w.guid(Entity{}) == kInvalidGuid);
}

// ---------------- ③ 冲突拒绝 ----------------
void groupConflict() {
    World w;
    const Entity a = w.create("A");
    const Entity b = w.create("B");
    const EntityGuid ga = w.guid(a);
    const EntityGuid gb = w.guid(b);

    const bool accepted = w.setGuid(b, ga);  // 期望：拒绝
    const bool aKept = (w.guid(a) == ga);
    const bool bKept = (w.guid(b) == gb);
    reportFlag("guid_set_duplicate_rejected", !accepted && aKept && bKept,
               std::string("acc=") + (accepted ? "1" : "0") +
                   ",a=" + (aKept ? "1" : "0") + ",b=" + (bKept ? "1" : "0"));

    // 边界：把**自己的** guid 再 set 一遍，必须成功（幂等）。
    // ⚠ 这条钉的是实现里那个 `it->second != e.id` 判据。写成"任何命中就 false"
    //   会让"读档时把文件里的 guid 写回同一个实体"变成一次静默的 dup 计数。
    const bool selfOk = w.setGuid(a, ga);
    reportFlag("guid_set_self_ok", selfOk && w.guid(a) == ga,
               std::string("ok=") + (selfOk ? "1" : "0"));
}

// ---------------- ④ setGuid 不留幽灵（本轮的核心否定测试） ----------------
void groupNoGhost() {
    World w;
    const Entity e = w.create("E");
    const EntityGuid before = w.guid(e);          // create() 给的临时身份
    const EntityGuid want = 0x1122334455667788ull; // 模拟"读档时用文件里的 guid"

    const bool ok = w.setGuid(e, want);
    const bool oldDead = !w.find(before).valid();  // ⚠ 忘了 erase 这里就是 false
    const bool newAlive = (w.find(want) == e);
    reportFlag("guid_no_ghost_after_set", ok && oldDead && newAlive,
               std::string("old=") + (oldDead ? "dead" : "ghost") +
                   ",new=" + (newAlive ? "ok" : "miss"));

    // 连续改 100 次：能命中的候选**必须恰好 1 个**（最后一个）。
    // 漏写 erase 的话这里是 100 —— 而 ④ 那条在"同一个实体不断换 guid"时看不出来。
    World w2;
    const Entity e2 = w2.create("E2");
    std::vector<EntityGuid> cand;
    cand.reserve(100);
    for (int i = 1; i <= 100; ++i) cand.push_back(0xAA00000000000000ull + static_cast<EntityGuid>(i));
    for (const EntityGuid g : cand) w2.setGuid(e2, g);

    int hits = 0;
    for (const EntityGuid g : cand) {
        if (w2.find(g).valid()) ++hits;
    }
    report("guid_set_chain_no_ghost", static_cast<double>(hits), 1.0, 0.0);
}

// ---------------- ⑤ 销毁 / 清空 ----------------
void groupLifetime() {
    World w;
    const Entity e = w.create("E");
    const EntityGuid g = w.guid(e);
    const EntityId oldId = e.id;
    w.destroy(e);

    const bool dead = !w.find(g).valid();
    const bool guidZero = (w.guid(e) == kInvalidGuid);  // 旧句柄已失效 → 0

    const Entity n = w.create("N");
    const bool newGuid = (w.guid(n) != g);
    // ⚠ 这条是"槽位复用"的杀手：新实体占了**同一个 id**，但**旧 guid 不能因此复活**。
    //   destroy 里"先清零再 erase"（顺序写反）就会让旧键留在表里指向这个 id，
    //   此刻记录已经 alive → find(旧guid) 命中新实体，旧存档的引用静默换了个物体。
    //   `dead`（在 create 之前取）看不出来 —— find 的 alive 防御会把它挡成空。
    const bool oldStillDead = !w.find(g).valid();
    reportFlag("guid_destroy_unmaps", dead && guidZero && newGuid && oldStillDead,
               std::string("dead=") + (dead ? "1" : "0") +
                   ",slot=" + (n.id == oldId ? "reused" : "new") +
                   ",distinct=" + (newGuid ? "1" : "0") +
                   ",old=" + (oldStillDead ? "dead" : "REVIVED"));

    // 同一个槽位反复 create/destroy 100 次：身份必须 100 个都不同。
    // 如果 destroy 没清 guid（或 create 没重新分配），这里会是 1。
    World w2;
    std::unordered_set<EntityGuid> seen;
    for (int i = 0; i < 100; ++i) {
        const Entity t = w2.create("x");
        seen.insert(w2.guid(t));
        w2.destroy(t);
    }
    report("guid_reuse_not_recycled", static_cast<double>(seen.size()), 100.0, 0.0);

    // clear() 之后全部失效
    World w3;
    std::vector<EntityGuid> gs;
    for (int i = 0; i < 50; ++i) gs.push_back(w3.guid(w3.create("c")));
    w3.clear();
    int aliveCount = 0;
    for (const EntityGuid x : gs) {
        if (w3.find(x).valid()) ++aliveCount;
    }
    report("guid_clear_unmaps", static_cast<double>(aliveCount), 0.0, 0.0);
}

// ---------------- ⑥ 跨 World 不串号 ----------------
void groupCrossWorld() {
    // RNG 是**进程级**的，两个 World 各自分配也必须不撞。
    // ⚠ 这条钉的是"RNG 别放进 World，也别用每个 World 一个固定 seed"（后者两批会重放同一串）。
    World w1, w2;
    std::unordered_set<EntityGuid> s1;
    s1.reserve(1000);
    for (int i = 0; i < 1000; ++i) s1.insert(w1.guid(w1.create("a")));

    int overlap = 0;
    for (int i = 0; i < 1000; ++i) {
        if (s1.count(w2.guid(w2.create("b"))) != 0) ++overlap;
    }
    report("guid_two_worlds_disjoint", static_cast<double>(overlap), 0.0, 0.0);
}

// ---------------- ⑦ 文本表示 ----------------
void groupHex() {
    // 前导零必须补满 16 位。不补的话 parse 回来长度不是 16 → 判成"无身份"，
    // 于是"存一次再读"会给这个实体补发一个新 guid —— 它从此变成另一个物体，
    // 而所有日志都显示"加载成功"。
    const EntityGuid low = 0x000000000000000Full;
    const std::string h = guidToHex(low);
    reportFlag("guid_hex_leading_zeros", h.size() == 16 && parseGuidHex(h) == low,
               std::string("s=") + h);

    // 大小写都接受（手写文件、别的工具生成的都会出现大写）
    reportFlag("guid_hex_case_insensitive",
               parseGuidHex("ABCDEF0123456789") == 0xABCDEF0123456789ull &&
                   parseGuidHex("abcdef0123456789") == 0xABCDEF0123456789ull);

    // 垃圾一律整串作废（不做"尽力而为"）：
    //   空 / 短 / 长度够但带 `0x` / 长度够但含非 hex / 全零（= 无身份）
    const bool rej = parseGuidHex("") == kInvalidGuid &&
                     parseGuidHex("abc") == kInvalidGuid &&
                     parseGuidHex("0x123456789abcde") == kInvalidGuid &&
                     parseGuidHex("123456789abcdeFG") == kInvalidGuid &&
                     parseGuidHex("0000000000000000") == kInvalidGuid;
    reportFlag("guid_hex_rejects_garbage", rej);
}

// ---------------- ⑧ 场景往返（G16-2 / G16-3） ----------------
//
// ⚠ 这一组**不需要 GPU**：fixture 里没有 `mesh`，`resolveMesh` 在
//   `if (!jm.is_object()) return nullptr;` 那一行直接返回，所以一个
//   `device == nullptr` 的 AssetManager 就够跑完整往返。
//   别以为"场景序列化测试"必须开窗口 —— 那样它就跑不进 CI，也跑不快。
void groupScene() {
    assets::AssetManager am{assets::TextureContext{}};

    // ---- ⑧ A → json1 → B，guid 必须逐个对上 ----
    scene::Scene a;
    std::vector<EntityGuid> gsA;
    for (int i = 0; i < 10; ++i) {
        const std::string n = "n" + std::to_string(i);
        const Entity e = a.createObject(n);
        // ⚠ 必须给夹具一点"非网格内容"，否则写端会把它整个跳过
        //   （`!mesh && !mat && !hasNonMeshContent` → 计入 skipped）。
        //   踩过的现场：不加这一句，json1 只有 500 字节、一个实体都没写，
        //   于是"往返"断言比的是**两份空文件** —— 而它会 PASS，看起来一切正常。
        //   `folder` 是最轻的选择：不引入任何资产，只多写一个 `"folder": true`。
        if (auto* h = a.world().get<HierarchyComponent>(e)) h->group = true;
        gsA.push_back(a.world().guid(e));
    }
    // 前置自检：源场景自己就没重复的话，下面"对不上"才有意义
    int badSrc = 0;
    {
        std::unordered_set<EntityGuid> s(gsA.begin(), gsA.end());
        if (s.size() != 10 || s.count(kInvalidGuid) != 0) badSrc = 1;
    }

    std::string json1;
    const scene::SceneIoResult wr = scene::sceneToJson(a, json1);

    scene::Scene b;
    const scene::SceneIoResult rd = scene::sceneFromJson(b, am, json1);

    int same = 0;
    for (int i = 0; i < 10; ++i) {
        const Entity e = b.find("n" + std::to_string(i));
        if (e.valid() && b.world().guid(e) == gsA[static_cast<size_t>(i)]) ++same;
    }
    reportFlag("scene_guid_roundtrip", badSrc == 0 && wr.ok && rd.ok && same == 10,
               std::string("same=") + std::to_string(same) + ",bad=" + std::to_string(badSrc) +
                   ",wr=" + (wr.ok ? "1" : "0") + ",rd=" + (rd.ok ? "1" : "0") +
                   ",n=" + std::to_string(json1.size()));

    // ---- ⑨ 二次导出必须逐字节相同 ----
    // 写端若把 guid 从"读回来的值"重算（比如又调了一次 allocateGuid），这里会不等。
    std::string json2;
    const scene::SceneIoResult wr2 = scene::sceneToJson(b, json2);
    reportFlag("scene_guid_stable_twice", wr2.ok && json1 == json2,
               std::string("len=") + std::to_string(json1.size()) + "/" +
                   std::to_string(json2.size()));

    // ---- ⑩ 老场景（没有 guid）→ 每个实体补发一个，且互不相同 ----
    std::string legacy = R"({"version":1,"entities":[)";
    for (int i = 0; i < 10; ++i) {
        if (i != 0) legacy += ",";
        legacy += std::string(R"({"name":"L)") + std::to_string(i) +
                  R"(","transform":{"position":[0,0,0],"rotation":[0,0,0],"scale":[1,1,1]}})";
    }
    legacy += "]}";

    scene::Scene l;
    const scene::SceneIoResult rl = scene::sceneFromJson(l, am, legacy);
    std::unordered_set<EntityGuid> lg;
    for (int i = 0; i < 10; ++i) {
        const Entity e = l.find("L" + std::to_string(i));
        if (e.valid()) lg.insert(l.world().guid(e));
    }
    reportFlag("scene_guid_assigned",
               rl.ok && lg.size() == 10 && lg.count(kInvalidGuid) == 0,
               std::string("n=") + std::to_string(lg.size()));

    // ---- ⑪ 文件里两个实体写同一个 guid → 保留一个、另一个补发，整体仍互不相同 ----
    const char* dupJson = R"({"version":2,"entities":[
      {"name":"D0","guid":"00000000000000aa","transform":{"position":[0,0,0],"rotation":[0,0,0],"scale":[1,1,1]}},
      {"name":"D1","guid":"00000000000000aa","transform":{"position":[0,0,0],"rotation":[0,0,0],"scale":[1,1,1]}},
      {"name":"D2","guid":"00000000000000bb","transform":{"position":[0,0,0],"rotation":[0,0,0],"scale":[1,1,1]}}]})";
    scene::Scene d;
    const scene::SceneIoResult rdup = scene::sceneFromJson(d, am, dupJson);
    std::unordered_set<EntityGuid> dg;
    EntityGuid g0 = kInvalidGuid, g1 = kInvalidGuid;
    if (const Entity e = d.find("D0"); e.valid()) {
        g0 = d.world().guid(e);
        dg.insert(g0);
    }
    if (const Entity e = d.find("D1"); e.valid()) {
        g1 = d.world().guid(e);
        dg.insert(g1);
    }
    if (const Entity e = d.find("D2"); e.valid()) dg.insert(d.world().guid(e));
    // 判据：总数仍是 3 个不同值，且**恰好一个**是文件里那个 aa
    int aaCount = 0;
    if (g0 == 0xaall) ++aaCount;
    if (g1 == 0xaall) ++aaCount;
    reportFlag("scene_guid_dup_reassign", rdup.ok && dg.size() == 3 && dg.count(kInvalidGuid) == 0,
               std::string("n=") + std::to_string(dg.size()) + ",aa=" + std::to_string(aaCount));

    // ---- ⑫ 两个独立场景的 guid 集合必须无交集（合并场景的前提）----
    scene::Scene s1, s2;
    std::unordered_set<EntityGuid> set1;
    for (int i = 0; i < 200; ++i) set1.insert(s1.world().guid(s1.createObject("p")));
    int overlap = 0;
    for (int i = 0; i < 200; ++i) {
        if (set1.count(s2.world().guid(s2.createObject("q"))) != 0) ++overlap;
    }
    report("scene_merge_no_collision", static_cast<double>(overlap), 0.0, 0.0);

    // ---- ⑬ 版本比我们新 → 明确拒绝，且**不能把当前场景清掉** ----
    // ⚠ 这条钉的是"校验必须排在 scene.clear() 之前"。顺序写反的话，
    //   打开一个来自新版引擎的文件 = 丢掉当前未保存的整个场景。
    scene::Scene fut;
    fut.createObject("keep");
    const scene::SceneIoResult rf =
        scene::sceneFromJson(fut, am, R"({"version":99,"entities":[]})");
    const bool kept = fut.find("keep").valid();
    reportFlag("scene_version_too_new", !rf.ok && kept,
               std::string("ok=") + (rf.ok ? "1" : "0") + ",kept=" + (kept ? "1" : "0"));

    // ---- ⑭ parentGuid 指不到人时不能崩、不能算失败（父可能在还没流送进来的块里）----
    const char* orphanJson = R"({"version":2,"entities":[
      {"name":"orphan","guid":"00000000000000ff","parentGuid":"1111111111111111",
       "transform":{"position":[0,0,0],"rotation":[0,0,0],"scale":[1,1,1]}}]})";
    scene::Scene o;
    const scene::SceneIoResult ro = scene::sceneFromJson(o, am, orphanJson);
    const Entity oe = o.find("orphan");
    bool hasParent = false;
    if (oe.valid()) {
        if (const auto* h = o.world().get<HierarchyComponent>(oe)) {
            hasParent = (h->parent != ecs::kInvalidEntity);
        }
    }
    reportFlag("scene_parent_guid_miss_safe",
               ro.ok && oe.valid() && !hasParent && o.world().guid(oe) == 0xffull,
               std::string("ok=") + (ro.ok ? "1" : "0") + ",parent=" + (hasParent ? "1" : "0"));
}

// ---------------- ⑨ 选择接回（G16-4） ----------------
//
// 验的是 `Scene::reconnectByGuidOrName` —— 编辑器撤销 / Play→Stop 之后
// "我刚才选的是谁"那条路。**两个实体同名**是这个 bug 的触发器：
// 按名字找永远返回第一个，而用户选的是第二个。
void groupReconnect() {
    assets::AssetManager am{assets::TextureContext{}};

    scene::Scene a;
    const Entity c1 = a.createObject("Crate");
    const Entity c2 = a.createObject("Crate");  // ⚠ 刻意同名
    // 给一点"非网格内容"，否则导出时会被跳过（见 ⑧ 的注释）
    if (auto* h = a.world().get<HierarchyComponent>(c1)) h->group = true;
    if (auto* h = a.world().get<HierarchyComponent>(c2)) h->group = true;
    const EntityGuid wantGuid = a.world().guid(c2);  // 用户选的是**第二个**

    // 模拟"撤销"：整场重建。guid 能保留是因为快照里写了它。
    std::string json;
    scene::sceneToJson(a, json);
    scene::Scene b;
    scene::sceneFromJson(b, am, json);

    const Entity got = b.reconnectByGuidOrName(wantGuid, "Crate");
    reportFlag("selection_reconnect_by_guid", got.valid() && b.world().guid(got) == wantGuid,
               std::string("got=") + (got.valid() ? "1" : "0") +
                   ",match=" + (got.valid() && b.world().guid(got) == wantGuid ? "1" : "0"));

    // 对照：壳里的 `find(name)`（旧行为）必须**确实**落在另一个实体上。
    // 如果它也挑中了同一个，说明夹具没造出歧义 → 上面那条断言什么都没测。
    // （这条 FAIL 不是"实现坏了"，是"夹具失效了"，先换夹具再谈实现。）
    const Entity byName = b.find("Crate");
    reportFlag("selection_fixture_has_ambiguity",
               byName.valid() && got.valid() && byName.id != got.id,
               std::string("same=") + (byName.valid() && got.valid() && byName.id == got.id
                                           ? "1"
                                           : "0"));

    // 没有线索（guid 0 + 空名字）→ 明确返回无效，而不是"随便给一个"
    reportFlag("selection_reconnect_no_hint_empty",
               !b.reconnectByGuidOrName(kInvalidGuid, std::string()).valid());
}

} // namespace

int runGuidSelfTest() {
    g_pass = 0;
    g_fail = 0;
    groupAllocate();
    groupLookup();
    groupConflict();
    groupNoGhost();
    groupLifetime();
    groupCrossWorld();
    groupHex();
    groupScene();
    groupReconnect();
    VK_LOG_INFO("GUID-TOTAL pass=%d fail=%d", g_pass, g_fail);
    return g_fail;
}

} // namespace ecs
