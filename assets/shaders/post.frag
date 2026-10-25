#version 450
// ============================================================
// post.frag —— 后处理合成（Tonemap + 相机显示域效果）
//
// 输入：HDR 颜色（set 0）+ Bloom 图（set 1）
// 输出：交换链 / 离屏图（SRGB 格式 → 硬件自动做线性→sRGB 编码）
//
// 因为目标是 SRGB 格式，这里必须输出**线性**颜色，绝不能手工再做一次
// gamma，否则会出现"亮部发灰、暗部发白"的双重校正。
//
// Tonemapping 把 HDR 的无界亮度压回 [0,1]：
//   ACES   —— 电影感，高光滚降柔和（默认）
//   Reinhard —— 简单，对比度低
//   Clamp  —— 直通，用来对照观察 HDR 截断
//
// 相机显示域效果（tonemap 之后、写在 SRGB 之前）：
//   色差    —— RGB 三通道沿径向分离（镜头色散的廉价近似）
//   暗角    —— 越靠边缘越暗
// 两者强度都为 0 时（色差默认 0）走与引入本模块**逐像素相同**的路径 ——
// 这是像素级回归的硬要求。
//
// 色彩分级 LUT（R1-3）插在 tonemap 与暗角之间：
//   信号已经是显示域 [0,1]，正是 3D LUT 约定的定义域；暗角属于"镜头"
//   而非"调色"，压在 LUT 之后只衰减亮度、不改色相。
//   关闭时（pc.lut.z <= 0.5）**整段跳过**，与引入前逐像素相同。
//
// 动态分辨率升采样（R1-4）：
//   开 DRS 时场景（HDR / 链输出）按 renderScale 缩小渲染，本 Pass 仍写
//   全分辨率的输出 —— 归一化 UV + 线性采样器给出双线性升采样，再叠一道
//   **对比度自适应的边缘感知锐化**（TSR/DLSS 之前各家引擎的 "TAAU"
//   空间分量；时间分量由 TAA 的抖动历史在渲染分辨率上累积提供）。
//   关闭（scale=1，pc.ups.z <= 0.5）→ **单次采样，与引入前逐字相同**。
//
// 描边（NPR，2026-10-25）：
//   输入是 set 3 的几何缓冲（rgb=世界法线，a=视图空间线性距离，米），
//   由**深度预通道**顺带写出 —— 不额外画一遍几何。
//   判据两路取 max：相对深度差 + 法线夹角。两者都过 smoothstep 软化，
//   否则边缘是一条硬邦邦的 1 像素线（在 MSAA/FXAA 下会出现摩尔纹）。
//   关闭（pc.outline.x <= 0.5）→ **整段跳过**，与引入前逐像素相同。
// ============================================================

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D hdrTex;
layout(set = 1, binding = 0) uniform sampler2D bloomTex;
// 色彩分级 LUT（32³ 3D 纹理，数组纹理不参与 —— 这是硬件三线性）
layout(set = 2, binding = 0) uniform sampler3D lutTex;
// 几何缓冲（深度预通道写出）：rgb = 世界法线（-1..1），a = 视图空间
// 线性距离（米）。**用最近邻采样器**（C++ 侧 m_geoNearestSampler）——
// 线性过滤会把物体边缘的深度插成中间值，一条轮廓被检出两条线。
layout(set = 3, binding = 0) uniform sampler2D geoTex;

// 推常量（96B，与 PostProcess::recordComposite 的 post[24] 逐字段一致）
layout(push_constant) uniform PC {
    vec4 params;     // x=曝光 y=bloom 强度 z=tonemap 模式 w=暗角强度
    vec4 camfx;      // x=色差强度 y=暗角径向幂 z/w=保留
    vec4 lut;        // x=LUT 缩放 y=LUT 偏置 z=开关(>0.5) w=混合权重
    vec4 ups;        // x/y=源纹素(1/渲染分辨率) z=升采样开关(>0.5) w=锐度
    vec4 outline;    // x=描边开关 y=宽度(像素) z=深度阈值 w=法线阈值
    vec4 outlineRgb; // rgb=线颜色（显示域） a=强度
} pc;

