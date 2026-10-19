#include "ui2/Ui2Settings.h"

#include "render/FeatureRegistry.h"

#include <algorithm>

namespace ui2 {

Ui2Settings& settings() {
    static Ui2Settings s;
    return s;
}

void bindFeatureParams() {
    // 幂等：bindParam 会把"此刻的值"记成默认值，重复绑会让默认值
    // 变成"上次被 env 改过的值" → resetToDefaults() 失效。
    static bool bound = false;
    if (bound) return;
    bound = true;

    using render::ParamKind;
    Ui2Settings& s = settings();
    render::bindParam("ui.enabled", &s.enabled, ParamKind::Bool);
    render::bindParam("ui.hudInEditor", &s.hudInEditor, ParamKind::Bool);
    render::bindParam("ui.scale", &s.scale, ParamKind::Float);
    render::bindParam("ui.debugBounds", &s.debugBounds, ParamKind::Bool);
    render::bindParam("ui.fontSize", &s.fontSize, ParamKind::Float);
}

float computeScale(uint32_t outW, uint32_t outH) {
    if (outW == 0 || outH == 0) return 1.0f;   // 窗口最小化：别算出 0/NaN
    const float sx = static_cast<float>(outW) / kRefWidth;
    const float sy = static_cast<float>(outH) / kRefHeight;
    return std::min(sx, sy) * settings().scale;
}

}  // namespace ui2
