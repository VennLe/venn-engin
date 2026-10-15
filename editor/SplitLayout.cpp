#include "SplitLayout.h"

#include "DebugRects.h"

#include <imgui.h>

#include <cstdio>

namespace editor {

namespace {

// 分栏条尺寸与最小像素（侧栏 <0 表示可折叠到 0）
constexpr float kSplitterHit = 7.0f;     // 命中/拖拽热区宽度（E06-1：从 6 抬到 7）
constexpr float kMinPane = 40.0f;        // 拖动时允许的最小可见尺寸

// 顶部菜单栏高度、底部状态栏高度（工具栏高度改为成员 m_toolbarH，可设置）
constexpr float kMenuH = 22.0f;
constexpr float kStatusH = 26.0f;

// 侧栏宽度：**按窗口宽度自适应**，而不是写死像素。
// 写死的问题在宽屏（比如最大化到 3840）上特别明显：320px 的侧栏只占 8%，
// 视口把整块地方全吞了，两侧挤成两条缝 —— 就是"比例不大方"的根源。
// 这里取"按比例 + 夹在合理区间"：窄窗口退化成固定宽度，宽窗口跟着放大，
// 两种情况下侧栏都读得清、视口也还是主体。
constexpr float kLeftFrac = 0.175f;
constexpr float kLeftMin = 300.0f;
constexpr float kLeftMax = 520.0f;
constexpr float kRightFrac = 0.205f;
constexpr float kRightMin = 330.0f;
constexpr float kRightMax = 560.0f;
constexpr float kLeftSplit = 0.50f;

// 分隔线配色（E06-1：三态）
constexpr ImU32 kSplitIdle = IM_COL32(70, 75, 82, 255);
constexpr ImU32 kSplitHover = IM_COL32(110, 150, 205, 255);
constexpr ImU32 kSplitDrag = IM_COL32(140, 190, 250, 255);

// 面板名（tooltip 用）
const char* slotName(int slot) {
    switch (slot) {
        case kSlotHierarchy: return "Hierarchy";
        case kSlotContent:   return "Content";
        case kSlotViewport:  return "Viewport";
        case kSlotInspector: return "Inspector";
        default:             return "Panel";
    }
}

} // namespace

void SplitLayout::buildDefault() {
    m_nodes.clear();

    // 树结构（下标）：
    //   0: Horizontal 切「左栏 | 右区」
    //        A = 节点 1（Vertical：Hierarchy 上 / Content 下）
    //        B = 节点 2（Horizontal：Viewport 左 / Inspector 右）
    // 与 Blender 的默认 Screen 布局同构。

    SplitNode root;
    root.dir = SplitNode::Dir::Horizontal;
    root.ratio = 0.0f;    // 首帧按窗口尺寸初始化（见 beginFrame）
    root.autoRatio = true;
    root.minA = kMinPane;
    root.minB = 0.0f;
    root.a = 1;
    root.b = 2;

    SplitNode left;
    left.dir = SplitNode::Dir::Vertical;
    left.ratio = kLeftSplit;
    left.autoRatio = false;   // 上下比例是人为定的，不跟窗口宽度走
    left.minA = kMinPane;
    left.minB = kMinPane;
    left.a = -1;  left.b = -1;
    left.panelA = kSlotHierarchy;
    left.panelB = kSlotContent;

    SplitNode right;
    right.dir = SplitNode::Dir::Horizontal;
    right.ratio = 0.0f;   // 首帧按窗口尺寸初始化
    right.autoRatio = true;
    right.minA = 0.0f;    // Viewport 可被压得很小（视口优先反着来：Inspector 先折叠）
    right.minB = kMinPane;
    right.a = -1;  right.b = -1;
    right.panelA = kSlotViewport;
    right.panelB = kSlotInspector;

    m_nodes.push_back(root);   // 0
    m_nodes.push_back(left);   // 1
    m_nodes.push_back(right);  // 2
    m_root = 0;

    m_initialized = false;
    m_lastInitW = 0.0f;
    m_dragNode = -1;
}

void SplitLayout::reset() {
    buildDefault();
    m_maximized = kSlotNone;
}

void SplitLayout::beginFrame(float w, float h) {
    m_dispW = w;
    m_dispH = h;

    if (m_root < 0 || w <= 0.0f) return;

    // 什么时候重算初始比例：
    //   · 首帧
    //   · 窗口宽度变化很大（典型场景：先按小窗启动、再最大化 —— 不然侧栏
    //     会一直停在小窗时的比例上，看起来就是"挤在左边两条"）
    // 而且只重算 `autoRatio == true` 的节点：用户拖过、或工作区预设指定过
    // 数字的节点，其比例是"人定的"，不该被窗口尺寸悄悄改掉。
    const bool firstFrame = !m_initialized;
    bool shouldInit = firstFrame;
    if (!firstFrame && m_lastInitW > 0.0f) {
        if (w > m_lastInitW * 1.25f || w < m_lastInitW * 0.8f) shouldInit = true;
    }
    if (!shouldInit) return;

    const float bodyW = std::max(w, kMinPane * 3.0f);

    SplitNode& root = m_nodes[m_root];
    const float leftW = root.autoRatio
                            ? std::clamp(bodyW * kLeftFrac, kLeftMin, kLeftMax)
                            : bodyW * root.ratio;
    if (root.autoRatio) root.ratio = std::clamp(leftW / bodyW, 0.05f, 0.45f);

    SplitNode& right = m_nodes[2];
    const float rightBodyW = std::max(bodyW - leftW, kMinPane);
    const float rightW = right.autoRatio
                             ? std::clamp(bodyW * kRightFrac, kRightMin, kRightMax)
                             : rightBodyW * right.ratio;
    if (right.autoRatio) {
        right.ratio = std::clamp((bodyW - leftW - rightW) / rightBodyW, 0.10f, 1.0f);
    }

    m_initialized = true;
    m_lastInitW = w;
}

void SplitLayout::syncPanelVisibility() {
    if (m_root < 0) return;

    // 面板被关掉 → 那一侧收成 0，邻居吃掉空间（E06-5）。
    // 只看四个**主体**面板；浮动的 Stats/Scripts 不进分栏树，天然无关。
    auto live = [](int slot) {
        if (slot == kSlotNone) return false;
        switch (slot) {
            case kSlotHierarchy: return panelOpen(panelIndexOf("hierarchy"));
            case kSlotContent:   return panelOpen(panelIndexOf("content"));
            case kSlotViewport:  return panelOpen(panelIndexOf("viewport"));
            case kSlotInspector: return panelOpen(panelIndexOf("inspector"));
            default:             return true;
        }
    };

    for (SplitNode& n : m_nodes) {
        if (n.a >= 0) {
            n.liveA = true;   // 子树：它自己的活性由更下层决定
        } else {
            n.liveA = live(n.panelA);
        }
        if (n.b >= 0) {
            n.liveB = true;
        } else {
            n.liveB = live(n.panelB);
        }
    }

    // 子树里还有没有活着的面板？自底向上推一遍。
    // ⚠ 递归 lambda 不能按名字自引用（名字在 auto 推导完成前还不存在），
    //   所以把"自己"作为第一个参数传进去 —— 这是标准做法，不需要
    //   std::function（省一个 include，也省一层类型擦除）。
    auto subtreeLive = [&](auto&& self, int nodeIdx) -> bool {
        const SplitNode& n = m_nodes[static_cast<size_t>(nodeIdx)];
        const bool aOk = (n.a >= 0) ? self(self, n.a) : n.liveA;
        const bool bOk = (n.b >= 0) ? self(self, n.b) : n.liveB;
        return aOk || bOk;
    };
    if (m_nodes[0].a >= 0)
        m_nodes[0].liveA = subtreeLive(subtreeLive, m_nodes[0].a);
    if (m_nodes[0].b >= 0)
        m_nodes[0].liveB = subtreeLive(subtreeLive, m_nodes[0].b);
}

bool SplitLayout::showsInLayout(int slot) const {
    if (slot == kSlotNone) return false;
    if (m_maximized != kSlotNone) return slot == m_maximized;
    return true;
}

void SplitLayout::bodyRect(float displayW, float displayH, float& x, float& y,
                           float& w, float& h) const {
    const float W = std::max(displayW, 320.0f);
    const float H = std::max(displayH, 240.0f);
    x = 0.0f;
    y = kMenuH + m_toolbarH;
    w = W;
    h = std::max(H - y - kStatusH, 120.0f);
}

void SplitLayout::apply(LayoutRects& out) {
    if (m_root < 0) return;

    const float W = std::max(m_dispW, 320.0f);
    const float H = std::max(m_dispH, 240.0f);
    const float bodyY = kMenuH + m_toolbarH;
    const float statusH = kStatusH;
    const float bodyH = std::max(H - bodyY - statusH, 120.0f);

    if (m_maximized != kSlotNone) {
        // 最大化：目标面板占满主体，其余三个收成 0。
        // EditorApp 用 showsInLayout() 跳过它们 —— 收成 0 而不是"不写"，
        // 是为了让"没被最大化的面板拿到 0 矩形"这件事是**显式**的。
        out.hierarchy = {0.0f, 0.0f, 0.0f, 0.0f};
        out.content = {0.0f, 0.0f, 0.0f, 0.0f};
        out.viewport = {0.0f, 0.0f, 0.0f, 0.0f};
        out.inspector = {0.0f, 0.0f, 0.0f, 0.0f};
        writePanel(m_maximized, 0.0f, bodyY, W, bodyH, out);
    } else {
        resolve(m_root, 0.0f, bodyY, W, bodyH, out);
    }

    // 三条固定条照旧由 EditorApp 填，这里补齐（保持接口完整）
    out.menu = {0.0f, 0.0f, W, kMenuH};
    out.toolbar = {0.0f, kMenuH, W, m_toolbarH};
    out.status = {0.0f, H - statusH, W, statusH};
}

void SplitLayout::resolve(int nodeIdx, float x, float y, float w, float h,
                          LayoutRects& out) {
    const SplitNode& n = m_nodes[nodeIdx];

    float aW = w, aH = h, bW = w, bH = h;
    float ax = x, ay = y, bx = x, by = y;

    // 面板被关掉时那一侧收成 0（E06-5）
    float eff = n.ratio;
    if (!n.liveA) eff = 0.0f;
    else if (!n.liveB) eff = 1.0f;

    if (n.dir == SplitNode::Dir::Horizontal) {
        aW = w * eff;
        bW = w - aW;
        bx = x + aW;
    } else {
        aH = h * eff;
        bH = h - aH;
        by = y + aH;
    }

    // A 半边：子节点或叶子面板
    if (n.a >= 0) resolve(n.a, ax, ay, aW, aH, out);
    else writePanel(n.panelA, ax, ay, aW, aH, out);
    // B 半边
    if (n.b >= 0) resolve(n.b, bx, by, bW, bH, out);
    else writePanel(n.panelB, bx, by, bW, bH, out);
}

void SplitLayout::writePanel(int panel, float x, float y, float w, float h,
                             LayoutRects& out) {
    switch (panel) {
        case kSlotHierarchy: out.hierarchy = {x, y, w, h}; break;
        case kSlotContent:   out.content   = {x, y, w, h}; break;
        case kSlotViewport:  out.viewport  = {x, y, w, h}; break;
        case kSlotInspector: out.inspector = {x, y, w, h}; break;
        default: break;
    }
}

void SplitLayout::handleSplitters() {
    m_splitHoverTaken = false;   // 每帧清一次：见 processSplitters 里的说明
    if (m_root < 0) return;
    if (m_maximized != kSlotNone) return;  // 最大化时不画分隔条

    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
    bodyRect(m_dispW, m_dispH, x, y, w, h);
    processSplitters(m_root, x, y, w, h);
}

void SplitLayout::processSplitters(int nodeIdx, float x, float y, float w,
                                   float h) {
    SplitNode& n = m_nodes[nodeIdx];

    float eff = n.ratio;
    if (!n.liveA) eff = 0.0f;
    else if (!n.liveB) eff = 1.0f;

    float aW = w, aH = h, bW = w, bH = h;
    float ax = x, ay = y, bx = x, by = y;
    if (n.dir == SplitNode::Dir::Horizontal) {
        aW = w * eff;
        bW = w - aW;
        bx = x + aW;
    } else {
        aH = h * eff;
        bH = h - aH;
        by = y + aH;
    }

    // 先递归子树
    if (n.a >= 0) processSplitters(n.a, ax, ay, aW, aH);
    if (n.b >= 0) processSplitters(n.b, bx, by, bW, bH);

    // 有一侧没有活着的面板 → 这条分隔条没有意义，不画也不交互
    if (!n.liveA || !n.liveB) return;

    // 本节点分隔条的位置（与 resolve 用同一公式，保证线与分栏对齐）
    ImVec2 segA, segB;
    if (n.dir == SplitNode::Dir::Horizontal) {
        segA = ImVec2(bx, y);
        segB = ImVec2(bx, y + h);
    } else {
        segA = ImVec2(x, by);
        segB = ImVec2(x + w, by);
    }

    // ---- 命中 / 拖拽：手动判断鼠标是否落在热区内 ----
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool horizontal = (n.dir == SplitNode::Dir::Horizontal);
    const float dist = horizontal ? std::abs(mouse.x - segA.x)
                                  : std::abs(mouse.y - segA.y);
    const float inRange =
        horizontal ? (mouse.y >= y && mouse.y <= y + h)
                   : (mouse.x >= x && mouse.x <= x + w);
    const bool hovered = (dist <= kSplitterHit * 0.5f) && inRange &&
                         !ImGui::IsAnyItemHovered() && !m_splitHoverTaken;
    // 同一像素上可能**叠着两条**分隔条：某个节点被完全折到 0 / 1 时，它的
    // 位置正好落在父节点那条线上（比如把 Inspector 侧折光，右区的分隔条就
    // 压到了"左栏 | 右区"那条线上）。
    // 让先处理的（子树先于父节点）独吞这次悬停 —— 否则一次双击/右键会被
    // 两条线同时吃到，等于"折一条 + 展一条"一起发生，看起来像 UI 在乱跳。
    // 代价：被压住的那条在这一点上不可交互 —— 但折着的那条双击一下就能
    // 展开，展开后两条自然分开，不会卡住。
    if (hovered) m_splitHoverTaken = true;

    const bool dragging = (m_dragNode == nodeIdx);

    // ---- 绘制：画在最顶层，确保不被面板盖住（E06-1 三态配色）----
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImU32 col = dragging ? kSplitDrag : (hovered ? kSplitHover : kSplitIdle);
    const float thick = (dragging || hovered) ? 2.0f : 1.0f;
    dl->AddLine(segA, segB, col, thick);

    // 悬停时在两端各补一小段"抓手"，让"这里能拖"一眼可见
    if (hovered || dragging) {
        const float grip = 14.0f;
        const ImVec2 mid((segA.x + segB.x) * 0.5f, (segA.y + segB.y) * 0.5f);
        if (horizontal) {
            dl->AddLine(ImVec2(segA.x, mid.y - grip * 0.5f),
                        ImVec2(segA.x, mid.y + grip * 0.5f), col, 3.0f);
        } else {
            dl->AddLine(ImVec2(mid.x - grip * 0.5f, segA.y),
                        ImVec2(mid.x + grip * 0.5f, segA.y), col, 3.0f);
        }
    }

    // 自动化用：分隔条的屏幕矩形（脚本靠它定位"拖这条线"）
    {
        char tag[32];
        std::snprintf(tag, sizeof(tag), "SPLIT-%d", nodeIdx);
        const ImVec2 pad = horizontal ? ImVec2(2.0f, 0.0f) : ImVec2(0.0f, 2.0f);
        logRect(tag, ImVec2(segA.x - pad.x, segA.y - pad.y),
                ImVec2(segB.x + pad.x, segB.y + pad.y));
    }

    // 光标
    if (hovered || dragging) {
        ImGui::SetMouseCursor(horizontal ? ImGuiMouseCursor_ResizeEW
                                         : ImGuiMouseCursor_ResizeNS);
    }

    // ---- 拖拽：左键按下且在热区内 → 开始；松开 → 结束 ----
    const bool mouseDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    if (m_dragNode == -1 && hovered &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        m_dragNode = nodeIdx;
    }
    if (m_dragNode == nodeIdx) {
        if (mouseDown) {
            const ImVec2 d = ImGui::GetIO().MouseDelta;
            n.autoRatio = false;   // 手动拖过 → 从此不再自动改这一条的比例
            if (horizontal) {
                const float aWn = std::clamp(w * n.ratio + d.x,
                                             (n.minA >= 0.0f ? n.minA : 0.0f),
                                             w - (n.minB >= 0.0f ? n.minB : 0.0f));
                n.ratio = aWn / std::max(w, 1.0f);
            } else {
                const float aHn = std::clamp(h * n.ratio + d.y,
                                             (n.minA >= 0.0f ? n.minA : 0.0f),
                                             h - (n.minB >= 0.0f ? n.minB : 0.0f));
                n.ratio = aHn / std::max(h, 1.0f);
            }
            n.collapsed = false;
        } else {
            m_dragNode = -1;
            m_hoverFrac = 0.0f;
        }
    }

    // ---- 拖动中跟一条比例 tooltip（E06-1）----
    if (dragging && mouseDown) {
        m_hoverFrac = n.ratio;
        ImGui::SetTooltip("%s %.0f%%  |  %s %.0f%%", slotName(n.panelA),
                          n.ratio * 100.0f, slotName(n.panelB),
                          (1.0f - n.ratio) * 100.0f);
    }

    // ---- 双击分隔条：折叠/展开（E06-2）----
    // 折叠哪一侧：折**靠近鼠标那一侧**（原来固定折 A 区，B 区永远是"被吃掉
    // 空间"的那一方，想折 Inspector 就没辙了）。
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
        m_dragNode == -1) {
        const bool nearA = horizontal ? (mouse.x < segA.x) : (mouse.y < segA.y);
        if (n.collapsed) {
            n.collapsed = false;
            n.ratio = (n.savedRatio > 0.0f) ? n.savedRatio : 0.3f;
        } else {
            n.savedRatio = n.ratio;
            n.collapsed = true;
            n.ratio = nearA ? 0.0f : 1.0f;
        }
        ImGui::SetTooltip(n.collapsed ? "collapsed (double-click to expand)"
                                      : "expanded");
    }

