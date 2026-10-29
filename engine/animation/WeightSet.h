#pragma once
// ============================================================
// animation/WeightSet —— **顶点组**（蒙皮权重的可编辑层，S-B）
//
// 为什么要有这一层（2026-10-29，S-B）：
//   导入之后每个顶点的权重只活在 `assets::Vertex::joints/weights` 的**四个
//   槽位**里。那是个**只能读、不能改**的产物，而且它丢了两样东西：
//     · 哪些骨**不该**有权重（控制器骨 / 用来摆姿势的辅助骨）；
//     · "这个顶点的权重是不是被作者调过"。
//   于是"权重绘制"这件事在数据上无处落脚 —— 想在 Blender 里那样刷一下，
//   只能直接改 Vertex 数组再整个重传，改坏了没有回头路（导入路径是
//   `AssetManager` 的缓存，重来一次要重新解析 FBX）。
//
//   所以这里另起一层：每顶点一张 (骨, 权重) 的小表，**不塞进
//   `assets::Mesh`**（`engine/assets/**` 在 GLOBAL_GLOBS 里，动它等于全量
//   回归）。烘焙时才写成 `JOINTS_0/WEIGHTS_0` 四槽，通过
//   `Mesh::upload()` 重新上传 —— 全仓库唯一的上传入口，不另开后门。
//
// 三条设计约定（和 `RigEdit` 的三条是同一个套路，省一条都会静默出错）：
//
//   ① **没改过的顶点，烘焙时逐位照抄原槽位。** 存一份 `origJoints /
//      `origWeights`，`touched[v] == false` 就把它们原样写回。理由是硬的：
//      GPU 只吃 4 槽，而我们的表能存 8 条 —— 只要"读出来再排个序写回去"，
//      权重就会以 1e-7 的量级漂。单看没有，但"打开权重页 → 存盘 → 角色
//      姿态变了一点点"是最难归因的一类回归，而它正是本模块唯一的验收底线：
//      **零编辑烘焙 ≡ 导入路径**。
//
//   ② **每个顶点的权重和恒为 1。** 不是审美要求：`pbr.vert` 是按
//      `weights` 之和 > 0 分支的（见 `assets/Mesh.h` 那段注释）。
//      一个顶点若"有表但和为 0"，它与全 0 无法区分 —— 走静态路径，
//      顶点留在**网格空间**，角色一动就是"身上粘着一坨没跟上的皮"。
//      所以任何会让和变 0 的操作（减权重、清骨、mask 掉非 deform 骨）
//      都必须就地重归一化，或者明确落成"孤立顶点"并统计出来。
//
//   ③ **只给 `deform` 的骨算权重。** 这是 S-A 的 `deform` 标志的第一个
//      真实消费者。少了它，自动权重会把控制器骨（`*_ctrl` / `*_ik` /
//      `*_end`）一起算进去 —— 那些骨在世界里恰好穿过身体，权重会被
//      正经的骨抢走，症状是"抬一下手指，胳膊肘塌了"。
//
// 坐标系：顶点位置是**网格空间**（= `assets::Vertex::pos` 的原样），
//   而 `RigEdit` 的 `head / dir` 是**骨架空间**（= `restLocal` 累乘出来
//   那个空间）。两者差一个 `inverse(invMeshNode)`，由调用方通过
//   `meshToRig` 传进来 —— glTF 路径下它不是单位阵（Y-up→Z-up 与节点
//   变换都藏在里面），FBX 路径下是单位阵。写死单位阵的表现在画面上是
//   "权重全跑到脚上"，而日志一行都不报。
// ============================================================

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace assets {
class Mesh;
struct Vertex;
} // namespace assets

