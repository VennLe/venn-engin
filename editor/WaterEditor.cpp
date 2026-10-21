#include "WaterEditor.h"

#include "DebugRects.h"
#include "EditModes.h"
#include "Localization.h"
#include "ModeRail.h"
#include "PanelRegistry.h"
#include "PickingSystem.h"

#include "core/Logger.h"
#include "physics/Heightfield.h"
#include "terrain/Terrain.h"
#include "scene/Scene.h"

#include <glm/gtc/type_ptr.hpp>

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

namespace editor {

namespace {

// 新建时水位抬高多少米。取 0.18 而不是 0：水面与地表**完全共面**时，
// 岸边那条线会因为深度精度抖动而闪（与"地形落座 2 cm"同一个原因，
// 只是水面是平的、需要的余量更大一点，因为水体顶点还带波浪位移）。
// 也不能太大 —— 抬得越多，从侧面看到的那道"水裙"越高，就越像浮在半空
// （2026-10-21 用户反馈"反重力"，原来这里是 0.35）。
constexpr float kDefaultRise = 0.18f;

// 侧壁（水体截面）从底部再往下沉多少米。地形是有起伏的，侧壁必须沉到
// 最低点以下才保证**任何**角度都被地形遮住；沉得多不花任何代价（多出来
// 的部分在土里）。
constexpr float kSkirtSink = 0.75f;

// 小于这个尺寸视为误触（拖一下就松手），不建水体。
constexpr float kMinExtent = 1.0f;

// 拖拽时水面的"可见下限"：太小的水体网格只有几个三角形，视觉上像噪点。
constexpr float kDragFloor = 0.5f;

// 在一块地形上打一条射线，取世界命中点。没有地形时返回 false。
bool hitTerrain(scene::Scene& sc, const Ray& ray,
                glm::vec3& outHit) {
    bool found = false;
    float bestT = 1e30f;
    sc.forEachTerrain([&](ecs::Entity e, ecs::TerrainComponent& tc) {
        // 与 LandscapeEditor::raycast 同一套：资产高度场 + 组件倍率
        terrain::TerrainAsset asset;
        if (!terrain::loadTerrain(tc.source, asset)) return;
        physics::Heightfield hf = asset.heightfield();
        hf.heightScale = asset.desc.maxHeight * std::max(tc.heightScale, 1.0e-4f);

        const glm::mat4 model = sc.worldMatrix(e);
        const glm::mat4 inv = glm::inverse(model);
        const glm::vec3 lo = glm::vec3(inv * glm::vec4(ray.origin, 1.0f));
        const glm::vec3 ld = glm::vec3(inv * glm::vec4(ray.dir, 0.0f));

        physics::HeightHit hit;
        if (!physics::raycast(hf, lo, ld, 0.0f, hit)) return;
        const glm::vec3 wp = glm::vec3(model * glm::vec4(hit.point, 1.0f));
        const float t = glm::length(wp - ray.origin);
        if (t < bestT) {
            bestT = t;
            outHit = wp;
            found = true;
        }
    });
    return found;
}

// 射线与水平面 z = h 的交点（地形没打中时的兜底）
bool hitPlane(const Ray& ray, float h, glm::vec3& out) {
    if (std::fabs(ray.dir.z) < 1e-5f) return false;
    const float t = (h - ray.origin.z) / ray.dir.z;
    if (t <= 0.0f || t > 5000.0f) return false;
    out = ray.origin + ray.dir * t;
    return true;
}

// ============================================================
// 地面采样器（2026-10-21 第六轮：让水体真正"坐在"地形里）
// ============================================================
// 水位贴合地形要在一个 footprint 里采上百个点。`hitTerrain` 每调一次就
// `loadTerrain` —— 那是读一次 height.r32 + 解析 JSON；采 150 个点等于十几
// 次顺序读盘，面板上拖一下半径就会卡住。
//
// 所以这里把地形**加载一次**（`GroundProbe` 的构造），之后每个点都是 O(1)
// 双线性插值。采样只在"用户松手"这一刻发生（创建 / 改完尺寸），拖动过程
// 里一律不采样 —— 这也是拖拽期间水面高度保持不变的原因。
//
// ⚠ `Heightfield::heights` 是裸指针，指向 `TerrainAsset::heights` ——
//   asset 必须活得比 hf 长，所以这里存 unique_ptr 而不是值（拷贝会悬垂）。
class GroundProbe {
public:
    explicit GroundProbe(scene::Scene& sc) {
        sc.forEachTerrain([&](ecs::Entity e, ecs::TerrainComponent& tc) {
            auto asset = std::make_unique<terrain::TerrainAsset>();
            if (!terrain::loadTerrain(tc.source, *asset, nullptr, true)) return;
            Layer layer;
            layer.asset = std::move(asset);
            layer.hf = layer.asset->heightfield();
            // 与 LandscapeEditor::raycast / hitTerrain 同一套高度口径
            layer.hf.heightScale =
                layer.asset->desc.maxHeight * std::max(tc.heightScale, 1.0e-4f);
            layer.model = sc.worldMatrix(e);
            layer.inv = glm::inverse(layer.model);
            m_layers.push_back(std::move(layer));
        });
    }

    bool valid() const { return !m_layers.empty(); }

