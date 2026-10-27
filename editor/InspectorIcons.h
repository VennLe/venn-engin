#pragma once
// ============================================================
// editor/InspectorIcons —— Inspector 左侧页签栏的矢量图标
//
// 与 `EditorIcons.h` 的**分工**（别把两边的图标混着放）：
//   EditorIcons.h —— 树 / 内容浏览器里的**资产图标**，颜色写死在函数里。
//                    理由：图标代表"这是什么东西"，属于**内容**，
//                    不该跟着面板配色漂移。
//   本文件        —— 属性面板页签栏的 **UI 图标**，颜色**必须**由调用方给。
//                    理由：常态 / 悬停 / 选中是三套颜色，它属于**控件**。
//                    另外它还要能在窄条里竖直排一列，尺寸必须跟着 DPI 走。
//
// 画法约定（所有图标一致，改起来才不费劲）：
//   · 一律画在 [p, p+s] 这个正方形里，用 **0..1 归一化坐标**描述形状，
//     由 IconPen 换算成屏幕坐标 —— 换尺寸 / 换 DPI / 改字号都不用动画法。
//   · 线宽按 s 归一（默认 0.09·s）：24px 时约 2.2px，缩到 16px 也不会糊。
//   · 只用描边（AddLine / AddRect / AddCircle / AddEllipse / PathStroke），
//     填充只用在"这个形状本身就该是实的"（粒子星、动画关键帧、All 的三条）。
//     **不要用 AddPolyline**：它的 thikness/flags 参数签名在新版 ImGui 里
//     调换过（imgui.h 里留了 deleted 重载），用它必踩坑。
// ============================================================

#include <imgui.h>

namespace editor {

// 把归一化坐标换算成屏幕坐标的小笔。图标函数都拿它画，避免每处重复算
// `p.x + x * s`。
struct IconPen {
    ImDrawList* dl;
    ImVec2 p;
    float s;
    ImU32 col;
    float th;

    IconPen(ImDrawList* d, ImVec2 at, float size, ImU32 color,
            float widthScale = 0.09f)
        : dl(d), p(at), s(size), col(color), th(size * widthScale) {}

    ImVec2 at(float x, float y) const {
        return ImVec2(p.x + x * s, p.y + y * s);
    }
    float len(float f) const { return f * s; }