    // 折叠态：在分隔线上画一个圆点提示"此处可展开"
    if (n.collapsed) {
        dl->AddCircleFilled(ImVec2((segA.x + segB.x) * 0.5f,
                                   (segA.y + segB.y) * 0.5f),
                            3.5f, kSplitHover);
    }

    // ---- 右键：比例预设菜单（E06-4）----
    char popupId[32];
    std::snprintf(popupId, sizeof(popupId), "##split_preset_%d", nodeIdx);
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        ImGui::OpenPopup(popupId);
    }
    if (ImGui::BeginPopup(popupId)) {
        ImGui::SeparatorText(horizontal ? "Horizontal split" : "Vertical split");
        if (ImGui::MenuItem("50 / 50")) applyPreset(nodeIdx, 0.5f);
        if (ImGui::MenuItem("70 / 30")) applyPreset(nodeIdx, 0.7f);
        if (ImGui::MenuItem("30 / 70")) applyPreset(nodeIdx, 0.3f);
        ImGui::Separator();
        if (ImGui::MenuItem(n.collapsed ? "Expand" : "Collapse")) {
            if (n.collapsed) {
                n.collapsed = false;
                n.ratio = (n.savedRatio > 0.0f) ? n.savedRatio : 0.3f;
            } else {
                n.savedRatio = n.ratio;
                n.collapsed = true;
                n.ratio = 0.0f;
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Maximize side A")) {
            setMaximized(n.panelA);
        }
        if (ImGui::MenuItem("Maximize side B")) {
            setMaximized(n.panelB);
        }
        ImGui::EndPopup();
    }
}

