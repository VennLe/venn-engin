#version 450
// ============================================================
// pbr.frag —— Cook-Torrance PBR 片元着色器（分簇前向 / clustered forward）
//
//   BRDF 三件套：
//     D 法线分布      GGX / Trowbridge-Reitz
//     G 几何遮蔽      Smith + Schlick-GGX
//     F 菲涅尔        Schlick 近似
//   直接光 = (kD * albedo/π + specular) * radiance * NdotL
//   环境光 = 简化 IBL（程序化天空梯度）
//
//   法线贴图用屏幕空间导数构造 TBN（cotangent frame），
//   因此顶点数据无需切线属性。
//
// ---- 分簇光照（本文件的核心改动）----
//   本像素属于哪个簇：
//     XY = gl_FragCoord.xy / tileSize           → 屏幕瓦片
//     Z  = 线性化(gl_FragCoord.z) 取指数分层     → 深度层
//   然后从 clusters[] 拿到"本簇的灯列表区间"，遍历 lightIndices[] 里的
//   若干盏灯，各自做一次完整的 BRDF 求和。
//   灯列表由 cluster_build.comp 事先算好 —— 片元阶段不做任何"这盏灯
//   离我远不远"的判断，直接处理已经确定相关的灯。
//
//   簇的编址必须与 cluster_build.comp **完全一致**，否则会出现
//   "亮了不该亮的区域"这类极难定位的问题。
// ============================================================

const float PI = 3.14159265359;

layout(set = 0, binding = 0) uniform FrameUBO {
    mat4 view;
    mat4 proj;
    vec4 lightDir;
    vec4 lightColor;
    vec4 camPos;
    mat4 lightSpace;
    vec4 cameraParams;  // x=zNear y=zFar z=切片最近深度 w=深度切片数
    vec4 clusterDims;   // x=gridX y=gridY z=depthSlices w=tileSize
    vec4 screenSize;    // x=width y=height z=1/width w=1/height
    vec4 lightParams;   // x=灯数 y=环境光倍率 z=是否启用剔除
                        // w=每簇槽位数上限（与 ClusteredLighting 的
                        //   maxLightsPerCluster 同源）
    mat4 invViewProj;
} ubo;

// 阴影贴图。用 sampler2DShadow：采样时硬件直接做深度比较，
// texture() 返回 0/1（是否被遮挡），着色器里不需要手写 if
layout(set = 0, binding = 1) uniform sampler2DShadow shadowMap;

// 与 scene::LightInstance 严格对应（std430，4×vec4 = 64 字节）
struct Light {
    vec4 positionRange;      // xyz=世界坐标 w=影响半径
    vec4 colorIntensity;     // rgb=颜色  a=强度
    vec4 directionCosInner;  // xyz=传播方向 w=cos(内锥角)
    vec4 cosOuterType;       // x=cos(外锥角) y=类型(0=点 1=射灯 2=方向光)
};

layout(std430, set = 0, binding = 2) readonly buffer LightBuffer {
    Light lights[];
};

layout(std430, set = 0, binding = 3) readonly buffer ClusterBuffer {
    uvec2 clusters[];  // x=灯索引槽位基址 y=本簇灯数
};

layout(std430, set = 0, binding = 4) readonly buffer LightIndexBuffer {
    uint lightIndices[];
};

layout(set = 1, binding = 0) uniform sampler2D albedoMap;
layout(set = 2, binding = 0) uniform sampler2D normalMap;
layout(set = 3, binding = 0) uniform sampler2D ormMap;  // R=AO G=Rough B=Metal

// 单因子贴图（UE5 风格：每个因子旁边可以再单独挂一张）。
// 与 ormMap 是**相乘**关系，未绑定时绑的是内置纯白 → 乘法恒等。
// 只读 R 通道（灰度），按线性采样（粗糙度/金属度是物理量，不是颜色）。
layout(set = 4, binding = 0) uniform sampler2D roughnessMap;
layout(set = 5, binding = 0) uniform sampler2D metallicMap;
// 自发光贴图：与因子**相加**，未绑定时绑内置纯黑 → 加法恒等。
layout(set = 6, binding = 0) uniform sampler2D emissiveMap;

layout(push_constant) uniform Push {
    mat4 model;
    vec4 baseColorFactor;
    vec4 pbr;
    vec4 emissive;
} push;

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vUV;
layout(location = 3) in vec4 vLightSpacePos;

layout(location = 0) out vec4 outColor;

// ---------------- BRDF 各项 ----------------

// GGX 法线分布：rough=0 时趋于镜面脉冲，rough=1 时非常平缓
float distributionGGX(vec3 N, vec3 H, float rough) {
    float a = rough * rough;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 1e-6);
}

