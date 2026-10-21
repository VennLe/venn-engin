#include "ParticlesPanel.h"

#include "Command.h"
#include "DebugRects.h"
#include "PanelDock.h"
#include "Localization.h"
#include "PanelRegistry.h"

#include "core/Logger.h"

#include "ecs/Components.h"
#include "render/FeatureRegistry.h"
#include "render/Renderer.h"
#include "scene/Scene.h"

#include <imgui.h>

#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace editor {

namespace {

constexpr float kLabelW = 140.0f;

// 预设：六个"开箱即用"的粒子。
//
// 数值不是随手填的 —— 每一个都能在 Unity / Blender 的对应模板里找到出处：
//   火焰 = 向上浮力 + 短寿命 + 加色；烟 = 长寿命 + 慢速 + alpha；
//   火花 = 高速 + 短寿命 + 沿速度拉伸。
//
// 2026-10-17 第四轮新增雨 / 雪 / 闪电（用户原话："粒子特效的类型，可以再多
// 加一些嘛，比如从上往下落的，类似，下雪，下雨，闪电之类的，并一一实现其
// 效果"）。这三种都靠**同一个机制**做到"从上往下落"：
//
//   ⚠ 发射方向永远是**局部 +Z 锥**（见 particle_sim.comp 的 fxSampleDir）。
//     想让它朝下，就把发射器整体绕 X 转 180°（`flipDown`）—— 世界空间下
//     `em.world` 会把这个轴翻过来。不要去改 startSpeed 的符号：那是标量
//     速度，负值只会让 fxMix 里的方向计算变成"反向但仍是锥轴"，且与
//     speedVariance 相乘时符号会抖。
// 一次"雷击"其实不是一发，而是 2~4 次**回击**（return stroke）挤在几十~
// 几百毫秒里：先一道极亮的主放电，随后两三次稍弱的重复放电。只发一发的话
// 看起来像"闪光灯闪了一下"，不像闪电。所以预设表额外带一张"补发"表。
struct FxBurst {
    float time;   // 本轮 cycle 内的时刻（秒）
    int count;    // 这一发多少颗（★ 上限 = 每发射器槽位数 512）
};

// 分叉（2026-10-18 为闪电加）。
//
// ⚠ 为什么非要"多发射器"：粒子的出生体是一个**轴对齐的长方体**，速度只有
//   "锥 + 恒定加速度"，两者都不带"x 与 z 相关"的自由度 → **单个发射器在
//   数学上只能画出直线或矩形**：箱子窄 = 一根直光柱，箱子宽 = 一片光幕。
//   实机截图已经证实（0.9 m 的柱子 / 6 cm 的光柱），再怎么调参数也长不出
//   枝杈。所以分叉只能靠**另外几个更短更暗的发射器**摆在旁边。
//   它们共用同一张 burst 表、同一个 duration → 永远"同一瞬间一起亮"。
struct FxFork {
    glm::vec3 offset;    // 相对主通道的世界偏移（米；z 是相对 originZ）
    // 发射轴倾斜（度）。x → 让轴朝 -X 偏，y → 朝 +Y 偏。
    // ⚠ 这是"枝杈"的关键：**整根轴（出生盒子 + 速度锥）一起斜**，于是
    //   支路本身就是一条**斜线**。只靠 orbital 不行 —— orbital 是
    //   `v += ω × (p - origin)`，柱顶和柱底反向加速，得到的是"上下拧"的
    //   扇面，不是一条斜线（实机截图：三条平行光柱）。
    glm::vec2 tilt;
    float halfZ;         // 支路半高（米）
    float size;          // startSize（米，光丝粗细）
    float emissive;      // 亮度倍数（比主通道暗）
    float countScale;    // burst 颗数倍率（0..1，支路不需要主通道那么密）
    glm::vec3 orbital;   // 额外的剪切（让支路带一点弧度，0 = 纯斜线）
};

struct FxPreset {
    const char* label;
    float rate;
    float lifetime;
    float lifetimeVar;
    float startSpeed;
    float spread;
    float drag;
    glm::vec3 gravity;
    float startSize;
    float emissive;
    ecs::ParticleBlend blend;
    ecs::ParticleRenderMode render;
    float stretchScale;
    // ---- 雨 / 雪 / 闪电这一批要用的 ----
    bool flipDown;                       // 发射器绕 X 转 180°（往下落）
    ecs::ParticleShape shape;
    glm::vec3 shapeExtents;              // Box 的半轴
    float shapeRadius;
    bool noise;                          // 湍流（雪的飘）
    float noiseStrength;
    float noiseFreq;
    glm::vec3 colorA;                    // 渐变起点
    glm::vec3 colorB;                    // 渐变终点
    glm::vec4 alphaOverLife;
    glm::vec4 sizeOverLife;
    int burstCount;                      // >0 → 不用 rate，用循环 burst（闪电）
    float duration;                      // >0 且 looping → 每 duration 秒重来一轮
    // 出生高度（米）。火 / 烟 / 火花要贴着地面，取 0；雨 / 雪 / 闪电是
    // "从上往下"的，必须**抬起来**再落 —— 放 z=0 的话第一帧就在地面以下，
    // 直接被地面挡住，看起来就是"预设点了没效果"（实测踩过）。
    float originZ;
    // 主回击之后的补发（见 FxBurst 的注释）。nullptr = 只有一对 burst。
    const FxBurst* moreBursts;
    int moreBurstCount;
    // 轨道角速度（度/秒，绕三轴）。给闪电用：`v += ω × (p - origin)` 是
    // 一个**与高度成正比**的横向速度 —— 柱顶往一边甩、柱底往另一边甩，
    // 于是直上直下的"光柱"被剪成**斜劈的电弧**。
    // ⚠ 这是本表里**唯一能让整根柱子一起弯**的手段：噪声的种子带了
    //   `p.extra.w * 13.7`（逐粒子随机），调大了只会把柱子打散成雾，
    //   弯不出连贯形状。
    glm::vec3 orbital;
    // 分叉表（见 FxFork 的注释）。nullptr = 单通道。
    const FxFork* forks;
    int forkCount;
    // 贴图形状（见 ecs::ParticleSprite）。放最后是为了让 6 个预设条目
    // 只需要在行尾补一个值，不用动前面的字段顺序。
    ecs::ParticleSprite sprite;
};

// 主回击之后的补发（秒 / 颗数）。count 是按**槽位号**分摊的
// （shader: `float(j) < bd.y`），上限就是每发射器 512 个槽位 ——
// 所以第一发直接吃满 512，后面三发只挑低号槽位，覆盖掉一部分旧粒子，
// 于是看起来是"整根柱子重新亮一次"，而不是旧粒子上再叠一层。
const FxBurst kBoltRestrikes[] = {{0.09f, 220}, {0.17f, 384}, {0.31f, 140}};

// 闪电的两条支路（见 FxFork 的注释）。主通道 z ∈ [0.1, 3.7]，支路各自更短、
// 更细、更暗，并且**整根轴斜着** —— 合起来才是"树杈状"的电弧。
// 倾斜的符号约定（Transform 的矩阵是 Rz·Ry·Rx，flipDown 时 rx = π）：
//   rotation.y = radians(tilt.x)  → 轴朝 -X 偏（往下走时向左）
//   rotation.x = π + radians(tilt.y) → 轴朝 +Y 偏
const FxFork kBoltForks[] = {
    // 0) **辉光鞘**：和主通道同位置、同倾角，但粗 4 倍、暗 5 倍 —— 叠出来的
    //    效果是"亮芯外面裹一层蓝白辉光"。粒子只能画均质光丝，没有 shader
    //    级的柔边，这是唯一能做出"核心 + 外晕"的办法。
    {glm::vec3(0.0f, 0.0f, 0.0f), glm::vec2(3.0f, -2.0f),
     1.80f, 0.130f, 1.1f, 0.55f, glm::vec3(30.0f, 120.0f, 0.0f)},
    // 1) 右上支路：从主通道上部岔出去，往左下方汇回
    {glm::vec3(0.42f, 0.10f, 0.55f), glm::vec2(20.0f, 6.0f),
     1.15f, 0.024f, 2.4f, 0.45f, glm::vec3(0.0f, 30.0f, 0.0f)},
    // 2) 左下支路：更短更细，反向岔开
    {glm::vec3(-0.30f, -0.10f, -0.35f), glm::vec2(-26.0f, -8.0f),
     0.80f, 0.019f, 1.7f, 0.35f, glm::vec3(0.0f, -24.0f, 0.0f)},
};

const FxPreset kPresets[] = {
    // ---------------- 火 ----------------
    // 热空气往上走 → 重力取正值（Z-up，+Z 是上）
    {"Fire", 90.0f, 1.4f, 0.25f, 1.6f, 18.0f, 0.55f, glm::vec3(0.0f, 0.0f, 1.8f),
     0.34f, 3.6f, ecs::ParticleBlend::Additive,
     ecs::ParticleRenderMode::Billboard, 0.0f,
     false, ecs::ParticleShape::Cone, glm::vec3(0.3f), 0.18f,
     false, 1.0f, 0.5f,
     glm::vec3(1.00f, 0.75f, 0.25f), glm::vec3(0.25f, 0.04f, 0.02f),
     glm::vec4(0.25f, 1.0f, 0.7f, 0.0f), glm::vec4(0.35f, 1.0f, 0.75f, 0.0f),
     0, 0.0f, 0.0f, nullptr, 0, glm::vec3(0.0f), nullptr, 0, ecs::ParticleSprite::Glow},

    // ---------------- 烟 ----------------
    // 慢、久、飘，普通 alpha 混合（加色会把烟变成一团发光的雾）
    {"Smoke", 40.0f, 3.6f, 0.3f, 0.7f, 30.0f, 0.85f, glm::vec3(0.0f, 0.0f, 0.45f),
     0.95f, 0.6f, ecs::ParticleBlend::Alpha,
     ecs::ParticleRenderMode::Billboard, 0.0f,
     false, ecs::ParticleShape::Cone, glm::vec3(0.4f), 0.25f,
     false, 1.0f, 0.5f,
     glm::vec3(0.55f, 0.55f, 0.58f), glm::vec3(0.22f, 0.22f, 0.24f),
     glm::vec4(0.0f, 0.45f, 0.30f, 0.0f), glm::vec4(0.5f, 1.0f, 1.6f, 1.0f),
     0, 0.0f, 0.0f, nullptr, 0, glm::vec3(0.0f), nullptr, 0, ecs::ParticleSprite::Smoke},

    // ---------------- 火花 ----------------
    // 真重力 + 高速 + 沿速度拉丝（不拉伸的话每一颗只是个圆点）
    {"Sparks", 220.0f, 0.55f, 0.4f, 6.5f, 42.0f, 0.1f,
     glm::vec3(0.0f, 0.0f, -9.8f), 0.055f, 4.5f, ecs::ParticleBlend::Additive,
     ecs::ParticleRenderMode::StretchedBillboard, 0.12f,
     false, ecs::ParticleShape::Point, glm::vec3(0.1f), 0.05f,
     false, 1.0f, 0.5f,
     glm::vec3(1.0f, 0.85f, 0.45f), glm::vec3(1.0f, 0.35f, 0.05f),
     glm::vec4(1.0f, 1.0f, 0.6f, 0.0f), glm::vec4(1.0f, 0.8f, 0.4f, 0.0f),
     0, 0.0f, 0.0f, nullptr, 0, glm::vec3(0.0f), nullptr, 0, ecs::ParticleSprite::Spark},

    // ---------------- 雨 ----------------
    // 一片 12×12m 的"雨云"（Box 半轴 6/6/0.2），朝下的窄锥 + 强初速 + 真重力。
    // 拉伸系数 0.05 让它变成"雨丝"而不是"圆点"；寿命 1.1s × 9.8m/s ≈ 落下
    // 十来米，正好穿过整个视口。
    {"Rain", 620.0f, 1.1f, 0.15f, 9.0f, 2.5f, 0.02f,
     glm::vec3(0.0f, 0.0f, -9.8f), 0.025f, 1.2f, ecs::ParticleBlend::Alpha,
     ecs::ParticleRenderMode::StretchedBillboard, 0.05f,
     true, ecs::ParticleShape::Box, glm::vec3(6.0f, 6.0f, 0.2f), 0.5f,
     false, 1.0f, 0.5f,
     glm::vec3(0.62f, 0.72f, 0.86f), glm::vec3(0.78f, 0.86f, 0.98f),
     glm::vec4(0.0f, 0.5f, 0.45f, 0.0f), glm::vec4(1.0f, 1.0f, 1.0f, 1.0f),
     0, 0.0f, 8.0f, nullptr, 0, glm::vec3(0.0f), nullptr, 0, ecs::ParticleSprite::Streak},

    // ---------------- 雪 ----------------
    // ⚠ shapeExtents 是**半轴**：雨用 (6,6,0.2) 是一片薄板（雨落得快，一片
    //   板一秒就填满视野）；雪落得慢，必须给**一整块空气** (7,7,4.0)，否则
    //   前几秒整片雪还在镜头上面没落下来，看着像"点了没效果"。
    // 极慢的下落（0.35 m/s² 的"轻重力" + 大阻力），长寿命 + 湍流 → 打着旋飘。
    // 尺寸比雨大一个量级、不拉伸（雪片是点状的，不是丝状的）。
    {"Snow", 320.0f, 9.0f, 0.3f, 0.9f, 32.0f, 0.55f,
     glm::vec3(0.0f, 0.0f, -0.5f), 0.09f, 0.9f, ecs::ParticleBlend::Alpha,
     ecs::ParticleRenderMode::Billboard, 0.0f,
     true, ecs::ParticleShape::Box, glm::vec3(7.0f, 7.0f, 4.0f), 0.5f,
     true, 0.55f, 0.65f,
     glm::vec3(0.95f, 0.97f, 1.00f), glm::vec3(0.88f, 0.93f, 1.00f),
     glm::vec4(0.0f, 0.9f, 0.9f, 0.0f), glm::vec4(1.0f, 1.0f, 1.0f, 1.0f),
     0, 0.0f, 4.0f, nullptr, 0, glm::vec3(0.0f), nullptr, 0, ecs::ParticleSprite::Flake},

    // ---------------- 闪电 ----------------
    // 2026-10-18 二版（用户原话："我想要的是闪电，就是雷雨天的那种闪电，
    // 你做出的效果和闪电差得很远，而且闪电是有一定的时间间隔频率的"）。
    //
    // ⚠ 一版（Box 半轴 0.45 + 26 m/s + 拉伸 0.12 + emissive 16）实机截图
    //   出来是**一根 0.9 m 粗、纯白过曝的实心柱子** —— 因为 512 颗全挤在
    //   同一段高度里、拉伸长度 3 m，加色叠加直接合成了"灯管"。这一版按
    //   截图上量到的错误逐条改：
    //   ① **截面收细 15 倍**：Box 半轴 0.45 → 0.03（截面 6 cm）→ 才是电弧
    //      而不是柱子。x 略宽于 y（0.03/0.02）给一点"帘状"毛边。
    //   ② **不再整根同时亮**：寿命 0.22 → 0.16、初速 26 → 16、拉伸 0.12 →
    //      0.05（每根光丝 ≈ 0.8 m）→ 颗粒之间能看出疏密，不是一块白。
    //   ③ **emissive 16 → 5**：一版的加色在任何重叠处都直接削顶到 1.0，
    //      蓝白色全被压成纯白。5 保留得住 colorB 的冷蓝。
    //   ④ **靠 orbital 剪成斜劈**：见 FxPreset::orbital 的注释 —— 这是唯一
    //      能让整根柱**连贯地弯**的通道（噪声的种子逐粒子随机，调大只会
    //      把柱子打散成雾）。ω ≈ (30, 120, 0)°/s → 柱顶横向 ~0.7 m。
    //   ⑤ **高度收进视口**：originZ 3.2 → 1.9、半轴 z 3.1 → 1.8 → 世界
    //      z ∈ [0.1, 3.7]（3.6 m 高）。默认机位（9 m、45° FOV）画面高
    //      约 7.4 m，一版的 6.2 m 柱加下落 5.7 m 必然冲出上下边界。
    //   ⑥ **有频率**：duration 2.4 + looping → 每 2.4 秒打一次；每次持续
    //      ~0.45 s（lifetime 0.16 + 补发拉到 0.31），其余约 2 秒全黑。
    //      rate = 0：不留常驻电晕（第一发就在 t=0，点完立刻看得见）。
    //   ⑦ **多段回击**：0.09 / 0.17 / 0.31 秒各补一发（kBoltRestrikes），
    //      这才是"闪电"和"闪光灯"的区别。
    {"Lightning", 0.0f, 0.16f, 0.35f, 16.0f, 3.0f, 0.1f,
     glm::vec3(0.0f), 0.026f, 4.5f, ecs::ParticleBlend::Additive,
     ecs::ParticleRenderMode::StretchedBillboard, 0.05f,
     true, ecs::ParticleShape::Box, glm::vec3(0.03f, 0.02f, 1.8f), 0.3f,
     true, 14.0f, 2.2f,
     glm::vec3(0.97f, 0.99f, 1.00f), glm::vec3(0.55f, 0.72f, 1.00f),
     glm::vec4(1.0f, 1.0f, 0.35f, 0.0f), glm::vec4(1.0f, 0.85f, 0.55f, 0.25f),
     360, 2.4f, 1.9f, kBoltRestrikes, 3, glm::vec3(30.0f, 120.0f, 0.0f),
     kBoltForks, 3, ecs::ParticleSprite::Glow},
};

constexpr int kPresetCount = static_cast<int>(sizeof(kPresets) / sizeof(kPresets[0]));

// `base 1`、`base 2`……里第一个没被占用的名字。
//
// ⚠ 从 1 开始（不是 2）：用户明确要"编号数字后缀，比如 1，2，3"。
//   和 SceneHierarchy 的 uniqueName（从 2 起，因为那是"同名再加一个"的
//   语义）刻意不同 —— 这里生成的是"第一种雨 / 第二种雨"。
std::string uniqueEmitterName(scene::Scene& sc, const std::string& base) {
    for (int i = 1; i < 10000; ++i) {
        const std::string n = base + " " + std::to_string(i);
        if (!sc.find(n).valid()) return n;
    }
    return base;
}

void tagItem(const char* tag) {
    logRect(tag, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
}

} // namespace

// ---------------------------------------------------------------- 增删改

void ParticlesPanel::addPreset(int preset) {
    if (preset < 0 || preset >= kPresetCount) return;
    const FxPreset& p = kPresets[preset];

    scene::Scene& sc = m_ctx.editorScene();
    ecs::Entity created{};
    std::string createdName;
    int forkMade = 0;

    // 建一个发射器（主通道与分叉共用这一段）。fork 参数为 nullptr 时
    // 全部取表里的值；否则覆盖半高 / 粗细 / 亮度 / 剪切 / 颗数倍率。
    auto spawnEmitter = [&](const std::string& name, const glm::vec3& pos,
                            const glm::vec2& tiltDeg,
                            float halfZ, float size, float emissive,
                            const glm::vec3& orbital,
                            float countScale) {
        ecs::Entity e = sc.createObject(name);
        if (auto* tc = sc.world().get<ecs::TransformComponent>(e)) {
            tc->position = pos;
            // 雨 / 雪 / 闪电要"从上往下落"：发射轴是局部 +Z，绕 X 翻 180°
            // （别用 glm::pi<float>()：这文件没 include gtc/constants）
            //
            // ⚠ 倾斜必须叠在**翻转之后**：Transform 的矩阵是
            //   `Rz·Ry·Rx`（见 Components.h:41-43），所以 rx 先作用。
            //   rx = π + ty → 轴 (0, sin ty, -cos ty)：朝 +Y 偏；
            //   ry = tx     → 再把轴转到 (-sin tx, 0, -cos tx)：朝 -X 偏。
            //   两个都写 0 时与原来的 (π,0,0) 逐位相同。
            const float baseX = p.flipDown ? 3.14159265f : 0.0f;
            // ⚠ 只有"往下落"的那几个预设（雨/雪/闪电）才吃倾斜；火/烟/火花
            //   保持 (0,0,0) —— 它们的老行为必须逐位不变。
            const glm::vec2 tilt = p.flipDown ? tiltDeg : glm::vec2(0.0f);
            tc->rotation = glm::vec3(baseX + glm::radians(tilt.y),
                                     glm::radians(tilt.x), 0.0f);
            tc->scale = glm::vec3(1.0f);
        }
        ecs::ParticleEmitterComponent fx;
        fx.rate = p.rate;
        fx.lifetime = p.lifetime;
        fx.lifetimeVariance = p.lifetimeVar;
        fx.startSpeed = p.startSpeed;
        fx.spread = p.spread;
        fx.drag = p.drag;
        fx.gravity = p.gravity;
        fx.startSize = size;
        // 贴图形状由预设带（雨 = 竖条、雪 = 六角、火花 = 星芒 ……）
        fx.sprite = p.sprite;
        fx.emissive = emissive;
        fx.blend = p.blend;
        fx.renderMode = p.render;
        fx.stretchScale = p.stretchScale;
        fx.shape = p.shape;
        // 只覆盖竖直半轴：截面（x/y）保持"细"不变，分叉才不会又变回柱子
        fx.shapeExtents = glm::vec3(p.shapeExtents.x, p.shapeExtents.y, halfZ);
        fx.shapeRadius = p.shapeRadius;
        fx.noiseEnabled = p.noise;
        fx.noiseStrength = p.noiseStrength;
        fx.noiseFrequency = p.noiseFreq;
        // 轨道剪切（闪电用，见 FxPreset::orbital）
        fx.orbital = orbital;
        fx.alphaOverLife = p.alphaOverLife;
        fx.sizeOverLife = p.sizeOverLife;
        // 渐变：三段（起点 → 中点 → 终点）。只给两个颜色时，中点取两者均值，
        // 这样"雨丝在半空中变亮"这种过渡是连续的，不会在 t=0.5 突变。
        fx.colorRamp.count = 3;
        fx.colorRamp.stops[0] = glm::vec4(p.colorA, 0.0f);
        fx.colorRamp.stops[1] = glm::vec4((p.colorA + p.colorB) * 0.5f, 0.5f);
        fx.colorRamp.stops[2] = glm::vec4(p.colorB, 1.0f);
        if (p.burstCount > 0) {
            // ⚠ 分叉与主通道**共用同一张 burst 表 + 同一个 duration** ——
            //   这是"同一瞬间一起亮"的唯一保证（模拟时刻 t 是全发射器共享的，
            //   每轮都从 mod(t, duration) 算起，所以它们永远同步）。
            auto burstCount = [countScale](int n) {
                const int v = static_cast<int>(
                    static_cast<float>(n) * countScale + 0.5f);
                return std::max(1, v);
            };
            // 主回击（t=0）。rate 由表里给 —— 闪电的 rate 是 0
            // （用户要的是"有间隔的雷击"，不要常驻电晕）。
            fx.bursts[0].time = 0.0f;
            fx.bursts[0].count = burstCount(p.burstCount);
            fx.bursts[0].cycles = 1;
            fx.bursts[0].interval = 0.0f;
            // 补发（多段回击）。上限 ecs::kMaxBursts = 4，主回击占第 0 条，
            // 所以最多再塞 3 条 —— 表里多给了就截断（而不是越界写）。
            const int extra = std::min(p.moreBurstCount, ecs::kMaxBursts - 1);
            for (int b = 0; b < extra; ++b) {
                fx.bursts[b + 1].time = p.moreBursts[b].time;
                fx.bursts[b + 1].count = burstCount(p.moreBursts[b].count);
                fx.bursts[b + 1].cycles = 1;
                fx.bursts[b + 1].interval = 0.0f;
            }
            fx.duration = p.duration;
            fx.looping = true;
        }
        sc.world().add<ecs::ParticleEmitterComponent>(e, fx);
        return e;
    };

    m_ctx.structuralEdit(std::string("Add ") + p.label + " Emitter", [&]() {
        // 名字按类型 + 编号（Rain 1 / Rain 2 ……），不再全员叫 Particle Effect
        createdName = uniqueEmitterName(sc, p.label);
        // 主通道也带一点天然的倾斜（完全铅直看着像灯柱）
        created = spawnEmitter(createdName, glm::vec3(0.0f, 0.0f, p.originZ),
                               glm::vec2(3.0f, -2.0f),
                               p.shapeExtents.z, p.startSize, p.emissive,
                               p.orbital, 1.0f);
        // 分叉（见 FxFork 的注释）：每一条都是**独立的普通发射器**，
        // 用户可以在层级里单独选中、单独调，不是黑魔法。
        if (p.forks != nullptr) {
            const int n = std::min(p.forkCount, 6);
            for (int i = 0; i < n; ++i) {
                const FxFork& f = p.forks[i];
                const std::string fn =
                    uniqueEmitterName(sc, createdName + " Fork");
                spawnEmitter(fn,
                             glm::vec3(f.offset.x, f.offset.y,
                                       p.originZ + f.offset.z),
                             f.tilt, f.halfZ, f.size, f.emissive, f.orbital,
                             f.countScale);
                ++forkMade;
            }
        }
    });
    if (!created.valid()) return;

    m_ctx.select(created);
    m_ctx.dirty() = true;
    // 总开关不开的话新特效是"隐形"的 —— 用户会以为按钮坏了。
    if (void* pp = render::paramPtr("fx.enabled")) {
        *static_cast<bool*>(pp) = true;
    }
    if (forkMade > 0) {
        // 用户点了 1 个按钮却多出几行，必须说清楚，否则像 bug
        m_ctx.notify(std::string(TR("Added ")) + createdName + " (+" +
                     std::to_string(forkMade) + " " + TR("forks") + ")");
    } else {
        m_ctx.notify(std::string(TR("Added ")) + createdName);
    }
    VK_LOG_INFO("FX-PANEL preset=%s name=\"%s\" entity=%u forks=%d",
                p.label, createdName.c_str(), created.id, forkMade);
}

void ParticlesPanel::beginRename(ecs::Entity e) {
    const std::string* nm = m_ctx.editorScene().world().name(e);
    std::snprintf(m_renameBuf, sizeof(m_renameBuf), "%s",
                  nm ? nm->c_str() : "Emitter");
    m_renameId = e.id;
    m_renameFocus = true;
    VK_LOG_INFO("FX-PANEL rename begin entity=%u", e.id);
}

void ParticlesPanel::commitRename(ecs::Entity e) {
    if (m_renameId != e.id) return;
    std::string want = m_renameBuf;
    // 去首尾空白；空名字不接受（空名字在层级树里就是一条看不见的行）
    const size_t b = want.find_first_not_of(" \t");
    const size_t en = want.find_last_not_of(" \t");
    want = (b == std::string::npos) ? std::string() : want.substr(b, en - b + 1);

    m_renameId = 0;
    if (want.empty()) {
        VK_LOG_INFO("FX-PANEL rename cancel entity=%u (empty)", e.id);
        return;
    }
    if (!m_ctx.editorScene().find(want).valid()) {
        m_ctx.structuralEdit("Rename Emitter", [&]() {
            m_ctx.editorScene().world().setName(e, want);
        });
        m_ctx.dirty() = true;
        VK_LOG_INFO("FX-PANEL rename entity=%u name=\"%s\"", e.id, want.c_str());
    } else {
        // 重名会把层级树弄成一堆同名的行，直接拒绝（比静默加后缀更可预期）
        m_ctx.notify(std::string(TR("Name already used: ")) + want);
        VK_LOG_INFO("FX-PANEL rename reject entity=%u name=\"%s\"", e.id,
                    want.c_str());
    }
}

void ParticlesPanel::duplicateEmitter(ecs::Entity e) {
    scene::Scene& sc = m_ctx.editorScene();
    const ecs::World& w = sc.world();
    const auto* src = w.get<ecs::ParticleEmitterComponent>(e);
    if (!src) return;
    const ecs::ParticleEmitterComponent copy = *src;
    const std::string* nm = w.name(e);
    const std::string base = nm ? *nm : "Emitter";
    // "Rain 1" → "Rain"（去尾部的 " 数字"），副本继续按类型编号，
    // 否则会造出 "Rain 1 1" 这种越堆越长的名字。
    std::string stem = base;
    const size_t sp = base.find_last_of(' ');
    if (sp != std::string::npos && sp + 1 < base.size() &&
        base.find_first_not_of("0123456789", sp + 1) == std::string::npos) {
        stem = base.substr(0, sp);
    }

    ecs::Entity dup{};
    std::string dupName;
    m_ctx.structuralEdit("Duplicate Emitter", [&]() {
        dupName = uniqueEmitterName(sc, stem);
        dup = sc.createObject(dupName);
        if (const auto* t = w.get<ecs::TransformComponent>(e)) {
            if (auto* td = sc.world().get<ecs::TransformComponent>(dup)) {
                *td = *t;
                td->position.x += 1.0f;   // 挪开 1m，否则和原来的完全重合看不出
            }
        }
        sc.world().add<ecs::ParticleEmitterComponent>(dup, copy);
    });
    if (!dup.valid()) return;
    m_ctx.select(dup);
    m_ctx.dirty() = true;
    VK_LOG_INFO("FX-PANEL duplicate from=%u to=%u name=\"%s\"", e.id, dup.id,
                dupName.c_str());
}

void ParticlesPanel::deleteEmitter(ecs::Entity e) {
    scene::Scene& sc = m_ctx.editorScene();
    const std::string* nm = sc.world().name(e);
    const std::string label = nm ? *nm : "Emitter";
    if (m_ctx.selection() == e) m_ctx.select(ecs::Entity{});
    m_ctx.structuralEdit("Delete Emitter", [&]() {
        sc.world().destroy(e);
    });
    m_ctx.dirty() = true;
    m_ctx.notify(std::string(TR("Deleted ")) + label);
    VK_LOG_INFO("FX-PANEL delete entity=%u name=\"%s\"", e.id, label.c_str());
}

// ---------------------------------------------------------------- 清单一行

bool ParticlesPanel::emitterRow(ecs::Entity e, const char* name, int index) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float h = 24.0f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;

