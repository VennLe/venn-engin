import json, struct, sys

def dump(p):
    with open(p, "rb") as f:
        data = f.read()
    if data[:4] == b"glTF":
        off = 12
        clen, ctype = struct.unpack_from("<II", data, off)
        js = data[off + 8: off + 8 + clen].decode("utf-8")
    else:
        js = data.decode("utf-8")
    g = json.loads(js)
    print("=== " + p + " ===")
    print("materials:", json.dumps(g.get("materials"), indent=1)[:1500])
    print("images:", g.get("images"))
    print("textures:", g.get("textures"))
    print("attrs:", [list(pr.get("attributes", {}).keys())
                     for m in g.get("meshes", []) for pr in m.get("primitives", [])])
    print("nodes:", json.dumps(g.get("nodes"))[:600])
    print()

for p in sys.argv[1:]:
    dump(p)
