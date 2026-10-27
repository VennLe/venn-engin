#include "script/ScriptSelfTest.h"

#include "assets/AssetManager.h"
#include "assets/AssetPath.h"
#include "core/Logger.h"
#include "ecs/Components.h"
#include "scene/Scene.h"
#include "scene/SceneSerializer.h"
#include "script/ScriptRuntime.h"

#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace vks {
namespace {

int g_pass = 0;
int g_fail = 0;

std::string fmt(const char* f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    return std::string(buf);
}

void check(const char* name, bool ok, const std::string& detail) {
    if (ok) {
        ++g_pass;
        VK_LOG_INFO("SCRIPT-T pass %s %s", name, detail.c_str());
    } else {
        ++g_fail;
        VK_LOG_INFO("SCRIPT-T FAIL %s %s", name, detail.c_str());
    }
}

// ---------------------------------------------------------------- 夹具

std::filesystem::path scriptDir() {
    std::error_code ec;
    const auto p = std::filesystem::temp_directory_path(ec) /
                   "venn_script_selftest";
    std::filesystem::create_directories(p, ec);
    return p;
}

std::string writeScript(const std::string& name, const std::string& src) {
    const auto p = scriptDir() / name;
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(src.data(), static_cast<std::streamsize>(src.size()));
    out.close();
    return p.string();
}

// 宿主能力的**记录器**：脚本调了什么、调了几次、参数是什么，全留在字段里。
// ⚠ 这是 A6"兼容"唯一站得住的做法 —— 比"看日志有没有那行"强得多：
//   日志可能被格式化改掉，而"health 最终是 70"是行为事实。
struct MockHost {
    float health = 100.0f;
    int popups = 0;
    int stopAll = 0;
    int busMusic = 0;
    int busOther = 0;
    std::vector<std::string> sounds;
    int errorReports = 0;
    std::string lastError;

    // ---- S2：世界动作 ----
    ecs::Entity playerEntity{};
    std::vector<glm::vec3> bodySetPos;      // setPos 是否**同帧**写进了刚体
    ecs::Entity lastBodyEntity{};
    glm::vec3 bodyVel{0.0f};
    int setBodyVelCalls = 0;
    int impulseCalls = 0;
    glm::vec3 lastImpulse{0.0f};
    bool groundedValue = false;

    // ---- S3：输入 ----
    std::unordered_map<std::string, bool> pressedMap, heldMap, releasedMap;
    std::unordered_map<std::string, float> axisMap;

    // ---- C6：演出三件（记录器）----
    // `*Ok` 是宿主"能不能播"的回答：false = 资产不存在 / 没挂 Animator。
    // 真链路的成败由 `verify_director_api.py` 验（那边有真的资产和骨架）。
    std::vector<std::string> sequences, dialogues;
    std::vector<std::string> animClips;
    ecs::Entity lastAnimEntity{};
    bool playSequenceOk = true;
    bool showDialogueOk = true;
    bool playAnimOk = true;

