// ============================================================
// nav/NavSerialize.cpp
// ============================================================

#include "nav/NavSerialize.h"

#include <cmath>
#include <string>

namespace nav {

using json = nlohmann::json;

namespace {
bool near(float a, float b) { return std::fabs(a - b) < 1e-6f; }
} // namespace

json navAgentToJson(const ecs::NavAgentComponent& a) {
    json j = json::object();
    if (!near(a.speed, 2.0f)) j["speed"] = a.speed;
    if (!near(a.turnSpeed, 8.0f)) j["turn"] = a.turnSpeed;
    if (!near(a.arriveDist, 0.30f)) j["arrive"] = a.arriveDist;
    if (!near(a.repathInterval, 0.5f)) j["repath"] = a.repathInterval;
    if (!a.enabled) j["enabled"] = false;
    if (!a.faceMoveDir) j["faceMove"] = false;
    if (!a.smooth) j["smooth"] = false;
    if (a.autoStart) j["autoStart"] = true;
    if (a.hasGoal) {
        json g = json::array();
        g.push_back(a.goal.x);
        g.push_back(a.goal.y);
        g.push_back(a.goal.z);
        j["goal"] = std::move(g);
    }
    // 空块也返回（不是 null）—— "有 `nav` 这个键" 就是"这实体是代理"，
    // 返回 null 会让一个纯默认值的代理存盘后**消失**。
    return j;
}

bool navAgentFromJson(const json& j, ecs::NavAgentComponent& out) {
    if (!j.is_object()) return false;
    out = ecs::NavAgentComponent{};
    if (j.contains("speed") && j["speed"].is_number()) out.speed = j["speed"].get<float>();
    if (j.contains("turn") && j["turn"].is_number()) out.turnSpeed = j["turn"].get<float>();
    if (j.contains("arrive") && j["arrive"].is_number()) out.arriveDist = j["arrive"].get<float>();
    if (j.contains("repath") && j["repath"].is_number())
        out.repathInterval = j["repath"].get<float>();
    if (j.contains("enabled") && j["enabled"].is_boolean()) out.enabled = j["enabled"].get<bool>();
    if (j.contains("faceMove") && j["faceMove"].is_boolean())
        out.faceMoveDir = j["faceMove"].get<bool>();
    if (j.contains("smooth") && j["smooth"].is_boolean()) out.smooth = j["smooth"].get<bool>();
    if (j.contains("autoStart") && j["autoStart"].is_boolean())
        out.autoStart = j["autoStart"].get<bool>();
    if (j.contains("goal") && j["goal"].is_array() && j["goal"].size() == 3) {
        out.goal = glm::vec3(j["goal"][0].get<float>(), j["goal"][1].get<float>(),
                             j["goal"][2].get<float>());
        out.hasGoal = true;
    }
    // ⚠ 防御性收尾：负数在这里没有意义，但让它们过会让"速度 -2"变成
    //   "永远到不了点"（每次移动都是负的），而日志里一切正常。
    if (out.speed < 0.0f) out.speed = 0.0f;
    if (out.turnSpeed < 0.0f) out.turnSpeed = 0.0f;
    if (out.arriveDist < 0.0f) out.arriveDist = 0.0f;
    if (out.repathInterval < 0.0f) out.repathInterval = 0.0f;
    return true;
}

json navGridToJson(const NavGrid& g) {
    json j = json::object();
    j["version"] = 1;
    j["origin"] = json::array({g.origin.x, g.origin.y, g.origin.z});
    j["cell"] = g.cellSize;
    j["cols"] = g.cols;
    j["rows"] = g.rows;
    j["maxStep"] = g.maxStep;
    std::string bits;
    bits.resize(g.cell.size());
    for (std::size_t i = 0; i < g.cell.size(); ++i) bits[i] = g.cell[i] ? '1' : '0';
    j["cells"] = std::move(bits);
    // 高度保留 3 位小数：毫米级够用，而全精度浮点会把文件撑大一倍、
    // 也会让"同一个网格存两次"逐字节不同（git 里全是噪音）。
    // ⚠ 不要 `json::reserve`：`nlohmann::json` 没有这个成员（只有底层
    //   `array_t` 有）。这个错误在别的 TU 里编译得过，一到这里就 C2039。
    json hs = json::array();
    for (float h : g.height) hs.push_back(std::round(h * 1000.0f) / 1000.0f);
    j["height"] = std::move(hs);
    return j;
}

bool navGridFromJson(const json& j, NavGrid& out, std::string& err) {
    err.clear();
    if (!j.is_object()) {
        err = "root is not an object";
        return false;
    }
    if (!j.contains("version") || !j["version"].is_number_integer()) {
        err = "missing version";
        return false;
    }
    if (j["version"].get<int>() > 1) {
        err = "version " + std::to_string(j["version"].get<int>()) + " is newer than 1";
        return false;
    }
    if (!j.contains("cols") || !j.contains("rows") || !j["cols"].is_number_integer() ||
        !j["rows"].is_number_integer()) {
        err = "missing cols/rows";
        return false;
    }
    const int cols = j["cols"].get<int>();
    const int rows = j["rows"].get<int>();
    if (cols <= 0 || rows <= 0) {
        err = "cols/rows must be > 0";
        return false;
    }
    if (!j.contains("cell") || !j["cell"].is_number()) {
        err = "missing cell";
        return false;
    }
    const float cell = j["cell"].get<float>();
    if (cell <= 1e-4f) {
        err = "cell must be > 0";
        return false;
    }
    if (!j.contains("origin") || !j["origin"].is_array() || j["origin"].size() != 3) {
        err = "missing origin";
        return false;
    }
    if (!j.contains("cells") || !j["cells"].is_string()) {
        err = "missing cells bitmap";
        return false;
    }
    const std::string bits = j["cells"].get<std::string>();
    const std::size_t n = static_cast<std::size_t>(cols) * rows;
    if (bits.size() != n) {
        // ⚠ 这一条是**尺寸纪律**：格子数与位图对不上时，"第 i 格"就不再有
        //   意义 —— 硬塞进去会让网格看起来正常、路径却乱穿墙。
        err = "bitmap size " + std::to_string(bits.size()) + " != cols*rows " + std::to_string(n);
        return false;
    }
    if (!j.contains("height") || !j["height"].is_array() || j["height"].size() != n) {
        err = "height array size mismatch";
        return false;
    }

    out = NavGrid{};
    out.origin = glm::vec3(j["origin"][0].get<float>(), j["origin"][1].get<float>(),
                           j["origin"][2].get<float>());
    out.cellSize = cell;
    out.cols = cols;
    out.rows = rows;
    out.maxStep = j.contains("maxStep") && j["maxStep"].is_number()
                      ? j["maxStep"].get<float>()
                      : 0.4f;
    out.cell.assign(n, 0u);
    out.height.assign(n, 0.0f);
    for (std::size_t i = 0; i < n; ++i) {
        // 只认 '1' 为可走：写坏了（'2' / ' ' / '\0'）一律当 Blocked ——
        // 反过来（非 '0' 即可走）会把"位图被截断"变成"整张图乱走"。
        out.cell[i] = (bits[i] == '1') ? 1u : 0u;
        out.height[i] = j["height"][i].get<float>();
    }
    return out.valid();
}

} // namespace nav
