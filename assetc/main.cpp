// ============================================================
// assetc/main.cpp —— AssetC：离线资产编译器
//
// 用法：
//   AssetC model.fbx                    → 同目录 model.vmesh
//   AssetC a.fbx b.fbx -o out/          → 批量，输出到 out/
//   AssetC assets/models --recursive    → 目录批处理
//   AssetC --info model.vmesh           → 只读体检（不解几何）
//   AssetC --check model.vmesh          → 校验完整性（CRC + 越界）
//
// 退出码：0 = 全部成功；1 = 有失败（批量模式不中断，逐个报）。
//
// 为什么这是个**独立可执行文件**而不是编辑器的一个子命令：
//   1. 它不该要求 Vulkan 设备活着（构建机 / CI 上没有）；
//   2. 它的依赖方向是 `assetc → assetio`，**不经过** engine/，
//      所以永远不会因为渲染层改动而需要重编这个工具；
//   3. 批量转码（一个目录几百个 FBX）是离线行为，跟"打开编辑器"
//      这件事没有关系，混在一起会让人以为必须先开编辑器才能烤资产。
// ============================================================

#include "assetio/AutoRig.h"
#include "assetio/Binary.h"
#include "assetio/FbxImport.h"
#include "assetio/MeshFormat.h"
#include "assetio/ProcAnim.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// 贴图路径必须写成"**相对资产根**"，资产才能连同贴图一起拷到别的机器。
// `--asset-root` 没给时按输入文件往上找最近的 `assets` 目录（本项目约定
// `<root>/assets`）；找不到就退化成该文件所在目录 —— 结果仍然是相对路径，
// 只是根不同。
//
// ⚠ 不推断的后果不是"贴图路径丑一点"，而是**全部为空**：
//   `relativizeTo(p, "")` 里有 `if (assetRoot.empty()) return "";`，
//   于是每个材质的 `albedoPath` 都是空 → `--info` 打 `albedo=-` →
//   角色渲染成**纯白石膏像**。Alicia 就这么白了一整轮
//   （见 docs/alicia_import.md §6 遗留 2）。
std::string guessAssetRoot(const std::string& input) {
    std::error_code ec;
    fs::path p = fs::weakly_canonical(fs::path(input), ec);
    if (ec) p = fs::path(input);
    for (fs::path q = p.parent_path(); !q.empty(); q = q.parent_path()) {
        if (q.filename() == "assets") return q.string();
        if (q == q.root_path()) break;
    }
    return p.parent_path().string();
}

struct Args {
    std::vector<std::string> inputs;
    std::string output;        // -o：文件或目录
    std::string assetRoot;
    float scale = 1.0f;
    float rotTolDeg = 0.1f;
    bool weld = true;
    bool optimize = true;
    bool reduceAnim = true;
    bool recursive = false;
    bool info = false;
    bool check = false;
    bool quiet = false;
    // ---- 自动绑骨（P2）----
    bool autRig = false;
    bool forceRig = false;
    bool stripSkin = false;
    // ---- 程序化动画（P3）----
    bool procAnim = false;
    float animFps = 30.0f;
};

