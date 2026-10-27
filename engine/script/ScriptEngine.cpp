#include "ScriptEngine.h"

#include "assets/AssetPath.h"
#include "assets/Material.h"
#include "core/Logger.h"
#include "ecs/Components.h"
#include "scene/Scene.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace vks {

// ============================================================
// AST —— 头文件只做前向声明（struct Expr; struct Stmt;），
// 完整定义留在本 .cpp 里：语法细节不对外暴露，但类型身份必须是
// vks::Expr / vks::Stmt —— 也就是头文件里 StmtPtr / ExprPtr
// 指向的那两个类型。
//
// 注意：**绝不能**把这两个结构体放进下面的匿名命名空间。
// 匿名命名空间里的 Expr/Stmt 是**另外两个类型**，和 vks::Expr /
// vks::Stmt 毫无关系，于是 StmtPtr 与 parse* 的返回值对不上，
// 编译器会报一堆 "could not convert unique_ptr<anonymous::Stmt> to
// unique_ptr<vks::Stmt>"。
//
// 放在 namespace vks 里同样对外不可见：别的编译单元只看到前向
// 声明，拿不到布局，更无法构造。
// ============================================================

// ---------------------------------------------------------------- 内建函数表（S1）
//
// 修 P5：36 个 `if (name == "…")` 顺序比较 → 一次 switch。
//   · 每次调用省掉平均 18 次 std::string 比较（不是主要理由）；
//   · **拼错函数名 = 编译错**（这是主要理由）。以前只能在运行期报，
//     而热重载是"编译通过才替换"—— 拼错时**旧版本还在跑**，你以为改了
//     其实没改，日志里那行 WARN 还被刷屏。
//
// ⚠ 加新内建函数要**同步改三处**（enum / kBuiltins / Evaluator::call 的
//   switch）。漏一处编译不过 —— 这正是 S-D6 要的"编译期强制同步"。
enum class Builtin : uint16_t {
    None = 0,

    // ---- 数学（17）----
    Sin, Cos, Tan, Asin, Acos, Atan, Abs, Floor, Ceil, Round, Sqrt,
    Atan2, Pow, Min, Max, Clamp, Lerp,

    // ---- 输出 ----
    Print,

    // ---- 值构造（S1）----
    Vec3, Count, At,

    // ---- 自我 / 查询（S1）----
    // `near` 在 S1 是**朴素版**（全扫 Transform 池 + 排序），S2 换成
    // "BVH 快查 + 补扫"，语义与排序规则不变。
    Self, NameOf, Exists, Find, Near, Dist,

    // ---- 变换（老签名保留）----
    Move, MoveBy, Rotate, RotateBy, Scale, ScaleBy,

    // ---- 世界访问（S2）----
    // 玩家 / 逐个实体读写变换 / 刚体 / 黑板 / 可见性。
    // ⚠ 与上面"老签名"那一组的区别：老的对**隐式 self** 生效，新的都带
    //   显式 `e` 参数（S-D5：脚本要能操作"别人"，而不只是自己）。
    Player,
    Pos, SetPos, Rot, SetRot, RotY, ScaleOf,
    Vel, SetVel, Impulse, Grounded,
    Attr, SetAttr, WorldAttr, SetWorldAttr,
    Visible,

    // ---- 输入（S3）----
    // 参数是**动作名**（InputMap 里的 "Jump"），不是键码。
    Pressed, Held, Released, Axis,

    // ---- 协程（S4）----
    // 这三个**不是普通内建**：它们不在表达式里返回一个值，而是让执行流
    // 挂起（Parser 把它们单独解析成 `Stmt::Kind::Wait`）。放进这张表只为
    // 复用"编译期认得这个名字 + 参数个数检查"。
    // ⚠ 谁在表达式里写 `let x = wait(1)` → 编译失败（见 parsePrimary）。
    Wait, WaitFrames, WaitUntil,

    // ---- HUD（U8）----
    SetHealth, Damage, Popup,

    // ---- 音频（G14-8）----
    PlaySound, StopAllSounds, SetBusVolume,

    // ---- 灯光 ----
    SetColor, SetIntensity, SetRange, SetDirection,

    // ---- 可见 / 材质 ----
    SetVisible, SetEmissive,

    // ---- 演出（C6 导演 API）----
    // 这三件让脚本能"指挥"而不是"摆弄"：播过场 / 弹对话 / 切动画。
    // ⚠ 它们全是**宿主动作**（`ScriptWorldActions` 里的 std::function），
    //   engine 不认识 SequencerSystem / DialogueSystem 的具体形态 ——
    //   与 `playSound` 同一条分层理由，也顺带让编辑器 Play 与 Game.exe
    //   各自注入自己的实现。
    PlaySequence, ShowDialogue, PlayAnim,

    // ---- 世界控制（C6 第二批）----
    // 演出三件让脚本"指挥一段表演"，这四件让它"改一局游戏"：
    //   setFlag / getFlag  叙事分支的状态（好感度 / 见过没见过）
    //   spawn              刷实体进场景（刷怪 / 刷掉落物）
    //   loadScene          换关卡（下一章 / 回标题）
    // ⚠ 全是宿主动作：旗标存在 runtime 的 `GameFlags` 里、场景在
    //   `core::Application::m_scene` 里 —— engine 两样都够不着。
    SetFlag, GetFlag, Spawn, LoadScene,

    Count_
};

struct BuiltinInfo {
    const char* name;   // nullptr = 占位（None）
    uint8_t minArgs;
    uint8_t maxArgs;    // 可选参数用 min != max 表达
};

struct Expr {
    enum class Kind { Number, Str, Bool, Var, Unary, Binary, Call, Member };

    Kind kind = Kind::Number;
    double num = 0.0;
    bool flag = false;  // Bool 字面量
    std::string text;  // Var 名 / Call 名 / Str 内容
    std::string op;    // Unary / Binary 的运算符文本
    int line = 1;
    ExprPtr a, b;               // Unary 用 a；Binary 用 a / b；Member 用 a
    std::vector<ExprPtr> args;  // Call
    // Call：**编译期**就解析好的内建编号（S1，修 P5）。
    // Member：`text` 是 ".x/.y/.z" 的成员名。
    Builtin builtin = Builtin::None;
};

struct Stmt {
    // Let / Foreach 是 S1 新增的两条语句。
    //   Let：幂等声明（S-D2）—— "不存在才初始化"，专为热重载而设。
    //   Foreach：只为遍历 `near()` / `all()` 返回的 list 服务。
    //   Wait：`wait(...)` / `waitFrames(...)` / `waitUntil(...)`（S4）。
    //     单独一种语句而不是普通 ExprStmt —— 因为它要**推进 ip 再挂起**，
    //     这个动作只有执行器知道，藏在 call() 里会变成"返回值被丢弃"。
    enum class Kind { Assign, ExprStmt, If, While, Block, Let, Foreach, Wait };

    Kind kind = Kind::ExprStmt;
    // ★ 稳定语句序号（S4）。源码顺序（先序遍历）分配，**改字面量不变、
    //   增删语句就变**。热重载时协程栈靠它把"挂在哪条语句上"翻译到新
    //   AST：id 能对上 = 从原位置继续，对不上 = 整体重启（S-D7 第 3 条）。
    uint32_t id = 0;
    std::string name;           // Assign 的目标变量
    ExprPtr expr;               // Assign 值 / ExprStmt / If 条件 / While 条件
    std::vector<StmtPtr> body;  // Block / then / 循环体
    std::vector<StmtPtr> elseBody;
    int line = 1;
};

// ============================================================
// ScriptCoroutine（S4）—— 显式帧栈
// ============================================================
//
// 为什么必须是显式栈：`wait` 要"停在 while 循环体第 3 句、下次从这里继续"。
// 递归 exec 把这个位置放在 CPU 栈上，函数一返回就没了。所以执行器改成
// **自己维护一个 frames 数组**，每一帧记录"哪个语句列表、跑到第几条"。
//
// 三层帧：
//   Seq     —— 顺序执行一个 `vector<StmtPtr>`（顶层 / 块 / 分支 / 循环体）
//   While   —— 每轮重估条件，为真就压一个 Seq 跑循环体
//   Foreach —— 持有**元素快照**（不是 list 的引用：挂起期间世界会变），
//              每轮给循环变量赋值后压一个 Seq
//
// ⚠ `list` 指向 AST 里的 vector，生命周期 = ScriptProgram。所以派生出一个
//   硬约束：**热重载必须对账**（reattach），否则栈里全是悬垂指针。
class ScriptCoroutine {
public:
    enum class State : uint8_t { Idle, Waiting };
    enum class Wake : uint8_t { None, Time, Frames, Until };

    struct Frame {
        enum class K : uint8_t { Seq, While, Foreach };

        K kind = K::Seq;
        // 指向的语句列表。⚠ 热重载后必须由 reattach 换成新 AST 的。
        const std::vector<StmtPtr>* list = nullptr;
        // 这个列表"属于"哪条语句（0 = 顶层 m_ast；钩子体 = HookBlock::id）。
        // 它就是跨版本翻译的锚点 —— 见 ScriptProgram::reattach。
        uint32_t ownerId = 0;
        uint8_t branch = 0;   // 0 = body/then，1 = elseBody
        uint32_t ip = 0;      // Seq：下一条要跑的语句下标
        uint32_t end = 0;     // Seq：上界（声明区只跑前 m_declCount 条）

        // While
        const Expr* cond = nullptr;
        int line = 1;

        // Foreach
        std::vector<Value> items;
        size_t itemIdx = 0;
        std::string var;
        bool varHad = false;
        Value varSaved;
    };

    State state = State::Idle;
    Wake wake = Wake::None;
    double wakeAt = 0.0;
    uint64_t wakeFrame = 0;
    // waitUntil 的条件：指向 AST，且必须记住"在哪条语句里"，重载时重绑。
    const Expr* cond = nullptr;
    uint32_t condOwnerId = 0;
    int condLine = 1;

    std::vector<Frame> frames;

    // 相位机（S4）：0 声明区 / 1 start / 2 接触事件 / 3 顶层 / 4 update
    uint8_t phase = 0;
    size_t contactIndex = 0;
    size_t hookIndex = 0;

    void clear() {
        state = State::Idle;
        wake = Wake::None;
        wakeAt = 0.0;
        wakeFrame = 0;
        cond = nullptr;
        condOwnerId = 0;
        condLine = 1;
        frames.clear();
        phase = 0;
        contactIndex = 0;
        hookIndex = 0;
    }
    bool waiting() const { return state == State::Waiting; }
    // 有没跑完的东西（挂起中）—— 热重载要不要对账看这个
    bool midFlight() const { return !frames.empty() || state == State::Waiting; }
};

namespace {

// 单次求值的总步数上限：脚本里写出死循环时报错，
// 而不是把编辑器挂死
constexpr int kMaxSteps = 200000;

// ★ S2：每帧每实例的**内建调用**上限。
//
// 步数预算（kMaxSteps）挡的是"死循环里做算术"，但挡不住"循环里查世界"：
// 30000 次 `pos()` 只有约 30000 步，远在预算之内，却要做 30000 次世界
// 矩阵累乘。这类脚本不会崩，只会让帧时间悄悄翻倍 —— 最难查的一类。
//
// 默认 20000 是"够写任何正常玩法"的量级。⚠ **实测**（Debug 构建）跑满
// 20000 次要 ~148 ms —— 也就是说这个上限在 Debug 里仍然能把一帧拖长，
// 它挡的是"无限增长"，不是"性能预算"（真正的帧预算靠 Release + 单次
// 调用本身够快）。`MYVK_VKS_QUOTA` 可调；自动化里压到 100 能在毫秒内
// 测出熔断。
int builtinQuota() {
    static const int v = [] {
        if (const char* e = std::getenv("MYVK_VKS_QUOTA")) {
            const int raw = std::atoi(e);
            if (raw > 0) return raw;
        }
        return 20000;
    }();
    return v;
}

// 可变参数（目前只有 print）。用一个"够大"的上界而不是特判：
// arity 检查的职责是"抓写错"，不是"精确表达签名"。
constexpr uint8_t kVarArgs = 255;

// ⚠ 这张表**必须与 `enum class Builtin` 的顺序逐项对齐** ——
//   下标就是 enum 值。下面有 static_assert 兜住"加漏了"。
const BuiltinInfo kBuiltins[] = {
    {nullptr, 0, 0},              // None
    {"sin", 1, 1},   {"cos", 1, 1},   {"tan", 1, 1},
    {"asin", 1, 1},  {"acos", 1, 1},  {"atan", 1, 1},
    {"abs", 1, 1},   {"floor", 1, 1}, {"ceil", 1, 1},
    {"round", 1, 1}, {"sqrt", 1, 1},
    {"atan2", 2, 2}, {"pow", 2, 2},   {"min", 2, 2}, {"max", 2, 2},
    {"clamp", 3, 3}, {"lerp", 3, 3},
    {"print", 0, kVarArgs},
    {"vec3", 3, 3},  {"count", 1, 1}, {"at", 2, 2},
    {"self", 0, 0},  {"name", 1, 1},  {"exists", 1, 1},
    {"find", 1, 1},  {"near", 2, 3},  {"dist", 2, 2},
    {"move", 3, 3},  {"moveBy", 3, 3},
    {"rotate", 3, 3}, {"rotateBy", 3, 3},
    {"scale", 3, 3}, {"scaleBy", 3, 3},
    // S2 世界访问
    {"player", 0, 0},
    {"pos", 1, 1},   {"setPos", 2, 2},
    {"rot", 1, 1},   {"setRot", 2, 2},  {"rotY", 1, 1},  {"scaleOf", 1, 1},
    {"vel", 1, 1},   {"setVel", 2, 2},  {"impulse", 2, 2}, {"grounded", 1, 1},
    {"attr", 2, 3},  {"setAttr", 3, 3},
    {"worldAttr", 1, 2}, {"setWorldAttr", 2, 2},
    {"visible", 1, 1},
    // S3 输入
    {"pressed", 1, 1}, {"held", 1, 1}, {"released", 1, 1}, {"axis", 1, 1},
    // S4 协程（只能单独成句）
    {"wait", 1, 1}, {"waitFrames", 1, 1}, {"waitUntil", 1, 1},
    {"setHealth", 1, 1}, {"damage", 1, 1}, {"popup", 1, 1},
    {"playSound", 1, 1}, {"stopAllSounds", 0, 0}, {"setBusVolume", 2, 2},
    {"setColor", 3, 3}, {"setIntensity", 1, 1}, {"setRange", 1, 1},
    {"setDirection", 3, 3},
    {"setVisible", 1, 2}, {"setEmissive", 3, 3},
    // C6：演出三件。`playAnim` 的第二个参数是可选的显式实体。
    {"playSequence", 1, 1}, {"showDialogue", 1, 1}, {"playAnim", 1, 2},
    // C6 第二批：世界四件。`spawn` 的位置可选（不给 = 原点）。
    {"setFlag", 2, 2}, {"getFlag", 1, 1}, {"spawn", 1, 2}, {"loadScene", 1, 1},
};

static_assert(sizeof(kBuiltins) / sizeof(kBuiltins[0]) ==
                  static_cast<size_t>(Builtin::Count_),
              "kBuiltins 与 enum class Builtin 对不上 —— 加内建函数时"
              "必须同时改 enum / 表 / switch 三处（S-D6）");

const BuiltinInfo& builtinInfo(Builtin b) {
    return kBuiltins[static_cast<uint16_t>(b)];
}

// 只被 Parser 调用（每个调用点一次），45 项线性扫完全够 ——
// 换成 map 反而要为它写一个静态初始化。
Builtin findBuiltin(const std::string& name) {
    for (uint16_t i = 1; i < static_cast<uint16_t>(Builtin::Count_); ++i) {
        if (name == kBuiltins[i].name) return static_cast<Builtin>(i);
    }
    return Builtin::None;
}

// ---------------------------------------------------------------- 词法

enum class T : uint8_t {
    End, Number, Str, Ident,
    Plus, Minus, Star, Slash, Percent,
    LParen, RParen, LBrace, RBrace, Comma, Semi, Dot,
    Assign, Eq, Ne, Lt, Le, Gt, Ge, AndAnd, OrOr, Not,
};

struct Token {
    T kind = T::End;
    double num = 0.0;
    std::string text;
    int line = 1;
};

class Lexer {
public:
    explicit Lexer(const std::string& src) : m_s(src) {}

