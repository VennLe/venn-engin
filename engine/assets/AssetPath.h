#pragma once
// ============================================================
// assets/AssetPath —— 资源文件定位（模型 / 贴图等）
//
// 与 render/ShaderPath 同思路，但 assets 不能反向依赖 render，
// 所以各留一份（逻辑只有几行，重复无妨）。
//
// 解析顺序：
//   1. 环境变量 MYVK_ASSET_DIR + "/" + relative
//   2. 可执行文件所在目录 /assets/relative   （CMake 会把 assets 拷过去）
//   3. 当前工作目录 /assets/relative
//   4. 当前工作目录 /relative                （从项目根目录直接运行）
// ============================================================

#include <string>

namespace assets {

std::string resolveAssetPath(const std::string& relative);

// 反向操作：把一个（通常是绝对的）路径还原成"相对资产目录"的形式，
// 供场景序列化写入 JSON —— 这样存下来的场景换台机器也能用。
// 若无法归属任何已知根目录，返回通用斜杠形式的原路径。
std::string makeAssetRelative(const std::string& path);

} // namespace assets
