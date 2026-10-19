#include "terrain/TerrainSelfTest.h"

#include "assets/AssetPath.h"
#include "core/Logger.h"
#include "physics/CharacterController.h"
#include "physics/Heightfield.h"
#include "physics/PhysicsWorld.h"
#include "terrain/Terrain.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace terrain {

namespace {

float lerpf(float a, float b, float t) { return a + (b - a) * t; }

int g_pass = 0;
int g_fail = 0;

void check(const char* name, bool ok, const char* detail = "") {
    if (ok) {
        ++g_pass;
        VK_LOG_INFO("TERRAIN-T pass %s %s", name, detail);
    } else {
        ++g_fail;
        VK_LOG_ERROR("TERRAIN-T FAIL %s %s", name, detail);
    }
}

void checkNear(const char* name, double actual, double expected, double tol) {
    const bool ok = std::fabs(actual - expected) <= tol;
    char buf[160];
    std::snprintf(buf, sizeof(buf), "actual=%.6f expected=%.6f tol=%.6f",
                  actual, expected, tol);
    check(name, ok, buf);
}

// 合成一张"纯斜坡"高度场：沿 +X 上升，坡角 = atan(grad)。
// raw 存归一化值，height = raw * maxHeight。
struct SlopeField {
    std::vector<float> raw;
    physics::Heightfield hf;

    // risePerMeter：每米升高多少米（= tan(坡角)）
    void build(int n, float mpp, float risePerMeter, float originX = 0.0f,
               float originY = 0.0f) {
        raw.assign(static_cast<std::size_t>(n) * static_cast<std::size_t>(n), 0.0f);
        const float spanM = static_cast<float>(n - 1) * mpp;
        const float top = risePerMeter * spanM;  // 最高处的米数
        for (int c = 0; c < n; ++c) {
            const float hM = risePerMeter * static_cast<float>(c) * mpp;
            const float r = (top > 1.0e-6f) ? (hM / top) : 0.0f;
            for (int r2 = 0; r2 < n; ++r2) {
                raw[static_cast<std::size_t>(c) * static_cast<std::size_t>(n) +
                    static_cast<std::size_t>(r2)] = r;
            }
        }
        hf.cols = n;
        hf.rows = n;
        hf.metersPerPixel = mpp;
        hf.heightScale = (top > 1.0e-6f) ? top : 1.0f;
        hf.origin = glm::vec2(originX, originY);
        hf.heights = raw.data();
    }
};

// 确定性伪随机（不用 <random>：不同标准库的实现不一样，
// 而"自检结果必须跨机器一致"）
struct Lcg {
    std::uint32_t s = 20261018u;
    float next01() {
        s = s * 1664525u + 1013904223u;
        return static_cast<float>((s >> 8) & 0xFFFFu) / 65535.0f;
    }
};

// 读一份裸文件（自检要**自己**读一遍 .r32，才能证明"资产的字节 ↔ 内存"）
bool slurp(const std::string& rel, std::vector<std::uint8_t>& out) {
    std::ifstream f(assets::resolveAssetPath(rel), std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    const std::streamoff n = f.tellg();
    if (n < 0) return false;
    f.seekg(0, std::ios::beg);
    out.resize(static_cast<std::size_t>(n));
    if (n > 0) f.read(reinterpret_cast<char*>(out.data()), n);
    return true;
}

} // namespace

int runTerrainSelfTest() {
    g_pass = 0;
    g_fail = 0;
    VK_LOG_INFO("== selftest: terrain ==");

    // ============================================================
    // A 组：加载与格式
    // ============================================================
    // 用**仓库里真实的那份**资产，而不是临时造一份：临时造的就测不到
    // "生成器写错了"（归一化、列主序、尺寸）这些最常犯的错。
    const char* kAsset = "terrain/hills/terrain.vterrain";
    TerrainAsset asset;
    std::string err;
    const bool loaded = loadTerrain(kAsset, asset, &err);
    check("A1 加载 .vterrain 成功", loaded, err.c_str());
    if (!loaded) {
        char summary[96];
        std::snprintf(summary, sizeof(summary), "pass=%d fail=%d", g_pass, g_fail);
        VK_LOG_INFO("TERRAIN-TOTAL %s", summary);
        return g_fail == 0 ? 0 : 1;
    }

    {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "cols=%d rows=%d mpp=%.3f maxh=%.2f",
                      asset.desc.cols, asset.desc.rows,
                      static_cast<double>(asset.desc.metersPerPixel),
                      static_cast<double>(asset.desc.maxHeight));
        check("A2 尺寸与米每像素有效", asset.desc.cols > 32 &&
                                            asset.desc.rows > 32 &&
                                            asset.desc.metersPerPixel > 0.0f,
              buf);
    }

