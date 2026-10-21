#version 450
// ============================================================
// water.frag —— 水面片元着色器（2026-10-21）
//
// 这一份片元是"用户要的真实细腻感"的全部所在，四段依次是：
//
//   1) 法线：4 项涌浪的解析梯度 + 两层细波纹噪声（顺流向拉长）
//   2) 折射：采样**本帧已画完的场景颜色**（带偏移），并用深度做有效性
//      校验 —— 偏移把前景物体拉进水面是这类着色器最典型的穿帮
//   3) 吸收：Beer–Lambert，按"垂直水深"把场景色推向深水色。
//      clarity 越高的水吸收越弱 → 浅处能看见底
//   4) 反射 + 高光 + 泡沫：菲涅尔混合天空立方体，Blinn-Phong 高光，
//      岸边（水浅）与浪尖（波形高）加噪声调制的白沫
//
// ------------------------------------------------------------
// 为什么水的深度测试是**手写**的
// ------------------------------------------------------------
// 这条通道没有深度附件（原因见 WaterPass.h 的文件头），所以"水面被
// 前景物体挡住"必须在这里自己判：从深度图重建场景世界坐标，比较
// 它与水面片元到相机的距离，场景更近就 discard。这与硬件深度测试
// 等价，只是要多一次采样 —— 换来的是"能在同一条通道里采样颜色"。
//
// ------------------------------------------------------------
// MSAA 深度：编两份
// ------------------------------------------------------------
// 深度图在多采样时只能用 sampler2DMS + texelFetch（见 space.glsl 顶部）。
// CMakeLists 的 SHADER_MSAA_DEPTH_SHADERS 里列了 water.frag，于是
//   water.frag.spv       单采样
//   water.msaa.frag.spv  多采样
// 运行期由 WaterPass 按 HdrTarget::samples() 选。
// ============================================================

#include "space.glsl"
#include "water_common.glsl"

layout(set = 0, binding = 0) uniform FrameUBO {
    mat4 view;
    mat4 proj;
    vec4 lightDir;
    vec4 lightColor;
    vec4 camPos;
    mat4 lightSpace[4];
    vec4 cameraParams;  // x=zNear y=zFar
    vec4 clusterDims;
    vec4 screenSize;    // x=width y=height z=1/width w=1/height
    vec4 lightParams;
    mat4 invViewProj;
} ubo;

layout(set = 0, binding = 10) uniform samplerCube skyMap;

layout(set = 1, binding = 0) uniform WaterUbo {
    vec4 shallow;  // rgb = 浅水色, w = opacity
    vec4 deep;     // rgb = 深水色, w = clarity
    vec4 p0;       // x=level y=waveHeight z=waveLength w=flowSpeed
    vec4 p1;       // x=flowDir.x y=flowDir.y z=ripple w=foam
    vec4 p2;       // x=refraction y=ior z=roughness w=time
    vec4 p3;       // x=kind y=shape z=halfX w=halfZ
    vec4 p4;       // x=bedLevel y=skirtBottom(world z)
    vec4 screen;   // x=width y=height z=1/width w=1/height
    vec4 camera;   // x=zNear y=zFar
} wub;

// 本帧已画完的不透明 + 透明场景色（水面之前的一次拷贝）
layout(set = 1, binding = 1) uniform sampler2D sceneColor;

// 场景深度（多采样时是 sampler2DMS，见 space.glsl）
#if VENN_MSAA_DEPTH
layout(set = 1, binding = 2) uniform sampler2DMS sceneDepth;
#else
layout(set = 1, binding = 2) uniform sampler2D sceneDepth;
#endif

layout(location = 0) in vec3 vWorld;
layout(location = 1) in vec3 vNrm;
layout(location = 2) in float vPart;

layout(location = 0) out vec4 outColor;

vec3 worldPosFromDepth(vec2 uv, float depth) {
    vec4 w = ubo.invViewProj * vec4(uv * 2.0 - 1.0, depth, 1.0);
    return w.xyz / w.w;
}

