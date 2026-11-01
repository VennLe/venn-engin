// ============================================================
// animation/AnimController.cpp
// ============================================================

#include "animation/AnimController.h"

#include "animation/ClipSwitch.h"
#include "assets/AssetPath.h"
#include "assets/Skeleton.h"
#include "core/Logger.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace animation {

namespace {

using json = nlohmann::json;

// 字符串 → 枚举。**不做大小写兼容**，也不接受别名：
// 资产里的拼写错误应当当场报出来（校验期），而不是被"智能匹配"吃掉
// —— 后者会让 `">="` 与 `">"` 写混的人查一整天。
bool parseOp(const std::string& s, CondOp& out) {
    if (s == ">") { out = CondOp::Greater; return true; }
    if (s == ">=") { out = CondOp::GreaterEqual; return true; }
    if (s == "<") { out = CondOp::Less; return true; }
    if (s == "<=") { out = CondOp::LessEqual; return true; }
    if (s == "==") { out = CondOp::Equal; return true; }
    if (s == "!=") { out = CondOp::NotEqual; return true; }
    if (s == "trigger") { out = CondOp::Trigger; return true; }
    return false;
}

bool parseParamType(const std::string& s, ParamType& out) {
    if (s == "float") { out = ParamType::Float; return true; }
    if (s == "bool") { out = ParamType::Bool; return true; }
    if (s == "trigger") { out = ParamType::Trigger; return true; }
    return false;
}

// 单条条件求值。缺参数 → false（加载期已校验过，走到这里说明黑板被清了）。
bool evalCondition(const AnimController& ctl, const ecs::AnimatorComponent& an,
                   const TransitionCondition& c) {
    const int pi = ctl.findParam(c.param);
    if (pi < 0) return false;
    const ParamType t = ctl.params[static_cast<size_t>(pi)].type;

    if (t == ParamType::Trigger || c.op == CondOp::Trigger) {
        const auto it = an.triggerParams.find(c.param);
        return it != an.triggerParams.end() && it->second;
    }
    if (t == ParamType::Bool) {
        const auto it = an.boolParams.find(c.param);
        const bool v = (it != an.boolParams.end()) ? it->second : false;
        const bool want = c.value != 0.0f;
        if (c.op == CondOp::Equal) return v == want;
        if (c.op == CondOp::NotEqual) return v != want;
        return false;   // bool 上没有大小比较，加载期已拦下
    }

    float v = 0.0f;
    const auto it = an.floatParams.find(c.param);
    if (it != an.floatParams.end()) v = it->second;
    switch (c.op) {
        case CondOp::Greater:      return v > c.value;
        case CondOp::GreaterEqual: return v >= c.value;
        case CondOp::Less:         return v < c.value;
        case CondOp::LessEqual:    return v <= c.value;
        case CondOp::Equal:        return std::fabs(v - c.value) <= 1.0e-6f;
        case CondOp::NotEqual:     return std::fabs(v - c.value) > 1.0e-6f;
        case CondOp::Trigger:      return false;
    }
    return false;
}

// 校验。返回空串 = 通过；否则是"人话"的失败原因。
std::string validate(const AnimController& ctl) {
    if (ctl.states.empty()) return "states 为空";
    if (ctl.defaultState < 0 || ctl.defaultState >= static_cast<int>(ctl.states.size()))
        return "defaultState 指向不存在的状态";

    for (size_t i = 0; i < ctl.states.size(); ++i) {
        if (ctl.states[i].name.empty())
            return "第 " + std::to_string(i) + " 个 state 没有 name";
        if (ctl.states[i].clip.empty())
            return "state '" + ctl.states[i].name + "' 没有 clip";
        for (size_t j = i + 1; j < ctl.states.size(); ++j) {
            if (ctl.states[i].name == ctl.states[j].name)
                return "状态名重复: '" + ctl.states[i].name + "'";
        }
    }

    for (size_t i = 0; i < ctl.params.size(); ++i) {
        if (ctl.params[i].name.empty())
            return "第 " + std::to_string(i) + " 个 parameter 没有 name";
        for (size_t j = i + 1; j < ctl.params.size(); ++j) {
            if (ctl.params[i].name == ctl.params[j].name)
                return "参数名重复: '" + ctl.params[i].name + "'";
        }
    }

    for (size_t i = 0; i < ctl.transitions.size(); ++i) {
        const ControllerTransition& tr = ctl.transitions[i];
        const std::string tag = "第 " + std::to_string(i) + " 条 transition";
        if (tr.to < 0 || tr.to >= static_cast<int>(ctl.states.size()))
            return tag + " 的 to 指向不存在的状态";
        if (tr.from >= static_cast<int>(ctl.states.size()))
            return tag + " 的 from 指向不存在的状态";
        if (tr.from == tr.to)
            return tag + " 的 from 与 to 相同（'" +
                   ctl.states[static_cast<size_t>(tr.to)].name + "'）";
        for (const TransitionCondition& c : tr.conditions) {
            const int pi = ctl.findParam(c.param);
            if (pi < 0) return tag + " 引用了未声明的参数 '" + c.param + "'";
            const ParamType pt = ctl.params[static_cast<size_t>(pi)].type;
            if (pt != ParamType::Trigger && c.op == CondOp::Trigger)
                return tag + " 对非 trigger 参数 '" + c.param +
                       "' 用了 op=trigger";
            if (pt == ParamType::Trigger && c.op != CondOp::Trigger)
                return tag + " 对 trigger 参数 '" + c.param +
                       "' 用了比较运算符（只能是 trigger）";
            if (pt == ParamType::Bool && c.op != CondOp::Equal &&
                c.op != CondOp::NotEqual)
                return tag + " 对 bool 参数 '" + c.param +
                       "' 用了大小比较（只能是 == / !=）";
        }
    }
    return {};
}

} // namespace