    // 世界 xy 处的地面高度。多块地形取**最高**的那块：水面若藏到下面那块
    // 里就完全看不见了，而看得见的水才是用户想摆的东西。
    bool heightAt(const glm::vec2& xy, float& outZ) const {
        bool found = false;
        float best = -1.0e30f;
        for (const Layer& l : m_layers) {
            if (!l.hf.valid()) continue;
            const glm::vec3 lp =
                glm::vec3(l.inv * glm::vec4(xy.x, xy.y, 0.0f, 1.0f));
            const float h = physics::heightAt(l.hf, lp.x, lp.y);
            const float wz = (l.model * glm::vec4(lp.x, lp.y, h, 1.0f)).z;
            if (!found || wz > best) {
                best = wz;
                found = true;
            }
        }
        if (found) outZ = best;
        return found;
    }

private:
    struct Layer {
        std::unique_ptr<terrain::TerrainAsset> asset;
        physics::Heightfield hf;
        glm::mat4 model{1.0f};
        glm::mat4 inv{1.0f};
    };
    std::vector<Layer> m_layers;
};

// footprint 上的采样点。两种形状各给一套，点数控制在 ~150 —— 双线性插值
// 下已经足够密（再密只会让松手那一下变慢，参数面板显示的水深看不出差别）。
//
// 回调第三个参数 = "这个点在边界上"。水位跟的就是**边界**的最低点，而不是
// 整片区域的最低点：水面是一个水平面，决定它能抬多高的永远是岸线，不是
// 池底（池底再深也不该把水位拽到池底去）。
template <typename Fn>
void forEachFootprintSample(const water::WaterBody& b, Fn&& fn) {
    const float rad = b.rotation * 3.14159265f / 180.0f;
    const float c = std::cos(rad), s = std::sin(rad);
    auto place = [&](glm::vec2 local, bool rim) {
        const glm::vec2 r(local.x * c - local.y * s,
                          local.x * s + local.y * c);
        fn(b.center + r, local, rim);
    };

    if (b.shape == water::Shape::Circle) {
        const float R = std::max(b.halfExtent.x, 0.25f);
        constexpr int kSeg = 48;
        place(glm::vec2(0.0f), false);
        const float rings[3] = {0.45f, 0.75f, 1.0f};
        for (int k = 0; k < 3; ++k) {
            const bool rim = (k == 2);
            for (int i = 0; i < kSeg; ++i) {
                const float a = 6.2831853f * static_cast<float>(i) /
                                static_cast<float>(kSeg);
                place(glm::vec2(R * rings[k] * std::cos(a),
                                R * rings[k] * std::sin(a)), rim);
            }
        }
    } else {
        constexpr int kN = 7;
        for (int j = 0; j < kN; ++j) {
            for (int i = 0; i < kN; ++i) {
                const bool rim = (i == 0 || i == kN - 1 || j == 0 || j == kN - 1);
                const float fx = -1.0f + 2.0f * static_cast<float>(i) /
                                            static_cast<float>(kN - 1);
                const float fz = -1.0f + 2.0f * static_cast<float>(j) /
                                            static_cast<float>(kN - 1);
                place(glm::vec2(fx * b.halfExtent.x, fz * b.halfExtent.y), rim);
            }
        }
    }
}

struct GroundFit {
    bool ok = false;
    float minZ = 0.0f;     // 区域内最低（= 池底）
    float maxZ = 0.0f;     // 区域内最高
    float rimMinZ = 0.0f;  // 岸线最低（决定水位能抬多高）
    float centerZ = 0.0f;  // 中心处（没有地形时当基准）
};

GroundFit measureGround(const GroundProbe& probe, const water::WaterBody& b) {
    GroundFit f;
    if (!probe.valid()) return f;

    // ⚠ `rimMinZ` **不能**跟着第一个采样点初始化。圆形的第一个点是**圆心**
    //   （碗底），拿它当岸线基准会把水位拽到碗底 —— 2026-10-21 实测：
    //   176 m 的湖 rim 记成 2.00 m（碗底）而不是 14.00 m（台地），于是
    //   水面只盖住碗底一小圈。只用 rim 点算，没有 rim 点就退回 minZ。
    f.minZ = 1.0e30f;
    f.maxZ = -1.0e30f;
    f.rimMinZ = 1.0e30f;
    bool any = false;
    bool anyRim = false;
    forEachFootprintSample(b, [&](const glm::vec2& xy, const glm::vec2& local,
                                  bool rim) {
        float z = 0.0f;
        if (!probe.heightAt(xy, z)) return;
        f.minZ = std::min(f.minZ, z);
        f.maxZ = std::max(f.maxZ, z);
        if (rim) {
            f.rimMinZ = std::min(f.rimMinZ, z);
            anyRim = true;
        }
        if (!any) f.centerZ = z;
        if (glm::length(local) < 1.0e-3f) f.centerZ = z;
        any = true;
    });
    f.ok = any;
    if (!anyRim) f.rimMinZ = f.minZ;   // 理论上不会发生（每种形状都有边界点）
    return f;
}


} // namespace

// ============================================================
// 绑定
// ============================================================
void WaterEditor::sync() {
    const ecs::Entity sel = m_ctx.selection();
    const std::uint32_t id = sel.valid() ? sel.id : 0;
    if (id == m_boundId) return;
    const bool wasBound = (m_boundId != 0);
    m_boundId = id;

    const ecs::WaterComponent* wc = selected();
    if (wc) {
        // 选中一块水体 → 把它当成"新建的模板"：接着拖下一块时沿用同样的水质
        m_proto.kind = wc->body.kind;
        VK_LOG_INFO("WATER-BIND entity=%u kind=%s shape=%s level=%.2f "
                    "halfExtent=(%.1f,%.1f)",
                    id, water::kindId(wc->body.kind),
                    wc->body.shape == water::Shape::Circle ? "circle" : "rect",
                    static_cast<double>(wc->body.level),
                    static_cast<double>(wc->body.halfExtent.x),
                    static_cast<double>(wc->body.halfExtent.y));
    } else if (wasBound) {
        // 从"选中一块"回到"没选中" = 面板切回新建态。这条日志是
        // "参数面板回来了"的可断言证据（`logRect` 对不动的矩形不重复输出，
        // 脚本没法靠锚点判断控件是不是还在画）。
        VK_LOG_INFO("WATER-UNBIND");
    }
}

bool WaterEditor::hasWater() const { return selected() != nullptr; }

ecs::WaterComponent* WaterEditor::selected() {
    return const_cast<ecs::WaterComponent*>(
        static_cast<const WaterEditor*>(this)->selected());
}

const ecs::WaterComponent* WaterEditor::selected() const {
    const ecs::Entity sel = m_ctx.selection();
    if (!sel.valid()) return nullptr;
    return m_ctx.editorScene().world().get<ecs::WaterComponent>(sel);
}

// ============================================================
// 建 / 改 / 删
// ============================================================
ecs::Entity WaterEditor::createBody(const glm::vec2& center, float level,
                                   water::Shape shape,
                                   const water::WaterBody& proto,
                                   const char* why, bool fitGround) {
    scene::Scene& sc = m_ctx.editorScene();
    char name[64];
    std::snprintf(name, sizeof(name), "Water_%d", m_seq++);

    ecs::Entity created{};
    m_ctx.structuralEdit("Create Water", [&] {
        created = sc.createObject(name);
        ecs::WaterComponent wc;
        wc.body = proto;
        wc.body.shape = shape;
        wc.body.center = center;
        wc.body.level = level;
        sc.world().add<ecs::WaterComponent>(created, wc);
    });

    if (!created.valid()) return {};

    m_ctx.select(created);
    m_ctx.dirty() = true;
    m_boundId = created.id;

    // 贴地：把池底定到 footprint 内的最低地形，水位抬到岸线最低点 + 余量。
    // 拖拽路径传 false（尺寸还在变，松手时才采样 —— 见 GroundProbe）。
    ecs::WaterComponent* wcm = sc.world().get<ecs::WaterComponent>(created);
    if (wcm && fitGround) fitToGround(wcm->body, /*followLevel=*/true);

    // 日志带全部要点：验收脚本靠它判断"拖出来的水体长什么样"
    const ecs::WaterComponent* wc = wcm;
    if (wc) {
        VK_LOG_INFO("WATER-CREATE id=%u name=%s shape=%s kind=%s "
                    "center=(%.1f,%.1f) level=%.2f halfExtent=(%.1f,%.1f) "
                    "bed=%.2f depth=%.2f src=%s",
                    created.id, name,
                    wc->body.shape == water::Shape::Circle ? "circle" : "rect",
                    water::kindId(wc->body.kind),
                    static_cast<double>(wc->body.center.x),
                    static_cast<double>(wc->body.center.y),
                    static_cast<double>(wc->body.level),
                    static_cast<double>(wc->body.halfExtent.x),
                    static_cast<double>(wc->body.halfExtent.y),
                    static_cast<double>(wc->body.bedLevel),
                    static_cast<double>(wc->body.maxDepth()), why);
    }
    return created;
}

void WaterEditor::applyDrag(const glm::vec2& anchor, const glm::vec2& cur) {
    scene::Scene& sc = m_ctx.editorScene();
    auto* wc = sc.world().get<ecs::WaterComponent>(m_dragEntity);
    if (!wc) return;

    // 圆形：起点 = 圆心，拖动距离 = 半径
    // 矩形：起点与当前点是对角 → 半长取差值
    if (wc->body.shape == water::Shape::Circle) {
        const float r = std::max(glm::length(cur - anchor), kDragFloor);
        wc->body.center = anchor;
        wc->body.halfExtent = glm::vec2(r, r);
    } else {
        wc->body.center = (anchor + cur) * 0.5f;
        wc->body.halfExtent =
            glm::max(glm::abs(cur - anchor) * 0.5f, glm::vec2(kDragFloor));
    }
    // 拖动过程中**不**采样地形（见 GroundProbe 的注释）：水位与池底先跟着
    // 落笔点走，松手时 fitToGround 一次性修正。
    m_ctx.dirty() = true;
}

// ============================================================
// 贴地：水位/池底与地形对齐（2026-10-21 第六轮）
// ============================================================
void WaterEditor::fitToGround(water::WaterBody& b, bool followLevel) {
    GroundProbe probe(m_ctx.editorScene());
    const GroundFit f = measureGround(probe, b);
    const std::uint32_t id = m_ctx.selection().valid() ? m_ctx.selection().id : 0;

    if (!f.ok) {
        // 场景里没有地形（沙盒只有 mesh 地面，hitTerrain 也打不中）。
        // 这时"池底"就是水面下方一个余量 —— 平地上的真实水体本来就是薄薄
        // 一层，侧壁只有十几厘米高，正好贴着地面，不会出现悬空的水块。
        b.bedLevel = b.level - kDefaultRise;
        VK_LOG_INFO("WATER-GROUND id=%u src=flat bed=%.2f level=%.2f "
                    "depth=%.2f", id, static_cast<double>(b.bedLevel),
                    static_cast<double>(b.level),
                    static_cast<double>(b.maxDepth()));
        return;
    }

    // 池底 = 区域内**最低**地形；侧壁从这里再往下沉（见 kSkirtSink），
    // 于是任何起伏都遮得住 —— 这就是"反重力"的根治点。
    b.bedLevel = f.minZ;
    if (followLevel) {
        // 水位跟**岸线**最低点，不是池底：池底再深也不该把水面拽下去。
        // 岸线最低 + 一丁点余量 → 水面永远略低于四周地势，既不会从岸边
        // 溢出去，也不会在边缘立起一道水墙。
        b.level = f.rimMinZ + kDefaultRise;
    }
    VK_LOG_INFO("WATER-GROUND id=%u src=terrain bed=%.2f rim=%.2f max=%.2f "
                "level=%.2f depth=%.2f%s",
                id, static_cast<double>(f.minZ),
                static_cast<double>(f.rimMinZ),
                static_cast<double>(f.maxZ), static_cast<double>(b.level),
                static_cast<double>(b.maxDepth()),
                followLevel ? "" : " (level kept)");
}


// ============================================================
// 视口输入：拖出一个水体
// ============================================================
bool WaterEditor::viewportInput(const glm::vec2& vpPos,
                                const glm::vec2& vpSize, bool hovered,
                                ImDrawList* dl) {
    sync();

    const ImGuiIO& io = ImGui::GetIO();
    const glm::vec2 mouse(io.MousePos.x, io.MousePos.y);
    const Ray ray = PickingSystem::rayFromViewport(
        m_ctx.editorScene().camera(), vpPos, vpSize, mouse);

    // ⚠ 只在真正需要的时候才打射线。`hitTerrain` 要走 loadTerrain（读
    //   磁盘上的 height.r32），每帧都打的话就是每帧一次 264 KB 读盘 ——
    //   鼠标在视口里划一圈就能把帧率拖下去。拖动过程本身用"水平面"
    //   求交就够（水面本来就是一个平面）。
    const bool wantHit =
        (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) ||
        (m_dragging && !ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
         false);

    // ---- 落笔 ----
    if (hovered && !m_dragging && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        glm::vec3 hit(0.0f);
        const bool onSurface = hitTerrain(m_ctx.editorScene(), ray, hit) ||
                               hitPlane(ray, 0.0f, hit);
        if (!onSurface) return false;

        m_dragging = true;
        m_dragAnchor = glm::vec2(hit.x, hit.y);
        m_dragLevel = hit.z + kDefaultRise;
        // 先建出来（尺寸取下限），拖动时直接改它 —— 见文件头的说明
        water::WaterBody proto = m_proto;
        proto.halfExtent = glm::vec2(kDragFloor);
        m_dragEntity = createBody(m_dragAnchor, m_dragLevel, m_proto.shape,
                                  proto, "drag", /*fitGround=*/false);
        VK_LOG_INFO("WATER-DRAG-BEGIN id=%u at=(%.1f,%.1f) level=%.2f "
                    "shape=%s",
                    m_dragEntity.valid() ? m_dragEntity.id : 0u,
                    static_cast<double>(m_dragAnchor.x),
                    static_cast<double>(m_dragAnchor.y),
                    static_cast<double>(m_dragLevel),
                    m_proto.shape == water::Shape::Circle ? "circle" : "rect");
    }
    (void)wantHit;

    // ---- 拖动 ----
    if (m_dragging) {
        // 拖到哪算到哪：始终用落笔时的水位那个水平面求交，打不中就用当前水位
        glm::vec3 cur(0.0f);
        if (!hitPlane(ray, m_dragLevel, cur)) {
            cur = glm::vec3(m_dragAnchor.x, m_dragAnchor.y, m_dragLevel);
        }
        applyDrag(m_dragAnchor, glm::vec2(cur.x, cur.y));

        // 拖动中的轮廓（水面本身已经实时在画了，这条线只是"范围"提示）
        if (const ecs::WaterComponent* wc = selected()) {
            drawFootprint(dl, vpPos, vpSize, wc->body,
                          IM_COL32(120, 210, 255, 220));
        }

        // ---- 松手 ----
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            m_dragging = false;
            scene::Scene& sc = m_ctx.editorScene();
            auto* wc = sc.world().get<ecs::WaterComponent>(m_dragEntity);
            const float extent =
                wc ? std::max(wc->body.halfExtent.x, wc->body.halfExtent.y)
                   : 0.0f;
            if (wc && extent < kMinExtent) {
                // 误触（点一下就松手）→ 撤掉这块水体
                m_ctx.structuralEdit("Cancel Water", [&] {
                    scene::Scene& s2 = m_ctx.editorScene();
                    s2.world().destroy(m_dragEntity);
                });
                m_ctx.select(ecs::Entity{});
                VK_LOG_INFO("WATER-DRAG-CANCEL extent=%.2f (< %.2f m)",
                            static_cast<double>(extent),
                            static_cast<double>(kMinExtent));
            } else if (wc) {
                // 尺寸定了 → 现在才值得采样地形（一次读盘）。这一步同时
                // 决定池底（侧壁下沿）和水面高度，是"紧贴地面"的全部实现。
                fitToGround(wc->body, /*followLevel=*/wc->body.autoFit);
                VK_LOG_INFO("WATER-DRAG-END id=%u shape=%s "
                            "center=(%.1f,%.1f) level=%.2f "
                            "halfExtent=(%.1f,%.1f) bed=%.2f depth=%.2f",
                            m_dragEntity.id,
                            wc->body.shape == water::Shape::Circle ? "circle"
                                                                  : "rect",
                            static_cast<double>(wc->body.center.x),
                            static_cast<double>(wc->body.center.y),
                            static_cast<double>(wc->body.level),
                            static_cast<double>(wc->body.halfExtent.x),
                            static_cast<double>(wc->body.halfExtent.y),
                            static_cast<double>(wc->body.bedLevel),
                            static_cast<double>(wc->body.maxDepth()));
                m_ctx.notify(TR("Water body created"));
            }
            m_dragEntity = ecs::Entity{};
        }
        return true;   // 吃掉左键，视口不要再用它做点选
    }

