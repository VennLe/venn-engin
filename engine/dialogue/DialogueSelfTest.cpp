// ============================================================
// dialogue/DialogueSelfTest.cpp
//
// C2 对话系统的无窗口自检。分六组：
//   A 解析（节点数 / 起点推导 / 自动 id）
//   B 校验（重复 id / 跳转缺失 / next 与 choices 并存 / 选项缺字段 / 空表）
//   C 打字机（cps=0 立刻全出 / 夹取 / elapsed 超长）
//   D 播放器（推进 / 选项刹车 / 越界不改状态 / 结束撒手 / 起点覆盖）
//   E 表现层（两段式按住推进 / 码点 vs 字节 / 换行复位 / 重播）
//   F 调度层（cue 的路径解析 / **不给场景加组件** / 组件自动起播 / 重置）
//   G 写回（toJson / saveDialogue 的往返：节点 / 中文原样 / 选项 / 起点
//      不丢 / 缓存不落盘 / 真写盘的临时文件能读回）
//
// ⚠ E/F 是第二步（接 UI 与时间轴）才加的两组。E 只碰 `DialoguePlayer`，
//   F 需要真的建一个 `scene::Scene`（`DialogueSystem::tick` 的签名就是它），
//   但仍然**不开窗口、不开 Vulkan** —— 所以整条还是一百毫秒级。
//
// ⚠ 每条断言都问过："**别的实现会给出不同的数吗**"。
//   打字机那条特意取 cps=30、elapsed=0.5 → 15，
//   既不等于 total(20) 也不等于 0 —— 取 elapsed 恰好到底那种值，
//   写成"恒返回 total"也照样 PASS。
// ============================================================

#include "dialogue/DialoguePlayer.h"
#include "dialogue/DialogueRunner.h"
#include "dialogue/DialogueSystem.h"

#include "core/Logger.h"
#include "ecs/Components.h"
#include "scene/Scene.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>

namespace dialogue {

namespace {

int g_pass = 0;
int g_fail = 0;

// ⚠ detail 里**不能有空格**：验收脚本用 `detail=(\S*)` 解析。
void report(const char* name, double actual, double expected, double tol) {
    const bool ok = std::fabs(actual - expected) <= tol;
    VK_LOG_INFO("DLG-TEST %s actual=%.6f expected=%.6f tol=%.6f result=%s", name,
                actual, expected, tol, ok ? "PASS" : "FAIL");
    if (ok) ++g_pass;
    else ++g_fail;
}

void reportFlag(const char* name, bool ok, const std::string& detail = {}) {
    VK_LOG_INFO("DLG-TEST %s actual=%d expected=1 tol=0 detail=%s result=%s", name,
                ok ? 1 : 0, detail.c_str(), ok ? "PASS" : "FAIL");
    if (ok) ++g_pass;
    else ++g_fail;
}

// 主夹具：a → q（两个选项）→ b / c（都结束）。
const char* kMain = R"({
  "name": "demo",
  "start": "a",
  "//": "speaker 是显示名，不是 id —— 同一人多处写同一个名字。",
  "lines": [
    { "id": "a", "speaker": "\u5343\u590f", "text": "\u65e9\u4e0a\u597d\u3002", "next": "q" },
    { "id": "q", "speaker": "\u5343\u590f", "text": "\u4eca\u5929\u8981\u505a\u4ec0\u4e48\uFF1F",
      "choices": [
        { "text": "\u53bb\u5b66\u6821",   "goto": "b" },
        { "text": "\u518d\u7761\u4e00\u4f1a", "goto": "c" }
      ] },
    { "id": "b", "speaker": "\u6211", "text": "\u53bb\u5b66\u6821\u5427\u3002" },
    { "id": "c", "speaker": "\u6211", "text": "\u2026\u2026\u518d\u7761\u4e94\u5206\u949f\u3002" }
  ]
})";

DialogueAsset parseIt(const char* text, const char* path = "demo.dlg.json") {
    DialogueAsset a;
    parseDialogue(text, path, a);
    return a;
}

bool errorMentions(const DialogueAsset& a, const char* needle) {
    return a.error.find(needle) != std::string::npos;
}

// ---------------------------------------------------------------- A 解析

