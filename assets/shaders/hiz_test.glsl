// ============================================================
// hiz_test.glsl —— Hi-Z 遮挡测试的公共数学（R2-1 / R2-2 共用）
//
// 一份实现，两个调用者：
//   · occlusion_test.comp  —— 把结果写进 vis 标志位（CPU 回读路径）
//   · mdi_compact.comp     —— 直接把"可见"的物体紧凑化进间接绘制缓冲
//                            （GPU-driven 路径，R2-2）
// 两者必须用**逐字相同**的判据，否则"CPU 说可见、GPU 说被挡"这类
// 分歧会变成随机丢失物体，且极难定位。
//
// ---- 保守性（这是本函数最要紧的性质）----
// 只要出现下列任一情况就直接返回 false（可见），绝不剔：
//   · 有角点落在相机后方（clip.w ≤ 0）或被近/远平面裁掉；
//   · 投影出的 uv 矩形出屏（屏幕上还有别的地方能看见它，不能剔）；
//   · Hi-Z 采样值 = 1.0（该区域是天空，没有遮挡体）。
// 于是"被剔的物体"一定是**整块都躲在更近的几何后面**的 —— 渲染结果
// 不该有任何可见变化。
//
// 参数：
//   mn, mx      物体世界空间 AABB 的 min / max
//   viewProj    未抖动的 proj * view
//   hizTex      Hi-Z 金字塔（全 mip 链，采样器用 NEAREST）
//   maxLod      可用最大 mip 级（= mipCount - 1）
//   depthBias   NDC 深度偏置（越大越保守）
//   screenSize  (宽, 高) 像素
// 返回 true = 判定为"被遮挡"（可剔）。
// ============================================================

bool vennHizOccluded(vec3 mn, vec3 mx, mat4 viewProj, sampler2D hizTex,
                     float maxLod, float depthBias, vec2 screenSize) {
    vec2 uvMin = vec2(1e30);
    vec2 uvMax = vec2(-1e30);
    float zNear = 1e30;    // 最近的 NDC z

    for (int c = 0; c < 8; ++c) {
        const vec3 w = vec3((c & 1) != 0 ? mx.x : mn.x,
                            (c & 2) != 0 ? mx.y : mn.y,
                            (c & 4) != 0 ? mx.z : mn.z);
        const vec4 clip = viewProj * vec4(w, 1.0);
        if (clip.w <= 1e-6) return false;             // 角点在相机后方
        const vec3 ndc = clip.xyz / clip.w;
        if (ndc.z < -1.0 || ndc.z > 1.0) return false; // 被近/远平面裁掉
        uvMin = min(uvMin, ndc.xy);
        uvMax = max(uvMax, ndc.xy);
        zNear = min(zNear, ndc.z);
    }

    // NDC → uv（本引擎 proj[1][1] 已做 Y 翻转，uv = ndc*0.5+0.5 直接成立）
    uvMin = uvMin * 0.5 + 0.5;
    uvMax = uvMax * 0.5 + 0.5;

    if (any(lessThan(uvMin, vec2(0.0))) ||
        any(greaterThan(uvMax, vec2(1.0)))) {
        return false;   // 出屏 —— 屏幕上还有别的地方能看见它
    }

    // 选 mip：让物体在屏幕上覆盖约 1~2 纹素
    const vec2 pix = (uvMax - uvMin) * screenSize;
    float lod = floor(log2(max(max(pix.x, pix.y), 1.0)));
    lod = clamp(lod, 0.0, maxLod);

    // 取 uv 矩形 4 角 + 中心的最大 Hi-Z 值（保守：一定是该区域最远/最外）
    float hiz = textureLod(hizTex, 0.5 * (uvMin + uvMax), lod).r;
    hiz = max(hiz, textureLod(hizTex, vec2(uvMin.x, uvMin.y), lod).r);
    hiz = max(hiz, textureLod(hizTex, vec2(uvMax.x, uvMin.y), lod).r);
    hiz = max(hiz, textureLod(hizTex, vec2(uvMin.x, uvMax.y), lod).r);
    hiz = max(hiz, textureLod(hizTex, vec2(uvMax.x, uvMax.y), lod).r);

    // Hi-Z 存的是深度附件原始值 d∈[0,1]；转回 NDC z = d*2-1。
    // 该区域是天空（d≈1）→ 没有遮挡体，不剔。
    if (hiz >= 0.99999) return false;
    const float hizNdc = hiz * 2.0 - 1.0;

    // 被遮挡 ⇔ 物体的最近点仍比该区域最远的遮挡体还远
    return zNear > hizNdc + depthBias;
}
