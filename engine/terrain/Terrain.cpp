#include "terrain/Terrain.h"

#include "assets/AssetPath.h"
#include "core/Logger.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

namespace terrain {

namespace {

using json = nlohmann::json;

bool readBinary(const std::string& absPath, std::vector<uint8_t>& out) {
    std::ifstream f(absPath, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    if (size < 0) return false;
    f.seekg(0, std::ios::beg);
    out.resize(static_cast<std::size_t>(size));
    if (size > 0) {
        f.read(reinterpret_cast<char*>(out.data()), size);
        if (!f) return false;
    }
    return true;
}

// 目录部分（含末尾斜杠）。`.vterrain` 里的两份二进制相对**它**解析，
// 而不是相对 assets 根 —— 否则地形资产就没法搬家。
std::string dirOf(const std::string& relPath) {
    const std::size_t pos = relPath.find_last_of("/\\");
    if (pos == std::string::npos) return std::string();
    return relPath.substr(0, pos + 1);
}

// `.vterrain` 的 JSON → TerrainDesc（含全部硬校验）。
//
// ⚠ 抽出来是**为了只有一条解析路径**：Inspector 的 Terrain 段只想看
//   尺寸 / maxHeight / 层材质，不该为此把 264 KB 的高度图和权重图读进内存
//   （那是 `loadTerrain` 干的活）。两条各写一遍解析 → 两边校验迟早对不上，
//   于是"编辑器里显示 257² 但加载报 size_too_small"这种自相矛盾会出现。
bool parseDesc(const nlohmann::json& doc, TerrainDesc& d, std::string& err) {
    if (!doc.is_object()) {
        err = "not_object";
        return false;
    }
    d.cols = doc.value("cols", 0);
    d.rows = doc.value("rows", 0);
    d.metersPerPixel = doc.value("metersPerPixel", 1.0f);
    d.maxHeight = doc.value("maxHeight", 1.0f);
    d.splatRepeat = doc.value("splatRepeat", 1.0f);
    d.layerTiling = doc.value("layerTiling", 8.0f);
    d.heightFile = doc.value("height", std::string("height.r32"));
    d.splatFile = doc.value("splat", std::string("splat.rgba8"));

    if (d.cols < 3 || d.rows < 3) {
        err = "size_too_small";
        return false;
    }
    if (d.metersPerPixel <= 0.0f) {
        err = "bad_mpp";
        return false;
    }
    if (d.maxHeight <= 0.0f) {
        err = "bad_maxh";
        return false;
    }
    // —— 与分块网格的硬约束：块边界必须落在采样点上 ——
    if ((d.cols - 1) % kChunkCells != 0 || (d.rows - 1) % kChunkCells != 0) {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "cols_rows_not_multiple_of_%d cols=%d rows=%d",
                      kChunkCells, d.cols, d.rows);
        err = buf;
        return false;
    }

    // 层材质：允许少于 4 个（缺的层权重恒 0，渲染侧绑缺省白图），
    // 但第 0 层必须有 —— 没有 albedo 的地形是一片纯黑。
    if (doc.contains("layers") && doc["layers"].is_array()) {
        const auto& arr = doc["layers"];
        for (std::size_t i = 0; i < 4 && i < arr.size(); ++i) {
            if (arr[i].is_string()) d.layerIds[i] = arr[i].get<std::string>();
        }
    }
    if (d.layerIds[0].empty()) {
        err = "layer0_missing";
        return false;
    }
    return true;
}

} // namespace

bool loadTerrainDesc(const std::string& relPath, TerrainDesc& out,
                     std::string* err) {
    out = TerrainDesc{};
    const std::string abs = assets::resolveAssetPath(relPath);
    std::ifstream f(abs);
    if (!f) {
        if (err) *err = "cannot_open " + abs;
        return false;
    }
    json doc;
    try {
        f >> doc;
    } catch (const std::exception& e) {
        if (err) *err = std::string("json ") + e.what();
        return false;
    }
    std::string why;
    if (!parseDesc(doc, out, why)) {
        if (err) *err = why;
        return false;
    }
    return true;
}

physics::Heightfield TerrainAsset::heightfield() const {
    physics::Heightfield hf;
    hf.cols = desc.cols;
    hf.rows = desc.rows;
    hf.metersPerPixel = desc.metersPerPixel;
    hf.heightScale = desc.maxHeight;
    // 地形以自身原点为中心：采样点 (0,0) 在 (-size/2, -size/2)
    hf.origin = -0.5f * worldSize();
    hf.heights = heights.empty() ? nullptr : heights.data();
    return hf;
}

bool loadTerrain(const std::string& relPath, TerrainAsset& out, std::string* err,
                 bool quiet) {
    out = TerrainAsset{};
    out.path = relPath;

    auto fail = [&](const std::string& why) {
        if (err) *err = why;
        VK_LOG_ERROR("TERRAIN-FAIL path=%s reason=%s", relPath.c_str(),
                     why.c_str());
        return false;
    };

    const std::string abs = assets::resolveAssetPath(relPath);
    std::ifstream f(abs);
    if (!f) return fail("cannot_open " + abs);

    json doc;
    try {
        f >> doc;
    } catch (const std::exception& e) {
        return fail(std::string("json ") + e.what());
    }
    if (!doc.is_object()) return fail("not_object");

    TerrainDesc d;
    std::string why;
    if (!parseDesc(doc, d, why)) return fail(why);

    const std::string base = dirOf(relPath);

    // ---- height.r32 ----
    std::vector<uint8_t> hbytes;
    if (!readBinary(assets::resolveAssetPath(base + d.heightFile), hbytes)) {
        return fail("height_read " + base + d.heightFile);
    }
    const std::size_t want = static_cast<std::size_t>(d.cols) *
                             static_cast<std::size_t>(d.rows) * 4u;
    if (hbytes.size() != want) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "height_size got=%zu want=%zu",
                      hbytes.size(), want);
        return fail(buf);
    }
    std::vector<float> heights(want / 4u);
    std::memcpy(heights.data(), hbytes.data(), want);

    // ---- splat.rgba8 ----
    std::vector<uint8_t> splat;
    if (!readBinary(assets::resolveAssetPath(base + d.splatFile), splat)) {
        return fail("splat_read " + base + d.splatFile);
    }
    const std::size_t swant = static_cast<std::size_t>(d.cols) *
                              static_cast<std::size_t>(d.rows) * 4u;
    if (splat.size() != swant) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "splat_size got=%zu want=%zu",
                      splat.size(), swant);
        return fail(buf);
    }

    // ---- 归一化检查 / 自动救援（2026-10-22 第十一轮）----
    //
    // `.r32` 的契约是 [0, 1]：1.0 代表"等于 maxHeight"。
    //
    // 但编辑器的抬升笔刷**故意**允许冲到 kMaxNorm = 8（那是"山峰不撞平顶"
    // 的余量），而老版本 `saveAssets()` 把这份未收敛的数组原样写盘了。
    // 于是形成闭环矛盾：写端允许 8、读端只接受 1 —— 用户抬完山保存一
    // 次，工程就再也打不开了（`TERRAIN-INSTANCE-FAIL height_not_normalized`）。
    //
    // 修法分两半，这里是"读端兜住存量坏数据"那一半：
    //   · 写端（LandscapeEditor::saveAssets）落盘前先收敛，保证磁盘上永远
    //     是 [0,1] —— 那是根治；
    //   · 读端遇到已经写坏的资产**按最高点重整**，让用户能打开、能看见、
    //     能继续编辑。因为世界高度是 `h * maxHeight`，同乘同除结果恒等。
    //
    // 为什么负高度仍然硬失败：抬山笔刷的下限是 0（`clamp(h, 0, kMaxNorm)`），
    // 编不出负数。负值只可能来自外部工具/生成器算错 —— 那就该炸出来看日志，
    // 而不是被"救援"悄悄抹平。
    float lo = 1.0e30f, hi = -1.0e30f;
    for (float h : heights) {
        lo = std::min(lo, h);
        hi = std::max(hi, h);
    }
    if (!std::isfinite(lo) || !std::isfinite(hi) || lo < -0.01f) {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "height_not_normalized min=%.3f max=%.3f",
                      static_cast<double>(lo), static_cast<double>(hi));
        return fail(buf);
    }
    if (hi > 1.01f) {
        // 上限保护：超过 1e6 的"最高点"不是抬山能产生的，多半是文件被截断/
        // 字节序搞反了读出来的垃圾 float。这种重整出来的 maxHeight 会是 inf。
        if (hi > 1.0e6f) {
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "height_not_normalized min=%.3f max=%.3f",
                          static_cast<double>(lo), static_cast<double>(hi));
            return fail(buf);
        }
        const float inv = 1.0f / hi;
        for (float& h : heights) h *= inv;
        const float oldMax = d.maxHeight;
        d.maxHeight *= hi;
        out.rescaled = true;
        // 日志里的 min/max 要报**收敛后**的值，与内存里的 heights 一致。
        // 否则日志写着 "max=8.000" 而实际数组是 [0,1]，下次排查会被它带偏。
        lo *= inv;
        hi *= inv;
        VK_LOG_WARN("TERRAIN-RENORMALIZE path=%s peak=%.3f maxHeight %.4f -> "
                    "%.4f（世界高度不变；下次保存会写回规范数据）",
                    relPath.c_str(), static_cast<double>(1.0f / inv),
                    static_cast<double>(oldMax),
                    static_cast<double>(d.maxHeight));
    }

    out.desc = d;
    out.heights = std::move(heights);
    out.splat = std::move(splat);

    // ---- holes.r8（可选）----
    // 读不到不是错误，只是"这块地形没挖过洞"。用 255 填满而不是留空：
    // 渲染侧与笔刷侧都按"每采样点一个字节"无条件访问。
    out.holes.assign(static_cast<std::size_t>(d.cols) *
                         static_cast<std::size_t>(d.rows),
                     255u);
    {
        std::vector<uint8_t> hmask;
        if (readBinary(assets::resolveAssetPath(base + d.holeFile), hmask) &&
            hmask.size() == out.holes.size()) {
            out.holes.swap(hmask);
        }
    }
    out.ok = true;

    // detail 里**不能有空格**（项目铁律）。路径里的空格换成下划线。
    // quiet：笔刷刷新时每 80 ms 读一次，日志会被这条灌满（实测一笔刷下来
    // 几十行 TERRAIN-LOAD），而且渲染层的调用方自己会打更有用的
    // TERRAIN-REFRESH —— 所以那条路径把它关掉。
    if (quiet) return true;
    std::string p = relPath;
    for (char& c : p) if (c == ' ') c = '_';
    VK_LOG_INFO("TERRAIN-LOAD path=%s size=%dx%d mpp=%.3f maxh=%.2f "
                "layers=%d chunk=%dx%d min=%.3f max=%.3f",
                p.c_str(), d.cols, d.rows, static_cast<double>(d.metersPerPixel),
                static_cast<double>(d.maxHeight),
                static_cast<int>(d.layerIds[0].empty() ? 0 : 4),
                chunkCountX(out), chunkCountY(out),
                static_cast<double>(lo), static_cast<double>(hi));
    return true;
}