void groupParse() {
    const DialogueAsset a = parseIt(kMain);
    reportFlag("parse_ok", a.valid, "err=" + a.error);
    report("parse_lines", static_cast<double>(a.nodes.size()), 4.0, 0.0);
    reportFlag("parse_start_explicit", a.startId == "a", a.startId);
    reportFlag("parse_index_built", findNode(a, "q") == 1,
               "q=" + std::to_string(findNode(a, "q")));
    reportFlag("parse_find_missing", findNode(a, "zzz") == -1);

    // 起点缺省 = 第一个节点（不是"最后写的那条"，也不是字典序）
    DialogueAsset b;
    parseDialogue(R"({"lines":[{"id":"zz","text":"x"},{"id":"aa","text":"y"}]})",
                  "b.dlg.json", b);
    reportFlag("start_defaults_to_first", b.valid && b.startId == "zz", b.startId);

    // 省 id 时自动编号，并记一条警告
    DialogueAsset c;
    parseDialogue(R"({"lines":[{"text":"one"},{"text":"two"}]})", "c.dlg.json", c);
    reportFlag("auto_id", c.valid && findNode(c, "line0") == 0 &&
                              c.warnings.size() == 2,
               "warns=" + std::to_string(c.warnings.size()));

    reportFlag("name_from_path", c.name == "c", c.name);
}

// ---------------------------------------------------------------- B 校验

void groupValidate() {
    // 重复 id：必须**加载失败**（静默合并 = 少一段剧情）
    const DialogueAsset dup = parseIt(
        R"({"lines":[{"id":"a","text":"1"},{"id":"a","text":"2"}]})");
    reportFlag("dup_id_rejected", !dup.valid && errorMentions(dup, "duplicate"),
               dup.error);

    // next 指向不存在的 id
    const DialogueAsset badNext = parseIt(
        R"({"lines":[{"id":"a","text":"1","next":"nope"}]})");
    reportFlag("bad_next_rejected",
               !badNext.valid && errorMentions(badNext, "not a line id"),
               badNext.error);

    // 选项 goto 指向不存在的 id
    const DialogueAsset badGoto = parseIt(
        R"({"lines":[{"id":"a","text":"1","choices":[{"text":"x","goto":"nope"}]}]})");
    reportFlag("bad_goto_rejected",
               !badGoto.valid && errorMentions(badGoto, "goto"), badGoto.error);

    // next 与 choices 并存 = 语义冲突
    const DialogueAsset both = parseIt(
        R"({"lines":[{"id":"a","text":"1","next":"b","choices":[{"text":"x","goto":"b"}]},
                     {"id":"b","text":"2"}]})");
    reportFlag("next_and_choices_rejected",
               !both.valid && errorMentions(both, "both"), both.error);

    // 选项缺 goto / 缺 text
    const DialogueAsset noGoto = parseIt(
        R"({"lines":[{"id":"a","text":"1","choices":[{"text":"x"}]}]})");
    reportFlag("choice_needs_goto", !noGoto.valid && errorMentions(noGoto, "goto"),
               noGoto.error);
    const DialogueAsset noText = parseIt(
        R"({"lines":[{"id":"a","text":"1","choices":[{"goto":"a"}]}]})");
    reportFlag("choice_needs_text", !noText.valid && errorMentions(noText, "text"),
               noText.error);

    // 空表 / 坏 JSON
    const DialogueAsset empty = parseIt(R"({"lines":[]})");
    reportFlag("empty_rejected", !empty.valid, empty.error);
    const DialogueAsset broken = parseIt("{ not json");
    reportFlag("bad_json_rejected", !broken.valid, broken.error);

    // start 指向不存在的 id
    const DialogueAsset badStart = parseIt(
        R"({"start":"zz","lines":[{"id":"a","text":"1"}]})");
    reportFlag("bad_start_rejected",
               !badStart.valid && errorMentions(badStart, "start"), badStart.error);
}

// ---------------------------------------------------------------- C 打字机

void groupTypewriter() {
    // 30 cps × 0.5 s = 15 个字（total 20 → 既不是 0 也不是 20）
    report("tw_mid", revealedChars(0.5f, 30.0f, 20), 15.0, 0.0);
    // cps=0 = 关掉打字机 → 立刻全出
    report("tw_cps0_is_all", revealedChars(0.0f, 0.0f, 20), 20.0, 0.0);
    // 负 cps 同 0（别让它变成负数）
    report("tw_negative_cps_is_all", revealedChars(0.1f, -5.0f, 20), 20.0, 0.0);
    // 超长 elapsed 夹到 total，不是 15×10
    report("tw_clamp_total", revealedChars(10.0f, 30.0f, 20), 20.0, 0.0);
    // elapsed=0 → 0 个字
    report("tw_zero_elapsed", revealedChars(0.0f, 30.0f, 20), 0.0, 0.0);
    // 空台词
    report("tw_empty_line", revealedChars(1.0f, 30.0f, 0), 0.0, 0.0);
}

