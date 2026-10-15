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
//
// E13 追加：LRU 上限。浏览大目录时，缓存会按"看过的每一张图"线性增长，
// 而每张都占一个 ImGui 描述符 + 一份 GPU 纹理引用。这里给缓存设一个条目
// 上限，超出后按**最近使用**淘汰最久未用的那张（淘汰时立刻注销句柄，
// 与 erase 同一条路径）。
//
// 关于"后台线程解码"：真正的异步解码需要把"读文件 + 解码像素"从
// AssetManager::loadTexture（它把解码与 GPU 上传绑在一次调用里）里拆出来，
// 中间加一个"待上传队列"，由渲染线程每帧取走上传 —— Vulkan 的纹理创建
// 不能在工作线程调用。这是独立的一块工作（记账在 editor_roadmap E13），
// 本缓存先把 LRU 与内存上限做掉。
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

    // E13 缓存统计（供日志/验收断言）
    std::size_t size() const { return m_items.size(); }
    std::size_t capacity() const { return m_cap; }
    void setCapacity(std::size_t cap) { m_cap = cap ? cap : 1; }

private:
    // LRU 淘汰最久未使用的一项（不淘汰 keep 指定项）
    void evictIfNeeded(const std::string& keep);

    struct Slot {
        Entry entry;
        std::uint64_t lastUse = 0;
    };

    std::unordered_map<std::string, Slot> m_items;
    std::unordered_map<std::string, bool> m_failed;
    std::uint64_t m_tick = 0;
    std::size_t m_cap = 256;   // 上限：大目录滚动时的内存/描述符兜底
};

} // namespace editor
