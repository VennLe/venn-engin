// ============================================================
// animation/ClipEdit.cpp
//
// 三件事：轨道编辑（打帧/删帧）、名字→下标解析、JSON 往返。
// 采样**不在这里** —— 全部转发给 assets::sampleTrack（见头文件约定 ③）。
// ============================================================

#include "animation/ClipEdit.h"

#include "animation/CurveBake.h"
#include "core/Logger.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>

namespace animation {

namespace {

using json = nlohmann::json;

// `pathName()` 现在只有一份，在 CurveBake.cpp（`animation::pathName`）——
// 序列化与曲线编辑器共用同一个字面表，省得"存的是 translation、曲线列表
// 里显示的是 Translation"这种对不上的事。这里只留反解。

bool pathFromName(const std::string& s, assets::AnimTrack::Path& out) {
    if (s == "translation" || s == "t" || s == "loc" || s == "0") {
        out = assets::AnimTrack::Translation;
        return true;
    }
    if (s == "rotation" || s == "r" || s == "rot" || s == "1") {
        out = assets::AnimTrack::Rotation;
        return true;
    }
    if (s == "scale" || s == "s" || s == "2") {
        out = assets::AnimTrack::Scale;
        return true;
    }
    return false;
}

json vec4ToJson(const glm::vec4& v) {
    return json::array({v.x, v.y, v.z, v.w});
}
json vec3ToJson(const glm::vec3& v) {
    return json::array({v.x, v.y, v.z});
}

// 读一个长度不足的数组时补默认值，而不是让整份资产打不开。
// 手改过的 JSON 里 `values:[[0,0,0]]`（少了 w）是常见笔误，静默补 w=1
// 比"加载失败"友好得多，而且对 T/S 完全等价（它们只用 xyz）。
glm::vec4 vec4FromJson(const json& j, const glm::vec4& def) {
    if (!j.is_array()) return def;
    glm::vec4 r = def;
    for (size_t i = 0; i < 4 && i < j.size(); ++i) {
        if (j[i].is_number()) r[static_cast<glm::length_t>(i)] =
            j[i].get<float>();
    }
    return r;
}
glm::vec3 vec3FromJson(const json& j, const glm::vec3& def) {
    if (!j.is_array()) return def;
    glm::vec3 r = def;
    for (size_t i = 0; i < 3 && i < j.size(); ++i) {
        if (j[i].is_number()) r[static_cast<glm::length_t>(i)] =
            j[i].get<float>();
    }
    return r;
}
glm::quat quatFromJson(const json& j) {
    const glm::vec4 v = vec4FromJson(j, glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
    return glm::quat(v.w, v.x, v.y, v.z);
}

// 名字→下标用的匹配标志。
//
// ⚠ **只用"精确 + 不分大小写"，不用包含匹配**（`kJointMatchDefault` 里带
//   `kJointMatchSubstring`）。存进来的是骨架自己的骨名，本来就该精确命中；
//   而包含匹配在换了骨架之后会**静默命中另一根骨** —— `hand_l` 找不到时
//   退化成"名字里含 hand_l 的第一根"，于是帧被搬到一根不相干的骨上，
//   画面歪了但日志干净。宁可进 `missing` 列表让用户看见。
constexpr unsigned kResolveFlags = assets::kJointMatchCaseInsensitive;

// ---- Dope Sheet 批量操作的内部工具 ----

// 重建一条轨道的帧表时用的中间项。`sel` 只影响"同刻合并谁活下来"，
// 不进数据。
struct KeyItem {
    float t = 0.0f;
    glm::vec4 v{0.0f};
    unsigned char type = 0;
    KeyEase ease;
    bool sel = false;
};

// 用 `items` 整体替换 `tr` 的帧表（升序 + 同刻合并）。
//
// 返回值：`items` 里每一项在**新表**中的下标；-1 = 被合并掉了。
// 合并规则：同一时刻（kKeyTimeEpsilon 内）只留一个，**选中的那个赢** ——
// 这也是 Blender 的行为（挪过去的帧盖掉原地的帧）。反过来（原地的赢）
// 会让"我明明拖过来了"变成静默无效，是最难查的一类。
std::vector<int> rebuildTrack(EditTrack& tr, std::vector<KeyItem>& items,
                              int& merged) {
    // 同一时刻下，未选中的排在前面 —— 后面那个（选中的）才会赢。
    std::stable_sort(items.begin(), items.end(),
                     [](const KeyItem& a, const KeyItem& b) {
                         if (a.t != b.t) return a.t < b.t;
                         return !a.sel && b.sel;
                     });

    std::vector<int> map(items.size(), -1);
    tr.times.clear();
    tr.values.clear();
    tr.types.clear();
    tr.ease.clear();
    tr.times.reserve(items.size());
    tr.values.reserve(items.size());

    std::vector<int> owner;   // 新表每一格来自 items 的哪一项
    for (std::size_t i = 0; i < items.size(); ++i) {
        const KeyItem& it = items[i];
        if (!tr.times.empty() &&
            std::fabs(it.t - tr.times.back()) <= kKeyTimeEpsilon) {
            ++merged;
            if (it.sel) {                    // 选中的赢
                tr.values.back() = it.v;
                tr.types.back() = it.type;
                tr.ease.back() = it.ease;
                owner.back() = static_cast<int>(i);
            }
            continue;
        }
        tr.times.push_back(it.t);
        tr.values.push_back(it.v);
        tr.types.push_back(it.type);
        tr.ease.push_back(it.ease);
        owner.push_back(static_cast<int>(i));
    }
    for (std::size_t k = 0; k < owner.size(); ++k)
        map[static_cast<std::size_t>(owner[k])] = static_cast<int>(k);

    // 类型表全默认就抹平 —— 存盘时只有非默认才写（见 saveEditClip），
    // 留着一条全 0 的表也不会错，但会让人误以为"这里有类型信息"。
    bool anyType = false;
    for (unsigned char t : tr.types) {
        if (t != static_cast<unsigned char>(KeyType::Key)) { anyType = true; break; }
    }
    if (!anyType) tr.types.clear();
    // 缓动表同理：全默认就抹平（存盘时只有非默认才写）
    tr.pruneDefaultEase();
    return map;
}

// 时间平移 / 镜像 / 缩放共用的骨架：按轨道分组 → 逐项算新时刻 →
// 重建 → 把新的下标写回 sel。
template <typename Fn>
BatchStats transformKeys(EditClip& clip, std::vector<KeyRef>& sel, Fn fn) {
    BatchStats st;
    // 组内保持 sel 的原次序（map 的 key 是有序的，所以按轨道下标升序处理）
    std::map<int, std::vector<int>> byTrack;
    for (const KeyRef& r : sel) {
        if (r.track < 0 || r.track >= static_cast<int>(clip.tracks.size())) {
            ++st.dropped;
            continue;
        }
        EditTrack& tr = clip.tracks[static_cast<std::size_t>(r.track)];
        if (r.key < 0 || r.key >= tr.keyCount()) {
            ++st.dropped;
            continue;
        }
        byTrack[r.track].push_back(r.key);
    }

    std::vector<KeyRef> next;
    float maxT = 0.0f;
    for (auto& kv : byTrack) {
        const int ti = kv.first;
        EditTrack& tr = clip.tracks[static_cast<std::size_t>(ti)];
        std::vector<int>& idx = kv.second;

        std::vector<KeyItem> items;
        items.reserve(static_cast<std::size_t>(tr.keyCount()));
        for (int i = 0; i < tr.keyCount(); ++i) {
            KeyItem it;
            it.v = tr.values[static_cast<std::size_t>(i)];
            it.type = static_cast<unsigned char>(tr.typeAt(i));
            it.ease = tr.easeAt(i);
            it.sel = (std::find(idx.begin(), idx.end(), i) != idx.end());
            it.t = it.sel ? fn(tr.times[static_cast<std::size_t>(i)])
                          : tr.times[static_cast<std::size_t>(i)];
            items.push_back(it);
        }

        const std::vector<int> map = rebuildTrack(tr, items, st.merged);
        // 新的下标写回：idx 里那一项在原 items 里的位置 == idx 里的值
        for (int oldKey : idx) {
            const int ni = map[static_cast<std::size_t>(oldKey)];
            if (ni >= 0) {
                next.push_back(KeyRef{ti, ni});
                ++st.moved;
            }
        }
        if (!tr.times.empty()) maxT = std::max(maxT, tr.times.back());
    }

    // duration 只增不减（缩到更短会把尾巴上的帧留在可见范围之外）
    if (st.moved > 0) clip.duration = std::max(clip.duration, maxT);
    sel = next;
    return st;
}

} // namespace

// ---------------------------------------------------------------- 类型名

const char* keyTypeName(KeyType t) {
    switch (t) {
        case KeyType::Extreme:   return "extreme";
        case KeyType::Breakdown: return "breakdown";
        case KeyType::Inbetween: return "inbetween";
        case KeyType::Key:       break;
    }
    return "key";
}

bool parseKeyType(const std::string& s, KeyType& out) {
    std::string low;
    low.reserve(s.size());
    for (char c : s) {
        if (c >= 'A' && c <= 'Z') low.push_back(static_cast<char>(c - 'A' + 'a'));
        else low.push_back(c);
    }
    if (low == "key" || low == "0" || low == "none" || low == "clear") {
        out = KeyType::Key;
    } else if (low == "extreme" || low == "1" || low == "e") {
        out = KeyType::Extreme;
    } else if (low == "breakdown" || low == "2" || low == "b" || low == "break") {
        out = KeyType::Breakdown;
    } else if (low == "inbetween" || low == "3" || low == "i" || low == "in") {
        out = KeyType::Inbetween;
    } else {
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- EditTrack

bool KeyEase::isDefault() const {
    // ⚠ 比的是**会进求值的字段**：Linear + None + Linear 时 `easeFn` 逐位
    //   返回 u，整条轨道等价于 `sampleTrack`（见 CurveBake.h 约定 ①）。
    //   `handle` 在 Linear 段上不参与求值，但它是作者按 V 存下的意图，
    //   所以也计入 —— 抹掉它等于"下次切到贝塞尔时手柄凭空复位"。
    return interp == Interp::Linear && ease == EaseMode::None &&
           shape == EaseShape::Linear && handle == HandleType::AutoClamped &&
           outHandle == glm::vec2(1.0f / 3.0f, 0.0f) &&
           inHandle == glm::vec2(-1.0f / 3.0f, 0.0f);
}

int EditTrack::findKey(float time) const {
    for (size_t i = 0; i < times.size(); ++i) {
        if (std::fabs(times[i] - time) <= kKeyTimeEpsilon)
            return static_cast<int>(i);
    }
    return -1;
}

KeyType EditTrack::typeAt(int i) const {
    // 越界 / 缺项 → 默认。见头文件里"允许比 times 短"的理由。
    if (i < 0 || static_cast<std::size_t>(i) >= types.size())
        return KeyType::Key;
    const unsigned char v = types[static_cast<std::size_t>(i)];
    if (v > static_cast<unsigned char>(KeyType::Inbetween)) return KeyType::Key;
    return static_cast<KeyType>(v);
}

void EditTrack::setTypeAt(int i, KeyType t) {
    if (i < 0 || i >= keyCount()) return;
    if (types.size() != times.size()) types.resize(times.size(), 0);
    types[static_cast<std::size_t>(i)] = static_cast<unsigned char>(t);
    // 全默认就清空（与 rebuildTrack 同一口径，存盘时不写这字段）
    bool any = false;
    for (unsigned char v : types) {
        if (v != static_cast<unsigned char>(KeyType::Key)) { any = true; break; }
    }
    if (!any) types.clear();
}

// ---- 缓动表（S-D）。口径与 types 完全一致：可短、全默认即抹平。----

const KeyEase& EditTrack::easeAt(int i) const {
    // ⚠ 返回的是**静态默认值**的引用：调用方只读，不要拿它当"这一键存在"。
    static const KeyEase kDef{};
    if (i < 0 || static_cast<std::size_t>(i) >= ease.size()) return kDef;
    return ease[static_cast<std::size_t>(i)];
}

void EditTrack::setEaseAt(int i, const KeyEase& e) {
    if (i < 0 || i >= keyCount()) return;
    if (ease.size() != times.size()) ease.resize(times.size(), KeyEase{});
    ease[static_cast<std::size_t>(i)] = e;
    pruneDefaultEase();
}

bool EditTrack::hasEasing() const {
    for (const KeyEase& e : ease) {
        if (!e.isDefault()) return true;
    }
    return false;
}

bool EditTrack::pruneDefaultEase() {
    if (ease.empty()) return false;
    if (hasEasing()) return false;
    ease.clear();
    return true;
}

// ---------------------------------------------------------------- EditClip

int EditClip::totalKeys() const {
    int n = 0;
    for (const auto& t : tracks) n += t.keyCount();
    return n;
}

int EditClip::findTrack(const std::string& joint,
                        assets::AnimTrack::Path p) const {
    for (size_t i = 0; i < tracks.size(); ++i) {
        if (tracks[i].path == p && tracks[i].joint == joint)
            return static_cast<int>(i);
    }
    return -1;
}

EditTrack* EditClip::track(const std::string& joint,
                           assets::AnimTrack::Path p) {
    const int i = findTrack(joint, p);
    return i < 0 ? nullptr : &tracks[static_cast<size_t>(i)];
}

const EditTrack* EditClip::track(const std::string& joint,
                                 assets::AnimTrack::Path p) const {
    const int i = findTrack(joint, p);
    return i < 0 ? nullptr : &tracks[static_cast<size_t>(i)];
}

std::vector<std::string> EditClip::keyedJoints() const {
    std::vector<std::string> out;
    for (const auto& t : tracks) {
        if (std::find(out.begin(), out.end(), t.joint) == out.end())
            out.push_back(t.joint);
    }
    return out;
}

bool EditClip::setKey(const std::string& joint, assets::AnimTrack::Path p,
                      float time, const glm::vec4& value, KeyType type) {
    if (joint.empty()) return false;
    EditTrack* tr = track(joint, p);
    if (!tr) {
        // 新建轨道。tracks 的顺序 = 首次打帧的顺序，UI 列表按它排。
        tracks.push_back(EditTrack{});
        tr = &tracks.back();
        tr->joint = joint;
        tr->path = p;
    }

    const int existing = tr->findKey(time);
    bool inserted = false;
    if (existing >= 0) {
        // 覆盖**不动类型**（见头文件）—— 类型是作者标的语义，不是值的附属。
        tr->values[static_cast<size_t>(existing)] = value;
    } else {
        // 按时间序插入。`upper_bound` 保证等时刻不重复插（等时刻已被
        // findKey 拦下，这里只是让比较函数与 findKey 的语义一致）。
        const auto it = std::upper_bound(tr->times.begin(), tr->times.end(), time);
        const size_t idx = static_cast<size_t>(it - tr->times.begin());
        tr->times.insert(tr->times.begin() + static_cast<long>(idx), time);
        tr->values.insert(tr->values.begin() + static_cast<long>(idx), value);
        // 类型表按需撑齐再插（撑齐时补 0 = Key，就是"老帧没有类型"）
        if (type != KeyType::Key) {
            if (tr->types.size() != tr->times.size() - 1)
                tr->types.resize(tr->times.size() - 1, 0);
            tr->types.insert(tr->types.begin() + static_cast<long>(idx),
                             static_cast<unsigned char>(type));
        } else if (!tr->types.empty()) {
            tr->types.insert(tr->types.begin() + static_cast<long>(idx), 0);
        }
        // 缓动表同理：表为空就别管（省得"打一帧就多一张全默认的表"），
        // 非空就按位置插一个默认项，否则后面的键会整体错位一格 ——
        // 那在画面上是"从这一帧起所有切线都挪到下一帧去了"。
        if (!tr->ease.empty()) {
            if (tr->ease.size() != tr->times.size() - 1)
                tr->ease.resize(tr->times.size() - 1, KeyEase{});
            tr->ease.insert(tr->ease.begin() + static_cast<long>(idx), KeyEase{});
        }
        inserted = true;
    }

    // duration 只会变长。理由见头文件。
    if (time > duration) duration = time;
    return inserted;
}

bool EditClip::removeKey(const std::string& joint, assets::AnimTrack::Path p,
                         int index) {
    const int ti = findTrack(joint, p);
    if (ti < 0) return false;
    EditTrack& tr = tracks[static_cast<size_t>(ti)];
    if (index < 0 || index >= tr.keyCount()) return false;
    tr.times.erase(tr.times.begin() + index);
    tr.values.erase(tr.values.begin() + index);
    if (!tr.types.empty() && static_cast<size_t>(index) < tr.types.size())
        tr.types.erase(tr.types.begin() + index);
    if (!tr.ease.empty() && static_cast<size_t>(index) < tr.ease.size())
        tr.ease.erase(tr.ease.begin() + index);
    tr.pruneDefaultEase();
    if (tr.times.empty()) tracks.erase(tracks.begin() + ti);
    return true;
}

int EditClip::removeJoint(const std::string& joint) {
    const size_t before = tracks.size();
    tracks.erase(std::remove_if(tracks.begin(), tracks.end(),
                                [&](const EditTrack& t) {
                                    return t.joint == joint;
                                }),
                 tracks.end());
    return static_cast<int>(before - tracks.size());
}

const EditPose* EditClip::findPose(const std::string& name) const {
    for (const auto& p : poses) {
        if (p.name == name) return &p;
    }
    return nullptr;
}

bool EditClip::storePose(const EditPose& p) {
    for (auto& e : poses) {
        if (e.name == p.name) {
            e = p;
            return false;   // 覆盖
        }
    }
    poses.push_back(p);
    return true;            // 新增
}

bool EditClip::removePose(const std::string& name) {
    const size_t before = poses.size();
    poses.erase(std::remove_if(poses.begin(), poses.end(),
                               [&](const EditPose& p) {
                                   return p.name == name;
                               }),
                poses.end());
    return poses.size() != before;
}

// ---------------------------------------------------------------- 批量编辑

bool selectedTimeRange(const EditClip& clip, const std::vector<KeyRef>& sel,
                       float& lo, float& hi) {
    bool any = false;
    for (const KeyRef& r : sel) {
        if (r.track < 0 || r.track >= static_cast<int>(clip.tracks.size())) continue;
        const EditTrack& tr = clip.tracks[static_cast<std::size_t>(r.track)];
        if (r.key < 0 || r.key >= tr.keyCount()) continue;
        const float t = tr.times[static_cast<std::size_t>(r.key)];
        if (!any) { lo = hi = t; any = true; }
        else { lo = std::min(lo, t); hi = std::max(hi, t); }
    }
    return any;
}

std::vector<KeyRef> allKeys(const EditClip& clip) {
    std::vector<KeyRef> out;
    for (std::size_t ti = 0; ti < clip.tracks.size(); ++ti) {
        const int n = clip.tracks[ti].keyCount();
        for (int k = 0; k < n; ++k)
            out.push_back(KeyRef{static_cast<int>(ti), k});
    }
    return out;
}

BatchStats moveKeys(EditClip& clip, std::vector<KeyRef>& sel, float delta) {
    BatchStats st;
    selectedTimeRange(clip, sel, st.lo, st.hi);
    if (delta == 0.0f) {
        // 0 不是错误（`G` 抓起来又原地松手），但也不该白跑一遍重建 ——
        // 重建会把"同刻容差内本该分开的帧"合并掉，白白丢数据。
        return st;
    }
    st = transformKeys(clip, sel, [delta](float t) { return t + delta; });
    selectedTimeRange(clip, sel, st.lo, st.hi);
    VK_LOG_INFO("DOPE-MOVE n=%d delta=%.6f merged=%d dropped=%d t=[%.4f,%.4f]",
                st.moved, static_cast<double>(delta), st.merged, st.dropped,
                static_cast<double>(st.lo), static_cast<double>(st.hi));
    if (st.merged > 0) {
        VK_LOG_WARN("DOPE-MERGE n=%d（有帧落到了同一时刻，被合并掉了）",
                    st.merged);
    }
    return st;
}

BatchStats mirrorKeys(EditClip& clip, std::vector<KeyRef>& sel, float pivot) {
    BatchStats st;
    selectedTimeRange(clip, sel, st.lo, st.hi);
    // ⚠ 用 double 算中间值再降回 float：这样"2·pivot − t"在 pivot 与 t
    //   都是可表示值时的结果与 Python 侧 `float32(2*pivot - t)` 逐位一致，
    //   验收 B 组才敢用 == 而不是容差。
    const double p = static_cast<double>(pivot);
    st = transformKeys(clip, sel, [p](float t) {
        return static_cast<float>(2.0 * p - static_cast<double>(t));
    });
    selectedTimeRange(clip, sel, st.lo, st.hi);
    VK_LOG_INFO("DOPE-MIRROR n=%d pivot=%.6f merged=%d t=[%.4f,%.4f]", st.moved,
                static_cast<double>(pivot), st.merged,
                static_cast<double>(st.lo), static_cast<double>(st.hi));
    return st;
}

BatchStats alignCycle(EditClip& clip, std::vector<KeyRef>& sel, float target) {
    BatchStats st;
    float lo = 0.0f, hi = 0.0f;
    if (!selectedTimeRange(clip, sel, lo, hi)) return st;
    st.lo = lo;
    st.hi = hi;
    const float span = hi - lo;
    // 只有一个帧（或全部同一时刻）时缩放没有意义：分母是 0，缩放比无穷。
    // 直接整体挪到 target 反而是作者要的（"这一帧就是循环点"）。
    if (span <= kKeyTimeEpsilon) {
        st = transformKeys(clip, sel,
                           [target, lo](float t) { return t + (target - lo); });
        st.scale = 1.0f;
        VK_LOG_INFO("DOPE-ALIGN n=%d target=%.6f span~0 -> 整体平移", st.moved,
                    static_cast<double>(target));
        return st;
    }
    const float scale = (target - lo) / span;
    st.scale = scale;
    // 末帧**走特判**，不参与乘法：`lo + (hi−lo)·scale` 的舍入可能差 1 ulp，
    // 而"对齐到 1.2 秒"这件事必须精确等于 1.2，否则循环点永远差一丁点。
    const bool scaleOk = std::fabs(scale) > 1.0e-6f && std::isfinite(scale);
    st = transformKeys(clip, sel, [lo, scale, hi, target, scaleOk](float t) {
        if (!scaleOk) return t;
        if (std::fabs(t - hi) <= kKeyTimeEpsilon) return target;
        return lo + (t - lo) * scale;
    });
    selectedTimeRange(clip, sel, st.lo, st.hi);
    VK_LOG_INFO("DOPE-ALIGN n=%d target=%.6f scale=%.6f first=%.6f last=%.6f",
                st.moved, static_cast<double>(target),
                static_cast<double>(scale), static_cast<double>(st.lo),
                static_cast<double>(st.hi));
    return st;
}

int setKeyTypes(EditClip& clip, const std::vector<KeyRef>& sel, KeyType type) {
    int n = 0;
    for (const KeyRef& r : sel) {
        if (r.track < 0 || r.track >= static_cast<int>(clip.tracks.size())) continue;
        EditTrack& tr = clip.tracks[static_cast<std::size_t>(r.track)];
        if (r.key < 0 || r.key >= tr.keyCount()) continue;
        tr.setTypeAt(r.key, type);
        ++n;
    }
    VK_LOG_INFO("DOPE-TYPE n=%d type=%s", n, keyTypeName(type));
    return n;
}

TypeCount countTypes(const EditClip& clip, const std::vector<KeyRef>& sel) {
    TypeCount c;
    for (const KeyRef& r : sel) {
        if (r.track < 0 || r.track >= static_cast<int>(clip.tracks.size())) continue;
        const EditTrack& tr = clip.tracks[static_cast<std::size_t>(r.track)];
        if (r.key < 0 || r.key >= tr.keyCount()) continue;
        switch (tr.typeAt(r.key)) {
            case KeyType::Extreme:   ++c.extreme; break;
            case KeyType::Breakdown: ++c.breakdown; break;
            case KeyType::Inbetween: ++c.inbetween; break;
            case KeyType::Key:       ++c.key; break;
        }
    }
    return c;
}

// ---------------------------------------------------------------- 解析

ResolveStats resolveEditTracks(const assets::Skeleton& skel,
                               const EditClip& clip,
                               std::vector<ResolvedTrack>& out) {
    ResolveStats st;
    out.clear();
    if (skel.jointCount() <= 0) return st;

    int easedChannels = 0;
    for (const auto& et : clip.tracks) {
        const int j = skel.findJoint(et.joint, kResolveFlags);
        if (j < 0) {
            if (std::find(st.missing.begin(), st.missing.end(), et.joint) ==
                st.missing.end()) {
                st.missing.push_back(et.joint);
                // 只报一次，且带名字 —— 这条日志是"为什么我的帧没生效"
                // 的唯一线索。
                VK_LOG_WARN("POSE-RESOLVE-MISS joint='%s' keys=%d "
                            "(骨架里没有这根骨，%d 个帧被跳过)",
                            et.joint.c_str(), et.keyCount(), et.keyCount());
            }
            st.droppedKeys += et.keyCount();
            continue;
        }
        ResolvedTrack rt;
        rt.track.joint = j;
        rt.track.path = et.path;
        rt.track.times = et.times;
        rt.track.values = et.values;
        rt.track.step = et.step;
        // S-D：带上缓动（没有缓动的轨道 `hasEase == false`，采样直接转发
        // `sampleTrack` —— 这就是"没进过曲线编辑器 = 逐位不变"）。
        bakeResolvedTrack(et, rt);
        if (rt.hasEase) ++easedChannels;
        out.push_back(std::move(rt));
        ++st.resolvedChannels;
    }

    // 不同关节数（按解析后的下标去重 —— 名字不同但映射到同一根骨时
    // 上层的"涉及几根骨"读数才和实际一致）
    std::vector<int> seen;
    for (const auto& t : out) {
        if (std::find(seen.begin(), seen.end(), t.track.joint) == seen.end())
            seen.push_back(t.track.joint);
    }
    st.resolvedJoints = static_cast<int>(seen.size());

    VK_LOG_INFO("POSE-RESOLVE clips=%d channels=%d joints=%d missing=%d "
                "droppedKeys=%d eased=%d",
                static_cast<int>(clip.tracks.size()), st.resolvedChannels,
                st.resolvedJoints, static_cast<int>(st.missing.size()),
                st.droppedKeys, easedChannels);
    return st;
}

// ---------------------------------------------------------------- 叠加

int overlayEditPose(const std::vector<ResolvedTrack>& resolved, float time,
                    Pose& pose) {
    // ⚠ 这里是"无编辑数据时逐位一致"的闸门：一个字节都不写。
    if (resolved.empty()) return 0;
    const int n = pose.count();
    if (n <= 0) return 0;

    int written = 0;
    for (const auto& rt : resolved) {
        const assets::AnimTrack& tr = rt.track;
        if (tr.joint < 0 || tr.joint >= n) continue;
        // 求值只走这两个入口（约定 ③ + S-D 的受控例外）：
        //   没缓动 → `sampleTrack`；有缓动 → 预烘的 `animation::Curve`。
        // 没有第三份插值。
        const glm::vec4 v = sampleResolvedTrack(rt, time);
        const size_t k = static_cast<size_t>(tr.joint);
        switch (tr.path) {
            case assets::AnimTrack::Translation:
                pose.translation[k] = glm::vec3(v.x, v.y, v.z);
                break;
            case assets::AnimTrack::Rotation:
                // 轨道值存 (x,y,z,w)，glm::quat 构造是 (w,x,y,z)。
                // 与 samplePose 里的换算**逐字符一致** —— 两处不一致的症状是
                // "编辑过的骨和没编辑过的骨旋转方向相反"。
                pose.rotation[k] = glm::quat(v.w, v.x, v.y, v.z);
                break;
            case assets::AnimTrack::Scale:
                pose.scale[k] = glm::vec3(v.x, v.y, v.z);
                break;
        }
        ++written;
    }
    return written;
}

int applyEditPose(const EditPose& p, const assets::Skeleton& skel, Pose& pose) {
    const int n = pose.count();
    if (n <= 0) return 0;
    int written = 0;
    for (const auto& b : p.bones) {
        const int j = skel.findJoint(b.joint, kResolveFlags);
        if (j < 0 || j >= n) continue;
        const size_t k = static_cast<size_t>(j);
        pose.translation[k] = b.translation;
        pose.rotation[k] = b.rotation;
        pose.scale[k] = b.scale;
        ++written;
    }
    VK_LOG_INFO("POSELIB-APPLY name='%s' bones=%d applied=%d",
                p.name.c_str(), static_cast<int>(p.bones.size()), written);
    return written;
}

EditPose capturePose(const Pose& pose, const assets::Skeleton& skel,
                     const std::string& name) {
    EditPose out;
    out.name = name;
    const int n = std::min(pose.count(), skel.jointCount());
    out.bones.reserve(static_cast<size_t>(n));
    for (int j = 0; j < n; ++j) {
        EditPose::Entry e;
        e.joint = skel.joints[static_cast<size_t>(j)].name;
        const size_t k = static_cast<size_t>(j);
        e.translation = pose.translation[k];
        e.rotation = pose.rotation[k];
        e.scale = pose.scale[k];
        out.bones.push_back(std::move(e));
    }
    return out;
}

// ---------------------------------------------------------------- JSON

bool saveEditClip(const std::string& path, const EditClip& clip,
                  std::string& err) {
    err.clear();
    if (path.empty()) {
        err = "路径为空";
        return false;
    }

    json doc;
    doc["format"] = "venn.animclip";
    doc["version"] = 1;
    doc["name"] = clip.name;
    doc["baseClip"] = clip.baseClip;
    doc["duration"] = clip.duration;
    doc["loop"] = clip.loop;

    json jt = json::array();
    for (const auto& t : clip.tracks) {
        json o;
        o["joint"] = t.joint;
        o["path"] = pathName(t.path);
        if (t.step) o["step"] = true;
        json times = json::array();
        for (float v : t.times) times.push_back(v);
        json values = json::array();
        for (const auto& v : t.values) values.push_back(vec4ToJson(v));
        o["times"] = std::move(times);
        o["values"] = std::move(values);
        // 关键帧类型（R8）。**只有非默认才写**：这样老资产重存一次还是
        // 逐字节的老样子，"加了个新字段"不会让所有文件在 diff 里翻腾。
        bool anyType = false;
        for (std::size_t i = 0; i < t.times.size(); ++i) {
            if (t.typeAt(static_cast<int>(i)) != KeyType::Key) { anyType = true; break; }
        }
        if (anyType) {
            json types = json::array();
            for (std::size_t i = 0; i < t.times.size(); ++i)
                types.push_back(static_cast<int>(t.typeAt(static_cast<int>(i))));
            o["types"] = std::move(types);
        }
        // 每键缓动（S-D）。**稀疏**写：只写非默认的键，带下标。
        // 全默认（绝大多数老资产、以及"只打了帧没进曲线编辑器"的轨道）
        // 一个字段都不出现 —— 与 types 同一口径，老文件重存还是老样子。
        {
            json arr = json::array();
            for (std::size_t i = 0; i < t.times.size(); ++i) {
                const KeyEase& e = t.easeAt(static_cast<int>(i));
                if (e.isDefault()) continue;
                json eo;
                eo["i"] = static_cast<int>(i);
                eo["interp"] = interpName(e.interp);
                eo["easeMode"] = easeModeName(e.ease);
                eo["shape"] = easeShapeName(e.shape);
                eo["handle"] = handleTypeName(e.handle);
                eo["out"] = json::array({e.outHandle.x, e.outHandle.y});
                eo["in"] = json::array({e.inHandle.x, e.inHandle.y});
                arr.push_back(std::move(eo));
            }
            if (!arr.empty()) o["keyEase"] = std::move(arr);
        }
        jt.push_back(std::move(o));
    }
    doc["tracks"] = std::move(jt);

    json jp = json::array();
    for (const auto& p : clip.poses) {
        json o;
        o["name"] = p.name;
        json bones = json::array();
        for (const auto& b : p.bones) {
            json bo;
            bo["joint"] = b.joint;
            bo["t"] = vec3ToJson(b.translation);
            // 与轨道值保持同一个约定：(x,y,z,w)
            bo["r"] = json::array({b.rotation.x, b.rotation.y, b.rotation.z,
                                   b.rotation.w});
            bo["s"] = vec3ToJson(b.scale);
            bones.push_back(std::move(bo));
        }
        o["bones"] = std::move(bones);
        jp.push_back(std::move(o));
    }
    if (!jp.empty()) doc["poses"] = std::move(jp);

    std::error_code ec;
    const std::filesystem::path fs(path);
    if (fs.has_parent_path()) {
        std::filesystem::create_directories(fs.parent_path(), ec);
        // 建目录失败不算致命（可能已存在），真正的判据是最后能不能写。
    }

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        err = "打不开文件写";
        return false;
    }
    const std::string text = doc.dump(2);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!f) {
        err = "写入失败";
        return false;
    }
    f.close();

    VK_LOG_INFO("POSE-SAVE path=%s tracks=%d keys=%d poses=%d bytes=%d",
                path.c_str(), static_cast<int>(clip.tracks.size()),
                clip.totalKeys(), static_cast<int>(clip.poses.size()),
                static_cast<int>(text.size()));
    return true;
}

bool loadEditClip(const std::string& path, EditClip& out, std::string& err) {
    err.clear();
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        err = "打不开文件";
        return false;
    }
    json doc;
    try {
        f >> doc;
    } catch (const std::exception& e) {
        err = std::string("JSON 解析失败: ") + e.what();
        return false;
    }
    if (!doc.is_object()) {
        err = "顶层不是对象";
        return false;
    }
    const std::string fmt = doc.value("format", std::string());
    if (fmt != "venn.animclip") {
        err = "不是 venn.animclip（format='" + fmt + "'）";
        return false;
    }
    const int ver = doc.value("version", 0);
    if (ver < 1 || ver > 1) {
        err = "版本不支持（version=" + std::to_string(ver) + "）";
        return false;
    }

