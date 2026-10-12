#version 450
// ============================================================
// FXAA 3.11（Quality 快速版，11 taps + 平坦区 early-out）
//
// 为什么放在 tonemap 之后：FXAA 的阈值与权重都是在**显示域**（约 sRGB）
// 上调的 —— HDR 值域上它的对比度判定完全失效。输入这张图是 tonemap 后
// 写入 sRGB 交换链格式（B8G8R8A8_SRGB）的 LDR 图，采样时硬件解码回线性
// —— 所以先对采样值做 pow(1/2.2) 近似还原显示域，用显示域 luma 做
// 边缘检测与方向估计；**颜色混合仍然用原始线性值**（线性插值才是物理
// 正确的半像素覆盖），最后写出时硬件再编码回 sRGB。
//
// 为什么加 early-out：lMax-lMin 低于阈值的像素直接原样输出 —— 平坦区
// 严格逐位恒等，一是省掉无意义的混合，二是让验证脚本可以断言
// "平坦区逐像素相同、差异只出现在高对比边缘"。
//
// taps 预算：5 次采样取 luma（中心 + 四角），4 次采样做沿边混合，
// 中心 rgb 复用 —— 共 9 次纹理读取。Quality 完整版的沿边步进循环
// （12+ taps）在 4x MSAA 已经存在的管线里收益递减，快速版够用。
// ============================================================

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D colorTex;

// x = 1/宽, y = 1/高（drawFullscreen 的 4-float push constant）
layout(push_constant) uniform PC {
    vec4 params;
} pc;

// 绝对下限：低于它的对比度当噪声跳过（暗部不闪）
const float EDGE_MIN = 0.0500;
// 方向估计的防抖项：四角 luma 均值的 1/32（FXAA quality 版经典系数）
const float DIR_REDUCE = 0.03125;
// 步进方向的最大 texel 数（quality 简版的搜索宽度）
const float SEARCH_CLAMP = 8.0;

// 显示域 luma：先近似还原 sRGB 再加权 —— 直接对线性值加权会让
// 暗部边缘（线性 luma 差极小、显示上很明显）漏检。
float displayLuma(vec3 linearRGB) {
    return dot(pow(linearRGB, vec3(1.0 / 2.2)),
               vec3(0.2126, 0.7152, 0.0722));
}

void main() {
    const vec2 texel = pc.params.xy;

    const vec3 rgbM = texture(colorTex, vUV).rgb;
    const float lM = displayLuma(rgbM);

    const float lNW = displayLuma(texture(colorTex, vUV + texel * vec2(-1.0, -1.0)).rgb);
    const float lNE = displayLuma(texture(colorTex, vUV + texel * vec2( 1.0, -1.0)).rgb);
    const float lSW = displayLuma(texture(colorTex, vUV + texel * vec2(-1.0,  1.0)).rgb);
    const float lSE = displayLuma(texture(colorTex, vUV + texel * vec2( 1.0,  1.0)).rgb);

    const float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));
    const float lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));

    // ---- 平坦区 early-out：严格恒等，一个 LSB 都不碰 ----
    if (lMax - lMin < EDGE_MIN) {
        outColor = vec4(rgbM, 1.0);
        return;
    }

    // ---- 边方向估计：对角 luma 差的反向 ----
    // dir 是"垂直于边缘"的方向（朝对比度变化最快的那边），后面沿它
    // 取混合点。x 用 NW+NE 与 SW+SE 之差 —— 上下两半不对称 → 水平
    // 分量非零，以此类推。
    vec2 dir = vec2(-((lNW + lNE) - (lSW + lSE)),
                     ((lNW + lSW) - (lNE + lSE)));
    const float dirReduce = max((lNW + lNE + lSW + lSE) * DIR_REDUCE * 0.25,
                                EDGE_MIN * 0.5);
    const float rcpDirMin =
        1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    // 归一化到"最多走 SEARCH_CLAMP 个 texel"，防极端对比下飞出老远
    dir = clamp(dir * rcpDirMin, vec2(-SEARCH_CLAMP),
                vec2(SEARCH_CLAMP)) * texel;

    // ---- 沿边混合：1/3、2/3 两个点先求平均，再补端点成 4 点加权 ----
    const vec3 rgbA = 0.5 * (texture(colorTex, vUV + dir * (1.0 / 3.0 - 0.5)).rgb +
                             texture(colorTex, vUV + dir * (2.0 / 3.0 - 0.5)).rgb);
    const vec3 rgbB = rgbA * 0.5 +
                      0.25 * (texture(colorTex, vUV + dir * -0.5).rgb +
                              texture(colorTex, vUV + dir *  0.5).rgb);
    const float lB = displayLuma(rgbB);

    // 混出来的 luma 若越出本像素邻域的 [lMin, lMax]，说明方向估歪了
    // —— 退回保守的 rgbA（只取 1/3、2/3 两点，窄一些但不会过冲）。
    outColor = vec4((lB < lMin || lB > lMax) ? rgbA : rgbB, 1.0);
}
