#version 450
// ============================================================
// ui2.vert —— 游戏 UI 的顶点着色器（U2）
//
// 一个 shader 覆盖四种图元（mode 是逐顶点的，用分支而不是四份 shader）：
//   0 = 纯色矩形      inPos = 中心，inUv = 角点符号 ∈ {-1,+1}
//   1 = 贴图四边形    inPos = 顶点本身，inUv = 图集 uv
//   2 = SDF 字形      inPos = 顶点本身，inUv = 图集 uv，inHalf.x = 平滑宽度
//   3 = SDF 圆角矩形  inPos = 中心，inUv = 角点符号，inRadius = 半径
//
// 为什么"矩形只传中心 + 半宽"而不是传四个角点：
//   SDF 距离场需要在**片元里知道相对中心的偏移**（sdRoundBox 的输入）。
//   传角点的话，片元还得再补一个中心 varying（多 8 字节/顶点）；传中心 +
//   符号则 local 在顶点里直接算好，且矩形永远只有 4 个顶点。
//
// 坐标：像素，y 向下（UI 的直觉方向）。换算成 NDC 时除的是**输出分辨率**
//   —— 由 push constant 给，绝不用 DRS 之后的场景分辨率（UI 不跟 DRS 缩放）。
// ============================================================

layout(location = 0) in vec2 inPos;      // 像素坐标
layout(location = 1) in vec2 inUv;       // 角点符号（mode 0/3）或图集 uv（mode 1/2）
layout(location = 2) in vec4 inColor;    // RGBA8 → 归一化到 0..1（sRGB 编码值）
layout(location = 3) in vec2 inHalf;     // 半宽/半高（mode 0/3）或 (平滑宽度,0)（mode 2）
layout(location = 4) in float inRadius;  // 圆角半径（像素）
layout(location = 5) in float inMode;    // 图元种类

// 8 字节：输出分辨率。**每帧一改**，所以走 push constant 而不是 UBO
// （UI 不需要 set 里的 UBO —— 整个通道只绑 1 个 sampler set）。
layout(push_constant) uniform PC {
    vec2 viewport;
} pc;

layout(location = 0) out vec2 vUv;
layout(location = 1) out vec4 vColor;
layout(location = 2) out vec2 vHalf;
layout(location = 3) out float vRadius;
layout(location = 4) out float vMode;
layout(location = 5) out vec2 vLocal;

void main() {
    vec2 px = inPos;
    vec2 local = vec2(0.0);

    // mode 0 / 3：inPos 是中心，按角点符号展开
    // （用 `inMode < 0.5 || inMode > 2.5` 而不是两次 if，省一次比较）
    if (inMode < 0.5 || inMode > 2.5) {
        local = inUv * inHalf;
        px = inPos + local;
    }

    // ⚠⚠ **Vulkan 的 NDC y 轴是向下的**（+1 在屏幕底部），与 OpenGL 相反。
    //   所以"像素 y 向下"到 NDC 是**直接线性映射**，不能写成 `1 - 2y/H`
    //   —— 那样整个 UI 会上下镜像：血条跑到屏幕顶部、字形倒过来。
    //   这和 3D 那边 `proj[1][1] *= -1` 是**同一件事的两面**：
    //   场景矩阵翻了一次（把 GL 约定改成 Vulkan），UI 这里就只剩零次。
    //   （2026-10-18 U8 实测踩到：血条出现在左上角且"100 / 100"倒着写。）
    vec2 ndc = vec2(px.x / pc.viewport.x * 2.0 - 1.0,
                    px.y / pc.viewport.y * 2.0 - 1.0);
    gl_Position = vec4(ndc, 0.0, 1.0);

    vUv = inUv;
    vColor = inColor;
    vHalf = inHalf;
    vRadius = inRadius;
    vMode = inMode;
    vLocal = local;
}
