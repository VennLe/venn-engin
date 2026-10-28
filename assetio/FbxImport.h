#pragma once
// ============================================================
// assetio/FbxImport —— ufbx → SourceModel（**纯 CPU，不需要 Vulkan**）
//
// 这一层是离线编译器的前半段。它与 `engine/assets/FbxLoader.cpp`
// 共享同一套约定（坐标系、单位、蒙皮烘焙、pivot 归位），但产物是
// 纯数据的 IR，因此可以在**没有显卡、没有窗口**的机器上跑
// （构建机 / CI / 美术机器批量转码）。
//
// ⚠ 为什么不直接复用 FbxLoader：它的签名里有 `rhi::Device&`、
//   `AssetManager&`、`TextureContext&` —— 三个都要求"Vulkan 活着"。
//   想在没有设备的进程里调它，唯一办法是伪造一个 Device，那比把
//   纯数学部分搬出来危险得多。
// ============================================================

#include "assetio/SourceModel.h"

#include <string>
#include <vector>

namespace assetio {

struct ImportOptions {
    // 尺寸倍数（`<model>.vmeta` 的 `import.scale`，1.0 = 原尺寸）。
    // ⚠ 与 ufbx 的 `target_unit_meters` 是**倒数**关系，见 .cpp 里的注释。
    float scale = 1.0f;
    bool importAnimations = true;
    bool importMaterials = true;
    // 资产根（绝对路径）。用来把贴图路径规范化成"相对资产根"。
    // 空 = 只做文件名回退，不做相对化。
    std::string assetRoot;
    bool verbose = false;
};

// 导入一个 FBX。失败时返回 `ok=false` 并用 `error` 说明原因
// （**不抛异常**：批量转码时一个坏文件不该中断整批）。
//
// `notes`（可选，追加写入）：导入过程中"值得打一行日志"的事件。目前只有
// clip 重名去重（`FBX-CLIPDUP`，见 `assetio/ClipDedup.h`）。
// ⚠ 为什么**不在这一层直接打**：`VennAssetIO` 只链 glm + ufbx，刻意不带
//   engine 的 Logger（要么把整个 engine 依赖拖进来，要么 AssetC 就得开
//   Vulkan）；而 AssetC 侧只有 `std::printf`。所以这一层只产出**文本**，
//   打印由调用方负责 —— 文字内容由 `clipDedupLine()` 统一生成，
//   两个调用点打印方式不同、内容不会漂。
SourceModel importFbx(const std::string& absPath, const ImportOptions& opt,
                      std::vector<std::string>* notes = nullptr);

// 把一个绝对/相对文件路径规范成"相对 assetRoot"的、分隔符为 '/' 的
// 形式。不在 assetRoot 下（或无法求相对路径）时返回空串。
// 公开出来是因为 `.vmeta` 侧也要用同一套规则，两边不一致会导致
// "贴图在编辑器里能找到、打包后找不到"。
std::string relativizeTo(const std::string& absPath, const std::string& assetRoot);

} // namespace assetio