    EditClip c;
    c.name = doc.value("name", std::string());
    c.baseClip = doc.value("baseClip", std::string());
    c.duration = doc.value("duration", 1.0f);
    c.loop = doc.value("loop", true);

    if (doc.contains("tracks") && doc["tracks"].is_array()) {
        for (const auto& o : doc["tracks"]) {
            if (!o.is_object()) continue;
            EditTrack t;
            t.joint = o.value("joint", std::string());
            if (t.joint.empty()) continue;
            assets::AnimTrack::Path p = assets::AnimTrack::Rotation;
            if (o.contains("path")) {
                if (o["path"].is_string()) {
                    if (!pathFromName(o["path"].get<std::string>(), p)) continue;
                } else if (o["path"].is_number_integer()) {
                    p = static_cast<assets::AnimTrack::Path>(
                        o["path"].get<int>());
                }
            }
            t.path = p;
            t.step = o.value("step", false);
            if (o.contains("times") && o["times"].is_array()) {
                for (const auto& v : o["times"]) {
                    if (v.is_number()) t.times.push_back(v.get<float>());
                }
            }
            if (o.contains("values") && o["values"].is_array()) {
                // 缺分量时的兜底值：T = 0、S = 1、R = 单位四元数。
                // 用错的兜底（比如 R 也补 0）会让根骨当场塌成一个点。
                glm::vec4 def(0.0f);
                if (p == assets::AnimTrack::Scale) {
                    def = glm::vec4(1.0f);
                } else if (p == assets::AnimTrack::Rotation) {
                    def = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
                }
                for (const auto& v : o["values"])
                    t.values.push_back(vec4FromJson(v, def));
            }
            // 类型表（R8）。**允许短、允许缺**，缺的一律当 Key（见头文件）。
            if (o.contains("types") && o["types"].is_array()) {
                for (const auto& v : o["types"]) {
                    int ti = 0;
                    if (v.is_number_integer()) ti = v.get<int>();
                    else if (v.is_string()) {
                        KeyType kt = KeyType::Key;
                        ti = parseKeyType(v.get<std::string>(), kt)
                                 ? static_cast<int>(kt) : 0;
                    }
                    if (ti < 0 || ti > static_cast<int>(KeyType::Inbetween)) ti = 0;
                    t.types.push_back(static_cast<unsigned char>(ti));
                }
            }
            // 长度不一致的轨道是坏数据（手改坏了 / 半截写入）。截齐而不是
            // 丢弃整条：能救回来的帧多半还有用。
            if (t.values.size() != t.times.size()) {
                const size_t n = std::min(t.values.size(), t.times.size());
                t.times.resize(n);
                t.values.resize(n);
            }
            if (t.types.size() > t.times.size()) t.types.resize(t.times.size());
            // 每键缓动（S-D）。稀疏表：`i` 越界 / 字段缺失 / 名字写错
            // 一律**落在默认值**上（= 逐位等价于没有缓动），不丢整条轨道。
            if (o.contains("keyEase") && o["keyEase"].is_array()) {
                t.ease.resize(t.times.size(), KeyEase{});
                for (const auto& eo : o["keyEase"]) {
                    if (!eo.is_object()) continue;
                    const int i = eo.value("i", -1);
                    if (i < 0 || static_cast<std::size_t>(i) >= t.times.size())
                        continue;
                    KeyEase& e = t.ease[static_cast<std::size_t>(i)];
                    Interp ip = Interp::Linear;
                    EaseMode em = EaseMode::None;
                    EaseShape sh = EaseShape::Linear;
                    HandleType ht = HandleType::AutoClamped;
                    if (eo.contains("interp") && eo["interp"].is_string())
                        parseInterp(eo["interp"].get<std::string>(), ip);
                    if (eo.contains("easeMode") && eo["easeMode"].is_string())
                        parseEaseMode(eo["easeMode"].get<std::string>(), em);
                    if (eo.contains("shape") && eo["shape"].is_string())
                        parseEaseShape(eo["shape"].get<std::string>(), sh);
                    if (eo.contains("handle") && eo["handle"].is_string())
                        parseHandleType(eo["handle"].get<std::string>(), ht);
                    e.interp = ip;
                    e.ease = em;
                    e.shape = sh;
                    e.handle = ht;
                    if (eo.contains("out") && eo["out"].is_array() &&
                        eo["out"].size() >= 2) {
                        e.outHandle.x = eo["out"][0].get<float>();
                        e.outHandle.y = eo["out"][1].get<float>();
                    }
                    if (eo.contains("in") && eo["in"].is_array() &&
                        eo["in"].size() >= 2) {
                        e.inHandle.x = eo["in"][0].get<float>();
                        e.inHandle.y = eo["in"][1].get<float>();
                    }
                }
                t.pruneDefaultEase();
            }
            if (t.times.empty()) continue;
            c.tracks.push_back(std::move(t));
        }
    }