    bool tokenize(std::vector<Token>& out, ScriptError& err) {
        while (!atEnd()) {
            const char c = peek();
            if (c == ' ' || c == '\t' || c == '\r') { advance(); continue; }
            if (c == '#') {  // 注释到行尾
                while (!atEnd() && peek() != '\n') advance();
                continue;
            }
            if (c == '\n') {
                advance();
                emit(out, T::Semi, "\n", 0.0, m_line);
                continue;
            }

            const int line = m_line;

            // ⚠ `.` 有两种身份：小数点和成员访问。只有**后面跟数字**时
            //   才是小数点 —— 否则 `p.x` 会在词法阶段就报 "unexpected '.'"。
            if (std::isdigit(static_cast<unsigned char>(c)) ||
                (c == '.' && std::isdigit(
                                 static_cast<unsigned char>(peek(1))))) {
                if (!lexNumber(out, err, line)) return false;
                continue;
            }
            if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
                lexIdent(out, line);
                continue;
            }
            if (c == '"') {
                if (!lexString(out, err, line)) return false;
                continue;
            }
            if (!lexOperator(out, err, line)) return false;
        }
        emit(out, T::End, "", 0.0, m_line);
        return true;
    }

private:
    bool atEnd() const { return m_i >= m_s.size(); }

    char peek(size_t k = 0) const {
        return m_i + k < m_s.size() ? m_s[m_i + k] : '\0';
    }

    void advance() {
        if (peek() == '\n') ++m_line;
        ++m_i;
    }

    static void emit(std::vector<Token>& out, T k, std::string text, double n,
                     int line) {
        Token t;
        t.kind = k;
        t.text = std::move(text);
        t.num = n;
        t.line = line;
        out.push_back(std::move(t));
    }

    bool lexNumber(std::vector<Token>& out, ScriptError& err, int line) {
        const size_t start = m_i;
        bool seenDot = false;
        while (!atEnd()) {
            const char c = peek();
            if (std::isdigit(static_cast<unsigned char>(c))) { advance(); continue; }
            if (c == '.' && !seenDot) { seenDot = true; advance(); continue; }
            break;
        }
        const std::string text = m_s.substr(start, m_i - start);
        if (text == ".") {
            err.line = line;
            err.message = "unexpected '.'";
            return false;
        }
        emit(out, T::Number, text, std::strtod(text.c_str(), nullptr), line);
        return true;
    }

    void lexIdent(std::vector<Token>& out, int line) {
        const size_t start = m_i;
        while (!atEnd()) {
            const char c = peek();
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
                advance();
                continue;
            }
            break;
        }
        emit(out, T::Ident, m_s.substr(start, m_i - start), 0.0, line);
    }

    bool lexString(std::vector<Token>& out, ScriptError& err, int line) {
        advance();  // 开引号
        std::string s;
        while (!atEnd() && peek() != '"') {
            if (peek() == '\n') {
                err.line = line;
                err.message = "unterminated string";
                return false;
            }
            if (peek() == '\\' && peek(1) != '\0') {
                advance();
                const char e = peek();
                s.push_back(e == 'n' ? '\n' : (e == 't' ? '\t' : e));
                advance();
                continue;
            }
            s.push_back(peek());
            advance();
        }
        if (atEnd()) {
            err.line = line;
            err.message = "unterminated string";
            return false;
        }
        advance();  // 闭引号
        emit(out, T::Str, std::move(s), 0.0, line);
        return true;
    }

    bool lexOperator(std::vector<Token>& out, ScriptError& err, int line) {
        const char c = peek();
        const char c1 = peek(1);
        auto one = [&](T k) {
            advance();
            emit(out, k, std::string(1, c), 0.0, line);
        };
        auto two = [&](T k, const char* txt) {
            advance();
            advance();
            emit(out, k, txt, 0.0, line);
        };

        switch (c) {
            case '+': one(T::Plus); return true;
            case '-': one(T::Minus); return true;
            case '*': one(T::Star); return true;
            case '/': one(T::Slash); return true;
            case '%': one(T::Percent); return true;
            case '(': one(T::LParen); return true;
            case ')': one(T::RParen); return true;
            case '{': one(T::LBrace); return true;
            case '}': one(T::RBrace); return true;
            case ',': one(T::Comma); return true;
            case ';': one(T::Semi); return true;
            case '.': one(T::Dot); return true;
            case '!':
                if (c1 == '=') { two(T::Ne, "!="); return true; }
                one(T::Not);
                return true;
            case '=':
                if (c1 == '=') { two(T::Eq, "=="); return true; }
                one(T::Assign);
                return true;
            case '<':
                if (c1 == '=') { two(T::Le, "<="); return true; }
                one(T::Lt);
                return true;
            case '>':
                if (c1 == '=') { two(T::Ge, ">="); return true; }
                one(T::Gt);
                return true;
            case '&':
                if (c1 == '&') { two(T::AndAnd, "&&"); return true; }
                break;
            case '|':
                if (c1 == '|') { two(T::OrOr, "||"); return true; }
                break;
            default: break;
        }

        err.line = line;
        err.message = std::string("unexpected character '") + c + "'";
        return false;
    }

    const std::string& m_s;
    size_t m_i = 0;
    int m_line = 1;
};

// ---------------------------------------------------------------- 语法

class Parser {
public:
    explicit Parser(const std::vector<Token>& toks) : m_t(toks) {}

    bool parseProgram(std::vector<StmtPtr>& out,
                      std::vector<std::unique_ptr<HookBlock>>& hooks,
                      ScriptError& err) {
        skipSeparators();
        while (!check(T::End)) {
            // `on` 块与普通语句混排，但去向不同（钩子表 vs 顶层 AST）
            if (check(T::Ident) && peek().text == "on") {
                if (!parseOn(hooks, err)) return false;
            } else {
                StmtPtr s = parseStatement(err);
                if (!s) return false;
                out.push_back(std::move(s));
            }
            skipSeparators();
        }
        return true;
    }

    // `on start { }` / `on collide(other) { }`
    bool parseOn(std::vector<std::unique_ptr<HookBlock>>& hooks,
                 ScriptError& err) {
        const Token& kw = peek();  // "on"
        ++m_i;
        const Token& nt = peek();
        if (nt.kind != T::Ident) {
            err.line = nt.line;
            err.message = "expected a hook name after 'on' "
                          "(start / update / collide / trigger / exit)";
            return false;
        }
        HookKind kind = HookKind::Update;
        if (!hookFromName(nt.text, &kind)) {
            err.line = nt.line;
            err.message = "unknown hook '" + nt.text +
                          "' (expected start / update / collide / trigger / exit)";
            return false;
        }
        ++m_i;

        auto blk = std::make_unique<HookBlock>();
        blk->kind = kind;
        blk->line = kw.line;
        // ⚠ 与语句共用一个发号器：协程栈里"挂在钩子体里"靠它对账。
        //   顺序也必须是源码顺序 —— 这里拿到的是解析完 on 头之后、
        //   解析体之前，与"先序遍历"的直觉一致（体里的语句 id 更大）。
        blk->id = m_nextId++;

        // 可选参数。⚠ 只有"接触类"钩子才允许带参数：`on update(x)` 无意义，
        //   而**允许**它会让用户以为 update 有参数可拿。
        if (check(T::LParen)) {
            if (kind == HookKind::Start || kind == HookKind::Update) {
                err.line = peek().line;
                err.message = std::string("on ") + hookName(kind) +
                              " does not take a parameter";
                return false;
            }
            ++m_i;
            const Token& pt = peek();
            if (pt.kind != T::Ident) {
                err.line = pt.line;
                err.message = "expected a parameter name inside '(...)'";
                return false;
            }
            blk->param = pt.text;
            ++m_i;
            if (!expect(T::RParen, "')' after the hook parameter", err))
                return false;
        }

        // 钩子体。⚠ m_depth++ 让 `let` 在这里**编译失败** —— 和 `{}` 里一样：
        //   钩子体每帧都跑，块内 let 会被永久跳过，那是个陷阱而不是特性。
        ++m_depth;
        if (!expect(T::LBrace, "'{' to open the hook body", err)) return false;
        skipSeparators();
        while (!check(T::RBrace) && !check(T::End)) {
            StmtPtr inner = parseStatement(err);
            if (!inner) return false;
            blk->body.push_back(std::move(inner));
            skipSeparators();
        }
        --m_depth;
        if (!expect(T::RBrace, "'}' to close the hook body", err)) return false;

        hooks.push_back(std::move(blk));
        return true;
    }

private:
    const Token& peek(size_t k = 0) const {
        const size_t i = m_i + k;
        return i < m_t.size() ? m_t[i] : m_t.back();
    }
    bool check(T k) const { return peek().kind == k; }
    bool match(T k) {
        if (!check(k)) return false;
        ++m_i;
        return true;
    }
    bool expect(T k, const char* what, ScriptError& err) {
        if (match(k)) return true;
        err.line = peek().line;
        err.message = std::string("expected ") + what;
        return false;
    }
    void skipSeparators() {
        while (check(T::Semi)) ++m_i;
    }

    // `wait` 家族**只能在语句位置**出现。放在表达式里（`let x = wait(1)`）
    // 无法表达"挂起后从哪里继续"，所以直接编译失败 —— 让用户在写的时候
    // 就知道，而不是上线后看到脚本每帧重跑一遍 wait。
    bool isWaitBuiltin(const std::string& name) const {
        return name == "wait" || name == "waitFrames" || name == "waitUntil";
    }

    StmtPtr parseStatement(ScriptError& err) {
        StmtPtr s = parseStatementInner(err);
        if (s) s->id = m_nextId++;
        return s;
    }

    StmtPtr parseStatementInner(ScriptError& err) {
        skipSeparators();
        // wait 家族单独成句（见 isWaitBuiltin 的注释）
        if (check(T::Ident) && isWaitBuiltin(peek().text)) return parseWait(err);
        if (check(T::LBrace)) return parseBlock(err);
        if (check(T::Ident) && peek().text == "if") return parseIf(err);
        if (check(T::Ident) && peek().text == "while") return parseWhile(err);
        if (check(T::Ident) && peek().text == "let") return parseLet(err);
        if (check(T::Ident) && peek().text == "foreach") return parseForeach(err);

        // 赋值要先看一眼第二个 token：`ident =`
        if (check(T::Ident) && peek(1).kind == T::Assign) {
            auto s = std::make_unique<Stmt>();
            s->kind = Stmt::Kind::Assign;
            s->line = peek().line;
            s->name = peek().text;
            m_i += 2;
            s->expr = parseExpr(err);
            if (!s->expr) return nullptr;
            return s;
        }

        auto s = std::make_unique<Stmt>();
        s->kind = Stmt::Kind::ExprStmt;
        s->line = peek().line;
        s->expr = parseExpr(err);
        if (!s->expr) return nullptr;
        return s;
    }

    // `wait(1.0)` / `waitFrames(10)` / `waitUntil(e > 3)` —— 语句，不是表达式
    StmtPtr parseWait(ScriptError& err) {
        auto s = std::make_unique<Stmt>();
        s->kind = Stmt::Kind::Wait;
        s->line = peek().line;
        m_waitStmt = true;
        s->expr = parseExpr(err);   // parsePrimary 内部会做内建名 + 参数个数检查
        m_waitStmt = false;
        if (!s->expr) return nullptr;
        // `wait(1) + 2` 这种：整句不是一次调用，执行器没有可挂起的锚点
        if (s->expr->kind != Expr::Kind::Call) {
            err.line = s->line;
            err.message = "wait family must stand alone on its line";
            return nullptr;
        }
        return s;
    }

    StmtPtr parseBlock(ScriptError& err) {
        // ⚠ `let` 只在**顶层**合法（S-D2）：进 `{}` / if / while 之后一律报错。
        //   代价写在文档里：用户在 `on start` 里写 `let` 会看到明确报错，
        //   而不是"每次 update 都静默跳过"。
        ++m_depth;
        auto s = std::make_unique<Stmt>();
        s->kind = Stmt::Kind::Block;
        s->line = peek().line;
        if (!expect(T::LBrace, "'{'", err)) return nullptr;
        skipSeparators();
        while (!check(T::RBrace) && !check(T::End)) {
            StmtPtr inner = parseStatement(err);
            if (!inner) return nullptr;
            s->body.push_back(std::move(inner));
            skipSeparators();
        }
        --m_depth;
        if (!expect(T::RBrace, "'}'", err)) return nullptr;
        return s;
    }

    StmtPtr parseIf(ScriptError& err) {
        auto s = std::make_unique<Stmt>();
        s->kind = Stmt::Kind::If;
        s->line = peek().line;
        ++m_i;  // if
        if (!expect(T::LParen, "'(' after if", err)) return nullptr;
        s->expr = parseExpr(err);
        if (!s->expr) return nullptr;
        if (!expect(T::RParen, "')'", err)) return nullptr;

        ++m_depth;
        StmtPtr thenStmt = parseStatement(err);
        --m_depth;
        if (!thenStmt) return nullptr;
        s->body.push_back(std::move(thenStmt));

        skipSeparators();
        if (check(T::Ident) && peek().text == "else") {
            ++m_i;
            ++m_depth;
            StmtPtr elseStmt = parseStatement(err);
            --m_depth;
            if (!elseStmt) return nullptr;
            s->elseBody.push_back(std::move(elseStmt));
        }
        return s;
    }

