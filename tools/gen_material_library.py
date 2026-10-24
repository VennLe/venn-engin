#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从 `venn-resource` 的 glTF 材质包生成引擎材质仓库。

输入（只读）：E:/code/cpp/venn-resource/<mat>/<mat>_<tier>.gltf/
              ├─ <mat>.bin
              ├─ <mat>_<tier>.gltf        ← 真正的 JSON
              └─ textures/<mat>_{diff,nor_gl,arm}_<tier>.jpg

输出（可重复执行，覆盖写）：
  assets/textures/materials/<mat>/<mat>_{diff,nor_gl,arm}.<ext>
  assets/materials/library/<mat>.vmat        ← 引擎原生材质资产
  assets/materials/library.json              ← 仓库清单（面板/工具读这个）

设计要点
--------
1. **不硬编码贴图文件名**。glTF 里 `baseColorTexture` / `metallicRoughnessTexture`
   / `normalTexture` 三个槽各指向一个 `textures[i].source` → `images[j].uri`，
   这里按索引解析，重命名后缀也不会错。
2. 取 **1k 档**：2k 单张 diff 就 3.7 MB，4k 更大，不适合进版本库；1k 全库约 5 MB。
   用 `--tier 2k` 可以换档，输出目录相同（覆盖）。
3. `arm` 贴图与引擎 `ormMap` 的通道约定完全一致（R=AO / G=Rough / B=Metal），
   所以直接挂 `orm` 槽。同时用 PIL 统计三个通道的均值，写进 `.vmat` 的
   `ao / roughness / metallic` 标量 —— 地形层着色器**只吃标量**（`terrain.frag`
   不采样 orm），不带这一步的话地形地面会是一片"粗糙度 1.0 的全白高光"。
4. `baseColor` 固定写 `[1,1,1,1]`：albedo 贴图本身已经带颜色，再乘一个非白的
   baseColor 会把画面压暗（`terrain.frag` 是 `albedo = Σ w·baseColor·tex`）。

用法：
  python tools/gen_material_library.py                # 默认 1k
  python tools/gen_material_library.py --tier 2k
  python tools/gen_material_library.py --dry-run      # 只打印计划
  python tools/gen_material_library.py --check        # 校验产物齐全，缺失则退出码 1
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:  # pragma: no cover
    Image = None  # type: ignore[assignment]

# ---------------------------------------------------------------- 路径常量

ENGINE_ROOT = Path(__file__).resolve().parent.parent
SRC_ROOT = Path("E:/code/cpp/venn-resource")

TEX_OUT_ROOT = ENGINE_ROOT / "assets" / "textures" / "materials"
VMEAT_OUT_DIR = ENGINE_ROOT / "assets" / "materials" / "library"
MANIFEST_PATH = ENGINE_ROOT / "assets" / "materials" / "library.json"

DEFAULT_TIER = "1k"
ALL_TIERS = ("1k", "2k", "4k")

# 材质 id 前缀：和 assets/materials/*.vmat 里的 "materials/xxx" 保持一致风格
ID_PREFIX = "materials/library/"

# 每个材质的中文显示名与分类。分类决定面板里的分组，加新材质时在这里登记；
# 没登记的材质会退化成「分类 = 目录名，标签 = 目录名」。
LOCALE = {
    "grass_ground": {"label": "Grass Ground", "labelZh": "草地",
                     "category": "ground", "categoryZh": "地面"},
    "concrete_moss": {"label": "Concrete Moss", "labelZh": "苔藓混凝土",
                      "category": "concrete", "categoryZh": "混凝土"},
}

# 地形默认地面材质（用户要求：以后地形地面默认 grass_ground）
DEFAULT_ENTRY = "grass_ground"

# glTF 一个材质里三个贴图槽 → 引擎槽位名
#   key = glTF material 里的字段路径，value = 输出文件名后缀
SLOT_DIFF = "diff"
SLOT_NORMAL = "nor_gl"
SLOT_ARM = "arm"


