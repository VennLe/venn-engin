#pragma once
// ============================================================
// ui2/UiCanvas —— 游戏 UI 的"绘制命令 → 顶点"积累器（U2 起）
//
// 职责边界（这条是整套 ui2 的分层核心）：
//   · UiCanvas **不认识** Vulkan、不认识场景、不认识控件树；
//   · 它只把"画一个矩形 / 一段文字 / 一张图"翻译成 32~36 字节的顶点；
//   · UiRenderer 负责上传 + 绑定 + 一次 draw。
// 于是"UI 长什么样"可以在**无窗口**的自检里跑（顶点数 / 位置 / 颜色都能
// 直接断言），不需要 GPU —— 这是本项目一贯的做法（数值先可测，像素后验收）。
//
// 坐标：**输出像素**，y 向下。逻辑像素 → 输出像素的缩放由调用方乘好
// （布局层 U4 负责；UiCanvas 不自作主张乘 scale，否则"调试图元"与
// "布局出来的控件"会走两条不同的缩放路径，迟早对不上）。
// ============================================================

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ui2 {

// 字体数据（纯 CPU，见 ui2/SdfFont.h）。这里只前置声明 —— UiCanvas 的
// 公开接口不该把整个字体头拖进来，更重要的是**不要把 rhi 拖进来**。
struct SdfFont;

// 顶点（36 字节，4 字节对齐）。字段语义随 mode 变：
//   mode 0 纯色矩形      pos=中心   uv=角点符号∈{-1,1}  half=半宽/半高
//   mode 1 贴图          pos=顶点   uv=图集 uv          half 忽略
//   mode 2 SDF 字形      pos=顶点   uv=图集 uv          half.x=平滑宽度
//   mode 3 SDF 圆角矩形  pos=中心   uv=角点符号          half=半宽/半高 radius=半径
//
// ⚠ 36 字节而不是 32：`radius` 单独一个 float 比"把半径塞进 uv.x、再在
//   着色器里按 mode 分支解包"清楚得多，而 UI 的顶点量级是几千，多 4 字节
//   完全不是瓶颈。（路线图里写的 32 是估的，实现时以可读性为准。）
struct Ui2Vertex {
    float x = 0.0f, y = 0.0f;    // 像素坐标
    float u = 0.0f, v = 0.0f;    // 图集 uv / 角点符号
    uint32_t color = 0xFFFFFFFFu;  // RGBA8（**sRGB 编码值**，shader 里反编码）
    float hx = 0.0f, hy = 0.0f;  // 半宽/半高 或 (平滑宽度, 0)
    float radius = 0.0f;         // 圆角半径（像素）
    float mode = 0.0f;           // 图元种类
};

static_assert(sizeof(Ui2Vertex) == 36,
              "ui2 顶点布局必须与 assets/shaders/ui2.vert 的 input 逐字段对齐");

// 图元种类（与 ui2.frag 的分支一一对应）。
// ⚠ 底层类型必须是整数：C++ 不允许 `enum class X : float`（值直接写进
//   顶点时再 static_cast<float>）。
enum class Ui2Prim : int {
    Solid = 0,
    Texture = 1,
    SdfText = 2,
    RoundBox = 3,
};

// RGBA8 打包成 `uint32_t`。
//
// ⚠⚠ 字节序陷阱（2026-10-18 踩过一次）：顶点属性是
//    `VK_FORMAT_R8G8B8A8_UNORM`，它按**字节顺序**读 —— R 在最**低**地址。
//    小端机上 `uint32_t` 的最低地址是**最低有效字节**，所以内存里要写成
//    `R | G<<8 | B<<16 | A<<24`（即数值上的 0xAABBGGRR），
//    而**不是**直觉上的 0xRRGGBBAA。
//    写反的症状：`#808080` 变成"R=0xFF, G=0x80, B=0x80, A=0x80"——红中透底、
//    半透明（那一次的截图就是一整屏鲑鱼粉）。
constexpr uint32_t rgba(int r, int g, int b, int a = 255) {
    return (static_cast<uint32_t>(r & 0xFF)) |
           (static_cast<uint32_t>(g & 0xFF) << 8) |
           (static_cast<uint32_t>(b & 0xFF) << 16) |
           (static_cast<uint32_t>(a & 0xFF) << 24);
}
// 0xRRGGBB + alpha → 打包值（与上面同一个字节序约定）
constexpr uint32_t rgbA(uint32_t rrggbb, int a = 255) {
    return rgba(static_cast<int>((rrggbb >> 16) & 0xFF),
                static_cast<int>((rrggbb >> 8) & 0xFF),
                static_cast<int>(rrggbb & 0xFF), a);
}

class UiCanvas {
public:
    void begin() { m_verts.clear(); m_draws = 0; }

    // 纯色矩形（左上角 + 宽高）。矩形永远 4 个顶点、2 个三角形。
    void rect(float x, float y, float w, float h, uint32_t color);

    // 圆角矩形。radius <= 0 自动退化到 rect（走同一条 mode 3 路径也行，
    // 但纯色矩形少一次距离场计算，而且"圆角 0"在语义上就是矩形）。
    void roundRect(float x, float y, float w, float h, float radius,
                   uint32_t color);

    // 贴图四边形：像素矩形 + uv 矩形（图集坐标 0..1）。
    void image(float x, float y, float w, float h,
               float u0, float v0, float u1, float v1, uint32_t tint);

    // ---- 文本（U1/U3）----
    // 在像素坐标 (x, baselineY) 起笔，逐字生成 SDF 字形 quad。
    //   sizePx：屏幕上的字号（图集里是 font.size，缩放 = sizePx / font.size，
    //           SDF 的锐利正是靠"任意字号只改这个比值"）
    // 支持 '\n' 换行（基线按 lineHeight 下移，笔位回到起始 x）。
    // `font` 无效（图集没加载）时什么都不画 —— 不画方块，因为它只是数据缺失。
    void text(const SdfFont& font, float x, float baselineY, const char* utf8,
              uint32_t color, float sizePx);

    // 单行宽度（像素）。布局层用它做右对齐/居中。
    float measure(const SdfFont& font, const char* utf8, float sizePx) const;

    // 再加 4 个顶点（内部用；text 在 U3 会按字形逐 quad 调用它）。
    void quad(const Ui2Vertex& v0, const Ui2Vertex& v1,
              const Ui2Vertex& v2, const Ui2Vertex& v3);

    const std::vector<Ui2Vertex>& vertices() const { return m_verts; }
    // 合批后的 draw call 数：单图集、无 scissor 变化 → 永远 1（空画布 = 0）。
    uint32_t drawCalls() const { return m_verts.empty() ? 0u : 1u; }
    bool empty() const { return m_verts.empty(); }

private:
    void pushModeQuad(float cx, float cy, float hx, float hy, float radius,
                      Ui2Prim mode, uint32_t color);

    std::vector<Ui2Vertex> m_verts;
    uint32_t m_draws = 0;
};

// ---- 调试图元（U2 的 "Hello World"，也是控制台 `ui.rect` 的存储）----
// 每帧重画一次，直到被显式清掉 —— 这样验收脚本可以"设一次、连拍多张"，
// 不必每帧重新注入。
struct DebugRect {
    bool enabled = false;
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
    uint32_t color = rgba(128, 128, 128, 255);
};
DebugRect& debugRect();

// ---- 调试文本（U1 的 "Hello World"，也是控制台 `ui.text` 的存储）----
// 与 debugRect 同样是"设一次、每帧重画"，方便验收脚本连拍。
struct DebugText {
    bool enabled = false;
    float x = 0.0f;
    float baselineY = 0.0f;
    float sizePx = 32.0f;
    uint32_t color = rgba(255, 255, 255, 255);
    std::string utf8;
};
DebugText& debugText();

// 本帧可用字体（UiRenderer 在灌画布之前设好；没加载图集时为 nullptr）。
// 走"帧上下文"而不是给 buildOverlay 加参数：buildOverlay 的签名已经
// 被自检与调试图元两处使用，加参数会迫使所有调用点都认识字体。
void setFrameFont(const SdfFont* font);
const SdfFont* frameFont();

// 本帧的输出尺寸（buildOverlay 记录）。命中测试要拿它把"参考像素"换算成
// 屏幕上真实的矩形；走帧上下文，免得每个控件调用点都多两个参数。
void setFrameExtent(uint32_t w, uint32_t h);
void frameExtent(uint32_t& w, uint32_t& h);

// 屏幕上"现在有没有 UI 要画"。false 时整条通道不进任何 pass
// （= 与没有这条通道时**逐位**相同，这是默认关的守卫）。
bool hasUiContent();

// ---- 游戏 UI 的绘制回调（U8）----
//
// 谁来画"血条 / 准星 / 伤害数字 / 暂停菜单"？是 **runtime**，不是 engine
// —— 而 `runtime → engine ← editor` 是单向的，engine 不能反过来认识
// runtime。所以这里开一个注入口：runtime 在启动时把自己的 builder 挂上。
//
// `setGameUiActive` 是**必需的第二半**：`hasUiContent()` 在"决定要不要进
// 这个 pass"时就被调用，那时还没有画布，没法"试画一下看看空不空"。
// 宿主每帧明确告诉它"这帧有没有东西要画"（HUD 一直有 → true；
// 只有菜单时 → 菜单开着为 true）。
using GameUiBuildFn = std::function<void(UiCanvas&, uint32_t, uint32_t)>;
void setGameUiBuilder(GameUiBuildFn fn);
void setGameUiActive(bool active);
bool gameUiActive();

// 把"这一帧要画的东西"灌进画布（调试矩形；U8 起还有 HUD 与暂停菜单）。
void buildOverlay(UiCanvas& canvas, uint32_t outW, uint32_t outH);

}  // namespace ui2
