#include "core/InputMap.h"

#include "core/Input.h"
#include "core/Logger.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace core {

namespace {

const char* sourceName(InputSource s) {
    switch (s) {
        case InputSource::Key: return "key";
        case InputSource::MouseButton: return "mouse";
        case InputSource::GamepadButton: return "padButton";
        case InputSource::GamepadAxis: return "padAxis";
    }
    return "key";
}

InputSource sourceFromName(const std::string& s) {
    if (s == "mouse") return InputSource::MouseButton;
    if (s == "padButton") return InputSource::GamepadButton;
    if (s == "padAxis") return InputSource::GamepadAxis;
    return InputSource::Key;
}

// 只给"日志与重绑定 UI 的提示"用。数量不追求全，认不出来的退化成 Key<N>
// —— 界面上仍然能区分，也不会因为漏一个键名而误判。
const char* keyName(int code) {
    switch (code) {
        case GLFW_KEY_SPACE: return "Space";
        case GLFW_KEY_ESCAPE: return "Esc";
        case GLFW_KEY_ENTER: return "Enter";
        case GLFW_KEY_TAB: return "Tab";
        case GLFW_KEY_LEFT_SHIFT: return "LShift";
        case GLFW_KEY_LEFT_CONTROL: return "LCtrl";
        case GLFW_KEY_LEFT_ALT: return "LAlt";
        case GLFW_KEY_UP: return "Up";
        case GLFW_KEY_DOWN: return "Down";
        case GLFW_KEY_LEFT: return "Left";
        case GLFW_KEY_RIGHT: return "Right";
        case GLFW_KEY_A: return "A";
        case GLFW_KEY_B: return "B";
        case GLFW_KEY_C: return "C";
        case GLFW_KEY_D: return "D";
        case GLFW_KEY_E: return "E";
        case GLFW_KEY_F: return "F";
        case GLFW_KEY_G: return "G";
        case GLFW_KEY_H: return "H";
        case GLFW_KEY_I: return "I";
        case GLFW_KEY_J: return "J";
        case GLFW_KEY_K: return "K";
        case GLFW_KEY_L: return "L";
        case GLFW_KEY_M: return "M";
        case GLFW_KEY_N: return "N";
        case GLFW_KEY_O: return "O";
        case GLFW_KEY_P: return "P";
        case GLFW_KEY_Q: return "Q";
        case GLFW_KEY_R: return "R";
        case GLFW_KEY_S: return "S";
        case GLFW_KEY_T: return "T";
        case GLFW_KEY_U: return "U";
        case GLFW_KEY_V: return "V";
        case GLFW_KEY_W: return "W";
        case GLFW_KEY_X: return "X";
        case GLFW_KEY_Y: return "Y";
        case GLFW_KEY_Z: return "Z";
        case GLFW_KEY_1: return "1";
        case GLFW_KEY_2: return "2";
        case GLFW_KEY_3: return "3";
        case GLFW_KEY_4: return "4";
        default: break;
    }
    return nullptr;
}

std::string describeOne(const InputBinding& b) {
    std::string out;
    if (b.ctrl) out += "Ctrl+";
    if (b.shift) out += "Shift+";
    if (b.alt) out += "Alt+";
    switch (b.source) {
        case InputSource::Key: {
            const char* n = keyName(b.code);
            if (n) return out + n;
            char buf[24];
            std::snprintf(buf, sizeof(buf), "Key%d", b.code);
            return out + buf;
        }
        case InputSource::MouseButton:
            return out + "Mouse" + std::to_string(b.code);
        case InputSource::GamepadButton:
            return out + "PadBtn" + std::to_string(b.code);
        case InputSource::GamepadAxis:
            return out + (b.scale < 0.0f ? "PadAxis-" : "PadAxis+") +
                   std::to_string(b.code);
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------- 构造

InputMap::InputMap() {
    // 出厂就有 Gameplay 这一层。栈先不压 —— 由调用方决定何时"进入游戏"，
    // 这样"没进游戏时任何动作都不成立"是默认行为（而不是默认全开）。
    ensureContext("Gameplay");
}

InputMap InputMap::makeDefault() {
    InputMap m;
    m.ensureContext("Gameplay");

    // ---- 移动（WASD + 手柄左摇杆）----
    m.defineAction("Gameplay", "MoveForward");
    m.defineAction("Gameplay", "MoveBack");
    m.defineAction("Gameplay", "MoveRight");
    m.defineAction("Gameplay", "MoveLeft");
    m.bindKey("Gameplay", "MoveForward", GLFW_KEY_W);
    m.bindKey("Gameplay", "MoveBack", GLFW_KEY_S);
    m.bindKey("Gameplay", "MoveRight", GLFW_KEY_D);
    m.bindKey("Gameplay", "MoveLeft", GLFW_KEY_A);
    m.bindGamepadAxis("Gameplay", "MoveForward", GLFW_GAMEPAD_AXIS_LEFT_Y, -1.0f);
    m.bindGamepadAxis("Gameplay", "MoveBack", GLFW_GAMEPAD_AXIS_LEFT_Y, 1.0f);
    m.bindGamepadAxis("Gameplay", "MoveRight", GLFW_GAMEPAD_AXIS_LEFT_X, 1.0f);
    m.bindGamepadAxis("Gameplay", "MoveLeft", GLFW_GAMEPAD_AXIS_LEFT_X, -1.0f);
    m.defineAxis("Gameplay", "MoveForward", "MoveBack", "MoveForward");
    m.defineAxis("Gameplay", "MoveRight", "MoveLeft", "MoveRight");

    // ---- 视角（鼠标 + 手柄右摇杆）----
    m.defineAxis("Gameplay", "LookYaw", "LookLeft", "LookRight");
    m.defineAxis("Gameplay", "LookPitch", "LookDown", "LookUp");
    m.defineAction("Gameplay", "LookLeft");
    m.defineAction("Gameplay", "LookRight");
    m.defineAction("Gameplay", "LookUp");
    m.defineAction("Gameplay", "LookDown");
    m.bindGamepadAxis("Gameplay", "LookRight", GLFW_GAMEPAD_AXIS_RIGHT_X, 1.0f);
    m.bindGamepadAxis("Gameplay", "LookLeft", GLFW_GAMEPAD_AXIS_RIGHT_X, -1.0f);
    m.bindGamepadAxis("Gameplay", "LookUp", GLFW_GAMEPAD_AXIS_RIGHT_Y, -1.0f);
    m.bindGamepadAxis("Gameplay", "LookDown", GLFW_GAMEPAD_AXIS_RIGHT_Y, 1.0f);

    // ---- 动作 ----
    m.defineAction("Gameplay", "Jump");
    m.bindKey("Gameplay", "Jump", GLFW_KEY_SPACE);
    m.bindGamepadButton("Gameplay", "Jump", GLFW_GAMEPAD_BUTTON_A);
    m.defineAction("Gameplay", "Sprint");
    // 冲刺 = **鼠标右键长按**（2026-10-22）。
    //
    // 为什么不是 Shift：第三人称里冲刺是"边跑边转视角"的连续动作，右手本来
    // 就在鼠标上；Shift 要求把左手小指按住不放，长距离跑图很快就累。而且
    // 右键在当前手感里是**空闲**的 —— 鼠标视角在光标锁定时是"移动即转"，
    // 不需要按住任何键（见 PlayerController 的 lookDelta）。
    //
    // ⚠ 手柄的 LB 保留：手柄上没有"右键"，LB 就是它的冲刺键。
    // ⚠ 老工程里存过 `Sprint=LShift` 的 `config/input.json` 不会被这条改掉
    //   （存档优先于出厂值）—— 需要用户在输入设置里点一次"恢复默认"。
    m.bindMouse("Gameplay", "Sprint", GLFW_MOUSE_BUTTON_RIGHT);
    m.bindGamepadButton("Gameplay", "Sprint", GLFW_GAMEPAD_BUTTON_LEFT_BUMPER);
    m.defineAction("Gameplay", "Interact");
    m.bindKey("Gameplay", "Interact", GLFW_KEY_E);
    m.bindGamepadButton("Gameplay", "Interact", GLFW_GAMEPAD_BUTTON_X);
    m.defineAction("Gameplay", "Pause");
    m.bindKey("Gameplay", "Pause", GLFW_KEY_ESCAPE);
    m.bindGamepadButton("Gameplay", "Pause", GLFW_GAMEPAD_BUTTON_START);

    // ---- 菜单（压栈后 Gameplay 整组失效）----
    m.defineAction("Menu", "MenuUp");
    m.defineAction("Menu", "MenuDown");
    // 左右也要有：U6 的焦点导航是**几何**的（两列布局里 ↓ 不该跳到旁边
    // 那一列，玩家会用 → 过去），只给上下等于把一半导航关掉。
    m.defineAction("Menu", "MenuLeft");
    m.defineAction("Menu", "MenuRight");
    m.defineAction("Menu", "MenuConfirm");
    m.defineAction("Menu", "MenuBack");
    m.bindKey("Menu", "MenuUp", GLFW_KEY_UP);
    m.bindKey("Menu", "MenuUp", GLFW_KEY_W);
    m.bindKey("Menu", "MenuDown", GLFW_KEY_DOWN);
    m.bindKey("Menu", "MenuDown", GLFW_KEY_S);
    m.bindKey("Menu", "MenuLeft", GLFW_KEY_LEFT);
    m.bindKey("Menu", "MenuLeft", GLFW_KEY_A);
    m.bindKey("Menu", "MenuRight", GLFW_KEY_RIGHT);
    m.bindKey("Menu", "MenuRight", GLFW_KEY_D);
    // Tab 在菜单里循环切换焦点（Shift+Tab 反向）。它绑在菜单上下文里
    // 而不是全局：游戏中按 Tab 不该有任何反应。
    m.defineAction("Menu", "MenuTab");
    m.bindKey("Menu", "MenuTab", GLFW_KEY_TAB);
    m.bindKey("Menu", "MenuConfirm", GLFW_KEY_ENTER);
    m.bindKey("Menu", "MenuConfirm", GLFW_KEY_SPACE);
    m.bindKey("Menu", "MenuBack", GLFW_KEY_ESCAPE);
    m.bindGamepadButton("Menu", "MenuUp", GLFW_GAMEPAD_BUTTON_DPAD_UP);
    m.bindGamepadButton("Menu", "MenuDown", GLFW_GAMEPAD_BUTTON_DPAD_DOWN);
    m.bindGamepadButton("Menu", "MenuLeft", GLFW_GAMEPAD_BUTTON_DPAD_LEFT);
    m.bindGamepadButton("Menu", "MenuRight", GLFW_GAMEPAD_BUTTON_DPAD_RIGHT);
    m.bindGamepadButton("Menu", "MenuConfirm", GLFW_GAMEPAD_BUTTON_A);
    m.bindGamepadButton("Menu", "MenuBack", GLFW_GAMEPAD_BUTTON_B);

    m.m_stack.push_back("Gameplay");
    return m;
}

void InputMap::resetToDefault() {
    const InputMap def = makeDefault();
    *this = def;
}

// ---------------------------------------------------------------- 定义

InputContext& InputMap::ensureContext(const std::string& name) {
    auto it = m_contexts.find(name);
    if (it != m_contexts.end()) return it->second;
    InputContext c;
    c.name = name;
    return m_contexts.emplace(name, std::move(c)).first->second;
}

void InputMap::defineAction(const std::string& ctx, const std::string& action) {
    InputContext& c = ensureContext(ctx);
    if (c.actions.find(action) == c.actions.end())
        c.actions.emplace(action, std::vector<InputBinding>{});
}

void InputMap::bindKey(const std::string& ctx, const std::string& action,
                       int key, bool ctrl, bool shift, bool alt) {
    defineAction(ctx, action);
    InputBinding b;
    b.source = InputSource::Key;
    b.code = key;
    b.ctrl = ctrl;
    b.shift = shift;
    b.alt = alt;
    m_contexts[ctx].actions[action].push_back(b);
}

void InputMap::bindMouse(const std::string& ctx, const std::string& action,
                         int button) {
    defineAction(ctx, action);
    InputBinding b;
    b.source = InputSource::MouseButton;
    b.code = button;
    m_contexts[ctx].actions[action].push_back(b);
}

void InputMap::bindGamepadButton(const std::string& ctx,
                                 const std::string& action, int button) {
    defineAction(ctx, action);
    InputBinding b;
    b.source = InputSource::GamepadButton;
    b.code = button;
    m_contexts[ctx].actions[action].push_back(b);
}

void InputMap::bindGamepadAxis(const std::string& ctx,
                               const std::string& action, int axis,
                               float scale, float deadzone) {
    defineAction(ctx, action);
    InputBinding b;
    b.source = InputSource::GamepadAxis;
    b.code = axis;
    b.scale = scale;
    b.deadzone = deadzone;
    m_contexts[ctx].actions[action].push_back(b);
}

void InputMap::defineAxis(const std::string& ctx, const std::string& axis,
                          const std::string& negative,
                          const std::string& positive) {
    InputContext& c = ensureContext(ctx);
    // 轴引用的动作自动补一份定义：漏定义时它恒为 0，不会崩，也不会
    // 因为"少一行 defineAction"整根轴悄悄失效（那种 bug 很难看出来）。
    if (c.actions.find(negative) == c.actions.end())
        c.actions.emplace(negative, std::vector<InputBinding>{});
    if (c.actions.find(positive) == c.actions.end())
        c.actions.emplace(positive, std::vector<InputBinding>{});
    c.axes[axis] = {negative, positive};
}

void InputMap::setAxisDeadzone(const std::string& ctx, float deadzone) {
    ensureContext(ctx).axisDeadzone = std::max(deadzone, 0.0f);
}

void InputMap::clearBindings(const std::string& ctx, const std::string& action) {
    auto it = m_contexts.find(ctx);
    if (it == m_contexts.end()) return;
    auto a = it->second.actions.find(action);
    if (a != it->second.actions.end()) a->second.clear();
}

// ---------------------------------------------------------------- 上下文

void InputMap::pushContext(const std::string& name) {
    ensureContext(name);
    m_stack.push_back(name);
    // 上下文一变，**这一帧的边沿作废**（held 照算）。见 m_suppressEdges。
    m_suppressEdges = true;
}

bool InputMap::popContext() {
    if (m_stack.size() <= 1) return false;
    m_stack.pop_back();
    m_suppressEdges = true;
    return true;
}

void InputMap::clearContexts() {
    m_stack.clear();
    m_prev = m_state;
    m_suppressEdges = true;
}

const std::string& InputMap::topContext() const {
    static const std::string kNone = "(none)";
    return m_stack.empty() ? kNone : m_stack.back();
}

// ---------------------------------------------------------------- 采样

void InputMap::poll() {
    RawInput raw;

    // 键盘：Input 的内部表就是 512 个 bool，全读一遍的代价是几次缓存行。
    for (int k = 0; k < RawInput::kKeys; ++k)
        raw.keys[k] = Input::keyDown(k);
    for (int b = 0; b < RawInput::kMouseButtons; ++b)
        raw.mouse[b] = Input::mouseDown(b);

    // 鼠标位移：**在这里消费**（Input::mouseDelta 是"读即清零"）。
    // 别的地方再读会拿到 0 —— 视角的唯一入口是本类。
    double dx = 0.0, dy = 0.0;
    Input::mouseDelta(&dx, &dy);
    m_lookDelta = glm::vec2(static_cast<float>(dx), static_cast<float>(dy));

    // 手柄：GLFW 的 gamepad API（不引 SDL —— 离线构建偏好）。
    GLFWgamepadstate gs;
    raw.padConnected = glfwGetGamepadState(GLFW_JOYSTICK_1, &gs) == GLFW_TRUE;
    if (raw.padConnected) {
        for (int b = 0; b < RawInput::kPadButtons; ++b)
            raw.padButtons[b] = gs.buttons[b] == GLFW_PRESS;
        for (int a = 0; a < RawInput::kPadAxes; ++a)
            raw.padAxes[a] = gs.axes[a];
    }

    evaluate(raw);
}

void InputMap::pollRaw(const RawInput& raw) {
    m_lookDelta = glm::vec2(0.0f);
    evaluate(raw);
}

bool InputMap::bindingActive(const InputBinding& b, const RawInput& raw) {
    switch (b.source) {
        case InputSource::Key: {
            if (b.code < 0 || b.code >= RawInput::kKeys) return false;
            if (!raw.keys[b.code]) return false;
            // 修饰键：要求"按住"而不是"刚好按下"。修好一半的 Ctrl+W
            // 不该被当成 W 单独触发 —— 那会让重绑定之后的组合键一半生效。
            if (b.ctrl &&
                !(raw.keys[GLFW_KEY_LEFT_CONTROL] || raw.keys[GLFW_KEY_RIGHT_CONTROL]))
                return false;
            if (b.shift &&
                !(raw.keys[GLFW_KEY_LEFT_SHIFT] || raw.keys[GLFW_KEY_RIGHT_SHIFT]))
                return false;
            if (b.alt &&
                !(raw.keys[GLFW_KEY_LEFT_ALT] || raw.keys[GLFW_KEY_RIGHT_ALT]))
                return false;
            return true;
        }
        case InputSource::MouseButton:
            if (b.code < 0 || b.code >= RawInput::kMouseButtons) return false;
            return raw.mouse[b.code];
        case InputSource::GamepadButton:
            if (!raw.padConnected) return false;
            if (b.code < 0 || b.code >= RawInput::kPadButtons) return false;
            return raw.padButtons[b.code];
        case InputSource::GamepadAxis: {
            if (!raw.padConnected) return false;
            if (b.code < 0 || b.code >= RawInput::kPadAxes) return false;
            const float v = raw.padAxes[b.code] * b.scale;
            return v > std::max(b.deadzone, 0.0f);
        }
    }
    return false;
}

float InputMap::axisValueOf(const InputBinding& b, const RawInput& raw) {
    switch (b.source) {
        case InputSource::Key:
        case InputSource::MouseButton:
        case InputSource::GamepadButton:
            return bindingActive(b, raw) ? 1.0f : 0.0f;
        case InputSource::GamepadAxis: {
            if (!raw.padConnected) return 0.0f;
            if (b.code < 0 || b.code >= RawInput::kPadAxes) return 0.0f;
            float v = raw.padAxes[b.code] * b.scale;
            // 单轴死区 + 重新归一化：过死区之后从 0 平滑涨到 1，
            // 而不是"一过阈值就跳到 0.25"（那会让摇杆初段很硬）。
            const float dz = std::max(b.deadzone, 0.0f);
            if (v <= dz) return 0.0f;
            return (v - dz) / std::max(1.0f - dz, 1.0e-3f);
        }
    }
    return 0.0f;
}

void InputMap::evaluate(const RawInput& raw) {
    m_padConnected = raw.padConnected;

    // 上一帧的结果整体搬到 m_prev，本帧从头算 —— **只有栈顶上下文**参与。
    // 这就是"压 Menu 后 Gameplay 的 Jump 不再触发"的实现点：不是把它的
    // 值减一下，而是它压根不在被求值的表里。
    m_prev = m_state;
    m_state.clear();

    const InputContext* ctx = context(topContext());
    if (!ctx) return;

    for (const auto& kv : ctx->actions) {
        ActionState st;
        for (const InputBinding& b : kv.second) {
            const float v = axisValueOf(b, raw);
            if (v > 0.0f) {
                st.held = true;
                st.value = std::max(st.value, v);
            }
        }
        const auto it = m_prev.find(kv.first);
        const bool wasHeld = it != m_prev.end() && it->second.held;
        st.pressed = st.held && !wasHeld;
        st.released = !st.held && wasHeld;
        if (m_suppressEdges) {
            st.pressed = false;
            st.released = false;
        }
        m_state.emplace(kv.first, st);
    }
    m_suppressEdges = false;

    if (m_trace) {
        for (const auto& kv : m_state) {
            if (!kv.second.held) continue;
            VK_LOG_INFO("INPUT %s=%d held=%d pressed=%d value=%.2f ctx=%s",
                        kv.first.c_str(), kv.second.held ? 1 : 0,
                        kv.second.pressed ? 1 : 0, kv.second.value,
                        topContext().c_str());
        }
    }
}

// ---------------------------------------------------------------- 查询

const ActionState* InputMap::action(const std::string& name) const {
    const auto it = m_state.find(name);
    return it == m_state.end() ? nullptr : &it->second;
}

bool InputMap::isHeld(const std::string& a) const {
    const ActionState* s = action(a);
    return s && s->held;
}

bool InputMap::wasPressed(const std::string& a) const {
    const ActionState* s = action(a);
    return s && s->pressed;
}

bool InputMap::wasReleased(const std::string& a) const {
    const ActionState* s = action(a);
    return s && s->released;
}

float InputMap::axis(const std::string& name) const {
    const InputContext* ctx = context(topContext());
    if (!ctx) return 0.0f;
    const auto it = ctx->axes.find(name);
    if (it == ctx->axes.end()) return 0.0f;

    float v = 0.0f;
    const ActionState* pos = action(it->second.second);
    const ActionState* neg = action(it->second.first);
    if (pos) v += pos->value;
    if (neg) v -= neg->value;
    v = std::clamp(v, -1.0f, 1.0f);

    // 死区在**合成之后**：两个半推的摇杆合起来 0.4 也不会让角色自己爬。
    const float dz = std::max(ctx->axisDeadzone, 0.0f);
    const float a = std::fabs(v);
    if (a <= dz) return 0.0f;
    return std::copysign((a - dz) / std::max(1.0f - dz, 1.0e-3f), v);
}

const InputContext* InputMap::context(const std::string& name) const {
    const auto it = m_contexts.find(name);
    return it == m_contexts.end() ? nullptr : &it->second;
}

std::vector<std::string> InputMap::actionNames(const std::string& ctx) const {
    std::vector<std::string> out;
    const InputContext* c = context(ctx);
    if (!c) return out;
    out.reserve(c->actions.size());
    for (const auto& kv : c->actions) out.push_back(kv.first);
    std::sort(out.begin(), out.end());
    return out;
}

std::string InputMap::describeBinding(const std::string& ctx,
                                      const std::string& actionName) const {
    const InputContext* c = context(ctx);
    if (!c) return "(no context)";
    const auto it = c->actions.find(actionName);
    if (it == c->actions.end() || it->second.empty()) return "(unbound)";
    std::string out;
    for (std::size_t i = 0; i < it->second.size(); ++i) {
        if (i) out += " / ";
        out += describeOne(it->second[i]);
    }
    return out;
}

std::string InputMap::describeBindingOne(const InputBinding& b) const {
    return describeOne(b);
}

// ---------------------------------------------------------------- 重绑定（G06）
//
// 四个写/查接口都是给**编辑器的重绑定面板**用的。它们刻意放在引擎侧而不是
// 面板里：断言"改一个键 → 存 → 读 → 生效"这条链需要在无窗口自检里跑，
// 而面板需要窗口 + ImGui。
//
// ⚠ 一处容易搞错的语义：`setActionBinding` 只替换**同一来源类型**的绑定。
//   默认表里 Jump 有两条（Space + 手柄 A），它们是 makeDefault 用 bindKey /
//   bindGamepadButton 分别加的；面板改键盘时如果整组清掉，手柄玩家会莫名
//   少一个键 —— 而"键盘 + 手柄双绑定"恰恰是默认表的设计。

bool InputMap::setActionBinding(const std::string& ctx,
                               const std::string& actionName,
                               const InputBinding& b) {
    auto it = m_contexts.find(ctx);
    if (it == m_contexts.end()) return false;
    auto a = it->second.actions.find(actionName);
    if (a == it->second.actions.end()) return false;   // 未定义的动作不改
    std::vector<InputBinding>& list = a->second;
    // 只摘掉**同一来源类型**的旧绑定（见头文件里的语义说明）。
    list.erase(std::remove_if(list.begin(), list.end(),
                              [&](const InputBinding& o) {
                                  return o.source == b.source;
                              }),
               list.end());
    list.push_back(b);
    // 绑定变了 → 这一帧的**边沿作废**。新键可能正按着（玩家按下 A 的
    // 那一刻面板把它绑成 Jump），照常算边沿会在同一帧凭空冒一次 pressed。
    m_suppressEdges = true;
    return true;
}

void InputMap::clearActionBinding(const std::string& ctx,
                                 const std::string& actionName,
                                 InputSource source) {
    auto it = m_contexts.find(ctx);
    if (it == m_contexts.end()) return;
    auto a = it->second.actions.find(actionName);
    if (a == it->second.actions.end()) return;
    std::vector<InputBinding>& list = a->second;
    list.erase(std::remove_if(list.begin(), list.end(),
                              [&](const InputBinding& o) {
                                  return o.source == source;
                              }),
               list.end());
    m_suppressEdges = true;
}

bool InputMap::resetActionBinding(const std::string& ctx,
                                 const std::string& actionName) {
    const InputMap def = makeDefault();
    const InputContext* dc = def.context(ctx);
    if (!dc) return false;
    const auto di = dc->actions.find(actionName);
    if (di == dc->actions.end()) return false;   // 出厂表里根本没这个动作

    auto it = m_contexts.find(ctx);
    if (it == m_contexts.end()) return false;
    auto a = it->second.actions.find(actionName);
    if (a == it->second.actions.end()) return false;
    a->second = di->second;   // 含键盘 + 手柄，即"这个词的出厂状态"
    m_suppressEdges = true;
    return true;
}

std::vector<std::string> InputMap::conflictingActions(
    const std::string& ctx, const InputBinding& b,
    const std::string& excludeAction) const {
    std::vector<std::string> out;
    const InputContext* c = context(ctx);
    if (!c) return out;
    for (const auto& kv : c->actions) {
        if (kv.first == excludeAction) continue;
        for (const InputBinding& o : kv.second) {
            if (o.source != b.source || o.code != b.code) continue;
            // ⚠ 手柄轴的正负两半**不是冲突**：MoveForward 与 MoveBack 都建在
            //   LEFT_Y 上（一个 scale=+1、一个 -1），物理上本来就是"同一根轴
            //   的两个方向"。只比 (source, code) 会把 8 对轴报成冲突，面板
            //   上满屏红字 —— 而真正需要警告的（两个动作抢同一个键）反而被
            //   淹掉了。方向相同才算抢。
            if (o.source == InputSource::GamepadAxis &&
                ((o.scale < 0.0f) != (b.scale < 0.0f)))
                continue;
            out.push_back(kv.first);
            break;   // 同一个动作只报一次（它可能有多条绑定）
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string> InputMap::contextNames() const {
    std::vector<std::string> out;
    out.reserve(m_contexts.size());
    for (const auto& kv : m_contexts) out.push_back(kv.first);
    std::sort(out.begin(), out.end());
    return out;
}

std::string InputMap::describe() const {
    std::string out;
    std::vector<std::string> names;
    for (const auto& kv : m_contexts) names.push_back(kv.first);
    std::sort(names.begin(), names.end());
    for (const std::string& n : names) {
        if (!out.empty()) out += ", ";
        out += n + "=" + std::to_string(m_contexts.at(n).actions.size());
    }
    out += " stack=";
    for (std::size_t i = 0; i < m_stack.size(); ++i) {
        if (i) out += ">";
        out += m_stack[i];
    }
    return out;
}

// ---------------------------------------------------------------- JSON

namespace {

nlohmann::json bindingToJson(const InputBinding& b) {
    nlohmann::json j;
    j["source"] = sourceName(b.source);
    j["code"] = b.code;
    if (b.source == InputSource::GamepadAxis) {
        j["scale"] = b.scale;
        j["deadzone"] = b.deadzone;
    }
    if (b.ctrl) j["ctrl"] = true;
    if (b.shift) j["shift"] = true;
    if (b.alt) j["alt"] = true;
    return j;
}

bool bindingFromJson(const nlohmann::json& j, InputBinding& b,
                     std::string* error) {
    if (!j.is_object()) {
        if (error) *error = "binding 不是对象";
        return false;
    }
    b.source = sourceFromName(j.value("source", std::string("key")));
    b.code = j.value("code", 0);
    b.scale = j.value("scale", 1.0f);
    b.deadzone = j.value("deadzone", 0.20f);
    b.ctrl = j.value("ctrl", false);
    b.shift = j.value("shift", false);
    b.alt = j.value("alt", false);
    if (b.scale == 0.0f) b.scale = 1.0f;
    return true;
}

} // namespace

std::string InputMap::toText() const {
    nlohmann::json root;
    root["version"] = 1;
    root["gamepad"] = "glfw";

    std::vector<std::string> names;
    for (const auto& kv : m_contexts) names.push_back(kv.first);
    std::sort(names.begin(), names.end());

    for (const std::string& n : names) {
        const InputContext& c = m_contexts.at(n);
        nlohmann::json jc;
        jc["axisDeadzone"] = c.axisDeadzone;

        nlohmann::json ja = nlohmann::json::object();
        std::vector<std::string> acts;
        for (const auto& kv : c.actions) acts.push_back(kv.first);
        std::sort(acts.begin(), acts.end());
        for (const std::string& a : acts) {
            nlohmann::json arr = nlohmann::json::array();
            for (const InputBinding& b : c.actions.at(a))
                arr.push_back(bindingToJson(b));
            ja[a] = arr;
        }
        jc["actions"] = ja;

        nlohmann::json jx = nlohmann::json::object();
        for (const auto& kv : c.axes) {
            jx[kv.first] = {{"negative", kv.second.first},
                            {"positive", kv.second.second}};
        }
        jc["axes"] = jx;
        root["contexts"][n] = jc;
    }
    return root.dump(2);
}

bool InputMap::saveToFile(const std::string& path) const {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        VK_LOG_WARN("INPUT bindings: cannot write '%s'", path.c_str());
        return false;
    }
    f << toText() << "\n";
    VK_LOG_INFO("INPUT bindings saved: %s", path.c_str());
    return true;
}

bool InputMap::loadFromText(const std::string& text, std::string* error) {
    nlohmann::json root;
    try {
        root = nlohmann::json::parse(text);
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
    if (!root.is_object() || !root.contains("contexts")) {
        if (error) *error = "缺少 contexts";
        return false;
    }

    // 解析成功才替换 —— 半途失败留下"删了一半的绑定表"比直接失败更糟。
    std::unordered_map<std::string, InputContext> fresh;
    int rejected = 0;
    for (auto it = root["contexts"].begin(); it != root["contexts"].end(); ++it) {
        const std::string name = it.key();
        InputContext c;
        c.name = name;
        c.axisDeadzone = it.value().value("axisDeadzone", 0.20f);

        if (it.value().contains("actions")) {
            for (auto a = it.value()["actions"].begin();
                 a != it.value()["actions"].end(); ++a) {
                std::vector<InputBinding> list;
                if (a.value().is_array()) {
                    for (const auto& jb : a.value()) {
                        InputBinding b;
                        if (bindingFromJson(jb, b, error)) list.push_back(b);
                        else ++rejected;
                    }
                }
                c.actions.emplace(a.key(), std::move(list));
            }
        }
        if (it.value().contains("axes")) {
            for (auto x = it.value()["axes"].begin();
                 x != it.value()["axes"].end(); ++x) {
                const std::string neg = x.value().value("negative", "");
                const std::string pos = x.value().value("positive", "");
                if (neg.empty() || pos.empty()) {
                    ++rejected;
                    continue;
                }
                c.axes[x.key()] = {neg, pos};
            }
        }
        fresh.emplace(name, std::move(c));
    }

    if (fresh.find("Gameplay") == fresh.end()) {
        // 没有 Gameplay 的绑定表 = 一张废表（游戏根本跑不动），直接拒绝。
        if (error) *error = "缺少 Gameplay 上下文";
        return false;
    }

    m_contexts = std::move(fresh);
    // 栈只保留还存在的上下文；全没了就退回 Gameplay（不然一个动作都不成立，
    // 看起来像"输入彻底坏了"）。
    std::vector<std::string> keep;
    for (const std::string& n : m_stack)
        if (m_contexts.count(n)) keep.push_back(n);
    if (keep.empty() && m_contexts.count("Gameplay")) keep.push_back("Gameplay");
    m_stack = std::move(keep);
    m_state.clear();
    m_prev.clear();

    if (rejected)
        VK_LOG_WARN("INPUT bindings: %d entr(ies) rejected", rejected);
    return true;
}

bool InputMap::loadFromFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    std::string err;
    if (!loadFromText(ss.str(), &err)) {
        VK_LOG_WARN("INPUT bindings: parse failed '%s': %s", path.c_str(),
                    err.c_str());
        return false;
    }
    VK_LOG_INFO("INPUT bindings loaded: %s (%s)", path.c_str(),
                describe().c_str());
    return true;
}

// ---------------------------------------------------------------- 自检

namespace {

int g_inputFail = 0;
int g_inputPass = 0;

void icheck(const char* what, bool ok, const std::string& detail = "") {
    if (ok) {
        ++g_inputPass;
        std::printf("INPUT-TEST pass %s%s%s\n", what, detail.empty() ? "" : " — ",
                    detail.c_str());
    } else {
        ++g_inputFail;
        std::printf("INPUT-TEST FAIL %s%s%s\n", what, detail.empty() ? "" : " — ",
                    detail.c_str());
    }
}

RawInput pressKey(RawInput r, int key, bool down) {
    if (key >= 0 && key < RawInput::kKeys) r.keys[key] = down;
    return r;
}

} // namespace

int runInputSelfTest() {
    g_inputPass = 0;
    g_inputFail = 0;
    std::printf("INPUT-TEST begin (no window, injected raw input)\n");

    // ---- A. 边沿：pressed 只在**一帧**为真 ----
    {
        InputMap m = InputMap::makeDefault();
        RawInput r;
        m.pollRaw(r);
        icheck("A0 初始未按下", !m.isHeld("Jump") && !m.wasPressed("Jump"));

        r = pressKey(r, GLFW_KEY_SPACE, true);
        m.pollRaw(r);
        icheck("A1 按下的那一帧 pressed=1", m.wasPressed("Jump") && m.isHeld("Jump"));

        m.pollRaw(r);
        icheck("A2 按住不放 pressed=0 held=1",
               !m.wasPressed("Jump") && m.isHeld("Jump"));

        r = pressKey(r, GLFW_KEY_SPACE, false);
        m.pollRaw(r);
        icheck("A3 松开的那一帧 released=1 held=0",
               m.wasReleased("Jump") && !m.isHeld("Jump"));

        m.pollRaw(r);
        icheck("A4 松开之后 released=0", !m.wasReleased("Jump"));
    }

    // ---- B. 上下文栈：压 Menu 后 Gameplay 的动作不再触发 ----
    {
        InputMap m = InputMap::makeDefault();
        RawInput r = pressKey(RawInput{}, GLFW_KEY_SPACE, true);
        m.pollRaw(r);
        icheck("B0 Gameplay 里 Space 触发 Jump", m.isHeld("Jump"));

        m.pushContext("Menu");
        m.pollRaw(r);   // Space 还按着
        icheck("B1 压 Menu 后 Jump 立即失效（不是下一帧）", !m.isHeld("Jump"));
        icheck("B2 Menu 自己的 Confirm 生效", m.isHeld("MenuConfirm"));

        m.pushContext("UI");
        m.pollRaw(r);
        icheck("B3 再压 UI 后 Menu 的 Confirm 也失效", !m.isHeld("MenuConfirm"));

        m.popContext();
        m.pollRaw(r);
        icheck("B4 弹回 Menu 后 Confirm 恢复，而 Jump 仍失效",
               m.isHeld("MenuConfirm") && !m.isHeld("Jump"));

        m.popContext();
        m.pollRaw(r);
        icheck("B5 弹回 Gameplay 后 Jump 恢复（按住不算 pressed）",
               m.isHeld("Jump") && !m.wasPressed("Jump"));
    }

    // ---- C. 手柄无设备：不崩、退化为 0 ----
    {
        InputMap m = InputMap::makeDefault();
        RawInput r;   // padConnected = false
        m.pollRaw(r);
        icheck("C1 无手柄时 Move 轴为 0", m.axis("MoveForward") == 0.0f);
        icheck("C2 无手柄时 Jump 不成立", !m.isHeld("Jump") && !m.isHeld("Interact"));
        icheck("C3 gamepadConnected() 报 false", !m.gamepadConnected());
    }

    // ---- D. 手柄轴：单轴死区 + 合成死区 ----
    {
        InputMap m = InputMap::makeDefault();
        RawInput r;
        r.padConnected = true;

        r.padAxes[GLFW_GAMEPAD_AXIS_LEFT_Y] = -0.10f;  // 在 0.20 单轴死区内
        m.pollRaw(r);
        icheck("D1 单轴死区生效（0.10 → 0）", m.axis("MoveForward") == 0.0f);

        r.padAxes[GLFW_GAMEPAD_AXIS_LEFT_Y] = -1.0f;
        m.pollRaw(r);
        icheck("D2 摇杆推到底 = 1", std::fabs(m.axis("MoveForward") - 1.0f) < 1e-4f);

        // 合成之后的死区：摇杆推到 0.4，单轴归一化后是 (0.4-0.2)/0.8 = 0.25。
        // 把**合成死区**提到 0.5，这 0.25 必须被吃掉 —— 这正是"两个半推
        // 的摇杆加起来不该让角色爬"的那条规则。
        m.setAxisDeadzone("Gameplay", 0.5f);
        r.padAxes[GLFW_GAMEPAD_AXIS_LEFT_Y] = -0.4f;
        m.pollRaw(r);
        const float half = m.axis("MoveForward");
        icheck("D3 合成死区把 0.25 吃掉（轴 = 0）", std::fabs(half) < 1e-4f,
               "axis=" + std::to_string(half));

        r.padAxes[GLFW_GAMEPAD_AXIS_LEFT_Y] = -1.0f;
        m.pollRaw(r);
        const float full = m.axis("MoveForward");
        icheck("D4 推到底仍然 = 1（过死区后重新归一化）",
               std::fabs(full - 1.0f) < 1e-4f, "axis=" + std::to_string(full));
    }

    // ---- E. JSON 往返：改一个键 → 存 → 读 → 生效 ----
    {
        InputMap m = InputMap::makeDefault();
        m.clearBindings("Gameplay", "Jump");
        m.bindKey("Gameplay", "Jump", GLFW_KEY_F);
        const std::string text = m.toText();

        InputMap loaded;
        std::string err;
        const bool ok = loaded.loadFromText(text, &err);
        icheck("E1 绑定表反序列化成功", ok, ok ? "" : err);
        loaded.pushContext("Gameplay");

        RawInput r = pressKey(RawInput{}, GLFW_KEY_SPACE, true);
        loaded.pollRaw(r);
        icheck("E2 改绑后 Space 不再触发 Jump", !loaded.isHeld("Jump"));

        RawInput r2 = pressKey(RawInput{}, GLFW_KEY_F, true);
        loaded.pollRaw(r2);
        icheck("E3 改绑后 F 触发 Jump", loaded.isHeld("Jump"));

        std::string dumped = loaded.toText();
        icheck("E4 二次序列化逐字符稳定", dumped == text);
    }

    // ---- F. 坏表被拒绝，且**不破坏**已有绑定 ----
    {
        InputMap m = InputMap::makeDefault();
        const std::string before = m.toText();
        std::string err;
        const bool ok = m.loadFromText("{ not json", &err);
        icheck("F1 坏 JSON 被拒绝", !ok);
        icheck("F2 拒绝时原绑定表不变", m.toText() == before);

        const bool ok2 = m.loadFromText(R"({"version":1,"contexts":{}})", &err);
        icheck("F3 缺 Gameplay 的表被拒绝", !ok2);
    }

    // ---- G. 组合键：按住 Ctrl 时单按 W 不触发 ----
    {
        InputMap m = InputMap::makeDefault();
        m.defineAction("Gameplay", "Save");
        m.bindKey("Gameplay", "Save", GLFW_KEY_S, /*ctrl=*/true);

        RawInput r = pressKey(RawInput{}, GLFW_KEY_S, true);
        m.pollRaw(r);
        icheck("G1 没按 Ctrl 时 Ctrl+S 不触发", !m.isHeld("Save"));
        icheck("G2 单独的 S 仍然走 MoveBack", m.isHeld("MoveBack"));

        r = pressKey(r, GLFW_KEY_LEFT_CONTROL, true);
        m.pollRaw(r);
        icheck("G3 按住 Ctrl 后 Ctrl+S 触发", m.isHeld("Save"));
    }

    // ---- H. 重绑定四件套（G06 面板唯一的写路径）----
    //
    // 这一组守的是"面板改一个键、存档、运行时读到"的整条链。面板本身要
    // 窗口 + ImGui 测不了，但**它的全部逻辑**在这里 —— 早先在 Panel 里
    // 直接拼 bindKey 的写法连"改完手柄绑定丢了"这种 bug 都发现不了。
    {
        InputMap m = InputMap::makeDefault();

        // H0 上下文枚举（UI 的选择器靠它，context() 只能按名字查单个）
        const std::vector<std::string> names = m.contextNames();
        icheck("H0 上下文枚举 = [Gameplay, Menu]",
               names.size() == 2 && names[0] == "Gameplay" &&
                   names[1] == "Menu");

        // H1/H2 冲突检测（出厂 Space 绑在 Jump 上）
        InputBinding space;
        space.source = InputSource::Key;
        space.code = GLFW_KEY_SPACE;
        const std::vector<std::string> c1 =
            m.conflictingActions("Gameplay", space, "Sprint");
        icheck("H1 冲突检测认出 Jump 占了 Space",
               c1.size() == 1 && c1[0] == "Jump");
        icheck("H2 被排除的动作本身不算冲突",
               m.conflictingActions("Gameplay", space, "Jump").empty());
        // 修饰键不参与比较：Shift+Space 与 Space 在"抢不抢键"上是同一件事
        InputBinding spaceShift = space;
        spaceShift.shift = true;
        icheck("H2b 修饰键不参与冲突比较",
               m.conflictingActions("Gameplay", spaceShift, "Sprint").size() ==
                   1);

        // ⚠ H2c/H2d：手柄轴的正负两半**不算冲突**。MoveForward / MoveBack 都
        //   建在 LEFT_Y 上（scale ±1），它们是同一根轴的两个方向；不排除的话
        //   出厂表会报 8 对假冲突，面板上满屏红字，真冲突反而被淹掉。
        InputBinding axisPos;
        axisPos.source = InputSource::GamepadAxis;
        axisPos.code = 0;        // GLFW_GAMEPAD_AXIS_LEFT_X
        axisPos.scale = 1.0f;
        const std::vector<std::string> sameDir =
            m.conflictingActions("Gameplay", axisPos, "MoveLeft");
        icheck("H2c 同轴**同向**仍算冲突（MoveRight 报出来）",
               sameDir.size() == 1 && sameDir[0] == "MoveRight",
               sameDir.empty() ? std::string("(空)") : sameDir[0]);
        const std::vector<std::string> oppDir =
            m.conflictingActions("Gameplay", axisPos, "MoveRight");
        icheck("H2d 同轴**反向**不算冲突（MoveLeft 不报）", oppDir.empty(),
               oppDir.empty() ? std::string("(空)") : oppDir[0]);

        // H3~H6 改绑：Sprint 的**键盘**从 Shift 换成 F
        InputBinding f;
        f.source = InputSource::Key;
        f.code = GLFW_KEY_F;
        icheck("H3 setActionBinding 成功",
               m.setActionBinding("Gameplay", "Sprint", f));
        const std::string sprint = m.describeBinding("Gameplay", "Sprint");
        icheck("H4 键盘绑定换成 F 且旧 Shift 已摘掉",
               sprint.find("F") != std::string::npos &&
                   sprint.find("Shift") == std::string::npos,
               sprint);
        // ⚠ 这一条是"只替换同一来源类型"的核心断言：手柄 LB 必须还在。
        //   写成整组替换时它立刻变红（历史实现就是整组替换）。
        icheck("H5 手柄绑定被保留（只换同一来源类型）",
               sprint.find("PadBtn") != std::string::npos, sprint);

        m.pushContext("Gameplay");
        RawInput r = pressKey(RawInput{}, GLFW_KEY_LEFT_SHIFT, true);
        m.pollRaw(r);
        icheck("H6 旧的 Shift 不再触发 Sprint", !m.isHeld("Sprint"));
        r = pressKey(RawInput{}, GLFW_KEY_F, true);
        m.pollRaw(r);
        icheck("H7 新的 F 触发 Sprint", m.isHeld("Sprint"));

        // H8/H9 存 → 读 → 生效（面板改完就地落盘，运行时读回来）
        const std::string text = m.toText();
        InputMap back;
        std::string err;
        const bool okLoad = back.loadFromText(text, &err);
        icheck("H8 重绑定后的表能往返", okLoad, err);
        icheck("H9 往返后键盘仍是 F",
               back.describeBinding("Gameplay", "Sprint").find("F") !=
                   std::string::npos);

        // H10/H11 未定义的动作被拒绝，且**表一个字节都不变**
        const std::string before = m.toText();
        icheck("H10 未定义动作返回 false",
               !m.setActionBinding("Gameplay", "NoSuchAction", f));
        icheck("H11 拒绝时绑定表不变", m.toText() == before);

        // H12/H13 单项重置 = 回到出厂（键盘 + 手柄都要回来）
        icheck("H12 resetActionBinding 成功",
               m.resetActionBinding("Gameplay", "Sprint"));
        const std::string reset = m.describeBinding("Gameplay", "Sprint");
        icheck("H13 重置后回到出厂（Shift 回来了）",
               reset.find("Shift") != std::string::npos, reset);

        // H14/H15 清空键盘绑定 ≠ 删动作（动作是轴的锚点）
        m.clearActionBinding("Gameplay", "Sprint", InputSource::Key);
        icheck("H14 清空后只剩手柄（不是 (unbound)）",
               m.describeBinding("Gameplay", "Sprint") != "(unbound)" &&
                   m.describeBinding("Gameplay", "Sprint").find("PadBtn") !=
                       std::string::npos,
               m.describeBinding("Gameplay", "Sprint"));
        const InputContext* gc = m.context("Gameplay");
        icheck("H15 动作定义仍在（删了会让整根轴静默失效）",
               gc != nullptr && gc->actions.count("Sprint") == 1);

        // H16 完全不存在的上下文 → 拒绝而不是新建
        icheck("H16 不存在的上下文返回 false",
               !m.setActionBinding("NoSuchContext", "Jump", f) &&
                   m.context("NoSuchContext") == nullptr);
    }

    std::printf("INPUT-TOTAL pass=%d fail=%d\n", g_inputPass, g_inputFail);
    return g_inputFail;
}

} // namespace core
