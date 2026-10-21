#version 450
// ============================================================
// water.vert —— 水面 / 侧壁顶点着色器（2026-10-21 第六轮）
//
//   set 0 / binding 0 : FrameUBO（相机矩阵，与 pbr.vert 同一块）
//   set 1 / binding 0 : WaterUbo（这块水体的全部参数）
//   push constant     : model（只含平移 + 绕 z 旋转，缩放烘进网格）
//
// ------------------------------------------------------------
// 一块水体有两种几何，靠顶点里的 `part` 区分
// ------------------------------------------------------------
//   part = 0  水面：平坦网格 + 顶点着色器里加波浪
//   part = 1  侧壁顶：贴在水面高度上
//   part = 2  侧壁底：z 由 UBO 的 p4.y（池底以下）给出
//
// 为什么侧壁的高度不在 CPU 烘进顶点：水位是一个可以实时拖的滑块。
// 把"水面到池底"这段距离放进 UBO，拖水位就只是写 16 字节的事；
// 烘进顶点的话每拖一格都要重建顶点缓冲。
//
// 侧壁的存在解决了"水体是一张悬空的纸"——从任何角度看，水的边界都有
// 厚度，而且下沿沉在地形里（见 p4.y 的取值）。
//
// ⚠ 水面位移加在**世界 z** 上（先乘 model 再加），不是本地 z。
//   本地 z 加完再旋转的话，矩形水体一旦有 rotation，波峰就会朝侧面
//   倒过去 —— 而重力永远朝下，水面永远是水平的。
// ============================================================

#include "water_common.glsl"

layout(set = 0, binding = 0) uniform FrameUBO {
    mat4 view;
    mat4 proj;
    vec4 lightDir;
    vec4 lightColor;
    vec4 camPos;
} ubo;

layout(set = 1, binding = 0) uniform WaterUbo {
    vec4 shallow;  // rgb = 浅水色, w = opacity
    vec4 deep;     // rgb = 深水色, w = clarity
    vec4 p0;       // x=level y=waveHeight z=waveLength w=flowSpeed
    vec4 p1;       // x=flowDir.x y=flowDir.y z=ripple w=foam
    vec4 p2;       // x=refraction y=ior z=roughness w=time
    vec4 p3;       // x=kind y=shape z=halfX w=halfZ
    vec4 p4;       // x=bedLevel y=skirtBottom(world z) z/w reserved
    vec4 screen;   // x=width y=height z=1/width w=1/height
    vec4 camera;   // x=zNear y=zFar
} wub;

layout(push_constant) uniform Push {
    mat4 model;
} push;

// 局部坐标。z 一律 0 —— 水面在 z=0，侧壁底由 part 决定。
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNrm;
layout(location = 2) in float inPart;

layout(location = 0) out vec3 vWorld;
layout(location = 1) out vec3 vNrm;
layout(location = 2) out float vPart;

void main() {
    // 侧壁底：世界 z 是 wub.p4.y，模型已经把水面摆到 p0.x 了，所以本地 z
    // 就是两者之差。
    float localZ = (inPart > 1.5) ? (wub.p4.y - wub.p0.x) : 0.0;
    vec4 world = push.model * vec4(inPos.xy, localZ, 1.0);

    vNrm = normalize(mat3(push.model) * inNrm);
    vPart = inPart;

    // 水面和侧壁**顶圈**都加波浪：顶圈必须跟水面边界逐顶点重合，否则
    // 水面一波动就会从侧壁上沿脱开一条缝（尤其水高于地形的地方很明显）。
    // 侧壁底不动 —— 那是插在土里的，动它没意义。
    if (inPart < 1.5) {
        float h;
        vec2 grad;
        vennWaterWaves(world.xy, wub.p2.w, wub.p0.y, wub.p0.z, 0.0, h, grad);
        world.z += h;
    }

    vWorld = world.xyz;
    gl_Position = ubo.proj * ubo.view * world;
}
