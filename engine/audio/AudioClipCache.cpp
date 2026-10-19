#include "audio/AudioClipCache.h"

namespace audio {

std::shared_ptr<AudioClip> AudioClipCache::get(const std::string& path,
                                               AudioClip::Mode mode,
                                               std::string& err) {
    err.clear();
    // ⚠ 流式**不进缓存**（§G14-6 第 4 条）。
    //   理由不是"省内存"，而是**文件句柄的生命周期**：缓存里的 shared_ptr
    //   会让解码器活到进程结束，一首歌一个句柄；而且"再 get 一次拿到同一个
    //   对象"对音乐是错的 —— 同一时刻只有一路流，第二次要的是**新的解码器**
    //   从 0 开始读（`playMusic` 先 `stopMusic` 再重来）。
    //   谁持有它？`AudioSystem::m_stream.clip`。`stopMusic` 一 reset，
    //   解码器与文件句柄立刻释放。
    if (mode == AudioClip::Mode::Streaming) {
        auto clip = std::make_shared<AudioClip>();
        if (!clip->load(path, mode, err)) return nullptr;
        return clip;
    }

    // ⚠ 命中判据要带 mode：同一个文件当音效用（内存）和当音乐用（流式）
    //   是两种不同的对象，混用会把 42 MB 的曲子整个读进内存。
    const std::string key = "m:" + path;
    const auto it = m_map.find(key);
    if (it != m_map.end()) {
        err.clear();
        return it->second;
    }

    auto clip = std::make_shared<AudioClip>();
    if (!clip->load(path, mode, err)) return nullptr;
    m_map.emplace(key, clip);
    return clip;
}

void AudioClipCache::clear() { m_map.clear(); }

std::uint64_t AudioClipCache::residentBytes() const {
    std::uint64_t total = 0;
    for (const auto& kv : m_map) total += kv.second->residentBytes();
    return total;
}

} // namespace audio