// ---------------------------------------------------------------- D 播放器

void groupRunner() {
    const DialogueAsset a = parseIt(kMain);
    DialogueRunner r;

    reportFlag("run_start", r.start(a) && r.currentId() == "a", r.currentId());
    reportFlag("run_active", r.active() && !r.finished());
    reportFlag("run_current_text", r.current() && r.current()->speaker.size() > 0);
    report("run_steps_after_start", static_cast<double>(r.steps()), 1.0, 0.0);

    // a → q
    const Advance adv = r.advance();
    reportFlag("run_advance_moved", adv == Advance::Moved && r.currentId() == "q",
               r.currentId());

    // q 有两个选项：advance 必须**原地不动**
    const Advance atQ = r.advance();
    reportFlag("run_choice_brakes", atQ == Advance::NeedChoice, "");
    reportFlag("run_choice_keeps_id", r.currentId() == "q", r.currentId());
    report("run_choice_count", static_cast<double>(r.choiceCount()), 2.0, 0.0);
    reportFlag("run_at_choice", r.atChoice());

    // 越界选择：返回 false 且**状态不变**
    const bool oob = r.choose(7);
    reportFlag("run_choose_oob", !oob && r.currentId() == "q" && !r.error().empty(),
               r.error());

    // 选第二项 → c
    reportFlag("run_choose_moves", r.choose(1) && r.currentId() == "c",
               r.currentId());

    // c 没有 next → 结束，并撒手
    const Advance end = r.advance();
    reportFlag("run_finishes", end == Advance::Finished && !r.active(), "");
    reportFlag("run_current_null_after_end", r.current() == nullptr);
    reportFlag("run_finished_flag", r.finished());
    reportFlag("run_advance_after_end", r.advance() == Advance::Inactive, "");

    // 没 start 就 advance / choose
    DialogueRunner idle;
    reportFlag("run_advance_inactive", idle.advance() == Advance::Inactive, "");
    reportFlag("run_choose_without_start", !idle.choose(0), idle.error());

    // 起点覆盖（调试"从第 N 句开始"）
    DialogueRunner r2;
    reportFlag("run_start_override", r2.start(a, "b") && r2.currentId() == "b",
               r2.currentId());
    // 覆盖成不存在的 id
    DialogueRunner r3;
    reportFlag("run_start_override_bad", !r3.start(a, "zzz") && !r3.active(),
               r3.error());

    // 无效资产不许 start
    DialogueAsset bad;
    bad.valid = false;
    DialogueRunner r4;
    reportFlag("run_invalid_asset", !r4.start(bad), r4.error());

    // 无选项的节点上调 choose
    DialogueRunner r5;
    r5.start(a);   // 停在 a（无选项）
    reportFlag("run_choose_on_plain_line", !r5.choose(0) && r5.currentId() == "a",
               r5.error());
}

// ---------------------------------------------------------------- E 演示资产

// ⚠ 这一组**读盘**（与 A~D 的"纯字符串"不同），钉的是"仓库里那份示例资产
//   真的能加载" —— 手写 JSON 里一个逗号写错，纯函数用例一条都抓不到。
//   路径能解析是因为 `resolveAssetPath` 的候选链里有 `<exe>/assets`，
//   **与当前工作目录无关**（从仓库根跑也找得到）。
void groupDemoAsset() {
    DialogueAsset a;
    const bool ok = loadDialogue("dialogues/ch01_open_01.dlg.json", a);
    reportFlag("demo_loads", ok, "err=" + a.error);
    if (!ok) return;
    report("demo_lines", static_cast<double>(a.nodes.size()), 7.0, 0.0);
    reportFlag("demo_start", a.startId == "l1", a.startId);
    // q1 是两个选项的那个分支点 —— 顺便证明"有 choices 的节点"解析对了
    const int q = findNode(a, "q1");
    reportFlag("demo_choice_count",
               q >= 0 && a.nodes[static_cast<size_t>(q)].choices.size() == 2);
    // 走一遍最短路径：l1 → l2 → q1 →（选 0）→ roof
    DialogueRunner r;
    r.start(a);
    r.advance();
    r.advance();
    const bool atQ = r.currentId() == "q1";
    const bool picked = r.choose(0);
    reportFlag("demo_walk_to_choice", atQ && picked && r.currentId() == "roof",
               r.currentId());
}

