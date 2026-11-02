#include "fx/AnimEventRouter.h"

#include "core/Logger.h"
#include "fx/EffectService.h"
#include "scene/Scene.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace fx {

namespace {

std::string lowerAscii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// `strtof` 而不是 `stof`：**不抛异常**。
// 一份写错的事件不该让整个 Play 掉进异常路径（而且在 `tick` 的循环里
// 抛出去会跳过这一帧剩下的所有事件）。
bool parseFloatStrict(const std::string& text, float& out) {
    const std::string t = trim(text);
    if (t.empty()) return false;
    char* end = nullptr;
    const float v = std::strtof(t.c_str(), &end);
    if (end == t.c_str()) return false;
    while (*end != '\0' && std::isspace(static_cast<unsigned char>(*end))) ++end;
    if (*end != '\0') return false;
    out = v;
    return true;
}

} // namespace

AnimEventAction parseAnimEventAction(const animation::FiredAnimEvent& ev) {
    AnimEventAction a;

    std::string kind = trim(ev.kind);
    std::string arg = trim(ev.payload);

    // 兜底写法：kind 为空、payload 形如 "Sound|xxx" → 拆开。
    if (kind.empty()) {
        const std::size_t bar = arg.find('|');
        if (bar != std::string::npos) {
            kind = trim(arg.substr(0, bar));
            arg = trim(arg.substr(bar + 1));
        }
    }

    const std::string k = lowerAscii(kind);
    if (k.empty()) {
        a.reason = "empty kind";
        return a;
    }

    // `Hit` 是**文档口径**（`docs/gameplay_fx_readiness.md` G3 写的是
    // 作者在事件表里填 `Hit|fx/sword_hit`）。它也归到 Effect —— 理由：
    //   · `.veffect` 本身就是**打包**（粒子 + 音效 + 震屏 + 闪白 + 顿帧五件套），
    //     所以"命中"这一个事件就能驱动全套反馈，不需要作者填五行；
    //   · 作者脑子里的词是"命中"，不是"特效"。要求他背 `Fx` 这个内部枚举名
    //     的下场是他写 `Hit`、引擎静默忽略 —— 而**忽略了不报错**，
    //     表现成"事件表填了但什么都没发生"。
    //   `attack` / `impact` 同理（都是"打到了"的近义词）。
    if (k == "fx" || k == "effect" || k == "hit" || k == "impact" ||
        k == "attack" || k == "hitfx") {
        if (arg.empty()) {
            a.reason = "empty effect path";
            return a;
        }
        a.kind = AnimEventAction::Kind::Effect;
        a.path = arg;
        return a;
    }
    if (k == "sound" || k == "sfx" || k == "audio") {
        if (arg.empty()) {
            a.reason = "empty sound path";
            return a;
        }
        a.kind = AnimEventAction::Kind::Sound;
        a.path = arg;
        return a;
    }
    if (k == "shake" || k == "camerashake") {
        float v = 0.0f;
        if (!parseFloatStrict(arg, v)) {
            a.reason = "shake payload is not a number: '" + arg + "'";
            return a;
        }
        a.kind = AnimEventAction::Kind::Shake;
        a.value = std::clamp(v, 0.0f, 1.0f);
        return a;
    }
    if (k == "hitstop" || k == "freeze") {
        float v = 0.0f;
        if (!parseFloatStrict(arg, v)) {
            a.reason = "hitstop payload is not a number: '" + arg + "'";
            return a;
        }
        // 0.5 秒以上就不是"顿帧"了，是卡顿（见 EffectAsset 的同一个夹法）。
        a.kind = AnimEventAction::Kind::Hitstop;
        a.value = std::clamp(v, 0.0f, 0.5f);
        return a;
    }

    a.reason = "unknown kind: '" + kind + "'";
    return a;
}

AnimEventStats dispatchAnimEvents(const std::vector<animation::FiredAnimEvent>& events,
                                  scene::Scene& scene, EffectService& fx,
                                  const EffectSinks& sinks) {
    AnimEventStats st;
    if (events.empty()) return st;

    for (const animation::FiredAnimEvent& ev : events) {
        ++st.total;
        const AnimEventAction a = parseAnimEventAction(ev);
        if (a.kind == AnimEventAction::Kind::None) {
            if (a.reason.rfind("unknown kind", 0) == 0) {
                ++st.ignored;
            } else {
                ++st.failed;
                // 只报一次每种错误，否则一段动画每帧都发会刷爆日志。
                static std::string lastReason;
                if (lastReason != a.reason) {
                    lastReason = a.reason;
                    VK_LOG_WARN("FX-EVENT ignored (%s:%s) -- %s",
                                ev.clip.c_str(), ev.kind.c_str(), a.reason.c_str());
                }
            }
            continue;
        }

        // 事件所属实体可能已经没了（脚本刚销毁它，动画事件还在这一帧里）。
        // ⚠⚠ `FiredAnimEvent::entity` **只有 id、没有 generation** ——
        //   自己拼 `Entity{id, 0}` 会永远 generation 不匹配（alive() 恒假），
        //   表现是"特效全部落在世界原点"。必须走 `World::handle(id)`
        //   让它把当前代际号填回来。
        ecs::Entity e = scene.world().handle(ev.entity);
        const bool alive = e.valid() && scene.world().alive(e);
        glm::vec3 pos(0.0f);
        glm::vec3 fwd(0.0f, 0.0f, 1.0f);
        if (alive) {
            const glm::mat4 w = scene.worldMatrix(e);
            pos = glm::vec3(w[3]);
            const glm::vec3 z(w[2]);
            if (glm::length(z) > 1.0e-6f) fwd = glm::normalize(z);
        }

        switch (a.kind) {
        case AnimEventAction::Kind::Effect: {
            EffectRequest r;
            r.asset = a.path;
            r.position = pos;
            r.forward = fwd;
            if (fx.play(r) != 0) {
                ++st.effects;
            } else {
                ++st.failed;   // 资产缺失 / 池满 —— 两种都会被 play 记进日志
            }
            break;
        }
        case AnimEventAction::Kind::Sound: {
            if (sinks.sound) {
                sinks.sound(a.path, 1.0f, 1.0f, pos);
                ++st.sounds;
            }
            break;
        }
        case AnimEventAction::Kind::Shake: {
            if (sinks.shake) {
                sinks.shake(a.value, pos, 12.0f);
                ++st.shakes;
            }
            break;
        }
        case AnimEventAction::Kind::Hitstop: {
            if (sinks.hitstop) {
                sinks.hitstop(a.value);
                ++st.hitstops;
            }
            break;
        }
        case AnimEventAction::Kind::None:
            break;
        }

        // 死了的实体也只算"尽力" —— 位置用了原点，但动作照样发生
        //（音效/震屏不该因为实体刚被销毁而静默）。
    }

    return st;
}