    // ---- 光标提示：选中水体的轮廓 ----
    if (dl) {
        if (const ecs::WaterComponent* wc = selected()) {
            drawFootprint(dl, vpPos, vpSize, wc->body,
                          IM_COL32(120, 210, 255, hovered ? 170 : 90));
        }
    }
    return false;
}

void WaterEditor::drawFootprint(ImDrawList* dl, const glm::vec2& vpPos,
                                const glm::vec2& vpSize,
                                const water::WaterBody& b, ImU32 col) {
    if (!dl) return;
    const glm::mat4 vp =
        PickingSystem::viewProj(m_ctx.editorScene().camera(), vpSize);

    // 圆形 48 段；矩形 4 个角。都摆在水面高度上（z = level）。
    const int segs = (b.shape == water::Shape::Circle) ? 48 : 4;
    ImVec2 pts[49];
    int n = 0;
    for (int i = 0; i < segs; ++i) {
        glm::vec2 local;
        if (b.shape == water::Shape::Circle) {
            const float a = static_cast<float>(i) / static_cast<float>(segs) *
                            6.2831853f;
            local = glm::vec2(b.halfExtent.x * std::cos(a),
                              b.halfExtent.x * std::sin(a));
        } else if (i == 0) {
            local = glm::vec2(-b.halfExtent.x, -b.halfExtent.y);
        } else if (i == 1) {
            local = glm::vec2(b.halfExtent.x, -b.halfExtent.y);
        } else if (i == 2) {
            local = glm::vec2(b.halfExtent.x, b.halfExtent.y);
        } else {
            local = glm::vec2(-b.halfExtent.x, b.halfExtent.y);
        }
        // 绕 z 旋转
        const float rad = b.rotation * 3.14159265f / 180.0f;
        const float c = std::cos(rad), s = std::sin(rad);
        const glm::vec2 rot(local.x * c - local.y * s,
                            local.x * s + local.y * c);
        const glm::vec3 world(b.center.x + rot.x, b.center.y + rot.y, b.level);
        glm::vec2 screen;
        if (!PickingSystem::worldToScreen(vp, vpPos, vpSize, world, screen)) {
            return;
        }
        pts[n++] = ImVec2(screen.x, screen.y);
    }
    if (n >= 2) dl->AddPolyline(pts, n, col, ImDrawFlags_Closed, 1.8f);
}

