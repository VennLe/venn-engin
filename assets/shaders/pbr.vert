#version 450
// ============================================================
// pbr.vert —— PBR 顶点着色器
//   set 0 / binding 0 : FrameUBO（相机 + 光照 + 光源空间矩阵）
//   set 0 / binding 6 : 骨骼矩阵 SSBO（蒙皮网格用，静态网格为空）
//   push constant     : 模型矩阵 + PBR 材质参数 + 蒙皮定位
//
// 布局必须与 C++ 端严格一致：
//   FrameUBO      -> render/RenderTypes.h  struct FrameUBO   （std140）
//   push_constant -> render/RenderTypes.h  struct PushConstants（std430）
// ============================================================

// 必须与 pbr.frag 的 FrameUBO **逐字段一致**：
// 同一组 set/binding 被多个阶段访问时，声明必须完全匹配。
// 顶点阶段只用到前 6 个成员，但多声明的成员只要偏移不变就无害。
layout(set = 0, binding = 0) uniform FrameUBO {
    mat4 view;
    mat4 proj;
    vec4 lightDir;     // xyz = 传播方向（指向被照物），w = 强度
    vec4 lightColor;   // rgb = 颜色
    vec4 camPos;       // xyz = 相机世界坐标
    mat4 lightSpace[4];// 4 级级联的光源空间矩阵（索引即级联号）
    vec4 cameraParams; // x=zNear y=zFar z=切片最近深度 w=深度切片数
    vec4 clusterDims;  // x=gridX y=gridY z=depthSlices w=tileSize
    vec4 screenSize;   // x=width y=height
    vec4 lightParams;  // x=灯数 y=环境光倍率
    mat4 invViewProj;
    vec4 envParams;    // x=IBL 开关 y=预过滤最大 mip（顶点阶段不用，
                       //   但必须与 pbr.frag 声明完全一致）
    vec4 shadowParams; // x=级联数 y=最大阴影距离 z=淡出起点 w=开关
    vec4 cascadeSplits;// x..w = 各级覆盖的远端世界距离
    vec4 shadowBias;   // x=常数偏移 y=斜率偏移 z=PCF 半径 w=远级联增长
    vec4 localShadowParams; // x=常数偏移 y=斜率偏移 z=PCF 半径 w=开关
                       //   （顶点阶段不用，但必须与 pbr.frag 声明一致）
    vec4 fogParams;    // x=密度 y=高度衰减 z=雾开关 w=保留
                       //   （顶点阶段不用，但必须与 pbr.frag 声明一致）
} ubo;

// 骨骼矩阵 SSBO：所有蒙皮模型共享一块大 buffer，靠 jointBase 定位。
// 静态网格不会绑定任何数据，这里声明成空数组（bindless 语义，尺寸运行时定）。
// readonly：顶点阶段只读，避免 vertexPipelineStoresAndAtomics 的验证报错。
layout(set = 0, binding = 6) readonly buffer JointMatrices {
    mat4 jointMats[];   // 运行时大小 = 所有蒙皮模型的关节矩阵总和
} jointBuf;

layout(push_constant) uniform Push {
    mat4 model;
    vec4 baseColorFactor;  // rgb = 基色，a = 未使用
    vec4 pbr;              // x=metallic, y=roughness, z=normalScale, w=ao
    vec4 emissive;         // rgb = 自发光
    vec4 skin;             // x=jointBase y=joint总数 z=1蒙皮/0静态
} push;

// GPU instancing（-DVENN_INSTANCING=1 编出 pbr.inst.vert.spv）：
// 合批时每实例的 model 矩阵放在 set 0 binding 13 的 SSBO 里，
// gl_InstanceIndex（**含** vkCmdDrawIndexedInstanced 的 firstInstance 偏移）
// 直接就是全局槽下标 —— 批与批的槽区间天然不重叠，不需要额外基址。
// 关键字掩码 / 材质在批内恒定，仍从 push 常量读（每批推一次）。
#ifdef VENN_INSTANCING
layout(set = 0, binding = 13) readonly buffer InstanceModels {
    mat4 vennInstanceModels[];
};
#endif

// Multi-draw indirect（-DVENN_MDI=1 编出 pbr.mdi.vert.spv，R2-2）：
// 每个实例一条**结构化记录**（world matrix + aux），由 mdi_compact.comp
// 在 GPU 上紧凑化后写进 binding 15。gl_InstanceIndex 含间接命令的
// firstInstance 偏移 → 直接就是紧凑化实例表的下标。
//
// 材质**仍走 push 常量**：一次 vkCmdDrawIndexedIndirect 覆盖的是
// "同管线 + 同材质"的一组批（set 1..6 的贴图也在组内恒定），所以
// push 在组内是一个常数，不需要按实例取材质。
//   aux.x = jointBase（蒙皮网格的关节矩阵起始下标），取代 push.skin.x ——
//   因为 push 现在是"每材质组一次"，没法再承载每网格的蒙皮定位。
#ifdef VENN_MDI
struct MdiInstance {
    mat4 model;
    ivec4 aux;   // x = jointBase（<0 静态），y = morph 槽位（§15）
};
layout(std430, set = 0, binding = 15) readonly buffer MdiInstances {
    MdiInstance items[];
};
#endif

