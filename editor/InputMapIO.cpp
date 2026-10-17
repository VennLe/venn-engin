#include "InputMapIO.h"

#include "assets/AssetPath.h"
#include "core/InputMap.h"
#include "core/Logger.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace editor {

namespace {

// 与 EditorLayoutIO::editorLayoutAutomationRun 同一判据 —— 刻意不共享那个
// 函数：它住在 layout 的 TU 里，语义是"布局的自动化判定"，绑在一起会
// 让以后改布局规则时莫名其妙改动键位的读盘行为。
bool automationRun() {
    return std::getenv("MYVK_FRAMES") != nullptr ||
           std::getenv("MYVK_NO_WINDOW_SAVE") != nullptr;
}

// 默认资产名（相对资产根）。**扩展名放进名字里**：resolveAssetPath 只认
// "相对资产根的文件名"，不接受目录。
constexpr const char* kDefaultName = "input_map.json";

} // namespace

bool editorInputMapPathExplicit() {
    const char* p = std::getenv("MYVK_EDITOR_INPUT_MAP");
    return p != nullptr && p[0] != '\0';
}

std::string editorInputMapPath() {
    if (const char* p = std::getenv("MYVK_EDITOR_INPUT_MAP")) {
        if (p[0] != '\0') return p;   // 脚本指定 → 原样用，不做资产根解析
    }
    // ⚠ 走资产的**同一套解析**（MYVK_ASSET_DIR > exe/assets > cwd/assets）：
    //   面板写出去的那份必须就是 Game.exe 读到的那份。若在这里写死
    //   "assets/input_map.json"，从 build/bin 启动的编辑器会把文件写到
    //   仓库根，而运行时从 exe/assets 找 —— "改完不生效"。
    return assets::resolveAssetPath(kDefaultName);
}

bool editorInputMapFileExists() {
    std::error_code ec;
    return std::filesystem::exists(editorInputMapPath(), ec) && !ec;
}

bool loadEditorInputMap(core::InputMap& map) {
    if (automationRun() && !editorInputMapPathExplicit()) {
        // 残留的绑定表会把自动化跑出的行为改掉（同布局的理由）。
        return false;
    }
    const std::string path = editorInputMapPath();
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) return false;
    if (!map.loadFromFile(path)) return false;   // 内部已打 WARN
    VK_LOG_INFO("INS-BIND loaded '%s' (%s)", path.c_str(),
                map.describe().c_str());
    return true;
}

bool saveEditorInputMap(const core::InputMap& map) {
    if (automationRun() && !editorInputMapPathExplicit()) {
        VK_LOG_WARN("INS-BIND save refused: automation run on the default "
                    "path (set MYVK_EDITOR_INPUT_MAP to opt in)");
        return false;
    }
    const std::string path = editorInputMapPath();
    std::error_code ec;
    const std::filesystem::path parent =
        std::filesystem::path(path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);
    if (!map.saveToFile(path)) return false;   // 内部已打 WARN
    // 这行是回归脚本的锚点（**写成功**与**文件在哪**一次给全）。
    VK_LOG_INFO("INS-BIND saved '%s' (%s)", path.c_str(),
                map.describe().c_str());
    return true;
}

} // namespace editor