    StmtPtr parseWhile(ScriptError& err) {
        auto s = std::make_unique<Stmt>();
        s->kind = Stmt::Kind::While;
        s->line = peek().line;
        ++m_i;  // while
        if (!expect(T::LParen, "'(' after while", err)) return nullptr;
        s->expr = parseExpr(err);
        if (!s->expr) return nullptr;
        if (!expect(T::RParen, "')'", err)) return nullptr;

        ++m_depth;
        StmtPtr body = parseStatement(err);
        --m_depth;
        if (!body) return nullptr;
        s->body.push_back(std::move(body));
        return s;
    }

    // `let x = <expr>`（幂等声明，S-D2）
    StmtPtr parseLet(ScriptError& err) {
        auto s = std::make_unique<Stmt>();
        s->kind = Stmt::Kind::Let;
        s->line = peek().line;
        if (m_depth > 0) {
            err.line = s->line;
            err.message =
                "let is only allowed at the top level "
                "(use plain assignment 'x = ...' to reset a value)";
            return nullptr;
        }
        ++m_i;  // let
        if (!check(T::Ident)) {
            err.line = s->line;
            err.message = "expected a name after 'let'";
            return nullptr;
        }
        s->name = peek().text;
        ++m_i;
        if (!expect(T::Assign, "'=' after let <name>", err)) return nullptr;
        s->expr = parseExpr(err);
        if (!s->expr) return nullptr;
        return s;
    }

    // `foreach (e in <expr>) <statement>`
    StmtPtr parseForeach(ScriptError& err) {
        auto s = std::make_unique<Stmt>();
        s->kind = Stmt::Kind::Foreach;
        s->line = peek().line;
        ++m_i;  // foreach
        if (!expect(T::LParen, "'(' after foreach", err)) return nullptr;
        if (!check(T::Ident)) {
            err.line = s->line;
            err.message = "expected a loop variable after 'foreach ('";
            return nullptr;
        }
        s->name = peek().text;
        ++m_i;
        // `in` 在词法层是普通标识符（这个语言没有保留字表）→ 这里显式认它。
        if (!(check(T::Ident) && peek().text == "in")) {
            err.line = peek().line;
            err.message = "expected 'in' in foreach";
            return nullptr;
        }
        ++m_i;
        s->expr = parseExpr(err);
        if (!s->expr) return nullptr;
        if (!expect(T::RParen, "')'", err)) return nullptr;
        ++m_depth;
        StmtPtr body = parseStatement(err);
        --m_depth;
        if (!body) return nullptr;
        s->body.push_back(std::move(body));
        return s;
    }

    // ---- 表达式：优先级由低到高 ----
    ExprPtr parseExpr(ScriptError& err) { return parseOr(err); }

    ExprPtr parseOr(ScriptError& err) {
        ExprPtr left = parseAnd(err);
        while (left && check(T::OrOr)) {
            const int line = peek().line;
            ++m_i;
            ExprPtr right = parseAnd(err);
            if (!right) return nullptr;
            left = makeBinary("||", std::move(left), std::move(right), line);
        }
        return left;
    }

    ExprPtr parseAnd(ScriptError& err) {
        ExprPtr left = parseEquality(err);
        while (left && check(T::AndAnd)) {
            const int line = peek().line;
            ++m_i;
            ExprPtr right = parseEquality(err);
            if (!right) return nullptr;
            left = makeBinary("&&", std::move(left), std::move(right), line);
        }
        return left;
    }

    ExprPtr parseEquality(ScriptError& err) {
        ExprPtr left = parseComparison(err);
        while (left && (check(T::Eq) || check(T::Ne))) {
            const std::string op = check(T::Eq) ? "==" : "!=";
            const int line = peek().line;
            ++m_i;
            ExprPtr right = parseComparison(err);
            if (!right) return nullptr;
            left = makeBinary(op, std::move(left), std::move(right), line);
        }
        return left;
    }

    ExprPtr parseComparison(ScriptError& err) {
        ExprPtr left = parseTerm(err);
        while (left && (check(T::Lt) || check(T::Le) || check(T::Gt) ||
                        check(T::Ge))) {
            std::string op;
            switch (peek().kind) {
                case T::Lt: op = "<"; break;
                case T::Le: op = "<="; break;
                case T::Gt: op = ">"; break;
                default: op = ">="; break;
            }
            const int line = peek().line;
            ++m_i;
            ExprPtr right = parseTerm(err);
            if (!right) return nullptr;
            left = makeBinary(op, std::move(left), std::move(right), line);
        }
        return left;
    }

    ExprPtr parseTerm(ScriptError& err) {
        ExprPtr left = parseFactor(err);
        while (left && (check(T::Plus) || check(T::Minus))) {
            const std::string op = check(T::Plus) ? "+" : "-";
            const int line = peek().line;
            ++m_i;
            ExprPtr right = parseFactor(err);
            if (!right) return nullptr;
            left = makeBinary(op, std::move(left), std::move(right), line);
        }
        return left;
    }

    ExprPtr parseFactor(ScriptError& err) {
        ExprPtr left = parseUnary(err);
        while (left &&
               (check(T::Star) || check(T::Slash) || check(T::Percent))) {
            std::string op;
            switch (peek().kind) {
                case T::Star: op = "*"; break;
                case T::Slash: op = "/"; break;
                default: op = "%"; break;
            }
            const int line = peek().line;
            ++m_i;
            ExprPtr right = parseUnary(err);
            if (!right) return nullptr;
            left = makeBinary(op, std::move(left), std::move(right), line);
        }
        return left;
    }

    ExprPtr parseUnary(ScriptError& err) {
        if (check(T::Minus) || check(T::Not)) {
            const std::string op = check(T::Minus) ? "-" : "!";
            const int line = peek().line;
            ++m_i;
            ExprPtr operand = parseUnary(err);
            if (!operand) return nullptr;
            auto e = std::make_unique<Expr>();
            e->kind = Expr::Kind::Unary;
            e->op = op;
            e->line = line;
            e->a = std::move(operand);
            return e;
        }
        return parsePostfix(err);
    }

    // 成员访问（S1）：**全语言唯一的成员语法**，只认 .x / .y / .z。
    // 值得为 vec3 开这一次口子 —— 否则每一行都在 `at3(v,0)` 里拆分量。
    ExprPtr parsePostfix(ScriptError& err) {
        ExprPtr e = parsePrimary(err);
        while (e && check(T::Dot)) {
            const int line = peek().line;
            ++m_i;
            if (!check(T::Ident)) {
                err.line = line;
                err.message = "expected a member name after '.'";
                return nullptr;
            }
            auto m = std::make_unique<Expr>();
            m->kind = Expr::Kind::Member;
            m->text = peek().text;
            m->line = line;
            m->a = std::move(e);
            ++m_i;
            e = std::move(m);
        }
        return e;
    }

    ExprPtr parsePrimary(ScriptError& err) {
        const Token& t = peek();

        if (t.kind == T::Number) {
            auto e = std::make_unique<Expr>();
            e->kind = Expr::Kind::Number;
            e->num = t.num;
            e->line = t.line;
            ++m_i;
            return e;
        }
        if (t.kind == T::Str) {
            auto e = std::make_unique<Expr>();
            e->kind = Expr::Kind::Str;
            e->text = t.text;
            e->line = t.line;
            ++m_i;
            return e;
        }
        // `true` / `false` 是关键字，不是变量名。缺了这两个字面量，
        // `let flag = false` 会被当成"未知变量 false"报错 —— 而 Bool 值
        // 本来就存在（比较运算、exists() 都产出它），没有字面量就写不出
        // "初始为假"这种最普通的声明。
        if (t.kind == T::Ident && (t.text == "true" || t.text == "false")) {
            auto e = std::make_unique<Expr>();
            e->kind = Expr::Kind::Bool;
            e->flag = (t.text == "true");
            e->line = t.line;
            ++m_i;
            return e;
        }
        if (t.kind == T::Ident) {
            auto e = std::make_unique<Expr>();
            e->text = t.text;
            e->line = t.line;
            ++m_i;
            if (check(T::LParen)) {  // 函数调用
                ++m_i;
                e->kind = Expr::Kind::Call;
                if (!check(T::RParen)) {
                    for (;;) {
                        ExprPtr arg = parseExpr(err);
                        if (!arg) return nullptr;
                        e->args.push_back(std::move(arg));
                        if (match(T::Comma)) continue;
                        break;
                    }
                }
                if (!expect(T::RParen, "')' to close call", err)) return nullptr;

                // ★ S1（修 P5）：函数名**在这里**就解析成编号。
                //   拼错 = 编译失败 = 面板红字 + 旧版本继续跑，
                //   而不是每帧一条 "unknown function 'moveby'"。
                e->builtin = findBuiltin(e->text);
                if (e->builtin == Builtin::None) {
                    err.line = t.line;
                    err.message = "unknown function '" + e->text + "'";
                    return nullptr;
                }
                const BuiltinInfo& bi = builtinInfo(e->builtin);
                const size_t n = e->args.size();
                if (n < bi.minArgs || n > bi.maxArgs) {
                    char buf[192];
                    if (bi.minArgs == bi.maxArgs) {
                        std::snprintf(buf, sizeof(buf),
                                      "'%s' expects %u argument(s), got %zu",
                                      bi.name, static_cast<unsigned>(bi.minArgs),
                                      n);
                    } else {
                        std::snprintf(buf, sizeof(buf),
                                      "'%s' expects %u..%u arguments, got %zu",
                                      bi.name, static_cast<unsigned>(bi.minArgs),
                                      static_cast<unsigned>(bi.maxArgs), n);
                    }
                    err.line = t.line;
                    err.message = buf;
                    return nullptr;
                }
                // `wait` 家族会**让执行流挂起** —— 它没法在表达式里"返回一个
                // 值"，写了也只能拿到 0 然后整句重跑。编译期挡掉。
                if (isWaitBuiltin(e->text) && !m_waitStmt) {
                    err.line = t.line;
                    err.message = "'" + e->text +
                                  "' must be a statement on its own line "
                                  "(it suspends the script, so it cannot be "
                                  "used inside an expression)";
                    return nullptr;
                }
            } else {
                e->kind = Expr::Kind::Var;
            }
            return e;
        }
        if (t.kind == T::LParen) {
            ++m_i;
            ExprPtr inner = parseExpr(err);
            if (!inner) return nullptr;
            if (!expect(T::RParen, "')'", err)) return nullptr;
            return inner;
        }

        err.line = t.line;
        err.message = "unexpected token in expression";
        return nullptr;
    }

    static ExprPtr makeBinary(std::string op, ExprPtr a, ExprPtr b, int line) {
        auto e = std::make_unique<Expr>();
        e->kind = Expr::Kind::Binary;
        e->op = std::move(op);
        e->a = std::move(a);
        e->b = std::move(b);
        e->line = line;
        return e;
    }

    const std::vector<Token>& m_t;
    size_t m_i = 0;
    // 语句嵌套深度：0 = 顶层。`let` 只允许在 0（S-D2）。
    int m_depth = 0;
    uint32_t m_nextId = 1;   // 稳定语句序号（S4）
    bool m_waitStmt = false; // 正在解析 wait 语句（允许 wait 家族出现）
};

// ---------------------------------------------------------------- 求值器

class Evaluator {
public:
    // 一次执行推进的结果（S4）
    enum class Step : uint8_t {
        Continue,  // 这一条跑完了，继续
        Suspend,   // 挂起了（wait），栈留着
        Done,      // 栈空了（这段跑完）
        Error      // 出错
    };
    // ⚠ `vars` / `prints` 由调用方持有（S0，修 P1）。
    //   构造时只覆盖写"每步都要重算"的那几个内置变量，**不清表** ——
    //   清了就等于回到"脚本失忆"。
    Evaluator(const ScriptHost& host, ScriptProgram& prog, ScriptError& err,
              std::unordered_map<std::string, Value>& vars,
              std::vector<std::string>* prints, ScriptCoroutine& coro)
        : m_host(host), m_prog(prog), m_err(err), m_vars(vars),
          m_prints(prints), m_coro(coro) {
        m_vars["index"] = Value::number(
            host.entity.valid() ? static_cast<double>(host.entity.id) : -1.0);
    }

    // S3：把"设内置变量"与"跑一段语句"拆开 —— 一帧里要跑好几段
    // （start / contacts / 顶层 / update），但**只能有一个 Evaluator**：
    // 配额 `m_calls` 是按"每帧每实例"算的，一段一个 Evaluator 会把它变成
    // "每段一份配额"，等于没有上限。
    void beginFrame(double time, double dt, uint64_t frame, uint64_t simFrame) {
        m_vars["time"] = Value::number(time);
        m_vars["dt"] = Value::number(dt);
        // `frame` 保持"渲染帧号"的**老语义**（hud_damage.vks / audio_play.vks
        // 都靠它），`simFrame` 才是固定步号（修 P7）。
        m_vars["frame"] = Value::number(static_cast<double>(frame));
        m_vars["simFrame"] = Value::number(static_cast<double>(simFrame));
        // 协程要用（`wait` 算唤醒时刻、`waitFrames` 算目标帧）
        m_time = time;
        m_simFrame = simFrame;
    }

    // 绑定一个变量（钩子参数用：`on collide(other)` 的 other）
    void bind(const std::string& name, Value v) {
        m_vars[name] = std::move(v);
    }

    // ★ S4：一段语句的执行入口。**栈里已经有东西时不再压新帧** ——
    //   那说明上一帧挂在段中间，这里要做的是"继续"。
    Step runSegment(const std::vector<StmtPtr>& list, std::size_t from,
                    std::size_t to, uint32_t ownerId, uint8_t branch = 0) {
        if (m_coro.frames.empty()) {
            ScriptCoroutine::Frame f;
            f.kind = ScriptCoroutine::Frame::K::Seq;
            f.list = &list;
            f.ownerId = ownerId;
            f.branch = branch;
            f.ip = static_cast<uint32_t>(from);
            f.end = static_cast<uint32_t>(to < list.size() ? to : list.size());
            m_coro.frames.push_back(std::move(f));
        }
        return drive();
    }

    // 老入口（预览/动画用）：不挂起、不留栈。`wait` 在这条路上等于
    // "本帧到此为止"—— 下一帧从头顶重跑（这是 S0 之前的老语义，动画脚本
    // 不该用 wait，注释见 ScriptEngine.h）。
    bool run(const std::vector<StmtPtr>& ast, double time, double dt,
             uint64_t frame, uint64_t simFrame) {
        beginFrame(time, dt, frame, simFrame);
        m_coro.clear();
        const Step st = runSegment(ast, 0, ast.size(), 0);
        m_coro.clear();
        return st != Step::Error;
    }