// ---- 描边：单侧边缘度量 ----
// 返回 0 = 两侧是同一表面（连续），1 = 硬边或轮廓。
//
// 为什么深度用**相对差**而不是绝对差：
//   透视投影下 1 像素对应的世界距离随距离线性增长。绝对差阈值必然
//   "近处过敏感（皮肤纹理都成边）、远处完全失灵（人物没有描边）"。
//   除以 min(a) 之后，1 米处 2cm 的台阶与 50 米处 1m 的台阶同量级。
float geoEdge(vec4 gc, vec4 gn) {
    const bool vc = gc.a > 1e-4;
    const bool vn = gn.a > 1e-4;
    if (!vc && !vn) return 0.0;   // 两边都是天空
    if (vc != vn) return 1.0;     // 物体 vs 天空 → 轮廓（天空 a 被清成 0）

    const float dRel = abs(gc.a - gn.a) / max(min(gc.a, gn.a), 1e-3);
    // 1-dot：0 = 同向、1 = 垂直、2 = 反向。用 1-dot 而非 acos 是因为
    // 它单调、无超越函数，且阈值可以直接表达成"夹角多少度"。
    const float nRel = 1.0 - dot(gc.rgb, gn.rgb);

    // 软化：硬阈值在 MSAA/FXAA 下会抖成虚线。深度那一侧上界取 3x
    // （掠射角地面的相对深度差是个常数、贴着阈值时会闪烁；宽过渡带把
    // 它压成很淡的一层而不是刺眼的散点）；法线取 1.6x（更窄，否则
    // 圆球会被描成一片）。
    const float dm = smoothstep(pc.outline.z, pc.outline.z * 3.0, dRel);
    const float nm = smoothstep(pc.outline.w, pc.outline.w * 1.6, nRel);
    return max(dm, nm);
}

