#pragma once
// ============================================================
// assets/SkeletonAsset —— 独立骨架资产 `.vskel` 的运行时装载
//
// 与 `VMeshLoader` 的分工：
//   · `VMeshLoader`  = 整个模型（子网格 / 材质 / 动画）→ `assets::Model`
//   · 这个文件       = 一具骨架 → `assets::Skeleton`
//
// 为什么骨架要单独一个入口（2026-11-06）：UE 里 `SK_Mannequin` 是被
// `SKM_Manny` / `SKM_Quinn` / 几百条 `AnimSequence` **共同引用**的资产。
// 我们的 `Model::skeleton` 是 `shared_ptr`，共享在数据层早就成立；缺的是
// "这份骨架在磁盘上有一个独立身份"。有了 `.vskel` 之后：
//   · 骨名/层级/挂点改一处就够（不用重烤每个 mesh）；
//   · `AssetC --info *.vskel` 能不看模型就查骨表；
//   · 加载期能拿 `boneDigest` 校验"这个 mesh 和这具骨架是不是同一套"。
//
// ⚠ 格式细节在 `assetio/SkelFormat.h`（这一层只做搬运 + 缓存）。
// ============================================================

#include "assets/Skeleton.h"

#include <cstddef>
#include <string>

namespace assets {

// 从 `.vskel` 装一份运行时骨架。**纯 CPU**：不碰 VkDevice，可以在
// Vulkan 之前调用（自检与离线体检都靠这一条）。
//
// 只填 `joints` / `topoOrder` / `invMeshNode` / `skeletonToEngine` /
// `sockets` / `boneDigest`；`clips` 保持为空 —— 动画这一刀仍在 `.vmesh`
// 里（`docs/mannequin_skeleton.md` §6 记着为什么分开做）。
//
// 挂点的 `bone` 下标在这里就解析好（`resolveSockets`）。解析不到的挂点
// 保留在表里但 `bone == -1` —— 不删，因为"挂点定义存在但骨不在"和
// "压根没定义这个挂点"是两件事，前者说明骨表对不上。
bool loadSkeletonAsset(const std::string& absPath, Skeleton& out,
                       std::string* err = nullptr);

// ------------------------------------------------------------ 缓存
//
// 同一份 `.vskel` 在一次进程里只解析一次。为什么值得缓存：`.vskel` 里
// 每根骨 200 字节（3 个 mat4），128 根就是 25 KB 的解析量；一个场景里
// 五个角色引用同一具骨架时，缓存省的不只是 IO。
//
// 失效口径 = **文件 mtime 变了**（与 `vmeshIsFresh` 同一套判据）。
// 不做"内容哈希"比较：这是开发期热更新用的，不是发布期的问题。
std::size_t skeletonAssetCacheSize();
void clearSkeletonAssetCache();

} // namespace assets