    // ★ 唤醒检查（S4）。返回 false = 还没到点，本帧什么都不做。
    bool tryWake() {
        if (m_coro.state != ScriptCoroutine::State::Waiting) return true;
        switch (m_coro.wake) {
            case ScriptCoroutine::Wake::None:
                break;
            case ScriptCoroutine::Wake::Time:
                if (m_time < m_coro.wakeAt) return false;
                break;
            case ScriptCoroutine::Wake::Frames:
                // 固定步号比较：与 dt 无关，掉帧也不会"多等"
                if (m_simFrame < m_coro.wakeFrame) return false;
                break;
            case ScriptCoroutine::Wake::Until: {
                if (!m_coro.cond) break;
                // ⚠ 条件求值也吃配额（m_calls），否则 `waitUntil(pos(e).x>0)`
                //   在 200 个实例下就是每帧 200 次未被计量的世界访问。
                const Value v = eval(*m_coro.cond);
                if (!m_err.ok()) return true;   // 交给上层报错
                if (!v.truthy()) return false;
                break;
            }
        }
        m_coro.state = ScriptCoroutine::State::Idle;
        m_coro.wake = ScriptCoroutine::Wake::None;
        m_coro.cond = nullptr;
        m_coro.condOwnerId = 0;
        return true;
    }

    // 本次消耗的步数（预算 / 基准 / 熔断诊断用）
    uint64_t steps() const { return static_cast<uint64_t>(m_steps); }

private:
    // ---- 宿主组件（脚本能碰到的东西全在这里，别处没有后门）----
    ecs::TransformComponent* xform() {
        return m_host.scene ? m_host.scene->world().get<ecs::TransformComponent>(
                                  m_host.entity)
                            : nullptr;
    }
    ecs::PointLightComponent* pointLight() {
        return m_host.scene
                   ? m_host.scene->world().get<ecs::PointLightComponent>(
                         m_host.entity)
                   : nullptr;
    }
    ecs::SpotLightComponent* spotLight() {
        return m_host.scene
                   ? m_host.scene->world().get<ecs::SpotLightComponent>(
                         m_host.entity)
                   : nullptr;
    }
    ecs::VisibilityComponent* visibility() {
        return m_host.scene
                   ? m_host.scene->world().get<ecs::VisibilityComponent>(
                         m_host.entity)
                   : nullptr;
    }
    assets::Material* material() {
        if (!m_host.scene) return nullptr;
        auto* mc =
            m_host.scene->world().get<ecs::MaterialComponent>(m_host.entity);
        return mc ? mc->material : nullptr;
    }

    bool step(int line) {
        if (++m_steps > kMaxSteps) {
            m_err.line = line;
            m_err.message =
                "script exceeded step budget (possible infinite loop)";
            return false;
        }
        return true;
    }

    // ---- 类型强制（S1）----
    //
    // ⚠ 所有"要一个数字 / 一个 vec3 / 一个实体"的地方都必须走这三个。
    //   直接调 `asNum()` 会让 vec3 **静默变成 0** —— 而"位置怎么变成 0 了"
    //   是这类 bug 里最难查的一种。这里带行号、带类型名报错。
    double numOf(const Value& v, int line, const char* what) {
        if (v.k == Value::K::Num) return v.num;
        if (v.k == Value::K::Bool) return v.b ? 1.0 : 0.0;
        if (v.k == Value::K::Str) return std::strtod(v.s.c_str(), nullptr);
        m_err.line = line;
        m_err.message = std::string("expected a number for ") + what + ", got " +
                        v.typeName();
        return 0.0;
    }

    glm::vec3 vecOf(const Value& v, int line, const char* what) {
        if (v.k == Value::K::Vec3) return v.v;
        m_err.line = line;
        m_err.message = std::string("expected a vec3 for ") + what + ", got " +
                        v.typeName();
        return glm::vec3(0.0f);
    }

    ecs::Entity entityOf(const Value& v, int line, const char* what) {
        if (v.k == Value::K::Entity) return v.e;
        m_err.line = line;
        m_err.message = std::string("expected an entity for ") + what +
                        ", got " + v.typeName();
        return ecs::Entity{};
    }

    // 路径 / 名字参数（C6 起的"演出三件"）。
    //
    // ⚠ 为什么不像 `playSound` 那样直接 `toStr()`：`toStr()` 会把数字
    //   转成字符串，于是 `playSequence(3)` 变成去读一个叫 "3" 的资产，
    //   报出来的错是"资产不存在"—— 排查方向从一开始就错了。
    //   `playSound` 保持宽松是因为它比这条纪律早了两天，不改它。
    std::string strOf(const Value& v, int line, const char* what) {
        if (v.k == Value::K::Str) return v.s;
        m_err.line = line;
        m_err.message = std::string("expected a string for ") + what + ", got " +
                        v.typeName();
        return {};
    }

    // ---- 世界查询（S1：朴素版；S2 换成 BVH 快查 + 补扫）----
    bool alive(ecs::Entity e) const {
        if (!m_host.scene || !e.valid()) return false;
        // `World::alive` 会校验 generation —— 这就是"删掉之后旧句柄失效"。
        return m_host.scene->world().alive(e);
    }

    std::string nameOf(ecs::Entity e) const {
        if (!m_host.scene || !e.valid()) return {};
        const std::string* n = m_host.scene->world().name(e);
        return n ? *n : std::string();
    }

    // 无效/已删实体 —— S2 新增的"带实体参数"的函数全都要过这一关。
    Value badEntity(int line, const char* what, ecs::Entity e) {
        m_err.line = line;
        m_err.message = std::string(what) +
                        " got an invalid or deleted entity (name='" +
                        nameOf(e) + "')";
        return Value::number(0.0);
    }
    Value noBody(int line, const char* what, ecs::Entity e) {
        m_err.line = line;
        m_err.message = std::string(what) + " needs a simulated rigid body on '" +
                        nameOf(e) +
                        "' (no physics world here, or the entity has no "
                        "RigidBody component)";
        return Value::number(0.0);
    }

    // `setPos` 写的是**世界**位置，而 Transform 存的是**本地**位置。
    // 无父节点时两者相同（99% 的实体）；有父节点时必须换算，否则"脚本把
    // 子物体搬到 (3,0,0)"会变成"相对父节点偏 3 米"。
    //
    // 怎么反推父节点世界矩阵：`world = M_parent * L`，所以
    // `M_parent = world * inverse(L)`，再 `local = inverse(M_parent) * p`。
    // ⚠ 缩放为 0 时 inverse 会出 NaN —— 显式挡掉，退回直接写。
    void setWorldPos(ecs::Entity e, const glm::vec3& p) {
        ecs::World& w = m_host.scene->world();
        ecs::TransformComponent* t = w.get<ecs::TransformComponent>(e);
        if (!t) return;
        const auto* h = w.get<ecs::HierarchyComponent>(e);
        if (!h || h->parent == ecs::kInvalidEntity) {
            t->position = p;
            return;
        }
        const glm::mat4 L = t->localMatrix();
        const glm::mat4 W = m_host.scene->worldMatrix(e);
        const glm::mat4 Mp = W * glm::inverse(L);
        const glm::mat4 invMp = glm::inverse(Mp);
        const glm::vec4 lp = invMp * glm::vec4(p, 1.0f);
        if (std::isfinite(lp.x) && std::isfinite(lp.y) && std::isfinite(lp.z))
            t->position = glm::vec3(lp);
        else
            t->position = p;
    }

    std::vector<Value> collectNear(glm::vec3 center, float radius, int limit);

    // ---------------------------------------------------------- 显式栈执行
    //
    // 老版本是 `bool exec(const Stmt&)` 递归：If 里 for 一遍 body、While 里
    // 无限 for。递归把"执行到哪了"放在 CPU 栈上，`wait` 无处可停。现在拆成
    // 两个函数：
    //   · execLeaf —— 真正的叶子语句（赋值 / 表达式语句 / let）
    //   · drive    —— 主循环，靠 m_coro.frames 记账
    //
    // ⚠ **控制流语句先 `++ip` 再压子帧**。顺序反了的话，挂起时父帧的 ip
    //   还指着那条 if/while，恢复后会把整条控制流再执行一遍（无限循环）。

    void pushSeq(const std::vector<StmtPtr>& list, uint32_t ownerId,
                 uint8_t branch) {
        ScriptCoroutine::Frame f;
        f.kind = ScriptCoroutine::Frame::K::Seq;
        f.list = &list;
        f.ownerId = ownerId;
        f.branch = branch;
        f.ip = 0;
        f.end = static_cast<uint32_t>(list.size());
        m_coro.frames.push_back(std::move(f));
    }

    Step execLeaf(const Stmt& s) {
        switch (s.kind) {
            case Stmt::Kind::Assign: {
                const Value v = eval(*s.expr);
                if (!m_err.ok()) return Step::Error;
                m_vars[s.name] = v;
                return Step::Continue;
            }
            case Stmt::Kind::ExprStmt: {
                eval(*s.expr);
                return m_err.ok() ? Step::Continue : Step::Error;
            }
            case Stmt::Kind::Let: {
                // ★ 幂等声明（S-D2）：已经有值就**连右侧都不求值**。
                if (m_vars.find(s.name) != m_vars.end()) return Step::Continue;
                const Value v = eval(*s.expr);
                if (!m_err.ok()) return Step::Error;
                m_vars[s.name] = v;
                return Step::Continue;
            }
            default:
                m_err.line = s.line;
                m_err.message = "internal: unexpected leaf statement";
                return Step::Error;
        }
    }

    // `wait(sec)` / `waitFrames(n)` / `waitUntil(cond)`。返回 true = 需要挂起。
    bool doWait(const Stmt& s) {
        const Expr& call = *s.expr;
        switch (call.builtin) {
            case Builtin::Wait: {
                const double sec = numOf(eval(*call.args[0]), s.line, "wait");
                if (!m_err.ok()) return false;
                m_coro.state = ScriptCoroutine::State::Waiting;
                m_coro.wake = ScriptCoroutine::Wake::Time;
                // 负数当 0：`wait(-1)` 不该挂起到"永远"
                m_coro.wakeAt = m_time + (sec > 0.0 ? sec : 0.0);
                return true;
            }
            case Builtin::WaitFrames: {
                const double nf =
                    numOf(eval(*call.args[0]), s.line, "waitFrames");
                if (!m_err.ok()) return false;
                // 至少 1 帧：`waitFrames(0)` 若判成"立刻继续"就等于没写，
                // 而用户写它的意图一定是"让出一帧"。
                const uint64_t n = nf > 1.0 ? static_cast<uint64_t>(nf) : 1;
                m_coro.state = ScriptCoroutine::State::Waiting;
                m_coro.wake = ScriptCoroutine::Wake::Frames;
                m_coro.wakeFrame = m_simFrame + n;
                return true;
            }
            case Builtin::WaitUntil: {
                m_coro.cond = call.args[0].get();
                m_coro.condOwnerId = s.id;
                m_coro.condLine = s.line;
                // 已经满足就**不等**（`waitUntil(x > 0)` 在 x 已为正时立刻走）
                const Value v = eval(*m_coro.cond);
                if (!m_err.ok()) return false;
                if (v.truthy()) {
                    m_coro.cond = nullptr;
                    m_coro.condOwnerId = 0;
                    return false;
                }
                m_coro.state = ScriptCoroutine::State::Waiting;
                m_coro.wake = ScriptCoroutine::Wake::Until;
                return true;
            }
            default:
                break;
        }
        m_err.line = s.line;
        m_err.message = "internal: Wait statement without a wait builtin";
        return false;
    }

    Step stepWhile() {
        const size_t fi = m_coro.frames.size() - 1;
        ScriptCoroutine::Frame& f = m_coro.frames[fi];
        // ⚠ 先拷走要用的东西：下面 push 会让 f 悬垂
        const Expr* cond = f.cond;
        const int line = f.line;
        const std::vector<StmtPtr>* body = f.list;
        const uint32_t owner = f.ownerId;

        if (!step(line)) return Step::Error;
        const Value c = eval(*cond);
        if (!m_err.ok()) return Step::Error;
        if (!c.truthy()) {
            m_coro.frames.pop_back();
            return Step::Continue;
        }
        pushSeq(*body, owner, 0);
        return Step::Continue;
    }

    Step stepForeach() {
        const size_t fi = m_coro.frames.size() - 1;
        ScriptCoroutine::Frame& f = m_coro.frames[fi];
        if (f.itemIdx >= f.items.size()) {
            // 循环结束：恢复被遮蔽的外层同名变量
            const std::string var = f.var;
            const bool had = f.varHad;
            const Value saved = f.varSaved;
            m_coro.frames.pop_back();   // f 从这里起悬垂
            if (had) m_vars[var] = saved;
            else m_vars.erase(var);
            return Step::Continue;
        }
        if (!step(f.line)) return Step::Error;
        const Value item = f.items[f.itemIdx];
        ++f.itemIdx;
        const std::string var = f.var;
        const std::vector<StmtPtr>* body = f.list;
        const uint32_t owner = f.ownerId;
        m_vars[var] = item;
        pushSeq(*body, owner, 0);
        return Step::Continue;
    }

    Step drive() {
        while (!m_coro.frames.empty()) {
            const size_t fi = m_coro.frames.size() - 1;
            ScriptCoroutine::Frame& f = m_coro.frames[fi];

            if (f.kind == ScriptCoroutine::Frame::K::While) {
                const Step st = stepWhile();
                if (st != Step::Continue) return st;
                continue;
            }
            if (f.kind == ScriptCoroutine::Frame::K::Foreach) {
                const Step st = stepForeach();
                if (st != Step::Continue) return st;
                continue;
            }

            if (f.ip >= f.end) {
                m_coro.frames.pop_back();
                continue;
            }
            const Stmt& st0 = *(*f.list)[f.ip];

            switch (st0.kind) {
                case Stmt::Kind::Block: {
                    ++f.ip;   // ⚠ 先越过自己
                    pushSeq(st0.body, st0.id, 0);
                    continue;
                }
                case Stmt::Kind::If: {
                    if (!step(st0.line)) return Step::Error;
                    const Value c = eval(*st0.expr);
                    if (!m_err.ok()) return Step::Error;
                    ++f.ip;
                    const bool thenBranch = c.truthy();
                    pushSeq(thenBranch ? st0.body : st0.elseBody, st0.id,
                            thenBranch ? 0 : 1);
                    continue;
                }
                case Stmt::Kind::While: {
                    ++f.ip;
                    ScriptCoroutine::Frame w;
                    w.kind = ScriptCoroutine::Frame::K::While;
                    w.list = &st0.body;
                    w.ownerId = st0.id;
                    w.cond = st0.expr.get();
                    w.line = st0.line;
                    m_coro.frames.push_back(std::move(w));
                    continue;
                }
                case Stmt::Kind::Foreach: {
                    if (!step(st0.line)) return Step::Error;
                    const Value it = eval(*st0.expr);
                    if (!m_err.ok()) return Step::Error;
                    if (it.k != Value::K::List) {
                        m_err.line = st0.line;
                        m_err.message =
                            std::string("foreach expects a list, got ") +
                            it.typeName();
                        return Step::Error;
                    }
                    ++f.ip;
                    ScriptCoroutine::Frame fe;
                    fe.kind = ScriptCoroutine::Frame::K::Foreach;
                    fe.list = &st0.body;
                    fe.ownerId = st0.id;
                    fe.line = st0.line;
                    fe.items = it.list;   // ★ 快照：挂起期间世界会变
                    fe.itemIdx = 0;
                    fe.var = st0.name;
                    auto prev = m_vars.find(st0.name);
                    fe.varHad = prev != m_vars.end();
                    fe.varSaved = fe.varHad ? prev->second : Value{};
                    m_coro.frames.push_back(std::move(fe));
                    continue;
                }
                case Stmt::Kind::Wait: {
                    ++f.ip;
                    if (!step(st0.line)) return Step::Error;
                    if (!doWait(st0)) {
                        if (!m_err.ok()) return Step::Error;
                        continue;   // waitUntil 已满足，不等
                    }
                    return Step::Suspend;
                }
                default: {
                    if (!step(st0.line)) return Step::Error;
                    const Step st = execLeaf(st0);
                    if (st != Step::Continue) return st;
                    ++f.ip;
                    continue;
                }
            }
        }
        return Step::Done;
    }

