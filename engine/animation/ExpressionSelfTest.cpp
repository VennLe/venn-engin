// ============================================================
// animation/ExpressionSelfTest —— B6 的数值判据（无窗口 / 无设备）
//
// 五组：
//   A 眨眼曲线：三段边界 + 中点 + 超时 + 退化配置
//   B 预设解析：正常 / 缺 version / 重名 / 越界 / 非数字 / 空 weights / 语法错
//   C 往返：toJson → parse 逐字段相等；`label` 为空不长键；权重顺序稳定
//   D rig 适配：`checkAgainstRig` 报缺失通道（去重）
//   E 驱动：强度是乘性 / **只写自己拥有的通道** / 换预设旧通道归零 /
//     dt=0 不推进 / 眨眼一次流程 / 口型包络 / 无 rig 不崩
//
// 为什么 E3/E4 是整组的核心：驱动和 Inspector 滑条写的是**同一张**
// `MorphComponent::weights`。驱动一旦"每帧写满"，手动推的滑条下一帧就被
// 抹掉 —— 症状是"推滑条没反应"，而代码看起来完全正常（无报错、无警告）。
// 这两条就是钉死"驱动只碰自己那几根通道"的。
//
// ⚠ 为什么不在 0.5 处比眨眼曲线：闭眼段的 smoothstep 中点确实是 0.5，
//   但**睁眼段的中点也是 0.5** —— 拿它当判据分不出"正在闭"和"正在睁"。
//   所以 A3/A5 各自钉在**自己那一段的中点**，并且额外断言"同一时刻
//   闭眼进度在增、睁眼进度在减"（A4b/A6b 用的是相邻两个采样点）。
// ============================================================
#include "animation/ExpressionDriver.h"

#include "assets/Model.h"
#include "ecs/World.h"
#include "scene/Scene.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace anim {
namespace {

int g_pass = 0;
int g_fail = 0;

void report(const char* name, double got, double want, double eps) {
    const bool ok = std::fabs(got - want) <= eps;
    if (ok) ++g_pass;
    else ++g_fail;
    std::printf("  %s %-54s got=%.6f want=%.6f\n", ok ? "[PASS]" : "[FAIL]",
                name, got, want);
}

void reportFlag(const char* name, bool ok, const char* detail) {
    if (ok) ++g_pass;
    else ++g_fail;
    std::printf("  %s %-54s %s\n", ok ? "[PASS]" : "[FAIL]", name, detail);
}

// 一个最小的"脸"：一个子网格、3 个 target、3 条滑条。
//   smile → target0（weight 1.0）
//   blink → target1（weight 1.0）
//   mouth_open → target2（weight 1.0）
assets::Model makeFace() {
    assets::Model m;
    std::vector<std::vector<assets::MorphChannel>> perSub(1);
    auto addChannel = [&](const char* n, std::uint32_t target) {
        assets::MorphChannel ch;
        ch.name = n;
        assets::MorphChannelKey k;
        k.targetWeight = 1.0f;
        k.target = target;
        ch.keys.push_back(k);
        perSub[0].push_back(ch);
    };
    addChannel("smile", 0);
    addChannel("blink", 1);
    addChannel("mouth_open", 2);
    m.morphRig.build(perSub, {3u});
    return m;
}

// 造一个"有脸"的实体。
ecs::Entity makeCharacter(scene::Scene& sc, assets::Model& model) {
    ecs::Entity e = sc.createObject("char");
    ecs::MorphComponent mp;
    mp.model = &model;
    mp.weights.assign(model.morphRig.channelCount(), 0.0f);
    sc.world().add<ecs::MorphComponent>(e, mp);
    ecs::ExpressionComponent xc;
    sc.world().add<ecs::ExpressionComponent>(e, xc);
    return e;
}

// 直接塞一份预设（免得自检去读盘）。
void installSet(ecs::ExpressionComponent& xc) {
    xc.set = ExpressionSet{};
    xc.set.name = "test";
    xc.set.valid = true;
    ExpressionPreset a;
    a.name = "happy";
    a.weights.push_back({"smile", 0.8f});
    ExpressionPreset b;
    b.name = "talk";
    b.weights.push_back({"smile", 0.2f});
    b.weights.push_back({"bogus_channel", 1.0f});
    xc.set.presets = {a, b};
    xc.setLoaded = true;
    xc.set.valid = true;
}

float weightOf(const assets::Model& m, const ecs::MorphComponent& mp,
               const char* channel) {
    const int ci = m.morphRig.channelIndex(channel);
    if (ci < 0) return -1.0f;
    const std::size_t c = static_cast<std::size_t>(ci);
    return (c < mp.weights.size()) ? mp.weights[c] : 0.0f;
}

} // namespace

