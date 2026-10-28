#include "SplitLayout.h"

#include "Localization.h"
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
// 右栏里 Hierarchy / Inspector 的上下比例（Hierarchy 占 42%）。
constexpr float kRightSplit = 0.42f;
// 左栏（模式栏）的宽度下限。
//
// 320 是"9 行列表刚好放得下"的宽度，但左栏下半现在已经不是列表、而是**各
// 模式自己的参数面板**，而它们原来是按 420~480 宽的浮动窗口设计的：建模面板
// 那排 `Subdivide selected(150) + Flip selected(140) + Delete(110)` 是
// SameLine 硬排的，横向合计 400 出头，加上窗口内边距 446 —— 320 宽时右边
// 那个 Delete 直接被窗口裁掉（点都点不到），400 宽也还差一点。
//
// ⚠ 2026-10-28：这个常量现在只当**首帧 / 窗口大幅缩放时的默认宽度下限**。
//   真正参与拖动的是 `root.minA`，它由 `setLeftMinWidth()` 按当前编辑模式
//   写进来（响应式的 Landscape / Foliage 报 300，硬排的 Modeling 报 460）。
//   两者分开，是为了让"默认布局逐像素不动"（verify_viewport 里钉了
//   CONVERGED_VP = (463,139,1241,778)，改默认宽度会让它和像素基线一起飘），
//   同时又能让用户往下拖。
constexpr float kMinModeRail = 460.0f;
// 拖动时**绝对**下限：再窄的话，左栏连"9 行列表 + 一列按钮"都放不下，
// 而且模式列表的图标 + 文字（x+50 起）会被裁。
constexpr float kMinModeRailDrag = 300.0f;

// 分隔线配色（E06-1：三态）
constexpr ImU32 kSplitIdle = IM_COL32(70, 75, 82, 255);
constexpr ImU32 kSplitHover = IM_COL32(110, 150, 205, 255);
constexpr ImU32 kSplitDrag = IM_COL32(140, 190, 250, 255);

// 角落抓手（E29-5）。命中半径 12px 是个权衡：
//   · 太小（6~8）→ 用户得怼准一个 4px 的交点，实际点不中；
//   · 太大（16+）→ 把分隔条两端各吃掉一大截，"在边上调大小"变得难。
// 12px 与 Windows 自己的窗口角缩放手感接近。
constexpr float kCornerHit = 12.0f;
// 静息态：只画一个很淡的加号（不画底块）——"这里有个东西"但不会看成按钮。
constexpr ImU32 kCornerIdle = IM_COL32(96, 104, 114, 170);
// 命中态：加号变强调色 + 描一个细框。
constexpr ImU32 kCornerHotFg = IM_COL32(150, 200, 255, 255);
constexpr ImU32 kCornerHotBg = IM_COL32(40, 52, 68, 210);

// 面板名（tooltip 用）
const char* slotName(int slot) {
    switch (slot) {
        case kSlotHierarchy: return "Hierarchy";
        case kSlotContent:   return "Content";
        case kSlotViewport:  return "Viewport";
        case kSlotInspector: return "Inspector";
        case kSlotModes:     return "Modes";
        default:             return "Panel";
    }
}

} // namespace

void SplitLayout::buildDefault() {
    m_nodes.clear();

    // 树结构（下标）—— 2026-10-21 按用户要求重排：
    //   0: Horizontal 切「左栏(模式) | 右区」
    //        A = 叶子 kSlotModes（模式列表 + 当前模式参数）
    //        B = 节点 1
    //   1: Horizontal 切「视口 | 右栏」
    //        A = 叶子 kSlotViewport
    //        B = 节点 2
    //   2: Vertical 切「Hierarchy 上 / Inspector 下」
    //        A = 叶子 kSlotHierarchy
    //        B = 叶子 kSlotInspector
    //
    // 原来是「左栏(Hierarchy 上 / Content 下) | 视口 | Inspector」。用户原话：
    // 「原本的 Hierarchy 面板挪到编辑器窗口右面的 Inspector 面板的上方，
    //   即在原来 Inspector 面板呈上下结构。」—— Content 早在 2026-10-17
    // 就改成了底栏抽屉（不占分栏），于是左栏正好空出来给模式栏。
    //
    // 节点数仍是 3（kNodeCount），所以工作区预设 / 落盘的数组长度不变；
    // 变的是各节点的**语义** —— 见 EditorLayoutIO 里 kFormatVersion 的说明。

    SplitNode root;
    root.dir = SplitNode::Dir::Horizontal;
    root.ratio = 0.0f;    // 首帧按窗口尺寸初始化（见 beginFrame）
    root.autoRatio = true;
    // ⚠ `minA` 是**拖动下限**，由 setLeftMinWidth() 按当前编辑模式写；
    //   这里先给绝对下限（300），EditorApp 每帧会按模式把它顶到该有的值。
    root.minA = kMinModeRailDrag;
    root.minB = 0.0f;
    root.a = -1;
    root.b = 1;
    root.groupA.push_back(kSlotModes);

    SplitNode mid;
    mid.dir = SplitNode::Dir::Horizontal;
    mid.ratio = 0.0f;   // 首帧按窗口尺寸初始化
    mid.autoRatio = true;
    mid.minA = 0.0f;    // 视口可以被压得很小（视口优先反着来：右栏先折叠）
    mid.minB = 0.0f;
    mid.a = -1;
    mid.b = 2;
    mid.groupA.push_back(kSlotViewport);

    SplitNode right;
    right.dir = SplitNode::Dir::Vertical;
    right.ratio = kRightSplit;   // Hierarchy 占右栏的 42%
    right.autoRatio = false;     // 上下比例是人为定的，不跟窗口尺寸走
    right.minA = kMinPane;
    right.minB = kMinPane;
    right.a = -1;  right.b = -1;
    right.groupA.push_back(kSlotHierarchy);
    right.groupB.push_back(kSlotInspector);

    m_nodes.push_back(root);   // 0
    m_nodes.push_back(mid);    // 1
    m_nodes.push_back(right);  // 2
    m_root = 0;

    m_initialized = false;
    m_lastInitW = 0.0f;
    m_dragNode = -1;
}

