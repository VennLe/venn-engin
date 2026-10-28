// ============================================================
// engine/assets/FoliageMesh.cpp —— 植被 / 岩石的内置程序化网格
//
// 2026-10-28 从 Mesh.cpp 拆出来并**整体重写**（用户报"生成的 grass/tree/
// flower/bush/rock 都太粗糙"）。
//
// 旧版为什么粗糙：每种都只做到"能被认出来"就停了 —— 草 3 张平面卡（2 个
// 三角形）、花 5 张卡、树 2 个八边锥、灌木一颗 8×5 球、石头 9×6 位移球。
// 单株看还行，但植被是**成片**看的：几百个同样的粗糙轮廓叠在一起，粗糙本身
// 会被放大成"一撮绿毛 / 一排圣诞树 / 一地塑料蛋"。
//
// 这一版把每种的"可辨认特征"补上（三角形涨 5~40 倍，但绝对量仍然很小：
// 一棵树 ~830 tri，UE 的树是 5k~20k）：
//
//   草   9 片带弯曲/锥度/4 段细分的叶片，按黄金角螺旋摊开   ~72 tri
//   花   2 株：3 段弯曲细茎 + 6 瓣花冠 + 六边形花心          ~90 tri
//   灌木 4 个位移球 + 8 张朝外的叶卡                        ~500 tri
//   树   锥形弯曲树干 + 3 根树枝 + 6 团位移树冠              ~830 tri
//   石头 14×9 四倍频位移 + 平面着色（三角形汤）              ~500 tri
//
// 三条贯穿全文件的约定：
//   1. **Z-up**（与引擎一致），`size` = 世界高度（米），XY 由形状按比例算；
//   2. 薄片（草叶/花/叶卡）的**法线统一朝上**：逆光时面法线会全黑，
//      朝上法线拿的是天光，观感稳定。材质必须 doubleSided；
//   3. 全部**确定性**（不用 rand）：同一份 `size`/`seed` 每次生成的顶点
//      逐字节相同 —— 截图能回归、测试能断言三角形数。
// ============================================================

#include "Mesh.h"

#include <cmath>

