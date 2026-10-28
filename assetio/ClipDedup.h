#pragma once
// ============================================================
// assetio/ClipDedup —— 动画 take 名归一化 + **重名去重**
//
// 为什么单独抽一个头文件（2026-10-28，任务 #329）：
//   仓库里有**两份**互不相干的 FBX 动画提取器
//     · `assetio/FbxImport.cpp`      —— AssetC（离线烤 .vmesh）与导入自检
//     · `engine/assets/FbxLoader.cpp` —— 运行时直读 .fbx（角色窗口那条路）
//   它们是同一段逻辑的两份拷贝（历史原因：一个要产出 IR、一个直接填
//   `assets::Skeleton`）。名字归一化那 6 行两边各写一遍，**重名去重
//   两边都没有** —— 于是同一个 bug 在两个入口各犯一次，症状还不一样
//   （AssetC 侧是 `--info` 报 clips=4；运行时侧是角色窗口里两行同名 clip，
//   被 ImGui 判成 ID 冲突以后**点哪都没反应**）。逻辑一旦有两份，
//   修一个漏一个几乎是必然的，所以这里抽出来给两边共用。
//
// 背景：`anime_girl.fbx`（2026-10-11 入库）里有 4 个 AnimationStack：
//   `Rig|Idle` ×2 + `Rig|Walk` ×2 —— 同一个动作被导出了两遍。归一化
//   掉 `<armature>|` 前缀之后名字变成 [Idle, Walk, Idle, Walk]。
//
// 两条规则（都是"能证明才动手"，不做猜测）：
//   · **同名 + 同时长** → 判定为同一个动作的重复导出 → **丢弃**后一段。
//     时长一致是最强的可用证据：两段真不同的动画恰好撞名、时长又恰好
//     相同，概率可以忽略；而"保留两段一模一样的"对谁都没好处。
//   · **同名 + 不同时长** → 不是重复，是"撞名"（不同动作取了同一个名字）。
//     **改名**而不是丢：丢一段动画是不可逆的数据损失，改名只是难看。
//     后缀从 `_2` 起，撞上已有名字就继续往上加。
//
// ⚠ 两边的调用点都必须在**采样之前**做这个决策：逐帧采样一段 2 秒的
//   动画是 61 帧 × N 骨 × 3 通道，丢掉的那段纯属白算（alicia 那种
//   128 骨的资产尤其明显）。
// ============================================================

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace assetio {

// 归一化 take 名：Blender 的 FBX 导出（`bake_anim_use_all_actions`）把
// take 名写成 "<armature 对象名>|<action 名>"，例如 `ALICIA|Idle`。
// Unity / UE / glTF 转换器一律把 `|` 之后当 clip 名，这里照办 ——
// 否则 `Skeleton::findClip("Idle")` 的逐字比较找不到，角色会**静默退回
// rest 姿势**（画面是 T-pose，日志却全绿）。
//
// 只剥第一个 `|` 之前的；`|` 后面为空（`"A|"`）时原样保留。
inline std::string clipNameFromTake(const std::string& take) {
    const std::size_t bar = take.find('|');
    if (bar != std::string::npos && bar + 1 < take.size())
        return take.substr(bar + 1);
    return take;
}

// 时长比较的容差。FBX 的时间戳是 double，但同一段动作导出两次也可能
// 差最后一帧的取整误差；1e-4 秒远小于 30fps 的一帧（0.033 s），
// 又远大于 double→float 的往返误差。
constexpr float kClipDurEps = 1.0e-4f;

struct ClipNamePlan {
    bool keep = true;         // false = 这段是重复导出，整段丢掉
    bool renamed = false;     // true  = 撞名但时长不同，已改成 name.name
    std::string name;         // 最终名字（keep=false 时无意义）
};
// 给一段候选 clip 决定"留不留、叫什么"。
//   kept —— **已经收下**的 clip 的 (名字, 时长) 列表（顺序无关）
inline ClipNamePlan planClipName(
    const std::vector<std::pair<std::string, float>>& kept,
    const std::string& name, float duration) {
    ClipNamePlan plan;
    plan.name = name;

    bool clash = false;
    for (const auto& k : kept) {
        if (k.first != name) continue;
        // 同名同时长 → 重复导出，丢。
        if (std::fabs(k.second - duration) <= kClipDurEps) {
            plan.keep = false;
            return plan;
        }
        clash = true;
    }
    if (!clash) return plan;

    // 同名不同时长 → 改名，不丢数据。
    plan.renamed = true;
    for (int n = 2; n < 1000; ++n) {
        const std::string cand = name + "_" + std::to_string(n);
        bool taken = false;
        for (const auto& k : kept)
            if (k.first == cand) {
                taken = true;
                break;
            }
        if (!taken) {
            plan.name = cand;
            break;
        }
    }
    return plan;
}

// ------------------------------------------------------------
// 事件 + 文本
//
// 为什么把"格式化"也放在这里：两个调用方（AssetC 用 `std::printf`、引擎用
// `VK_LOG_INFO`）打印方式不同，但**文字内容必须一字不差** —— 验收脚本
// 按 `FBX-CLIPDUP ... action=drop` 去 grep。两边各拼一遍字符串，
// 迟早有一边改格式另一边忘。文字只在这里生成。
// ------------------------------------------------------------
struct ClipDedupEvent {
    std::string take;      // FBX 里原始的 take 名（`Rig|Idle`）
    std::string name;      // 归一化之后的名字（`Idle`）
    std::string finalName; // 改名后的名字（没改名就是空的）
    float duration = 0.0f;
    bool dropped = false;  // true=丢弃 / false=改名保留
};

inline std::string clipDedupLine(const ClipDedupEvent& ev) {
    char buf[512];
    if (ev.dropped) {
        std::snprintf(buf, sizeof(buf),
                      "FBX-CLIPDUP take='%s' name='%s' dur=%.4f action=drop "
                      "(same name & same duration -> duplicate export)",
                      ev.take.c_str(), ev.name.c_str(),
                      static_cast<double>(ev.duration));
    } else {
        std::snprintf(buf, sizeof(buf),
                      "FBX-CLIPDUP take='%s' name='%s' dur=%.4f action=rename "
                      "to='%s' (same name, different duration -> keep both)",
                      ev.take.c_str(), ev.name.c_str(),
                      static_cast<double>(ev.duration),
                      ev.finalName.c_str());
    }
    return std::string(buf);
}

} // namespace assetio