    // A3/A4：**自己**再读一遍两份二进制，逐字节核对。
    // 这条就是"地形与高度图逐像素对应"的实现形式：资产的 heights[i] 必须
    // 等于文件里第 i 个 float（列主序），而不是"某个看起来合理的值"。
    {
        std::vector<std::uint8_t> hbytes;
        const bool okFile = slurp("terrain/hills/height.r32", hbytes);
        const std::size_t want = static_cast<std::size_t>(asset.desc.cols) *
                                 static_cast<std::size_t>(asset.desc.rows) * 4u;
        char buf[160];
        std::snprintf(buf, sizeof(buf), "file=%zu want=%zu", hbytes.size(), want);
        check("A3 height.r32 的大小与 cols*rows*4 一致",
              okFile && hbytes.size() == want, buf);

        bool bitEqual = (hbytes.size() == want);
        int mismatch = 0;
        if (bitEqual) {
            for (std::size_t i = 0; i < asset.heights.size(); ++i) {
                float f = 0.0f;
                std::memcpy(&f, hbytes.data() + i * 4u, 4u);
                if (std::memcmp(&f, &asset.heights[i], 4u) != 0) {
                    ++mismatch;
                    bitEqual = false;
                }
            }
        }
        char buf2[128];
        std::snprintf(buf2, sizeof(buf2), "mismatch=%d", mismatch);
        check("A4 内存里的高度 == 文件里的 float（逐位）", bitEqual, buf2);
    }
    {
        std::vector<std::uint8_t> sbytes;
        const std::size_t want = static_cast<std::size_t>(asset.desc.cols) *
                                 static_cast<std::size_t>(asset.desc.rows) * 4u;
        const bool okFile = slurp("terrain/hills/splat.rgba8", sbytes);
        check("A5 splat.rgba8 的大小与 cols*rows*4 一致",
              okFile && sbytes.size() == want && asset.splat.size() == want);
    }
    {
        float lo = 1.0e30f, hi = -1.0e30f;
        for (float h : asset.heights) {
            lo = std::min(lo, h);
            hi = std::max(hi, h);
        }
        char buf[128];
        std::snprintf(buf, sizeof(buf), "min=%.4f max=%.4f",
                      static_cast<double>(lo), static_cast<double>(hi));
        check("A6 高度已归一化到 [0,1]（否则 maxHeight 会被乘两次）",
              lo >= -0.001f && hi <= 1.001f, buf);
    }

