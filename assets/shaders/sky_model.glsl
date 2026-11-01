// ============================================================
// sky_model.glsl —— 参数化的程序化天空（**唯一**的天空模型）
//
// ---- 它取代了什么 ----
//   以前 common.glsl 里只有一个 `vennSkyColor(dir, zenith, ground)`：
//   天顶色与地面色之间做一次平方插值。能看，但只能看 —— 没有太阳、
//   没有云、没有星，而且"改一点颜色"唯一的入口是环境变量或改源码。
//
//   现在天空是一组**可序列化的参数**（晴 / 阴雨 / 夜晚 / 黄昏四套预设
//   只是这组参数的不同取值），由 Environment 面板在运行时调，
//   跟场景一起落盘。
//
// ---- 为什么背景改成"实时算"，而不是采样烘好的立方体 ----
//   旧链路：sky_cube.comp 烘 128² 立方体 → sky.frag 只做一次 texture()。
//   改成实时算有三个理由，都跟"参数化"直接相关：
//
//     1. **改参数要立刻见效**。云边、星点是高频形状；128² 的立方体
//        在天空铺满屏幕时一个面只摊到几十个像素，云边会糊成色块。
//        提到 512² 又要 25 MB 显存 + 每次拖滑条重烘（会卡）。
//     2. **抗锯齿**。实时算按像素中心求值，云边是连续的 smoothstep；
//        立方体插值则引入一层与分辨率绑定的台阶。
//     3. **不引入"两个模型"**。旧的"背景采样立方体"之所以对，是因为
//        烘焙和背景读的是同一张图。现在烘焙（sky_cube.comp）与背景
//        （sky.frag）**调用同一个函数、吃同一份 push constant**，
//        一致性由"只有一处公式"保证，而不是由"只有一份数据"保证 ——
//        这比复制一份数据更强：数据会过期，代码不会。
//
//   ⚠ 代价：全屏 4~5 次 fbm。已按"h > 地平线才算云""星亮度为 0 时
//     整段跳过"做了早退，空场景实测开销在 0.3 ms 量级（RTX 级）。
//
// ---- 坐标系 ----
//   本引擎是 **Z-up**（地面 z = 0）。所以"朝上"是 dir.z。
//   用 dir.y 会让梯度落到世界 +Y（水平方向），地平线上会出现一条
//   笔直的明暗带 —— 这个坑 pbr.frag 的历史注释里也记着。
// ============================================================

#ifndef VENN_SKY_MODEL_GLSL
#define VENN_SKY_MODEL_GLSL

#include "common.glsl"

// ------------------------------------------------------------
// 参数块（push constant，128 字节 = 8 × vec4）
//
// 与 `engine/render/SkyParams.h` 的 `SkyParams::toGpu()` **逐字段对应**。
// 两边任何一处增删都必须同步 —— 错位不会编译报错，只会让"云的厚度"
// 悄悄变成"星星的数量"，症状是滑条互相串台。
//
// ⚠ 128 字节是本引擎管线布局给的固定额度（见 Renderer 里
//   `desc.pushConstant.size = sizeof(PushConstants)`）。想加参数先
//   合并同类项，不要直接往后追加。
// ------------------------------------------------------------
layout(push_constant) uniform VennSkyPush {
    vec4 zenith;      // rgb = 天顶色（线性 HDR）   a = 曝光
    vec4 horizon;     // rgb = 地平线色            a = 太阳视半径（弧度）
    vec4 ground;      // rgb = 地平线以下的地面色  a = 云覆盖度 0..1
    vec4 cloudColor;  // rgb = 云亮部色            a = 云厚度（不透明度）
    vec4 cloudParams; // x = 云块尺度  y = 云层高度  z = 风速  w = 云阴影强度
    vec4 starParams;  // rgb = 星色                a = 星星数量 0..1
    vec4 sunParams;   // xyz = 太阳方向（归一化）  a = 太阳强度
    vec4 misc;        // x = 星空亮度 y = 时间(s) z = 云边锐度 w = 地平线辉光
} skyPC;