// Schlick-GGX 几何项（直接光用 k = (r+1)^2 / 8）
float geometrySchlickGGX(float NdotV, float rough) {
    float r = rough + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / max(NdotV * (1.0 - k) + k, 1e-6);
}

float geometrySmith(vec3 N, vec3 V, vec3 L, float rough) {
    return geometrySchlickGGX(max(dot(N, V), 0.0), rough) *
           geometrySchlickGGX(max(dot(N, L), 0.0), rough);
}

vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// 程序化环境：用方向采样"天空梯度"代替常量环境色。
// 这样金属反射会随法线朝向变化 —— 朝上的面反射天空、朝下的面反射地面，
// 这是让金属"看起来像金属"的关键（常量环境色会把金属压成一块死色）。
vec3 envColor(vec3 dir) {
    // ⚠ 本引擎是 **Z-up**（地面 = z = 0，光源 up 轴 = +Z），所以"朝上"
    // 对应的是 dir.z，不是 dir.y。用 dir.y 的话梯度方向会落到世界 +Y —
    // 那是水平方向，地面上的环境光就变成沿 Y 轴的一条明暗带：近处暗、
    // 远处亮，中间还有一条笔直的硬边，看起来就像地面上多了一块阴影。
    float t = clamp(dir.z * 0.5 + 0.5, 0.0, 1.0);
    vec3 ground = vec3(0.10, 0.09, 0.09);
    vec3 sky = vec3(0.55, 0.70, 0.95);
    return mix(ground, sky, t * t);
}

// 用位置/UV 的屏幕空间导数构造切线基（无需顶点切线属性）
mat3 cotangentFrame(vec3 N, vec3 p, vec2 uv) {
    vec3 dp1 = dFdx(p);
    vec3 dp2 = dFdy(p);
    vec2 duv1 = dFdx(uv);
    vec2 duv2 = dFdy(uv);

    vec3 dp2perp = cross(dp2, N);
    vec3 dp1perp = cross(N, dp1);
    vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;

    float m = max(dot(T, T), dot(B, B));
    if (m < 1e-12) return mat3(vec3(1, 0, 0), vec3(0, 1, 0), N);
    float inv = inversesqrt(m);
    return mat3(T * inv, B * inv, N);
}

// 3×3 PCF 软阴影。单点采样会得到锯齿状硬边，对邻域取平均后边缘自然柔和
float shadowFactor(vec4 lightSpacePos, float NdotL) {
    vec3 proj = lightSpacePos.xyz / lightSpacePos.w;
    // 只有 xy 需要从 NDC [-1,1] 映射到纹理坐标 [0,1]。
    // ⚠ z **不能**再 *0.5+0.5：光源投影是 Zero-to-One 约定（近=0 远=1，
    // 见 ShadowPass::updateLightMatrix），深度已经是 Vulkan 的 [0,1]。
    // 再压一次会把参考深度变成实际深度的一半，整片覆盖区都会被误判成阴影。
    proj.xy = proj.xy * 0.5 + 0.5;

    // 落在光源视野之外 → 视为无遮挡
    if (proj.z > 1.0 || proj.z < 0.0 || proj.x < 0.0 || proj.x > 1.0 ||
        proj.y < 0.0 || proj.y > 1.0) {
        return 1.0;
    }

    // 斜率相关深度偏移：表面越趋近平行于光线，自阴影条纹越严重
    float bias = max(0.0015 * (1.0 - NdotL), 0.0004);

    vec2 texel = 1.0 / vec2(textureSize(shadowMap, 0));
    float sum = 0.0;
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            sum += texture(shadowMap,
                           vec3(proj.xy + vec2(x, y) * texel, proj.z - bias));
        }
    }
    return sum / 9.0;
}

// ---------------- 分簇：定位本像素所属的簇 ----------------

// 视图空间深度 d ↔ NDC z 的互换（与 cluster_build.comp 用同一组公式）
float ndcToDepth(float z, float zn, float zf) {
    return (2.0 * zn * zf) / ((zn + zf) - z * (zf - zn));
}

uint clusterZIndex(float ndcZ) {
    const float zn = ubo.cameraParams.x;
    const float zf = ubo.cameraParams.y;
    const float clusterNear = ubo.cameraParams.z;
    const float slices = ubo.cameraParams.w;

    float d = ndcToDepth(ndcZ, zn, zf);
    d = clamp(d, clusterNear, zf);

    // 与 compute 侧 d(k) = clusterNear·(zf/clusterNear)^(k/slices) 互逆
    float k = log(d / clusterNear) / log(zf / clusterNear) * slices;
    return uint(clamp(k, 0.0, slices - 1.0));
}