namespace assets {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kTwoPi = 6.28318530717959f;

// 薄片统一的"朝天法线"（见文件头第 2 条）
const glm::vec3 kUpCard(0.0f, 0.0f, 1.0f);

// ------------------------------------------------------------ 基础拼装

// 四边形（bl-br-tr-tl 逆时针），UV 走 (0,0)(1,0)(1,1)(0,1)。
// 退化四边形（两个角重合）= 三角形，用于花瓣这种"一头尖"的形状 ——
// 退化三角形光栅化时直接被丢掉，不会留下伪影，比多写一套三角形代码干净。
void pushQuad(std::vector<Vertex>& verts, std::vector<uint32_t>& idx,
              const glm::vec3& bl, const glm::vec3& br, const glm::vec3& tr,
              const glm::vec3& tl, const glm::vec3& n) {
    const uint32_t b = static_cast<uint32_t>(verts.size());
    verts.push_back(Vertex{bl, n, glm::vec2(0.0f, 0.0f)});
    verts.push_back(Vertex{br, n, glm::vec2(1.0f, 0.0f)});
    verts.push_back(Vertex{tr, n, glm::vec2(1.0f, 1.0f)});
    verts.push_back(Vertex{tl, n, glm::vec2(0.0f, 1.0f)});
    idx.insert(idx.end(), {b + 0, b + 1, b + 2, b + 0, b + 2, b + 3});
}

// 一段**带弯曲和锥度**的叶片/茎：沿 +Z 长 `height`，横向被 `lean` 压弯
// （偏移量按 t² 增长 —— 线性偏移看起来像"斜插的板子"，二次才是被自重压弯）。
//
// 返回顶端中心位置：花冠/花心要接在杆顶上，不能让调用方重算一遍弯曲量
// （两处公式一旦不一致，花冠就会飘在杆旁边）。
glm::vec3 addBlade(std::vector<Vertex>& verts, std::vector<uint32_t>& idx,
                   const glm::vec3& base, const glm::vec3& dir, float height,
                   float width, float lean, int segs) {
    if (segs < 1) segs = 1;
    const glm::vec3 across = glm::normalize(glm::cross(dir, kUpCard));

    const uint32_t start = static_cast<uint32_t>(verts.size());
    glm::vec3 top = base;
    for (int i = 0; i <= segs; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(segs);
        const glm::vec3 center =
            base + dir * (lean * t * t) + kUpCard * (height * t);
        // 宽度：t=1 时收到 8%，留下一个尖 —— 草尖/花茎都是尖的。
        const float w = width * (1.0f - 0.92f * std::pow(t, 1.15f)) * 0.5f;
        verts.push_back(Vertex{center - across * w, kUpCard, glm::vec2(0.0f, t)});
        verts.push_back(Vertex{center + across * w, kUpCard, glm::vec2(1.0f, t)});
        top = center;
    }
    for (int i = 0; i < segs; ++i) {
        const uint32_t a = start + static_cast<uint32_t>(i) * 2;
        idx.insert(idx.end(), {a, a + 1, a + 3, a, a + 3, a + 2});
    }
    return top;
}

// 一个**被噪声揉过**的球团（树冠/灌木的基本单元）。
// 用经纬球而不是二十面体：本文件的其它球都是一样的拓扑，法线是解析的
// （= 归一化位置），不用重算法线，代码短一半。
void addBlob(std::vector<Vertex>& verts, std::vector<uint32_t>& idx,
             const glm::vec3& center, float radius, int seg, int ring,
             float seed) {
    if (seg < 4) seg = 4;
    if (ring < 3) ring = 3;
    const uint32_t base = static_cast<uint32_t>(verts.size());
    const uint32_t stride = static_cast<uint32_t>(seg) + 1;

    for (int y = 0; y <= ring; ++y) {
        const float v = static_cast<float>(y) / static_cast<float>(ring);
        const float phi = v * kPi;
        const float sp = std::sin(phi);
        const float cp = std::cos(phi);
        for (int x = 0; x <= seg; ++x) {
            const float u = static_cast<float>(x) / static_cast<float>(seg);
            const float th = u * kTwoPi;
            const glm::vec3 n(sp * std::cos(th), sp * std::sin(th), cp);
            // 两段不同频率的起伏：低频给"团块的形状"，高频给"叶簇的碎边"。
            // 只用一段会得到一颗像被捏过的球，而不是一团叶子。
            float d = 1.0f;
            d += 0.16f * std::sin(th * 3.0f + seed) * std::cos(phi * 2.0f + seed * 0.7f);
            d += 0.07f * std::sin(th * 7.0f + seed * 1.7f) * std::sin(phi * 5.0f + seed);
            verts.push_back(Vertex{center + n * (radius * d), n,
                                   glm::vec2(u, v)});
        }
    }
    // 绕序与 makeSphereIndices 一致（ring 0 = 顶）
    for (int y = 0; y < ring; ++y) {
        for (int x = 0; x < seg; ++x) {
            const uint32_t a = base + static_cast<uint32_t>(y) * stride +
                               static_cast<uint32_t>(x);
            const uint32_t b = a + stride;
            idx.insert(idx.end(), {a, a + 1, b, a + 1, b + 1, b});
        }
    }
}

// 一段**任意朝向**的管子（无端盖）。树干、树枝共用。
// 与 Mesh.cpp 里那个 addPrism 的区别：那个只能竖直（轴向 +Z），
// 树枝要斜着长，所以这里用两个端点 + 一个参考轴算正交基。
void addTube(std::vector<Vertex>& verts, std::vector<uint32_t>& idx,
             const glm::vec3& from, const glm::vec3& to, float rFrom,
             float rTo, int sides) {
    if (sides < 3) sides = 3;
    const glm::vec3 axis = to - from;
    const float len = glm::length(axis);
    if (len < 1.0e-5f) return;
    const glm::vec3 dir = axis / len;
    // 参考轴：与 dir 夹角足够大才稳（与上方向平行时退化）
    glm::vec3 ref = std::abs(dir.z) > 0.9f ? glm::vec3(1.0f, 0.0f, 0.0f)
                                           : glm::vec3(0.0f, 0.0f, 1.0f);
    const glm::vec3 t1 = glm::normalize(glm::cross(ref, dir));
    const glm::vec3 t2 = glm::cross(dir, t1);

    const uint32_t start = static_cast<uint32_t>(verts.size());
    for (int s = 0; s <= sides; ++s) {
        const float a = kTwoPi * static_cast<float>(s) /
                        static_cast<float>(sides);
        const glm::vec3 n = t1 * std::cos(a) + t2 * std::sin(a);
        const float u = static_cast<float>(s) / static_cast<float>(sides);
        verts.push_back(Vertex{from + n * rFrom, n, glm::vec2(u, 0.0f)});
        verts.push_back(Vertex{to + n * rTo, n, glm::vec2(u, 1.0f)});
    }
    for (int s = 0; s < sides; ++s) {
        const uint32_t b = start + static_cast<uint32_t>(s) * 2;
        idx.insert(idx.end(), {b + 0, b + 1, b + 3, b + 0, b + 3, b + 2});
    }
}

} // namespace