// ---------------------------------------------------------------- 节点参数

float SplitLayout::ratio(int node) const {
    if (node < 0 || node >= static_cast<int>(m_nodes.size())) return 0.0f;
    return m_nodes[static_cast<size_t>(node)].ratio;
}

void SplitLayout::setRatio(int node, float r) {
    if (node < 0 || node >= static_cast<int>(m_nodes.size())) return;
    if (!(r >= 0.0f && r <= 1.0f)) return;   // 拒绝 NaN / 越界
    SplitNode& n = m_nodes[static_cast<size_t>(node)];
    n.ratio = r;
    n.collapsed = false;
    // 人定的比例 → 从此不再被窗口尺寸变化自动覆盖
    n.autoRatio = false;
}

void SplitLayout::setAutoRatio(int node, bool autoRatio) {
    if (node < 0 || node >= static_cast<int>(m_nodes.size())) return;
    m_nodes[static_cast<size_t>(node)].autoRatio = autoRatio;
}

bool SplitLayout::autoRatio(int node) const {
    if (node < 0 || node >= static_cast<int>(m_nodes.size())) return false;
    return m_nodes[static_cast<size_t>(node)].autoRatio;
}

bool SplitLayout::collapsed(int node) const {
    if (node < 0 || node >= static_cast<int>(m_nodes.size())) return false;
    return m_nodes[static_cast<size_t>(node)].collapsed;
}

