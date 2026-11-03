#pragma once
// ============================================================
// core/Utf8 —— 工程里唯一的 UTF-8 ↔ UTF-16 转换（Win32 边界用）
//
// ⚠ 现状说明（诚实）：`project/ProjectCreate.cpp` 与 `scene/SaveGame.cpp`
//   里各自有一份同功能的**本地**实现。它们先于本头存在、且被 60+ 条判据
//   盖着；本次**不动**它们（把已稳定的实现换个地方调用，收益是"少两份
//   样板"，风险是"多一次横切回归"）。新代码一律用这里。
//   将来若真要统一，一次一刀、单独跑全量。
//
// ⚠ 只在 Win32 API 边界用（`LoadLibraryW` / `GetFileAttributesW` /
//   `CreateProcessW`）。工程内部的路径字符串一律 UTF-8 + 正斜杠。
// ============================================================

#include <string>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace core {

#if defined(_WIN32)
// UTF-8 → UTF-16（失败返回空串，调用方可据此判"路径里有非法字节"）。
inline std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int need = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (need <= 0) return std::wstring();
    std::wstring w(static_cast<size_t>(need), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], need);
    if (!w.empty() && w.back() == L'\0') w.pop_back();   // 去掉结尾 NUL
    return w;
}

// UTF-16 → UTF-8（Win32 返回值用，比如 GetLastError 的格式化消息）。
inline std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int need = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0,
                                           nullptr, nullptr);
    if (need <= 0) return std::string();
    std::string s(static_cast<size_t>(need), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], need, nullptr, nullptr);
    if (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}
#endif

} // namespace core
