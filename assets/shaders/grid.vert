#version 450
// ============================================================
// grid.vert —— 地平面参考栅格（编辑器视口专用）
//
// 这个 pass 的特别之处：**没有任何顶点缓冲**。
//   · 顶点由 gl_VertexIndex 现算 —— 4 个顶点组成一个铺在 z=0 平面上的
//     大四边形（TRIANGLE_STRIP，Z-up 右手系），世界的格线图案完全交给
//     片元着色器按"到最近格线的距离"解析求出。
//   · 好处：线宽与分辨率无关、自带抗锯齿（fwidth 算屏幕导数），
//     也不需要为栅格维护网格资产 / 顶点格式。
//
// 相机矩阵直接从 FrameUBO 读（和 pbr.vert 同一份 set 0 / binding 0）——
// 不必再给它单独传一个 viewProj。
//
// 深度偏移：这个四边形和地板共面（都在 z = 0），几何上必然 z-fighting。
// 所以把裁剪空间的 z 往近平面推一个极小量：
//     ndc_z = clip.z / clip.w，减小 clip.z 就是"更靠近相机"。
// 只减一个正比于 w 的量，等价于在所有距离上都是同一个 NDC 偏移。
// ============================================================

// 必须与 pbr.vert 的 FrameUBO **逐字段一致**（同一组 set/binding 被多个
// 管线/阶段访问时声明必须匹配）；这里只用 view / proj。
layout(set = 0, binding = 0) uniform FrameUBO {
    mat4 view;
    mat4 proj;
    vec4 lightDir;
    vec4 lightColor;
    vec4 camPos;
    mat4 lightSpace;
    vec4 cameraParams;
    vec4 clusterDims;
    vec4 screenSize;
    vec4 lightParams;
    mat4 invViewProj;
} ubo;

// x = 半边长(米) y = 最小格(米) z = 主格(米) w = 平面高度(z)
layout(push_constant) uniform GridPC {
    vec4 params;
    vec4 color;  // rgb = 线色, a = 总不透明度
    vec4 fade;   // x = 开始淡出距离 y = 完全消失距离 z = 线宽(px) w = 最小格强度
} pc;

layout(location = 0) out vec3 vWorld;

void main() {
    // 0 → (-1,-1)  1 → (1,-1)  2 → (-1,1)  3 → (1,1)
    vec2 c = vec2(float(gl_VertexIndex & 1), float((gl_VertexIndex >> 1) & 1));
    c = c * 2.0 - 1.0;

    const float e = pc.params.x;
    const vec3 world = vec3(c.x * e, c.y * e, pc.params.w);

    vWorld = world;

    // ⚠⚠ 2026-10-17 晚：**这里不再做深度偏移**，改由片元着色器按"当前像素
    //   自己的世界位置"精确重算 gl_FragDepth（见 grid.frag 的 kDepthBias）。
    //
    //   原来这里写的是 `clip.z -= 1e-6 * clip.w`（NDC 上恒定减 1e-6，
    //   几十米处相当于几毫米，看起来"远大于 D32 的分辨率极限"）。实测
    //   **不够**：这个四边形横跨 ±2000 m，光栅化器要在 4 km 的跨度上插值
    //   clip.z，插值误差（≈1e-7 相对 × clip.z）与那 1e-6 是同一量级，
    //   谁大谁小完全看机位 —— 表现就是"栅格盖在地板上时好时坏、
    //   一片一片地消失"。把偏移放大到 1e-3 能盖住误差，但 20 m 处等于
    //   抬高 4 m，旁边的物体会穿模。
    //   正确做法是把这段数学搬到片元里：那里世界坐标只剩毫米级误差，
    //   1e-5 的偏移就有 100 倍余量，也不会有可见的抬升。
    gl_Position = ubo.proj * ubo.view * vec4(world, 1.0);
}