void usage() {
    std::printf(
        "AssetC —— Venn 资产编译器（FBX → .vmesh）\n"
        "\n"
        "  AssetC <input.fbx|dir> [...] [options]\n"
        "\n"
        "选项：\n"
        "  -o <path>        输出文件或目录（默认：与源文件同目录同名 .vmesh）\n"
        "  --asset-root <d> 资产根（用于把贴图路径规范成相对路径）\n"
        "  --scale <s>      尺寸倍数（默认 1.0），对应 .vmeta 的 import.scale\n"
        "  --no-weld        不合并重复顶点\n"
        "  --no-optimize    不做顶点缓存重排\n"
        "  --no-anim-reduce 不做动画抽帧\n"
        "  --rot-tol <deg>  旋转抽帧容差（默认 0.1 度）\n"
        "  -r, --recursive  目录递归\n"
        "  --info           只打印 .vmesh 的体检信息\n"
        "  --check          校验 .vmesh 完整性（CRC/越界），不写任何东西\n"
        "  -q, --quiet      安静模式（只输出 ASSETC 结果行）\n"
        "  -h, --help       这条帮助\n"
        "\n"
        "自动绑骨（P2，对标 Mixamo Auto-Rigger）：\n"
        "  --autorig        对**静态**人形网格自动生成骨架 + 蒙皮权重\n"
        "  --force-rig      已有骨架时也覆盖\n"
        "  --strip-skin     丢掉骨架/动画/权重，产出静态网格\n"
        "                   （用来做\"先去掉原厂绑定、再自动绑\"的对照）\n"
        "\n"
        "程序化动画（P3）：\n"
        "  --procanim       生成 idle / walk / run / jump 四段基础动画\n"
        "                   （需要骨架；可跟在 --autorig 后面一次做完）\n"
        "  --anim-fps <n>   关键帧采样率（默认 30）\n");
}

bool isFbx(const fs::path& p) {
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".fbx";
}

bool isVmesh(const fs::path& p) {
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".vmesh";
}

std::vector<std::uint8_t> readFile(const fs::path& p, bool& ok) {
    std::ifstream f(p, std::ios::binary);
    if (!f) { ok = false; return {}; }
    f.seekg(0, std::ios::end);
    const std::streamoff n = f.tellg();
    if (n < 0) { ok = false; return {}; }
    f.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(n));
    if (n > 0) f.read(reinterpret_cast<char*>(buf.data()), n);
    ok = f.good() || f.eof();
    return buf;
}

bool writeFile(const fs::path& p, const std::vector<std::uint8_t>& data) {
    std::error_code ec;
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    if (!data.empty())
        f.write(reinterpret_cast<const char*>(data.data()),
                static_cast<std::streamsize>(data.size()));
    return f.good();
}

std::string humanBytes(std::uint64_t n) {
    char buf[64];
    if (n >= 1024ull * 1024ull)
        std::snprintf(buf, sizeof(buf), "%.2f MB", static_cast<double>(n) / (1024.0 * 1024.0));
    else if (n >= 1024ull)
        std::snprintf(buf, sizeof(buf), "%.1f KB", static_cast<double>(n) / 1024.0);
    else
        std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(n));
    return buf;
}