const char* condOpName(CondOp op) {
    switch (op) {
        case CondOp::Greater: return ">";
        case CondOp::GreaterEqual: return ">=";
        case CondOp::Less: return "<";
        case CondOp::LessEqual: return "<=";
        case CondOp::Equal: return "==";
        case CondOp::NotEqual: return "!=";
        case CondOp::Trigger: return "trigger";
    }
    return "?";
}

const char* paramTypeName(ParamType t) {
    switch (t) {
        case ParamType::Float: return "float";
        case ParamType::Bool: return "bool";
        case ParamType::Trigger: return "trigger";
    }
    return "?";
}

int AnimController::findState(const std::string& name) const {
    for (size_t i = 0; i < states.size(); ++i)
        if (states[i].name == name) return static_cast<int>(i);
    return -1;
}

int AnimController::findParam(const std::string& name) const {
    for (size_t i = 0; i < params.size(); ++i)
        if (params[i].name == name) return static_cast<int>(i);
    return -1;
}

const char* AnimController::stateName(int idx) const {
    if (idx < 0 || idx >= static_cast<int>(states.size())) return "<none>";
    return states[static_cast<size_t>(idx)].name.c_str();
}

std::shared_ptr<AnimController> parseAnimController(const std::string& text) {
    auto ctl = std::make_shared<AnimController>();
    json j;
    try {
        j = json::parse(text);
    } catch (const std::exception& e) {
        ctl->error = std::string("JSON 解析失败: ") + e.what();
        return ctl;
    }
    if (!j.is_object()) {
        ctl->error = "顶层不是对象";
        return ctl;
    }

    // ---- 参数 ----
    if (j.contains("parameters") && j["parameters"].is_array()) {
        for (const auto& p : j["parameters"]) {
            ControllerParam cp;
            cp.name = p.value("name", std::string());
            const std::string ts = p.value("type", std::string("float"));
            if (!parseParamType(ts, cp.type)) {
                ctl->error = "参数 '" + cp.name + "' 的 type 不认识: '" + ts +
                             "'（只能是 float / bool / trigger）";
                return ctl;
            }
            cp.defaultValue = p.value("default", 0.0f);
            ctl->params.push_back(cp);
        }
    }

    // ---- 状态 ----
    if (j.contains("states") && j["states"].is_array()) {
        for (const auto& s : j["states"]) {
            ControllerState cs;
            cs.name = s.value("name", std::string());
            cs.clip = s.value("clip", std::string());
            cs.speed = s.value("speed", 1.0f);
            cs.loop = s.value("loop", true);
            ctl->states.push_back(cs);
        }
    }

    // ---- 编辑器画布坐标（可选，纯显示用）----
    //
    // 缺失 / 类型不对时**全部留 (0,0)** 而不是报错：老资产文件没有这一段，
    // 而"图编辑器第一次打开时自动布点"是完全可接受的降级。
    // 反过来说，如果这里报错，一个手写的 .anim.json 在重新保存前就再也
    // 加载不了 —— 编辑器加的一个装饰字段不该有这种否决权。
    ctl->nodePos.assign(ctl->states.size(), {0.0f, 0.0f});
    if (j.contains("editor") && j["editor"].is_object()) {
        const auto& ed = j["editor"];
        if (ed.contains("nodes") && ed["nodes"].is_object()) {
            for (size_t i = 0; i < ctl->states.size(); ++i) {
                const auto it = ed["nodes"].find(ctl->states[i].name);
                if (it == ed["nodes"].end()) continue;
                if (!it->is_array() || it->size() < 2) continue;
                ctl->nodePos[i] = {(*it)[0].get<float>(),
                                   (*it)[1].get<float>()};
            }
        }
    }

    // ---- 转移 ----
    if (j.contains("transitions") && j["transitions"].is_array()) {
        for (const auto& t : j["transitions"]) {
            ControllerTransition ct;
            // from 缺省 = Any。**显式区分"没写"与"写了空串"**：没写才是 Any，
            // 写了空串是配置错误（由 validate 之外这里直接判为 Any 也行，
            // 但那样用户"想删一条边改成 Any"和"打错字"就没区别了）。
            if (t.contains("from")) {
                const std::string f = t.value("from", std::string());
                // `"from": "Any"` = 任意状态（schema 里的约定）。
                // ⚠ 这一句漏掉的话，"Any" 会被拿去 `findState` 找不到 → 整个
                //   控制器被判无效 → 状态机**完全不接管**，而报错信息是
                //   "from 'Any' 不是已声明的状态" —— 看起来像用户配错，
                //   实际是解析器不认识自己的 schema（2026-10-25 第一次跑
                //   自检时就是这条）。
                if (f == "Any") {
                    ct.from = -1;
                } else {
                    ct.from = ctl->findState(f);
                    if (ct.from < 0) {
                        ctl->error =
                            "transition 的 from '" + f + "' 不是已声明的状态";
                        return ctl;
                    }
                }
            }
            const std::string to = t.value("to", std::string());
            ct.to = ctl->findState(to);
            if (ct.to < 0) {
                ctl->error = "transition 的 to '" + to + "' 不是已声明的状态";
                return ctl;
            }
            ct.duration = t.value("duration", 0.15f);
            ct.hasExitTime = t.value("hasExitTime", false);
            ct.exitTime = t.value("exitTime", 0.0f);
            ct.restart = t.value("restart", true);
            if (t.contains("conditions") && t["conditions"].is_array()) {
                for (const auto& c : t["conditions"]) {
                    TransitionCondition cc;
                    cc.param = c.value("param", std::string());
                    const std::string ops = c.value("op", std::string(">"));
                    if (!parseOp(ops, cc.op)) {
                        ctl->error = "条件运算符不认识: '" + ops + "'";
                        return ctl;
                    }
                    // `value` 允许写 `true` / `false`（bool 参数的 `==` 用它
                    // 最自然）。nlohmann 的 `value("value", 0.0f)` 遇到 bool
                    // 会**抛类型错**，所以这里手判。
                    if (c.contains("value")) {
                        const auto& v = c["value"];
                        if (v.is_boolean())
                            cc.value = v.get<bool>() ? 1.0f : 0.0f;
                        else if (v.is_number())
                            cc.value = v.get<float>();
                        else {
                            ctl->error = "条件 '" + cc.param +
                                         "' 的 value 只能是数字或 true/false";
                            return ctl;
                        }
                    }
                    ct.conditions.push_back(cc);
                }
            }
            ctl->transitions.push_back(ct);
        }
    }

    // ---- defaultState ----
    if (j.contains("defaultState")) {
        const std::string ds = j.value("defaultState", std::string());
        ctl->defaultState = ctl->findState(ds);
        if (ctl->defaultState < 0) {
            // 不立刻返回 error：validate 会给出一句更准确的"states 为空"
            // 或"重复名"。这里只在 states 非空时补一条更具体的错。
            if (!ctl->states.empty()) {
                ctl->error = "defaultState '" + ds + "' 不是已声明的状态";
                return ctl;
            }
        }
    } else if (!ctl->states.empty()) {
        // 没写 = 用第一个。**不报错**：单状态控制器不必写这一行，
        // 而"必须写"只会让最小样例多一行噪音。
        ctl->defaultState = 0;
    }

    ctl->error = validate(*ctl);
    return ctl;
}

