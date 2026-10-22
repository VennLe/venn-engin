#pragma once
// ============================================================
// assets/MeshLoader —— 模型加载
//   * loadOBJ  ：tinyobjloader，自动补法线
//   * loadGLTF ：tinygltf，支持 .gltf / .glb（含材质与内嵌/外部贴图）
//
// glTF 的加载会**登记** Mesh/Material/Texture 到 AssetManager，
// 以便统一按名字复用与释放；调用方只需按 SubMesh 建实体。
// ============================================================

#include "assets/Mesh.h"
#include "assets/Model.h"
#include "assets/Texture.h"

#include <memory>
#include <string>

namespace rhi {
class Device;
class CommandPool;
}

namespace assets {

class AssetManager;

// 从 .obj 文件加载网格（mtl 材质暂不解析，统一使用程序材质）
std::unique_ptr<Mesh> loadOBJ(rhi::Device& device, rhi::CommandPool& cmdPool,
                              const std::string& path);

// 从 .gltf / .glb 加载完整模型（网格 + PBR 材质 + 贴图）
// name 作为资源名前缀，便于在 AssetManager 里区分多个模型
// （名字带 Model 后缀，避免与 AssetManager::loadGLTF 成员同名互相遮蔽）
std::unique_ptr<Model> loadGLTFModel(rhi::Device& device,
                                     rhi::CommandPool& cmdPool,
                                     const TextureContext& ctx,
                                     AssetManager& assets,
                                     const std::string& path,
                                     const std::string& name);

// 从 .fbx 加载完整模型（静态网格 / 骨骼网格 + 蒙皮 + 动画）。
// 实现见 FbxLoader.cpp（ufbx 单头库）。与 loadGLTFModel 同一套产物：
// 返回 assets::Model —— 无骨骼是静态网格体（skeleton 为空），有骨骼
// 则带上 Skeleton（joints + clips），渲染走蒙皮路径。
std::unique_ptr<Model> loadFBXModel(rhi::Device& device,
                                    rhi::CommandPool& cmdPool,
                                    const TextureContext& ctx,
                                    AssetManager& assets,
                                    const std::string& path,
                                    const std::string& name);

// ---- 自研二进制模型（`.vmesh`，见 assetio/MeshFormat.h）----
//
// 与 `loadFBXModel` 产出同一个 `assets::Model`，区别只在于**数据从哪来**：
// 前者现场解析 FBX，后者直接 memcpy 烤好的字节。
//
// `vmeshAbsPath`  —— 实际要读的文件
// `name`          —— 模型键（AssetManager 缓存 + 材质 id 的基名）
// `sourceRelPath` —— 写进 `MeshSource::path` 的**源**路径（.fbx），
//                    用于场景存盘往返；不是 .vmesh 路径。
std::unique_ptr<Model> loadVMeshModel(rhi::Device& device,
                                      rhi::CommandPool& cmdPool,
                                      const TextureContext& ctx,
                                      AssetManager& assets,
                                      const std::string& vmeshAbsPath,
                                      const std::string& name,
                                      const std::string& sourceRelPath);

// `.vmesh` 是否存在且不比源文件旧（源文件改了就该回去重烤）。
// 这条判据是"改了 FBX 但画面没变"这类问题的唯一出口，别删。
bool vmeshIsFresh(const std::string& sourcePath, const std::string& vmeshPath);

} // namespace assets
