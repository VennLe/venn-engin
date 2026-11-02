#pragma once
// ============================================================
// editor/FxPanel —— 特效面板（P1）
//
// 三件事：
//   ① 看池子状态（预算 / 活跃 / 累计播放 / 丢弃数 / 缓存资产数）
//   ② 点按钮直接播一个 `.veffect`（不用进 Play、不用挂动画）
//   ③ 清空（怀疑"是不是有特效卡住没回收"时按一下）
//
// 为什么需要它（而不是"特效自己会播就行"）：
//   · **调参**：改 `.veffect` 里的 burst 数量 / 颜色，一键重播看效果，
//     不用搭一个能触发的场景再跑过去打怪；
//   · **诊断**：`dropped` 不为 0 = 池满，玩家看到的是"某些命中没火花"，
//     而这件事在画面里**看不出来**（你没法知道"这里本该有一个特效"）。
// ============================================================

#include "fx/EffectService.h"

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace editor {

class FxPanel {
public:
    void attach(fx::EffectService* svc) { m_fx = svc; }
    void draw();

    // ---- 命令入口（与按钮走同一条路）----
    std::string stateLine() const;
    uint32_t playPath(const std::string& path, const glm::vec3& at,
                      std::string* err);
    void stopAll();
    bool setBudget(int n, std::string* err);

    // 面板里列出的"常用特效"（内容来自 assets/fx/ 的约定命名）。
    // 写成常量而不是扫目录：扫目录会在打包后（`pak:`）失败，
    // 而面板是开发期工具，列几个已知的就够。
    static const std::vector<std::string>& presets();

private:
    fx::EffectService* m_fx = nullptr;
    float m_lastPlayX = 0.0f;
    float m_lastPlayY = 0.0f;
    float m_lastPlayZ = 1.0f;
    std::string m_status;
};

} // namespace editor