int doInfo(const Args& a) {
    for (const std::string& in : a.inputs) {
        bool ok = false;
        const std::vector<std::uint8_t> bytes = readFile(in, ok);
        if (!ok) {
            std::printf("ASSETC FAIL %s reason=read_error\n", in.c_str());
            return 1;
        }
        assetio::MeshFileInfo info;
        std::string err;
        if (!assetio::inspectMeshFile(bytes.data(), bytes.size(), info, &err)) {
            std::printf("ASSETC FAIL %s reason=%s\n", in.c_str(), err.c_str());
            return 1;
        }
        std::printf("ASSETC INFO %s\n", in.c_str());
        std::printf("ASSETC   version=%u flags=0x%x sections=%u bytes=%s\n",
                    info.version, info.flags, info.sectionCount,
                    humanBytes(info.bytes).c_str());
        std::printf("ASSETC   submeshes=%u vertices=%llu indices=%llu index16=%d\n",
                    info.subMeshCount,
                    static_cast<unsigned long long>(info.vertexCount),
                    static_cast<unsigned long long>(info.indexCount),
                    info.index16 ? 1 : 0);
        // 顶点格式世代（§13）。挂在**独立一行**上而不是接在上一行末尾：
        // verify_vmesh 的 B2 正则是 `submeshes=(\d+) vertices=(\d+)
        // indices=(\d+) index16=(\d)`，往它后面追加字段本身不会红，
        // 但下一个人再追加就会踩在同一个位置上各写一遍。
        std::printf("ASSETC   vfmt meshVersion=%u stride=%u\n",
                    info.meshVersion, info.vertexStride);
        std::printf("ASSETC   joints=%u clips=%u materials=%u sourceHash=%016llx\n",
                    info.jointCount, info.clipCount, info.materialCount,
                    static_cast<unsigned long long>(info.sourceHash));
        // morph 单独一行（§14）：0 也要打 —— "查过了，没有"和"忘了查"
        // 在验收脚本里是两件事。
        std::printf("ASSETC   morph targets=%u deltas=%llu channels=%u\n",
                    info.morphTargetCount,
                    static_cast<unsigned long long>(info.morphDeltaCount),
                    info.morphChannelCount);

        // 世界包围盒 + "多高"。
        //
        // 为什么要专门打这一行：**尺度是导入链最容易错、也最难靠肉眼发现的东西**
        // （差 100 倍时渲染出来仍然"像个模型"，只是人物比树还高）。围栏数字给人看，
        // 也给脚本断言用 —— 单位是**米**，引擎约定 Z 轴朝上。
        //
        // 口径与运行时一致：子网格顶点 AABB × `transform` 的 8 角 → 世界 AABB
        // （见 `MeshFormat.cpp` 读端末尾那段）。不另写一套算法，否则 CLI 报的数和
        // 引擎里实际的数会对不上。
        assetio::SourceModel m;
        if (assetio::readMeshFile(bytes.data(), bytes.size(), m, &err)) {
            const glm::vec3 sz = m.boundsMax - m.boundsMin;
            std::printf("ASSETC   bounds min=(%.3f, %.3f, %.3f) max=(%.3f, %.3f, %.3f)\n",
                        m.boundsMin.x, m.boundsMin.y, m.boundsMin.z,
                        m.boundsMax.x, m.boundsMax.y, m.boundsMax.z);
            std::printf("ASSETC   size=%.3f x %.3f x %.3f m  height(Z)=%.3f m\n",
                        sz.x, sz.y, sz.z, sz.z);

            // 片段名 + 时长：导入之后第一个要问的就是"这模型有哪些动画"，
            // 而 `clipName` 必须一字不差地写进场景 JSON（写错 = 静默不动）。
            for (const assetio::SourceClip& c : m.clips) {
                std::printf("ASSETC   clip \"%s\" %.2f s  tracks=%zu\n",
                            c.name.c_str(), static_cast<double>(c.duration),
                            c.tracks.size());
            }
            // 材质名同理（材质覆盖按名字/ID 挂在 `.vmeta` 上）。
            std::size_t shown = 0;
            for (const assetio::SourceMaterial& mt : m.materials) {
                if (shown++ >= 8) { std::printf("ASSETC   material ...\n"); break; }
                std::printf("ASSETC   material \"%s\" albedo=%s\n",
                            mt.name.c_str(),
                            mt.albedoPath.empty() ? "-" : mt.albedoPath.c_str());
            }

            // 关节：位置与父下标都打出来。
            // 为什么需要：自动绑骨的正确性靠"与厂商骨架按名字对比位置"来
            // 判定（见 tools/verify_autorig.py），没有这一行就没法建立
            // ground truth。位置打到 0.1 mm，够看，也不至于刷屏。
            //
            // 附带的蒙皮统计（`skinVerts`）能区分两种"没有权重"：
            // ① 顶点权重全 0（漏写）② 权重有但全指向 0 号关节（写错）。
            // 只看 `joints>0` 是分辨不出来的。
            std::size_t skinVerts = 0;
            for (const assetio::SourceSubMesh& sm : m.subMeshes) {
                for (const assetio::Vertex& v : sm.vertices) {
                    const float w = v.weights[0] + v.weights[1] +
                                    v.weights[2] + v.weights[3];
                    if (w > 1.0e-4f) ++skinVerts;
                }
            }
            std::printf("ASSETC   skinVerts=%zu / %zu\n", skinVerts,
                        m.vertexCount());
            // 世界位置 = 沿父链累乘 restLocal（**用整矩阵**，不是只累加
            // 平移）。Mixamo 的骨骼局部轴是 Y-up、单位 cm，靠 restLocal
            // 的旋转+缩放映射到引擎的 Z-up / 米 —— 只累加平移列会得到
            // 一堆"人体比例全错"的假数据（实测：脊柱只涨 9.9，像是
            // 差了 100 倍）。
            std::vector<glm::mat4> world(m.joints.size(), glm::mat4(1.0f));
            for (std::size_t i = 0; i < m.joints.size(); ++i) {
                const int par = m.joints[i].parent;
                world[i] = (par >= 0 && static_cast<std::size_t>(par) < i)
                               ? world[static_cast<std::size_t>(par)] *
                                     m.joints[i].restLocal
                               : m.joints[i].restLocal;
            }
            for (std::size_t i = 0; i < m.joints.size(); ++i) {
                const assetio::SourceJoint& j = m.joints[i];
                const glm::vec3 up = glm::vec3(j.restLocal[3]);
                const glm::vec3 wp = glm::vec3(world[i][3]);
                std::printf("ASSETC   joint %zu \"%s\" parent=%d local=(%.4f, "
                            "%.4f, %.4f) world=(%.4f, %.4f, %.4f)\n",
                            i, j.name.c_str(), j.parent,
                            static_cast<double>(up.x),
                            static_cast<double>(up.y),
                            static_cast<double>(up.z),
                            static_cast<double>(wp.x),
                            static_cast<double>(wp.y),
                            static_cast<double>(wp.z));
            }
        }
    }
    return 0;
}

