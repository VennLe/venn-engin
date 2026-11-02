#pragma once
// ============================================================
// gameplay/DamageNumbers —— 伤害数字层（P3-4）
//
// 一句话：**一个独立的、纯数据的飘字层**。它不属于 `UiDocument`。
//
// ------------------------------------------------------------
// 为什么必须独立成一层（这是路线图 §4.5 的原话，这里是实现处的对应）
// ------------------------------------------------------------
// `ui2` 的文档布局是"锚点 + 树 + 每一帧重算"。飘字要的是相反的语义：
//   · 位置**每帧变**（世界点投影 + 上浮），而文档的 rect 是作者写死的；
//   · 位置来自**世界坐标**，文档树里根本没有这个概念；
//   · 会被父节点的 `pushClip` 裁掉 —— 一条"敌人头顶"的数字恰好落在
//     HUD 面板的裁剪区里就整条消失，而它跟那个面板毫无关系。
// 所以飘字走**画布直绘**（U8 的 game-UI builder 就是一块裸 `UiCanvas`），
// 一棵树都不经过。
//
// ------------------------------------------------------------
// 为什么"数据层"要单独存在（而不是直接在绘制处算）
// ------------------------------------------------------------
// 合并 / 寿命 / 上浮曲线 / 确定性漂移全是纯数值，能毫秒级断言；
// 绘制要 `UiCanvas`，而它只在有帧的时候存在。这条分层与
// `runtime/ui/HudModel`（数据）↔ `runtime/ui/Hud`（几何）是同一条思路。
//
// ------------------------------------------------------------
// 四条设计决定
// ------------------------------------------------------------
// 1) **合并（merge）是默认行为**。同一目标在半秒内被打三下，弹出三个
//    "37" 只会互相盖住；弹一个 "37 x3" 才读得出"连击"。窗口与半径都
//    是常量（`kMergeWindow` / `kMergeRadius`），不开放配置 —— 配置出来的
//    窗口一旦大于寿命，屏幕上就永远只有一个数字，那时候没人会想到是配置。
//
// 2) **漂移必须确定**。同一个 `serial` 永远得到同一个偏移（整数散列，
//    不碰 `rand()`）。理由不是洁癖：`verify_*` 要连拍两帧比对像素，
//    随机漂移会让"两张图不一样"变成常态，像素判据就废了。
//
// 3) **`t` 是归一化年龄，不是秒**。`0..1` 让"寿命"只影响 `tick` 的步长，
//    绘制侧所有曲线（上浮 / 缩放 / 淡出）都不必再除以寿命 —— 否则每个
//    用到它的地方都多一个"除以 life"的机会，写错一次就是"数字活着不动"。
//
// 4) **池满时替换最老的那条**（与 `HudModel` 一致）。挤掉最新的会让
//    "刚打中的那一击"看不见，正好反了。
// ============================================================

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gameplay {

// ---- 飘字种类 ----
//
// 只写**语义**，不写颜色：颜色/字号/寿命都在 `popupStyle()` 那张表里。
// 加一种新种类 = 这里加一行 + 表里加一行（两处，而且顺序必须一致 ——
// 表的索引就是枚举值）。
enum class PopupKind : int {
    Damage = 0,   // 普通伤害：白/橙，中等字号
    Crit = 1,     // 暴击：更大、更亮、上浮更高
    Heal = 2,     // 治疗：绿，"+" 前缀
    Shield = 3,   // 护盾：蓝，"[]" 包起来（和伤害区分得开又不依赖图标）
    Buff = 4,     // 状态/增益：紫，小字号
    Miss = 5,     // 未命中 / 无效果：灰，固定文本 "MISS"
    Count = 6,
};

// 一种飘字的**表现模板**。所有字段都是"参考像素 / 秒"，绘制侧乘 `uiScale`。
struct PopupStyle {
    uint32_t color = 0u;        // sRGB 字节序，与 `ui2::rgba` 一致
    float size = 26.0f;         // 字号（参考像素）
    float life = 1.0f;          // 寿命（秒）
    float rise = 56.0f;         // 上浮距离（参考像素）
    float popScale = 1.30f;     // 出生瞬间的放大倍数（1 = 不弹）
    float popTime = 0.18f;      // 从 popScale 收到 1 需要的时间（占寿命的比例）
    float hold = 0.60f;         // 之后才开始淡出（占寿命的比例）
    float driftRef = 14.0f;     // 水平漂移幅度（参考像素，确定性）
    bool outline = true;        // 是否画描边（压在亮背景上也能读）
    const char* prefix = "";    // 数字前面的符号（"+"）
    const char* suffix = "";    // 数字后面的符号（""）
    const char* fixed = nullptr;  // 非空 = 不显示数字，显示这段固定文本
};

// 模板表。`k` 越界 → 返回 Damage（越界不抛、不留空，否则一处枚举笔误
// 就是"这个数字整条不画"，而它看起来像"伤害没算出来"）。
const PopupStyle& popupStyle(PopupKind k);

// 世界 → 屏幕（输出像素，y 向下）。
//
// ⚠ **这是全仓唯一一份**。`runtime::Hud::worldToScreen` 直接转调它 ——
//   投影里 Y 只翻一次（`Camera::projMatrix` 已经翻过），两条各写一份必然
//   有一条多翻一次，症状是"数字上下镜像但还在屏幕附近"，最难发现。
// 返回 false = 相机背后 / 屏幕外（调用方**不画**，不要贴边）。
bool projectToScreen(const glm::mat4& viewProj, const glm::vec3& world,
                     uint32_t outW, uint32_t outH, glm::vec2* out);

// 一次"要弹字"的请求。
struct PopupSpawn {
    glm::vec3 worldPos{0.0f};
    float amount = 0.0f;
    PopupKind kind = PopupKind::Damage;
    // 是否允许和已有条目合并（同点同类、且在 `kMergeWindow` 之内）。
    // ⚠ 默认 true：卡牌/回合制里"同一目标连续挨打"是常态，不合并就互相盖住。
    //   调试/自检需要"每次都单独一条"时显式给 false。
    bool merge = true;
};

// 屏幕上的一条飘字（池里的一格）。
struct DamageNumber {
    glm::vec3 worldPos{0.0f};
    float amount = 0.0f;
    PopupKind kind = PopupKind::Damage;
    float t = 0.0f;          // 归一化年龄 0..1
    int combo = 1;           // 合并了几次
    float driftX = 0.0f;     // 参考像素（确定性，见头注释第 2 条）
    float driftY = 0.0f;
    uint32_t serial = 0;     // 出生序号（漂移的散列输入，也是唯一标识）

    const PopupStyle& style() const { return popupStyle(kind); }
    float life() const { return style().life; }
};

// 一个飘字"现在长什么样"（绘制侧只认这个结构，不自己算曲线）。
struct PopupLayout {
    float centerX = 0.0f;    // 文字**中心**的 x。绘制侧减 `measure/2`
    float baselineY = 0.0f;
    float sizePx = 0.0f;     // 已经乘过 uiScale
    float alpha = 0.0f;      // 0..1
    uint32_t color = 0u;     // 已经乘过 alpha（直接给画布）
    bool outline = false;
    std::string text;        // 已经格式化好（含 x3 / 前缀 / MISS）
};

class DamageNumberLayer {
public:
    // 同屏上限。一片 AoE 打中二十个敌人时，上百个数字既看不清也没有信息量。
    static constexpr std::size_t kMax = 32;
    // 合并窗口（秒）：`t * life` 之差小于它、且同点同类 → 合并。
    static constexpr float kMergeWindow = 0.35f;
    // 合并半径（米）：世界坐标的距离小于它才算"同一个目标"。
    // ⚠ 用世界距离而不是"实体 id"：这一层不认识 `ecs::Entity`（gameplay
    //   层不依赖 ecs），而"两个敌人的头顶只差 0.9 米"本来就该合并。
    static constexpr float kMergeRadius = 1.0f;

    void clear();

    // 弹一条。返回 true = 新建了一条；false = 合并进了已有那条（或池满被丢）。
    bool spawn(const PopupSpawn& s);

    // 推进一帧（秒）。`dt <= 0` 直接返回（暂停时数字不该偷偷变老）。
    void tick(float dt);

    // 世界 → 屏幕布局。背后 / 屏幕外的条目被**跳过**（不是贴边）。
    // `uiScale` 参考像素 → 输出像素（`ui2::computeScale`）。
    void layout(const glm::mat4& viewProj, uint32_t outW, uint32_t outH,
                float uiScale, std::vector<PopupLayout>& out) const;

    std::size_t size() const { return m_items.size(); }
    bool empty() const { return m_items.empty(); }
    const std::vector<DamageNumber>& items() const { return m_items; }

    // 观测值（日志 / 判据）：合并了多少次、因池满丢了多少条。
    uint32_t mergedCount() const { return m_merged; }
    uint32_t droppedCount() const { return m_dropped; }

    // 格式化（`123` / `+45` / `[8]` / `MISS` / `37 x3`）。公开给自检用 ——
    // 判据要看**文本**，而不是"有个字符串"。
    static std::string formatText(const DamageNumber& n);

private:
    std::vector<DamageNumber> m_items;
    uint32_t m_serial = 0;
    uint32_t m_merged = 0;
    uint32_t m_dropped = 0;
};

} // namespace gameplay
