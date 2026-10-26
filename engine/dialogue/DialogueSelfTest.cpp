// ============================================================
// dialogue/DialogueSelfTest.cpp
//
// C2 对话系统的无窗口自检。分四组：
//   A 解析（节点数 / 起点推导 / 自动 id）
//   B 校验（重复 id / 跳转缺失 / next 与 choices 并存 / 选项缺字段 / 空表）
//   C 打字机（cps=0 立刻全出 / 夹取 / elapsed 超长）
//   D 播放器（推进 / 选项刹车 / 越界不改状态 / 结束撒手 / 起点覆盖）
//
// ⚠ 每条断言都问过："**别的实现会给出不同的数吗**"。
//   打字机那条特意取 cps=30、elapsed=0.5 → 15，
//   既不等于 total(20) 也不等于 0 —— 取 elapsed 恰好到底那种值，
//   写成"恒返回 total"也照样 PASS。
// ============================================================

#include "dialogue/DialogueRunner.h"

#include "core/Logger.h"

#include <cmath>
#include <cstdio>
#include <string>

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

} // namespace

int runDialogueSelfTest() {
    g_pass = 0;
    g_fail = 0;
    groupParse();
    groupValidate();
    groupTypewriter();
    groupRunner();
    groupDemoAsset();
    VK_LOG_INFO("DLG-TOTAL pass=%d fail=%d", g_pass, g_fail);
    return g_fail;
}

} // namespace dialogue