// ---------------------------------------------------------------- E 表现层

// `DialoguePlayer` 的三条硬性质：**两段式推进**、**按码点走**、**结束之后
// `finished` 与 `running` 是两个不同的答案**。
void groupPlayer() {
    DialoguePlayer p;
    const bool loaded = p.load("dialogues/ch01_open_01.dlg.json");
    reportFlag("player_load", loaded, p.error());
    if (!loaded) return;
    reportFlag("player_start", p.start(), p.error());

    const DlgNode* n = p.current();
    // ⚠ 这一对是**故意**的：8 个码点 / 24 个字节。只断言 8 的话，一个
    //   按字节计数的实现（`text.size()`）会给出 24 —— 两个都对不上就说明
    //   连"哪个数才对"都没定下来，所以必须成对写。
    report("player_chars", n ? static_cast<double>(p.lineChars()) : -1.0, 8.0,
           0.0);
    report("player_bytes", n ? static_cast<double>(n->text.size()) : -1.0, 24.0,
           0.0);

    // ---- 打字机 ----
    p.tick(0.1f, 20.0f);   // floor(0.1 * 20) = 2
    report("player_tick_2", static_cast<double>(p.revealed()), 2.0, 0.0);
    // 前两个字是 "……"（各 3 字节）。按字节切会得到半个汉字 → 这里必须
    // 与 `utf8Prefix` 逐字节相同。
    reportFlag("player_prefix_ok", p.shownText() == "\u2026\u2026",
               p.shownText());

    // ---- 两段式推进 ----
    reportFlag("player_press1_reveal", p.pressAdvance(20.0f) == Press::Revealed);
    report("player_press1_full", static_cast<double>(p.revealed()), 8.0, 0.0);
    reportFlag("player_press2_move", p.pressAdvance(20.0f) == Press::Moved);
    reportFlag("player_moved_l2", p.currentId() == "l2", p.currentId());
    report("player_l2_reset", static_cast<double>(p.revealed()), 0.0, 0.0);

    // ---- cps <= 0 = 立刻全出 ----
    p.tick(0.0f, 0.0f);
    report("player_cps0_full", static_cast<double>(p.revealed()), 27.0, 0.0);

    // ---- 走到有选项的那一行 ----
    reportFlag("player_walk_q1", p.pressAdvance(20.0f) == Press::Moved &&
                                     p.currentId() == "q1",
               p.currentId());
    reportFlag("player_q1_reveal", p.pressAdvance(20.0f) == Press::Revealed);
    // ⚠ 第二次按的结果是 **NeedChoice**，不是 Moved —— 这就是"选项不会
    //   被顺手跳过去"的那一条。
    reportFlag("player_q1_needchoice", p.pressAdvance(20.0f) == Press::NeedChoice);
    report("player_q1_choices", static_cast<double>(p.choiceCount()), 2.0, 0.0);
    reportFlag("player_choose", p.choose(1) && p.currentId() == "sleep",
               p.currentId());

    // ---- 走到结束 ----
    reportFlag("player_end_reveal", p.pressAdvance(20.0f) == Press::Revealed);
    reportFlag("player_end_move", p.pressAdvance(20.0f) == Press::Moved);
    reportFlag("player_end_id", p.currentId() == "sleep_end", p.currentId());
    reportFlag("player_end_reveal2", p.pressAdvance(20.0f) == Press::Revealed);
    reportFlag("player_finished", p.pressAdvance(20.0f) == Press::Finished);
    reportFlag("player_not_running", !p.running());
    reportFlag("player_finished_flag", p.finished());
    // 结束之后再按：Inactive（不是崩溃、不是重复 Finished）。
    reportFlag("player_after_end", p.pressAdvance(20.0f) == Press::Inactive);

    // ---- 重播 ----
    reportFlag("player_replay",
               p.start() && p.running() && p.revealed() == 0 && !p.finished());

    // ---- UTF-8 工具本体 ----
    reportFlag("utf8_count_cjk", utf8Count("\u5343\u590f") == 2,
               std::to_string(utf8Count("\u5343\u590f")));
    reportFlag("utf8_prefix_cjk",
               utf8Prefix("\u5343\u590f" "ab", 3) == "\u5343\u590fa",
               utf8Prefix("\u5343\u590f" "ab", 3));
}

// ---------------------------------------------------------------- F 调度层