    // ============================================================
    // B 组：高度场查询（解析解当基准）
    // ============================================================
    {
        // B1：整数采样点处，插值必须**恰好**等于该点
        const physics::Heightfield hf = asset.heightfield();
        int worstIdx = -1;
        float worst = 0.0f;
        for (int c = 0; c < asset.desc.cols; c += 17) {
            for (int r = 0; r < asset.desc.rows; r += 17) {
                const glm::vec3 p = hf.worldOf(c, r);
                const float got = physics::heightAt(hf, p.x, p.y);
                const float d = std::fabs(got - p.z);
                if (d > worst) { worst = d; worstIdx = c * asset.desc.rows + r; }
            }
        }
        char buf[128];
        std::snprintf(buf, sizeof(buf), "maxdiff=%.6f idx=%d",
                      static_cast<double>(worst), worstIdx);
        check("B1 节点处 heightAt == 采样点高度", worst < 1.0e-5f, buf);
    }
    {
        // B2：45° 纯斜坡上的双线性必须给出解析高度（斜坡是**线性可插值**的，
        // 任何沿 X 的插值误差都说明权重写错了）
        SlopeField sf;
        sf.build(65, 1.0f, 1.0f);  // 1 m/米 = 45°
        Lcg rng;
        double worst = 0.0;
        for (int i = 0; i < 64; ++i) {
            const float x = sf.hf.origin.x + rng.next01() * 60.0f;
            const float y = sf.hf.origin.y + rng.next01() * 60.0f;
            const double expect = static_cast<double>(x - sf.hf.origin.x);
            const double got = physics::heightAt(sf.hf, x, y);
            worst = std::max(worst, std::fabs(got - expect));
        }
        checkNear("B2 45° 斜坡上双线性误差", worst, 0.0, 1.0e-4);
    }
    {
        // B3：30° 斜坡的解析法线 = (-sin30, 0, cos30)
        SlopeField sf;
        const float rise = std::tan(30.0f * 3.14159265358979f / 180.0f);
        sf.build(65, 1.0f, rise);
        const glm::vec3 n = physics::normalAt(sf.hf, 30.0f, 30.0f);
        const double want = static_cast<double>(std::sin(30.0f * 3.14159265358979f / 180.0f));
        char buf[160];
        std::snprintf(buf, sizeof(buf), "n=(%.4f,%.4f,%.4f)",
                      static_cast<double>(n.x), static_cast<double>(n.y),
                      static_cast<double>(n.z));
        checkNear("B3 30° 坡法线的 -x 分量 == sin30", -static_cast<double>(n.x),
                  want, 2.0e-3);
        checkNear("B3b 30° 坡法线的 z 分量 == cos30",
                  static_cast<double>(n.z),
                  static_cast<double>(std::cos(30.0f * 3.14159265358979f / 180.0f)),
                  2.0e-3);
        check("B3c 法线已归一化",
              std::fabs(glm::length(n) - 1.0f) < 1.0e-4f, buf);
    }
    {
        // B4：**W4 的验收口径** —— 射线命中点高度与采样高度差 < 1 cm
        const physics::Heightfield hf = asset.heightfield();
        Lcg rng;
        double worst = 0.0;
        int miss = 0;
        for (int i = 0; i < 200; ++i) {
            const glm::vec2 sz = asset.worldSize();
            const float x = lerpf(-0.5f * sz.x, 0.5f * sz.x, rng.next01());
            const float y = lerpf(-0.5f * sz.y, 0.5f * sz.y, rng.next01());
            const float h = physics::heightAt(hf, x, y);
            physics::HeightHit hit;
            const bool ok = physics::raycast(
                hf, glm::vec3(x, y, h + 200.0f), glm::vec3(0.0f, 0.0f, -1.0f),
                400.0f, hit);
            if (!ok) { ++miss; continue; }
            worst = std::max(worst, static_cast<double>(std::fabs(hit.point.z - h)));
        }
        char buf[128];
        std::snprintf(buf, sizeof(buf), "maxdiff=%.6f miss=%d",
                      worst, miss);
        check("B4 垂直射线命中高度 vs heightAt（< 1 cm）", miss == 0 && worst < 0.01,
              buf);
    }
    {
        // B5：斜射线（45°）也要 < 1 cm —— 步进 + 二分细化都必须是对的
        const physics::Heightfield hf = asset.heightfield();
        Lcg rng;
        const glm::vec3 dir =
            glm::normalize(glm::vec3(0.6f, 0.4f, -0.7f));
        double worst = 0.0;
        int miss = 0;
        for (int i = 0; i < 120; ++i) {
            const float x = -200.0f + rng.next01() * 300.0f;
            const float y = -200.0f + rng.next01() * 300.0f;
            const glm::vec3 o(x, y, 260.0f);
            physics::HeightHit hit;
            if (!physics::raycast(hf, o, dir, 900.0f, hit)) { ++miss; continue; }
            const float h = physics::heightAt(hf, hit.point.x, hit.point.y);
            worst = std::max(worst, static_cast<double>(std::fabs(hit.point.z - h)));
        }
        char buf[128];
        std::snprintf(buf, sizeof(buf), "maxdiff=%.6f miss=%d", worst, miss);
        check("B5 45° 射线命中高度 vs heightAt（< 1 cm）",
              miss == 0 && worst < 0.01, buf);
    }
    {
        // B6：maxT 太短 → 必须报"没命中"，不能返回一个超出 maxT 的点
        const physics::Heightfield hf = asset.heightfield();
        physics::HeightHit hit;
        const bool ok = physics::raycast(hf, glm::vec3(0.0f, 0.0f, 5000.0f),
                                         glm::vec3(0.0f, 0.0f, -1.0f), 100.0f, hit);
        check("B6 maxT 不够时不命中", !ok);
    }
    {
        // B7：起点在地表以下 → t = 0 命中（而不是"走到地形外面才翻转符号"）
        SlopeField sf;
        sf.build(65, 1.0f, 1.0f);
        const float h = physics::heightAt(sf.hf, 20.0f, 20.0f);
        physics::HeightHit hit;
        const bool ok = physics::raycast(sf.hf, glm::vec3(20.0f, 20.0f, h - 5.0f),
                                         glm::vec3(0.0f, 0.0f, -1.0f), 50.0f, hit);
        check("B7 从地表以下出发 → t≈0 命中", ok && hit.t < 1.0e-4f);
    }
    {
        // B8：地形之外沿用边缘高度（不返回 0，否则"走出地形一步"= 掉坑）
        const physics::Heightfield hf = asset.heightfield();
        const glm::vec2 sz = asset.worldSize();
        // 取**正好**落在最后一个采样点上（x = +size/2）：这里最容易踩
        // 双线性的边界 bug（floor 给出 N-1，格子只有 N-2..N-1）。
        const float inside = physics::heightAt(hf, 0.5f * sz.x, 0.5f * sz.y);
        const float outside = physics::heightAt(hf, 0.5f * sz.x + 50.0f,
                                                0.5f * sz.y + 50.0f);
        checkNear("B8 地形外沿用边缘高度", static_cast<double>(outside),
                  static_cast<double>(inside), 1.0e-4);
    }

