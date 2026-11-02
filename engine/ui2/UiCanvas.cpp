#include "ui2/UiCanvas.h"

#include "ui2/SdfFont.h"
#include "ui2/UiWidget.h"

#include <cmath>
#include <cstring>

namespace ui2 {

// ---------------------------------------------------------------- 变换栈

Ui2Xform Ui2Xform::rotateScaleAbout(float rad, float s, float px, float py) {
    const float c = std::cos(rad) * s;
    const float n = std::sin(rad) * s;
    // 绕 pivot：T(p)·R·S·T(-p)
    Ui2Xform x;
    x.m00 = c;   x.m01 = -n;
    x.m10 = n;   x.m11 = c;
    x.tx = px - (c * px - n * py);
    x.ty = py - (n * px + c * py);
    return x;
}

Ui2Xform Ui2Xform::concat(const Ui2Xform& b, const Ui2Xform& a) {
    Ui2Xform r;
    r.m00 = b.m00 * a.m00 + b.m01 * a.m10;
    r.m01 = b.m00 * a.m01 + b.m01 * a.m11;
    r.m10 = b.m10 * a.m00 + b.m11 * a.m10;
    r.m11 = b.m10 * a.m01 + b.m11 * a.m11;
    r.tx = b.m00 * a.tx + b.m01 * a.ty + b.tx;
    r.ty = b.m10 * a.tx + b.m11 * a.ty + b.ty;
    return r;
}

void UiCanvas::recomputeState() {
    // 三条栈都从索引 0 重新累一遍。**每次 push/pop 重算**而不是"弹出时
    // 除回去"：除回去遇到 opacity=0 会变成 NaN，而 NaN 的颜色在屏幕上
    // 是"随机变成某个颜色"，比不画难查得多。
    m_curXf = Ui2Xform{};
    for (size_t i = 1; i < m_xf.size(); ++i)
        m_curXf = Ui2Xform::concat(m_xf[i], m_curXf);

    m_curOp = 1.0f;
    for (size_t i = 1; i < m_op.size(); ++i) m_curOp *= m_op[i];
    if (m_curOp < 0.0f) m_curOp = 0.0f;
    if (m_curOp > 1.0f) m_curOp = 1.0f;

    m_curClip = Ui2Scissor{};
    for (size_t i = 1; i < m_clip.size(); ++i) {
        const Ui2Scissor& s = m_clip[i];
        if (!s.any || s.w <= 0.0f || s.h <= 0.0f) {
            m_curClip = Ui2Scissor{0.0f, 0.0f, 0.0f, 0.0f, true};
            continue;
        }
        if (!m_curClip.any) { m_curClip = s; continue; }
        const float x0 = std::fmax(m_curClip.x, s.x);
        const float y0 = std::fmax(m_curClip.y, s.y);
        const float x1 = std::fmin(m_curClip.x + m_curClip.w, s.x + s.w);
        const float y1 = std::fmin(m_curClip.y + m_curClip.h, s.y + s.h);
        m_curClip.x = x0; m_curClip.y = y0;
        m_curClip.w = x1 - x0; m_curClip.h = y1 - y0;
        m_curClip.any = true;
    }
}

void UiCanvas::pushTransform(const Ui2Xform& x) {
    m_xf.push_back(x);
    recomputeState();
}
void UiCanvas::popTransform() {
    if (m_xf.size() > 1) m_xf.pop_back();
    recomputeState();
}
void UiCanvas::pushOpacity(float mul) {
    m_op.push_back(mul);
    recomputeState();
}
void UiCanvas::popOpacity() {
    if (m_op.size() > 1) m_op.pop_back();
    recomputeState();
}
void UiCanvas::pushClip(float x, float y, float w, float h) {
    m_clip.push_back(Ui2Scissor{x, y, w, h, true});
    recomputeState();
}
void UiCanvas::popClip() {
    if (m_clip.size() > 1) m_clip.pop_back();
    recomputeState();
}

uint32_t UiCanvas::drawCalls() const {
    if (m_verts.empty()) return 0u;
    // 按 scissor 切 run（相邻且相同算一个）。没裁剪时恒 1。
    uint32_t n = 0;
    Ui2Scissor prev{};
    bool first = true;
    for (const Ui2Scissor& s : m_scissors) {
        if (first || !s.same(prev)) { ++n; prev = s; first = false; }
    }
    return n == 0u ? 1u : n;
}

namespace {
// 乘透明度（只动 alpha 字节；颜色本身是 sRGB 编码值，**不是**线性值，
// 所以这里绝不能做"先反编码再乘再编码"—— 那会把半透黑变成中灰）。
inline uint32_t mulAlpha(uint32_t c, float mul) {
    const uint32_t a = (c >> 24) & 0xFFu;
    const uint32_t na = static_cast<uint32_t>(a * mul + 0.5f);
    return (c & 0x00FFFFFFu) | (na << 24);
}
}  // namespace

void UiCanvas::emitQuad(const float lx[4], const float ly[4],
                        const Ui2Vertex proto[4]) {
    if (m_curOp <= 0.0009f) return;                 // 全透明 → 一个顶点都不要

    // 本地 → 输出像素
    float px[4], py[4];
    float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
    for (int i = 0; i < 4; ++i) {
        m_curXf.mapPoint(lx[i], ly[i], px[i], py[i]);
        minX = std::fmin(minX, px[i]); maxX = std::fmax(maxX, px[i]);
        minY = std::fmin(minY, py[i]); maxY = std::fmax(maxY, py[i]);
    }

    Ui2Scissor sc{};
    if (m_curClip.any) {
        if (m_curClip.w <= 0.0f || m_curClip.h <= 0.0f) return;
        // 完全在外 → 丢掉（不进顶点缓冲，也就不占 draw）
        if (maxX <= m_curClip.x || minX >= m_curClip.x + m_curClip.w ||
            maxY <= m_curClip.y || minY >= m_curClip.y + m_curClip.h) {
            return;
        }
        // ⚠ 部分在外的 quad **不裁几何**，只记 scissor 让 GPU 裁。
        //   在 CPU 上裁会把 mode 0/3 的"中心 + 半宽"语义剪坏（圆角跑到
        //   错误的位置），而 scissor 是像素级精确的。
        sc = m_curClip;
    }

    Ui2Vertex v[4];
    const bool xformRot = !m_curXf.axisAligned();
    for (int i = 0; i < 4; ++i) {
        v[i] = proto[i];
        if (v[i].mode == static_cast<float>(Ui2Prim::Solid) ||
            v[i].mode == static_cast<float>(Ui2Prim::RoundBox)) {
            // mode 0/3 的顶点里存的是"中心 + 角点符号"，顶点坐标由着色器
            // 展开；这里只在**没有旋转**时改中心/半宽（缩放可交换），
            // 有旋转时调用方会改用 mode 4（顶点自己给坐标）。
            if (xformRot) {
                v[i].x = px[i];
                v[i].y = py[i];
                v[i].mode = static_cast<float>(Ui2Prim::RoundBoxRotated);
            } else {
                float cx, cy;
                m_curXf.mapPoint(proto[i].x, proto[i].y, cx, cy);
                v[i].x = cx;
                v[i].y = cy;
                v[i].hx = proto[i].hx * std::fabs(m_curXf.m00);
                v[i].hy = proto[i].hy * std::fabs(m_curXf.m11);
                v[i].radius = proto[i].radius *
                              (std::fabs(m_curXf.m00) + std::fabs(m_curXf.m11)) *
                              0.5f;
            }
        } else {
            // mode 1/2：pos 本来就是顶点
            v[i].x = px[i];
            v[i].y = py[i];
            // 平滑宽度随缩放走，否则放大后的字边缘会变硬（SDF 的前提是
            // "过渡带在屏幕空间里恒为 ~1px"，缩放后它得跟着缩）
            if (v[i].mode == static_cast<float>(Ui2Prim::SdfText) &&
                m_curXf.axisAligned()) {
                const float sx = std::fabs(m_curXf.m00);
                const float sy = std::fabs(m_curXf.m11);
                v[i].hx *= 0.5f * (sx + sy);
            }
        }
        v[i].color = mulAlpha(proto[i].color, m_curOp);
    }

    // 两个三角形：(0,1,2) + (0,2,3)
    const int idx[6] = {0, 1, 2, 0, 2, 3};
    for (int k = 0; k < 6; ++k) m_verts.push_back(v[idx[k]]);
    m_scissors.push_back(sc);
    m_draws = 1;
}

void UiCanvas::pushModeQuad(float cx, float cy, float hx, float hy,
                            float radius, Ui2Prim mode, uint32_t color) {
    // 角点符号（与 ui2.vert 的约定：inUv 是 ±1，inPos 是中心）
    const float sx[4] = {-1.0f, 1.0f, 1.0f, -1.0f};
    const float sy[4] = {-1.0f, -1.0f, 1.0f, 1.0f};

    Ui2Vertex proto[4];
    float lx[4], ly[4];
    for (int i = 0; i < 4; ++i) {
        proto[i].x = cx;
        proto[i].y = cy;
        proto[i].u = sx[i];
        proto[i].v = sy[i];
        proto[i].color = color;
        proto[i].hx = hx;
        proto[i].hy = hy;
        proto[i].radius = radius;
        proto[i].mode = static_cast<float>(mode);
        lx[i] = cx + sx[i] * hx;
        ly[i] = cy + sy[i] * hy;
    }
    emitQuad(lx, ly, proto);
}

void UiCanvas::rect(float x, float y, float w, float h, uint32_t color) {
    if (w <= 0.0f || h <= 0.0f) return;   // 空矩形不产生顶点（否则会画出一个点）
    pushModeQuad(x + w * 0.5f, y + h * 0.5f, w * 0.5f, h * 0.5f, 0.0f,
                 Ui2Prim::Solid, color);
}

void UiCanvas::roundRect(float x, float y, float w, float h, float radius,
                         uint32_t color) {
    if (w <= 0.0f || h <= 0.0f) return;
    // 半径大于半宽/半高时会被 shader 里的 clamp 收住，但这里先夹一次：
    // 让"顶点数据本身"就合法，无窗口自检断言 hx/hy/radius 关系时才有意义。
    const float maxR = 0.5f * (w < h ? w : h);
    if (radius > maxR) radius = maxR;
    pushModeQuad(x + w * 0.5f, y + h * 0.5f, w * 0.5f, h * 0.5f, radius,
                 radius > 0.0f ? Ui2Prim::RoundBox : Ui2Prim::Solid, color);
}

void UiCanvas::image(float x, float y, float w, float h,
                     float u0, float v0, float u1, float v1, uint32_t tint) {
    if (w <= 0.0f || h <= 0.0f) return;
    // 贴图是"顶点 = 实际位置"的路径（mode 1），所以四个角直接展开。
    Ui2Vertex proto[4];
    const float lx[4] = {x, x + w, x + w, x};
    const float ly[4] = {y, y, y + h, y + h};
    const float uu[4] = {u0, u1, u1, u0};
    const float vv[4] = {v0, v0, v1, v1};
    for (int i = 0; i < 4; ++i) {
        proto[i].x = lx[i];
        proto[i].y = ly[i];
        proto[i].u = uu[i];
        proto[i].v = vv[i];
        proto[i].color = tint;
        proto[i].hx = 0.0f;
        proto[i].hy = 0.0f;
        proto[i].radius = 0.0f;
        proto[i].mode = static_cast<float>(Ui2Prim::Texture);
    }
    emitQuad(lx, ly, proto);
}

void UiCanvas::quad(const Ui2Vertex& v0, const Ui2Vertex& v1,
                    const Ui2Vertex& v2, const Ui2Vertex& v3) {
    // ⚠ 这个入口是"调用方已经算好顶点"的逃生口（自检与调试图元用）。
    //   它**不走**变换栈 —— 顶点已经是输出像素了。要变换请用 rect /
    //   roundRect / image / text，那四个走 `emitQuad`。
    m_verts.push_back(v0);
    m_verts.push_back(v1);
    m_verts.push_back(v2);
    m_verts.push_back(v0);
    m_verts.push_back(v2);
    m_verts.push_back(v3);
    m_scissors.push_back(Ui2Scissor{});
    m_draws = 1;
}

// ---------------------------------------------------------------- 文本

void UiCanvas::text(const SdfFont& font, float x, float baselineY,
                    const char* utf8, uint32_t color, float sizePx) {
    if (!utf8 || !font.valid() || font.size <= 0.0f) return;
    if (sizePx <= 0.0f) return;

    // 缩放 = 屏幕字号 / 图集字号。整条 SDF 链路只有一个可变量，就是它。
    const float scale = sizePx / font.size;

    // 平滑宽度：图集每像素对应 SDF 值 1/(2*spread)；屏幕上 1 像素 = 1/scale
    // 个图集像素 → 过渡带半宽 ≈ 0.5 屏幕像素 = 0.5/(2*spread*scale)。
    // clamp 下界防止 spread=0 时除零，上界 0.5 让 smoothstep 不至于反过来。
    const float smooth = (font.spread > 0.0f)
        ? 0.5f / (2.0f * font.spread * scale)
        : 0.05f;
    const float w = std::fmax(1e-3f, std::fmin(0.5f, smooth));

    const float lineH = font.lineHeight() * scale;
    float penX = x;
    float penY = baselineY;

    const char* p = utf8;
    const char* end = utf8 + std::strlen(utf8);
    while (p < end) {
        const uint32_t cp = utf8Next(p, end);
        if (cp == 0) break;
        if (cp == '\n') {
            penX = x;
            penY += lineH;
            continue;
        }
        const SdfGlyph* g = font.findOrFallback(cp);
        if (!g) continue;   // 连 '?' 都没有 → 图集是坏的，保持沉默

        // 格子左上角 = 笔位 + 格子左/上偏移（bearX/bearY 都是负数）
        const float x0 = penX + g->bearX * scale;
        const float y0 = penY + g->bearY * scale;
        const float x1 = x0 + g->w * scale;
        const float y1 = y0 + g->h * scale;

        Ui2Vertex proto[4];
        const float lx[4] = {x0, x1, x1, x0};
        const float ly[4] = {y0, y0, y1, y1};
        const float uu[4] = {g->u0, g->u1, g->u1, g->u0};
        const float vv[4] = {g->v0, g->v0, g->v1, g->v1};
        for (int i = 0; i < 4; ++i) {
            proto[i].x = lx[i];
            proto[i].y = ly[i];
            proto[i].u = uu[i];
            proto[i].v = vv[i];
            proto[i].color = color;
            proto[i].hx = w;        // mode 2：hx = 平滑宽度
            proto[i].hy = 0.0f;
            proto[i].radius = 0.0f;
            proto[i].mode = static_cast<float>(Ui2Prim::SdfText);
        }
        emitQuad(lx, ly, proto);

        // 推进按**真实 adv**，不是格子宽度（格子左右各含 spread+2 的边，
        // 用它推进会让 48 号字每字宽多 16px，整句字距散开）。
        penX += g->adv * scale;
    }
}

float UiCanvas::measure(const SdfFont& font, const char* utf8,
                        float sizePx) const {
    if (!font.valid() || font.size <= 0.0f || sizePx <= 0.0f) return 0.0f;
    return measureUtf8(font, utf8, sizePx / font.size);
}

// ---------------------------------------------------------------- 调试图元

DebugRect& debugRect() {
    static DebugRect r;
    return r;
}

DebugText& debugText() {
    static DebugText t;
    return t;
}

namespace {
const SdfFont* g_frameFont = nullptr;
uint32_t g_extW = 0;
uint32_t g_extH = 0;
}
void setFrameFont(const SdfFont* font) { g_frameFont = font; }
const SdfFont* frameFont() { return g_frameFont; }
void setFrameExtent(uint32_t w, uint32_t h) { g_extW = w; g_extH = h; }
void frameExtent(uint32_t& w, uint32_t& h) { w = g_extW; h = g_extH; }

namespace {
// 游戏 UI 的绘制回调（由 runtime 注入，见 UiCanvas.h）。
GameUiBuildFn g_gameUi;
// 这一帧有没有游戏 UI 要画。与 `g_gameUi` 分开：回调存在 ≠ 这一帧有内容
// （HUD 可以在暂停时继续画，也可以被 ui.hudInEditor 关掉）。
bool g_gameUiActive = false;
}  // namespace

void setGameUiBuilder(GameUiBuildFn fn) { g_gameUi = std::move(fn); }
void setGameUiActive(bool active) { g_gameUiActive = active; }
bool gameUiActive() { return g_gameUiActive; }

bool hasUiContent() {
    // ⚠ 放在**一个函数**里是有意的：它同时决定"要不要建 gameUi 回调"、
    //   "要不要进输出 pass"。散在多处一定会漏一条（漏的那条会让 HUD
    //   偶尔不画，而且只在某些状态下复现）。
    if (gameUiActive()) return true;
    if (debugRect().enabled) return true;
    if (debugText().enabled && !debugText().utf8.empty()) return true;
    if (debugButton().enabled) return true;
    return false;
}

void buildOverlay(UiCanvas& canvas, uint32_t outW, uint32_t outH) {
    // 帧上下文：命中测试 / 控件布局都从这里取输出尺寸。
    setFrameExtent(outW, outH);

    // ---- 游戏 UI（U8）：HUD → 暂停菜单 ----
    // ⚠ 画在**调试图元之前**：debugRect / debugText 是排查用的"临时眼睛"，
    //   被 HUD 盖住就没意义了。HUD 内部再分先后（伤害数字在血条之上、
    //   暂停菜单在最上），那是 runtime 的事。
    if (g_gameUi && g_gameUiActive) g_gameUi(canvas, outW, outH);

    const DebugRect& r = debugRect();
    if (r.enabled) canvas.rect(r.x, r.y, r.w, r.h, r.color);

    // 顺序：底板 → 按钮 → 文本。按钮自带文字，所以文本放在最后画
    // （U8 的伤害数字要盖在所有东西上面）。
    DebugButton& dbg = debugButton();
    if (dbg.enabled) {
        dbg.button.held = dbg.pressFrames > 0;
        if (dbg.pressFrames > 0) --dbg.pressFrames;
        drawButton(canvas, dbg.button, outW, outH, frameFont());
    }
    // 待处理点击在这里落地（布局刚算完，此刻的命中判定才是真的）
    processPendingClick(outW, outH);

    const DebugText& t = debugText();
    if (t.enabled && !t.utf8.empty()) {
        // 字体没加载时这里什么都不画（**不画方块**）——"数据缺失"和
        // "字形缺失"是两件事，混在像素里就分不清是加载失败还是编码错了。
        if (const SdfFont* f = frameFont()) {
            canvas.text(*f, t.x, t.baselineY, t.utf8.c_str(), t.color, t.sizePx);
        }
    }
}

}  // namespace ui2
