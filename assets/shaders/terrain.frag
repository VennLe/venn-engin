#version 450
// ============================================================
// terrain.frag —— 地形片元着色器（W4-2 / D5）
//
//   set 0 / binding 0  : FrameUBO
//   set 0 / binding 1  : 级联阴影图（与 pbr.frag 同一张）
//   set 0 / binding 7  : irradianceMap（IBL 漫反射，用作环境光）
//   set 0 / binding 10 : skyMap（雾色，保证地平线不出现断层）
//   set 1 / binding 0  : albedo[4]
//   set 1 / binding 1  : normal[4]（切线空间）
//   set 1 / binding 2  : splatMap（RGBA8 = 4 层权重）
//   set 1 / binding 3  : heightMap（R32_SFLOAT，顶点阶段也用）
//   set 1 / binding 4  : 层参数 UBO（baseColor / roughness / metallic / ao）
//
// ------------------------------------------------------------
// 为什么要独立管线（D5）
// ------------------------------------------------------------
// 前向管线的纹理槽是 7 个（`kTextureSetCount`），已经满了。地形 4 层
// 需要 4 albedo + 4 normal + 1 权重 + 1 高度 = 10 张，只能走**自己的**
// set 布局。本机 `maxBoundDescriptorSets = 32`，加到 9 个 set 技术上可行，
// 但低端设备只有 8 —— 而 8 是本引擎已经踩在线的实际要求（见
// RenderTypes.h 的 kTextureSetCount 注释）。所以：
//   · 权重打包成**一张** RGBA8（4 层各 1 通道）
//   · 4 层材质走**这一份** set 1，一次绑定画完整块地形
//
// ------------------------------------------------------------
// 混合规则
// ------------------------------------------------------------
//   albedo = Σ w_i × albedo_i
//   normal = normalize(Σ w_i × normal_i)
// **不能**只取权重最大的那一层：单层硬切在权重过渡带上是一条看得见的
// 硬接缝（"草皮和石头之间像刀切的"）。加权和才是连续过渡。
// ============================================================

layout(set = 0, binding = 0) uniform FrameUBO {
    mat4 view;
    mat4 proj;
    vec4 lightDir;
    vec4 lightColor;
    vec4 camPos;
    mat4 lightSpace[4];
    vec4 cameraParams;
    vec4 clusterDims;
    vec4 screenSize;
    vec4 lightParams;
    mat4 invViewProj;
    vec4 envParams;
    vec4 shadowParams;
    vec4 cascadeSplits;
    vec4 shadowBias;
    vec4 localShadowParams;  // 地形不用（局部光不投影到地形上），
                             //   但**必须声明**：要读到 fogParams / shadowFilter
                             //   就必须逐字段占位，否则偏移整体错位。
    vec4 fogParams;
    vec4 viewModeParams;
    vec4 shadowFilter;       // x=级联混合带宽 y=接触硬化强度
} ubo;

layout(set = 0, binding = 1) uniform sampler2DArrayShadow shadowMap;
layout(set = 0, binding = 7) uniform samplerCube irradianceMap;
layout(set = 0, binding = 10) uniform samplerCube skyMap;

layout(set = 1, binding = 0) uniform sampler2D albedoLayer[4];
layout(set = 1, binding = 1) uniform sampler2D normalLayer[4];
layout(set = 1, binding = 2) uniform sampler2D splatMap;
layout(set = 1, binding = 3) uniform sampler2D heightMap;
// 挖洞遮罩（R8）：1 = 实心，0 = 挖穿。见 main() 里的 discard。
layout(set = 1, binding = 5) uniform sampler2D holeMap;

struct TerrainLayer {
    vec4 baseColor;  // rgb = 基色（线性），a = 未用
    vec4 params;     // x=roughness y=metallic z=ao w=normalScale
};
layout(set = 1, binding = 4) uniform LayerBlock {
    TerrainLayer layers[4];
} layerBlock;

layout(push_constant) uniform Push {
    mat4 model;
    vec4 params;   // x=maxHeight y=splatRepeat z=layerTiling w=metersPerPixel
    vec4 params2;  // x=cols y=rows z=skirtDepth w=层 normal 贴图位掩码
} push;

layout(location = 0) in vec3 vWorld;
layout(location = 1) in vec2 vUv;

layout(location = 0) out vec4 outColor;

const float kPi = 3.14159265359;

