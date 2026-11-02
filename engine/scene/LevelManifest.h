#pragma once
// ============================================================
// scene/LevelManifest —— 关卡清单（G19 关卡流送的第一半）
//
// 2026-11-02 之前：`grep -rn "LevelStreaming\|LevelManifest"` 全仓 **0 命中**。
// 编辑器只认"当前打开的那一个场景文件"，游戏只能一次加载整个世界。
// 后果不是"慢"，是**做不了**：
//   · 一张 4km 的大世界 = 几千个实体全在内存里、全进 BVH、全参与剔除；
//   · 没有"章节列表"，第二章要重开一个进程；
//   · 也没有"这一片区域的资源在玩家走近之前不加载"。
//
// 清单回答"这局游戏由哪些关卡组成"：
//   · `alwaysLoaded` = 常驻（主城 / UI 场景 / 常开的室内）；
//   · 有体积 = 走进包围盒就加载（按需流送）；
//   · 没有体积、也**不是**常驻 = 只能手动加载（调试用 / 一次性的过场）。
//
// ⚠ 清单是**独立资产**（`assets/levels/*.vlevels`），不塞进场景文件：
//   它是"整个世界"的视图，而场景文件是"世界的一部分"。塞进场景的话，
//   加载 A 场景才知道 B 场景的存在 —— 而 A 恰恰是还没加载的那一半。
// ============================================================

#include <glm/vec3.hpp>

#include <string>
#include <vector>

namespace scene {

// 一张关卡。
struct LevelEntry {
    std::string name;            // 清单内**唯一**的 id（命令与面板都用它）
    std::string path;            // **相对资产根**的场景文件（`resolveAssetPath` 认得）
    bool alwaysLoaded = false;   // 常驻：`update()` 会保证它一直加载着
    bool enabled = true;         // 关掉 = 流送不看它（手动 load 仍然可以）
    // 触发盒（世界坐标，米；引擎右手系 Z-up）。
    // `halfExtent` 全零 = 没有触发盒 → 不做距离判定（常驻 / 手动）。
    glm::vec3 center = glm::vec3(0.0f);
    glm::vec3 halfExtent = glm::vec3(0.0f);
};

struct LevelManifest {
    std::string name = "levels";
    std::vector<LevelEntry> levels;

    const LevelEntry* find(const std::string& n) const;
    LevelEntry* find(const std::string& n);
};

struct LevelManifestIo {
    bool ok = false;
    std::string error;
    int levels = 0;
};

// 从 JSON 文本解析。格式：
//   { "name": "world",
//     "levels": [ { "name": "hub", "path": "scenes/levels/hub.json",
//                   "alwaysLoaded": true },
//                 { "name": "forest", "path": "...",
//                   "center": [0,0,0], "halfExtent": [64,64,32] } ] }
//
// ⚠ 校验是**严格**的：`name` 空、`path` 空、重名 —— 一律整份失败。
//   "收下一半"的清单会在两个关卡重名时静默只加载其中一个，而现场看起来
//   像"这张关卡的数据丢了"。
LevelManifestIo parseLevelManifest(const std::string& jsonText, LevelManifest& out);

LevelManifestIo loadLevelManifest(const std::string& path, LevelManifest& out);

// 写盘（编辑器"Save manifest"用）。失败返回 false 并填 `err`。
bool saveLevelManifest(const LevelManifest& m, const std::string& path,
                       std::string* err = nullptr);

// 序列化成 JSON 文本（与 `parseLevelManifest` 往返一致）。
std::string levelManifestToJson(const LevelManifest& m);

// ============================================================
// 判定（纯函数，便于自检与判据）
// ============================================================

// `viewer` 到触发盒的距离（米）。盒内 = 0，无盒 = 无穷大。
// ⚠ 用"到盒子的距离"而不是"到中心的距离"：一张 128m 宽的关卡用中心距算，
//   玩家站在盒子里最边上时距离是 64 —— 按半径判定就会把它误判成"太远"。
float levelDistanceTo(const LevelEntry& e, const glm::vec3& viewer);

// 是否该加载：有盒且 viewer 在盒内（逐轴，含边界）。
bool levelWantsLoad(const LevelEntry& e, const glm::vec3& viewer);

// 是否该卸载：viewer 已经出了**放大后**的盒（`unloadScale` 倍半尺寸之外）。
//
// ⚠ 加载与卸载不能用同一个盒：玩家在边界上左右晃时，同一帧里
//   "加载 → 卸载 → 加载"，每次都重新解析一遍场景文件。
//   表现是"走到某条线上游戏就卡"，而日志里是一串 SCENE-STREAM。
bool levelWantsUnload(const LevelEntry& e, const glm::vec3& viewer,
                      float unloadScale);

// 自检（接进 `--selftest project`）。输出格式与其他自检一致：
//   LEVEL-TEST PASS|FAIL <名字>   /   LEVEL-TEST total pass= fail=
int runLevelManifestSelfTest();

} // namespace scene
