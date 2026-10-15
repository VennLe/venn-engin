// ============================================================
// particle_common.glsl —— GPU 粒子系统（R6-1）共享定义
//
// 被这些着色器 include：
//   particle_sim.comp      模拟（出生 / 老化 / 死亡 / 积分 / 曲线 / 噪声）
//   particle.vert          攒 billboard（四种渲染模式）
//   particle.frag          圆形羽化 + 软粒子
//   particle_histo.comp    按排序键分桶（Sort Mode != None 时）
//   particle_scatter.comp  把粒子按桶序写进排序后的紧凑表
//
// 布局必须与 C++ 的 fx::ParticleGpu / fx::ParticleEmitterGpu 逐字对齐
// （那边各有一条 static_assert 钉住大小，字段顺序只能靠人同步 ——
//  所以两边都写了完整注释，改一处记得照着改另一处）。
//
// ---- 为什么全部用 vec4 打包 ----
// std430 里 vec4 的步长恒为 16 字节，所以"字段清单"就等于"偏移量表"，
// 两边照着同一份清单写就不可能错位。散装 float / int 会引入 4、8 字节
// 的对齐与尾填充 —— layout 错位最常见的那类来源。
// ============================================================

#ifndef VENN_PARTICLE_COMMON_GLSL
#define VENN_PARTICLE_COMMON_GLSL

// ---------------------------------------------------------------- 数据

// 一个粒子（std430，80 B）。与 fx::ParticleGpu 一致。
//
// ⚠ 比 v1 多了一个 vec4（extra）：Rotation over Lifetime 需要一个
//   逐粒子的角度，而原来的 4 个 vec4 一个空位都没有。80 B × 4096 槽位
//   = 320 KB，对显存是无所谓的。
struct Particle {
    vec4 posSize;   // xyz = 世界坐标（米）；w = 当前尺寸（米，已乘曲线）
    vec4 velLife;   // xyz = 速度（米/秒）；w = 已存活秒数。**< 0 = 死**
    vec4 color;     // rgb = 颜色（线性 HDR，已乘自发光）；a = 不透明度
    vec4 misc;      // x = 总寿命 y = 随机种子 z = 拉伸长度(米) w = 发射器下标
    vec4 extra;     // x = 当前旋转（度）
                    // y = 位置是否是**发射器局部坐标**(0/1)
                    //     → Simulation Space = Local 时为 1，顶点着色器
                    //       要用 Emitter.world 把它变到世界
                    // z,w = 保留
};

// 一个发射器（std430，624 B）。与 fx::ParticleEmitterGpu 一致。
//
// 字段分组与 ecs::ParticleEmitterComponent / Inspector 面板的模块一一对应，
// 顺序也刻意照 Unity Shuriken 的面板顺序排（Main → Emission → Shape →
// Velocity → Force → Rotation → Noise → Renderer）。
struct Emitter {
    mat4 world;         //   0  发射器局部 → 世界
    vec4 origin;        //  64  xyz = 世界坐标；w = 形状半径
    vec4 gravityDrag;   //  80  xyz = 重力；w = 阻尼（1/秒）
    vec4 lifeSpeed;     //  96  x = 寿命 y = 寿命方差 z = 初速 w = 速度方差
    vec4 shapeExtents;  // 112  xyz = 盒体半轴；w = 扩散半角 cos 阈值
    vec4 sizeCurve;     // 128  尺寸曲线 4 控制点（t = 0, 1/3, 2/3, 1）
    vec4 alphaCurve;    // 144  不透明度曲线 4 控制点
    vec4 play;          // 160  x = duration y = looping z = prewarm
                        //      w = startDelay
    vec4 emit;          // 176  x = rate y = rateOverDistance z = 形状枚举
                        //      w = emitFrom
    vec4 emit2;         // 192  x = 形状旋转(度) y = radiusThickness
                        //      z = arc(度) w = simulationSpace
    vec4 shapeAux;      // 208  xyz = **发射器局部空间里的重力**
                        //      w = speedModifier 曲线是否启用(0/1)
    vec4 velX;          // 224  轴向速度曲线（加在速度上，米/秒）
    vec4 velY;          // 240
    vec4 velZ;          // 256
    vec4 speedMod;      // 272  速度倍率曲线（乘在 |v| 上）
    vec4 orbital;       // 288  xyz = 轨道角速度（度/秒）w = 轨道半径偏移（米）
    vec4 force;         // 304  xyz = 力（米/秒²）w = 逐粒子随机化比例
    vec4 limitVel;      // 320  x = 速度上限 y = dampen z = 开关(0/1)
                        //      w = 径向速度（米/秒，正 = 向外）
    vec4 rot;           // 336  x = 初始旋转(度) y = 初始随机幅度(度)
                        //      z = 角速度(度/秒) w = 角速度随机比例
    vec4 noiseA;        // 352  x = 强度 y = 频率 z = 滚动速度 w = 阻尼
    vec4 noiseB;        // 368  x = 开关(0/1) y = 倍频层数
                        //      z = velOverLife 用世界空间(0/1) w = 保留
    vec4 miscParams;    // 384  x = 基础尺寸 y = 混合模式 z = 拉伸系数 w = 自发光
    vec4 renderMisc;    // 400  x = 渲染模式 y = 排序模式
                        //      z = 渐变停靠点数 w = 有效 burst 条数
    vec4 dist;          // 416  x = 本帧累积距离 y = 上一步累积距离
                        //      z = 启动延迟结束时的基准距离 w = 保留
    vec4 ramp[8];       // 432..544  颜色渐变（xyz = 颜色，w = 位置）
    vec4 bursts[4];     // 560..608  burst（time, count, cycles, interval）
};