// ------------------------------------------------------------
// 哈希 / 噪声
//
// 全部是纯整数位运算 + fract 的"无贴图"实现：天空不能依赖任何
// 资产文件（IBL 在 init 阶段就要能算，那时资产系统还没热起来）。
// ------------------------------------------------------------

// 2D → 1D，Dave Hoskins 的经典构造。乘数 0.1031 不是随手取的：
// 它让相邻整数格的输出去相关（用 1.0 / 7.0 这类值会出现明显的网格纹）。
float vennHash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

// 3D → 1D，星点用。同样的构造，换成三轴混合。
float vennHash13(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return fract((p.x + p.y) * p.z);
}

// 值噪声：格点随机值 + smoothstep 插值。
// 为什么不用 Perlin：值噪声的"团块感"更像二次元云的色块，
// 而且只有 4 次哈希，比梯度噪声便宜。
float vennValueNoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);   // smoothstep，一阶导连续
    float a = vennHash12(i);
    float b = vennHash12(i + vec2(1.0, 0.0));
    float c = vennHash12(i + vec2(0.0, 1.0));
    float d = vennHash12(i + vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

// 分形叠加。`p * 2.03 + offset` 里那个 2.03 和偏移量是刻意的：
// 整数倍频会让各层的格点对齐，云上会出现规则的十字纹（"形状太规则"
// 的观感来源之一）。非整数倍 + 每层平移把相位打散。
float vennFbm(vec2 p, int octaves) {
    float sum = 0.0;
    float amp = 0.5;
    float norm = 0.0;
    for (int i = 0; i < octaves; ++i) {
        sum += amp * vennValueNoise(p);
        norm += amp;
        p = p * 2.03 + vec2(17.1, 9.7);
        amp *= 0.5;
    }
    return sum / max(norm, 1e-5);
}

// ------------------------------------------------------------
// 星场
//
// 把方向量化成 3D 格点，每个格点按哈希决定"这里有星没有"。
// 星点藏在格心附近的随机偏移上，所以不会出现规则的网格分布。
//
// 多层叠加（见 vennSkyStars）比单层自然：近处的亮星稀、远处的小星密。
// ------------------------------------------------------------
float vennStarLayer(vec3 dir, float scale, float density, float t) {
    vec3 p = dir * scale;
    vec3 cell = floor(p);
    vec3 f = fract(p) - 0.5;

    float rnd = vennHash13(cell);
    if (rnd > density) return 0.0;   // 这一格里没有星

    // 星点在格内的随机位置（-0.5..0.5 之间收窄到 ±0.28，避免跨格断开）
    vec3 off = (vec3(vennHash13(cell + 1.7),
                     vennHash13(cell + 4.3),
                     vennHash13(cell + 9.1)) - 0.5) * 0.55;
    float dist = length(f - off);

    // 核心：小半径的 soft dot。平方一次让边缘更收，像"点"而不是"球"
    float core = smoothstep(0.11, 0.0, dist);
    core *= core;

    // 闪烁：每颗星相位不同，幅度小（0.65..1.0），不然整片天在抖
    float twinkle = 0.65 + 0.35 * sin(t * 2.4 + rnd * 47.0);
    return core * twinkle * (0.35 + 0.65 * rnd / max(density, 1e-3));
}

vec3 vennSkyStars(vec3 dir, vec3 tint, float amount, float t) {
    if (amount <= 0.001) return vec3(0.0);

    vec3 s = vec3(0.0);
    s += vennStarLayer(dir,  54.0, 0.30 * amount, t) * 1.00;
    s += vennStarLayer(dir, 104.0, 0.42 * amount, t + 3.1) * 0.62;
    s += vennStarLayer(dir, 205.0, 0.50 * amount, t + 7.3) * 0.34;
    return s * tint;
}

// ------------------------------------------------------------
// 云
//
// 二次元云的关键不是"像真的"，是**块状、软边、有层次**：
//   · 块状   —— 用 fbm 的阈值切割（smoothstep），不是一个连续密度场
//   · 软边   —— 切割的过渡带宽由"云边锐度"控制（0 = 柔雾，1 = 剪纸）
//   · 有层次 —— 朝太阳方向再采一次，两次的差当作"受光程度"
//
// 天穹投影：把视线方向投到一个水平面上。越靠近地平线，同一个云块
// 在屏幕上被压得越扁、拉得越长 —— 这正是"云一直铺到天边"的透视。
// 直接用 dir.xy 而不除 dir.z 的话，北边那片云会缩成一个小点。
// ------------------------------------------------------------
float vennSkyCloudDensity(vec3 dir, out vec2 planeOut) {
    // 地平线以下没有云。留 0.02 的余量避免除零
    float h = max(dir.z, 0.02);
    planeOut = dir.xy / h * skyPC.cloudParams.y;     // 天穹平面坐标
    planeOut *= skyPC.cloudParams.x * 0.0016;        // 尺度 → 可调的块大小

    // 风：只推 X。y 轴不动是因为推起来云会绕着天顶转，看着像天在转
    planeOut.x += skyPC.misc.y * skyPC.cloudParams.z;

    float n = vennFbm(planeOut, 5);

    // 覆盖度 → 阈值。0.0 时阈值 1.05（没有任何噪声能超过 → 无云），
    // 1.0 时阈值 0.10（几乎整个噪声场都在阈值以上 → 满天）。
    float cov = clamp(skyPC.ground.a, 0.0, 1.0);
    if (cov <= 0.001) return 0.0;
    float thr = mix(1.05, 0.10, cov);

    // 锐度：过渡带从 0.30（柔边，像水彩）收到 0.03（硬边，像剪纸）
    float edge = mix(0.30, 0.03, clamp(skyPC.misc.z, 0.0, 1.0));
    float dens = smoothstep(thr, thr + edge, n);

    // 靠近地平线淡出：远处的云被大气散射吞掉，硬切会露出一条锯齿边
    dens *= smoothstep(0.0, 0.09, dir.z);

    // 厚度：整体不透明度。厚度小 → 白云透出底色（薄云/卷云）
    dens *= clamp(skyPC.cloudColor.a, 0.0, 1.0);
    return dens;
}

vec3 vennSkyCloudColor(vec3 dir, vec2 plane, float dens, vec3 sunColor,
                       float sunHalo) {
    // 朝太阳方向偏移再采一次 fbm：差越大 = 这一块越"背光"
    vec3 sd = normalize(skyPC.sunParams.xyz + vec3(0.0, 0.0, 1e-5));
    vec2 toSun = normalize(sd.xy + vec2(1e-4)) * 0.09;
    float n = vennFbm(plane, 4);
    float n2 = vennFbm(plane + toSun, 4);

    // (n - n2)：朝太阳那侧更薄 → 亮点；背光侧更厚 → 暗点
    float lit = clamp(0.5 + (n - n2) * 11.0, 0.0, 1.0);

    vec3 bright = skyPC.cloudColor.rgb;
    // 背光色：往云亮部压暗，同时混一点地平线色 —— 真实云的阴影不是
    // 纯黑，是"被环境光染过的灰蓝"。混多少由"云阴影强度"控制。
    vec3 dark = mix(bright * 0.62, skyPC.horizon.rgb * 0.55,
                    clamp(skyPC.cloudParams.w, 0.0, 1.0));

    vec3 col = mix(dark, bright, lit);

    // 银边：太阳附近的云边缘被照亮。乘 dens 的补，
    // 保证只有"薄的地方"被点亮（厚的云中心不该透光）
    col += sunColor * sunHalo * 0.55 * (1.0 - dens) * dens * 4.0;
    return col;
}

// ------------------------------------------------------------
// 主函数：给定视线方向，返回天空辐亮度（线性 HDR）
//
// 分段（从下往上）：
//   地平线以下  → ground 色，向下压暗（模拟远处地面/雾）
//   地平线附近  → ground → horizon 过渡
//   上半球      → horizon → zenith 过渡
// 三段用两次 smoothstep 拼，而不是一次 mix —— 一次 mix 的话
// "地平线那一圈偏白"这个二次元天空最标志性的特征做不出来。
// ------------------------------------------------------------
vec3 vennSkyRadiance(vec3 dirIn) {
    vec3 d = normalize(dirIn);

    // t: 0 = 天底, 0.5 = 地平线, 1 = 天顶（与旧 vennSkyColor 的映射一致）
    float t = clamp(d.z * 0.5 + 0.5, 0.0, 1.0);

    // ---------- 1) 底色 ----------
    // 上段的过渡终点是 0.64（= 仰角约 8°）而不是 0.95：
    // 二次元天空的地平线本来就白，但"白"只该占贴地那一条窄带。
    // 终点给大时，盯着一片浅灰白算线性值会觉得"0.29 已经够蓝了"，
    // 但那个值经 sRGB 编码后是 0.57 —— 屏幕上是浅蓝白，不是蓝天。
    // 想让仰角 15° 处就明显发蓝，过渡必须在 10° 之前走完。
    vec3 lower = mix(skyPC.ground.rgb, skyPC.horizon.rgb,
                     smoothstep(0.08, 0.50, t));
    vec3 upper = mix(skyPC.horizon.rgb, skyPC.zenith.rgb,
                     smoothstep(0.50, 0.64, t));
    vec3 col = t < 0.5 ? lower : upper;

    // 地平线以下再压暗一档：天空球下半部不该和地面一样亮，
    // 否则仰视时"地平线"这条线会消失
    col *= mix(1.0, 0.42, smoothstep(0.0, -0.45, d.z));
    col *= max(skyPC.zenith.a, 0.0);   // 曝光

    // ---------- 2) 太阳 ----------
    // 方向可能没归一化（面板给的是欧拉角转出来的），这里兜一次
    vec3 sd = normalize(skyPC.sunParams.xyz + vec3(0.0, 0.0, 1e-5));
    float mu = dot(d, sd);
    float radius = max(skyPC.horizon.a, 0.002);   // 弧度
    // 太阳盘：mu 从 cos(r) 到 cos(0.82r) 之间从 0 收到 1 →
    // 圆盘边缘是柔的（二次元太阳也不该有锯齿）
    float disk = smoothstep(cos(radius), cos(radius * 0.82), mu);
    // 光晕：两个指数项叠 —— 窄的那层是日冕，宽的那层是整片天被照亮的氛围
    float halo = pow(max(mu, 0.0), 12.0) * 0.55
               + pow(max(mu, 0.0), 3.0) * 0.14;
    halo *= max(skyPC.misc.w, 0.0);

    // 太阳颜色：暖白，再往地平线色偏一点 —— 黄昏时地平线是橙的，
    // 太阳自然跟着橙，不需要额外一个"太阳色"参数（128 字节装不下）
    vec3 sunTint = mix(vec3(1.0, 0.96, 0.88), skyPC.horizon.rgb, 0.35);
    float sunAmt = max(skyPC.sunParams.a, 0.0);
    col += sunTint * sunAmt * (disk * 18.0 + halo);

    // ---------- 3) 云 ----------
    // 只在有覆盖度时走完整路径（fbm 是这条 shader 里最贵的部分）
    if (skyPC.ground.a > 0.001 && d.z > -0.02) {
        vec2 plane;
        float dens = vennSkyCloudDensity(d, plane);
        if (dens > 0.001) {
            vec3 cc = vennSkyCloudColor(d, plane, dens, sunTint, halo);
            col = mix(col, cc, dens);
        }
    }

    // ---------- 4) 星空 ----------
    // 星空亮度为 0（白天）时整段跳过 —— 三次多层哈希不是免费的
    float starAmt = clamp(skyPC.misc.x, 0.0, 1.0);
    if (starAmt > 0.001 && d.z > -0.05) {
        vec3 st = vennSkyStars(d, skyPC.starParams.rgb,
                               clamp(skyPC.starParams.a, 0.0, 1.0),
                               skyPC.misc.y);
        // 地平线附近淡出：贴地的那圈星星会被"地面"挡住
        st *= smoothstep(-0.02, 0.16, d.z);
        col += st * starAmt;
    }

    return max(col, vec3(0.0));
}

#endif // VENN_SKY_MODEL_GLSL
