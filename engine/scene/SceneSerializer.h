#pragma once
// ============================================================
// scene/SceneSerializer —— 场景 <-> JSON
//
// 只序列化"场景内容"（ECS 实体 / 层级 / 相机 / 方向光），
// **不**序列化 GPU 资源本身：
//   * 网格 / 材质只写"来源描述"（MeshSource / MaterialSource /
//     TextureSource）
//   * 加载时由 AssetManager 按描述重建：
//       程序化形状 → 现场生成
//       文件模型   → 按路径重新加载
//       glTF       → 重新解析整个模型（网格+材质+贴图一起回来）
//
// 好处：JSON 是人可读、可手改、可进版本库的；
//       二进制资源（.glb/.png）仍留在 assets 目录里，不必内嵌 base64。
// ============================================================

#include <string>

namespace assets {
class AssetManager;
}
namespace scene {
class Scene;
}

namespace scene {

// 序列化结果（同时充当错误上报通道，避免异常打断主循环）
struct SceneIoResult {
    bool ok = false;
    std::string error;
    int entities = 0;  // 成功处理（保存 / 恢复）的实体数
    int skipped = 0;   // 缺少来源信息等被跳过的数量
};

// 把场景写入 JSON 文件
SceneIoResult saveScene(const Scene& scene, const std::string& path);

// 从 JSON 文件恢复场景（会先清空当前世界；AssetManager 缓存保留）
SceneIoResult loadScene(Scene& scene, assets::AssetManager& assets,
                        const std::string& path);

// ============================================================
// 内存态往返（编辑器用）
//
// 为什么不共用文件版：
//   EditorScene → RuntimeScene 的复制、以及"快照式撤销"，都在同一帧内
//   频繁发生，走磁盘既慢又会在工程目录里留下垃圾文件。
//
// 序列化格式与文件版**完全一致** —— 同一份 JSON，
// 既能 dump 到内存，也能落盘成场景文件，两条路可以互相验证。
// ============================================================

// 把场景序列化成 JSON 文本
SceneIoResult sceneToJson(const Scene& scene, std::string& outJson);

// 从 JSON 文本恢复场景（同样会先清空当前世界）
SceneIoResult sceneFromJson(Scene& scene, assets::AssetManager& assets,
                            const std::string& jsonText);

} // namespace scene