// ---------------------------------------------------------------- 绑骨相关

// 丢掉骨架 / 动画 / 权重 → 静态网格。
// 用途：拿一个**原厂已绑好**的角色做自动绑骨的对照实验 —— 先剥掉原厂
// 绑定得到"静态网格"，再跑 --autorig，最后按关节名与厂商骨架逐点比较。
// 没有这条，验证就只剩"看起来像个人"，那是没有说服力的。
void stripSkin(assetio::SourceModel& m) {
    const std::size_t j = m.joints.size();
    const std::size_t c = m.clips.size();
    m.joints.clear();
    m.topoOrder.clear();
    m.clips.clear();
    m.invMeshNode = glm::mat4(1.0f);
    for (assetio::SourceSubMesh& sm : m.subMeshes)
        for (assetio::Vertex& v : sm.vertices) {
            v.joints[0] = v.joints[1] = v.joints[2] = v.joints[3] = 0;
            v.weights[0] = v.weights[1] = v.weights[2] = v.weights[3] = 0.0f;
        }
    std::printf("ASSETC STRIP-SKIN joints=%zu clips=%zu -> 0/0\n", j, c);
}

// 跑自动绑骨并打锚点。返回 false = 失败（调用方计一次 fail）。
//
// 锚点分两行：
//   `AUTORIG`        —— 量测 + 汇总（回归断总量）
//   `AUTORIG-JOINT`  —— 每个关节的位置（回归按**名字**与厂商骨架对比）
// 位置精度打到小数点后 4 位（0.1 mm）：差 1 cm 是"能接受的比例误差"，
// 差 0.1 m 就是"绑歪了"，两者必须在日志里就能分辨。
bool runAutoRig(assetio::SourceModel& m, const Args& a) {
    assetio::AutoRigOptions ro;
    ro.overwriteExisting = a.forceRig;
    assetio::AutoRigReport rep;
    std::string err;
    if (!assetio::autoRigHumanoid(m, ro, &rep, &err)) {
        std::printf("ASSETC AUTORIG ok=0 reason=%s\n", err.c_str());
        return false;
    }
    std::printf("ASSETC AUTORIG ok=1 height=%.3f armSpan=%.3f armAxis=%d "
                "fwdSign=%.0f halfSpan=%.3f joints=%d verts=%d rigged=%d "
                "maxInf=%.0f maxErr=%.2e\n",
                static_cast<double>(rep.height),
                static_cast<double>(rep.armSpan), rep.armAxis,
                static_cast<double>(rep.fwdSign),
                static_cast<double>(rep.halfSpan), rep.jointCount,
                rep.vertices, rep.verticesRigged,
                static_cast<double>(rep.maxInfluences),
                static_cast<double>(rep.maxWeightError));

    // 每根骨的"主影响"顶点数（weight[0] 指向它）。用来抓**死骨**：
    // 一根骨一个顶点都不主导，说明它落在空气里 —— 姿势上看不出来，
    // 但程序化动画里那条腿会完全不动。
    std::vector<int> primary(static_cast<std::size_t>(rep.jointCount), 0);
    for (const assetio::SourceSubMesh& sm : m.subMeshes) {
        for (const assetio::Vertex& v : sm.vertices) {
            if (v.weights[0] <= 0.0f) continue;
            const std::uint32_t j = v.joints[0];
            if (j < primary.size()) ++primary[j];
        }
    }
    for (std::size_t i = 0; i < rep.jointNames.size(); ++i) {
        const glm::vec3& p = rep.jointPositions[i];
        std::printf("ASSETC AUTORIG-JOINT %s (%.4f, %.4f, %.4f) verts=%d\n",
                    rep.jointNames[i].c_str(), static_cast<double>(p.x),
                    static_cast<double>(p.y), static_cast<double>(p.z),
                    primary[i]);
    }
    return true;
}

