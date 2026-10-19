#pragma once
// ============================================================
// render/QualityPresets —— 画质预设表（**唯一权威**，2026-10-19 从
//                          `editor/FeatureUIGen.cpp` 下沉而来）
//
// 为什么下沉：启动器（Launcher.exe）要往工程的 `config/render.json` 里写
// "这一档画质打开了哪些开关"。如果表还留在 `editor/` 里，启动器就得
// 依赖 editor 应用层 —— 而依赖方向是 `launcher → engine`（与
// `runtime → engine ← editor` 同一条纪律）。
//
// 下沉的是**数据**，不是 UI：`editor::applyQualityPreset()` 保留（它还要
// 调 FeatureUIGen 的参数变更钩子），内部改成 `render::applyPreset(...)`。
// 于是"加一档画质"仍然只改这一处，两个消费者（编辑器顶栏 / 启动器）自动跟上。
//
// ------------------------------------------------------------
// 语义（与下沉前一模一样，不能改）
// ------------------------------------------------------------
// 每档是"**明确打开**的启用参数清单"，没列到的一律**关**。
// 这样预设是确定性的：从任何当前状态切过去，结果都一样。
//
// ⚠ "启用参数"的约定是 `id == "<featureId>.enabled"` 且 kind == Bool
//   （见 `isEnableParam` 的原定义，这里逐字保留）。
//
// ------------------------------------------------------------
// E26（2026-10-19）：数值档位化 —— 只登记**该随档位变**的那几个
// ------------------------------------------------------------
// "只拨开关"不够：Low 和 Cinematic 的差别不只是开不开，还有开多少。
// 但**不是每个数值参数都该进表**，这里刻意排除了两类：
//
//   ① **改了要重建资源的**（`shadow.resolution` / `lshadow.resolution`）。
//      它们只在初始化时生效，切档后必须重启 —— 放进档位表等于在面板上
//      撒谎（用户点了 High，画面没变，日志却说成功了）。
//      面板上用 `qualityRestartOnlyCount()` 单独列出来提醒。
//   ② **美术意图参数**（`ssao.radius` / `vfog.albedo` / 色调映射曲线）。
//      那些是"风格"不是"质量"。档位去改风格是最讨厌的那种自作聪明。
//
// 剩下 3 个都是"当帧生效 + 明确的性能/质量权衡"：
//   shadow.cascades / shadow.distance / lshadow.maxLights。
// 每档的具体值与理由见 `QualityPresets.cpp` 的 `kValueRows`。
//
// ⚠ **值的权威永远是参数本身**。`config/render.json` 里那个
//   `"quality": "High"` 只是**标签**（面板显示用）；它和实际值不一致时
//   显示 Custom，**不**反向去"纠正"参数 —— 与启动器 M4 解掉 P4 空挂
//   时的判断一致（详见 `docs/launcher_roadmap.md` §11.4）。
// ============================================================

#include "render/FeatureRegistry.h"