std::shared_ptr<AnimController> loadAnimController(const std::string& path) {
    auto ctl = std::make_shared<AnimController>();
    if (path.empty()) {
        ctl->error = "路径为空";
        return ctl;
    }
    const std::string abs = assets::resolveAssetPath(path);
    FILE* fp = std::fopen(abs.c_str(), "rb");
    if (!fp) {
        ctl->error = "打不开文件: " + abs;
        return ctl;
    }
    std::string text;
    char buf[4096];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), fp)) > 0) text.append(buf, n);
    std::fclose(fp);

    auto parsed = parseAnimController(text);
    if (!parsed->error.empty())
        parsed->error = abs + ": " + parsed->error;
    return parsed;
}

// ---------------------------------------------------------------- 序列化
//
// 写出去的东西必须能被上面的 parseAnimController 原样读回来 —— 这是一条
// **双向契约**，只写不读的字段（那三个浮点在编辑器里加过又忘了解析）会
// 让"保存 → 重开 → 值变回去"变成一类查不到源头的怪 bug。
//
// 两个刻意的取舍：
//   ① **不排序**。转移的优先级是数组顺序（文件头第 3 条），排序会静默
//      改掉 "Any → Jump 先于 Idle → Walk" 这种刻意安排。
//   ② 浮点先量化到 1e-6 再写。`0.15f` 的精确值是 0.1500000059604645，
//      直接交给 nlohmann 会写出 17 位小数，人看着像被谁改过。
namespace {

double f6(float v) { return std::round(static_cast<double>(v) * 1e6) / 1e6; }

}  // namespace