// ============================================================
// 草：一丛 9 片叶
// ============================================================
// 旧版是 3 张交叉的**平面**卡：任何角度看都是"三块竖直的绿板"，边缘笔直、
// 顶端齐平，近看立刻穿帮。
//
// 这一版：9 片，每片 4 段（看得出弯曲）、带锥度（尖）、高度/弯曲量/朝向
// 全都不一样。摊开位置用**黄金角螺旋**（137.5°/片）而不是均匀扇形 ——
// 均匀扇形会出现明显的放射状空档，螺旋在任何角度上都不重叠、不空。
void buildGrassMesh(float size, std::vector<Vertex>& verts,
                    std::vector<uint32_t>& idx) {
    if (!(size > 0.0f)) size = 1.0f;
    constexpr int kBlades = 9;
    for (int b = 0; b < kBlades; ++b) {
        const float t = static_cast<float>(b) / static_cast<float>(kBlades);
        const float a = static_cast<float>(b) * 2.39996323f;   // 黄金角
        const float r = size * 0.17f * std::sqrt(t);
        const glm::vec3 base(std::cos(a) * r, std::sin(a) * r, 0.0f);
        const glm::vec3 dir(std::cos(a), std::sin(a), 0.0f);

        // 高度 0.62~1.00：丛的边缘矮、中间高 —— 顶端齐平像栅栏
        const float h = size * (0.62f + 0.38f * (0.5f + 0.5f * std::sin(a * 1.7f)));
        // 单片宽度 5.5%~7.5%
        const float w = size * (0.055f + 0.020f * std::cos(a * 2.3f));
        // 外圈更弯（外侧的草被中间挤出来，本来就往外倒）
        const float lean = size * (0.10f + 0.24f * t);
        addBlade(verts, idx, base, dir, h, w, lean, 4);
    }
}

// ============================================================
// 花：两株 茎 + 花冠 + 花心
// ============================================================
// 旧版只有一根杆挑一个三片的花冠，撒在草里几乎看不见"这是花"。
// 现在两株、茎有弯曲、花冠 6 瓣朝外张开、中间一个六边形花心（深色材质
// 下能看出"花心 vs 花瓣"的层次）。
void buildFlowerMesh(float size, std::vector<Vertex>& verts,
                     std::vector<uint32_t>& idx) {
    if (!(size > 0.0f)) size = 1.0f;

    for (int f = 0; f < 2; ++f) {
        const float a0 = static_cast<float>(f) * 2.1f + 0.6f;
        const glm::vec3 base(std::cos(a0) * size * 0.10f,
                             std::sin(a0) * size * 0.10f, 0.0f);
        const glm::vec3 dir(std::cos(a0), std::sin(a0), 0.0f);
        // 两株一高一矮，不然像复制粘贴
        const float stemH = size * (f ? 0.98f : 0.74f) * 0.80f;
        const glm::vec3 head =
            addBlade(verts, idx, base, dir, stemH, size * 0.042f,
                     size * 0.055f, 3);

        // ---- 花冠：6 瓣，从花心向外张开、外缘抬高一点（像真的花瓣）----
        constexpr int kPetals = 6;
        const float petalLen = size * 0.145f;
        const float petalW = size * 0.075f;
        for (int p = 0; p < kPetals; ++p) {
            const float ang = kTwoPi * static_cast<float>(p) /
                                  static_cast<float>(kPetals) +
                              a0 + static_cast<float>(f) * 0.5f;
            const glm::vec3 outDir(std::cos(ang), std::sin(ang), 0.0f);
            const glm::vec3 side = glm::cross(outDir, kUpCard) * (petalW * 0.5f);
            const glm::vec3 inner = head;
            const glm::vec3 outer = head + outDir * petalLen +
                                    kUpCard * (size * 0.035f);
            // 菱形花瓣：内尖 → 左右中段 → 外尖。内尖退化成一个点。
            pushQuad(verts, idx, inner, inner + side, outer, outer - side,
                     kUpCard);
            // 上面那个四边形只有 3 个有效角（inner 重合），形状是三角形；
            // 想要更"花瓣"，再加一片朝外翘的小尖。
            pushQuad(verts, idx, outer - side * 0.6f, outer + side * 0.6f,
                     outer + outDir * (petalLen * 0.35f) + kUpCard * (size * 0.02f),
                     outer + outDir * (petalLen * 0.35f) + kUpCard * (size * 0.02f),
                     kUpCard);
        }

        // ---- 花心：六边形（6 个三角形的扇），略高于花瓣根 ----
        const glm::vec3 c = head + kUpCard * (size * 0.012f);
        const float cr = size * 0.045f;
        const uint32_t centerIdx = static_cast<uint32_t>(verts.size());
        verts.push_back(Vertex{c, kUpCard, glm::vec2(0.5f, 0.5f)});
        for (int s = 0; s <= 6; ++s) {
            const float ang = kTwoPi * static_cast<float>(s) / 6.0f;
            verts.push_back(Vertex{c + glm::vec3(std::cos(ang), std::sin(ang), 0.0f) * cr,
                                   kUpCard,
                                   glm::vec2(0.5f + 0.5f * std::cos(ang),
                                             0.5f + 0.5f * std::sin(ang))});
        }
        for (uint32_t s = 0; s < 6; ++s) {
            idx.insert(idx.end(), {centerIdx, centerIdx + 1 + s,
                                   centerIdx + 2 + s});
        }
    }
}

