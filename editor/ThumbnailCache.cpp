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

// E13 LRU：超出上限时淘汰"最久未使用"的一项。
// keep = 本次刚插入/刚取用的 key —— 它必须留下，否则调用方拿到的
// `const Entry*` 会当场变成悬垂指针（下一步就要用它画图）。
void ThumbnailCache::evictIfNeeded(const std::string& keep) {
    while (m_items.size() > m_cap) {
        auto victim = m_items.end();
        for (auto it = m_items.begin(); it != m_items.end(); ++it) {
            if (it->first == keep) continue;
            if (victim == m_items.end() ||
                it->second.lastUse < victim->second.lastUse) {
                victim = it;
            }
        }
        if (victim == m_items.end()) return;   // 只剩 keep 一项
        if (victim->second.entry.ds != VK_NULL_HANDLE) {
            ImGui_ImplVulkan_RemoveTexture(victim->second.entry.ds);
        }
        m_items.erase(victim);
    }
}

const ThumbnailCache::Entry* ThumbnailCache::get(assets::AssetManager& am,
                                                 const std::string& filePath) {
    if (filePath.empty()) return nullptr;

    auto it = m_items.find(filePath);
    if (it != m_items.end()) {
        it->second.lastUse = ++m_tick;   // 命中即刷新"最近使用"
        return &it->second.entry;
    }
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
    if (it != m_items.end()) {
        it->second.lastUse = ++m_tick;
        return &it->second.entry;
    }
    if (!tex || tex->view() == VK_NULL_HANDLE) return nullptr;

    Entry e;
    e.ds = ImGui_ImplVulkan_AddTexture(tex->view(),
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (e.ds == VK_NULL_HANDLE) return nullptr;
    e.w = static_cast<float>(tex->width() ? tex->width() : 1u);
    e.h = static_cast<float>(tex->height() ? tex->height() : 1u);

    Slot s;
    s.entry = e;
    s.lastUse = ++m_tick;
    auto inserted = m_items.emplace(key, s).first;
    evictIfNeeded(key);   // 先插入再按 LRU 收口，keep 保证不被误伤
    return &inserted->second.entry;
}

void ThumbnailCache::erase(const std::string& key) {
    auto it = m_items.find(key);
    if (it != m_items.end()) {
        if (it->second.entry.ds != VK_NULL_HANDLE) {
            ImGui_ImplVulkan_RemoveTexture(it->second.entry.ds);
        }
        m_items.erase(it);
    }
    m_failed.erase(key);
}

void ThumbnailCache::eraseTree(const std::string& prefix) {
    for (auto it = m_items.begin(); it != m_items.end();) {
        if (it->first.rfind(prefix, 0) == 0) {
            if (it->second.entry.ds != VK_NULL_HANDLE) {
                ImGui_ImplVulkan_RemoveTexture(it->second.entry.ds);
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
        if (kv.second.entry.ds != VK_NULL_HANDLE) {
            ImGui_ImplVulkan_RemoveTexture(kv.second.entry.ds);
        }
    }
    m_items.clear();
    m_failed.clear();
}

} // namespace editor
