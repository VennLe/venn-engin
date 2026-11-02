// ============================================================
// nav/NavSelfTest.cpp —— C8 导航验收（`Game.exe --selftest nav`）
//
// 七组，每组对应"这套东西会怎么错"：
//   A 网格   —— 尺寸/原点/哨兵（"没有地面"和"地面在 -60 米"必须分得开）
//   B A*     —— 直线 / 绕墙 / 不可达 / 不切角 / 台阶阻断 / 确定性 / 吸附
//   C 视线   —— 穿墙为假 / 平滑后点变少且不变长 / 首尾不丢
//   D 吸附   —— 墙里的点吸到最近可走格；外面一格都没有时返回 false
//   E 代理   —— 推进 / 到点 / dt=0 不动 / 无网格失败 / 朝向插值 / 胶囊路
//   F 存盘   —— 往返逐字段 / 默认值不写 / 负数夹回 / 非 object 拒绝
//   G ASCII  —— 打印出来的图形状（r 从大到小 = 俯视图）
//
// ⚠ detail 里**不能有空格**（回归脚本用 `detail=(\S*)` 解析）。
// ⚠ 每条断言都钉在"两种实现会给出不同数字"的量上：路径**长度**为什么
//   能区分"绕过去了"和"直接穿墙"，因为前者一定更长。
// ============================================================

#include "nav/NavSelfTest.h"

#include "core/Logger.h"
#include "ecs/Components.h"
#include "nav/NavSerialize.h"
#include "nav/NavSystem.h"
#include "scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace nav {

