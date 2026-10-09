#version 450
// ============================================================
// pbr.vert —— PBR 顶点着色器
//   set 0 / binding 0 : FrameUBO（相机 + 光照 + 光源空间矩阵）
//   push constant     : 模型矩阵 + PBR 材质参数
//
// 布局必须与 C++ 端严格一致：
//   FrameUBO      -> render/RenderTypes.h  struct FrameUBO   （std140）
//   push_constant -> render/RenderTypes.h  struct PushConstants（std430）
// ============================================================

// 必须与 pbr.frag 的 FrameUBO **逐字段一致**：
// 同一组 set/binding 被多个阶段访问时，声明必须完全匹配。
// 顶点阶段只用到前 6 个成员，但多声明的成员只要偏移不变就无害。
layout(set = 0, binding = 0) uniform FrameUBO {
    mat4 view;
    mat4 proj;
    vec4 lightDir;     // xyz = 传播方向（指向被照物），w = 强度
    vec4 lightColor;   // rgb = 颜色
    vec4 camPos;       // xyz = 相机世界坐标
    mat4 lightSpace;   // 光源空间矩阵（阴影投影用）
    vec4 cameraParams; // x=zNear y=zFar z=切片最近深度 w=深度切片数
    vec4 clusterDims;  // x=gridX y=gridY z=depthSlices w=tileSize
    vec4 screenSize;   // x=width y=height
    vec4 lightParams;  // x=灯数 y=环境光倍率
    mat4 invViewProj;
} ubo;

layout(push_constant) uniform Push {
    mat4 model;
    vec4 baseColorFactor;  // rgb = 基色，a = 未使用
    vec4 pbr;              // x=metallic, y=roughness, z=normalScale, w=ao
    vec4 emissive;         // rgb = 自发光
} push;

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vUV;
layout(location = 3) out vec4 vLightSpacePos;

void main() {
    vec4 world = push.model * vec4(inPos, 1.0);

    vWorldPos = world.xyz;
    // 注：非等比缩放时严格应使用逆转置矩阵，当前场景均为均匀缩放
    vNormal = normalize(mat3(push.model) * inNormal);
    vUV = inUV;
    vLightSpacePos = ubo.lightSpace * world;

    gl_Position = ubo.proj * ubo.view * world;
}