// ------------------------------------------------------------
// 值域收敛（与 loadTerrain 里的救援共用同一套数学）
// ------------------------------------------------------------
float normalizeHeightRange(TerrainAsset& asset) {
    if (asset.heights.empty()) return 1.0f;
    float hi = 0.0f;
    for (float h : asset.heights) {
        if (h > hi) hi = h;
    }
    // 留 1e-5 的余量：笔刷的 clamp 是精确的 1.0，但浮点累加会给出
    // 1.0000001 这种值 —— 不值得为它做一次全量缩放。
    if (!(hi > 1.0f + 1.0e-5f)) return 1.0f;
    // 上限保护：与 loadTerrain 的救援同口径。超过 1e6 的"峰值"不是抬山
    // 能产生的，多半是文件被截断/读成垃圾 float —— 缩放它只会放大错误。
    if (!std::isfinite(hi) || hi > 1.0e6f) return 1.0f;

    const float inv = 1.0f / hi;
    for (float& h : asset.heights) h *= inv;
    asset.desc.maxHeight *= hi;
    return inv;
}

bool rewriteMaxHeight(const std::string& relPath, float maxHeight) {
    const std::string abs = assets::resolveAssetPath(relPath);
    json j = json::object();
    {
        std::ifstream jf(abs);
        if (jf) {
            try {
                jf >> j;
            } catch (...) {
                j = json::object();
            }
        }
    }
    if (!j.is_object()) j = json::object();
    j["maxHeight"] = maxHeight;
    std::ofstream jo(abs, std::ios::trunc);
    if (!jo) return false;
    jo << j.dump(2) << "\n";
    return static_cast<bool>(jo);
}