std::string serializeAnimController(const AnimController& ctl) {
    json j;
    j["version"] = 1;

    // defaultState 按**名字**写（schema 用的是名字，不是下标）。下标
    // 越界时写空串 —— 这样重新加载会落到 states[0]，比留一个错名字好查：
    // 错名字会撞上"引用了未知状态"的校验，而用户的意图多半就是第一个。
    j["defaultState"] = ctl.defaultState >= 0
                            ? std::string(ctl.stateName(ctl.defaultState))
                            : std::string();

    json jp = json::array();
    for (const ControllerParam& p : ctl.params) {
        json e;
        e["name"] = p.name;
        e["type"] = paramTypeName(p.type);
        // trigger 没有默认值（恒为 false），写出来反而让人以为可以设。
        if (p.type == ParamType::Float) e["default"] = f6(p.defaultValue);
        else if (p.type == ParamType::Bool) e["default"] = p.defaultValue != 0.0f;
        jp.push_back(std::move(e));
    }
    j["parameters"] = std::move(jp);

    json js = json::array();
    for (const ControllerState& s : ctl.states) {
        json e;
        e["name"] = s.name;
        e["clip"] = s.clip;
        e["speed"] = f6(s.speed);
        e["loop"] = s.loop;
        js.push_back(std::move(e));
    }
    j["states"] = std::move(js);

    json jt = json::array();
    for (const ControllerTransition& t : ctl.transitions) {
        json e;
        e["from"] = t.from < 0 ? std::string("Any")
                               : std::string(ctl.stateName(t.from));
        e["to"] = std::string(ctl.stateName(t.to));
        e["duration"] = f6(t.duration);
        // 只有非默认值才写 —— hasExitTime=false / exitTime=0 / restart=true
        // 是三份占位噪音，一份状态机里 8 条边就是 24 行垃圾。
        if (t.hasExitTime) {
            e["hasExitTime"] = true;
            e["exitTime"] = f6(t.exitTime);
        }
        if (!t.restart) e["restart"] = false;

        json jc = json::array();
        for (const TransitionCondition& c : t.conditions) {
            json ce;
            ce["param"] = c.param;
            ce["op"] = condOpName(c.op);
            if (c.op != CondOp::Trigger) ce["value"] = f6(c.value);
            jc.push_back(std::move(ce));
        }
        if (!jc.empty()) e["conditions"] = std::move(jc);
        jt.push_back(std::move(e));
    }
    j["transitions"] = std::move(jt);

    // 画布坐标。按名字存（删一个状态不会让别的错位）。
    // 只有**每个**状态都有坐标时才写，否则整段省略、由编辑器重排 ——
    // 半份坐标比没有更难解释（一半节点在原位、一半堆在原点）。
    if (ctl.nodePos.size() == ctl.states.size()) {
        json nodes = json::object();
        bool any = false;
        for (size_t i = 0; i < ctl.states.size(); ++i) {
            const auto& p = ctl.nodePos[i];
            nodes[ctl.states[i].name] = json::array({f6(p[0]), f6(p[1])});
            if (p[0] != 0.0f || p[1] != 0.0f) any = true;
        }
        if (any) j["editor"] = json{{"nodes", std::move(nodes)}};
    }

    return j.dump(2) + "\n";
}