// ============================================================
// 面板
// ============================================================
void WaterEditor::drawPanel() {
    bool& open = panelOpenRef("water");
    if (!open) return;

    sync();

    ImGuiWindowFlags wf = 0;
    const bool railed = beginModeRailPanel(m_ctx, wf);
    if (!railed) {
        const ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowSizeConstraints(ImVec2(400.0f, 420.0f),
                                            ImVec2(FLT_MAX, FLT_MAX));
        ImGui::SetNextWindowSize(ImVec2(460.0f, 720.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.32f,
                                       io.DisplaySize.y * 0.08f),
                                ImGuiCond_FirstUseEver);
    }
    // ID 必须是 `###panel_<面板表里的 id>`（底栏托盘按这个名字找窗口）
    if (!ImGui::Begin(TR("Water###panel_water"), railed ? nullptr : &open,
                      wf)) {
        ImGui::End();
        return;
    }
    logRect("WATER-PANEL", ImGui::GetWindowPos(),
            ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(),
                   ImGui::GetWindowPos().y + ImGui::GetWindowHeight()));

    // 面板内容在"新建 / 选中"之间切换时高度会跳，ImGui 会保留上一步的滚动
    // 位置 —— 停在中间看起来就像"面板是空的"。创建/回到新建时强制回顶。
    if (m_scrollTop) {
        ImGui::SetScrollY(0.0f);
        m_scrollTop = false;
    }

    ImGui::TextDisabled("%s", TR(editModeHint(EditMode::Water)));
    ImGui::Separator();

    // ------------------------------------------------------------
    // 分组折叠，而不是一长串（2026-10-21）
    // ------------------------------------------------------------
    // 左栏下方的参数区只有 ~400px 高，而这块面板的全部控件排下来近 1000px。
    // 一长串的直接后果是"尺寸/水位这些最常用的滑块要滚下去才点得到"，而且
    // 滚出去的控件看起来就像"功能失灵"（点了没反应）。按用途折成三段、
    // 默认只展开前两段，常用参数就都落在首屏：
    //   Shape          —— 形状 + 范围 + 水位（拖完一块水体最先调的就是它）
    //   Water quality  —— 水质预设 + 颜色 + 透明度/折射/清澈度
    //   Surface motion —— 波高/波长/涟漪/泡沫/流速（调细节时才展开）
    //   New water body —— 再摆一块（默认收起：它是"下一步动作"，不是参数）
    //
    // 锚点只在**画出来的时候**才输出（ui::logRect 对没移动的矩形不重复
    // 打印），所以收起一段 = 那段的锚点消失，自动化据此判断"这一段展开没"。
    ecs::WaterComponent* wc = selected();
    if (!wc) {
        drawCreateSection();
    } else {
        // 选中一块水体后，"新建"那一整段会被挪到最底部并默认收起 ——
        // 用户拖完第一块想再摆一块时，面板里看不到它（2026-10-21 反馈的
        // "创建水体的时候参数面板会消失不见"说的就是这个）。这里把它钉成
        // 首屏第一行，任何时候都能再建。
        if (ImGui::Button(TR("+ New water body"), ImVec2(-1.0f, 0.0f))) {
            // ⚠ 必须用 `clearSelection()` 而不是 `select(ecs::Entity{})`：
            // 后者只清句柄，**不**清 `m_selectionName`，而下一帧
            // `EditorContext::validateSelection()` 看到"没选中但有名字"就会
            // 按名字把它找回来 —— 表现是按钮点了、日志打了，但面板纹丝不动
            //（2026-10-21 实测 J2 一直 FAIL 的根因）。
            //
            // 也不要顺手把 m_boundId 清掉：`sync()` 靠"id 变了"来打
            // WATER-UNBIND，先清会让它以为没变化而早退。
            m_ctx.clearSelection();
            m_scrollTop = true;
            VK_LOG_INFO("WATER-NEW-ENTRY");
        }
        logRect("WATER-NEW-ENTRY", ImGui::GetItemRectMin(),
                ImGui::GetItemRectMax());
        ImGui::Separator();

        drawStatsSection();

        constexpr ImGuiTreeNodeFlags kHeader =
            ImGuiTreeNodeFlags_DefaultOpen |
            ImGuiTreeNodeFlags_SpanAvailWidth;

        if (ImGui::CollapsingHeader(TR("Shape"), kHeader)) {
            drawShapeSection(*wc);
        }
        logRect("WATER-SEC shape", ImGui::GetItemRectMin(),
                ImGui::GetItemRectMax());
        if (ImGui::CollapsingHeader(TR("Water quality"), kHeader)) {
            drawLookSection(*wc);
        }
        logRect("WATER-SEC quality", ImGui::GetItemRectMin(),
                ImGui::GetItemRectMax());
        // 默认收起：波高/流速这些是"调到手感对"用的，不必占首屏
        if (ImGui::CollapsingHeader(TR("Surface motion"))) {
            drawMotionSection(*wc);
        }
        logRect("WATER-SEC motion", ImGui::GetItemRectMin(),
                ImGui::GetItemRectMax());
        if (ImGui::CollapsingHeader(TR("New water body"))) {
            drawCreateSection();   // 收起时也能连续摆几块
        }
        logRect("WATER-SEC new", ImGui::GetItemRectMin(),
                ImGui::GetItemRectMax());
    }

    ImGui::End();
}

