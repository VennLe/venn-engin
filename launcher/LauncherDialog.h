#pragma once
// ============================================================
// launcher/LauncherDialog —— 系统文件夹选择器（IFileDialog + FOS_PICKFOLDERS）
//
// 为什么不用 `SHBrowseForFolderW`：那个是老式树形对话框，Windows Vista 之后
// 系统自己都不再用了；而且它拿到的路径要自己 `SHGetPathFromIDListW` 转，
// 遇到 OneDrive / 库路径会返回空。
//
// 为什么必须显式 `CoInitializeEx`：GLFW 不初始化 COM，而
// `CoCreateInstance(CLSID_FileOpenDialog)` 在没有 apartment 的线程上直接
// 返回 `CO_E_NOTINITIALIZED` —— 表现是"点了浏览… 什么都没发生"。
// 这里按"调用前后成对 Init/Uninit"处理，**不**假设上层已经初始化过：
// 若返回 `RPC_E_CHANGED_MODE`（上层已用另一种 apartment 初始化过 COM），
// 就照常继续用 —— 那种情况下 COM 是可用的，只是不能重复 Init。
// ============================================================

#include <string>

namespace launcher {

// 选一个**文件夹**。返回正斜杠、无尾斜杠的绝对路径；用户取消返回空串。
// `initialDir` 为空时用系统默认位置。
std::string pickFolder(const std::string& title, const std::string& initialDir);

// 选一个 `venn.project.json` 文件（"打开已有工程"的两条路之一：
// 选文件，或者选文件夹 —— 两条都要支持，因为用户手里可能只有目录）。
// 取消返回空串。
std::string pickProjectFile(const std::string& title,
                            const std::string& initialDir);

} // namespace launcher
