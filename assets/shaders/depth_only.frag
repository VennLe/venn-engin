#version 450
// ============================================================
// depth_only.frag —— 几何预通道的片元着色器（深度 + 几何缓冲）
//
// 顶点着色器**复用 pbr.vert**：这样深度预通道与主 Pass 的顶点变换
// 逐位一致，主 Pass 用 LESS_OR_EQUAL 做深度测试时不会出现"自己和自己
// 打架"的深度闪烁（浮点误差导致的 z-fighting）。
//
// ---- 2026-10-25：从"只写深度"变成"深度 + 几何" ----
// 预通道多了一个颜色附件（HdrTarget::kGeometryFormat，R16G16B16A16_SFLOAT）：
//   rgb = 世界空间法线（**原始 -1..1，不编码成 0..1**）
//   a   = 视图空间线性距离（米）
//
// 为什么不另开一个 pass 去拿法线：
//   不透明几何已经在这里完整画过一遍。多画一遍是顶点 + 光栅化成本翻倍，
//   而这里只多写一个附件 —— 后处理描边、线宽视距补偿、以后的 SSAO/DOF
//   全都吃这一份。
//
// 为什么 a 通道存"距离"而不是 NDC 深度：
//   描边的深度判据要用**与距离无关**的相对差（`|d0-d1| / min(d0,d1)`）。
//   直接存 NDC 深度的话，同一个阈值在近处过敏感、远处完全失灵 ——
//   表现是"贴着镜头的人物描边粗成一团、远处的人物没有描边"。
//   `1.0 / gl_FragCoord.w == -viewSpaceZ`，是透视投影下的线性距离，
//   拿它做相对差就与距离无关了。
//
// ⚠ **正交投影下 `gl_FragCoord.w` 恒为 1** → a 通道退化成常数 1.0，
//   深度边缘检测失效（法线边缘仍然有效）。有意接受的局限：正交只出现在
//   编辑器的前视/顶视图，而描边是给游戏视口用的。
//
// ⚠ 双面材质（DOUBLE_SIDED）在预通道里**不翻转背面法线**：本管线是
//   SingleSpv，只有 cullMode 随掩码变，不至于为翻转再编一份。后果是
//   看到裙摆内侧时法线朝里 → 内侧检出假描边。等真遇到再按掩码编变体。
// ============================================================

// pbr.vert 的输出（世界空间法线；pbr.inst.vert / pbr.mdi.vert 同 layout）
layout(location = 1) in vec3 vNormal;

// ---- 2026-11-05：选中描边的标记位 ----
// 推常量的布局与 pbr.vert 完全一致（同一个 VkPipelineLayout，范围 128B、
// 阶段 VERTEX | FRAGMENT）。这里只读 pbr.x：
//   0 = 普通物体，1 = 编辑器选中的物体
// 标记方式：把 alpha 写成**负的**视图距离。正负号就是"是不是选中物"，
// 绝对值和原来逐位相同 —— 所以描边（NPR）那边的深度差值判据只要取 abs()
// 就一点也不受影响（见 post.frag 的 geoEdge）。
layout(push_constant) uniform Push {
    mat4 model;
    vec4 baseColorFactor;
    vec4 pbr;
    vec4 emissive;
    vec4 skin;
} push;

layout(location = 0) out vec4 outGeo;

void main() {
    const vec3 n = normalize(vNormal);
    // 透视：clip.w == -viewSpaceZ，取倒数即"相机到该点的线性距离（米）"。
    // 除零保护：理论上 w 不会为 0（意味着点在相机平面上），但退化三角形
    // 上驱动给出 0 是有先例的，一个 max 换确定性。
    const float viewDist = 1.0 / max(gl_FragCoord.w, 1e-8);
    const float signedDist = push.pbr.x > 0.5 ? -viewDist : viewDist;
    outGeo = vec4(n, signedDist);
}
