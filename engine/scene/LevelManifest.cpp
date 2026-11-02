// engine/scene/LevelManifest.cpp —— 关卡清单：解析 / 写盘 / 判定 / 自检
//
// 为什么判定（距离、该不该加载）是**纯函数**：它们是流送里唯一"错了也不会
// 崩、只会表现成卡顿或数据丢失"的部分。放进 `LevelStreamer` 里就只能在
// 有场景、有资产的进程里验 —— 而这些函数一个字节的 GPU 都不需要。

#include "scene/LevelManifest.h"

#include "core/Logger.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <set>

namespace scene {

using nlohmann::json;

const LevelEntry* LevelManifest::find(const std::string& n) const {
    for (const LevelEntry& e : levels)
        if (e.name == n) return &e;
    return nullptr;
}

LevelEntry* LevelManifest::find(const std::string& n) {
    for (LevelEntry& e : levels)
        if (e.name == n) return &e;
    return nullptr;
}

// ---------------------------------------------------------------- 解析

LevelManifestIo parseLevelManifest(const std::string& jsonText, LevelManifest& out) {
    LevelManifestIo r;
    out = LevelManifest{};

    json root;
    try {
        root = json::parse(jsonText);
    } catch (const std::exception& ex) {
        r.error = std::string("JSON parse error: ") + ex.what();
        return r;
    }
    if (!root.is_object()) {
        r.error = "root is not an object";
        return r;
    }

    out.name = root.value("name", std::string("levels"));

    if (!root.contains("levels")) {
        // 没有 `levels` 也算成功（空清单是合法的：一张纯程序生成的世界）。
        r.ok = true;
        return r;
    }
    if (!root["levels"].is_array()) {
        r.error = "`levels` is not an array";
        return r;
    }

    std::set<std::string> seen;
    int idx = 0;
    for (const json& jl : root["levels"]) {
        if (!jl.is_object()) {
            r.error = "levels[" + std::to_string(idx) + "] is not an object";
            return r;
        }
        LevelEntry e;
        e.name = jl.value("name", std::string());
        e.path = jl.value("path", std::string());
        // ⚠ 名字与路径都必填：缺名字 = 命令与面板都点不到它；
        //   缺路径 = 加载时才发现，而那时已经在流送路径上了（失败要回滚）。
        if (e.name.empty()) {
            r.error = "levels[" + std::to_string(idx) + "] has no `name`";
            return r;
        }
        if (e.path.empty()) {
            r.error = "level '" + e.name + "' has no `path`";
            return r;
        }
        // 重名 = 整份失败（见头文件：收下一半会让其中一个静默不加载）。
        if (!seen.insert(e.name).second) {
            r.error = "duplicate level name '" + e.name + "'";
            return r;
        }
        e.alwaysLoaded = jl.value("alwaysLoaded", false);
        e.enabled = jl.value("enabled", true);

        auto readVec3 = [&jl](const char* key, glm::vec3& out, bool& present) {
            present = false;
            if (!jl.contains(key)) return;
            const json& jv = jl[key];
            if (!jv.is_array() || jv.size() != 3) return;
            out = glm::vec3(jv[0].get<float>(), jv[1].get<float>(),
                            jv[2].get<float>());
            present = true;
        };
        bool hasCenter = false, hasExtent = false;
        readVec3("center", e.center, hasCenter);
        readVec3("halfExtent", e.halfExtent, hasExtent);
        // 只有 center 没有 halfExtent（或反之）是**手写清单最常见**的错：
        // 它不会报错，只会让这张关卡"永远不流送"（halfExtent 全零）。
        // 所以这里直接把"写了 center 却没写 halfExtent"判成错。
        if (hasCenter != hasExtent) {
            r.error = "level '" + e.name +
                      "' has only one of `center` / `halfExtent`";
            return r;
        }
        // 负的尺寸是笔误（写成 -64），abs 一下比让玩家永远进不去盒子好。
        e.halfExtent = glm::abs(e.halfExtent);
        out.levels.push_back(std::move(e));
        ++idx;
    }

    r.ok = true;
    r.levels = static_cast<int>(out.levels.size());
    return r;
}

LevelManifestIo loadLevelManifest(const std::string& path, LevelManifest& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        LevelManifestIo r;
        r.error = "cannot open: " + path;
        return r;
    }
    std::string text((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    LevelManifestIo r = parseLevelManifest(text, out);
    if (!r.ok) VK_LOG_ERROR("LEVELS-IO parse-failed path=%s err=%s",
                            path.c_str(), r.error.c_str());
    return r;
}

std::string levelManifestToJson(const LevelManifest& m) {
    json root;
    root["name"] = m.name;
    json arr = json::array();
    for (const LevelEntry& e : m.levels) {
        json jl;
        jl["name"] = e.name;
        jl["path"] = e.path;
        if (e.alwaysLoaded) jl["alwaysLoaded"] = true;
        // `enabled` 只在 false 时写：清单是人读的，默认值占屏没意义。
        if (!e.enabled) jl["enabled"] = false;
        // 触发盒**两个都存在**才写（与解析端的校验对称）。
        if (e.halfExtent.x != 0.0f || e.halfExtent.y != 0.0f ||
            e.halfExtent.z != 0.0f) {
            jl["center"] = json::array({e.center.x, e.center.y, e.center.z});
            jl["halfExtent"] =
                json::array({e.halfExtent.x, e.halfExtent.y, e.halfExtent.z});
        }
        arr.push_back(std::move(jl));
    }
    root["levels"] = std::move(arr);
    return root.dump(2);
}

bool saveLevelManifest(const LevelManifest& m, const std::string& path,
                       std::string* err) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        if (err) *err = "cannot open for write: " + path;
        return false;
    }
    out << levelManifestToJson(m) << '\n';
    if (!out) {
        if (err) *err = "write failed: " + path;
        return false;
    }
    VK_LOG_INFO("LEVELS-IO saved path=%s levels=%zu", path.c_str(),
                m.levels.size());
    return true;
}

