#version 450
// ============================================================
// bloom_blur.frag —— 可分离高斯模糊（9 抽头近似）
//
// Bloom 的模糊质量直接决定光晕是否"脏"。这里用 9 抽头近似
// 高斯核（权重取自线性采样优化），横竖各跑一次：
//   brightness → blurH → blurV → 结果即半分辨率的 bloom 图
// 两趟都在半分辨率下进行，开销约为全分辨率的 1/4。
// ============================================================

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D srcTex;

// xy = 一个纹素的 UV 步长, zw = 模糊方向（±1,0 或 0,±1）
layout(push_constant) uniform PC {
    vec4 params;
} pc;

// 9 抽头高斯权重（中心 + 4 对对称采样）
const float kWeight[5] = float[](
    0.227027, 0.194595, 0.121622, 0.054054, 0.016216);

void main() {
    // 注意：变量不能叫 step —— 那是 GLSL 内建函数名
    vec2 delta = pc.params.xy * pc.params.zw;

    vec3 sum = texture(srcTex, vUv).rgb * kWeight[0];
    for (int i = 1; i < 5; ++i) {
        vec2 off = delta * float(i);
        sum += texture(srcTex, vUv + off).rgb * kWeight[i];
        sum += texture(srcTex, vUv - off).rgb * kWeight[i];
    }

    outColor = vec4(sum, 1.0);
}