// ---------------------------------------------------------------- 自检

namespace {

int g_routerFail = 0;
int g_routerPass = 0;

void rCheck(const char* name, bool ok, const std::string& detail = {}) {
    if (ok) {
        ++g_routerPass;
        std::printf("FX-EVENT PASS %s\n", name);
    } else {
        ++g_routerFail;
        std::printf("FX-EVENT FAIL %s%s%s\n", name, detail.empty() ? "" : " -- ",
                    detail.c_str());
    }
}

animation::FiredAnimEvent mk(const char* kind, const char* payload) {
    animation::FiredAnimEvent e;
    e.entity = 1;
    e.clip = "test";
    e.time = 0.1f;
    e.kind = kind ? kind : "";
    e.payload = payload ? payload : "";
    return e;
}

} // namespace

int runAnimEventRouterSelfTest() {
    g_routerFail = 0;
    g_routerPass = 0;

    // R1 kind 驱动
    {
        const auto a = parseAnimEventAction(mk("Fx", "fx/slash_a.veffect"));
        rCheck("R1 fx-kind", a.kind == AnimEventAction::Kind::Effect &&
                             a.path == "fx/slash_a.veffect");
    }
    // R2 kind 大小写不敏感（作者写小写是最常见的手滑）
    {
        const auto a = parseAnimEventAction(mk("EFFECT", "fx/a.veffect"));
        rCheck("R2 case-insensitive", a.kind == AnimEventAction::Kind::Effect);
    }
    // R3 kind 为空时认 payload 里的 "Sound|xxx" 兜底写法
    {
        const auto a = parseAnimEventAction(mk("", "Sound|sfx/swing.wav"));
        rCheck("R3 payload-fallback", a.kind == AnimEventAction::Kind::Sound &&
                                      a.path == "sfx/swing.wav");
    }
    // R4 震屏数值 + 夹取
    {
        const auto a = parseAnimEventAction(mk("Shake", "0.35"));
        rCheck("R4 shake-value", a.kind == AnimEventAction::Kind::Shake &&
                                 std::fabs(a.value - 0.35f) < 1e-4f);
        const auto b = parseAnimEventAction(mk("Shake", "7.0"));
        rCheck("R4 shake-clamped", b.kind == AnimEventAction::Kind::Shake &&
                                   b.value <= 1.0f);
    }
    // R5 参数坏 → 拒绝并给出原因（不抛异常、不静默当 0）
    {
        const auto a = parseAnimEventAction(mk("Shake", "abc"));
        rCheck("R5 bad-number-rejected", a.kind == AnimEventAction::Kind::None &&
                                         !a.reason.empty(), a.reason);
    }
    // R6 顿帧
    {
        const auto a = parseAnimEventAction(mk("Hitstop", "0.08"));
        rCheck("R6 hitstop", a.kind == AnimEventAction::Kind::Hitstop &&
                             std::fabs(a.value - 0.08f) < 1e-4f);
    }
    // R7 未知 kind 被忽略（不是错误，是"留给脚本"）
    {
        const auto a = parseAnimEventAction(mk("Footstep", "x"));
        rCheck("R7 unknown-ignored", a.kind == AnimEventAction::Kind::None &&
                                     a.reason.rfind("unknown kind", 0) == 0,
               a.reason);
    }
    // R8 空参数被拒（`Fx` 没给路径是最常见的笔误）
    {
        const auto a = parseAnimEventAction(mk("Fx", ""));
        rCheck("R8 empty-path-rejected", a.kind == AnimEventAction::Kind::None &&
                                         !a.reason.empty());
    }
    // R9 路径带空格时被 trim（从日志里拷路径常带上空格）
    {
        const auto a = parseAnimEventAction(mk("Fx", "  fx/b.veffect  "));
        rCheck("R9 trim-path", a.kind == AnimEventAction::Kind::Effect &&
                               a.path == "fx/b.veffect", a.path);
    }
    // R10 文档口径 `Hit|fx/sword_hit`（kind 空、payload 带竖线）必须能出特效。
    //     ⚠ 这条是"文档写了、实现没认"的护栏：作者照文档填，引擎静默忽略，
    //       症状是"事件表填了但什么都没发生"，而日志里一个错误都没有。
    {
        const auto a = parseAnimEventAction(mk("", "Hit|fx/sword_hit"));
        rCheck("R10 doc-form Hit|path", a.kind == AnimEventAction::Kind::Effect &&
                                        a.path == "fx/sword_hit", a.path);
    }

    std::printf("FX-EVENT-TOTAL pass=%d fail=%d\n", g_routerPass, g_routerFail);
    return g_routerFail;
}

} // namespace fx
