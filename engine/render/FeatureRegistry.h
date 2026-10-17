#pragma once
// ============================================================
// render/FeatureRegistry —— 特性注册表（SSOT，单一事实源）
//
// 为什么需要它（这是本项目的一条结构性欠账）：
//   现状是**两套平行世界**：
//     · env 路径 —— Renderer 构造函数里散着 100 多处 std::getenv，
//       目的是让自动化测试能预置状态；
//     · UI 路径 —— 编辑器 Engine 面板里手写 500 行控件，直接改同一批
//       成员，目的是给人用。
//   两者都能写同一个字段，但**谁都没有义务覆盖全部字段**。于是
//   "字段加了、env 加了、面板忘了"就变成"只有测试能用、人用不了"的功能。
//   实测（2026-10-14）：32 组渲染特性里 24 组有 UI 面，**8 组完全没有**。
//
// 本注册表把"每个可调项"登记**一次**，然后五路自动派生：
//   env 读取 / UI 控件 / 序列化 / 工具提示 / 回归触发映射
// 手写量从"每参数一次"降到"每特性一次"。
//
// ---- 两条硬性设计约束（别轻易改，改了就会踩回归）----
//
// ① **不改存储布局**。本文件只有**元数据**（静态 const 表）+ **绑定点**
//    （运行时把参数 id 指到既有成员/字段的地址）。权威存储仍然是
//    ScreenSpaceSettings / CameraEffectSettings / PostSettings / Renderer
//    的成员。这样搬迁是**纯等价变换**：不设任何 env 时行为逐位不变，
//    39 例回归基线不受影响。
//
// ② **默认值在"绑定那一刻"捕获**。binding 发生在成员默认初始化之后、
//    env 覆盖之前，所以此刻的当前值**就是**默认值 —— 这样
//    resetToDefaults() 不需要把默认常量再抄一遍（抄一遍就一定会漂移）。
//
// ---- 一次只应用"未应用过"的绑定 ----
// 参数分散在三个不同的构造阶段才可用（m_post 要等 createMainTarget，
// EnvironmentSettings 是栈上的）：所以 loadEnvOverrides() 允许多次调用，
// 每次只处理**尚未应用**的绑定并打上标记。这与原代码"各在各自阶段读
// 一次 env"完全等价，且避免了"重读第二次把 init 里被设备能力修正过的
// 值又打回 env 值"这类隐蔽 bug（m_bindlessEnabled 就会在设备不支持时
// 被强制关掉）。
// ============================================================

#include <cstdint>
#include <string>
#include <vector>