// ---------------------------------------------------------------- 判定

float levelDistanceTo(const LevelEntry& e, const glm::vec3& viewer) {
    // 无盒 = 不参与距离判定。用无穷大而不是 0：0 会让"距离 <= 0 就加载"
    // 这条规则把没有盒的关卡**每帧**都判成"该加载"。
    if (e.halfExtent.x == 0.0f && e.halfExtent.y == 0.0f &&
        e.halfExtent.z == 0.0f) {
        return std::numeric_limits<float>::infinity();
    }
    const glm::vec3 d = glm::abs(viewer - e.center) - e.halfExtent;
    // 逐轴取正部分再求长度 = 到 AABB 的欧氏距离（盒内 = 0）。
    const glm::vec3 p = glm::max(d, glm::vec3(0.0f));
    return glm::length(p);
}

bool levelWantsLoad(const LevelEntry& e, const glm::vec3& viewer) {
    if (e.halfExtent.x == 0.0f && e.halfExtent.y == 0.0f &&
        e.halfExtent.z == 0.0f) {
        return false;   // 没有触发盒：只能常驻 / 手动
    }
    const glm::vec3 d = glm::abs(viewer - e.center);
    return d.x <= e.halfExtent.x && d.y <= e.halfExtent.y &&
           d.z <= e.halfExtent.z;
}

bool levelWantsUnload(const LevelEntry& e, const glm::vec3& viewer,
                      float unloadScale) {
    if (e.halfExtent.x == 0.0f && e.halfExtent.y == 0.0f &&
        e.halfExtent.z == 0.0f) {
        return false;
    }
    if (unloadScale < 1.0f) unloadScale = 1.0f;   // 缩水就没有缓冲带了
    const glm::vec3 d = glm::abs(viewer - e.center);
    const glm::vec3 lim = e.halfExtent * unloadScale;
    return d.x > lim.x || d.y > lim.y || d.z > lim.z;
}

// ---------------------------------------------------------------- 自检