// ============================================================
// GPU 形变（§15）
//
//   binding 25  MorphDeltas   delta 流（32 B/条；布局与磁盘逐字节一致）
//   binding 26  MorphWeights  每实例一段 float，下标 = target 下标
//   binding 27  MorphSlots    每实例一条 {deltaBase, weightBase, targetCount}
//
// 一个顶点只遍历**自己**的 delta（`inMorphCount` 条，紧凑排列在
// `deltaBase + inMorphBase` 处），权重查 `weightBase + delta.target`。
// 这就是"稀疏 morph"在 GPU 上的形态：没被任何 target 影响的顶点不进循环，
// 一层表情只动几千个顶点，代价与"动了多少"成正比而不是与"有多少 target"
// 成正比。
//
// ⚠ `MorphDelta` 的布局不能随手改：std430 下 vec3 的对齐是 16，所以
//   position(0) target(12) normal(16) vertex(28) 正好 32 字节，
//   与 C++ 的 `assets::MorphDelta` 逐字节相同（那边有 static_assert）。
//   把 `target` 和 `normal` 的顺序换一下就会多出 12 字节填充，
//   症状是"形变按错位的 target 施加"——几何全对，只有表情是乱的。
// ============================================================
struct MorphDelta {
    vec3 position;   //  0
    uint target;     // 12
    vec3 normal;     // 16
    uint vertex;     // 28（GPU 侧不读，文件里是"流序==顶点序"的校验锚）
};

layout(set = 0, binding = 25) readonly buffer MorphDeltas {
    MorphDelta morphDeltas[];
};
layout(set = 0, binding = 26) readonly buffer MorphWeights {
    float morphWeights[];
};
layout(set = 0, binding = 27) readonly buffer MorphSlots {
    uvec4 morphSlots[];   // x=deltaBase y=weightBase z=targetCount w=保留
};

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in uvec4 inJoints;  // 骨架内 joint 下标
layout(location = 4) in vec4 inWeights;  // 权重（和 > 0 才蒙皮）
layout(location = 5) in vec4 inColor;    // 顶点色（Mesh Paint；默认全 1）
// morph（§15）：x = delta 流起点，y = 条数（0 = 这个顶点不参与形变）。
// 上一刀（§14）只把这两个字段落在顶点里没人读；这一刀才是它们的消费者。
// 属性表那侧必须同时喂到 location 8（见 Mesh::attributeDescriptions），
// 少喂一条读到的是全 0 —— 而 0 条 delta 意味着"静默不形变"。
layout(location = 7) in uint inMorphBase;
layout(location = 8) in uint inMorphCount;
// 切线（§13 落盘，A4 2026-10-26 起有消费者）：xyz = 沿 UV.x 的单位向量，
// w = 副切线手性。**0 = 没有切线**（程序化以外的旧资产、LOD 层都是 0）。
// ⚠ 属性表那侧必须同时喂到 location 9（Mesh::attributeDescriptions），
//   少喂一条读到的是全 0 —— 对切线来说正好是"静默没有各向异性"。
layout(location = 9) in vec4 inTangent;

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vUV;
layout(location = 3) out vec4 vColor;
// 世界空间切线（xyz 单位化，w 原样传手性）。长度 0 = 这个顶点没有切线，
// 片元侧据此回退到屏幕空间导数建切线基（见 pbr.frag 的 anisoTangent）。
layout(location = 4) out vec4 vTangent;
// 注意：**没有** vLightSpacePos 了。改 CSM 之后级联是按视深度在片元阶段
// 选的，顶点阶段不知道会落在哪一级，所以只传世界坐标，由 pbr.frag 用
// ubo.lightSpace[cascade] 现算。

