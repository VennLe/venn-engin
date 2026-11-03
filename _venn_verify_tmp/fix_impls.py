import io, re

CPP = r"D:\A-VENN\code\cpp\venn-engin\engine\script\LuaBackend.cpp"
src = io.open(CPP, encoding="utf-8").read()

# ---- ① 补 engine.exists（ApiTable 里有条目，实现是 valid 的别名）----
if "int l_engine_exists" not in src:
    anchor = "int l_engine_name(lua_State* L) {"
    alias = ("// `exists` 是 `valid` 的语义化别名：文档里两个都写，实现只有一份。\n"
             "int l_engine_exists(lua_State* L) { return l_engine_valid(L); }\n\n")
    src = src.replace(anchor, alias + anchor, 1)

# ---- ② 重建 kImpls（正确的大小写正则）----
rows = re.findall(r"^int (l_[A-Za-z0-9_]+)\(lua_State\* L\) \{", src, re.M)
rows = sorted(set(rows))

NAMESPACES = ("engine", "world", "actor", "input", "anim", "nav", "fx",
              "audio", "light", "ui", "game")

def split_name(fn):
    body = fn[2:]
    # 最长前缀优先，避免 anim/fx 之类被更短的名字抢走
    for ns in sorted(NAMESPACES, key=len, reverse=True):
        if body.startswith(ns + "_"):
            return ns, body[len(ns) + 1:]
    raise SystemExit("cannot map " + fn)

table = "const Impl kImpls[] = {\n"
for fn in rows:
    ns, nm = split_name(fn)
    # 少数实现名与公开名不同（Lua 保留字 / 历史名）
    nm = {"goto": "moveTo"}.get(nm, nm)
    table += '    {"%s", "%s", %s},\n' % (ns, nm, fn)
table += "};"

start = src.index("const Impl kImpls[] = {")
end = src.index("\n};", start) + len("\n};")
src = src[:start] + table + src[end:]

io.open(CPP, "w", encoding="utf-8", newline="\n").write(src)
print("kImpls rows =", len(rows))
