// ============================================================
// common.glsl —— 跨着色器共享的小工具
//
// ⚠ 本文件**没有** #version：它总是被 #include 进别的着色器，
//   而 #version 必须是编译单元的第一行。所以这里只有纯函数与常量。
//
// 怎么被找到：CMake 给 glslc 传了 `-I <assets/shaders>`，于是
//   #include "common.glsl" 在所有着色器里都能解析。
//   （注意 glslc 的 -I 与源文件目录无关，不传就找不到。）
//
// 为什么值得单独成文件：立方体贴图的面号→方向映射、GGX 的重要性
// 采样、以及"程序化天空"这套公式，都必须在**多个**着色器里保持
// 逐位一致。天空一旦两边算法漂移（一边取了 dir.z、另一边取了 dir.y），
// 症状是"背景是亮的天顶色、反射却是别的东西"，极难定位。
// 共享头文件把这类不一致从"靠人记住"变成"编译期只有一处"。
// ============================================================

#ifndef VENN_COMMON_GLSL
#define VENN_COMMON_GLSL

const float VENN_PI = 3.14159265359;

// ------------------------------------------------------------
// 立方体贴图面号（与 Vulkan 的 +X, -X, +Y, -Y, +Z, -Z 顺序一致）
// ------------------------------------------------------------
const int VENN_FACE_PX = 0;
const int VENN_FACE_NX = 1;
const int VENN_FACE_PY = 2;
const int VENN_FACE_NY = 3;
const int VENN_FACE_PZ = 4;
const int VENN_FACE_NZ = 5;

// 立方体贴图 面 + 面上 UV(各 ∈ [-1,1]) → 世界方向（已归一化）。
//
// 这张表是 GL/Vulkan 立方体贴图规范的直接反解。由它生成的贴图，
// 之后用 texture(samplerCube, dir) 采样时才能对得上 —— 表写错的话
// 表现是"反射里的图案上下颠倒或左右镜像"，也能用，但不对。
vec3 vennCubeDir(int face, vec2 uv) {
    if (face == VENN_FACE_PX) return normalize(vec3( 1.0, -uv.y, -uv.x));
    if (face == VENN_FACE_NX) return normalize(vec3(-1.0, -uv.y,  uv.x));
    if (face == VENN_FACE_PY) return normalize(vec3( uv.x,  1.0,  uv.y));
    if (face == VENN_FACE_NY) return normalize(vec3( uv.x, -1.0, -uv.y));
    if (face == VENN_FACE_PZ) return normalize(vec3( uv.x, -uv.y,  1.0));
    return normalize(vec3(-uv.x, -uv.y, -1.0));   // VENN_FACE_NZ
}

// 把立方体面在 [0,1]² 的像素坐标映射到 [-1,1]² 的面内 UV（取像素中心）
vec2 vennCubeFaceUv(ivec2 pixel, ivec2 size) {
    return (vec2(pixel) + 0.5) / vec2(size) * 2.0 - 1.0;
}

// 世界方向 → 立方体面号（与 vennCubeDir 的表一致：取绝对值最大的主轴）。
// **点光源阴影**要靠它把"从灯指向片元的向量"变成面号，从而选对深度图的层。
// 这里必须与 vennCubeDir 逐条对应 —— 对不上就会取到相邻面的深度，
// 表现为"物体边缘一圈错误的暗边"。
int vennCubeFaceIndex(vec3 d) {
    vec3 a = abs(d);
    if (a.x >= a.y && a.x >= a.z) return d.x > 0.0 ? VENN_FACE_PX : VENN_FACE_NX;
    if (a.y >= a.z)               return d.y > 0.0 ? VENN_FACE_PY : VENN_FACE_NY;
    return d.z > 0.0 ? VENN_FACE_PZ : VENN_FACE_NZ;
}

// 世界方向 → 该面上的 UV ∈ [-1,1]（vennCubeDir 的逆）。与面号配套使用。
vec2 vennCubeFaceUvFromDir(vec3 d, int face) {
    vec3 a = max(abs(d), vec3(1e-6));
    if (face == VENN_FACE_PX) return vec2(-d.z, -d.y) / a.x;
    if (face == VENN_FACE_NX) return vec2( d.z, -d.y) / a.x;
    if (face == VENN_FACE_PY) return vec2( d.x,  d.z) / a.y;
    if (face == VENN_FACE_NY) return vec2( d.x, -d.z) / a.y;
    if (face == VENN_FACE_PZ) return vec2( d.x, -d.y) / a.z;
    return vec2(-d.x, -d.y) / a.z;   // VENN_FACE_NZ
}

// ------------------------------------------------------------
// 程序化天空（唯一的天空模型）
//
// 参数化成"天顶色 + 地面色"两色调和：t = dir.z 映射到 [0,1]，
// 再取平方让地平线附近的过渡更收拢。
//
// ⚠ 本引擎是 **Z-up**（地面 = z = 0，世界 up 轴 = +Z）。
//   所以"朝上"对应 dir.z，**不是** dir.y。用 dir.y 的话梯度会
//   落到世界 +Y —— 那是水平方向，地面上会出现一条笔直的明暗带，
//   看起来像多了一块阴影。这个坑在 pbr.frag 的历史注释里也记着。
// ------------------------------------------------------------
vec3 vennSkyColor(vec3 dir, vec3 zenith, vec3 ground) {
    float t = clamp(dir.z * 0.5 + 0.5, 0.0, 1.0);
    return mix(ground, zenith, t * t);
}

// ------------------------------------------------------------
// 低差异序列 + GGX（split-sum IBL 的基础）
// ------------------------------------------------------------

// Van der Corput 基 2 反演：把整数 i 的比特倒过来当作 [0,1) 的小数
float vennRadicalInverse(uint bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;  // / 2^32
}

// Hammersley 点集：第 i 个点是 (i/N, 反演(i))，比随机采样收敛快得多
vec2 vennHammersley(uint i, uint n) {
    return vec2(float(i) / float(n), vennRadicalInverse(i));
}

// GGX 法线分布（各向同性，a = roughness²）
float vennDistributionGGX(float NdotH, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / max(VENN_PI * d * d, 1e-6);
}

// 按 GGX 分布重要性采样出半程向量 H（在 N 的切线基里构造）。
// 因为按 D 采样，"重要的方向被采到的概率高"，用几百个样本就能
// 得到接近上千个均匀样本的效果。
vec3 vennImportanceSampleGGX(vec2 xi, vec3 N, float roughness) {
    float a = roughness * roughness;
    float phi = 2.0 * VENN_PI * xi.x;
    float cosTheta = sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y));
    float sinTheta = sqrt(max(1.0 - cosTheta * cosTheta, 0.0));

    vec3 h = vec3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);

    // 把 h 从切线空间旋到 N 为 z 轴的世界基（避免 N≈up 时基退化）
    vec3 up = abs(N.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 T = normalize(cross(up, N));
    vec3 B = cross(N, T);
    return normalize(T * h.x + B * h.y + N * h.z);
}

// Schlick 菲涅尔（环境反射版：粗糙度参与，掠射角的白边收敛）
vec3 vennFresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness) {
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) *
                    pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

#endif // VENN_COMMON_GLSL
