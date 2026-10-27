#include "scene/Prefab.h"

#include "assets/AssetManager.h"
#include "assets/AssetPath.h"
#include "core/Logger.h"
#include "ecs/Components.h"
#include "scene/Scene.h"
#include "scene/SceneEntityIO.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <unordered_set>

namespace scene {

namespace {

constexpr int kMaxDepth = 64;        // 父子链上限（环用，也防文件写坏）
constexpr int kMaxNodes = 4096;      // 一份 prefab 的节点上限

std::string slugOf(const std::string& name) {
    std::string s;
    s.reserve(name.size());
    for (char c : name) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u) || c == '_' || c == '-')
            s.push_back(c);
        else if (c == ' ' || c == '.' || c == '/')
            s.push_back('_');
    }
    if (s.empty()) s = "node";
    if (s.size() > 48) s.resize(48);
    return s;
}

// 覆盖路径拆两半：`crate/transform.position` → ("crate", "transform", "position")
bool splitOverridePath(const std::string& path, std::string& localId,
                       std::string& component, std::string& field) {
    const std::size_t slash = path.find('/');
    if (slash == std::string::npos || slash == 0) return false;
    localId  = path.substr(0, slash);
    const std::string rest = path.substr(slash + 1);
    const std::size_t dot = rest.find('.');
    if (dot == std::string::npos) {
        component = rest;
        field.clear();
    } else {
        component = rest.substr(0, dot);
        field     = rest.substr(dot + 1);
    }
    return !component.empty();
}

nlohmann::json getPath(const nlohmann::json& j, const std::string& comp,
                       const std::string& field) {
    if (!j.is_object() || !j.contains(comp)) return nlohmann::json();
    if (field.empty()) return j[comp];
    const nlohmann::json& c = j[comp];
    if (!c.is_object() || !c.contains(field)) return nlohmann::json();
    return c[field];
}

// 写回。**允许新建字段**：prefab 里没有 `visible` 而实例把它设成 false 是
// 合法的覆盖，拒绝写入会让"Apply 之后文件里没有这个字段"变成静默丢改动。
void setPath(nlohmann::json& j, const std::string& comp, const std::string& field,
             const nlohmann::json& v) {
    if (!j.is_object()) j = nlohmann::json::object();
    if (field.empty()) {
        j[comp] = v;
        return;
    }
    if (!j.contains(comp) || !j[comp].is_object()) j[comp] = nlohmann::json::object();
    j[comp][field] = v;
}

std::uint32_t nextInstanceId(const scene::Scene& sc) {
    std::uint32_t maxId = 0;
    sc.world().each<ecs::PrefabLinkComponent>(
        [&](ecs::Entity, const ecs::PrefabLinkComponent& pl) {
            maxId = std::max(maxId, pl.instance);
        });
    return maxId + 1;
}

// 子树展平（根在最前）。只收带 Transform 的实体 —— 与场景写端口径一致
// （太阳没有变换，永远不进 JSON）。
void collectSubtree(const ecs::World& w, ecs::Entity e, int depth,
                    std::vector<ecs::Entity>& out) {
    if (!e.valid() || depth > kMaxDepth) return;
    if (!w.get<ecs::TransformComponent>(e)) return;
    out.push_back(e);
    const auto* h = w.get<ecs::HierarchyComponent>(e);
    if (!h) return;
    for (ecs::EntityId cid : h->children) {
        const ecs::Entity child = w.handle(cid);
        if (child.valid()) collectSubtree(w, child, depth + 1, out);
    }
}

} // namespace

// ---------------------------------------------------------------- PrefabAsset

int PrefabAsset::indexOf(const std::string& localId) const {
    for (std::size_t i = 0; i < nodes.size(); ++i)
        if (nodes[i].localId == localId) return static_cast<int>(i);
    return -1;
}

const PrefabNode* PrefabAsset::find(const std::string& localId) const {
    const int i = indexOf(localId);
    return i < 0 ? nullptr : &nodes[static_cast<std::size_t>(i)];
}

// ---------------------------------------------------------------- 解析 / 写