    const bool selected = (m_ctx.selection() == e);
    ImGui::PushID(index);
    const bool clicked = ImGui::Selectable("##fxrow", selected,
                                           ImGuiSelectableFlags_AllowOverlap,
                                           ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();

    // ---- 右键菜单：重命名 / 复制 / 删除 ----
    // 挂着 Selectable 上（BeginPopupContextItem），所以只有这一行会响，
    // 点在空白处不会弹出"不知道操作的是谁"的菜单。
    if (ImGui::BeginPopupContextItem("##fxctx")) {
        // 自动化锚点：菜单窗口矩形（脚本据此断言"右键真的弹出了菜单"）
        logRect("FX-CTX", ImGui::GetWindowPos(),
                ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(),
                       ImGui::GetWindowPos().y + ImGui::GetWindowHeight()));
        // 每个菜单项自己的矩形（脚本按它点，不按"第几行"估位 —— 行高随
        // 主题/翻译变，估位会在换语言之后点空）
        if (ImGui::MenuItem(TR("Rename"))) {
            beginRename(e);
            ImGui::CloseCurrentPopup();
        }
        tagItem("FX-CTX-Rename");
        if (ImGui::MenuItem(TR("Duplicate"))) duplicateEmitter(e);
        tagItem("FX-CTX-Duplicate");
        ImGui::Separator();
        if (ImGui::MenuItem(TR("Delete"))) deleteEmitter(e);
        tagItem("FX-CTX-Delete");
        ImGui::EndPopup();
    }

