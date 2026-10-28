#pragma once
// ============================================================
// editor/EditorLayoutIO —— 编辑器布局落盘 / 读回（E04 + E05 的持久化面）
//
// 落盘内容（一个 json，够小、够稳）：
//   panels[]        面板显隐（id → bool，与 PanelRegistry 的 kPanels 对账）
//   workspace       当前工作区 id（level / rendering / …）
//   split[]         分栏树每个节点的比例 + 折叠态
//   viewportLayout  视口布局（single / 2v / 2h / quad）
//   viewMode        渲染视图模式（lit / unlit / normal / …）
//   maximized       被最大化的面板 id（"" = 没有）
//   slots[]         叶子 → 面板（E29-1；缺省 = 默认树）
//
// ------------------------------------------------------------
// 两条硬规则（和 core/WindowGeometry 同一套）
// ------------------------------------------------------------
// 1) **自动化运行绝不读、绝不写**。跑回归时命令行会设 MYVK_FRAMES 或
//    MYVK_NO_WINDOW_SAVE；只要看到这两个之一，load 直接返回 false、
//    save 直接空转。理由：面板矩形的像素位置进截图断言，任何"上次会话
//    留下的布局"都会让同一份二进制两次跑出不同的图。
// 2) 默认路径 `config/editor_layout.json` 落在**当前工作目录**（`make run`
//    会把 cwd 设到 build/bin，双击 exe 时就是 exe 目录）—— 与
//    editor_window.ini / imgui.ini 保持一致，别分三处。
//    想指定别处：MYVK_EDITOR_LAYOUT=<路径>。
// ============================================================

#include <string>
#include <utility>
#include <vector>

namespace editor {

// 分栏树节点数（与 SplitLayout 的 m_nodes 对应：0=根 1=左栏 2=右区）
constexpr int kLayoutSplitNodes = 3;

struct LayoutState {
    // 面板显隐（id → 开）。顺序无关，加载时按 id 匹配。
    std::vector<std::pair<std::string, bool>> panels;

    std::string workspace = "level";

    // 每个分栏节点的比例与折叠态
    float splitRatio[kLayoutSplitNodes] = {0.0f, 0.0f, 0.0f};
    bool splitCollapsed[kLayoutSplitNodes] = {false, false, false};
    float splitSaved[kLayoutSplitNodes] = {0.30f, 0.50f, 0.60f};

    // 叶子 → 面板 的映射（E29-1）。slots[node][side]，side: 0=A 1=B。
    // **存面板 id 字符串，不存槽位下标** —— 与 inspTab 同一条理由：下标会
    // 随着"往表中间插一个面板"整体漂移，老配置读回来就落到别的面板上。
    // 空 = 用默认树（老文件没有这一段，读回来行为与改造前完全一致）。
    std::vector<std::string> slotGroups[kLayoutSplitNodes][2];
    int viewportLayout = 0;   // 0=single 1=2v 2=2h 3=quad
    int viewMode = 0;         // Renderer::ViewMode 的整数值
    std::string maximized;    // 被最大化的面板 id（"" = 无）

    // Inspector 当前选中的页签。**存的是 id 字符串**（"object" / "material" …），
    // 不是下标 —— 下标会随着"往中间插一个页签"整体漂移，那样老配置读回来
    // 会静默跳到另一个页签上。id 由 InspectorPanel::tabId / tabIndexById 提供，
    // 本文件因此**不需要**认识 InspectorPanel（两边只共享字符串）。
    std::string inspTab = "object";

    // File ▸ Open Recent（最新在前）。落在这里而不是场景文件里，因为它是
    // **编辑器状态**：换台机器打开同一个工程，看见的应该是各自的最近列表。
    std::vector<std::string> recent;

    // 文件里至少出现过一项有效内容（避免"空文件被当成一次合法加载"）
    bool valid = false;
};

// 是否处于自动化运行（读了就破坏确定性）
bool editorLayoutAutomationRun();

// 布局文件绝对/相对路径（可用 MYVK_EDITOR_LAYOUT 覆盖）
std::string editorLayoutPath();

// 读。**文件不存在 / 解析失败** → 返回 false 且完全不改动 out 之外的任何
// 东西（调用方据此保持"出厂默认"）。
//
// ⚠ 自动化运行（MYVK_FRAMES / MYVK_NO_WINDOW_SAVE）下**默认路径不读** ——
//   上一次手动会话留下的布局会把自动化跑出的画面改掉，像素级对照就废了。
//   但显式给了 MYVK_EDITOR_LAYOUT 时**照读**：那是脚本自己指定的文件，
//   不是残留状态；没有这条，"落盘往返"就只能靠人开窗口手验。
bool loadEditorLayout(LayoutState& out);

// 写。失败只打一条 WARN，不抛。
// ⚠ 自动化运行下一律拒写（免得把测试跑出来的布局存成"用户的"）。
bool saveEditorLayout(const LayoutState& s);

// ---- E17：偏好（Preferences）落盘 ----
// 键值对形式（键 = EditorApp 偏好表的稳定 id，如 "fontScale"），
// 落在 config/editor_prefs.json。自动化守卫与布局同一条规则：
// 默认路径在自动化运行下**不读不写**；显式 MYVK_EDITOR_PREFS_PATH
// 时读放行（那是脚本指定的文件，不是残留状态）、写仍禁止。
std::string editorPrefsPath();
bool saveEditorPrefs(const std::vector<std::pair<std::string, double>>& kv);
// 读回全部键值对；文件不存在 / 解析失败 / 自动化运行 → false。
bool loadEditorPrefs(std::vector<std::pair<std::string, double>>& out);

} // namespace editor
