#include "LauncherSpawn.h"

#include "core/Logger.h"

#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace launcher {

namespace {

#if defined(_WIN32)

std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                        static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                          &w[0], n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(),
                                        static_cast<int>(w.size()), nullptr, 0,
                                        nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s(static_cast<size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                          &s[0], n, nullptr, nullptr);
    return s;
}

// 大小写不敏感的 ASCII 比较（环境变量名在 Windows 上不分大小写）。
bool iequalsW(const wchar_t* a, const wchar_t* b) {
    for (; *a && *b; ++a, ++b) {
        wchar_t ca = *a, cb = *b;
        if (ca >= L'A' && ca <= L'Z') ca = static_cast<wchar_t>(ca - L'A' + L'a');
        if (cb >= L'A' && cb <= L'Z') cb = static_cast<wchar_t>(cb - L'A' + L'a');
        if (ca != cb) return false;
    }
    return *a == L'\0' && *b == L'\0';
}

// 键名是否等于 "NAME"（"NAME=value" 的前缀形式）。
bool keyIs(const std::wstring& entry, const wchar_t* name) {
    const size_t n = std::wcslen(name);
    if (entry.size() < n || entry[n] != L'=') return false;
    return iequalsW(entry.substr(0, n).c_str(), name);
}

// 把一条覆盖写进列表（同名替换，不是追加 —— 追加会让子进程拿到两份，
// 取哪一份取决于 CRT 的实现，属于不可依赖的行为）。
void putEnv(std::vector<std::wstring>& list, const wchar_t* key,
            const std::wstring& value) {
    for (auto& e : list) {
        if (keyIs(e, key)) {
            e = std::wstring(key) + L"=" + value;
            return;
        }
    }
    list.push_back(std::wstring(key) + L"=" + value);
}

#  ifndef CREATE_UNICODE_ENVIRONMENT
#    define CREATE_UNICODE_ENVIRONMENT 0x00000400
#  endif

#endif // _WIN32

} // namespace

std::string exeDir() {
#if defined(_WIN32)
    wchar_t buf[MAX_PATH * 4];
    const DWORD n = ::GetModuleFileNameW(nullptr, buf,
                                         static_cast<DWORD>(sizeof(buf) /
                                                            sizeof(buf[0])));
    if (n == 0 || n >= sizeof(buf) / sizeof(buf[0])) return std::string();
    std::wstring full(buf, n);
    const size_t slash = full.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return std::string();
    std::string dir = narrow(full.substr(0, slash));
    for (char& c : dir)
        if (c == '\\') c = '/';
    return dir;
#else
    return std::string();
#endif
}

std::string siblingExe(const char* baseName) {
    const std::string d = exeDir();
    if (d.empty()) return std::string();
    return d + "/" + baseName;
}