    // ============================================================
    // C 组：分块 LOD 与裙边
    // ============================================================
    std::vector<TerrainChunk> chunks = buildChunks(asset);
    {
        const int nx = chunkCountX(asset);
        const int ny = chunkCountY(asset);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "chunks=%zu want=%d",
                      chunks.size(), nx * ny);
        check("C1 块数与 (cols-1)/kChunkCells 一致",
              static_cast<int>(chunks.size()) == nx * ny, buf);
    }
    {
        // 三角形数逐级严格递减 —— 不然"LOD"只是个名字
        bool mono = true;
        char buf[200];
        std::snprintf(buf, sizeof(buf), "tris=");
        for (int lod = 0; lod < kLodCount; ++lod) {
            const std::size_t tris = chunks.front().lods[lod].idx.size() / 3u;
            char piece[32];
            std::snprintf(piece, sizeof(piece), "%zu ", tris);
            std::strncat(buf, piece, sizeof(buf) - std::strlen(buf) - 1);
            if (lod > 0 &&
                chunks.front().lods[lod].idx.size() >=
                    chunks.front().lods[lod - 1].idx.size()) {
                mono = false;
            }
        }
        check("C2 各级三角形数严格递减", mono, buf);
    }
    {
        // ⚠ **本阶段最关键的一条**：裙边必须比"粗边 vs 细边"的高度差更深。
        //   这条不过 → 远处地形会漏出天空（而近处完全正常，极易漏测）。
        float worst = 0.0f;
        for (const auto& ch : chunks) {
            for (int lod = 1; lod < kLodCount; ++lod) {
                worst = std::max(worst, ch.lods[lod].edgeGap);
            }
        }
        char buf[160];
        std::snprintf(buf, sizeof(buf), "maxGap=%.4f skirt=%.2f",
                      static_cast<double>(worst),
                      static_cast<double>(kSkirtDepth));
        check("C3 相邻 LOD 的最大高度差 < 裙边深度（接缝被兜住）",
              worst > 0.0f && worst < kSkirtDepth, buf);
    }
    {
        const int side = kChunkCells + 1;
        const std::size_t want =
            static_cast<std::size_t>(side) * side + 4u * 2u * static_cast<std::size_t>(side);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "verts=%zu want=%zu",
                      chunks.front().lods[0].verts.size(), want);
        check("C4 LOD0 顶点数 = 表面 (n+1)² + 四条裙边",
              chunks.front().lods[0].verts.size() == want, buf);
    }
    {
        // 裙边顶点与对应的表面顶点 uv 必须**相同**：否则裙边会在水平方向
        // 插值到别的纹素，边缘颜色与上面差一截（一条亮边）。
        const TerrainMeshData& m = chunks.front().lods[0];
        const int side = kChunkCells + 1;
        const std::size_t surfCount = static_cast<std::size_t>(side) * side;
        bool same = m.verts.size() > surfCount;
        int checked = 0;
        for (std::size_t k = surfCount; k + 1 < m.verts.size(); k += 2) {
            if (m.verts[k].uv != m.verts[k + 1].uv) same = false;
            if (m.verts[k].skirt != 0.0f || m.verts[k + 1].skirt != 1.0f) same = false;
            ++checked;
        }
        char buf[96];
        std::snprintf(buf, sizeof(buf), "pairs=%d", checked);
        check("C5 裙边上下顶点 uv 相同、skirt 标记正确", same && checked > 0, buf);
    }
    {
        // 块中心与 LOD 阈值的配合：块尺寸 64 m（32 格 × 2 m）
        // 阈值 = 1.5×块尺寸，之后每级 ×2 → 96 / 192 / 384 m
        const float cs = static_cast<float>(kChunkCells) * asset.desc.metersPerPixel;
        const bool ok = lodForDistance(0.0f, cs) == 0 &&
                        lodForDistance(cs * 1.4f, cs) == 0 &&
                        lodForDistance(cs * 1.6f, cs) == 1 &&
                        lodForDistance(cs * 2.9f, cs) == 1 &&
                        lodForDistance(cs * 3.1f, cs) == 2 &&
                        lodForDistance(cs * 5.9f, cs) == 2 &&
                        lodForDistance(cs * 6.1f, cs) == 3 &&
                        // 再远也封顶在最后一级（不能越界读 lods[]）
                        lodForDistance(cs * 1000.0f, cs) == kLodCount - 1;
        check("C6 lodForDistance 的阈值与封顶", ok);
    }

    // ============================================================
    // D 组：角色在坡上不下滑、不抖动（G21 验收）
    // ============================================================
    {
        // 30° 坡：n.z = cos30 ≈ 0.866 > cos46 ≈ 0.694 → 站得住
        //
        // ⚠ 起点**从空中落下**，不是直接摆在"静止高度"上。
        //   斜面上胶囊中心到底面的**竖直**距离是 (halfHeight+radius)/n.z
        //   ≈ 1.04 m，而不是 0.90 m。直接按 0.90 m 摆会摆到"已经陷进去
        //   0.12 m"的位置，⑥ 的 excess < -0.02 判定为穿透 → 不接受这份
        //   地面 → 一路自由落体（第一版自检就是这么写错的，D1~D3 全红、
        //   而 D4 反而"假通过"：它断言的是"往下滑"，自由落体也算下滑）。
        SlopeField sf;
        const float rise = std::tan(30.0f * 3.14159265358979f / 180.0f);
        sf.build(129, 1.0f, rise);
        physics::PhysicsWorld world;
        physics::CharacterDesc desc;
        desc.ground = &sf.hf;
        physics::CharacterState st;
        st.position = glm::vec3(30.0f, 30.0f,
                                physics::heightAt(sf.hf, 30.0f, 30.0f) + 1.5f);
        st.onGround = false;

        // 前 90 帧用来落地（1.5 m 自由落体 ≈ 0.55 s = 33 帧，留足余量），
        // 之后 90 帧才是"站在坡上"的稳态段。
        const int kLandFrames = 90;
        const int kTotalFrames = 180;
        glm::vec3 prev = st.position;
        glm::vec3 settled = st.position;
        double worstStep = 0.0;
        int grounded = 0;
        for (int i = 0; i < kTotalFrames; ++i) {
            physics::stepCharacter(world, desc, st, glm::vec3(0.0f), 1.0f / 60.0f);
            if (i + 1 == kLandFrames) settled = st.position;
            if (i >= kLandFrames) {
                worstStep = std::max(
                    worstStep,
                    static_cast<double>(glm::length(st.position - prev)));
                if (st.onGround) ++grounded;
            }
            prev = st.position;
        }
        const double drift = static_cast<double>(glm::length(st.position - settled));
        char buf[220];
        std::snprintf(buf, sizeof(buf),
                      "worstStep=%.5f drift=%.5f grounded=%d/%d nz=%.3f",
                      worstStep, drift, grounded, kTotalFrames - kLandFrames,
                      static_cast<double>(st.groundNormalZ));
        check("D1 30° 坡上稳态段不下滑（总位移 < 5 cm）", drift < 0.05, buf);
        check("D2 30° 坡上逐帧位移 < 5 mm（不抖动）", worstStep < 0.005, buf);
        check("D3 30° 坡上稳态段全程贴地", grounded == kTotalFrames - kLandFrames,
              buf);
    }
    {
        // 60° 坡：n.z = 0.5 < cos46 → 不算地面 → 应该滑下去
        SlopeField sf;
        const float rise = std::tan(60.0f * 3.14159265358979f / 180.0f);
        sf.build(129, 1.0f, rise);
        physics::PhysicsWorld world;
        physics::CharacterDesc desc;
        desc.ground = &sf.hf;
        physics::CharacterState st;
        st.position = glm::vec3(20.0f, 20.0f,
                                physics::heightAt(sf.hf, 20.0f, 20.0f) + 1.5f);
        st.onGround = false;
        for (int i = 0; i < 60; ++i) {  // 先落地
            physics::stepCharacter(world, desc, st, glm::vec3(0.0f), 1.0f / 60.0f);
        }
        const glm::vec3 landed = st.position;
        int groundedFrames = 0;
        for (int i = 0; i < 120; ++i) {
            physics::stepCharacter(world, desc, st, glm::vec3(0.0f), 1.0f / 60.0f);
            if (st.onGround) ++groundedFrames;
        }
        const glm::vec3 d = st.position - landed;
        const double drop = static_cast<double>(-d.z);
        char buf[160];
        std::snprintf(buf, sizeof(buf), "dz=%.4f dx=%.4f grounded=%d/120",
                      static_cast<double>(d.z), static_cast<double>(d.x),
                      groundedFrames);
        check("D4 60° 坡上站不住（往下滑 > 0.5 m）",
              drop > 0.5 && groundedFrames < 120, buf);
    }
    {
        // 回落：没有高度场时必须与旧行为完全一致（贴在地面上）
        physics::PhysicsWorld world;
        physics::RigidBody g;
        g.type = physics::BodyType::Static;
        g.shape = physics::Shape::plane(glm::vec3(0.0f, 0.0f, 1.0f), 0.0f);
        world.addBody(g);
        physics::CharacterDesc desc;  // ground 保持 nullptr
        physics::CharacterState st;
        st.position = glm::vec3(0.0f, 0.0f, 1.0f);
        for (int i = 0; i < 60; ++i) {
            physics::stepCharacter(world, desc, st, glm::vec3(0.0f), 1.0f / 60.0f);
        }
        char buf[128];
        std::snprintf(buf, sizeof(buf), "z=%.5f onGround=%d",
                      static_cast<double>(st.position.z), st.onGround ? 1 : 0);
        check("D5 无高度场时回落到 groundPlane（z ≈ 0.9）",
              st.onGround && std::fabs(st.position.z - 0.9f) < 0.01f, buf);
    }

    char summary[96];
    std::snprintf(summary, sizeof(summary), "pass=%d fail=%d", g_pass, g_fail);
    VK_LOG_INFO("TERRAIN-TOTAL %s", summary);
    return g_fail == 0 ? 0 : 1;
}

} // namespace terrain
