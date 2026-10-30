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
    // B7+ R5（2026-10-28）：插值统一到 `animation::Curve` 之后，
    // **老三个模式必须逐位不变**；新增的 bezier 手柄必须真的改曲线。
    // ============================================================

    // B7 迁移锚：linear / smooth / step 在六个采样点上与本文件的
    // 独立参考实现（`smoothstep01` 那份副本 + 逐字复述的老公式）**逐位**相同。
    // ⚠ 这是"把三份插值合成一份"唯一可信的证据 —— 任何一条漂了 1e-7，
    //   老资产的运镜就整体变形，而屏幕上看不出来。
    {
        // ⚠ 这个副本**故意**不调用 `SequenceAsset.cpp` 里那份（它是
        //   anonymous namespace，本来就外部不可见）—— 参考实现必须独立，
        //   拿新实现和新实现比是循环论证。
        auto legacySmooth = [](float u) { return u * u * (3.0f - 2.0f * u); };

        bool bitwise = true;
        float worstU = 0.0f;
        for (int i = 0; i <= 10; ++i) {
            const float u = static_cast<float>(i) / 10.0f;
            const float t = u * 2.0f;

            SequenceAsset lin = twoKeys("linear");
            SeqCameraKey k;
            sampleCamera(lin, t, k);
            if (k.position.x != 0.0f + (10.0f - 0.0f) * u) { bitwise = false; worstU = u; }

            SequenceAsset sm = twoKeys("smooth");
            sampleCamera(sm, t, k);
            if (k.position.x != 0.0f + (10.0f - 0.0f) * legacySmooth(u)) {
                bitwise = false; worstU = u;
            }
        }
        reportFlag("seq_sample_legacy_bitwise", bitwise,
                   "worst_u=" + std::to_string(worstU));

        // step 的语义是"选择"：段内保持 a（连 `out.t` 也是 a.t）
        SequenceAsset st = twoKeys("step");
        SeqCameraKey k;
        sampleCamera(st, 1.5f, k);
        reportFlag("seq_step_keeps_a_time", k.t == 0.0f && k.position.x == 0.0f,
                   "t=" + std::to_string(k.t));
    }

    // B8 默认手柄的 bezier ≈ ease-in-out：u=0.25 处 x=1.5625（与 smooth 同值）。
    // 这一条同时说明"bezier 不是随便画一条线"—— 它落点对了。
    {
        SequenceAsset s = parse(R"({"camera":[
            {"t":0,"pos":[0,0,3],"ease":"bezier"},
            {"t":2,"pos":[10,0,3]}
        ]})");
        SeqCameraKey k;
        sampleCamera(s, 0.5f, k);   // u = 0.25
        report("seq_bezier_default_quarter_x", k.position.x, 1.5625, 1e-3);
    }

    // B9 手柄真的改曲线：把手柄拉平（y=0 → 起步更慢）之后，同一时刻的值
    // 必须**更小**。否定测试的写法：只断"变了"是不够的，还要断方向对。
    {
        SequenceAsset fast = parse(R"({"camera":[
            {"t":0,"pos":[0,0,3],"ease":"bezier",
             "outHandle":[0.02,0.9],"inHandle":[-0.6,0.0]},
            {"t":2,"pos":[10,0,3]}
        ]})");
        SequenceAsset slow = parse(R"({"camera":[
            {"t":0,"pos":[0,0,3],"ease":"bezier",
             "outHandle":[0.6,0.0],"inHandle":[-0.02,0.0]},
            {"t":2,"pos":[10,0,3]}
        ]})");
        SeqCameraKey a, b;
        sampleCamera(fast, 0.5f, a);
        sampleCamera(slow, 0.5f, b);
        reportFlag("seq_bezier_handle_changes_shape", a.position.x > b.position.x + 1.0f,
                   "fast=" + std::to_string(a.position.x) +
                       ",slow=" + std::to_string(b.position.x));
    }

    // B10 写反的手柄要**钳回**而不是让整份资产打不开（x 符号约束）。
    {
        SequenceAsset s = parse(R"({"camera":[
            {"t":0,"pos":[0,0,3],"ease":"bezier",
             "outHandle":[-0.5,0.0],"inHandle":[0.5,0.0]},
            {"t":2,"pos":[10,0,3]}
        ]})");
        reportFlag("seq_bezier_handle_clamped",
                   s.valid && s.cameraKeys[0].outHandle.x == 0.0f &&
                       s.cameraKeys[0].inHandle.x == 0.0f,
                   "out=" + std::to_string(s.cameraKeys.empty()
                                               ? -1.0f
                                               : s.cameraKeys[0].outHandle.x));
    }

    // B11 手柄只在 bezier 上写出；round-trip 之后逐字段相等。
    {
        SequenceAsset s = parse(R"({"camera":[
            {"t":0,"pos":[0,0,3],"ease":"bezier",
             "outHandle":[0.25,0.75],"inHandle":[-0.4,-0.2]},
            {"t":2,"pos":[10,0,3],"ease":"smooth"}
        ]})");
        const std::string text = toJson(s);
        SequenceAsset back = parse(text.c_str());
        const bool handlesOk =
            back.valid && back.cameraKeys.size() == 2 &&
            back.cameraKeys[0].outHandle.x == 0.25f &&
            back.cameraKeys[0].outHandle.y == 0.75f &&
            back.cameraKeys[0].inHandle.x == -0.4f &&
            back.cameraKeys[0].inHandle.y == -0.2f;
        reportFlag("seq_bezier_roundtrip_handles", handlesOk, "ok=1");
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

    // ============================================================
    // E. 点播（C6 脚本导演 API）
    // ============================================================
    // ⚠ 这一组的场景**一个 SequencerComponent 都不建** —— 它同时钉住
    //   "点播不依赖场景里的实体"。真要按组件走的话 `selectActive` 扫不到
    //   任何东西，`tick` 直接 return，playhead 永远 0。

    // E1 点播成功：返回 true、`cuePlaying()` 为真、tick 之后播放头在走。
    {
        scene::Scene s;
        SequencerSystem sys;
        const bool ok = sys.cue(s, "sequences/opening.seq.json");
        reportFlag("seq_cue_ok", ok, sys.cuePath());
        reportFlag("seq_cue_playing", sys.cuePlaying(), sys.cuePath());

        sys.tick(s, 0.1f);
        report("seq_cue_playhead", sys.lastPlayhead(), 0.1, 1e-3);
        report("seq_cue_running", sys.lastRunning(), 1.0, 0.5);
        reportFlag("seq_cue_has_pose", sys.hasPose(), "opening 首帧应有相机键");
    }

    // E2 资产不存在 → false，且**不进播放态**（否则 tick 会一直空转）。
    {
        scene::Scene s;
        SequencerSystem sys;
        const bool ok = sys.cue(s, "sequences/__no_such_file__.seq.json");
        reportFlag("seq_cue_missing_false", !ok, "不存在的资产必须返回 false");
        reportFlag("seq_cue_missing_not_playing", !sys.cuePlaying(), "");
        sys.tick(s, 0.1f);
        report("seq_cue_missing_no_playhead", sys.lastPlayhead(), 0.0, 1e-6);
    }

    // E3 播完自动让位：`cuePlaying()` 变回 false。
    // ⚠ 判据读的是**访问器**，不是 `lastRunning` —— 后者每帧被清。
    //   opening 是 8 秒，kMaxStep=0.10 让每帧最多走 0.1 秒，所以要 80+ 帧。
    {
        scene::Scene s;
        SequencerSystem sys;
        sys.cue(s, "sequences/opening.seq.json");
        for (int i = 0; i < 120; ++i) sys.tick(s, 0.1f);
        reportFlag("seq_cue_finished_releases", !sys.cuePlaying(),
                   "播完必须把播放权还给场景组件");
    }

    // E4 点播压过场景组件（脚本说"现在播这段"就是现在播）。
    // 场景里放一个 priority=99 的组件，点播仍应胜出 —— 胜出的证据是
    // 播放头按 cue 那段走（而不是场景那段 1 秒的）。
    {
        scene::Scene s;
        std::string j = R"({"duration":1,"camera":[{"t":0,"pos":[99,0,3]}]})";
        addSeq(s, "SceneSeq", parse(j.c_str()), 99);
        SequencerSystem sys;
        sys.cue(s, "sequences/opening.seq.json");
        sys.tick(s, 0.1f);
        reportFlag("seq_cue_beats_component",
                   sys.lastSequenceName() == "opening",
                   "点播应胜过 priority=99 的场景组件：" + sys.lastSequenceName());
    }

    // ========================================================
    // F 写回往返（#311 时间轴编辑器）
    //
    // 判据只有一句：**`toJson` 之后 `parseSequence` 一遍，内容逐字段相等。**
    // 这不是"顺便测一下"—— 编辑器每次改动都走这条路做校验（与剧本编辑器
    // 同一条纪律），这里红了就代表"面板上看到的"与"存进磁盘的"是两份东西。
    // ========================================================
    {
        // 造一份带**所有字段非默认**的资产：默认值会让断言失去分辨力
        //（`ease` 若取默认 smooth，"写没写 ease" 都读回 smooth）。
        SequenceAsset src;
        src.name = "roundtrip";
        src.loop = true;
        src.duration = 12.5f;
        SeqCameraKey k0;
        k0.t = 0.0f;
        k0.position = glm::vec3(1.5f, -2.25f, 3.75f);
        k0.yaw = 0.8125f;
        k0.pitch = -0.4375f;
        k0.fov = 37.5f;
        k0.ease = Ease::Step;
        SeqCameraKey k1;
        k1.t = 4.25f;
        k1.position = glm::vec3(-6.0f, 0.5f, 2.0f);
        k1.yaw = -2.5f;
        k1.pitch = 0.125f;
        k1.fov = 52.0f;
        k1.ease = Ease::Linear;
        src.cameraKeys.push_back(k0);
        src.cameraKeys.push_back(k1);
        src.events.push_back(SeqEvent{1.0f, "Sound", "se_footstep"});
        src.events.push_back(SeqEvent{4.25f, "Line", "千夏：小心脚下"});

        const std::string text = toJson(src);
        SequenceAsset dst;
        const bool parsed = parseSequence(text, "roundtrip.seq.json", dst);

        // F1 框架层往返：能被读回、且 cameraKeys 全字段相等
        bool keysEqual = parsed && dst.cameraKeys.size() == 2;
        if (keysEqual) {
            for (int i = 0; i < 2; ++i) {
                const SeqCameraKey& a = src.cameraKeys[static_cast<std::size_t>(i)];
                const SeqCameraKey& b = dst.cameraKeys[static_cast<std::size_t>(i)];
                keysEqual = keysEqual && std::fabs(a.t - b.t) < 1.0e-4f &&
                            std::fabs(a.position.x - b.position.x) < 1.0e-5f &&
                            std::fabs(a.position.y - b.position.y) < 1.0e-5f &&
                            std::fabs(a.position.z - b.position.z) < 1.0e-5f &&
                            std::fabs(a.yaw - b.yaw) < 1.0e-5f &&
                            std::fabs(a.pitch - b.pitch) < 1.0e-5f &&
                            std::fabs(a.fov - b.fov) < 1.0e-5f && a.ease == b.ease;
            }
        }
        char f1[160];
        std::snprintf(f1, sizeof(f1), "parsed=%d,keys=%d,dur=%.3f", parsed ? 1 : 0,
                      static_cast<int>(dst.cameraKeys.size()),
                      static_cast<double>(dst.duration));
        reportFlag("rt_camera_keys", keysEqual, f1);

        // F2 事件往返（含中文 payload：ensure_ascii=false 才留得住）
        const bool eventsEqual = dst.events.size() == 2 &&
                                 dst.events[0].kind == "Sound" &&
                                 dst.events[0].payload == "se_footstep" &&
                                 dst.events[1].kind == "Line" &&
                                 dst.events[1].payload == "千夏：小心脚下";
        char f2[128];
        std::snprintf(f2, sizeof(f2), "events=%d", static_cast<int>(dst.events.size()));
        reportFlag("rt_events", eventsEqual, f2);

        // F3 duration 是**显式**写出的，不是按内容推算
        //    内容最晚才 4.25s，而 duration=12.5 —— 只有写了才可能读回 12.5
        char f3[96];
        std::snprintf(f3, sizeof(f3), "dur=%.3f", static_cast<double>(dst.duration));
        reportFlag("rt_duration_explicit", std::fabs(dst.duration - 12.5f) < 1.0e-4f, f3);

        // F4 诊断字段不进文件（`valid` / `error` / `sourcePath` 都不该出现）
        const bool noDiag = text.find("\"valid\"") == std::string::npos &&
                            text.find("\"error\"") == std::string::npos &&
                            text.find("\"sourcePath\"") == std::string::npos &&
                            text.find("\"warnings\"") == std::string::npos;
        reportFlag("rt_no_diagnostics", noDiag, "");

        // F5 中文没被转义成 \uXXXX（转义之后 git diff 全废）
        const bool rawUtf8 = text.find("千夏") != std::string::npos &&
                             text.find("\\u") == std::string::npos;
        reportFlag("rt_utf8_raw", rawUtf8, "");

        // F6 `ease` 总是写出来（默认 smooth 也要写：它是"这两帧怎么走"的
        //    唯一表述，省略就得翻代码查默认值）
        const bool easeWritten = text.find("\"ease\": \"step\"") != std::string::npos &&
                                 text.find("\"ease\": \"linear\"") != std::string::npos;
        reportFlag("rt_ease_always_written", easeWritten, "");

        // F7 空的 payload 整个字段省略（不写 `"payload": ""`）
        SequenceAsset noPayload;
        noPayload.duration = 1.0f;
        noPayload.cameraKeys.push_back(SeqCameraKey{});
        noPayload.events.push_back(SeqEvent{0.5f, "Flag", ""});
        const std::string t7 = toJson(noPayload);
        reportFlag("rt_empty_payload_omitted",
                   t7.find("\"payload\"") == std::string::npos, "");

        // F8 作者的 `"//"` 说明必须原样往返。
        //    JSON 没有注释语法，`opening.seq.json` 里那段"yaw 是弧度、step
        //    的切点落在下一帧"是用 `"//"` 键写的**唯一一份格式说明**。
        //    不往返的话：作者第一次按 Save，注释就无声消失了。
        SequenceAsset withNote;
        withNote.comment = "yaw is radians; step cuts at the NEXT key";
        withNote.duration = 2.0f;
        withNote.cameraKeys.push_back(SeqCameraKey{});
        const std::string t8 = toJson(withNote);
        SequenceAsset back8;
        parseSequence(t8, "note.seq.json", back8);
        reportFlag("rt_comment_roundtrip",
                   back8.comment == withNote.comment && back8.valid,
                   back8.comment.size() > 40 ? "len=long" : "len=short,got=empty");

        // F9 没写过注释的资产**不要**凭空长出一个空的 `"//": ""`
        reportFlag("rt_empty_comment_omitted",
                   toJson(noPayload).find("\"//\"") == std::string::npos, "");
    }

    // ============================================================
    // G. 多机位 marker（运镜体系 §七，2026-10-30）
    // ============================================================
    //
    // 这一组钉的是**"加了功能但不改老资产"**。G2 用字节比对、G9 用访问器
    // 比对：没有 `shots` 键的资产从解析 → 写回 → 播放，行为必须逐位不变。
    // 换句话说，下面每一条都可能被"多加了一个默认分支"弄红。
    {
        // 16 位十六进制（`parseGuidHex` 的长度要求，见 G6 的反向用例）
        const char* kGuidHex = "00000000000a1b2c";
        const std::uint64_t kGuid = 0x0a1b2cull;

        // G1 四个字段都解析出来。
        {
            SequenceAsset s = parse(R"({
                "camera":[{"t":0,"pos":[0,0,3]}],
                "shots":[{"t":1.0,"camera":"00000000000a1b2c",
                          "blend":0.4,"label":"A 特写"}]
            })");
            char d[128];
            std::snprintf(d, sizeof(d), "n=%d,guid=%s,blend=%.2f,label=%s",
                          static_cast<int>(s.shots.size()),
                          s.shots.empty()
                              ? "-"
                              : ecs::guidToHex(s.shots[0].cameraGuid).c_str(),
                          s.shots.empty() ? -1.0
                                          : static_cast<double>(s.shots[0].blendSeconds),
                          s.shots.empty() ? "-" : s.shots[0].label.c_str());
            reportFlag("seq_shot_parse_fields",
                       s.valid && s.shots.size() == 1 &&
                           s.shots[0].cameraGuid == kGuid &&
                           std::fabs(s.shots[0].blendSeconds - 0.4f) < 1e-5f &&
                           s.shots[0].label == "A 特写",
                       d);
            // 中文 label 写回时**不许**转义成 \uXXXX（转义之后 git diff 全废，
            // 而且作者在文件里就看不懂自己的标签了）。
            reportFlag("seq_shot_label_utf8",
                       toJson(s).find("A 特写") != std::string::npos,
                       "raw-utf8");
        }

        // G2 老资产（没有 shots 键）写回时**不许**长出 `"shots": []`。
        // ⚠ 这条是"字节级"判据：一旦写出空数组，所有既有 .seq.json 的
        //   diff 立刻变脏，而且 `rt_*` 那组"写回再读回"的比对也会失真。
        {
            SequenceAsset s = twoKeys("linear");
            const std::string text = toJson(s);
            reportFlag("seq_shot_absent_not_written",
                       text.find("\"shots\"") == std::string::npos,
                       "no-empty-array");
            SequenceAsset back;
            parseSequence(text, "t.seq.json", back);
            reportFlag("seq_shot_absent_roundtrip",
                       back.valid && back.shots.empty() &&
                           std::fabs(back.duration - 2.0f) < 1e-5f,
                       back.valid ? "valid" : "invalid");
        }

        // G3 duration 推算要把 marker 算进去。
        // ⚠ 只看关键帧的话会得到 2.0，最后那个 marker（3.5）永远不生效 ——
        //   而它恰恰是"切到片尾那台机位"的那一下。
        {
            SequenceAsset s = parse(R"({
                "camera":[{"t":0,"pos":[0,0,3]},{"t":2,"pos":[5,0,3]}],
                "shots":[{"t":1.0},{"t":3.5}]
            })");
            report("seq_shot_dur_derived", s.duration, 3.5, 1e-4);
        }

        // G4 乱序输入按 t 升序排好（作者手写 JSON 时很常见）。
        {
            SequenceAsset s = parse(R"({
                "camera":[{"t":0,"pos":[0,0,3]}],
                "shots":[{"t":2.0,"label":"B"},{"t":0.5,"label":"A"}]
            })");
            reportFlag("seq_shot_sorted",
                       s.shots.size() == 2 && s.shots[0].t < s.shots[1].t &&
                           s.shots[0].label == "A" && s.shots[1].label == "B",
                       s.shots.size() == 2 ? s.shots[0].label.c_str() : "-");
        }

        // G5 `shotIndexAt` 的区间是 **[marker.t, 下一个 marker.t)**，
        //    返回的是**0 基下标**（−1 专门留给"一个 marker 都不在生效"），
        //    并且**早于第一个 marker 返回 −1**（不是 0）。
        // ⚠⚠ 这两条是这一组最该测的：照抄 `cameraKeyIndexAt`（早于首帧返回 0）
        //    的话，开场黑场那一秒会突然亮出第一个机位的画面。
        // ⚠ 第一版这里把期望写成了 1/1/2/2（把"第几个"当成"下标"）→ 四条全红。
        //   红的时候先怀疑期望：返回 0 表示"0 号 marker 生效"才是对的。
        {
            SequenceAsset s = parse(R"({
                "camera":[{"t":0,"pos":[0,0,3]}],
                "shots":[{"t":1.0},{"t":2.0}]
            })");
            report("seq_shot_idx_before_first", shotIndexAt(s, 0.4f), -1.0, 0.5);
            report("seq_shot_idx_at_boundary", shotIndexAt(s, 1.0f), 0.0, 0.5);
            report("seq_shot_idx_inside", shotIndexAt(s, 1.999f), 0.0, 0.5);
            report("seq_shot_idx_at_second", shotIndexAt(s, 2.0f), 1.0, 0.5);
            report("seq_shot_idx_past_end", shotIndexAt(s, 9.0f), 1.0, 0.5);
        }

        // G6 短 guid（手写成 `a1b2c`）→ **不崩**，回落 + 一条警告。
        //    ⚠ 这条必须留：`parseGuidHex` 长度不足时返回 0，于是"手写一个
        //      短 guid"会静默退化成老行为（画面照常动，只是不切机位）。
        //      有警告才有人查得出来。
        {
            SequenceAsset s = parse(R"({
                "camera":[{"t":0,"pos":[0,0,3]}],
                "shots":[{"t":0.0,"camera":"a1b2c"}]
            })");
            reportFlag("seq_shot_bad_guid_warns",
                       s.valid && s.shots.size() == 1 &&
                           s.shots[0].cameraGuid == 0 && !s.warnings.empty(),
                       s.warnings.empty() ? "no-warning" : "warned");
        }

        // G7 写回再读回：guid / blend / label 逐字段相等，且 guid 是
        //    **16 位小写 hex 字符串**（不是 JSON number —— 中间任何工具都会
        //    把 uint64 截成 float）。
        {
            SequenceAsset a;
            a.duration = 4.0f;
            a.cameraKeys.push_back(SeqCameraKey{});
            SeqShot sh;
            sh.t = 1.5f;
            sh.cameraGuid = kGuid;
            sh.blendSeconds = 0.25f;
            sh.label = "B";
            a.shots.push_back(sh);
            const std::string text = toJson(a);
            reportFlag("seq_shot_guid_is_hex_string",
                       text.find(std::string("\"camera\": \"") + kGuidHex + "\"") !=
                           std::string::npos,
                       "hex16");
            SequenceAsset b;
            parseSequence(text, "rt.seq.json", b);
            reportFlag("seq_shot_roundtrip",
                       b.valid && b.shots.size() == 1 &&
                           b.shots[0].cameraGuid == kGuid &&
                           std::fabs(b.shots[0].blendSeconds - 0.25f) < 1e-5f &&
                           b.shots[0].label == "B" &&
                           std::fabs(b.shots[0].t - 1.5f) < 1e-5f,
                       b.shots.empty() ? "n=0" : "ok");
        }

        // G8 播放器把"这一帧用哪台"报出来（宿主拿它去驱动导演覆盖）。
        //    marker 在 0.5s 与 2.0s。
        // ⚠⚠ **每帧最多走 `kMaxStep`=0.10 秒**（见 `SequencerSystem.cpp`），
        //    所以这里必须用小步累加，不能 `tick(s, 0.6f)` 一把推到 0.7 ——
        //    那样实际只到 0.2，断言会红在"根本没走到 marker"上，而人以为是
        //    切镜逻辑坏了。
        {
            scene::Scene s;
            addSeq(s, "Seq", parse(R"({
                "duration":3.0,
                "camera":[{"t":0,"pos":[0,0,3]},{"t":3,"pos":[9,0,3]}],
                "shots":[{"t":0.5,"camera":"00000000000a1b2c","blend":0.3},
                         {"t":2.0}]
            })"));
            SequencerSystem sys;
            auto run = [&](int steps) {
                for (int i = 0; i < steps; ++i) sys.tick(s, 0.1f);
            };
            run(4);                     // playhead 0.4 —— 早于第一个 marker
            report("seq_shot_player_before", sys.shotIndex(), -1.0, 0.5);
            report("seq_shot_player_cam_before",
                   static_cast<double>(sys.shotCameraGuid()), 0.0, 0.5);

            run(3);                     // 0.7 —— 落在 0 号上
            report("seq_shot_player_idx0", sys.shotIndex(), 0.0, 0.5);
            reportFlag("seq_shot_player_cam0", sys.shotCameraGuid() == kGuid,
                       ecs::guidToHex(sys.shotCameraGuid()).c_str());
            report("seq_shot_player_blend0", sys.shotBlendSeconds(), 0.3, 1e-4);

            run(15);                    // 2.2 —— 1 号（没绑 → 0 = 回落关键帧轨）
            report("seq_shot_player_idx1", sys.shotIndex(), 1.0, 0.5);
            reportFlag("seq_shot_player_cam1_released", sys.shotCameraGuid() == 0,
                       "keytrack");
        }

        // G9 老资产 tick 之后三个输出恒为 −1 / 0 / 0 → 宿主**一次事都不做**。
        //    这是"多机位不影响既有过场"的运行时侧判据。
        {
            scene::Scene s;
            addSeq(s, "Seq", twoKeys("linear"));
            SequencerSystem sys;
            sys.tick(s, 0.5f);
            report("seq_shot_none_idx", sys.shotIndex(), -1.0, 0.5);
            report("seq_shot_none_cam",
                   static_cast<double>(sys.shotCameraGuid()), 0.0, 0.5);
            report("seq_shot_none_blend", sys.shotBlendSeconds(), 0.0, 1e-6);
        }
    }

    const int failures = g_fail;
    VK_LOG_INFO("SEQ-TOTAL pass=%d fail=%d", g_pass, g_fail);
    return failures;
}

} // namespace sequence
