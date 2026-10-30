#include "scene/CameraMoveKind.h"

#include <cstring>

namespace scene {

namespace {
struct MoveName {
    CameraMoveKind kind;
    const char* name;
};
constexpr MoveName kNames[] = {
    {CameraMoveKind::Pan, "Pan"},
    {CameraMoveKind::Tilt, "Tilt"},
    {CameraMoveKind::Roll, "Roll"},
    {CameraMoveKind::Dolly, "Dolly"},
    {CameraMoveKind::Truck, "Truck"},
    {CameraMoveKind::Pedestal, "Pedestal"},
    {CameraMoveKind::Orbit, "Orbit"},
    {CameraMoveKind::Crane, "Crane"},
    {CameraMoveKind::Handheld, "Handheld"},
};
} // namespace

const char* cameraMoveName(CameraMoveKind k) {
    for (const auto& n : kNames)
        if (n.kind == k) return n.name;
    return "Pan";
}

bool parseCameraMove(const std::string& s, CameraMoveKind& out) {
    // 大小写不敏感：JSON 里作者写 "pan" / "Pan" 都该认。`==` 而不是
    // `strcasecmp` —— 后者是 POSIX，MSVC 上叫 `_stricmp`，为一个三行函数
    // 引平台宏不值得。
    for (const auto& n : kNames) {
        const char* a = n.name;
        std::size_t i = 0;
        for (; i < s.size() && a[i]; ++i) {
            char c = s[i];
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            char d = a[i];
            if (d >= 'A' && d <= 'Z') d = static_cast<char>(d - 'A' + 'a');
            if (c != d) break;
        }
        if (i == s.size() && a[i] == '\0') {
            out = n.kind;
            return true;
        }
    }
    return false;
}

bool cameraMoveIsAngular(CameraMoveKind k) {
    switch (k) {
        case CameraMoveKind::Pan:
        case CameraMoveKind::Tilt:
        case CameraMoveKind::Roll:
        case CameraMoveKind::Orbit:
        case CameraMoveKind::Crane:
            return true;
        default:
            return false;
    }
}

bool cameraMoveNeedsPivot(CameraMoveKind k) {
    return k == CameraMoveKind::Orbit || k == CameraMoveKind::Crane;
}

} // namespace scene
