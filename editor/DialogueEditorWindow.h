#pragma once
// ============================================================
// editor/DialogueEditorWindow —— 剧本编辑器（C2 的第三块：面板）
//
// #308 只给了 Inspector 的 4 个控件（路径 / 启用 / 打字速度 / 起始节点），
// 剧本正文仍然要**手写 JSON** —— 也就是"引擎能播对话，作者却得开文本
// 编辑器写台词"。这个窗口把正文也吃掉：
//
//   ┌ Script editor ─ ch01_open_01 *───────────────────────────────────┐
//   │ [dialogues/ch01_open_01.dlg.json] [Reload] [Save] [New]          │
//   ├───────────────┬──────────────────────────────────────────────────┤
//   │ Lines (7)     │ id [l1]      speaker [千夏]      [Set as start]   │
//   │ [+][Dup][Del] │ text ┌──────────────────────────────────────────┐ │
//   │ [↑][↓]        │      │ ……又是这个梦。                            │ │
//   │               │      └──────────────────────────────────────────┘ │
//   │ ▶ l1  千夏    │ next [l2 ▾]                                      │
//   │   l2  千夏    │ ── choices ──  [text]          [goto ▾]   [x]    │
//   │   q1  千夏    │ [+ choice]                                       │
//   ├───────────────┴──────────────────────────────────────────────────┤
//   │ Load check: ok / 1 error   ·  ── advice ──                       │
//   └──────────────────────────────────────────────────────────────────┘
//
// ------------------------------------------------------------
// 五条"省了就错"的设计约束
// ------------------------------------------------------------
// ⚠⚠ ① **校验不在这里实现**。每次改动都是"序列化 → 丢回 `parseDialogue`"，
//     用的是运行时同一个校验器。自己再写一套"顺手检查一下"的后果是两套
//     规则，而作者只在运行时看到另一套 —— 症状是"编辑器说没问题，进 Play
//     就 DLG-LOAD-FAIL"，而且两边都"有理"。
//
// ⚠⚠ ② **改 id 要连引用一起改**。id 是身份（见 `DialogueAsset.h` ①），
//     只改本节点的话所有 `next` / `goto` 立刻悬空。校验器会很诚实地报
//     `line "x": next "old" is not a line id`，但作者得自己一行行找回去。
//     这里改名时把全表的 `next` / `goto` / `start` 一起打补丁并记账。
//
// ⚠⚠ ③ **读盘失败时禁止存盘**。`parseDialogue` 是"边解析边填"，半路出错
//     时 `nodes` 里是**前几行的残片**。此时按 Save 会把残片写回原文件 ——
//     一份 300 行的剧本因为第 120 行少了个逗号，被截成 119 行。
//     所以失败即 `readOnly`：能看、能改缓冲区、**不能落盘**，修好后 Reload。
//
// ⚠⚠ ④ **存盘前必须校验通过**，除非明确按 "Save anyway"。写一份加载不了
//     的文件等于把问题推给运行时，而它的症状是"进 Play 黑屏 + 一行
//     DLG-LOAD-FAIL"。逃生门留着（WIP 存档是正常需求），但它是个**显式
//     按钮**，不是默认行为。
//
// ⚠⚠ ⑤ **不在编辑态跑 `DialogueSystem`**。与 #308 同一条纪律：它只在运行时
//     tick。所以这里**没有**预览播放按钮 —— 按下去什么都不动的按钮比没有
//     更糟。能给的是"这份 JSON 解析得过"，那正是底部校验栏。
//
// ------------------------------------------------------------
// 边界：它改的是 `.dlg.json`，只碰一次场景
// ------------------------------------------------------------
// 存盘之后顺手让场景里**引用同一份剧本**的组件重新读一遍。不刷的话：
// 作者按了 Save、窗口里是新台词、进 Play 还是旧台词（`DialoguePlayer::load`
// 按路径缓存，同路径重复调用**不重读**）。正在播的那一份不打断，只记账
// 等它下一次起播。
// ============================================================

#include "dialogue/DialogueAsset.h"

#include <string>
#include <vector>

namespace editor {

class EditorContext;

class DialogueEditorWindow {
  public:
    explicit DialogueEditorWindow(EditorContext& ctx);

    // 打开并读一份资产。`relPath` 为空 = 空手开窗（只剩"New script"）——
    // 从 View 菜单点开时就是这条路径。
    // ⚠ 读失败也**开窗**：把错误摆出来，比"点了没反应"有用得多。
    bool open(const std::string& relPath);
    void draw(bool& open);

