#pragma once
// ============================================================
// engine/audio/AudioClipCache —— CPU 侧 clip 缓存（**不是** AssetManager）
//
// 为什么单独一个类（docs/audio_roadmap.md §3.3）：
//   `engine/assets/AssetManager` 全是 GPU 资源（构造要 `VkDevice` +
//   `VkCommandPool`）。把 PCM 塞进去有两个直接后果：
//     · 音频会被 GPU 资源的缓存策略（LRU / 常驻）管理，语义错配；
//     · 想跑 `--selftest audio` 就得先有 Vulkan 设备 → 无窗口自检不可能。
//
// 与 `ThumbnailCache` 是"另一个维度的缓存"同一个道理：同一个路径只解一次，
// 引用计数归零就释放。
//
// ⚠ 流式 clip **不进**这个缓存（§G14-6 第 4 条）：播完就释放解码器，
//   否则一首歌的文件句柄会一直挂着。所以 `get(Streaming)` 每次都会
//   **新建**一个解码器（同一时刻只允许一路流，见 AudioSystem::playMusic）。
// ============================================================

#include "audio/AudioClip.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

namespace audio {

class AudioClipCache {
public:
    // 同一路径第二次调用返回**同一个** `AudioClip` 指针（D4 的验收）。
    // 失败返回 nullptr，err 里是原因（不会缓存失败结果，改好文件可以重试）。
    std::shared_ptr<AudioClip> get(const std::string& path,
                                   AudioClip::Mode mode, std::string& err);

    void clear();
    std::size_t count() const { return m_map.size(); }
    std::uint64_t residentBytes() const;

    // 当前缓存里的路径（自检 / Console 用）
    const std::unordered_map<std::string, std::shared_ptr<AudioClip>>& all()
        const {
        return m_map;
    }

private:
    std::unordered_map<std::string, std::shared_ptr<AudioClip>> m_map;
};

} // namespace audio
