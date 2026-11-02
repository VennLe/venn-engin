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
// · **不做变量替换**（`{player}` 之类）：那需要黑板/存档，现在做等于先造
//   一个用不上的求值器。文本原样交给 UI。
// · **不做立绘/表情字段**：等资产真的有了再加 —— 加了没人消费的字段，
//   序列化与校验会立刻变成"永远为空的必填项"。
//
// ------------------------------------------------------------
// C7（2026-11-05）：条件分支 + 选项写旗标
// ------------------------------------------------------------
// 这一版补的正是上面"不做变量替换 / 不做条件分支"里被推迟的那半：
//   · `DlgNode::conds`（JSON 的 `if`）+ `DlgNode::elseId`（`else`）——
//     "条件不满足就跳到另一句"；
//   · `DlgChoice::writes`（`set`)—— 选了就写旗标。
// 变量替换仍然不做（文本里插值要格式串求值器，那是另一件事）。
//
// ⚠⚠ **条件只读、不写**。写只发生在"选了某个选项"这一处 —— 于是一条分支
//   的副作用永远能在一个地方读完（选项列表），而不是散落在节点上。
// ⚠⚠ **`else` 为空 = 这段对话到此结束**，不是"顺着 next 走"。后者看起来
//   更"顺"，但会让作者分不清"条件句"和"分支句"，而两种写法都合法时
//   总有人写出自己不想要的那一种。要"跳过这句继续"就显式写 `"else": "下一句"`。
// ============================================================

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace dialogue {

// 一条旗标条件。多条之间是 **AND**（全部成立才进这句）。
//
// `op` 只认六种：`>=` `<=` `>` `<` `==` `!=`（也接受写法 `=`）。
// ⚠ 不做 `&&` / `||` 嵌套表达式：那需要一个求值器，而剧本作者要的
//   "好感度 ≥ 2 且 见过她" 用**多条条件**就表达了。OR 用两个节点 +
//   `else` 拼，读起来更清楚。
struct DlgCond {
    std::string flag;
    std::string op = ">=";
    double value = 0.0;
};

// 一条旗标写入（选项的副作用）。
struct DlgWrite {
    std::string flag;
    double value = 0.0;
};

struct DlgChoice {
    std::string text;    // 按钮上显示的字
    std::string gotoId;  // 选了之后跳到哪（必须是存在的 id）
    // 选了之后写哪些旗标。空 = 什么都不写（老资产逐位不变）。
    std::vector<DlgWrite> writes;
};

struct DlgNode {
    std::string id;       // 全局唯一
    std::string speaker;  // 说话人**显示名**（同一个人多处写同一个名字）
    std::string text;     // 台词本体（可以含 `\n`）
    std::string nextId;   // 说完去哪；空 = 结束（有 choices 时必须为空）
    // 这句的语音（音频路径，走 `assets/` 根）。空 = 这句没有语音（老资产）。
    // D1：`DialoguePlayer` 通过宿主注入的钩子播它，并在"还没播完"时不翻页。
    std::string voice;
    // C7：进入这句之前要成立的条件（空 = 无条件，老资产逐位不变）。
    // 不成立时跳到 `elseId`；`elseId` 空 → **结束**。
    std::vector<DlgCond> conds;
    std::string elseId;
    std::vector<DlgChoice> choices;
};

// 旗标仓库的**接口**（宿主实现，引擎只认这三个方法）。
//
// 为什么是纯虚接口而不是 `std::function` 三件套：条件求值要**同时**读
// 多个旗标（AND 链），而 `std::function<double(const std::string&)>` 那样
// 也行 —— 但"写"必须和"读"落在同一张表上，两个独立 std::function 没法
// 保证这件事（宿主完全可能把它们接到两个不同的 map 上，而症状是
// "选了选项、条件没变"，一路无错）。一个接口把这条约束表达在类型里。
//
// ⚠ 未设过 = 0（与 `scene::GameFlags` 同一条语义）。
class DlgFlagStore {
  public:
    virtual ~DlgFlagStore() = default;
    virtual double get(const std::string& name) const = 0;
    virtual void set(const std::string& name, double value) = 0;
};

// 条件求值（`conds` 为空 → true）。**纯函数**，所以自检不需要任何宿主。
bool evalConds(const std::vector<DlgCond>& conds, const DlgFlagStore* store);

// `op` 合法性（解析期用）。返回 nullptr = 合法。
const char* condOpError(const std::string& op);

// 把"两个 std::function"包成 `DlgFlagStore` 的桥。
//
// 为什么需要它：两个宿主（`Game.exe` / 编辑器）各有一张
// `scene::GameFlags`，而 `engine/scene` **不能** include `engine/dialogue`
// （依赖方向反了，而且 `DialogueSystem` 只是前向声明 `scene::Scene`）。
// 于是桥放在 dialogue 这一侧、只认两个函数 —— 宿主各写两行 lambda 即可，
// 不必各自复制一个 class（复制两份 = 改一处漏一处）。
class FlagStoreBridge final : public DlgFlagStore {
  public:
    FlagStoreBridge(std::function<double(const std::string&)> getter,
                    std::function<void(const std::string&, double)> setter)
        : m_get(std::move(getter)), m_set(std::move(setter)) {}
    double get(const std::string& name) const override {
        return m_get ? m_get(name) : 0.0;
    }
    void set(const std::string& name, double value) override {
        if (m_set) m_set(name, value);
    }
    // 两个函数都在才算"可用"：只装一半的桥写进去没效果，与没装一样危险。
    bool usable() const { return m_get && m_set; }

  private:
    std::function<double(const std::string&)> m_get;
    std::function<void(const std::string&, double)> m_set;
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