    ImVec4 tc = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    if (!selected && !hovered) tc.w *= 0.88f;
    const ImU32 col = ImGui::GetColorU32(tc);
    const float cy = p.y + h * 0.5f;

    // 一颗小火花（和底栏那个 Particles 图标同形）
    const float cx = p.x + 12.0f;
    dl->AddLine(ImVec2(cx - 3.0f, cy), ImVec2(cx + 3.0f, cy), col, 1.4f);
    dl->AddLine(ImVec2(cx, cy - 3.0f), ImVec2(cx, cy + 3.0f), col, 1.4f);
    dl->AddLine(ImVec2(cx - 2.2f, cy - 2.2f), ImVec2(cx + 2.2f, cy + 2.2f), col,
                1.1f);
    dl->AddLine(ImVec2(cx - 2.2f, cy + 2.2f), ImVec2(cx + 2.2f, cy - 2.2f), col,
                1.1f);

    // ---- 名字（重命名中就换成输入框）----
    if (m_renameId == e.id) {
        ImGui::SetCursorScreenPos(ImVec2(p.x + 24.0f, p.y + 1.0f));
        ImGui::SetNextItemWidth(w - 30.0f);
        if (m_renameFocus) {
            ImGui::SetKeyboardFocusHere();
            m_renameFocus = false;
        }
        const bool entered = ImGui::InputText(
            "##fxrename", m_renameBuf, sizeof(m_renameBuf),
            ImGuiInputTextFlags_EnterReturnsTrue |
                ImGuiInputTextFlags_AutoSelectAll);
        // Esc / 点到别处 = 取消（结束编辑、不改名）；回车 = 确认
        const bool cancelled = ImGui::IsItemDeactivated() && !entered;
        if (entered) {
            commitRename(e);
        } else if (cancelled && m_renameId == e.id) {
            m_renameId = 0;
            VK_LOG_INFO("FX-PANEL rename cancel entity=%u", e.id);
        }
        // 行内控件会把光标推下去：画完摆回"这一行之后"，下一行才不会错位
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
    } else {
        dl->AddText(ImVec2(p.x + 24.0f, cy - ImGui::GetTextLineHeight() * 0.5f),
                    col, name);
    }

