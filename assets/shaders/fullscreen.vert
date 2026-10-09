#version 450
// ============================================================
// fullscreen.vert —— 全屏三角形
//
// 用 gl_VertexID 直接生成三角形，不需要顶点缓冲：
//   顶点 0 → (-1,-1)   顶点 1 → ( 3,-1)   顶点 2 → (-1, 3)
// 这个"大三角形"覆盖整个裁剪空间，比全屏四边形少一次三角形
// 且没有对角线接缝（后处理里最常见的小瑕疵）。
//
// 注意 Vulkan 的 NDC：y = -1 是屏幕**上方**（图像第 0 行）。
// 因此 vUv.y = 0 对应屏幕顶部，与 texture() 的行序一致，
// 后处理采样无需额外翻转。
// ============================================================

layout(location = 0) out vec2 vUv;

void main() {
    // Vulkan GLSL 里是 gl_VertexIndex（不是 gl_VertexID）
    // (gl_VertexIndex<<1)&2 → 0,2,0 ; gl_VertexIndex&2 → 0,0,2
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    vUv = p;                                   // 屏幕内插值恰好落在 [0,1]
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