namespace animation {

struct RigEdit;   // animation/RigEdit.h

// 每顶点最多记几条影响。GPU 只吃 4 槽（JOINTS_0/WEIGHTS_0），
// 多出来的在 `bake` 时按权重大小截断 —— 记 8 条是为了让"截断"这件事
// **可断言**（`stats().overFour` 能数出来），而不是在写入时就悄悄丢掉。
constexpr int kMaxInfluence = 8;

struct WeightEntry {
    int bone = -1;         // 骨架内 joint 下标
    float weight = 0.0f;
};

// 统计（UI 角标与日志都读它）
struct WeightStats {
    int vertices = 0;
    int touched = 0;        // 作者动过的顶点数
    int unweighted = 0;     // 一条权重都没有的顶点（约定 ②：必须数出来）
    int overFour = 0;       // 影响条数 > 4（烘焙会截断）
    int maxInfluences = 0;
    double avgInfluences = 0.0;
    float minSum = 0.0f;    // 有效顶点里的最小 / 最大权重和
    float maxSum = 0.0f;
};

enum PaintMode : int {
    kPaintAdd = 0,        // w += delta
    kPaintSubtract = 1,   // w -= delta
    kPaintSet = 2,        // w  = value
};

// 自动权重参数（包络 / 距离衰减，不是 bone-heat）
struct AutoWeightParams {
    // 每根骨的**作用半径** = 骨长 × radiusScale（有下限 minRadius）。
    // 骨长做基准而不是写死一个米数：角色的手指和脊梁不可能共用一个半径。
    float radiusScale = 1.6f;
    float minRadius = 0.02f;
    // 衰减幂次：w = clamp(1 − d/R, 0, 1)^falloff。
    // 1 = 线性（接缝软而糊）、2 = 默认（Blender 包络的观感）、4 = 硬。
    float falloff = 2.0f;
    // 每顶点保留几条（写表时就截断，比"先存 8 条再烤掉"省内存）。
    int maxInfluence = 4;
    // 只给 deform 骨算（约定 ③）。关掉只在"我想看看控制器骨的影响范围"时用。
    bool deformOnly = true;
};

struct WeightSet {
    std::string source;    // 来源模型路径（显示 + 日志）
    std::string mesh;      // 子网格名（`assets::SubMesh::name`）
    int vertexCount = 0;

    // 定长表：第 v 个顶点的条目在 entries[v * kMaxInfluence, ...)，
    // 有效条数由 count[v] 给。定长而不是 `vector<vector<>>`：
    // 绘制时每帧要随机访问几万个顶点，散开的堆块是主要开销。
    std::vector<WeightEntry> entries;
    std::vector<unsigned char> count;
    // 作者动过没有（约定 ①）。`setWeight/addWeight/normalize/...` 都会置真。
    std::vector<char> touched;

    // 导入时的四槽**原样**（约定 ①：没动过就逐位照抄）
    std::vector<glm::uvec4> origJoints;
    std::vector<glm::vec4> origWeights;

    int influenceCount(int v) const;
    float weightOf(int v, int bone) const;
    bool touchedAny() const;
    void clear();

    // ------------------------------------------------------------
    // 导入 / 烘焙
    // ------------------------------------------------------------

    // 从网格的 CPU 顶点读一份可编辑权重。**不改 mesh 一个字节**（纯读）。
    // 只保留 `weight > 0` 的槽位（导入器常写 `(5,0,0,0)/(1,0,0,0)` 这种
    // 带零尾巴的四元组，原样留着会让"影响条数"虚高 4 倍）。
    static WeightSet fromMesh(const assets::Mesh& mesh,
                              const std::string& sourcePath = std::string());
    // 同上，但直接吃顶点数组 —— 自检没有 GPU、造不出 `assets::Mesh`
    //（`Mesh::vertices()` 只有 `upload()` 之后才有内容）。两者共用一份实现。
    static WeightSet fromVertices(const std::vector<assets::Vertex>& verts,
                                  const std::string& meshName,
                                  const std::string& sourcePath = std::string());

    // 烘焙成 `JOINTS_0/WEIGHTS_0` 四槽写回 `verts`（`vertexCount` 个）。
    //   · 没动过的顶点：`origJoints/origWeights` 逐位照抄（约定 ①）；
    //   · 动过的顶点：按权重降序取前 4，重归一化后写入（约定 ②）。
    // `deformMask` 非空时（长度 = 骨数）：mask 掉的骨**整体丢掉**再归一化。
    // 返回**真正变了**的顶点数（0 = 什么都不用重传 GPU）。
    int bake(assets::Vertex* verts, const std::vector<char>* deformMask = nullptr) const;

    // ------------------------------------------------------------
    // 编辑（UI 与注入脚本共用这一层 —— 验收因此不依赖鼠标坐标）
    // ------------------------------------------------------------

    // 绝对设置。`w <= kWeightEps` 视为"删掉这条"。
    // 表满（kMaxInfluence 条）且是新骨时：替换**最小**的那条（返回 1）。
    int setWeight(int v, int bone, float w);
    int addWeight(int v, int bone, float delta, float lo = 0.0f, float hi = 1.0f);

