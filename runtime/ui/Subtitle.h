#pragma once
// ============================================================
// runtime/ui/Subtitle —— 字幕框（C2）
//
// 与 `Hud` 同一条分工：**模型是纯数据**（`SubtitleModel`），这里只做几何与
// 绘制，所以"字有没有出完 / 有几个选项 / 焦点在第几个"可以无窗口断言，
// 不必截图猜。
//
// ------------------------------------------------------------
// 为什么字幕框不自己读 `DialogueSystem`
// ------------------------------------------------------------
// `DialogueSystem` 在 engine，`Subtitle` 在 runtime —— 反过来 include 是
// 允许的，但那样"字幕框长什么样"就和"对话怎么推进"绑死了：以后要做
// "旁白/气泡/底部大字幕"三种皮肤，就得在同一个类里分支。
// 现在 `RuntimeApp` 每帧把播放器的状态抄进 `SubtitleModel`，换皮肤只换绘制。
//
// ------------------------------------------------------------
// 三个"位置写死就会错"的约束
// ------------------------------------------------------------
// ⚠ ① **必须避开左下角的血条**。字幕框走 `BottomCenter`，底边留 96 参考像素
//      —— 血条在 `BottomLeft`、高 24 + margin 24，两者不会叠。
// ⚠ ② **选项列表长在框里，框高跟着涨**。把选项画在框外（比如框下方）会
//      在 720p 上被裁掉一半，而"看起来只是少了一个选项"。
// ⚠ ③ **正文不自动折行**。作者用 `\n` 控制断句（中文没有空格，自动折行
//      得先做断词，而断词错了比溢出更难看）。框高按 `\n` 的行数算。
// ============================================================

#include "ui2/UiCanvas.h"
#include "ui2/UiLayout.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ui2 {
struct SdfFont;
}

namespace runtime {

struct SubtitleModel {
    bool visible = false;
    std::string id;         // 台词 id（日志 + UI 高亮用）
    std::string speaker;    // 显示名（可能为空 = 旁白）
    std::string text;       // **已经按打字机截断**的正文（可含 '\n'）
    int totalChars = 0;     // 这一行一共多少字符（码点）
    int shownChars = 0;     // 已经出到第几个字符（码点，不是字节）
    bool typing = false;    // 还没出完 → 画提示符
    bool atChoice = false;
    std::vector<std::string> choices;
    int focus = 0;
};

class SubtitleBox {
  public:
    // 主框（输出像素）。高度由"行数 + 选项数"算出来，见文件头 ②③。
    ui2::UiRect boxRect(const SubtitleModel& m, uint32_t outW,
                        uint32_t outH) const;
    // 第 i 个选项的按钮矩形（i 从 0 起，不检查 atChoice）。
    ui2::UiRect choiceRect(const SubtitleModel& m, int i, uint32_t outW,
                           uint32_t outH) const;

    void build(ui2::UiCanvas& c, const SubtitleModel& m, uint32_t outW,
               uint32_t outH, const ui2::SdfFont* font) const;

  private:
    // 日志节流：SUB-BOX / SUB-CHOICE 只在布局变化时打，SUB-TYPE 每 30 帧
    // 打一次（证明打字机真的在走）。mutable：build 是 const，但打日志不改
    // 画出来的东西（与 `Hud` 同一个理由）。
    mutable unsigned m_frames = 0;
    mutable std::string m_lastLayoutKey;
};

} // namespace runtime
