#version 450
// ============================================================
// shadow.vert —— 阴影深度图顶点着色器（depth-only Pass）
//
// **级联方向光（CSM）与局部光源（点光 / 聚光灯）共用这一个着色器**。
// 两者都只做一件事：把顶点变换到**某一层阴影图**对应的光源裁剪空间。
// 深度值由光栅化阶段自动写入深度附件，不需要输出颜色。
//
// 两种模式的差别只有"矩阵从哪来"（靠 push.which.y 选）：
//   push.which.y == 0 → ubo.lightSpace[push.which.x]        （CSM，级联号）
//   push.which.y == 1 → lightShadows.records[push.which.x]  （局部光，层号）
//
// 之所以能有一套和主前向管线不同的推常量范围，是因为本 Pass 在**自己的
// render pass 内自行绑定 set 0** —— 绑定时与绘制时用的是同一个管线布局
// 对象，永远兼容。前向 Pass 那边就不行：它"绑一次 set 0 再切一堆管线"，
// 那些管线的推常量范围必须逐字段相同（踩过一次，见 grid 管线的注释）。
//
// 注意：这里刻意不对投影做 Y 翻转（与主相机不同）。
// 阴影图是"自洽"的：渲染与采样使用同一个矩阵，
// 两边映射一致，结果就正确。
//
// ⚠ 立方体面序（点光的 6 层）必须与 common.glsl 的 vennCubeDir 表一致。
//   由 LocalShadowPass 在 CPU 侧用同一张标准 GL 立方体面视图矩阵生成 ——
//   两边对不上时的症状是"某个面的阴影跑到相邻面上"，非常难定位。
// ============================================================

// 只声明到 lightSpace[4]：本 Pass 用不到后面的字段。
// 前缀声明是允许的（GLSL 的 uniform block 可以比绑定的 buffer 短），
// 但**偏移必须与 pbr.vert / pbr.frag 完全一致**。
layout(set = 0, binding = 0) uniform FrameUBO {
    mat4 view;
    mat4 proj;
    vec4 lightDir;
    vec4 lightColor;
    vec4 camPos;
    mat4 lightSpace[4];  // 索引 = 级联号
} ubo;

// 局部光源阴影矩阵（与 pbr.frag 的 set0/binding11 同一块 buffer）。
// 逐层一条记录，索引 = 阴影图层号。点光占连续的 6 层（6 个立方体面），
// 聚光灯占 1 层。矩阵已经在 CPU 侧算好（含投影），这里只做取值。
struct LightShadow {
    mat4 matrix;  // 世界 → 光源裁剪空间（请用 *_ZO 投影，保证 z ∈ [0,1]）
    vec4 params;  // x = near, y = far, zw 保留
};

layout(set = 0, binding = 11) readonly buffer LightShadows {
    LightShadow records[];
} lightShadows;

// 骨骼矩阵 SSBO（与 pbr.vert 的 set0/binding6 同一块 buffer）。
// 静态网格 inWeights 全 0，走原路径。readonly：顶点阶段只读。
layout(set = 0, binding = 6) readonly buffer JointMatrices {
    mat4 jointMats[];
} jointBuf;

layout(push_constant) uniform Push {
    mat4 model;
    vec4 skin;   // x = jointBase（y/z/w 未用）
    // x = 索引（级联号 或 阴影图层号）
    // y = 0 → CSM 取 FrameUBO.lightSpace[x]；1 → 局部光取 records[x].matrix
    vec4 which;
} push;

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in uvec4 inJoints;
layout(location = 4) in vec4 inWeights;

void main() {
    vec4 localPos = vec4(inPos, 1.0);
    if (dot(inWeights, vec4(1.0)) > 0.0) {
        mat4 skinMat =
              jointBuf.jointMats[int(push.skin.x) + int(inJoints.x)] * inWeights.x
            + jointBuf.jointMats[int(push.skin.x) + int(inJoints.y)] * inWeights.y
            + jointBuf.jointMats[int(push.skin.x) + int(inJoints.z)] * inWeights.z
            + jointBuf.jointMats[int(push.skin.x) + int(inJoints.w)] * inWeights.w;
        localPos = skinMat * vec4(inPos, 1.0);
    }

    const int idx = int(push.which.x);
    mat4 lightClip;
    if (int(push.which.y + 0.5) == 1) {
        lightClip = lightShadows.records[idx].matrix;
    } else {
        lightClip = ubo.lightSpace[idx];
    }
    gl_Position = lightClip * push.model * localPos;
}