    static bool valuesEqual(const Value& a, const Value& b) {
        if (a.k != b.k) {
            // 老行为：str 与数字之间按字符串比。保留它（现有脚本依赖
            // `name(e) == "Player"` 之外的那些字符串比较）。
            if (a.k == Value::K::Str || b.k == Value::K::Str)
                return a.toStr() == b.toStr();
        }
        switch (a.k) {
            case Value::K::Num: return a.num == b.asNum();
            case Value::K::Bool: return a.b == b.truthy();
            case Value::K::Str: return a.s == b.toStr();
            case Value::K::Vec3:
                if (b.k != Value::K::Vec3) return false;
                return a.v.x == b.v.x && a.v.y == b.v.y && a.v.z == b.v.z;
            case Value::K::Entity:
                if (b.k != Value::K::Entity) return false;
                // id + generation 一起比 —— 这就是 B4"删掉之后旧句柄不等"
                return a.e.id == b.e.id && a.e.generation == b.e.generation;
            case Value::K::List:
                if (b.k != Value::K::List) return false;
                if (a.list.size() != b.list.size()) return false;
                for (std::size_t i = 0; i < a.list.size(); ++i) {
                    if (!valuesEqual(a.list[i], b.list[i])) return false;
                }
                return true;
        }
        return false;
    }

    Value binaryOp(const Value& l, const Value& r, const std::string& op,
                   int line) {
        const bool lv = l.k == Value::K::Vec3;
        const bool rv = r.k == Value::K::Vec3;

        // ---- vec3（S1）----
        // 只开 `+ - *`（逐分量 / 标量缩放）。**混合 vec3 与数字的加减
        // 一律报错**（B2）：`vec3(1,0,0) + 1` 看起来"合理"，但它是
        // "给三个分量各加 1"还是"沿 x 加 1"？含糊的语义比报错更贵。
        if (lv || rv) {
            if (op == "==") return Value::boolean(valuesEqual(l, r));
            if (op == "!=") return Value::boolean(!valuesEqual(l, r));
            if (op == "+" || op == "-") {
                if (!lv || !rv) {
                    m_err.line = line;
                    m_err.message = "cannot use '" + op + "' between vec3 and " +
                                    (lv ? r : l).typeName() +
                                    " (use vec3(...) or a component like .x)";
                    return Value::number(0.0);
                }
                return Value::makeVec3(op == "+" ? l.v + r.v : l.v - r.v);
            }
            if (op == "*") {
                if (lv && rv) return Value::makeVec3(l.v * r.v);
                if (lv) {
                    const double k = numOf(r, line, "vec3 * number");
                    return m_err.ok()
                               ? Value::makeVec3(l.v * static_cast<float>(k))
                               : Value::number(0.0);
                }
                const double k = numOf(l, line, "number * vec3");
                return m_err.ok()
                           ? Value::makeVec3(static_cast<float>(k) * r.v)
                           : Value::number(0.0);
            }
            m_err.line = line;
            m_err.message = "operator '" + op + "' is not defined for vec3";
            return Value::number(0.0);
        }

        // ---- 标量 ----
        const double ln = l.asNum();
        const double rn = r.asNum();
        if (op == "+") {
            if (l.k == Value::K::Str || r.k == Value::K::Str)
                return Value::str(l.toStr() + r.toStr());
            return Value::number(ln + rn);
        }
        if (op == "-") return Value::number(ln - rn);
        if (op == "*") return Value::number(ln * rn);
        if (op == "/") {
            if (rn == 0.0) {
                m_err.line = line;
                m_err.message = "division by zero";
                return Value::number(0.0);
            }
            return Value::number(ln / rn);
        }
        if (op == "%") {
            if (rn == 0.0) {
                m_err.line = line;
                m_err.message = "modulo by zero";
                return Value::number(0.0);
            }
            return Value::number(std::fmod(ln, rn));
        }
        if (op == "==") return Value::boolean(valuesEqual(l, r));
        if (op == "!=") return Value::boolean(!valuesEqual(l, r));
        if (op == "<") return Value::boolean(ln < rn);
        if (op == "<=") return Value::boolean(ln <= rn);
        if (op == ">") return Value::boolean(ln > rn);
        if (op == ">=") return Value::boolean(ln >= rn);

        m_err.line = line;
        m_err.message = "unknown operator '" + op + "'";
        return Value::number(0.0);
    }

    Value eval(const Expr& e) {
        if (!step(e.line)) return Value::number(0.0);

        switch (e.kind) {
            case Expr::Kind::Number: return Value::number(e.num);
            case Expr::Kind::Bool:   return Value::boolean(e.flag);
            case Expr::Kind::Str: return Value::str(e.text);

            case Expr::Kind::Var: {
                auto it = m_vars.find(e.text);
                if (it == m_vars.end()) {
                    m_err.line = e.line;
                    m_err.message = "undefined variable '" + e.text + "'";
                    return Value::number(0.0);
                }
                return it->second;
            }

            case Expr::Kind::Member: {
                const Value o = eval(*e.a);
                if (!m_err.ok()) return Value::number(0.0);
                if (o.k != Value::K::Vec3) {
                    m_err.line = e.line;
                    m_err.message = std::string("expected vec3 for '.") +
                                    e.text + "', got " + o.typeName();
                    return Value::number(0.0);
                }
                if (e.text == "x") return Value::number(o.v.x);
                if (e.text == "y") return Value::number(o.v.y);
                if (e.text == "z") return Value::number(o.v.z);
                m_err.line = e.line;
                m_err.message = "unknown member '." + e.text +
                                "' (only .x / .y / .z)";
                return Value::number(0.0);
            }

            case Expr::Kind::Unary: {
                const Value v = eval(*e.a);
                if (!m_err.ok()) return Value::number(0.0);
                if (e.op == "-") {
                    if (v.k == Value::K::Vec3) return Value::makeVec3(-v.v);
                    return Value::number(-numOf(v, e.line, "unary '-'"));
                }
                return Value::boolean(!v.truthy());
            }

            case Expr::Kind::Binary: {
                // 短路：&& / || 只按需对右侧求值
                if (e.op == "&&" || e.op == "||") {
                    const Value l = eval(*e.a);
                    if (!m_err.ok()) return Value::number(0.0);
                    const bool shortCircuit =
                        (e.op == "&&") ? !l.truthy() : l.truthy();
                    if (shortCircuit) return Value::boolean(e.op == "||");
                    const Value r = eval(*e.b);
                    if (!m_err.ok()) return Value::number(0.0);
                    return Value::boolean(r.truthy());
                }

                const Value l = eval(*e.a);
                if (!m_err.ok()) return Value::number(0.0);
                const Value r = eval(*e.b);
                if (!m_err.ok()) return Value::number(0.0);
                return binaryOp(l, r, e.op, e.line);
            }

            case Expr::Kind::Call: {
                std::vector<Value> args;
                args.reserve(e.args.size());
                for (const auto& a : e.args) {
                    args.push_back(eval(*a));
                    if (!m_err.ok()) return Value::number(0.0);
                }
                return call(e.builtin, args, e.line);
            }
        }
        return Value::number(0.0);
    }

    Value call(Builtin b, const std::vector<Value>& a, int line);

    const ScriptHost& m_host;
    ScriptProgram& m_prog;
    ScriptError& m_err;
    std::unordered_map<std::string, Value>& m_vars;
    std::vector<std::string>* m_prints = nullptr;
    int m_steps = 0;
    // S4
    ScriptCoroutine& m_coro;
    double m_time = 0.0;
    uint64_t m_simFrame = 0;
    // S2：本帧已用的内建调用数（配额用）
    int m_calls = 0;
    // S2：空间索引在这个 run 里刷过没有。一次 run 里最多 refit 一次 ——
    //      否则"循环里查 near"会变成"循环里 refit 全场景"。
    bool m_indexFresh = false;
    scene::Bvh* m_bvh = nullptr;
};

// ---------------------------------------------------------------- near（S2）
//
// 两级：**BVH 快查 + 补扫 Transform 池**。
//
// 为什么必须有补扫：`Scene::spatialIndex()` 只索引"可见 + 有网格"的实体
// （见 Scene::forEachRenderable）—— 隐藏的敌人、纯逻辑的空物体、被关掉的
// 陷阱，在 BVH 里**根本不存在**。少了补扫，"看不见的东西就摸不到"，
// 而这个 bug 只在"玩家把某物设成隐藏"时才出现。
//
// 排序规则 (距离, id) 与 S1 朴素版**完全一致**，所以脚本侧语义没变。
std::vector<Value> Evaluator::collectNear(glm::vec3 center, float radius,
                                          int limit) {
    std::vector<Value> out;
    if (!m_host.scene) return out;
    scene::Scene& sc = *m_host.scene;

    // ① 空间索引：一次 run 里最多刷一次。`spatialIndex()` 每次调用都要把
    //    全场景的世界矩阵 + 8 个角点算一遍，放在循环里就是灾难。
    if (!m_indexFresh) {
        m_bvh = &sc.spatialIndex();
        m_indexFresh = true;
    }
    if (!m_bvh) return out;  // 理论上到不了这里，守一手

    // 球 vs AABB（保守：必须能命中"盒角在球外、但盒面离球心最近点在内"
    // 的那些 —— 用最近点距离，不放大盒）。
    struct Sphere {
        glm::vec3 c;
        float r;
        bool intersects(const glm::vec3& lo, const glm::vec3& hi) const {
            const glm::vec3 q = glm::clamp(c, lo, hi);
            const glm::vec3 d = q - c;
            return glm::dot(d, d) <= r * r;
        }
    };
    std::vector<ecs::Entity> cand;
    m_bvh->query(Sphere{center, radius}, cand);

    // ② 补扫：BVH 查不到的实体里，凡是带 Transform 的都要再试一次。
    std::unordered_set<ecs::EntityId> seen;
    seen.reserve(cand.size() * 2 + 8);
    for (ecs::Entity e : cand) seen.insert(e.id);
    std::vector<ecs::Entity> extra;
    sc.world().each<ecs::TransformComponent>(
        [&](ecs::Entity e, ecs::TransformComponent&) {
            if (seen.count(e.id) == 0) extra.push_back(e);
        });

    struct Hit {
        float d;
        ecs::Entity e;
    };
    std::vector<Hit> hits;
    hits.reserve(cand.size() + extra.size());

    auto consider = [&](ecs::Entity e) {
        // 不含自己：`near` 的语义是"我周围有什么"。
        if (e.id == m_host.entity.id) return;
        const glm::vec3 p(sc.worldMatrix(e)[3]);
        const float d = glm::length(p - center);
        if (d <= radius) hits.push_back(Hit{d, e});
    };
    for (ecs::Entity e : cand) {
        if (sc.world().get<ecs::TransformComponent>(e)) consider(e);
    }
    for (ecs::Entity e : extra) consider(e);

    // ③ 确定性（S-D11）：按 (dist, id) 排序 —— 与 S1 逐位相同。
    std::sort(hits.begin(), hits.end(), [](const Hit& x, const Hit& y) {
        if (x.d != y.d) return x.d < y.d;
        return x.e.id < y.e.id;
    });
    if (limit > 0 && hits.size() > static_cast<std::size_t>(limit)) {
        hits.resize(static_cast<std::size_t>(limit));
    }

    out.reserve(hits.size());
    for (const Hit& h : hits) {
        out.push_back(Value::makeEntity(h.e, nameOf(h.e)));
    }
    return out;
}

