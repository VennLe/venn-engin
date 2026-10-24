#pragma once
// ============================================================
// assets/MaterialImport —— 从"材质包目录"直接导入材质仓库
//
// 用户原话（2026-10-24 第六轮）：
//   "在 Terrain 的属性面板中的 Layer materials 那里，那里的地形材质一般是
//    指定一个目录里面，将对应的材质信息按照之前 E:\code\cpp\venn-resource
//    中的材质信息的形式去自动添加，这个通过一个刷新按钮去找到最新的材质信息"
//
// 因此本模块就是 `tools/gen_material_library.py` 的**运行时版本**：同一个
// 目录约定、同一套 glTF 解析规则、同一份 `.vmat` 产物，只是不需要用户
// 退出编辑器去跑 Python。
//
// ------------------------------------------------------------
// 目录约定（与 venn-resource 完全一致）
// ------------------------------------------------------------
//   <sourceDir>/<name>/<name>_<tier>.gltf/          ← 一个"材质包"目录
//                       ├─ <name>_<tier>.gltf       ← glTF JSON
//                       ├─ <name>.bin
//                       └─ textures/<name>_diff_<tier>.jpg …
//
// 三个贴图槽**按 glTF 索引解析**（`material.pbrMetallicRoughness
// .baseColorTexture` → `textures[i].source` → `images[j].uri`），
// 不猜文件名 —— 资源换个命名后缀也不会错。
//
// ------------------------------------------------------------
// 产物（写进资产根，与 Python 脚本一致）
// ------------------------------------------------------------
//   assets/textures/materials/<name>/<name>_{diff,nor_gl,arm}.<ext>
//   assets/materials/library/<name>.vmat
//   assets/materials/library.json          ← 只在**运行时资产根**更新
//
// 为什么清单只写运行时根：`assets/materials` 是 CMake 里"每次构建都覆盖"
// 的那一档，源清单被 tools/gen_material_library.py 的 `--check` 当基准用。
// 编辑器往里塞条目会让那条用例永远 DIFF。而运行时清单被覆盖也不致命 ——
// `MaterialLibrary::scanDirectory` 会把源根里新导入的 `.vmat` 补扫回来
// （每个 .vmat 自带 `library` 块，元数据不丢）。
// ============================================================

#include <string>
#include <vector>

namespace assets {

struct MaterialImportOptions {
    std::string sourceDir;      // venn-resource 根目录（绝对路径或相对 cwd）
    std::string tier = "1k";    // 1k / 2k / 4k
    // 是否同时写"工程源资产目录"（`<repo>/assets`，见文件的产物说明）。
    // 编辑器里的刷新要 true —— 只写 build/bin/assets 的话，下次构建就被
    // CMake 的 copy_directory 抹掉一半。自动化指向临时工程时设 false。
    bool mirrorToSourceTree = true;
};

struct MaterialImportResult {
    bool ok = false;
    std::string error;          // ok=false 时的原因（给 UI 直接显示）

    int packages = 0;           // 扫到的材质包数
    int imported = 0;           // 成功写出 / 更新的材质数
    int added = 0;              // 清单里新增的条数
    std::vector<std::string> names;
    std::vector<std::string> warnings;

    std::string manifestPath;   // 写出的清单（运行时根）
    std::string summary() const;
};

// 扫描 + 导入。可重复调用（同一 name 覆盖写，清单里同名条目原地更新）。
MaterialImportResult importMaterialPackages(const MaterialImportOptions& opt);

// ------------------------------------------------------------
// "上次用过的目录"记忆（`assets/materials/library.source.json`）
//
// 与启动器的 newproject.json 同一个思路：不要每次都让用户从 C 盘开始翻。
// ------------------------------------------------------------
struct MaterialSourceSettings {
    std::string dir;
    std::string tier = "1k";
};

MaterialSourceSettings loadMaterialSourceSettings();
void saveMaterialSourceSettings(const MaterialSourceSettings& s);

// 首次使用时的默认猜测：仓库旁边的 `venn-resource`（用户明确提到过这个目录）。
// 猜不到就返回空串 —— 面板会显示输入框让用户自己填。
std::string guessMaterialSourceDir();

} // namespace assets