// 形状枚举（与 ecs::ParticleShape 一致）
const int FX_SHAPE_POINT = 0;
const int FX_SHAPE_BOX = 1;
const int FX_SHAPE_SPHERE = 2;
const int FX_SHAPE_HEMISPHERE = 3;
const int FX_SHAPE_CONE = 4;
const int FX_SHAPE_CIRCLE = 5;
const int FX_SHAPE_EDGE = 6;

// Emit From（与 ecs::ParticleEmitFrom 一致）
const int FX_EMIT_VOLUME = 0;
const int FX_EMIT_EDGE = 1;
const int FX_EMIT_SHELL = 2;

// 混合模式（与 ecs::ParticleBlend 一致）
const int FX_BLEND_ADDITIVE = 0;
const int FX_BLEND_ALPHA = 1;

// 渲染模式（与 ecs::ParticleRenderMode 一致）
const int FX_RENDER_BILLBOARD = 0;
const int FX_RENDER_STRETCHED = 1;
const int FX_RENDER_HORIZONTAL = 2;
const int FX_RENDER_VERTICAL = 3;

// 排序模式（与 ecs::ParticleSortMode 一致）
const int FX_SORT_NONE = 0;
const int FX_SORT_BY_DISTANCE = 1;
const int FX_SORT_OLDEST_FIRST = 2;
const int FX_SORT_YOUNGEST_FIRST = 3;

// 模拟空间（与 ecs::ParticleSimulationSpace 一致）
const int FX_SPACE_LOCAL = 0;
const int FX_SPACE_WORLD = 1;

// ---------------------------------------------------------------- 曲线
//
// 4 个控制点在 t = 0、1/3、2/3、1 处**线性**插值。
//
// 为什么不是样条：控制点是给人用滑条拖的（路线图："曲线编辑先用滑条组"），
// 线性段的"折角"恰好让每个滑条的视觉含义是确定的 —— 拖第 2 个控制点只
// 改变 t ∈ [1/3, 2/3] 那一段的斜率。换成贝塞尔会让每个滑条都影响整条曲线，
// 手感反而更差。真正的曲线编辑器留给后续的粒子编辑器任务。
float fxCurve4(vec4 c, float t) {
    const float s = clamp(t, 0.0, 1.0) * 3.0;
    const int i = int(min(floor(s), 2.0));
    const float f = s - float(i);
    const float a = (i == 0) ? c.x : ((i == 1) ? c.y : c.z);
    const float b = (i == 0) ? c.y : ((i == 1) ? c.z : c.w);
    return mix(a, b, f);
}

// ------------------------------------------------- 颜色渐变（Color over Lifetime）
//
// 8 个停靠点，位置递增，段内线性插值。这就是 Unity 的 Color over Lifetime
// 那条色带（也是 Blender 的 Color Ramp）—— 比 v1 的"三段固定 0/0.5/1"
// 多出来的能力是**不均匀的停靠点**：火焰的"根部白 → 中段黄 → 尾端烟黑"
// 需要把中间两个点挤在靠前的位置，三段固定插值做不到。
//
// 传 count 而不是靠哨兵值判断"有效停靠点"：哨兵（比如 alpha = 0）会和
// 合法的黑颜色撞车（黑 = 0,0,0 恰恰是烟最常用的颜色）。
vec3 fxRampEval(vec4 ramp[8], int count, float t) {
    if (count <= 0) return vec3(1.0);
    if (count == 1) return ramp[0].xyz;
    const float u = clamp(t, 0.0, 1.0);
    if (u <= ramp[0].w) return ramp[0].xyz;
    // 线性扫描：8 个点最多 7 次迭代，比二分更短也更好读。
    for (int i = 1; i < 8; ++i) {
        if (i >= count) break;
        if (u <= ramp[i].w) {
            const float span = ramp[i].w - ramp[i - 1].w;
            const float f = (span > 1e-6) ? (u - ramp[i - 1].w) / span : 0.0;
            return mix(ramp[i - 1].xyz, ramp[i].xyz, f);
        }
    }
    return ramp[count - 1].xyz;
}