namespace render {

// `fillLayerForPreset` 的入参在 `RenderSettingsIO.h` 里定义。这里只前置
// 声明 —— 两个头文件互相 include 会形成环（RenderSettingsIO.h 又
// include 本文件拿 qualityPresetName）。
struct SettingsLayer;

// 档数 / 档名（"Low" / "Medium" / "High" / "Epic" / "Cinematic"）。
// 越界时 name 返回 "Custom"。
int qualityPresetCount();
const char* qualityPresetName(int preset);

// 这一档"明确打开"的 enable 参数 id 列表，以 nullptr 结尾。
// 越界返回 nullptr。
const char* const* qualityPresetEnableList(int preset);

// 按名字找档位（大小写敏感的精确匹配，"High"）；找不到返回 -1。
int qualityPresetByName(const char* name);

// ---------------------------------------------------------------- 枚举全部开关
//
// `kFeatures[]` 里每个特性最多一个 enable 参数。启动器写 render.json 时
// 需要**全量**（开的和关的都要写，否则"没写 = 用默认"会让画质档位
// 只生效一半 —— 默认开着的特性会漏过"这一档应该关掉"）。
struct EnableParam {
    const char* featureId;   // "ssao"
    const char* paramId;     // "ssao.enabled"
};
int enableParamCount();
const EnableParam& enableParamAt(int index);

// 这个参数是不是某特性的启用开关（约定 `<featureId>.enabled` + Bool）。
bool isEnableParam(const ParamDesc& p, const char* featureId);

// ---------------------------------------------------------------- 应用
//
// 把某档写进 registry 里已绑定的 bool。返回**翻转**了多少个。
// `hook` 可为空；非空时每个被改的参数各回调一次（编辑器用它做
// FTR-UI 日志 + 副作用）。
//
// ⚠ 未绑定的参数（paramPtr 为 nullptr）会被跳过并计入"未绑定"，
//   不静默当成"关了" —— 那样会把一个"还没接线的特性"写成关。
int applyPreset(int preset, void (*hook)(const char* paramId));

// `applyPreset` 之后"有几个 enable 参数没有绑定"（诊断用）。
int unboundEnableCount();

// ---------------------------------------------------------------- 数值档位（E26）

// 一行 = 一个参数在五档下的目标值。`v[i]` 对应档位下标
// （0=Low … 4=Cinematic）。档位数超过 kQualityPresetMax 时多出来的忽略。
constexpr int kQualityPresetMax = 8;

struct QualityValueRow {
    const char* paramId;   // "shadow.cascades"
    double v[kQualityPresetMax];
};

int qualityValueCount();
const QualityValueRow& qualityValueAt(int index);

// 某档下这个参数的期望值。未登记 / 档位非法 → false。
bool qualityValueFor(int preset, const char* paramId, double* out);

// "需要重启才生效、因此**不**由档位驱动"的参数（面板上要如实提示）。
// 目前是 shadow.resolution / lshadow.resolution（各一张 / 每层一张图）。
int qualityRestartOnlyCount();
const char* qualityRestartOnlyParam(int index);

// ---------------------------------------------------------------- 平台预设（E26）

// "平台" = 一档画质的别名。刻意**只做映射**，不引入第二套数值表 ——
// 两套表意味着同一个"High"在两条路径上给出不同结果，那是这个项目
// 已经吃过很多次亏的分裂。
struct PlatformPreset {
    const char* name;    // "PC High"
    int preset;          // 映射到的档位下标
};

int platformPresetCount();
const PlatformPreset& platformPresetAt(int index);
// 按名字找平台（"PC High"，精确匹配）；找不到返回 -1。
int platformPresetByName(const char* name);

// ---------------------------------------------------------------- 差异 / 一致性（E26）

// "把档位 applied 之后，哪些参数会和现在不同"。
// 面板的**差异预览**与**一致性检查**共用这一个函数 —— 两处各写一套比较
// 逻辑必然会漂移（这个项目已经栽过"兜底条件与放行条件不同源"）。
struct PresetChange {
    const char* paramId;
    const char* label;      // 英文原文（调用方自己 TR）
    ParamKind kind;
    char from[40];          // "on" / "off" / "90" / "4"
    char to[40];
};

// 返回"会变"的条数（可能 > maxOut；此时只填前 maxOut 条）。
// `preset` 非法 → 返回 -1。
int presetDiff(int preset, PresetChange* out, int maxOut);

// 这一档一共驱动多少个参数（enable + 数值）。面板上"差异项数 / 总数"用。
int presetDrivenCount(int preset);

// ---------------------------------------------------------------- 落盘（E26）

// 把某档**完整**写进一层覆盖：每个 enable 参数都写（true/false 都写，
// 与启动器 `writeQuality` 同一条理由）+ 每个数值项。
// 返回写入的键数；preset 非法 → 0。
//
// ⚠ 为什么不复用 `captureFromRegistry`（只抓 != 默认）：
//   切到 Low 时大量参数会被**关**，而"关"往往正好等于默认值 →
//   抓不到 → 写出来的文件缺键 → 重载后又变回默认（= 开着）。
//   启动器那一轮已经踩过这个坑，见 `ProjectDisplay.h` 的注释。
int fillLayerForPreset(int preset, SettingsLayer& out);

// 无窗口自检（`MYVK_QUALITY_TEST=1`）：表结构 / 平台映射 / 应用后
// 一致性为 0 / 差异条数与驱动数一致 / 落盘键数。
// 返回失败条数（0 = 全过）。
int runQualitySelfTest();

} // namespace render