namespace {

int g_pass = 0;
int g_fail = 0;

void report(const char* name, double actual, double expected, double tol = 0.0) {
    const bool ok = (actual - expected) <= tol && (expected - actual) <= tol;
    VK_LOG_INFO("NAV-TEST %s actual=%.6f expected=%.6f tol=%.6f result=%s", name, actual, expected,
                tol, ok ? "PASS" : "FAIL");
    if (ok) ++g_pass;
    else ++g_fail;
}

void reportFlag(const char* name, bool ok, const std::string& detail = {}) {
    VK_LOG_INFO("NAV-TEST %s actual=%d expected=1 tol=0 detail=%s result=%s", name, ok ? 1 : 0,
                detail.c_str(), ok ? "PASS" : "FAIL");
    if (ok) ++g_pass;
    else ++g_fail;
}

// ---- 造格子用的两个小工具（用例里到处都是，写一次）----
NavGrid makeGrid(int cols, int rows, float cell, float h = 0.0f) {
    NavGridDesc d;
    d.min = glm::vec3(0.0f, 0.0f, -10.0f);
    d.max = glm::vec3(static_cast<float>(cols) * cell, static_cast<float>(rows) * cell, 10.0f);
    d.cellSize = cell;
    d.maxStep = 0.5f;
    std::vector<float> heights(static_cast<std::size_t>(cols) * rows, h);
    NavGrid g;
    buildFromHeights(g, d, heights);
    return g;
}

void blockCol(NavGrid& g, int c, int rFrom, int rTo) {
    for (int r = rFrom; r <= rTo; ++r) {
        if (g.inBounds(c, r)) g.set(c, r, false, 0.0f);
    }
}

// ============================================================
// A 组：格子本身
// ============================================================
void groupGrid() {
    NavGrid g = makeGrid(20, 20, 1.0f);
    reportFlag("A1.build_valid", g.valid());
    report("A1.cols", g.cols, 20.0);
    report("A1.rows", g.rows, 20.0);
    report("A1.walk_all", static_cast<double>(g.walkableCount()), 400.0);

    // 格心 = origin + (c+0.5)*cell
    const glm::vec3 c00 = g.centerOf(0, 0);
    report("A2.center_x", c00.x, 0.5, 1e-5);
    report("A2.center_y", c00.y, 0.5, 1e-5);

    // cellOf(centerOf(c,r)) 必须回到 (c,r) —— 差一格的经典来源是
    // 一边用 floor、一边用 round。
    int mismatch = 0;
    for (int r = 0; r < g.rows; ++r) {
        for (int c = 0; c < g.cols; ++c) {
            const glm::ivec2 back = g.cellOf(g.centerOf(c, r));
            if (back.x != c || back.y != r) ++mismatch;
        }
    }
    report("A3.cell_roundtrip_bad", static_cast<double>(mismatch), 0.0);

    // 越界判定
    reportFlag("A4.oob_left", !g.inBounds(-1, 0));
    reportFlag("A4.oob_top", !g.inBounds(0, 20));

    // "没有地面"的哨兵：NaN 与"比底面还低"都必须变 Blocked，
    // 但**高度**要能与真实地面区分开（不能都变成 0 —— 那是地面高度）。
    {
        NavGridDesc d;
        d.min = glm::vec3(0.0f, 0.0f, -10.0f);
        d.max = glm::vec3(2.0f, 1.0f, 10.0f);
        d.cellSize = 1.0f;
        std::vector<float> h{0.0f, std::nanf("")};   // 第 2 格是坑
        NavGrid gg;
        reportFlag("A5.nan_ok", buildFromHeights(gg, d, h));
        reportFlag("A5.nan_blocked", !gg.walkable(1, 0));
        report("A5.walk_count", static_cast<double>(gg.walkableCount()), 1.0);
        report("A5.height_kept", gg.heightAt(0, 0), 0.0, 1e-6);
    }
    // 尺寸对不上必须被拒（否则会拿着一张"格子数比 cols*rows 少"的图跑）
    {
        NavGridDesc d;
        d.min = glm::vec3(0.0f);
        d.max = glm::vec3(4.0f, 4.0f, 1.0f);
        d.cellSize = 1.0f;
        std::vector<float> tooFew(8, 0.0f);
        NavGrid gg;
        reportFlag("A6.wrong_size_rejected", !buildFromHeights(gg, d, tooFew));
    }
}

// ============================================================
// B 组：A*
// ============================================================
void groupPath() {
    const glm::vec3 p11(1.5f, 1.5f, 0.0f);
    const glm::vec3 p91(9.5f, 1.5f, 0.0f);

    // B1 直线：20x20 空地，长度必须就等于两点距离（8 米）
    {
        NavGrid g = makeGrid(20, 20, 1.0f);
        NavPath p;
        reportFlag("B1.ok", findPath(g, p11, p91, p));
        report("B1.reached", p.reached ? 1.0 : 0.0, 1.0);
        report("B1.length", p.length, 8.0, 1e-3);
        reportFlag("B1.expanded_positive", p.expanded > 0, std::to_string(p.expanded));
    }

    // B2 绕墙：c=5 那一列挡住（r=0 留口），长度必须**明显大于** 8
    NavGrid wall = makeGrid(12, 12, 1.0f);
    blockCol(wall, 5, 1, 11);
    {
        NavPath p;
        reportFlag("B2.ok", findPath(wall, glm::vec3(1.5f, 5.5f, 0.0f),
                                     glm::vec3(9.5f, 5.5f, 0.0f), p));
        report("B2.reached", p.reached ? 1.0 : 0.0, 1.0);
        reportFlag("B2.detoured", p.length > 8.5, std::to_string(p.length));
    }

    // B3 不可达：终点在一间 2x2 的密封房里（房本身可走，四周全封）。
    //    → 吸附能成功（终点格可走），但 A* 到不了 → reached=0 且有路径。
    {
        NavGrid g = makeGrid(16, 16, 1.0f);
        for (int r = 5; r <= 10; ++r) {
            for (int c = 5; c <= 10; ++c) g.set(c, r, false, 0.0f);
        }
        // 房间：中心 2x2
        g.set(7, 7, true, 0.0f);
        g.set(8, 7, true, 0.0f);
        g.set(7, 8, true, 0.0f);
        g.set(8, 8, true, 0.0f);
        NavPath p;
        reportFlag("B3.ok", findPath(g, glm::vec3(1.5f, 1.5f, 0.0f), glm::vec3(7.5f, 7.5f, 0.0f), p));
        report("B3.reached", p.reached ? 1.0 : 0.0, 0.0);
        reportFlag("B3.partial_path", p.pts.size() >= 2, std::to_string(p.pts.size()));
    }

    // B4 不许斜向切角：(6,5)/(5,6) 都堵上，(5,5)→(6,6) 的直线对角必须绕
    {
        NavGrid g = makeGrid(8, 8, 1.0f);
        g.set(6, 5, false, 0.0f);
        g.set(5, 6, false, 0.0f);
        NavPath p;
        findPath(g, glm::vec3(5.5f, 5.5f, 0.0f), glm::vec3(6.5f, 6.5f, 0.0f), p);
        report("B4.reached", p.reached ? 1.0 : 0.0, 1.0);
        reportFlag("B4.no_cut", p.length > 3.0, std::to_string(p.length));
    }

    // B5 高差超 maxStep 的边不通：一格高台（2 米）在直线上，必须绕
    {
        NavGrid g = makeGrid(8, 8, 1.0f);
        g.set(6, 5, true, 2.0f);
        NavPath p;
        findPath(g, glm::vec3(5.5f, 5.5f, 0.0f), glm::vec3(7.5f, 5.5f, 0.0f), p);
        report("B5.reached", p.reached ? 1.0 : 0.0, 1.0);
        reportFlag("B5.detoured", p.length > 2.2, std::to_string(p.length));
    }

    // B6 确定性：同一次输入跑两遍，逐点相同
    {
        NavPath a;
        NavPath b;
        findPath(wall, glm::vec3(1.5f, 5.5f, 0.0f), glm::vec3(9.5f, 5.5f, 0.0f), a);
        findPath(wall, glm::vec3(1.5f, 5.5f, 0.0f), glm::vec3(9.5f, 5.5f, 0.0f), b);
        int diff = (a.pts.size() == b.pts.size()) ? 0 : 1;
        for (std::size_t i = 0; i < a.pts.size() && i < b.pts.size(); ++i) {
            if (glm::length(a.pts[i] - b.pts[i]) > 1e-6f) ++diff;
        }
        report("B6.same_path_diff", static_cast<double>(diff), 0.0);
    }

    // B7 终点落在墙里 → 吸附到最近可走格，路径末点 = 那一格的格心
    {
        NavPath p;
        findPath(wall, glm::vec3(1.5f, 5.5f, 0.0f), glm::vec3(5.5f, 5.5f, 0.0f), p);
        report("B7.reached", p.reached ? 1.0 : 0.0, 1.0);
        const double endX = p.pts.empty() ? -1.0 : static_cast<double>(p.pts.back().x);
        report("B7.end_x", endX, 4.5, 1e-4);
    }

    // B8 没有网格 → 求路失败（不是"返回一条空路径装作成功"）
    {
        NavGrid empty;
        NavPath p;
        reportFlag("B8.invalid_grid_rejected", !findPath(empty, p11, p91, p));
    }
}

// ============================================================
// C 组：视线与平滑
// ============================================================
void groupSmooth() {
    NavGrid wall = makeGrid(12, 12, 1.0f);
    blockCol(wall, 5, 1, 11);

    reportFlag("C1.line_through_wall_false",
               !lineWalkable(wall, glm::vec3(1.5f, 5.5f, 0.0f), glm::vec3(9.5f, 5.5f, 0.0f)));

    NavGrid open = makeGrid(12, 12, 1.0f);
    reportFlag("C2.line_open_true",
               lineWalkable(open, glm::vec3(1.5f, 1.5f, 0.0f), glm::vec3(9.5f, 1.5f, 0.0f)));

    NavPath p;
    findPath(wall, glm::vec3(1.5f, 5.5f, 0.0f), glm::vec3(9.5f, 5.5f, 0.0f), p);
    const std::size_t before = p.pts.size();
    const float lenBefore = p.length;
    const glm::vec3 firstBefore = p.pts.empty() ? glm::vec3(0.0f) : p.pts.front();
    const glm::vec3 lastBefore = p.pts.empty() ? glm::vec3(0.0f) : p.pts.back();
    smoothPath(wall, p);

    reportFlag("C3.smoother_fewer", p.pts.size() < before,
               std::to_string(before) + "->" + std::to_string(p.pts.size()));
    reportFlag("C4.ends_kept",
               !p.pts.empty() && glm::length(p.pts.front() - firstBefore) < 1e-4f &&
                   glm::length(p.pts.back() - lastBefore) < 1e-4f);
    // 平滑只会"抄近路"，长度不许变长（变长 = 视线判定比 A* 还严，
    // 那种情况下平滑出来的路径会绕着走，比不平滑更难看）。
    reportFlag("C5.not_longer", p.length <= lenBefore + 1e-3f,
               std::to_string(lenBefore) + "->" + std::to_string(p.length));
    reportFlag("C6.still_walkable", p.pts.size() >= 2);
}

// ============================================================
// D 组：吸附
// ============================================================
void groupNearest() {
    NavGrid wall = makeGrid(12, 12, 1.0f);
    blockCol(wall, 5, 1, 11);
    glm::ivec2 c{0, 0};
    reportFlag("D1.found", nearestWalkable(wall, glm::vec3(5.5f, 5.5f, 0.0f), 2.0f, c));
    report("D1.cell_x", static_cast<double>(c.x), 4.0);
    report("D1.cell_y", static_cast<double>(c.y), 5.0);

    NavGrid tiny = makeGrid(4, 4, 1.0f);
    for (int r = 0; r < 4; ++r) {
        for (int cc = 0; cc < 4; ++cc) tiny.set(cc, r, false, 0.0f);
    }
    tiny.set(0, 0, true, 0.0f);
    glm::ivec2 d{0, 0};
    reportFlag("D2.too_far_false", !nearestWalkable(tiny, glm::vec3(3.5f, 3.5f, 0.0f), 1.0f, d));
    reportFlag("D3.far_enough_true", nearestWalkable(tiny, glm::vec3(3.5f, 3.5f, 0.0f), 8.0f, d));
    report("D3.cell_x", static_cast<double>(d.x), 0.0);
}

// ============================================================
// E 组：代理推进
// ============================================================
void groupAgents() {
    // E1 没有网格：tick 必须报 failed，且不许移动
    {
        scene::Scene s;
        ecs::Entity e = s.createObject("Npc");
        s.world().add<ecs::NavAgentComponent>(e);
        NavSystem sys;
        sys.gotoPoint(s, e, glm::vec3(9.5f, 1.5f, 0.0f));
        sys.tick(s, 0.1f);
        report("E1.failed", static_cast<double>(sys.stats().failed), 1.0);
        report("E1.moving", static_cast<double>(sys.stats().moving), 0.0);
    }

    // E2 正常推进 / 到点 / 朝向
    NavGrid g = makeGrid(20, 20, 1.0f);
    {
        scene::Scene s;
        ecs::Entity e = s.createObject("Npc");
        auto* tr = s.world().get<ecs::TransformComponent>(e);
        tr->position = glm::vec3(1.5f, 1.5f, 0.0f);
        tr->rotation.z = 1.5707963f;   // 先朝 +Y，验证它会转回 +X
        ecs::NavAgentComponent agent;
        agent.speed = 2.0f;
        agent.turnSpeed = 12.0f;
        s.world().add<ecs::NavAgentComponent>(e, agent);
        // ⚠ 必须从池子里读：`agent` 是**副本**，tick 改的是池子里那一份。
        //   用副本判断"到点没有"会让循环跑满 guard 次、然后断言"没到"。
        auto* ag = s.world().get<ecs::NavAgentComponent>(e);

        NavSystem sys;
        sys.setGrid(g);
        reportFlag("E2.has_grid", sys.hasGrid());
        reportFlag("E2.goto", sys.gotoPoint(s, e, glm::vec3(9.5f, 1.5f, 0.0f)));

        // dt = 0 不该推进（暂停语义）
        report("E3.dt_zero_moved", static_cast<double>(sys.tick(s, 0.0f)), 0.0);
        report("E3.dt_zero_x", static_cast<double>(tr->position.x), 1.5, 1e-5);

        int guard = 0;
        while (!ag->reached && guard < 200) {
            sys.tick(s, 0.1f);
            ++guard;
        }
        report("E4.reached", ag->reached ? 1.0 : 0.0, 1.0);
        report("E4.x", static_cast<double>(tr->position.x), 9.5, 0.35);
        // 高度贴网格（2.5D）：网格高度 0 → 位置 z 必须回到 0
        report("E4.z", static_cast<double>(tr->position.z), 0.0, 1e-4);
        // 朝向：从 +Y(π/2) 转到 +X(0)，只许走 90°（不许绕 270°）
        report("E5.yaw", static_cast<double>(tr->rotation.z), 0.0, 0.2);
        reportFlag("E5.expanded_seen", ag->lastExpanded > 0, std::to_string(ag->lastExpanded));
        report("E6.moving_last", static_cast<double>(sys.stats().arrived >= 1 ? 1 : 0), 1.0);

        // 到点之后 tick 不该继续改速度/位置
        const glm::vec3 rest = tr->position;
        sys.tick(s, 0.1f);
        report("E6.arrived_still", static_cast<double>(glm::length(tr->position - rest)), 0.0, 1e-4);

        // stop：清目标，之后不动
        sys.stop(s, e);
        reportFlag("E7.stop_cleared", !ag->hasGoal);
        sys.tick(s, 0.1f);
        report("E7.moving_after_stop", static_cast<double>(sys.stats().moving), 0.0);
    }

    // E8 有胶囊 → 只写期望速度，位置由物理那套推（这里位置必须**不动**）
    {
        scene::Scene s;
        ecs::Entity e = s.createObject("Walker");
        auto* tr = s.world().get<ecs::TransformComponent>(e);
        tr->position = glm::vec3(1.5f, 1.5f, 0.0f);
        s.world().add<ecs::NavAgentComponent>(e);
        s.world().add<ecs::CharacterControllerComponent>(e);

        NavSystem sys;
        sys.setGrid(g);
        sys.gotoPoint(s, e, glm::vec3(9.5f, 1.5f, 0.0f));
        sys.tick(s, 0.1f);
        auto* cc = s.world().get<ecs::CharacterControllerComponent>(e);
        report("E8.vel_x", static_cast<double>(cc->velocity.x), 2.0, 1e-4);
        report("E8.pos_untouched", static_cast<double>(tr->position.x), 1.5, 1e-5);
    }

    // E9 角度插值走最短弧
    {
        const float cur = 2.9670597f;    // 170°
        const float want = -2.9670597f;  // -170°
        const float mid = approachAngle(cur, want, 0.5f);
        report("E9.short_arc", static_cast<double>(mid), 3.1415927, 0.02);
        report("E9.snap", static_cast<double>(approachAngle(cur, want, 1.0f)), -2.9670597, 1e-4);
    }
}

// ============================================================
// F 组：存盘往返
// ============================================================
void groupSerialize() {
    // F1 默认值 → 一个键都不写
    {
        ecs::NavAgentComponent a;
        const auto j = navAgentToJson(a);
        report("F1.default_keys", static_cast<double>(j.size()), 0.0);
        reportFlag("F1.default_is_object", j.is_object());
    }

    // F2 全字段往返
    {
        ecs::NavAgentComponent a;
        a.speed = 3.25f;
        a.turnSpeed = 5.5f;
        a.arriveDist = 0.45f;
        a.repathInterval = 0.75f;
        a.enabled = false;
        a.faceMoveDir = false;
        a.smooth = false;
        a.autoStart = true;
        a.hasGoal = true;
        a.goal = glm::vec3(1.0f, -2.0f, 3.0f);
        // 运行态必须**不**被写进去
        a.path.push_back(glm::vec3(7.0f));
        a.next = 3;
        a.reached = true;

        const auto j = navAgentToJson(a);
        ecs::NavAgentComponent b;
        reportFlag("F2.read_ok", navAgentFromJson(j, b));
        report("F2.speed", static_cast<double>(b.speed), 3.25, 1e-5);
        report("F2.turn", static_cast<double>(b.turnSpeed), 5.5, 1e-5);
        report("F2.arrive", static_cast<double>(b.arriveDist), 0.45, 1e-5);
        report("F2.repath", static_cast<double>(b.repathInterval), 0.75, 1e-5);
        report("F2.enabled", b.enabled ? 1.0 : 0.0, 0.0);
        report("F2.face", b.faceMoveDir ? 1.0 : 0.0, 0.0);
        report("F2.smooth", b.smooth ? 1.0 : 0.0, 0.0);
        report("F2.autostart", b.autoStart ? 1.0 : 0.0, 1.0);
        reportFlag("F2.goal_has", b.hasGoal);
        report("F2.goal_x", static_cast<double>(b.goal.x), 1.0, 1e-5);
        report("F2.goal_z", static_cast<double>(b.goal.z), 3.0, 1e-5);
        // 运行态读回来必须是干净的（否则读档会"沿着上一条路径走"）
        report("F2.path_empty", static_cast<double>(b.path.size()), 0.0);
        report("F2.next", static_cast<double>(b.next), 0.0);
        report("F2.reached", b.reached ? 1.0 : 0.0, 0.0);
    }

    // F3 负数夹回 + 非 object 拒绝
    {
        nlohmann::json j = nlohmann::json::object();
        j["speed"] = -5.0;
        j["turn"] = -1.0;
        j["arrive"] = -0.2;
        j["repath"] = -3.0;
        ecs::NavAgentComponent a;
        navAgentFromJson(j, a);
        report("F3.speed", static_cast<double>(a.speed), 0.0);
        report("F3.turn", static_cast<double>(a.turnSpeed), 0.0);
        report("F3.arrive", static_cast<double>(a.arriveDist), 0.0);
        report("F3.repath", static_cast<double>(a.repathInterval), 0.0);

        ecs::NavAgentComponent b;
        reportFlag("F4.reject_array", !navAgentFromJson(nlohmann::json::array(), b));
        reportFlag("F4.reject_number", !navAgentFromJson(nlohmann::json(3.0), b));
    }

    // F5 只写 goal 的块也要能读回（"站在那儿等脚本"的巡逻点）
    {
        nlohmann::json j = nlohmann::json::object();
        j["goal"] = nlohmann::json::array({4.0, 5.0, 6.0});
        ecs::NavAgentComponent a;
        reportFlag("F5.read_ok", navAgentFromJson(j, a));
        reportFlag("F5.has_goal", a.hasGoal);
        report("F5.goal_y", static_cast<double>(a.goal.y), 5.0, 1e-5);
        reportFlag("F5.default_speed", std::fabs(a.speed - 2.0f) < 1e-6f);
    }
}

// ============================================================
// G 组：ASCII（人眼排查用）
// ============================================================
void groupAscii() {
    NavGrid g = makeGrid(12, 12, 1.0f);
    blockCol(g, 5, 1, 11);
    const std::string art = toAscii(g);
    int lines = 0;
    int dots = 0;
    int hashes = 0;
    for (char ch : art) {
        if (ch == '\n') ++lines;
        else if (ch == '.') ++dots;
        else if (ch == '#') ++hashes;
    }
    report("G1.lines", static_cast<double>(lines), 12.0);
    reportFlag("G1.has_walk", dots > 0, std::to_string(dots));
    reportFlag("G1.has_block", hashes > 0, std::to_string(hashes));
    // 最后一行（r=0）在墙那一列必须是可走的（B2 留的口）
    std::size_t lastLineStart = art.rfind('\n', art.size() - 2);
    if (lastLineStart == std::string::npos) lastLineStart = 0;
    const std::string lastLine = art.substr(lastLineStart + 1);
    reportFlag("G2.gap_on_bottom_row", lastLine.size() > 5 && lastLine[5] == '.', lastLine);
}

} // namespace

int runNavSelfTest() {
    g_pass = 0;
    g_fail = 0;
    VK_LOG_INFO("== selftest: nav ==");
    groupGrid();
    groupPath();
    groupSmooth();
    groupNearest();
    groupAgents();
    groupSerialize();
    groupAscii();
    std::printf("NAV-TOTAL pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

} // namespace nav
