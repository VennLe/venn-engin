#version 450
// ============================================================
// fg_motionblur.frag —— 相机运动模糊（屏幕空间重投影）
//
// 只用**相机**运动（不做逐物体运动向量 / 速度缓冲）：对每个像素用
// 深度重建视图空间点，再用 clipFromView = prevViewProj × inverse(view)
// 投到"上一帧屏幕坐标"，两者之差即屏幕空间速度；沿速度方向做 N 次
// 方向性采样累加。
//
// 为什么只用一个矩阵：推常量限 128B，cur VP + prev VP + invProj 三个
// mat4 就 192B 放不下。把"视图空间点 → 上一帧屏幕坐标"整条链压成一个
// 矩阵（clipFromView）后，片元只需从深度重建视图点（用解析投影系数
// P00/P11 反投影）+ 一次矩阵乘即可。
//
// 相机静止（或首帧，prev = cur）→ 速度 = 0 → 原图返回，与"关闭"逐像素
// 一致 —— 验证脚本靠这个做 off/static 的恒等断言。
// ============================================================

#include "space.glsl"

layout(set = 0, binding = 0) uniform sampler2D sceneColor;
#if VENN_MSAA_DEPTH
layout(set = 1, binding = 0) uniform sampler2DMS mbDepthTex;
#else
layout(set = 1, binding = 0) uniform sampler2D mbDepthTex;
#endif

layout(push_constant) uniform PC {
    vec4 cam;          // x=P00 y=P11 z=zn w=zf
    mat4 clipFromView; // prevViewProj × inverse(view)
    vec4 params;       // x=强度 y=采样数 z=最大速度(uv) w=快门开窗位置
} pc;

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

void main() {
    const vec3 src = texture(sceneColor, vUv).rgb;

    const float z = vennViewDepthFromNdc(vennSceneDepth(mbDepthTex, vUv),
                                         pc.cam.z, pc.cam.w);  // 正距离
    const vec2 ndc = vUv * 2.0 - 1.0;
    // 视图空间点（RH：z = -距离；x/y 用投影系数解析反投影）
    const vec3 viewPos = vec3(ndc.x * z / pc.cam.x, ndc.y * z / pc.cam.y, -z);
    const vec4 prev = pc.clipFromView * vec4(viewPos, 1.0);

    vec2 vel = vec2(0.0);
    if (prev.w > 1e-5) {
        const vec2 uvPrev = (prev.xy / prev.w) * 0.5 + 0.5;
        vel = (vUv - uvPrev) * pc.params.x;
    }
    const float len = length(vel);
    if (len < 1e-5) {           // 静止 / 首帧：原图返回（逐像素不变）
        outColor = vec4(src, 1.0);
        return;
    }
    // 限制最大速度（防极端拉伸把画面拉花）
    if (len > pc.params.z) vel *= pc.params.z / len;

    const int n = int(pc.params.y + 0.5);
    // 快门开窗位置（-1..+1）把整段采样窗沿速度方向平移半个窗口。
    //   pos = 0 → t ∈ [-0.5, +0.5]：关于当前帧居中（默认，与旧画面一致）
    //   pos = -1 → t ∈ [-1, 0]：只用上一帧 → 当前帧的拖尾
    //   pos = +1 → t ∈ [0, +1]：只取"下一帧会来的"内容
    // ⚠ 必须是**带符号**的平移。写成 abs(pos) 会让 ±1 落到同一个窗口，
    //   看起来"参数在动"（UI 滑条在跑）而画面左右两半的行为完全对称 ——
    //   验证用例 `B5c`（±1 必须互不相同）专门钉这一条。
    const float off = pc.params.w * 0.5;
    vec3 acc = vec3(0.0);
    for (int i = 0; i < n; ++i) {
        const float t = (float(i) / float(n - 1)) - 0.5 + off;  // -0.5+off .. 0.5+off
        acc += texture(sceneColor, clamp(vUv + vel * t, vec2(0.0), vec2(1.0)))
                   .rgb;
    }
    outColor = vec4(acc / float(n), 1.0);
}
