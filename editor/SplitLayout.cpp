#include "SplitLayout.h"

#include <imgui.h>

namespace editor {

namespace {

// 主体区域面板索引（对应 LayoutRects 的字段，用下标访问更省事）
enum PanelIdx {
    kHierarchy = 0,
    kContent = 1,
    kViewport = 2,
    kInspector = 3,
};

// 分栏条尺寸与最小像素（侧栏 <0 表示可折叠到 0）
constexpr float kSplitterHit = 6.0f;     // 命中/拖拽热区宽度
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
constexpr float kLeftSplit = 0.50f;      // Hierarchy / Content 上下比例

} // namespace

void SplitLayout::buildDefault() {
    m_nodes.clear();

    // 树结构（下标）：
    //   0: Horizontal 切「左栏 | 右区」
    //        A = 叶子 Hierarchy？ 不 —— 左栏是上下两格，所以：
    //        A = 节点 1（Vertical：Hierarchy 上 / Content 下）
    //        B = 节点 2（Horizontal：Viewport 左 / Inspector 右）
    // 与 Blender 的默认 Screen 布局同构。

    SplitNode root;
    root.dir = SplitNode::Dir::Horizontal;
    root.ratio = 0.0f;    // 首帧按窗口尺寸初始化（见 beginFrame）
    root.minA = kMinPane;
    root.minB = 0.0f;
    root.a = 1;
    root.b = 2;

    SplitNode left;
    left.dir = SplitNode::Dir::Vertical;
    left.ratio = kLeftSplit;
    left.minA = kMinPane;
    left.minB = kMinPane;
    left.a = -1;  left.b = -1;
    left.panelA = kHierarchy;
    left.panelB = kContent;

    SplitNode right;
    right.dir = SplitNode::Dir::Horizontal;
    right.ratio = 0.0f;   // 首帧按窗口尺寸初始化
    right.minA = 0.0f;    // Viewport 可被压得很小（视口优先反着来：Inspector 先折叠）
    right.minB = kMinPane;
    right.a = -1;  right.b = -1;
    right.panelA = kViewport;
    right.panelB = kInspector;

    m_nodes.push_back(root);   // 0
    m_nodes.push_back(left);   // 1
    m_nodes.push_back(right);  // 2
    m_root = 0;

    m_initialized = false;
    m_userAdjusted = false;
    m_lastInitW = 0.0f;
}

void SplitLayout::beginFrame(float w, float h) {
    m_dispW = w;
    m_dispH = h;

    if (m_root < 0 || w <= 0.0f) return;

    // 什么时候重算初始比例：
    //   · 首帧
    //   · 用户**没有**手动拖过分隔条，而窗口宽度变化很大
    //     （典型场景：先按小窗启动、再最大化 —— 不然侧栏会一直停在小窗
    //      时的比例上，看起来就是"挤在左边两条"）
    const bool firstFrame = !m_initialized;
    bool shouldInit = firstFrame;
    if (!firstFrame && !m_userAdjusted && m_lastInitW > 0.0f) {
        if (w > m_lastInitW * 1.25f || w < m_lastInitW * 0.8f) shouldInit = true;
    }
    if (!shouldInit) return;

    const float bodyW = std::max(w, kMinPane * 3.0f);
    const float leftW = std::clamp(bodyW * kLeftFrac, kLeftMin, kLeftMax);
    const float rightW = std::clamp(bodyW * kRightFrac, kRightMin, kRightMax);

    SplitNode& root = m_nodes[m_root];
    root.ratio = std::clamp(leftW / bodyW, 0.05f, 0.45f);

    SplitNode& right = m_nodes[2];
    const float rightBodyW = std::max(bodyW - leftW, kMinPane);
    right.ratio = std::clamp((bodyW - leftW - rightW) / rightBodyW, 0.10f, 1.0f);

    m_initialized = true;
    m_lastInitW = w;
}

void SplitLayout::apply(LayoutRects& out) {
    if (m_root < 0) return;

    const float W = std::max(m_dispW, 320.0f);
    const float H = std::max(m_dispH, 240.0f);

    // 主体区域：菜单 + 工具栏在上，状态栏在下（这三条仍然是全宽固定条）
    const float bodyY = kMenuH + m_toolbarH;
    const float statusH = kStatusH;
    const float bodyH = std::max(H - bodyY - statusH, 120.0f);

    resolve(m_root, 0.0f, bodyY, W, bodyH, out);

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
    if (n.dir == SplitNode::Dir::Horizontal) {
        aW = w * n.ratio;
        bW = w - aW;
        bx = x + aW;
    } else {
        aH = h * n.ratio;
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
        case kHierarchy: out.hierarchy = {x, y, w, h}; break;
        case kContent:   out.content   = {x, y, w, h}; break;
        case kViewport:  out.viewport  = {x, y, w, h}; break;
        case kInspector: out.inspector = {x, y, w, h}; break;
        default: break;
    }
}

void SplitLayout::handleSplitters() {
    if (m_root < 0) return;
    const float W = std::max(m_dispW, 320.0f);
    const float H = std::max(m_dispH, 240.0f);
    const float bodyY = kMenuH + m_toolbarH;
    const float bodyH = std::max(H - bodyY - kStatusH, 120.0f);
    processSplitters(m_root, 0.0f, bodyY, W, bodyH);
}

void SplitLayout::processSplitters(int nodeIdx, float x, float y, float w,
                                   float h) {
    SplitNode& n = m_nodes[nodeIdx];

    float aW = w, aH = h, bW = w, bH = h;
    float ax = x, ay = y, bx = x, by = y;
    if (n.dir == SplitNode::Dir::Horizontal) {
        aW = w * n.ratio;
        bW = w - aW;
        bx = x + aW;
    } else {
        aH = h * n.ratio;
        bH = h - aH;
        by = y + aH;
    }

    // 先递归子树
    if (n.a >= 0) processSplitters(n.a, ax, ay, aW, aH);
    if (n.b >= 0) processSplitters(n.b, bx, by, bW, bH);

    // 本节点分隔条的位置（与 resolve 用同一公式，保证线与分栏对齐）
    ImVec2 segA, segB;
    if (n.dir == SplitNode::Dir::Horizontal) {
        segA = ImVec2(bx, y);
        segB = ImVec2(bx, y + h);
    } else {
        segA = ImVec2(x, by);
        segB = ImVec2(x + w, by);
    }

    // ---- 绘制：画在最顶层，确保不被面板盖住 ----
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddLine(segA, segB, IM_COL32(70, 75, 82, 255), 1.0f);

    // ---- 命中 / 拖拽：手动判断鼠标是否落在热区内 ----
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool horizontal = (n.dir == SplitNode::Dir::Horizontal);
    const float dist = horizontal ? std::abs(mouse.x - segA.x)
                                  : std::abs(mouse.y - segA.y);
    const float inRange =
        horizontal ? (mouse.y >= y && mouse.y <= y + h)
                   : (mouse.x >= x && mouse.x <= x + w);
    const bool hovered = (dist <= kSplitterHit * 0.5f) && inRange &&
                         !ImGui::IsAnyItemHovered();

    // 光标
    if (hovered || m_dragNode == nodeIdx) {
        ImGui::SetMouseCursor(horizontal ? ImGuiMouseCursor_ResizeEW
                                         : ImGuiMouseCursor_ResizeNS);
    }

    // 拖拽：左键按下且在热区内 → 开始；松开 → 结束
    const bool mouseDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    if (m_dragNode == -1 && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        m_dragNode = nodeIdx;
    }
    if (m_dragNode == nodeIdx) {
        if (mouseDown) {
            const ImVec2 d = ImGui::GetIO().MouseDelta;
            m_userAdjusted = true;   // 从此不再自动改比例
            if (horizontal) {
                const float aW = std::clamp(w * n.ratio + d.x,
                                            (n.minA >= 0.0f ? n.minA : 0.0f),
                                            w - (n.minB >= 0.0f ? n.minB : 0.0f));
                n.ratio = aW / std::max(w, 1.0f);
            } else {
                const float aH = std::clamp(h * n.ratio + d.y,
                                            (n.minA >= 0.0f ? n.minA : 0.0f),
                                            h - (n.minB >= 0.0f ? n.minB : 0.0f));
                n.ratio = aH / std::max(h, 1.0f);
            }
        } else {
            m_dragNode = -1;
        }
    }

    // 双击分隔条：折叠/展开 A 区
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        n.collapsed = !n.collapsed;
        if (n.collapsed) {
            n.savedRatio = n.ratio;
            n.ratio = 0.0f;
        } else {
            n.ratio = (n.savedRatio > 0.0f) ? n.savedRatio : 0.3f;
        }
    }

    // 折叠态：画一个细条提示"此处可展开"
    if (n.collapsed) {
        dl->AddCircleFilled(ImVec2((segA.x + segB.x) * 0.5f,
                                   (segA.y + segB.y) * 0.5f),
                            3.0f, IM_COL32(120, 130, 140, 255));
    }
}

} // namespace editor
