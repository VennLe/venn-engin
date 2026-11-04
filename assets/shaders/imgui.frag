#version 450 core
// ============================================================
// imgui.frag —— ImGui 的片元着色器（**替换品**，不是新管线）
//
// 为什么需要它：ImGui 的 Vulkan 后端自带一份片元着色器，内容是
//     fColor = In.Color * texture(...);
// 也就是把顶点色**原样**写进附件。而本引擎的交换链优先选
// B8G8R8A8_SRGB（见 rhi/Swapchain.cpp），sRGB 附件意味着**硬件在写入时
// 自动做 linear → sRGB 编码**。ImGui 的顶点色本来就是 sRGB（主题里的
// rgb(0.113,0.117,0.125) 就是 #1D1E20 这个设计色），于是：
//     设计色 #1D1E20（0.1137）→ 硬件编码 → 0.370 → 屏幕读到 **95**
//     整块 UI 亮了一大截，暗色主题变成一片中灰，文字对比度也被压平。
// 这正是 ui2.frag 顶部那段注释描述的同一个坑 —— 那边做了反编码，
// ImGui 这条路径一直漏着，症状是"编辑器整体发灰、顶栏看着像一条白杠"。
//
// 做法：在片元里先 srgbToLinear 反编码，硬件再编码回去，屏幕上就正好
// 是设计色。验收按实测值钉：菜单栏应该读到 **29**（0x1D），读到 95 就是
// 这一步漏了。
//
// 为什么不像 ui2 那样自己建管线：ImGui 的顶点/描述符布局是后端私有的，
// 自己建管线要重新实现一遍后端。1.93 的后端留了口子 ——
// ImGui_ImplVulkan_InitInfo::CustomShaderFragCreateInfo，只换片元模块，
// 顶点模块、描述符布局、混合状态全部沿用后端的。见 ui/ImGuiManager.cpp。
//
// ⚠ 输入/输出签名必须与后端顶点着色器逐字匹配（下面是 ImGui 官方那份
//   glsl_shader.frag 的签名，一个字符都不能改）：
//     layout(location=0) out vec4 fColor;
//     set 0 binding 0 = texture2D，set 1 binding 0 = sampler（**分离式**
//     纹理/采样器，不是 sampler2D）—— 换成 sampler2D 会与后端建的
//     描述符集布局对不上，vkCreateGraphicsPipelines 直接失败。
// ============================================================

layout(location = 0) out vec4 fColor;

layout(set = 0, binding = 0) uniform texture2D _Texture;
layout(set = 1, binding = 0) uniform sampler _Sampler;

layout(location = 0) in struct { vec4 Color; vec2 UV; } In;

// 精确的 sRGB → 线性（分段函数，不是 pow 2.2 近似）。
// 与 ui2.frag 里那份保持一致：近似会让 0.5 → 0.2176 而不是 0.2158，
// 屏幕上差 1~2 级。既然一行就能写精确的，就没必要留这个含糊。
vec3 srgbToLinear(vec3 c) {
    return mix(c / 12.92,
               pow((c + 0.055) / 1.055, vec3(2.4)),
               step(vec3(0.04045), c));
}

void main() {
    // ⚠⚠ 只反编码**顶点色**，贴图一个字都别碰。
    //
    // 为什么贴图不能再解一次：
    //   · UI 贴图（类型卡插图 / 工程封面 / 缩略图）都走
    //     `assets::loadTexture(path, key, /*srgb=*/true)` → 视图是 **SRGB**
    //     格式 → 采样时**硬件已经解码成线性**了。再解一次等于暗两次：
    //     实测 152 → 79 / 113 → 43 / 96 → 32，整张插图黑掉一大截。
    //   · 字形图集是 R8G8B8A8_**UNORM**、RGB 全白（只有 alpha 存覆盖率），
    //     乘进来等于"没有"，两种格式下都安全。
    //   · 顶点色乘贴图本身在两种情况下都成立：现在 == 改造前的
    //     `In.Color * texture`（只是把顶点色换到了线性空间）。
    vec4 t = texture(sampler2D(_Texture, _Sampler), In.UV.st);
    fColor = vec4(srgbToLinear(In.Color.rgb) * t.rgb, In.Color.a * t.a);
}