namespace render {

// ---------------------------------------------------------------- 元数据

enum class ParamKind : uint8_t {
    Bool,     // bool（ImGui::Checkbox）
    Int,      // 32 位整数（ImGui::SliderInt）
    Enum,     // 32 位整数 + enumNames（ImGui::Combo）
    Float,    // float（ImGui::SliderFloat / DragFloat）
    Color3,   // float[3]（ImGui::ColorEdit3，颜色语义）
    Vec3,     // float[3]（ImGui::SliderFloat3，非颜色语义，如偏置三元组）
};

// 暴露分级。158 个旋钮平铺 = 没人能用，所以每参数必须有档位。
// 面板顶部三档开关（Basic / Advanced / Debug），默认只显示 Basic。
enum class ParamTier : uint8_t {
    Basic = 0,     // 日常必须见到（开关 + 一两个主参数）
    Advanced = 1,  // 调优时才看（半径/偏置/质量档）
    Debug = 2,     // 验证钩子 / 诊断（普通用户永远不需要）
};

// env 解析策略 —— **必须与搬迁前逐位等价**，所以每种都对应原代码里
// 一个具体的解析函数。不要"顺手统一"，那会改变边界行为。
enum class EnvMode : uint8_t {
    None,         // 无 env（纯 UI 参数）
    Bool,         // envBool：首字符 0/f/F 或 "off" → false，其余 → true
    FloatNonNeg,  // envFloat：解析失败或 < 0（含 NaN）→ 拒绝，保持默认
    FloatPositive,// 原代码写 `if (v > 0.0f)` 的那几处（严格正数）
    FloatAnySign, // atof 直取（允许负数，如 EV 偏置）
    IntRaw,       // atoi 直取，不做任何校验
    IntBool,      // atoi(s) != 0（MYVK_SHADOW_SNAP 那种写法）
    IntFromFloat, // envFloat 再截断成 int（MYVK_MB_SAMPLES）
    Vec3,         // "r,g,b"，必须三个都解析成功
    Vec3Partial,  // "%f,%f,%f"，解析到几个就写几个（MYVK_LOCAL_SHADOW_BIAS）
    FloatPair,    // "a,b"，要求 a >= 0 且 b > a（MYVK_POM_FADE）
};

// 哨兵值：表示"不检查 / 不夹"
constexpr float kNoMin = -1.0e30f;
constexpr float kNoMax = 1.0e30f;
constexpr float kNoClampLo = 1.0e30f;   // clampLo >= clampHi 即不夹
constexpr float kNoClampHi = -1.0e30f;

struct ParamDesc {
    const char* id;        // "ssr.strength"（唯一，同时是 UI 稳定 ID 的来源）
    const char* label;     // "Strength"（UI 显示）
    const char* env;       // "MYVK_SSR_STRENGTH"；nullptr = 无 env
    ParamKind kind;
    ParamTier tier;
    float lo, hi;          // UI 取值范围（Bool 忽略）
    EnvMode envMode;
    float minAccept;       // 解析值 < minAccept → 拒绝（保持默认）
    float maxAccept;       // 解析值 > maxAccept → 拒绝
    float clampLo;         // clampLo < clampHi 时，解析值夹紧到这个区间
    float clampHi;
    const char* tooltip;
    // ParamKind::Enum 用的选项名（以 nullptr 结尾）；其它 kind 为 nullptr
    const char* const* enumNames;
};

struct FeatureDesc {
    const char* id;         // "ssr"
    const char* name;       // "Screen Space Reflections"
    const char* group;      // 面板分区："Post" / "Scene" / "Shadows" / ...
    const char* roadmap;    // "R3-1"；nullptr = 路线图之前的既有特性
    const char* gpu;        // GpuTimer 段名（见 GpuTimer::sectionName），可为 nullptr
    const char* test;       // 回归用例名（tools/verify_*.py），可为 nullptr
    const ParamDesc* params;
    int paramCount;
};

extern const FeatureDesc kFeatures[];
extern const int kFeatureCount;

// ---------------------------------------------------------------- 小工具构造器
// 让 kmFeatures[] 表能自解释地写出来（C++17 没有指派初始化器，所以
// 用一组 constexpr 包装，参数名即字段名）。

constexpr ParamDesc mkParam(const char* id, const char* label, const char* env,
                            ParamKind kind, ParamTier tier, float lo, float hi,
                            EnvMode mode, const char* tip = "",
                            float minAccept = kNoMin, float maxAccept = kNoMax,
                            float clampLo = kNoClampLo,
                            float clampHi = kNoClampHi,
                            const char* const* enumNames = nullptr) {
    return ParamDesc{id,        label,   env,       kind,
                     tier,      lo,      hi,        mode,
                     minAccept, maxAccept, clampLo, clampHi,
                     tip,       enumNames};
}

constexpr ParamDesc PBool(const char* id, const char* label, const char* env,
                          ParamTier t, const char* tip = "") {
    return mkParam(id, label, env, ParamKind::Bool, t, 0.0f, 0.0f,
                   EnvMode::Bool, tip);
}

// Float，遵循 envFloat 语义（拒绝负数 / NaN）
constexpr ParamDesc PFloat(const char* id, const char* label, const char* env,
                           float lo, float hi, ParamTier t,
                           const char* tip = "", float minAccept = 0.0f) {
    return mkParam(id, label, env, ParamKind::Float, t, lo, hi,
                   env ? EnvMode::FloatNonNeg : EnvMode::None, tip, minAccept);
}

// Float，atof 直取（允许负值）
constexpr ParamDesc PFloatAny(const char* id, const char* label, const char* env,
                              float lo, float hi, ParamTier t,
                              const char* tip = "") {
    return mkParam(id, label, env, ParamKind::Float, t, lo, hi,
                   env ? EnvMode::FloatAnySign : EnvMode::None, tip);
}

// Float，严格正数（原代码里 `if (v > 0.0f)` 的那几处）
constexpr ParamDesc PFloatPos(const char* id, const char* label, const char* env,
                              float lo, float hi, ParamTier t,
                              const char* tip = "") {
    return mkParam(id, label, env, ParamKind::Float, t, lo, hi,
                   env ? EnvMode::FloatPositive : EnvMode::None, tip);
}

// Int，atoi 直取；可选 min/max 拒绝
constexpr ParamDesc PInt(const char* id, const char* label, const char* env,
                         float lo, float hi, ParamTier t, const char* tip = "",
                         float minAccept = kNoMin, float maxAccept = kNoMax) {
    return mkParam(id, label, env, ParamKind::Int, t, lo, hi,
                   env ? EnvMode::IntRaw : EnvMode::None, tip, minAccept,
                   maxAccept);
}

// 纯 UI 的 float（无 env）
constexpr ParamDesc PUFloat(const char* id, const char* label, float lo,
                            float hi, ParamTier t, const char* tip = "") {
    return mkParam(id, label, nullptr, ParamKind::Float, t, lo, hi,
                   EnvMode::None, tip);
}

// 纯 UI 的 int（无 env）
constexpr ParamDesc PUInt(const char* id, const char* label, float lo, float hi,
                          ParamTier t, const char* tip = "") {
    return mkParam(id, label, nullptr, ParamKind::Int, t, lo, hi, EnvMode::None,
                   tip);
}

// 纯 UI 的 bool（无 env）
constexpr ParamDesc PUBool(const char* id, const char* label, ParamTier t,
                           const char* tip = "") {
    return mkParam(id, label, nullptr, ParamKind::Bool, t, 0.0f, 0.0f,
                   EnvMode::None, tip);
}

constexpr ParamDesc PColor3(const char* id, const char* label, const char* env,
                            ParamTier t, const char* tip = "") {
    return mkParam(id, label, env, ParamKind::Color3, t, 0.0f, 4.0f,
                   env ? EnvMode::Vec3 : EnvMode::None, tip);
}

// 三个 float 的"非颜色"三元组（如阴影偏置），走 "%f,%f,%f" 部分解析
constexpr ParamDesc PVec3(const char* id, const char* label, const char* env,
                          ParamTier t, float lo, float hi,
                          const char* tip = "") {
    return mkParam(id, label, env, ParamKind::Vec3, t, lo, hi,
                   env ? EnvMode::Vec3Partial : EnvMode::None, tip);
}

// 枚举（Int + 选项名），如 tonemap
constexpr ParamDesc PEnum(const char* id, const char* label, const char* env,
                          ParamTier t, const char* const* names,
                          const char* tip = "") {
    return mkParam(id, label, env, ParamKind::Enum, t, 0.0f, 0.0f,
                   env ? EnvMode::IntRaw : EnvMode::None, tip, kNoMin, kNoMax,
                   kNoClampLo, kNoClampHi, names);
}

// Bool，但 env 用 atoi(s) != 0 解析（MYVK_SHADOW_SNAP 那种写法）
constexpr ParamDesc PBoolAto(const char* id, const char* label, const char* env,
                             ParamTier t, const char* tip = "") {
    return mkParam(id, label, env, ParamKind::Bool, t, 0.0f, 0.0f,
                   EnvMode::IntBool, tip);
}

// Float，atof 直取 + 夹紧（MYVK_SHADOW_BLEND / _HARDEN / _LAMBDA 那种）。
// 原代码写法是 `atof(s)` 后 `min(max(v, lo), hi)` —— **不拒绝负数**，
// 负数会被夹到下界，所以这里必须用 FloatAnySign（用 FloatNonNeg 会变成
// "拒绝并保持默认"，两者在 v<0 时结果不同）。
constexpr ParamDesc PFloatClamp(const char* id, const char* label,
                                const char* env, float lo, float hi, ParamTier t,
                                float clampLo, float clampHi,
                                const char* tip = "") {
    return mkParam(id, label, env, ParamKind::Float, t, lo, hi,
                   env ? EnvMode::FloatAnySign : EnvMode::None, tip, kNoMin,
                   kNoMax, clampLo, clampHi);
}

// Float，"envFloat 语义 + 夹紧"（MYVK_RENDER_SCALE 那种）。
// 与 PFloatClamp 的唯一区别：解析失败或负数**拒绝**（保持默认），
// 而不是夹到下界 —— 因为原代码先调 envFloat（拒绝 <0/NaN）再夹紧。
constexpr ParamDesc PFloatClampNN(const char* id, const char* label,
                                  const char* env, float lo, float hi,
                                  ParamTier t, float clampLo, float clampHi,
                                  const char* tip = "") {
    return mkParam(id, label, env, ParamKind::Float, t, lo, hi,
                   env ? EnvMode::FloatNonNeg : EnvMode::None, tip, kNoMin,
                   kNoMax, clampLo, clampHi);
}

// ---------------------------------------------------------------- 绑定

// 把一个参数 id 接到真实存储地址上。**必须在成员默认初始化之后调用**
// （此刻的当前值会被记成默认值，供 resetToDefaults 用）。
//
// kind 必须与 ParamDesc 里登记的 kind 一致（调试期会 WARN）。
void bindParam(const char* id, void* ptr, ParamKind kind);

// 带"副地址"的绑定：只给 EnvMode::FloatPair 用（一个 env 串写两个字段，
// 如 MYVK_POM_FADE="10,26" → fadeStart + fadeEnd）。aux 就是第二个字段。
void bindParamAux(const char* id, void* ptr, void* aux, ParamKind kind);

int  bindingCount();
// 回退到某个计数（局部作用域用，见 BindingScope）
void popBindingsAfter(int mark);

// 局部作用域绑定：用于把"只在一段函数里有效"的地址（栈上的
// EnvSettings 之类）临时接进来。作用域结束自动摘掉，避免留下悬垂指针。
struct BindingScope {
    BindingScope() : m_mark(bindingCount()) {}
    ~BindingScope() { popBindingsAfter(m_mark); }
    BindingScope(const BindingScope&) = delete;
    BindingScope& operator=(const BindingScope&) = delete;
private:
    int m_mark;
};

// ---------------------------------------------------------------- 查找

const ParamDesc* findParam(const char* id);
const FeatureDesc* findFeature(const char* id);
// 已绑定参数的地址；未绑定返回 nullptr
void* paramPtr(const char* id);
// 该参数在哪个特性里（面板分组用）。找不到返回 nullptr。
const FeatureDesc* ownerFeature(const char* paramId);

// ---------------------------------------------------------------- 五路派生

// 把 env 覆盖写进已绑定参数。**只处理本次调用前尚未应用过的绑定**，
// 所以可以安全地分阶段多次调用（构造期 / createMainTarget / 建环境）。
// 返回本次实际写入的参数条数。
int loadEnvOverrides();

// 恢复全部已绑定参数到"绑定那一刻"的默认值。
void resetToDefaults();

// 把某一个参数恢复默认（UI 的"复位"小按钮用）。返回是否成功。
bool resetParam(const char* id);

// ---------------------------------------------------------------- 文件层覆盖（E03）
// RenderSettingsIO 的覆盖链要往"同一个存储"里写值，所以它需要的**只是**
// 绑定表，而不是第二个影子结构体。这里把绑定暴露成一份只读视图。
//
// ⚠ 为什么"文件层"和"env 层"要用**两个独立**的 applied 标记：
//   同一个参数完全可能既被 `config/render.json` 覆盖、又被 `MYVK_*` 覆盖。
//   env 是测试钩子，必须**永远赢**。于是每个阶段的顺序被钉死成：
//       applySettingsOnce() → loadEnvOverrides()
//   两边各记各的"处理过没有"，才能在**分阶段绑定**（构造期 /
//   createMainTarget 之后）的两次调用里都保持这个顺序。

struct BindingView {
    // 用 std::string 而不是 const char*：绑定表的 vector 会随 bindParam
    // 扩容，裸指针会在下一次绑定后悬垂。
    std::string id;
    ParamKind kind = ParamKind::Float;
    void* ptr = nullptr;
    void* aux = nullptr;
    // 绑定那一刻捕获的默认值（与 resetToDefaults() 用的是同一份）
    bool dBool = false;
    int dInt = 0;
    float dFloat = 0.0f;
    float dColor[3] = {0.0f, 0.0f, 0.0f};
};

// 取走"尚未被文件层处理过"的绑定（**取即标记**，语义与 loadEnvOverrides
// 的"只处理一次"完全一致）。返回本次新取到的条数。
int takeUnappliedBindings(std::vector<BindingView>& out);

// 全部已绑定参数（"导出当前设置"用）。不改变任何标记。
int collectBindings(std::vector<BindingView>& out);

// 把文件层的 applied 标记全部清掉 —— 只给 UI 的"重新载入设置"用
// （用户手改 json 后要求当场生效）。env 的标记**不动**。
void resetSettingsApplied();

// ---------------------------------------------------------------- 自检

// 登记了 ParamDesc 但没人在代码里 bind 的参数（拼写错 / 忘了接线）。
// > 0 说明表与代码脱节 —— 启动时打一次 WARN 点名。
int unboundParamCount(const char** outIds, int maxOut);
void logRegistrySelfCheck();

} // namespace render