    // ---- C6 第二批：世界控制（记录器）----
    // 旗标故意用**真的一张 map**而不是计数器：要验的是"写进去的能被读
    // 回来"，不是"调用发生了"。
    std::map<std::string, double> flags;
    int setFlagCalls = 0;
    bool setFlagOk = true;
    // spawn：`spawnEntity` 由用例自己先造好（真实体，所以脚本能接着
    // `setPos()` / `alive()` 用它）。默认无效 = "刷不出来"。
    std::vector<std::string> spawnPaths;
    std::vector<glm::vec3> spawnPositions;
    ecs::Entity spawnEntity{};
    // loadScene：只记请求，真切换由 verify_world_api.py 验（那边有真文件）
    std::vector<std::string> sceneRequests;
    bool loadSceneOk = true;
};

void installMock(ScriptRuntime& rt, MockHost& mh) {
    ScriptServices svc;
    svc.setHealth = [&mh](float v) { mh.health = v; };
    svc.damage = [&mh](float v) {
        mh.health -= v;
        ++mh.popups;
    };
    svc.popup = [&mh](float, float, float, float) { ++mh.popups; };
    svc.playSound = [&mh](const std::string& clip) { mh.sounds.push_back(clip); };
    svc.stopAllSounds = [&mh]() { ++mh.stopAll; };
    svc.setBusVolume = [&mh](const std::string& bus, float) {
        if (bus == "music") ++mh.busMusic;
        else ++mh.busOther;
    };
    // S2：世界动作。物理在这里是**记录器**而不是真物理世界 —— 要验的是
    // "脚本调了、参数对、发生在同一帧"，真刚体的那一半由 verify_script.py
    // 在 Play 态里验（那才是端到端）。
    svc.act.player = [&mh]() { return mh.playerEntity; };
    svc.act.setBodyPos = [&mh](ecs::Entity e, glm::vec3 p) {
        mh.lastBodyEntity = e;
        mh.bodySetPos.push_back(p);
    };
    svc.act.bodyVel = [&mh](ecs::Entity, glm::vec3* out) {
        if (!out) return false;
        *out = mh.bodyVel;
        return true;
    };
    svc.act.setBodyVel = [&mh](ecs::Entity, glm::vec3 v) {
        mh.bodyVel = v;
        ++mh.setBodyVelCalls;
    };
    svc.act.impulse = [&mh](ecs::Entity, glm::vec3 v) {
        mh.lastImpulse = v;
        ++mh.impulseCalls;
    };
    svc.act.grounded = [&mh](ecs::Entity) { return mh.groundedValue; };
    // S3 输入：查表，查不到 = false / 0（与真宿主"动作没绑定"一致）
    svc.act.pressed = [&mh](const std::string& a) {
        auto it = mh.pressedMap.find(a);
        return it != mh.pressedMap.end() && it->second;
    };
    svc.act.held = [&mh](const std::string& a) {
        auto it = mh.heldMap.find(a);
        return it != mh.heldMap.end() && it->second;
    };
    svc.act.released = [&mh](const std::string& a) {
        auto it = mh.releasedMap.find(a);
        return it != mh.releasedMap.end() && it->second;
    };
    svc.act.axis = [&mh](const std::string& a) {
        auto it = mh.axisMap.find(a);
        return it == mh.axisMap.end() ? 0.0f : it->second;
    };
    // C6 演出三件：记录 + 由 `*Ok` 决定"宿主说什么"。
    svc.act.playSequence = [&mh](const std::string& path) {
        mh.sequences.push_back(path);
        return mh.playSequenceOk;
    };
    svc.act.showDialogue = [&mh](const std::string& id) {
        mh.dialogues.push_back(id);
        return mh.showDialogueOk;
    };
    svc.act.playAnim = [&mh](ecs::Entity e, const std::string& clip) {
        mh.lastAnimEntity = e;
        mh.animClips.push_back(clip);
        return mh.playAnimOk;
    };
    // C6 第二批：旗标是真的存、spawn / loadScene 只记录。
    svc.act.setFlag = [&mh](const std::string& name, double v) {
        ++mh.setFlagCalls;
        if (mh.setFlagOk) mh.flags[name] = v;
        return mh.setFlagOk;
    };
    svc.act.getFlag = [&mh](const std::string& name) {
        auto it = mh.flags.find(name);
        return it == mh.flags.end() ? 0.0 : it->second;
    };
    svc.act.spawn = [&mh](const std::string& path, glm::vec3 pos) {
        mh.spawnPaths.push_back(path);
        mh.spawnPositions.push_back(pos);
        return mh.spawnEntity;
    };
    svc.act.loadScene = [&mh](const std::string& path) {
        mh.sceneRequests.push_back(path);
        return mh.loadSceneOk;
    };
    rt.setServices(std::move(svc));
    rt.setErrorSink([&mh](const ScriptInstance& inst, const std::string& msg) {
        ++mh.errorReports;
        mh.lastError = msg;
        (void)inst;
    });
}

ecs::Entity addScriptEntity(scene::Scene& sc, const std::string& name,
                            const std::string& path, bool enabled = true,
                            float timeScale = 1.0f) {
    const ecs::Entity e = sc.createObject(name);
    ecs::ScriptComponent c;
    c.path = path;
    c.enabled = enabled;
    c.timeScale = timeScale;
    sc.world().add<ecs::ScriptComponent>(e, c);
    return e;
}

double varNum(const ScriptInstance* inst, const char* key) {
    if (!inst) return -1e30;
    auto it = inst->vars.find(key);
    return it == inst->vars.end() ? -1e30 : it->second.asNum();
}

bool varBool(const ScriptInstance* inst, const char* key, bool fallback = false) {
    if (!inst) return fallback;
    auto it = inst->vars.find(key);
    return it == inst->vars.end() ? fallback : it->second.truthy();
}

std::string varStr(const ScriptInstance* inst, const char* key) {
    if (!inst) return "<no instance>";
    auto it = inst->vars.find(key);
    return it == inst->vars.end() ? std::string("<absent>") : it->second.toStr();
}

// 变量里的实体句柄（C17）。不是实体 / 不存在 → 无效句柄。
// ⚠ 不能用 `varNum` 顶替：`Value::asNum()` 对 Entity 走 default 分支返回 0
//   （见 Value 的注释："那正是最难查的那类 bug"）。
ecs::Entity varEntity(const ScriptInstance* inst, const char* key) {
    if (!inst) return ecs::Entity{};
    auto it = inst->vars.find(key);
    if (it == inst->vars.end() || it->second.k != Value::K::Entity)
        return ecs::Entity{};
    return it->second.e;
}

// 变量里的 vec3（C17）。不是 vec3 → 返回一个不可能的哨兵值，让断言自己红。
glm::vec3 varVec(const ScriptInstance* inst, const char* key) {
    if (!inst) return glm::vec3(-1e30f);
    auto it = inst->vars.find(key);
    if (it == inst->vars.end() || it->second.k != Value::K::Vec3)
        return glm::vec3(-1e30f);
    return it->second.v;
}

// 只编译、不求值。B 组的负向用例全靠它 —— 编译期检查的价值就在于
// "脚本根本不会跑起来"，所以断言对象必须是 **compile 的返回值**。
bool compiles(const std::string& src, std::string& errMsg, int* errLine) {
    ScriptProgram p;
    ScriptError err;
    const bool ok = p.compile(src, "selftest.vks", err);
    errMsg = err.toString();
    if (errLine) *errLine = err.line;
    return ok;
}

constexpr float kDt = 1.0f / 60.0f;

// 跑一个脚本文本 n 步，返回执行者实体（实例在 tick 里建）。
ecs::Entity runScript(scene::Scene& sc, ScriptRuntime& rt, const char* name,
                      const std::string& src, int steps) {
    const std::string p = writeScript((std::string(name) + ".vks").c_str(), src);
    const ecs::Entity e = addScriptEntity(sc, name, p);
    for (int i = 0; i < steps; ++i) {
        rt.tick(sc, ScriptRuntime::kSelfTimed, kDt, static_cast<uint64_t>(i));
    }
    return e;
}


// ---------------------------------------------------------------- A 组

// A1 ★ S0 的靶心：变量跨帧存活。
// ⚠ 为什么用 `sync()` 先建实例再注初值：S0 还没有 `let`（S-D2 在 S1），
//   而 `n = n + 1` 在第一帧读到未定义的 `n` 会报错 —— 这正是"今天必然失败"
//   的原因。测试注入初始值，把这个靶心单独隔离出来。
void testA1() {
    scene::Scene sc;
    const std::string p = writeScript("a1.vks", "n = n + 1\n");
    const ecs::Entity e = addScriptEntity(sc, "A1", p);

    ScriptRuntime rt;
    MockHost mh;
    installMock(rt, mh);
    rt.sync(sc);                                  // 建实例，不执行
    ScriptInstance* inst = rt.find(e);
    if (!inst) {
        check("A1 变量跨帧存活", false, "no_instance");
        return;
    }
    inst->vars["n"] = Value::number(0.0);
    for (int i = 0; i < 10; ++i) {
        rt.tick(sc, ScriptRuntime::kSelfTimed, kDt, static_cast<uint64_t>(i));
    }
    const double n = varNum(inst, "n");
    check("A1 变量跨帧存活", n == 10.0,
          fmt("n=%.1f expect=10 failed=%d err=%s", n,
              static_cast<int>(rt.lastStats().failed), mh.lastError.c_str()));
}

// A2 两个实体挂同一脚本 → 状态必须独立。
// ⚠ 同时证明"编译产物按路径共享"（engine().scriptCount()==1）。
void testA2() {
    scene::Scene sc;
    const std::string p = writeScript("a2.vks", "n = n + 1\n");
    const ecs::Entity e1 = addScriptEntity(sc, "A2a", p);
    const ecs::Entity e2 = addScriptEntity(sc, "A2b", p);

    ScriptRuntime rt;
    MockHost mh;
    installMock(rt, mh);
    rt.sync(sc);
    ScriptInstance* i1 = rt.find(e1);
    ScriptInstance* i2 = rt.find(e2);
    if (!i1 || !i2) {
        check("A2 两实例状态独立", false, "instances_missing");
        return;
    }
    i1->vars["n"] = Value::number(0.0);
    i2->vars["n"] = Value::number(2.0);
    for (int i = 0; i < 3; ++i) rt.tick(sc, ScriptRuntime::kSelfTimed, kDt, 0);

    const double n1 = varNum(rt.find(e1), "n");
    const double n2 = varNum(rt.find(e2), "n");
    check("A2 两实例状态独立", n1 == 3.0 && n2 == 5.0,
          fmt("n1=%.1f n2=%.1f expect=3/5", n1, n2));
    check("A2b 编译产物按路径共享",
          rt.engine().scriptCount() == 1 && rt.instanceCount() == 2,
          fmt("scripts=%zu instances=%u", rt.engine().scriptCount(),
              rt.instanceCount()));
}

// A3 ★ 熔断（P4）：死循环脚本第 1 步后失效，第 2 步**一步都不跑**，
//     并且错误只上报一次。
void testA3() {
    scene::Scene sc;
    const std::string p = writeScript("a3.vks", "while (1) { }\n");
    addScriptEntity(sc, "A3", p);

    ScriptRuntime rt;
    MockHost mh;
    installMock(rt, mh);

    rt.tick(sc, ScriptRuntime::kSelfTimed, kDt, 0);
    const auto& s1 = rt.lastStats();
    const uint64_t steps1 = s1.steps;
    const int executed1 = static_cast<int>(s1.executed);
    const int failed1 = static_cast<int>(s1.failed);

    rt.tick(sc, ScriptRuntime::kSelfTimed, kDt, 1);
    const auto& s2 = rt.lastStats();

    // 断言三件事，缺一不可：
    //   ① 第一次真的跑了并且失败（steps 很大 → 确实撞上了预算）
    //   ② 第二次**没执行**（failed 而不是 executed，steps 归零）
    //   ③ 错误只上报 1 次（否则每帧一条 WARN，日志被刷爆）
    check("A3 死循环熔断", executed1 == 0 && failed1 == 1 && steps1 >= 200000,
          fmt("exec=%d failed=%d steps=%llu", executed1, failed1,
              static_cast<unsigned long long>(steps1)));
    check("A3b 熔断后不再执行", s2.executed == 0 && s2.failed == 1 &&
                                    s2.steps == 0,
          fmt("exec=%u failed=%u steps=%llu", s2.executed, s2.failed,
              static_cast<unsigned long long>(s2.steps)));
    check("A3c 错误只上报一次", mh.errorReports == 1,
          fmt("reports=%d last='%s'", mh.errorReports, mh.lastError.c_str()));
}

// A4 ★ 本地时间（P3）：改 timeScale 不能让 `time` 倒退。
// 老实现 `time = t * ts`：t=0.8, ts 1.0→0.5 ⇒ time 从 0.800 掉到 0.450。
void testA4() {
    scene::Scene sc;
    const std::string p = writeScript("a4.vks", "lastTime = time\n");
    const ecs::Entity e = addScriptEntity(sc, "A4", p, true, 1.0f);

    ScriptRuntime rt;
    MockHost mh;
    installMock(rt, mh);
    rt.sync(sc);
    ScriptInstance* inst = rt.find(e);
    if (!inst) {
        check("A4 本地时间单调不减", false, "no_instance");
        return;
    }

    for (int i = 0; i < 8; ++i) rt.tick(sc, ScriptRuntime::kSelfTimed, kDt, 0);
    const double tBefore = inst->localTime;

    // 改倍速：从组件改（和用户在 Inspector 里拖是同一个数据源）
    if (auto* c = sc.world().get<ecs::ScriptComponent>(e)) c->timeScale = 0.5f;
    for (int i = 0; i < 8; ++i) rt.tick(sc, ScriptRuntime::kSelfTimed, kDt, 0);
    const double tAfter = inst->localTime;

    const double expectAfter = tBefore + 8.0 * kDt * 0.5;
    check("A4 本地时间单调不减", tAfter >= tBefore,
          fmt("before=%.4f after=%.4f", tBefore, tAfter));
    check("A4b 本地时间按新倍速累加",
          std::abs(tAfter - expectAfter) < 1e-6,
          fmt("after=%.4f expect=%.4f", tAfter, expectAfter));
    // 老实现会给出 8 * kDt * 0.5 = 0.0667（比 before 小一半），这里再钉一次
    check("A4c 不是 t_times_ts 语义", tAfter > 8.0 * kDt * 0.5 + 1e-6,
          fmt("after=%.4f legacy_would_be=%.4f", tAfter, 8.0 * kDt * 0.5));
}

// A5 ★ `simFrame` 是固定步号（P7），渲染帧号照旧在 `frame` 里。
void testA5() {
    scene::Scene sc;
    const std::string p = writeScript(
        "a5.vks", "lastSim = simFrame\nlastFrame = frame\n");
    const ecs::Entity e = addScriptEntity(sc, "A5", p);

    ScriptRuntime rt;
    MockHost mh;
    installMock(rt, mh);
    rt.sync(sc);
    ScriptInstance* inst = rt.find(e);
    if (!inst) {
        check("A5 simFrame 严格 +1", false, "no_instance");
        return;
    }

    // 故意让 renderFrame **不动**（模拟"一帧里跑两个定步"—— P7 的原始场景）
    for (int i = 0; i < 5; ++i) rt.tick(sc, ScriptRuntime::kSelfTimed, kDt, 60);
    const double sim = varNum(rt.find(e), "lastSim");
    const double rnd = varNum(rt.find(e), "lastFrame");

    check("A5 simFrame 严格 +1", sim == 5.0, fmt("simFrame=%.1f expect=5", sim));
    check("A5b frame 仍是渲染帧号", rnd == 60.0,
          fmt("frame=%.1f expect=60", rnd));
}

// A6 ★ 兼容：两个现成夹具的行为**逐位不变**。
//    hud_damage.vks  → 第 60 帧扣 30，随后 setHealth(70) 钉死 → health == 70
//    audio_play.vks  → 4 个音频动作各一次，且 `musik` 走"认不出"分支
void testA6() {
    const std::string hudPath =
        assets::resolveAssetPath("scenes/runtime_test/hud_damage.vks");
    const std::string audPath =
        assets::resolveAssetPath("scenes/runtime_test/audio_play.vks");

    {
        scene::Scene sc;
        addScriptEntity(sc, "HUD", hudPath);
        ScriptRuntime rt;
        MockHost mh;
        installMock(rt, mh);
        for (uint64_t f = 0; f <= 70; ++f) {
            rt.tick(sc, ScriptRuntime::kSelfTimed, kDt, f);
        }
        check("A6 hud_damage 兼容", std::abs(mh.health - 70.0f) < 1e-4f,
              fmt("health=%.3f expect=70 popups=%d", mh.health, mh.popups));
    }

    {
        scene::Scene sc;
        addScriptEntity(sc, "AUD", audPath);
        ScriptRuntime rt;
        MockHost mh;
        installMock(rt, mh);
        for (uint64_t f = 0; f <= 40; ++f) {
            rt.tick(sc, ScriptRuntime::kSelfTimed, kDt, f);
        }
        const bool clipOk = mh.sounds.size() == 1 &&
                            mh.sounds[0] == "audio/tone_1k_mono.wav";
        check("A6b audio_play 兼容",
              clipOk && mh.busMusic == 1 && mh.busOther == 1 &&
                  mh.stopAll == 1,
              fmt("sounds=%zu bus_ok=%d bus_bad=%d stop=%d", mh.sounds.size(),
                  mh.busMusic, mh.busOther, mh.stopAll));
    }
}

// A7 `reset()` 之后实例全部重建、vars 清空、simFrame 归零。
void testA7() {
    scene::Scene sc;
    const std::string p = writeScript("a7.vks", "n = n + 1\n");
    const ecs::Entity e = addScriptEntity(sc, "A7", p);

    ScriptRuntime rt;
    MockHost mh;
    installMock(rt, mh);
    rt.sync(sc);
    rt.find(e)->vars["n"] = Value::number(0.0);
    for (int i = 0; i < 4; ++i) rt.tick(sc, ScriptRuntime::kSelfTimed, kDt, 0);
    const double before = varNum(rt.find(e), "n");

    rt.reset();
    const uint32_t afterReset = rt.instanceCount();
    rt.sync(sc);
    const double afterSync = varNum(rt.find(e), "n");
    const uint64_t sim = rt.find(e) ? rt.find(e)->simFrames : 999;

    check("A7 reset 清空实例表", afterReset == 0,
          fmt("before_n=%.1f instances=%u", before, afterReset));
    check("A7b reset 后 vars 清空", afterSync < -1e29 && sim == 0,
          fmt("n=%s simFrame=%llu", afterSync < -1e29 ? "(absent)" : "present",
              static_cast<unsigned long long>(sim)));
}

// A8 换脚本 → 状态清零（与"热重载"是两回事：热重载保状态，换脚本不保）。
void testA8() {
    scene::Scene sc;
    const std::string p1 = writeScript("a8a.vks", "n = n + 1\n");
    const std::string p2 = writeScript("a8b.vks", "n = n + 10\n");
    const ecs::Entity e = addScriptEntity(sc, "A8", p1);

    ScriptRuntime rt;
    MockHost mh;
    installMock(rt, mh);
    rt.sync(sc);
    rt.find(e)->vars["n"] = Value::number(0.0);
    rt.tick(sc, ScriptRuntime::kSelfTimed, kDt, 0);
    const double n1 = varNum(rt.find(e), "n");

    sc.world().get<ecs::ScriptComponent>(e)->path = p2;
    rt.sync(sc);  // 换脚本 → 必须清状态（否则 n 会带着旧值进新逻辑）
    const double n2 = varNum(rt.find(e), "n");

    check("A8 换脚本清状态", n1 == 1.0 && n2 < -1e29,
          fmt("before=%.1f after=%s", n1, n2 < -1e29 ? "(absent)" : "present"));
}

// A9 disabled / 空路径的实体不执行，但要算进 total（面板要看得见它们）。
void testA9() {
    scene::Scene sc;
    const std::string p = writeScript("a9.vks", "n = n + 1\n");
    addScriptEntity(sc, "A9off", p, false);
    addScriptEntity(sc, "A9empty", "", true);

    ScriptRuntime rt;
    MockHost mh;
    installMock(rt, mh);
    rt.tick(sc, ScriptRuntime::kSelfTimed, kDt, 0);

    const auto& st = rt.lastStats();
    check("A9 未启用/空路径不执行",
          st.total == 2 && st.skipped == 2 && st.executed == 0,
          fmt("total=%u skipped=%u exec=%u", st.total, st.skipped, st.executed));
}

// A10 实体删掉 → 实例回收（否则实例表只增不减，Play 一次长一截）。
void testA10() {
    scene::Scene sc;
    const std::string p = writeScript("a10.vks", "n = n + 1\n");
    const ecs::Entity e = addScriptEntity(sc, "A10", p);

    ScriptRuntime rt;
    MockHost mh;
    installMock(rt, mh);
    rt.sync(sc);
    const uint32_t before = rt.instanceCount();
    sc.world().destroy(e);
    rt.sync(sc);
    check("A10 实体删除后实例回收",
          before == 1 && rt.instanceCount() == 0,
          fmt("before=%u after=%u", before, rt.instanceCount()));
}

// ============================================================ B 组（S1）
// 语言层：vec3 / entity / list / let / foreach / 编译期检查。

// B1 vec3 的构造、逐分量算术、标量缩放与成员访问。
void testB1() {
    scene::Scene sc;
    ScriptRuntime rt;
    MockHost mh;
    installMock(rt, mh);
    const ecs::Entity e = runScript(sc, rt, "B1", R"VKS(
let a = vec3(1, 2, 3)
let b = vec3(4, 5, 6)
let s = a + b
let d = b - a
let m = a * 2.0
let n = 2.0 * a
let w = a * b
let ax = a.x
let sum = s.x + s.y + s.z
let dsum = d.x + d.y + d.z
let msum = m.x + m.y + m.z
let nsum = n.x + n.y + n.z
let wsum = w.x + w.y + w.z
)VKS", 1);
    const ScriptInstance* inst = rt.find(e);
    check("B1 vec3 逐分量加法", varNum(inst, "sum") == 21.0,
          fmt("sum=%.1f expect=21 (5,7,9)", varNum(inst, "sum")));
    check("B1b vec3 减法", varNum(inst, "dsum") == 9.0,
          fmt("dsum=%.1f expect=9", varNum(inst, "dsum")));
    check("B1c vec3 标量缩放(两侧)", varNum(inst, "msum") == 12.0 &&
                                        varNum(inst, "nsum") == 12.0,
          fmt("msum=%.1f nsum=%.1f expect=12", varNum(inst, "msum"),
              varNum(inst, "nsum")));
    check("B1d vec3 逐分量乘法", varNum(inst, "wsum") == 32.0,
          fmt("wsum=%.1f expect=32 (4,10,18)", varNum(inst, "wsum")));
    check("B1e 成员访问 .x", varNum(inst, "ax") == 1.0,
          fmt("ax=%.1f expect=1", varNum(inst, "ax")));
}

// B2 `vec3 + num` 必须报**清晰**错误（不是静默变 NaN / 静默加分量）
void testB2() {
    scene::Scene sc;
    ScriptRuntime rt;
    MockHost mh;
    installMock(rt, mh);
    const ecs::Entity e = runScript(sc, rt, "B2",
                                    "let v = vec3(1, 0, 0) + 1\n", 1);
    const ScriptInstance* inst = rt.find(e);
    const bool ok = inst && inst->failed && inst->errorKind ==
                                     ScriptInstance::ErrorKind::Runtime &&
                    inst->lastError.find("cannot use '+'") != std::string::npos;
    check("B2 vec3 + number 报清晰错误", ok,
          inst ? inst->lastError : std::string("<no instance>"));
}

// B3 entity 往返 + 找不到时 exists()==false
void testB3() {
    scene::Scene sc;
    sc.createObject("Target");
    ScriptRuntime rt;
    MockHost mh;
    installMock(rt, mh);
    const ecs::Entity e = runScript(sc, rt, "B3", R"VKS(
let t = find("Target")
let nameOk = name(t) == "Target"
let missing = exists(find("Nope"))
let selfOk = exists(self())
)VKS", 1);
    const ScriptInstance* inst = rt.find(e);
    check("B3 find + name 往返", varBool(inst, "nameOk"),
          fmt("name=%s", varStr(inst, "nameOk").c_str()));
    check("B3b find 找不到 → exists 为 false", !varBool(inst, "missing", true),
          fmt("missing=%s", varStr(inst, "missing").c_str()));
    check("B3c self() 是活着的实体", varBool(inst, "selfOk"),
          fmt("selfOk=%s", varStr(inst, "selfOk").c_str()));
}

// B4 悬空：实体删掉之后，**旧句柄**的 exists() 必须变 false（generation 生效）
void testB4() {
    scene::Scene sc;
    const ecs::Entity victim = sc.createObject("Target");
    ScriptRuntime rt;
    MockHost mh;
    installMock(rt, mh);
    // ⚠ 只有 `let` 是幂等的；要"第二步重新问一次"必须用**普通赋值**，
    //   否则三句 let 在第二步全被跳过，断言测的就成了"变量没变"。
    const ecs::Entity e = runScript(sc, rt, "B4", R"VKS(
let h = find("Target")
let aliveBefore = exists(h)
let aliveAfter = false
aliveAfter = exists(h)
)VKS", 1);

    // 第 1 步：held 的 h 已经是"旧句柄"了，但实体还在 → 两个都是 true
    sc.world().destroy(victim);
    rt.tick(sc, ScriptRuntime::kSelfTimed, kDt, 1);

    const ScriptInstance* inst = rt.find(e);
    check("B4 删前 exists 为 true", varBool(inst, "aliveBefore"),
          fmt("before=%s", varStr(inst, "aliveBefore").c_str()));
    // `let h` 幂等 → 第二步 h 仍是**旧句柄**（正是要测的东西）；
    // `aliveAfter = exists(h)` 是普通赋值 → 会重新求值。
    check("B4b 删后 exists 为 false（generation 生效）",
          !varBool(inst, "aliveAfter", true),
          fmt("after=%s", varStr(inst, "aliveAfter").c_str()));
}

// B5 负向：拼错函数名 → **编译失败**，错误信息带行号（修 P5 的靶心）
void testB5() {
    std::string em;
    int el = 0;
    // 故意写错大小写：moveBy → moveby
    const bool ok = compiles("let a = 1\nmoveby(1, 2, 3)\n", em, &el);
    check("B5 拼错函数名编译失败",
          !ok && el == 2 && em.find("unknown function 'moveby'") != std::string::npos,
          fmt("ok=%d line=%d err='%s'", static_cast<int>(ok), el, em.c_str()));
}

// B6 负向：参数个数不对 → 编译失败
void testB6() {
    std::string em;
    int el = 0;
    const bool ok = compiles("sin()\n", em, &el);
    check("B6 参数个数不对编译失败",
          !ok && em.find("'sin' expects 1 argument(s), got 0") !=
                     std::string::npos,
          fmt("ok=%d err='%s'", static_cast<int>(ok), em.c_str()));
}

// B7 负向：`let` 只在顶层（S-D2）。S3 之前没有 `on` 块，用普通 `{}` 测 ——
// 判据完全相同（m_depth > 0）。
void testB7() {
    std::string em;
    int el = 0;
    const bool ok = compiles("if (1) { let x = 1 }\n", em, &el);
    check("B7 let 在块内编译失败",
          !ok && em.find("top level") != std::string::npos,
          fmt("ok=%d err='%s'", static_cast<int>(ok), em.c_str()));
}

// B8 foreach 遍历 list：顺序 = (距离, id)，连跑两次逐位相同（S-D11）
void testB8() {
    auto once = []() {
        scene::Scene sc;
        // 5 个目标排成一条线，距离 1..5
        for (int i = 0; i < 5; ++i) {
            const ecs::Entity t = sc.createObject("T" + std::to_string(i));
            if (auto* xf = sc.world().get<ecs::TransformComponent>(t)) {
                xf->position = glm::vec3(static_cast<float>(i + 1), 0.0f, 0.0f);
            }
        }
        ScriptRuntime rt;
        MockHost mh;
        installMock(rt, mh);
        const ecs::Entity e = runScript(sc, rt, "B8", R"VKS(
let hits = near(vec3(0, 0, 0), 100.0)
let n = count(hits)
let acc = ""
foreach (h in hits) {
    acc = acc + name(h) + ","
}
)VKS", 1);
        const ScriptInstance* inst = rt.find(e);
        return std::make_pair(static_cast<int>(varNum(inst, "n")),
                              varStr(inst, "acc"));
    };

    const auto r1 = once();
    const auto r2 = once();
    check("B8 foreach 命中 5 个且按距离排序",
          r1.first == 5 && r1.second == "T0,T1,T2,T3,T4,",
          fmt("count=%d acc='%s'", r1.first, r1.second.c_str()));
    check("B8b 连跑两次逐位相同", r1.second == r2.second,
          fmt("r1='%s' r2='%s'", r1.second.c_str(), r2.second.c_str()));
}

// B9 `let` 的幂等性：每帧跑不会把计数器重置回 0（热重载能留状态的前提）
void testB9() {
    scene::Scene sc;
    ScriptRuntime rt;
    MockHost mh;
    installMock(rt, mh);
    const ecs::Entity e = runScript(sc, rt, "B9", R"VKS(
let n = 0
n = n + 1
let flag = 0
flag = flag + 1
)VKS", 4);
    const ScriptInstance* inst = rt.find(e);
    check("B9 let 幂等（不每帧重置）", varNum(inst, "n") == 4.0,
          fmt("n=%.1f expect=4", varNum(inst, "n")));
}

// B10 foreach 的循环变量**遮蔽**外层同名变量（用完恢复）
void testB10() {
    scene::Scene sc;
    for (int i = 0; i < 3; ++i) sc.createObject("T" + std::to_string(i));
    ScriptRuntime rt;
    MockHost mh;
    installMock(rt, mh);
    const ecs::Entity e = runScript(sc, rt, "B10", R"VKS(
let h = find("B10")
let before = name(h)
foreach (h in near(vec3(0, 0, 0), 100.0)) {
}
let after = name(h)
)VKS", 1);
    const ScriptInstance* inst = rt.find(e);
    check("B10 foreach 变量遮蔽后恢复",
          varStr(inst, "after") == varStr(inst, "before") &&
              varStr(inst, "before") == "B10",
          fmt("before='%s' after='%s'", varStr(inst, "before").c_str(),
              varStr(inst, "after").c_str()));
}


// ============================================================ C 组（S2）
// 世界访问：查询 + 组件 + 黑板。这一组之前，脚本只能动**自己**。

// C1/C2 `setPos` 必须**同一帧**生效：渲染用的世界矩阵、物理刚体，都不能
// 等到下一帧。前者靠 "worldMatrix 是现算的"，后者靠注入的 setBodyPos。
void testC1() {
    scene::Scene sc;
    const ecs::Entity box = sc.createObject("Box");
    ScriptRuntime rt;
    MockHost mh;
    installMock(rt, mh);
    const ecs::Entity e = runScript(sc, rt, "C1", R"VKS(
let b = find("Box")
setPos(b, vec3(3, 4, 5))
let after = pos(b)
let asum = after.x + after.y + after.z
)VKS", 1);

    // ★ 同一帧：渲染器读的就是这个矩阵（worldMatrix 现算，不缓存）
    const glm::vec3 wp(sc.worldMatrix(box)[3]);
    const bool sameFrame = std::fabs(wp.x - 3.0f) < 1e-4f &&
                           std::fabs(wp.y - 4.0f) < 1e-4f &&
                           std::fabs(wp.z - 5.0f) < 1e-4f;
    check("C1 setPos 同帧改到渲染用的世界矩阵", sameFrame,
          fmt("world=(%.2f,%.2f,%.2f) expect=(3,4,5)", wp.x, wp.y, wp.z));

    const ScriptInstance* inst = rt.find(e);
    check("C1b 同帧 pos() 读回新位置", varNum(inst, "asum") == 12.0,
          fmt("asum=%.1f expect=12", varNum(inst, "asum")));

    // ★ C2：物理刚体同帧跟上（否则下一步刚体把 Transform 改回去）
    const bool bodyWritten = mh.bodySetPos.size() == 1 &&
                             std::fabs(mh.bodySetPos[0].x - 3.0f) < 1e-4f &&
                             mh.lastBodyEntity.id == box.id;
    check("C2 setPos 同帧写进刚体（不是下一帧）", bodyWritten,
          fmt("calls=%zu first=(%.2f,%.2f,%.2f) entity=%u/%u",
              mh.bodySetPos.size(),
              mh.bodySetPos.empty() ? 0.0f : mh.bodySetPos[0].x,
              mh.bodySetPos.empty() ? 0.0f : mh.bodySetPos[0].y,
              mh.bodySetPos.empty() ? 0.0f : mh.bodySetPos[0].z,
              mh.lastBodyEntity.id, box.id));
}

// C3 `near` 命中数 == 手工数出来的数，且按距离升序。
void testC3() {
    scene::Scene sc;
    // 6 个目标沿 x 轴排成 1..6 米；第 7 个在 30 米外（不该被查到）
    for (int i = 0; i < 6; ++i) {
        const ecs::Entity t = sc.createObject("T" + std::to_string(i));
        if (auto* xf = sc.world().get<ecs::TransformComponent>(t))
            xf->position = glm::vec3(static_cast<float>(i + 1), 0.0f, 0.0f);
    }
    if (auto* xf = sc.world().get<ecs::TransformComponent>(
            sc.createObject("Far")))
        xf->position = glm::vec3(30.0f, 0.0f, 0.0f);

    ScriptRuntime rt;
    MockHost mh;
    installMock(rt, mh);
    const ecs::Entity e = runScript(sc, rt, "C3", R"VKS(
let hits = near(vec3(0, 0, 0), 20.0)
let n = count(hits)
let acc = ""
let prev = 0.0
let asc = 1
let d = 0.0
foreach (h in hits) {
    acc = acc + name(h) + ","
    d = dist(pos(h), vec3(0, 0, 0))
    if (d < prev) { asc = 0 }
    prev = d
}
)VKS", 1);
    const ScriptInstance* inst = rt.find(e);
    // "手工数出来的数" = 6：脚本自己的实体在原点，被 near 排除（不含自己）
    check("C3 near 命中数正确且按距离升序",
          varNum(inst, "n") == 6.0 && varNum(inst, "asc") == 1.0 &&
              varStr(inst, "acc") == "T0,T1,T2,T3,T4,T5,",
          fmt("n=%.0f asc=%.0f acc='%s'", varNum(inst, "n"),
              varNum(inst, "asc"), varStr(inst, "acc").c_str()));
}

// C4 隐藏实体也查得到。这一条**必须**同时钉住"BVH 里确实没有它们" ——
// 否则自检场景里根本没有网格，BVH 空着也能"通过"，等于什么都没测。
void testC4() {
    scene::Scene sc;
    const ecs::Entity hidden = sc.createObject("Ghost");
    if (auto* xf = sc.world().get<ecs::TransformComponent>(hidden))
        xf->position = glm::vec3(2.0f, 0.0f, 0.0f);
    sc.world().add<ecs::VisibilityComponent>(hidden).visible = false;
    sc.createObject("Solid");

    ScriptRuntime rt;
    MockHost mh;
    installMock(rt, mh);
    const ecs::Entity e = runScript(sc, rt, "C4", R"VKS(
let hits = near(vec3(0, 0, 0), 20.0)
let n = count(hits)
let acc = ""
let seen = 0
foreach (h in hits) {
    acc = acc + name(h) + ","
    if (name(h) == "Ghost") { seen = 1 }
}
)VKS", 1);
    const ScriptInstance* inst = rt.find(e);
    // 索引规模：测试场景没有网格 → BVH 一个实体都索引不到。
    // 于是"C4 查得到"只可能来自补扫。
    const auto st = sc.spatialStats();
    check("C4 BVH 查不到时补扫兜住（隐藏实体也在结果里）",
          varNum(inst, "seen") == 1.0 && varNum(inst, "n") == 2.0,
          fmt("bvh_leaves=%zu n=%.0f acc='%s'", st.entities, varNum(inst, "n"),
              varStr(inst, "acc").c_str()));
}

// C5 `attr` 的默认值：没有时返回默认值**并且写回**。
void testC5() {
    scene::Scene sc;
    ScriptRuntime rt;
    MockHost mh;
    installMock(rt, mh);
    const ecs::Entity e = runScript(sc, rt, "C5", R"VKS(
let first = attr(self(), "hp", 100)
let second = attr(self(), "hp", 999)
let hasKey = 1
)VKS", 1);
    const ScriptInstance* inst = rt.find(e);
    // 第二次必须拿到 100（说明第一次写回了），而不是 999
    check("C5 attr 默认值写回（第二次读到的是上次写的）",
          varNum(inst, "first") == 100.0 && varNum(inst, "second") == 100.0,
          fmt("first=%.0f second=%.0f expect=100/100", varNum(inst, "first"),
              varNum(inst, "second")));

    const auto* sv = sc.world().get<ecs::ScriptVarsComponent>(e);
    check("C5b 黑板落在 ScriptVarsComponent 上",
          sv && sv->values.count("hp") && sv->values.at("hp") == 100.0,
          fmt("component=%s entries=%zu", sv ? "yes" : "no",
              sv ? sv->values.size() : 0));
}

// C6 两个实体的同名 attr 互不影响。
void testC6() {
    scene::Scene sc;
    sc.createObject("A");
    sc.createObject("B");
    ScriptRuntime rt;
    MockHost mh;
    installMock(rt, mh);
    const ecs::Entity e = runScript(sc, rt, "C6", R"VKS(
setAttr(find("A"), "hp", 10)
setAttr(find("B"), "hp", 20)
let a = attr(find("A"), "hp", 0)
let b = attr(find("B"), "hp", 0)
let diff = b - a
)VKS", 1);
    const ScriptInstance* inst = rt.find(e);
    check("C6 同名 attr 按实体隔离", varNum(inst, "diff") == 10.0,
          fmt("a=%.0f b=%.0f diff=%.0f", varNum(inst, "a"), varNum(inst, "b"),
              varNum(inst, "diff")));
}

// C7 存档往返：worldAttr 保住，逐实体 attr **不出现**在 JSON 里。
void testC7() {
    scene::Scene sc;
    sc.createObject("Player");
    ScriptRuntime rt;
    MockHost mh;
    installMock(rt, mh);
    runScript(sc, rt, "C7", R"VKS(
setWorldAttr("quest_stage", 3)
setAttr(self(), "zz_local_only", 7)
)VKS", 1);

    std::string json;
    const auto r = scene::sceneToJson(sc, json);
    const bool hasWorld = json.find("quest_stage") != std::string::npos &&
                          json.find("scriptVars") != std::string::npos;
    const bool leaked = json.find("zz_local_only") != std::string::npos;
    check("C7 worldAttr 进存档、逐实体 attr 不进", r.ok && hasWorld && !leaked,
          fmt("ok=%d hasWorld=%d leaked=%d", static_cast<int>(r.ok),
              static_cast<int>(hasWorld), static_cast<int>(leaked)));

    // 读回来：scriptVars 要原样恢复
    scene::Scene loaded;
    assets::AssetManager am{assets::TextureContext{}};
    const auto rr = scene::sceneFromJson(loaded, am, json);
    const auto it = loaded.scriptVars().find("quest_stage");
    check("C7b 读回后 worldAttr 还在",
          rr.ok && it != loaded.scriptVars().end() && it->second == 3.0,
          fmt("ok=%d stage=%.0f", static_cast<int>(rr.ok),
              it == loaded.scriptVars().end() ? -1.0 : it->second));
}

// C8 配额：循环里狂查世界 → 报错 + 熔断，且**帧时间不超预算**。
void testC8() {
    scene::Scene sc;
    ScriptRuntime rt;
    MockHost mh;
    installMock(rt, mh);

    const auto t0 = std::chrono::steady_clock::now();
    const ecs::Entity e = runScript(sc, rt, "C8", R"VKS(
let i = 0
let acc = 0.0
while (i < 30000) {
    acc = acc + pos(self()).x
    i = i + 1
}
)VKS", 1);
    const auto t1 = std::chrono::steady_clock::now();
    const double ms =
        std::chrono::duration<double, std::milli>(t1 - t0).count();

    const ScriptInstance* inst = rt.find(e);
    const bool quotaHit =
        inst && inst->failed &&
        inst->lastError.find("builtin call quota") != std::string::npos;
    check("C8 内建调用配额触发并熔断", quotaHit,
          inst ? inst->lastError : std::string("<no instance>"));
    // 预算：一次 tick 里 20000 次内建调用必须远快于 16 ms。
    // 阈值取 500 ms —— 这里要抓的是"配额根本不生效 → 跑满 30000 次并且
    // 更长的循环会把帧拖死"，不是测性能。
    check("C8b 熔断后帧时间仍在预算内", ms < 500.0, fmt("%.2f ms < 500", ms));
}


// C9 `player()` 由宿主注入；没有注入时 `exists(player())` 为 false（不报错）。
void testC9() {
    scene::Scene sc;
    const ecs::Entity who = sc.createObject("Hero");
    ScriptRuntime rt;
    MockHost mh;
    installMock(rt, mh);
    mh.playerEntity = who;
    const ecs::Entity e = runScript(sc, rt, "C9", R"VKS(
let me = player()
let pname = name(me)
let ok = exists(me)
)VKS", 1);
    const ScriptInstance* inst = rt.find(e);
    check("C9 player() 拿到宿主注入的实体",
          varBool(inst, "ok") && varStr(inst, "pname") == "Hero",
          fmt("ok=%s name='%s'", varStr(inst, "ok").c_str(),
              varStr(inst, "pname").c_str()));

    // 没有玩家：不报错、exists 为 false（单机里"玩家还没出生"是正常状态）
    scene::Scene sc2;
    ScriptRuntime rt2;
    MockHost mh2;
    installMock(rt2, mh2);
    const ecs::Entity e2 = runScript(sc2, rt2, "C9b", R"VKS(
let ok2 = exists(player())
)VKS", 1);
    const ScriptInstance* inst2 = rt2.find(e2);
    check("C9b 没有玩家时 exists(player()) 为 false 且不报错",
          !varBool(inst2, "ok2", true) && !inst2->failed,
          fmt("ok2=%s failed=%d", varStr(inst2, "ok2").c_str(),
              inst2 ? static_cast<int>(inst2->failed) : -1));
}

// C10 刚体四件套：vel / setVel / impulse / grounded 走注入。
void testC10() {
    scene::Scene sc;
    ScriptRuntime rt;
    MockHost mh;
    installMock(rt, mh);
    mh.bodyVel = glm::vec3(1.0f, 2.0f, 3.0f);
    mh.groundedValue = true;
    const ecs::Entity e = runScript(sc, rt, "C10", R"VKS(
let me = self()
let v = vel(me)
let vsum = v.x + v.y + v.z
setVel(me, vec3(0, 5, 0))
impulse(me, vec3(0, 0, 9))
let onGround = grounded(me)
)VKS", 1);
    const ScriptInstance* inst = rt.find(e);
    check("C10 vel/setVel/impulse/grounded 全部打通",
          varNum(inst, "vsum") == 6.0 && mh.setBodyVelCalls == 1 &&
              std::fabs(mh.bodyVel.y - 5.0f) < 1e-4f && mh.impulseCalls == 1 &&
              std::fabs(mh.lastImpulse.z - 9.0f) < 1e-4f &&
              varBool(inst, "onGround"),
          fmt("vsum=%.0f setVel=%d velY=%.1f imp=%d impZ=%.1f ground=%s",
              varNum(inst, "vsum"), mh.setBodyVelCalls, mh.bodyVel.y,
              mh.impulseCalls, mh.lastImpulse.z,
              varStr(inst, "onGround").c_str()));
}

// C11 `visible()` + `setVisible()`。老 `setVisible` 只对 self 生效，
// S2 之后带上显式实体 —— 两条路都要能用。
void testC11() {
    scene::Scene sc;
    const ecs::Entity target = sc.createObject("Lamp");
    ScriptRuntime rt;
    MockHost mh;
    installMock(rt, mh);
    const ecs::Entity e = runScript(sc, rt, "C11", R"VKS(
let t = find("Lamp")
let before = visible(t)
setVisible(t, false)
let after = visible(t)
)VKS", 1);
    const ScriptInstance* inst = rt.find(e);
    check("C11 visible() 反映 setVisible 的结果",
          varBool(inst, "before") && !varBool(inst, "after", true),
          fmt("before=%s after=%s", varStr(inst, "before").c_str(),
              varStr(inst, "after").c_str()));
    (void)target;
}

// C12 负向：对已删实体调 `pos()` 必须**报错**，而不是静默返回 (0,0,0)。
void testC12() {
    scene::Scene sc;
    const ecs::Entity dead = sc.createObject("Doomed");
    ScriptRuntime rt;
    MockHost mh;
    installMock(rt, mh);
    const std::string p = writeScript("C12.vks", R"VKS(
let d = find("Doomed")
pos(d)
)VKS");
    const ecs::Entity e = addScriptEntity(sc, "C12", p);
    rt.tick(sc, ScriptRuntime::kSelfTimed, kDt, 0);   // 先让实例建起来

    sc.world().destroy(dead);
    rt.tick(sc, ScriptRuntime::kSelfTimed, kDt, 1);

    const ScriptInstance* inst = rt.find(e);
    check("C12 pos() 拿到已删实体时清晰报错",
          inst && inst->failed &&
              inst->lastError.find("invalid or deleted entity") !=
                  std::string::npos,
          inst ? inst->lastError : std::string("<no instance>"));
}


// C13 演出三件（C6 导演 API）正向：三条都能调通、参数原样送到宿主、
// `playAnim` 的隐式 self 与显式实体两种形态都成立。
//
// ⚠ 这一组验的是**接线**，不是"过场真的播了" —— 那个由
//   `tools/verify_director_api.py` 端到端验（那边有真的资产和骨架）。
//   在这里铺一套假 SequencerSystem 只会把"宿主实现了什么"和"引擎传了什么"
//   混成一个结论。
void testC13() {
    scene::Scene sc;
    ScriptRuntime rt;
    MockHost mh;
    installMock(rt, mh);
    const ecs::Entity e = runScript(sc, rt, "C13", R"VKS(
let a = playSequence("sequences/opening.seq.json")
let b = showDialogue("ch01_open_01")
let c = playAnim("Run")
let me = self()
let d = playAnim(me, "Idle")
)VKS", 1);
    const ScriptInstance* inst = rt.find(e);

    const bool bothOk = mh.sequences.size() == 1 &&
                        mh.sequences[0] == "sequences/opening.seq.json" &&
                        mh.dialogues.size() == 1 &&
                        mh.dialogues[0] == "ch01_open_01" &&
                        mh.animClips.size() == 2 && mh.animClips[0] == "Run" &&
                        mh.animClips[1] == "Idle";
    check("C13 演出三件：参数原样到宿主、playAnim 两种形态都对",
          varBool(inst, "a") && varBool(inst, "b") && varBool(inst, "c") &&
              varBool(inst, "d") && bothOk &&
              // 隐式 self 与显式 `self()` 必须落到**同一个**实体上
              mh.lastAnimEntity.id == e.id,
          fmt("seq=%d dlg=%d anim=%d first=[%s] last=[%s] ent=%u/%u",
              static_cast<int>(mh.sequences.size()),
              static_cast<int>(mh.dialogues.size()),
              static_cast<int>(mh.animClips.size()),
              mh.animClips.empty() ? "" : mh.animClips[0].c_str(),
              mh.animClips.size() < 2 ? "" : mh.animClips[1].c_str(),
              static_cast<unsigned>(mh.lastAnimEntity.id),
              static_cast<unsigned>(e.id)));
}

// C14 演出三件负向：宿主说"播不了"时，脚本必须拿到 false **并且报错**。
//
// ⚠ 这是本组真正的价值所在。静默返回 true 的症状是"剧本写完了、日志全绿、
//   画面什么都没发生"—— 排查时第一个怀疑的永远是"资产路径写错了"，而
//   实际原因是宿主那边根本没实现。与 `setVel` 缺刚体时 `noBody()` 报错
//   同一条纪律：**没钱就说没钱**。
void testC14() {
    scene::Scene sc;
    ScriptRuntime rt;
    MockHost mh;
    installMock(rt, mh);
    mh.playSequenceOk = false;
    const ecs::Entity e = runScript(sc, rt, "C14", R"VKS(
let a = playSequence("nope.seq.json")
)VKS", 2);
    const ScriptInstance* inst = rt.find(e);
    const bool errored =
        inst && (inst->failed ||
                 inst->lastError.find("playSequence") != std::string::npos);
    // ⚠ 判据是 `!varBool(inst, "a", false)` —— **默认给 false**，不是 true。
    //   运行期错误会让那条语句**中止**，于是 `let a = ...` 的赋值根本没发生
    //   （日志里是 `a=<absent>`）。默认值给 true 的话，"没赋值"会被读成
    //   "成功了"，整条断言直接反过来（第一版就是这么红的）。
    //   所以这条测的是：**要么 a 是 false、要么 a 压根不存在**，两者都算
    //   "没有静默成功"。
    check("C14 点播失败必须报错并返回 false（不许静默成功）",
          !varBool(inst, "a", false) && errored,
          inst ? fmt("failed=%d a=%s err=%s",
                     inst->failed ? 1 : 0, varStr(inst, "a").c_str(),
                     inst->lastError.c_str())
               : std::string("<no instance>"));
}

// C15 类型纪律：路径参数必须是字符串。传数字时**不许**被 `toStr()` 静默
// 转成 "3"（那样报出来的错会是"资产不存在"，排查方向从一开始就错了）。
void testC15() {
    scene::Scene sc;
    ScriptRuntime rt;
    MockHost mh;
    installMock(rt, mh);
    const ecs::Entity e = runScript(sc, rt, "C15", R"VKS(
let a = playSequence(3)
)VKS", 2);
    const ScriptInstance* inst = rt.find(e);
    check("C15 playSequence(数字) 报类型错、不送进宿主",
          inst && inst->lastError.find("expected a string") != std::string::npos &&
              mh.sequences.empty(),
          inst ? fmt("calls=%d err=%s",
                     static_cast<int>(mh.sequences.size()),
                     inst->lastError.c_str())
               : std::string("<no instance>"));
}

// ============================================================ C6 第二批
// 世界控制：旗标 / 刷实体 / 换场景。
//
// 这一组全部是"接线"层面的断言（参数到宿主、返回值到脚本、失败必报错）。
// **真链路**（旗标真的能跨场景留着、spawn 出来的东西真的出现在画面上、
// loadScene 真的把世界换掉）由 `tools/verify_world_api.py` 验 —— 那边有
// 真场景、真模型、真渲染循环。这里不重复造那个场地。

// C16 旗标：写进去 / 读回来 / 布尔写法 / 未设过 = 0 / 覆盖不累加。
//
// ⚠ "未设过 = 0 且**不报错**"是刻意的语义：`getFlag("met_alice")` 在第 1 章
//   问到第 5 章的角色时，正确答案就是 0。把它做成错误会让每个分支判断都要
//   先记得初始化一遍。
void testC16() {
    scene::Scene sc;
    ScriptRuntime rt;
    MockHost mh;
    installMock(rt, mh);
    const ecs::Entity e = runScript(sc, rt, "C16", R"VKS(
let a = setFlag("affection", 42)
let b = getFlag("affection")
let c = setFlag("met_alice", true)
let d = getFlag("met_alice")
let m = getFlag("never_set")
let o = setFlag("affection", 60)
let n = getFlag("affection")
)VKS", 1);
    const ScriptInstance* inst = rt.find(e);
    const bool ok = inst && varBool(inst, "a") && varBool(inst, "c") &&
                    varBool(inst, "o") &&
                    varNum(inst, "b") == 42.0 && varNum(inst, "d") == 1.0 &&
                    varNum(inst, "m") == 0.0 &&
                    // 覆盖而不是累加：42 + 60 = 102 是错的，正确答案是 60
                    varNum(inst, "n") == 60.0 &&
                    mh.setFlagCalls == 3 && mh.flags.size() == 2 &&
                    mh.flags.count("affection") == 1 &&
                    mh.flags.at("affection") == 60.0;
    check("C16 旗标：写读往返 / bool→1 / 未设过=0 / 覆盖不累加", ok,
          inst ? fmt("b=%.1f d=%.1f m=%.1f n=%.1f calls=%d keys=%zu",
                     varNum(inst, "b"), varNum(inst, "d"), varNum(inst, "m"),
                     varNum(inst, "n"), mh.setFlagCalls, mh.flags.size())
               : std::string("<no instance>"));
}

// C17 spawn：位置参数原样到宿主，返回的句柄**当场可用**。
//
// ⚠ 后半句是这条用例的重点。宿主这里返回的是一个**真实体**（用例自己
//   `createObject` 造的），于是紧接着的 `pos(s)` 必须能读到它的变换 ——
//   这就把"返回值是真的能用的句柄"和"只是回了个数"区分开了。真实装配
//   （Model → 实体 + 组件）在 verify_world_api.py 里验。
void testC17() {
    scene::Scene sc;
    ScriptRuntime rt;
    MockHost mh;
    installMock(rt, mh);
    mh.spawnEntity = sc.createObject("slime");   // 宿主"刷出来"的东西
    const ecs::Entity e = runScript(sc, rt, "C17", R"VKS(
let s = spawn("models/slime.vmesh", vec3(1, 2, 3))
let t = spawn("models/slime.vmesh")
setPos(s, vec3(5, 6, 7))
let p = pos(s)
)VKS", 1);
    const ScriptInstance* inst = rt.find(e);
    const bool twoCalls = mh.spawnPaths.size() == 2 &&
                          mh.spawnPositions.size() == 2 &&
                          mh.spawnPositions[0] == glm::vec3(1.0f, 2.0f, 3.0f) &&
                          // 不给位置 = 原点（不是"上一次的位置"）
                          mh.spawnPositions[1] == glm::vec3(0.0f);
    const glm::vec3 p0 =
        mh.spawnPositions.empty() ? glm::vec3(-1e30f) : mh.spawnPositions[0];
    const glm::vec3 pv = varVec(inst, "p");
    check("C17 spawn：两种形态的位置都对、返回的句柄当场可 setPos/pos",
          inst && varEntity(inst, "s").id == mh.spawnEntity.id &&
              varEntity(inst, "t").id == mh.spawnEntity.id && twoCalls &&
              pv == glm::vec3(5.0f, 6.0f, 7.0f),
          inst ? fmt("spawns=%zu p0=(%.1f,%.1f,%.1f) sid=%u/%u p=(%.1f,%.1f,%.1f)",
                     mh.spawnPaths.size(), static_cast<double>(p0.x),
                     static_cast<double>(p0.y), static_cast<double>(p0.z),
                     static_cast<unsigned>(varEntity(inst, "s").id),
                     static_cast<unsigned>(mh.spawnEntity.id),
                     static_cast<double>(pv.x), static_cast<double>(pv.y),
                     static_cast<double>(pv.z))
               : std::string("<no instance>"));
}

// C18 spawn 失败：宿主返回无效句柄时必须报错。
//
// ⚠ 静默返回一个无效句柄的后果：下一句 `setPos(s, ...)` 只会说"实体无效"，
//   而真正的原因（模型路径拼错 / 加载器不支持这个格式）就此消失。
void testC18() {
    scene::Scene sc;
    ScriptRuntime rt;
    MockHost mh;
    installMock(rt, mh);
    // spawnEntity 保持无效 = "刷不出来"
    const ecs::Entity e = runScript(sc, rt, "C18", R"VKS(
let s = spawn("models/nope.vmesh")
)VKS", 2);
    const ScriptInstance* inst = rt.find(e);
    const bool errored =
        inst && (inst->failed ||
                 inst->lastError.find("spawn") != std::string::npos);
    check("C18 spawn 失败必须报错（不许静默返回无效句柄）",
          !varEntity(inst, "s").valid() && errored && mh.spawnPaths.size() == 1,
          inst ? fmt("calls=%d err=%s", static_cast<int>(mh.spawnPaths.size()),
                     inst->lastError.c_str())
               : std::string("<no instance>"));
}

// C19 loadScene：正向（路径原样到宿主 + 拿到 true）与负向（宿主拒绝 →
// false + 报错）。
//
// ⚠ 类型纪律（`loadScene(3)`）不在这里重测：它走的是 C15 已经钉住的
//   同一个 `strOf()`。再加一条只会让"strOf 坏了"表现为两处红，没有新信息。
void testC19() {
    {
        scene::Scene sc;
        ScriptRuntime rt;
        MockHost mh;
        installMock(rt, mh);
        const ecs::Entity e = runScript(sc, rt, "C19A", R"VKS(
let a = loadScene("scenes/ch02.json")
)VKS", 1);
        const ScriptInstance* inst = rt.find(e);
        check("C19a loadScene 正向：路径原样到宿主、脚本拿到 true",
              inst && varBool(inst, "a") && mh.sceneRequests.size() == 1 &&
                  mh.sceneRequests[0] == "scenes/ch02.json",
              fmt("reqs=%zu first=[%s]", mh.sceneRequests.size(),
                  mh.sceneRequests.empty() ? "" : mh.sceneRequests[0].c_str()));
    }
    {
        scene::Scene sc;
        ScriptRuntime rt;
        MockHost mh;
        installMock(rt, mh);
        mh.loadSceneOk = false;   // 宿主：文件不存在 / 读不了
        const ecs::Entity e = runScript(sc, rt, "C19B", R"VKS(
let a = loadScene("scenes/nope.json")
)VKS", 2);
        const ScriptInstance* inst = rt.find(e);
        const bool errored =
            inst && (inst->failed ||
                     inst->lastError.find("loadScene") != std::string::npos);
        // ⚠ 默认值给 false：运行期错误会**中止**那条语句，`let a = ...`
        //   根本没赋值 —— 默认给 true 会把"没赋值"读成"成功了"（C14 踩过）。
        check("C19b loadScene 失败必须报错并返回 false（不许静默受理）",
              !varBool(inst, "a", false) && errored && mh.sceneRequests.size() == 1,
              inst ? fmt("reqs=%zu err=%s", mh.sceneRequests.size(),
                         inst->lastError.c_str())
                   : std::string("<no instance>"));
    }
}

// ============================================================ D 组（S3）
// 事件与输入：脚本从"每帧跑一遍"变成"能响应"。

// 取已编译的 Program（钩子表在里面）
const ScriptProgram* progOf(ScriptRuntime& rt, const ScriptInstance* inst) {
    if (!inst) return nullptr;
    ScriptAsset* a = rt.engine().get(inst->path);
    return a ? a->program() : nullptr;
}

// D0 编译期把 `on` 块收进 hooks()，顺序 = 声明顺序
void testD0() {
    scene::Scene sc;
    ScriptRuntime rt;
    MockHost mh;
    installMock(rt, mh);
    const ecs::Entity e = runScript(sc, rt, "D0", R"VKS(
let n = 0
on start {
    n = n + 100
}
on update {
    n = n + 1
}
)VKS", 1);
    const ScriptProgram* p = progOf(rt, rt.find(e));
    const bool shape =
        p && p->hooks().size() == 2 &&
        p->hooks()[0]->kind == HookKind::Start &&
        p->hooks()[1]->kind == HookKind::Update;
    check("D0 编译期收集 on start / on update", shape,
          fmt("hooks=%zu %s,%s", p ? p->hooks().size() : 0u,
              p && p->hooks().size() > 0 ? hookName(p->hooks()[0]->kind) : "-",
              p && p->hooks().size() > 1 ? hookName(p->hooks()[1]->kind) : "-"));
    // 第一帧：start(+100) 然后 update(+1) = 101
    check("D0b start 在 update 之前跑", varNum(rt.find(e), "n") == 101.0,
          fmt("n=%.0f expect=101", varNum(rt.find(e), "n")));
}

// D1 注入 `Jump` 边沿 → setPos 抬升，位移量符合预期
void testD1() {
    scene::Scene sc;
    const ecs::Entity mover = sc.createObject("Mover");
    ScriptRuntime rt;
    MockHost mh;
    installMock(rt, mh);
    mh.pressedMap["Jump"] = true;
    const ecs::Entity e = runScript(sc, rt, "D1", R"VKS(
if (pressed("Jump")) {
    setPos(self(), pos(self()) + vec3(0, 2.5, 0))
}
)VKS", 1);
    const glm::vec3 p(sc.worldMatrix(e)[3]);
    check("D1 pressed 边沿触发一次位移（2.5 米）",
          std::fabs(p.y - 2.5f) < 1e-4f,
          fmt("y=%.3f expect=2.5", p.y));
    (void)mover;
}

// D2 held 连按 3 帧为 true，第 4 帧松开后 false
void testD2() {
    scene::Scene sc;
    ScriptRuntime rt;
    MockHost mh;
    installMock(rt, mh);
    const std::string path = writeScript("D2.vks", R"VKS(
let n = 0
if (held("Jump")) {
    n = n + 1
}
)VKS");
    const ecs::Entity e = addScriptEntity(sc, "D2", path);
    rt.tick(sc, ScriptRuntime::kSelfTimed, kDt, 0);

    mh.heldMap["Jump"] = true;
    for (int i = 0; i < 3; ++i)
        rt.tick(sc, ScriptRuntime::kSelfTimed, kDt, static_cast<uint64_t>(i + 1));
    const double heldFrames = varNum(rt.find(e), "n");

    mh.heldMap["Jump"] = false;
    for (int i = 0; i < 3; ++i)
        rt.tick(sc, ScriptRuntime::kSelfTimed, kDt, static_cast<uint64_t>(i + 10));
    const double afterRelease = varNum(rt.find(e), "n");
    check("D2 held 3 帧计数、松开后不再加", heldFrames == 3.0 && afterRelease == 3.0,
          fmt("held=%0.f afterRelease=%.0f expect=3/3", heldFrames,
              afterRelease));
}

// D3 on collide(other)：other 就是实际被撞的那个实体
void testD3() {
    scene::Scene sc;
    const ecs::Entity wall = sc.createObject("Wall");
    ScriptRuntime rt;
    MockHost mh;
    installMock(rt, mh);
    const ecs::Entity e = runScript(sc, rt, "D3", R"VKS(
let hitName = ""
on collide(other) {
    hitName = name(other)
}
)VKS", 1);
    // 第一帧已经把 start/update 跑完了，这里补投一次 Enter
    ScriptContact c;
    c.kind = ScriptContact::Kind::Enter;
    c.a = e;
    c.b = wall;
    rt.postContact(c);
    rt.tick(sc, ScriptRuntime::kSelfTimed, kDt, 1);

    check("D3 on collide(other) 的 other 是被撞实体",
          varStr(rt.find(e), "hitName") == "Wall",
          fmt("hitName='%s'", varStr(rt.find(e), "hitName").c_str()));
}

// D4 接触序列：Enter×1 → Stay×N → Exit×1
void testD4() {
    scene::Scene sc;
    const ecs::Entity wall = sc.createObject("Wall");
    ScriptRuntime rt;
    MockHost mh;
    installMock(rt, mh);
    const ecs::Entity e = runScript(sc, rt, "D4", R"VKS(
let hits = 0
let exits = 0
on collide(other) {
    hits = hits + 1
}
on exit(other) {
    exits = exits + 1
}
)VKS", 1);

    auto post = [&](ScriptContact::Kind k) {
        ScriptContact c;
        c.kind = k;
        c.a = e;
        c.b = wall;
        rt.postContact(c);
    };
    post(ScriptContact::Kind::Enter);
    rt.tick(sc, ScriptRuntime::kSelfTimed, kDt, 10);
    post(ScriptContact::Kind::Stay);
    rt.tick(sc, ScriptRuntime::kSelfTimed, kDt, 11);
    post(ScriptContact::Kind::Stay);
    rt.tick(sc, ScriptRuntime::kSelfTimed, kDt, 12);
    const double midHits = varNum(rt.find(e), "hits");
    post(ScriptContact::Kind::Exit);
    rt.tick(sc, ScriptRuntime::kSelfTimed, kDt, 13);

    const ScriptInstance* inst = rt.find(e);
    check("D4 Enter + Stay 进 on collide（1+2）", midHits == 3.0,
          fmt("hits=%.0f expect=3", midHits));
    check("D4b Exit 进 on exit 且只一次", varNum(inst, "exits") == 1.0,
          fmt("exits=%.0f expect=1", varNum(inst, "exits")));
}

// D5 相位顺序：start 里改的状态，**同一帧**的 update 就能看到
// （"on start 里创建的实体同一帧能被 find 找到"需要 spawn/预制体，见台账）
void testD5() {
    scene::Scene sc;
    ScriptRuntime rt;
    MockHost mh;
    installMock(rt, mh);
    const ecs::Entity e = runScript(sc, rt, "D5", R"VKS(
let tag = "none"
on start {
    tag = "ready"
}
on update {
    seen = tag
}
)VKS", 1);
    check("D5 start 写的状态同帧 update 可见",
          varStr(rt.find(e), "seen") == "ready",
          fmt("seen='%s'", varStr(rt.find(e), "seen").c_str()));
}

// D6 顶层语句**先于** on update（顺序写死在 runFrame 里）
void testD6() {
    scene::Scene sc;
    ScriptRuntime rt;
    MockHost mh;
    installMock(rt, mh);
    const ecs::Entity e = runScript(sc, rt, "D6", R"VKS(
let order = 0
order = order * 10 + 1
on update {
    order = order * 10 + 2
}
)VKS", 1);
    check("D6 顶层先、on update 后", varNum(rt.find(e), "order") == 12.0,
          fmt("order=%.0f expect=12", varNum(rt.find(e), "order")));
}

// D7 宿主没填输入 → pressed/held/axis 恒 false/0，且**不报错**
void testD7() {
    scene::Scene sc;
    ScriptRuntime rt;
    // 只装 HUD/音频，输入一个都不装
    ScriptServices svc;
    rt.setServices(std::move(svc));
    const ecs::Entity e = runScript(sc, rt, "D7", R"VKS(
let p = pressed("Jump")
let h = held("Jump")
let r = released("Jump")
let ax = axis("Move")
let sum = ax
)VKS", 1);
    const ScriptInstance* inst = rt.find(e);
    check("D7 没装输入时恒 false/0 且不报错",
          inst && !inst->failed && !varBool(inst, "p", true) &&
              !varBool(inst, "h", true) && !varBool(inst, "r", true) &&
              varNum(inst, "sum") == 0.0,
          inst ? fmt("failed=0 p=%s h=%s ax=%.1f",
                     varStr(inst, "p").c_str(), varStr(inst, "h").c_str(),
                     varNum(inst, "sum"))
               : std::string("<no instance>"));
}

// D8 start 只跑一次（连跑 4 帧计数仍是 1）
void testD8() {
    scene::Scene sc;
    ScriptRuntime rt;
    MockHost mh;
    installMock(rt, mh);
    const ecs::Entity e = runScript(sc, rt, "D8", R"VKS(
let starts = 0
on start {
    starts = starts + 1
}
)VKS", 4);
    check("D8 on start 只跑一次", varNum(rt.find(e), "starts") == 1.0,
          fmt("starts=%.0f expect=1", varNum(rt.find(e), "starts")));
}

// D9~D11 负向：钩子名的三种写错方式都必须**编译失败**
void testD9() {
    std::string em;
    int el = 0;
    const bool ok1 = compiles("on foo {\n}\n", em, &el);
    check("D9 未知钩子名编译失败",
          !ok1 && em.find("unknown hook 'foo'") != std::string::npos,
          fmt("ok=%d err='%s'", static_cast<int>(ok1), em.c_str()));

    em.clear();
    const bool ok2 = compiles("on update(x) {\n}\n", em, &el);
    check("D9b on update 带参数编译失败",
          !ok2 && em.find("does not take a parameter") != std::string::npos,
          fmt("ok=%d err='%s'", static_cast<int>(ok2), em.c_str()));

    em.clear();
    const bool ok3 = compiles("on update {\n    let x = 1\n}\n", em, &el);
    check("D9c 钩子体内 let 编译失败",
          !ok3 && em.find("top level") != std::string::npos,
          fmt("ok=%d err='%s'", static_cast<int>(ok3), em.c_str()));
}

// D12 互相碰撞：**双方**都要收到事件（撞人的和被撞的）
void testD12() {
    scene::Scene sc;
    sc.createObject("A");
    sc.createObject("B");
    ScriptRuntime rt;
    MockHost mh;
    installMock(rt, mh);
    const std::string pathA = writeScript("D12a.vks", R"VKS(
let got = ""
on collide(other) {
    got = name(other)
}
)VKS");
    const ecs::Entity ea = addScriptEntity(sc, "EA", pathA);
    const ecs::Entity eb = addScriptEntity(sc, "EB", pathA + "");  // 同一份脚本
    // 让两个实体都带上自己的脚本：EB 用另一份文件，路径不同 → 两个实例
    const std::string pathB = writeScript("D12b.vks", R"VKS(
let got = ""
on collide(other) {
    got = name(other)
}
)VKS");
    ecs::ScriptComponent cb;
    cb.path = pathB;
    cb.enabled = true;
    sc.world().add<ecs::ScriptComponent>(eb, cb);
    rt.tick(sc, ScriptRuntime::kSelfTimed, kDt, 0);

    ScriptContact c;
    c.kind = ScriptContact::Kind::Enter;
    c.a = ea;
    c.b = eb;
    rt.postContact(c);
    rt.tick(sc, ScriptRuntime::kSelfTimed, kDt, 1);

    check("D12 接触事件双方各自收到（other 互为对方）",
          varStr(rt.find(ea), "got") == "EB" && varStr(rt.find(eb), "got") == "EA",
          fmt("a.got='%s' b.got='%s'", varStr(rt.find(ea), "got").c_str(),
              varStr(rt.find(eb), "got").c_str()));
}


// ============================================================ E 组（S4）
// 协程：执行模型从"递归"换成"显式帧栈"。这一组守的是"换了之后还能停"。

// 跑 n 帧并返回最后一帧的实例
const ScriptInstance* runFrames(scene::Scene& sc, ScriptRuntime& rt,
                                ecs::Entity e, int n, uint64_t baseFrame) {
    for (int i = 0; i < n; ++i) {
        rt.tick(sc, ScriptRuntime::kSelfTimed, kDt,
                baseFrame + static_cast<uint64_t>(i));
    }
    return rt.find(e);
}

// E1 `wait(1.0)` 的恢复时刻误差 < 1 个固定步
void testE1() {
    scene::Scene sc;
    ScriptRuntime rt;
    MockHost mh;
    installMock(rt, mh);
    const ecs::Entity e = runScript(sc, rt, "E1", R"VKS(
let t0 = 0.0
let elapsed = -1.0
t0 = time
wait(1.0)
elapsed = time - t0
)VKS", 90);
    const ScriptInstance* inst = rt.find(e);
    const double el = varNum(inst, "elapsed");
    // 误差 < 1 个固定步（1/60 ≈ 0.01667）。判据用 0.02 是为了留一点浮点余量。
    check("E1 wait(1.0) 恢复误差 < 1 个固定步",
          el > 0.0 && std::fabs(el - 1.0) < 0.02,
          fmt("elapsed=%.5f expect≈1.0 ±0.02", el));
    // 而且确实挂起过若干帧（不是"立刻返回 0 然后一直被跳过"）
    check("E1b 等待期间实例被记为空转", inst && inst->simFrames >= 60,
          fmt("simFrames=%llu",
              inst ? static_cast<unsigned long long>(inst->simFrames) : 0ULL));
}

// E2 三层嵌套（if → while → if）里的 wait，挂起位置精确
void testE2() {
    scene::Scene sc;
    ScriptRuntime rt;
    MockHost mh;
    installMock(rt, mh);
    const ecs::Entity e = runScript(sc, rt, "E2", R"VKS(
let seq = ""
let i = 0
if (1) {
    while (i < 3) {
        if (i == 1) {
            seq = seq + "A"
            wait(0.1)
            seq = seq + "B"
        }
        seq = seq + "c"
        i = i + 1
    }
}
)VKS", 60);
    const ScriptInstance* inst = rt.find(e);
    // i=0 → "c"；i=1 → "A"（挂起）"B""c"；i=2 → "c"
    check("E2 三层嵌套里的 wait 从原位恢复",
          varStr(inst, "seq") == "cABcc",
          fmt("seq='%s' expect='cABcc'", varStr(inst, "seq").c_str()));
}

// E3 while 里 wait(0.1) 跑 10 次 → 总时长 ≈ 1.0（不重入、不加速）
void testE3() {
    scene::Scene sc;
    ScriptRuntime rt;
    MockHost mh;
    installMock(rt, mh);
    const ecs::Entity e = runScript(sc, rt, "E3", R"VKS(
let n = 0
let t0 = 0.0
let elapsed = -1.0
on start {
    t0 = time
}
while (n < 10) {
    wait(0.1)
    n = n + 1
}
if (elapsed < 0.0) {
    elapsed = time - t0
}
)VKS", 120);
    const ScriptInstance* inst = rt.find(e);
    const double el = varNum(inst, "elapsed");
    // ⚠ `elapsed = time - t0` 若写成顶层语句，会**每帧重算**，终值就成了
    //   最后一帧的时间。所以脚本里加了 `if (elapsed < 0.0)` 守卫 —— 只记
    //   循环真正结束的那一刻。这是"顶层 = 隐式 update"的直接后果。
    check("E3 while 里 10 次 wait(0.1) 总时长 ≈ 1.0s",
          varNum(inst, "n") == 10.0 && el > 0.85 && el < 1.15,
          fmt("n=%.0f elapsed=%.4f expect 10 / ≈1.0", varNum(inst, "n"), el));
}

// E4 1000 个实例同时 wait：帧时间与"全在跑"同量级，且 skipped==1000
void testE4() {
    // ① 全部在等
    scene::Scene sc;
    ScriptRuntime rt;
    MockHost mh;
    installMock(rt, mh);
    // ⚠ 对照组的循环要小：`while (n < 100000)` 会撞上 200000 步的次数预算
    //   直接熔断，于是"对照组"其实也在空转，比较就没意义了。
    const std::string path = writeScript("E4.vks", R"VKS(
let n = 0
while (n < 20) {
    n = n + 1
}
)VKS");
    const std::string waitPath = writeScript("E4w.vks", R"VKS(
let n = 0
wait(100.0)
n = n + 1
)VKS");

    std::vector<ecs::Entity> waiting;
    waiting.reserve(1000);
    for (int i = 0; i < 1000; ++i)
        waiting.push_back(addScriptEntity(sc, "W" + std::to_string(i), waitPath));
    rt.tick(sc, ScriptRuntime::kSelfTimed, kDt, 0);   // 建实例 + 挂起

    const auto t0 = std::chrono::steady_clock::now();
    for (int f = 0; f < 60; ++f)
        rt.tick(sc, ScriptRuntime::kSelfTimed, kDt, static_cast<uint64_t>(f + 1));
    const auto t1 = std::chrono::steady_clock::now();
    const double waitMs =
        std::chrono::duration<double, std::milli>(t1 - t0).count();
    const auto stWait = rt.lastStats();

    // ② 全部在跑（对照组）
    scene::Scene sc2;
    ScriptRuntime rt2;
    MockHost mh2;
    installMock(rt2, mh2);
    for (int i = 0; i < 1000; ++i)
        addScriptEntity(sc2, "R" + std::to_string(i), path);
    rt2.tick(sc2, ScriptRuntime::kSelfTimed, kDt, 0);
    const auto t2 = std::chrono::steady_clock::now();
    for (int f = 0; f < 60; ++f)
        rt2.tick(sc2, ScriptRuntime::kSelfTimed, kDt, static_cast<uint64_t>(f + 1));
    const auto t3 = std::chrono::steady_clock::now();
    const double runMs =
        std::chrono::duration<double, std::milli>(t3 - t2).count();

    check("E4 1000 个等 wait 的实例全部被跳过",
          stWait.skipped == 1000 && stWait.executed == 0,
          fmt("skipped=%u executed=%u steps=%llu", stWait.skipped,
              stWait.executed,
              static_cast<unsigned long long>(stWait.steps)));
    // "同量级"取 2×：等待态只做一次时间比较，实际应远快于对照组。
    // 阈值给 2 倍是为了在 CI 噪声下不假红 —— 要抓的是"等待态也在跑语句"。
    check("E4b 等待态帧时间不快于全跑态（O(活跃)）", waitMs < runMs * 2.0 + 5.0,
          fmt("wait=%.1fms run=%.1fms", waitMs, runMs));
    (void)waiting;
}

// E5 热重载：wait 挂起中只改一个数字 → 仍从原位置恢复（变量不清、栈不丢）
void testE5() {
    scene::Scene sc;
    ScriptRuntime rt;
    MockHost mh;
    installMock(rt, mh);
    const std::string path = writeScript("E5.vks", R"VKS(
let n = 0
while (n < 100) {
    wait(0.1)
    n = n + 1
}
)VKS");
    const ecs::Entity e = addScriptEntity(sc, "E5", path);
    runFrames(sc, rt, e, 40, 0);   // 跑 ~0.66s → 走了约 6 轮
    const double before = varNum(rt.find(e), "n");

    // 只改一个数字（100 → 200）：结构不变 → 语句 id 不变 → 对账成功
    writeScript("E5.vks", R"VKS(
let n = 0
while (n < 200) {
    wait(0.1)
    n = n + 1
}
)VKS");
    rt.engine().reloadNow(path);
    rt.tick(sc, ScriptRuntime::kSelfTimed, kDt, 100);

    const ScriptInstance* inst = rt.find(e);
    const double after = varNum(inst, "n");
    check("E5 改数字后热重载：从原位置恢复（变量不清零）",
          before > 0.0 && after >= before && inst->restartCount == 0,
          fmt("before=%.0f after=%.0f restarts=%u", before, after,
              inst->restartCount));
}

// E6 热重载：把 wait 那行删掉 → 结构变了 → 整体重启
void testE6() {
    scene::Scene sc;
    ScriptRuntime rt;
    MockHost mh;
    installMock(rt, mh);
    const std::string path = writeScript("E6.vks", R"VKS(
let n = 0
let tag = "old"
while (n < 100) {
    wait(0.1)
    n = n + 1
    tag = "kept"
}
)VKS");
    const ecs::Entity e = addScriptEntity(sc, "E6", path);
    runFrames(sc, rt, e, 40, 0);
    const double before = varNum(rt.find(e), "n");

    // 删掉 wait 行 → 后面的语句 id 全部前移 → 对账必然失败
    writeScript("E6.vks", R"VKS(
let n = 0
let tag = "old"
while (n < 100) {
    n = n + 1
    tag = "kept"
}
)VKS");
    rt.engine().reloadNow(path);
    rt.tick(sc, ScriptRuntime::kSelfTimed, kDt, 100);
    const ScriptInstance* inst = rt.find(e);
    check("E6 删掉 wait 行 → 整体重启（VKS-RELOAD-RESTART）",
          before > 0.0 && inst->restartCount == 1,
          fmt("before=%.0f restarts=%u n=%.0f", before, inst->restartCount,
              varNum(inst, "n")));
}

// E7 黄金值：一批老语法 + 钩子跑固定帧数，变量终值写死比对。
// 这一条是"递归 → 显式栈"改写**没有改变语义**的显式证据。
void testE7() {
    scene::Scene sc;
    sc.createObject("Target");
    ScriptRuntime rt;
    MockHost mh;
    installMock(rt, mh);
    // ⚠ 只跑 **1 帧**：顶层语句是"隐式 update"，每帧都会重跑一遍
    //   （S0 之前就是这条语义）。要一个可写死的黄金值，就必须钉住帧数。
    const ecs::Entity e = runScript(sc, rt, "E7", R"VKS(
let n = 0
let acc = 0.0
let tag = ""
on start {
    tag = tag + "S"
}
on update {
    tag = tag + "u"
}
while (n < 3) {
    acc = acc + n * 2.0
    n = n + 1
}
if (acc > 5.0) {
    tag = tag + "H"
} else {
    tag = tag + "L"
}
foreach (h in near(vec3(0, 0, 0), 50.0)) {
    tag = tag + name(h)
}
)VKS", 1);
    const ScriptInstance* inst = rt.find(e);
    // 1 帧内的顺序：声明区 → start("S") → 顶层(while/if/foreach) → update("u")
    check("E7 黄金值：老语法在显式栈下逐位不变",
          varNum(inst, "n") == 3.0 && varNum(inst, "acc") == 6.0 &&
              varStr(inst, "tag") == "SHTargetu",
          fmt("n=%.0f acc=%.0f tag='%s'", varNum(inst, "n"),
              varNum(inst, "acc"), varStr(inst, "tag").c_str()));
}

// E8 waitFrames：按**固定步**等，与 dt 无关
void testE8() {
    scene::Scene sc;
    ScriptRuntime rt;
    MockHost mh;
    installMock(rt, mh);
    const ecs::Entity e = runScript(sc, rt, "E8", R"VKS(
let after = -1.0
on start {
    waitFrames(5)
    after = simFrame
}
)VKS", 20);
    const ScriptInstance* inst = rt.find(e);
    // ⚠ `simFrame` 是 **1-based**：tick 里先 `++inst.simFrames` 再填 ctx，
    //   所以首帧看到的是 1。`waitFrames(5)` 从 1 等 5 步 → 在 6 恢复。
    check("E8 waitFrames(5) 等 5 个固定步后恢复（simFrame 1-based）",
          varNum(inst, "after") == 6.0,
          fmt("after=%.0f expect=6", varNum(inst, "after")));
}

// E9 waitUntil：条件早满足就**不等**；晚满足则等到那一刻
void testE9() {
    scene::Scene sc;
    ScriptRuntime rt;
    MockHost mh;
    installMock(rt, mh);
    const ecs::Entity e = runScript(sc, rt, "E9", R"VKS(
let flag = 0
let passed = 0
waitUntil(flag > 0)
passed = passed + 1
)VKS", 30);
    // flag 永远是 0 → 一直等；passed 不该变成 1
    check("E9 waitUntil 条件不满足时一直等", varNum(rt.find(e), "passed") == 0.0,
          fmt("passed=%.0f expect=0", varNum(rt.find(e), "passed")));

    // 条件一开始就满足 → 不等
    scene::Scene sc2;
    ScriptRuntime rt2;
    MockHost mh2;
    installMock(rt2, mh2);
    const ecs::Entity e2 = runScript(sc2, rt2, "E9b", R"VKS(
let flag = 1
let passed = 0
waitUntil(flag > 0)
passed = passed + 1
)VKS", 1);
    check("E9b waitUntil 条件已满足则立刻继续",
          varNum(rt2.find(e2), "passed") == 1.0,
          fmt("passed=%.0f expect=1", varNum(rt2.find(e2), "passed")));
}

// E10 负向：wait 出现在表达式里 → 编译失败（没有可挂起的锚点）
void testE10() {
    std::string em;
    int el = 0;
    const bool ok1 = compiles("let x = wait(1.0)\n", em, &el);
    check("E10 let x = wait(1) 编译失败",
          !ok1 && em.find("must be a statement on its own line") !=
                     std::string::npos,
          fmt("ok=%d err='%s'", static_cast<int>(ok1), em.c_str()));

    em.clear();
    const bool ok2 = compiles("if (wait(1.0) > 0) {\n}\n", em, &el);
    check("E10b if 条件里写 wait 编译失败", !ok2,
          fmt("ok=%d err='%s'", static_cast<int>(ok2), em.c_str()));
}

// E11 同一个实体的两个脚本实例各自挂起 —— 协程必须 per-instance
void testE11() {
    scene::Scene sc;
    ScriptRuntime rt;
    MockHost mh;
    installMock(rt, mh);
    const std::string pa = writeScript("E11a.vks", R"VKS(
let n = 0
wait(0.5)
n = 1
)VKS");
    const std::string pb = writeScript("E11b.vks", R"VKS(
let n = 0
wait(0.1)
n = 1
)VKS");
    const ecs::Entity ea = addScriptEntity(sc, "E11A", pa);
    const ecs::Entity eb = addScriptEntity(sc, "E11B", pb);
    runFrames(sc, rt, ea, 20, 0);   // 0.33s：b 早醒了，a 还在等
    check("E11 两个实例各自计时（协程不共享）",
          varNum(rt.find(eb), "n") == 1.0 && varNum(rt.find(ea), "n") == 0.0,
          fmt("a.n=%.0f b.n=%.0f expect 0/1", varNum(rt.find(ea), "n"),
              varNum(rt.find(eb), "n")));
}

// E12 Reset：清协程后从头顶重跑（S5 的 Reset 按钮走这条）
void testE12() {
    scene::Scene sc;
    ScriptRuntime rt;
    MockHost mh;
    installMock(rt, mh);
    const ecs::Entity e = runScript(sc, rt, "E12", R"VKS(
let n = 0
let starts = 0
on start {
    starts = starts + 1
}
wait(100.0)
n = 5
)VKS", 10);
    check("E12a Reset 前停在 wait 里", varNum(rt.find(e), "n") == 0.0,
          fmt("n=%.0f", varNum(rt.find(e), "n")));
    rt.resetInstance(e);
    rt.tick(sc, ScriptRuntime::kSelfTimed, kDt, 20);
    const ScriptInstance* inst = rt.find(e);
    check("E12 Reset 后 on start 重跑、又停在 wait 里",
          varNum(inst, "starts") == 1.0 && varNum(inst, "n") == 0.0,
          fmt("starts=%.0f n=%.0f", varNum(inst, "starts"), varNum(inst, "n")));
}

} // namespace