// ------------------------------------------------------------
// 侧壁（水体的截面）
// ------------------------------------------------------------
// 用户最直接的抱怨是"水体只有一个平面"。平面之所以看起来像纸，是因为
// 它的边界没有厚度：从低角度看过去，水面边缘直接断在半空。
//
// 侧壁补的就是这层厚度。它不做折射（折射是"透过去看水底"的事，而侧壁
// 背后是土），做的是三件更朴素的事：
//   ① Beer–Lambert：这个像素离水面多深就有多暗 —— 这一条让水体读起来
//      是"一汪水"而不是"一块色卡"；
//   ② 菲涅尔 + 天空：垂直面的反射比水平面弱，但掠射时同样会亮起来；
//   ③ 水线附近一条被噪声打散的亮边 + 沿水深流动的细纹。
vec3 shadeSkirt(vec3 P, vec3 N, float t) {
    const vec3 V = normalize(ubo.camPos.xyz - P);
    const float depthBelow = max(wub.p0.x - P.z, 0.0);
    const float absorb =
        1.0 - exp(-depthBelow * 0.65 / max(wub.deep.w, 0.02));
    vec3 body = mix(wub.shallow.rgb, wub.deep.rgb,
                    smoothstep(0.0, 1.0, absorb));
    body *= 0.82;   // 截面比水面暗一档：它后面是土，不是天空

    const float ior = clamp(wub.p2.y, 1.0, 2.5);
    const float f0 = pow((ior - 1.0) / (ior + 1.0), 2.0);
    const float fres =
        f0 + (1.0 - f0) * pow(1.0 - clamp(dot(N, V), 0.0, 1.0), 5.0);
    const vec3 R = reflect(-V, N);
    const vec3 skyDir = normalize(vec3(R.x, R.y, max(R.z, 0.04)));
    const vec3 refl = texture(skyMap, skyDir).rgb;
    vec3 col = mix(body, refl, clamp(fres * 0.6, 0.0, 1.0));

    // 沿水深的细纹：一个轴顺水平走向、一个轴是深度 —— 像水下的纹理柱，
    // 让垂直截面不至于是一块纯色
    const float along = dot(P.xy, normalize(vec2(1.0, 0.35)));
    const float n =
        vennValueNoise(vec2(along * 1.7, depthBelow * 3.2 - t * 0.35));
    col *= 0.86 + 0.28 * n;

    // 水线：水面以下很短的一段提亮 + 泡沫，与水面上的岸边泡沫接得上
    const float band = 1.0 - smoothstep(0.0, 0.45, depthBelow);
    col = mix(col, vec3(0.86, 0.93, 0.96),
              band * 0.55 * clamp(wub.p1.w, 0.0, 1.0));
    return col;
}

