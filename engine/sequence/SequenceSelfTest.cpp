// ============================================================
// sequence/SequenceSelfTest.cpp
//
// C1 时间轴的无窗口自检。分四组：
//   A 解析（含三条"不致命但要报"的警告）
//   B 采样（线性 / smoothstep / step / yaw 短路 / 边界夹取）
//   C 事件区间（半开 / 空区间 / 跨圈两段）
//   D 播放器（推进 / 首帧 t=0 / 起点晚播 / 结束 / 循环 / 编辑态 / 优先级 / 接管相机）
//
// ⚠ 每条断言都问过一句："**别的实现会给出不同的数吗**"。
//   比如 smoothstep 那条特意取 u=0.25 而不是 0.5 —— 两者在中点都等于
//   0.5，测中点等于什么都没测。0.25 处 linear=0.25、smooth=0.15625，
//   差 0.094，肉眼与浮点都分得开。
// ============================================================

#include "sequence/SequencerSystem.h"

#include "core/Logger.h"
#include "ecs/Components.h"
// 自检要读 `pose().position` / `pose().fov`，需要 `CameraPose` 的**完整定义**
// （`SequencerSystem.h` 只前向声明它 —— 见那个文件的说明）。
#include "scene/CameraSystem.h"
#include "scene/Scene.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace sequence {

namespace {

int g_pass = 0;
int g_fail = 0;

// ⚠ detail 里**不能有空格**：验收脚本用 `detail=(\S*)` 解析。
//   多项一律逗号分隔（这是全仓自检的硬约定，见 AnimationSelfTest.cpp）。
void report(const char* name, double actual, double expected, double tol) {
    const bool ok = std::fabs(actual - expected) <= tol;
    VK_LOG_INFO("SEQ-TEST %s actual=%.6f expected=%.6f tol=%.6f result=%s", name,
                actual, expected, tol, ok ? "PASS" : "FAIL");
    if (ok) ++g_pass;
    else ++g_fail;
}

void reportFlag(const char* name, bool ok, const std::string& detail = {}) {
    VK_LOG_INFO("SEQ-TEST %s actual=%d expected=1 tol=0 detail=%s result=%s", name,
                ok ? 1 : 0, detail.c_str(), ok ? "PASS" : "FAIL");
    if (ok) ++g_pass;
    else ++g_fail;
}

constexpr float kDeg = 3.14159265358979323846f / 180.0f;

SequenceAsset parse(const char* text, const char* path = "test.seq.json") {
    SequenceAsset s;
    parseSequence(text, path, s);
    return s;
}

// 两帧的关键帧夹具：t=0 在原点(fov 20)，t=2 在 x=10(fov 60)。
// ease 由参数给，方便同一份夹具跑三组插值断言。
SequenceAsset twoKeys(const char* ease) {
    std::string j = R"({"duration":2.0,"camera":[)";
    j += R"({"t":0,"pos":[0,0,0],"fov":20,"ease":")";
    j += ease;
    j += R"("},)";
    j += R"({"t":2,"pos":[10,0,0],"fov":60})";
    j += "]}";
    return parse(j.c_str());
}

// 一个带 SequencerComponent 的实体（挂到 `sc` 上，返回实体）。
// `json` 直接写进组件 —— 用 `parseSequence` 而不是读盘，用例跑得飞快
// 且不依赖资产目录（这是 `parse*` 与 `load*` 分开的第二个理由）。
ecs::Entity addSeq(scene::Scene& s, const char* name, SequenceAsset asset,
                   int priority = 0, float startTime = 0.0f, bool loop = false) {
    const ecs::Entity e = s.createObject(name);
    ecs::SequencerComponent sc;
    sc.sequencePath = "inline-test";   // 非空即可，走 assetLoaded 短路
    sc.priority = priority;
    sc.startTime = startTime;
    sc.loop = loop;
    sc.asset = std::move(asset);
    sc.assetLoaded = true;             // 跳过读盘
    s.world().add<ecs::SequencerComponent>(e, std::move(sc));
    return e;
}

} // namespace

