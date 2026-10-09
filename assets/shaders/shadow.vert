#version 450
// ============================================================
// shadow.vert —— 阴影贴图顶点着色器（depth-only Pass）
//
// 只做一件事：把顶点变换到光源裁剪空间。
// 深度值由光栅化阶段自动写入深度附件，不需要输出颜色。
//
// 注意：这里刻意不对投影做 Y 翻转（与主相机不同）。
// 阴影图是"自洽"的：渲染与采样使用同一个 lightSpace 矩阵，
// 两边映射一致，结果就正确。
// ============================================================

layout(set = 0, binding = 0) uniform FrameUBO {
    mat4 view;
    mat4 proj;
    vec4 lightDir;
    vec4 lightColor;
    vec4 camPos;
    mat4 lightSpace;
} ubo;

layout(push_constant) uniform Push {
    mat4 model;
    vec4 baseColorFactor;
    vec4 pbr;
    vec4 emissive;
} push;

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;

void main() {
    gl_Position = ubo.lightSpace * push.model * vec4(inPos, 1.0);
}
