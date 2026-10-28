#pragma once
// ============================================================
// assets/AssetSource —— 资产来源：磁盘目录 or `assets.pak`
//
// ------------------------------------------------------------
// 包里的路径 = **相对 dist 根**
// ------------------------------------------------------------
// 一个 `game.pak` 同时装两个根：
//     assets/…    （工程资产 + 引擎内置资产）
//     shaders/…   （263 个 .spv）
// 所以查询/落盘的 key 一律带根前缀（`"assets/textures/a.png"`、
// `"shaders/pbr.vert.spv"`）。这样只有一个包、一个挂载点，
// "还有哪些目录能进包"变成加前缀，而不是加一条挂载链。
//
// ------------------------------------------------------------
// 一个函数就够了，为什么要单独一层
// ------------------------------------------------------------
// 运行时要读的资产有三十来处读取点，写法各不相同：
//   · `std::ifstream(resolveAssetPath(rel))`
//   · `stbi_load(resolveAssetPath(rel).c_str(), …)`
//   · `ufbx_load_file(path)` / `tinyobj::LoadObj(path)`
//   · `std::ifstream(absPath)`（地形自己拼的绝对路径）
//
// 前三种都经过 `resolveAssetPath`，第四种在 `resolveAssetPath` 的下游
// （它拿到的是"资产根 + 相对路径"，根来自同一套候选）。
//
// 所以**拦截点只有一个**：`resolveAssetPath` 找不到磁盘文件时，若挂了包，
// 就把那个条目**落到内容寻址缓存**里并返回缓存路径。这样：
//   · 三十个读取点**一行都不用改**（也就不会改错）；
//   · `stbi_load` / ufbx 这些"只吃路径"的第三方库照样能用；
//   · 缓存文件名就是内容的 FNV 哈希 → 永远不会读到旧版本；
//   · 第二次启动全是命中，不重复解压。
//
// ------------------------------------------------------------
// 代价（必须说清楚，别假装没有）
// ------------------------------------------------------------
// 这是"**按需解包**"，不是"零解包直读"：首次访问某个资产会在缓存目录里
// 落一份字节。收益仍然是实的 ——
//   · 分发是**一个文件**（300+ → 3），拷贝/上传/杀软扫描都快；
//   · 完整性可校验（包的索引里有每条内容的哈希，读了会核对）；
//   · 玩家不会看到"少了一个 png 所以黑屏"（少文件在打开包时就报错）。
// 真正的零解包直读（内存映射 + 每次从包里解到内存）要改那三十处调用点，
// 那些改动各自都有回归风险；等有性能诉求时再分格式迁移（贴图/场景优先）。
// ============================================================

#include <string>

namespace assets {

// 挂载指定 pak。已挂载时先卸载。失败返回 false 并写 err（"不是 pak" 等）。
bool mountAssetPak(const std::string& absPakPath, std::string& err);

// 挂载 `<exe 所在目录>/game.pak`（存在才挂）。返回是否**挂了**。
// 找不到文件不算错误（开发态就是散文件目录）。
bool mountAssetPakFromExeDir();

// 幂等版：只尝试一次，之后不再探测（resolveAssetPath 的热路径用它）。
// 返回当前是否挂着包。
bool ensureAssetPakMounted();

void unmountAssetPak();
bool assetPakMounted();
const std::string& assetPakPath();

// 包里有没有这一条（**相对 dist 根**，带 `assets/` 或 `shaders/` 前缀）。
// 未挂载恒 false。
bool assetPakHas(const std::string& entry);

// 直接从包读原始字节（不落盘）。失败返回 false 并写 err。
bool readFromAssetPak(const std::string& entry, std::string& out,
                      std::string& err);

// 把包里的条目落到内容寻址缓存，返回**绝对路径**（正斜杠）。
// 失败返回空串（调用方回退成"文件不存在"，与今天的表现一致）。
std::string materializePakEntry(const std::string& entry);

// 反向查：`abs` 是不是我们落出来的缓存文件？是则写回它的相对路径。
// 给 `makeAssetRelative` 用 —— 不然"从包里落出来的贴图"被写回场景时，
// 存进去的会是一串 `C:/Users/.../assetcache/<hash>/a.png`（换台机器就打不开）。
bool relativeForMaterialized(const std::string& abs, std::string& rel);

// 诊断用：包里的条目数 / 原始字节 / 压缩后字节（未挂载全 0）。
int assetPakEntryCount();
unsigned long long assetPakRawBytes();
unsigned long long assetPakPackedBytes();

// 缓存根目录（绝对路径）。给日志/测试用；不存在时返回空串。
const std::string& assetCacheRoot();

// 清空缓存目录（测试用；运行时不调用）。返回删掉的字节数近似值。
long long clearAssetCache();

} // namespace assets
