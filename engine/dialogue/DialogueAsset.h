#pragma once
// ============================================================
// dialogue/DialogueAsset —— 对话资产（`.dlg.json`）的解析与校验
//
// 这是 C2「对话 / 字幕」的**数据层**：把一份 JSON 变成"一串按 id 串起来的
// 台词节点"，并在加载时就把所有"演到一半才会炸"的问题查出来。
//
// ------------------------------------------------------------
// 四个语义（都是"看起来能省、省了就错"的）
// ------------------------------------------------------------
// ⚠⚠ ① **节点按 `id` 寻址，不按下标**。用下标当身份，改剧本（中间插一句）
//     就会让所有 `goto` 整体错位 —— 而且新剧本"能跑"，只是跳错地方。
//     这是材质系统踩过的同一个坑（见 MEMORY「材质身份是数组下标」）。
//
// ⚠⚠ ② **`goto` / `next` 的目标在加载时校验**。留到运行期才查的话，症状是
//     "前 20 句都好的，第 21 句突然没了" —— 作者要重播整段才复现。
//     指向不存在的 id = **加载失败**（不是警告）：它一定是作者的手误。
//
// ⚠⚠ ③ **有选项的节点，`advance()` 不前进**（见 DialogueRunner）。写法上
//     把"还有下一句"和"等玩家选"混成一个"可以继续"，就会出现"选项自己
//     跳过去了、玩家根本没看见"。
//
// ⚠⚠ ④ **打字机 `cps <= 0` 表示"立刻全出"，不是"永远不显示"**。
//     `floor(elapsed * cps)` 在 cps=0 时恒等于 0 —— 字幕永远是空的，
//     而日志里一切正常。0 在这里是"关掉打字机"的约定值。
//
// ------------------------------------------------------------
// 不做什么（刻意的）
// ------------------------------------------------------------
// · **不做变量替换**（`{player}` 之类）：那需要黑板/存档（C3/C6），现在做
//   等于先造一个用不上的求值器。文本原样交给 UI。
// · **不做条件分支**（`if flag`）：同上，要 C7 的任务标记。
// · **不做立绘/表情/语音字段**：等资产真的有了再加 —— 加了没人消费的字段，
//   序列化与校验会立刻变成"永远为空的必填项"。
// ============================================================

#include <string>
#include <unordered_map>
#include <vector>

namespace dialogue {

struct DlgChoice {
    std::string text;    // 按钮上显示的字
    std::string gotoId;  // 选了之后跳到哪（必须是存在的 id）
};

struct DlgNode {
    std::string id;       // 全局唯一
    std::string speaker;  // 说话人**显示名**（同一个人多处写同一个名字）
    std::string text;     // 台词本体（可以含 `\n`）
    std::string nextId;   // 说完去哪；空 = 结束（有 choices 时必须为空）
    std::vector<DlgChoice> choices;
};

struct DialogueAsset {
    std::string name;
    std::string sourcePath;
    std::vector<DlgNode> nodes;   // **文件顺序**（作者写的顺序，不重排）
    std::string startId;          // `start` 字段；缺省 = 第一个节点的 id
    bool valid = false;
    std::string error;
    std::vector<std::string> warnings;

    // 运行时索引：id → nodes 下标。⚠ 它是由 `parseDialogue` 重建的**缓存**，
    // 序列化不写它（写了就等于把"身份"落盘，和 ① 那条坑同源）。
    std::unordered_map<std::string, int> index;
};

// 解析 + 校验。`text` 是 JSON 全文，`sourcePath` 只用于日志与 `name` 缺省值。
// 返回 `out.valid`。失败时 `out.error` 是"人能读懂的一句话"。
bool parseDialogue(const std::string& text, const std::string& sourcePath,
                   DialogueAsset& out);

// 读盘后走 `parseDialogue`。路径按 `assets::resolveAssetPath` 解析。
bool loadDialogue(const std::string& path, DialogueAsset& out);

// ------------------------------------------------------------
// 写回（剧本编辑器的存盘路径）
// ------------------------------------------------------------
// `toJson` 只写**作者写的东西**，四样东西刻意不写：
//   · `index` —— 它是 `parseDialogue` 重建的缓存。落盘等于把"身份"写进
//     文件，与 ① 那条坑同源（改一句 → 下标全错）。
//   · `error` / `warnings` —— 加载期的诊断，不是内容。写进去下次读盘会
//     把上次的警告当成"这次也这样"。
//   · 空 `next` / 空 `choices` —— 整个字段省略。写 `"next": ""` 会让
//     校验器以为作者显式指定了"跳到空 id"。
//   · 中文**不转义**（`ensure_ascii=false`）：转义之后 diff 是一屏
//     `\u5343\u590f`，作者没法用 git 看自己改了什么。
//
// `saveDialogue` 是**写临时文件再改名**，不是直接覆盖：写一半断电/磁盘满
// 的后果是"文件打不开"，而存档点就在一个快捷键之外。
bool saveDialogue(const std::string& path, const DialogueAsset& a,
                  std::string& err);
std::string toJson(const DialogueAsset& a);

// id → 下标；找不到返回 -1。⚠ 调用方必须处理 -1，别拿它当"最后一个"。
int findNode(const DialogueAsset& a, const std::string& id);

// 打字机：`elapsed` 秒后应该显示多少个字符。
// `cps <= 0` → 立刻全出（见 ④）；结果夹在 `[0, total]`。
int revealedChars(float elapsed, float cps, int total);

// ------------------------------------------------------------
// UTF-8：打字机按**字符**走，不是按字节
// ------------------------------------------------------------
// ⚠⚠ 这是"省了就错"的第五条：`text[i]` 切出来的不是字，是字节。一个汉字
//     占 3 字节，按字节截断会在中间劈开 —— 症状是**整行变成一串 `?`**
//     （SdfFont 的缺字兜底），而且 `shown` 计数看起来"很合理"，日志也对。
//     `str.substr(0, n)` 同理。
//
// 两个工具都按**码点**计：`utf8Count` 数出 n 个字符，`utf8Prefix` 取前 n 个
// 字符。非法序列按 1 字节前进（与 `ui2::utf8Next` 同一条纪律：不能跳整段，
// 否则一个坏字节会让后面整行字全丢）。
int utf8Count(const std::string& s);
std::string utf8Prefix(const std::string& s, int chars);

} // namespace dialogue
