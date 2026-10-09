#include "ScriptEngine.h"

#include "assets/AssetPath.h"
#include "assets/Material.h"
#include "core/Logger.h"
#include "ecs/Components.h"
#include "scene/Scene.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace editor {

// ============================================================
// AST —— 头文件只做前向声明（struct Expr; struct Stmt;），
// 完整定义留在本 .cpp 里：语法细节不对外暴露，但类型身份必须是
// editor::Expr / editor::Stmt —— 也就是头文件里 StmtPtr / ExprPtr
// 指向的那两个类型。
//
// 注意：**绝不能**把这两个结构体放进下面的匿名命名空间。
// 匿名命名空间里的 Expr/Stmt 是**另外两个类型**，和 editor::Expr /
// editor::Stmt 毫无关系，于是 StmtPtr 与 parse* 的返回值对不上，
// 编译器会报一堆 "could not convert unique_ptr<anonymous::Stmt> to
// unique_ptr<editor::Stmt>"。
//
// 放在 namespace editor 里同样对外不可见：别的编译单元只看到前向
// 声明，拿不到布局，更无法构造。
// ============================================================

struct Expr {
    enum class Kind { Number, Str, Var, Unary, Binary, Call };

    Kind kind = Kind::Number;
    double num = 0.0;
    std::string text;  // Var 名 / Call 名 / Str 内容
    std::string op;    // Unary / Binary 的运算符文本
    int line = 1;
    ExprPtr a, b;               // Unary 用 a；Binary 用 a / b
    std::vector<ExprPtr> args;  // Call
};

struct Stmt {
    enum class Kind { Assign, ExprStmt, If, While, Block };

    Kind kind = Kind::ExprStmt;
    std::string name;           // Assign 的目标变量
    ExprPtr expr;               // Assign 值 / ExprStmt / If 条件 / While 条件
    std::vector<StmtPtr> body;  // Block / then / 循环体
    std::vector<StmtPtr> elseBody;
    int line = 1;
};

namespace {

// 单次求值的总步数上限：脚本里写出死循环时报错，
// 而不是把编辑器挂死
constexpr int kMaxSteps = 200000;

// ---------------------------------------------------------------- 词法

enum class T : uint8_t {
    End, Number, Str, Ident,
    Plus, Minus, Star, Slash, Percent,
    LParen, RParen, LBrace, RBrace, Comma, Semi,
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

