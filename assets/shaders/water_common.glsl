// ============================================================
// water_common.glsl —— 水面的波形场（顶点与片元**共用同一份**）
//
// 为什么必须共用：顶点着色器拿它做几何位移，片元着色器拿它的解析
// 梯度算法线。两处各写一遍的话，只要有一处改了常数，"水面的形状"
// 与"水面的反光"就会开始打架 —— 表现是高光乱飘，但几何看起来正常，
// 非常难查。
//
// 波形 = 4 个方向不同的**深水重力波**叠加（Gerstner 的线性化版本）：
//
//     h(p) = Σ Aᵢ · sin(kᵢ·(dᵢ·p) + ωᵢ·t + φᵢ)
//     ∇h   = Σ Aᵢ·kᵢ · cos(…) · dᵢ
//
// 每一项的振幅 ×0.55、波长 ×0.62 —— 高频分量衰减得快，叠出来是
// "有主涌浪 + 表面碎浪"的自然谱，而不是四条等幅正弦叠出的棋盘格。
// ω 用深水色散关系 ω = √(g·k) 的 0.35 倍：真正的 √(g·k) 在 k 大时
// 频率高到会闪（编辑器里 60 fps 下混叠），压慢以后是"缓缓推过来"的观感。
// ============================================================

#ifndef VENN_WATER_GLSL
#define VENN_WATER_GLSL

const float VENN_TAU = 6.28318530718;

// 4 项叠加后的总振幅系数（1 + 0.55 + 0.3025 + 0.166 ≈ 2.02）。
// 除以它，`waveHeight` 才是**真实的峰谷半高**（米）。
const float VENN_WAVE_NORM = 1.0 / 2.02;

// 波形场。p = 世界 xz（米），t = 秒。
// 输出 h（米）与 ∇h（无量纲，每米）。
void vennWaterWaves(vec2 p, float t, float waveH, float waveLen,
                    float flowPhase, out float h, out vec2 grad) {
    // 四个方向：主波沿 +x 略偏，其余三个散开，避免"同一方向的纹路"
    vec2 dirs[4] = vec2[4](vec2(1.00, 0.35), vec2(0.72, -0.69),
                           vec2(-0.42, 0.91), vec2(-0.96, -0.28));

    h = 0.0;
    grad = vec2(0.0);

    float amp = 1.0;
    float lambda = max(waveLen, 0.05);
    float phase = flowPhase;

    for (int i = 0; i < 4; ++i) {
        vec2 d = normalize(dirs[i]);
        const float k = VENN_TAU / lambda;
        const float omega = sqrt(9.81 * k) * 0.35;
        const float ph = k * dot(p, d) + omega * t + phase;
        h += amp * sin(ph);
        grad += amp * k * cos(ph) * d;
        amp *= 0.55;
        lambda *= 0.62;
        phase += 1.7;
    }

    h *= waveH * VENN_WAVE_NORM;
    grad *= waveH * VENN_WAVE_NORM;
}

// ---- 细波纹（毛细波 / 涟漪）----
// 纯噪声，不参与几何位移（波长只有几厘米，网格分辨率根本表达不了），
// 只贡献法线扰动 —— 这也正是"涟漪让水面亮起来"的物理来源。

float vennHash21(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

// 值噪声（双线性 + smoothstep 插值）。四角哈希 → 平滑混合。
float vennValueNoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = vennHash21(i);
    float b = vennHash21(i + vec2(1.0, 0.0));
    float c = vennHash21(i + vec2(0.0, 1.0));
    float d = vennHash21(i + vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

// 噪声梯度（中心差分）。strength 把"噪声值域 0..1"放大成法线扰动。
vec2 vennNoiseGrad(vec2 p, float eps, float strength) {
    float hx = vennValueNoise(p + vec2(eps, 0.0)) -
               vennValueNoise(p - vec2(eps, 0.0));
    float hy = vennValueNoise(p + vec2(0.0, eps)) -
               vennValueNoise(p - vec2(0.0, eps));
    return vec2(hx, hy) * (strength / (2.0 * eps));
}

#endif // VENN_WATER_GLSL