// 生成四段基础动画并打锚点。
//
// `PROCANIM-PROBE` 那一行是**这条路唯一的验收依据**：
// 一段姿势数据是否"在走"不能靠看截图（静帧永远像在走），只能靠
// "脚相对髋关节的前向位移范围"和"抬升高度"这两个数。
// 前向范围小 = 腿没摆起来；抬升小 = 脚在地上拖；方向反了则相位错，
// 所以 verify 脚本还会检查"脚最前的那一刻是不是 p≈0.25"。
bool runProcAnim(assetio::SourceModel& m, const Args& a) {
    assetio::ProcAnimOptions po;
    po.fps = a.animFps;
    assetio::ProcAnimReport rep;
    std::string err;
    if (!assetio::generateLocomotionClips(m, po, &rep, &err)) {
        std::printf("ASSETC PROCANIM ok=0 reason=%s\n", err.c_str());
        return false;
    }
    std::printf("ASSETC PROCANIM ok=1 clips=%d tracks=%d keys=%d fps=%.0f\n",
                static_cast<int>(rep.clipNames.size()), rep.totalTracks,
                rep.totalKeys, static_cast<double>(po.fps));
    for (std::size_t i = 0; i < rep.clipNames.size(); ++i) {
        std::printf("ASSETC PROCANIM-CLIP %s dur=%.2f\n",
                    rep.clipNames[i].c_str(),
                    static_cast<double>(rep.durations[i]));
    }
    std::printf("ASSETC PROCANIM-PROBE walkFootFwd=%.4f walkFootLift=%.4f "
                "runFootFwd=%.4f runFootLift=%.4f walkHandFwd=%.4f "
                "idleDrift=%.4f walkFootFwdMaxPhase=%.3f "
                "walkSwingFwdVel=%.3f walkHandLat=%.4f runHandLat=%.4f "
                "runHandFwd=%.4f\n",
                static_cast<double>(rep.walkFootForwardRange),
                static_cast<double>(rep.walkFootLift),
                static_cast<double>(rep.runFootForwardRange),
                static_cast<double>(rep.runFootLift),
                static_cast<double>(rep.walkHandForwardRange),
                static_cast<double>(rep.idleRootDrift),
                static_cast<double>(rep.walkFootFwdMaxPhase),
                static_cast<double>(rep.walkSwingFwdVel),
                static_cast<double>(rep.walkHandLateral),
                static_cast<double>(rep.runHandLateral),
                static_cast<double>(rep.runHandForwardRange));
    // 跳跃的"双脚离地"单独一行：它是 P6 那条需求的唯一硬证据
    // （静帧截图里"腿收了"和"腿没动"几乎分不出来 —— 膝盖是沿视线摆的）。
    std::printf("ASSETC PROCANIM-JUMP jumpFootLift=%.4f jumpFootPeakZ=%.4f\n",
                static_cast<double>(rep.jumpFootLift),
                static_cast<double>(rep.jumpFootPeakZ));
    return true;
}

