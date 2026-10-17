#pragma once
// ============================================================
// render/RenderSettingsIO —— E03 · 设置层级与持久化
//
// 要解决的问题：E01 的 FeatureRegistry 把"每个旋钮"收敛成了一处**存储**，
// 但存储只有**一层**（默认值 + env）。真实项目里同一个参数在不同粒度上
// 有不同的正确值：
//
//     Editor Preferences   跨项目（"我这台机器就是关着 SSAO 干活"）
//       └─ Project         config/render.json（"这个项目用 4 级联"）
//           └─ Scene       （"这一关是夜景，曝光 0.6"）
//               └─ Object  （"这台机器人的轮廓光更强"）
//                   └─ Camera（"这台特写机位用长焦景深"）
//
// 本文件提供**覆盖链**：读的时候从**最具体往最泛**回退，写的时候只写
// 被显式覆盖的键。缺一层就跳一层，一层都没有 = 出厂默认。
//
// ---- 四条硬性约束（违反了会直接踩回归）----
//
// ① **无文件 = 零动作**。`config/render.json` 不存在时不产生任何日志、
//    不写任何参数 → 与 E03 之前**逐像素一致**。这是 §9 的硬约束。
//
// ② **首次运行不自动生成文件**。只有"用户真的改了参数"或显式点
//    "Export" 才落盘。否则一次手动会话就会在仓库里留下一个 json，
//    把所有像素基线悄悄换掉。
//
// ③ **自动化跑不读默认路径、写一律拒绝**。与 EditorLayoutIO 同一条规则
//    （`MYVK_FRAMES` / `MYVK_NO_WINDOW_SAVE` 视为自动化）。但**显式**给
//    路径（`MYVK_RENDER_SETTINGS`）照读 —— 那是脚本自己要的。
//
// ④ **文件层与 env 层用各自的 applied 标记，env 永远后写**。
//    env 是测试钩子，必须能压过任何存档；所以每个绑定阶段的顺序钉死成
//    `applyToRegistry()` → `loadEnvOverrides()`。
//
// ---- 校验为什么和 env 完全一致 ----
// 同一个参数不该因为"从哪一层来"而收到不同的夹紧规则。所以这里只做
// env 那套：拒绝非有限值 / 按 minAccept,maxAccept 拒绝 / 按
// clampLo,clampHi 夹紧。**不**额外按 UI 的 lo,hi 夹 —— 已经踩过：
// `lshadow.bias` 的 UI 范围是 0~1，而默认 z 分量是 1.2，按 UI 夹会把
// 默认值本身改坏（导出→重载一次就漂移）。
// ============================================================