// ============================================================
// 预设选择器：一排按钮
// ============================================================
// 见 WaterEditor.h 里 drawKindButtons 的注释：Combo 的"点开再点一项"对
// 5 选 1 的枚举太重，弹出层行高还依赖主题。按钮行一次点击就到位。
//
// 锚点命名：`<tag> <id>`（如 `WATER-KIND sea`）—— 每个按钮一条，
// 自动化可以按**值**点，不用算弹出层坐标。
namespace {

// 一排 n 个等宽按钮各自的宽度（撑满面板内容区）
float rowButtonWidth(int n) {
    if (n <= 0) return 0.0f;
    const ImGuiStyle& st = ImGui::GetStyle();
    const float avail = ImGui::GetContentRegionAvail().x;
    return (avail - st.ItemSpacing.x * static_cast<float>(n - 1)) /
           static_cast<float>(n);
}

void beginActiveButton(bool active) {
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button,
                              ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImGui::GetStyleColorVec4(ImGuiCol_Text));
    }
}

void endActiveButton(bool active) {
    if (active) ImGui::PopStyleColor(2);
}

} // namespace

void WaterEditor::drawKindButtons(water::WaterBody& b, const char* tag,
                                  std::uint32_t ownerId, bool keepLayout) {
    const float w = rowButtonWidth(water::kKindCount);
    // 同一个窗口里两处都调这个函数（新建模板 / 当前水体），按钮文字一样 ——
    // 不隔离 ID 会撞 ImGui 的控件 ID（点了没反应，且只在调试层报错）。
    ImGui::PushID(tag);
    for (int k = 0; k < water::kKindCount; ++k) {
        const water::Kind kind = water::kindFromIndex(k);
        const bool active = (b.kind == kind);
        if (k > 0) ImGui::SameLine();
        beginActiveButton(active);
        if (ImGui::Button(TR(water::kindShortName(kind)), ImVec2(w, 0.0f))) {
            // 换水质 = 换一整套水感（透明度/折射/流速/波高/涟漪/泡沫…），
            // 但**布局**是用户的：形状、位置、范围、水位、朝向都不动。
            const glm::vec2 center = b.center;
            const glm::vec2 half = b.halfExtent;
            const float level = b.level;
            const float rot = b.rotation;
            const water::Shape shape = b.shape;
            water::applyPreset(b, kind);
            if (keepLayout) {
                b.center = center;
                b.halfExtent = half;
                b.level = level;
                b.rotation = rot;
                b.shape = shape;
            }
            if (ownerId) {
                m_ctx.dirty() = true;
                VK_LOG_INFO("WATER-KIND id=%u kind=%s", ownerId,
                            water::kindId(kind));
            } else {
                VK_LOG_INFO("WATER-NEW-KIND kind=%s", water::kindId(kind));
            }
        }
        char sub[64];
        std::snprintf(sub, sizeof(sub), "%s %s", tag, water::kindId(kind));
        logRect(sub, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        endActiveButton(active);
    }
    ImGui::PopID();
}

void WaterEditor::drawShapeButtons(water::WaterBody& b, const char* tag,
                                   std::uint32_t ownerId) {
    const float w = rowButtonWidth(2);
    ImGui::PushID(tag);
    for (int s = 0; s < 2; ++s) {
        const water::Shape shape =
            (s == 0) ? water::Shape::Circle : water::Shape::Rectangle;
        const bool active = (b.shape == shape);
        if (s > 0) ImGui::SameLine();
        beginActiveButton(active);
        if (ImGui::Button(TR(shape == water::Shape::Circle ? "Circle"
                                                          : "Rectangle"),
                          ImVec2(w, 0.0f))) {
            b.shape = shape;
            // 圆形只认 x；切过来时把 y 拉齐，免得面板显示一个用不到的半长
            if (shape == water::Shape::Circle) b.halfExtent.y = b.halfExtent.x;
            const char* id =
                (shape == water::Shape::Circle) ? "circle" : "rect";
            if (ownerId) {
                m_ctx.dirty() = true;
                VK_LOG_INFO("WATER-SHAPE id=%u shape=%s", ownerId, id);
            } else {
                VK_LOG_INFO("WATER-NEW-SHAPE shape=%s", id);
            }
        }
        char sub[64];
        std::snprintf(sub, sizeof(sub), "%s %s", tag,
                      (shape == water::Shape::Circle) ? "circle" : "rect");
        logRect(sub, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        endActiveButton(active);
    }
    ImGui::PopID();
}

void WaterEditor::drawCreateSection() {
    ImGui::SeparatorText(TR("New water body"));

    drawShapeButtons(m_proto, "WATER-NEW-SHAPE", 0);
    drawKindButtons(m_proto, "WATER-NEW-KIND", 0, /*keepLayout=*/false);

    if (ImGui::Button(TR("Create at view target"), ImVec2(-1.0f, 0.0f))) {
        // 相机正对着的那个点（轨道相机的 target）。这条路径给"不想精确
        // 拖拽"和自动化用 —— 拖拽那条在脚本里更容易误触。
        const glm::vec3 target = m_ctx.editorScene().camera().target();
        water::WaterBody proto = m_proto;
        proto.halfExtent = glm::vec2(10.0f, 10.0f);
        createBody(glm::vec2(target.x, target.y), target.z + kDefaultRise,
                   m_proto.shape, proto, "button", /*fitGround=*/true);
    }
    logRect("WATER-ADD", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::TextDisabled("%s",
                        TR("Or drag in the viewport: the water body is created "
                           "on press and grows while you drag."));
}

void WaterEditor::drawStatsSection() {
    const ecs::WaterComponent* wc = selected();
    if (!wc) return;
    const water::WaterBody& b = wc->body;

    ImGui::Text(TR("Kind: %s   Shape: %s"), TR(water::kindName(b.kind)),
                b.shape == water::Shape::Circle ? TR("Circle")
                                                : TR("Rectangle"));
    ImGui::Text(TR("Water level: %.2f m   Bed: %.2f m"),
                static_cast<double>(b.level),
                static_cast<double>(b.bedLevel));
    // 水深 = 水面 - 池底。<= 0 意味着水面整片都在地面之下（用户把水位拖
    // 到地下了），这时屏幕上根本看不到水 —— 用警告色说清楚，而不是显示
    // 一个让人困惑的 "-2.30 m"。
    if (b.maxDepth() < 0.02f) {
        ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.25f, 1.0f), "%s",
                           TR("Max depth: 0.00 m (below ground)"));
    } else {
        ImGui::Text(TR("Max depth: %.2f m   Footprint: %.1f m"),
                    static_cast<double>(b.maxDepth()),
                    static_cast<double>(b.footprintRadius() * 2.0f));
    }
    if (ImGui::Button(TR("Delete water body"), ImVec2(-1.0f, 0.0f))) {
        const std::uint32_t id = m_ctx.selection().id;
        m_ctx.structuralEdit("Delete Water", [&] {
            m_ctx.editorScene().world().destroy(m_ctx.selection());
        });
        m_ctx.select(ecs::Entity{});
        m_ctx.dirty() = true;
        VK_LOG_INFO("WATER-DELETE id=%u", id);
    }
    logRect("WATER-DELETE", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
}

void WaterEditor::drawShapeSection(ecs::WaterComponent& wc) {
    water::WaterBody& b = wc.body;
    // 段名由调用方的 CollapsingHeader 承担（见 drawPanel），不重复画

    // 形状换了 → footprint 换了（圆只认半径，矩形有半宽/半深）→ 池底与
    // 岸线都得重新采样，否则切形状之后水位会停在旧形状的位置上。
    const water::Shape before = b.shape;
    drawShapeButtons(b, "WATER-SHAPE", m_ctx.selection().id);
    if (b.shape != before) fitToGround(b, b.autoFit);

    if (b.shape == water::Shape::Circle) {
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::SliderFloat(TR("Radius"), &b.halfExtent.x, 0.5f, 200.0f,
                               TR("%.1f m"))) {
            b.halfExtent.y = b.halfExtent.x;
            m_ctx.dirty() = true;
        }
        logRect("WATER-RADIUS", ImGui::GetItemRectMin(),
                ImGui::GetItemRectMax());
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            // 松手才采样：拖动过程中读盘会掉帧（见 GroundProbe）
            fitToGround(b, b.autoFit);
            VK_LOG_INFO("WATER-SIZE id=%u radius=%.1f", m_ctx.selection().id,
                        static_cast<double>(b.halfExtent.x));
        }
    } else {
        // `IsItemDeactivatedAfterEdit()` 只认**紧挨着的上一个 item**。之前
        // 把这条日志放在两个滑块之后，等于只对"半深"生效 —— 只拖"半宽"
        // 永远打不出 WATER-SIZE（2026-10-21 验收时发现的假失败源，日志
        // 缺失会让人以为是尺寸没生效）。所以两个滑块各挂一次。
        const auto logSize = [&] {
            fitToGround(b, b.autoFit);
            VK_LOG_INFO("WATER-SIZE id=%u halfExtent=(%.1f,%.1f)",
                        m_ctx.selection().id,
                        static_cast<double>(b.halfExtent.x),
                        static_cast<double>(b.halfExtent.y));
        };
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::SliderFloat(TR("Half width"), &b.halfExtent.x, 0.5f, 200.0f,
                               TR("%.1f m"))) {
            m_ctx.dirty() = true;
        }
        logRect("WATER-HALFX", ImGui::GetItemRectMin(),
                ImGui::GetItemRectMax());
        if (ImGui::IsItemDeactivatedAfterEdit()) logSize();

        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::SliderFloat(TR("Half depth"), &b.halfExtent.y, 0.5f, 200.0f,
                               TR("%.1f m"))) {
            m_ctx.dirty() = true;
        }
        logRect("WATER-HALFZ", ImGui::GetItemRectMin(),
                ImGui::GetItemRectMax());
        if (ImGui::IsItemDeactivatedAfterEdit()) logSize();

        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat(TR("Rotation"), &b.rotation, -180.0f, 180.0f,
                           TR("%.0f deg"));
        logRect("WATER-ROT", ImGui::GetItemRectMin(),
                ImGui::GetItemRectMax());
    }

    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::SliderFloat(TR("Water level"), &b.level, -20.0f, 200.0f,
                           TR("%.2f m"))) {
        // 手动拖过水位 → 不再自动贴合（否则下次改半径就把它顶回去了）
        b.autoFit = false;
        m_ctx.dirty() = true;
    }
    logRect("WATER-LEVEL", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        VK_LOG_INFO("WATER-LEVEL id=%u level=%.2f depth=%.2f",
                    m_ctx.selection().id, static_cast<double>(b.level),
                    static_cast<double>(b.maxDepth()));
    }

    // 手动调过水位之后想回到"贴着地形"的状态，按这一个按钮就行
    if (ImGui::Button(TR("Fit to ground"), ImVec2(-1.0f, 0.0f))) {
        b.autoFit = true;
        fitToGround(b, /*followLevel=*/true);
        m_ctx.dirty() = true;
    }
    logRect("WATER-FIT", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
}

