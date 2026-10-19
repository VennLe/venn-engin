#include "LauncherDialog.h"

#include "core/Logger.h"

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <shobjidl.h>
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

std::string narrow(const wchar_t* w) {
    if (!w || !w[0]) return std::string();
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr,
                                        nullptr);
    if (n <= 1) return std::string();
    std::string s(static_cast<size_t>(n - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

std::string toSlashes(std::string p) {
    for (char& c : p)
        if (c == '\\') c = '/';
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    return p;
}

// COM 的成对初始化。返回 true 表示"这个函数负责 Uninit"。
bool comEnter() {
    const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (hr == S_OK || hr == S_FALSE) return true;
    if (hr == RPC_E_CHANGED_MODE) {
        // 上层已用 MTA 初始化过 —— COM 可用，只是不能重复 Init。
        VK_LOG_INFO("LAUNCHER-DIALOG com already initialized (MTA), reusing");
        return false;
    }
    VK_LOG_WARN("LAUNCHER-DIALOG CoInitializeEx failed hr=0x%08lX",
                static_cast<unsigned long>(hr));
    return false;
}

std::string runDialog(const std::string& title, const std::string& initialDir,
                      bool pickFolders) {
    const bool ownsCom = comEnter();

    std::string result;
    IFileOpenDialog* dlg = nullptr;
    HRESULT hr = ::CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                   CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg));
    if (FAILED(hr) || !dlg) {
        VK_LOG_WARN("LAUNCHER-DIALOG create failed hr=0x%08lX",
                    static_cast<unsigned long>(hr));
        if (ownsCom) ::CoUninitialize();
        return result;
    }

    DWORD opts = 0;
    if (SUCCEEDED(dlg->GetOptions(&opts))) {
        opts |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
        if (pickFolders) opts |= FOS_PICKFOLDERS;
        else opts |= FOS_FILEMUSTEXIST;
        dlg->SetOptions(opts);
    }
    if (!title.empty()) dlg->SetTitle(widen(title).c_str());

    if (!initialDir.empty()) {
        IShellItem* folder = nullptr;
        if (SUCCEEDED(::SHCreateItemFromParsingName(widen(initialDir).c_str(),
                                                    nullptr, IID_PPV_ARGS(&folder))) &&
            folder) {
            dlg->SetFolder(folder);
            folder->Release();
        }
    }

    if (pickFolders) {
        // 文件夹模式默认按钮文案是"选择文件夹"；显式设一次，避免不同
        // Windows 版本上按钮读作"打开"（用户会以为选的是里面的某个文件）。
        dlg->SetOkButtonLabel(L"Select Folder");
    }

    hr = dlg->Show(nullptr);
    if (SUCCEEDED(hr)) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)) && item) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                result = toSlashes(narrow(path));
                ::CoTaskMemFree(path);
            }
            item->Release();
        }
    } else if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        // 取消不是错误，不打 WARN（日志里"用户按了取消"不该长得像故障）。
    } else {
        VK_LOG_WARN("LAUNCHER-DIALOG show failed hr=0x%08lX",
                    static_cast<unsigned long>(hr));
    }

    dlg->Release();
    if (ownsCom) ::CoUninitialize();
    return result;
}

#endif // _WIN32

} // namespace

std::string pickFolder(const std::string& title, const std::string& initialDir) {
#if defined(_WIN32)
    return runDialog(title, initialDir, true);
#else
    (void)title;
    (void)initialDir;
    return std::string();
#endif
}

std::string pickProjectFile(const std::string& title,
                            const std::string& initialDir) {
#if defined(_WIN32)
    // 先选目录再拼文件名：`FOS_PICKFOLDERS` 与"限定文件名"不能同时用，
    // 而"打开工程"实际要的是目录（工程根）。这里复用文件夹选择器，
    // 拿到目录后再由调用方 `loadProject` 判断是不是工程 —— 这样"选错目录"
    // 会得到一句人话，而不是一个打不开的文件对话框。
    return runDialog(title, initialDir, true);
#else
    (void)title;
    (void)initialDir;
    return std::string();
#endif
}

} // namespace launcher