// ============================================================
// 灌木：4 团 + 8 张叶卡
// ============================================================
// 旧的是一颗 8×5 的光滑球（还压扁了）：远看就是一个绿球。
// 现在 4 团大小不一的叶簇堆成一个不规则的冠，外加 8 张朝外的叶卡补
// 轮廓 —— 侧影不再是圆的，这是"灌木"和"球"的唯一区别。
void buildBushMesh(float size, std::vector<Vertex>& verts,
                   std::vector<uint32_t>& idx) {
    if (!(size > 0.0f)) size = 1.0f;

    struct Blob { float a; float r; float z; float rad; };
    // 一低三高：底部那团把"灌木坐在地上"这件事交代清楚
    const Blob blobs[4] = {
        {0.0f, 0.00f, 0.34f, 0.34f},
        {2.40f, 0.22f, 0.46f, 0.26f},
        {4.80f, 0.20f, 0.40f, 0.24f},
        {1.20f, 0.16f, 0.56f, 0.22f},
    };
    for (int b = 0; b < 4; ++b) {
        const Blob& B = blobs[b];
        const glm::vec3 c(std::cos(B.a) * size * B.r,
                          std::sin(B.a) * size * B.r, size * B.z);
        addBlob(verts, idx, c, size * B.rad, 10, 6,
                static_cast<float>(b) * 1.7f);
    }

    // 叶卡：朝 8 个方向探出来，给侧影加"碎边"
    for (int i = 0; i < 8; ++i) {
        const float a = kTwoPi * static_cast<float>(i) / 8.0f + 0.35f;
        const glm::vec3 dir(std::cos(a), std::sin(a), 0.0f);
        const glm::vec3 base = dir * (size * 0.24f) + kUpCard * (size * 0.30f);
        addBlade(verts, idx, base, dir, size * 0.30f, size * 0.16f,
                 size * 0.16f, 2);
    }
}

// ============================================================
// 树：锥形弯曲树干 + 3 根树枝 + 6 团树冠
// ============================================================
// 旧版是"6 边柱 + 两个 8 边锥"—— 就是圣诞树，而且树干直得像管子。
// 现在：树干分 5 环、带锥度和一点点 S 弯；3 根树枝从不同高度斜伸出去；
// 树冠 6 团大小不一的叶簇堆成椭球，每团都带噪声位移。
void buildTreeMesh(float size, std::vector<Vertex>& verts,
                   std::vector<uint32_t>& idx) {
    if (!(size > 0.0f)) size = 1.0f;

    // ---- 树干：5 环、8 边、半径 0.075 → 0.036，带 S 弯 ----
    constexpr int kRings = 5;
    constexpr int kSides = 8;
    const float hTrunk = size * 0.62f;
    glm::vec3 prev(0.0f, 0.0f, 0.0f);
    float prevR = size * 0.075f;
    for (int i = 1; i <= kRings; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kRings);
        // S 弯：t=0 与 t=1 时偏移为 0，中间最大 —— 直管感就消失在这一行
        const float bend = size * 0.035f * std::sin(t * kPi);
        const glm::vec3 cur(bend, 0.0f, hTrunk * t);
        const float r = size * (0.075f - 0.039f * t);
        addTube(verts, idx, prev, cur, prevR, r, kSides);
        prev = cur;
        prevR = r;
    }

    // ---- 树枝：3 根，从 0.42 / 0.52 / 0.60 高处斜伸 ----
    for (int b = 0; b < 3; ++b) {
        const float t = 0.42f + 0.09f * static_cast<float>(b);
        const float ang = static_cast<float>(b) * 2.1f + 0.8f;
        const float bend = size * 0.035f * std::sin(t * kPi);
        const glm::vec3 from(bend, 0.0f, hTrunk * t);
        const glm::vec3 dir(std::cos(ang), std::sin(ang), 0.0f);
        const glm::vec3 to = from + dir * (size * 0.20f) +
                             kUpCard * (size * 0.16f);
        addTube(verts, idx, from, to, size * 0.028f, size * 0.014f, 5);
    }

    // ---- 树冠：6 团，堆成一个略扁的椭球 ----
    // 半径不能超过树干高度太多，否则"树干 + 球"的缝合处会露馅。
    struct Blob { float a; float r; float z; float rad; };
    const Blob canopy[6] = {
        {0.00f, 0.00f, 0.72f, 0.36f},
        {1.05f, 0.26f, 0.80f, 0.30f},
        {2.10f, 0.28f, 0.68f, 0.27f},
        {3.14f, 0.24f, 0.78f, 0.29f},
        {4.19f, 0.27f, 0.66f, 0.26f},
        {5.24f, 0.22f, 0.84f, 0.25f},
    };
    for (int c = 0; c < 6; ++c) {
        const Blob& B = canopy[c];
        const glm::vec3 center(std::cos(B.a) * size * B.r,
                               std::sin(B.a) * size * B.r, size * B.z);
        addBlob(verts, idx, center, size * B.rad, 10, 6,
                static_cast<float>(c) * 1.3f + 0.4f);
    }
}