void WaterEditor::drawLookSection(ecs::WaterComponent& wc) {
    water::WaterBody& b = wc.body;
    // 段名由调用方的 CollapsingHeader 承担（见 drawPanel），不重复画

    // 换类型时**只覆盖水质参数**，形状/位置/水位保持 —— 用户拖好的
    // 范围不该因为换了个水类型就跳走（keepLayout=true）
    drawKindButtons(b, "WATER-KIND", m_ctx.selection().id, /*keepLayout=*/true);

    ImGui::ColorEdit3("##water_shallow", glm::value_ptr(b.shallowColor),
                      ImGuiColorEditFlags_NoInputs |
                          ImGuiColorEditFlags_NoLabel);
    ImGui::SameLine();
    ImGui::TextUnformatted(TR("Shallow color"));
    logRect("WATER-SHALLOW", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());

    ImGui::ColorEdit3("##water_deep", glm::value_ptr(b.deepColor),
                      ImGuiColorEditFlags_NoInputs |
                          ImGuiColorEditFlags_NoLabel);
    ImGui::SameLine();
    ImGui::TextUnformatted(TR("Deep color"));
    logRect("WATER-DEEP", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());

    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::SliderFloat(TR("Opacity"), &b.opacity, 0.0f, 1.0f, "%.2f")) {
        m_ctx.dirty() = true;
    }
    logRect("WATER-OPACITY", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());

    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::SliderFloat(TR("Refraction"), &b.refraction, 0.0f, 1.0f,
                           "%.2f")) {
        m_ctx.dirty() = true;
    }
    logRect("WATER-REFRACTION", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());

    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::SliderFloat(TR("IOR"), &b.ior, 1.0f, 1.6f, "%.3f")) {
        m_ctx.dirty() = true;
    }
    logRect("WATER-IOR", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::SliderFloat(TR("Clarity"), &b.clarity, 0.05f, 5.0f, "%.2f")) {
        m_ctx.dirty() = true;
    }
    logRect("WATER-CLARITY", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
    ImGui::TextDisabled("%s", TR("Clarity: how far you can see through the "
                                 "water (high = spring, low = pond)."));
}