int doCheck(const Args& a) {
    int fails = 0;    for (const std::string& in : a.inputs) {
        bool ok = false;
        const std::vector<std::uint8_t> bytes = readFile(in, ok);
        if (!ok) {
            std::printf("ASSETC FAIL %s reason=read_error\n", in.c_str());
            ++fails;
            continue;
        }
        assetio::SourceModel m;
        std::string err;
        if (!assetio::readMeshFile(bytes.data(), bytes.size(), m, &err)) {
            std::printf("ASSETC FAIL %s reason=%s\n", in.c_str(), err.c_str());
            ++fails;
            continue;
        }
        // 语义自检：索引不能越界（这是"能读出来"和"能用"之间的差别）
        bool bad = false;
        for (const auto& sm : m.subMeshes) {
            for (std::uint32_t idx : sm.indices) {
                if (idx >= sm.vertices.size()) { bad = true; break; }
            }
            if (bad) break;
        }
        if (bad) {
            std::printf("ASSETC FAIL %s reason=index_out_of_range\n", in.c_str());
            ++fails;
            continue;
        }
        std::printf("ASSETC OK %s submeshes=%zu verts=%zu tris=%zu joints=%zu clips=%zu\n",
                    in.c_str(), m.subMeshes.size(), m.vertexCount(),
                    m.triangleCount(), m.joints.size(), m.clips.size());
    }
    return fails ? 1 : 0;
}

} // namespace

