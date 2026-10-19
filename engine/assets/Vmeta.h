#pragma once
// ============================================================
// assets/Vmeta —— 导入设置 sidecar（W2-2，见 docs/world_scale_roadmap.md §7）
//
// 每个导入资产旁边放一份 `<模型>.vmeta`，记三件事：
//
//   1. **sourceHash**：源文件内容的 FNV-1a。源变了才"重新导入"。
//      ⚠ 本引擎没有 cooked 缓存（每次启动都从源解析），所以这里的"重导"
//        不是"少解析一次"，而是**要不要重建材质 id 映射** —— 见 2。
//   2. **materials**：`glTF 材质名 → 材质 id`。这是 F2 的落地：材质身份
//      记在 sidecar 里，重导出时按**名字**重新绑定，而不是按下标。
//      ⚠ 按名字（而不是 `#mat3` 这种下标）才能做到"重导出换了顺序，
//        场景里的材质覆盖还挂在原来那个材质上"。
//   3. **import**：导入参数。用户改了这些参数 → 强制重导（不动材质 id 之外的
//      任何东西；场景里的材质覆盖属于场景，一律不碰）。
//
// 三种"变了"分得很清楚，混在一起就会丢数据：
//   | 情况                     | 行为                                        |
//   |--------------------------|---------------------------------------------|
//   | 源没变、参数没改          | 什么都不做（映射照用）                       |
//   | 源变了                   | 按名字重建映射；名字没了的打 REMAP-FAIL      |
//   | 用户改了 import 参数      | 强制重导（材质 id 映射按名字保留）           |
//   | 场景里的材质覆盖          | 不碰（那是场景的事，不是导入的事）           |
//
// ⚠ 判定"用户改了 import 参数"靠 `appliedImportSig`：文件里记着**上次真正
//   应用过**的那组参数的签名，读的时候重新算一遍对不上就是被改过。
//   没有这个字段就永远判不出来（文件是用户手改的，没有别的参照物）。
// ============================================================

#include <array>
#include <map>
#include <set>
#include <string>

namespace assets {

struct VmetaImport {
    float scale = 1.0f;
    bool generateLods = true;
    std::array<int, 3> lodGridRes{{24, 12, 6}};
    bool importMaterials = true;
    bool importAnimations = true;
    // "name"：`模型路径#glTF材质名`（默认，重导出安全）
    // "index"：`模型路径#mat<下标>`（老行为，重导出会错位，只给对照用）
    std::string materialIdScheme = "name";
};

struct Vmeta {
    int version = 1;
    std::string source;            // 相对资产根的源文件路径
    std::string sourceHash;        // "fnv1a:0x...."
    std::string appliedImportSig;  // 上次**应用过**的 import 参数签名
    VmetaImport import;
    std::map<std::string, std::string> materials;  // glTF 名 -> 材质 id

    // ---- 本次导入的判定结果（不写进文件）----
    bool existed = false;   // 读到现成的 sidecar
    bool changed = false;   // 源文件的 hash 变了（要重建映射）
    bool forced = false;    // import 参数被用户改过（强制重导）
    std::map<std::string, std::string> prevMaterials;  // 读进来的原始映射
    // 本次导入**真的见到**的源材质名。
    // ⚠ 不能拿 materials 当"见过的"：它一开头就是文件的副本，
    //   于是"文件里有、源里没了"的名字永远判不出来（REMAP-FAIL 变哑巴）。
    std::set<std::string> seen;
};

// FNV-1a 32 位（与 persistence_roadmap 的增量存档同一套，不引新哈希）
std::uint32_t fnv1a32(const void* data, std::size_t size, std::uint32_t h = 2166136261u);
std::string fnv1aFile(const std::string& absPath);          // "fnv1a:0x........"
std::string importSignature(const VmetaImport& imp);        // 同上格式

// `models/girl.glb` → `models/girl.vmeta`（换扩展名，不带点结尾）
std::string vmetaPathFor(const std::string& modelPath);

bool loadVmeta(const std::string& absVmetaPath, Vmeta& out);
bool saveVmeta(const std::string& absVmetaPath, const Vmeta& v);

// 开始一次导入：读（或初始化）sidecar、算 hash、判定 changed / forced。
// modelRelPath 是**相对资产根**的路径（就是 AssetManager 里当 key 用的那个）。
Vmeta beginModelImport(const std::string& modelRelPath);

// 结束一次导入：把本次见到的材质名补进映射，按需写回文件，并打 ASSET-VMETA。
// 返回 true = 文件真的被写了。
bool finishModelImport(const std::string& modelRelPath, Vmeta& meta);

// 决定某个源材质名的稳定 id（同时把它记进 meta.materials）。
//   * 映射里已有 → 原样返回（这就是"重导出换序不串味"）
//   * 没有 → 按 scheme 生成，并写进映射
std::string resolveMaterialId(Vmeta& meta, const std::string& base,
                              const std::string& matName, int matIdx);

} // namespace assets
