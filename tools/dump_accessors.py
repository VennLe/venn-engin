import json, struct, sys

p = sys.argv[1]
with open(p, "rb") as f:
    data = f.read()
off = 12
clen, ctype = struct.unpack_from("<II", data, off)
g = json.loads(data[off + 8: off + 8 + clen].decode("utf-8"))

print("=== " + p)
print("bufferViews:")
for i, bv in enumerate(g.get("bufferViews", [])):
    print("  [%d] byteOffset=%s byteLength=%s byteStride=%s target=%s"
          % (i, bv.get("byteOffset", 0), bv.get("byteLength"),
             bv.get("byteStride"), bv.get("target")))
print("accessors:")
for i, a in enumerate(g.get("accessors", [])):
    print("  [%d] bv=%s off=%s type=%s comp=%s count=%s min=%s max=%s"
          % (i, a.get("bufferView"), a.get("byteOffset", 0), a.get("type"),
             a.get("componentType"), a.get("count"), a.get("min"), a.get("max")))
print("primitives:")
for m in g.get("meshes", []):
    for pr in m.get("primitives", []):
        print("  attributes=%s indices=%s mode=%s material=%s"
              % (pr.get("attributes"), pr.get("indices"), pr.get("mode"),
                 pr.get("material")))
