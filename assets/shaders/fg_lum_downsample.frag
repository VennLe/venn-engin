#version 450
// ============================================================
// fg_lum_downsample.frag —— 自动曝光（R1-2）第一步：亮度测量
//
// 把 HDR 场景色（无界的线性颜色）降采样成一张**固定尺寸**的
// "块平均对数亮度图"（每个输出纹素 = 对应区块内 n×n 个分层采样点
// 的 log2 亮度均值）。
//
// 为什么用 log2 而不是线性亮度：
//   · 曝光是**乘性**的，几何平均（log 域算术平均）才是它的自然
//     平均量 —— 线性平均会被太阳/高光这类离群亮点主导；
//   · 直方图在 log 域上是均匀分箱，亮部暗部同等精度（正是人眼
//     与传感器的响应方式）。
//
// 为什么走**片元**而不是 compute：
//   HdrTarget 的前向 renderpass 只把"颜色写"依赖到 FRAGMENT
//   （见 HdrTarget.cpp deps[1].dstStageMask），compute 阶段读这张图
//   属于未声明依赖 —— 要么补一条需要 VkImage 的显式屏障，要么
//   老老实实走片元。走片元还能白蹭 renderpass 的布局迁移。
//
// 为什么需要这张中间图（而不是让 compute 直接数 HDR 的像素）：
//   ① 逼 compute 去读一张"外部拥有"的图，就要那条 VkImage 屏障；
//   ② 降采样后总量从 ~150 万像素降到 4096，直方图的原子操作
//      压力小两个数量级。
//
// 关闭时本 pass 不录 —— 零开销、零行为差异。
// ============================================================

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outLum;

layout(set = 0, binding = 0) uniform sampler2D srcTex;

layout(push_constant) uniform PC {
    vec4 texel;   // x = 1/lumW  y = 1/lumH（输出纹素在 UV 空间的尺寸）
    vec4 params;  // x = 每纹素每轴采样数 n  y = log2 下限  z = log2 上限
} pc;

float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

void main() {
    const int n = int(pc.params.x + 0.5);

    // 区块左上角：vUv 落在纹素中心，减半格得到块原点
    const vec2 origin = vUv - 0.5 * pc.texel.xy;

    float sum = 0.0;
    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < n; ++i) {
            // 分层采样：(i+0.5)/n 均匀铺满整块，避免规则网格与
            // 场景栅格共振（地面格线是高频周期结构）
            const vec2 uv = origin +
                            pc.texel.xy * ((vec2(float(i), float(j)) + 0.5) /
                                           float(n));
            const float l = luma(texture(srcTex, clamp(uv, 0.0, 1.0)).rgb);
            // 下限 1e-5（约 -16.6 档）—— log2(0) 是 -inf，会污染整张图
            sum += log2(max(l, 1e-5));
        }
    }

    const float avg = clamp(sum / float(n * n), pc.params.y, pc.params.z);
    outLum = vec4(avg, 0.0, 0.0, 1.0);
}