// ------------------------------------------------------------
// 网格生成
// ------------------------------------------------------------
namespace {

// 一个顶点在整张地形里的采样坐标 → 顶点
TerrainVertex makeVertex(const TerrainDesc& d, int col, int row, float skirt) {
    // 平地坐标：以地形中心为原点
    const float cx = static_cast<float>(d.cols - 1) * 0.5f;
    const float cy = static_cast<float>(d.rows - 1) * 0.5f;
    TerrainVertex v;
    v.pos = glm::vec3((static_cast<float>(col) - cx) * d.metersPerPixel,
                      (static_cast<float>(row) - cy) * d.metersPerPixel, 0.0f);
    // ⚠ 纹素中心对齐：直接写 col/(cols-1) 会让 uv 落在纹素**边界**上，
    //   线性过滤在那里是相邻两个纹素各占一半 —— 与 CPU 侧"整数坐标取
    //   单个采样点"差半个像素，正是"角色站在地形上悬空 2 cm"的来源。
    v.uv = glm::vec2((static_cast<float>(col) + 0.5f) / static_cast<float>(d.cols),
                     (static_cast<float>(row) + 0.5f) / static_cast<float>(d.rows));
    v.skirt = skirt;
    return v;
}

// 在共享边上量"这一级相对上一级"的最大高度误差。
// 上一级（更细）沿边每隔 step/2 有一个采样点，粗边是相邻两点之间的直线，
// 两者之差就是裙边必须兜住的缝。
float edgeGapFor(const TerrainAsset& a, int col0, int row0, int step,
                 int edgeIndex) {
    if (step <= 1) return 0.0f;
    const int half = step / 2;
    const TerrainDesc& d = a.desc;
    float worst = 0.0f;
    // 四条边：0 = -Y（row0 恒定）、1 = +Y、2 = -X（col0 恒定）、3 = +X
    for (int k = 0; k < kChunkCells; k += step) {
        const int c0 = (edgeIndex == 0 || edgeIndex == 1) ? col0 + k : col0;
        const int r0 = (edgeIndex == 0 || edgeIndex == 1) ? row0 : row0 + k;
        const int c1 = (edgeIndex == 0 || edgeIndex == 1) ? c0 + step : c0;
        const int r1 = (edgeIndex == 0 || edgeIndex == 1) ? r0 : r0 + step;
        const float hc0 = a.heights[static_cast<std::size_t>(c0) * d.rows + r0];
        const float hc1 = a.heights[static_cast<std::size_t>(c1) * d.rows + r1];
        const int mc = (c0 + c1) / 2;
        const int mr = (r0 + r1) / 2;
        const float hm = a.heights[static_cast<std::size_t>(mc) * d.rows + mr];
        worst = std::max(worst, std::fabs(0.5f * (hc0 + hc1) - hm));
        (void)half;
    }
    return worst * d.maxHeight;  // 归一化 → 米
}

} // namespace