// ---------------------------------------------------------------- S6 基准
//
// 判据只有一个：**每帧毫秒数**。但为了让这个数字有意义，脚本必须是
// "真正的活儿"而不是空转：
//   · `stmts` 条语句里混着算术 / 属性读 / 内建调用（`abs` / `min`），
//     不是同一句复制 100 遍（那样求值器的分支预测会好到不真实）；
//   · 每个实例都有自己的 `let`，所以 200 份状态真的各自在动 ——
//     顺带把"变量跨帧存活"在规模下再证一次。
//
// ⚠ 别把断言写成"毫秒数 == 某个值"：那是**性能**不是**语义**，机器一换
//   就红。这里只打数字 + 校验累加结果，预算交给调用方（G7）钉。
int runScriptBench() {
    constexpr int kInstances = 200;
    constexpr int kStmtsPerFrame = 100;

    // 生成脚本：`let acc = 0` + 100 条各自独立的赋值。
    // 每条的表达式都不同（`acc + i` / `abs(-i)` / `min(acc, i)`），
    // 这样"100 条"不会被编译器折叠掉，也不会被求值器的短路径吃掉。
    std::string src = "let acc = 0\non update {\n";
    for (int i = 0; i < kStmtsPerFrame; ++i) {
        switch (i % 4) {
            case 0: src += fmt("    acc = acc + %d\n", i); break;
            case 1: src += fmt("    acc = acc - abs(-%d)\n", i); break;
            case 2: src += fmt("    acc = min(acc + %d, 100000)\n", i); break;
            default: src += fmt("    acc = max(acc - %d, -100000)\n", i); break;
        }
    }
    src += "}\n";

    scene::Scene sc;
    ScriptRuntime rt;
    MockHost mh;
    installMock(rt, mh);
    const std::string p = writeScript("bench.vks", src);
    std::vector<ecs::Entity> ents;
    ents.reserve(kInstances);
    for (int i = 0; i < kInstances; ++i) {
        ents.push_back(addScriptEntity(sc, fmt("Bench_%d", i), p));
    }

    constexpr int kWarmup = 20;
    constexpr int kMeasure = 180;
    for (int i = 0; i < kWarmup; ++i) {
        rt.tick(sc, ScriptRuntime::kSelfTimed, kDt, static_cast<uint64_t>(i));
    }
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kMeasure; ++i) {
        rt.tick(sc, ScriptRuntime::kSelfTimed, kDt, static_cast<uint64_t>(i));
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    const double perFrame = ms / static_cast<double>(kMeasure);
    const double perStmt = ms / (static_cast<double>(kMeasure) * kInstances *
                                 kStmtsPerFrame);

    // 正确性兜底：语句真的被执行了（不然帧时间好看得毫无意义）。
    // 每个实例的 acc 与它自己的执行次数有关，所以用"一个实例 + 步数"对账。
    const auto& st = rt.lastStats();
    const double acc0 = varNum(rt.find(ents.front()), "acc");
    const bool ran = st.executed == static_cast<uint32_t>(kInstances) &&
                     acc0 != 0.0 && mh.errorReports == 0;

    VK_LOG_INFO("VKS-BENCH instances=%d stmts=%d ms=%.3f "
                "per_stmt_us=%.4f executed=%u failed=%u acc0=%.0f",
                kInstances, kInstances * kStmtsPerFrame, perFrame,
                perStmt * 1000.0, st.executed, st.failed, acc0);
    VK_LOG_INFO("SCRIPT-T %s %s", ran ? "pass" : "FAIL",
                fmt("S6-bench %d 实例 × %d 语句/帧 每帧 %.3f ms 每语句 %.3f us",
                    kInstances, kStmtsPerFrame, perFrame, perStmt * 1000.0));
    return ran ? 0 : 1;
}

