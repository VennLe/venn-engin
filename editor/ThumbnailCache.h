#pragma once
// ============================================================
// editor/ThumbnailCache —— 资产缩略图的共享缓存
//
// ContentBrowser（网格里的图片预览）和 InspectorPanel（材质纹理槽）
// 都要在 ImGui 里画一张纹理缩略图，走的是同一条路：
//
//   AssetManager::loadTexture()  →  **同一张** GPU 纹理
//     →  ImGui_ImplVulkan_AddTexture()  →  ImDrawList::AddImage()
//
// 复用引擎自己的资源缓存是有意为之：编辑器里预览过的图，拖进场景后
// 不会二次上传。
//
// 这条路的生命周期很容易写错 —— ImGui 的纹理句柄必须在 ImGui 后端
// shutdown **之前**注销（Removing a descriptor set after the backend
// is gone = 释放后使用）。所以收敛到一个类里，析构时统一 RemoveTexture。
// （EditorApp 的成员先于 Application 里的 Renderer 释放，这个顺序天然成立。）
//
// 取不到图的行会记进 m_failed 并**不再重试** —— 否则一个坏文件会让
// 日志每帧刷一行 WARN。
// ============================================================

#include <cstdint>
#include <string>
#include <unordered_map>

#include <vulkan/vulkan.h>

namespace assets {
class AssetManager;
class Texture;
} // namespace assets

namespace editor {

class ThumbnailCache {
public:
    struct Entry {
        VkDescriptorSet ds = VK_NULL_HANDLE;
        float w = 1.0f;  // 原图尺寸（画等比例裁剪时要用）
        float h = 1.0f;
    };

    ThumbnailCache() = default;
    ~ThumbnailCache();

    ThumbnailCache(const ThumbnailCache&) = delete;
    ThumbnailCache& operator=(const ThumbnailCache&) = delete;

    // 按**文件路径**取缩略图：内部走上层的 AssetManager 缓存。
    // key 建议用绝对路径 —— AssetManager 只按 key 缓存，同名不同目录的
    // 文件用相对路径当 key 会互相覆盖。
    const Entry* get(assets::AssetManager& am, const std::string& filePath);

    // 按**已经存在的纹理**取缩略图（程序化 checker / solid 没有文件路径，
    // 场景里绑好的材质槽属于这种）。key 由调用方保证唯一且稳定。
    const Entry* getBound(const std::string& key, assets::Texture* tex);

    // 文件被改/删后要失效，否则网格里会一直显示旧图
    void erase(const std::string& key);
    void eraseTree(const std::string& prefix);
    void clear();

private:
    std::unordered_map<std::string, Entry> m_items;
    std::unordered_map<std::string, bool> m_failed;
};

} // namespace editor