std::vector<TerrainChunk> buildChunks(const TerrainAsset& asset) {
    std::vector<TerrainChunk> chunks;
    if (!asset.ok) return chunks;
    const TerrainDesc& d = asset.desc;
    const int nx = chunkCountX(asset);
    const int ny = chunkCountY(asset);
    chunks.resize(static_cast<std::size_t>(nx) * static_cast<std::size_t>(ny));

    const float halfCx = static_cast<float>(d.cols - 1) * 0.5f;
    const float halfCy = static_cast<float>(d.rows - 1) * 0.5f;
    const float chunkSizeM = static_cast<float>(kChunkCells) * d.metersPerPixel;

    for (int cyi = 0; cyi < ny; ++cyi) {
        for (int cxi = 0; cxi < nx; ++cxi) {
            TerrainChunk& ch = chunks[static_cast<std::size_t>(cyi) * nx + cxi];
            ch.cx = cxi;
            ch.cy = cyi;
            // 块中心：格坐标 → 世界（地形以中心为原点）
            ch.center = glm::vec2(
                (static_cast<float>(cxi * kChunkCells) +
                 static_cast<float>(kChunkCells) * 0.5f - halfCx) * d.metersPerPixel,
                (static_cast<float>(cyi * kChunkCells) +
                 static_cast<float>(kChunkCells) * 0.5f - halfCy) * d.metersPerPixel);
            ch.radius = chunkSizeM * 0.7072f;  // 半对角线

            for (int lod = 0; lod < kLodCount; ++lod) {
                const int step = 1 << lod;
                const int n = kChunkCells / step;  // 每边格数
                TerrainMeshData& m = ch.lods[static_cast<std::size_t>(lod)];
                const int baseCol = cxi * kChunkCells;
                const int baseRow = cyi * kChunkCells;

                // ---- 表面顶点：(n+1)² ----
                const int side = n + 1;
                m.verts.reserve(static_cast<std::size_t>(side) * side +
                                4u * 2u * static_cast<std::size_t>(side));
                for (int j = 0; j <= n; ++j) {
                    for (int i = 0; i <= n; ++i) {
                        m.verts.push_back(makeVertex(d, baseCol + i * step,
                                                     baseRow + j * step, 0.0f));
                    }
                }

                // ---- 表面索引：每格两个三角形，逆时针（从 +Z 看）----
                m.idx.reserve(static_cast<std::size_t>(n) * n * 6u);
                for (int j = 0; j < n; ++j) {
                    for (int i = 0; i < n; ++i) {
                        const std::uint32_t v00 =
                            static_cast<std::uint32_t>(j * side + i);
                        const std::uint32_t v10 = v00 + 1u;
                        const std::uint32_t v01 = v00 + static_cast<std::uint32_t>(side);
                        const std::uint32_t v11 = v01 + 1u;
                        m.idx.push_back(v00);
                        m.idx.push_back(v10);
                        m.idx.push_back(v11);
                        m.idx.push_back(v00);
                        m.idx.push_back(v11);
                        m.idx.push_back(v01);
                    }
                }

                // ---- 裙边：四条边各一圈，底面顶点 skirt=1 ----
                // 顶点成对追加（上、下），索引按"上a, 下a, 上b" / "上b, 下a, 下b"
                // 组两个三角形。绕序无所谓：地形管线 cullMode = NONE
                // （见 Renderer::createTerrainPipeline 的注释）。
                auto addSkirtEdge = [&](const std::vector<glm::ivec2>& path) {
                    const std::uint32_t start =
                        static_cast<std::uint32_t>(m.verts.size());
                    for (const auto& cr : path) {
                        m.verts.push_back(makeVertex(d, cr.x, cr.y, 0.0f));
                        m.verts.push_back(makeVertex(d, cr.x, cr.y, 1.0f));
                    }
                    for (std::size_t k = 0; k + 1 < path.size(); ++k) {
                        const std::uint32_t top0 = start + static_cast<std::uint32_t>(k * 2);
                        const std::uint32_t bot0 = top0 + 1u;
                        const std::uint32_t top1 = top0 + 2u;
                        const std::uint32_t bot1 = top0 + 3u;
                        m.idx.push_back(top0);
                        m.idx.push_back(bot0);
                        m.idx.push_back(top1);
                        m.idx.push_back(top1);
                        m.idx.push_back(bot0);
                        m.idx.push_back(bot1);
                    }
                };
                std::vector<glm::ivec2> path;
                path.reserve(static_cast<std::size_t>(side));
                // -Y 边
                path.clear();
                for (int i = 0; i <= n; ++i)
                    path.emplace_back(baseCol + i * step, baseRow);
                addSkirtEdge(path);
                // +Y 边
                path.clear();
                for (int i = 0; i <= n; ++i)
                    path.emplace_back(baseCol + i * step, baseRow + kChunkCells);
                addSkirtEdge(path);
                // -X 边
                path.clear();
                for (int j = 0; j <= n; ++j)
                    path.emplace_back(baseCol, baseRow + j * step);
                addSkirtEdge(path);
                // +X 边
                path.clear();
                for (int j = 0; j <= n; ++j)
                    path.emplace_back(baseCol + kChunkCells, baseRow + j * step);
                addSkirtEdge(path);

                // 误差：取四条边上最坏的那条
                float gap = 0.0f;
                if (lod > 0) {
                    for (int e = 0; e < 4; ++e) {
                        gap = std::max(gap, edgeGapFor(asset, baseCol, baseRow,
                                                       step, e));
                    }
                }
                m.edgeGap = gap;
            }
        }
    }
    return chunks;
}

} // namespace terrain