// `DialogueSystem` 的四件事：路径解析、cue 起播、**不往场景加组件**、
// 组件自动起播与 `resetRuntime`。
void groupSystem() {
    reportFlag("sys_path_short",
               resolveDialoguePath("ch01_open_01") ==
                   "dialogues/ch01_open_01.dlg.json",
               resolveDialoguePath("ch01_open_01"));
    reportFlag("sys_path_explicit",
               resolveDialoguePath("dialogues/x.dlg.json") ==
                   "dialogues/x.dlg.json",
               resolveDialoguePath("dialogues/x.dlg.json"));
    reportFlag("sys_path_empty", resolveDialoguePath("").empty());

    scene::Scene sc;
    const ecs::Entity dir = sc.createObject("Director");
    DialogueSystem sys;
    reportFlag("sys_cue", sys.cue(sc, dir, "ch01_open_01"), sys.error());
    // ⚠⚠ 这一条最重要：cue **不给场景加组件**。加了的话，场景一存盘就
    //   永久多出一段作者从没配过的 `dialogue` 块。
    reportFlag("sys_no_component", !sc.world().has<ecs::DialogueComponent>(dir));
    reportFlag("sys_active", sys.active(sc));

    sys.tick(sc, 0.1f);   // 默认 cps=30 → floor(3)
    {
        const DialoguePlayer* p = sys.activePlayer(sc);
        report("sys_tick_3", p ? static_cast<double>(p->revealed()) : -1.0, 3.0,
               0.0);
    }

    reportFlag("sys_reveal", sys.pressAdvance(sc) == Press::Revealed);
    reportFlag("sys_move", sys.pressAdvance(sc) == Press::Moved);
    {
        const DialoguePlayer* p = sys.activePlayer(sc);
        reportFlag("sys_at_l2", p && p->currentId() == "l2",
                   p ? p->currentId() : std::string("null"));
    }
    // l2 → q1
    sys.pressAdvance(sc);   // Revealed
    reportFlag("sys_to_q1", sys.pressAdvance(sc) == Press::Moved);
    sys.pressAdvance(sc);   // Revealed
    reportFlag("sys_needchoice", sys.pressAdvance(sc) == Press::NeedChoice);
    reportFlag("sys_choose", sys.choose(sc, 0));
    {
        const DialoguePlayer* p = sys.activePlayer(sc);
        reportFlag("sys_at_roof", p && p->currentId() == "roof",
                   p ? p->currentId() : std::string("null"));
    }
    // roof → roof_end → 结束
    sys.pressAdvance(sc);
    sys.pressAdvance(sc);
    sys.pressAdvance(sc);
    reportFlag("sys_end_finished", sys.pressAdvance(sc) == Press::Finished);
    reportFlag("sys_inactive_after_end", !sys.active(sc));

    // ---- 组件驱动 + autoStart ----
    scene::Scene sc2;
    const ecs::Entity npc = sc2.createObject("NPC");
    ecs::DialogueComponent dc;
    dc.dialoguePath = "dialogues/ch01_open_01.dlg.json";
    dc.autoStart = true;
    dc.cps = 10.0f;
    sc2.world().add<ecs::DialogueComponent>(npc, dc);

    DialogueSystem sys2;
    sys2.tick(sc2, 0.016f);
    reportFlag("sys_auto_active", sys2.active(sc2));
    {
        const DialoguePlayer* p = sys2.activePlayer(sc2);
        report("sys_auto_reveal0", p ? static_cast<double>(p->revealed()) : -1.0,
               0.0, 0.0);
    }
    sys2.tick(sc2, 0.5f);   // (0.016+0.5)*10 = 5.16 → 5
    {
        const DialoguePlayer* p = sys2.activePlayer(sc2);
        report("sys_auto_reveal5", p ? static_cast<double>(p->revealed()) : -1.0,
               5.0, 0.0);
    }
    sys2.resetRuntime(sc2);
    reportFlag("sys_reset", !sys2.active(sc2));
}