    void line(float x0, float y0, float x1, float y1) const {
        dl->AddLine(at(x0, y0), at(x1, y1), col, th);
    }
    void line(const ImVec2& a, const ImVec2& b) const {
        dl->AddLine(a, b, col, th);
    }
    void rect(float x0, float y0, float x1, float y1, float round = 0.0f) const {
        dl->AddRect(at(x0, y0), at(x1, y1), col, len(round), 0, th);
    }
    void rectFill(float x0, float y0, float x1, float y1,
                  float round = 0.0f) const {
        dl->AddRectFilled(at(x0, y0), at(x1, y1), col, len(round));
    }
    void circle(float cx, float cy, float r) const {
        dl->AddCircle(at(cx, cy), len(r), col, 0, th);
    }
    void disc(float cx, float cy, float r) const {
        dl->AddCircleFilled(at(cx, cy), len(r), col);
    }
    void ellipse(float cx, float cy, float rx, float ry, float rotDeg) const {
        dl->AddEllipse(at(cx, cy), ImVec2(len(rx), len(ry)), col,
                       rotDeg * 3.14159265358979f / 180.0f, 0, th);
    }
    // 开口圆弧（用于扳手 / 括号这类"有缺口的圆"）。角度制，逆时针扫。
    void arc(float cx, float cy, float r, float aMinDeg, float aMaxDeg) const {
        dl->PathArcTo(at(cx, cy), len(r), aMinDeg * 3.14159265358979f / 180.0f,
                      aMaxDeg * 3.14159265358979f / 180.0f, 0);
        dl->PathStroke(col, 0, th);
    }
    void triFill(float x0, float y0, float x1, float y1, float x2, float y2) const {
        dl->AddTriangleFilled(at(x0, y0), at(x1, y1), at(x2, y2), col);
    }
    void quadFill(float x0, float y0, float x1, float y1, float x2, float y2,
                  float x3, float y3) const {
        dl->AddQuadFilled(at(x0, y0), at(x1, y1), at(x2, y2), at(x3, y3), col);
    }
    // ⚠ 刻意**不**提供 AddPolyline 包装：它的 (thickness, flags) 参数在新版
    // ImGui 里调换过，旧签名被标成 obsolete / deleted，包一层就是给自己埋雷。
    // 折线直接用 line() 逐段画即可（图标都只有两三段）。
};

// ---------------------------------------------------------------- 页签图标
//
// 顺序与 InspectorPanel 的 InspTab 枚举一一对应（表里逐项引用，不靠下标猜）。

// 物体属性：等距立方体（外六边形 + 内部 Y 形三条棱）
inline void iconObject(ImDrawList* dl, ImVec2 p, float s, ImU32 col) {
    const IconPen k(dl, p, s, col);
    const ImVec2 hex[6] = {k.at(0.50f, 0.05f), k.at(0.94f, 0.28f),
                           k.at(0.94f, 0.72f), k.at(0.50f, 0.95f),
                           k.at(0.06f, 0.72f), k.at(0.06f, 0.28f)};
    for (int i = 0; i < 6; ++i) k.line(hex[i], hex[(i + 1) % 6]);
    const ImVec2 c = k.at(0.50f, 0.50f);
    k.line(c, hex[5]);   // 往左上
    k.line(c, hex[1]);   // 往右上
    k.line(c, hex[3]);   // 往下（可见的三条内棱）
}

// 工具：扳手（右上开口头 + 左下闭口环 + 斜手柄）
inline void iconTool(ImDrawList* dl, ImVec2 p, float s, ImU32 col) {
    const IconPen k(dl, p, s, col);
    // 手柄：加粗斜线，两端各缩一点，别捅进两个头里
    k.line(0.62f, 0.38f, 0.30f, 0.70f);
    k.circle(0.20f, 0.79f, 0.115f);            // 闭口端
    k.arc(0.73f, 0.26f, 0.20f, -10.0f, 280.0f); // 开口端（缺口朝右上，即手柄的反方向）
}

// 渲染：相机（机身 + 镜头 + 取景器）
inline void iconRender(ImDrawList* dl, ImVec2 p, float s, ImU32 col) {
    const IconPen k(dl, p, s, col);
    k.rect(0.04f, 0.28f, 0.96f, 0.90f, 0.10f);
    k.circle(0.50f, 0.59f, 0.20f);
    k.rect(0.30f, 0.14f, 0.62f, 0.28f, 0.05f);
}

// 输出：托盘 + 向外的箭头（"导出"语义，比打印机更好认）
inline void iconOutput(ImDrawList* dl, ImVec2 p, float s, ImU32 col) {
    const IconPen k(dl, p, s, col);
    // 箭头杆 + 箭头（朝上，表示"往外送"）
    k.line(0.50f, 0.72f, 0.50f, 0.16f);
    k.triFill(0.50f, 0.05f, 0.33f, 0.28f, 0.67f, 0.28f);
    // 下面的托盘（三面，顶部开口，箭头从中穿出）
    k.line(0.08f, 0.56f, 0.08f, 0.92f);
    k.line(0.08f, 0.92f, 0.92f, 0.92f);
    k.line(0.92f, 0.92f, 0.92f, 0.56f);
}

// 场景：带太阳与山的画面（"这是一幅场景"）
inline void iconScene(ImDrawList* dl, ImVec2 p, float s, ImU32 col) {
    const IconPen k(dl, p, s, col);
    k.rect(0.05f, 0.16f, 0.95f, 0.84f, 0.10f);
    k.disc(0.30f, 0.36f, 0.075f);
    k.line(0.12f, 0.78f, 0.40f, 0.46f);   // 左侧山峰
    k.line(0.40f, 0.46f, 0.60f, 0.78f);
    k.line(0.46f, 0.78f, 0.66f, 0.58f);   // 右侧小峰
    k.line(0.66f, 0.58f, 0.90f, 0.78f);
}

// 世界属性：地球（圆 + 经线椭圆 + 赤道）
inline void iconWorld(ImDrawList* dl, ImVec2 p, float s, ImU32 col) {
    const IconPen k(dl, p, s, col);
    k.circle(0.50f, 0.50f, 0.42f);
    k.ellipse(0.50f, 0.50f, 0.17f, 0.42f, 0.0f);
    k.line(0.08f, 0.50f, 0.92f, 0.50f);
}

// 粒子系统：四角星 + 两颗小星（"一堆飞散的粒子"）
inline void iconParticles(ImDrawList* dl, ImVec2 p, float s, ImU32 col) {
    const IconPen k(dl, p, s, col);
    k.quadFill(0.42f, 0.10f, 0.60f, 0.42f, 0.42f, 0.74f, 0.24f, 0.42f);
    k.quadFill(0.79f, 0.20f, 0.88f, 0.32f, 0.79f, 0.44f, 0.70f, 0.32f);
    k.quadFill(0.76f, 0.62f, 0.86f, 0.74f, 0.76f, 0.86f, 0.66f, 0.74f);
}

// 物理系统：小球 + 向下的重力箭头。
//
// 一开始画的是"原子"（两条倾斜轨道），但在 24px 下两条椭圆正好交叉成一个
// **蝴蝶结**，完全读不出原子 —— 见 2026-10-16 的探针截图。小球 + 重力箭头
// 只有两个形状，缩到 16px 依然分得清。
inline void iconPhysics(ImDrawList* dl, ImVec2 p, float s, ImU32 col) {
    const IconPen k(dl, p, s, col);
    k.circle(0.50f, 0.30f, 0.21f);                        // 球
    k.line(0.50f, 0.60f, 0.50f, 0.80f);                   // 箭头杆
    k.triFill(0.50f, 0.95f, 0.36f, 0.72f, 0.64f, 0.72f);  // 箭头
}

// 材质：球体 + 高光点（"一颗被照着的材质球"）
inline void iconMaterial(ImDrawList* dl, ImVec2 p, float s, ImU32 col) {
    const IconPen k(dl, p, s, col);
    k.circle(0.50f, 0.50f, 0.42f);
    k.disc(0.34f, 0.33f, 0.085f);
}

// 动画：坐标轴 + 一条缓动曲线（"曲线编辑器"）。
//
// 试过两版"关键帧菱形"，都在这个尺寸上失败：图标实际只有约 19px 高，
//   · 一颗大菱形 + 贯穿的轴线 → 整体就是个**加号**；
//   · 两颗菱形 + 轴线 → 变成**哑铃**。
// 根因是"实心菱形和轴线会糊成一块"。曲线方案全是线段、形状方向各异，
// 19px 下反而分得清，而且语义更准（本页就是动画曲线/片段播放）。
inline void iconAnimation(ImDrawList* dl, ImVec2 p, float s, ImU32 col) {
    const IconPen k(dl, p, s, col);
    k.line(0.17f, 0.05f, 0.17f, 0.91f);   // 纵轴
    k.line(0.10f, 0.91f, 0.96f, 0.91f);   // 横轴
    k.line(0.17f, 0.68f, 0.44f, 0.68f);   // 曲线：平 → 陡 → 平（缓动）
    k.line(0.44f, 0.68f, 0.64f, 0.24f);
    k.line(0.64f, 0.24f, 0.94f, 0.24f);
}

// 过场时间轴（C1）：一条轨道 + 三根关键帧刻度 + 一根播放头竖杆。
// 不用"胶片/场记板"那种画法：19px 下齿孔会糊成虚线，反而像"列表"。
inline void iconSequence(ImDrawList* dl, ImVec2 p, float s, ImU32 col) {
    const IconPen k(dl, p, s, col);
    k.line(0.10f, 0.60f, 0.94f, 0.60f);              // 轨道
    k.line(0.18f, 0.46f, 0.18f, 0.74f);              // 关键帧刻度
    k.line(0.46f, 0.46f, 0.46f, 0.74f);
    k.line(0.74f, 0.46f, 0.74f, 0.74f);
    k.rectFill(0.60f, 0.14f, 0.66f, 0.46f, 0.0f);    // 播放头
}

// 对话（C2）：三行文字 + 一个尾巴，像一块字幕框。
// ⚠ 不做"气泡轮廓"：IconPen 只有 `line` / `rectFill`，画轮廓要 12 条线，
//   在这个 14 px 的尺寸上看不出圆角，只有糊。三行实心条 + 尾巴反而更像
//   "字幕框"这个它真正代表的东西。
inline void iconDialogue(ImDrawList* dl, ImVec2 p, float s, ImU32 col) {
    const IconPen k(dl, p, s, col);
    k.rectFill(0.10f, 0.16f, 0.90f, 0.30f, 0.04f);   // 说话人
    k.rectFill(0.10f, 0.40f, 0.72f, 0.52f, 0.03f);   // 正文第 1 行
    k.rectFill(0.10f, 0.60f, 0.56f, 0.72f, 0.03f);   // 正文第 2 行
    k.line(0.74f, 0.72f, 0.74f, 0.90f);              // 继续箭头（尾巴）
}

// 全部：三条横杠（列表 = "把上面所有页签摊平"）
inline void iconAll(ImDrawList* dl, ImVec2 p, float s, ImU32 col) {
    const IconPen k(dl, p, s, col);
    k.rectFill(0.12f, 0.19f, 0.88f, 0.33f, 0.05f);
    k.rectFill(0.12f, 0.43f, 0.88f, 0.57f, 0.05f);
    k.rectFill(0.12f, 0.67f, 0.88f, 0.81f, 0.05f);
}

} // namespace editor
