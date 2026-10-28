#pragma once
// ============================================================
// project/AssetScan —— 资产引用闭包（G29-a）
//
// 出包第一版（E28-1）是"整棵 assets 拷过去"：**宁可多带，不可少带**。
// 这一层把它换成"只带被引用到的"，代价是必须证明**没有漏**。所以本文件的
// 设计目标不是"扫得准"，而是**漏了要叫**：
//
//   · 扫到的 → 进闭包（会进包）
//   · 看起来像资产路径、但找不到 → **unresolved**，调用方必须**打包失败**
//   · 完全不认识的字符串（实体名 / 剧情文本 / 日志格式串）→ 安静跳过
//
// ------------------------------------------------------------
// 为什么用"字符串 + 索引探测"而不是"按字段名解析"
// ------------------------------------------------------------
// 场景 JSON 里装资产路径的字段有几十个（`script.path` / `mesh.path` /
// `mesh.modelKey` / `terrain.source` / `terrain.layers[]` / `material` 的
// 十几种贴图槽 / 动画的 `skeleton.path` / `clipName` / 粒子贴图 / 贴花…），
// 而且**每加一个特性就多一个**。按字段名写的扫描器一定会落后于格式：
// 漏掉的那一个不会报错，只会在玩家机器上黑屏 —— 正是 G29 要防的事。
//
// 这里反过来做：**任何 JSON 字符串值都是候选**，用一份**资产索引**去判定
// 它是不是真文件。加新字段不需要改扫描器（新字段的值天然会被看到）。
//
// 索引长这样（建一次，之后全是哈希查表，不碰磁盘）：
//   exact[rel]            = rel              // "textures/a.png"
//   noExt[去掉扩展名的rel] = [rel...]         // "materials/ground" → ground.vmat
//   noExt[去掉所有后缀]    = [rel...]         // "sequences/intro" → intro.seq.json
// 于是三种写法都能命中：
//   · 带全名的路径        `terrain/ground/terrain.vterrain`
//   · 材质 id（无扩展名）  `materials/template_ground`  ← 地形分层图就是这么写的
//   · 剧本/过场的纯 id     `playSequence("intro")`      ← 只有 basename
//
// ------------------------------------------------------------
// 怎么判"这串东西是个引用"
// ------------------------------------------------------------
// 只有两种字符串会被当成引用（其余的静默跳过）：
//   ① 以**已知资产扩展名**结尾（`.json/.vmat/.vterrain/.png/.glb/...`）
//   ② 以**已知资产目录**开头（`materials/`、`textures/`、`sequences/`…）
// 这两条把"散文"挡在外面（`"generator": "gen_templates.py (third_person)"`
// 不含斜杠也不以资产扩展名结尾），同时把"材质 id 少写一个文件"抓出来。
//
// ⚠ 判据只在你**写了资产名**时才生效，所以"未解析"是真的坏了，不是猜错。
//
// ------------------------------------------------------------
// 三种"扫描方式"（按扩展名分派）
// ------------------------------------------------------------
//   · JSON 系（`.json` / `.vmat` / `.vterrain`）：解析成树，递归取所有字符串。
//     ⚠ 解析失败**不算致命**（记 warning，当叶子处理）：一个坏 JSON 不该让
//       出包停下 —— 但也不能假装扫过了，所以 warning 里会点名。
//   · `.vks` 脚本：抽**引号里的字符串**。脚本能按 id 引用资产
//     （`playSequence("intro")` / `showDialogue("ch01")` / `loadScene(...)`），
//     而这些 id 不带扩展名 —— 正是 §上面 ② 那条规则要救的。
//   · `.gltf`：也是 JSON，但**里面的相对路径是相对它自己**（glTF 规范），
//     所以它的 base 是所在目录而不是 assets 根。`.glb` 是二进制，当叶子。
//   · 其它（png/dds/r32/rgba8/vmesh/fbx/ogg/ttf…）：叶子，不解析。
//
// ⚠ 模型会顺带带上**同名的 `.vmesh` / `.vmeta` 缓存**（同一个 stem）：
//   那是导入缓存，少一个就要在玩家机器上重烤（慢一个量级），而它不会被
//   任何 JSON 直接引用（路径规则见 `assets/Mesh.cpp`）。
// ============================================================

#include <string>
#include <vector>

namespace project {

struct ScanResult {
    bool ok = true;
    // 闭包里的文件（相对 assets 根、正斜杠、按字典序）。调用方直接拿去拷。
    std::vector<std::string> files;
    // "谁引用的 → 引用了什么"。**非空 = 必须打包失败**（G29 验收第 2 条：
    // 故意漏一个材质引用 → 打包报错，不是运行时黑屏）。
    std::vector<std::string> unresolved;
    // 不致命但要看得见：坏 JSON / 没见过的扩展名 / 绝对路径引用。
    std::vector<std::string> warnings;
    int scanned = 0;        // 真正解析过的文件数（json / vks / gltf）
    int indexSize = 0;      // 索引里的文件总数（用来算"剔掉了多少"）
};

struct ScanOptions {
    // 无论有没有人引用，都整目录带上（相对 assets 根）。
    // 现在只有一类东西需要：**运行时按固定名字打开**的目录 —— `ui/`
    // （HUD 布局 / SDF 字体）与 `fonts/`。它们没有"引用者"，靠闭包永远
    // 扫不到，而少了就是字体断言框（见 docs/package_roadmap.md §4）。
    std::vector<std::string> alwaysInclude;
};

// assetsRootAbs：工程的 `assets/` 绝对路径（正斜杠）。
// roots：入口文件，**相对 assets 根**（`game.json` 的 `scene`、工程声明的
//        起始场景……）。入口自己不存在 → unresolved 也会点名。
ScanResult scanAssetClosure(const std::string& assetsRootAbs,
                            const std::vector<std::string>& roots,
                            const ScanOptions& opt = {});

// ---------------------------------------------------------------- 自检
// `--selftest scan`：纯文件 IO + 一点点 JSON 解析，不开窗口也不碰 GPU。
int runScanSelfTest();

} // namespace project
