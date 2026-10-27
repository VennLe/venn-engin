#pragma once
// ============================================================
// editor/UiEditorWindow —— UI 编辑器（#312 的第三块：面板）
//
// #312 之前，"界面上有什么"只存在于 C++ 里：血条坐标写死在 `Hud.cpp`、
// 暂停菜单三个按钮写死在 `PauseMenu.cpp`、字幕框写死在 `Subtitle.cpp`。
// 策划能调的只有 `config/ui.json` 的 6 个数字。对二次元游戏来说，UI 的
// 工作量经常**大于 3D 部分** —— 这个窗口是"能不能做内容"的分界线。
//
//   ┌ UI editor — hud_demo *───────────────────────────────────────────┐
//   │ [ui/docs/hud_demo.ui.json] [Reload] [Save] [New]  1920x1080      │
//   ├────────────┬────────────────────────────────┬────────────────────┤
//   │ Widgets (6)│  ┌─ 设计画布（16:9）─────────┐ │ id    [hpbar]      │
//   │ [+][Dup]   │  │  ┏━━━━━━━━━━━━┓           │ │ type  progress     │
//   │ [Del][▲][▼]│  │  ┃ hud 面板    ┃           │ │ anchor [3×3 九宫格]│
//   │ ▾ hud      │  │  ┗━━━━━━━━━━━━┛           │ │ rect x y w h       │
//   │    hpbar   │  │      ▣ ← 锚点标记          │ │ color / radius     │
//   │    hpnum   │  └────────────────────────────┘ │ text / bind        │
//   ├────────────┴────────────────────────────────┴────────────────────┤
//   │ check: ok — 6 widgets, 0 warnings                                │
//   └──────────────────────────────────────────────────────────────────┘
//
// ------------------------------------------------------------
// 六条"省了就错"的设计约束
// ------------------------------------------------------------
// ⚠⚠ ① **校验不在这里实现**。每次改动都是"`toJson` → 丢回
//     `parseUiDocument`"，用的是运行时同一个解析器。自己再写一套"顺手检查
//     一下"的后果是两套规则分叉，症状是"编辑器说没问题，进 Play 一片空白"。
//     （与剧本/时间轴编辑器同一条纪律。）
//
// ⚠⚠ ② **读盘失败 → 只读**。`parseUiDocument` 边解析边填，半路出错时
//     `widgets` 里是前几个控件的残片。此时按 Save 会把 40 个控件的界面
//     截成 3 个 —— 而且**看不出来**（文件结构仍然合法）。
//
// ⚠⚠ ③ **拖锚点要保持屏幕位置不变**。锚点决定"x/y 相对谁量"，换锚点却不
//     重算 x/y 的话，控件会瞬间跳到自己宽高那么远的地方 —— 作者以为拖坏了。
//     所以换锚点走 `actSetAnchorKeepRect`：算出新的等价 x/y，屏幕上**一个
//     像素都不动**。这条是 UI 编辑器与"手写 JSON"最大的体验差别之一。
//
// ⚠⚠ ④ **拖动中不改锚点/尺寸，只改 x/y**。拖边框改尺寸要额外的命中区域与
//     手势状态，第一版**不做**（作者用 w/h 数字框）。做了但半吊子的手柄
//     比没有更糟：作者会以"拖了没反应"的方式发现它坏了。
//
// ⚠⚠ ⑤ **画布缩放与 `ui.scale` 无关**。设计画布上的缩放由面板自己算
//     （`canvasW / refW`），走 `placeDocumentAt(doc, b, canvas, scale)`。
//     用 `placeDocument`（内部乘 `computeScale`）的话，作者在 Inspector 里
//     调了全局 UI 缩放，画布上的控件位置会跟着变 —— 那不是"设计视图"。
//
// ⚠⚠ ⑥ **不做运行时预览**。和剧本编辑器同一条：`Game.exe` 才有游戏 UI 的
//     输出 pass。这个面板给的是"文档解析得过 + 几何算得对"，真正的像素
//     验证交给 `tools/verify_ui_editor.py` 的运行时那一段。
//
// ------------------------------------------------------------
// 为什么"在视口里拖"落在**设计画布**上，而不是 3D 视口
// ------------------------------------------------------------
// 3D 视口里拖 UI 需要：① 相机投影 → 屏幕 → 设计像素的三段换算；② 一条
// 反向通道把拖拽结果写回资产；③ 与 gizmo / 选中 / picking 三层互斥。
// 而 UI 编辑 90% 的时间是"对着 16:9 的稿子摆位置"，3D 背景反而是干扰。
// 所以这一版把画布做成面板里的独立 16:9 区域 —— **同一个坐标系、同一套
// 拖拽逻辑**，等 `U9` 的视口叠加落地时，这段代码整体搬过去即可。
// ============================================================

#include "ui2/UiDocument.h"
#include "ui2/UiLayout.h"

#include <string>
#include <vector>

namespace editor {

class EditorContext;

class UiEditorWindow {
  public:
    explicit UiEditorWindow(EditorContext& ctx);

    // 打开并读一份 `.ui.json`。`relPath` 为空 = 空手开窗（只剩 "New"）。
    // ⚠ 读失败也**开窗**：把错误摆出来，比"点了没反应"有用。
    bool open(const std::string& relPath);
    void draw(bool& open);

    bool isOpen() const { return m_open; }
    void close();
    bool dirty() const { return m_dirty; }