bool parsePrefab(const std::string& text, PrefabAsset& out) {
    out = PrefabAsset{};
    nlohmann::json root;
    try {
        root = nlohmann::json::parse(text);
    } catch (const std::exception& e) {
        out.error = std::string("json: ") + e.what();
        VK_LOG_ERROR("PREFAB-PARSE ok=0 err=%s", out.error.c_str());
        return false;
    }
    if (!root.is_object()) {
        out.error = "root is not an object";
        VK_LOG_ERROR("PREFAB-PARSE ok=0 err=%s", out.error.c_str());
        return false;
    }
    out.version = root.value("version", 1);
    out.name    = root.value("name", std::string("Prefab"));
    out.comment = root.value("//", std::string());

    if (!root.contains("entities") || !root["entities"].is_array() ||
        root["entities"].empty()) {
        out.error = "entities missing or empty";
        VK_LOG_ERROR("PREFAB-PARSE ok=0 err=%s", out.error.c_str());
        return false;
    }
    const auto& arr = root["entities"];
    if (static_cast<int>(arr.size()) > kMaxNodes) {
        out.error = "too many entities";
        VK_LOG_ERROR("PREFAB-PARSE ok=0 err=%s n=%d", out.error.c_str(),
                     static_cast<int>(arr.size()));
        return false;
    }

    std::unordered_set<std::string> ids;
    for (const auto& je : arr) {
        if (!je.is_object()) {
            out.error = "entity is not an object";
            VK_LOG_ERROR("PREFAB-PARSE ok=0 err=%s", out.error.c_str());
            return false;
        }
        PrefabNode n;
        n.entity = je;
        n.localId = je.value("localId", std::string());
        if (n.localId.empty()) {
            out.error = "entity without localId";
            VK_LOG_ERROR("PREFAB-PARSE ok=0 err=%s index=%d", out.error.c_str(),
                         static_cast<int>(out.nodes.size()));
            return false;
        }
        // localId 只允许"标识符字符"——它会被拼进覆盖路径（`lid/transform.x`），
        // 带 `/` 或 `.` 会让路径解析产生歧义。
        for (char c : n.localId) {
            const unsigned char u = static_cast<unsigned char>(c);
            if (!(std::isalnum(u) || c == '_' || c == '-')) {
                out.error = "localId has illegal character: " + n.localId;
                VK_LOG_ERROR("PREFAB-PARSE ok=0 err=%s", out.error.c_str());
                return false;
            }
        }
        if (!ids.insert(n.localId).second) {
            out.error = "duplicate localId: " + n.localId;
            VK_LOG_ERROR("PREFAB-PARSE ok=0 err=%s", out.error.c_str());
            return false;
        }
        n.parentIndex = je.value("parent", -1);
        out.nodes.push_back(std::move(n));
    }

    // 父下标合法性 + 环检测（沿着 parent 往上走，走不出根就是环）
    for (std::size_t i = 0; i < out.nodes.size(); ++i) {
        int p = out.nodes[i].parentIndex;
        int guard = 0;
        while (p >= 0) {
            if (p >= static_cast<int>(out.nodes.size()) ||
                p == static_cast<int>(i) || guard++ > kMaxDepth) {
                out.error = "bad/cyclic parent at node " + out.nodes[i].localId;
                VK_LOG_ERROR("PREFAB-PARSE ok=0 err=%s", out.error.c_str());
                return false;
            }
            p = out.nodes[static_cast<std::size_t>(p)].parentIndex;
        }
    }

    out.valid = true;
    VK_LOG_INFO("PREFAB-PARSE path=%s ok=1 nodes=%d name=%s",
                out.path.c_str(), static_cast<int>(out.nodes.size()),
                out.name.c_str());
    return true;
}