void SplitLayout::setCollapsed(int node, bool c) {
    if (node < 0 || node >= static_cast<int>(m_nodes.size())) return;
    m_nodes[static_cast<size_t>(node)].collapsed = c;
}

float SplitLayout::savedRatio(int node) const {
    if (node < 0 || node >= static_cast<int>(m_nodes.size())) return 0.0f;
    return m_nodes[static_cast<size_t>(node)].savedRatio;
}

void SplitLayout::setSavedRatio(int node, float r) {
    if (node < 0 || node >= static_cast<int>(m_nodes.size())) return;
    if (!(r > 0.0f && r <= 1.0f)) return;
    m_nodes[static_cast<size_t>(node)].savedRatio = r;
}

// ---------------------------------------------------------------- 最大化

void SplitLayout::setMaximized(int slot) {
    if (slot != kSlotNone && slot != kSlotHierarchy && slot != kSlotContent &&
        slot != kSlotViewport && slot != kSlotInspector) {
        slot = kSlotNone;
    }
    m_maximized = slot;
}

void SplitLayout::toggleMaximize(int slot) {
    if (slot == kSlotNone) {
        m_maximized = kSlotNone;
        return;
    }
    m_maximized = (m_maximized == slot) ? kSlotNone : slot;
}

// ---------------------------------------------------------------- 预设