// ---------------------------------------------------------------- 随机
//
// **确定性哈希**而不是随机数发生器：给定 (槽位, 出生时刻) 就能推出同一颗
// 粒子的全部随机量，不需要保存也不依赖调用顺序。验证脚本要的"同一配置
// 两次运行逐像素一致"就建立在这上面。
//
// 常数取自 Chris Wellons 的 32 位整数哈希（lowbias32）：雪崩性好、
// 一条 xor-multiply 链、在 GPU 上就是几条整数指令。
uint fxHash(uint x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// [0, 1)
float fxHash01(uint x) {
    return float(fxHash(x)) * (1.0 / 4294967296.0);
}

// [0,1)³
vec3 fxHash31(uint x) {
    return vec3(fxHash01(x), fxHash01(x * 1664525u + 1013904223u),
                fxHash01(x * 22695477u + 1u));
}

// ---------------------------------------------------------------- 噪声
//
// **值噪声（value noise）三重采样**，不是 curl noise。
//
// 为什么是值噪声：粒子湍流需要的是"空间上连续、时间上可滚动、给定输入
// 完全确定"的一个矢量场。值噪声用 8 个角的哈希 + 平滑插值就能给出，
// 十几行、无纹理、无预计算，而且**天然确定**（哈希是纯函数）。
// curl noise 的好处是散度为零（不产生"源/汇"），外观更接近流体，
// 但它要多算 6 次噪声再取旋度 —— 对粒子湍流这种"看不出来"的量不划算。
// 升级路径：把 fxNoise3 换成 curl 版本即可，接口不变。
float fxHash13(vec3 p) {
    // Dave Hoskins 的 3D→1D 哈希（无 sin，精度稳定）。
    p = fract(p * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return fract((p.x + p.y) * p.z);
}

float fxNoise3(vec3 p) {
    const vec3 i = floor(p);
    const vec3 f = fract(p);
    const vec3 u = f * f * (3.0 - 2.0 * f);  // smoothstep 权重
    const float n000 = fxHash13(i + vec3(0.0, 0.0, 0.0));
    const float n100 = fxHash13(i + vec3(1.0, 0.0, 0.0));
    const float n010 = fxHash13(i + vec3(0.0, 1.0, 0.0));
    const float n110 = fxHash13(i + vec3(1.0, 1.0, 0.0));
    const float n001 = fxHash13(i + vec3(0.0, 0.0, 1.0));
    const float n101 = fxHash13(i + vec3(1.0, 0.0, 1.0));
    const float n011 = fxHash13(i + vec3(0.0, 1.0, 1.0));
    const float n111 = fxHash13(i + vec3(1.0, 1.0, 1.0));
    const float nx00 = mix(n000, n100, u.x);
    const float nx10 = mix(n010, n110, u.x);
    const float nx01 = mix(n001, n101, u.x);
    const float nx11 = mix(n011, n111, u.x);
    return mix(mix(nx00, nx10, u.y), mix(nx01, nx11, u.y), u.z);
}

// 分形叠加（fBm）：每层频率 ×2、振幅 ×0.5，归一化到 [0,1]。
// octaves 由 Emitter 给出（1..4）。
float fxNoiseFbm(vec3 p, int octaves) {
    float sum = 0.0;
    float amp = 0.5;
    float norm = 0.0;
    for (int o = 0; o < 4; ++o) {
        if (o >= octaves) break;
        sum += fxNoise3(p) * amp;
        norm += amp;
        p *= 2.02;   // 略微偏离 2.0，避免层与层的格点重合出现条纹
        amp *= 0.5;
    }
    return (norm > 0.0) ? sum / norm : 0.0;
}

// 湍流矢量场：三个轴向各取一次 fBm，中心化到 [-1,1]。
// 三次采样用不同的偏移，避免"三个轴完全相关"（那样粒子会沿对角线跑）。
vec3 fxTurbulence(vec3 p, int octaves) {
    return vec3(fxNoiseFbm(p, octaves) * 2.0 - 1.0,
                fxNoiseFbm(p + vec3(19.7, 5.3, 7.1), octaves) * 2.0 - 1.0,
                fxNoiseFbm(p + vec3(3.9, 27.4, 11.6), octaves) * 2.0 - 1.0);
}

#endif // VENN_PARTICLE_COMMON_GLSL