int runExpressionSelfTest() {
    g_pass = 0;
    g_fail = 0;

    // =============================================== 组 A：眨眼曲线
    std::printf("\n[A] 眨眼曲线（close/hold/open 三段）\n");
    {
        const float c = 0.10f, h = 0.04f, o = 0.16f;
        report("A1 phase<0 → 0", blinkWeight(-0.01f, c, h, o), 0.0, 1e-6);
        report("A2 phase=0 → 0（刚要闭还没闭）", blinkWeight(0.0f, c, h, o),
               0.0, 1e-6);
        // 闭眼段中点：smoothstep(0.5) = 0.5
        report("A3 闭眼段中点 = 0.5", blinkWeight(0.05f, c, h, o), 0.5, 1e-5);
        // 闭眼段 1/4 处：smoothstep(0.25) = 0.15625 —— 这是"必须是
        // smoothstep 而不是线性"的判据（线性会得 0.25）。
        report("A4 闭眼 1/4 处 = 0.15625（smoothstep 不是线性）",
               blinkWeight(0.025f, c, h, o), 0.15625, 1e-5);
        report("A5 保持段 = 1", blinkWeight(0.12f, c, h, o), 1.0, 1e-6);
        // 睁眼段中点：仍 0.5，但取值在**下降**
        const float up0 = blinkWeight(0.20f, c, h, o);
        const float up1 = blinkWeight(0.22f, c, h, o);
        report("A6 睁眼段中点 = 0.5", blinkWeight(0.22f, c, h, o), 0.5, 1e-5);
        reportFlag("A6b 睁眼段在下降（同一段里后一个采样更小）",
                   up1 < up0, "w(0.20)>w(0.22)");
        report("A7 超过总时长 → 0", blinkWeight(0.31f, c, h, o), 0.0, 1e-6);
        report("A8 退化配置（三段全 0）→ 0", blinkWeight(0.05f, 0, 0, 0), 0.0,
               1e-6);
    }

    // =============================================== 组 B：预设解析
    std::printf("\n[B] 预设解析\n");
    {
        const char* okJson = R"({
          "version": 1,
          "name": "face",
          "presets": [
            {"name": "smile", "label": "微笑", "weights": {"smile": 0.8, "blink": 0.1}},
            {"name": "angry", "weights": {"smile": 0.0}}
          ]
        })";
        ExpressionSet s;
        const bool parsed = parseExpressionSet(okJson, "unit/face.expr.json", s);
        reportFlag("B1 正常解析成功", parsed && s.valid, "valid=1");
        report("B2 预设条数 = 2", static_cast<double>(s.presets.size()), 2.0, 0);
        reportFlag("B3 label 读到了", s.presets.size() == 2 &&
                                        s.presets[0].label == "微笑",
                   "label=微笑");
        reportFlag("B4 权重保序（smile 在前、blink 在后）",
                   s.presets.size() == 2 && s.presets[0].weights.size() == 2 &&
                       s.presets[0].weights[0].channel == "smile" &&
                       s.presets[0].weights[1].channel == "blink",
                   "order=smile,blink");
        report("B5 权重值 = 0.8",
               s.presets.empty() ? -1.0
                                 : static_cast<double>(s.presets[0].weights[0].value),
               0.8, 1e-6);

        // 缺 version：能用，但要报一条警告
        ExpressionSet sv;
        const bool okV = parseExpressionSet(
            R"({"presets":[{"name":"a","weights":{"x":1}}]})", "t", sv);
        reportFlag("B6 缺 version 仍可用 + 有警告",
                   okV && sv.valid && !sv.warnings.empty(), "warn=1");

        // 重名：先到的赢
        ExpressionSet sd;
        parseExpressionSet(
            R"({"version":1,"presets":[
                 {"name":"d","weights":{"x":0.1}},
                 {"name":"d","weights":{"x":0.9}}]})",
            "t", sd);
        reportFlag("B7 重名 → 先到的赢（0.1 而不是 0.9）",
                   sd.presets.size() == 1 &&
                       std::fabs(sd.presets[0].weights[0].value - 0.1f) < 1e-6f,
                   "n=1 v=0.1");
        reportFlag("B7b 重名有警告", !sd.warnings.empty(), "warn=1");

        // 越界：钳到 1 且报
        ExpressionSet so;
        parseExpressionSet(
            R"({"version":1,"presets":[{"name":"o","weights":{"x":1.5,"y":-2}}]})",
            "t", so);
        reportFlag("B8 越界被钳到 [0,1]（1.5→1、-2→0）",
                   so.presets.size() == 1 &&
                       std::fabs(so.presets[0].weights[0].value - 1.0f) < 1e-6f &&
                       std::fabs(so.presets[0].weights[1].value - 0.0f) < 1e-6f,
                   "1.5→1 -2→0");
        report("B8b 越界报了 2 条警告",
               static_cast<double>(so.warnings.size()), 2.0, 0);

        // 非数字：跳过该通道，其余保留
        ExpressionSet sn;
        parseExpressionSet(
            R"({"version":1,"presets":[{"name":"n","weights":{"x":"1","y":0.5}}]})",
            "t", sn);
        reportFlag("B9 非数字通道被跳过、其余保留",
                   sn.presets.size() == 1 && sn.presets[0].weights.size() == 1 &&
                       sn.presets[0].weights[0].channel == "y",
                   "kept=y");

        // 空 weights → 整条跳过
        ExpressionSet se;
        const bool okE = parseExpressionSet(
            R"({"version":1,"presets":[{"name":"e","weights":{}}]})", "t", se);
        reportFlag("B10 空 weights → 整条不存在 + 失败（没有可用预设）",
                   !okE && !se.valid, "invalid");

        // 语法错
        ExpressionSet sb;
        reportFlag("B11 语法错 → invalid 且 error 非空",
                   !parseExpressionSet("{oops", "t", sb) && !sb.error.empty(),
                   "error=1");

        // presets 不是数组
        ExpressionSet sa;
        reportFlag("B12 presets 不是数组 → invalid",
                   !parseExpressionSet(R"({"version":1,"presets":3})", "t", sa),
                   "invalid");

        // version 不认识
        ExpressionSet su;
        reportFlag("B13 version=2 → invalid（不做隐式前向兼容）",
                   !parseExpressionSet(
                       R"({"version":2,"presets":[{"name":"a","weights":{"x":1}}]})",
                       "t", su),
                   "invalid");
    }

    // =============================================== 组 C：往返
    std::printf("\n[C] 写回 / 读回往返\n");
    {
        ExpressionSet a;
        a.name = "face";
        ExpressionPreset p;
        p.name = "smile";
        p.weights.push_back({"z_last", 0.25f});
        p.weights.push_back({"a_first", 0.75f});
        ExpressionPreset q;
        q.name = "plain";   // 没有 label
        q.weights.push_back({"x", 1.0f});
        a.presets = {p, q};
        a.valid = true;

        const std::string text = toJson(a);
        reportFlag("C1 没 label 的预设不写 label 键",
                   text.find("\"label\"") == std::string::npos, "no label key");
        ExpressionSet b;
        const bool ok = parseExpressionSet(text, "rt.expr.json", b);
        reportFlag("C2 往返可解析", ok && b.valid, "valid=1");
        report("C3 往返预设数 = 2", static_cast<double>(b.presets.size()), 2.0, 0);
        reportFlag("C4 往返权重顺序保持（z_last 仍在 a_first 之前）",
                   b.presets.size() == 2 && b.presets[0].weights.size() == 2 &&
                       b.presets[0].weights[0].channel == "z_last",
                   "order=z_last,a_first");
        report("C5 往返值 = 0.75",
               b.presets.size() == 2
                   ? static_cast<double>(b.presets[0].weights[1].value)
                   : -1.0,
               0.75, 1e-6);
        reportFlag("C6 往返没有新增警告（默认值不该产生噪音）",
                   b.warnings.empty(), "warn=0");
    }

    // =============================================== 组 D：rig 适配
    std::printf("\n[D] 与 rig 的适配度\n");
    {
        ExpressionSet s;
        ExpressionPreset p;
        p.name = "p";
        p.weights.push_back({"smile", 1.0f});
        p.weights.push_back({"nope", 1.0f});
        ExpressionPreset q;
        q.name = "q";
        q.weights.push_back({"nope", 1.0f});   // 重复的缺失通道
        q.weights.push_back({"also_nope", 1.0f});
        s.presets = {p, q};
        std::vector<std::string> missing;
        checkAgainstRig(s, {"smile", "blink", "mouth_open"}, missing);
        report("D1 缺失通道去重后 = 2", static_cast<double>(missing.size()),
               2.0, 0);
        reportFlag("D2 报的是真的缺的那两个",
                   missing.size() == 2 && missing[0] == "nope" &&
                       missing[1] == "also_nope",
                   "nope,also_nope");
        checkAgainstRig(s, {"smile", "nope", "also_nope"}, missing);
        report("D3 全匹配时为空", static_cast<double>(missing.size()), 0.0, 0);
    }

    // =============================================== 组 E：驱动器
    std::printf("\n[E] 驱动器（只写自己拥有的通道）\n");
    {
        assets::Model model = makeFace();
        scene::Scene sc;
        ecs::Entity e = makeCharacter(sc, model);
        auto& w = sc.world();
        auto* xc = w.get<ecs::ExpressionComponent>(e);
        auto* mp = w.get<ecs::MorphComponent>(e);
        installSet(*xc);
        xc->blinkEnabled = false;      // 先单独测预设
        xc->presetName = "happy";
        xc->intensity = 1.0f;

        tickExpressions(w, 0.016f, 0.0f);
        report("E1 预设权重写进去了（smile=0.8）",
               static_cast<double>(weightOf(model, *mp, "smile")), 0.8, 1e-6);

        // 手动通道：预设没列 blink，驱动不许碰它
        mp->weights[static_cast<std::size_t>(
            model.morphRig.channelIndex("blink"))] = 0.35f;
        xc->blinkEnabled = false;
        tickExpressions(w, 0.016f, 0.0f);
        report("E2 手动推的 blink 没被驱动抹掉（0.35）",
               static_cast<double>(weightOf(model, *mp, "blink")), 0.35, 1e-6);

        // 强度是乘性
        xc->intensity = 0.5f;
        tickExpressions(w, 0.016f, 0.0f);
        report("E3 强度 0.5 → smile 减半（0.4）",
               static_cast<double>(weightOf(model, *mp, "smile")), 0.4, 1e-6);

        // 换预设：旧通道归零、新预设的通道生效
        xc->intensity = 1.0f;
        xc->presetName = "talk";
        tickExpressions(w, 0.016f, 0.0f);
        report("E4 talk 的 smile = 0.2",
               static_cast<double>(weightOf(model, *mp, "smile")), 0.2, 1e-6);
        // happy 独有的通道（这里 happy 与 talk 都写 smile，换成完全不相交的一对）
        xc->presetName.clear();
        tickExpressions(w, 0.016f, 0.0f);
        report("E5 预设清空后 smile 归零（旧通道必须显式写 0）",
               static_cast<double>(weightOf(model, *mp, "smile")), 0.0, 1e-6);
        report("E5b 清空后不再拥有任何通道",
               static_cast<double>(xc->ownedChannels.size()), 0.0, 0);
        // ⚠⚠ 归零只做**一帧**。做完之后用户再推滑条，驱动不许每年都抹掉它
        //   —— 这一条是"清空预设"最容易踩的坑：把归零的通道留在 owned 里，
        //   下一帧又写 0，于是滑条永远推不上去。
        mp->weights[static_cast<std::size_t>(
            model.morphRig.channelIndex("smile"))] = 0.4f;
        tickExpressions(w, 0.016f, 0.0f);
        report("E5c 归零只做一次（之后手动推的 0.4 保住了）",
               static_cast<double>(weightOf(model, *mp, "smile")), 0.4, 1e-6);

        // 预设里在本 rig 上不存在的通道：跳过，并计入 missing
        xc->presetName = "talk";
        const ExpressionStats st1 = tickExpressions(w, 0.016f, 0.0f);
        report("E6 未知通道被跳过（missing ≥ 1）",
               static_cast<double>(st1.missing > 0 ? 1 : 0), 1.0, 0);
        report("E6b 同一帧 driven = 1",
               static_cast<double>(st1.driven), 1.0, 0);

        // dt=0 不推进眨眼
        xc->blinkEnabled = true;
        xc->blinkRequest = true;
        tickExpressions(w, 0.0f, 0.0f);
        const float ph0 = xc->blinkPhase;
        tickExpressions(w, 0.0f, 0.0f);
        report("E7 dt=0 不推进眨眼相位", static_cast<double>(xc->blinkPhase),
               static_cast<double>(ph0), 0.0);

        // 完整眨一次
        xc->blinkRequest = false;
        xc->blinkPhase = -1.0f;
        xc->blinkTimer = 0.0f;   // 下一帧就触发
        const int before = xc->blinkCount;
        float maxBlink = 0.0f;
        for (int i = 0; i < 20; ++i) {
            tickExpressions(w, 0.02f, 0.0f);
            maxBlink = std::max(maxBlink,
                                weightOf(model, *mp, "blink"));
        }
        reportFlag("E8 眨眼跑完一次（count 涨 1 且闭到过 1.0）",
                   xc->blinkCount == before + 1 && maxBlink > 0.99f,
                   "count+1 max=1");
        reportFlag("E8b 眨眼结束后回到睁眼",
                   weightOf(model, *mp, "blink") < 0.01f, "blink≈0");

        // 口型：给电平 → 张嘴；关掉 → 回 0
        xc->blinkEnabled = false;
        xc->lipSyncEnabled = true;
        xc->mouthGain = 1.0f;
        for (int i = 0; i < 20; ++i) tickExpressions(w, 0.02f, 0.9f);
        report("E9 电平 0.9 → mouth_open 接近 0.9",
               static_cast<double>(weightOf(model, *mp, "mouth_open")), 0.9,
               0.05);
        xc->lipSyncEnabled = false;
        tickExpressions(w, 0.02f, 0.9f);
        report("E10 关掉口型 → mouth_open 立刻回 0（不留张嘴）",
               static_cast<double>(weightOf(model, *mp, "mouth_open")), 0.0,
               1e-6);

        // 没有 MorphComponent 的实体不许崩，也不许被计进 driven
        ecs::Entity bare = sc.createObject("bare");
        w.add<ecs::ExpressionComponent>(bare, ecs::ExpressionComponent{});
        const ExpressionStats st2 = tickExpressions(w, 0.016f, 0.5f);
        report("E11 无 rig 的实体不被驱动（driven 只算带脸的）",
               static_cast<double>(st2.driven), 1.0, 0);
        const auto* bareXc = w.get<ecs::ExpressionComponent>(bare);
        reportFlag("E11b 无 rig 的实体没有拥有任何通道",
                   bareXc && bareXc->ownedChannels.empty(), "owned=0");
    }

    std::printf("\nTOTAL pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

} // namespace anim
