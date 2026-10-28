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
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan.h>

namespace editor {

class ContentBrowser {
public:
    explicit ContentBrowser(EditorContext& ctx) : m_ctx(ctx) {}
    ~ContentBrowser() = default;

    void draw();

    // 抽屉形态：调用方已经 Begin 了一个无标题栏窗口（头部行由 EditorApp 画），
    // 这里只画导航 + 工具行 + 内容区。见 ContentBrowser.cpp 的注释。
    void drawBody();

    // 底栏统计用（"7 entries | assets"）。抽屉关着时也能取到 —— 内部按需
    // 刷新（dirty 才列目录）。
    std::size_t entryCount();
    const std::string& currentRel() const { return m_rel; }

    // 从别处（File ▸ Import 菜单）触发**同一条**导入通道。
    // 只置位、不开窗：对话框必须等本帧 ImGui 数据提交完再弹（见 .cpp 里
    // "导入对话框放在最后"那段注释）。⚠ 调用方有责任保证本面板这一帧
    // 会被 draw —— 被收进底栏托盘时 draw 不跑，置位会一直挂着。
    void requestImportFileDialog();

private:
    struct DirEntry {
        std::string name;
        std::string absPath;
        bool isDir = false;
        std::uintmax_t size = 0;
    };

    // ---- E13 视图 / 尺寸 / 类型过滤 ----
    enum class ViewMode { Grid, List };
    enum class ThumbSize { Small, Medium, Large, Huge };
    // 类型过滤：按扩展名映射（Material 认 .mat.json / .vmat；Scene 认 .json）
    // ⚠ 只能**往后加**（枚举值 = 单选列下标 = MYVK_EDITOR_CB_TYPE /
    //   CB-FILTER 日志里那个数字，脚本按下标取值）。W4-4 追加了 Terrain。
    enum class TypeFilter {
        All, Model, Texture, Material, Scene, Script, Shader, Folder, Terrain
    };
    // 排序字段 / 方向（2026-10-17：抽屉三栏改造时加的，见 drawSortMenu）
    enum class SortBy { Name, Size };

    // Add ▸ 子菜单的一项（13 种资源类型，顺序即菜单顺序、也是脚本按下标
    // 取矩形时依赖的顺序 —— 只能往后加）。
    struct AddKind {
        const char* key;    // 日志 / 文件名里用的 ASCII 键（不进译文表）
        const char* label;  // 英文原文（过 TR()）
        const char* ext;    // 落盘扩展名（不含点）
    };
    static const AddKind* addKinds(int& count);

    void refresh();

    // ---- 三栏（目录树 | 类型过滤 | 内容）----
    // 2026-10-17 用户需求：把 Content 拆成左中右三栏。
    void drawTreePane(float w);    // 左：项目目录树（根 = 项目内容目录）
    void drawTypePane(float w);    // 中：资源类型单选竖列
    void drawItemsPane(float w);   // 右：当前目录内容 + 搜索/排序
    void drawNavRow();             // 左栏头部四个图标按钮（↑ ⌂ ⟳ +）
    void drawSearchRow();          // 右栏顶部：搜索框 + 历史 + 排序图标 + 视图
    void drawSortMenu();           // 排序图标弹出的菜单内容（在 popup 里调）

    // ---- 右栏最上面那一行动作条（2026-10-17 晚 用户需求 4）----
    //   左端："Add"（图标+文字，点开一个**向上**弹的子菜单）
    //   中间："Save All" + 上一层 / 下一层（历史导航）
    //   右端：路径框（相对项目根目录的相对路径，只读、可选中复制）
    void drawActionRow();
    void drawAddMenu();            // Add 按钮上方弹出的子菜单（popup 里调）
    // Add ▸ 各类资源。2026-10-17 阶段：**落一个最小的资产文件**
    // （{"type":...,"version":1} 骨架）并立刻进入重命名 —— 和 New Folder
    // 的手感一致。真正的加载器还没写，所以文件暂时只是"占位声明"。
    void createAsset(const char* kindKey, const char* kindLabel,
                     const char* ext);
    void saveAll();                // 置位 EditorContext 的"保存所有"请求
    // 导入：把系统文件对话框选中的文件**拷贝**进当前目录。
    // ⚠ 对话框是模态阻塞的，必须在**帧末**调（见 drawBody 的收尾块）——
    //   在 ImGui 遍历途中卡住会让本帧的顶点缓冲对不上。
    void importFiles();
    void openImportDialog();       // Win32 GetOpenFileNameW（非 Windows 走提示）
    void drawDirNode(const std::string& rel, const std::string& label,
                     int depth);
    // 某个相对目录下的子目录名（带缓存 —— 每帧递归扫盘会拖慢 UI）
    std::vector<std::string> subDirs(const std::string& rel);
    void pushRecentSearch(std::string q);

    void drawGrid();
    void drawList();
    void drawEntry(const DirEntry& ent, float cellW, float cellH);
    void drawListRow(const DirEntry& ent, float rowH);

    // 当前类型过滤是否命中该条目
    bool entryMatchesType(const DirEntry& ent) const;