// ---------------------------------------------------------------- G 写回
//
// 剧本编辑器（`editor/DialogueEditorWindow`）的存盘就走这一组。
// ⚠ 这一组钉的不是"能写出文件"，是**往返不丢东西** —— 写出去再读回来，
//   节点顺序 / id / 文本 / 跳转必须一模一样。它是编辑器敢按 Save 的前提：
//   丢掉一句台词的表现是"我又没动它，它怎么没了"，比打不开更难受。
void groupRoundTrip() {
    const DialogueAsset a = parseIt(kMain);
    const std::string text = toJson(a);

    DialogueAsset b;
    parseDialogue(text, "demo.dlg.json", b);
    reportFlag("rt_ok", b.valid, b.error);

    bool sameShape = b.nodes.size() == a.nodes.size();
    if (sameShape) {
        for (size_t i = 0; i < a.nodes.size(); ++i) {
            if (b.nodes[i].id != a.nodes[i].id ||
                b.nodes[i].nextId != a.nodes[i].nextId ||
                b.nodes[i].choices.size() != a.nodes[i].choices.size()) {
                sameShape = false;
                break;
            }
        }
    }
    reportFlag("rt_nodes", sameShape,
               "a=" + std::to_string(a.nodes.size()) + "/b=" +
                   std::to_string(b.nodes.size()));

    // 中文必须**原样**（不是 \u5343\u590f）。按字节比：两个方向的错都能抓到
    // —— 转义了（存进去是反斜杠）与编码坏了（存进去是 ?）都不等于原文。
    const std::string cn = "\u5343\u590f";   // 千夏
    const bool textKept = b.nodes.size() > 0 && b.nodes[0].speaker == cn &&
                          b.nodes[0].text == a.nodes[0].text;
    reportFlag("rt_utf8_raw", textKept &&
                                  text.find("\\u") == std::string::npos,
               "esc=" + std::to_string(text.find("\\u") == std::string::npos));

    // 选项：两条 goto 都要在
    const int qi = findNode(b, "q");
    const bool chOk = qi >= 0 && b.nodes[static_cast<size_t>(qi)].choices.size() == 2 &&
                      b.nodes[static_cast<size_t>(qi)].choices[0].gotoId == "b" &&
                      b.nodes[static_cast<size_t>(qi)].choices[1].gotoId == "c";
    reportFlag("rt_choices", chOk,
               "qi=" + std::to_string(qi) +
                   "/n=" + std::to_string(qi >= 0
                                              ? static_cast<int>(b.nodes[
                                                    static_cast<size_t>(qi)]
                                                        .choices.size())
                                              : -1));

    // 空 next / 空 choices **整字段省略**：b 与 c 都没有 next，
    // 原文里 `"next"` 只该出现两次（a→q，q 没有 next）。
    int nextKeys = 0;
    for (size_t p = text.find("\"next\""); p != std::string::npos;
         p = text.find("\"next\"", p + 1))
        ++nextKeys;
    reportFlag("rt_omits_empty_next", nextKeys == 1,
               "next_keys=" + std::to_string(nextKeys));

    // 起点**不是第一个节点**时也要原样（写死成 nodes[0] 的实现在这里红）
    DialogueAsset c;
    parseDialogue(R"({"name":"s","start":"b","lines":[
        {"id":"a","text":"1","next":"b"},{"id":"b","text":"2"}]})",
                  "s.dlg.json", c);
    DialogueAsset d;
    parseDialogue(toJson(c), "s.dlg.json", d);
    reportFlag("rt_keeps_start", c.valid && d.valid && d.startId == "b",
               "start=" + d.startId);

    // `index` 是缓存，**不许**落盘（写进去等于把身份写死成下标）
    reportFlag("rt_no_index_key", text.find("\"index\"") == std::string::npos);

    // 真写盘再读回（临时文件，跑完删）
    std::error_code ec;
    const std::filesystem::path tmp =
        std::filesystem::temp_directory_path(ec) / "venn_dlg_rt.dlg.json";
    std::string err;
    const bool saved = !ec && saveDialogue(tmp.generic_string(), a, err);
    DialogueAsset e;
    const bool reloaded = saved && loadDialogue(tmp.generic_string(), e);
    const bool fileOk = reloaded && e.valid && e.nodes.size() == a.nodes.size() &&
                        e.nodes.size() > 1 && e.nodes[1].id == "q" &&
                        e.startId == "a";
    reportFlag("rt_file", fileOk,
               "saved=" + std::to_string(saved) + "/err=" + err);
    if (!ec) std::filesystem::remove(tmp, ec);
}

} // namespace

int runDialogueSelfTest() {
    g_pass = 0;
    g_fail = 0;
    groupParse();
    groupValidate();
    groupTypewriter();
    groupRunner();
    groupDemoAsset();
    groupPlayer();
    groupSystem();
    groupRoundTrip();
    VK_LOG_INFO("DLG-TOTAL pass=%d fail=%d", g_pass, g_fail);
    return g_fail;
}

} // namespace dialogue