    bool isOpen() const { return m_open; }
    void close();

    // 有未存的改动（关闭时要不要提醒，由调用方决定）
    bool dirty() const { return m_dirty; }

  private:
    // ---------------------------------------------------------- 数据
    dialogue::DialogueAsset m_asset;
    std::string m_path;        // 资产路径原样（存回同一处）
    std::string m_error;       // 读/写盘的**人话**错误（不随编辑消失）
    std::string m_uiError;     // 最近一次操作被拒的原因（下一次成功操作清掉）
    bool m_open = false;
    bool m_loaded = false;     // open() 已经跑完（注入脚本要等它）
    bool m_dirty = false;
    bool m_readOnly = false;   // 见 ③
    bool m_injected = false;

    // ---------------------------------------------------------- 校验
    // `m_checkValid` 是**序列化后重新解析**的结论，不是编辑器自己算的。
    bool m_checkValid = false;
    std::string m_checkError;
    std::vector<std::string> m_checkWarnings;
    // 上一次打日志时的结论，用于"只在结论变化时打"（否则每敲一个字一行）
    std::string m_checkLogKey;
    void revalidate();

    // ---------------------------------------------------------- 选择
    int m_sel = 0;
    void selectNode(int i);
    int nodeCount() const { return static_cast<int>(m_asset.nodes.size()); }
    dialogue::DlgNode* selNode();

    // ---------------------------------------------------------- 编辑缓冲
    //
    // ⚠ 不直接让 InputText 写 `m_asset.nodes[sel]`：删节点 / 上移下移会让
    //   指针立刻悬垂，而 ImGui 的 activeId 还记着那个控件（下一帧就写进
    //   已释放内存）。缓冲按**节点 id**失效重建（不是按下标 —— 下标会变）。
    std::string m_bufForId;
    char m_idBuf[128] = {};
    char m_speakerBuf[256] = {};
    char m_pathBuf[512] = {};      // 工具栏里的资产路径输入框
    std::vector<char> m_textBuf;   // 多行文本（imgui_stdlib 没编进工程）
    void syncBuffers();
    void refillIdBuf();

    // ---------------------------------------------------------- 动作
    // UI 按钮与注入脚本**共用**这一份：脚本改的和手点的是同一条路，
    // 于是"能不能自动验"不取决于有没有把按钮坐标算对。
    bool actNew(const std::string& wantId = {});   // 末尾加一句
    bool actDuplicate();
    bool actDelete();
    bool actMove(int dir);                         // -1 上移 / +1 下移
    bool actSetId(const std::string& newId);       // 连引用一起打补丁（②）
    bool actSetSpeaker(const std::string& s);
    bool actSetText(const std::string& t);
    bool actSetNext(const std::string& id);        // "" = 说完结束
    bool actSetStart();
    bool actAddChoice(const std::string& gotoId);
    bool actDelChoice(int i);
    bool actSetChoiceText(int i, const std::string& t);
    bool actSetChoiceGoto(int i, const std::string& id);
    bool actNewScript();                           // 从零建一份两行的骨架

    bool save(bool force);
    void reload();
    void refreshSceneComponents();

    // 唯一 id：`base` 被占用就 base2 / base3 ……（id 是身份，绝不合并）
    std::string uniqueId(const std::string& base) const;
    // `index` 是运行时缓存，任何"改了 id / 删了节点"的动作之后必须重建
    // （不重建的后果：`findNode` 指着已经换了位置的节点）
    void rebuildIndex();
    // 还指向这个 id 的引用条数（删节点前记账，也让脚本能断言）
    int countRefs(const std::string& id) const;
    // 从 start 出发能走到的节点（"游离节点"提示用，不是校验规则）
    std::vector<char> reachable() const;
    void markDirty();
    // 操作被拒的统一出口：记一行日志 + 存一句人话给界面
    bool reject(const char* op, const std::string& reason);

    // ---------------------------------------------------------- 绘制
    void drawToolbar(bool& open);
    void drawNodeList(float height);
    void drawNodeEditor(float height);
    void drawValidation();

    // ---------------------------------------------------------- 自动化
    // `MYVK_EDITOR_DLGED_SCRIPT="sel:2;settext:你好;new:l9;save"`，
    // 分号分隔、冒号分隔参数。**只跑一次**（与 MYVK_EDITOR_REBIND 同纪律）。
    // ⚠ 参数里不能再出现 `;` / `:` —— 它是个测试钩子，不是脚本语言。
    void runInjectedScript();

    EditorContext& m_ctx;
};

} // namespace editor