int runLevelManifestSelfTest() {
    int pass = 0;
    int fail = 0;
    auto check = [&](bool ok, const char* name, const std::string& detail) {
        if (ok) {
            ++pass;
            std::printf("LEVEL-TEST PASS %s\n", name);
        } else {
            ++fail;
            std::printf("LEVEL-TEST FAIL %s (%s)\n", name, detail.c_str());
        }
    };

    // ---- L1 基本解析 ----
    {
        LevelManifest m;
        const auto io = parseLevelManifest(
            R"({"name":"world","levels":[
                 {"name":"hub","path":"scenes/levels/hub.json","alwaysLoaded":true},
                 {"name":"forest","path":"scenes/levels/forest.json",
                  "center":[100,0,0],"halfExtent":[32,32,16]}]})",
            m);
        check(io.ok, "L1 parse-ok", io.error);
        check(m.levels.size() == 2, "L1b level-count",
              std::to_string(m.levels.size()));
        check(m.name == "world", "L1c manifest-name", m.name);
        const LevelEntry* hub = m.find("hub");
        check(hub && hub->alwaysLoaded, "L1d always-loaded",
              hub ? std::to_string(hub->alwaysLoaded) : "null");
        const LevelEntry* fo = m.find("forest");
        check(fo && fo->halfExtent.x == 32.0f, "L1e half-extent",
              fo ? std::to_string(fo->halfExtent.x) : "null");
    }

    // ---- L2 校验：缺 name / 缺 path / 重名 一律整份失败 ----
    {
        LevelManifest m;
        auto bad = [&m](const char* text) {
            return !parseLevelManifest(text, m).ok;
        };
        check(bad(R"({"levels":[{"path":"a.json"}]})"), "L2 reject-no-name", "");
        check(bad(R"({"levels":[{"name":"a"}]})"), "L2b reject-no-path", "");
        check(bad(R"({"levels":[{"name":"a","path":"a.json"},)"
                  R"({"name":"a","path":"b.json"}]})"),
              "L2c reject-duplicate", "");
        // 只有 center 没有 halfExtent：手写清单最常见的错，必须拒收
        check(bad(R"({"levels":[{"name":"a","path":"a.json","center":[0,0,0]}]})"),
              "L2d reject-half-box", "");
        // 空清单是合法的
        LevelManifest empty;
        check(parseLevelManifest(R"({"levels":[]})", empty).ok &&
                  empty.levels.empty(),
              "L2e empty-is-ok", "");
        // 完全没有 levels 键也合法
        LevelManifest none;
        check(parseLevelManifest(R"({"name":"x"})", none).ok,
              "L2f no-levels-key-ok", "");
    }

    // ---- L3 往返 ----
    {
        LevelManifest m;
        parseLevelManifest(R"({"name":"w","levels":[
            {"name":"a","path":"a.json","alwaysLoaded":true},
            {"name":"b","path":"b.json","center":[1,2,3],
             "halfExtent":[8,9,10]}]})", m);
        LevelManifest back;
        const auto io = parseLevelManifest(levelManifestToJson(m), back);
        check(io.ok && back.levels.size() == 2, "L3 roundtrip-count",
              io.error);
        const LevelEntry* b = back.find("b");
        check(b && b->center.z == 3.0f && b->halfExtent.y == 9.0f,
              "L3b roundtrip-box", b ? std::to_string(b->halfExtent.y) : "null");
        check(back.find("a") && back.find("a")->alwaysLoaded,
              "L3c roundtrip-always", "");
    }

    // ---- L4 距离：盒内 = 0，盒外 = 到盒面 ----
    {
        LevelEntry e;
        e.name = "x";
        e.path = "x.json";
        e.center = glm::vec3(100.0f, 0.0f, 0.0f);
        e.halfExtent = glm::vec3(10.0f, 10.0f, 10.0f);
        // 中心 → 0
        check(levelDistanceTo(e, glm::vec3(100.0f, 0.0f, 0.0f)) == 0.0f,
              "L4 center-is-zero", "");
        // 盒内 → 0（这一点是"用中心距"会算错的：中心距是 9.5）
        check(levelDistanceTo(e, glm::vec3(109.5f, 0.0f, 0.0f)) == 0.0f,
              "L4b inside-is-zero", "");
        // 盒外 x 方向 5 米 → 5（**不是** 15）
        {
            const float d = levelDistanceTo(e, glm::vec3(115.0f, 0.0f, 0.0f));
            check(std::fabs(d - 5.0f) < 1e-4f, "L4c outside-to-face",
                  std::to_string(d));
        }
        // 斜角：x 差 3、y 差 4（都出了盒）→ 5
        {
            const float d =
                levelDistanceTo(e, glm::vec3(113.0f, 14.0f, 0.0f));
            check(std::fabs(d - 5.0f) < 1e-4f, "L4d outside-diagonal",
                  std::to_string(d));
        }
        // 没有盒 → 无穷大
        {
            LevelEntry n;
            n.name = "n";
            n.path = "n.json";
            const float d = levelDistanceTo(n, glm::vec3(1e6f));
            check(std::isinf(d), "L4e no-box-is-inf", std::to_string(d));
        }
    }

    // ---- L5 该不该加载：盒内才要 ----
    {
        LevelEntry e;
        e.name = "x";
        e.path = "x.json";
        e.center = glm::vec3(0.0f);
        e.halfExtent = glm::vec3(10.0f, 10.0f, 10.0f);
        check(levelWantsLoad(e, glm::vec3(0.0f)), "L5 inside-wants", "");
        check(levelWantsLoad(e, glm::vec3(10.0f, 10.0f, 10.0f)),
              "L5b boundary-inclusive", "");
        check(!levelWantsLoad(e, glm::vec3(10.1f, 0.0f, 0.0f)),
              "L5c outside-not-wanted", "");
        LevelEntry n;
        n.name = "n";
        n.path = "n.json";
        check(!levelWantsLoad(n, glm::vec3(0.0f)), "L5d no-box-never-wants",
              "");
    }

    // ---- L6 滞回：加载盒与卸载盒必须是**两个** ----
    {
        LevelEntry e;
        e.name = "x";
        e.path = "x.json";
        e.center = glm::vec3(0.0f);
        e.halfExtent = glm::vec3(10.0f, 10.0f, 10.0f);
        // 缓冲带内（11 米）：已经加载的**不该**卸
        const glm::vec3 in(11.0f, 0.0f, 0.0f);
        check(!levelWantsLoad(e, in), "L6 buffer-not-wanted",
              "在此处加载 = 一进缓冲带就重复加载");
        check(!levelWantsUnload(e, in, 1.25f), "L6b buffer-not-unloaded", "");
        // 缓冲带外（13 米）才卸
        check(levelWantsUnload(e, glm::vec3(13.0f, 0.0f, 0.0f), 1.25f),
              "L6c outside-unloads", "");
        // unloadScale 缩到 1.0 也不能出现"12 米内还不卸"之外的怪事
        check(levelWantsUnload(e, glm::vec3(12.0f, 0.0f, 0.0f), 1.0f),
              "L6d scale-one", "");
        // 小于 1 的 scale 被钳回 1（否则卸载盒比加载盒还小 = 永远卸不掉）
        check(levelWantsUnload(e, glm::vec3(11.0f, 0.0f, 0.0f), 0.5f),
              "L6e scale-clamped", "");
    }

    // ---- L7 负的半尺寸取绝对值（手写 `-32` 是笔误）----
    {
        LevelManifest m;
        parseLevelManifest(
            R"({"levels":[{"name":"a","path":"a.json",
                 "center":[0,0,0],"halfExtent":[-32,-32,-16]}]})", m);
        check(m.levels.size() == 1 && m.levels[0].halfExtent.x == 32.0f,
              "L7 abs-half-extent",
              m.levels.empty() ? "none" : std::to_string(m.levels[0].halfExtent.x));
    }

    // ---- L8 enabled=false 不是"删掉" ----
    {
        LevelManifest m;
        parseLevelManifest(
            R"({"levels":[{"name":"a","path":"a.json","enabled":false}]})", m);
        check(m.levels.size() == 1 && !m.levels[0].enabled,
              "L8 disabled-kept", "");
        // 默认是 true
        LevelManifest d;
        parseLevelManifest(R"({"levels":[{"name":"a","path":"a.json"}]})", d);
        check(!d.levels.empty() && d.levels[0].enabled, "L8b default-enabled",
              "");
    }

    std::printf("LEVEL-TEST total pass=%d fail=%d\n", pass, fail);
    return fail;
}

} // namespace scene
