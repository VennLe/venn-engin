#include "EditModes.h"

#include <cmath>

// 图标为什么手画：ImGui 自带字体只有 ASCII + 少量符号，UE 那套 9 个
// 编辑模式图标（山 / 草 / 调色板 / 立方体 / 碎裂 / 笔刷 / 小人 / 节点）
// 一个都不在字体里。用矢量画的好处是任意 DPI 都清楚、随主题变色 ——
// 与顶栏播放条（Toolbar.cpp 的 paintTransportIcon）同一套做法。

namespace editor {

namespace {

// 16×16 的单位网格 → 屏幕坐标。全部图标都按这个坐标系写，
// 这样"图标多大"只由 s 决定，形状本身与分辨率无关。
struct Pen {
    ImDrawList* dl;
    ImVec2 o;
    float k;   // 1 单位 = k 像素
    ImU32 col;

    ImVec2 p(float x, float y) const {
        return ImVec2(o.x + x * k, o.y + y * k);
    }
    void line(float x0, float y0, float x1, float y1, float th = 1.0f) const {
        dl->AddLine(p(x0, y0), p(x1, y1), col, th * k);
    }
    void tri(float x0, float y0, float x1, float y1, float x2, float y2) const {
        dl->AddTriangleFilled(p(x0, y0), p(x1, y1), p(x2, y2), col);
    }
    void quad(float x0, float y0, float x1, float y1, float x2, float y2,
              float x3, float y3) const {
        dl->AddQuadFilled(p(x0, y0), p(x1, y1), p(x2, y2), p(x3, y3), col);
    }
    void rect(float x0, float y0, float x1, float y1, float th = 1.0f) const {
        dl->AddRect(p(x0, y0), p(x1, y1), col, 0.0f, 0, th * k);
    }
    void dot(float x, float y, float r) const {
        dl->AddCircleFilled(p(x, y), r * k, col);
    }
    void ring(float x, float y, float r, float th = 1.0f) const {
        dl->AddCircle(p(x, y), r * k, col, 0, th * k);
    }
    void poly(const ImVec2* pts, int n, float th = 1.0f) const {
        dl->AddPolyline(pts, n, col, ImDrawFlags_Closed, th * k);
    }
};

void paintSelect(const Pen& g) {
    // 鼠标箭头：三角头 + 斜向的尾巴（分成两块填充，避免了凹多边形）
    g.tri(3.0f, 1.5f, 3.0f, 11.5f, 11.4f, 7.6f);
    g.quad(8.2f, 8.0f, 12.4f, 14.6f, 10.4f, 15.4f, 6.6f, 9.2f);
}

void paintLandscape(const Pen& g) {
    // 两座山峰 + 一条地平线
    g.tri(0.8f, 13.4f, 6.4f, 3.8f, 12.0f, 13.4f);
    g.tri(7.6f, 13.4f, 11.6f, 7.0f, 15.6f, 13.4f);
    g.line(0.5f, 14.8f, 15.5f, 14.8f, 1.0f);
}

void paintFoliage(const Pen& g) {
    // 三根草叶从底部同一处散开，再加两片小叶子
    g.line(7.6f, 15.0f, 3.2f, 5.4f, 1.1f);
    g.line(8.0f, 15.0f, 8.0f, 2.6f, 1.1f);
    g.line(8.4f, 15.0f, 12.8f, 5.8f, 1.1f);
    g.line(3.2f, 5.4f, 5.6f, 4.4f, 0.9f);
    g.line(12.8f, 5.8f, 10.4f, 4.8f, 0.9f);
}

void paintMeshPaint(const Pen& g) {
    // 调色板（圆 + 缺口）+ 三颗颜料点 + 一支斜插的笔
    g.ring(6.6f, 9.4f, 5.0f, 1.1f);
    g.line(11.0f, 10.4f, 15.4f, 4.6f, 1.6f);
    g.dot(4.8f, 7.6f, 1.0f);
    g.dot(7.2f, 5.8f, 1.0f);
    g.dot(8.4f, 9.0f, 1.0f);
}

void paintModeling(const Pen& g) {
    // 立方体线框：外六边形 + 三条内棱
    const ImVec2 hex[6] = {g.p(8.0f, 1.2f),  g.p(14.2f, 4.8f),
                           g.p(14.2f, 11.2f), g.p(8.0f, 14.8f),
                           g.p(1.8f, 11.2f), g.p(1.8f, 4.8f)};
    g.poly(hex, 6, 1.1f);
    g.line(8.0f, 1.2f, 8.0f, 8.0f, 1.0f);
    g.line(8.0f, 8.0f, 14.2f, 4.8f, 1.0f);
    g.line(8.0f, 8.0f, 1.8f, 4.8f, 1.0f);
}

void paintFracture(const Pen& g) {
    // 方块 + 贯穿的裂缝（两段折线）
    g.rect(1.6f, 1.6f, 14.4f, 14.4f, 1.1f);
    g.line(9.4f, 1.6f, 6.2f, 6.0f, 1.0f);
    g.line(6.2f, 6.0f, 9.6f, 8.6f, 1.0f);
    g.line(9.6f, 8.6f, 6.8f, 14.4f, 1.0f);
    g.line(9.6f, 8.6f, 14.4f, 10.4f, 0.9f);
}

void paintBrushEdit(const Pen& g) {
    // 画布 + 一支压在上面的笔
    g.rect(1.4f, 3.6f, 9.8f, 12.4f, 1.1f);
    g.line(7.6f, 9.6f, 14.6f, 2.6f, 1.8f);
    g.tri(6.2f, 11.4f, 8.8f, 10.2f, 7.4f, 12.8f);
}

void paintAnimation(const Pen& g) {
    // 小人跑姿：头 + 躯干 + 两条腿 + 两条手臂
    g.ring(9.6f, 3.2f, 1.9f, 1.1f);
    g.line(9.2f, 5.2f, 7.2f, 9.4f, 1.4f);
    g.line(7.2f, 9.4f, 3.4f, 12.4f, 1.3f);
    g.line(7.2f, 9.4f, 10.6f, 13.6f, 1.3f);
    g.line(8.8f, 6.4f, 4.6f, 6.6f, 1.1f);
    g.line(8.8f, 6.4f, 12.6f, 4.2f, 1.1f);
}

void paintPcg(const Pen& g) {
    // 三个节点 + 两条连线（程序化内容生成：图）
    const float n = 4.2f;
    g.rect(1.2f, 1.4f, 1.2f + n, 1.4f + n, 1.0f);
    g.rect(10.6f, 1.4f, 10.6f + n, 1.4f + n, 1.0f);
    g.rect(5.9f, 10.2f, 5.9f + n, 10.2f + n, 1.0f);
    g.line(5.4f, 3.5f, 10.6f, 3.5f, 1.0f);
    g.line(7.8f, 5.6f, 7.8f, 10.2f, 1.0f);
}

void paintWater(const Pen& g) {
    // 两道水波（上疏下密）+ 一个水滴 —— 三条正弦折线的振幅递减，
    // 与真实水面"一条主涌浪 + 表面细纹"的层次一致。
    auto wave = [&g](float y, float amp, float th) {
        const int segs = 8;
        ImVec2 pts[segs + 1];
        for (int i = 0; i <= segs; ++i) {
            const float x = 1.2f + 13.6f * static_cast<float>(i) /
                                       static_cast<float>(segs);
            const float yy = y + amp * std::sin(static_cast<float>(i) * 1.35f);
            pts[i] = g.p(x, yy);
        }
        g.dl->AddPolyline(pts, segs + 1, g.col, 0, th * g.k);
    };
    wave(6.4f, 1.5f, 1.15f);
    wave(9.8f, 0.9f, 0.9f);
    // 水滴：上面一个小圆点 + 下面两条收敛的斜线
    g.dot(8.0f, 2.2f, 1.15f);
    g.line(7.1f, 3.4f, 8.0f, 4.9f, 0.9f);
    g.line(8.9f, 3.4f, 8.0f, 4.9f, 0.9f);
}

} // namespace

const char* editModeName(EditMode m) {
    switch (m) {
        case EditMode::Select:     return "Select";
        case EditMode::Landscape:  return "Landscape";
        case EditMode::Foliage:    return "Foliage";
        case EditMode::MeshPaint:  return "Mesh Paint";
        case EditMode::Modeling:   return "Modeling";
        case EditMode::Fracture:   return "Fracture";
        case EditMode::BrushEdit:  return "Brush Editing";
        case EditMode::Animation:  return "Animation";
        case EditMode::PCG:        return "PCG";
        case EditMode::Water:      return "Water";
    }
    return "Select";
}

const char* editModeId(EditMode m) {
    switch (m) {
        case EditMode::Select:     return "select";
        case EditMode::Landscape:  return "landscape";
        case EditMode::Foliage:    return "foliage";
        case EditMode::MeshPaint:  return "mesh_paint";
        case EditMode::Modeling:   return "modeling";
        case EditMode::Fracture:   return "fracture";
        case EditMode::BrushEdit:  return "brush_edit";
        case EditMode::Animation:  return "animation";
        case EditMode::PCG:        return "pcg";
        case EditMode::Water:      return "water";
    }
    return "select";
}

const char* editModeHint(EditMode m) {
    switch (m) {
        case EditMode::Select:
            return "Click to select, then W / E / R to move, rotate or scale.";
        case EditMode::Landscape:
            return "Sculpt the terrain heightmap and paint its 4 material layers.";
        case EditMode::Foliage:
            return "Scatter grass, bushes and trees over the terrain.";
        case EditMode::MeshPaint:
            return "Paint vertex colors straight onto a mesh.";
        case EditMode::Modeling:
            return "Edit meshes with a poly/edge modelling toolkit.";
        case EditMode::Fracture:
            return "Break a mesh into chunks with a physics fracture solver.";
        case EditMode::BrushEdit:
            return "Compose level geometry by stamping brush solids.";
        case EditMode::Animation:
            return "Animate skeletons with a timeline and curves.";
        case EditMode::PCG:
            return "Generate content procedurally from a node graph.";
        case EditMode::Water:
            return "Drag out ponds, lakes and rivers, then shape the water "
                   "colour, waves and flow.";
    }
    return "";
}

// 十项全部落地（Select/Landscape/Foliage 三项 2026-10-20，ToolkitEditors 那
// 六个模式同一批，Water 2026-10-21）。这个函数**不是**"有没有代码"，而是
// "顶栏下拉里那一行能不能点 / 选到之后有没有面板" —— Toolbar.cpp 用它决定
// 置灰，EditorContext 用它写进 `EDITMODE ... implemented=` 日志（自动化按它
// 断言）。
//
// 所以只写 `return true` 是不够的：每个模式必须真的有一条
//     切模式 → EditorApp::serviceEditMode 开面板 → 面板有 logRect 锚点 → 能改场景
// 的链路，否则点进去就是一个空窗口 = 用户说的"没实现"。
//
// 十项现在都是 true，于是 switch 退化成"全部返回 true"。保留 switch 形式是
// 为了留一个显式的落点：将来若某个模式要临时收回（比如重做），把对应 case
// 挪到 return false 那一支即可，不用改调用方。
bool editModeImplemented(EditMode m) {
    switch (m) {
        case EditMode::Select:
        case EditMode::Landscape:
        case EditMode::Foliage:
        case EditMode::MeshPaint:
        case EditMode::Modeling:
        case EditMode::Fracture:
        case EditMode::BrushEdit:
        case EditMode::Animation:
        case EditMode::PCG:
        case EditMode::Water:
            return true;
    }
    return false;
}

// ------------------------------------------------------------
// 左栏最小宽度：谁家的面板能换列，谁就能被拖窄
// ------------------------------------------------------------
// 判据只有一条：**面板在 300 宽时按钮还点得到吗**。
//   · Landscape / Foliage / Select —— 按钮按 `GetContentRegionAvail()` 换列，
//     300 时退化成 1~2 列，全都还在窗口里 → 300。
//   · 其余七项（ToolkitEditors 那六个 + Water）是一条 `SameLine` 硬排到底
//     （建模面板那排合计 400 出头），300 宽时最右边那个按钮会被窗口裁掉
//     —— 裁掉的控件"有矩形但点不到"，比直接不给拖更糟 → 460。
//
// ⚠ 这个数字**同时**是默认宽度与拖动下限（2026-10-28 第二刀，见 #317）：
//   SplitLayout 的默认宽度 = `clamp(bodyW * kLeftFrac, editModeRailMinWidth(m), …)`，
//   拖动下限 = 同一个数。所以 Select 下默认左栏就是 300、视口 936px 宽，
//   右侧工具条 7 颗按钮刚好放得下；切到 Modeling 会被顶到 460。
float editModeRailMinWidth(EditMode m) {
    switch (m) {
        case EditMode::Select:
        case EditMode::Landscape:
        case EditMode::Foliage:
            return 300.0f;
        default:
            return 460.0f;
    }
}

EditMode clampEditMode(int v) {
    if (v < 0) v = 0;
    if (v >= kEditModeCount) v = kEditModeCount - 1;
    return static_cast<EditMode>(v);
}

void paintEditModeIcon(ImDrawList* dl, ImVec2 p, float s, EditMode m,
                       ImU32 col) {
    if (!dl) return;
    const Pen g{dl, p, s / 16.0f, col};
    switch (m) {
        case EditMode::Select:     paintSelect(g); break;
        case EditMode::Landscape:  paintLandscape(g); break;
        case EditMode::Foliage:    paintFoliage(g); break;
        case EditMode::MeshPaint:  paintMeshPaint(g); break;
        case EditMode::Modeling:   paintModeling(g); break;
        case EditMode::Fracture:   paintFracture(g); break;
        case EditMode::BrushEdit:  paintBrushEdit(g); break;
        case EditMode::Animation:  paintAnimation(g); break;
        case EditMode::PCG:        paintPcg(g); break;
        case EditMode::Water:      paintWater(g); break;
    }
}

} // namespace editor