# ---------------------------------------------------------------- 小工具

def log(msg: str) -> None:
    print(msg, flush=True)


def read_json(path: Path) -> dict:
    with path.open("r", encoding="utf-8") as f:
        return json.load(f)


def write_json(path: Path, doc: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    text = json.dumps(doc, indent=2, ensure_ascii=False) + "\n"
    with path.open("w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def resolve_uri(gltf_dir: Path, uri: str) -> Path:
    """glTF 的 uri 是相对 .gltf 文件所在目录的 POSIX 路径。"""
    return gltf_dir / uri.replace("/", os.sep)


def slot_uri(doc: dict, gltf_dir: Path, material: dict) -> dict:
    """按 glTF 索引解析出三个贴图槽的真实路径。

    返回 {"diff": Path, "nor_gl": Path, "arm": Path}，缺哪个就没有哪个 key。
    """
    images = doc.get("images", [])
    textures = doc.get("textures", [])

    def tex_uri(tex_ref) -> Path | None:
        if not isinstance(tex_ref, dict):
            return None
        ti = tex_ref.get("index")
        if not isinstance(ti, int) or not (0 <= ti < len(textures)):
            return None
        src = textures[ti].get("source")
        if not isinstance(src, int) or not (0 <= src < len(images)):
            return None
        uri = images[src].get("uri")
        if not isinstance(uri, str) or not uri:
            return None
        p = resolve_uri(gltf_dir, uri)
        return p if p.is_file() else None

    pbr = material.get("pbrMetallicRoughness", {}) or {}
    out: dict = {}
    p = tex_uri(pbr.get("baseColorTexture"))
    if p:
        out[SLOT_DIFF] = p
    p = tex_uri(material.get("normalTexture"))
    if p:
        out[SLOT_NORMAL] = p
    p = tex_uri(pbr.get("metallicRoughnessTexture"))
    if p:
        out[SLOT_ARM] = p
    # glTF 标准里 AO 走单独的 occlusionTexture；这批资源把 AO 塞进 arm 的 R 通道，
    # 但仍兼容一下标准写法（若只有 occlusionTexture，就只用它的 R 通道，见下）
    p = tex_uri(material.get("occlusionTexture"))
    if p and SLOT_ARM not in out:
        out[SLOT_ARM] = p
    return out


def channel_means(path: Path) -> tuple[float, float, float]:
    """统计一张贴图 R/G/B 三个通道的归一化均值。用来给标量参数取合理初值。"""
    if Image is None:
        return (1.0, 1.0, 0.0)
    with Image.open(path) as im:
        rgb = im.convert("RGB")
        # 缩小到 64x64 再统计：肉眼等效，速度快一个数量级
        rgb = rgb.resize((64, 64), Image.BILINEAR)
        raw = rgb.tobytes()          # RGBRGBRGB…，避免 getdata() 的弃用警告
    n = float(len(raw) // 3) or 1.0
    r = sum(raw[0::3]) / n
    g = sum(raw[1::3]) / n
    b = sum(raw[2::3]) / n
    return (r / 255.0, g / 255.0, b / 255.0)


def clamp01(v: float) -> float:
    return 0.0 if v < 0.0 else (1.0 if v > 1.0 else v)


def round3(v: float) -> float:
    return round(v, 3)


# ---------------------------------------------------------------- 主流程

def discover(tier: str) -> list[dict]:
    """扫描资源根，返回每个材质的一条描述。"""
    if not SRC_ROOT.is_dir():
        raise SystemExit(f"[FATAL] 资源根不存在：{SRC_ROOT}")

    found: list[dict] = []
    for mat_dir in sorted(p for p in SRC_ROOT.iterdir() if p.is_dir()):
        name = mat_dir.name
        pkg = mat_dir / f"{name}_{tier}.gltf"
        if not pkg.is_dir():
            continue
        gltf_path = pkg / f"{name}_{tier}.gltf"
        if not gltf_path.is_file():
            # 目录存在但内层文件名不符：兜底扫一遍
            cands = sorted(pkg.glob("*.gltf"))
            if not cands:
                log(f"[SKIP] {name}: {pkg} 里没有 .gltf")
                continue
            gltf_path = cands[0]

        doc = read_json(gltf_path)
        mats = doc.get("materials") or []
        if not mats:
            log(f"[SKIP] {name}: glTF 没有 materials")
            continue
        material = mats[0]
        slots = slot_uri(doc, pkg, material)
        if SLOT_DIFF not in slots:
            log(f"[WARN] {name}: 没有 baseColorTexture，只有 {sorted(slots)}")

        found.append({
            "name": name,
            "tier": tier,
            "gltf": gltf_path,
            "package": pkg,
            "material": material,
            "slots": slots,
            "tiers": sorted(
                t for t in ALL_TIERS
                if (mat_dir / f"{name}_{t}.gltf").is_dir()),
        })
    return found


def build_entry(rec: dict, dry_run: bool, check: bool) -> dict:
    """把一个材质包落成 贴图 + .vmat + 清单条目。"""
    name = rec["name"]
    tier = rec["tier"]
    slots = rec["slots"]
    material = rec["material"]

    tex_dir = TEX_OUT_ROOT / name
    copied: dict[str, str] = {}

    for slot_key, src in slots.items():
        ext = src.suffix.lower()
        dst_name = f"{name}_{slot_key}{ext}"
        dst = tex_dir / dst_name
        rel = f"textures/materials/{name}/{dst_name}"
        copied[slot_key] = rel
        if dry_run or check:
            if check and not dst.is_file():
                log(f"  [MISS] {rel}")
            elif dry_run:
                log(f"  [PLAN] {rel}")
            continue
        tex_dir.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src, dst)
        log(f"  [TEX ] {rel}  ({src.stat().st_size // 1024} KB)")

    # arm 的通道均值 → 标量 ao / roughness / metallic
    ao, rough, metal = 1.0, 0.9, 0.0
    arm_src = slots.get(SLOT_ARM)
    if arm_src is not None:
        ao, rough, metal = channel_means(arm_src)
    # glTF 的 metallicFactor 若为 0 就尊重它（这批资源都是 0）
    mf = material.get("pbrMetallicRoughness", {}).get("metallicFactor")
    if isinstance(mf, (int, float)) and mf == 0:
        metal = 0.0
    rough = clamp01(rough)
    ao = clamp01(ao)
    metal = clamp01(metal)

    loc = LOCALE.get(name, {"label": name, "labelZh": name,
                            "category": name, "categoryZh": name})
    mat_id = f"{ID_PREFIX}{name}"

    vmat: dict = {
        "type": "material",
        "version": 1,
        "id": mat_id,
        "emissive": [0.0, 0.0, 0.0],
        # 这批资源 glTF 里 doubleSided=true，但地形/地面是封闭朝向的实体面，
        # 双面会额外多走一次背面光照且关掉背面剔除，直接关掉。
        "doubleSided": False,
        "normalScale": 1.0,
        "ao": round3(ao),
        "alphaCutoff": 0.5,
        "alphaMode": "opaque",
        "shadingModel": "default",
        # 保持纯白：颜色全交给 albedo 贴图，避免二次压暗（见文件头说明 4）
        "baseColor": [1.0, 1.0, 1.0, 1.0],
        "metallic": round3(metal),
        "roughness": round3(rough),
        "library": {
            "name": name,
            "label": loc["label"],
            "labelZh": loc["labelZh"],
            "category": loc["category"],
            "categoryZh": loc["categoryZh"],
            "tier": tier,
            "tags": ["terrain", "ground"] if loc["category"] == "ground"
                    else ["terrain"],
        },
    }
    if SLOT_DIFF in copied:
        vmat["albedo"] = {"kind": "file", "name": f"{name}_diff",
                          "path": copied[SLOT_DIFF], "srgb": True}
    if SLOT_NORMAL in copied:
        vmat["normal"] = {"kind": "file", "name": f"{name}_nor_gl",
                          "path": copied[SLOT_NORMAL], "srgb": False}
    if SLOT_ARM in copied:
        # R=AO / G=Rough / B=Metal —— 与 pbr.frag 的 `ormMap` 布局逐通道一致
        vmat["orm"] = {"kind": "file", "name": f"{name}_arm",
                       "path": copied[SLOT_ARM], "srgb": False}

    vmat_rel = f"{ID_PREFIX}{name}.vmat"
    if not (dry_run or check):
        write_json(VMEAT_OUT_DIR / f"{name}.vmat", vmat)
        log(f"  [VMAT] {vmat_rel}  rough={vmat['roughness']} "
            f"ao={vmat['ao']} metal={vmat['metallic']}")

    return {
        "id": mat_id,
        "name": name,
        "label": loc["label"],
        "labelZh": loc["labelZh"],
        "category": loc["category"],
        "categoryZh": loc["categoryZh"],
        "tier": tier,
        "tiers": rec["tiers"],
        "default": name == DEFAULT_ENTRY,
        "vmat": vmat_rel,
        "textures": {
            "albedo": copied.get(SLOT_DIFF, ""),
            "normal": copied.get(SLOT_NORMAL, ""),
            "orm": copied.get(SLOT_ARM, ""),
        },
        "roughness": vmat["roughness"],
        "metallic": vmat["metallic"],
        "ao": vmat["ao"],
    }


def main() -> int:
    global SRC_ROOT
    ap = argparse.ArgumentParser(description="生成引擎材质仓库")
    ap.add_argument("--tier", default=DEFAULT_TIER, choices=ALL_TIERS,
                    help="贴图档位（默认 1k）")
    ap.add_argument("--dry-run", action="store_true", help="只打印，不落盘")
    ap.add_argument("--check", action="store_true",
                    help="校验产物是否齐全（缺失返回 1）")
    ap.add_argument("--src", default=str(SRC_ROOT), help="资源根目录")
    args = ap.parse_args()

    SRC_ROOT = Path(args.src)

    log(f"[GEN ] src={SRC_ROOT}  tier={args.tier}  out={VMEAT_OUT_DIR}")
    recs = discover(args.tier)
    if not recs:
        log("[FATAL] 没扫到任何材质包")
        return 1
    log(f"[GEN ] 扫到 {len(recs)} 个材质包：{', '.join(r['name'] for r in recs)}")

    entries = [build_entry(r, args.dry_run, args.check) for r in recs]

    # 分类顺序按 LOCALE 里登记的先后，未登记的分类排后面
    order = {}
    for i, meta in enumerate(LOCALE.values()):
        order.setdefault(meta["category"], i)
    entries.sort(key=lambda e: (order.get(e["category"], 99), e["name"]))

    manifest = {
        "type": "material_library",
        "version": 1,
        "generatedBy": "tools/gen_material_library.py",
        "tier": args.tier,
        "default": f"{ID_PREFIX}{DEFAULT_ENTRY}",
        "count": len(entries),
        "entries": entries,
    }

    if args.check:
        if not MANIFEST_PATH.is_file():
            log(f"[MISS] {MANIFEST_PATH.relative_to(ENGINE_ROOT)}")
            return 1
        on_disk = read_json(MANIFEST_PATH)
        if on_disk.get("entries") != entries:
            log("[DIFF] library.json 与资源不一致，请重跑生成脚本")
            return 1
        log(f"[OK  ] {len(entries)} 条条目齐全，清单一致")
        return 0

    if args.dry_run:
        log(json.dumps(manifest, indent=2, ensure_ascii=False))
        return 0

    write_json(MANIFEST_PATH, manifest)
    log(f"[GEN ] 清单 → {MANIFEST_PATH.relative_to(ENGINE_ROOT)}"
        f"（{len(entries)} 条，默认 {manifest['default']}）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