    // 取缩略图（缓存命中/失败都在内部处理，返回 nullptr = 画不了）
    const ThumbnailCache::Entry* thumbnail(const std::string& absPath);

    // ---- 文件系统操作（都做完整错误处理 + 刷新 + 提示）----
    void navigateTo(const std::string& rel);
    void goUp();
    // E13 历史栈：←/→ 在访问过的目录间往返
    void back();
    void forward();
    void pushHistory(const std::string& rel);
    bool canBack() const;
    bool canForward() const;

    void openEntry(const DirEntry& ent);          // 等同双击
    void beginRename(const std::string& absPath, const std::string& name);
    void commitRename();
    void duplicateEntry(const DirEntry& ent);
    void requestDelete(const DirEntry& ent);
    void doDelete();
    void createFolder();
    // E13 New ▸ Material / Scene / Script
    void createScene();
    void createScript();
    // W4-4：Add ▸ Terrain —— 在 dir 下落一份**能直接打开**的平地三件套
    // （`<dir>/New Terrain/{terrain.vterrain,height.r32,splat.rgba8}`）。
    // 与其它 Add 项不同，它建的是**目录**而不是单文件：地形资产天生三件套，
    // 拆开放就没有意义（`.vterrain` 里的 height/splat 是相对它自己解析的）。
    void createTerrainAsset(const std::string& dir);
    // E13 Show References：扫描资产根下引用该文件名的文本资产（简版）
    void showReferences(const DirEntry& ent);
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

    // ---- E13 视图状态 ----
    ViewMode m_view = ViewMode::Grid;
    ThumbSize m_thumb = ThumbSize::Medium;
    TypeFilter m_typeFilter = TypeFilter::All;
    int m_typeFilterIdx = 0;   // 单选列 / Combo 索引（与 TypeFilter 一一对应）
    int m_thumbIdx = 1;        // 尺寸档索引（0=Small .. 3=Huge）
    float m_cellW = 96.0f;     // 当前网格格子尺寸（drawGrid 每帧填，drawEntry 用）
    float m_cellH = 104.0f;

    // ---- 排序（右栏排序图标的菜单）----
    // 目录永远排在文件前面 —— 这一条与排序字段无关（和资源管理器一致）。
    SortBy m_sortBy = SortBy::Name;
    bool m_sortAsc = true;

    // ---- 最近搜索（搜索框历史）----
    // 只在**回车**或从历史里选时记录：边打边过滤很常见，但每敲一个字就
    // 存一条历史会把列表刷满没用的前缀。
    std::vector<std::string> m_recentSearches;
    // 左栏目录树的子目录缓存：相对目录 → 子目录名（按名字排好）。
    // refresh() 时整个清空（文件系统可能已经变了）。
    std::unordered_map<std::string, std::vector<std::string>> m_dirCache;
    // 目录树的根（相对 m_root）。有 assets/ 就是 "assets" —— "当前游戏项目
    // 的内容目录"；没有则退化成 m_root 本身。
    std::string m_contentRootRel;
    // 历史栈：m_histPos 指向当前目录；back/forward 只移动游标，不截断
    std::vector<std::string> m_history;
    int m_histPos = -1;
    bool m_histInit = false;

    std::vector<DirEntry> m_entries;
    bool m_dirty = true;
    // 首次 refresh 时把初始目录定到 assets/（只做一次）
    bool m_initializedOnce = false;

    // 选中项（绝对路径）。单击选中，右键菜单默认作用于它。
    std::string m_selected;
    // G14-8：正在试听的素材（**绝对路径**；空 = 没在试听）。
    // ⚠ 这是 Content 面板自己的界面状态，**不是**音频系统的状态 ——
    //   设备打不开时这里照样会进"播放态"，因为面板拿不到播放结果。
    //   真实失败由 AudioSystem 打 `AUDIO-CLIP-FAIL`；本面板在发请求前也会
    //   先做一次存在性检查（不存在就当场通知，不进播放态）。
    std::string m_previewPath;

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

    // ---- 动作条（Add / Save All / 上一层·下一层 / 路径框）----
    // Add 按钮的矩形：子菜单要**贴着它往上**弹，所以每帧记一份
    // （不使用 ImGui 的默认"鼠标下方"定位 —— 抽屉在屏幕最底下，往下弹
    //  会直接出屏幕）。
    float m_addBtnX0 = 0.0f;
    float m_addBtnY0 = 0.0f;
    float m_addBtnX1 = 0.0f;
    float m_addBtnY1 = 0.0f;
    // 导入对话框：点击只置位，真正的阻塞调用在 drawBody 帧末执行
    bool m_importRequest = false;
    // 路径框的显示缓冲（只读 InputText 需要一个可写缓冲；**不能**复用
    // m_pathBuf —— 那是搜索框的，混用会让搜索内容被路径覆盖）
    char m_pathDisplay[512] = {0};
    // 路径变化才打 CB-PATH 日志（每帧打会把日志刷爆）
    std::string m_lastPathLogged;

    // 缩略图缓存：绝对路径 → ImGui 纹理句柄（共享实现，见 ThumbnailCache.h）
    ThumbnailCache m_thumbs;

    char m_pathBuf[512] = {0};
};

} // namespace editor