void SplitLayout::applyPreset(int node, float aFrac) {
    if (node < 0 || node >= static_cast<int>(m_nodes.size())) return;
    if (!(aFrac > 0.0f && aFrac < 1.0f)) return;

    SplitNode& n = m_nodes[static_cast<size_t>(node)];

    // 按当前实际像素尺寸反推：预设是"视觉上 70/30"，不是"比例 0.7 然后
    // 被 min 约束推到别的地方"。先算像素、夹进 min，再折回比例。
    const float W = std::max(m_dispW, 320.0f);
    const float H = std::max(m_dispH, 240.0f);
    const float span = (n.dir == SplitNode::Dir::Horizontal) ? W : (H - kMenuH - m_toolbarH - kStatusH);
    const float usable = std::max(span, 1.0f);
    const float px = usable * aFrac;
    const float lo = (n.minA >= 0.0f ? n.minA : 0.0f);
    const float hi = usable - (n.minB >= 0.0f ? n.minB : 0.0f);
    const float clamped = std::clamp(px, std::min(lo, hi), std::max(lo, hi));

    n.ratio = clamped / usable;
    n.collapsed = false;
    n.savedRatio = n.ratio;
    n.autoRatio = false;

    // 刻意**不做**级联重算：树只有两层，"改根比例"与"改子比例"在用户视角
    // 下是两件独立的事，级联会让"我明明点的是 70/30"变成别的数字。
}

} // namespace editor