// ACES filmic 近似（Narkowicz 2015）—— 便宜且观感接近正式 ACES
vec3 acesFilm(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

vec3 reinhard(vec3 x) { return x / (1.0 + x); }

// 相对亮度（Rec.709 权重）—— 用来做对比度判据，与色相无关。
float lumaOf(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

// ---- 对比度自适应的边缘感知升采样（R1-4）----
// 关（pc.ups.z <= 0.5）→ 单次 texture()，与原实现逐字相同（回归基线）。
// 开：中心 = 硬件双线性，再补一道由局部对比度调制的 unsharp：
//   · 平坦区（对比度小）→ 几乎不动（避免把噪声/闪烁当细节放大）；
//   · 边缘区（对比度大）→ 提高锐度，把双线性"摊平"的阶梯拉回来。
// 结果被**钳制到局部邻域的范围**内 → 不产生振铃（overshoot/undershoot）。
vec3 upsampleScene(vec2 uv) {
    const vec3 c = texture(hdrTex, uv).rgb;
    if (pc.ups.z <= 0.5) return c;  // 关闭 → 逐字同原路径

    const vec2 tx = pc.ups.xy;
    const vec3 n = texture(hdrTex, uv + vec2(0.0, -tx.y)).rgb;
    const vec3 s = texture(hdrTex, uv + vec2(0.0, tx.y)).rgb;
    const vec3 e = texture(hdrTex, uv + vec2(tx.x, 0.0)).rgb;
    const vec3 w = texture(hdrTex, uv + vec2(-tx.x, 0.0)).rgb;

    const vec3 blur = (n + s + e + w) * 0.25;

    const float lc = lumaOf(c);
    const float lo = min(min(lumaOf(n), lumaOf(s)),
                         min(lumaOf(e), lumaOf(w)));
    const float hi = max(max(lumaOf(n), lumaOf(s)),
                         max(lumaOf(e), lumaOf(w)));
    const float range = max(hi - lo, 0.0);
    // range ∈ [0,∞) → [0,1)：小对比度趋 0、强边缘趋 1
    const float adapt = range / (range + 0.25);

    const vec3 sharp = c + (c - blur) * (pc.ups.w * adapt);
    // 抗振铃：把结果钳回"邻域 + 中心"张成的范围。
    const vec3 lo3 = min(min(n, s), min(e, w));
    const vec3 hi3 = max(max(n, s), max(e, w));
    return clamp(sharp, min(lo3, c), max(hi3, c));
}

void main() {
    // 径向坐标（靠近 0 = 画面中心）
    const vec2 d = vUv - 0.5;

    vec3 hdr;
    vec3 bloom;

    const float chroma = pc.camfx.x;
    if (chroma > 1e-6) {
        // 色差：R / B 沿径向相对 G 反向偏移。偏移量与 d 成正比 →
        // 中心无色散、边缘最强（真实镜头色散的样子）。
        const vec2 off = d * chroma;
        hdr = vec3(texture(hdrTex, clamp(vUv + off, 0.0, 1.0)).r,
                   upsampleScene(vUv).g,
                   texture(hdrTex, clamp(vUv - off, 0.0, 1.0)).b);
        bloom = vec3(texture(bloomTex, clamp(vUv + off, 0.0, 1.0)).r,
                     texture(bloomTex, vUv).g,
                     texture(bloomTex, clamp(vUv - off, 0.0, 1.0)).b);
    } else {
        // 关闭路径：与原实现逐字相同（保证逐像素不变）。
        // upsampleScene 在 ups.z<=0.5 时就是 texture(hdrTex, vUv).rgb。
        hdr = upsampleScene(vUv);
        bloom = texture(bloomTex, vUv).rgb;
    }

    // 先叠 bloom，再统一曝光
    vec3 color = hdr + bloom * pc.params.y;
    color *= pc.params.x;

    int mode = int(pc.params.z + 0.5);
    if (mode == 0)      color = acesFilm(color);
    else if (mode == 1) color = reinhard(color);
    else                color = clamp(color, 0.0, 1.0);  // 直通（观察用）

    // ---- 色彩分级 LUT（R1-3）----
    // lutTex 是 32³ 的 3D LUT，纹素 (i,j,k) 存的是"输入 (i,j,k)/31 的
    // 期望输出"。采样坐标必须落在纹素中心：
    //     uvw = c * (N-1)/N + 1/(2N)     （N=32 → ×31/32 + 1/64）
    // 硬件三线性在纹素间插值。恒等 LUT（存的就是格点本身）在仿射函数上
    // 可被三线性**精确**重建 → 开着但参数全默认时画面几乎不变。
    if (pc.lut.z > 0.5) {
        const vec3 uvw = clamp(color, 0.0, 1.0) * pc.lut.x + pc.lut.y;
        color = mix(color, texture(lutTex, uvw).rgb, pc.lut.w);
    }

    // 暗角：越靠画面边缘越暗，弱化边缘、聚焦中心。
    // pow 的底数是 r²（0..0.5）；幂 = 1 时退化为旧公式，基线不变。
    const float r2 = dot(d, d) * 2.0;
    const float vig = 1.0 - pc.params.w * pow(max(r2, 0.0), pc.camfx.y);
    color *= clamp(vig, 0.0, 1.0);

    // ---- 描边（NPR）----
    // 位置很讲究：必须在 tonemap **之后**（线颜色是显示域值，跟着曝光
    // 浮动会"人物换个灯光线就变灰"）、在 LUT 与暗角**之后**
    // （线是风格化元素，不该被镜头暗角和调色改变）。
    if (pc.outline.x > 0.5) {
        // 8 邻域。步长取**渲染分辨率**的纹素（pc.ups.xy 现在是无条件填的，
        // 与 DRS 开关无关），所以线宽是"渲染像素"而不是"输出像素" ——
        // DRS 降分辨率时线会随之粗一档，升采样后仍是连续的细线。
        const vec2 tx = pc.ups.xy * max(pc.outline.y, 1.0);
        const vec4 gc = texture(geoTex, vUv);

        vec2 kOff[8] = vec2[8](
            vec2( 1.0,  0.0), vec2(-1.0,  0.0),
            vec2( 0.0,  1.0), vec2( 0.0, -1.0),
            vec2( 1.0,  1.0), vec2( 1.0, -1.0),
            vec2(-1.0,  1.0), vec2(-1.0, -1.0));

        float e = 0.0;
        for (int i = 0; i < 8; ++i) {
            const vec2 uv = clamp(vUv + kOff[i] * tx, vec2(0.0), vec2(1.0));
            e = max(e, geoEdge(gc, texture(geoTex, uv)));
        }
        color = mix(color, pc.outlineRgb.rgb,
                    clamp(e, 0.0, 1.0) * pc.outlineRgb.a);
    }

    outColor = vec4(color, 1.0);
}