int main(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string s = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                std::printf("ASSETC FAIL reason=missing_arg option=%s\n", what);
                std::exit(2);
            }
            return argv[++i];
        };
        if (s == "-h" || s == "--help") { usage(); return 0; }
        else if (s == "-o") a.output = next("-o");
        else if (s == "--asset-root") a.assetRoot = next("--asset-root");
        else if (s == "--scale") a.scale = std::stof(next("--scale"));
        else if (s == "--rot-tol") a.rotTolDeg = std::stof(next("--rot-tol"));
        else if (s == "--no-weld") a.weld = false;
        else if (s == "--no-optimize") a.optimize = false;
        else if (s == "--no-anim-reduce") a.reduceAnim = false;
        else if (s == "-r" || s == "--recursive") a.recursive = true;
        else if (s == "--info") a.info = true;
        else if (s == "--check") a.check = true;
        else if (s == "-q" || s == "--quiet") a.quiet = true;
        else if (s == "--autorig") a.autRig = true;
        else if (s == "--force-rig") a.forceRig = true;
        else if (s == "--strip-skin") a.stripSkin = true;
        else if (s == "--procanim") a.procAnim = true;
        else if (s == "--anim-fps") a.animFps = std::stof(next("--anim-fps"));
        else if (!s.empty() && s[0] == '-') {
            std::printf("ASSETC FAIL reason=unknown_option option=%s\n", s.c_str());
            return 2;
        } else a.inputs.push_back(s);
    }

    if (a.inputs.empty()) { usage(); return 2; }

    // 目录展开
    std::vector<std::string> files;
    for (const std::string& in : a.inputs) {
        std::error_code ec;
        if (!fs::is_directory(in, ec)) {
            files.push_back(in);
            continue;
        }
        const bool wantVmesh = a.info || a.check;
        auto take = [&](const fs::directory_entry& e) {
            if (!e.is_regular_file(ec) || ec) return;
            if (wantVmesh ? isVmesh(e.path()) : isFbx(e.path()))
                files.push_back(e.path().string());
        };
        if (a.recursive) {
            for (auto it = fs::recursive_directory_iterator(in, ec);
                 !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
                take(*it);
        } else {
            for (auto it = fs::directory_iterator(in, ec);
                 !ec && it != fs::directory_iterator(); it.increment(ec))
                take(*it);
        }
        if (ec) {
            std::printf("ASSETC FAIL %s reason=scan_error\n", in.c_str());
            return 1;
        }
    }

    // 排序，让批量输出稳定（回归脚本按顺序比对，文件系统顺序不可靠）
    std::sort(files.begin(), files.end());
    a.inputs = files;

    if (a.inputs.empty()) {
        std::printf("ASSETC FAIL reason=no_input\n");
        return 2;
    }

    if (a.info) return doInfo(a);
    if (a.check) return doCheck(a);

    const bool outIsDir = !a.output.empty() &&
                          (a.inputs.size() > 1 || fs::is_directory(a.output));

    int fails = 0;
    std::size_t totalIn = 0, totalOut = 0;

    for (const std::string& in : a.inputs) {
        // ---- .vmesh 输入：只有"再加工"这一种用法（绑骨 / 剥骨架）----
        // 从 FBX 直接烤 + 绑骨也行（下面那条路），但"先烤再绑"更可控：
        // 烤制会做顶点合并与缓存重排，绑骨前顶点已经是最终拓扑，权重
        // 不会因为合并而丢失（合并是**按全属性**比较的，权重也参与比较，
        // 所以合并后再算是最安全的顺序）。
        if (isVmesh(in) && (a.autRig || a.stripSkin || a.procAnim)) {
            bool ok = false;
            const std::vector<std::uint8_t> bytes = readFile(in, ok);
            if (!ok) {
                std::printf("ASSETC FAIL %s reason=read_error\n", in.c_str());
                ++fails;
                continue;
            }
            assetio::SourceModel m;
            std::string err;
            if (!assetio::readMeshFile(bytes.data(), bytes.size(), m, &err)) {
                std::printf("ASSETC FAIL %s reason=%s\n", in.c_str(),
                            err.c_str());
                ++fails;
                continue;
            }
            std::printf("ASSETC OK %s\n", in.c_str());
            if (a.stripSkin) stripSkin(m);
            if (a.autRig && !runAutoRig(m, a)) {
                ++fails;
                continue;
            }
            if (a.procAnim && !runProcAnim(m, a)) {
                ++fails;
                continue;
            }
            assetio::WriteOptions wo;
            wo.weld = a.weld;
            wo.optimizeCache = a.optimize;
            wo.reduceAnimation = a.reduceAnim;
            wo.rotationTolDeg = a.rotTolDeg;
            assetio::WriteStats st;
            const std::vector<std::uint8_t> out = assetio::writeMeshFile(m, wo, &st);
            fs::path outPath;
            if (a.output.empty())
                outPath = fs::path(in);
            else if (outIsDir)
                outPath = fs::path(a.output) / fs::path(in).filename();
            else
                outPath = a.output;
            if (!writeFile(outPath, out)) {
                std::printf("ASSETC FAIL %s reason=write_error out=%s\n",
                            in.c_str(), outPath.string().c_str());
                ++fails;
                continue;
            }
            std::printf("ASSETC   submeshes=%u joints=%u clips=%u materials=%u\n",
                        st.subMeshes, st.joints, st.clips, st.materials);
            std::printf("ASSETC   wrote %s\n", outPath.string().c_str());
            continue;
        }

        if (!isFbx(in)) {
            std::printf("ASSETC SKIP %s reason=not_fbx\n", in.c_str());
            continue;
        }
        const auto t0 = std::chrono::steady_clock::now();

        assetio::ImportOptions io;
        io.scale = a.scale;
        io.assetRoot = a.assetRoot.empty() ? guessAssetRoot(in) : a.assetRoot;
        io.verbose = !a.quiet;
        // 导入期的事件行（目前只有 clip 重名去重 `FBX-CLIPDUP`）。
        // 这一层只收文本、自己打印：`VennAssetIO` 不链 engine 的 Logger，
        // 见 `assetio/FbxImport.h` 里那段说明。
        std::vector<std::string> importNotes;
        assetio::SourceModel model = assetio::importFbx(in, io, &importNotes);
        for (const std::string& line : importNotes) std::printf("%s\n", line.c_str());

        if (!model.ok) {
            std::printf("ASSETC FAIL %s reason=%s\n", in.c_str(),
                        model.error.empty() ? "import_failed" : model.error.c_str());
            ++fails;
            continue;
        }

        if (a.stripSkin) stripSkin(model);
        if (a.autRig && !runAutoRig(model, a)) {
            ++fails;
            continue;
        }
        if (a.procAnim && !runProcAnim(model, a)) {
            ++fails;
            continue;
        }

        assetio::WriteOptions wo;
        wo.weld = a.weld;
        wo.optimizeCache = a.optimize;
        wo.reduceAnimation = a.reduceAnim;
        wo.rotationTolDeg = a.rotTolDeg;

        assetio::WriteStats st;
        const std::vector<std::uint8_t> bytes =
            assetio::writeMeshFile(model, wo, &st);

        fs::path outPath;
        if (a.output.empty()) {
            outPath = fs::path(in).parent_path() /
                      (fs::path(in).stem().string() + ".vmesh");
        } else if (outIsDir) {
            outPath = fs::path(a.output) /
                      (fs::path(in).stem().string() + ".vmesh");
        } else {
            outPath = a.output;
        }

        if (!writeFile(outPath, bytes)) {
            std::printf("ASSETC FAIL %s reason=write_error out=%s\n", in.c_str(),
                        outPath.string().c_str());
            ++fails;
            continue;
        }

        const double ms =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0).count();

        totalIn += st.rawGeometryBytes;
        totalOut += bytes.size();

        std::printf("ASSETC OK %s\n", in.c_str());
        std::printf("ASSETC   submeshes=%u joints=%u clips=%u materials=%u\n",
                    st.subMeshes, st.joints, st.clips, st.materials);
        std::printf("ASSETC   vertices %u -> %u  indices %u -> %u  acmr %.3f -> %.3f\n",
                    st.verticesIn, st.verticesOut, st.indicesIn, st.indicesOut,
                    static_cast<double>(st.acmrIn), static_cast<double>(st.acmrOut));
        std::printf("ASSETC   keys %zu -> %zu\n", st.keysIn, st.keysOut);
        if (st.morphTargets > 0) {
            std::printf("ASSETC   morph %u targets / %u channels, %zu deltas (%s)\n",
                        st.morphTargets, st.morphChannels, st.morphDeltas,
                        humanBytes(static_cast<std::uint32_t>(
                            st.morphDeltas * sizeof(assetio::SourceMorphDelta))).c_str());
        }
        std::printf("ASSETC   geometry %s -> file %s (%.2fx) in %.1f ms\n",
                    humanBytes(st.rawGeometryBytes).c_str(),
                    humanBytes(st.bytes).c_str(),
                    st.bytes > 0 ? static_cast<double>(st.rawGeometryBytes) /
                                       static_cast<double>(st.bytes)
                                 : 0.0,
                    ms);
        std::printf("ASSETC   wrote %s\n", outPath.string().c_str());
    }

    if (!a.quiet) {
        std::printf("ASSETC TOTAL files=%zu fail=%d geometry=%s output=%s\n",
                    a.inputs.size(), fails, humanBytes(totalIn).c_str(),
                    humanBytes(totalOut).c_str());
    }
    return fails ? 1 : 0;
}
