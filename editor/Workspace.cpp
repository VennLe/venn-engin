#include "Workspace.h"

#include "EditorContext.h"

#include "core/Logger.h"
#include "render/Renderer.h"

#include <cstring>

namespace editor {

namespace {

// ---------------------------------------------------------------- 面板清单
// 用 id 字符串而不是下标 —— 见头文件里的说明（下标错位是静默的）。
// 这些字符串必须与 PanelRegistry 表里的 id 完全一致。

// Level：默认。三个主面板（Content 已改成底栏抽屉，默认关着）。
const char* const kLevelOpen[] = {"hierarchy", "viewport", "inspector", nullptr};
const char* const kLevelClose[] = {"stats", "scripts", "engine", "console",
                                   nullptr};

// Rendering：大视口 + Engine 调参 + 统计 + GPU 时间轴。Hierarchy 让位。
// E25 把 GPU 时间轴归到这套工作区 —— "调渲染的人"正是要盯时戳的人。
const char* const kRenderOpen[] = {"viewport", "inspector", "engine", "stats",
                                   "gputimeline", nullptr};
const char* const kRenderClose[] = {"hierarchy", "scripts", "console",
                                    nullptr};

// Character：搭角色 / 摆位。层级 + 检查器 + 视口（Content 走底栏抽屉）。
const char* const kCharOpen[] = {"hierarchy", "viewport", "inspector", nullptr};
const char* const kCharClose[] = {"stats", "scripts", "engine", "console",
                                  nullptr};

// Animation：脚本 + 时间轴。Hierarchy 要开（选骨骼/节点）。
const char* const kAnimOpen[] = {"hierarchy", "viewport", "inspector", "scripts",
                                 nullptr};
const char* const kAnimClose[] = {"engine", "console", nullptr};

// Cinematic：几乎只剩视口 —— 看构图。
const char* const kCineOpen[] = {"viewport", "inspector", nullptr};
const char* const kCineClose[] = {"hierarchy", "content", "stats", "scripts",
                                  "engine", "console", nullptr};

// Debug：全开。视图模式切到 LightComplexity（簇内灯数热度图）。
const char* const kDebugOpen[] = {"viewport", "stats", "console", "hierarchy",
                                  "inspector", "engine", nullptr};
const char* const kDebugClose[] = {"scripts", nullptr};

} // namespace

const WorkspacePreset kWorkspaces[] = {
    // id          title         hint                                     ratio{根,左栏,右区}      布局                     viewMode  open          close
    {"level", "Level", "Level editing: hierarchy + content + viewport + inspector",
     {0.0f, 0.50f, 0.0f}, ViewportLayout::Single, 0, kLevelOpen, kLevelClose},
    {"rendering", "Rendering", "Render tuning: large viewport + Engine panel + stats",
     {0.0f, 0.50f, 0.0f}, ViewportLayout::Single, 0, kRenderOpen, kRenderClose},
    {"character", "Character", "Character: hierarchy + content browser + inspector",
     {0.0f, 0.50f, 0.0f}, ViewportLayout::TwoVertical, 0, kCharOpen,
     kCharClose},
    {"animation", "Animation", "Animation: scripts + hierarchy + inspector",
     {0.0f, 0.55f, 0.0f}, ViewportLayout::TwoHorizontal, 0, kAnimOpen,
     kAnimClose},
    {"cinematic", "Cinematic", "Cinematic: the viewport dominates, everything else collapsed",
     {0.10f, 0.50f, 1.0f}, ViewportLayout::Single, 0, kCineOpen, kCineClose},
    // Debug 的视图模式 = LightComplexity(7)：这是"调试视角"该有的样子，
    // 也顺带验证"工作区能记住视图模式"这条语义确实生效。
    {"debug", "Debug", "Debug: everything open + per-cluster light-count heatmap",
     {0.0f, 0.45f, 0.0f}, ViewportLayout::Quad, 7, kDebugOpen, kDebugClose},
};

const int kWorkspaceCount =
    static_cast<int>(sizeof(kWorkspaces) / sizeof(kWorkspaces[0]));

const char* viewportLayoutName(ViewportLayout l) {
    switch (l) {
        case ViewportLayout::Single: return "single";
        case ViewportLayout::TwoVertical: return "two_vertical";
        case ViewportLayout::TwoHorizontal: return "two_horizontal";
        case ViewportLayout::Quad: return "quad";
    }
    return "single";
}

int viewportLayoutCount() { return 4; }

int viewportSubviewCount(ViewportLayout l) {
    switch (l) {
        case ViewportLayout::Single: return 0;
        case ViewportLayout::TwoVertical:
        case ViewportLayout::TwoHorizontal: return 1;
        case ViewportLayout::Quad: return 3;
    }
    return 0;
}

void applyViewportLayout(EditorContext& ctx, render::Renderer& renderer,
                         ViewportLayout l) {
    ctx.setViewportLayout(l);
    const int sub = viewportSubviewCount(l);
    // Single = 只有主视口；其余布局每个格子各一个离屏目标，
    // 主视口用 0 号，剩下的都是"子视口"。
    renderer.setSubviewCount(static_cast<size_t>(sub));
    VK_LOG_INFO("VIEWPORT-LAYOUT %s (subviews=%d)", viewportLayoutName(l), sub);
}

int workspaceIndexOf(const char* id) {
    if (!id) return -1;
    for (int i = 0; i < kWorkspaceCount; ++i) {
        if (std::strcmp(kWorkspaces[i].id, id) == 0) return i;
    }
    return -1;
}

const char* workspaceId(int index) {
    if (index < 0 || index >= kWorkspaceCount) return "";
    return kWorkspaces[index].id;
}

const char* workspaceTitle(int index) {
    if (index < 0 || index >= kWorkspaceCount) return "";
    return kWorkspaces[index].title;
}

} // namespace editor
