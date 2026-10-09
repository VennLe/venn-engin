#pragma once
// ============================================================
// render/RenderTypes.h —— 渲染层共享数据结构
//
// 单独成文件的原因：Renderer 与 ShadowPass 都要用到这些结构，
// 若定义在 Renderer.h 里就会形成循环 include。
//
// 改这里的任何一个字段，都必须同步修改对应的着色器声明：
//   FrameUBO      -> pbr.vert / pbr.frag / shadow.vert
//   PushConstants -> pbr.vert / pbr.frag / shadow.vert
// ============================================================

#include "rhi/VulkanCommon.h"

namespace render {

// 每帧全局常量缓冲（std140 布局，共 368 字节）
//
// 前半段（240 字节）是阴影 Pass 就在用的老字段；着色器只声明自己
// 需要的前缀即可 —— GLSL 的 uniform block 允许比绑定的 buffer 短。
// 后半段是分簇光照新增的。
struct FrameUBO {
    glm::mat4 view;        // offset   0
    glm::mat4 proj;        // offset  64
    glm::vec4 lightDir;    // offset 128  xyz=传播方向（指向被照物），w=强度
    glm::vec4 lightColor;  // offset 144  rgb=颜色
    glm::vec4 camPos;      // offset 160  xyz=相机世界坐标
    glm::mat4 lightSpace;  // offset 176  光源空间矩阵（阴影投影）

    // ---- 分簇前向渲染（clustered forward）----

    // x = zNear, y = zFar
    // z = 分簇切片的最近深度（**不是** zNear，见下）
    // w = 深度切片数（与 clusterDims.z 冗余，方便着色器少读一个分量）
    //
    // 为什么 z ≠ zNear：本引擎的投影由 glm::perspective 生成，且**没有**
    // 定义 GLM_FORCE_DEPTH_ZERO_TO_ONE，所以 NDC z 的映射范围是 [-1, 1]。
    // 而 Vulkan 只在 clip z ∈ [0,1] 内保留几何 —— 于是真正"可见"的最近
    // 深度是 z_ndc=0 对应的那个值：
    //     d_min = 2 * zNear * zFar / (zNear + zFar)
    // 深度切片必须从 d_min 起算，否则靠近相机的几层切片全落在裁剪区外，
    // 白白浪费掉（而且 AABB 也会算错）。
    glm::vec4 cameraParams;  // offset 240

    // x = gridX, y = gridY, z = depthSlices, w = tileSize（像素）
    glm::vec4 clusterDims;   // offset 256

    // x = 宽度, y = 高度, z = 1/宽度, w = 1/高度
    glm::vec4 screenSize;    // offset 272

    // x = 光源数量, y = 环境光强度倍率, z = 是否启用分簇剔除(0/1),
    // w = 每簇槽位数上限（须与 ClusteredLighting::maxLightsPerCluster 相同，
    //     片元着色器靠它做防御性截断）
    glm::vec4 lightParams;   // offset 288

    // NDC(齐次 w=1) → 世界 的反投影矩阵，= inverse(proj * view)
    // 分簇 compute 用它把每个簇的 8 个角点还原到世界空间求 AABB
    glm::mat4 invViewProj;   // offset 304
};

// 每 draw call 的推送常量（std430 布局，共 112 字节）
// Vulkan 规范至少保证 128 字节，此处留有余量
struct PushConstants {
    glm::mat4 model;           //   0
    glm::vec4 baseColorFactor; //  64
    glm::vec4 pbr;             //  80  x=metallic y=roughness z=normalScale w=ao
    glm::vec4 emissive;        //  96  rgb=自发光
};

// 地平面栅格的推送常量（与 assets/shaders/grid.{vert,frag} 一一对应）。
// 数据只有 48 字节 —— 它不需要额外描述符（相机矩阵从 FrameUBO 读），
// 所以单独开一份小的，而不是复用上面那 112 字节的 PushConstants。
// ⚠ 注意：这 48 字节只是"实际推多少"。栅格**管线布局**仍然声明与主前向
// 管线相同的 112 字节 range（见 Renderer::createPipelines 里的注释），
// 否则它的 layout 会与 set 0 的绑定不兼容，把整个 Pass 的描述符状态打断。
struct GridPushConstants {
    // x = 半边长(米) y = 最小格(米) z = 主格(米) w = 平面高度 y
    glm::vec4 params;
    // rgb = 线色, a = 总不透明度
    glm::vec4 color;
    // x = 开始淡出距离 y = 完全消失距离 z = 线宽(px) w = 最小格强度
    glm::vec4 fade;
};

static constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 2;
static constexpr uint32_t kTextureSetCount = 3;  // albedo / normal / orm

} // namespace render
