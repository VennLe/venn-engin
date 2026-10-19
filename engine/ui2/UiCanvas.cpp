#include "ui2/UiCanvas.h"

#include "ui2/SdfFont.h"
#include "ui2/UiWidget.h"

#include <cmath>
#include <cstring>

namespace ui2 {

void UiCanvas::pushModeQuad(float cx, float cy, float hx, float hy,
                            float radius, Ui2Prim mode, uint32_t color) {
    // 角点符号（与 ui2.vert 的约定：inUv 是 ±1，inPos 是中心）
    const float sx[4] = {-1.0f, 1.0f, 1.0f, -1.0f};
    const float sy[4] = {-1.0f, -1.0f, 1.0f, 1.0f};

    Ui2Vertex v[4];
    for (int i = 0; i < 4; ++i) {
        v[i].x = cx;
        v[i].y = cy;
        v[i].u = sx[i];
        v[i].v = sy[i];
        v[i].color = color;
        v[i].hx = hx;
        v[i].hy = hy;
        v[i].radius = radius;
        v[i].mode = static_cast<float>(mode);
    }
    // 两个三角形：(0,1,2) + (0,2,3)，三角形列表拓扑
    quad(v[0], v[1], v[2], v[3]);
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
    Ui2Vertex v[4];
    const float px[4] = {x, x + w, x + w, x};
    const float py[4] = {y, y, y + h, y + h};
    const float uu[4] = {u0, u1, u1, u0};
    const float vv[4] = {v0, v0, v1, v1};
    for (int i = 0; i < 4; ++i) {
        v[i].x = px[i];
        v[i].y = py[i];
        v[i].u = uu[i];
        v[i].v = vv[i];
        v[i].color = tint;
        v[i].hx = 0.0f;
        v[i].hy = 0.0f;
        v[i].radius = 0.0f;
        v[i].mode = static_cast<float>(Ui2Prim::Texture);
    }
    quad(v[0], v[1], v[2], v[3]);
}

void UiCanvas::quad(const Ui2Vertex& v0, const Ui2Vertex& v1,
                    const Ui2Vertex& v2, const Ui2Vertex& v3) {
    m_verts.push_back(v0);
    m_verts.push_back(v1);
    m_verts.push_back(v2);
    m_verts.push_back(v0);
    m_verts.push_back(v2);
    m_verts.push_back(v3);
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

        Ui2Vertex v[4];
        const float px[4] = {x0, x1, x1, x0};
        const float py[4] = {y0, y0, y1, y1};
        const float uu[4] = {g->u0, g->u1, g->u1, g->u0};
        const float vv[4] = {g->v0, g->v0, g->v1, g->v1};
        for (int i = 0; i < 4; ++i) {
            v[i].x = px[i];
            v[i].y = py[i];
            v[i].u = uu[i];
            v[i].v = vv[i];
            v[i].color = color;
            v[i].hx = w;        // mode 2：hx = 平滑宽度
            v[i].hy = 0.0f;
            v[i].radius = 0.0f;
            v[i].mode = static_cast<float>(Ui2Prim::SdfText);
        }
        quad(v[0], v[1], v[2], v[3]);

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
