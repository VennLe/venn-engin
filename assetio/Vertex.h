#pragma once
// ============================================================
// assetio/Vertex —— 磁盘格式里的顶点（**纯 POD，不依赖 Vulkan**）
//
// 为什么不在格式层直接用 `assets::Vertex`：
//   `engine/assets/Mesh.h` 为了 `bindingDescription()` 拉进了
//   `rhi/VulkanCommon.h`（vulkan.h / vk_mem_alloc.h）。离线编译器
//   （AssetC.exe）是**纯 CPU 工具**，不该为了写一个 80 字节的结构体
//   把整个 Vulkan SDK 和 VMA 拉进 include 图 —— 那样它就不能在没有
//   显卡驱动 / 没有 Vulkan 环境的机器上跑（CI、构建机、美术机器）。
//
// 所以这里重新声明一份，布局与 `assets::Vertex` **逐字节一致**：
//   pos(12) normal(12) uv(8) joints(16) weights(16) color(16) = 80
// 一致性由 `engine/assets/VMeshLoader.cpp` 的 static_assert 双向锁死
// （那边同时看得见两个类型）。一旦有人给 `assets::Vertex` 加字段，
// 编译期就会红，而不是"读出来全是错位的数据"那种运行期怪 bug。
// ============================================================

#include <cstddef>
#include <cstdint>

namespace assetio {

struct Vertex {
    float pos[3];
    float normal[3];
    float uv[2];
    // 蒙皮：joints 是**骨架内下标**（0..jointCount-1），不是 SSBO 里的
    // 全局槽位。全局槽位在渲染时由 push.skin.x（jointBase）加上去 ——
    // 这样同一份 `.vmesh` 可以被任意多个实体复用，实体之间互不干扰。
    std::uint32_t joints[4];
    float weights[4];
    // 顶点色（Mesh Paint）。非蒙皮/未染色时全 1 = "不染色"。
    float color[4];
};

constexpr std::size_t kVertexSize = 80;

// 各字段偏移也要对上 —— `assets::Vertex` 的 attributeDescriptions()
// 用的是 offsetof，两边错一个字段就是"法线读成 UV"。
static_assert(sizeof(Vertex) == kVertexSize, "Vertex 必须是 80 字节");
static_assert(offsetof(Vertex, pos) == 0, "pos 偏移");
static_assert(offsetof(Vertex, normal) == 12, "normal 偏移");
static_assert(offsetof(Vertex, uv) == 24, "uv 偏移");
static_assert(offsetof(Vertex, joints) == 32, "joints 偏移");
static_assert(offsetof(Vertex, weights) == 48, "weights 偏移");
static_assert(offsetof(Vertex, color) == 64, "color 偏移");

inline Vertex makeVertex() {
    Vertex v{};
    v.color[0] = v.color[1] = v.color[2] = v.color[3] = 1.0f;
    return v;
}

} // namespace assetio