    char tag[48];
    std::snprintf(tag, sizeof(tag), "FX-ROW %d", index);
    logRect(tag, p, ImVec2(p.x + w, p.y + h));
    ImGui::PopID();
    return clicked;
}

// ============================================================ 选中发射器编辑
//
// 2026-10-18（用户原话："粒子面板中，添加不同的粒子类型时，其对应下方的
// 粒子相关参数和配置应该都是一一独立的，不要全部一样公用的，比如说，点击
// Smoke 1 是下方显示的就应该是 Smoke 1 的属性配置，点击 Sparks 1 时下方
// 显示的应该是 Sparks 1 的属性配置"）。
//
// 之前这块面板只有**全局** `fx::ParticleSettings`（总开关 / 亮度 / 尺寸 /
// 软化 / 频率 / 容量）—— 那是整个粒子 pass 共享的一份，与"哪一个发射器"
// 无关，所以六种预设点出来的面板长得一模一样。现在补上**逐发射器**的一段：
//
//   · 数据源只有一个：`m_ctx.selection()` 那个实体的
//     `ecs::ParticleEmitterComponent`。**每一行控件都直接读写它**，没有
//     任何镜像或缓存 —— 所以"点 Smoke 1 看到 Smoke 1"是结构上成立的，
//     不靠同步代码维持（同步代码迟早会漏一条路径）。
//   · 模块划分照 Unity / UE5：Main / Emission（含 Bursts）/ Shape /
//     Colour over Lifetime（含渐变编辑器）/ Size / Velocity / Noise /
//     Renderer。Inspector 的 Particles 页是同一份组件的另一个入口，
//     改的是同一份数据 —— 这是刻意的：面板负责"快"，Inspector 负责"全"。
//   · 撤销：一次交互一条历史（整组件快照，POD 六百多字节）。拖动中的中间
//     帧不记 —— 否则拖一次滑条会压出几十条历史。

