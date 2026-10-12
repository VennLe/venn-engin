#version 450
// ============================================================
// sky.vert —— 天空盒背景（全屏三角形 + 反投影出视线方向）
//
// 顶点由 gl_VertexIndex 现算，没有顶点缓冲：
//   顶点 0 → (-1,-1)   顶点 1 → ( 3,-1)   顶点 2 → (-1, 3)
// 这个大三角形铺满裁剪空间，片元阶段每个像素得到一条独立视线。
//
// ---- 为什么用反投影而不是画一个立方体 ----
//   画立方体要先知道它的世界位置与尺寸，还得处理"相机在盒子里"的
//   近平面裁剪；而反投影只需要 invViewProj，永远是全屏无缝隙的。
//
// ---- 举一反三：视线怎么来的 ----
//   NDC (x, y, 1) 是**远平面**上的一个点（本引擎投影是 GL 约定，
//   近平面 → z=-1、远平面 → z=+1，见 Camera::projMatrix）。
//   拿 invViewProj 乘回去就得到该点的世界坐标，再减去相机位置
//   就是方向。这里只关心方向，所以不归一化（片元里再归一化）。
//
// ⚠ 本引擎投影对 Y 做了翻转（proj[1][1] *= -1，适配 Vulkan 的
//   "+Y 向下"）。invViewProj 里带着同一个翻转，所以这里**直接**
//   把 gl_VertexIndex 生成的 NDC 喂进去就对了，不需要额外再翻一次。
// ============================================================

// 与 pbr.vert / pbr.frag 的 FrameUBO 逐字段一致
// （同一组 set/binding 被同一管线的多个阶段访问时必须完全匹配）
layout(set = 0, binding = 0) uniform FrameUBO {
    mat4 view;
    mat4 proj;
    vec4 lightDir;
    vec4 lightColor;
    vec4 camPos;
    mat4 lightSpace[4];  // 4 级级联（天空不用，但偏移必须与 pbr.vert 一致）
    vec4 cameraParams;   // x=zNear y=zFar z=切片最近深度 w=深度切片数
    vec4 clusterDims;    // x=gridX y=gridY z=depthSlices w=tileSize
    vec4 screenSize;     // x=width y=height z=1/width w=1/height
    vec4 lightParams;    // x=灯数 y=环境光倍率 z=剔除开关 w=每簇槽位上限
    mat4 invViewProj;
    vec4 envParams;      // x=IBL 开关 y=预过滤最大 mip z/w=保留
    vec4 shadowParams;
    vec4 cascadeSplits;
    vec4 shadowBias;
} ubo;

layout(location = 0) out vec3 vDir;

void main() {
    // Vulkan GLSL 里是 gl_VertexIndex（不是 gl_VertexID）
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    vec2 ndc = p * 2.0 - 1.0;

    // z = 1 → 远平面。depthTest 关掉了，这个值只用于让图元不被裁剪
    // （Vulkan 要求 clip z ∈ [0, w]，w=1 时 z 必须落在 [0,1]）。
    gl_Position = vec4(ndc, 1.0, 1.0);

    vec4 farPoint = ubo.invViewProj * vec4(ndc, 1.0, 1.0);
    vDir = farPoint.xyz / farPoint.w - ubo.camPos.xyz;
}
