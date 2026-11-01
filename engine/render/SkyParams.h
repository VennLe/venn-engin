#pragma once
// ============================================================
// render/SkyParams —— 程序化天空的全部可调参数
//
// ---- 这份结构存在的原因 ----
//   天空以前是两粒颜色（天顶 / 地面）写死在 `EnvSettings` 里，
//   面板上没有任何入口，改一次要重新编译。现在它是一份**可序列化**
//   的参数：编辑器里拖滑条 → 立刻见效 → 跟场景一起落盘。
//
// ---- 与着色器的契约 ----
//   `toGpu()` 产出的 `Gpu`（8 个 vec4 = 128 字节）就是
//   `assets/shaders/sky_model.glsl` 里 `VennSkyPush` uniform block 的
//   逐字段镜像。两边的**字段顺序与含义必须完全一致** —— 错位不会
//   编译报错，只会让"云的厚度"悄悄变成"星星的数量"，症状是面板上
//   的滑条互相串台，极难定位。
//   下面每个字段后面的注释都标了它落在哪个 vec4 的哪个通道。
//
// ---- 为什么只有 8 个 vec4 ----
//   本引擎的管线布局给 push constant 的额度是 128 字节
//   （见 Renderer 里 `desc.pushConstant.size = sizeof(PushConstants)`）。
//   想加参数要先合并同类项（比如太阳颜色是从地平线色推出来的，
//   没有独立参数），不要直接往后追加。
// ============================================================

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace render {

struct SkyParams {
    // ------------------------------------------------------------
    // 1) 底色：三段渐变（天顶 / 地平线 / 地面）
    // ------------------------------------------------------------
    // ⚠ 线性 HDR，不是 sRGB。面板上的取色器要自己做 sRGB→线性。
    glm::vec3 zenith{0.55f, 0.70f, 0.95f};   // → zenith.rgb
    float     exposure = 1.0f;               // → zenith.a    整体亮度倍率

    // 地平线那一圈的颜色。二次元天空最标志性的特征是"地平线偏白、
    // 天顶饱和"——三段渐变的中间段就是为它留的。
    glm::vec3 horizon{0.21f, 0.24f, 0.31f};  // → horizon.rgb
    float     sunAngularRadius = 0.018f;     // → horizon.a   太阳视半径（弧度，≈1°）

    // 地平线以下（仰视时看到的那一层）。压暗一档写在 shader 里，
    // 这里给的是"地平线正下方"的色。
    glm::vec3 ground{0.10f, 0.09f, 0.09f};   // → ground.rgb
    float     cloudCoverage = 0.0f;          // → ground.a    云覆盖度 0..1（0 = 无云）

    // ------------------------------------------------------------
    // 2) 云层
    // ------------------------------------------------------------
    glm::vec3 cloudColor{1.0f, 1.0f, 1.0f};  // → cloudColor.rgb  云亮部
    float     cloudThickness = 0.85f;        // → cloudColor.a    不透明度 0..1

    // x = 云块尺度（1 左右正常；<0.5 是大片连天，>2 是碎云）
    // y = 云层高度（世界单位，只影响天穹投影的压缩比）
    // z = 风速（每秒推进多少，0 = 静止）
    // w = 云阴影强度 0..1（背光侧压多暗）
    float cloudScale = 1.4f;                 // → cloudParams.x
    float cloudHeight = 900.0f;              // → cloudParams.y
    float cloudSpeed = 0.0f;                 // → cloudParams.z
    float cloudShadow = 0.55f;               // → cloudParams.w

    // ------------------------------------------------------------
    // 3) 星空
    // ------------------------------------------------------------
    glm::vec3 starColor{0.90f, 0.93f, 1.0f}; // → starParams.rgb
    float     starAmount = 0.0f;             // → starParams.a   星星数量 0..1

    // ------------------------------------------------------------
    // 4) 太阳 / 月亮
    // ------------------------------------------------------------
    // 存笛卡尔方向而不是角度：shader 里要拿来点乘，面板上转成
    // "高度角 / 方位角"两个滑条（见 SkyParams.h 末尾的角度辅助函数）。
    glm::vec3 sunDirection{0.0f, 0.0f, 1.0f};// → sunParams.xyz   归一化后使用
    float     sunIntensity = 0.0f;           // → sunParams.a     0 = 不画（旧观感）

    // ------------------------------------------------------------
    // 5) 杂项
    // ------------------------------------------------------------
    float starBrightness = 0.0f;             // → misc.x   星空整体亮度 0..1
    float time = 0.0f;                       // → misc.y   动画时间（秒），驱动闪烁与风
    float cloudEdgeSharpness = 0.55f;        // → misc.z   云边锐度 0=柔雾 1=剪纸
    float horizonGlow = 0.35f;               // → misc.w   太阳光晕强度

    // ------------------------------------------------------------
    // GPU 镜像：与 sky_model.glsl 的 VennSkyPush 逐字段一致
    // ------------------------------------------------------------
    struct Gpu {
        glm::vec4 zenith;
        glm::vec4 horizon;
        glm::vec4 ground;
        glm::vec4 cloudColor;
        glm::vec4 cloudParams;
        glm::vec4 starParams;
        glm::vec4 sunParams;
        glm::vec4 misc;
    };

    Gpu toGpu() const {
        Gpu g;
        g.zenith     = glm::vec4(zenith, exposure);
        g.horizon    = glm::vec4(horizon, sunAngularRadius);
        g.ground     = glm::vec4(ground, cloudCoverage);
        g.cloudColor = glm::vec4(cloudColor, cloudThickness);
        g.cloudParams = glm::vec4(cloudScale, cloudHeight, cloudSpeed, cloudShadow);
        g.starParams = glm::vec4(starColor, starAmount);

        // 方向必须归一化：面板可能给出 (0,0,0)（滑条拖到一半）或者
        // 长度不为 1 的值，shader 里直接点乘会得到错误的太阳盘大小。
        glm::vec3 sd = sunDirection;
        const float len2 = sd.x * sd.x + sd.y * sd.y + sd.z * sd.z;
        if (len2 < 1e-8f) sd = glm::vec3(0.0f, 0.0f, 1.0f);
        else sd /= std::sqrt(len2);
        g.sunParams = glm::vec4(sd, sunIntensity);

        g.misc = glm::vec4(starBrightness, time, cloudEdgeSharpness, horizonGlow);
        return g;
    }

    // ------------------------------------------------------------
    // 预设：四套常用天气。面板上的四个按钮就是这四个函数。
    //
    // ⚠ 这里的数值都是**线性 HDR**，会经过后处理的 tonemap 才上屏。
    //   从截图里吸一个颜色填进来会偏亮 —— 取色器要做 sRGB→线性。
    // ------------------------------------------------------------

    // 晴天：蓝天白云。太阳 52° 高、偏西南。
    static SkyParams clearDay() {
        SkyParams p;
        p.zenith  = {0.16f, 0.36f, 0.82f};
        p.horizon = {0.66f, 0.80f, 0.96f};
        p.ground  = {0.42f, 0.46f, 0.42f};
        p.exposure = 1.0f;
        p.cloudColor = {1.0f, 1.0f, 1.0f};
        p.cloudCoverage = 0.42f;
        p.cloudThickness = 0.92f;
        p.cloudScale = 1.35f;
        p.cloudHeight = 900.0f;
        p.cloudSpeed = 0.015f;
        p.cloudShadow = 0.55f;
        p.cloudEdgeSharpness = 0.58f;
        p.starAmount = 0.0f;
        p.starBrightness = 0.0f;
        p.sunDirection = directionFromAngles(212.0f, 52.0f);
        p.sunIntensity = 1.0f;
        p.sunAngularRadius = 0.017f;
        p.horizonGlow = 0.30f;
        return p;
    }

    // 阴雨：满天灰云、太阳被挡掉大半、整体降曝。
    static SkyParams overcast() {
        SkyParams p;
        p.zenith  = {0.30f, 0.33f, 0.38f};
        p.horizon = {0.44f, 0.46f, 0.49f};
        p.ground  = {0.18f, 0.18f, 0.19f};
        p.exposure = 0.88f;
        p.cloudColor = {0.58f, 0.60f, 0.64f};
        p.cloudCoverage = 0.94f;     // 几乎满覆盖
        p.cloudThickness = 1.0f;
        p.cloudScale = 0.62f;        // 大片连天，不是一朵一朵
        p.cloudHeight = 700.0f;
        p.cloudSpeed = 0.05f;
        p.cloudShadow = 0.80f;
        p.cloudEdgeSharpness = 0.16f;// 软边 —— 雨天没有"云的轮廓"
        p.starAmount = 0.0f;
        p.starBrightness = 0.0f;
        p.sunDirection = directionFromAngles(200.0f, 38.0f);
        p.sunIntensity = 0.14f;
        p.sunAngularRadius = 0.02f;
        p.horizonGlow = 0.20f;
        return p;
    }

    // 夜晚：深蓝渐变 + 星场 + 一轮冷月。
    static SkyParams night() {
        SkyParams p;
        p.zenith  = {0.012f, 0.022f, 0.070f};
        p.horizon = {0.055f, 0.080f, 0.145f};
        p.ground  = {0.012f, 0.014f, 0.020f};
        p.exposure = 1.0f;
        p.cloudColor = {0.16f, 0.18f, 0.26f};
        p.cloudCoverage = 0.26f;
        p.cloudThickness = 0.70f;
        p.cloudScale = 1.1f;
        p.cloudHeight = 800.0f;
        p.cloudSpeed = 0.008f;
        p.cloudShadow = 0.65f;
        p.cloudEdgeSharpness = 0.62f;
        p.starColor = {0.90f, 0.93f, 1.0f};
        p.starAmount = 0.85f;
        p.starBrightness = 1.0f;
        p.sunDirection = directionFromAngles(120.0f, 42.0f);   // 月亮
        p.sunIntensity = 0.55f;
        p.sunAngularRadius = 0.013f;
        p.horizonGlow = 0.55f;
        return p;
    }

    // 黄昏：地平线烧起来、天顶还很深、云被镶了金边、刚冒出几颗星。
    static SkyParams dusk() {
        SkyParams p;
        p.zenith  = {0.075f, 0.105f, 0.28f};
        p.horizon = {0.92f, 0.44f, 0.20f};
        p.ground  = {0.10f, 0.075f, 0.065f};
        p.exposure = 1.05f;
        p.cloudColor = {1.0f, 0.70f, 0.46f};
        p.cloudCoverage = 0.56f;
        p.cloudThickness = 0.88f;
        p.cloudScale = 1.15f;
        p.cloudHeight = 850.0f;
        p.cloudSpeed = 0.012f;
        p.cloudShadow = 0.62f;
        p.cloudEdgeSharpness = 0.48f;
        p.starColor = {0.95f, 0.90f, 0.92f};
        p.starAmount = 0.18f;
        p.starBrightness = 0.35f;
        p.sunDirection = directionFromAngles(248.0f, 6.0f);
        p.sunIntensity = 1.5f;
        p.sunAngularRadius = 0.026f;
        p.horizonGlow = 0.70f;
        return p;
    }

    // ------------------------------------------------------------
    // 角度 ↔ 方向的换算（面板用高度角/方位角，shader 用笛卡尔）
    //
    // 约定：方位角 0° 指向 +X，逆时针（俯视）增大；高度角 0° 在地平线，
    // 90° 在天顶。与"太阳从东边升"这类直觉对得上，比让用户拖一个
    // 三维方向容易得多。
    // ------------------------------------------------------------
    static glm::vec3 directionFromAngles(float azimuthDeg, float elevationDeg) {
        const float az = azimuthDeg * 3.14159265358979f / 180.0f;
        const float el = elevationDeg * 3.14159265358979f / 180.0f;
        const float ce = std::cos(el);
        return glm::vec3(ce * std::cos(az), ce * std::sin(az), std::sin(el));
    }

    static void anglesFromDirection(const glm::vec3& dir, float& azimuthDeg,
                                    float& elevationDeg) {
        glm::vec3 d = dir;
        const float len2 = d.x * d.x + d.y * d.y + d.z * d.z;
        if (len2 < 1e-8f) d = glm::vec3(0.0f, 0.0f, 1.0f);
        else d /= std::sqrt(len2);
        elevationDeg = std::asin(std::max(-1.0f, std::min(1.0f, d.z)))
                     * 180.0f / 3.14159265358979f;
        azimuthDeg = std::atan2(d.y, d.x) * 180.0f / 3.14159265358979f;
        if (azimuthDeg < 0.0f) azimuthDeg += 360.0f;
    }
};

// 编译期守住"8 个 vec4 = 128 字节"这条契约 —— 改了结构体却忘了
// 同步 shader 的 uniform block，就是在这里红的。
static_assert(sizeof(SkyParams::Gpu) == 128,
              "SkyParams::Gpu 必须正好 128 字节（= push constant 额度，"
              "与 sky_model.glsl 的 VennSkyPush 逐字段对应）");

} // namespace render
