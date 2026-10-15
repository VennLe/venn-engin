#pragma once
// ============================================================
// FeatureUIGen —— E02 UI 生成层（ParamDesc → 控件）
//
// 这是"特性交付五件套"里的第③件（①env 读取 ②注册表 ③UI 控件
// ④序列化 ⑤回归映射）。核心想法：
//
//   一个参数**登记一次**（FeatureRegistry 的 ParamDesc），
//   控件就从元数据自动长出来 —— 不再"每参数手写一遍"。
//
// 与 Engine 面板（E09）的关系：面板只负责"分节 + 顶栏（档位/搜索/
// 预设）"，每个特性块本体由本文件生成。写值走注册表绑定的地址
// （render::paramPtr），与 env 路径写的是**同一份存储**。
//
// ---- 两条设计约束 ----
//
// ① **控件 ID 唯一且稳定**：每参数 PushID(param.id)（如 "ssr.strength"），
//   每特性再套一层 PushID(feature.id)。id 是持久化/UI 稳定标识，
//   不会因为显示文案调整而漂移（MYVK_LOG_RECTS 下无重复 ###）。
//
// ② **副作用走钩子**：个别参数"写成员不够、还要动手"（rp.bindless
//   切换要重建前向管线）。生成器不认识 Renderer，所以写完值后回调
//   宿主注入的钩子，由宿主决定要不要追加动作 + 打 FTR-UI 日志。
// ============================================================

#include <cstddef>
#include <functional>

namespace render {
struct ParamDesc;
struct FeatureDesc;
class GpuTimer;
}  // namespace render

namespace editor {

// 参数变更钩子：控件写完值之后回调（参数 id）。
// 宿主用来做副作用（如 rp.bindless 重建管线）与 FTR-UI 日志。
void setParamChangeHook(std::function<void(const char* paramId)> hook);

// E19：参数编辑**结束**时的撤销钩子。
//
// 控件在 `IsItemActivated()` 时抄一份旧值，`IsItemDeactivatedAfterEdit()`
// 时若值真的变了就回调这里（paramId + 改前/改后的原始字节）。
// 本文件不认识 CommandStack —— 由宿主推一条 `ParamEditCommand`。
//
// 为什么用"激活/失活"而不是"每帧 changed"：拖滑条时每帧都在变，按帧推
// 会把一次拖动写成几十条历史；按"编辑结束"推才是"一次操作一条"。
void setParamUndoHook(std::function<void(const char* paramId, const void* before,
                                         const void* after, std::size_t size)>
                          hook);

// 外部（E16 Console 命令、E18 命令面板）改完参数值后也走这一步 ——
// 让"写成员不够、还要动手"的副作用与 UI 路径完全一致。
void emitParamChange(const char* paramId);

// 画一个参数的控件（读写注册表绑定的地址）。返回本帧值是否被改。
// 未绑定的参数静默跳过（返回 false）——绑定缺失有启动自检兜底。
bool drawParamControl(const render::ParamDesc& p);

// 画一个特性块：标题行（启用开关 + 名称 + roadmap 徽标 + GPU 成本 +
// 右上角复位）+ 参数区。
//   tierLevel  0=Basic 1=Advanced 2=Debug（只显示 <= 该档的参数）
//   filter     搜索子串；空 = 不过滤。匹配特性名 → 整块可见；
//              只匹配参数 label → 只显示命中的参数。
//   gt         GpuTimer（可空）：非空且特性有 gpu 段名时显示毫秒成本。
// 返回是否有任何可见内容（全被过滤掉时返回 false，调用方据此跳过分节标题）。
bool drawFeatureBlock(const render::FeatureDesc& f, int tierLevel,
                      const char* filter, const render::GpuTimer* gt);

// 画质预设（E09 顶栏的 Preset ▾；数据档位化是 E26 的事，这里只拨开关）。
// preset: 0=Low 1=Medium 2=High 3=Epic 4=Cinematic。
// 恰好覆盖所有"有 .enabled 参数"的特性：不在清单里的一律关。
void applyQualityPreset(int preset);
const char* qualityPresetName(int preset);   // "Low"..."Cinematic"
int qualityPresetCount();

}  // namespace editor