// 内建函数派发（S1）。**编译期**已经把名字解析成 `Builtin`，这里是纯 switch。
Value Evaluator::call(Builtin b, const std::vector<Value>& a, int line) {
    auto n = [&](std::size_t i, const char* what) {
        return numOf(a[i], line, what);
    };
    auto f = [&](std::size_t i, const char* what) {
        return static_cast<float>(n(i, what));
    };
    auto v3 = [&](std::size_t i, const char* what) {
        return vecOf(a[i], line, what);
    };

    // ★ S2 配额。位置很关键：**在求值任何内建之前**，否则 30000 次
    //   `pos()` 里那句世界矩阵累乘已经先跑完了。
    if (++m_calls > builtinQuota()) {
        m_err.line = line;
        m_err.message = "script exceeded builtin call quota (" +
                        std::to_string(builtinQuota()) +
                        " calls per frame, MYVK_VKS_QUOTA to raise)";
        return Value::number(0.0);
    }

    switch (b) {
        // ---- 数学 ----
        case Builtin::Sin: return Value::number(std::sin(n(0, "sin")));
        case Builtin::Cos: return Value::number(std::cos(n(0, "cos")));
        case Builtin::Tan: return Value::number(std::tan(n(0, "tan")));
        case Builtin::Asin: return Value::number(std::asin(n(0, "asin")));
        case Builtin::Acos: return Value::number(std::acos(n(0, "acos")));
        case Builtin::Atan: return Value::number(std::atan(n(0, "atan")));
        case Builtin::Abs: return Value::number(std::fabs(n(0, "abs")));
        case Builtin::Floor: return Value::number(std::floor(n(0, "floor")));
        case Builtin::Ceil: return Value::number(std::ceil(n(0, "ceil")));
        case Builtin::Round: return Value::number(std::round(n(0, "round")));
        case Builtin::Sqrt: return Value::number(std::sqrt(n(0, "sqrt")));
        case Builtin::Atan2:
            return Value::number(std::atan2(n(0, "atan2"), n(1, "atan2")));
        case Builtin::Pow:
            return Value::number(std::pow(n(0, "pow"), n(1, "pow")));
        case Builtin::Min:
            return Value::number(std::fmin(n(0, "min"), n(1, "min")));
        case Builtin::Max:
            return Value::number(std::fmax(n(0, "max"), n(1, "max")));
        case Builtin::Clamp:
            return Value::number(std::fmin(std::fmax(n(0, "clamp"), n(1, "clamp")),
                                           n(2, "clamp")));
        case Builtin::Lerp: {
            const double t = n(2, "lerp");
            return Value::number(n(0, "lerp") * (1.0 - t) + n(1, "lerp") * t);
        }

        // ---- 输出 ----
        case Builtin::Print: {
            std::string msg;
            for (std::size_t i = 0; i < a.size(); ++i) {
                if (i) msg += ' ';
                msg += a[i].toStr();
            }
            // ⚠ 写到**调用方**的 vector（S0）。以前写到 ScriptProgram 上，
            //   而 Program 按路径共享 —— 两个实体跑同一脚本会互相清输出。
            if (m_prints) m_prints->push_back(std::move(msg));
            return Value::number(0.0);
        }

        // ---- 值构造（S1）----
        case Builtin::Vec3:
            return Value::makeVec3(glm::vec3(f(0, "vec3"), f(1, "vec3"),
                                             f(2, "vec3")));
        case Builtin::Count: {
            if (a[0].k == Value::K::List)
                return Value::number(static_cast<double>(a[0].list.size()));
            if (a[0].k == Value::K::Str)
                return Value::number(static_cast<double>(a[0].s.size()));
            m_err.line = line;
            m_err.message = std::string("count() expects a list, got ") +
                            a[0].typeName();
            return Value::number(0.0);
        }
        case Builtin::At: {
            if (a[0].k != Value::K::List) {
                m_err.line = line;
                m_err.message = std::string("at() expects a list, got ") +
                                a[0].typeName();
                return Value::number(0.0);
            }
            const double raw = n(1, "at(index)");
            const auto idx = static_cast<long long>(raw);
            if (idx < 0 || static_cast<std::size_t>(idx) >= a[0].list.size()) {
                // ⚠ 越界**报错**而不是返回 0：`at(l, 5)` 在只有 3 个元素时
                //   返回 0（即"无效实体"）会让脚本静默做错事，而错的地方
                //   离这里很远。
                m_err.line = line;
                m_err.message = "at() index " + std::to_string(idx) +
                                " out of range (size " +
                                std::to_string(a[0].list.size()) + ")";
                return Value::number(0.0);
            }
            return a[0].list[static_cast<std::size_t>(idx)];
        }

        // ---- 自我 / 查询（S1）----
        case Builtin::Self:
            return Value::makeEntity(m_host.entity, nameOf(m_host.entity));
        case Builtin::NameOf: {
            const ecs::Entity e = entityOf(a[0], line, "name()");
            if (!m_err.ok()) return Value::number(0.0);
            return Value::str(nameOf(e));
        }
        case Builtin::Exists: {
            const ecs::Entity e = entityOf(a[0], line, "exists()");
            if (!m_err.ok()) return Value::boolean(false);
            return Value::boolean(alive(e));
        }
        case Builtin::Find: {
            if (!m_host.scene) return Value::makeEntity(ecs::Entity{});
            const std::string want = a[0].toStr();
            const ecs::Entity e = m_host.scene->find(want);
            // 找不到 → 无效实体（`exists()` 会是 false），**不报错**：
            // "可能不存在"是查询的正常结果，不是程序错误。
            return Value::makeEntity(e, e.valid() ? want : std::string());
        }
        case Builtin::Near: {
            const glm::vec3 c = v3(0, "near(center)");
            const float r = f(1, "near(radius)");
            if (!m_err.ok()) return Value::makeList({});
            int limit = 0;
            if (a.size() >= 3)
                limit = static_cast<int>(n(2, "near(limit)"));
            return Value::makeList(collectNear(c, r, limit));
        }
        case Builtin::Dist: {
            const glm::vec3 p0 = v3(0, "dist(a)");
            const glm::vec3 p1 = v3(1, "dist(b)");
            if (!m_err.ok()) return Value::number(0.0);
            return Value::number(static_cast<double>(glm::length(p0 - p1)));
        }

        // ---- 世界访问（S2）----
        case Builtin::Player: {
            // 没有安装 = 这一局没有玩家（`exists(player())` 为 false）。
            // 这不算错误：单机里"玩家还没出生"是正常状态。
            if (!m_host.act.player) return Value::makeEntity(ecs::Entity{});
            const ecs::Entity p = m_host.act.player();
            return Value::makeEntity(p, p.valid() ? nameOf(p) : std::string());
        }
        case Builtin::Pos: {
            const ecs::Entity e = entityOf(a[0], line, "pos()");
            if (!m_err.ok()) return Value::makeVec3(glm::vec3(0.0f));
            // ⚠ 无效实体必须报错。返回 vec3(0) 会让 `pos(player())` 在
            //   "玩家还没出生"时静默把东西搬到原点 —— 而错的地方离这里很远。
            if (!alive(e)) return badEntity(line, "pos()", e);
            return Value::makeVec3(glm::vec3(m_host.scene->worldMatrix(e)[3]));
        }
        case Builtin::SetPos: {
            const ecs::Entity e = entityOf(a[0], line, "setPos()");
            const glm::vec3 p = v3(1, "setPos(position)");
            if (!m_err.ok()) return Value::number(0.0);
            if (!alive(e)) return badEntity(line, "setPos()", e);
            if (!m_host.scene->world().get<ecs::TransformComponent>(e)) {
                m_err.line = line;
                m_err.message = std::string("setPos() needs a transform on '") +
                                nameOf(e) + "'";
                return Value::number(0.0);
            }
            setWorldPos(e, p);
            m_indexFresh = false;  // 空间索引里的包围盒过期了
            // ★ C2：物理刚体必须**同一帧**跟上。只写 Transform 的话，
            //   下一次物理步进会用刚体自己的位置把变换"改回去"，表现成
            //   "设了位置又弹回原处"，且日志无痕。
            if (m_host.act.setBodyPos) m_host.act.setBodyPos(e, p);
            return Value::number(0.0);
        }
        case Builtin::Rot: {  // 角度制，与 rotate() 一致
            const ecs::Entity e = entityOf(a[0], line, "rot()");
            if (!m_err.ok()) return Value::makeVec3(glm::vec3(0.0f));
            if (!alive(e)) return badEntity(line, "rot()", e);
            const auto* t =
                m_host.scene->world().get<ecs::TransformComponent>(e);
            return Value::makeVec3(t ? glm::degrees(t->rotation)
                                     : glm::vec3(0.0f));
        }
        case Builtin::SetRot: {  // 角度制（门转 -90 度比 -1.5708 好写）
            const ecs::Entity e = entityOf(a[0], line, "setRot()");
            const glm::vec3 r = v3(1, "setRot(rotation)");
            if (!m_err.ok()) return Value::number(0.0);
            if (!alive(e)) return badEntity(line, "setRot()", e);
            if (ecs::TransformComponent* t =
                    m_host.scene->world().get<ecs::TransformComponent>(e)) {
                t->rotation = glm::radians(r);
                m_indexFresh = false;
            }
            return Value::number(0.0);
        }
        case Builtin::RotY: {
            const ecs::Entity e = entityOf(a[0], line, "rotY()");
            if (!m_err.ok()) return Value::number(0.0);
            if (!alive(e)) return badEntity(line, "rotY()", e);
            const auto* t =
                m_host.scene->world().get<ecs::TransformComponent>(e);
            return Value::number(t ? static_cast<double>(glm::degrees(t->rotation.y))
                                   : 0.0);
        }
        case Builtin::ScaleOf: {
            const ecs::Entity e = entityOf(a[0], line, "scaleOf()");
            if (!m_err.ok()) return Value::makeVec3(glm::vec3(1.0f));
            if (!alive(e)) return badEntity(line, "scaleOf()", e);
            const auto* t =
                m_host.scene->world().get<ecs::TransformComponent>(e);
            return Value::makeVec3(t ? t->scale : glm::vec3(1.0f));
        }
        case Builtin::Vel: {
            const ecs::Entity e = entityOf(a[0], line, "vel()");
            if (!m_err.ok()) return Value::makeVec3(glm::vec3(0.0f));
            glm::vec3 v(0.0f);
            if (!m_host.act.bodyVel || !m_host.act.bodyVel(e, &v))
                return noBody(line, "vel()", e);
            return Value::makeVec3(v);
        }
        case Builtin::SetVel: {
            const ecs::Entity e = entityOf(a[0], line, "setVel()");
            const glm::vec3 v = v3(1, "setVel(velocity)");
            if (!m_err.ok()) return Value::number(0.0);
            if (!m_host.act.setBodyVel) return noBody(line, "setVel()", e);
            m_host.act.setBodyVel(e, v);
            return Value::number(0.0);
        }
        case Builtin::Impulse: {
            const ecs::Entity e = entityOf(a[0], line, "impulse()");
            const glm::vec3 v = v3(1, "impulse(impulse)");
            if (!m_err.ok()) return Value::number(0.0);
            if (!m_host.act.impulse) return noBody(line, "impulse()", e);
            m_host.act.impulse(e, v);
            return Value::number(0.0);
        }
        case Builtin::Grounded: {
            const ecs::Entity e = entityOf(a[0], line, "grounded()");
            if (!m_err.ok()) return Value::boolean(false);
            // 没有物理世界时永远 false（不报错：地形缺失的游戏仍应能跑）。
            if (!m_host.act.grounded) return Value::boolean(false);
            return Value::boolean(m_host.act.grounded(e));
        }
        case Builtin::Attr: {
            const ecs::Entity e = entityOf(a[0], line, "attr()");
            if (!m_err.ok()) return Value::number(0.0);
            if (!alive(e)) return badEntity(line, "attr()", e);
            const std::string key = a[1].toStr();
            auto* sv = m_host.scene->world().get<ecs::ScriptVarsComponent>(e);
            if (sv) {
                auto it = sv->values.find(key);
                if (it != sv->values.end()) return Value::number(it->second);
            }
            // ★ C5：给了默认值 → 返回它**并写回**。写回是必须的：
            //   否则 `if (attr(e,"hp",100) < 20)` 每次都会重新取默认值，
            //   "血慢慢掉"这类写法永远不生效。
            if (a.size() >= 3) {
                const double d = n(2, "attr(default)");
                if (!m_err.ok()) return Value::number(0.0);
                if (!sv) sv = &m_host.scene->world().add<ecs::ScriptVarsComponent>(e);
                sv->values[key] = d;
                return Value::number(d);
            }
            m_err.line = line;
            m_err.message = "attr '" + key + "' not set on '" + nameOf(e) +
                            "' (pass a default: attr(e, \"" + key + "\", 0))";
            return Value::number(0.0);
        }
        case Builtin::SetAttr: {
            const ecs::Entity e = entityOf(a[0], line, "setAttr()");
            if (!m_err.ok()) return Value::number(0.0);
            if (!alive(e)) return badEntity(line, "setAttr()", e);
            const double v = n(2, "setAttr(value)");
            if (!m_err.ok()) return Value::number(0.0);
            auto& sv = m_host.scene->world().add<ecs::ScriptVarsComponent>(e);
            sv.values[a[1].toStr()] = v;
            return Value::number(0.0);
        }
        case Builtin::WorldAttr: {
            if (!m_host.scene) return Value::number(0.0);
            const std::string key = a[0].toStr();
            auto& m = m_host.scene->scriptVars();
            auto it = m.find(key);
            if (it != m.end()) return Value::number(it->second);
            if (a.size() >= 2) {
                const double d = n(1, "worldAttr(default)");
                if (!m_err.ok()) return Value::number(0.0);
                m[key] = d;
                return Value::number(d);
            }
            m_err.line = line;
            m_err.message = "worldAttr '" + key +
                            "' not set (pass a default: worldAttr(\"" + key +
                            "\", 0))";
            return Value::number(0.0);
        }
        case Builtin::SetWorldAttr: {
            if (!m_host.scene) return Value::number(0.0);
            const double v = n(1, "setWorldAttr(value)");
            if (!m_err.ok()) return Value::number(0.0);
            m_host.scene->scriptVars()[a[0].toStr()] = v;
            return Value::number(0.0);
        }
        case Builtin::Visible: {
            const ecs::Entity e = entityOf(a[0], line, "visible()");
            if (!m_err.ok()) return Value::boolean(false);
            if (!alive(e)) return badEntity(line, "visible()", e);
            const auto* vis =
                m_host.scene->world().get<ecs::VisibilityComponent>(e);
            return Value::boolean(!vis || vis->visible);
        }

        // ---- 协程（S4）----
        // 正常到不了这里：Parser 只把它们生成 `Stmt::Kind::Wait`。留一个
        // 明确的分支，是为了让"新增内建但忘了处理"在 switch 上露出马脚。
        case Builtin::Wait:
        case Builtin::WaitFrames:
        case Builtin::WaitUntil: {
            m_err.line = line;
            m_err.message = "wait family cannot be used inside an expression";
            return Value::number(0.0);
        }

        // ---- 输入（S3）----
        //
        // ⚠ 宿主没填（编辑态 / 自检 / 回放）时一律 false / 0，**不报错**：
        //   "这一局没有输入"是正常状态，不是脚本写错。D7 就是这个判据。
        case Builtin::Pressed: {
            const std::string act = a[0].toStr();
            return Value::boolean(m_host.act.pressed && m_host.act.pressed(act));
        }
        case Builtin::Held: {
            const std::string act = a[0].toStr();
            return Value::boolean(m_host.act.held && m_host.act.held(act));
        }
        case Builtin::Released: {
            const std::string act = a[0].toStr();
            return Value::boolean(m_host.act.released &&
                                  m_host.act.released(act));
        }
        case Builtin::Axis: {
            const std::string act = a[0].toStr();
            if (!m_host.act.axis) return Value::number(0.0);
            return Value::number(static_cast<double>(m_host.act.axis(act)));
        }

        // ---- 变换（老签名）----
        case Builtin::Move: {
            ecs::TransformComponent* t = xform();
            if (t) t->position = glm::vec3(f(0, "move"), f(1, "move"), f(2, "move"));
            return Value::number(0.0);
        }
        case Builtin::MoveBy: {
            ecs::TransformComponent* t = xform();
            if (t)
                t->position +=
                    glm::vec3(f(0, "moveBy"), f(1, "moveBy"), f(2, "moveBy"));
            return Value::number(0.0);
        }
        case Builtin::Rotate: {  // 角度制
            ecs::TransformComponent* t = xform();
            if (t)
                t->rotation = glm::radians(
                    glm::vec3(f(0, "rotate"), f(1, "rotate"), f(2, "rotate")));
            return Value::number(0.0);
        }
        case Builtin::RotateBy: {
            ecs::TransformComponent* t = xform();
            if (t)
                t->rotation += glm::radians(glm::vec3(
                    f(0, "rotateBy"), f(1, "rotateBy"), f(2, "rotateBy")));
            return Value::number(0.0);
        }
        case Builtin::Scale: {
            ecs::TransformComponent* t = xform();
            if (t)
                t->scale = glm::vec3(f(0, "scale"), f(1, "scale"), f(2, "scale"));
            return Value::number(0.0);
        }
        case Builtin::ScaleBy: {
            ecs::TransformComponent* t = xform();
            if (t)
                t->scale *=
                    glm::vec3(f(0, "scaleBy"), f(1, "scaleBy"), f(2, "scaleBy"));
            return Value::number(0.0);
        }

        // ---- HUD（U8）----
        //
        // 语法形状与变换一致（全局函数、对"当前实体"生效），不引入成员访问。
        case Builtin::SetHealth: {
            if (m_host.setHealth) m_host.setHealth(f(0, "setHealth"));
            return Value::number(0.0);
        }
        case Builtin::Damage: {
            const float v = f(0, "damage");
            if (m_host.damage) m_host.damage(v);
            // 扣血默认在**当前实体头顶**弹一条数字（"击杀木桶 → 数字在
            // 木桶上方"是 U8 的验收条款）。没有 Transform 的实体只扣血。
            if (m_host.popup) {
                if (ecs::TransformComponent* t = xform()) {
                    m_host.popup(t->position.x, t->position.y,
                                 t->position.z + 1.6f, v);
                }
            }
            return Value::number(0.0);
        }
        case Builtin::Popup: {
            const float v = f(0, "popup");
            if (m_host.popup) {
                if (ecs::TransformComponent* t = xform()) {
                    m_host.popup(t->position.x, t->position.y,
                                 t->position.z + 1.6f, v);
                }
            }
            return Value::number(0.0);
        }

        // ---- 音频（G14-8）----
        case Builtin::PlaySound: {
            if (m_host.playSound) m_host.playSound(a[0].toStr());
            return Value::number(0.0);
        }
        case Builtin::StopAllSounds: {
            if (m_host.stopAllSounds) m_host.stopAllSounds();
            return Value::number(0.0);
        }
        case Builtin::SetBusVolume: {
            if (m_host.setBusVolume)
                m_host.setBusVolume(a[0].toStr(), f(1, "setBusVolume"));
            return Value::number(0.0);
        }

        // ---- 灯光 ----
        case Builtin::SetColor: {
            const glm::vec3 c =
                glm::vec3(f(0, "setColor"), f(1, "setColor"), f(2, "setColor"));
            if (auto* pl = pointLight()) pl->color = c;
            if (auto* sl = spotLight()) sl->color = c;
            return Value::number(0.0);
        }
        case Builtin::SetIntensity: {
            const float v = f(0, "setIntensity");
            if (auto* pl = pointLight()) pl->intensity = v;
            if (auto* sl = spotLight()) sl->intensity = v;
            return Value::number(0.0);
        }
        case Builtin::SetRange: {
            const float v = f(0, "setRange");
            if (auto* pl = pointLight()) pl->range = v;
            if (auto* sl = spotLight()) sl->range = v;
            return Value::number(0.0);
        }
        case Builtin::SetDirection: {
            if (auto* sl = spotLight()) {
                const glm::vec3 d =
                    glm::vec3(f(0, "setDirection"), f(1, "setDirection"),
                              f(2, "setDirection"));
                sl->direction = glm::length(d) > 1e-6f ? glm::normalize(d)
                                                       : glm::vec3(0, -1, 0);
            }
            return Value::number(0.0);
        }

        // ---- 可见性 / 材质 ----
        case Builtin::SetVisible: {
            // 两种签名并存（§5.4：「老签名保留」）：
            //   setVisible(false)      —— 对自己（S1 之前唯一的写法）
            //   setVisible(other, false) —— 对别人（S2）
            // 参数个数在**编译期**就已经定死了，所以这里只是分岔，不查错。
            if (a.size() == 1) {
                if (auto* v = visibility()) v->visible = a[0].truthy();
                return Value::number(0.0);
            }
            const ecs::Entity e = entityOf(a[0], line, "setVisible()");
            if (!m_err.ok()) return Value::number(0.0);
            if (!alive(e)) return badEntity(line, "setVisible()", e);
            ecs::World& w = m_host.scene->world();
            auto* vis = w.get<ecs::VisibilityComponent>(e);
            if (!vis) vis = &w.add<ecs::VisibilityComponent>(e);
            vis->visible = a[1].truthy();
            return Value::number(0.0);
        }
        case Builtin::SetEmissive: {
            // ⚠ 材质是**共享资产**（AssetManager 按名字缓存），改它会影响
            //   所有用同一材质的实体。脚本里只应对独占材质使用。
            if (assets::Material* m = material()) {
                m->emissive = glm::vec3(f(0, "setEmissive"), f(1, "setEmissive"),
                                        f(2, "setEmissive"));
            }
            return Value::number(0.0);
        }

        // ---- 演出（C6 导演 API）----
        //
        // 三件的共同契约：**失败必须报错，不许静默返回 true**。宿主动作
        // 缺失（编辑器 Play 没注入 / Game.exe 没接）也报错 —— "这一局没有
        // 演出能力"本身就是需要知道的事实。
        case Builtin::PlaySequence: {
            const std::string path = strOf(a[0], line, "playSequence(path)");
            if (!m_err.ok()) return Value::boolean(false);
            if (!m_host.act.playSequence) {
                m_err.line = line;
                m_err.message =
                    "playSequence() is not available in this host (no "
                    "sequencer wired up)";
                return Value::boolean(false);
            }
            const bool ok = m_host.act.playSequence(path);
            if (!ok) {
                m_err.line = line;
                m_err.message = "playSequence('" + path +
                                "'): failed to load (see the SEQ-CUE-FAIL "
                                "log line for err=)";
            }
            return Value::boolean(ok);
        }
        case Builtin::ShowDialogue: {
            const std::string id = strOf(a[0], line, "showDialogue(id)");
            if (!m_err.ok()) return Value::boolean(false);
            if (!m_host.act.showDialogue) {
                m_err.line = line;
                m_err.message =
                    "showDialogue() is not available in this host (no "
                    "dialogue system wired up)";
                return Value::boolean(false);
            }
            const bool ok = m_host.act.showDialogue(id);
            if (!ok) {
                m_err.line = line;
                m_err.message = "showDialogue('" + id +
                                "'): failed to load (see the DLG-LOAD-FAIL "
                                "log line for the reason)";
            }
            return Value::boolean(ok);
        }
        case Builtin::PlayAnim: {
            // `playAnim(clip)` → 隐式 self；`playAnim(e, clip)` → 指定的那个。
            // ⚠ 用 `a.size()` 而不是"第二个参数是否为空"来判形态：脚本没有
            //   "空参数"，只有"少传参数"。
            const std::size_t n = a.size();
            const ecs::Entity e = (n >= 2)
                                      ? entityOf(a[0], line, "playAnim(entity)")
                                      : m_host.entity;
            const std::string clip = strOf(a[n - 1], line, "playAnim(clip)");
            if (!m_err.ok()) return Value::boolean(false);
            if (!m_host.act.playAnim) {
                m_err.line = line;
                m_err.message =
                    "playAnim() is not available in this host (no animator "
                    "wired up)";
                return Value::boolean(false);
            }
            if (!alive(e)) {
                m_err.line = line;
                m_err.message = "playAnim(): target entity is invalid or "
                                "already destroyed";
                return Value::boolean(false);
            }
            const bool ok = m_host.act.playAnim(e, clip);
            if (!ok) {
                m_err.line = line;
                m_err.message = "playAnim('" + clip + "'): '" + nameOf(e) +
                                "' has no Animator component (or the clip does "
                                "not exist in its skeleton)";
            }
            return Value::boolean(ok);
        }

        // ============================================================
        // C6 第二批：世界控制
        // ============================================================
        case Builtin::SetFlag: {
            const std::string name = strOf(a[0], line, "setFlag(name)");
            // 值是 Num 或 Bool：`setFlag("met_alice", true)` 与
            // `setFlag("affection", 42)` 都是正常写法，两者在表里都存
            // 成 double（Bool → 1/0）。字符串 / vec3 / 实体一律报错 ——
            // 这张表是**数值**表，混进字符串就会在存档格式上炸开。
            double v = 0.0;
            if (a[1].k == Value::K::Bool) {
                v = a[1].b ? 1.0 : 0.0;
            } else {
                v = numOf(a[1], line, "setFlag(value)");
            }
            if (!m_err.ok()) return Value::boolean(false);
            if (name.empty()) {
                m_err.line = line;
                m_err.message = "setFlag(): the flag name is empty";
                return Value::boolean(false);
            }
            if (!m_host.act.setFlag) {
                m_err.line = line;
                m_err.message =
                    "setFlag() is not available in this host (no flag store "
                    "wired up)";
                return Value::boolean(false);
            }
            const bool ok = m_host.act.setFlag(name, v);
            if (!ok) {
                m_err.line = line;
                m_err.message = "setFlag('" + name + "') was rejected by the "
                                "host (see the RUNTIME-FLAG log line)";
            }
            return Value::boolean(ok);
        }
        case Builtin::GetFlag: {
            const std::string name = strOf(a[0], line, "getFlag(name)");
            if (!m_err.ok()) return Value::number(0.0);
            if (name.empty()) {
                m_err.line = line;
                m_err.message = "getFlag(): the flag name is empty";
                return Value::number(0.0);
            }
            if (!m_host.act.getFlag) {
                m_err.line = line;
                m_err.message =
                    "getFlag() is not available in this host (no flag store "
                    "wired up)";
                return Value::number(0.0);
            }
            // ⚠ 未设过 = 0 且**不报错**（见 ScriptEngine.h 的注释）。
            return Value::number(m_host.act.getFlag(name));
        }
        case Builtin::Spawn: {
            // `spawn(path)` → 原点；`spawn(path, vec3(x,y,z))` → 指定位置。
            const std::string path = strOf(a[0], line, "spawn(path)");
            glm::vec3 pos(0.0f);
            if (a.size() >= 2) pos = vecOf(a[1], line, "spawn(pos)");
            if (!m_err.ok()) return Value::makeEntity(ecs::Entity{});
            if (!m_host.act.spawn) {
                m_err.line = line;
                m_err.message =
                    "spawn() is not available in this host (no scene "
                    "instantiation wired up)";
                return Value::makeEntity(ecs::Entity{});
            }
            const ecs::Entity e = m_host.act.spawn(path, pos);
            if (!e.valid()) {
                // 失败必须报错：刷不出来却静默返回一个无效句柄，下一句
                // `setPos(e, ...)` 只会说"实体无效"，根因就此消失。
                m_err.line = line;
                m_err.message = "spawn('" + path +
                                "'): failed to instantiate the model (see the "
                                "RUNTIME-SPAWN-FAIL log line for why)";
                return Value::makeEntity(ecs::Entity{});
            }
            return Value::makeEntity(e, nameOf(e));
        }
        case Builtin::LoadScene: {
            const std::string path = strOf(a[0], line, "loadScene(path)");
            if (!m_err.ok()) return Value::boolean(false);
            if (!m_host.act.loadScene) {
                m_err.line = line;
                m_err.message =
                    "loadScene() is not available in this host (no scene "
                    "router wired up)";
                return Value::boolean(false);
            }
            // 返回值 = "请求已受理"（下一帧生效），见 ScriptEngine.h。
            const bool ok = m_host.act.loadScene(path);
            if (!ok) {
                m_err.line = line;
                m_err.message =
                    "loadScene('" + path +
                    "'): the scene file does not exist or cannot be read "
                    "(nothing was scheduled)";
            }
            return Value::boolean(ok);
        }

        case Builtin::None:
        case Builtin::Count_:
            break;
    }

    m_err.line = line;
    m_err.message = "internal: unhandled builtin id " +
                    std::to_string(static_cast<int>(b));
    return Value::number(0.0);
}

