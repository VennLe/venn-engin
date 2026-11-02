#pragma once
// ============================================================
// editor/LevelPanel —— 关卡流送面板（G19 的界面那一半）
//
// 三块：
//   ① 清单概览（多少张、几张常驻、几张已加载、当前 viewer 在哪）；
//   ② 每张关卡一行（名字 / 路径 / 常驻 / 触发盒 / 状态 / 实体数）；
//   ③ 动词（Reload manifest / Load / Unload / Unload all）。
//
// ⚠ 面板**不持有** Scene / AssetManager，也不自己调 `loadSceneAdditive`：
//   加载要写进"运行态副本"，而那份场景由 `EditorApp` 管。所以动词走
//   `Action` 回调（由 EditorApp 注入），与按钮/命令同一条路 ——
//   "能点不能存"这类缺陷就是从两条路分叉开始的。
// ============================================================

#include <glm/vec3.hpp>

#include <functional>
#include <string>
#include <vector>

namespace scene {
class LevelStreamer;
}

namespace editor {

class LevelPanel {
public:
    using Action = std::function<void(const std::string& name)>;
    using PlainAction = std::function<void()>;

    void attach(scene::LevelStreamer* s) { m_streamer = s; }
    // 注入动词。`nullptr` 时按钮禁用（而不是静默什么都没发生）。
    void setActions(Action load, Action unload, PlainAction unloadAll,
                    PlainAction reload) {
        m_load = std::move(load);
        m_unload = std::move(unload);
        m_unloadAll = std::move(unloadAll);
        m_reload = std::move(reload);
    }
    void setStatus(const std::string& s) { m_status = s; }

    // ---- 命令入口（与按钮同一条路）----
    std::size_t count() const;
    std::size_t loadedCount() const;
    // 一行一条：`name=hub loaded=1 always=1 entities=12 path=...`
    // ⚠ 这是**给脚本读**的转储，不上屏（D 组 i18n-keep 同一条理由）。
    std::vector<std::string> rowsText() const;
    std::string stateLine() const;
    std::string statusText() const { return m_status; }

    void draw();

private:
    scene::LevelStreamer* m_streamer = nullptr;
    Action m_load;
    Action m_unload;
    PlainAction m_unloadAll;
    PlainAction m_reload;
    std::string m_status;
    std::string m_selected;
    bool m_showPaths = true;
};

} // namespace editor