bool loadPrefab(const std::string& path, PrefabAsset& out) {
    const std::string full = assets::resolveAssetPath(path);
    std::ifstream in(full, std::ios::binary);
    if (!in) {
        out = PrefabAsset{};
        out.path = path;
        out.error = "cannot open: " + full;
        VK_LOG_ERROR("PREFAB-LOAD path=%s ok=0 err=%s", path.c_str(),
                     out.error.c_str());
        return false;
    }
    std::string text((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    out.path = path;
    if (!parsePrefab(text, out)) {
        out.path = path;
        return false;
    }
    out.path = path;
    return true;
}

// `Crate 2` → `prefabs/crate.vprefab.json`；撞名就 `_2`、`_3`。
// 放在引擎侧（而不是编辑器里各写一份）：两个调用点（层级右键、将来的
// 拖拽放置）都用同一个"默认路径"口径，否则同一次会话里能生成两份名字
// 只差一个下划线的资产。
std::string prefabDefaultPath(const std::string& name) {
    std::string slug = slugOf(name);
    for (char& c : slug)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    const std::string dir = "prefabs/";
    // 资产根可能还没被创建（第一次给这个工程做 prefab），resolveAssetPath
    // 只拼路径不建目录，所以这里只看文件在不在。
    std::string rel = dir + slug + ".vprefab.json";
    for (int n = 2; n < 1000; ++n) {
        std::error_code ec;
        if (!std::filesystem::exists(assets::resolveAssetPath(rel), ec))
            return rel;
        rel = dir + slug + "_" + std::to_string(n) + ".vprefab.json";
    }
    return dir + slug + "_x.vprefab.json";
}

std::string prefabToJson(const PrefabAsset& a) {
    nlohmann::json root = nlohmann::json::object();
    root["kind"]    = "venn.prefab";
    root["version"] = a.version;
    root["name"]    = a.name;
    if (!a.comment.empty()) root["//"] = a.comment;
    nlohmann::json arr = nlohmann::json::array();
    for (const PrefabNode& n : a.nodes) {
        nlohmann::json je = n.entity;
        if (!je.is_object()) je = nlohmann::json::object();
        je["localId"] = n.localId;
        je["parent"]  = n.parentIndex;
        arr.push_back(std::move(je));
    }
    root["entities"] = std::move(arr);
    return root.dump(2) + "\n";
}

bool savePrefab(const PrefabAsset& a, std::string& err) {
    if (a.path.empty()) {
        err = "prefab path is empty";
        return false;
    }
    const std::string full = assets::resolveAssetPath(a.path);
    const std::string tmp  = full + ".tmp";
    const std::string text = prefabToJson(a);
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            err = "cannot open for write: " + tmp;
            VK_LOG_ERROR("PREFAB-SAVE path=%s ok=0 err=%s", a.path.c_str(),
                         err.c_str());
            return false;
        }
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        f.flush();
        if (!f) {
            err = "write failed: " + tmp;
            VK_LOG_ERROR("PREFAB-SAVE path=%s ok=0 err=%s", a.path.c_str(),
                         err.c_str());
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, full, ec);
    if (ec) {
        err = "rename failed: " + ec.message();
        std::error_code ec2;
        std::filesystem::remove(tmp, ec2);
        VK_LOG_ERROR("PREFAB-SAVE path=%s ok=0 err=%s", a.path.c_str(),
                     err.c_str());
        return false;
    }
    VK_LOG_INFO("PREFAB-SAVE path=%s full=%s nodes=%d bytes=%d",
                a.path.c_str(), full.c_str(), static_cast<int>(a.nodes.size()),
                static_cast<int>(text.size()));
    return true;
}

// ---------------------------------------------------------------- 从子树导出

bool prefabFromSubtree(Scene& scene, ecs::Entity root, PrefabAsset& out,
                       std::string* err) {
    out = PrefabAsset{};
    const ecs::World& w = scene.world();
    if (!root.valid() || !w.alive(root)) {
        if (err) *err = "invalid root";
        return false;
    }
    std::vector<ecs::Entity> members;
    collectSubtree(w, root, 0, members);
    if (members.empty()) {
        if (err) *err = "root has no transform";
        VK_LOG_ERROR("PREFAB-EXTRACT ok=0 err=root has no transform");
        return false;
    }

    std::unordered_map<ecs::EntityId, int> indexOf;
    for (std::size_t i = 0; i < members.size(); ++i)
        indexOf[members[i].id] = static_cast<int>(i);

    std::unordered_set<std::string> used;
    for (std::size_t i = 0; i < members.size(); ++i) {
        const ecs::Entity e = members[i];
        PrefabNode n;
        const std::string* nm = w.name(e);
        const std::string base = slugOf(nm ? *nm : std::string("node"));
        std::string id = base;
        int k = 2;
        while (!used.insert(id).second) id = base + "_" + std::to_string(k++);
        n.localId = id;

        nlohmann::json je;
        if (!entityToJson(w, e, indexOf, je)) {
            // 没有内容 = 纯分组节点（只有 name + transform）。**不能**因此
            // 拒收整棵子树：真实 prefab 的根往往就是个空节点（"一整个箱子"
            // 的容器），拒收会让"选择根 → Create Prefab"直接失败。
            // 这里补最小外壳，与 `entityToJson` 写的同名同义。
            je = nlohmann::json::object();
            je["name"] = nm ? *nm : std::string("Entity");
            if (const auto* t = w.get<ecs::TransformComponent>(e)) {
                je["transform"] = nlohmann::json{
                    {"position", {t->position.x, t->position.y, t->position.z}},
                    {"rotation", {t->rotation.x, t->rotation.y, t->rotation.z}},
                    {"scale", {t->scale.x, t->scale.y, t->scale.z}},
                };
            }
        }
        je.erase("parent");           // parent 由 localId/下标统一重写
        je["localId"] = id;
        n.entity = std::move(je);
        n.parentIndex = (i == 0) ? -1 : indexOf[members[0].id]; // 下面统一修
        out.nodes.push_back(std::move(n));
    }

    // 重新算 parent：子树内下标（根 -1）
    for (std::size_t i = 0; i < members.size(); ++i) {
        const auto* h = w.get<ecs::HierarchyComponent>(members[i]);
        int pi = -1;
        if (h && h->parent != ecs::kInvalidEntity) {
            auto it = indexOf.find(h->parent);
            if (it != indexOf.end() && it->second != static_cast<int>(i))
                pi = it->second;
        }
        out.nodes[i].parentIndex = pi;
    }

    const std::string* rootName = w.name(root);
    out.name = rootName ? *rootName : std::string("Prefab");
    out.version = 1;
    out.valid = true;
    VK_LOG_INFO("PREFAB-EXTRACT ok=1 nodes=%d name=%s",
                static_cast<int>(out.nodes.size()), out.name.c_str());
    return true;
}

// ---------------------------------------------------------------- 实例化

bool instantiatePrefab(Scene& scene, assets::AssetManager& assets,
                       const PrefabAsset& asset, const glm::vec3& at,
                       PrefabInstance& out, std::string* err) {
    out = PrefabInstance{};
    if (!asset.valid || asset.nodes.empty()) {
        if (err) *err = "prefab is not valid";
        return false;
    }
    ecs::World& w = scene.world();
    const std::uint32_t instanceId = nextInstanceId(scene);

    // 名字去重："Crate / Crate 2 / Crate 3"。重名会让 `Scene::find(name)`
    // （撤销后接回选中、脚本按名字找实体）永远只命中第一个 —— 而 3 份
    // 长得一模一样的 prefab 实例是**最典型的重名来源**。
    std::unordered_map<std::string, int> nameUse;
    w.each<ecs::TransformComponent>([&](ecs::Entity e, const ecs::TransformComponent&) {
        if (const std::string* n = w.name(e)) ++nameUse[*n];
    });

    // ① 建实体 + 外壳（transform / visible / locked）
    out.members.reserve(asset.nodes.size());
    for (std::size_t i = 0; i < asset.nodes.size(); ++i) {
        const PrefabNode& n = asset.nodes[i];
        std::string name = n.entity.value("name", n.localId);
        if (nameUse[name] > 0) {
            const std::string base = name;
            int k = 2;
            while (nameUse.count(base + " " + std::to_string(k))) ++k;
            name = base + " " + std::to_string(k);
        }
        ++nameUse[name];
        const ecs::Entity e = scene.createObject(name);
        entityApplyShell(scene, n.entity, e);
        ecs::PrefabLinkComponent pl;
        pl.prefabPath = asset.path;
        pl.localId    = n.localId;
        pl.instance   = instanceId;
        pl.root       = (i == 0);
        if (auto* p = w.get<ecs::PrefabLinkComponent>(e)) *p = pl;
        else w.add<ecs::PrefabLinkComponent>(e, pl);
        out.members.push_back(e);
    }

    // ② 父子（先建完再挂，`setParent` 会维护 children 双向表）
    for (std::size_t i = 0; i < asset.nodes.size(); ++i) {
        const int pi = asset.nodes[i].parentIndex;
        if (pi >= 0 && pi < static_cast<int>(out.members.size()))
            scene.setParent(out.members[i], out.members[static_cast<std::size_t>(pi)]);
    }

    // ③ 组件（走与场景加载**同一个** `entityFromJson`；`created` 传实例成员，
    //    于是文件里的 parent 下标也能正确解析 —— 与②结果一致）
    for (std::size_t i = 0; i < asset.nodes.size(); ++i) {
        entityFromJson(scene, assets, asset.nodes[i].entity, out.members[i],
                       out.members);
    }

    out.root = out.members.front();
    out.id   = instanceId;
    // ④ 落点只作用于根：子节点保持 prefab 内的相对变换
    if (auto* t = w.get<ecs::TransformComponent>(out.root)) {
        const glm::vec3 authored = t->position;   // 母版作者写的位置
        t->position = at;
        // 落点与母版位置不同 → 这**就是**一条覆盖，必须记。
        // 不记的后果不是"少一条账"，而是：用户把实例摆到 (3,0,0)，之后
        // 母版改了个缩放，再同步一次 —— 实例**自己跳回**母版坐标。摆好的
        // 东西自己跑掉是最没道理的一种 bug，而且现象看起来像"缩放把位置
        // 也带歪了"，排查方向完全是错的。
        if (glm::any(glm::notEqual(authored, at)))
            recordOverride(scene, out.root, "transform", "position");
    }

    VK_LOG_INFO("PREFAB-INSTANCE path=%s instance=%u root=%u members=%d at=%.2f,%.2f,%.2f",
                asset.path.c_str(), instanceId,
                static_cast<unsigned>(out.root.id),
                static_cast<int>(out.members.size()),
                static_cast<double>(at.x), static_cast<double>(at.y),
                static_cast<double>(at.z));
    return true;
}

std::uint32_t prefabNewInstanceId(const Scene& scene) {
    return nextInstanceId(scene);
}

bool prefabBindExistingSubtree(Scene& scene, const PrefabAsset& asset,
                               ecs::Entity root, std::uint32_t* outInstance) {
    if (!asset.valid) return false;
    ecs::World& w = scene.world();
    std::vector<ecs::Entity> members;
    collectSubtree(w, root, 0, members);
    if (members.size() != asset.nodes.size()) {
        VK_LOG_ERROR("PREFAB-BIND ok=0 members=%d nodes=%d",
                     static_cast<int>(members.size()),
                     static_cast<int>(asset.nodes.size()));
        return false;
    }
    const std::uint32_t inst = nextInstanceId(scene);
    for (std::size_t i = 0; i < members.size(); ++i) {
        ecs::PrefabLinkComponent pl;
        pl.prefabPath = asset.path;
        pl.localId    = asset.nodes[i].localId;
        pl.instance   = inst;
        pl.root       = (i == 0);
        if (auto* p = w.get<ecs::PrefabLinkComponent>(members[i])) *p = pl;
        else w.add<ecs::PrefabLinkComponent>(members[i], pl);
    }
    if (outInstance) *outInstance = inst;
    VK_LOG_INFO("PREFAB-BIND ok=1 path=%s instance=%u members=%d",
                asset.path.c_str(), inst, static_cast<int>(members.size()));
    return true;
}

// ---------------------------------------------------------------- 覆盖

std::string prefabOverridePath(const std::string& localId,
                               const std::string& component,
                               const std::string& field) {
    std::string p = localId + "/" + component;
    if (!field.empty()) p += "." + field;
    return p;
}

bool isPrefabMember(const Scene& scene, ecs::Entity e) {
    return e.valid() && scene.world().get<ecs::PrefabLinkComponent>(e) != nullptr;
}

std::string prefabPathOf(const Scene& scene, ecs::Entity member) {
    const auto* pl = scene.world().get<ecs::PrefabLinkComponent>(member);
    return pl ? pl->prefabPath : std::string();
}

ecs::Entity prefabInstanceRoot(Scene& scene, ecs::Entity member) {
    ecs::World& w = scene.world();
    const auto* pl = w.get<ecs::PrefabLinkComponent>(member);
    if (!pl) return ecs::Entity{};
    if (pl->root) return member;
    // 根不一定是它的父节点（prefab 可能根在上、成员挂在中间层），所以按
    // (prefabPath, instance) 扫 —— 也顺带覆盖"根已删但成员还在"的残缺情况。
    ecs::Entity found{};
    w.each<ecs::PrefabLinkComponent>(
        [&](ecs::Entity e, const ecs::PrefabLinkComponent& o) {
            if (found.valid()) return;
            if (o.root && o.instance == pl->instance &&
                o.prefabPath == pl->prefabPath)
                found = e;
        });
    return found;
}

std::uint32_t prefabInstanceId(Scene& scene, ecs::Entity member) {
    const auto* pl = scene.world().get<ecs::PrefabLinkComponent>(member);
    return pl ? pl->instance : 0u;
}

bool recordOverride(Scene& scene, ecs::Entity member,
                    const std::string& component, const std::string& field) {
    ecs::World& w = scene.world();
    const auto* pl = w.get<ecs::PrefabLinkComponent>(member);
    if (!pl) return false;                 // 不是 prefab 成员：常态，不报
    const ecs::Entity root = prefabInstanceRoot(scene, member);
    if (!root.valid()) return false;
    auto* rp = w.get<ecs::PrefabLinkComponent>(root);
    if (!rp) return false;

    const std::string path = prefabOverridePath(pl->localId, component, field);
    if (std::find(rp->overrides.begin(), rp->overrides.end(), path) !=
        rp->overrides.end())
        return false;                      // 已经记过（去重）
    rp->overrides.push_back(path);
    VK_LOG_INFO("PREFAB-OVERRIDE path=%s instance=%u localId=%s total=%d",
                path.c_str(), pl->instance, pl->localId.c_str(),
                static_cast<int>(rp->overrides.size()));
    return true;
}

int countOverrides(const Scene& scene, std::uint32_t instance) {
    int n = 0;
    scene.world().each<ecs::PrefabLinkComponent>(
        [&](ecs::Entity, const ecs::PrefabLinkComponent& pl) {
            if (pl.root && pl.instance == instance)
                n = static_cast<int>(pl.overrides.size());
        });
    return n;
}

int clearInstanceOverrides(Scene& scene, std::uint32_t instance) {
    int cleared = 0;
    scene.world().each<ecs::PrefabLinkComponent>(
        [&](ecs::Entity, ecs::PrefabLinkComponent& pl) {
            if (pl.root && pl.instance == instance) {
                cleared = static_cast<int>(pl.overrides.size());
                pl.overrides.clear();
            }
        });
    if (cleared > 0)
        VK_LOG_INFO("PREFAB-REVERT instance=%u cleared=%d", instance, cleared);
    return cleared;
}

// ---------------------------------------------------------------- 同步

PrefabSyncResult syncInstances(Scene& scene, assets::AssetManager& assets,
                               const PrefabAsset& asset) {
    PrefabSyncResult r;
    if (!asset.valid) return r;
    ecs::World& w = scene.world();

    // 先按实例分组（成员可能散布在场景里任意位置）
    std::unordered_map<std::uint32_t, std::vector<ecs::Entity>> byInstance;
    std::vector<std::uint32_t> order;
    w.each<ecs::PrefabLinkComponent>(
        [&](ecs::Entity e, const ecs::PrefabLinkComponent& pl) {
            if (pl.prefabPath != asset.path) return;
            auto it = byInstance.find(pl.instance);
            if (it == byInstance.end()) {
                byInstance.emplace(pl.instance, std::vector<ecs::Entity>{e});
                order.push_back(pl.instance);
            } else {
                it->second.push_back(e);
            }
        });

    const std::unordered_map<ecs::EntityId, int> emptyIndex;

    for (std::uint32_t instId : order) {
        auto& members = byInstance[instId];
        ++r.instances;

        // 该实例的覆盖表（在根上）
        std::vector<std::string> overrides;
        ecs::Entity root{};
        for (ecs::Entity e : members) {
            if (const auto* pl = w.get<ecs::PrefabLinkComponent>(e)) {
                if (pl->root) {
                    root = e;
                    overrides = pl->overrides;
                }
            }
        }

        for (ecs::Entity e : members) {
            const auto* pl = w.get<ecs::PrefabLinkComponent>(e);
            if (!pl) continue;
            ++r.members;
            const PrefabNode* node = asset.find(pl->localId);
            if (!node) {
                ++r.failed;
                continue;
            }

            // prefab 的权威值
            nlohmann::json base = node->entity;
            // 名字 / 父子是**实例自己的**（同一个 prefab 摆三份，名字可能
            // 被作者改成 "Crate 1/2/3"，不能跟着母版走）
            base.erase("name");
            base.erase("parent");

            // 覆盖的字段：用实例当前值盖回 base，于是"只剩未覆盖的字段跟随"
            bool needCurrent = false;
            for (const std::string& path : overrides) {
                std::string lid, comp, field;
                if (!splitOverridePath(path, lid, comp, field)) continue;
                if (lid != pl->localId) continue;
                needCurrent = true;
                break;
            }
            if (needCurrent) {
                nlohmann::json cur;
                if (entityToJson(w, e, emptyIndex, cur)) {
                    for (const std::string& path : overrides) {
                        std::string lid, comp, field;
                        if (!splitOverridePath(path, lid, comp, field)) continue;
                        if (lid != pl->localId) continue;
                        const nlohmann::json v = getPath(cur, comp, field);
                        if (!v.is_null()) setPath(base, comp, field, v);
                    }
                }
            }

            entityApplyShell(scene, base, e);
            entityFromJson(scene, assets, base, e, members);
            ++r.applied;
        }
    }

    VK_LOG_INFO("PREFAB-SYNC path=%s instances=%d members=%d applied=%d failed=%d",
                asset.path.c_str(), r.instances, r.members, r.applied, r.failed);
    return r;
}

// ---------------------------------------------------------------- Apply

int applyInstanceToAsset(Scene& scene, PrefabAsset& asset,
                         std::uint32_t instance, int* cleared) {
    if (cleared) *cleared = 0;
    ecs::World& w = scene.world();

    std::vector<ecs::Entity> members;
    std::vector<std::string> overrides;
    w.each<ecs::PrefabLinkComponent>(
        [&](ecs::Entity e, const ecs::PrefabLinkComponent& pl) {
            if (pl.prefabPath != asset.path || pl.instance != instance) return;
            members.push_back(e);
            if (pl.root) overrides = pl.overrides;
        });
    if (members.empty()) return 0;

    const std::unordered_map<ecs::EntityId, int> emptyIndex;
    int wrote = 0;
    for (const std::string& path : overrides) {
        std::string lid, comp, field;
        if (!splitOverridePath(path, lid, comp, field)) continue;
        const int nodeIndex = asset.indexOf(lid);
        if (nodeIndex < 0) continue;
        ecs::Entity member{};
        for (ecs::Entity e : members) {
            if (const auto* pl = w.get<ecs::PrefabLinkComponent>(e)) {
                if (pl->localId == lid) { member = e; break; }
            }
        }
        if (!member.valid()) continue;
        nlohmann::json cur;
        if (!entityToJson(w, member, emptyIndex, cur)) continue;
        const nlohmann::json v = getPath(cur, comp, field);
        if (v.is_null()) {
            // 覆盖路径指向的组件/字段在场景里已经没有了（比如组件被删）。
            // 静默跳过会让"Apply 了但文件没变"变成一个谜，所以留痕。
            VK_LOG_WARN("PREFAB-APPLY-MISS path=%s (no such field in scene)",
                        path.c_str());
            continue;
        }
        setPath(asset.nodes[static_cast<std::size_t>(nodeIndex)].entity, comp,
                field, v);
        ++wrote;
    }

    const int c = clearInstanceOverrides(scene, instance);
    if (cleared) *cleared = c;
    VK_LOG_INFO("PREFAB-APPLY path=%s instance=%u wrote=%d cleared=%d",
                asset.path.c_str(), instance, wrote, c);
    return wrote;
}

} // namespace scene