// ---------------------------------------------------------------- 文件时间戳

int64_t fileWriteTime(const std::string& path) {
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(path, ec);
    if (ec) return 0;
    return static_cast<int64_t>(t.time_since_epoch().count());
}

} // namespace

// ---------------------------------------------------------------- 值
//
// `Value` 的**定义**在 ScriptEngine.h（S0：实例表要跨编译单元持有它）。
// 这里只放需要 <cstdio> 的那一个成员。
// ⚠ 必须在匿名命名空间**外面** —— `vks::Value` 的成员函数不能在
//   `vks::{anonymous}` 里定义（编译器会认成另一个类的成员，报 C2888）。

double Value::asNum() const {
    switch (k) {
        case K::Num: return num;
        case K::Bool: return b ? 1.0 : 0.0;
        case K::Str: return std::strtod(s.c_str(), nullptr);
        // ⚠ 见头文件：这三个落到 default 是**故意**的兜底，调用方必须
        //   先用 `Evaluator::numOf()` 拦下。任何直接把 vec3 当数字用的
        //   路径都是 bug（"位置怎么变成 0 了"）。
        default: return 0.0;
    }
}

bool Value::truthy() const {
    switch (k) {
        case K::Num: return num != 0.0;
        case K::Bool: return b;
        case K::Str: return !s.empty();
        case K::Vec3: return v.x != 0.0f || v.y != 0.0f || v.z != 0.0f;
        case K::Entity: return e.valid();
        case K::List: return !list.empty();
    }
    return false;
}

const char* Value::typeName() const {
    switch (k) {
        case K::Num: return "number";
        case K::Bool: return "bool";
        case K::Str: return "string";
        case K::Vec3: return "vec3";
        case K::Entity: return "entity";
        case K::List: return "list";
    }
    return "?";
}

std::string Value::toStr() const {
    char buf[96];
    switch (k) {
        case K::Bool: return b ? "true" : "false";
        case K::Str: return s;
        case K::Vec3:
            std::snprintf(buf, sizeof(buf), "(%g, %g, %g)",
                          static_cast<double>(v.x), static_cast<double>(v.y),
                          static_cast<double>(v.z));
            return buf;
        case K::Entity:
            // `Enemy#12` 而不是 `entity#12`：日志里要人看得懂。
            // 名字是 find() 时缓存的（引擎的 Entity 不存名字）。
            if (!e.valid()) return "entity#none";
            if (!s.empty()) {
                std::snprintf(buf, sizeof(buf), "%s#%u", s.c_str(),
                              static_cast<unsigned>(e.id));
            } else {
                std::snprintf(buf, sizeof(buf), "entity#%u",
                              static_cast<unsigned>(e.id));
            }
            return buf;
        case K::List: {
            std::string out = "[";
            for (std::size_t i = 0; i < list.size(); ++i) {
                if (i) out += ", ";
                out += list[i].toStr();
            }
            out += "]";
            return out;
        }
        default:
            std::snprintf(buf, sizeof(buf), "%g", num);
            return buf;
    }
}

// ============================================================
// ScriptProgram
// ============================================================

const char* hookName(HookKind k) {
    switch (k) {
        case HookKind::Start: return "start";
        case HookKind::Update: return "update";
        case HookKind::Collide: return "collide";
        case HookKind::Trigger: return "trigger";
        case HookKind::Exit: return "exit";
    }
    return "?";
}

bool hookFromName(const std::string& name, HookKind* out) {
    static const HookKind kAll[] = {HookKind::Start, HookKind::Update,
                                    HookKind::Collide, HookKind::Trigger,
                                    HookKind::Exit};
    for (HookKind k : kAll) {
        if (name == hookName(k)) {
            if (out) *out = k;
            return true;
        }
    }
    return false;
}

