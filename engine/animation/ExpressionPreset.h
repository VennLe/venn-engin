#pragma once
// ============================================================
// animation/ExpressionPreset —— 表情预设资产（`.expr.json`）
//
// 这是 B6「表情预设」的**数据层**。在此之前引擎已经有：
//
//   FBX blend shape → MORPH 区块 → 顶点 morphBase/Count → 运行期 Mesh
//   → `assets::MorphRig`（滑条名 → 每子网格每 target 权重）
//   → `ecs::MorphComponent::weights` → GPU 形变
//
// 整条链是通的，Inspector 上也有一排滑条。但"**能形变**"和"**能做表情**"
// 是两件不同的事：
//
//   * 做「笑」要同时推 6 根滑条（嘴角上提 + 眼轮匝肌 + 眉毛下垂 …），
//     而且这 6 个数的**比例**是美术定的，不是玩家现调出来的；
//   * 同一份表情要在十几张脸上复用；
//   * 眨眼要按时间自动跑，口型要跟着语音音量走 —— 都由人手推滑条做不到。
//
// 本文件解决第一条与第二条（一份可复用的"命名权重组合"），
// `ExpressionDriver` 解决第三条。
//
// ------------------------------------------------------------
// 三条语义（都是"看起来能省、省了就错"的）
// ------------------------------------------------------------
// ⚠⚠ ① **权重按通道名而不是下标存**。`MorphRig` 的通道下标是"建表顺序"
//     （子网格序 × channel 序），重烤一次 FBX 就会变；而表情资产是要跨
//     模型、跨烤制版本复用的。名字是唯一的稳定键（与
//     `assets::setMorphWeight` 同一个理由）。
//
// ⚠⚠ ② **未知通道是警告，不是失败**。同一份"人脸通用表情"要挂到不同
//     角色上：某个角色没有 `brow_angry` 是**正常**的，不该整份加载失败；
//     但必须报出来 —— 否则"这个角色笑起来只有嘴动、眉毛不动"会被当成
//     rig 的 bug 查半天。`ExpressionSet::missing` 就是这份账。
//
// ⚠⚠ ③ **权重钳在 [0,1]，且越界要报**。静默钳位会让"我写 1.5 想夸张
//     一点"变成"和 1.0 一模一样"，而作者以为是资产没生效。这里钳位但
//     记 warning（与 `MorphRig::evaluate` 的钳位策略一致）。
//
// ------------------------------------------------------------
// 为什么本文件零引擎依赖
// ------------------------------------------------------------
// `ecs/Components.h` 要按**值**持有 `ExpressionSet`（与 `SequenceAsset`
// 同一个理由：资产小、由组件拥有所有权就不必给组件塞 AssetManager 指针，
// 也就不必在"组件里能不能做 IO"上打补丁）。所以这里只允许
// `<string> / <vector> / <cmath>` 这一级的依赖。
// ============================================================

#include <string>
#include <utility>
#include <vector>

namespace anim {

// 一条通道上的目标权重。
struct ExprWeight {
    std::string channel;
    float value = 1.0f;
};

// 一份命名表情：名字（键）+ 显示名（可翻）+ 一串权重。
struct ExpressionPreset {
    std::string name;   // 存档/脚本里用的键，**不翻译**
    std::string label;  // 面板上显示的名字（空 = 用 name），可翻译
    // 用 vector 保序：落盘后作者看到的顺序就是他写下的顺序。
    // 另外 `nlohmann::json` 的对象默认按键**字典序**排，直接用 map 会让
    // 一份表情落盘时字段乱序 —— 每次保存都产生无意义的 diff。
    std::vector<ExprWeight> weights;
};

struct ExpressionSet {
    std::string name;
    std::vector<ExpressionPreset> presets;
    // 解析期发现的问题（未知通道 / 重复名 / 越界 / 空预设）。
    // 加载成功也可以是**非空**的 —— 见文件头第 ② 条。
    std::vector<std::string> warnings;
    // 真正阻止使用的问题（JSON 语法、version 不认识、presets 不是数组）。
    std::string error;
    bool valid = false;

    // 名字 → 下标；没有返回 -1。空名字返回 -1。
    int find(const std::string& presetName) const;
    const ExpressionPreset* preset(const std::string& presetName) const;
};

// 解析。`tag` 只用于日志与错误信息（通常是相对路径）。
// 返回 false 时 `out.error` 有原因，`out.valid == false`。
bool parseExpressionSet(const std::string& text, const std::string& tag,
                        ExpressionSet& out);

// 读盘 + 解析。路径走 `assets::resolveAssetPath`，所以相对路径可用。
// ⚠ 读不到文件时 `out.valid == false` 且 `error` 写明**解析后的绝对路径**
//   —— 只写相对路径的话，"文件到底去哪找"永远是个谜。
bool loadExpressionSet(const std::string& path, ExpressionSet& out);

// 写回（缩进 2 / 不转义非 ASCII / 末尾换行，与其它资产一致）。
std::string toJson(const ExpressionSet& s);
bool saveExpressionSet(const std::string& path, const ExpressionSet& s,
                       std::string& err);

// 校验一份 set 对某个 rig 的适配度，把**没有对应滑条**的通道名填进
// `missing`（去重、保序）。`rigChannels` 传 rig 的通道名表。
//
// 单独做成纯函数而不是塞进 parse：解析期不知道要挂到哪个模型上。
void checkAgainstRig(const ExpressionSet& s,
                     const std::vector<std::string>& rigChannels,
                     std::vector<std::string>& missing);

} // namespace anim
