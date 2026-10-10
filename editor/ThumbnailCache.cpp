#include "ThumbnailCache.h"

#include "assets/AssetManager.h"
#include "assets/Texture.h"
#include "core/Logger.h"

#include <backends/imgui_impl_vulkan.h>

#include <exception>
#include <utility>

namespace editor {

ThumbnailCache::~ThumbnailCache() {
    // 必须还在 ImGui 后端存活期内（EditorApp 的成员先于 Renderer 释放）
    clear();
}

const ThumbnailCache::Entry* ThumbnailCache::get(assets::AssetManager& am,
                                                 const std::string& filePath) {
    if (filePath.empty()) return nullptr;

    auto it = m_items.find(filePath);
    if (it != m_items.end()) return &it->second;
    if (m_failed.count(filePath)) return nullptr;

    assets::Texture* tex = nullptr;
    try {
        // 缩略图一律按 sRGB 取：它只是给人看的预览图，颜色要对。
        // 真正绑到材质槽时用的是**另一个**缓存键（见 InspectorPanel 里的
        // slotCacheKey），否则法线/ORM 贴图会被这张 sRGB 版本顶掉。
        tex = am.loadTexture(filePath, filePath, true);
    } catch (const std::exception& ex) {
        VK_LOG_WARN("ThumbnailCache: load failed (%s): %s", filePath.c_str(),
                    ex.what());
    }

    const Entry* e = getBound(filePath, tex);
    if (!e) m_failed[filePath] = true;
    return e;
}

const ThumbnailCache::Entry* ThumbnailCache::getBound(const std::string& key,
                                                      assets::Texture* tex) {
    if (key.empty()) return nullptr;

    auto it = m_items.find(key);
    if (it != m_items.end()) return &it->second;
    if (!tex || tex->view() == VK_NULL_HANDLE) return nullptr;

    Entry e;
    e.ds = ImGui_ImplVulkan_AddTexture(tex->view(),
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (e.ds == VK_NULL_HANDLE) return nullptr;
    e.w = static_cast<float>(tex->width() ? tex->width() : 1u);
    e.h = static_cast<float>(tex->height() ? tex->height() : 1u);

    return &m_items.emplace(key, e).first->second;
}

void ThumbnailCache::erase(const std::string& key) {
    auto it = m_items.find(key);
    if (it != m_items.end()) {
        if (it->second.ds != VK_NULL_HANDLE) {
            ImGui_ImplVulkan_RemoveTexture(it->second.ds);
        }
        m_items.erase(it);
    }
    m_failed.erase(key);
}

void ThumbnailCache::eraseTree(const std::string& prefix) {
    for (auto it = m_items.begin(); it != m_items.end();) {
        if (it->first.rfind(prefix, 0) == 0) {
            if (it->second.ds != VK_NULL_HANDLE) {
                ImGui_ImplVulkan_RemoveTexture(it->second.ds);
            }
            it = m_items.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = m_failed.begin(); it != m_failed.end();) {
        if (it->first.rfind(prefix, 0) == 0) {
            it = m_failed.erase(it);
        } else {
            ++it;
        }
    }
}

void ThumbnailCache::clear() {
    for (auto& kv : m_items) {
        if (kv.second.ds != VK_NULL_HANDLE) {
            ImGui_ImplVulkan_RemoveTexture(kv.second.ds);
        }
    }
    m_items.clear();
    m_failed.clear();
}

} // namespace editor