namespace {

// 面板里标签列的宽度。比 Inspector 的 140 窄一点（窗口只有 420 宽），
// 但要够放 "Rate over distance" 这种最长的标签。
constexpr float kFxRowW = 138.0f;

// 渐变编辑器 + burst 表都要"改完一条记一条历史"，而它们不知道宿主是谁。
// 这里用回调把"怎么记历史"传进来（模板，不引入 std::function 的分配）。
template <typename Commit>
void fxDrawRampEditor(EditorContext& ctx, ecs::ParticleEmitterComponent* fx,
                      Commit&& commit) {
    ecs::ParticleColorRamp& ramp = fx->colorRamp;

    // 序列化层可能读进乱序 / 越界的 count，先夹住再干活
    if (ramp.count < 1) ramp.count = 1;
    if (ramp.count > ecs::kMaxColorStops) ramp.count = ecs::kMaxColorStops;

    // -------------------------------------------------------------- 预览带
    // 一排 1px 竖条铺满宽度。颜色是**线性 HDR**（>1 才是火焰发光的来源），
    // 直接当 8 位色画会在 1.0 处整片截白 → 先 Reinhard + sRGB，只为看清走向。
    {
        float bandW = ImGui::GetContentRegionAvail().x;
        const float bandH = 18.0f;
        if (bandW < 24.0f) bandW = 24.0f;
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##rampband", ImVec2(bandW, bandH));
        logRect("FX-RAMP-BAND", p0, ImVec2(p0.x + bandW, p0.y + bandH));
        ImDrawList* dl = ImGui::GetWindowDrawList();

        // 求值用**已排序的副本**：拖动中颜色仍然连续（原数组可能在乱序中间态）
        ecs::ParticleColorRamp view = ramp;
        view.sort();
        const int cols = static_cast<int>(bandW) < 2
                             ? 2
                             : static_cast<int>(bandW);
        auto to8 = [](float v) {
            const float s = std::pow(glm::clamp(v, 0.0f, 1.0f), 1.0f / 2.2f);
            return static_cast<int>(glm::clamp(s, 0.0f, 1.0f) * 255.0f + 0.5f);
        };
        for (int i = 0; i < cols; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(cols - 1);
            const glm::vec3 c = view.eval(t);
            const glm::vec3 m = c / (c + glm::vec3(1.0f));
            dl->AddRectFilled(
                ImVec2(p0.x + static_cast<float>(i), p0.y),
                ImVec2(p0.x + static_cast<float>(i + 1), p0.y + bandH),
                IM_COL32(to8(m.x), to8(m.y), to8(m.z), 255));
        }
        dl->AddRect(p0, ImVec2(p0.x + bandW, p0.y + bandH),
                    IM_COL32(255, 255, 255, 70));
        // 停靠点竖线用**原数组**（拖动时标记要跟着鼠标走，不能跟着排序跳）
        for (int i = 0; i < ramp.count; ++i) {
            const float x =
                p0.x + glm::clamp(ramp.stops[i].w, 0.0f, 1.0f) * bandW;
            dl->AddLine(ImVec2(x, p0.y - 3.0f), ImVec2(x, p0.y + bandH + 3.0f),
                        IM_COL32(255, 255, 255, 190), 1.5f);
        }
        ImGui::Spacing();
    }

    // ------------------------------------------------------------ 停靠点行
    for (int i = 0; i < ramp.count; ++i) {
        glm::vec4& stop = ramp.stops[i];
        ImGui::PushID(i);
        {
            const ecs::ParticleColorRamp c0 = ramp;
            ImGui::ColorEdit3("##c", glm::value_ptr(stop),
                              ImGuiColorEditFlags_NoInputs |
                                  ImGuiColorEditFlags_NoLabel |
                                  ImGuiColorEditFlags_HDR);
            if (ImGui::IsItemDeactivatedAfterEdit()) commit(c0, "Colour Stop");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(TR("Linear HDR colour of this stop.\n"
                                     "Values above 1 are allowed and are what\n"
                                     "makes a spark bloom."));
            }
        }
        ImGui::SameLine();
        {
            const float avail = ImGui::GetContentRegionAvail().x;
            ImGui::SetNextItemWidth(avail - 30.0f);
            const ecs::ParticleColorRamp t0 = ramp;
            float t = stop.w;
            ImGui::SliderFloat("##t", &t, 0.0f, 1.0f, "t=%.2f");
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                stop.w = t;
                // ⚠ 排序只在**松手**时做：拖到一半就排，行号会换成别人，
                //   滑条上的值看起来像"跳到了另一个颜色"（色带控件经典坑）
                ramp.sort();
                commit(t0, "Colour Stop Position");
            } else {
                stop.w = t;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(TR("Where this stop sits along the particle's\n"
                                     "life (0 = born, 1 = about to die).\n"
                                     "Stops are re-sorted when you let go, so\n"
                                     "you can drag one past its neighbours."));
            }
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(ramp.count <= 1);
        if (ImGui::SmallButton("x")) {
            const int k = i;
            ctx.structuralEdit("Colour Ramp Remove Stop", [&]() {
                for (int j = k; j < ramp.count - 1; ++j) {
                    ramp.stops[j] = ramp.stops[j + 1];
                }
                if (ramp.count > 1) --ramp.count;
            });
        }
        ImGui::EndDisabled();
        ImGui::PopID();
    }

    // ------------------------------------------------------------ Add stop
    ImGui::BeginDisabled(ramp.count >= ecs::kMaxColorStops);
    if (ImGui::SmallButton(TR("Add stop"))) {
        // 插在**最大空隙**的正中，颜色取该处的渐变值 → 加点不改变外观，
        // 只是多一个能拖的控制点（插在末尾会立刻变形，用户以为点错了）
        int at = ramp.count;
        float bestGap = -1.0f;
        float bestT = 1.0f;
        float prev = 0.0f;
        for (int i = 0; i < ramp.count; ++i) {
            const float gap = ramp.stops[i].w - prev;
            if (gap > bestGap) {
                bestGap = gap;
                bestT = (prev + ramp.stops[i].w) * 0.5f;
                at = i;
            }
            prev = ramp.stops[i].w;
        }
        if (1.0f - prev > bestGap) {
            bestT = (prev + 1.0f) * 0.5f;
            at = ramp.count;
        }
        const glm::vec3 c = ramp.eval(bestT);
        ctx.structuralEdit("Colour Ramp Add Stop", [&]() {
            for (int i = ramp.count; i > at; --i) {
                ramp.stops[i] = ramp.stops[i - 1];
            }
            ramp.stops[at] = glm::vec4(c, bestT);
            if (ramp.count < ecs::kMaxColorStops) ++ramp.count;
            ramp.sort();
        });
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled(TR("%d / %d stops"), ramp.count, ecs::kMaxColorStops);
}

} // namespace

