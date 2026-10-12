#version 450
// ============================================================
// sky.frag —— 天空盒片元着色器
//
// 只做一件事：把视线方向丢进天空立方体贴图。
//
// 为什么是"采样贴图"而不是"再算一遍解析式天空"：
//   环境光（IBL 的 irradiance / prefilter）都以这张立方体为唯一
//   输入。背景如果另算一遍，两边就会各自漂移 —— 一改天顶色，
//   背景变了而反射没变（或者反过来），是最难查的那类不一致。
//   所以天空**只生成一次**，之后所有地方都读同一张图。
//
// 立方体是 128²、线性过滤，视线方向跨面插值不连续的问题由
// Vulkan 的 seamless cube filtering 兜住。
// ============================================================

layout(location = 0) in vec3 vDir;
layout(location = 0) out vec4 outColor;

// set 0 / binding 10：天空立方体（只在本 pass 用）
layout(set = 0, binding = 10) uniform samplerCube skyMap;

void main() {
    // 已经是 HDR 线性值（R16G16B16A16 会丢精度，所以天空图是 RGBA32F），
    // 直接写进 HDR 目标，交给后处理做 tonemap。
    outColor = vec4(texture(skyMap, normalize(vDir)).rgb, 1.0);
}