std::string saveAnimController(const AnimController& ctl,
                               const std::string& path) {
    if (path.empty()) return "路径为空";
    const std::string abs = assets::resolveAssetPath(path);
    FILE* fp = std::fopen(abs.c_str(), "wb");
    if (!fp) return "写不了文件: " + abs;
    const std::string text = serializeAnimController(ctl);
    const size_t n = std::fwrite(text.data(), 1, text.size(), fp);
    std::fclose(fp);
    if (n != text.size()) return "写入不完整（" + std::to_string(n) + "/" +
                                     std::to_string(text.size()) + "）: " + abs;
    return std::string();
}

void initControllerParams(const AnimController& ctl,
                          ecs::AnimatorComponent& an) {    for (const ControllerParam& p : ctl.params) {
        switch (p.type) {
            case ParamType::Float:
                // **只填缺失的**：脚本可能已经在 setFloat 里写过值了，
                // 无条件覆盖会把脚本的初值抹掉（且时序敏感、极难查）。
                if (an.floatParams.find(p.name) == an.floatParams.end())
                    an.floatParams[p.name] = p.defaultValue;
                break;
            case ParamType::Bool:
                if (an.boolParams.find(p.name) == an.boolParams.end())
                    an.boolParams[p.name] = (p.defaultValue != 0.0f);
                break;
            case ParamType::Trigger:
                // trigger 的初值恒为 false，**不看 default** —— trigger 是
                // "一次性事件"，初始就是"还没发生"。
                if (an.triggerParams.find(p.name) == an.triggerParams.end())
                    an.triggerParams[p.name] = false;
                break;
        }
    }
}

