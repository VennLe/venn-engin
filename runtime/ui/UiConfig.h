#pragma once
// ============================================================
// runtime/ui/UiConfig —— HUD 外观配置（`config/ui.json`）
//
// ⚠ 为什么不放进场景 JSON：
//   HUD **不是场景对象**。放进场景的话，"换一个关卡"会把玩家调好的血条
//   位置一起换掉 —— 那是游戏设置，不是关卡内容。而且场景一存盘就会写进去，
//   两个关卡各存一份互相覆盖。
//
// 四条纪律沿用 `config/README.md`（E03 的同一套）：
//   ① 文件不存在 = **完全静默**（用默认值，不报错、不生成文件）；
//   ② 坏文件只 WARN，单键类型不对只拒那一个键；
//   ③ 自动化不写（这里只有显式 save 才写，运行时不自动落盘）；
//   ④ 值一律夹到合理范围（0.5×~4× 默认），否则血条能被配成负数宽度。
// ============================================================

#include <string>

namespace runtime {

struct UiConfig {
    // ---- 血条（参考分辨率 1920×1080 下的逻辑像素）----
    float barWidth = 360.0f;
    float barHeight = 18.0f;
    float barMargin = 24.0f;

    // ---- 准星 ----
    bool crosshair = true;
    float crosshairLen = 10.0f;    // 每条线的长度
    float crosshairGap = 4.0f;     // 中心到线起点的空隙
};

// `exeDir/config/ui.json`；不存在时再找 `cwd/config/ui.json`（开发期从仓库根跑）。
// 都不存在时返回 exeDir 那一份 —— 作为**写**的目标路径。
std::string defaultUiConfigPath();

// 读。返回 false = 没读到（文件不存在 / 解析失败），此时 `out` 保持默认值。
bool loadUiConfig(const std::string& path, UiConfig* out);

// 写（目录不存在就建）。返回是否成功。
bool saveUiConfig(const std::string& path, const UiConfig& c);

} // namespace runtime
