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

} // namespace assets