void WaterEditor::drawMotionSection(ecs::WaterComponent& wc) {
    water::WaterBody& b = wc.body;
    // 段名由调用方的 CollapsingHeader 承担（见 drawPanel），不重复画

    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::SliderFloat(TR("Wave height"), &b.waveHeight, 0.0f, 2.0f,
                           TR("%.2f m"))) {
        m_ctx.dirty() = true;
    }
    logRect("WATER-WAVEH", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());

    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::SliderFloat(TR("Wave length"), &b.waveLength, 0.5f, 60.0f,
                           TR("%.1f m"))) {
        m_ctx.dirty() = true;
    }
    logRect("WATER-WAVEL", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());

    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::SliderFloat(TR("Ripple"), &b.ripple, 0.0f, 2.0f, "%.2f")) {
        m_ctx.dirty() = true;
    }
    logRect("WATER-RIPPLE", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());

    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::SliderFloat(TR("Foam"), &b.foam, 0.0f, 1.0f, "%.2f")) {
        m_ctx.dirty() = true;
    }
    logRect("WATER-FOAM", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::SliderFloat(TR("Flow speed"), &b.flowSpeed, 0.0f, 5.0f,
                           TR("%.2f m/s"))) {
        m_ctx.dirty() = true;
    }
    logRect("WATER-FLOW", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::SliderFloat(TR("Flow direction"), &b.flowDir.x, -1.0f, 1.0f,
                           "%.2f")) {
        m_ctx.dirty() = true;
    }
    logRect("WATER-FLOWDIR", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());

    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::SliderFloat(TR("Roughness"), &b.roughness, 0.0f, 1.0f,
                           "%.2f")) {
        m_ctx.dirty() = true;
    }
    logRect("WATER-ROUGH", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
}

} // namespace editor