uint clusterIndexOf(vec2 fragCoord, float ndcZ) {
    const uint gridX = uint(ubo.clusterDims.x);
    const uint gridY = uint(ubo.clusterDims.y);
    const uint slices = uint(ubo.clusterDims.z);
    const float tileSize = ubo.clusterDims.w;

    // gl_FragCoord 是像素中心坐标（从 0.5 开始），除以 tileSize 向下取整
    // 得到瓦片号；夹到网格内避免右/下边缘越界
    uvec2 tile = min(uvec2(fragCoord / tileSize), uvec2(gridX - 1u, gridY - 1u));
    uint z = min(clusterZIndex(ndcZ), slices - 1u);

    // 与 compute 侧的 (z*gridY + y)*gridX + x 完全一致
    return (z * gridY + tile.y) * gridX + tile.x;
}

// ---------------- 局部光源求值 ----------------

// 窗口化平方反比衰减：range 处严格归零。
// 朴素的 1/d² 在 range 边界处会留下一个突兀的硬边 —— 乘上一个
// "在 range 处恰好降到 0"的窗口函数才能让灯的影响范围自然消失。
float distanceAttenuation(float dist, float range) {
    float ratio = dist / max(range, 1e-4);
    float r4 = ratio * ratio * ratio * ratio;
    float window = clamp(1.0 - r4, 0.0, 1.0);
    return (window * window) / max(dist * dist, 1e-4);
}

// 锥形衰减：内锥内为 1，外锥外为 0，中间平滑过渡
float spotAttenuation(float cosAngle, float cosInner, float cosOuter) {
    return clamp((cosAngle - cosOuter) / max(cosInner - cosOuter, 1e-4),
                 0.0, 1.0);
}

// 单盏局部光源的完整 BRDF 贡献。
// 类型：0 = 点光，1 = 射灯，2 = 方向光（实体级补光：无衰减、无阴影，
//       L 直接取 -direction；分簇剔除靠它 10⁵ 米的"影响球"保证全覆盖）。
// 注意：灯列表是 compute 侧"保守剔除"的结果，可能包含实际照不到
// 这里的灯（尤其是射灯 —— 只按 range 球剔除），所以这里每一步
// 都要老老实实判断并提前退出。
vec3 shadeLocalLight(Light L, vec3 N, vec3 V, vec3 worldPos, vec3 albedo,
                     vec3 F0, float roughness, float metallic) {
    vec3 Ld;
    float att;

    if (L.cosOuterType.y > 1.5) {
        // ---- 方向光 ----
        Ld = normalize(-L.directionCosInner.xyz);
        att = 1.0;
    } else {
        // ---- 点光 / 射灯 ----
        vec3 toLight = L.positionRange.xyz - worldPos;
        float dist = length(toLight);
        float range = L.positionRange.w;
        if (dist >= range) return vec3(0.0);

        Ld = toLight / max(dist, 1e-5);
        att = distanceAttenuation(dist, range);

        if (L.cosOuterType.y > 0.5) {  // 射灯
            float cosAngle = dot(-Ld, L.directionCosInner.xyz);
            float s = spotAttenuation(cosAngle, L.directionCosInner.w,
                                      L.cosOuterType.x);
            att *= s * s;  // 平方一下让边缘收得更利落
        }
    }

    float NdotL = dot(N, Ld);
    if (NdotL <= 0.0) return vec3(0.0);
    if (att <= 0.0) return vec3(0.0);

    vec3 H = normalize(V + Ld);
    float NdotV = max(dot(N, V), 1e-4);

    float NDF = distributionGGX(N, H, roughness);
    float G = geometrySmith(N, V, Ld, roughness);
    vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);

    vec3 specular = (NDF * G * F) / max(4.0 * NdotV * NdotL, 1e-4);
    vec3 kS = F;
    vec3 kD = (vec3(1.0) - kS) * (1.0 - metallic);

    // 强度与颜色一起构成辐射亮度；HDR 下可以远大于 1
    vec3 radiance = L.colorIntensity.rgb * L.colorIntensity.a * att;
    return (kD * albedo / PI + specular) * radiance * NdotL;
}