// ------------------------------------------------------------
// 级联阴影
// ------------------------------------------------------------
// ⚠ 这三段必须与 pbr.frag 的 selectCascade / pcfCascade / shadowFactor
//   **逐行一致**：同一张影子图、同一套偏置约定（光源投影是 Zero-to-One，
//   所以 proj.z 不能再 *0.5+0.5）、同一套距离淡出。任何一处漂移，地形上
//   的阴影就会比站在它上面的木箱的阴影"错开半级"或"突然消失一片"。
//   （没有抽成共享 .glsl 是因为 pbr.frag 属于横切文件，本轮不动它。）
int selectCascade(float viewDepth) {
    const int n = int(ubo.shadowParams.x);
    const int last = max(n - 1, 0);
    int c = last;
    for (int i = 0; i < 4; ++i) {
        if (i >= n) break;
        if (viewDepth <= ubo.cascadeSplits[i]) {
            c = i;
            break;
        }
    }
    return c;
}

float pcfCascade(vec3 proj, int cascade, float bias, float radiusScale) {
    const ivec3 size = textureSize(shadowMap, 0);
    const vec2 texel = 1.0 / vec2(size.xy);
    const float radius = ubo.shadowBias.z *
                         (1.0 + ubo.shadowBias.w * float(cascade)) *
                         radiusScale;
    float sum = 0.0;
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            sum += texture(shadowMap,
                           vec4(proj.xy + vec2(x, y) * texel * radius,
                                float(cascade), proj.z - bias));
        }
    }
    return sum / 9.0;
}

float sampleCascade(vec3 worldPos, int cascade, float NdotL,
                    float radiusScale) {
    const vec4 lsp = ubo.lightSpace[cascade] * vec4(worldPos, 1.0);
    vec3 proj = lsp.xyz / lsp.w;
    proj.xy = proj.xy * 0.5 + 0.5;  // z 已是 [0,1]，不能再压
    if (proj.z > 1.0 || proj.z < 0.0 || proj.x < 0.0 || proj.x > 1.0 ||
        proj.y < 0.0 || proj.y > 1.0) {
        return 1.0;
    }
    const float bias = max(ubo.shadowBias.y * (1.0 - NdotL), ubo.shadowBias.x);
    return pcfCascade(proj, cascade, bias, radiusScale);
}

float shadowFactor(vec3 worldPos, float NdotL) {
    if (ubo.shadowParams.w < 0.5) return 1.0;
    const float viewDepth = -(ubo.view * vec4(worldPos, 1.0)).z;
    const float maxDist = ubo.shadowParams.y;
    if (viewDepth > maxDist) return 1.0;
    const float fade = 1.0 - smoothstep(maxDist * ubo.shadowParams.z, maxDist,
                                        viewDepth);

    const int cascade = selectCascade(viewDepth);
    const int n = int(ubo.shadowParams.x);

    // 接触硬化（与 pbr.frag 同一条公式：接收点在本级覆盖跨度内的相对位置
    // 越大，PCF 半径越大 → 阴影越软）。强度为 0 时 radiusScale 恒 1。
    float radiusScale = 1.0;
    if (ubo.shadowFilter.y > 0.001) {
        const float splitEnd = cascade < 3 ? ubo.cascadeSplits[cascade]
                                           : maxDist;
        const float t = clamp(viewDepth / max(splitEnd, 1e-3), 0.0, 1.0);
        radiusScale = mix(1.0, 1.0 + ubo.shadowFilter.y * 4.0, t);
    }

    float sh = sampleCascade(worldPos, cascade, NdotL, radiusScale);

    // 级联间混合带（R3-1 之①）。**地形上比物体上更重要**：地形是连成
    // 一片的地面，级联交界那条亮线会横穿整个画面；物体上它只是一闪而过。
    const float band = ubo.shadowFilter.x;
    if (band > 0.001 && cascade + 1 < n) {
        const float splitEnd = ubo.cascadeSplits[cascade];
        const float splitPrev = cascade > 0 ? ubo.cascadeSplits[cascade - 1] : 0.0;
        const float span = max(splitEnd - splitPrev, 1e-3);
        const float bandLo = splitEnd - span * band;
        if (viewDepth > bandLo) {
            const float t = clamp((viewDepth - bandLo) / (span * band), 0.0, 1.0);
            const float shNext = sampleCascade(worldPos, cascade + 1, NdotL,
                                               radiusScale);
            sh = mix(sh, shNext, t);
        }
    }

    return mix(1.0, sh, fade);
}

