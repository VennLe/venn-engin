#version 450
// ============================================================
// terrain.vert —— 地形顶点着色器（W4-2）
//
//   set 0 / binding 0 : FrameUBO（相机矩阵等，与 pbr.vert 同一块）
//   set 1 / binding 3 : 高度图（R32_SFLOAT，**线性**，见下）
//   push constant     : 模型矩阵 + 地形参数
//
// ------------------------------------------------------------
// 顶点是"平坦格子"，高度在这里加
// ------------------------------------------------------------
// CPU 侧生成的顶点 **pos.z 恒 0**，起伏完全来自高度图的采样：
//
//     pos.z += texture(heightMap, uv).r * maxHeight
//
// 两个理由：
//   ① 同一位置在不同 LOD 下采到**完全相同**的纹素 → 相邻块在共享边上
//      天然对齐，裙边只需要兜住"粗边 vs 细边"的插值误差；
//   ② 改高度图不用重建网格（W4 的非目标里没有"雕刻"，但重载资产很常见）。
//
// ⚠ 高度图必须**线性**采样（VK_FORMAT_R32_SFLOAT，没有 sRGB 变体）。
//   如果哪天换成 R8 并把 sRGB 打开，0.5 会被解码成 0.21，地形整体塌下去
//   一半 —— 而画面只是"山变矮了"，完全不像 bug。
//
// ⚠ uv 是 **纹素中心对齐**的（CPU 侧算成 (i+0.5)/N）。直接写 i/(N-1)
//   会让 uv 落在纹素**边界**上，线性过滤在那里是两个纹素各占一半，
//   与 CPU 的 heightAt（整数坐标取单个采样点）差半个像素 —— 表现是
//   "角色站在地形上悬空 2 cm"，看起来像物理错了。
// ============================================================

// 与 pbr.vert 的 FrameUBO **逐字段一致**（同一 binding 被多个阶段访问，
// 声明必须完全匹配）。这里只声明到 shadowBias，后面的字段用不到 ——
// UBO 块允许比缓冲短，偏移不变就无害。
layout(set = 0, binding = 0) uniform FrameUBO {
    mat4 view;
    mat4 proj;
    vec4 lightDir;
    vec4 lightColor;
    vec4 camPos;
    mat4 lightSpace[4];
    vec4 cameraParams;
    vec4 clusterDims;
    vec4 screenSize;
    vec4 lightParams;
    mat4 invViewProj;
    vec4 envParams;
    vec4 shadowParams;
    vec4 cascadeSplits;
    vec4 shadowBias;
    vec4 localShadowParams;  // 地形不用（局部光不投影到地形上），
                             //   但**必须声明**：要读到 fogParams / shadowFilter
                             //   就必须逐字段占位，否则偏移整体错位。
    vec4 fogParams;
    vec4 viewModeParams;
    vec4 shadowFilter;       // x=级联混合带宽 y=接触硬化强度
} ubo;

layout(set = 1, binding = 3) uniform sampler2D heightMap;

layout(push_constant) uniform Push {
    mat4 model;      // 地形实体的世界矩阵（只允许绕 Z 旋转 + 平移）
    // x = maxHeight（归一化 → 米）
    // y = splatRepeat（权重图 UV 重复）
    // z = layerTiling（每层 albedo 的世界平铺尺寸，米）
    // w = metersPerPixel（法线中心差分的世界步长要用它）
    vec4 params;
    // x = cols  y = rows  z = skirtDepth  w = 层 normal 贴图位掩码
    vec4 params2;
} push;

layout(location = 0) in vec3 inPos;    // 平坦格子（z = 0）
layout(location = 1) in vec2 inUv;     // 纹素中心对齐的高度图 UV
layout(location = 2) in float inSkirt; // 0 = 表面顶点，1 = 裙边底面

layout(location = 0) out vec3 vWorld;
layout(location = 1) out vec2 vUv;

void main() {
    const float heightM = texture(heightMap, inUv).r * push.params.x;
    vec3 p = inPos;
    // 裙边：在**位移之后**再往下拉 —— 高度取自同一个纹素，
    // 所以裙边顶边与表面边线严丝合缝（不会露出一条水平缝）。
    p.z += heightM - inSkirt * push.params2.z;

    const vec4 world = push.model * vec4(p, 1.0);
    vWorld = world.xyz;
    vUv = inUv;
    gl_Position = ubo.proj * ubo.view * world;
}
