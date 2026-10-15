#version 450
// ============================================================
// decal.vert —— 延迟贴花的盒体几何（R4-4）
//
// 画的是**单位立方体**（[-0.5, 0.5]³，由 render/DecalBuffer 自建），
// 乘上贴花的世界矩阵之后就是它在场景里占的那个盒体。
//
// 顶点阶段只用到了 position —— 盒体自己的法线/UV 都不参与着色：
//   · uv 由片元按**局部坐标的平面投影**现算（贴花是"投影"上去的，
//     不是"贴在盒体表面上"）；
//   · 法线由片元用深度重建出的**场景表面**算（不是盒体的面法线）。
//
// 世界矩阵与参数都在 SSBO 里（render::DecalRecord），push 只带 4 字节
// 下标 —— world + worldToLocal 两个 mat4 就已经吃满 push 的 128 字节
// 下限，塞不下 tint / 尺寸 / 粗糙度这些。
//
// 顶点布局与主链**逐字段一致**（assets::Vertex）：这样才能复用同一份
// bindingDescription，盒体网格也就能直接走 assets::Mesh::draw。
// ============================================================

// ---- 与 C++ 的 render::DecalRecord 逐字节一致（std430，192 字节）----
struct DecalRecord {
    mat4 world;         //   0  贴花局部 → 世界
    mat4 worldToLocal;  //  64  世界 → 贴花局部
    vec4 tintOpacity;   // 128  rgb = tint, a = opacity
    vec4 sizeEdge;      // 144  xyz = 盒体尺寸(米), w = 边缘淡出比例
    vec4 params;        // 160  x = roughness, y = metallic, z = normalScale
    vec4 fade;          // 176  x = 淡出起点(米), y = 淡出终点(米)
};

layout(std430, set = 0, binding = 2) readonly buffer DecalBuffer {
    DecalRecord decals[];
};

// FrameUBO 只声明用到的前缀（GLSL 的 uniform block 允许比 buffer 短）。
// view / proj 在 [0,128)，invViewProj 那一段留给片元。
layout(set = 0, binding = 0) uniform FrameUBO {
    mat4 view;
    mat4 proj;
} ubo;

layout(push_constant) uniform Push {
    uint decalIndex;    // 本 draw 用的贴花记录下标
} pc;

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
// ⚠ 必须是 uvec4（不是 ivec4）：顶点属性的 numeric type 要与
//   VkVertexInputAttributeDescription::format 一致，R8G8B8A8_UINT 是
//   **UINT**，写成 ivec4 会被验证层判 VUID-...-Input-08733（管线直接
//   创建失败、画面里什么都看不见）。主链 pbr.vert 用的就是 uvec4。
layout(location = 3) in uvec4 inJoints;
layout(location = 4) in vec4 inWeights;

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vBoxN;

void main() {
    const mat4 w = decals[pc.decalIndex].world;
    const vec4 wp = w * vec4(inPos, 1.0);
    vWorldPos = wp.xyz;
    // 盒体自己的面法线：只给"重建法线退化"时的兜底，不参与正常着色。
    vBoxN = normalize(mat3(w) * inNormal);
    gl_Position = ubo.proj * ubo.view * wp;
}
