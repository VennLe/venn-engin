#pragma once
// ============================================================
// editor/ContentBrowser —— 资源浏览器（文件系统 + 缩略图）
//
// 四件事：
//   1. 用 std::filesystem 列目录（引擎本来就没有虚拟文件系统，
//      直接用标准库最省事，也不需要额外依赖）
//   2. 图片文件生成缩略图：走 AssetManager 上传一张 GPU 纹理，再用
//      ImGui_ImplVulkan_AddTexture 注册成 ImGui 能用的句柄。
//      —— 这里复用了引擎自己的资源缓存，缩略图和场景里用的是**同一张**
//      GPU 纹理，编辑器里加载过的图，拖进场景后不会再上传一次。
//   3. 拖拽：模型（.gltf/.glb/.obj）拖到视口里实例化；
//      脚本（.vks）双击挂给当前选中的实体。
//   4. 文件管理：返回上一级 / 面包屑跳转 / 右键菜单（打开、重命名、
//      复制、删除、新建文件夹、在资源管理器中显示、复制路径）。
//
// 图标全部用 ImDrawList **矢量绘制**（不是字体字形，也不是 "DIR"/"IMG"
// 那种文字色块）—— 字体里没有这些字形，而矢量在任何 DPI / 缩放下都清晰。
//
// 破坏性操作（删除）会先弹一次确认框，并且列出完整路径；
// 删除后当前目录会立刻刷新，缩略图缓存里该子树一并失效。
//
// 缩略图句柄在析构时统一注销（必须在 ImGui 后端 shutdown 之前 ——
// EditorApp 的成员先于 Application 里的 Renderer 释放，这个顺序天然成立）。
// 这部分逻辑已经抽到 editor/ThumbnailCache，InspectorPanel 的材质纹理槽
// 跟这里共用同一份实现。
// ============================================================

#include "EditorContext.h"
#include "PickingSystem.h"   // m_picking：双击模型时算包围盒做"贴地"
#include "ThumbnailCache.h"

#include <cstdint>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

namespace editor {

class ContentBrowser {
public:
    explicit ContentBrowser(EditorContext& ctx) : m_ctx(ctx) {}
    ~ContentBrowser() = default;

    void draw();

private:
    struct DirEntry {
        std::string name;
        std::string absPath;
        bool isDir = false;
        std::uintmax_t size = 0;
    };

    void refresh();

    // ---- 三段式 UI：导航行 / 搜索行 / 网格 ----
    void drawNavRow();
    void drawSearchRow();
    void drawGrid();
    void drawEntry(const DirEntry& ent, float cellW, float cellH);

    // 取缩略图（缓存命中/失败都在内部处理，返回 nullptr = 画不了）
    const ThumbnailCache::Entry* thumbnail(const std::string& absPath);

    // ---- 文件系统操作（都做完整错误处理 + 刷新 + 提示）----
    void navigateTo(const std::string& rel);
    void goUp();
    void openEntry(const DirEntry& ent);          // 等同双击
    void beginRename(const std::string& absPath, const std::string& name);
    void commitRename();
    void duplicateEntry(const DirEntry& ent);
    void requestDelete(const DirEntry& ent);
    void doDelete();
    void createFolder();
    void revealInExplorer(const std::string& absPath);

    void invalidateThumb(const std::string& absPath);
    void invalidateThumbTree(const std::string& absPath);
    std::string uniqueChildPath(const std::string& parent,
                                const std::string& stem,
                                const std::string& ext) const;

    void drawEntryContextMenu();
    void drawEmptyContextMenu();
    void drawDeleteConfirm();

    EditorContext& m_ctx;

    // 只为了 meshAabb / worldAabb：双击模型放进场景时要按包围盒把底部
    // 贴到栅格面上。缩略图那条路用不到它。
    PickingSystem m_picking;

    std::string m_root;    // 浏览根（一般是进程工作目录）
    std::string m_rel;     // 相对根的当前目录
    std::string m_filter;  // 名字过滤（大小写不敏感）

    std::vector<DirEntry> m_entries;
    bool m_dirty = true;
    // 首次 refresh 时把初始目录定到 assets/（只做一次）
    bool m_initializedOnce = false;

    // 选中项（绝对路径）。单击选中，右键菜单默认作用于它。
    std::string m_selected;

    // ---- 条目右键菜单 ----
    // 请求由 drawEntry()（格子）置位，真正的 OpenPopup + 绘制在 drawGrid()
    // 的格子循环之外完成 —— 菜单必须每帧无条件提交，否则 ImGui 会在
    // 没被提交的那一帧直接关掉它（菜单盖住光标 → 格子不再 hovered →
    // 若绘制被 hovered 守着，菜单就只闪一帧）。
    DirEntry m_ctxEntry{};        // 目标快照（不要存引用/下标，refresh 会重建 m_entries）
    bool m_ctxRequest = false;    // 本帧有人请求打开菜单
    bool m_ctxOpen = false;       // 菜单当前是否处于打开状态（BeginPopup 返回 false 时自动清）

    // ---- 重命名（画在格子里的内联输入框）----
    std::string m_renameTarget;
    char m_renameBuf[256] = {0};
    bool m_renameFocus = false;
    bool m_renameCommit = false;
    bool m_renameCancel = false;

    // ---- 删除确认（破坏性操作，先弹一次）----
    std::string m_deleteTarget;
    std::string m_deleteName;
    bool m_deleteIsDir = false;
    bool m_openDeleteConfirm = false;

    // 新建文件夹：建完立刻进入重命名
    bool m_newFolderRequest = false;

    // 缩略图缓存：绝对路径 → ImGui 纹理句柄（共享实现，见 ThumbnailCache.h）
    ThumbnailCache m_thumbs;

    char m_pathBuf[512] = {0};
};

} // namespace editor
