#pragma once
// ============================================================
// editor/ParticlesPanel —— 粒子系统面板（底栏"+"托盘里勾出来的功能面板）
//
// 定位很明确：**它不是 Inspector 的替代品**，而是"粒子这一摊的总览 + 入口"。
// Inspector 里的 Particles 页是**逐参数**的深度编辑（二十多个模块），
// 那块东西没法当总览用 —— 场景里挂了三四个发射器时，你得先在层级树里
// 找到它、再翻到那个页签。这块面板给的是：
//
//   ① 场景里**所有**发射器的清单（名字 / 发射率 / 寿命），点一行就选中，
//      Inspector 立刻跟过去；右键一行可以重命名 / 复制 / 删除；
//   ② 六个开箱即用的预设（火焰 / 烟 / 火花 / 雨 / 雪 / 闪电）—— 一键在原点
//      生成一个配好的发射器并选中，省掉"新建 + 挂组件 + 调二十个参数"；
//   ③ 全局设置（fx::ParticleSettings：总开关 / 亮度 / 尺寸 / 软化 / 模拟
//      频率 / 每发射器容量）—— 这些原来只能从 Engine 面板的 Debug 档里找。
//
// 一条纪律：**面板里出现的每个开关都必须真的接到渲染器**。所以清单和
// 预设之外，其余全部直接读写 render::Renderer 的 particleSettings()。
// ============================================================

#include "EditorContext.h"

#include "ecs/Entity.h"

namespace render {
class Renderer;
}

namespace editor {

class ParticlesPanel {
public:
    ParticlesPanel(EditorContext& ctx, render::Renderer& renderer)
        : m_ctx(ctx), m_renderer(renderer) {}

    void draw();

private:
    // 在原点造一个带粒子的实体，套用某个预设（下标见 kPresets）
    //
    // 命名：`<预设名> <序号>`（Fire 1 / Fire 2 / Rain 1 ……）。原来一律叫
    // "Particle Effect"，连开三个之后清单里三行一模一样 —— 用户原话：
    // "为什么名字都是一样的……初始名字应该和它的粒子特效的类型有关，并用
    // 编号数字后缀区分开来"。
    void addPreset(int preset);
    // 清单一行。返回 true = 这一行被点了（选中）。
    bool emitterRow(ecs::Entity e, const char* name, int index);
    // 「选中发射器」那一整段（模块化属性编辑）。
    // 只在选中的实体**真的带 ParticleEmitterComponent** 时才画；否则给一行
    // "先在清单里点一行"的提示。**参数一律直接读写该实体的组件** ——
    // 这是"Smoke 1 显示 Smoke 1 的配置"的唯一实现方式（2026-10-18 用户
    // 明确要求：不同类型/不同实例的参数必须各自独立，不能共用一份）。
    void drawSelectedEmitter(ecs::Entity e, ecs::ParticleEmitterComponent* fx);
    // 右键菜单里那三项（重命名开始 / 复制 / 删除）
    void beginRename(ecs::Entity e);
    void commitRename(ecs::Entity e);
    void duplicateEmitter(ecs::Entity e);
    void deleteEmitter(ecs::Entity e);

    EditorContext& m_ctx;
    render::Renderer& m_renderer;
    // 清单里选中行的"上一次日志签名"，只在变化时打一行（不刷爆日志）
    unsigned m_lastListSig = 0;
    // 「选中发射器」那一段的日志签名：id + 关键参数。只在变化时打一行
    // `FX-SEL id=.. rate=.. life=.. sprite=..`，自动化据此断言"面板显示的
    // 确实是这一个发射器的值"（比截图像素比对稳定得多）。
    unsigned m_selSig = 0;

    // ---- 重命名（行内编辑）----
    // 只允许一行处于编辑态：记 id 而不是下标（清单每帧重排，下标会指向别人）
    unsigned m_renameId = 0;
    bool m_renameFocus = false;      // 下一帧把键盘焦点塞进输入框
    char m_renameBuf[64] = {0};
};

} // namespace editor
