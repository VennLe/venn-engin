#version 450
// ============================================================
// particle.vert —— 粒子的 billboard（R6-1）
//
// **没有顶点缓冲**：一个实例 = 一个四边形，6 个顶点由 gl_VertexIndex
// 现拼（与 fullscreen.vert 同一个路子）。每实例的粒子数据从 SSBO 取。
//
// 实例号 → 紧凑表 → 粒子下标：
//
//     gl_InstanceIndex ──► alive[aliveBase + gl_InstanceIndex] ──► 粒子下标
//
// aliveBase 由 push 常量给出（0 = 加色那张表，总槽数 = Alpha 那张表）。
// 于是同一个 SSBO 只读一次，两条间接绘制各自从自己那一段开始。
//
// ---- 四种渲染模式（Renderer > Render Mode）----
//
//   Billboard          四边形永远正对相机（默认）
//   StretchedBillboard 长轴换成**速度方向**，长度加上 |v| × 拉伸系数
//                       —— 雨丝、火星、快速碎屑
//   HorizontalBillboard 躺平：法线 = 世界 +Z，up = 世界 +Z
//                       —— 地面上的环形冲击波、涟漪
//   VerticalBillboard  竖立但不跟着相机滚转：up 恒为世界 +Z
//                       —— 火柱、瀑布、激光（相机俯仰时不"倒"）
//
// 与 Unity 的 Render Mode 一一对应（Unity 的 "Billboard" 默认就是
// 前两种的合体：拉伸系数为 0 时退化成 Billboard）。
//
// ---- 旋转（Rotation over Lifetime）----
//   逐粒子角度存在 p.extra.x（度）。旋转施加在**角点**上：
//   先把 (cx,cy) 转一个角度再乘到 right/up 上。vUv 仍然用未旋转的
//   (cx,cy) —— 它是"四边形上的参数"，圆盘形状与旋转无关，但保持
//   [0,1]² 的一致参数化才能让片元的圆形判定正确。
//
// ---- Simulation Space = Local ----
//   位置存的是**发射器局部坐标**（p.extra.y = 1），这里用
//   emitters[p.misc.w].world 变到世界。于是"移动 / 旋转发射器"自动带着
//   粒子走，不需要 CPU 去动每个粒子。
// ============================================================

#include "particle_common.glsl"

layout(std430, set = 0, binding = 0) readonly buffer ParticleBuf {
    Particle particles[];
};

layout(std430, set = 0, binding = 1) readonly buffer AliveBuf {
    uint alive[];
};

// FrameUBO 只声明用到的**前缀**（GLSL 的 uniform block 允许比 buffer 短）。
// 这里必须声明到 camPos（offset 160）为止 —— 中间两个 vec4 用不上，
// 但 offset 是位置决定的，跳不过去。
layout(set = 0, binding = 2) uniform FrameUBO {
    mat4 view;
    mat4 proj;
    vec4 lightDir;
    vec4 lightColor;
    vec4 camPos;
} ubo;

// 排序后的紧凑表（Sort Mode != None 时有效）
layout(std430, set = 0, binding = 4) readonly buffer AliveSortedBuf {
    uint aliveSorted[];
};

// 发射器表（Simulation Space = Local 时要用 world 把粒子变到世界）
layout(std430, set = 0, binding = 5) readonly buffer EmitterBuf {
    Emitter emitters[];
};

layout(push_constant) uniform FxPush {
    // x = 软化距离(米) y = 软化开关(0/1) z = 尺寸倍率 w = 亮度倍率
    vec4 params;
    // x = 视口宽 y = 视口高 z = 近平面 w = 远平面
    vec4 screen;
    // x = aliveBase（这一条 draw 用哪张紧凑表：加色 0，Alpha = 总槽数）
    // y = 用排序后的表(1) 还是原子顺序的表(0)
    // z = 发射器数量（把 p.misc.w 夹进合法范围用）
    // w = 保留。
    // **x 必须逐 draw 变** —— 两张表挤在同一个 SSBO 的两段里，两条
    // 间接绘制共用同一个描述符集，只有 push 常量能区分。
    ivec4 extra;
} pc;

layout(location = 0) out vec2 vUv;
layout(location = 1) out vec4 vColor;
// 该顶点的**视图空间深度**（正数，米）。片元用它和场景深度比较做软粒子；
// 这个 pass 没有深度附件，所以 gl_FragCoord.z 是没有意义的。
layout(location = 2) out float vViewZ;

