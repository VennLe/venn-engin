#pragma once
// ============================================================
// core/WindowGeometry —— 窗口几何的"落位 + 存档"
//
// 解决一个很实际的问题：编辑器每次打开都回到 1600x900 的左上角，用户
// 每次都得自己摆一次窗口。这里的策略是：
//
//   · **没有存档** → 主显示器工作区**居中**，宽高 = 工作区 * fraction
//                    （编辑器传 0.7 = "以显示器大小的 70% 打开"）
//   · **有存档**   → 用上次关闭时的位置与尺寸（含最大化状态）
//
// 存档是纯文本的 key=value（和 imgui.ini 一样放在工作目录），坏了或删了
// 最多就是回到"居中 70%"，不会影响启动。
//
// 为什么不直接交给 ImGui 的 ini：imgui.ini 记的是**面板**窗口，不是
// GLFW 的顶层窗口 —— 顶层窗口的位置/尺寸 GLFW 自己不管，必须我们自己存。
//
// 一个必须做的校验：**存档里的位置可能已经不在任何显示器上了**（换过
// 显示器、拔过外接屏、分辨率变过）。直接用的话窗口会开在屏幕外，用户
// 看不到也拖不回来。所以恢复前要求它至少有一部分落在某个显示器的工作区
// 里，否则退回"居中"。
// ============================================================

#include <string>

namespace core {

class Window;

// 把窗口摆到启动位置。
//   fraction      —— 无存档时占主显示器工作区的比例（0.7 = 70%）
//   stateFile     —— 存档文件名（相对当前工作目录）
//   fallbackW/H   —— 连显示器都问不到时的兜底尺寸（几乎不会用到）
//
// 必须在窗口创建之后调用；尺寸变化会让 Renderer 在下一帧重建交换链。
void applyStartupGeometry(Window& window, float fraction,
                          const std::string& stateFile, int fallbackWidth,
                          int fallbackHeight);

// 保存当前几何。全屏时保存的是进入全屏前的窗口几何 —— 否则下次启动
// 会拿全屏尺寸当窗口尺寸。
void saveWindowGeometry(const Window& window, const std::string& stateFile);

} // namespace core