// ============================================================
// 石头：四倍频位移 + 平面着色
// ============================================================
// 逐面法线意味着**不能共用顶点**，所以这里是三角形汤（索引 = 0..n-1）。
// 14×9 的经纬网格（252 环节点）在 Debug 下也不贵；撒几百块石头没压力。
void buildRockMesh(float size, int seed, std::vector<Vertex>& verts,
                   std::vector<uint32_t>& idx) {
    if (!(size > 0.0f)) size = 1.0f;
    const float radius = size * 0.5f;
    const float s = static_cast<float>(seed) * 0.37f;

    constexpr int kSeg = 14;
    constexpr int kRing = 9;

    // 四倍频：低频决定"这块石头是扁的还是方的"，高频是表面的碎石纹。
    // 只用两三个倍频会得到一颗"光滑的土豆"。
    auto pointAt = [&](int seg, int ring) -> glm::vec3 {
        const float theta = kTwoPi * static_cast<float>(seg) /
                            static_cast<float>(kSeg);
        const float phi = kPi * static_cast<float>(ring) /
                          static_cast<float>(kRing);
        const float sp = std::sin(phi);
        glm::vec3 n(sp * std::cos(theta), sp * std::sin(theta), std::cos(phi));
        float d = 1.0f;
        d += 0.22f * std::sin(theta * 2.0f + s) * std::cos(phi * 2.0f + s * 0.7f);
        d += 0.13f * std::sin(theta * 5.0f + 1.3f + s * 1.7f) * std::sin(phi * 4.0f + s);
        d += 0.07f * std::sin(theta * 11.0f + phi * 7.0f + s * 2.3f);
        d += 0.035f * std::sin(theta * 23.0f + phi * 17.0f + s * 3.1f);
        glm::vec3 p = n * radius * d;
        p.z *= 0.74f;   // 竖向压扁：真石头很少是个立方
        return p;
    };

    auto addTri = [&](const glm::vec3& a, const glm::vec3& b,
                      const glm::vec3& c) {
        glm::vec3 n = glm::cross(b - a, c - a);
        if (glm::length(n) < 1.0e-9f) return;
        n = glm::normalize(n);
        const uint32_t base = static_cast<uint32_t>(verts.size());
        verts.push_back(Vertex{a, n, glm::vec2(0.0f, 0.0f)});
        verts.push_back(Vertex{b, n, glm::vec2(1.0f, 0.0f)});
        verts.push_back(Vertex{c, n, glm::vec2(1.0f, 1.0f)});
        idx.insert(idx.end(), {base, base + 1, base + 2});
    };

    for (int ring = 0; ring < kRing; ++ring) {
        for (int seg = 0; seg < kSeg; ++seg) {
            const glm::vec3 p00 = pointAt(seg, ring);
            const glm::vec3 p10 = pointAt(seg + 1, ring);
            const glm::vec3 p01 = pointAt(seg, ring + 1);
            const glm::vec3 p11 = pointAt(seg + 1, ring + 1);
            addTri(p00, p10, p11);
            addTri(p00, p11, p01);
        }
    }
}

} // namespace assets
