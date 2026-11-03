import io, re, sys

CPP = r"D:\A-VENN\code\cpp\venn-engin\engine\script\LuaBackend.cpp"
BLOCK = r"D:\A-VENN\code\cpp\venn-engin\_venn_verify_tmp\bind_block.cpp"

src = io.open(CPP, encoding="utf-8").read()
block = io.open(BLOCK, encoding="utf-8").read()

lines = src.split("\n")

def find(pred, start=0):
    for i in range(start, len(lines)):
        if pred(lines[i]):
            return i
    return -1

b0 = find(lambda l: l.startswith("// ---------------------------------------------------------------- 绑定"))
b1 = find(lambda l: l.startswith("// ---------------------------------------------------------------- 注册"), b0)
assert b0 > 0 and b1 > b0, (b0, b1)

new_lines = lines[:b0] + block.split("\n") + lines[b1:]
src = "\n".join(new_lines)

# ---- 替换 installBindings ----
start = src.index("void LuaBackend::installBindings() {")
end = src.index("\n} // namespace vks", start)

IMPL = r'''
namespace {

// (ns, name) → 实现。与 `ApiTable` 的条目一一对应。
// ⚠ 这里**不是**事实源：名字和参数个数以 ApiTable 为准，这里只是把名字
//   接到函数上。漏一条 → `installBindings()` 打 `LUA-API missing ...`，
//   不会变成"文档里有、脚本里调不到"。
struct Impl {
    const char* ns;
    const char* name;
    lua_CFunction fn;
};

const Impl kImpls[] = {
'''
rows = re.findall(r"^int (l_[a-z0-9_]+)\(lua_State\* L\) \{", block, re.M)
# 名字映射：l_world_setPos -> world, setPos
def split_name(fn):
    body = fn[2:]
    for ns in ("engine", "world", "actor", "input", "anim", "nav", "fx",
               "audio", "light", "ui", "game"):
        if body.startswith(ns + "_"):
            return ns, body[len(ns) + 1:]
    raise SystemExit("cannot map " + fn)

for fn in rows:
    ns, nm = split_name(fn)
    IMPL += '    {"%s", "%s", %s},\n' % (ns, nm, fn)

IMPL += r'''};

lua_CFunction findImpl(const char* ns, const char* name) {
    for (const Impl& i : kImpls)
        if (std::strcmp(i.ns, ns) == 0 && std::strcmp(i.name, name) == 0)
            return i.fn;
    return nullptr;
}

} // namespace

void LuaBackend::installBindings() {
    lua_State* L = m_L;
    LuaBackend* be = this;

    // ① 命名空间表：**从 ApiTable 里出现过的 ns 生成**，不再手写 `newTable`。
    std::vector<std::string> namespaces;
    for (int i = 0; i < apiEntryCount(); ++i) {
        const char* ns = apiEntries()[i].ns;
        bool seen = false;
        for (const auto& s : namespaces)
            if (s == ns) { seen = true; break; }
        if (!seen) namespaces.emplace_back(ns);
    }
    for (const auto& ns : namespaces) newTable(L, ns.c_str());
    // newTable 会把表留在栈上并设了全局 —— 逐个弹掉，后面按名取。
    lua_pop(L, int(namespaces.size()));

    // ② 函数：逐条从表注册。**缺实现就报错**，不静默跳过。
    int missing = 0;
    for (int i = 0; i < apiEntryCount(); ++i) {
        const ApiEntry& e = apiEntries()[i];
        lua_CFunction fn = findImpl(e.ns, e.name);
        if (!fn) {
            ++missing;
            VK_LOG_ERROR("LUA-API missing implementation: %s.%s", e.ns, e.name);
            continue;
        }
        lua_getglobal(L, e.ns);
        regFn(L, be, e.name, fn);
        lua_pop(L, 1);
    }

    // ③ 字段（不是函数）：每帧由实例刷新，函数调用不值。
    lua_getglobal(L, "engine");
    lua_pushnumber(L, 0.0); lua_setfield(L, -2, "dt");
    lua_pushnumber(L, 0.0); lua_setfield(L, -2, "time");
    lua_pushinteger(L, 0);  lua_setfield(L, -2, "frame");
    lua_pop(L, 1);

    if (missing > 0)
        VK_LOG_ERROR("LUA-API %d of %d entries have no implementation",
                     missing, apiEntryCount());
    VK_LOG_INFO("LUA-API registered %d functions in %d namespaces",
                apiEntryCount() - missing, int(namespaces.size()));
}
'''

src = src[:start] + IMPL + src[end:]

# include ApiTable
src = src.replace('#include "script/ScriptEngine.h"',
                  '#include "script/ApiTable.h"\n#include "script/ScriptEngine.h"', 1)

io.open(CPP, "w", encoding="utf-8", newline="\n").write(src)
print("ok: %d impls mapped, file now %d lines" % (len(rows), src.count("\n")))