    // 就地归一化（和为 0 时不动 —— 约定 ②：宁可是"孤立顶点"也不要造出
    // 一堆 0 权重）。返回是否改动。
    bool normalizeVertex(int v);
    int normalizeAll();

    // 刷子：对一批顶点做同一次操作。`autoNormalize` = 每次刷完立刻归一化
    // （Blender 的 Auto Normalize），关掉则是"刷子只管加，回头自己归一"。
    int paint(const std::vector<int>& verts, int bone, float value, PaintMode mode,
              bool autoNormalize = true);
    // 单顶点版。笔刷按"到笔心的距离"给每个顶点不同的量，所以真正的调用点是
    // 一个一个的 —— `paint` 就是它的循环包装（UI、脚本、自检共用这一份实现）。
    int paintVertex(int v, int bone, float value, PaintMode mode,
                    bool autoNormalize = true);

    // 把某根骨从所有顶点上摘掉（删骨 / 关掉 deform 之后收尾用）。
    int clearBone(int bone);

    // 平滑：把 `ringOff/ringIdx`（1-ring 邻接，见 `buildVertexRing`）上的
    // 权重与本顶点按 `strength` 混合 `rounds` 轮。只对**已有表的顶点**做
    // （没表的顶点参与"平均"会把权重扩散到整个网格）。
    int blur(const std::vector<int>& ringOff, const std::vector<int>& ringIdx,
             int rounds, float strength);

    // ------------------------------------------------------------
    // 自动权重（包络 / 距离衰减）
    // ------------------------------------------------------------

    // 每顶点取最近的若干 deform 骨，`w = clamp(1 − d/R, 0, 1)^falloff`，
    // 取前 `maxInfluence` 条并归一化。**落在所有包络之外的顶点会被塞给
    // 最近的那根骨（w = 1）** —— 约定 ②：宁可硬一点，也不许留孤立顶点。
    //
    // `vertPos` 是网格空间坐标；`meshToRig` = `inverse(invMeshNode)`，
    // 用来把骨（骨架空间）搬到网格空间，见文件头。
    // 返回赋过权重的顶点数。
    int autoAssign(const RigEdit& rig, const std::vector<glm::vec3>& vertPos,
                   const glm::mat4& meshToRig, const AutoWeightParams& p);

    // 把 v 的权重抄到"关于镜像平面镜像"的那个顶点上，骨名 `.L`/`.R` 互换。
    // `axis` = 0/1/2（与 `RigEdit::mirrorAxis` 同一套）；`eps` 是配对容差
    // （米）—— 真实角色左右顶点位置**不会**精确对称，1 mm 量级是常态。
    // 找不到配对顶点 / 骨名没有左右标记 → 返回 -1。
    int mirrorVertex(const RigEdit& rig, int axis,
                     const std::vector<glm::vec3>& vertPos, int v, float eps);

    WeightStats stats() const;
    void logSummary(const char* tag) const;

    // 逐顶点日志（`%a` 十六进制浮点，供逐位比对）
    void dumpVertex(int v, const char* tag) const;
    // FNV-1a 摘要：整张表压成一个数，供"两次操作结果一致 / 存读一致"的断言
    unsigned long long digest() const;
};

// 1-ring 邻接（CSR：`off[v]..off[v+1]` 是 `idx` 里的邻居区间）。
// 由索引表建一次，之后只读。两个入口共用一份实现（同上：自检没有 GPU）。
void buildVertexRing(const std::vector<uint32_t>& indices, int vertexCount,
                     std::vector<int>& ringOff, std::vector<int>& ringIdx);
void buildVertexRing(const assets::Mesh& mesh, std::vector<int>& ringOff,
                     std::vector<int>& ringIdx);

// 每顶点位置（网格空间）。烘焙与自动权重都要，抽出来避免两处各拷一遍。
void meshVertexPositions(const assets::Mesh& mesh, std::vector<glm::vec3>& out);

// 权重和小于它就当 0（删条目 / 判孤立顶点用同一个阈值）
constexpr float kWeightEps = 1.0e-5f;

// ---- JSON 往返（`venn.weights` / version 1）----
// 只存**稀疏**条目（count > 0 的顶点）。读回来一律标成 touched ——
// 文件里的表是"作者写成这样的"，没有原槽位可抄了。
bool saveWeights(const std::string& path, const WeightSet& ws, std::string& err);
bool loadWeights(const std::string& path, WeightSet& out, std::string& err);

} // namespace animation