            if (std::isdigit(static_cast<unsigned char>(c)) || c == '.') {
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

    bool parseProgram(std::vector<StmtPtr>& out, ScriptError& err) {
        skipSeparators();
        while (!check(T::End)) {
            StmtPtr s = parseStatement(err);
            if (!s) return false;
            out.push_back(std::move(s));
            skipSeparators();
        }
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

    StmtPtr parseStatement(ScriptError& err) {
        skipSeparators();
        if (check(T::LBrace)) return parseBlock(err);
        if (check(T::Ident) && peek().text == "if") return parseIf(err);
        if (check(T::Ident) && peek().text == "while") return parseWhile(err);

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

    StmtPtr parseBlock(ScriptError& err) {
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

        StmtPtr thenStmt = parseStatement(err);
        if (!thenStmt) return nullptr;
        s->body.push_back(std::move(thenStmt));

        skipSeparators();
        if (check(T::Ident) && peek().text == "else") {
            ++m_i;
            StmtPtr elseStmt = parseStatement(err);
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

        StmtPtr body = parseStatement(err);
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
        return parsePrimary(err);
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
};

// ---------------------------------------------------------------- 值

struct Value {
    enum class K { Num, Bool, Str };

    K k = K::Num;
    double num = 0.0;
    bool b = false;
    std::string s;

    static Value number(double v) {
        Value x;
        x.k = K::Num;
        x.num = v;
        return x;
    }
    static Value boolean(bool v) {
        Value x;
        x.k = K::Bool;
        x.b = v;
        return x;
    }
    static Value str(std::string v) {
        Value x;
        x.k = K::Str;
        x.s = std::move(v);
        return x;
    }

    double asNum() const {
        switch (k) {
            case K::Num: return num;
            case K::Bool: return b ? 1.0 : 0.0;
            default: return std::strtod(s.c_str(), nullptr);
        }
    }
    bool truthy() const {
        switch (k) {
            case K::Num: return num != 0.0;
            case K::Bool: return b;
            default: return !s.empty();
        }
    }
    std::string toStr() const {
        switch (k) {
            case K::Bool: return b ? "true" : "false";
            case K::Str: return s;
            default: {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%g", num);
                return buf;
            }
        }
    }
};

// ---------------------------------------------------------------- 求值器

class Evaluator {
public:
    Evaluator(const ScriptHost& host, ScriptProgram& prog, ScriptError& err)
        : m_host(host), m_prog(prog), m_err(err) {
        m_vars["index"] = Value::number(
            host.entity.valid() ? static_cast<double>(host.entity.id) : -1.0);
    }

    bool run(const std::vector<StmtPtr>& ast, double time, double dt,
             uint64_t frame) {
        m_vars["time"] = Value::number(time);
        m_vars["dt"] = Value::number(dt);
        m_vars["frame"] = Value::number(static_cast<double>(frame));
        for (const auto& s : ast) {
            if (!exec(*s)) return false;
        }
        return true;
    }

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

    bool exec(const Stmt& s) {
        if (!step(s.line)) return false;

        switch (s.kind) {
            case Stmt::Kind::Assign: {
                const Value v = eval(*s.expr);
                if (!m_err.ok()) return false;
                m_vars[s.name] = v;
                return true;
            }
            case Stmt::Kind::ExprStmt: {
                eval(*s.expr);
                return m_err.ok();
            }
            case Stmt::Kind::Block: {
                for (const auto& inner : s.body) {
                    if (!exec(*inner)) return false;
                }
                return true;
            }
            case Stmt::Kind::If: {
                const Value cond = eval(*s.expr);
                if (!m_err.ok()) return false;
                const auto& branch = cond.truthy() ? s.body : s.elseBody;
                for (const auto& inner : branch) {
                    if (!exec(*inner)) return false;
                }
                return true;
            }
            case Stmt::Kind::While: {
                for (;;) {
                    const Value cond = eval(*s.expr);
                    if (!m_err.ok()) return false;
                    if (!cond.truthy()) break;
                    if (!step(s.line)) return false;
                    for (const auto& inner : s.body) {
                        if (!exec(*inner)) return false;
                    }
                }
                return true;
            }
        }
        return true;
    }

    static bool valuesEqual(const Value& a, const Value& b) {
        if (a.k == Value::K::Str || b.k == Value::K::Str)
            return a.toStr() == b.toStr();
        return a.asNum() == b.asNum();
    }

    Value eval(const Expr& e) {
        if (!step(e.line)) return Value::number(0.0);

        switch (e.kind) {
            case Expr::Kind::Number: return Value::number(e.num);
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

            case Expr::Kind::Unary: {
                const Value v = eval(*e.a);
                if (!m_err.ok()) return Value::number(0.0);
                if (e.op == "-") return Value::number(-v.asNum());
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

                const std::string& op = e.op;
                if (op == "+") {
                    if (l.k == Value::K::Str || r.k == Value::K::Str)
                        return Value::str(l.toStr() + r.toStr());
                    return Value::number(l.asNum() + r.asNum());
                }
                if (op == "-") return Value::number(l.asNum() - r.asNum());
                if (op == "*") return Value::number(l.asNum() * r.asNum());
                if (op == "/") {
                    const double d = r.asNum();
                    if (d == 0.0) {
                        m_err.line = e.line;
                        m_err.message = "division by zero";
                        return Value::number(0.0);
                    }
                    return Value::number(l.asNum() / d);
                }
                if (op == "%") {
                    const double d = r.asNum();
                    if (d == 0.0) {
                        m_err.line = e.line;
                        m_err.message = "modulo by zero";
                        return Value::number(0.0);
                    }
                    return Value::number(std::fmod(l.asNum(), d));
                }
                if (op == "==") return Value::boolean(valuesEqual(l, r));
                if (op == "!=") return Value::boolean(!valuesEqual(l, r));
                if (op == "<") return Value::boolean(l.asNum() < r.asNum());
                if (op == "<=") return Value::boolean(l.asNum() <= r.asNum());
                if (op == ">") return Value::boolean(l.asNum() > r.asNum());
                if (op == ">=") return Value::boolean(l.asNum() >= r.asNum());

                m_err.line = e.line;
                m_err.message = "unknown operator '" + op + "'";
                return Value::number(0.0);
            }

            case Expr::Kind::Call: {
                std::vector<Value> args;
                args.reserve(e.args.size());
                for (const auto& a : e.args) {
                    args.push_back(eval(*a));
                    if (!m_err.ok()) return Value::number(0.0);
                }
                return call(e.text, args, e.line);
            }
        }
        return Value::number(0.0);
    }

    bool needArgs(const std::string& name, const std::vector<Value>& args,
                  size_t n, int line) {
        if (args.size() == n) return true;
        m_err.line = line;
        m_err.message = "'" + name + "' expects " + std::to_string(n) +
                        " argument(s), got " + std::to_string(args.size());
        return false;
    }

    Value call(const std::string& name, const std::vector<Value>& a, int line);

    const ScriptHost& m_host;
    ScriptProgram& m_prog;
    ScriptError& m_err;
    std::unordered_map<std::string, Value> m_vars;
    int m_steps = 0;
};

// 内置函数表。数学函数用一元 lambda 批量注册，引擎绑定逐个写清楚。
Value Evaluator::call(const std::string& name, const std::vector<Value>& a,
                      int line) {
    auto num1 = [&](double (*f)(double)) -> Value {
        if (!needArgs(name, a, 1, line)) return Value::number(0.0);
        return Value::number(f(a[0].asNum()));
    };
    auto num2 = [&](double (*f)(double, double)) -> Value {
        if (!needArgs(name, a, 2, line)) return Value::number(0.0);
        return Value::number(f(a[0].asNum(), a[1].asNum()));
    };

    // ---- 数学 ----
    if (name == "sin") return num1([](double x) { return std::sin(x); });
    if (name == "cos") return num1([](double x) { return std::cos(x); });
    if (name == "tan") return num1([](double x) { return std::tan(x); });
    if (name == "asin") return num1([](double x) { return std::asin(x); });
    if (name == "acos") return num1([](double x) { return std::acos(x); });
    if (name == "atan") return num1([](double x) { return std::atan(x); });
    if (name == "abs") return num1([](double x) { return std::fabs(x); });
    if (name == "floor") return num1([](double x) { return std::floor(x); });
    if (name == "ceil") return num1([](double x) { return std::ceil(x); });
    if (name == "round") return num1([](double x) { return std::round(x); });
    if (name == "sqrt") return num1([](double x) { return std::sqrt(x); });
    if (name == "atan2")
        return num2([](double y, double x) { return std::atan2(y, x); });
    if (name == "pow")
        return num2([](double x, double y) { return std::pow(x, y); });
    if (name == "min")
        return num2([](double x, double y) { return std::fmin(x, y); });
    if (name == "max")
        return num2([](double x, double y) { return std::fmax(x, y); });
    if (name == "clamp") {
        if (!needArgs(name, a, 3, line)) return Value::number(0.0);
        return Value::number(
            std::fmin(std::fmax(a[0].asNum(), a[1].asNum()), a[2].asNum()));
    }
    if (name == "lerp") {
        if (!needArgs(name, a, 3, line)) return Value::number(0.0);
        const double t = a[2].asNum();
        return Value::number(a[0].asNum() * (1.0 - t) + a[1].asNum() * t);
    }

    // ---- 输出 ----
    if (name == "print") {
        std::string msg;
        for (size_t i = 0; i < a.size(); ++i) {
            if (i) msg += ' ';
            msg += a[i].toStr();
        }
        m_prog.addPrint(std::move(msg));
        return Value::number(0.0);
    }

    auto vec3From = [&](size_t base) {
        return glm::vec3(static_cast<float>(a[base].asNum()),
                         static_cast<float>(a[base + 1].asNum()),
                         static_cast<float>(a[base + 2].asNum()));
    };

    // ---- 变换绑定 ----
    ecs::TransformComponent* t = xform();
    if (name == "move") {
        if (!needArgs(name, a, 3, line)) return Value::number(0.0);
        if (t) t->position = vec3From(0);
        return Value::number(0.0);
    }
    if (name == "moveBy") {
        if (!needArgs(name, a, 3, line)) return Value::number(0.0);
        if (t) t->position += vec3From(0);
        return Value::number(0.0);
    }
    if (name == "rotate") {  // 角度制
        if (!needArgs(name, a, 3, line)) return Value::number(0.0);
        if (t) t->rotation = glm::radians(vec3From(0));
        return Value::number(0.0);
    }
    if (name == "rotateBy") {
        if (!needArgs(name, a, 3, line)) return Value::number(0.0);
        if (t) t->rotation += glm::radians(vec3From(0));
        return Value::number(0.0);
    }
    if (name == "scale") {
        if (!needArgs(name, a, 3, line)) return Value::number(0.0);
        if (t) t->scale = vec3From(0);
        return Value::number(0.0);
    }
    if (name == "scaleBy") {
        if (!needArgs(name, a, 3, line)) return Value::number(0.0);
        if (t) t->scale *= vec3From(0);
        return Value::number(0.0);
    }

    // ---- 灯光绑定 ----
    if (name == "setColor") {
        if (!needArgs(name, a, 3, line)) return Value::number(0.0);
        const glm::vec3 c = vec3From(0);
        if (auto* pl = pointLight()) pl->color = c;
        if (auto* sl = spotLight()) sl->color = c;
        return Value::number(0.0);
    }
    if (name == "setIntensity") {
        if (!needArgs(name, a, 1, line)) return Value::number(0.0);
        const float v = static_cast<float>(a[0].asNum());
        if (auto* pl = pointLight()) pl->intensity = v;
        if (auto* sl = spotLight()) sl->intensity = v;
        return Value::number(0.0);
    }
    if (name == "setRange") {
        if (!needArgs(name, a, 1, line)) return Value::number(0.0);
        const float v = static_cast<float>(a[0].asNum());
        if (auto* pl = pointLight()) pl->range = v;
        if (auto* sl = spotLight()) sl->range = v;
        return Value::number(0.0);
    }
    if (name == "setDirection") {
        if (!needArgs(name, a, 3, line)) return Value::number(0.0);
        if (auto* sl = spotLight()) {
            const glm::vec3 d = vec3From(0);
            sl->direction =
                glm::length(d) > 1e-6f ? glm::normalize(d) : glm::vec3(0, -1, 0);
        }
        return Value::number(0.0);
    }

    // ---- 可见性 / 材质 ----
    if (name == "setVisible") {
        if (!needArgs(name, a, 1, line)) return Value::number(0.0);
        if (auto* v = visibility()) v->visible = a[0].truthy();
        return Value::number(0.0);
    }
    if (name == "setEmissive") {
        if (!needArgs(name, a, 3, line)) return Value::number(0.0);
        // 注意：材质是**共享资产**（AssetManager 按名字缓存），改它会影响
        // 所有用同一材质的实体。脚本里只应对独占材质使用。
        if (assets::Material* m = material()) m->emissive = vec3From(0);
        return Value::number(0.0);
    }

    m_err.line = line;
    m_err.message = "unknown function '" + name + "'";
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

// ============================================================
// ScriptProgram
// ============================================================

ScriptProgram::~ScriptProgram() = default;

void ScriptProgram::addPrint(std::string s) { m_prints.push_back(std::move(s)); }

bool ScriptProgram::compile(const std::string& source,
                            const std::string& debugName, ScriptError& err) {
    err = ScriptError{};
    m_valid = false;
    m_source = source;
    m_debugName = debugName;
    m_ast.clear();

    std::vector<Token> tokens;
    {
        Lexer lex(source);
        if (!lex.tokenize(tokens, err)) return false;
    }

    std::vector<StmtPtr> ast;
    {
        Parser parser(tokens);
        if (!parser.parseProgram(ast, err)) return false;
    }

    m_ast = std::move(ast);
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

    Evaluator eval(host, *this, err);
    if (!eval.run(m_ast, time, dt, frame)) return false;
    return err.ok();
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

} // namespace editor