void SplitLayout::reset() {
    buildDefault();
    m_maximized = kSlotNone;
    m_leftMin = 0.0f;
}

void SplitLayout::setLeftMinWidth(float px) {
    if (m_root < 0) return;
    const float want =
        std::clamp(px, kMinModeRailDrag, std::max(kLeftMax, kMinModeRailDrag));
    m_leftMin = want;

    SplitNode& root = m_nodes[static_cast<size_t>(m_root)];
    root.minA = want;

    // 上限**抬高**时（切到需要宽排的模式）把当前宽度顶上去。
    // ⚠ 必须主动顶：`ratio` 是用户/首帧定的，光改 minA 不会让已有宽度变大，
    //   于是"在 Landscape 里拖窄 → 切到 Modeling"会得到一条 300 宽、
    //   按钮被裁掉一半的左栏（而且裁掉的部分点都点不到）。
    const float bodyW = std::max(m_dispW, kMinPane * 3.0f);
    const float curW = bodyW * root.ratio;
    if (curW < want - 0.5f) {
        root.ratio = std::clamp(want / std::max(bodyW, 1.0f), 0.0f, 1.0f);
        root.autoRatio = false;
        root.collapsed = false;
    }
}

float SplitLayout::leftMinWidth() const {
    if (m_root < 0) return kMinModeRailDrag;
    return m_nodes[static_cast<size_t>(m_root)].minA;
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

    // 节点 0：模式栏 | 其余
    SplitNode& root = m_nodes[m_root];
    const float leftW = root.autoRatio
                            ? std::clamp(bodyW * kLeftFrac, kMinModeRail,
                                         std::max(kLeftMax, kMinModeRail))
                            : bodyW * root.ratio;
    if (root.autoRatio) root.ratio = std::clamp(leftW / bodyW, 0.05f, 0.45f);

    // 节点 1：视口 | 右栏。右栏宽按窗口宽度自适应（夹在 330..560）
    SplitNode& mid = m_nodes[1];
    const float restW = std::max(bodyW - leftW, kMinPane);
    const float rightW = mid.autoRatio
                             ? std::clamp(bodyW * kRightFrac, kRightMin, kRightMax)
                             : restW * mid.ratio;
    if (mid.autoRatio) {
        mid.ratio = std::clamp((restW - rightW) / restW, 0.10f, 1.0f);
    }

    // 节点 2（右栏上下）：kRightSplit 是人定的，autoRatio=false，这里不动它。
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
            // ⚠ 2026-10-17：Content 不再是常驻分栏面板，改成了"从底栏往上
            //   弹出的抽屉"（见 EditorApp::drawContentDrawer）。所以这里恒
            //   false —— 左栏下半收成 0、Hierarchy 吃满整条左栏。抽屉打开
            //   时的矩形由 EditorApp 直接写进 L.content，不经过分栏树。
            //   保留 kSlotContent 这个分支（而不是删掉 case）是为了让
            //   "content 确实还在分栏槽位体系里"这件事在类型上是显式的：
            //   一条 case 恒 false 比"少一个 case 掉进 default: return true"
            //   安全得多 —— 后者会让左栏下半留一块空。 
            case kSlotContent:   return false;
            case kSlotViewport:  return panelOpen(panelIndexOf("viewport"));
            case kSlotInspector: return panelOpen(panelIndexOf("inspector"));
            // 模式栏恒在：它是"当前在哪个模式 + 这个模式的参数"，关掉它等于
            // 让整个编辑模式失去一切可见入口（顶栏下拉还在，但那是第二入口）。
            case kSlotModes:     return true;
            default:             return true;
        }
    };

    for (SplitNode& n : m_nodes) {
        if (n.a >= 0) {
            n.liveA = true;   // 子树：它自己的活性由更下层决定
        } else {
            n.liveA = live(n.panelA());
        }
        if (n.b >= 0) {
            n.liveB = true;
        } else {
            n.liveB = live(n.panelB());
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
    h = std::max(H - y - kStatusH - m_bottomInset, 120.0f);
}

void SplitLayout::apply(LayoutRects& out) {
    if (m_root < 0) return;

    const float W = std::max(m_dispW, 320.0f);
    const float H = std::max(m_dispH, 240.0f);
    const float bodyY = kMenuH + m_toolbarH;
    const float statusH = kStatusH;
    const float bodyH =
        std::max(H - bodyY - statusH - m_bottomInset, 120.0f);

    if (m_maximized != kSlotNone) {
        // 最大化：目标面板占满主体，其余三个收成 0。
        // EditorApp 用 showsInLayout() 跳过它们 —— 收成 0 而不是"不写"，
        // 是为了让"没被最大化的面板拿到 0 矩形"这件事是**显式**的。
        out.hierarchy = {0.0f, 0.0f, 0.0f, 0.0f};
        out.content = {0.0f, 0.0f, 0.0f, 0.0f};
        out.viewport = {0.0f, 0.0f, 0.0f, 0.0f};
        out.inspector = {0.0f, 0.0f, 0.0f, 0.0f};
        out.modes = {0.0f, 0.0f, 0.0f, 0.0f};
        writePanel(m_maximized, 0.0f, bodyY, W, bodyH, out);
    } else {
        // E29-1：先把四个树内槽位清成 0。
        // ⚠ 必须清：面板被搬走 / 合并成标签组后，那个槽位在树里就不存在了，
        //   而 LayoutRects 是**逐帧复用**的 —— 不清就会留着上一帧的矩形，
        //   于是同一个面板在旧位置和新位置各画一遍（"面板被劈成两块"）。
        //   out.content 不清：它由 EditorApp 的底栏抽屉直接写，不经过分栏树。
        out.hierarchy = {0.0f, 0.0f, 0.0f, 0.0f};
        out.viewport = {0.0f, 0.0f, 0.0f, 0.0f};
        out.inspector = {0.0f, 0.0f, 0.0f, 0.0f};
        out.modes = {0.0f, 0.0f, 0.0f, 0.0f};
        resolve(m_root, 0.0f, bodyY, W, bodyH, out);
    }

    // 三条固定条照旧由 EditorApp 填，这里补齐（保持接口完整）
    out.menu = {0.0f, 0.0f, W, kMenuH};
    out.toolbar = {0.0f, kMenuH, W, m_toolbarH};
    out.status = {0.0f, H - statusH, W, statusH};
    // ⚠ out.bottom **不在这里写**：m_bottomInset 是"主体底部让出的总高度"
    //   （底栏 + 停靠的抽屉），而底栏自己的矩形只是其中最下面 32px。
    //   SplitLayout 不知道这个拆分比例 —— 由 EditorApp::computeLayout 写。
    //   写错过一次：直接拿 m_bottomInset 当底栏高度，底栏被撑成 352px 高，
    //   内容画在顶部 32px、下面 320px 全是空白。
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
    else writePanel(n.panelA(), ax, ay, aW, aH, out);
    // B 半边
    if (n.b >= 0) resolve(n.b, bx, by, bW, bH, out);
    else writePanel(n.panelB(), bx, by, bW, bH, out);
}

void SplitLayout::writePanel(int panel, float x, float y, float w, float h,
                             LayoutRects& out) {
    switch (panel) {
        case kSlotHierarchy: out.hierarchy = {x, y, w, h}; break;
        case kSlotContent:   out.content   = {x, y, w, h}; break;
        case kSlotViewport:  out.viewport  = {x, y, w, h}; break;
        case kSlotInspector: out.inspector = {x, y, w, h}; break;
        case kSlotModes:     out.modes     = {x, y, w, h}; break;
        default: break;
    }
}

void SplitLayout::handleSplitters() {
    m_splitHoverTaken = false;   // 每帧清一次：见 processSplitters 里的说明
    m_corners.clear();
    m_vLines.clear();
    m_hLines.clear();
    m_cornerHot = -1;
    if (m_root < 0) return;
    if (m_maximized != kSlotNone) return;  // 最大化时不画分隔条

    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
    bodyRect(m_dispW, m_dispH, x, y, w, h);

    // ------------------------------------------------------------
    // ① 先算交点（只算几何，不画）
    // ------------------------------------------------------------
    // 顺序很要紧：角落要在 processSplitters 之前决定"这次悬停归谁"，
    // 否则两条线都已经各自判过命中，再补一句"角上优先"就是事后打补丁
    // （而且 m_splitHoverTaken 已经被其中一条占掉了）。
    collectCornerLines(m_root, x, y, w, h);
    // 横线 × 竖线求交。交点必须**落在两条线的跨度内**才算数。
    // ⚠ 容差 1.5px：T 形交点处横线的端点在数学上正好等于竖线坐标，但
    //   比例是浮点算出来的（`bx = x + w * ratio`），窗口缩放非整数比例时
    //   会差个零点几像素 —— 不留容差就会"有的窗口尺寸下有角、有的没有"。
    constexpr float kTouchEps = 1.5f;
    for (const LineSeg& H : m_hLines) {
        for (const LineSeg& V : m_vLines) {
            if (V.c < H.t0 - kTouchEps || V.c > H.t1 + kTouchEps) continue;
            if (H.c < V.t0 - kTouchEps || H.c > V.t1 + kTouchEps) continue;
            // 被浮层（抽屉 / 菜单 / 浮动面板）盖住的交点既不画也不能点：
            // 分隔线本来就会在这些矩形处被裁开，角上不裁的话会出现一个
            // 浮在菜单之上的加号。
            bool covered = false;
            for (const OcclRect& o : m_occluders) {
                if (V.c > o.x0 && V.c < o.x1 && H.c > o.y0 && H.c < o.y1) {
                    covered = true;
                    break;
                }
            }
            if (covered) continue;
            SplitCorner sc;
            sc.x = V.c;
            sc.y = H.c;
            sc.hNode = H.node;
            sc.vNode = V.node;
            m_corners.push_back(sc);
        }
    }

    // ------------------------------------------------------------
    // ② 命中：取最近的那个交点在 kCornerHit 以内
    // ------------------------------------------------------------
    {
        const ImVec2 mp = ImGui::GetIO().MousePos;
        float best = kCornerHit;
        for (size_t i = 0; i < m_corners.size(); ++i) {
            const float dx = mp.x - m_corners[i].x;
            const float dy = mp.y - m_corners[i].y;
            const float d = std::sqrt(dx * dx + dy * dy);
            // `<=` 而不是 `<`：`best` 从 kCornerHit 起算，正好在半径上的
            // 点也该算命中（边界只有一种写法，免得"看起来在里面却点不到"）。
            if (d <= best) {
                best = d;
                m_cornerHot = static_cast<int>(i);
            }
        }
        // 浮层/弹窗正在接管鼠标时不算命中（与分隔条同一套判据）。
        if (m_cornerHot >= 0 && ImGui::IsAnyItemHovered()) m_cornerHot = -1;
    }
    // 命中角落 → 本帧所有分隔条都不响应悬停（它们只画线）。
    // 少了这一句，角和线会抢同一个像素：光标在双箭头和加号之间跳，
    // 拖拽时"到底在合并还是在改比例"取决于哪条线先被处理。
    if (m_cornerHot >= 0) m_splitHoverTaken = true;

    processSplitters(m_root, x, y, w, h);
    drawCornerGrips();   // 画在最后：压在分隔线之上
}

void SplitLayout::collectCornerLines(int nodeIdx, float x, float y, float w,
                                     float h) {
    SplitNode& n = m_nodes[static_cast<size_t>(nodeIdx)];

    // ⚠ 这里的 eff / aW / bx 必须与 resolve() 和 processSplitters() 逐字一致。
    //   三处各算一遍本来就是隐患，但把"切分"抽成公共函数要连 resolve 的
    //   递归签名一起动（它会写 LayoutRects），改动面比收益大。用注释钉住。
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

    if (n.a >= 0) collectCornerLines(n.a, ax, ay, aW, aH);
    if (n.b >= 0) collectCornerLines(n.b, bx, by, bW, bH);

    if (!n.liveA || !n.liveB) return;   // 收起的那一侧没有线（与 processSplitters 一致)

    if (n.dir == SplitNode::Dir::Horizontal) {
        if (w <= 1.0f) return;
        m_vLines.push_back({bx, y, y + h, nodeIdx});
    } else {
        if (h <= 1.0f) return;
        m_hLines.push_back({by, x, x + w, nodeIdx});
    }
}

void SplitLayout::drawCornerGrips() {
    if (m_corners.empty()) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();

    for (size_t i = 0; i < m_corners.size(); ++i) {
        const SplitCorner& c = m_corners[i];
        const bool hot = (static_cast<int>(i) == m_cornerHot);

        // 加号的两根短线：静息 3px、命中 4.5px。
        const float g = hot ? 4.5f : 3.0f;
        const ImU32 fg = hot ? kCornerHotFg : kCornerIdle;
        if (hot) {
            const float r = 7.0f;
            dl->AddRectFilled(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r),
                              kCornerHotBg, 3.0f);
            dl->AddRect(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), fg,
                        3.0f, 0, 1.0f);
        }
        dl->AddLine(ImVec2(c.x - g, c.y), ImVec2(c.x + g, c.y), fg, 1.6f);
        dl->AddLine(ImVec2(c.x, c.y - g), ImVec2(c.x, c.y + g), fg, 1.6f);

        // 自动化锚点：`SPLIT-CORNER-<i>=(x0,y0)-(x1,y1)`，中心就是交点。
        // 用**可见的加号尺寸**而不是 kCornerHit：脚本按锚点中心去按，落在
        // 半径内即可命中，但日志里读到的位置要与人眼看到的一致。
        char tag[40];
        std::snprintf(tag, sizeof(tag), "SPLIT-CORNER-%d", static_cast<int>(i));
        logRect(tag, ImVec2(c.x - 6.0f, c.y - 6.0f),
                ImVec2(c.x + 6.0f, c.y + 6.0f));
    }

    // 光标：四向箭头（Windows 的 IDC_SIZEALL，视觉上就是一个"加号/十字"）。
    // 用户原话要的就是"显示成一个加号"；自绘的加号 + 这个光标一起给，
    // 因为纯自绘的加号在鼠标遮住的地方会看不全。
    if (m_cornerHot >= 0) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
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

    // ------------------------------------------------------------
    // 遮挡矩形裁切（浮层 / 弹层 / 浮动面板，2026-10-18 泛化）
    // ------------------------------------------------------------
    // 这条线画在 ForegroundDrawList 上（压在所有面板之上），而浮在分栏之上
    // 的东西（Content 抽屉、`+` 勾选菜单、右键菜单、下拉、角色窗口、浮动
    // 面板、tooltip）都**不在分栏树里** —— 不裁的话那条线会直接穿过它们。
    //
    // 2026-10-17 只处理了抽屉一个矩形；用户随后报"`+` 菜单列表中间也穿插
    // 着分割线，其他所有面板窗口/子菜单都一样"，所以这里改成**通用算法**：
    // 把线段沿它的轴向参数化，减去所有遮挡矩形在该轴上的投影区间，
    // 剩下的就是可见段（0..N 段）。
    //
    //   横线（上下分栏）：固定 y，遮挡矩形只要**跨过这一行**就挖掉它的 x 区间
    //   竖线（左右分栏）：固定 x，遮挡矩形只要**跨过这一列**就挖掉它的 y 区间
    const bool lineHoriz = (n.dir == SplitNode::Dir::Vertical);
    float t0 = lineHoriz ? std::min(segA.x, segB.x) : std::min(segA.y, segB.y);
    float t1 = lineHoriz ? std::max(segA.x, segB.x) : std::max(segA.y, segB.y);
    const float lineC = lineHoriz ? segA.y : segA.x;   // 固定坐标

    // 收集要挖掉的区间
    //
    // ⚠⚠ 判据必须是**严格穿过**，不能是"碰到也算"（2026-10-28 修）：
    //   布局里的面板是**紧贴着**分隔条摆的 —— 左栏窗口的右边界正好落在
    //   分隔条那个 x 上（`x1 == lineC`）。用 `lineC < c0 || lineC > c1`
    //   来判断时，`lineC == c1` 不算越界 → 这条"只是贴边"的窗口被当成
    //   "盖住了整条线"，于是**整条分隔条被裁光**（spans 为空 → 不画、
    //   不悬停、不能拖）。用户看到的就是"往左拖视口扩不开、左边 Modes
    //   一动不动"；而日志里 `SPLIT-0` 锚点**还在**（首帧遮挡列表为空时
    //   打过一次），所以按锚点去拖也毫无反应。
    //   左右各留 0.5px 的余量：真正的浮层（抽屉、菜单、浮动面板）总是
    //   明显地横跨这条线，不会被这点余量误放行。
    std::vector<std::pair<float, float>> cut;
    cut.reserve(m_occluders.size());
    for (const OcclRect& o : m_occluders) {
        const float lo = lineHoriz ? o.x0 : o.y0;
        const float hi = lineHoriz ? o.x1 : o.y1;
        const float c0 = lineHoriz ? o.y0 : o.x0;
        const float c1 = lineHoriz ? o.y1 : o.x1;
        if (lineC <= c0 + 0.5f || lineC >= c1 - 0.5f) continue;   // 没穿过这条线
        const float a = std::max(lo, t0), b = std::min(hi, t1);
        if (b > a) cut.emplace_back(a, b);
    }
    // 排序 + 合并（多块遮挡可能重叠：抽屉和菜单会叠在一起）
    std::sort(cut.begin(), cut.end());
    std::vector<std::pair<float, float>> merged;
    for (const auto& iv : cut) {
        if (!merged.empty() && iv.first <= merged.back().second + 0.01f) {
            merged.back().second = std::max(merged.back().second, iv.second);
        } else {
            merged.push_back(iv);
        }
    }
    // 相减 → 可见段
    std::vector<std::pair<float, float>> spans;
    float cur = t0;
    for (const auto& iv : merged) {
        if (iv.first - cur > 0.5f) spans.emplace_back(cur, iv.first);
        cur = std::max(cur, iv.second);
    }
    if (t1 - cur > 0.5f) spans.emplace_back(cur, t1);

    auto spanToSeg = [&](const std::pair<float, float>& s, ImVec2& outA,
                         ImVec2& outB) {
        if (lineHoriz) {
            outA = ImVec2(s.first, segA.y);
            outB = ImVec2(s.second, segB.y);
        } else {
            outA = ImVec2(segA.x, s.first);
            outB = ImVec2(segB.x, s.second);
        }
    };

    if (spans.empty()) {
        // 整条都被盖住：不画、不记锚点、不参与命中。
        // ⚠ 若这一条正好在被拖着，得顺手结束拖拽 —— 否则 m_dragNode 卡在
        //   这里，鼠标松开也清不掉，之后再一点就会"线跟着鼠标跑"。
        if (m_dragNode == nodeIdx) {
            m_dragNode = -1;
            m_hoverFrac = 0.0f;
        }
        return;
    }
    // ⚠ 锚点 / 抓手 / tooltip 都记在**最长的那一段**上：拖拽脚本点的是锚点
    //   矩形的中心，被裁之后中心仍然落在"这条线真实可见的位置"上，不用改
    //   脚本。（原来是 pieces[0]，泛化后第一段可能是被菜单切出来的一小截，
    //   拿它当中心会点到菜单底下。）
    size_t mainIdx = 0;
    for (size_t i = 1; i < spans.size(); ++i) {
        if (spans[i].second - spans[i].first >
            spans[mainIdx].second - spans[mainIdx].first) {
            mainIdx = i;
        }
    }
    spanToSeg(spans[mainIdx], segA, segB);

    // ---- 命中 / 拖拽：手动判断鼠标是否落在**可见段**的热区内 ----
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool horizontal = (n.dir == SplitNode::Dir::Horizontal);
    const float lineCoord = horizontal ? bx : by;
    const float dist = horizontal ? std::abs(mouse.x - lineCoord)
                                  : std::abs(mouse.y - lineCoord);
    // ⚠ 范围判定从"整个 body 跨度"改成"鼠标落在某一段可见段里"：
    //   原来用 inRange = 整条跨度，抽屉盖上来的那截照样能拖分栏
    //   （线和抽屉的把手只差几像素时最容易误触）。
    bool inRange = false;
    for (const auto& s : spans) {
        if (horizontal) {
            if (mouse.y >= s.first && mouse.y <= s.second) { inRange = true; break; }
        } else {
            if (mouse.x >= s.first && mouse.x <= s.second) { inRange = true; break; }
        }
    }
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
    // 逐段画：被遮挡矩形裁开后可能不止两段（抽屉 + 菜单 + 浮动面板叠在一起）。
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImU32 col = dragging ? kSplitDrag : (hovered ? kSplitHover : kSplitIdle);
    const float thick = (dragging || hovered) ? 2.0f : 1.0f;
    ImVec2 pa, pb;
    for (const auto& s : spans) {
        spanToSeg(s, pa, pb);
        dl->AddLine(pa, pb, col, thick);
    }

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
        ImGui::SetTooltip(TR("%s %.0f%%  |  %s %.0f%%"), slotName(n.panelA()),
                          n.ratio * 100.0f, slotName(n.panelB()),
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
        ImGui::SetTooltip(n.collapsed ? TRL("collapsed (double-click to expand)")
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
        ImGui::SeparatorText(horizontal ? TRL("Horizontal split") : TRL("Vertical split"));
        if (ImGui::MenuItem(TR("50 / 50"))) applyPreset(nodeIdx, 0.5f);
        if (ImGui::MenuItem(TR("70 / 30"))) applyPreset(nodeIdx, 0.7f);
        if (ImGui::MenuItem(TR("30 / 70"))) applyPreset(nodeIdx, 0.3f);
        ImGui::Separator();
        if (ImGui::MenuItem(n.collapsed ? TRL("Expand") : TRL("Collapse"))) {
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
        if (ImGui::MenuItem(TR("Maximize side A"))) {
            setMaximized(n.panelA());
        }
        if (ImGui::MenuItem(TR("Maximize side B"))) {
            setMaximized(n.panelB());
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


// ============================================================
// 叶子面板组（E29-1：槽位 → 面板的映射从常量变成运行时可写）
// ============================================================
//
// 这一层只做"谁在哪个位置"，不碰绘制。E29-2 的拖拽落点、E29-3 的标签组
// 都调这里的 movePanel / swapActive / setGroup。
namespace {

// 能进分栏树的面板槽位。
// ⚠ kSlotContent **不在**其中：Content 早在 2026-10-17 就改成了底栏抽屉，
//   它恒不 live（见 syncPanelVisibility 的 case 注释）。放它进树会得到
//   "一个永远不显示的槽位"，而那个槽位还会把邻居挤窄 —— 所以直接拒绝。
bool isTreeSlot(int slot) {
    switch (slot) {
        case kSlotHierarchy:
        case kSlotViewport:
        case kSlotInspector:
        case kSlotModes:
            return true;
        default:
            return false;
    }
}

} // namespace

bool SplitLayout::isLeaf(int node, int side) const {
    if (node < 0 || node >= static_cast<int>(m_nodes.size())) return false;
    const SplitNode& n = m_nodes[static_cast<size_t>(node)];
    if (side == kSideA) return n.a < 0;
    if (side == kSideB) return n.b < 0;
    return false;
}

int SplitLayout::groupSize(int node, int side) const {
    if (!isLeaf(node, side)) return 0;
    const SplitNode& n = m_nodes[static_cast<size_t>(node)];
    return static_cast<int>((side == kSideA ? n.groupA : n.groupB).size());
}

int SplitLayout::groupPanel(int node, int side, int idx) const {
    if (!isLeaf(node, side)) return kSlotNone;
    const SplitNode& n = m_nodes[static_cast<size_t>(node)];
    const std::vector<int>& g = (side == kSideA) ? n.groupA : n.groupB;
    if (idx < 0 || idx >= static_cast<int>(g.size())) return kSlotNone;
    return g[static_cast<size_t>(idx)];
}

int SplitLayout::activeIndex(int node, int side) const {
    if (!isLeaf(node, side)) return -1;
    const SplitNode& n = m_nodes[static_cast<size_t>(node)];
    return (side == kSideA) ? n.activeA : n.activeB;
}

bool SplitLayout::setGroup(int node, int side, const std::vector<int>& panels,
                           int active) {
    if (!isLeaf(node, side)) return false;
    SplitNode& n = m_nodes[static_cast<size_t>(node)];
    std::vector<int>& g = (side == kSideA) ? n.groupA : n.groupB;

    g.clear();
    for (int p : panels) {
        if (!isTreeSlot(p)) continue;                       // 非法槽位：静默丢掉
        if (std::find(g.begin(), g.end(), p) != g.end()) continue;  // 组内去重
        // 跨位置去重：同一面板不能同时出现在树里两处（否则写矩形会后者覆盖
        // 前者，而绘制是两处各画一遍 —— 视觉上"面板被劈成两块"）。
        int qn = -1, qs = -1, qi = -1;
        if (locatePanel(p, qn, qs, qi)) continue;
        g.push_back(p);
    }

    int& a = (side == kSideA) ? n.activeA : n.activeB;
    if (g.empty()) a = 0;
    else a = std::clamp(active, 0, static_cast<int>(g.size()) - 1);

    syncPanelVisibility();   // 摘空的一侧立刻收成 0，不等下一帧
    return true;
}

bool SplitLayout::activateInGroup(int node, int side, int idx) {
    if (!isLeaf(node, side)) return false;
    SplitNode& n = m_nodes[static_cast<size_t>(node)];
    const std::vector<int>& g = (side == kSideA) ? n.groupA : n.groupB;
    if (idx < 0 || idx >= static_cast<int>(g.size())) return false;
    int& a = (side == kSideA) ? n.activeA : n.activeB;
    a = idx;
    syncPanelVisibility();
    return true;
}

void SplitLayout::restoreGroups(const std::vector<int> (&groups)[kNodeCount][2]) {
    // 先整棵清空，再一次性灌进去。中间没有"面板已经在别处"这种状态，
    // 所以互换、轮转、整体重排都能一次到位。
    for (SplitNode& n : m_nodes) {
        if (n.a < 0) { n.groupA.clear(); n.activeA = 0; }
        if (n.b < 0) { n.groupB.clear(); n.activeB = 0; }
    }

    // 全树范围内去重：同一面板出现在两处时，**先出现的赢**，后者丢弃。
    // （写矩形是后者覆盖前者、绘制却是两处各画一遍 —— 视觉上是"面板被劈成
    //   两块"，比丢掉更难查。）
    std::vector<int> used;
    for (int i = 0; i < kNodeCount && i < static_cast<int>(m_nodes.size()); ++i) {
        for (int side = 0; side < 2; ++side) {
            if (!isLeaf(i, side)) continue;
            SplitNode& n = m_nodes[static_cast<size_t>(i)];
            std::vector<int>& g = (side == kSideA) ? n.groupA : n.groupB;
            int& a = (side == kSideA) ? n.activeA : n.activeB;
            for (int panel : groups[i][side]) {
                if (!isTreeSlot(panel)) continue;
                if (std::find(used.begin(), used.end(), panel) != used.end()) continue;
                used.push_back(panel);
                g.push_back(panel);
            }
            a = 0;
        }
    }
    syncPanelVisibility();
}
bool SplitLayout::locatePanel(int panel, int& node, int& side, int& idx) const {
    node = -1; side = -1; idx = -1;
    if (!isTreeSlot(panel)) return false;
    for (int i = 0; i < static_cast<int>(m_nodes.size()); ++i) {
        const SplitNode& n = m_nodes[static_cast<size_t>(i)];
        for (int k = 0; k < static_cast<int>(n.groupA.size()); ++k) {
            if (n.groupA[static_cast<size_t>(k)] == panel) {
                node = i; side = kSideA; idx = k;
                return true;
            }
        }
        for (int k = 0; k < static_cast<int>(n.groupB.size()); ++k) {
            if (n.groupB[static_cast<size_t>(k)] == panel) {
                node = i; side = kSideB; idx = k;
                return true;
            }
        }
    }
    return false;
}

bool SplitLayout::movePanel(int panel, int node, int side) {
    if (!isTreeSlot(panel)) return false;
    if (!isLeaf(node, side)) return false;   // 目标是子节点侧 → 拒绝（E29-2 再做切分）

    int on = -1, os = -1, oi = -1;
    const bool found = locatePanel(panel, on, os, oi);

    // 已经在目标组里 → 只切活动项。**这不算失败**：拖到自己身上是常见的
    // 误操作，返回 false 会让落点逻辑弹出"不能放在这里"的提示。
    if (found && on == node && os == side) return activateInGroup(node, side, oi);

    if (found) {
        SplitNode& src = m_nodes[static_cast<size_t>(on)];
        std::vector<int>& sg = (os == kSideA) ? src.groupA : src.groupB;
        sg.erase(sg.begin() + oi);
        int& sa = (os == kSideA) ? src.activeA : src.activeB;
        // 摘掉后活动下标可能越界：夹到组内最后一格（组空则归 0）
        sa = sg.empty() ? 0 : std::min(sa, static_cast<int>(sg.size()) - 1);
    }

    SplitNode& dst = m_nodes[static_cast<size_t>(node)];
    std::vector<int>& dg = (side == kSideA) ? dst.groupA : dst.groupB;
    dg.push_back(panel);
    int& da = (side == kSideA) ? dst.activeA : dst.activeB;
    da = static_cast<int>(dg.size()) - 1;

    syncPanelVisibility();
    return true;
}

bool SplitLayout::swapActive(int nodeA, int sideA, int nodeB, int sideB) {
    if (!isLeaf(nodeA, sideA) || !isLeaf(nodeB, sideB)) return false;
    if (nodeA == nodeB && sideA == sideB) return true;   // 自己换自己：幂等

    SplitNode& na = m_nodes[static_cast<size_t>(nodeA)];
    SplitNode& nb = m_nodes[static_cast<size_t>(nodeB)];
    std::vector<int>& ga = (sideA == kSideA) ? na.groupA : na.groupB;
    std::vector<int>& gb = (sideB == kSideA) ? nb.groupA : nb.groupB;
    int& ia = (sideA == kSideA) ? na.activeA : na.activeB;
    int& ib = (sideB == kSideA) ? nb.activeA : nb.activeB;

    if (ga.empty() || gb.empty()) return false;   // 空侧没有可交换的东西
    if (ia < 0 || ia >= static_cast<int>(ga.size())) return false;
    if (ib < 0 || ib >= static_cast<int>(gb.size())) return false;

    std::swap(ga[static_cast<size_t>(ia)], gb[static_cast<size_t>(ib)]);
    syncPanelVisibility();
    return true;
}

} // namespace editor