int runSequencerSelfTest() {
    g_pass = 0;
    g_fail = 0;
    std::printf("SEQ-TEST begin (no window)\n");

    // ============================================================
    // A. 解析
    // ============================================================

    // A1 duration 缺省时按"最晚的关键帧或事件"推算。
    // ⚠ 如果实现只看关键帧，duration 会是 2.0 而不是 3.0 —— 那条 t=3 的
    //   事件就永远不发，且**没有任何报错**。这就是要测它的原因。
    {
        SequenceAsset s = parse(R"({
            "camera":[{"t":0,"pos":[0,0,3]},{"t":2,"pos":[5,0,3]}],
            "events":[{"t":3,"kind":"End"}]
        })");
        report("seq_dur_derived", s.duration, 3.0, 1e-4);
        reportFlag("seq_dur_derived_valid", s.valid);
    }

    // A2 关键帧乱序 → 加载后升序（文件头 ②）。
    {
        SequenceAsset s = parse(R"({"camera":[
            {"t":5,"pos":[5,0,0]},{"t":0,"pos":[0,0,0]},{"t":2,"pos":[2,0,0]}
        ]})");
        report("seq_sorted_first", s.cameraKeys.empty() ? -1.0 : s.cameraKeys[0].t, 0.0,
               1e-4);
        report("seq_sorted_last", s.cameraKeys.empty() ? -1.0 : s.cameraKeys[2].t, 5.0,
               1e-4);
    }

    // A3 不认识的 ease → 当场失败（不"智能匹配"成 smooth）。
    {
        SequenceAsset s = parse(R"({"camera":[{"t":0,"ease":"ease-in-out"}]})");
        reportFlag("seq_bad_ease_rejected", !s.valid);
        reportFlag("seq_bad_ease_mentions_ease",
                   s.error.find("ease") != std::string::npos, s.error);
    }

    // A4 缺 kind 的事件 → 失败（否则派发时 `kind` 是空串，日志里看不出来）。
    {
        SequenceAsset s = parse(R"({"events":[{"t":0.5}]})");
        reportFlag("seq_event_needs_kind", !s.valid);
    }

    // A5 空资产（既没相机也没事件）→ 失败，不该悄悄"播空气"。
    {
        SequenceAsset s = parse(R"({"duration":5.0})");
        reportFlag("seq_empty_rejected", !s.valid);
    }

    // A6 `t == duration` 的事件要进 warnings（文件头 ③）。
    {
        SequenceAsset s = parse(R"({
            "duration":2.0,
            "camera":[{"t":0,"pos":[0,0,3]}],
            "events":[{"t":2.0,"kind":"AtEnd"}]
        })");
        report("seq_warn_at_dur_count", static_cast<double>(s.warnings.size()), 1.0, 0.5);
    }

    // A7 显式 duration 比内容短 → 要报"尾巴会被切掉"。
    {
        SequenceAsset s = parse(R"({
            "duration":1.0,
            "camera":[{"t":0,"pos":[0,0,3]},{"t":5,"pos":[9,0,3]}]
        })");
        report("seq_warn_short_dur_count", static_cast<double>(s.warnings.size()), 1.0,
               0.5);
    }

    // A8 name 缺省取文件名（`opening` 而不是整条路径）。
    {
        SequenceAsset s = parse(R"({"camera":[{"t":0,"pos":[0,0,3]}]})",
                                "sequences/opening.seq.json");
        reportFlag("seq_name_from_path", s.name == "opening", s.name);
    }

    // A9 重复 t 的相邻关键帧 → 一条 warning（退化成硬切，不致命）。
    {
        SequenceAsset s = parse(R"({"camera":[
            {"t":0,"pos":[0,0,3]},{"t":1,"pos":[1,0,3]},{"t":1,"pos":[5,0,3]}
        ]})");
        report("seq_warn_dup_key_count", static_cast<double>(s.warnings.size()), 1.0, 0.5);
    }

    // ============================================================
    // B. 采样
    // ============================================================

    // B1 线性中点 = 中点（x=5, fov=40）。
    {
        SequenceAsset s = twoKeys("linear");
        SeqCameraKey k;
        const bool ok = sampleCamera(s, 1.0f, k);
        reportFlag("seq_sample_linear_ok", ok);
        report("seq_linear_mid_x", k.position.x, 5.0, 1e-4);
        report("seq_linear_mid_fov", k.fov, 40.0, 1e-4);
    }

    // B2 smoothstep 在 u=0.25：linear 给 2.5，smooth 给 1.5625。
    // ⚠ 取中点会**两者都是 5.0** —— 那样这条断言什么都测不到。
    {
        SequenceAsset s = twoKeys("smooth");
        SeqCameraKey k;
        sampleCamera(s, 0.5f, k);   // u = 0.25
        report("seq_smooth_quarter_x", k.position.x, 1.5625, 1e-3);
    }

    // B3 step：段内任意时刻都保持**前帧**（不是 0/1 二分的 u>0.5 就跳）。
    {
        SequenceAsset s = twoKeys("step");
        SeqCameraKey k;
        sampleCamera(s, 1.9f, k);
        report("seq_step_holds_a", k.position.x, 0.0, 1e-4);
        // 到达下一帧的时刻才切过去
        sampleCamera(s, 2.0f, k);
        report("seq_step_at_b", k.position.x, 10.0, 1e-4);
    }

    // B4 ⚠⚠ yaw 必须走最短弧：170° → −170° 的中点应该是 ±180°，不是 0°。
    // 走远路（直接 lerp）会得到 yaw=0 —— 镜头会**原地转一大圈**，
    // 而"转了多少度"在日志里看不出来，只有画面能暴露。
    {
        SequenceAsset s = parse(R"({"camera":[
            {"t":0,"yaw":2.9670597,"pos":[0,0,3],"ease":"linear"},
            {"t":2,"yaw":-2.9670597,"pos":[0,0,3]}
        ]})");
        SeqCameraKey k;
        sampleCamera(s, 1.0f, k);
        report("seq_yaw_short_arc_abs", std::fabs(k.yaw), 3.14159265, 2e-2);
    }

    // B5 边界夹取：早于第一帧取第一帧，晚于最后一帧取最后一帧。
    {
        SequenceAsset s = twoKeys("linear");
        SeqCameraKey k;
        sampleCamera(s, -10.0f, k);
        report("seq_clamp_before", k.position.x, 0.0, 1e-4);
        sampleCamera(s, 100.0f, k);
        report("seq_clamp_after", k.position.x, 10.0, 1e-4);
    }

    // B6 没有关键帧 → false（调用方据此**不覆盖**相机，而不是把相机挪到原点）。
    {
        SequenceAsset s = parse(R"({"events":[{"t":0.1,"kind":"Only"}]})");
        SeqCameraKey k;
        reportFlag("seq_no_camera_returns_false", !sampleCamera(s, 0.0f, k));
    }

    // ============================================================
    // C. 事件区间
    // ============================================================

    // C1 半开 [from,to)：t=1.0 算在内，t=2.0 不算。
    {
        SequenceAsset s = parse(R"({"duration":5,"events":[
            {"t":1.0,"kind":"A"},{"t":2.0,"kind":"B"}
        ]})");
        std::vector<int> ids;
        const int n = collectEvents(s, 1.0f, 2.0f, &ids);
        report("seq_ev_halfopen_count", n, 1.0, 0.5);
        reportFlag("seq_ev_halfopen_is_a",
                   ids.size() == 1 && s.events[static_cast<std::size_t>(ids[0])].kind == "A");
    }

    // C2 空区间（from == to）不发。
    {
        SequenceAsset s = parse(R"({"duration":5,"events":[{"t":1.0,"kind":"A"}]})");
        report("seq_ev_empty_range", collectEvents(s, 1.0f, 1.0f, nullptr), 0.0, 0.5);
    }

    // C3 跨圈要拆两段扫，两段合起来不漏。
    // 场景：dur=2，事件在 0.5 与 1.5；播放头从 1.8 跨圈到 0.3。
    //   第一段 [1.8, 2.0) → 0 条；第二段 [0, 0.3) → 0 条。共 0。
    // 再走一步 [0.3, 0.6) → 命中 0.5，1 条。
    // ⚠ 如果实现只扫 `[prev, next)` 且 prev > next，**一条都不发** → 循环
    //   运镜里每圈少一个音效，只在跨越那一刻少，极难定位。
    {
        SequenceAsset s = parse(R"({"duration":2,"events":[
            {"t":0.5,"kind":"L"},{"t":1.5,"kind":"R"}
        ]})");
        std::vector<int> a;
        collectEvents(s, 1.8f, 2.0f, &a);
        collectEvents(s, 0.0f, 0.3f, &a);
        report("seq_ev_wrap_none", static_cast<double>(a.size()), 0.0, 0.5);

        std::vector<int> b;
        collectEvents(s, 1.8f, 2.0f, &b);
        collectEvents(s, 0.0f, 0.6f, &b);
        report("seq_ev_wrap_left", static_cast<double>(b.size()), 1.0, 0.5);
    }

    // ============================================================
    // D. 播放器
    // ============================================================

    // D1 推进：dt=0.1 → playhead 0.1。
    {
        scene::Scene s;
        addSeq(s, "Seq", twoKeys("linear"));
        SequencerSystem sys;
        sys.tick(s, 0.1f);
        report("seq_tick_advance", sys.lastPlayhead(), 0.1, 1e-4);
        report("seq_tick_running", sys.lastRunning(), 1.0, 0.5);
    }

    // D2 首帧要发 t=0 的事件（"开场音效/字幕"就写在这里）。
    // ⚠ 把 prev 初值写成 playhead 的话，t=0 永远落在区间外 → 开场静音。
    {
        scene::Scene s;
        std::string j = R"({"duration":2,"camera":[{"t":0,"pos":[0,0,3]}],"events":[
            {"t":0.0,"kind":"Open"},{"t":1.0,"kind":"Later"}]})";
        addSeq(s, "Seq", parse(j.c_str()));
        SequencerSystem sys;
        sys.tick(s, 0.016f);
        report("seq_tick_first_frame_t0", static_cast<double>(sys.firedEvents().size()),
               1.0, 0.5);
    }

    // D3 从 startTime=2.0 起播 → 1.5 秒那个"过去"的事件**不该**补发。
    {
        scene::Scene s;
        std::string j = R"({"duration":4,"camera":[{"t":0,"pos":[0,0,3]}],"events":[
            {"t":1.5,"kind":"Past"},{"t":2.5,"kind":"Next"}]})";
        addSeq(s, "Seq", parse(j.c_str()), 0, 2.0f);
        SequencerSystem sys;
        sys.tick(s, 0.016f);
        report("seq_tick_no_backfill", static_cast<double>(sys.firedEvents().size()),
               0.0, 0.5);
        report("seq_tick_start_playhead", sys.lastPlayhead(), 2.0 + 0.016, 1e-3);
    }

    // D4 非循环走到结尾：playhead 夹在 duration，finished 生效。
    // ⚠ kMaxStep=0.10 会把每帧的推进夹住，所以要 tick 多次 —— 这条同时
    //   验证了"夹步长"这件事（一次 tick(2.0) 只前进 0.1）。
    {
        scene::Scene s;
        std::string j = R"({"duration":1,"camera":[
            {"t":0,"pos":[0,0,3]},{"t":1,"pos":[1,0,3]}]})";
        const ecs::Entity e = addSeq(s, "Seq", parse(j.c_str()));
        SequencerSystem sys;
        for (int i = 0; i < 12; ++i) sys.tick(s, 0.1f);
        const auto* sc = s.world().get<ecs::SequencerComponent>(e);
        // ⚠ 读**组件**的 playhead，不是 `sys.lastPlayhead()`。
        //   `lastPlayhead` 是"本帧"统计：播完之后 selectActive 不再选中它，
        //   tick 开头会把它清成 0 —— 拿它断言就成了"断言一个每帧归零的量"，
        //   永远 0（第一版就是这么红的）。帧统计用于日志，状态用组件读。
        report("seq_tick_clamp_step", sc ? sc->playhead : -1.0, 1.0, 1e-4);
        reportFlag("seq_tick_finished", sc && sc->finished);
        // 结束后不再算"在跑"（这一条**才**该看帧统计）
        sys.tick(s, 0.1f);
        report("seq_tick_finished_not_running", sys.lastRunning(), 0.0, 0.5);
        // ⚠ 播完必须**丢掉**姿态：留着上一帧的 pose，调用方就会把相机
        //   冻在结尾那个机位上 —— 玩家再也拿不回镜头，症状是"过场结束
        //   后人物在动、画面不动"。
        reportFlag("seq_tick_finished_no_pose", !sys.hasPose());
    }

    // D5 循环：跨过结尾后折回开头。
    {
        scene::Scene s;
        std::string j = R"({"duration":0.5,"loop":true,"camera":[
            {"t":0,"pos":[0,0,3]},{"t":0.5,"pos":[1,0,3]}]})";
        const ecs::Entity e = addSeq(s, "Seq", parse(j.c_str()), 0, 0.0f, true);
        SequencerSystem sys;
        for (int i = 0; i < 7; ++i) sys.tick(s, 0.1f);   // 0.7s → 跨 0.5 一次
        const auto* sc = s.world().get<ecs::SequencerComponent>(e);
        report("seq_tick_loop_wrapped", sys.lastPlayhead(), 0.2, 1e-3);
        reportFlag("seq_tick_loop_not_finished", sc && !sc->finished);
    }

    // D6 编辑态（dt=0）：不推进、不发事件，但相机姿态要跟着播放头。
    // ⚠ 与 B3「dt==0 不提取根运动」相反 —— 那里的跳帧是噪声，这里的跳帧
    //   就是用户拖滑条想看的东西。
    {
        scene::Scene s;
        const ecs::Entity e = addSeq(s, "Seq", twoKeys("linear"));
        auto* sc = s.world().get<ecs::SequencerComponent>(e);
        sc->playhead = 1.0f;
        SequencerSystem sys;
        sys.tick(s, 0.0f);
        report("seq_edit_no_advance", sc->playhead, 1.0, 1e-6);
        reportFlag("seq_edit_has_pose", sys.hasPose());
        report("seq_edit_desired_x", sys.pose().position.x, 5.0, 1e-3);
        report("seq_edit_no_events", static_cast<double>(sys.firedEvents().size()), 0.0,
               0.5);
    }

    // D7 优先级：高的那段在跑（与 CameraComponent::priority 无关）。
    {
        scene::Scene s;
        std::string lo = R"({"name":"lo","duration":2,"camera":[{"t":0,"pos":[1,0,3]}]})";
        std::string hi = R"({"name":"hi","duration":2,"camera":[{"t":0,"pos":[7,0,3]}]})";
        addSeq(s, "Low", parse(lo.c_str()), 0);
        addSeq(s, "High", parse(hi.c_str()), 5);
        SequencerSystem sys;
        sys.tick(s, 0.1f);
        report("seq_priority_running", sys.lastRunning(), 2.0, 0.5);
        report("seq_priority_pick_x", sys.pose().position.x, 7.0, 1e-3);
        reportFlag("seq_priority_name", sys.lastSequenceName() == "hi",
                   sys.lastSequenceName());
    }

    // D8 相机真的被接管（desired 是从时间轴来的，不是场景里原来那台）。
    {
        scene::Scene s;
        std::string j = R"({"duration":2,"camera":[
            {"t":0,"pos":[0,0,9],"yaw":0,"pitch":-0.5,"fov":70},
            {"t":2,"pos":[20,0,9],"yaw":0.7,"pitch":-0.5,"fov":70}]})";
        addSeq(s, "Seq", parse(j.c_str()));
        SequencerSystem sys;
        sys.tick(s, 0.1f);
        // t=0.1 → u=0.05 → smoothstep(0.05)=0.00725 → x≈0.145
        reportFlag("seq_takeover_has_pose", sys.hasPose());
        report("seq_takeover_x", sys.pose().position.x, 0.145, 2e-3);
        report("seq_takeover_fov", sys.pose().fov, 70.0, 1e-3);
    }

    const int failures = g_fail;
    VK_LOG_INFO("SEQ-TOTAL pass=%d fail=%d", g_pass, g_fail);
    return failures;
}

} // namespace sequence
