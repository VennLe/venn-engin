#pragma once
// ============================================================
// editor/script/ScriptEngine —— 极简脚本语言 + 引擎属性绑定 + 热重载
//
// ------------------------------------------------------------
// 一、为什么自己写一个脚本语言，而不是嵌 Lua / Wren
// ------------------------------------------------------------
//   · 本项目是离线构建的教学向引擎，third_party 里没有脚本 VM，
//     引入 = 新增一份需要长期维护的第三方源码；
//   · 编辑器需要的能力其实很窄：**每帧对一个实体做属性动画**。
//     为此背一整套通用语言的语义与 GC 不划算；
//   · 自己写之后，"脚本能碰什么"完全由 ScriptHost 决定 ——
//     语言里根本没有文件/网络/系统调用，安全性是结构上的。
//
//   整套东西只有两个文件，语言特性一目了然，改起来毫无负担。
//   如果将来要换成 Lua：只需要按同一个接缝（ScriptHost +
//   "每帧求值一次"的语义）重新实现本类即可，编辑器其余代码不用动。
//
// ------------------------------------------------------------
// 二、语言（文件扩展名 .vks）
// ------------------------------------------------------------
//   # 注释到行尾
//   变量 = 表达式
//   函数调用(...)                    # 表达式语句
//   if (条件) 语句 [else 语句]
//   while (条件) { 语句... }
//   语句之间用换行或 ';' 分隔
//
//   运算符：+ - * / %   < <= > >= == !=   && || !
//   内置变量：time / dt / frame / index
//   内置函数（数学）：sin cos tan asin acos atan atan2 abs floor ceil
//                     round sqrt pow min max clamp lerp
//   内置函数（引擎）：move moveBy rotate rotateBy scale scaleBy
//                     setColor setIntensity setRange setDirection
//                     setVisible setEmissive
//                     print
//
//   数值一律是 double；角度（rotate）单位是**度**。
//
// ------------------------------------------------------------
// 三、热重载
// ------------------------------------------------------------
//   每个脚本记录了源文件的 last_write_time。每帧（或定时）扫一遍，
//   时间戳变了就重新读盘 + 重新编译：成功则原子替换，失败则**保留旧的
//   可用版本**并把错误显示在面板上 —— 编辑到一半的语法错误不该让
//   场景里的物体直接僵住。
// ============================================================

#include "ecs/Entity.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace scene {
class Scene;
}

namespace editor {

// 编译期 / 运行期错误
struct ScriptError {
    int line = 0;
    std::string message;
    bool ok() const { return message.empty(); }
    std::string toString() const {
        if (message.empty()) return {};
        return "line " + std::to_string(line) + ": " + message;
    }
};

// 脚本与引擎之间的**唯一**接缝：脚本能碰到的世界就这么多。
// 想扩展脚本能力，只需要在这里加字段 + 在求值器里加一个内置函数。
struct ScriptHost {
    scene::Scene* scene = nullptr;
    ecs::Entity entity{};
};

// ---------------------------------------------------------------- AST（内部）

struct Expr;
struct Stmt;
using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;

// ---------------------------------------------------------------- 脚本程序

// 一个已编译的脚本。不可拷贝（持有 AST）。
class ScriptProgram {
public:
    ScriptProgram() = default;
    ~ScriptProgram();
    ScriptProgram(const ScriptProgram&) = delete;
    ScriptProgram& operator=(const ScriptProgram&) = delete;

    // 编译源码；失败时 valid()==false，err 里是原因
    bool compile(const std::string& source, const std::string& debugName,
                 ScriptError& err);

    bool valid() const { return m_valid; }
    const std::string& source() const { return m_source; }
    const std::string& debugName() const { return m_debugName; }

    // 求值一次（脚本里的 time/dt/frame 由调用方给出）。
    // 返回 false 时 err 里是运行期错误（例如语句数超限的死循环）。
    bool run(const ScriptHost& host, double time, double dt, uint64_t frame,
             ScriptError& err);

    // 脚本里 print(...) 的输出（每次 run 前自动清空）
    const std::vector<std::string>& prints() const { return m_prints; }
    // 求值器内部用的输出出口（脚本语言的 print 落到这里）
    void addPrint(std::string s);

private:
    std::vector<StmtPtr> m_ast;
    std::string m_source;
    std::string m_debugName;
    bool m_valid = false;
    std::vector<std::string> m_prints;
};

// ---------------------------------------------------------------- 脚本资产

// 磁盘上的一个脚本 + 热重载状态
class ScriptAsset {
public:
    ScriptAsset() = default;

    // 由 ScriptEngine 设置"场景里记的那个路径"
    void setRequestedPath(std::string p) { m_requestedPath = std::move(p); }

    const std::string& requestedPath() const { return m_requestedPath; }
    const std::string& resolvedPath() const { return m_resolvedPath; }

    // 读盘 + 编译。返回是否成功（失败时保留上一次可用的程序）
    bool load();

    // 检查文件时间戳，变了就重载。返回 true 表示发生了重载。
    bool poll();

    ScriptProgram* program() { return m_program ? m_program.get() : nullptr; }

    const ScriptError& lastError() const { return m_lastError; }
    // 累计重载次数（UI 上显示，用来确认热重载真的生效了）
    uint64_t reloadCount() const { return m_reloadCount; }
    bool fileExists() const { return m_fileExists; }

private:
    std::string m_requestedPath;  // 场景里记的那个相对路径
    std::string m_resolvedPath;   // AssetPath 解析后的真实路径
    std::unique_ptr<ScriptProgram> m_program;
    ScriptError m_lastError;
    uint64_t m_reloadCount = 0;
    int64_t m_lastWriteTime = 0;
    bool m_fileExists = false;
    bool m_loadedOnce = false;
};

// ---------------------------------------------------------------- 引擎

class ScriptEngine {
public:
    // 按（相对资产根的）路径取脚本，首次访问时加载。
    // 返回的指针在下一次 pollHotReload 之后依然有效（对象不搬）。
    ScriptAsset* get(const std::string& relativePath);

    // 扫描全部已加载脚本的时间戳，变了就重编译。返回本次重载的脚本数。
    int pollHotReload();

    // 立即重载某个脚本（面板上的"Reload"按钮）
    bool reloadNow(const std::string& relativePath);

    // 强制重新读盘（丢弃缓存）
    void clear();

    // 最近一次热重载的日志（滚动保留最后若干条）
    const std::vector<std::string>& log() const { return m_log; }
    void logLine(const std::string& s);

    size_t scriptCount() const { return m_assets.size(); }
    // 已加载脚本的路径列表（UI 用）
    std::vector<std::string> loadedPaths() const;

private:
    std::map<std::string, std::unique_ptr<ScriptAsset>> m_assets;
    std::vector<std::string> m_log;
};

} // namespace editor