// ------------------------------------------------------------
// 法线：对高度图做中心差分
// ------------------------------------------------------------
// ⚠ 与 `physics::normalAt` 用的是**同一个算子、同一个步长**（半像素）。
//   两边不一致 → "看起来的坡度"和"物理判定的坡度"打架：明明看着能站，
//   角色却往上滑。逐顶点算也不行（LOD3 只有 4×4 格，法线会一块块突变），
//   所以放在片元里算 —— 每个像素的坡度都是精确的，与 LOD 无关。
vec3 terrainNormal(vec2 uv) {
    const vec2 texel = 1.0 / vec2(push.params2.xy);   // cols, rows
    const vec2 half_ = texel * 0.5;
    const float maxH = push.params.x;
    const float hL = texture(heightMap, uv - vec2(half_.x, 0.0)).r * maxH;
    const float hR = texture(heightMap, uv + vec2(half_.x, 0.0)).r * maxH;
    const float hD = texture(heightMap, uv - vec2(0.0, half_.y)).r * maxH;
    const float hU = texture(heightMap, uv + vec2(0.0, half_.y)).r * maxH;
    // uv 走半个纹素 = 世界走 0.5 × metersPerPixel
    const float d = max(push.params.w * 0.5, 1e-4);
    return normalize(vec3(-(hR - hL) / (2.0 * d), -(hU - hD) / (2.0 * d), 1.0));
}

