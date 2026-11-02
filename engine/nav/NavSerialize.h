#pragma once
// ============================================================
// nav/NavSerialize —— 导航代理组件的存盘形态（C8-4）
//
// 单独一个头文件而不是塞进 `SceneSerializer.cpp` 的匿名命名空间：
// `--selftest nav` 要**逐字段**验往返（十几个字段里有三个"默认值也要
// 写"的特例），而场景文件的往返需要 `AssetManager`（要设备），自检跑不
// 了那条路。做成公开函数之后，自检与场景序列化走的是**同一份**代码。
//
// 规矩与其它组件一致：
//   · **非默认才写**。挂一个代理但一个字段都没改，存回去不许长出多行
//     diff（否则整个资产库一存盘就是一片噪音）。
//   · 运行态（`path` / `next` / `repathTimer` / `reached` / `failed` /
//     `lastExpanded`）**一律不写**。
//   · `hasGoal` 由 `goal` 是否存在推出（写 `goal` 就等于有目标），
//     不单独存一个容易和它对不上的 bool。
// ============================================================

#include "ecs/Components.h"
#include "nav/NavGrid.h"

#include <nlohmann/json.hpp>

#include <string>

namespace nav {

// 返回 null：所有字段都是默认值且没有目标 —— 调用方**仍然应该写这个块**
// （它代表"这实体是导航代理"这件事），除非调用方另有约定。
nlohmann::json navAgentToJson(const ecs::NavAgentComponent& a);

// 返回 false：`j` 不是 object（调用方不建组件）。缺字段 = 用默认值。
bool navAgentFromJson(const nlohmann::json& j, ecs::NavAgentComponent& out);

// ---- 网格资产（`.vnav.json`）------------------------------------------
//
// 为什么网格要能存成**资产**而不是"每次进场景现场烘"：
//   · 烘焙要射线，射线要物理世界；而物理世界的开关、地面参数都是场景设置，
//     出包后玩家机器上的结果必须与作者机器上**逐格相同**（否则"这里明明
//     能过去"变成玄学）；
//   · 现场烘一次 200×200 格的图是几十毫秒，放在进场景那一下还行，放在
//     "换关卡/读档"就变成卡顿尖刺。
// 所以：作者在面板里烘一次 → 存进 `assets/nav/*.vnav.json` → 运行时优先读
// 资产，**读不到才现场烘**（宽松兜底，但日志会写清走的是哪条路）。
//
// 格式（`version` 1）：格子位图用**字符串**（'0' / '1'），高度用数组。
// ⚠ 位图不压成 base64 是刻意的：这个文件要能用文本编辑器打开、能用
//   `git diff` 看出"哪一片格子变了"。
nlohmann::json navGridToJson(const NavGrid& g);
// 返回 false 且填 `err`（"哪个字段不对"比"加载失败"有用得多）。
bool navGridFromJson(const nlohmann::json& j, NavGrid& out, std::string& err);

} // namespace nav