int runScriptSelfTest() {
    VK_LOG_INFO("=== vks script runtime self-test (S0) ===");
    testA1();
    testA2();
    testA3();
    testA4();
    testA5();
    testA6();
    testA7();
    testA8();
    testA9();
    testA10();
    VK_LOG_INFO("--- S1 语言层 ---");
    testB1();
    testB2();
    testB3();
    testB4();
    testB5();
    testB6();
    testB7();
    testB8();
    testB9();
    testB10();
    VK_LOG_INFO("--- S2 世界访问 ---");
    testC1();
    testC3();
    testC4();
    testC5();
    testC6();
    testC7();
    testC8();
    testC9();
    testC10();
    testC11();
    testC12();
    // C6：演出三件（正向接线 / 失败必报错 / 路径参数类型纪律）
    testC13();
    testC14();
    testC15();
    // C6 第二批：世界控制（旗标 / spawn / loadScene）
    testC16();
    testC17();
    testC18();
    testC19();
    VK_LOG_INFO("--- S3 事件与输入 ---");
    testD0();
    testD1();
    testD2();
    testD3();
    testD4();
    testD5();
    testD6();
    testD7();
    testD8();
    testD9();
    testD12();
    VK_LOG_INFO("--- S4 协程 ---");
    testE1();
    testE2();
    testE3();
    testE4();
    testE5();
    testE6();
    testE7();
    testE8();
    testE9();
    testE10();
    testE11();
    testE12();
    VK_LOG_INFO("SCRIPT-TOTAL pass=%d fail=%d", g_pass, g_fail);
    return g_fail;
}

} // namespace vks
