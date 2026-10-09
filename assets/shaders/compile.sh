#!/bin/sh
# ============================================================
# compile.sh —— 手动编译着色器（glslc 来自 Vulkan SDK）
# 输出 .spv 到本目录（构建时 CMake 也会自动编译，二者等价）
#
# 遍历所有 *.vert / *.frag / *.comp —— 加新着色器不用改本脚本
# ============================================================
set -e
cd "$(dirname "$0")"

if [ -z "$VULKAN_SDK" ]; then
    echo "[error] VULKAN_SDK is not set. Install Vulkan SDK first."
    exit 1
fi

GLSLC="$VULKAN_SDK/bin/glslc"
if [ ! -x "$GLSLC" ]; then
    GLSLC="$(command -v glslc || true)"
fi
if [ -z "$GLSLC" ]; then
    echo "[error] glslc not found."
    exit 1
fi

for f in *.vert *.frag *.comp; do
    [ -e "$f" ] || continue
    echo "  $f"
    "$GLSLC" "$f" -o "$f.spv"
done

echo "Done. SPIR-V files generated in $(pwd)"
