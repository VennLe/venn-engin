#include "SelectionModes.h"

#include <cmath>

namespace editor {

namespace {

// 与 EditModes.cpp 里那套 Pen 同一坐标系（16×16 单位网格），
// 但只保留这里用得到的几个图元 —— 抄 30 行比把那个匿名结构体提到公共
// 头文件里、再给十个调用点加 include 要便宜。
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

// 四张图都画在 1..15 的范围内，留 1 单位出血量 —— 径向菜单里图标是
// 24~28px 现场放大绘制的，边缘贴到 0 会看起来"被裁掉"。

void paintPick(const Pen& g) {
    // 鼠标箭头：三角头 + 斜尾巴（和 EditMode 的 Select 同一形状，
    // 它们在用户眼里就是同一件事，换个画法反而让人以为是新功能）
    g.tri(3.0f, 1.5f, 3.0f, 11.5f, 11.4f, 7.6f);
    g.quad(8.2f, 8.0f, 12.4f, 14.6f, 10.4f, 15.4f, 6.6f, 9.2f);
}

void paintBox(const Pen& g) {
    // 一个虚线感觉的矩形：四段实线 + 两个角上补一点
    g.line(2.0f, 3.0f, 9.0f, 3.0f, 1.2f);
    g.line(2.0f, 3.0f, 2.0f, 9.0f, 1.2f);
    g.line(14.0f, 7.0f, 14.0f, 13.0f, 1.2f);
    g.line(7.0f, 13.0f, 14.0f, 13.0f, 1.2f);
    g.line(9.0f, 3.0f, 14.0f, 3.0f, 0.9f);
    g.line(14.0f, 3.0f, 14.0f, 7.0f, 0.9f);
    g.line(2.0f, 9.0f, 2.0f, 13.0f, 0.9f);
    g.line(2.0f, 13.0f, 7.0f, 13.0f, 0.9f);
    g.dot(8.0f, 8.0f, 1.1f);
}

void paintLasso(const Pen& g) {
    // 一条收口的水滴状曲线（套索的典型画法）+ 收尾的小尾巴
    const ImVec2 pts[10] = {
        g.p(8.0f, 1.6f),  g.p(13.2f, 4.6f), g.p(14.2f, 9.0f),
        g.p(11.0f, 12.8f), g.p(6.2f, 13.4f), g.p(2.4f, 10.4f),
        g.p(2.0f, 6.0f),  g.p(4.8f, 2.6f),  g.p(8.0f, 1.6f),
        g.p(8.0f, 1.6f),
    };
    g.poly(pts, 9, 1.2f);
    g.line(8.0f, 13.0f, 9.4f, 15.2f, 1.0f);
}

void paintBrush(const Pen& g) {
    // 笔刷：一个斜置的笔杆 + 一撮刷毛
    g.line(11.6f, 2.2f, 6.4f, 9.6f, 1.6f);
    g.tri(6.4f, 9.6f, 3.0f, 13.4f, 8.4f, 12.2f);
    g.ring(4.4f, 12.0f, 0.9f, 0.9f);
}

} // namespace

const char* selectionModeName(SelectionMode m) {
    switch (m) {
        case SelectionMode::Pick:  return "Select";
        case SelectionMode::Box:   return "Box Select";
        case SelectionMode::Lasso: return "Lasso Select";
        case SelectionMode::Brush: return "Brush Select";
    }
    return "Select";
}

const char* selectionModeId(SelectionMode m) {
    switch (m) {
        case SelectionMode::Pick:  return "pick";
        case SelectionMode::Box:   return "box";
        case SelectionMode::Lasso: return "lasso";
        case SelectionMode::Brush: return "brush";
    }
    return "pick";
}

const char* selectionModeHint(SelectionMode m) {
    switch (m) {
        case SelectionMode::Pick:
            return "Click an object to select it, click empty space to clear.";
        case SelectionMode::Box:
            return "Drag a rectangle; everything it touches gets selected.";
        case SelectionMode::Lasso:
            return "Draw a freehand outline; objects inside it get selected.";
        case SelectionMode::Brush:
            return "Hold the left button and paint over objects to select them.";
    }
    return "";
}

SelectionMode clampSelectionMode(int v) {
    if (v < 0) v = 0;
    if (v >= kSelectionModeCount) v = kSelectionModeCount - 1;
    return static_cast<SelectionMode>(v);
}

void paintSelectionModeIcon(ImDrawList* dl, ImVec2 p, float s, SelectionMode m,
                            ImU32 col) {
    if (!dl) return;
    const Pen g{dl, p, s / 16.0f, col};
    switch (m) {
        case SelectionMode::Pick:  paintPick(g); break;
        case SelectionMode::Box:   paintBox(g); break;
        case SelectionMode::Lasso: paintLasso(g); break;
        case SelectionMode::Brush: paintBrush(g); break;
    }
}

bool isSelectableObject(const ecs::World& w, ecs::Entity e) {
    if (!e.valid()) return false;
    // 天空 / 云层 / 星空不是实体（程序化绘制），走到这里就说明是别的东西。
    if (!w.has<ecs::TransformComponent>(e)) return false;
    // 地面 / 地形：编辑器参照物，任何方式都不选。
    if (w.has<ecs::LockedComponent>(e)) return false;
    return true;
}

} // namespace editor
