#include "ui2/SdfFont.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace ui2 {

namespace fs = std::filesystem;

const SdfGlyph* SdfFont::find(uint32_t cp) const {
    auto it = glyphs.find(cp);
    return it == glyphs.end() ? nullptr : &it->second;
}

const SdfGlyph* SdfFont::findOrFallback(uint32_t cp) const {
    if (const SdfGlyph* g = find(cp)) return g;
    return find(0x3F);   // '?'
}

bool loadSdfFontJson(const std::string& jsonPath, SdfFont& out, std::string& err) {
    std::ifstream f(jsonPath, std::ios::binary);
    if (!f) {
        err = "打不开 " + jsonPath;
        return false;
    }
    const std::string text((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());

    // 用 allow_exceptions=false：图集是**生成的产物**，损坏时我们要一条可读的
    // WARN，而不是一个把整个 Renderer::init 掀翻的异常。
    nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        err = "JSON 解析失败 " + jsonPath;
        return false;
    }

    SdfFont& fo = out;
    fo = SdfFont{};
    fo.face = j.value("font", std::string("?"));
    fo.license = j.value("license", std::string(""));
    fo.size = j.value("size", 0.0f);
    fo.spread = j.value("sdfRange", 0.0f);
    if (j.contains("atlas")) {
        fo.atlasW = j["atlas"].value("w", 0u);
        fo.atlasH = j["atlas"].value("h", 0u);
    }
    if (j.contains("metrics")) {
        fo.ascent = j["metrics"].value("ascent", 0.0f);
        fo.descent = j["metrics"].value("descent", 0.0f);
        fo.lineGap = j["metrics"].value("lineGap", 0.0f);
    }
    if (fo.atlasW == 0 || fo.atlasH == 0) {
        err = "图集尺寸为 0（JSON 缺 atlas 字段）";
        return false;
    }
    if (!j.contains("glyphs") || !j["glyphs"].is_object()) {
        err = "JSON 缺 glyphs 表";
        return false;
    }

    const float aw = static_cast<float>(fo.atlasW);
    const float ah = static_cast<float>(fo.atlasH);
    fo.glyphs.reserve(j["glyphs"].size());
    for (auto it = j["glyphs"].begin(); it != j["glyphs"].end(); ++it) {
        // 键是 %04X 的码点。**别用 stoul 的异常**做错误处理 —— 一个坏键
        // 不该让整张图集作废，跳过它并继续。
        uint32_t cp = 0;
        try {
            cp = static_cast<uint32_t>(std::stoul(it.key(), nullptr, 16));
        } catch (...) {
            continue;
        }
        const nlohmann::json& g = it.value();
        if (!g.contains("uv") || !g["uv"].is_array() || g["uv"].size() < 4) continue;
        const float x = g["uv"][0].get<float>();
        const float y = g["uv"][1].get<float>();
        const float w = g["uv"][2].get<float>();
        const float h = g["uv"][3].get<float>();

        SdfGlyph gl;
        gl.u0 = x / aw;
        gl.v0 = y / ah;
        gl.u1 = (x + w) / aw;
        gl.v1 = (y + h) / ah;
        gl.w = w;
        gl.h = h;
        gl.adv = g.value("adv", w);
        if (g.contains("bear") && g["bear"].is_array() && g["bear"].size() >= 2) {
            gl.bearX = g["bear"][0].get<float>();
            gl.bearY = g["bear"][1].get<float>();
        }
        fo.glyphs.emplace(cp, gl);
    }

    if (fo.glyphs.empty()) {
        err = "glyphs 表为空";
        return false;
    }
    return true;
}

uint32_t utf8Next(const char*& p, const char* end) {
    if (p >= end) return 0;
    const unsigned char b0 = static_cast<unsigned char>(p[0]);
    if (b0 < 0x80) { ++p; return b0; }

    int len = 0;
    uint32_t cp = 0;
    if ((b0 & 0xE0) == 0xC0)      { len = 2; cp = b0 & 0x1Fu; }
    else if ((b0 & 0xF0) == 0xE0) { len = 3; cp = b0 & 0x0Fu; }
    else if ((b0 & 0xF8) == 0xF0) { len = 4; cp = b0 & 0x07u; }
    else { ++p; return 0xFFFD; }

    // ⚠ 必须先判越界再读：文本末尾截断的多字节序列在读第 len 个字节时
    //    就会跑出缓冲区（这是最经典的 UTF-8 崩溃点）。
    if (p + len > end) { ++p; return 0xFFFD; }
    for (int i = 1; i < len; ++i) {
        const unsigned char b = static_cast<unsigned char>(p[i]);
        if ((b & 0xC0) != 0x80) { ++p; return 0xFFFD; }
        cp = (cp << 6) | (b & 0x3Fu);
    }
    p += len;
    return cp;
}

float measureUtf8(const SdfFont& font, const char* text, float scale) {
    if (!text || !font.valid()) return 0.0f;
    const char* p = text;
    const char* end = text + std::strlen(text);
    float w = 0.0f;
    while (p < end) {
        const uint32_t cp = utf8Next(p, end);
        if (cp == '\n') break;   // 单行语义：多行由调用方逐行量
        if (const SdfGlyph* g = font.findOrFallback(cp)) w += g->adv * scale;
    }
    return w;
}

std::string resolveUiAsset(const std::string& rel) {
    std::vector<std::string> cands;
    if (const char* env = std::getenv("MYVK_UI_ASSET_DIR")) {
        cands.push_back(std::string(env) + "/" + rel);
    }
    char buf[1024] = {};
#if defined(_WIN32)
    GetModuleFileNameA(nullptr, buf, sizeof(buf));
#else
    {
        const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n > 0) buf[n] = '\0';
    }
#endif
    if (buf[0]) {
        cands.push_back((fs::path(buf).parent_path() / "assets" / "ui" / rel).string());
    }
    cands.push_back("assets/ui/" + rel);
    cands.push_back((fs::path("..") / "assets" / "ui" / rel).string());
    // 从 build/bin 跑（自检 / 回归都是）时，仓库根在上两级。
    // 这条只是**开发期兜底**：正式包一定走 exeDir 那条。
    cands.push_back((fs::path("..") / ".." / "assets" / "ui" / rel).string());

    for (const auto& c : cands) {
        std::error_code ec;
        if (fs::exists(c, ec) && !ec) return c;
    }
    return cands.front();   // 让读取处报出可读的路径
}

}  // namespace ui2
