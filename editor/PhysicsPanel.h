#pragma once
// ============================================================
// editor/PhysicsPanel —— 物理系统面板（底栏"+"托盘里勾出来的功能面板）
//
// 为什么需要它：物理的场景级参数（重力 / 固定步长 / 求解器迭代 / 休眠）
// 原来只能通过 Inspector → World 页或手改场景 JSON 来调，而 Play 期间
// **看不到任何运行时数字** —— 想知道"现在几个刚体在睡、最大穿透多少"
// 只能翻日志。这块面板两头都补上：
//
//   ① 运行态只读快照（EditorContext::PhysicsRuntimeInfo，由
//      EditorApp::stepPhysics 每帧写）—— 刚体数 / 休眠数 / 接触点 /
//      宽相配对数 / 最大穿透 / 最大速度 / 累计步数 / 仿真时间。
//   ② 场景级设置的真编辑（scene::PhysicsSettings）—— 与 Inspector 写
//      同一份存储，改完标脏，随场景一起序列化。
//
// 一条必须说清楚的语义：**设置只在下一次 Run 生效**。物理世界是在进入
// Play 的第一帧按当时的设置建起来的（见 EditorApp::buildPhysicsWorld），
// 跑到一半改重力不会影响已经建好的世界 —— 面板里对这一点有明确提示，
// 而不是让人怀疑"改了没反应"。
// ============================================================

#include "EditorContext.h"

namespace editor {

class PhysicsPanel {
public:
    explicit PhysicsPanel(EditorContext& ctx) : m_ctx(ctx) {}

    // 每帧绘制。显隐状态直接绑面板表里的 "physics" 那一格
    // （底栏托盘 / View 菜单 / Alt 快捷键 / 落盘 全部共用同一份）。
    void draw();

private:
    // 一条设置在两个数字之间夹一行小字说明的排版太啰嗦，统一走这个：
    // 标签左、控件右，宽度固定 —— 面板窄的时候也不会把控件挤没。
    void labelValue(const char* label, const char* tooltip);

    EditorContext& m_ctx;
};

} // namespace editor