void ParticlesPanel::drawSelectedEmitter(ecs::Entity e,
                                         ecs::ParticleEmitterComponent* fx) {
    ImGui::SeparatorText(TR("Selected emitter"));
    if (!e.valid() || !fx) {
        ImGui::TextDisabled(TR("Click a row in the list above (or a particle\n"
                               "emitter in the Hierarchy) - this section then\n"
                               "shows that emitter's own parameters."));
        return;
    }
    // 自动化锚点（见头文件 m_selSig 的注释）：id 与关键参数一起进签名，
    // 任何一个变了才打一行 —— 拖动滑条时最多每帧一行，停下来就不打了。
    {
        const unsigned sig =
            e.id * 1000003u +
            static_cast<unsigned>(fx->rate * 10.0f + 0.5f) * 131u +
            static_cast<unsigned>(fx->lifetime * 100.0f + 0.5f) * 17u +
            static_cast<unsigned>(static_cast<int>(fx->sprite)) * 7u;
        if (sig != m_selSig) {
            m_selSig = sig;
            VK_LOG_INFO("FX-SEL id=%u rate=%.1f life=%.2f sprite=%d",
                        e.id, static_cast<double>(fx->rate),
                        static_cast<double>(fx->lifetime),
                        static_cast<int>(fx->sprite));
        }
    }
    const std::string* nm = m_ctx.editorScene().world().name(e);
    ImGui::Text("%s", nm ? nm->c_str() : "Emitter");
    logRect("FX-SEL-NAME", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::SameLine();
    ImGui::TextDisabled(TR("(entity %u)"), e.id);

    if (!m_ctx.isEditing()) {
        ImGui::TextDisabled(TR("run mode - emitters are read-only here"));
        return;
    }

    // ---- 撤销：整组件快照，一次交互一条历史 ----
    auto commit = [&](const ecs::ParticleEmitterComponent& before,
                      const char* undoName) {
        if (std::memcmp(&before, fx, sizeof(before)) == 0) return;
        m_ctx.commands().pushAlreadyApplied(std::make_unique<RawBytesEditCommand>(
            undoName, fx, &before, fx, sizeof(before)));
        m_ctx.dirty() = true;
        m_ctx.setStatus(undoName);
    };

    auto label = [](const char* text, const char* tip) {
        ImGui::TextUnformatted(TR(text));
        if (tip && *tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", TR(tip));
        ImGui::SameLine(kFxRowW);
        ImGui::SetNextItemWidth(-1.0f);
    };
    auto dragF = [&](const char* text, const char* id, float* v, float speed,
                     float lo, float hi, const char* fmt, const char* tip) {
        label(text, tip);
        const ecs::ParticleEmitterComponent before = *fx;
        ImGui::PushID(id);
        ImGui::DragFloat("##v", v, speed, lo, hi, fmt);
        ImGui::PopID();
        if (ImGui::IsItemDeactivatedAfterEdit()) commit(before, id);
    };
    auto dragI = [&](const char* text, const char* id, int* v, int lo, int hi,
                     const char* tip) {
        label(text, tip);
        const ecs::ParticleEmitterComponent before = *fx;
        ImGui::PushID(id);
        ImGui::SliderInt("##v", v, lo, hi);
        ImGui::PopID();
        if (ImGui::IsItemDeactivatedAfterEdit()) commit(before, id);
    };
    auto check = [&](const char* text, const char* id, bool* v, const char* tip) {
        ImGui::PushID(id);
        const ecs::ParticleEmitterComponent before = *fx;
        if (ImGui::Checkbox(TR(text), v)) commit(before, id);
        ImGui::PopID();
        if (tip && *tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", TR(tip));
    };
    auto combo = [&](const char* text, const char* id, int* v,
                     const char* const* items, int n, const char* tip) {
        label(text, tip);
        ImGui::PushID(id);
        const ecs::ParticleEmitterComponent before = *fx;
        if (ImGui::Combo("##v", v, items, n)) commit(before, id);
        ImGui::PopID();
    };

    // ================================================================ Main
    if (ImGui::CollapsingHeader(TR("Main"), ImGuiTreeNodeFlags_DefaultOpen)) {
        logRect("FX-MOD-MAIN", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        dragF("Rate over time", "RateOverTime", &fx->rate, 1.0f, 0.0f, 5000.0f,
              "%.0f /s",
              "Particles spawned per second. The GPU accumulates this per\n"
              "slot, so the rate is identical at any frame rate.");
        dragF("Rate over distance", "RateOverDistance", &fx->rateOverDistance,
              0.05f, 0.0f, 200.0f, "%.2f /m",
              "Extra particles per metre the emitter travels - this is what\n"
              "leaves a trail behind a moving emitter. 0 = off.");
        dragF("Start lifetime", "StartLifetime", &fx->lifetime, 0.01f, 0.02f,
              120.0f, "%.2f s", "How long one particle lives.");
        dragF("Lifetime variance", "LifetimeVariance", &fx->lifetimeVariance,
              0.01f, 0.0f, 1.0f, "%.2f",
              "Random +/- fraction on the lifetime (0.3 = 30%). This is what\n"
              "stops every particle dying at the same instant.");
        dragF("Start speed", "StartSpeed", &fx->startSpeed, 0.02f, 0.0f, 200.0f,
              "%.2f m/s", "Initial speed along the emitter's +Z axis.");
        dragF("Speed variance", "SpeedVariance", &fx->speedVariance, 0.01f, 0.0f,
              1.0f, "%.2f", "Random +/- fraction on the initial speed.");
        dragF("Spread", "Spread", &fx->spread, 0.5f, 0.0f, 180.0f, "%.0f deg",
              "Half angle of the emission cone.");
        dragF("Drag", "Drag", &fx->drag, 0.01f, 0.0f, 10.0f, "%.2f /s",
              "Velocity decays as exp(-drag * t). 0 = vacuum.");
        label("Gravity (m/s2)", "Constant acceleration. Z-up: -9.8 pulls down,\n"
                                "+1.8 makes fire rise.");
        {
            const ecs::ParticleEmitterComponent before = *fx;
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::DragFloat3("##gravity", glm::value_ptr(fx->gravity), 0.05f,
                              -50.0f, 50.0f, "%.2f");
            if (ImGui::IsItemDeactivatedAfterEdit()) commit(before, "Gravity");
        }
        dragF("Duration", "Duration", &fx->duration, 0.01f, 0.0f, 120.0f,
              "%.2f s",
              "0 = infinite. With Looping this is the period of one cycle -\n"
              "bursts replay every Duration seconds (that is the 'interval'\n"
              "of a lightning strike).");
        check("Looping", "Looping", &fx->looping,
              "Restart a cycle every Duration seconds. Off = emit once.");
        dragF("Start delay", "StartDelay", &fx->startDelay, 0.01f, 0.0f, 60.0f,
              "%.2f s", "Nothing is emitted before this time.");
        check("Prewarm", "Prewarm", &fx->prewarm,
              "Simulate one full cycle before the first frame, so the effect\n"
              "is already in steady state when it appears.");
        {
            static const char* kSpaceNames[] = {"World", "Local"};
            int space = static_cast<int>(fx->simulationSpace);
            combo("Simulation space", "SimulationSpace", &space, kSpaceNames, 2,
                  "World: particles stay where they were born.\n"
                  "Local: they follow the emitter (fire on a moving torch).");
            if (space != static_cast<int>(fx->simulationSpace)) {
                const ecs::ParticleEmitterComponent before = *fx;
                fx->simulationSpace =
                    static_cast<ecs::ParticleSimulationSpace>(space);
                commit(before, "Simulation Space");
            }
        }
    }

    // ============================================================ Emission
    if (ImGui::CollapsingHeader(TR("Emission / Bursts"),
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        logRect("FX-MOD-EMISSION", ImGui::GetItemRectMin(),
                ImGui::GetItemRectMax());
        ImGui::TextDisabled(TR("A burst fires Count particles at Time, and\n"
                               "repeats every Interval (when Cycles > 1).\n"
                               "Count = 0 disables that row."));
        for (int b = 0; b < ecs::kMaxBursts; ++b) {
            ecs::ParticleBurst& bu = fx->bursts[b];
            ImGui::PushID(700 + b);
            ImGui::TextDisabled(TR("#%d"), b + 1);
            ImGui::SameLine(32.0f);
            ImGui::SetNextItemWidth(58.0f);
            {
                const ecs::ParticleEmitterComponent before = *fx;
                ImGui::DragFloat("##time", &bu.time, 0.01f, 0.0f, 60.0f,
                                 "t=%.2f");
                if (ImGui::IsItemDeactivatedAfterEdit()) commit(before, "Burst Time");
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(52.0f);
            {
                const ecs::ParticleEmitterComponent before = *fx;
                ImGui::DragInt("##count", &bu.count, 1.0f, 0, 512, "%d");
                if (ImGui::IsItemDeactivatedAfterEdit()) commit(before, "Burst Count");
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(46.0f);
            {
                const ecs::ParticleEmitterComponent before = *fx;
                ImGui::DragInt("##cycles", &bu.cycles, 0.1f, 0, 64, "x%d");
                if (ImGui::IsItemDeactivatedAfterEdit()) commit(before, "Burst Cycles");
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1.0f);
            {
                const ecs::ParticleEmitterComponent before = *fx;
                ImGui::DragFloat("##interval", &bu.interval, 0.01f, 0.0f, 60.0f,
                                 TR("every %.2fs"));
                if (ImGui::IsItemDeactivatedAfterEdit()) commit(before, "Burst Interval");
            }
            char tag[48];
            std::snprintf(tag, sizeof(tag), "FX-BURST-%d", b);
            logRect(tag, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            ImGui::PopID();
        }
        ImGui::TextDisabled(TR("Tip: Count is capped by the slot count\n"
                               "(%u per emitter)."), 512u);
    }

    // =============================================================== Shape
    if (ImGui::CollapsingHeader(TR("Shape"))) {
        logRect("FX-MOD-SHAPE", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        static const char* kShapeNames[] = {"Point", "Sphere", "Cone",
                                            "Box",   "Circle", "Edge"};
        int shape = static_cast<int>(fx->shape);
        combo("Shape", "ShapeType", &shape, kShapeNames, 6,
              "Where new particles are born inside the emitter.");
        if (shape != static_cast<int>(fx->shape)) {
            const ecs::ParticleEmitterComponent before = *fx;
            fx->shape = static_cast<ecs::ParticleShape>(shape);
            commit(before, "Emitter Shape");
        }
        if (fx->shape == ecs::ParticleShape::Box) {
            label("Box half extents", "Half the box size, in metres.");
            const ecs::ParticleEmitterComponent before = *fx;
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::DragFloat3("##ext", glm::value_ptr(fx->shapeExtents), 0.01f,
                              0.0f, 100.0f, "%.2f");
            if (ImGui::IsItemDeactivatedAfterEdit()) commit(before, "Shape Extents");
        } else if (fx->shape != ecs::ParticleShape::Point) {
            dragF("Radius", "ShapeRadius", &fx->shapeRadius, 0.01f, 0.0f, 100.0f,
                  "%.2f m", "Radius of the sphere / circle / cone base.");
            dragF("Radius thickness", "RadiusThickness", &fx->radiusThickness,
                  0.01f, 0.0f, 1.0f, "%.2f",
                  "1 = solid volume, 0 = only the surface.");
            dragF("Arc", "ShapeArc", &fx->arc, 1.0f, 0.0f, 360.0f, "%.0f deg",
                  "How much of the circle is used (360 = all around).");
        }
        static const char* kEmitNames[] = {"Volume", "Edge", "Shell"};
        int from = static_cast<int>(fx->emitFrom);
        combo("Emit from", "EmitFrom", &from, kEmitNames, 3,
              "Volume = inside the shape, Edge = its outline.");
        if (from != static_cast<int>(fx->emitFrom)) {
            const ecs::ParticleEmitterComponent before = *fx;
            fx->emitFrom = static_cast<ecs::ParticleEmitFrom>(from);
            commit(before, "Emit From");
        }
    }

    // ==================================================== Colour over life
    if (ImGui::CollapsingHeader(TR("Colour over Lifetime"),
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        logRect("FX-MOD-COLOUR", ImGui::GetItemRectMin(),
                ImGui::GetItemRectMax());
        ImGui::TextDisabled(TR("Colour as a gradient along the particle's life -\n"
                               "add stops to make multi-colour effects."));
        fxDrawRampEditor(m_ctx, fx, [&](const ecs::ParticleColorRamp& before,
                                        const char* what) {
            if (std::memcmp(&before, &fx->colorRamp, sizeof(before)) == 0) return;
            m_ctx.commands().pushAlreadyApplied(
                std::make_unique<RawBytesEditCommand>(what, &fx->colorRamp,
                                                      &before, &fx->colorRamp,
                                                      sizeof(before)));
            m_ctx.dirty() = true;
            m_ctx.setStatus(what);
        });
        ImGui::Spacing();
        ImGui::TextDisabled(TR("Alpha curve (4 points, t = 0, 1/3, 2/3, 1)"));
        {
            label("Alpha", "Opacity multiplier over life. The shape is the\n"
                           "curve through these four values.");
            const ecs::ParticleEmitterComponent before = *fx;
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::DragFloat4("##alpha", glm::value_ptr(fx->alphaOverLife),
                              0.005f, 0.0f, 4.0f, "%.2f");
            if (ImGui::IsItemDeactivatedAfterEdit()) commit(before, "Alpha Curve");
        }
    }

    // ================================================================ Size
    if (ImGui::CollapsingHeader(TR("Size over Lifetime"))) {
        logRect("FX-MOD-SIZE", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        label("Size curve", "Multiplier on Start Size. Four points at\n"
                            "t = 0, 1/3, 2/3, 1.");
        {
            const ecs::ParticleEmitterComponent before = *fx;
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::DragFloat4("##sizec", glm::value_ptr(fx->sizeOverLife), 0.005f,
                              0.0f, 8.0f, "%.2f");
            if (ImGui::IsItemDeactivatedAfterEdit()) commit(before, "Size Curve");
        }
    }

    // ============================================================ Velocity
    if (ImGui::CollapsingHeader(TR("Velocity / Force / Noise"))) {
        logRect("FX-MOD-VELOCITY", ImGui::GetItemRectMin(),
                ImGui::GetItemRectMax());
        label("Orbital (deg/s)", "Angular velocity around the three axes -\n"
                                 "this is what makes a vortex or a swirl.");
        {
            const ecs::ParticleEmitterComponent before = *fx;
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::DragFloat3("##orb", glm::value_ptr(fx->orbital), 0.5f, -720.0f,
                              720.0f, "%.0f");
            if (ImGui::IsItemDeactivatedAfterEdit()) commit(before, "Orbital");
        }
        dragF("Radial speed", "Radial", &fx->radial, 0.02f, -50.0f, 50.0f,
              "%.2f m/s",
              "Positive = away from the centre, negative = implosion.");
        check("Force over lifetime", "ForceOn", &fx->forceEnabled,
              "A constant acceleration added on top of gravity.");
        if (fx->forceEnabled) {
            label("Force (m/s2)", "Same units as gravity.");
            const ecs::ParticleEmitterComponent before = *fx;
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::DragFloat3("##force", glm::value_ptr(fx->force), 0.02f,
                              -50.0f, 50.0f, "%.2f");
            if (ImGui::IsItemDeactivatedAfterEdit()) commit(before, "Force");
        }
        check("Limit velocity", "LimitOn", &fx->limitVelEnabled,
              "Clamp the speed to a maximum (air resistance).");
        if (fx->limitVelEnabled) {
            dragF("Max speed", "LimitSpeed", &fx->limitSpeed, 0.02f, 0.0f, 100.0f,
                  "%.2f m/s", "Speed ceiling.");
            dragF("Dampen", "LimitDampen", &fx->limitDampen, 0.01f, 0.0f, 1.0f,
                  "%.2f", "Fraction of the excess speed lost per second.");
        }
        check("Noise (turbulence)", "NoiseOn", &fx->noiseEnabled,
              "Curl-noise field - smoke drifting, snow fluttering.");
        if (fx->noiseEnabled) {
            dragF("Noise strength", "NoiseStrength", &fx->noiseStrength, 0.02f,
                  0.0f, 50.0f, "%.2f", "How hard the field pushes (m/s2).");
            dragF("Noise frequency", "NoiseFreq", &fx->noiseFrequency, 0.01f,
                  0.0f, 8.0f, "%.2f", "Lower = bigger, lazier swirls.");
            dragF("Noise scroll", "NoiseScroll", &fx->noiseScrollSpeed, 0.01f,
                  0.0f, 10.0f, "%.2f", "Speed the noise field itself drifts.");
            dragI("Noise octaves", "NoiseOctaves", &fx->noiseOctaves, 1, 4,
                  "More octaves = finer detail (and more cost).");
        }
    }

    // ============================================================ Rotation
    if (ImGui::CollapsingHeader(TR("Rotation"))) {
        logRect("FX-MOD-ROT", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        dragF("Start rotation", "StartRotation", &fx->startRotation, 1.0f, 0.0f,
              360.0f, "%.0f deg", "Initial spin of the quad.");
        dragF("Rotation variance", "StartRotationVar",
              &fx->startRotationVariance, 1.0f, 0.0f, 360.0f, "+/- %.0f deg",
              "Random spread added to the start rotation.");
        dragF("Angular velocity", "AngularVelocity", &fx->angularVelocity, 1.0f,
              -720.0f, 720.0f, "%.0f deg/s", "Spin over the particle's life.");
    }

    // ============================================================ Renderer
    if (ImGui::CollapsingHeader(TR("Renderer"),
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        logRect("FX-MOD-RENDERER", ImGui::GetItemRectMin(),
                ImGui::GetItemRectMax());
        dragF("Start size", "StartSize", &fx->startSize, 0.002f, 0.002f, 5.0f,
              "%.3f m", "Edge length of the quad (the sprite is a disc, so this\n"
                        "is a diameter).");
        // ---- 贴图 / 资源槽 ----
        // 这一格在 UE5 里是 Renderer ▸ Material，在 Blender 里是
        // Render ▸ Material。本引擎的粒子 pass 没有贴图槽（见
        // ecs::ParticleSprite 的注释），所以这里给的是**内置形状库**：
        // 真实改变像素的轮廓，不是只存一个名字。
        static const char* kSpriteNames[] = {"Disc (soft)", "Glow", "Spark",
                                             "Smoke puff", "Ring", "Streak",
                                             "Flake"};
        int spr = static_cast<int>(fx->sprite);
        combo("Sprite", "SpriteShape", &spr, kSpriteNames, 7,
              "The silhouette of every particle of this emitter.\n"
              "Disc = the soft round sprite (default). Glow adds an outer\n"
              "halo, Spark is a 4-point star, Streak is a vertical bar\n"
              "(rain), Flake is a 6-arm snowflake.\n"
              "Custom PNG textures need a texture array in the particle\n"
              "pass - not wired up yet.");
        if (spr != static_cast<int>(fx->sprite)) {
            const ecs::ParticleEmitterComponent before = *fx;
            fx->sprite = static_cast<ecs::ParticleSprite>(spr);
            commit(before, "Sprite Shape");
        }
        dragF("Emissive", "Emissive", &fx->emissive, 0.02f, 0.0f, 40.0f, "%.2f",
              "Brightness. Particles are self-lit, so this is the only light\n"
              "term they get - above ~3 it starts to bloom.");
        static const char* kRenderNames[] = {"Billboard", "Stretched Billboard",
                                             "Horizontal", "Vertical"};
        int rmode = static_cast<int>(fx->renderMode);
        combo("Render mode", "RenderMode", &rmode, kRenderNames, 4,
              "Billboard: faces the camera.\nStretched: the long axis follows\n"
              "the velocity (rain, sparks).");
        if (rmode != static_cast<int>(fx->renderMode)) {
            const ecs::ParticleEmitterComponent before = *fx;
            fx->renderMode = static_cast<ecs::ParticleRenderMode>(rmode);
            commit(before, "Render Mode");
        }
        dragF("Stretch", "Stretch", &fx->stretchScale, 0.005f, 0.0f, 5.0f, "%.2f",
              "Stretched mode only: quad length = |v| * this.");
        static const char* kBlendNames[] = {"Additive", "Alpha"};
        int blend = static_cast<int>(fx->blend);
        combo("Blend", "Blend", &blend, kBlendNames, 2,
              "Additive adds light (fire, sparks, lightning).\nAlpha covers what\n"
              "is behind it (smoke, dust, rain).");
        if (blend != static_cast<int>(fx->blend)) {
            const ecs::ParticleEmitterComponent before = *fx;
            fx->blend = static_cast<ecs::ParticleBlend>(blend);
            commit(before, "Blend Mode");
        }
        static const char* kSortNames[] = {"None", "By Distance", "Oldest First",
                                           "Youngest First"};
        int smode = static_cast<int>(fx->sortMode);
        combo("Sort mode", "SortMode", &smode, kSortNames, 4,
              "Only matters for Alpha blending (additive is order-independent).");
        if (smode != static_cast<int>(fx->sortMode)) {
            const ecs::ParticleEmitterComponent before = *fx;
            fx->sortMode = static_cast<ecs::ParticleSortMode>(smode);
            commit(before, "Sort Mode");
        }
        check("Soft particles", "SoftParticles", &fx->softParticles,
              "Fade out as the particle approaches geometry, so it never cuts\n"
              "a hard edge on a wall.");
        if (fx->softParticles) {
            dragF("Soft distance", "SoftDistance", &fx->softDistance, 0.01f, 0.0f,
                  5.0f, "%.2f m", "Distance over which the fade happens.");
        }
    }

    // 这一整段用了 12 个模块标题，收起状态由 ImGui 自己记在窗口里；
    // 面板窗口关闭重开时回到默认（Main / Emission / Colour / Renderer 开）。
}

// ---------------------------------------------------------------- 面板

void ParticlesPanel::draw() {
    bool& open = panelOpenRef("particles");
    if (!open) return;

    // 2026-10-21：视口标签页的一页。托管时只用内容区矩形，浮动默认值
    // （以及下面那条最小尺寸约束）一律不参与 —— 约束会让窗口顶出内容区。
    ImGuiWindowFlags wf = 0;
    const bool docked = applyDockHint(wf);
    if (!docked) {
        const ImGuiIO& io = ImGui::GetIO();
        // 430x780 只在"第一次出现"时生效；老用户 imgui.ini 里存着更小的尺寸
        // （380x520 那版）时就轮不到它了，属性行会被挤成一团。所以再加一条
        // **最小尺寸约束** —— 它每帧都生效，会把存档里的尺寸顶上去。
        ImGui::SetNextWindowSizeConstraints(ImVec2(430.0f, 560.0f),
                                            ImVec2(FLT_MAX, FLT_MAX));
        ImGui::SetNextWindowSize(ImVec2(470.0f, 780.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.34f,
                                       io.DisplaySize.y * 0.10f),
                                ImGuiCond_FirstUseEver);
    }

    if (!ImGui::Begin(trId("Particles", "panel_particles"),
                      docked ? nullptr : &open, wf)) {
        ImGui::End();
        return;
    }
    logRect("FX-WIN", ImGui::GetWindowPos(),
            ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(),
                   ImGui::GetWindowPos().y + ImGui::GetWindowHeight()));

    render::Renderer& r = m_renderer;
    fx::ParticleSettings& ps = r.particleSettings();
    scene::Scene& sc = m_ctx.editorScene();

    // ---------------------------------------------------------- 全局开关
    if (ImGui::Checkbox(TR("Particle system enabled"), &ps.enabled)) {
        if (void* pp = render::paramPtr("fx.enabled")) {
            *static_cast<bool*>(pp) = ps.enabled;
        }
        VK_LOG_INFO("FX-PANEL enabled=%d", ps.enabled ? 1 : 0);
    }
    tagItem("FX-SET enabled");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(TR("Master switch: off = the GPU simulation is skipped\n"
                          "entirely (no particles are drawn at all)."));
    }
    ImGui::SameLine();
    ImGui::TextDisabled(TR("%u emitter(s) live, %u slots each"),
                        r.particleEmitterCount(), ps.perEmitterCapacity);

    // ---------------------------------------------------------- 场景清单
    ImGui::SeparatorText(TR("Emitters in this scene"));
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(TR("Right-click a row to rename, duplicate or delete it."));
    }

    int count = 0;
    unsigned sig = 0;
    ImGui::BeginChild("##fx_list", ImVec2(-1.0f, 156.0f), ImGuiChildFlags_Borders);
    sc.world().each<ecs::ParticleEmitterComponent>(
        [&](ecs::Entity e, ecs::ParticleEmitterComponent& c) {
            const int idx = count++;
            const std::string* nm = sc.world().name(e);
            char label[128];
            std::snprintf(label, sizeof(label), TR("%s   %.0f/秒  %.2f 秒"),
                          nm ? nm->c_str() : TR("Emitter"),
                          static_cast<double>(c.rate),
                          static_cast<double>(c.lifetime));
            sig = sig * 131u + static_cast<unsigned>(e.id) + 1u;
            if (emitterRow(e, label, idx)) {
                m_ctx.select(e);
                VK_LOG_INFO("FX-PANEL select entity=%u", e.id);
            }
        });
    if (count == 0) {
        ImGui::TextDisabled(TR("No particle emitter in this scene yet.\n"
                               "Use a preset below to add one."));
    }
    ImGui::EndChild();
    if (sig != m_lastListSig) {
        m_lastListSig = sig;
        VK_LOG_INFO("FX-PANEL list=%d live=%u", count, r.particleEmitterCount());
    }

    // ---------------------------------------------------------- 预设
    ImGui::SeparatorText(TR("Add a preset"));
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(TR("Fire / Smoke / Sparks appear at the world origin.\n"
                          "Rain / Snow / Lightning are raised 6-8 m so they\n"
                          "have room to fall."));
    }
    for (int i = 0; i < kPresetCount; ++i) {
        if (i % 3) ImGui::SameLine();
        ImGui::PushID(900 + i);
        if (ImGui::Button(TR(kPresets[i].label), ImVec2(104.0f, 0.0f))) {
            addPreset(i);
        }
        ImGui::PopID();
        char tag[40];
        std::snprintf(tag, sizeof(tag), "FX-PRESET %d", i);
        tagItem(tag);
    }
    ImGui::TextDisabled(TR("Fire / Smoke / Sparks / Rain / Snow / Lightning -\n"
                           "tuned emitters you can then edit parameter by\n"
                           "parameter in the Inspector."));

    // ---------------------------------------------------------- 选中发射器
    //
    // 2026-10-18 用户要求："点击 Smoke 1 下方显示的就应该是 Smoke 1 的属性
    // 配置"。所以这一段的数据源是**当前选中的那个实体**，不是全局设置 ——
    // 详细理由见 drawSelectedEmitter 头部的注释。
    {
        ecs::Entity sel = m_ctx.selection();
        ecs::ParticleEmitterComponent* sfx =
            sel.valid() ? sc.world().get<ecs::ParticleEmitterComponent>(sel)
                        : nullptr;
        drawSelectedEmitter(sel, sfx);
    }

    // ---------------------------------------------------------- 全局调参
    ImGui::SeparatorText(TR("Global"));
    const auto row = [](const char* label, const char* tip) {
        ImGui::TextUnformatted(TR(label));
        if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", TR(tip));
        ImGui::SameLine(kLabelW);
        ImGui::SetNextItemWidth(-1.0f);
    };

    row("Intensity", "Global brightness multiplier on particle colour");
    if (ImGui::DragFloat("##fx_int", &ps.intensity, 0.02f, 0.0f, 10.0f, "%.2f")) {
        VK_LOG_INFO("FX-PANEL intensity=%.2f", static_cast<double>(ps.intensity));
    }
    row("Size scale", "Global size multiplier (fades the whole effect up/down)");
    if (ImGui::DragFloat("##fx_size", &ps.sizeScale, 0.01f, 0.05f, 10.0f, "%.2f")) {
        VK_LOG_INFO("FX-PANEL sizeScale=%.2f", static_cast<double>(ps.sizeScale));
    }
    row("Softness", "Soft-particle fade distance multiplier");
    if (ImGui::DragFloat("##fx_soft", &ps.softness, 0.01f, 0.0f, 5.0f, "%.2f")) {
        VK_LOG_INFO("FX-PANEL softness=%.2f", static_cast<double>(ps.softness));
    }
    row("Simulation", "Fixed simulation step in Hz - 60 is Unity/Blender's default");
    int hz = static_cast<int>(ps.simHz);
    if (ImGui::SliderInt("##fx_hz", &hz, 20, 120, TR("%d Hz"))) {
        ps.simHz = static_cast<uint32_t>(hz);
        VK_LOG_INFO("FX-PANEL simHz=%d", hz);
    }
    row("Slots / emitter",
        "Per-emitter particle capacity. Changing it reallocates the GPU\n"
        "buffers on the next frame.");
    int cap = static_cast<int>(ps.perEmitterCapacity);
    if (ImGui::SliderInt("##fx_cap", &cap, 64, 4096)) {
        ps.perEmitterCapacity = static_cast<uint32_t>(cap);
        VK_LOG_INFO("FX-PANEL capacity=%d", cap);
    }

    ImGui::End();
}

} // namespace editor
