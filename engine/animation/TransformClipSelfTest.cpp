// ============================================================
// animation/TransformClipSelfTest —— C5 的数值判据（无窗口 / 无设备）
//
// 五组：
//   A 采样：空轨道 / 单帧 / 首尾夹取 / smoothstep（不是线性）/ 关键帧上精确
//   B 推进：dt<=0 不动 / loop 用 fmod / 不循环停在 duration / duration=0 不出 NaN
//   C 每帧：**没推进就不写 transform**（这条是整组的核心）/ 播完不再写 / 多实体
//   D 状态同步：playing = autoplay / resetTime / 同步时采样一次
//   E 与编辑器同源：同一组关键帧在"编辑器用过的 t 值"上逐位一致
//
// 为什么 C 组最重要：这一层的头号事故是"每帧无条件采样"，它会把脚本/用户
// 摆好的 transform 按最后一帧覆盖掉，症状是"脚本 setPos 没反应"—— 而代码
// 上看起来完全正常（没有报错、没有警告）。
// ============================================================
#include "animation/TransformClip.h"

#include "scene/Scene.h"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <cmath>
#include <cstdio>
#include <string>

namespace anim {
namespace {

int g_pass = 0;
int g_fail = 0;

void report(const char* name, double got, double want, double eps) {
    const bool ok = std::fabs(got - want) <= eps;
    if (ok) ++g_pass;
    else ++g_fail;
    std::printf("  %s %-52s got=%.6f want=%.6f\n", ok ? "[PASS]" : "[FAIL]",
                name, got, want);
}

void reportFlag(const char* name, bool ok, const char* detail) {
    if (ok) ++g_pass;
    else ++g_fail;
    std::printf("  %s %-52s %s\n", ok ? "[PASS]" : "[FAIL]", name, detail);
}

ecs::AnimationClipComponent makeClip(float dur, bool loop) {
    ecs::AnimationClipComponent c;
    c.duration = dur;
    c.loop = loop;
    c.playing = true;
    // 0s 在原点、`dur` 秒到 (100,0,0) —— 数值好算，midpoint 一看就看出来。
    // ⚠⚠ 末帧时刻**必须落在 duration 上**，第一版这里写死 4.0，于是
    //   `makeClip(2.0f)` 造出"duration=2 但轨道到 4 秒"的夹具：
    //   播到 2 秒停住时采样的是 t=2（u=0.5 → 50），而不是终点 100。
    //   两条期望（C3 的 100、C6-B 的 15.625）因此全红 —— 而**实现是对的**。
    //   编辑器那边同样强制 `duration >= keys.back().t`（`drawPanel` 的
    //   `minDur`），夹具不该造出编辑器造不出来的状态。
    ecs::TransformKey k0;
    k0.t = 0.0f;
    k0.position = glm::vec3(0.0f);
    ecs::TransformKey k1;
    k1.t = dur;
    k1.position = glm::vec3(100.0f, 0.0f, 0.0f);
    c.keys = {k0, k1};
    return c;
}

} // namespace

int runTransformClipSelfTest() {
    std::printf("=== TransformClip (C5) ===\n");

    // ---------------------------------------------------------------- A 采样
    std::printf("[A] 采样\n");
    {
        ecs::AnimationClipComponent empty;
        ecs::TransformComponent tr;
        tr.position = glm::vec3(7.0f, 8.0f, 9.0f);
        sampleTransformClip(empty, 1.0f, tr);
        // 纪律 ③：空轨道不许动 transform
        report("A1 空轨道不碰 transform (x)", static_cast<double>(tr.position.x),
               7.0, 1e-6);
        report("A1 空轨道不碰 transform (z)", static_cast<double>(tr.position.z),
               9.0, 1e-6);
    }
    {
        auto c = makeClip(4.0f, true);
        ecs::TransformComponent tr;
        c.keys.pop_back();   // 只剩一帧
        sampleTransformClip(c, 999.0f, tr);
        report("A2 单帧：任意 t 都等于那一帧", static_cast<double>(tr.position.x),
               0.0, 1e-6);
    }
    {
        auto c = makeClip(4.0f, true);
        ecs::TransformComponent tr;
        sampleTransformClip(c, -5.0f, tr);
        report("A3 早于首帧 → 首帧", static_cast<double>(tr.position.x), 0.0, 1e-6);
        sampleTransformClip(c, 99.0f, tr);
        report("A4 晚于末帧 → 末帧", static_cast<double>(tr.position.x), 100.0,
               1e-6);
        sampleTransformClip(c, 4.0f, tr);
        report("A5 恰在末帧上 → 精确等于末帧",
               static_cast<double>(tr.position.x), 100.0, 1e-6);
    }
    {
        // ⚠ 这一条是整组里唯一能区分 smoothstep 与线性的量。
        //   u = 0.25 时：线性 → 25；smoothstep(0.25)=0.15625 → 15.625。
        //   用 u=0.5 是**测不出来**的（两者都给 50）—— 第一版就这么写的，
        //   把 ease 改成线性照样绿。
        auto c = makeClip(4.0f, true);
        ecs::TransformComponent tr;
        sampleTransformClip(c, 1.0f, tr);   // t=1 在 0..4 的 1/4 处
        report("A6 t=1/4 处是 smoothstep（不是线性 25）",
               static_cast<double>(tr.position.x), 15.625, 1e-4);
    }

    // ---------------------------------------------------------------- B 推进
    std::printf("[B] 推进\n");
    {
        auto c = makeClip(4.0f, true);
        const bool moved = advanceTransformClip(c, 0.0f);
        reportFlag("B1 dt=0 不推进", !moved && c.time == 0.0f, "moved=0 time=0");
        const bool moved2 = advanceTransformClip(c, 0.5f);
        reportFlag("B2 dt>0 推进", moved2 && std::fabs(c.time - 0.5f) < 1e-6f,
                   "time=0.5");
    }
    {
        auto c = makeClip(2.0f, true);
        c.time = 1.9f;
        advanceTransformClip(c, 0.3f);      // 2.2 → fmod(2.2, 2) = 0.2
        report("B3 loop 回绕用 fmod", static_cast<double>(c.time), 0.2, 1e-5);
        reportFlag("B3 回绕后仍在播", c.playing, "playing=1");
    }
    {
        auto c = makeClip(2.0f, false);
        c.time = 1.9f;
        advanceTransformClip(c, 0.5f);
        report("B4 不循环 → 停在 duration", static_cast<double>(c.time), 2.0, 1e-6);
        reportFlag("B4 不循环 → playing=false", !c.playing, "playing=0");
    }
    {
        // duration=0 是**非法但可达**的状态（手改 JSON）。不兜的话
        // loop 分支会 fmod(x, 0) → NaN，之后所有区间比较全不命中。
        auto c = makeClip(0.0f, true);
        c.time = 3.0f;
        advanceTransformClip(c, 1.0f);
        reportFlag("B5 duration=0 不产生 NaN", !std::isnan(c.time), "not NaN");
        ecs::TransformComponent tr;
        sampleTransformClip(c, c.time, tr);
        reportFlag("B5 NaN 没有污染采样", !std::isnan(tr.position.x), "x ok");
    }
    {
        // 卡顿一帧跨过好几圈
        auto c = makeClip(2.0f, true);
        c.time = 5.0f;
        advanceTransformClip(c, 2.0f);      // 7 → fmod(7,2) = 1
        report("B6 大 dt 跨多圈", static_cast<double>(c.time), 1.0, 1e-5);
    }

    // ---------------------------------------------------------------- C 每帧
    std::printf("[C] tick 纪律\n");
    {
        // ⚠⚠ 核心判据：dt=0 时**不许**写 transform。
        //   写成"无论 dt 都采样"的话，编辑暂停一帧就会把脚本刚设的位置
        //   拍回轨道值 —— 而日志里一条错都没有。
        scene::Scene sc;
        ecs::Entity e = sc.createObject("Lift");
        auto& c = sc.world().add<ecs::AnimationClipComponent>(e);
        c = makeClip(4.0f, true);
        c.time = 2.0f;                      // 轨道上 x=50 的位置
        auto* tr = sc.world().get<ecs::TransformComponent>(e);
        tr->position = glm::vec3(7.0f, 8.0f, 9.0f);   // 被人摆到别处

        anim::tickTransformClips(sc, 0.0f);
        report("C1 dt=0 不写 transform（脚本摆的位还在）",
               static_cast<double>(tr->position.x), 7.0, 1e-6);

        // 推进 0.5s → time=2.5。期望值**独立手算**，不照实现抄：
        //   u = 2.5/4 = 0.625
        //   smoothstep = u²(3−2u) = 0.390625 × 1.75 = 0.68359375
        //   x = 100 × 0.68359375 = 68.359375
        anim::tickTransformClips(sc, 0.5f);
        report("C2 推进 0.5s 后写回（手算 68.359）",
               static_cast<double>(tr->position.x), 68.359375, 1e-3);
    }
    {
        // 不循环播完之后：再 tick 不许继续写（否则脚本改不动它）
        scene::Scene sc;
        ecs::Entity e = sc.createObject("Once");
        auto& c = sc.world().add<ecs::AnimationClipComponent>(e);
        c = makeClip(2.0f, false);
        auto* tr = sc.world().get<ecs::TransformComponent>(e);
        anim::tickTransformClips(sc, 3.0f);          // 一次播完
        report("C3 播完停在终点", static_cast<double>(tr->position.x), 100.0,
               1e-3);

        tr->position = glm::vec3(1.0f, 2.0f, 3.0f);  // 脚本/用户改了它
        anim::tickTransformClips(sc, 0.5f);          // 已经不在播
        report("C4 播完之后不再覆盖 transform",
               static_cast<double>(tr->position.x), 1.0, 1e-6);
    }
    {
        // 多实体各播各的：各自按自己的 duration 采样
        scene::Scene sc;
        ecs::Entity ea = sc.createObject("A");
        sc.world().add<ecs::AnimationClipComponent>(ea) = makeClip(4.0f, true);
        ecs::Entity eb = sc.createObject("B");
        sc.world().add<ecs::AnimationClipComponent>(eb) = makeClip(8.0f, true);
        const auto st = anim::tickTransformClips(sc, 2.0f);
        report("C5 两个实体都推进了", static_cast<double>(st.playing), 2.0, 0.5);
        const float ax = sc.world().get<ecs::TransformComponent>(ea)->position.x;
        const float bx = sc.world().get<ecs::TransformComponent>(eb)->position.x;
        report("C6 A（dur=4）t=2 → 50", static_cast<double>(ax), 50.0, 1e-2);
        report("C6 B（dur=8）t=2 → 15.6", static_cast<double>(bx), 15.625, 1e-2);
    }

    // ---------------------------------------------------------------- D 同步
    std::printf("[D] 进入时的状态同步\n");
    {
        scene::Scene sc;
        ecs::Entity e = sc.createObject("Door");
        auto& c = sc.world().add<ecs::AnimationClipComponent>(e);
        c = makeClip(4.0f, true);
        c.autoplay = false;
        c.playing = true;    // 编辑器里点过"预览播放"留下的
        c.time = 3.0f;
        syncTransformClipPlayState(sc, true);
        reportFlag("D1 playing 跟 autoplay 走（覆盖遗留的 true）", !c.playing,
                   "playing=0");
        report("D2 resetTime=true → time 归零", static_cast<double>(c.time), 0.0,
               1e-6);
        // 同步时会采样一次 → transform 已经是第 0 帧
        const float x = sc.world().get<ecs::TransformComponent>(e)->position.x;
        report("D3 同步时采样一次（进入第一帧就对）", static_cast<double>(x), 0.0,
               1e-6);
    }
    {
        scene::Scene sc;
        ecs::Entity e = sc.createObject("Door2");
        auto& c = sc.world().add<ecs::AnimationClipComponent>(e);
        c = makeClip(4.0f, true);
        c.autoplay = true;
        c.time = 2.0f;
        syncTransformClipPlayState(sc, false);
        reportFlag("D4 autoplay=true → playing", c.playing, "playing=1");
        report("D5 resetTime=false → time 保留（读档接上）",
               static_cast<double>(c.time), 2.0, 1e-6);
        const float x = sc.world().get<ecs::TransformComponent>(e)->position.x;
        report("D5 保留 time 时按该时刻采样", static_cast<double>(x), 50.0, 1e-2);
    }

    // ---------------------------------------------------------------- E 同源
    std::printf("[E] 与编辑器同一套插值\n");
    {
        // 编辑器里"拖时间轴"会落在一堆非整点上。这里取几个代表值，
        // 手算 smoothstep 作为期望 —— 若有人把引擎层改成线性，E 组全红。
        auto c = makeClip(4.0f, true);
        ecs::TransformComponent tr;
        struct { float t, want; } cases[] = {
            {0.5f, 4.296875f},    // u=0.125 → 0.015625×2.75 = 0.04296875
            {1.0f, 15.625f},      // u=0.25  → 0.0625×2.5
            {2.0f, 50.0f},        // u=0.5（线性也 50，单看这条区分不了）
            {3.0f, 84.375f},      // u=0.75  → 0.5625×1.5
            {3.9f, 99.815625f},   // u=0.975 → 0.950625×1.05
        };
        for (const auto& cs : cases) {
            sampleTransformClip(c, cs.t, tr);
            char nm[80];
            std::snprintf(nm, sizeof(nm), "E1 t=%.2f 的采样值", 
                          static_cast<double>(cs.t));
            report(nm, static_cast<double>(tr.position.x),
                   static_cast<double>(cs.want), 1e-2);
        }
    }
    {
        // rotation / scale 也要走同一条混合（只混合 position 是常见的漏改）
        ecs::AnimationClipComponent c;
        c.duration = 2.0f;
        ecs::TransformKey a;
        a.t = 0.0f;
        a.scale = glm::vec3(1.0f);
        ecs::TransformKey b;
        b.t = 2.0f;
        b.scale = glm::vec3(3.0f);
        c.keys = {a, b};
        ecs::TransformComponent tr;
        sampleTransformClip(c, 1.0f, tr);
        report("E2 scale 也参与混合（1→3 中点 2）",
               static_cast<double>(tr.scale.x), 2.0, 1e-4);
    }

    std::printf("\nTOTAL pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

} // namespace anim
