#pragma once
// ============================================================
// ui2/SdfFont —— SDF 字体图集的 **CPU 侧数据**（U1）
//
// 为什么单独一层：UiCanvas 要在**无窗口、无 GPU** 的自检里排版（量宽度、
// 数字形、验字节），所以"字体度量"必须是一份纯数据。谁把 `rhi::Image`
// 塞进这里，那套自检就废了 —— 那正是本项目一贯的红线（数值先可测）。
//
// 数据来源：`tools/gen_sdf_atlas.py` 离线生成的
//   assets/ui/fonts/noto_sc_sdf.png    R8，单通道距离场
//   assets/ui/fonts/noto_sc_sdf.json   字形表（图集像素坐标）
//
// ⚠ 图集**不是颜色**：它存的是"0.5 = 轮廓、±spread 映射到 0..1 两端"的
//   距离值。所以视图必须是 UNORM，绝不能挂 sRGB（否则采样时被当颜色解码，
//   整个字形的边缘会整体偏移）。
// ============================================================

#include <cstdint>
#include <string>
#include <unordered_map>

namespace ui2 {

// 一个字形在**图集空间**里的位置与推进（单位都是图集像素）。
struct SdfGlyph {
    // 图集 uv（已归一化到 0..1）。四角顺序无关，由调用方按 (u0,v0)-(u1,v1) 用。
    float u0 = 0.0f, v0 = 0.0f, u1 = 0.0f, v1 = 0.0f;
    float adv = 0.0f;      // 笔位推进
    float bearX = 0.0f;    // 格子左边缘相对笔位的偏移（负数 = 往左探出去）
    float bearY = 0.0f;    // 格子顶边相对基线的偏移（负数 = 基线上方）
    float w = 0.0f, h = 0.0f;   // 格子尺寸
};

struct SdfFont {
    std::string face;
    std::string license;
    uint32_t atlasW = 0;
    uint32_t atlasH = 0;
    float size = 0.0f;      // 图集里的字号（像素）—— 排版缩放的分母
    float spread = 0.0f;    // 距离场半径（图集像素）
    float ascent = 0.0f;
    float descent = 0.0f;
    float lineGap = 0.0f;

    std::unordered_map<uint32_t, SdfGlyph> glyphs;

    bool valid() const {
        return atlasW != 0 && atlasH != 0 && !glyphs.empty();
    }
    // 查不到返回 nullptr（**不做**静默替换 —— 缺字是数据问题，得让人看见）。
    const SdfGlyph* find(uint32_t cp) const;
    // 缺字时退回 '?'，用作"玩家看不见方块"的兜底。
    const SdfGlyph* findOrFallback(uint32_t cp) const;
    float lineHeight() const { return ascent + descent + lineGap; }
};

// 读 JSON（不碰 GPU）。失败返回 false，原因写进 err（调用方负责 WARN）。
bool loadSdfFontJson(const std::string& jsonPath, SdfFont& out, std::string& err);

// UTF-8：解出一个码点并前进 `p`。非法序列按 1 字节前进、返回 0xFFFD
// （**不能**跳过整段 —— 否则一个坏字节会让后面整行字全丢）。
uint32_t utf8Next(const char*& p, const char* end);

// 单行文本宽度（输出像素）。遇 '\n' 停下（多行宽度另算）。
float measureUtf8(const SdfFont& font, const char* text, float scale);

// 定位 ui2 的资产（相对 `assets/ui` 的路径，如 "fonts/noto_sc_sdf.json"）。
// 顺序：MYVK_UI_ASSET_DIR → exeDir/assets/ui → cwd/assets/ui → repo/assets/ui
std::string resolveUiAsset(const std::string& rel);

}  // namespace ui2
