#pragma once
// ============================================================
// ui2/UiDocInput —— UI 文档的命中测试与点击派发（2026-11-02）
//
// ------------------------------------------------------------
// 它补的是什么洞
// ------------------------------------------------------------
// `UiDocument`（#312）让作者能在 `.ui.json` 里摆控件；`drawDocument` 把它们
// 画出来。但**没有任何命中测试** —— 文档里的按钮是纯装饰，点下去什么也
// 不会发生。而 `UiButton` + `UiFocus`（U2/U6）那套是有交互的，只是它
// 服务的是**暂停菜单那条硬编码的路径**，跟文档层完全不通。
//
// 这个文件把"文档层"接到同一条链上：命中 → 按下/松开 → 产出 action 字符串
// → 由宿主执行（引擎不执行，见 `UiAction.h`）。
//
// ------------------------------------------------------------
// 四条语义（每一条都是踩过的那种坑）
// ------------------------------------------------------------
// ① **最上面的赢**。`placeDocument` 输出的是 DFS 序（父在前、子在后），
//    所以命中测试**从后往前**扫 —— 后画的盖住先画的。反过来扫会让
//    "面板底下的按钮"抢走点击，而它明明被盖住了。
//
// ② **按下与松开必须在同一个控件上**。按下后拖出去再松开**不算点击**
//    （这是所有桌面 UI 的行为）。用一次 `hit==press` 判定，不做时间窗。
//
// ③ `visible == false` 与 `enabled == false` **都不命中**。前者由
//    `placeDocument` 直接跳过（不产出记录），后者在这里查。禁用还能命中
//    是最糟的一种 —— "点了没反应"会被误判成"按钮坏了"。
//
// ④ **注入的点击要排队**。控制台命令在首帧之前执行，那时既没有输出尺寸、
//    也没算过布局，当场做命中测试必然 miss，而 miss 会被误读成"按钮没画上"。
//    排队之后判定发生在"布局刚算完"的那一刻（与 `UiWidget.h` 的
//    `requestClick` 同一条理由）。
//
// ------------------------------------------------------------
// 日志锚点
// ------------------------------------------------------------
//   UIDOC-CLICK hit=0 x=<px> y=<py>            没点中（**也要打**：
//                                              否则"点歪了"和"按钮没画"分不清）
//   UIDOC-CLICK hit=1 id=<id> x= y=            点中了
//   UIDOC-NOACTION id=<id>                     点中了，但它没配 action
//                                              （**只打一次**，不刷屏）
// ============================================================

#include "ui2/UiDocument.h"

#include <string>

namespace ui2 {

// 当前的悬停 / 按下控件（给 `drawDocument` 调色用）。空串 = 没有。
struct UiDocInteraction {
    std::string hovered;
    std::string pressed;

    // ---- 拖放（P3-2）----
    // 正在被拖的控件 id（空 = 没在拖）。`drawDocument` 用它判断
    // "要不要画合法目标高亮"。
    std::string dragging;
    // 被拖控件的 `dragTag`（高亮判据的输入）。
    std::string draggingTag;
    // 指针**当前**落在哪个合法目标上（空 = 没有）。决定高亮是"待选"
    // 还是"命中"。
    std::string dropTarget;
    // 指针位置（输出像素）：拖拽中的"幽灵"画在这里。只在这两个字段
    // 都有意义时使用（dragging 非空）。
    float pointerX = 0.0f;
    float pointerY = 0.0f;
};

// 命中测试（输出像素）。返回最上面那个**可交互且 enabled** 的控件 id；
// 没有返回空串。
//
// `placed` 必须来自同一帧的 `placeDocument`（同一份 bindings）：自己再调
// 一次不但慢，还会在"绑定这一帧变了"的时候与绘制侧错位一格。
//
// ⚠ P3-2：**先逆变换再判矩形**。转了 12° 的卡牌用未旋转的 AABB 判定会出现
//   "点卡片外面也能选中 / 点卡片角上反而选不中"，而截图完全看不出来。
std::string hitTestDocument(const std::vector<UiDocPlaced>& placed, float px,
                            float py);

// 找"指针底下、接受 `tag` 的那个控件"（输出像素）。倒序扫（最上面的赢）。
// 空串 = 没有合法落点。**松手时也用它** —— 与高亮共用同一个判据。
std::string findDropTarget(const UiDocument& d,
                           const std::vector<UiDocPlaced>& placed, float px,
                           float py, const std::string& tag);

class UiDocInput {
public:
    // 一帧的结果：本帧**完成点击**的控件 + 它的 action 原文。
    struct Result {
        std::string id;       // 完成点击的控件 id（空 = 没有）
        std::string action;   // 该控件的 action 原文（可能为空 = 没配）

        // ---- 拖放（P3-2）----
        // ⚠ 拖与点是**互斥**的：一旦真的开始拖，同一帧的松开就**不再**
        //   产出点击（`id` 为空）。否则"把牌拖到敌人身上"会同时触发
        //   "点了一下这张牌" —— 卡牌游戏里那通常意味着"打出去"，于是
        //   一次拖拽结算了两次。
        bool dragBegan = false;
        bool dragEnded = false;     // 本帧松手（放没放进去都算）
        std::string dragId;         // 拖拽源
        std::string dragTag;        // 它携带的 tag
        std::string dropId;         // 落点（空 = 丢在空白 = 取消）
    };

    // 每帧一次。
    //   d            文档（要按 id 取回控件的 `action` 原文 / `dragTag`）
    //   px/py        指针位置（输出像素）
    //   down         本帧左键是否**处于按下状态**（用于 pressed 视觉）
    //   pressedEdge  本帧是否**刚按下**
    //   releasedEdge 本帧是否**刚松开**
    Result update(const UiDocument& d, const std::vector<UiDocPlaced>& placed,
                  float px, float py, bool down, bool pressedEdge,
                  bool releasedEdge);

    // 注入一次点击（**参考像素**，与 `ui.button` / 布局同一坐标系）。
    // 自动化用这个：脚本不必先知道视口多大就能点中"设计上那个位置"。
    // ⚠ 排队，由下一次 `update` 消费。
    void requestClickRef(float rx, float ry, uint32_t outW, uint32_t outH);

    const UiDocInteraction& state() const { return m_st; }
    void reset();

    // 正在拖的控件 id（空 = 没在拖）。宿主据此决定"要不要接管鼠标"。
    const std::string& draggingId() const { return m_dragId; }

    // 有排队的点击吗（宿主据此判断"这一帧要不要假装按了一下"）。
    bool hasPendingClick() const { return m_hasPending; }

private:
    UiDocInteraction m_st;
    // 按下时命中的控件。松开在同一控件上才算点击（语义 ②）。
    std::string m_pressId;
    // 已经为哪个 id 打过 UIDOC-NOACTION（不刷屏）
    std::string m_warnedNoAction;

    // 拖拽源（空 = 没在拖）与它携带的 tag。**与 m_pressId 分开存**：
    // 按下后如果命中的控件可以拖，m_pressId 会在拖拽结束时不参与点击判定。
    std::string m_dragId;
    std::string m_dragTag;

    bool m_hasPending = false;
    float m_pendingX = 0.0f;
    float m_pendingY = 0.0f;
};

}  // namespace ui2