int stepAnimController(const AnimController& ctl, ecs::AnimatorComponent& an,
                       ecs::SkinnedMeshComponent& sk,
                       const assets::Skeleton& skel, float dt, const char* tag) {
    if (!ctl.valid()) return 0;
    const char* t = (tag && tag[0]) ? tag : "ANIM";

    if (!an.paramsInitialized) {
        an.paramsInitialized = true;
        initControllerParams(ctl, an);
    }

    const int nStates = static_cast<int>(ctl.states.size());

    // ---- ① 初始化（或状态表被热改过导致越界）----
    // 硬切：从"还没有状态"到"第一个状态"没有可淡入的旧 clip，
    // 而 defaultClip 可能已经播了一会儿 —— 此时 blend 只会让它抖一下。
    if (an.stateIndex < 0 || an.stateIndex >= nStates) {
        an.stateIndex = ctl.defaultState;
        an.stateTime = 0.0f;
        const ControllerState& st = ctl.states[static_cast<size_t>(an.stateIndex)];
        switchClip(sk, &an, st.clip, true, 0.0f);
        setClipPlaying(sk, &an, true);
        setClipLoop(sk, &an, st.loop);
        an.speed = st.speed;
        VK_LOG_INFO("%s-CTL-INIT state=%s clip=%s states=%d params=%d "
                    "transitions=%d",
                    t, st.name.c_str(), st.clip.c_str(), nStates,
                    static_cast<int>(ctl.params.size()),
                    static_cast<int>(ctl.transitions.size()));
    }

    // ---- ② dt == 0：不推进、不求值 ----
    // 编辑器的语义是"停在当前这一刻"。但**初始化要在上面已经做完** ——
    // 否则用户打开场景看到的是被 defaultClip 定住的姿势，而状态机
    // 明明配了 Idle（症状：Inspector 显示 state=Idle，视口却是 Walk）。
    if (dt <= 0.0f) return 0;

    an.stateTime += dt;

    // ---- ③ 求值转移：数组顺序 = 优先级，命中即 break ----
    const ControllerState& cur = ctl.states[static_cast<size_t>(an.stateIndex)];
    const assets::AnimationClip* curClip =
        cur.clip.empty() ? nullptr : skel.findClip(cur.clip);
    const float curDur = curClip ? curClip->duration : 0.0f;

    for (const ControllerTransition& tr : ctl.transitions) {
        if (tr.from >= 0 && tr.from != an.stateIndex) continue;
        if (tr.to < 0 || tr.to >= nStates) continue;

        // exitTime：不可循环 + 未到点 → 不许走。
        // `curDur <= 0` 时**放行**（零长 clip 没有"时刻"概念，卡死的风险
        // 比误放行更严重）。
        if (tr.hasExitTime && curDur > 0.0f) {
            const float need = std::clamp(tr.exitTime, 0.0f, 1.0f) * curDur;
            if (an.stateTime < need) continue;
        }

        bool ok = true;
        for (const TransitionCondition& c : tr.conditions) {
            if (!evalCondition(ctl, an, c)) { ok = false; break; }
        }
        if (!ok) continue;

        const ControllerState& to = ctl.states[static_cast<size_t>(tr.to)];

        // ⚠ 目标 clip 不在骨架里 → **不采纳**这条转移，并且只喊一次。
        //   采纳的话 `findClip` 返回 nullptr → samplePose 静默退回 rest，
        //   画面上是 T-pose（比"不切换、保持原动作"糟得多）。
        if (!skel.findClip(to.clip)) {
            const unsigned bit =
                (static_cast<unsigned>(tr.to) < 31u)
                    ? (1u << static_cast<unsigned>(tr.to))
                    : 0x80000000u;
            if ((an.ctlWarnMask & bit) == 0u) {
                an.ctlWarnMask |= bit;
                VK_LOG_WARN("%s-CTL-CLIP-MISSING state=%s clip='%s' "
                            "不在骨架里（共 %d 段）→ 该转移被忽略",
                            t, to.name.c_str(), to.clip.c_str(),
                            static_cast<int>(skel.clips.size()));
            }
            continue;
        }

        // ---- 采纳 ----
        const int fromIdx = an.stateIndex;
        const float oldTime = an.time;
        const float oldDur = curDur;

        switchClip(sk, &an, to.clip, tr.restart, tr.duration);
        setClipPlaying(sk, &an, true);
        setClipLoop(sk, &an, to.loop);
        an.speed = to.speed;
        an.stateIndex = tr.to;
        an.stateTime = 0.0f;

        // 相位连续（restart=false）：按**归一化相位**换算播放头。
        // 直接保留秒数在 walk(1.0s) → run(0.6s) 时会越界，run 就总是从
        // 第 0.4 秒开始播 —— 看起来像"跑步从半步开始"。
        if (!tr.restart) {
            const assets::AnimationClip* newClip = skel.findClip(to.clip);
            const float newDur = newClip ? newClip->duration : 0.0f;
            if (oldDur > 0.0f && newDur > 0.0f) {
                const float phase = oldTime / oldDur;   // 已在 [0,1) 区间内
                float nt = phase * newDur;
                if (nt < 0.0f || nt >= newDur) nt = 0.0f;
                an.time = nt;
                sk.time = nt;
            }
        }

        // ---- 消费 trigger ----
        // 只清**这条转移读过**的 trigger，不清整张表：同帧另一条转移可能
        // 还要读别的 trigger（虽然本帧不会再采纳了，但下一帧要用）。
        for (const TransitionCondition& c : tr.conditions) {
            const int pi = ctl.findParam(c.param);
            if (pi < 0) continue;
            if (ctl.params[static_cast<size_t>(pi)].type != ParamType::Trigger)
                continue;
            auto it = an.triggerParams.find(c.param);
            if (it != an.triggerParams.end() && it->second) {
                it->second = false;
                VK_LOG_INFO("%s-TRIGGER name=%s consumed=1", t,
                            c.param.c_str());
            }
        }

        VK_LOG_INFO("%s-STATE from=%s to=%s t=%.3f dur=%.2f",
                    t, ctl.stateName(fromIdx), to.name.c_str(),
                    static_cast<double>(an.stateTime),
                    static_cast<double>(tr.duration));
        return 1;
    }
    return 0;
}

} // namespace animation
