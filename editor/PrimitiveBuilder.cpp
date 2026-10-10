#include "PrimitiveBuilder.h"

#include "assets/AssetManager.h"

#include <cstdio>

namespace editor {

namespace {

// 每种图元的默认值 —— 和 EditorScene::createPrimitive 里原来的硬编码
// 参数一一对应（cube 1.0 / sphere r0.5 / cylinder r0.5 / plane 2.0）。
constexpr float kDefaultSize = 1.0f;
constexpr float kDefaultRadius = 0.5f;
constexpr int kDefaultSegments = 48;
constexpr int kDefaultRings = 24;

} // namespace

std::string primitiveKey(const ecs::MeshPrimitiveParams& p) {
    char buf[96];

    switch (p.primitive) {
        case ecs::MeshPrimitive::Cube:
            std::snprintf(buf, sizeof(buf), "builtin/cube/s%.4f", p.size);
            return buf;
        case ecs::MeshPrimitive::Plane:
            std::snprintf(buf, sizeof(buf), "builtin/plane/s%.4f", p.size);
            return buf;
        case ecs::MeshPrimitive::Sphere:
            std::snprintf(buf, sizeof(buf), "builtin/sphere/r%.4f/g%d/n%d",
                          static_cast<double>(p.radius), p.segments, p.rings);
            return buf;
        case ecs::MeshPrimitive::Cylinder:
            std::snprintf(buf, sizeof(buf), "builtin/cylinder/r%.4f/g%d",
                          static_cast<double>(p.radius), p.segments);
            return buf;
        case ecs::MeshPrimitive::None:
            break;
    }
    return {};
}

void rebuildPrimitive(assets::AssetManager& am, ecs::MeshComponent& mc) {
    ecs::MeshPrimitiveParams& p = mc.params;
    const std::string key = primitiveKey(p);
    if (key.empty()) return;  // None：几何由文件决定，没有参数可改

    // 下限兜底：分段数太小会生成退化几何（甚至除零），Inspector 那边也
    // 限了范围，这里再挡一次，防止脚本 / 场景文件里塞进脏数据。
    const int segments = p.segments < 3 ? 3 : p.segments;
    const int rings = p.rings < 2 ? 2 : p.rings;

    switch (p.primitive) {
        case ecs::MeshPrimitive::Cube:
            mc.mesh = am.cube(p.size, key);
            break;
        case ecs::MeshPrimitive::Plane:
            mc.mesh = am.plane(p.size, key);
            break;
        case ecs::MeshPrimitive::Sphere:
            mc.mesh = am.sphere(p.radius, key, segments, rings);
            break;
        case ecs::MeshPrimitive::Cylinder:
            mc.mesh = am.cylinder(p.radius, key, segments);
            break;
        case ecs::MeshPrimitive::None:
            break;
    }
}

ecs::MeshPrimitiveParams defaultPrimitiveParams(ecs::MeshPrimitive kind) {
    ecs::MeshPrimitiveParams p;
    p.primitive = kind;
    p.size = kDefaultSize;
    p.radius = kDefaultRadius;
    p.segments = kDefaultSegments;
    p.rings = kDefaultRings;
    return p;
}

} // namespace editor
