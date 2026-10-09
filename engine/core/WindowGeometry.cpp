#include "core/WindowGeometry.h"

#include "core/Logger.h"
#include "core/Window.h"

#include <algorithm>
#include <fstream>
#include <string>

namespace core {

namespace {

// 一个窗口至少要露出这么多像素，才算"用户还能看到并拖回来"
constexpr int kMinVisibleW = 120;
constexpr int kMinVisibleH = 40;
// 尺寸下限：比这再小就没法用了（也防住存档里的垃圾数字）
constexpr int kMinWindowW = 320;
constexpr int kMinWindowH = 240;

struct Geometry {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    bool maximized = false;
};

// 窗口矩形是否落在某个显示器的工作区里（多显示器也算）。
// 问不到任何显示器信息时保守放行 —— 宁可位置可能不好，也不要莫名不生效。
bool visibleOnSomeMonitor(int x, int y, int w, int h) {
    int count = 0;
    GLFWmonitor** monitors = glfwGetMonitors(&count);
    if (!monitors || count <= 0) return true;

    for (int i = 0; i < count; ++i) {
        int mx = 0, my = 0, mw = 0, mh = 0;
        glfwGetMonitorWorkarea(monitors[i], &mx, &my, &mw, &mh);
        if (mw <= 0 || mh <= 0) continue;

        const int ix0 = std::max(x, mx);
        const int iy0 = std::max(y, my);
        const int ix1 = std::min(x + w, mx + mw);
        const int iy1 = std::min(y + h, my + mh);
        if (ix1 - ix0 >= kMinVisibleW && iy1 - iy0 >= kMinVisibleH) return true;
    }
    return false;
}

// 读存档。文件不存在 / 缺字段 / 数字不合理 → false（调用方会退回默认落位）
bool loadGeometry(const std::string& path, Geometry& out) {
    std::ifstream f(path);
    if (!f) return false;

    bool haveW = false, haveH = false;
    std::string line;
    while (std::getline(f, line)) {
        // 容忍 CRLF 与两侧空白
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' ||
                                 line.back() == '\t')) {
            line.pop_back();
        }
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        const std::string val = line.substr(eq + 1);

        // 解析失败就保留上一个值（x / y 失败 = 当作 0，无害）
        auto toInt = [&val](int& dst) {
            try {
                dst = std::stoi(val);
            } catch (...) {
            }
        };

        if (key == "w") {
            toInt(out.w);
            haveW = true;
        } else if (key == "h") {
            toInt(out.h);
            haveH = true;
        } else if (key == "x") {
            toInt(out.x);
        } else if (key == "y") {
            toInt(out.y);
        } else if (key == "maximized") {
            out.maximized = (val == "1");
        }
    }
    return haveW && haveH && out.w >= kMinWindowW && out.h >= kMinWindowH;
}

} // namespace

void applyStartupGeometry(Window& window, float fraction,
                          const std::string& stateFile, int fallbackWidth,
                          int fallbackHeight) {
    int mx = 0, my = 0, mw = 0, mh = 0;
    const bool haveMonitor = window.primaryMonitorWorkArea(&mx, &my, &mw, &mh);

    // ---- 默认落位：工作区居中 + 占 fraction ----
    Geometry g;
    if (haveMonitor) {
        g.w = std::max(kMinWindowW, static_cast<int>(mw * fraction));
        g.h = std::max(kMinWindowH, static_cast<int>(mh * fraction));
        g.x = mx + (mw - g.w) / 2;
        g.y = my + (mh - g.h) / 2;
    } else {
        g.w = fallbackWidth;
        g.h = fallbackHeight;
        g.x = 60;
        g.y = 60;
    }

    // ---- 有存档就用存档（前提是它还在屏幕上）----
    Geometry saved;
    const bool used = loadGeometry(stateFile, saved) &&
                      visibleOnSomeMonitor(saved.x, saved.y, saved.w, saved.h);
    if (used) {
        g = saved;
        // 换过显示器的话存档可能比当前工作区还大 → 收进工作区
        if (haveMonitor) {
            g.w = std::min(g.w, mw);
            g.h = std::min(g.h, mh);
            // 位置也收一下，保证标题栏区域可见（能拖回来）
            g.x = std::clamp(g.x, mx - (g.w - kMinVisibleW), mx + mw - kMinVisibleW);
            g.y = std::clamp(g.y, my, my + mh - kMinVisibleH);
        }
    }

    window.setWindowSize(g.w, g.h);
    window.setWindowPos(g.x, g.y);
    if (g.maximized) window.maximize();

    if (used) {
        VK_LOG_INFO("Window geometry: restored %d x %d at (%d, %d)%s from %s",
                    g.w, g.h, g.x, g.y, g.maximized ? " [maximized]" : "",
                    stateFile.c_str());
    } else {
        VK_LOG_INFO("Window geometry: default %.0f%% of monitor -> %d x %d at "
                    "(%d, %d)%s",
                    static_cast<double>(fraction) * 100.0, g.w, g.h, g.x, g.y,
                    haveMonitor ? "" : " (no monitor info, fallback size)");
    }
}

void saveWindowGeometry(const Window& window, const std::string& stateFile) {
    int x = 0, y = 0, w = 0, h = 0;
    bool maxed = false;
    window.windowedGeometry(&x, &y, &w, &h, &maxed);
    if (w < kMinWindowW || h < kMinWindowH) return;  // 最小化之类的坏值，不写

    std::ofstream f(stateFile, std::ios::trunc);
    if (!f) {
        VK_LOG_WARN("Window geometry: cannot write %s", stateFile.c_str());
        return;
    }
    f << "# Venn window geometry (auto-saved; delete to reset)\n"
      << "x=" << x << "\n"
      << "y=" << y << "\n"
      << "w=" << w << "\n"
      << "h=" << h << "\n"
      << "maximized=" << (maxed ? 1 : 0) << "\n";
    VK_LOG_INFO("Window geometry: saved %d x %d at (%d, %d)%s -> %s", w, h, x, y,
                maxed ? " [maximized]" : "", stateFile.c_str());
}

} // namespace core