void main() {
    // ---- 挖洞（UE 的 Landscape Visibility 层）----
    // 放在**所有计算之前**：挖穿的像素连贴图都不该采（被挖掉的格子下面
    // 通常什么都没有，白白算一遍 4 层 albedo + 阴影再丢掉）。
    //
    // ⚠ 用 discard 而不是把顶点压到地下：
    //   · 顶点级方案会在洞的边缘留下"被拉成尖刺的三角形"（高度图是
    //     双线性插值的，一半顶点的洞会把相邻三角形拽成斜面）；
    //   · discard 是按像素的，洞边正好落在遮罩的插值边界上 —— 视觉上
    //     就是 UE 那种"边缘略带抗锯齿的洞"。
    // ⚠ 副作用（与 UE 一致，不是 bug）：被挖掉的部分**不写深度**，
    //   所以洞下面的物体会露出来；同时它也不再遮挡阴影（地形本来就不投
    //   阴影，这一条暂时无所谓）。
    const float solid = texture(holeMap, vUv).r;
    if (solid < 0.5) discard;

    const vec4 rawW = texture(splatMap, vUv * push.params.y);
    // 权重之和必须归一化到 1。CPU 侧（tools/gen_terrain.py）已经把四路
    // 量化成和恰好 255，这里再兜一次：手改过的权重图（或将来某个编辑器
    // 笔刷）没归一化时，地形会整体偏亮/偏暗，而那种偏差很难归因。
    vec4 w = rawW;
    const float wsum = dot(w, vec4(1.0));
    if (wsum > 1e-4) w /= wsum; else w = vec4(1.0, 0.0, 0.0, 0.0);

    // ---- albedo：4 层加权和 ----
    // 每层按**世界坐标**平铺（layerTiling 米一次重复），所以地块大小变化
    // 不会拉花贴图，也不会因为 LOD 变了而让纹理跳动。
    const vec2 layerUv = vWorld.xy / max(push.params.z, 0.01);
    vec3 albedo = vec3(0.0);
    float roughness = 0.0;
    float metallic = 0.0;
    float ao = 0.0;
    albedo += w.x * layerBlock.layers[0].baseColor.rgb *
              texture(albedoLayer[0], layerUv).rgb;
    albedo += w.y * layerBlock.layers[1].baseColor.rgb *
              texture(albedoLayer[1], layerUv).rgb;
    albedo += w.z * layerBlock.layers[2].baseColor.rgb *
              texture(albedoLayer[2], layerUv).rgb;
    albedo += w.w * layerBlock.layers[3].baseColor.rgb *
              texture(albedoLayer[3], layerUv).rgb;
    roughness = w.x * layerBlock.layers[0].params.x +
                w.y * layerBlock.layers[1].params.x +
                w.z * layerBlock.layers[2].params.x +
                w.w * layerBlock.layers[3].params.x;
    metallic = w.x * layerBlock.layers[0].params.y +
               w.y * layerBlock.layers[1].params.y +
               w.z * layerBlock.layers[2].params.y +
               w.w * layerBlock.layers[3].params.y;
    ao = w.x * layerBlock.layers[0].params.z +
         w.y * layerBlock.layers[1].params.z +
         w.z * layerBlock.layers[2].params.z +
         w.w * layerBlock.layers[3].params.z;

    // ---- 法线 ----
    vec3 N = terrainNormal(vUv);
    // 层 normal 贴图：只有真的挂了图的层才参与（位掩码来自 CPU），
    // 全都没挂时这一整段跳过 —— 当前 4 个 .vmat 都只给了 baseColor，
    // 所以这里是零成本。加权和之后再做一次归一化（D5 明确要求）。
    const int nmask = int(push.params2.w);
    if (nmask != 0) {
        vec3 tangentNormal = vec3(0.0);
        for (int i = 0; i < 4; ++i) {
            if ((nmask & (1 << i)) == 0) continue;
            const vec3 t = texture(normalLayer[i], layerUv).xyz * 2.0 - 1.0;
            const float scale = layerBlock.layers[i].params.w;
            tangentNormal += w[i] * normalize(vec3(t.xy * scale, max(t.z, 1e-3)));
        }
        // 高度场的切线基：+X / +Y 与世界轴一致，只需对 N 正交化
        vec3 T = normalize(vec3(1.0, 0.0, 0.0) - N * N.x);
        vec3 B = cross(N, T);
        const vec3 tn = length(tangentNormal) > 1e-4
                            ? normalize(tangentNormal)
                            : vec3(0.0, 0.0, 1.0);
        N = normalize(normalize(T * tn.x + B * tn.y + N * tn.z));
    }

    // ---- 直接光（方向光 + 阴影）----
    const vec3 V = normalize(ubo.camPos.xyz - vWorld);
    const vec3 L = normalize(-ubo.lightDir.xyz);
    const vec3 H = normalize(V + L);
    const float NdotL = max(dot(N, L), 0.0);
    const float NdotV = max(dot(N, V), 1e-4);
    const vec3 radiance = ubo.lightColor.rgb * ubo.lightDir.w;

    const vec3 F0 = mix(vec3(0.04), albedo, metallic);
    const vec3 F = F0 + (vec3(1.0) - F0) * pow(clamp(1.0 - max(dot(H, V), 0.0),
                                                    0.0, 1.0), 5.0);
    // 地形基本都是粗糙表面：用"GGX 形状 + Schlick 菲涅尔"的简化镜面，
    // 不用完整 split-sum（那需要 brdfLut，而地形没有逐层粗糙度贴图）。
    const float a = max(roughness * roughness, 1e-3);
    const float dGGX = a * a /
        (kPi * pow(max(dot(N, H) * dot(N, H) * (a * a - 1.0) + 1.0, 1e-4), 2.0));
    const float k = a * 0.5;
    const float gSmith = (NdotL / (NdotL * (1.0 - k) + k)) *
                         (NdotV / (NdotV * (1.0 - k) + k));
    const vec3 specular = (dGGX * gSmith * F) / max(4.0 * NdotV * NdotL, 1e-4);
    const vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);

    const float shadow = shadowFactor(vWorld, NdotL);
    vec3 color = (kD * albedo / kPi + specular) * radiance * NdotL * shadow;

    // ---- 环境光 ----
    // IBL 开着就采 irradianceMap（与 pbr.frag 的漫反射通道同一张图），
    // 关着时退化成天空渐变近似 —— 两条路径都要能看，且开关两次画面必须
    // 明显不同（这就是 verify_terrain 里"关 IBL"对照组的依据）。
    const float ambientScale = ubo.lightParams.y;
    vec3 ambient;
    if (ubo.envParams.x > 0.5) {
        ambient = texture(irradianceMap, N).rgb * albedo * ao * ambientScale;
    } else {
        const vec3 up = vec3(0.32, 0.40, 0.52);
        const vec3 down = vec3(0.11, 0.12, 0.13);
        ambient = mix(down, up, N.z * 0.5 + 0.5) * albedo * ao * ambientScale;
    }
    color += ambient;

    // ---- 高度雾（与 pbr.frag 同一条公式，同一个雾色来源）----
    // 少了这一段，远处的地形会比远处的木箱"晚消失"，地平线上出现一条
    // 清晰的山脊 —— 与天空盒的接缝非常显眼。
    if (ubo.fogParams.z > 0.5) {
        const float dist = distance(ubo.camPos.xyz, vWorld);
        const float h = max(vWorld.z, 0.0);
        const float fogAmount = dist * ubo.fogParams.x * exp(-ubo.fogParams.y * h);
        const float fogFactor = clamp(1.0 - exp(-fogAmount), 0.0, 1.0);
        const vec3 vdir = normalize(vWorld - ubo.camPos.xyz);
        const vec3 fogColor = (ubo.envParams.x > 0.5)
            ? texture(skyMap, vdir).rgb
            : vec3(0.55, 0.65, 0.80);
        color = mix(color, fogColor, fogFactor);
    }

    outColor = vec4(color, 1.0);
}
