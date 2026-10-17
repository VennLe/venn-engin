#pragma once
// ============================================================
// render/ShaderHotReload —— .spv 产物的 mtime 轮询
//
// 只负责回答一个问题："哪些 .spv 文件变了？"。**怎么重建**是
// Renderer 的事（它知道哪个文件属于哪条管线）—— 本类刻意不持有
// 任何 Vk 对象，重建失败也碰不到它。
//
// 为什么轮询 mtime 而不是 ReadDirectoryChangesW：
//   · 与脚本热重载（engine/script/ScriptEngine.cpp 的 fileWriteTime/
//     poll）同一套模式，行为可预期、跨平台、无窗口消息依赖；
//   · 轮询间隔 0.25s（15 帧 @60fps）对"改 shader → 存盘 → 看效果"
//     完全够用，一秒的 fs event 也省不出可感知的延迟。
//
// 为什么监听 .spv 产物而不是 .frag/.vert 源码：
//   · .spv 是唯一事实产物 —— cmake 构建、手动 glslc、未来任何材质
//     工具，只要更新了产物就能触发，编辑器不关心它怎么来的；
//   · 监听源码就得在编辑器里复刻 CMake 的变体宏表（8 份 -D 组合），
//     两处维护必然漂移。
//
// 时序契约：watch() 要在管线创建**之后**调用 —— 它记下初始 mtime，
// 保证启动后第一轮 poll 不会把"本来就在的文件"误报成变化。
// ============================================================

#include <cstdint>
#include <string>
#include <vector>

namespace render {

class ShaderHotReload {
public:
    // 注册一个 .spv 文件名（相对 shader 目录，如 "pbr.k3.frag.spv"）。
    // 文件此刻不存在也没关系（lastWrite 记 0，出现时自然触发）。
    void watch(const std::string& spvFile);

    // 扫一轮 mtime。返回变化的文件数（0 = 无事发生）。
    uint32_t poll();

    // 最近一轮 poll 的变化名单（顺序 = watch 注册顺序）。
    const std::vector<std::string>& dirty() const { return m_dirty; }

private:
    struct Watched {
        std::string name;      // 日志里显示的名字（相对文件名）
        std::string resolved;  // resolveShaderPath 的绝对路径
        int64_t lastWrite = 0;
    };
    std::vector<Watched> m_watched;
    std::vector<std::string> m_dirty;
};

} // namespace render