    if (doc.contains("poses") && doc["poses"].is_array()) {
        for (const auto& o : doc["poses"]) {
            if (!o.is_object()) continue;
            EditPose p;
            p.name = o.value("name", std::string());
            if (p.name.empty()) continue;
            if (o.contains("bones") && o["bones"].is_array()) {
                for (const auto& bo : o["bones"]) {
                    if (!bo.is_object()) continue;
                    EditPose::Entry e;
                    e.joint = bo.value("joint", std::string());
                    if (e.joint.empty()) continue;
                    if (bo.contains("t"))
                        e.translation = vec3FromJson(bo["t"], glm::vec3(0.0f));
                    if (bo.contains("r")) e.rotation = quatFromJson(bo["r"]);
                    if (bo.contains("s"))
                        e.scale = vec3FromJson(bo["s"], glm::vec3(1.0f));
                    p.bones.push_back(std::move(e));
                }
            }
            c.poses.push_back(std::move(p));
        }
    }

    out = std::move(c);
    VK_LOG_INFO("POSE-LOAD path=%s name='%s' base='%s' tracks=%d keys=%d "
                "poses=%d duration=%.3f",
                path.c_str(), out.name.c_str(), out.baseClip.c_str(),
                static_cast<int>(out.tracks.size()), out.totalKeys(),
                static_cast<int>(out.poses.size()),
                static_cast<double>(out.duration));
    return true;
}

std::string clipNameFromPath(const std::string& path) {
    if (path.empty()) return std::string();
    std::string s = std::filesystem::path(path).filename().string();
    const char* kAnim = ".anim.json";
    const char* kJson = ".json";
    auto endsWith = [&](const char* suf) {
        const size_t n = std::string(suf).size();
        return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
    };
    if (endsWith(kAnim)) s.resize(s.size() - std::string(kAnim).size());
    else if (endsWith(kJson)) s.resize(s.size() - std::string(kJson).size());
    return s;
}

} // namespace animation