  private:
    // ---------------------------------------------------------- 数据
    ui2::UiDocument m_doc;
    std::string m_path;
    std::string m_error;      // 读/写盘的人话错误
    std::string m_uiError;    // 最近一次被拒的操作原因
    bool m_open = false;
    bool m_loaded = false;
    bool m_dirty = false;
    bool m_readOnly = false;
    bool m_injected = false;

    // ---------------------------------------------------------- 校验
    bool m_checkValid = false;
    std::string m_checkError;
    std::vector<std::string> m_checkWarnings;
    std::string m_checkLogKey;
    void revalidate();

    // ---------------------------------------------------------- 选择
    // ⚠ 存**路径**而不是指针：`add` / `del` / `move` 会让指针悬垂，而
    //   ImGui 的 activeId 还记着那些控件。路径在下一次操作时重新解析，
    //   找不到就退到"什么都没选"。
    std::string m_sel;                 // "hud/hpbar"

    ui2::UiWidget* sel();
    const ui2::UiWidget* sel() const;
    // 选中项的父路径（顶层 = 空串）
    static std::string parentPathOf(const std::string& path);
    static std::string leafOf(const std::string& path);

    // ---------------------------------------------------------- 编辑缓冲
    // 与另两个编辑器同一条纪律：不让 InputText 直接写资产里的 std::string。
    // 缓冲按"选中路径"失效重建。
    std::string m_bufKey;
    char m_idBuf[128] = {};
    char m_textBuf[512] = {};
    char m_bindBuf[128] = {};
    char m_bindMaxBuf[128] = {};
    char m_pathBuf[512] = {};
    void syncBuffers();

    // ---------------------------------------------------------- 画布
    // 设计画布的几何（ImGui 屏幕坐标）。`scale` = 屏幕像素 / 参考像素。
    // ⚠ 刻意不用 `ImVec2`：面板头文件不引入 imgui.h —— 它被 EditorApp.h
    //   间接包含，而 imgui.h 一进来就会和 `ImGuiChildFlags` 之类的版本宏
    //   打架（#311 踩过一次，见 Memory）。
    struct CanvasGeom {
        float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
        float scale = 1.0f;
        bool valid() const { return x1 > x0 && y1 > y0 && scale > 0.0f; }
    };
    struct Pt {
        float x = 0.0f, y = 0.0f;
    };
    CanvasGeom m_canvas;
    // 拖动状态：0=无 1=拖控件 2=拖锚点。⚠ 成员而不是函数内 static ——
    // 关窗换文档时残留的拖拽状态会让下一次点击"接着上次拖"。
    int m_dragMode = 0;
    std::string m_dragId;

    // 参考像素空间下每个控件的矩形（设计画布与"换锚点保位置"共用）
    std::vector<ui2::UiDocPlaced> designRects() const;
    // 某控件的父矩形（参考像素）。顶层返回画布矩形。
    ui2::UiRect parentRectOf(const std::string& path) const;
    // 锚点标记在参考像素空间的位置
    Pt anchorMarkOf(const std::string& path) const;

    // ---------------------------------------------------------- 动作
    // UI 按钮与注入脚本**共用**这一份（同另两个编辑器）。
    bool actNew();                          // 一块面板 + 一行文字的骨架
    bool actAdd(ui2::UiWidgetType type);     // 在选中项之后加同级
    bool actDuplicate();
    bool actDelete();
    bool actMove(int dir);                   // 同级内上/下移
    bool actSelect(const std::string& path);
    bool actSetId(const std::string& id);
    bool actSetPos(float x, float y);
    bool actSetSize(float w, float h);
    // 换锚点并**保持屏幕矩形不变**（见 ③）
    bool actSetAnchorKeepRect(ui2::UiAnchor a);
    bool actSetColor(uint32_t c);            // 主色
    bool actSetColor2(uint32_t c);           // 副色
    bool actSetRadius(float r);
    bool actSetInset(float v);
    bool actSetText(const std::string& t);
    bool actSetFontSize(float s);
    bool actSetAlign(int a);
    bool actSetBind(const std::string& key);
    bool actSetBindMax(const std::string& key);
    bool actSetValue(float v);
    bool actSetVisible(bool v);
    // 拖动（画布）：把选中控件挪到参考像素 (x,y)
    bool actDragTo(float x, float y);

    bool save(bool force);
    void reload();

    std::string uniqueId(const std::string& base) const;
    // id 是否已被**全树**用掉（`exceptPath` = 允许撞自己的那条路径）。
    // ⚠ 必须全树查：`parseUiDocument` 要求跨层唯一，所以"本层没有"不等于
    //   能用 —— 只查一层的话，加一个子控件就会让整份文档变成 `duplicate id`，
    //   而作者的第一反应是"编辑器把我文件写坏了"。
    bool idUsed(const std::string& id, const std::string& exceptPath) const;
    void markDirty();
    bool reject(const char* op, const std::string& reason);

    // ---------------------------------------------------------- 绘制
    void drawToolbar(bool& open);
    void drawTreePanel(float height);
    void drawTreeNode(const std::string& path, ui2::UiWidget& w);
    void drawCanvas(float height);
    void drawInspector(float height);
    void drawValidation();

    // ---------------------------------------------------------- 自动化
    // `MYVK_EDITOR_UIED_SCRIPT="sel:hud/hpbar;setpos:40,40;save"`，只跑一次。
    void runInjectedScript();

    EditorContext& m_ctx;
};

}  // namespace editor
