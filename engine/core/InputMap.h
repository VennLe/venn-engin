#pragma once
// ============================================================
// core/InputMap —— 动作 / 轴的输入映射（G06）
//
// 游戏逻辑只认**动作名**（`Jump` / `MoveForward`），不认键码。这条分层
// 不是为了好看，而是为了下面三件事能各自独立变化：
//
//   · 玩家改成手柄玩       → 绑定点改，角色代码一行不动
//   · 打开菜单时角色停下   → 压一个 `Menu` 上下文，绑定整组失效
//   · 重绑定 UI（E 系列）  → 只读写这张表
//
// ------------------------------------------------------------
// 为什么不改 `core::Input`
// ------------------------------------------------------------
// `Input` 是**原始输入**（键码 / 鼠标 / 边沿），编辑器视口导航直接依赖它，
// 语义越简单越好。`InputMap` 是**语义输入**，建在它上面。两者分开之后：
// 编辑器一格代码都不用改，运行时用到的仍是同一份经过验证的按键状态。
//
// ------------------------------------------------------------
// 上下文栈：只有**栈顶**的绑定生效
// ------------------------------------------------------------
// 这是"打开菜单，角色还在跑"这类 bug 的根治办法 —— 它不是靠每个调用点
// 都记得 `if (!menuOpen)`，而是靠**只有一层绑定被求值**。
//
//     pushContext("Gameplay")   // WASD / Space
//     pushContext("Menu")       // Esc / Up / Down / Enter
//     → 此时 Gameplay 的 Jump 恒为 false，哪怕 Space 真的按着
//     popContext()              // 回到 Gameplay
//
// ------------------------------------------------------------
// 死区在**轴合成之后**
// ------------------------------------------------------------
// 单个摇杆轴先按各自的 deadzone 归一化，再合成 `positive - negative`，
// 最后对**合成结果**再做一次死区。只在单轴上做死区的话，两个半推的摇杆
// （各 0.3）合起来是 0.6 —— 角色会自己慢慢爬。
// ============================================================

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace core {

// 绑定的一条来源。键盘 / 鼠标 / 手柄按钮 / 手柄轴共用这一个结构
//（而不是四个 union 分支）：重绑定 UI 与 JSON 读写都只面对一种形状。
enum class InputSource : int {
    Key = 0,            // code = GLFW_KEY_*
    MouseButton = 1,    // code = GLFW_MOUSE_BUTTON_*
    GamepadButton = 2,  // code = GLFW_GAMEPAD_BUTTON_*
    GamepadAxis = 3,    // code = GLFW_GAMEPAD_AXIS_*，value ∈ [-1,1] × scale
};

struct InputBinding {
    InputSource source = InputSource::Key;
    int code = 0;
    // 轴来源的方向（+1 / -1）。键盘 / 按钮恒为 +1。
    float scale = 1.0f;
    // 手柄轴的死区（只对 GamepadAxis 有意义）。
    float deadzone = 0.20f;
    // 修饰键（只对键盘有意义）：三者都要按住才算触发。
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
};

// 一帧的**原始**输入快照。真实运行时由 GLFW 填，自检时由测试代码填 ——
// 于是"边沿是否正确"这类断言不需要窗口、不需要真按键。
struct RawInput {
    static constexpr int kKeys = 512;
    static constexpr int kMouseButtons = 8;
    static constexpr int kPadButtons = 15;
    static constexpr int kPadAxes = 6;

    std::array<bool, kKeys> keys{};
    std::array<bool, kMouseButtons> mouse{};
    std::array<bool, kPadButtons> padButtons{};
    std::array<float, kPadAxes> padAxes{};
    bool padConnected = false;

    void clear() {
        keys.fill(false);
        mouse.fill(false);
        padButtons.fill(false);
        padAxes.fill(0.0f);
        padConnected = false;
    }
};

// 一个上下文的全部绑定。名字就是 JSON 里的键（"Gameplay" / "Menu" / "UI"）。
struct InputContext {
    std::string name;
    // 动作 → 若干来源。任一来源成立即"成立"（多绑定的语义是 OR）。
    std::unordered_map<std::string, std::vector<InputBinding>> actions;
    // 轴 → (负向动作, 正向动作)
    std::unordered_map<std::string, std::pair<std::string, std::string>> axes;
    // 轴的合成死区（对合成结果再做一次）
    float axisDeadzone = 0.20f;
};

struct ActionState {
    bool held = false;
    bool pressed = false;
    bool released = false;
    float value = 0.0f;   // 模拟量：键盘 = 1，手柄轴 = |v|
};

class InputMap {
public:
    InputMap();

    // ------------------------------------------------ 定义（初始化期）
    // 定义动作（已存在则什么都不做）。动作必须在**用它的上下文**里定义。
    void defineAction(const std::string& context, const std::string& action);
    void bindKey(const std::string& context, const std::string& action,
                 int key, bool ctrl = false, bool shift = false,
                 bool alt = false);
    void bindMouse(const std::string& context, const std::string& action,
                   int button);
    void bindGamepadButton(const std::string& context,
                           const std::string& action, int button);
    void bindGamepadAxis(const std::string& context,
                         const std::string& action, int axis,
                         float scale = 1.0f, float deadzone = 0.20f);
    void defineAxis(const std::string& context, const std::string& axis,
                    const std::string& negative, const std::string& positive);
    // 清空某个动作的全部绑定（重绑定 UI 的"重置这一项"）
    void clearBindings(const std::string& context, const std::string& action);
    // 轴合成之后的死区（0.2 = 摇杆推不到 20% 就当没动）
    void setAxisDeadzone(const std::string& context, float deadzone);

    // 出厂绑定：移动 / 视角 / 跳跃 / 冲刺 / 交互 / 暂停
    static InputMap makeDefault();
    // 把出厂绑定灌进当前实例（已经改过的绑定会被覆盖 —— 只在"恢复默认"时调）
    void resetToDefault();

    // ------------------------------------------------ 上下文栈
    void pushContext(const std::string& name);
    bool popContext();
    void clearContexts();
    const std::string& topContext() const;
    int depth() const { return static_cast<int>(m_stack.size()); }

    // ------------------------------------------------ 采样
    // 从 core::Input + GLFW 手柄采样一帧。**每帧一次**（在固定步之前）。
    // 鼠标位移由这里消费并缓存 —— 别的地方再调 Input::mouseDelta 会拿到 0。
    void poll();
    // 注入一帧原始输入（自检 / 录像回放）。内部只做边沿计算，不碰 GLFW。
    void pollRaw(const RawInput& raw);
    // 当前缓存的鼠标位移（look 轴用），单位像素
    glm::vec2 lookDelta() const { return m_lookDelta; }

    // ------------------------------------------------ 查询
    const ActionState* action(const std::string& name) const;
    bool isHeld(const std::string& action) const;
    bool wasPressed(const std::string& action) const;
    bool wasReleased(const std::string& action) const;
    float axis(const std::string& name) const;
    bool gamepadConnected() const { return m_padConnected; }

    // ------------------------------------------------ 绑定表读写
    bool loadFromFile(const std::string& path);
    bool saveToFile(const std::string& path) const;
    bool loadFromText(const std::string& text, std::string* error = nullptr);
    std::string toText() const;
    // 单行摘要（日志 / 自检用）：`Gameplay=12 actions, Menu=4 actions`
    std::string describe() const;
    // 某个动作的可读绑定描述（重绑定 UI 的提示："W", "Space", "Pad A"）
    std::string describeBinding(const std::string& context,
                                const std::string& action) const;
    // **单条**绑定的可读文本。与 describeBinding 同一个格式化器，但不需要
    // 先进表 —— 捕获键位时"刚按下、还没绑上"的那一条也要显示得出来。
    std::string describeBindingOne(const InputBinding& binding) const;
    std::vector<std::string> actionNames(const std::string& context) const;

    // ------------------------------------------------ 重绑定（G06 的 UI 写入口）
    // 下面四个接口是**编辑器重绑定面板唯一的写路径**。它们刻意放在引擎侧：
    // 断言"改一个键 → 存 → 读 → 生效"这条链要在无窗口自检里跑，而面板要
    // 窗口 + ImGui。
    //
    // ⚠ 语义：只替换**同一来源类型**的绑定。
    //   玩家在面板里把 Jump 从 Space 改成 F，意思是"键盘上改按 F"，
    //   不是"顺便把手柄 A 也解绑"。整组清掉会让手柄玩家莫名少一个键，
    //   而且"键盘 + 手柄双绑定"本来就是默认表的设计（见 makeDefault）。

    // 把某个动作 `binding.source` 这一类绑定替换成这一条，其他来源原样保留。
    // 返回 false = 上下文不存在或动作从未定义（此时**不改任何东西**）。
    bool setActionBinding(const std::string& context, const std::string& action,
                          const InputBinding& binding);
    // 清空某个动作某一类来源的绑定。**动作定义保留** —— 动作是轴的锚点
    //（见 defineAxis 的注释），删掉会让整根轴静默失效。
    void clearActionBinding(const std::string& context, const std::string& action,
                            InputSource source);
    // 把某个动作恢复成**出厂绑定**（面板上的"重置这一项"）。
    // 实现是从 makeDefault() 里抄那一条：出厂表只有一份，面板不另存一份，
    // 否则两边迟早不同步（"重置"会把默认值改回旧版）。
    bool resetActionBinding(const std::string& context,
                            const std::string& action);

    // 同一上下文里**已经占用了这个键**的其他动作（重绑定冲突检测）。
    //
    // ⚠ 只比 (source, code)，修饰键不参与：对"这个键会不会被抢"这个问题
    //   来说，Shift+W 和 W 是同一件事 —— 玩家按 W 时两者都可能成立。
    // ⚠ **手柄轴的正负两半不算冲突**：MoveForward / MoveBack 都建在
    //   LEFT_Y 上（scale ±1），它们本来就是同一根轴的两个方向。
    //   不排除的话出厂表会报 8 对假冲突，真冲突被淹掉。
    // ⚠ 只查**同一个上下文**：Gameplay 的 Space 与 Menu 的 Space 是刻意
    //   共存的（压栈后只有栈顶生效），跨上下文报冲突等于永远报冲突。
    std::vector<std::string> conflictingActions(
        const std::string& context, const InputBinding& binding,
        const std::string& excludeAction) const;

    // 全部上下文名（已排序）。UI 的上下文选择器需要枚举 —— context()
    // 只能按名字查单个，枚举不出来。
    std::vector<std::string> contextNames() const;

    // 只读访问（序列化 / 冲突检测用）
    const InputContext* context(const std::string& name) const;
    const std::vector<std::string>& stack() const { return m_stack; }

    // ------------------------------------------------ 日志
    void setTrace(bool on) { m_trace = on; }

private:
    InputContext& ensureContext(const std::string& name);
    void evaluate(const RawInput& raw);
    static float axisValueOf(const InputBinding& b, const RawInput& raw);
    static bool bindingActive(const InputBinding& b, const RawInput& raw);

    std::unordered_map<std::string, InputContext> m_contexts;
    std::vector<std::string> m_stack;
    std::unordered_map<std::string, ActionState> m_state;
    std::unordered_map<std::string, ActionState> m_prev;

    glm::vec2 m_lookDelta{0.0f};
    bool m_padConnected = false;
    bool m_trace = false;
    // 上下文切换后的**第一帧**吞掉边沿（pressed / released）。
    // 为什么需要：从 Menu 弹回 Gameplay 时，如果 Space 还按着，按"正常"
    // 的边沿算法它会被判成一次**新的按下** —— 玩家用 Space 确认菜单，
    // 菜单一关角色就跳一下。这不是输入坏了，是"跨上下文不该继承边沿"。
    bool m_suppressEdges = false;
    // 上一帧的原始快照（边沿判定用；同时让"按住不放"不重复触发）
    RawInput m_raw;
};

// ------------------------------------------------------------ 自检
// 无窗口、无 GLFW：直接注入 RawInput，验证边沿 / 上下文 / 死区 / JSON 往返。
// 由 `Game.exe --selftest input` 调用（与 physics 的 MYVK_PHYS_TEST 同一条路）。
int runInputSelfTest();

} // namespace core