// ⚠ 必须 out-of-line：`body` 是 `vector<unique_ptr<Stmt>>`，而 `Stmt` 只在
//   本文件里完整。写在头文件里等于让每个 TU 都去析构一个不完整类型。
HookBlock::HookBlock() = default;
HookBlock::~HookBlock() = default;

ScriptProgram::ScriptProgram() = default;
ScriptProgram::~ScriptProgram() = default;

void ScriptProgram::addPrint(std::string s) { m_prints.push_back(std::move(s)); }

bool ScriptProgram::compile(const std::string& source,
                            const std::string& debugName, ScriptError& err) {
    err = ScriptError{};
    m_valid = false;
    m_source = source;
    m_debugName = debugName;
    m_ast.clear();
    m_hooks.clear();

    std::vector<Token> tokens;
    {
        Lexer lex(source);
        if (!lex.tokenize(tokens, err)) return false;
    }

    std::vector<StmtPtr> ast;
    std::vector<std::unique_ptr<HookBlock>> hooks;
    {
        Parser parser(tokens);
        if (!parser.parseProgram(ast, hooks, err)) return false;
    }

    m_ast = std::move(ast);
    m_hooks = std::move(hooks);

    // 声明区（开头连续的 let）—— 见头文件里的理由
    m_declCount = 0;
    while (m_declCount < m_ast.size() && m_ast[m_declCount] &&
           m_ast[m_declCount]->kind == Stmt::Kind::Let)
        ++m_declCount;

    indexAst();   // S4：建 id → 语句 的反查表（热重载对账用）
    m_valid = true;
    return true;
}

bool ScriptProgram::run(const ScriptHost& host, double time, double dt,
                        uint64_t frame, ScriptError& err) {
    err = ScriptError{};
    m_prints.clear();

    if (!m_valid) {
        err.message = "program not compiled";
        return false;
    }

    // ⚠ 这张表**随调用结束就丢** —— 这就是 P1。每帧跑的地方一律走下面
    //   那个带 ScriptRunContext 的重载。
    std::unordered_map<std::string, Value> vars;
    ScriptCoroutine scratch;   // 一次性求值：用完就丢（`wait` 在这条路上无意义）
    Evaluator eval(host, *this, err, vars, &m_prints, scratch);
    if (!eval.run(m_ast, time, dt, frame, 0)) return false;
    return err.ok();
}

bool ScriptProgram::run(const ScriptHost& host, ScriptRunContext& ctx,
                        ScriptError& err) {
    err = ScriptError{};
    ctx.steps = 0;

    if (!m_valid) {
        err.message = "program not compiled";
        return false;
    }
    if (!ctx.vars) {
        err.message = "run context has no variable table";
        return false;
    }
    if (ctx.prints) ctx.prints->clear();

    ScriptCoroutine scratch;
    Evaluator eval(host, *this, err, *ctx.vars, ctx.prints, scratch);
    if (!eval.run(m_ast, ctx.time, ctx.dt, ctx.frame, ctx.simFrame)) {
        ctx.steps = eval.steps();
        return false;
    }
    ctx.steps = eval.steps();
    return err.ok();
}

bool ScriptProgram::runFrame(const ScriptHost& host, ScriptRunContext& ctx,
                             ScriptCoroutine& coro, const ScriptFrameInput& in,
                             ScriptError& err) {
    err = ScriptError{};
    ctx.steps = 0;
    ctx.executed = false;

    if (!m_valid) {
        err.message = "program not compiled";
        return false;
    }
    if (!ctx.vars) {
        err.message = "run context has no variable table";
        return false;
    }
    if (ctx.prints) ctx.prints->clear();

    // ⚠ 全帧**一个** Evaluator：配额是按"每帧每实例"算的。
    Evaluator eval(host, *this, err, *ctx.vars, ctx.prints, coro);
    eval.beginFrame(ctx.time, ctx.dt, ctx.frame, ctx.simFrame);

    // 一个钩子的执行（S4）。`hookIndex` 让"第 2 个 on collide 挂起"这件事
    // 能正确恢复：已跑完的钩子不重跑，挂起的那一个从栈里继续。
    // ⚠ 写成 lambda 而不是成员函数：参数里有 Evaluator，而 Evaluator 只在
    //   本文件里存在 —— 头文件没法声明这样的签名。
    auto runHookSegment = [&](HookKind kind, const Value* param) {
        size_t idx = 0;
        for (const auto& hb : m_hooks) {
            if (!hb || hb->kind != kind) continue;
            if (idx < coro.hookIndex) {
                ++idx;
                continue;
            }
            if (param && !hb->param.empty()) eval.bind(hb->param, *param);
            const Evaluator::Step st =
                eval.runSegment(hb->body, 0, hb->body.size(), hb->id);
            if (st != Evaluator::Step::Done) return st;
            ++coro.hookIndex;
            ++idx;
        }
        coro.hookIndex = 0;
        return Evaluator::Step::Done;
    };

    // ⓪-a 还挂在 wait 里 → 先看能不能唤醒。没到点就**整帧空转**：
    //      连 update 都不跑（"这个脚本在等，它什么也不做"）。
    if (!eval.tryWake()) {
        ctx.steps = eval.steps();
        return true;
    }
    if (!err.ok()) {
        ctx.steps = eval.steps();
        return false;
    }

    // ⓪-b 相位机。phase 存在协程里 —— 挂起跨帧，恢复时要知道自己跑到哪一相。
    //   0 声明区 / 1 start / 2 接触事件 / 3 顶层 / 4 update
    while (coro.phase <= 4) {
        Evaluator::Step st = Evaluator::Step::Done;
        switch (coro.phase) {
            case 0:
                // 声明区（开头连续的 let）。必须在 on start 之前 —— 否则
                // start 里读 `let hp = 100` 声明的 hp 就是"未定义变量"。
                st = eval.runSegment(m_ast, 0, m_declCount, 0);
                break;
            case 1:
                if (in.runStart)
                    st = runHookSegment(HookKind::Start, nullptr);
                break;
            case 2: {
                while (coro.contactIndex < in.contacts.size()) {
                    const ScriptFrameInput::Contact& c =
                        in.contacts[coro.contactIndex];
                    if (c.kind != HookKind::Collide &&
                        c.kind != HookKind::Trigger &&
                        c.kind != HookKind::Exit) {
                        ++coro.contactIndex;
                        continue;
                    }
                    st = runHookSegment(c.kind, &c.other);
                    if (st != Evaluator::Step::Done) break;  // 挂起/出错
                    ++coro.contactIndex;
                }
                if (st == Evaluator::Step::Done) coro.contactIndex = 0;
                break;
            }
            case 3:
                st = eval.runSegment(m_ast, m_declCount, m_ast.size(), 0);
                break;
            case 4:
                st = runHookSegment(HookKind::Update, nullptr);
                break;
            default:
                break;
        }
        if (st == Evaluator::Step::Error) {
            ctx.steps = eval.steps();
            return false;
        }
        if (st == Evaluator::Step::Suspend) {
            ctx.steps = eval.steps();
            ctx.executed = true;   // 挂起也是"动了"（跟"空转"是两回事）
            return true;
        }
        ++coro.phase;
    }

    // 一帧完整跑完 → 相位归零，下一帧从声明区（空转）重新开始
    coro.phase = 0;
    ctx.steps = eval.steps();
    ctx.executed = true;
    return err.ok();
}

// ---------------------------------------------------------------- 索引（S4）
//
// 对账要知道"旧栈里那一帧挂在哪条语句上"。语句靠 `Stmt::id` 认身份，
// 所以编译成功之后把所有 id 收进一张表 —— 之后 reattach 只查表，不再遍历。

namespace {
void indexStmts(const std::vector<StmtPtr>& list,
                std::unordered_map<uint32_t, const Stmt*>& out) {
    for (const auto& sp : list) {
        if (!sp) continue;
        out[sp->id] = sp.get();
        // ⚠ body 与 elseBody 都要走 —— If 的两个分支各是一份列表
        indexStmts(sp->body, out);
        indexStmts(sp->elseBody, out);
    }
}
}  // namespace

void ScriptProgram::indexAst() {
    m_byId.clear();
    m_hookById.clear();
    indexStmts(m_ast, m_byId);
    for (const auto& hb : m_hooks) {
        if (!hb) continue;
        m_hookById[hb->id] = hb.get();
        indexStmts(hb->body, m_byId);
    }
}

const Stmt* ScriptProgram::stmtById(uint32_t id) const {
    auto it = m_byId.find(id);
    return it == m_byId.end() ? nullptr : it->second;
}

const std::vector<StmtPtr>* ScriptProgram::listFor(uint32_t ownerId,
                                                  uint8_t branch) const {
    if (ownerId == 0) return &m_ast;
    auto hit = m_hookById.find(ownerId);
    if (hit != m_hookById.end()) return &hit->second->body;
    const Stmt* s = stmtById(ownerId);
    if (!s) return nullptr;
    return branch ? &s->elseBody : &s->body;
}

bool ScriptProgram::reattach(ScriptCoroutine& coro) const {
    // 没在飞 → 没什么可翻译的
    if (coro.frames.empty() && coro.state != ScriptCoroutine::State::Waiting)
        return true;

    std::vector<ScriptCoroutine::Frame> rebuilt;
    rebuilt.reserve(coro.frames.size());
    for (const ScriptCoroutine::Frame& f : coro.frames) {
        ScriptCoroutine::Frame nf = f;
        const std::vector<StmtPtr>* list = listFor(f.ownerId, f.branch);
        if (!list) return false;                      // 这条语句没了
        if (nf.ip > list->size()) return false;       // 位置越界
        if (nf.end > list->size())
            nf.end = static_cast<uint32_t>(list->size());
        nf.list = list;
        if (f.kind == ScriptCoroutine::Frame::K::While) {
            const Stmt* st = stmtById(f.ownerId);
            if (!st || !st->expr) return false;
            nf.cond = st->expr.get();
        } else if (f.kind == ScriptCoroutine::Frame::K::Foreach) {
            const Stmt* st = stmtById(f.ownerId);
            // 元素快照 `items` 不动 —— 它是运行期数据，与 AST 无关
            if (!st || st->kind != Stmt::Kind::Foreach) return false;
            nf.var = st->name;
        }
        rebuilt.push_back(std::move(nf));
    }

    // waitUntil 的条件表达式也指向旧 AST
    const Expr* newCond = nullptr;
    if (coro.state == ScriptCoroutine::State::Waiting &&
        coro.wake == ScriptCoroutine::Wake::Until) {
        const Stmt* st = stmtById(coro.condOwnerId);
        if (!st || st->kind != Stmt::Kind::Wait || !st->expr ||
            st->expr->args.empty())
            return false;
        newCond = st->expr->args[0].get();
    }

    coro.frames = std::move(rebuilt);
    coro.cond = newCond;
    return true;
}

std::shared_ptr<ScriptCoroutine> ScriptEngine::makeCoroutine() {
    return std::make_shared<ScriptCoroutine>();
}

// ============================================================
// ScriptAsset
// ============================================================

bool ScriptAsset::load() {
    m_resolvedPath = assets::resolveAssetPath(m_requestedPath);

    std::error_code ec;
    m_fileExists = std::filesystem::exists(m_resolvedPath, ec) && !ec;
    if (!m_fileExists) {
        m_lastError.line = 0;
        m_lastError.message = "script file not found: " + m_resolvedPath;
        return false;
    }

    std::ifstream in(m_resolvedPath, std::ios::binary);
    if (!in) {
        m_lastError.line = 0;
        m_lastError.message = "cannot open: " + m_resolvedPath;
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();

    auto prog = std::make_unique<ScriptProgram>();
    ScriptError err;
    if (!prog->compile(ss.str(), m_requestedPath, err)) {
        // 关键：**保留旧的可用程序**。热重载最常见的场景是"文件刚存了一半
        // /语法写错了"，此时让脚本继续按上一版跑，比让场景里的物体直接
        // 僵住好得多。
        m_lastError = err;
        return false;
    }

    if (m_loadedOnce) ++m_reloadCount;
    m_loadedOnce = true;
    m_lastWriteTime = fileWriteTime(m_resolvedPath);
    m_program = std::move(prog);
    // 版本号只在**成功编译**后发：失败时保留旧程序，版本号不动 ——
    // 于是"改了但没编译过"不会解除熔断（正是 S-D8 想要的）。
    if (m_versionSeq) m_version = ++(*m_versionSeq);
    m_lastError = ScriptError{};
    return true;
}

bool ScriptAsset::poll() {
    if (m_resolvedPath.empty())
        m_resolvedPath = assets::resolveAssetPath(m_requestedPath);

    const int64_t t = fileWriteTime(m_resolvedPath);
    if (t == m_lastWriteTime) return false;  // 时间戳没变
    m_lastWriteTime = t;                     // 先记下，避免每帧重试
    return load();
}

// ============================================================
// ScriptEngine
// ============================================================

ScriptAsset* ScriptEngine::get(const std::string& relativePath) {
    auto it = m_assets.find(relativePath);
    if (it != m_assets.end()) return it->second.get();

    auto asset = std::make_unique<ScriptAsset>();
    asset->setRequestedPath(relativePath);
    asset->attachVersionSeq(&m_versionSeq);
    if (asset->load()) {
        logLine("[load] " + relativePath + "  OK");
    } else {
        logLine("[load] " + relativePath + "  FAILED: " +
                asset->lastError().toString());
    }

    ScriptAsset* raw = asset.get();
    m_assets.emplace(relativePath, std::move(asset));
    return raw;
}

int ScriptEngine::pollHotReload() {
    int n = 0;
    for (auto& kv : m_assets) {
        ScriptAsset& a = *kv.second;
        if (!a.poll()) continue;
        ++n;
        if (a.lastError().ok()) {
            logLine("[hot-reload] " + a.requestedPath() + "  OK (#" +
                    std::to_string(a.reloadCount()) + ")");
            VK_LOG_INFO("Script hot-reloaded: %s", a.requestedPath().c_str());
        } else {
            logLine("[hot-reload] " + a.requestedPath() +
                    "  FAILED: " + a.lastError().toString());
            VK_LOG_WARN("Script hot-reload failed: %s -> %s",
                        a.requestedPath().c_str(),
                        a.lastError().toString().c_str());
        }
    }
    return n;
}

bool ScriptEngine::reloadNow(const std::string& relativePath) {
    auto it = m_assets.find(relativePath);
    if (it == m_assets.end()) {
        get(relativePath);
        return true;
    }
    const bool ok = it->second->load();
    logLine(std::string("[reload] ") + relativePath +
            (ok ? "  OK" : "  FAILED: " + it->second->lastError().toString()));
    return ok;
}

void ScriptEngine::clear() { m_assets.clear(); }

void ScriptEngine::logLine(const std::string& s) {
    m_log.push_back(s);
    if (m_log.size() > 200) m_log.erase(m_log.begin());
}

std::vector<std::string> ScriptEngine::loadedPaths() const {
    std::vector<std::string> out;
    out.reserve(m_assets.size());
    for (const auto& kv : m_assets) out.push_back(kv.first);
    return out;
}

} // namespace vks