#include "render/FeatureRegistry.h"   // ParamKind / ParamDesc / BindingView

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace render {

// ---------------------------------------------------------------- 作用域

// 从**最泛**到**最具体**排列。resolve() 反向走。
enum class SettingScope : uint8_t {
    EditorPrefs = 0,   // 跨项目：config/render_prefs.json
    Project     = 1,   // 项目：config/render.json
    Scene       = 2,   // 场景文件内嵌块
    Object      = 3,   // 实体组件
    Camera      = 4,   // 相机组件
};
constexpr int kSettingScopeCount = 5;

const char* settingScopeName(SettingScope s);

// ---------------------------------------------------------------- 数据

// 一个参数的值。够同时表示六种 ParamKind：
//   Bool/Int/Enum/Float → count = 1
//   Float + aux（pom.fade 那种"一个键写两个字段"）→ count = 2
//   Color3/Vec3 → count = 3
// 统一用 double 存，是为了"json 里写 0.1 读回来还是 0.1"。
struct SettingValue {
    int count = 1;
    double v[3] = {0.0, 0.0, 0.0};
    bool isVec() const { return count > 1; }
};

// 一个覆盖层。`present == false` 表示"这一层不存在"（文件没读到 / 场景里
// 没有这个块）—— 与"存在但是空的"区分开，日志和诊断都靠这个区分。
struct SettingsLayer {
    SettingScope scope = SettingScope::Project;
    std::string source;                 // 文件路径 / "scene:xxx.json#renderOverrides"
    bool present = false;
    std::map<std::string, SettingValue> values;
    int rejected = 0;                   // 未知键 / 类型不符 / 越界 的条数

    bool empty() const { return values.empty(); }
    void clear() {
        source.clear();
        present = false;
        values.clear();
        rejected = 0;
    }
};

// ---------------------------------------------------------------- 解析 / 落盘

// 从文件读一层。文件不存在 → 返回 false 且 `out.present == false`
// （**这不是错误**，是"没有覆盖"的正常情况，不打 WARN）。
// json 语法错 → 返回 false 且打一条 WARN（验收条款：非法 json 只告警不崩）。
bool loadLayerFile(SettingScope scope, const std::string& path,
                   SettingsLayer& out);

// 从内存里的 json 文本读一层（场景内嵌块 / 试验用）。`source` 只用于日志。
bool parseLayerText(SettingScope scope, const std::string& source,
                    const std::string& text, SettingsLayer& out);

// 写一层到文件（会建父目录）。`overwrite=false` 且文件已存在时拒绝。
bool saveLayerFile(const SettingsLayer& layer, const std::string& path);

// 把已绑定参数的当前值抓成一层 —— 只抓**与默认值不同**的那些，
// 所以导出结果天然是"最小覆盖集"，重载后与导出前逐位一致。
SettingsLayer captureFromRegistry(SettingScope scope);

// ---------------------------------------------------------------- 覆盖链

class SettingsChain {
public:
    void setLayer(const SettingsLayer& layer);   // 按 layer.scope 归位
    void clearLayer(SettingScope s);
    void clear();

    const SettingsLayer* layer(SettingScope s) const;
    int presentLayerCount() const;
    int valueCount() const;

    // 从最具体往最泛找第一个有这个键的层。
    // 命中 → true（`out` 是值，`from` 是来源层）；否则 false（保持默认）。
    bool resolve(const std::string& paramId, SettingValue& out,
                 SettingScope* from = nullptr) const;

    // "prefs=2,project=5"（日志用）
    std::string describe() const;

private:
    SettingsLayer m_layers[kSettingScopeCount];
};

// 进程内唯一的链。引擎层持有它，编辑器与运行时**共用同一份** ——
// 与"场景⇄物理只在 engine/physics/ScenePhysics 里翻译一次"同一个理由。
SettingsChain& activeChain();

// ---------------------------------------------------------------- 路径与守卫

// `MYVK_RENDER_SETTINGS` > "config/render.json"
std::string projectSettingsPath();
// `MYVK_RENDER_PREFS` > "config/render_prefs.json"
std::string prefsSettingsPath();

bool projectSettingsPathExplicit();
bool prefsSettingsPathExplicit();

// 自动化运行（`MYVK_FRAMES` / `MYVK_NO_WINDOW_SAVE` 存在即为真）。
bool renderSettingsAutomationRun();

// ---------------------------------------------------------------- 应用

// 启动期读盘：prefs + project 两层。**只读一次**（重复调用直接返回 0）。
// 返回装进链里的参数总数。无文件时完全静默（约束 ①）。
int loadStartupLayers(bool logSummary = true);

// 把链里的值写进已绑定参数。只处理"尚未被文件层处理过"的绑定
// （与 loadEnvOverrides 同语义），所以可以在每个绑定阶段各调一次。
// `force=true` 时改处理**全部**已绑定参数（UI 的"重新载入"用）。
int applyToRegistry(bool force = false, bool logApplied = true);

// UI 用：重新读盘 → 重新应用（用户手改 json 后要求当场生效）。
bool reloadFromDisk();

// ---------------------------------------------------------------- 自检

// 无窗口、无 Vulkan、无 GPU 的自检（`MYVK_RENDER_SETTINGS_TEST=1`）。
// 覆盖：解析 / 未知键拒绝 / 类型拒绝 / 五层优先级 / 应用到绑定 /
// 只抓非默认 / 落盘往返 / 非法 json 不崩。
// 返回失败条数（0 = 全过）。
int runSettingsSelfTest();

} // namespace render