void main() {
    const uint inst = gl_InstanceIndex;
    const uint listIdx = uint(pc.extra.x) + inst;
    const uint pidx = (pc.extra.y != 0) ? aliveSorted[listIdx]
                                        : alive[listIdx];
    const Particle p = particles[pidx];

    // 死粒子不该出现在紧凑表里，但防御一下：退化到裁剪空间外
    // （w = 0 会让光栅化直接丢掉这个三角形）。
    if (p.velLife.w < 0.0 || p.posSize.w <= 0.0) {
        gl_Position = vec4(0.0, 0.0, 0.0, 0.0);
        vUv = vec2(0.0);
        vColor = vec4(0.0);
        vViewZ = 1.0;
        return;
    }

    // ---- 6 个顶点 → 两个三角形 ----------------------------------------
    //
    // ⚠⚠ 这里是 **TRIANGLE_LIST**（见 rhi::PipelineDesc::topology 的默认
    // 值），不是 strip。两者的"顶点 → 三角形"配对**不一样**：
    //
    //     TRIANGLE_LIST   → (0,1,2) (3,4,5)
    //     TRIANGLE_STRIP  → (0,1,2) (2,1,3)
    //
    // 顶点数同样是 6，写法也一样，但在 list 下照 strip 的直觉去摆角点会
    // **少画一个楔形**：若角点按 (vi&1, vi&2) 给出，第二个三角形会变成
    // (3,4,5) = 右上、左下、右下 —— 于是两个三角形只覆盖了
    // {x+y ≤ 0} ∪ {y ≤ x}，而**正上方那块楔形（顶点为左上、中心、右上）
    // 没有任何三角形覆盖**。症状极具迷惑性：圆的左右两侧和下半部都在、
    // 尺寸也对，只在**顶部中心缺一个从圆心张开到两个上角的 V 形缺口**
    // —— 单看画好的那半边完全像是"羽化 / 软化算错了"。
    //
    // 直接把 6 个角点列出来，配对照着拓扑读，就不会再踩。
    // gl_VertexIndex 恒在 [0, 5]（间接命令的 vertexCount = 6，**非索引**）。
    const vec2 kCorner[6] = vec2[6](
        vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(-1.0, 1.0),   // 左下三角形
        vec2( 1.0,  1.0), vec2(-1.0, 1.0), vec2( 1.0, -1.0)); // 右上三角形
    const uint vi = gl_VertexIndex;
    const vec2 cn = kCorner[vi];

    // ---- 所属发射器（渲染模式 / Local 空间的 world 都在它身上）----
    // p.misc.w 是出生时记下的发射器下标。发射器被删掉之后这个下标可能
    // 越界，所以夹一下 —— 读 SSBO 越界不会报错，只会拿到垃圾，
    // 画面上的表现是"某颗粒子突然变成另一种渲染模式"，很难往回查。
    const uint eIdx = min(uint(max(p.misc.w, 0.0)),
                          uint(max(pc.extra.z - 1, 0)));
    const Emitter em = emitters[eIdx];

    // ---- 位置（Local 空间要先变到世界）----
    const vec3 center = (p.extra.y > 0.5)
                            ? (em.world * vec4(p.posSize.xyz, 1.0)).xyz
                            : p.posSize.xyz;
    const float halfSize = p.posSize.w * 0.5 * pc.params.z;

    const vec3 fwd = normalize(ubo.camPos.xyz - center + vec3(1e-6));
    const vec3 wUp = vec3(0.0, 0.0, 1.0);   // 本引擎是 Z-up 右手系

    // ---- 基（right / up）与长轴缩放 ----
    vec3 right = cross(fwd, wUp);
    const float rl = length(right);
    right = (rl > 1e-4) ? right / rl : vec3(1.0, 0.0, 0.0);
    vec3 up = cross(right, fwd);
    float upScale = halfSize;

    const int mode = int(em.renderMisc.x + 0.5);
    if (mode == FX_RENDER_HORIZONTAL || mode == FX_RENDER_VERTICAL) {
        // 两者的 (right, up) 基是**同一个**：up 恒为世界 +Z、right 水平。
        // 差别在四边形怎么解释这个基 —— 对四边形来说完全等价，
        // 但保留两个枚举值是因为 Unity 面板上就是两项，而且将来
        // 网格化 / 纹理朝向时它们会分开。
        up = wUp;
        right = cross(up, fwd);
        const float h = length(right);
        right = (h > 1e-4) ? right / h : vec3(1.0, 0.0, 0.0);
    } else if (mode == FX_RENDER_STRETCHED && p.misc.z > 1e-5) {
        // 拉伸：长轴换成速度方向，长度 = 直径 + 拉伸长度
        up = normalize(p.velLife.xyz + vec3(1e-6));
        upScale = halfSize + p.misc.z * 0.5;
    }

    // ---- 逐粒子旋转 ----
    const float rot = radians(p.extra.x);
    const float cr = cos(rot);
    const float sr = sin(rot);
    const vec2 rc = vec2(cn.x * cr - cn.y * sr, cn.x * sr + cn.y * cr);

    const vec3 world =
        center + right * (rc.x * halfSize) + up * (rc.y * upScale);
    const vec4 viewPos = ubo.view * vec4(world, 1.0);

    gl_Position = ubo.proj * viewPos;
    // vUv 用**未旋转**的角点：它是"四边形上的参数"，圆盘与旋转无关，
    // 但保持 [0,1]² 的一致参数化才能让片元的圆形判定正确。
    vUv = cn * 0.5 + 0.5;
    vColor = p.color;
    // RH 视图空间里相机看 -Z → 正深度 = -z
    vViewZ = -viewPos.z;
}