void main() {
    // ---- 每实例的 model 矩阵 + 蒙皮定位 ----
    // 三条路径的**顶点变换数学必须逐位一致**（深度预通道与前向共用同一份
    // 顶点着色器，浮点差异会让 early-Z 闪烁），所以这里只换"数据从哪来":
    //   MDI   ：结构化实例记录（binding 15），jointBase 也在记录里
    //   合批  ：实例 model 表（binding 13），jointBase 走 push.skin.x
    //   基线  ：push.model，jointBase 走 push.skin.x
    mat4 modelMat;
    int jointBase;
    // morph 槽位：合批 / 基线两条路径都等于 gl_InstanceIndex（基线把槽号
    // 直接当 firstInstance 传给 vkCmdDrawIndexed），MDI 只能走 aux.y
    // —— 它的 gl_InstanceIndex 是**紧凑化之后**的下标，与全局槽空间无关。
    uint morphSlot;
#ifdef VENN_MDI
    const MdiInstance rec = items[gl_InstanceIndex];
    modelMat = rec.model;
    jointBase = rec.aux.x;
    morphSlot = uint(rec.aux.y);
#elif defined(VENN_INSTANCING)
    modelMat = vennInstanceModels[gl_InstanceIndex];
    jointBase = int(push.skin.x);
    morphSlot = uint(gl_InstanceIndex);
#else
    modelMat = push.model;
    jointBase = int(push.skin.x);
    morphSlot = uint(gl_InstanceIndex);
#endif

    // ---- GPU 形变（§15）：**在蒙皮之前**施加 ----
    //
    // 顺序不能反：delta 是与 `inPos` 同一个空间的偏移（导入时过了
    // geometry_to_world 的线性部分），而 skinMat 是"静止世界 → 当前姿势"。
    // 先形变再蒙皮 = 顶点在当前姿势下被推着走；反过来（先蒙皮再形变）
    // 会让 delta 被当成世界空间的方向直接加，角色一转身表情就往侧边飞。
    vec3 morphPos = vec3(0.0);
    vec3 morphNrm = vec3(0.0);
    if (inMorphCount > 0u) {
        const uvec4 mi = morphSlots[morphSlot];
        // mi.z == 0 → 这条实例没有形变（渲染层把"没有 morph 的项"的槽
        // 显式清零，见 Renderer::writeMorphSlot）。不判它的话，槽里上一帧
        // 的残留记录会让顶点拿**别的实例**的权重去形变。
        if (mi.z > 0u) {
            const uint base = mi.x + inMorphBase;
            // 上限 256：morphCount 来自顶点缓冲，一份坏数据能让循环跑到
            // 几十万次（GPU 挂起 = 驱动超时 + 设备丢失，比画错难查得多）。
            const uint n = min(inMorphCount, 256u);
            for (uint i = 0u; i < n; ++i) {
                const MorphDelta d = morphDeltas[base + i];
                const float w = morphWeights[mi.y + d.target];
                morphPos += d.position * w;
                morphNrm += d.normal * w;
            }
        }
    }

    // ---- 蒙皮 ----
    // 静态网格：inWeights 全 0，走原路径（等价于恒等变换）。
    vec4 localPos = vec4(inPos + morphPos, 1.0);
    vec4 localNrm = vec4(inNormal + morphNrm, 0.0);
    // 切线：只过蒙皮/模型变换，**不过 morph**（morph delta 流没有切线通道，
    // 表情形变对头发切线的影响可以忽略）。
    vec3 localTan = inTangent.xyz;
    if (dot(inWeights, vec4(1.0)) > 0.0) {
        mat4 skinMat =
              jointBuf.jointMats[jointBase + int(inJoints.x)] * inWeights.x
            + jointBuf.jointMats[jointBase + int(inJoints.y)] * inWeights.y
            + jointBuf.jointMats[jointBase + int(inJoints.z)] * inWeights.z
            + jointBuf.jointMats[jointBase + int(inJoints.w)] * inWeights.w;
        localPos = skinMat * vec4(inPos + morphPos, 1.0);
        localNrm = skinMat * vec4(inNormal + morphNrm, 0.0);
        localTan = (skinMat * vec4(inTangent.xyz, 0.0)).xyz;
    }

    vec4 world = modelMat * localPos;

    vWorldPos = world.xyz;
    // 注：非等比缩放时严格应使用逆转置矩阵，当前场景均为均匀缩放。
    // 蒙皮后的法线：skinMat 含旋转+缩放，用归一化近似（角色均近似刚性）。
    vNormal = normalize(mat3(modelMat) * localNrm.xyz);
    // 切线同上；⚠ **不能无条件 normalize**：没有切线的顶点（导入资产多数
    // 没有 TANGENT、LOD 层全部没有）在这里是零向量，normalize(0) = NaN，
    // 而 NaN 会在光栅化插值里扩散成整片"神秘消失"的三角形。
    // 所以零长度原样传 0，片元侧据长度判断"要不要回退到屏幕空间导数"。
    const vec3 wTan = mat3(modelMat) * localTan;
    const float tanLen = length(wTan);
    vTangent = vec4(tanLen > 1.0e-5 ? wTan / tanLen : vec3(0.0), inTangent.w);
    vUV = inUV;
    vColor = inColor;

    gl_Position = ubo.proj * ubo.view * world;
}
