#include "scene/BlendCurve.h"

#include <algorithm>
#include <cstring>

namespace scene {

float blendCurveApply(BlendCurve c, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    switch (c) {
    case BlendCurve::Linear:
        return t;
    case BlendCurve::Smooth:
        // ⚠⚠ 与 `CameraSystem.cpp` 里原来那个 `smoothStep()` 逐位一致
        //    （同一条表达式、同一种求值顺序）。差一个运算次序就可能差 1 ulp，
        //    而"默认曲线逐位没变"是这次改动唯一的回归保险。
        return t * t * (3.0f - 2.0f * t);
    case BlendCurve::EaseIn:
        return t * t;
    case BlendCurve::EaseOut: {
        const float u = 1.0f - t;
        return 1.0f - u * u;
    }
    case BlendCurve::Smoother:
        // Perlin 的 quintic：两端一阶 + 二阶导都为 0。
        // 写成 t³(6t²−15t+10) —— 与 `t*t*t*(t*(t*6−15)+10)` 同值。
        return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
    }
    return t;
}

const char* blendCurveName(BlendCurve c) {
    switch (c) {
    case BlendCurve::Linear: return "linear";
    case BlendCurve::Smooth: return "smooth";
    case BlendCurve::EaseIn: return "easein";
    case BlendCurve::EaseOut: return "easeout";
    case BlendCurve::Smoother: return "smoother";
    }
    return "smooth";
}

bool blendCurveFromName(const char* s, BlendCurve& out) {
    if (!s) return false;
    // 大小写不敏感 + 忽略 `ease_in` / `ease-in` 两类分隔符：作者手写文件时
    // 三种写法都会出现，而"手滑打了连字符就被静默退回默认曲线"极难发现
    // （画面照常混，只是形状不对）。
    char buf[24];
    std::size_t n = 0;
    for (const char* p = s; *p && n + 1 < sizeof(buf); ++p) {
        const char ch = *p;
        if (ch == '_' || ch == '-' || ch == ' ') continue;
        buf[n++] = (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch;
    }
    buf[n] = '\0';
    if (std::strcmp(buf, "linear") == 0) { out = BlendCurve::Linear; return true; }
    if (std::strcmp(buf, "smooth") == 0) { out = BlendCurve::Smooth; return true; }
    if (std::strcmp(buf, "easein") == 0) { out = BlendCurve::EaseIn; return true; }
    if (std::strcmp(buf, "easeout") == 0) { out = BlendCurve::EaseOut; return true; }
    if (std::strcmp(buf, "smoother") == 0) { out = BlendCurve::Smoother; return true; }
    return false;
}

} // namespace scene
