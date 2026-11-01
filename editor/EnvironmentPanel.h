#pragma once
// ============================================================
// editor/EnvironmentPanel —— Sky & Atmosphere（天空 / 云 / 星空）
//
// ---- 为什么需要它 ----
//   在 2026-11-01 之前，天空是 `EnvSettings` 里两粒写死的颜色
//   （天顶 / 地面），面板上一个入口都没有 —— 想换天气只能改源码重编，
//   或者用 `MYVK_SKY_*` 环境变量（那是给自动化用的，不是给美术用的）。
//   同时"晴天 / 阴天 / 夜战"这类需求在二次元项目里出现频率极高，
//   每次都改代码显然不成立。
//
//   这块面板把天空拆成**可序列化的一组参数**（见 `render/SkyParams.h`），
//   四套预设只是这组参数的不同取值。
//
// ------------------------------------------------------------
// 三个刻意的设计
// ------------------------------------------------------------
// ① **改即所见，重烘节流**。面板每帧把结果丢给 `Renderer::setSkyParams()`，
//    背景（sky.frag 实时解析）下一帧就变；IBL 的三张图由 Renderer 内部
//    节流重烘（隔 6 帧）。所以拖滑条是"背景立刻跟、反射慢半拍"，
//    而不是两者一起卡。见 `Renderer::setSkyParams` 的注释。
//
// ② **面板不持有真值**。每帧从 `renderer.skyParams()` 读、改完写回。
//    不做"本地副本 + 同步按钮"—— 那种设计一旦有第二个写入者
//    （脚本、环境变量、场景加载）就会静默分叉，症状是"面板显示晴天、
//    画面是黄昏"。
//
// ③ **颜色是线性 HDR**。`zenith` 之类的数值会经过后处理的 tonemap 才上屏，
//    所以面板里看到的 0.16 不是屏幕上的 0.16。取色器按 Float 显示，
//    并标了 `HDR` 允许 >1。想"照着截图吸色"要自己转 sRGB→线性。
//
// ------------------------------------------------------------
// 自动化锚点（`tools/verify_sky.py` 用）
// ------------------------------------------------------------
//   SKY-RECT              面板内容区矩形
//   SKY-PRESET-day / -overcast / -night / -dusk
//   SKY-APPLY             回写参数（等价于 Inspector 上的"应用"）
// 日志锚点（由 Renderer 打）：
//   SKY-REBUILD zenith=(...) horizon=(...) cloud=... scale=... thick=...
//               star=... sun=...
// ============================================================

namespace render {
class Renderer;
struct SkyParams;
}

namespace editor {

class EnvironmentPanel {
public:
    // 每帧绘制（由 `EditorApp::panelDrawEnvironment` 派发）。
    //
    // ---- 为什么真值是 `sky` 而不是 Renderer 里那份 ----
    //   天气是**场景数据**（`scene::Scene::environment()`，随场景序列化）。
    //   Renderer 只是消费者：EditorApp 每帧把场景里那份推给它
    //   （`setSkyParams` 内部按字节比较，没变就返回）。
    //   如果反过来以 Renderer 为真值，就会多出一份"面板改了但没存进场景"
    //   的中间态 —— 保存时漏掉、换场景时被冲掉，两种症状都很难查。
    //
    //   `renderer` 只用来读"IBL 还在重烘吗"和"立刻重烘一次"。
    void draw(render::SkyParams& sky, render::Renderer& renderer);

private:
    // 四个页签的展开状态。默认全开 —— 这是"偶尔打开一次就要看全"的面板，
    // 折叠起来等于让人多点三下。
    bool m_openSky = true;
    bool m_openCloud = true;
    bool m_openStars = true;

    // 上一次预设按钮按下的名字（只为日志去重，不影响行为）。
    int m_lastPreset = -1;
};

} // namespace editor
