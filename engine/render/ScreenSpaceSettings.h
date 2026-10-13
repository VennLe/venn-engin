#pragma once
// ============================================================
// render/ScreenSpaceSettings —— 屏幕空间效果的总设置
//
// 一份结构体同时喂三个消费者：
//   · SSAOPass      （读 ssao* 字段，深度预通道后执行）
//   · pbr.frag      （读 fog* 字段 —— 走 FrameUBO.fogParams）
//   · FrameGraph    （读 ssgi / ssr / godrays 字段，前向后执行）
//
// 全部默认**关**。这不是保守，是硬性要求：像素级回归依赖
// "什么都不改 = 画面逐像素不变"的基线 —— 每个新效果都必须
// 满足"开着时画面不同、关着时逐位回到基线"的对照约定
// （与 FXAA / instancing 的先例完全一致）。
//
// 环境变量初值（Renderer 构造函数读）：
//   MYVK_SSAO / MYVK_FOG / MYVK_SSGI / MYVK_SSR / MYVK_GODRAYS = 1 开
//   MYVK_TAA = 1 开；MYVK_TAA_BLEND / MYVK_TAA_CLAMP = 数值覆盖
//   MYVK_FOG_DENSITY / MYVK_FOG_FALLOFF = 数值覆盖
//   MYVK_SSAO_RADIUS / MYVK_SSAO_STRENGTH = 数值覆盖
// 运行时由 Engine 面板实时改（Renderer::screenSpaceSettings()）。
//
// UI 切开关不需要任何资源重建：
//   · fog       每帧进 FrameUBO，天然实时；
//   · ssao      切换只影响 set0/binding14 绑的是 SSAO 结果还是
//               缺省白图（Renderer 检测到变化后重写描述符）；
//   · ssgi/ssr/godrays（FrameGraph 链）全关时 PostProcess 直接
//               采样 HDR，有任一开启时改采链的输出图 —— Renderer
//               检测到"链激活状态翻转"后调一次 PostProcess::recreate。
// ============================================================

#include "rhi/VulkanCommon.h"

namespace render {

struct ScreenSpaceSettings {
    // ---- SSAO（屏幕空间环境光遮蔽）----
    bool ssao = false;
    float ssaoRadius = 0.5f;      // 采样半径（米）
    float ssaoStrength = 1.0f;    // 遮蔽强度（0~2，1 = 标准）
    float ssaoBias = 0.025f;      // 遮蔽判定的深度偏置（米，防自遮蔽条纹）

    // ---- 高度雾（指数雾 × 高度衰减，Z-up：高度 = 世界 z）----
    bool fog = false;
    float fogDensity = 0.012f;     // 水平方向的基础密度
    float fogHeightFalloff = 0.08f; // 越高越稀的衰减系数（1/米）

    // ---- SSGI-lite（屏幕空间全局光，加色注入）----
    bool ssgi = false;
    float ssgiStrength = 0.5f;    // 间接光注入强度
    float ssgiRadius = 1.0f;      // 采样半径（米，决定"弹光"的邻域范围）

    // ---- SSR（屏幕空间反射）----
    bool ssr = false;
    float ssrStrength = 0.6f;     // 反射叠加强度（菲涅尔再乘一层）

    // ---- God rays（屏幕空间径向体积光）----
    bool godrays = false;
    float godraysStrength = 0.45f; // 光柱强度

    // ---- TAA（时域抗锯齿，R1-1）----
    // 链的**头**：前向之后先跑 TAA，再把解析结果喂给后面的屏幕空间效果。
    // 默认为**关**（与 FXAA / MSAA 的既有默认一致 —— 像素级回归要求
    // "不设任何 env 时与引入本模块之前逐像素不变"）。
    // 关着时：不抖动投影、一个 pass 都不录、PostProcess 直接采 HDR。
    //
    //   taaBlend  = 当前帧权重（越小越稳、越容易留下拖影）。0.1 是
    //               UE 默认量级（约 10 帧收敛）。
    //   taaClamp  = 邻域方差钳制的 sigma 系数（1.0 标准；>2 更保历史、
    //               更容易鬼影；<0.5 更激进地杀鬼影但会闪）。
    bool taa = false;
    float taaBlend = 0.1f;
    float taaClamp = 1.0f;
};

} // namespace render