void main() {
    const vec2 uv = gl_FragCoord.xy * wub.screen.zw;

    // ---- 0) 手写深度测试：场景里有东西挡在水面前面就整片丢掉 ----
    const float sceneRawDepth = vennSceneDepth(sceneDepth, uv);
    const vec3 sceneW = worldPosFromDepth(uv, sceneRawDepth);
    const vec3 camPos = ubo.camPos.xyz;
    const float sceneDist = length(sceneW - camPos);
    const float waterDist = length(vWorld - camPos);
    // 0.03 m 的偏置：水面与地形在岸边**几乎共面**，不偏置会跟地形打架
    //（表现为沿水线一圈闪烁的麻点）。
    if (sceneDist < waterDist - 0.03) discard;

    // ---- 侧壁：走另一条更简单的着色路径（不折射、不做高光）----
    if (vPart > 0.5) {
        outColor = vec4(shadeSkirt(vWorld, normalize(vNrm), wub.p2.w), 1.0);
        return;
    }

    // ---- 1) 法线：涌浪解析梯度 + 细波纹噪声 ----
    const float t = wub.p2.w;
    const vec2 xz = vWorld.xy;

    float h;
    vec2 grad;
    vennWaterWaves(xz, t, wub.p0.y, wub.p0.z, 0.0, h, grad);

    // 流向：归一化后的方向 + 垂直于它的分量。河水的条纹就是"沿流向
    // 把噪声坐标拉长"得到的 —— 不需要额外的贴图。
    vec2 fdir = wub.p1.xy;
    const float flen = length(fdir);
    fdir = (flen > 1e-4) ? (fdir / flen) : vec2(1.0, 0.0);
    const vec2 fperp = vec2(-fdir.y, fdir.x);
    const float flowSpeed = wub.p0.w;

    // 把 xz 投到 (顺流, 垂直) 基底，顺流方向压扁 4 倍 → 细长条纹
    vec2 sp = vec2(dot(xz, fdir) * 0.3, dot(xz, fperp));
    sp.x -= flowSpeed * t * 0.9;

    const float rip = wub.p1.z;
    vec2 ng = vec2(0.0);
    // 三层：被流向拉长的一层（水流感）+ 各向同性短波（涟漪）+ 更细的一层
    //（高频细纹）。第三层是"细腻感"的关键：只有两层时掠射角下水面会读成
    // 一块光滑的塑料，加一层高频就够让高光碎起来。
    ng += vennNoiseGrad(sp * 1.6, 0.06, 0.020);
    ng += vennNoiseGrad(xz * 2.4 + vec2(t * 0.11, -t * 0.07), 0.06, 0.012);
    ng += vennNoiseGrad(xz * 6.8 + vec2(-t * 0.19, t * 0.13), 0.05, 0.007);
    grad += ng * rip * 3.0;

    vec3 N = normalize(vec3(-grad.x, -grad.y, 1.0));

    // ---- 2) 折射：本帧场景色 + 偏移，并校验偏移后的样本是否合法 ----
    const vec3 V = normalize(camPos - vWorld);

    // 水深（垂直方向）：水面高度 - 场景表面高度。水面之上（天空 /
    // 岸边高处的物体）夹到 0，那时水是"薄薄一层"、几乎全透。
    const float waterDepth = max(wub.p0.x - sceneW.z, 0.0);

    // 偏移量随水深衰减：浅水处拧得厉害（能看到水底的砂石位移），
    // 深水处几乎没有可参照的东西，拧太多只会让画面抖。
    const float distort = wub.p2.x * 0.06 /
                          (1.0 + waterDepth * 0.55);
    vec2 refUv = uv + N.xy * distort;
    refUv = clamp(refUv, vec2(0.0015), vec2(0.9985));

    // 有效性校验：如果偏移后的位置采到的是**比水面更近**的几何，
    // 说明我们把前景物体的像素拖进了水面（典型穿帮：岸边石头被"吸"
    // 进水里的扭曲）。这时退回不偏移的样本。
    const float refRawDepth = vennSceneDepth(sceneDepth, refUv);
    const vec3 refSceneW = worldPosFromDepth(refUv, refRawDepth);
    if (length(refSceneW - camPos) < waterDist - 0.05) {
        refUv = uv;
    }
    vec3 refracted = texture(sceneColor, refUv).rgb;

    // ---- 3) 吸收：越深越看不见底，颜色推向 deep ----
    // 0.65 = 清水的基准消光（1/米 量级）。clarity 是用户旋钮：
    // 山泉 3.2（几乎不吸收）→ 池塘 0.45（2 m 就浑）
    const float absorb =
        1.0 - exp(-waterDepth * 0.65 / max(wub.deep.w, 0.02));
    const vec3 body = mix(wub.shallow.rgb, wub.deep.rgb,
                          smoothstep(0.0, 1.0, absorb));
    // 混合权重：吸收（深度）+ 表观浑浊（opacity）。opacity 的系数压到 0.5 ——
    // 它原本是 1.0，等于"只要水体不是完全清透就直接糊成一块色卡"，
    // 2 m 深的湖看不到底（2026-10-21 实测：碗形池塘整片都是一块青蓝，
    // 一点水底都不透）。0.5 让它仍能表达"浑浊"，但不至于压掉折射。
    const float bodyMix = clamp(absorb * 0.92 + wub.shallow.w * 0.5, 0.0, 1.0);
    vec3 col = mix(refracted, body, bodyMix);

    // ---- 4) 菲涅尔反射 ----
    // F0 由折射率算，水 1.333 → 0.02。看水面越斜，反射越强（这就是
    // "远看整片水是天空的颜色、低头才看得见水底"的来源）。
    const float ior = clamp(wub.p2.y, 1.0, 2.5);
    const float f0 = pow((ior - 1.0) / (ior + 1.0), 2.0);
    const float fres = f0 + (1.0 - f0) * pow(1.0 - clamp(dot(N, V), 0.0, 1.0),
                                             5.0);
    const vec3 R = reflect(-V, N);
    // 只反射"水面以上"的方向；朝下的分量（不可能从天空来）夹到水平
    vec3 skyDir = vec3(R.x, R.y, max(R.z, 0.04));
    const vec3 refl = texture(skyMap, normalize(skyDir)).rgb;
    // 反射**带一点水色**：真实湖面反射天空时总被一层体色压过。纯镜面反射
    // 一片灰色天空会让整个水面读成"结冰"——2026-10-21 实测，掠射角下
    // 25 m 的池塘边缘是一大圈白，完全不像水。0.28 是"能看出来、又不假"的量。
    const vec3 reflTint = mix(vec3(1.0), wub.shallow.rgb * 1.8, 0.28);
    col = mix(col, refl * reflTint, clamp(fres, 0.0, 0.92));

    // ---- 5) 高光（Blinn-Phong，roughness 调锐度）----
    const vec3 L = normalize(-ubo.lightDir.xyz);
    const vec3 H = normalize(L + V);
    const float rough = clamp(wub.p2.z, 0.0, 1.0);
    const float shininess = mix(2400.0, 24.0, rough);
    const float specPow = pow(max(dot(N, H), 0.0), shininess);
    // 太阳的辐射强度在 lightDir.w 里（与 pbr.frag 的 radiance 同源）
    col += ubo.lightColor.rgb * ubo.lightDir.w * specPow *
           (1.2 - rough * 0.8);

    // ---- 6) 泡沫：岸边（水浅）+ 浪尖（陡到会破碎的浪），都用噪声打散 ----
    // 岸线泡沫的宽度要克制：1.5 m 的过渡带在缓坡池塘上会盖掉大半个水面
    // （碗形地形的边缘就是缓坡），看起来像结冰而不是水。0.25 m 只够勾出
    // 一条水线 —— 剩下的"浅水变亮"交给吸收去做，那才是物理的。
    const float shore = 1.0 - smoothstep(0.0, 0.10, waterDepth);
    // 白浪不是"浪有多高"，而是"浪有多陡" —— 深水重力波的破碎阈值是
    // 陡度 A/λ ≈ 1/7 ≈ 0.14（实际海面在 0.02~0.03 就开始起白沫）。
    // 只看相对波高的写法会让**平静的湖/池塘**也长出一片白斑（2026-10-21
    // 实测：湖面 preset 下四团白斑糊在中间，非常假）。
    const float steep = wub.p0.y / max(wub.p0.z, 0.05);
    const float whitecap = smoothstep(0.02, 0.075, steep);
    const float crest = smoothstep(0.55, 1.0,
                                   h / max(wub.p0.y * 0.85, 1e-4)) * whitecap;
    // 岸线泡沫要**斑驳**，不能是一条连续的白带：缓坡池塘上"水深 0~0.1 m"
    // 那圈范围有好几米宽，连成一片就是结了冰的边（2026-10-21 实测）。
    // 用一个阈值化的噪声把这条带打成碎块，只在噪声的高处留泡沫。
    const float lace = smoothstep(0.40, 0.72,
        vennValueNoise(xz * 2.6 + vec2(t * 0.22, -t * 0.15)));
    float foam = (shore * 0.30 * lace + crest * 0.50) *
                 clamp(wub.p1.w, 0.0, 1.0);
    // 噪声调制：没有它，岸边就是一条硬邦邦的白边
    foam *= 0.45 + 0.70 * vennValueNoise(xz * 2.2 + vec2(t * 0.25, t * 0.18));
    foam = clamp(foam, 0.0, 0.9);
    col = mix(col, vec3(0.86, 0.92, 0.94), foam);

    outColor = vec4(col, 1.0);
}