void main() {
    // ---------------- 材质采样 ----------------
    vec4 albedoSample = texture(albedoMap, vUV);
    vec3 albedo = albedoSample.rgb * push.baseColorFactor.rgb;
    float alpha = albedoSample.a * push.baseColorFactor.a;

    float metallic = push.pbr.x;
    float roughness = push.pbr.y;
    float normalScale = push.pbr.z;
    float ao = push.pbr.w;

    vec3 orm = texture(ormMap, vUV).rgb;
    ao *= orm.r;
    roughness *= orm.g;
    metallic *= orm.b;

    // 单因子贴图再乘一层（未绑定时是纯白，等于没乘）。
    // 于是 glTF 那种打包 ORM 与"一张一张单独指定"两种用法可以共存：
    // 想单独换粗糙度，只需要在粗糙度槽里放一张图，不用去动 ORM。
    roughness *= texture(roughnessMap, vUV).r;
    metallic *= texture(metallicMap, vUV).r;

    roughness = clamp(roughness, 0.04, 1.0);
    metallic = clamp(metallic, 0.0, 1.0);

    // ---------------- 法线 ----------------
    vec3 N = normalize(vNormal);
    vec3 nSample = texture(normalMap, vUV).xyz * 2.0 - 1.0;
    nSample.xy *= normalScale;
    if (dot(nSample.xy, nSample.xy) > 1e-8) {
        mat3 TBN = cotangentFrame(N, vWorldPos, vUV);
        N = normalize(TBN * normalize(nSample));
    }

    vec3 V = normalize(ubo.camPos.xyz - vWorldPos);
    float NdotV = max(dot(N, V), 0.0);

    // 电介质基础反射率 0.04；金属的 F0 取 albedo
    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    // ---------------- 方向光（唯一带阴影的灯）----------------
    vec3 Ld = normalize(-ubo.lightDir.xyz);
    vec3 H = normalize(V + Ld);
    float NdotL = max(dot(N, Ld), 0.0);

    float NDF = distributionGGX(N, H, roughness);
    float G = geometrySmith(N, V, Ld, roughness);
    vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);
    vec3 specular = (NDF * G * F) / max(4.0 * NdotV * NdotL, 1e-4);

    vec3 kS = F;
    vec3 kD = (vec3(1.0) - kS) * (1.0 - metallic);

    vec3 radiance = ubo.lightColor.rgb * ubo.lightDir.w;

    // 阴影只衰减直接光；环境光不受遮挡影响（简化处理）
    float shadow = shadowFactor(vLightSpacePos, NdotL);
    vec3 direct = (kD * albedo / PI + specular) * radiance * NdotL * shadow;

    // ---------------- 分簇局部光源 ----------------
    // 只遍历"本簇灯列表"里的灯，数量由场景布置决定（室内小范围灯通常
    // 每簇几盏），与场景总灯数解耦 —— 这正是分簇的意义。
    vec3 localLights = vec3(0.0);
    const uint clusterIndex = clusterIndexOf(gl_FragCoord.xy, gl_FragCoord.z);
    const uvec2 cluster = clusters[clusterIndex];

    // 槽位上限从 UBO 读，与 compute 侧（push constant）同源。
    // clusters[].y 已被 compute clamp 过，这里只是防御性上界 ——
    // 首帧描述符内容未定时不至于读到越界的灯。
    const uint cap = uint(ubo.lightParams.w);
    const uint lightCount = min(cluster.y, cap);

    for (uint i = 0u; i < lightCount; ++i) {
        const uint li = lightIndices[cluster.x + i];
        localLights += shadeLocalLight(lights[li], N, V, vWorldPos, albedo, F0,
                                       roughness, metallic);
    }

    // ---------------- 环境光（程序化 IBL 近似）----------------
    // lightParams.y 是环境光倍率：室内场景把它压小，局部光源才有存在感
    const float ambientScale = ubo.lightParams.y;

    // 漫反射：朝 N 方向取环境辐照度（半球积分用单次采样近似）
    vec3 irradiance = envColor(N) * ao * ambientScale;
    vec3 ambientDiffuse = albedo * irradiance;

    // 镜面：沿反射方向取环境色；粗糙度越高反射越模糊，
    // 用 R 与 N 之间的插值模拟这个"模糊"，并让能量随粗糙度衰减。
    vec3 R = reflect(-V, N);
    vec3 specDir = normalize(mix(R, N, roughness * roughness));
    vec3 envSpec = envColor(specDir) * ao * ambientScale;
    // 用菲涅尔项调制：掠射角（边缘）反射更强 —— 金属边缘高光带由此而来
    vec3 F_env = fresnelSchlick(NdotV, F0);
    vec3 ambientSpecular = envSpec * F_env * (1.0 - roughness * 0.75);

    vec3 ambient = ambientDiffuse + ambientSpecular;

    // 自发光 = 因子 + 贴图。贴图槽空着时绑的是内置纯黑，加法恒等；
    // 放一张图进去立刻发光（因子保留 0 也亮），不用两头调。
    vec3 emissive = push.emissive.rgb + texture(emissiveMap, vUV).rgb;

    vec3 color = ambient + direct + localLights + emissive;

    // alpha 参与混合：透明材质走 baseline blend 管线
    outColor = vec4(color, alpha);
}