bool fileExists(const std::string& path) {
    if (path.empty()) return false;
#if defined(_WIN32)
    const std::wstring w = widen(path);
    if (w.empty()) return false;
    const DWORD a = ::GetFileAttributesW(w.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
#else
    return false;
#endif
}

bool dirExists(const std::string& path) {
    if (path.empty()) return false;
#if defined(_WIN32)
    const std::wstring w = widen(path);
    if (w.empty()) return false;
    const DWORD a = ::GetFileAttributesW(w.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    return false;
#endif
}

SpawnPlan plan(const SpawnTarget& t) {
    SpawnPlan p;
    p.commandLine = "\"" + t.exe + "\"";
    p.workingDir = t.projectRoot;
    if (!t.assetDir.empty()) p.envOverrides.push_back("MYVK_ASSET_DIR=" + t.assetDir);
    if (!t.scene.empty()) p.envOverrides.push_back("MYVK_EDITOR_SCENE=" + t.scene);
    if (!t.projectFile.empty())
        p.envOverrides.push_back("MYVK_EDITOR_PROJECT=" + t.projectFile);
    // 子进程知道自己是被启动器拉起来的（将来做"启动器常驻"时的判据）。
    p.envOverrides.push_back("MYVK_LAUNCHER_CHILD=1");
    return p;
}

SpawnResult spawn(const SpawnTarget& t) {
    SpawnResult r;

    // ---- 前置校验（每一条都给**完整路径**，不要只说"找不到 exe"）----
    if (t.exe.empty()) {
        r.error = "Engine executable path is empty.";
        return r;
    }
    if (!fileExists(t.exe)) {
        r.error = "Engine executable not found: " + t.exe;
        return r;
    }
    if (!dirExists(t.projectRoot)) {
        r.error = "Project folder not found: " + t.projectRoot;
        return r;
    }

#if !defined(_WIN32)
    r.error = "Process spawning is only implemented on Windows.";
    return r;
#else
    // ---- 环境块：读自己的一份拷贝，只在拷贝上覆盖 ----
    std::vector<std::wstring> env;
    if (LPWCH block = ::GetEnvironmentStringsW()) {
        for (LPWCH p = block; *p;) {
            const size_t len = std::wcslen(p);
            if (len > 0) env.emplace_back(p, len);
            p += len + 1;
        }
        ::FreeEnvironmentStringsW(block);
    }
    const SpawnPlan pl = plan(t);
    for (const std::string& kv : pl.envOverrides) {
        const size_t eq = kv.find('=');
        if (eq == std::string::npos) continue;
        const std::wstring k = widen(kv.substr(0, eq));
        const std::wstring v = widen(kv.substr(eq + 1));
        putEnv(env, k.c_str(), v);
    }

    // 块必须以**双 NUL** 结束。漏掉第二个 NUL 的表现是子进程环境被截断
    // 到最后一条 —— 通常看不出来，直到某条依赖 PATH 的调用失败。
    std::wstring envBlock;
    for (const std::wstring& e : env) {
        envBlock += e;
        envBlock.push_back(L'\0');
    }
    envBlock.push_back(L'\0');

    std::wstring cmd = widen(pl.commandLine);
    std::wstring cwd = widen(t.projectRoot);
    std::wstring exeW = widen(t.exe);
    if (cmd.empty() || exeW.empty()) {
        r.error = "Path contains characters that cannot be encoded as UTF-16: " +
                  t.exe;
        return r;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);

    // ---- 把 stdout / stderr 交给子进程 ----
    //
    // 为什么要显式做：`Logger` **只写 stdout**（没有文件日志）。启动器拉起的
    // 编辑器如果拿不到有效的 stdout，它的整条日志就**静默消失** —— 表现是
    // "编辑器开了，但日志里一个字都没有"，而那时你已经没有别的诊断手段了。
    //
    // 为什么用 `DuplicateHandle(..., TRUE)` 而不是直接传 `GetStdHandle`：
    // 父进程的 std 句柄**不一定**带 `HANDLE_FLAG_INHERIT`（取决于它是被
    // 谁、怎么创建的：cmd 重定向 / 管道 / 父进程是 Python 的 Popen）。
    // 不处理的后果是"有时能拿到日志、有时拿不到"，属于最难查的一类。
    //
    // ⚠ `bInheritHandles` 必须给 TRUE，否则上面这两条白设。
    // ⚠ 副作用：启动器退出后**管道仍然被编辑器握着**，所以
    //   `communicate()` 会一直等到编辑器也退出才返回 EOF。这是有意的 ——
    //   验收脚本要的就是"一个流里同时看到启动器和编辑器的日志"。
    HANDLE hOut = ::GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE hErr = ::GetStdHandle(STD_ERROR_HANDLE);
    HANDLE ownOut = nullptr, ownErr = nullptr;
    if (hOut && hOut != INVALID_HANDLE_VALUE) {
        ::DuplicateHandle(::GetCurrentProcess(), hOut, ::GetCurrentProcess(),
                          &ownOut, 0, TRUE, DUPLICATE_SAME_ACCESS);
    }
    if (hErr && hErr != INVALID_HANDLE_VALUE) {
        ::DuplicateHandle(::GetCurrentProcess(), hErr, ::GetCurrentProcess(),
                          &ownErr, 0, TRUE, DUPLICATE_SAME_ACCESS);
    }
    if (ownOut || ownErr) {
        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = ownOut ? ownOut : hOut;
        si.hStdError = ownErr ? ownErr : hErr;
    } else {
        VK_LOG_WARN("LAUNCH-SPAWN no usable std handles: child logs will be lost");
    }
    const BOOL inherit = (ownOut || ownErr) ? TRUE : FALSE;

    PROCESS_INFORMATION pi{};

    // CREATE_UNICODE_ENVIRONMENT 必须给：否则 Windows 把这块当 ANSI 解释，
    // 非 ASCII 路径直接变成乱码 —— 而中文 Windows 上的用户目录几乎必然
    // 含中文（`C:\Users\张三\...`）。
    const BOOL ok = ::CreateProcessW(
        exeW.c_str(), &cmd[0], nullptr, nullptr, inherit,
        CREATE_UNICODE_ENVIRONMENT, &envBlock[0], cwd.c_str(), &si, &pi);
    if (ownOut) ::CloseHandle(ownOut);
    if (ownErr) ::CloseHandle(ownErr);
    if (!ok) {
        const DWORD e = ::GetLastError();
        char buf[512];
        switch (e) {
            case ERROR_FILE_NOT_FOUND:
                std::snprintf(buf, sizeof(buf),
                              "Engine executable not found: %s", t.exe.c_str());
                break;
            case ERROR_ACCESS_DENIED:
                std::snprintf(buf, sizeof(buf),
                              "Access denied (is the program blocked by "
                              "antivirus or policy?): %s", t.exe.c_str());
                break;
            case ERROR_DIRECTORY:
                std::snprintf(buf, sizeof(buf),
                              "Working directory is not a folder: %s",
                              t.projectRoot.c_str());
                break;
            default:
                std::snprintf(buf, sizeof(buf),
                              "CreateProcess failed (error %lu) for %s",
                              static_cast<unsigned long>(e), t.exe.c_str());
                break;
        }
        r.error = buf;
        return r;
    }

    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);

    r.ok = true;
    r.pid = static_cast<unsigned long>(pi.dwProcessId);
    VK_LOG_INFO("LAUNCH-SPAWN ok pid=%lu exe=%s cwd=%s", r.pid, t.exe.c_str(),
                t.projectRoot.c_str());
    for (const std::string& kv : pl.envOverrides)
        VK_LOG_INFO("LAUNCH-SPAWN env %s", kv.c_str());
    return r;
#endif
}

} // namespace launcher
