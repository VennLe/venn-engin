#pragma once
// ============================================================
// render/ShaderPath —— 着色器文件定位
// 单独成文件，供 Renderer 与各 Pass 共用（避免互相 include）
// ============================================================

#include <string>

namespace render {

// 解析顺序：
//   1. 环境变量 MYVK_SHADER_DIR
//   2. 可执行文件所在目录 /shaders      （双击运行也有效）
//   3. 当前工作目录 /shaders
//   4. 当前工作目录 /assets/shaders
std::string resolveShaderPath(const std::string& filename);

} // namespace render
