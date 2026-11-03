#pragma once
// ============================================================
// editor/ProjectSettingsPanel —— 项目设置面板（2026-11-02）
//
// ------------------------------------------------------------
// 它补的是什么洞
// ------------------------------------------------------------
// `Edit ▸ Project Settings...` 在 2026-11-02 之前打开的是**画质档位**那张
// 面板（E26）。而真正的"项目设置"住在 `assets/game.json`：标题 / 分辨率 /
// 全屏 / 帧率上限 / 语言 / 默认场景 / UI 文档 / 音频出厂值 —— **全部只能
// 手改 JSON**。一个策划要改游戏标题，得先知道这个文件在哪。
//
// 这块面板就是 `game.json` 的 UI。读写层在 `project/GameFile.*`（那边负责
// 合并未知键、原子写、夹取），这里只负责"排成能改的样子"。
//
// ------------------------------------------------------------
// 五条刻意的设计
// ------------------------------------------------------------
// ① **落盘即真值**。面板不维护"第二份配置"：打开时读盘，按 Save 写回同一
//    个文件，Reload 重新读。改了一半不保存就退出 = 没改
//    （所以界面上必须常驻"未保存"标记，见 ③）。
//
// ② **按钮与命令走同一条路**。`save()` / `reload()` / `setField()` 是唯一
//    入口，面板按钮和控制台命令（`project.save` / `project.set`）都调它们。
//    两条路各写一份的后果是"能点不能存"或"命令改了界面不认"。
//
// ③ **"未保存"必须看得见**。改了任何一格 → `PROJ-DIRTY` 变成 unsaved，
//    标题栏带 `*`。不显示的话，用户按 Reload / 关窗口才发现白改了。
//
// ④ **夹取而不是拒绝**。越界值（宽 40 / 音量 2.5）在保存时夹回合法区间并
//    打 `PROJ-GAME-CLAMP`；面板上那一行同时显示"夹了什么"。拒绝保存会让
//    作者卡在一个不知道是哪一格坏了的状态。
//
// ⑤ **`quality` 只是标签**。真正的画质值在 `config/render.json`
//    （`QualitySettingsPanel.h` 第 ③ 条）。这里改它只影响"标签"，
//    界面上明写这一点，并给一个直达 Quality Settings 的按钮。
//
// ------------------------------------------------------------
// 自动化锚点（`tools/verify_project_settings.py` 用）
// ------------------------------------------------------------
//   PROJ-RECT            窗口矩形
//   PROJ-PATH            当前文件路径
//   PROJ-DIRTY           状态行（saved / unsaved / load-failed）
//   PROJ-SAVE / PROJ-RELOAD / PROJ-OPEN-QUALITY
//   PROJ-TITLE / PROJ-SCENE / PROJ-UIDOC / PROJ-STARTSTATE
//   PROJ-WIDTH / PROJ-HEIGHT / PROJ-MAXFPS / PROJ-UNFOCUSFPS / PROJ-TIMESCALE
//   PROJ-STEP-%s         数值行的 - / + 微调按钮（"-" 在后，"+"/"-" 各带后缀）
//   PROJ-FULLSCREEN / PROJ-LANG / PROJ-QUALITY
//   PROJ-AUDIO-%s        音频出厂值的每一行
//   PROJ-PLAYER-MODEL / -PICK / -CTRL / -CTRLPICK / -FITBODY
//   PROJ-PLAYER-IDLE / -WALK / -RUN / -JUMP / -NOTE
//                        玩家外观（2026-11-03）。`-PICK` / `-CTRLPICK` 是
//                        "从盘上挑一个"的按钮（下拉里第 0 项 = 清空）。
//   PROJ-NOTE            夹取 / 错误的提示行
//   PROJ-PUBLISH         发布摘要（只读）
// 日志锚点：
//   PROJ-GAME-LOAD / PROJ-GAME-SAVE / PROJ-GAME-CLAMP（在 project/GameFile）
//   PROJ-UI save / reload / set / set-bad（在本文件）
// ============================================================

#include "project/GameFile.h"

#include <string>
#include <vector>

namespace editor {

class ProjectSettingsPanel {
public:
    // 本帧发生了什么。EditorApp 取走它去 notify / 打日志。
    struct Result {
        bool saved = false;      // 这一帧真的写盘成功了
        bool reloaded = false;   // 这一帧真的重读了
        std::string message;     // 给用户看的一句话（可为空）
    };

    void draw();

    Result takeResult() {
        Result r = m_result;
        m_result = Result{};
        return r;
    }

    // ---- 给控制台命令用（按钮与命令共用这几个入口，见头注释 ②）----
    const project::GameSettings& settings() const { return m_st; }
    const std::string& path() const { return m_path; }
    bool loaded() const { return m_loaded; }
    bool dirty() const { return m_dirty; }
    const std::string& lastError() const { return m_err; }

    // 从盘重读（丢弃未保存改动）。返回是否成功。
    bool reload(std::string* msg);
    // 写盘。**唯一落盘点**。
    bool save(std::string* msg);
    // 改一个字段（键名见 `fieldNames()`，大小写不敏感）。返回 false = 不认识的键
    // 或值解析失败（`msg` 里说原因）。
    bool setField(const std::string& key, const std::string& value,
                  std::string* msg);
    // 可 set 的键（命令补全 / 判据里列清单 / 面板行顺序的唯一来源）
    static const char* const* fieldNames(int* count);
    // 读一个字段的当前值（**格式化后**的字符串，给 `project.settings` 读回用）
    std::string fieldValue(const std::string& key);

private:
    void loadIfNeeded();
    void syncBuffers();

    project::GameSettings m_st;
    // 上一次落盘 / 读盘时的快照。**"未保存"靠它比较得出** —— 不是靠每个
    // 控件置脏位（20 多个控件漏一个 = 改了却显示已保存 = 关窗口丢改动）。
    project::GameSettings m_saved;
    std::string m_path;
    std::string m_err;
    // 夹取 / 保存结果的提示（一行，显示在按钮下面）
    std::string m_note;
    bool m_loaded = false;
    bool m_dirty = false;
    Result m_result;

    // ImGui 的输入缓冲（`InputText` 要稳定地址）。与 `UiEditorWindow` 同款：
    // 加载 / 命令改值之后必须 `syncBuffers()`，否则界面上还是旧字符串。
    char m_titleBuf[128] = {};
    char m_sceneBuf[192] = {};
    char m_uiDocBuf[192] = {};
    char m_deviceBuf[96] = {};

    // ---- 玩家外观（2026-11-03）----
    // 模型 / 动画器两条路径允许**手填**（资产可能不在被扫描的那几个目录里），
    // 同时给一个下拉列出盘上真实存在的选项 —— 只给输入框的话，作者仍然得
    // 先知道 `assets/` 下面的相对路径怎么拼。
    char m_modelBuf[192] = {};
    char m_ctrlBuf[192] = {};
    char m_idleBuf[48] = {};
    char m_walkBuf[48] = {};
    char m_runBuf[48] = {};
    char m_jumpBuf[48] = {};
    std::vector<std::string> m_modelChoices;   // 相对 assets 根
    std::vector<std::string> m_animChoices;    // 相对 assets 根
    bool m_choicesScanned = false;
};

} // namespace editor
