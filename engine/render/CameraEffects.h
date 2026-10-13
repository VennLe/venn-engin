#pragma once
// ============================================================
// render/CameraEffects —— 相机级后处理效果的总设置
//
// 四个效果分两处执行（各落在其自然的管线阶段）：
//   · 景深 DoF / 运动模糊 MB → FrameGraph 链（前向之后，需要读深度与
//     相机矩阵，与 SSGI/SSR/godrays 同一条链）；
//   · 色差 / 暗角           → PostProcess 合成（tonemap 之后的显示域，
//     单次采样就能做，不需要深度）。
//
// 全部默认关（暗角例外 —— 它在引入本模块前就存在于合成 push 里，
// 默认 0.25，为了不破坏基线保持不变）。关着时对应 pass 不录 /
// 对应 push 参数为 0，画面逐像素回到基线 —— 与 FXAA、屏幕空间效果的
// 对照约定完全一致。
//
// 环境变量初值（Renderer 构造函数读；验证脚本靠它们做对照）：
//   MYVK_DOF=1  MYVK_DOF_FOCUS=米  MYVK_DOF_APERTURE  MYVK_DOF_RADIUS=像素
//   MYVK_MOTIONBLUR=1  MYVK_MB_STRENGTH  MYVK_MB_SAMPLES
//   MYVK_CHROMA=1  MYVK_CHROMA_STRENGTH
//   MYVK_VIGNETTE_STRENGTH
// ============================================================

#include <glm/glm.hpp>

namespace render {

struct CameraEffectSettings {
    // ---- 景深（DoF，深度驱动的散景模糊）----
    bool dof = false;
    float dofFocus = 6.0f;        // 对焦距离（米）——此距离处完全清晰
    float dofAperture = 1.2f;     // 光圈：离焦量 → CoC 的比例系数
    float dofMaxRadius = 10.0f;   // CoC 上限（像素）——最大模糊半径

    // ---- 运动模糊（相机运动，屏幕空间重投影速度）----
    bool motionBlur = false;
    float motionBlurStrength = 1.0f;  // 速度缩放（>1 更夸张）
    int motionBlurSamples = 8;        // 沿速度方向的采样数（2..16）

    // ---- 色差（径向 RGB 分离）----
    bool chromatic = false;
    float chromaStrength = 0.4f;      // 边缘处的色散（占屏幕半径的比例，%）

    // ---- 暗角（保持既有默认 0.25）----
    bool vignette = true;
    float vignetteStrength = 0.25f;
    float vignettePower = 1.0f;       // >1 把暗角更压向边缘

    // 是否有任一"需要 FrameGraph pass"的效果开启（决定链是否被激活）
    bool needsChain() const { return dof || motionBlur; }
    // 是否存在任何"会改变画面"的效果（供日志 / 调试）
    bool anyActive() const { return dof || motionBlur || chromatic; }
};

} // namespace render
