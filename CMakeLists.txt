cmake_minimum_required(VERSION 3.16)
# venn —— 从零手写的 Vulkan 引擎。工程里只有一个可执行文件 Editor.exe。
project(Venn VERSION 0.2.0 LANGUAGES C CXX)

# 导出 compile_commands.json（供 VS Code IntelliSense / clangd 精确解析）
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ============================================================
# 编译器设置
# ============================================================
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# ------------------------------------------------------------
# MSVC 全局开关
# ------------------------------------------------------------
# /utf-8：本工程的源文件是 UTF-8 无 BOM，注释与日志字符串里有大量中文。
#   不指定的话 MSVC 按系统 ANSI 代码页（本机是 936=GBK）解析源文件，
#   某些中文字符的 UTF-8 字节序列会在 GBK 下被切成"半个字符"，让编译器
#   以为字符串没有闭合 —— 报 C2001「常量中有换行符」、C2143/C2059 一串
#   语法错误。MinGW 默认按 UTF-8 处理，所以这个坑只有在 MSVC 下才炸。
#   /utf-8 等价于 /source-charset:utf-8 /execution-charset:utf-8。
# NOMINMAX：Windows.h 的 min/max 宏会咬 std::min/std::max（GLFW 会拉进
#   Windows.h）。MinGW 侧我们也一直假定没有这两个宏。
# WIN32_LEAN_AND_MEAN：少拉一堆用不上的 Windows 头（含 winsock1）。
if(MSVC)
    add_compile_options(/utf-8 /W3 /permissive- /Zc:__cplusplus)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    # C4819（源文件代码页无法表示）已被 /utf-8 解决；这里再显式静音，
    # 免得第三方头文件里的既有字节仍刷屏。
    add_compile_options(/wd4819)
endif()

# 输出目录：build/bin
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

find_package(Vulkan REQUIRED)

# ============================================================
# Windows 图标资源（venn 的豚鼠 logo）
# ------------------------------------------------------------
# 用 MinGW 自带的 windres 把 assets/icons/venn.rc（引用同目录的 venn.ico）
# 编进可执行文件：这样资源管理器里看到的 exe 图标、以及任务栏在程序
# 还没调用 glfwSetWindowIcon 之前用的默认图标都是对的。
# OPTIONAL：万一这个工具链找不到 windres，也只是没有 exe 图标，不该
# 让整个配置失败（运行时那条 glfwSetWindowIcon 路径仍然生效）。
# ============================================================
if(WIN32)
    enable_language(RC OPTIONAL)
    if(CMAKE_RC_COMPILER)
        set(VENN_ICON_RC ${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/venn.rc)
    endif()
endif()

# ============================================================
# 第三方依赖
# ============================================================

# ---- GLFW 3.4（子目录编译，静态库）----
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_X11 OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/glfw EXCLUDE_FROM_ALL)

# glfw 是独立项目（add_subdirectory），顶层 add_compile_options 虽会沿目录继承，
# 但它自带的 target 可能自行覆盖编译选项 —— 显式再钉一次 /utf-8，避免
# glfw 源码里（有非 ASCII 注释）在 MSVC 下报 C4819/C2001。
if(MSVC)
    target_compile_options(glfw PRIVATE /utf-8 /wd4819)
endif()

# ---- ImGui（核心 + GLFW/Vulkan 后端）----
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)
add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp
)
target_include_directories(imgui PUBLIC
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)
target_link_libraries(imgui PUBLIC glfw Vulkan::Vulkan)
if(MSVC)
    target_compile_options(imgui PRIVATE /utf-8 /wd4819)
endif()

# ---- header-only 库（interface target）----
add_library(glm INTERFACE)
target_include_directories(glm INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/glm)

add_library(stb INTERFACE)
target_include_directories(stb INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb)

add_library(tinyobjloader INTERFACE)
target_include_directories(tinyobjloader INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/tinyobjloader)

add_library(tinygltf INTERFACE)
target_include_directories(tinygltf INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/tinygltf)

# ---- ufbx（FBX 加载：ufbx.h 声明 + ufbx.c 实现，双文件库）----
# ufbx.c 是纯 C，必须作为 C 源文件单独编译（不能混进 C++ 的 GLOB）。
add_library(ufbx STATIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ufbx/ufbx.c)
target_include_directories(ufbx PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ufbx)
set_target_properties(ufbx PROPERTIES LINKER_LANGUAGE CXX)
if(MSVC)
    # ufbx.c 是纯 C，但它被当作 C++ 编译（LINKER_LANGUAGE CXX）；
    # 源码里同样有非 ASCII 字符，一并钉 /utf-8。
    target_compile_options(ufbx PRIVATE /utf-8 /wd4819)
endif()

# ---- assetio（离线资产格式：.vmesh 读写 + FBX 导入 + 几何/动画修正）----
# ------------------------------------------------------------
# 这一层的依赖方向是刻意的：
#
#     assetc/  ─┐
#                ├─► assetio ─► (glm, ufbx)
#     engine/  ─┘
#
# **assetio 不认识 Vulkan，也不认识 engine/**。原因只有一条：
# 离线编译器必须能在没有显卡、没有 Vulkan SDK 的机器上跑（构建机、CI、
# 美术机器批量转码）。只要这一层不小心 include 了 `rhi/VulkanCommon.h`，
# AssetC.exe 就会开始要求一个 Vulkan 环境 —— 而这个错误在开发机上
# **永远不会暴露**（本机装着 SDK），只在别人机器上炸。
# ============================================================
file(GLOB_RECURSE ASSETIO_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/assetio/*.cpp
)
add_library(VennAssetIO STATIC ${ASSETIO_SOURCES})
target_include_directories(VennAssetIO PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(VennAssetIO PUBLIC glm)
target_link_libraries(VennAssetIO PRIVATE ufbx)
if(NOT MSVC)
    target_compile_options(VennAssetIO PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# ---- stb_vorbis（OGG 解码：stb_vorbis.c 既是声明也是实现）----
#
# ⚠ 与 ufbx 完全同款：`third_party/stb/stb_vorbis.c` 从 2026-10-14 起就在
#   仓库里，但 `stb` 是 **INTERFACE**（纯头文件）→ 那个 .c **没有任何
#   target 编译它** → 想解 OGG 就是"文件在、符号没有"。
#   G14-1 补上这个 target；C++ 侧只 `#define STB_VORBIS_HEADER_ONLY` 后
#   include 同一个文件拿声明（stb 自带 extern "C" 保护）。
add_library(stb_vorbis STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb/stb_vorbis.c)
target_include_directories(stb_vorbis PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb)
if(MSVC)
    # 纯 C 源，但会刷几百条 /W3 警告（未使用变量 / 有符号无符号比较）。
    target_compile_options(stb_vorbis PRIVATE /utf-8 /wd4819 /W0)
endif()

# ---- nlohmann/json（单头文件；场景序列化用）----
# 头文件放在 third_party/nlohmann/json.hpp，include 目录指到 third_party，
# 这样代码里统一写 #include <nlohmann/json.hpp>
add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

add_library(vma INTERFACE)
target_include_directories(vma INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/VulkanMemoryAllocator/include)

# ============================================================
# 引擎静态库（target 名 = VennEngine）
# ------------------------------------------------------------
# G01（2026-10-17）：从"一个 Editor.exe"拆成"一个引擎库 + 两个可执行文件"。
# 依赖方向是硬约束（见 docs/ship_roadmap.md §13）：
#
#     runtime/  ──►  engine/  ◄──  editor/
#
# VennEngine **只**收 engine/**，两个消费者都链它。
# 它不认识 editor/、也不认识 runtime/ —— 这是下面那条 GLOB
# 唯一的作用域，也是"引擎库的源文件列表里不含编辑器文件"这条验收的实现。
# ============================================================
file(GLOB_RECURSE ENGINE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/engine/*.cpp)
add_library(VennEngine STATIC ${ENGINE_SOURCES})
# 旧名别名：工程里还有若干处按 `venn` 指代引擎库（历史遗留），
# 用 ALIAS 保住它们，避免"改名"变成一次大面积机械替换。
add_library(venn ALIAS VennEngine)
target_include_directories(VennEngine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
)
target_link_libraries(VennEngine PUBLIC
    Vulkan::Vulkan
    glfw
    imgui
    glm
    stb
    tinyobjloader
    tinygltf
    ufbx
    nlohmann_json
    vma
    stb_vorbis
    VennAssetIO
)
# G14-3：WASAPI 后端要 COM（ole32）+ 多媒体线程提升（avrt）+ 内核流（ksuser）
# 与 waveOut 兜底的 winmm。全部只在 Windows 上存在；engine/audio/WasapiBackend.cpp
# 整个文件被 `#if defined(_WIN32)` 包住，非 Windows 上是一个空 TU。
# ⚠ propsys：`PKEY_Device_FriendlyName` 在 MSVC 下由 propsys.lib 提供；
#   漏了它链接期才报 `undefined reference`（编译期完全看不出来）。
#   MinGW 的 libpropsys.a **不导出**这些数据符号 → 那边由
#   WasapiBackend.cpp 里的本地定义兜底（见该文件）。
if(WIN32)
    target_link_libraries(VennEngine PUBLIC ole32 avrt ksuser winmm propsys)
endif()
if(NOT MSVC)
    target_compile_options(VennEngine PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
# MSVC 侧：/utf-8 /W3 /permissive- 已在文件顶部的全局 add_compile_options 给出。
# （VENN_KEYWORD_COUNT 的透传在下方"Shader 变体"一节末尾 —— 那时
#   SHADER_KEYWORDS 才数得出长度。）

# ============================================================
# 编辑器（editor/ → Editor.exe）
# ------------------------------------------------------------
# editor/ 里是只读依赖 engine/，反过来 engine/ 不认识 editor/。
# （曾经还有一个 Sandbox 独立示例程序，2026-10-09 按用户要求整体删除。）
# ============================================================
file(GLOB_RECURSE EDITOR_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/editor/*.cpp
)
add_executable(VennEditor ${EDITOR_SOURCES})
set_target_properties(VennEditor PROPERTIES OUTPUT_NAME Editor)
if(DEFINED VENN_ICON_RC)
    target_sources(VennEditor PRIVATE ${VENN_ICON_RC})
    # venn.rc 里是 `1 ICON "venn.ico"`，windres 在编译 rc 时才去读那个 ico。
    # 但 ninja 只把 rc 本身当作输入，不看 rc 里 `ICON` 语句引用的文件——
    # 于是单独重做 venn.ico（tools/make_icon.py 生成的 17 档高清图标）时，
    # rc.obj 被判定为最新，Editor.exe 里仍然是老图标。2026-10-11 就是这么
    # 踩的：ico 换成 17 档后 exe 内嵌资源仍是旧的 7 档。
    # 用 OBJECT_DEPENDS 把 ico 显式挂成 rc 的编译期依赖。
    set_source_files_properties(${VENN_ICON_RC} PROPERTIES
        OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/venn.ico")
endif()
target_link_libraries(VennEditor PRIVATE VennEngine)
if(WIN32)
    # 导入资源用的系统"打开文件"对话框（GetOpenFileNameW）在 comdlg32 里。
    # glfw/shell32 那几条是它隐式带进来的，comdlg32 没有 —— 不显式链接会
    # 在链接期报 LNK2019: __imp_GetOpenFileNameW。
    target_link_libraries(VennEditor PRIVATE comdlg32)
endif()

# ============================================================
# 运行时（runtime/ → Game.exe）
# ------------------------------------------------------------
# 出货用的那个可执行文件。与编辑器**完全平行**：同一个 VennEngine，
# 两个 main()，互相之间没有依赖，也不共享任何应用层代码。
# ============================================================
file(GLOB_RECURSE RUNTIME_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime/*.cpp
)
add_executable(VennRuntime ${RUNTIME_SOURCES})
set_target_properties(VennRuntime PROPERTIES OUTPUT_NAME Game)
if(DEFINED VENN_ICON_RC)
    target_sources(VennRuntime PRIVATE ${VENN_ICON_RC})
    set_source_files_properties(${VENN_ICON_RC} PROPERTIES
        OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/venn.ico")
endif()
target_link_libraries(VennRuntime PRIVATE VennEngine)

# ============================================================
# 启动器（launcher/ → Launcher.exe）
# ------------------------------------------------------------
# 路线图 D1/D2：**独立进程**，不是编辑器的一个模式。三个应用层平行：
#
#     launcher/ ─┐
#     editor/   ─┼─►  VennEngine
#     runtime/  ─┘
#
# ⚠ 它**不**链接 editor/ 的任何东西（连 `editor::tr` 都不用 ——
#   直接 `i18n::tr`）。这是"启动器能独立发出去"的前提，也让
#   `engine/` 之外的三份应用层互不影响。
# ============================================================
file(GLOB_RECURSE LAUNCHER_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/launcher/*.cpp
)
add_executable(VennLauncher ${LAUNCHER_SOURCES})
set_target_properties(VennLauncher PROPERTIES OUTPUT_NAME Launcher)
if(DEFINED VENN_ICON_RC)
    target_sources(VennLauncher PRIVATE ${VENN_ICON_RC})
    set_source_files_properties(${VENN_ICON_RC} PROPERTIES
        OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/venn.ico")
endif()
target_link_libraries(VennLauncher PRIVATE VennEngine)
if(WIN32)
    # `SHCreateItemFromParsingName` / `IFileOpenDialog` 的 COM 侧符号在
    # ole32 + shell32 + uuid 里。ole32 已由 VennEngine PUBLIC 带过来
    # （WASAPI 需要），这里补另外两个。
    target_link_libraries(VennLauncher PRIVATE shell32 uuid)
endif()

if(NOT MSVC)
    target_link_options(VennEditor PRIVATE -static-libgcc -static-libstdc++)
    target_link_options(VennRuntime PRIVATE -static-libgcc -static-libstdc++)
    target_link_options(VennLauncher PRIVATE -static-libgcc -static-libstdc++)
    if(WIN32)
        target_link_options(VennEditor PRIVATE -static)
        target_link_options(VennRuntime PRIVATE -static)
        target_link_options(VennLauncher PRIVATE -static)
    endif()
endif()

# Windows 上窗口程序若要隐藏控制台可改 WIN32，v1 保留控制台便于看日志

# ============================================================
# 着色器编译（glslc）
# ============================================================
set(GLSLC "" CACHE PATH "Path to glslc")
if(NOT GLSLC)
    if(Vulkan_GLSLC_EXECUTABLE)
        set(GLSLC ${Vulkan_GLSLC_EXECUTABLE})
    elseif(DEFINED ENV{VULKAN_SDK})
        if(EXISTS "$ENV{VULKAN_SDK}/Bin/glslc.exe")
            set(GLSLC "$ENV{VULKAN_SDK}/Bin/glslc.exe")
        else()
            set(GLSLC "$ENV{VULKAN_SDK}/bin/glslc")
        endif()
    endif()
endif()
if(NOT GLSLC OR NOT EXISTS "${GLSLC}")
    message(FATAL_ERROR "glslc not found. Set -DGLSLC=<path> or VULKAN_SDK env.")
endif()

set(SHADER_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders)
set(SHADER_OUT_DIR ${CMAKE_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_OUT_DIR})

# 着色器命名 <名字>.<阶段>：pbr.vert / shadow.frag / post.frag / fullscreen.vert
# / cluster_build.comp ...
# 直接遍历目录里实际存在的 *.vert / *.frag / *.comp —— 有的着色器只有单个阶段
# （fullscreen 只有 vert，bloom_* / post 只有 frag，cluster_build 只有 comp），
# 按名字硬编码会试图编译不存在的文件。
file(GLOB SHADER_GLSL_SOURCES CONFIGURE_DEPENDS
    ${SHADER_SRC_DIR}/*.vert
    ${SHADER_SRC_DIR}/*.frag
    ${SHADER_SRC_DIR}/*.comp
)
# 共享的 GLSL 片段（*.glsl）。它们**不**单独编译，只被 #include 进来。
# glslc 的 #include 只按 -I 给的目录找（不认源文件所在目录），
# 所以下面必须显式传 -I；否则报 "Cannot find or open include file"。
# 它们同时进 DEPENDS —— 否则改了共享文件不会触发重编译，
# 调试时会出现"改了没反应"的假象。
file(GLOB SHADER_INCLUDES CONFIGURE_DEPENDS ${SHADER_SRC_DIR}/*.glsl)

set(SHADER_TARGETS "")

# ------------------------------------------------------------
# Shader 变体：维度声明 → 白名单（2026-10-25 重构）
# ------------------------------------------------------------
# 【为什么改】
# 旧实现是 `foreach(MASK RANGE 0..1<<N)`，把 2^N 种组合**无条件全展开**。
# 纯 PBR 时代 6 位 = 64 份还勉强能接受；但二次元渲染要加 Toon / Hair /
# Eye / FaceSDF 四个着色模型：9 位 = 512 组合 × 2 个 .spv 族 = 1024 份
# （约 80 MB），第 10 位就是 160 MB，第 15 位直接是 5 GB —— 这条路走不通。
#
# 【新机制】按两个观察削掉绝大部分无效组合：
#   ① 有些关键字**语义互斥**。alphaMode 只能是 Opaque / Blend / Mask 之一，
#      shadingModel 只能是 Default / Skin / Toon 之一。"既 Blend 又 Mask"
#      这个材质根本不存在，编它纯属浪费（旧机制编了 32 份这样的废品）。
#   ② 有些关键字**只对特定模型有意义**。Parallax 视差贴图在 Toon 上没人用，
#      按模型裁剪特性集，8 种组合直接降到 4 种。
#
# 于是组合数 = Σ_模型 (alpha 取值数 × 2^该模型允许的特性数)，
# 加模型是**线性累加**而不是翻倍：
#     pbr 3×8=24   skin 3×8=24   toon 3×4=12   → 60 份（旧机制 2^7 = 128）
#     再加 hair/eye/face 各 12   → 96 份（旧机制 2^10 = 1024）—— 差 10.7 倍
#
# 【位序仍然必须与 C++ 一致】
# 掩码值就是 .spv 文件名里的数字（render::variantShaderFile 用同一条规则拼）。
# 这条一致性由下面的 -DVENN_KEYWORD_COUNT 与 ShaderKeywords.h 的
# static_assert 兜住：两张表长度不等直接编译失败，而不是等到运行时
# 报"找不到变体着色器 pbr.k<N>.frag.spv"。
# ------------------------------------------------------------

# ---- ① 位序（= engine/render/ShaderKeywords.h 的 ShaderKeyword 枚举顺序）----
# 加一个关键字要改三处：这里追加一行、ShaderKeywords.h 加一位、着色器加
# 一段 #if。漏改任何一处都会被 static_assert（长度不等）或运行时日志
# （掩码对应的文件不存在）抓到。
set(SHADER_KEYWORDS
    VENN_DOUBLE_SIDED   # bit0  双面：关剔除 + 背面翻转法线
    VENN_ALPHA_BLEND    # bit1  透明混合
    VENN_NORMAL_MAP     # bit2  法线贴图
    VENN_ALPHA_MASK     # bit3  裁切
    VENN_SHADING_SKIN   # bit4  预积分皮肤散射（与 bit6 互斥）
    VENN_PARALLAX       # bit5  POM 视差
    VENN_SHADING_TOON   # bit6  二次元素描着色（与 bit4 互斥）
    # bit7 是**纯状态位**（只改 depthTest/depthWrite，不改一行代码）：
    # 它出现在这张表里只为两件事 —— 让位宽与 ShaderKeywords.h 对齐，
    # 以及让 collect_variants 认得这个宏名。**不要把它写进任何
    # FEATURES_<model>**：白名单里一旦出现它，combination 数立刻翻倍，
    # 而编出来的 .spv 与不带的版本逐字节相同。
    # 运行时由 VariantPipelines 的 spvMask 剔除，见 kKeywordStateOnly。
    VENN_OVERLAY        # bit7  覆盖带：不写深度 + 深度测试恒通过（P0-2/B）
)
list(LENGTH SHADER_KEYWORDS KEYWORD_COUNT)

# 宏名 → 位掩码。未声明的宏返回 0（生成时用不到，写错了会默默少一位，
# 所以下面每个取值都来自本文件的声明，不接受外部拼字符串）。
function(keyword_bit MACRO OUT)
    set(idx 0)
    set(bit 0)
    foreach(kw ${SHADER_KEYWORDS})
        if(kw STREQUAL "${MACRO}")
            math(EXPR bit "1 << ${idx}")
        endif()
        math(EXPR idx "${idx} + 1")
    endforeach()
    set(${OUT} ${bit} PARENT_SCOPE)
endfunction()

# ---- ② 维度声明 ----
# 互斥组：取值是宏名，组内**同时只能选一个**。
#
# ⚠ 用 "none" 表示"该组不激活"，**不能写空串** —— 这就是上面陷阱 1 的
#   另一种形态：`set(GROUP_alpha "" A B)` 里的空串会被 CMake 的 list
#   直接丢掉，于是这组只剩 2 个取值，所有"不透明"的变体（也就是最常见的
#   那批）一份都不会生成。实测症状：60 份变成 40 份，且 configure 不报错。
#   "none" 不在 SHADER_KEYWORDS 里，keyword_bit() 对它返回 0，正好等于
#   "这一位不置"。
set(GROUP_alpha none VENN_ALPHA_BLEND VENN_ALPHA_MASK)

# 着色模型：名字列表 + 名字→宏（"" = Default / PBR）。
# 名字同时是 FEATURES_<name> 的后缀，所以三者必须同名。
set(SHADING_MODELS pbr skin toon)
set(MODEL_MACRO_pbr  "")
set(MODEL_MACRO_skin "VENN_SHADING_SKIN")
set(MODEL_MACRO_toon "VENN_SHADING_TOON")

# 每个模型允许叠加的自由开关 —— **省组合数的关键**。
# Toon 不需要 POM（视差贴图在赛璐璐上没有意义），8 种组合降到 4 种。
set(FEATURES_pbr  "VENN_DOUBLE_SIDED;VENN_NORMAL_MAP;VENN_PARALLAX")
set(FEATURES_skin "VENN_DOUBLE_SIDED;VENN_NORMAL_MAP;VENN_PARALLAX")
set(FEATURES_toon "VENN_DOUBLE_SIDED;VENN_NORMAL_MAP")

# 每个 shader 支持哪些模型。不同 .frag 可以有不同集合（比如将来的
# toon_hair.frag 只支持 toon）。
set(SHADER_MODELS_pbr.frag "pbr;skin;toon")

# 凡是源码里出现 #if VENN_<材质关键字> 的着色器，都必须列进来。
# （VENN_BINDLESS / VENN_MDI / VENN_INSTANCING / VENN_MSAA_DEPTH 是
#   **全局开关**，走各自独立的 .spv 族，不占关键字位。）
set(SHADER_VARIANT_SHADERS pbr.frag)

# ---- ③ 生成白名单（掩码 + -D 串，两个平行列表）----
# 一个 shader 一份。返回给调用方的是 PARENT_SCOPE 变量。
function(collect_variants FNAME OUT_MASKS OUT_DEFS)
    if(NOT DEFINED SHADER_MODELS_${FNAME})
        message(FATAL_ERROR
            "${FNAME} 列在 SHADER_VARIANT_SHADERS 里，但没有声明 "
            "SHADER_MODELS_${FNAME}（它支持哪些着色模型）。")
    endif()

    set(masks "")
    set(defs "")

    foreach(MODEL ${SHADER_MODELS_${FNAME}})
        set(model_macro "${MODEL_MACRO_${MODEL}}")
        keyword_bit("${model_macro}" base_mask)

        set(feats "${FEATURES_${MODEL}}")
        list(LENGTH feats feat_n)
        # 子集个数 = 2^feat_n；用子集位图枚举而不是递归，CMake 里最直白
        math(EXPR sub_last "1 << ${feat_n}")
        math(EXPR sub_last "${sub_last} - 1")

        foreach(alpha ${GROUP_alpha})
            keyword_bit("${alpha}" alpha_bit)

            # ⚠ 必须把 `RANGE 0 ${sub_last}` 内联在 foreach 里。
            #   写成 `set(r RANGE 0 7)` + `foreach(sub ${r})` 是**错的**：
            #   ${r} 展开成 "RANGE;0;7" 三个**字面量**元素，foreach 会把
            #   "RANGE" 和 "7" 当成子集编号传给 math(EXPR)，结果是静默少
            #   生成（本次实测 60 份变成 40 份），不报错。
            foreach(sub RANGE 0 ${sub_last})
                set(mask ${base_mask})
                math(EXPR mask "${mask} | ${alpha_bit}")
                set(d "")
                if(NOT model_macro STREQUAL "")
                    list(APPEND d -D${model_macro}=1)
                endif()
                if(NOT alpha STREQUAL "none")
                    list(APPEND d -D${alpha}=1)
                endif()
                set(fi 0)
                foreach(f ${feats})
                    math(EXPR fbit "1 << ${fi}")
                    math(EXPR hit "${sub} & ${fbit}")
                    if(hit)
                        keyword_bit("${f}" f_mask)
                        math(EXPR mask "${mask} | ${f_mask}")
                        list(APPEND d -D${f}=1)
                    endif()
                    math(EXPR fi "${fi} + 1")
                endforeach()
                if(d)
                    list(JOIN d " " d_str)
                else()
                    # 哨兵：**空串不能进 list**（陷阱 1）。第一个组合
                    # （pbr + 不透明 + 无任何特性）的 -D 串正好是空的，
                    # 直接 append 会被丢掉，于是 defs 比 masks 少一项、
                    # 两者从此错位。实测症状：list index 59 out of range。
                    set(d_str "_")
                endif()
                list(APPEND masks "${mask}")
                list(APPEND defs "${d_str}")
            endforeach()
        endforeach()
    endforeach()

    set(${OUT_MASKS} "${masks}" PARENT_SCOPE)
    set(${OUT_DEFS} "${defs}" PARENT_SCOPE)
endfunction()

# -D 串列表项 → glslc 参数。空组合在列表里存的是哨兵 "_"，这里还原成
# "没有任何 -D"。两处消费点（主族 / bindless 族）共用，避免各写一遍
# 还原逻辑、其中一处忘了就变成给 glslc 传一个字面量 "-D_=1"。
macro(variant_defs_to_args STR OUT)
    if("${STR}" STREQUAL "_")
        set(${OUT} "")
    else()
        separate_arguments(${OUT} NATIVE_COMMAND "${STR}")
    endif()
endmacro()

# 位宽透传给 C++：ShaderKeywords.h 用它 static_assert 与上面 SHADER_KEYWORDS
# 的长度对齐。错位时的症状是运行期"找不到变体着色器 pbr.k<N>.frag.spv"——
# 那时编译、链接、启动、加载场景全都跑完了，排查成本高得多。
# PUBLIC 是因为 engine/editor/runtime 都可能间接 include 那个头。
target_compile_definitions(VennEngine PUBLIC
    VENN_KEYWORD_COUNT=${KEYWORD_COUNT}
)

# ⚠ 两个 CMake 陷阱（都踩过，写在这里免得后人重踩）：
#   1. `list(APPEND tbl "${EMPTY}")` 会**丢掉空串** —— 展开后所有下标往前
#      挪一位，掩码与 -D 表全部错位。所以上面 collect_variants 里空项一律
#      走 `if(d) ... else() set(d_str "")`，不往列表里塞空串。
#   2. `${DEFS}` 本身是**列表**，直接 append 会被展平成多个表项。所以
#      用 list(JOIN) 拍成一个以空格分隔的单串，compile 时再 separate_arguments。
#
# （旧的"2^N 全展开"代码在这里，2026-10-25 已由 collect_variants() 取代。
#   它留下的 VARIANT_COUNT / VARIANT_MAX 仍被下面复用 —— 但那两个现在是
#   **白名单长度**，不再是 2^位数，别按旧含义读。）

foreach(SRC ${SHADER_GLSL_SOURCES})
    get_filename_component(FNAME ${SRC} NAME)       # 例：bloom_bright.frag
    string(REGEX REPLACE "\\.[^.]*$" "" STEM ${FNAME})   # bloom_bright
    string(REGEX REPLACE "^.*\\." "" EXT ${FNAME})       # frag

    if(FNAME IN_LIST SHADER_VARIANT_SHADERS)
        # 带材质关键字的着色器：只编白名单里的组合（不是 2^N）
        collect_variants(${FNAME} V_MASKS V_DEFS)
        list(LENGTH V_MASKS V_N)
        # configure 时打一行：改了维度声明立刻能看出组合数怎么变
        # （旧机制的全展开份数是 2^位数，可以在改动前后直接对比）。
        message(STATUS "shader variants: ${FNAME} -> ${V_N} 份（白名单）")
        math(EXPR V_LAST "${V_N} - 1")
        foreach(IDX RANGE ${V_LAST})
            list(GET V_MASKS ${IDX} MASK)
            list(GET V_DEFS ${IDX} DEFS_STR)
            # DEFS_STR 是空格分隔的单串（空组合是哨兵 "_"），转成 glslc
            # 参数列表。多关键字时若直接传单串会拼成一个畸形 -D，glslc
            # 报 "unexpected tokens following directive"。
            variant_defs_to_args("${DEFS_STR}" DEFS)
            set(OUT ${SHADER_OUT_DIR}/${STEM}.k${MASK}.${EXT}.spv)
            add_custom_command(
                OUTPUT ${OUT}
                COMMAND ${GLSLC} -I${SHADER_SRC_DIR} ${DEFS} ${SRC} -o ${OUT}
                DEPENDS ${SRC} ${SHADER_INCLUDES}
                COMMENT "glslc: ${FNAME} k${MASK} -> ${STEM}.k${MASK}.${EXT}.spv"
                VERBATIM
            )
            list(APPEND SHADER_TARGETS ${OUT})
        endforeach()
    else()
        set(OUT ${SHADER_OUT_DIR}/${FNAME}.spv)     # 例：bloom_bright.frag.spv
        add_custom_command(
            OUTPUT ${OUT}
            COMMAND ${GLSLC} -I${SHADER_SRC_DIR} ${SRC} -o ${OUT}
            DEPENDS ${SRC} ${SHADER_INCLUDES}
            COMMENT "glslc: ${FNAME} -> ${FNAME}.spv"
            VERBATIM
        )
        list(APPEND SHADER_TARGETS ${OUT})
    endif()
endforeach()

# ------------------------------------------------------------
# pbr.bindless.frag —— Bindless 版片元着色器（R2-3）
# ------------------------------------------------------------
# 同一份 pbr.frag 再编一遍白名单里的组合，额外加 -DVENN_BINDLESS=1：六张贴图槽从
# "set 1..6 各一个 sampler2D" 改成 "set 1 binding 0 的运行时数组 + 材质页表"。
#
# 为什么仍然要 ×N 份而不是"把 bindless 做成一个关键字位"：
#   关键字位是**材质属性**（双面/透明/法线图/裁切），bindless 是**全局开关**
#   （Renderer 按它选 set 布局），两者正交。混进同一个位掩码会让
#   "全局开关 = 材质组合"这种错配有机会发生，也会让掩码空间白白翻倍。
#   所以沿用 instancing / mdi 顶点着色器的处理方式：全局开关 → 独立的
#   .spv 文件族，名字是 pbr.bindless.k<N>.frag.spv。
#   VariantPipelines 只要把 fragBaseName 换成 "pbr.bindless.frag" 即可命中
#   （variantShaderFile 的拼法是 <stem>.k<N>.<ext>.spv）。
#
# 白名单与主族**是同一份**（collect_variants 幂等，同一组声明生成同一组
# 掩码）—— 两个族的文件名数字必须一一对应，否则 bindless 开关一翻转就会
# 读到不存在的变体。这里复用函数而不是复算一遍，就是为了不可能不一致。
collect_variants(pbr.frag BL_MASKS BL_DEFS)
list(LENGTH BL_MASKS BL_N)
math(EXPR BL_LAST "${BL_N} - 1")
foreach(IDX RANGE ${BL_LAST})
    list(GET BL_MASKS ${IDX} MASK)
    list(GET BL_DEFS ${IDX} DEFS_STR)
    # 与主族同一个转换（空组合是哨兵 "_"）。多关键字时必须拆回多个参数，
    # 否则会变成一个带空格的畸形 -D：
    # `"-DVENN_DOUBLE_SIDED=1 -DVENN_ALPHA_BLEND=1"`，glslc 报
    # "'#if' : unexpected tokens following directive"。
    variant_defs_to_args("${DEFS_STR}" DEFS)
    set(OUT ${SHADER_OUT_DIR}/pbr.bindless.k${MASK}.frag.spv)
    add_custom_command(
        OUTPUT ${OUT}
        COMMAND ${GLSLC} -I${SHADER_SRC_DIR} -DVENN_BINDLESS=1 ${DEFS}
                ${SHADER_SRC_DIR}/pbr.frag -o ${OUT}
        DEPENDS ${SHADER_SRC_DIR}/pbr.frag ${SHADER_INCLUDES}
        COMMENT "glslc: pbr.frag (VENN_BINDLESS) k${MASK} -> pbr.bindless.k${MASK}.frag.spv"
        VERBATIM
    )
    list(APPEND SHADER_TARGETS ${OUT})
endforeach()

# ------------------------------------------------------------
# pbr.inst.vert —— GPU instancing 版顶点着色器
# ------------------------------------------------------------
# 同一份 pbr.vert 加 -DVENN_INSTANCING=1：顶点变换从实例 SSBO
# （set 0 binding 13）按 gl_InstanceIndex 取 model，而不是 push 常量。
# 不用关键字变体机制排 8 份 —— 顶点阶段与 3 个片元关键字正交，
# instancing 是**全局开关**（Renderer 按它选 vert），不是材质属性。
# 深度预通道与主前向两个 Pass 用同一份（见 createPipelines 的说明）。
set(PBR_INST_OUT ${SHADER_OUT_DIR}/pbr.inst.vert.spv)
add_custom_command(
    OUTPUT ${PBR_INST_OUT}
    COMMAND ${GLSLC} -I${SHADER_SRC_DIR} -DVENN_INSTANCING=1
            ${SHADER_SRC_DIR}/pbr.vert -o ${PBR_INST_OUT}
    DEPENDS ${SHADER_SRC_DIR}/pbr.vert ${SHADER_INCLUDES}
    COMMENT "glslc: pbr.vert (VENN_INSTANCING) -> pbr.inst.vert.spv"
    VERBATIM
)
list(APPEND SHADER_TARGETS ${PBR_INST_OUT})

# ------------------------------------------------------------
# pbr.mdi.vert —— Multi-draw indirect 版顶点着色器（R2-2）
# ------------------------------------------------------------
# 同一份 pbr.vert 加 -DVENN_MDI=1：model 矩阵与蒙皮定位都从
# **结构化实例记录**（set 0 binding 15，由 mdi_compact.comp 紧凑化）
# 里取。材质仍走 push 常量 —— 一次 vkCmdDrawIndexedIndirect 覆盖的是
# "同管线 + 同材质"的一组批。
set(PBR_MDI_OUT ${SHADER_OUT_DIR}/pbr.mdi.vert.spv)
add_custom_command(
    OUTPUT ${PBR_MDI_OUT}
    COMMAND ${GLSLC} -I${SHADER_SRC_DIR} -DVENN_MDI=1
            ${SHADER_SRC_DIR}/pbr.vert -o ${PBR_MDI_OUT}
    DEPENDS ${SHADER_SRC_DIR}/pbr.vert ${SHADER_INCLUDES}
    COMMENT "glslc: pbr.vert (VENN_MDI) -> pbr.mdi.vert.spv"
    VERBATIM
)
list(APPEND SHADER_TARGETS ${PBR_MDI_OUT})

# ------------------------------------------------------------
# 屏幕空间效果的 MSAA 深度变体（SSAO / SSR / SSGI / god rays）
# ------------------------------------------------------------
# 这些全屏 pass 都要读 HdrTarget 的**深度图**。深度图是 MSAA 的：
# 多采样图像不能用 sampler2D 采样，必须 sampler2DMS + texelFetch
# （见 assets/shaders/space.glsl 顶部的说明）。同一份源码编两份：
#   <名字>.<阶段>.spv        默认（VENN_MSAA_DEPTH=0，sampler2D）
#   <名字>.msaa.<阶段>.spv   -DVENN_MSAA_DEPTH=1（sampler2DMS）
# 运行期按 HdrTarget::samples() 选哪份 —— MYVK_MSAA=1 时用前者，
# 4x（默认）时用后者。给空间.glsl 的函数族提供两种重载签名。
set(SHADER_MSAA_DEPTH_SHADERS
    ssao.frag ssao_blur.frag fg_ssr.frag fg_ssgi.frag fg_godrays.frag
    fg_dof.frag fg_motionblur.frag fg_taa.frag decal.frag particle.frag
    water.frag)

foreach(SRC ${SHADER_MSAA_DEPTH_SHADERS})
    set(SRC_PATH ${SHADER_SRC_DIR}/${SRC})
    string(REGEX REPLACE "\\.[^.]*$" "" STEM ${SRC})
    set(MSAA_OUT ${SHADER_OUT_DIR}/${STEM}.msaa.frag.spv)
    add_custom_command(
        OUTPUT ${MSAA_OUT}
        COMMAND ${GLSLC} -I${SHADER_SRC_DIR} -DVENN_MSAA_DEPTH=1
                ${SRC_PATH} -o ${MSAA_OUT}
        DEPENDS ${SRC_PATH} ${SHADER_INCLUDES}
        COMMENT "glslc: ${SRC} (VENN_MSAA_DEPTH) -> ${STEM}.msaa.frag.spv"
        VERBATIM
    )
    list(APPEND SHADER_TARGETS ${MSAA_OUT})
endforeach()

# ------------------------------------------------------------
# MSAA 深度变体的 **compute** 版（Hi-Z 金字塔，R2-1）
# ------------------------------------------------------------
# hiz_build.comp 要从 HdrTarget 的深度图建金字塔 mip0。深度是 MSAA 的，
# 只能用 sampler2DMS + texelFetch 读（与上面那批 frag 同一条理由）。
# 编两份：
#   hiz_build.comp.spv        VENN_MSAA_DEPTH=0 → sampler2D
#   hiz_build.msaa.comp.spv   VENN_MSAA_DEPTH=1 → sampler2DMS
# 运行期按 HdrTarget::samples() 选（见 HiZOcclusion::createPipelines）。
set(SHADER_MSAA_DEPTH_COMPUTE hiz_build.comp fog_inject.comp)

foreach(SRC ${SHADER_MSAA_DEPTH_COMPUTE})
    set(SRC_PATH ${SHADER_SRC_DIR}/${SRC})
    string(REGEX REPLACE "\\.[^.]*$" "" STEM ${SRC})
    set(MSAA_OUT ${SHADER_OUT_DIR}/${STEM}.msaa.comp.spv)
    add_custom_command(
        OUTPUT ${MSAA_OUT}
        COMMAND ${GLSLC} -I${SHADER_SRC_DIR} -DVENN_MSAA_DEPTH=1
                ${SRC_PATH} -o ${MSAA_OUT}
        DEPENDS ${SRC_PATH} ${SHADER_INCLUDES}
        COMMENT "glslc: ${SRC} (VENN_MSAA_DEPTH) -> ${STEM}.msaa.comp.spv"
        VERBATIM
    )
    list(APPEND SHADER_TARGETS ${MSAA_OUT})
endforeach()

add_custom_target(Shaders ALL DEPENDS ${SHADER_TARGETS})

# ------------------------------------------------------------
# 清理白名单外的旧变体 .spv
# ------------------------------------------------------------
# 为什么需要这段：add_custom_command 只会"生成"，**从不删除**。一旦改了
# SHADER_KEYWORDS 位序或白名单（比如把某个模型从 FEATURES_ 里去掉），
# 上一轮编出来的那些 .spv 会原样留在输出目录里：
#   · 运行时的 variantShaderFile() 按掩码拼文件名 → 旧文件"恰好"还能被
#     读到，于是新配置本该报"找不到变体"的地方静默读到了**过期字节码**；
#   · `ls *.spv | wc -l` 也不再能当作"当前白名单规模"的判据。
# 这条清理只在 configure 时枚举，所以它准确对应"这一轮声明的白名单"。
set(SHADER_STALE "")
foreach(_stem pbr pbr.bindless)
    file(GLOB _found ${SHADER_OUT_DIR}/${_stem}.k*.frag.spv)
    foreach(_f ${_found})
        get_filename_component(_n ${_f} NAME)
        if(_n MATCHES "^${_stem}\\.k([0-9]+)\\.frag\\.spv$")
            if(NOT CMAKE_MATCH_1 IN_LIST BL_MASKS)
                list(APPEND SHADER_STALE ${_f})
            endif()
        endif()
    endforeach()
endforeach()
# 在 **configure 阶段**删，而不是 POST_BUILD：configure 正是"白名单刚被
# 重新声明"的时刻，语义最准，也不依赖 custom target 的构建顺序。
if(SHADER_STALE)
    list(LENGTH SHADER_STALE _stale_n)
    file(REMOVE ${SHADER_STALE})
    message(STATUS
        "shader variants: 清理了 ${_stale_n} 个白名单外的旧 .spv")
endif()
# 两个可执行文件都要按构建顺序排在着色器之后（真正"每次都拷"在下面的
# VennShadersCopy 里，这里管的是"编译顺序"）。
add_dependencies(VennEditor Shaders)
add_dependencies(VennRuntime Shaders)

# ============================================================
# 运行期资源拷贝（Editor 用）
# ------------------------------------------------------------
# 运行时统一按 <exe_dir>/shaders 与 <exe_dir>/assets/... 查找资源
# （见 assets/AssetPath.cpp 的解析顺序）。
# ============================================================
function(myvk_copy_runtime_assets target)
    # ⚠ G01 起，这里不再"按 target 各拷一份" —— Editor.exe 与 Game.exe 需要
    #   完全相同的 shaders/ 与 assets/，各拷一遍等于把拷贝量翻倍。
    #   两个 exe 都 add_dependencies 到下面这两个**全局** target 上。
    #
    # 拷贝 .spv 与 GLSL 源码到可执行目录。
    #
    # ⚠ **不能**用 `add_custom_command(TARGET ... POST_BUILD)`：POST_BUILD 只在
    #   **目标自身被重建（重新链接）**时执行。于是"只改了一个 .frag、没动任何
    #   C++"→ `Shaders` 目标重新生成 .spv，但可执行文件被认为是最新的、不重链接
    #   → 拷贝步骤整段跳过 → 运行时加载的还是**上一次**的 .spv。现象极具迷惑性：
    #   改了着色器却"看不到任何变化"，或者看到的是上一版的行为。
    #   （`add_dependencies(... Shaders)` 只保证**构建顺序**，不触发重链接。）
    #
    # 正确做法：把拷贝挂在一个**没有输出文件的 custom target** 上 —— 这种目标的
    # COMMAND 每次构建都会执行；再让它 DEPENDS 全部 .spv，于是顺序变成
    # "先把着色器编出来 → 再拷过去"，改一处着色器也能立刻生效。
    if(NOT TARGET VennShadersCopy)
        add_custom_target(VennShadersCopy ALL
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${SHADER_OUT_DIR} ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders
            COMMAND ${CMAKE_COMMAND} -E copy
                ${SHADER_GLSL_SOURCES} ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders
            DEPENDS ${SHADER_TARGETS}
            COMMENT "Copying shaders to binary dir"
        )
    endif()

    # ---- 只读资源（纹理/模型/字体/图标 + game.json）：同样必须**每次构建**都拷 ----
    #
    # ⚠ 与上面的 .spv 完全同一个坑：这些是**构建输入**（由 tools/ 下的脚本离线
    #   产出），改了源文件却不重新链接 C++ 时，POST_BUILD 不会跑 → 运行时读到的
    #   还是旧资产。调试时表现为"我明明重新生成了贴图，画面却一点没变"，
    #   极难定位。所以一并挂到"每次都跑"的 custom target 上。
    #
    # ⚠ assets/scenes 与 assets/scripts **不在此列**：它们是编辑器运行时**会写**
    #   的目录（存档场景 / 自研脚本），每次构建都从仓库覆盖过去会把用户的存档
    #   冲掉 —— 那两个目录保持 POST_BUILD（只在重新链接时同步一次）。
    #
    # ⚠ `assets/game.json` 是 **G05 的运行时启动配置**：它是只读的出厂默认，
    #   运行时**不写**它（写的是用户目录），所以走"每次拷贝"这一档。
    if(NOT TARGET VennAssetsCopy)
        set(VENN_ALWAYS_ASSET_DIRS
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/models|assets/models"
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/textures|assets/textures"
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons|assets/icons"
            # 启动器新建向导的类型卡片插图（第一/第三人称，动漫风格）。
            # 只给 `Launcher.exe` 的 UI 用，但**跟着资产根走**：启动器也用
            # `assets::resolveAssetPath`，和编辑器同一套候选链，不另开路径规则。
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/launcher|assets/launcher"
            # U1（游戏 UI）：SDF 字体图集（PNG + JSON）由 tools/gen_sdf_atlas.py
            # **构建期离线生成**并提交进仓库。运行时不读系统字体 —— 否则
            # 打包到玩家机器上中文全是方块，且系统字体不可再分发。
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/ui|assets/ui"
            # G14：音频素材。⚠ 必须进"每次都拷"这一档 —— `POST_BUILD` 只在
            # 重新链接时跑，改了 wav 却不重新链接时，运行时读到的还是旧素材
            # （表现是"我明明换了音效，听起来没变"）。
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/audio|assets/audio"
            # W2-1：材质资产（`.vmat`）。母材质要能"被多个关卡共享"，
            # 所以它必须有自己的文件；而且改一次要立刻生效 → 进"每次都拷"。
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/materials|assets/materials"
            # W4：地形资产（`.vterrain` + `.r32` + `.rgba8`）。与材质同一条
            # 理由 —— 改一次高度图要立刻生效，否则"我明明加高了山"只能
            # 在重新链接之后才看得到。
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/terrain|assets/terrain"
            # A2b（2026-10-25）：动画状态机控制器（`.anim.json`）。
            # 同样是"改一次就想立刻看到"的资产（调一个转移的 duration 要
            # 马上能试手感），所以进每次都拷这一档。
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/animators|assets/animators"
        )
        set(always_asset_cmds "")
        foreach(item IN LISTS VENN_ALWAYS_ASSET_DIRS)
            string(REPLACE "|" ";" pair "${item}")
            list(GET pair 0 src)
            list(GET pair 1 rel)
            if(EXISTS "${src}")
                list(APPEND always_asset_cmds
                    COMMAND ${CMAKE_COMMAND} -E make_directory
                        ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${rel}
                    COMMAND ${CMAKE_COMMAND} -E copy_directory
                        "${src}" ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${rel}
                )
            endif()
        endforeach()
        if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/misc/fonts)
            list(APPEND always_asset_cmds
                COMMAND ${CMAKE_COMMAND} -E make_directory
                    ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/assets/fonts
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/misc/fonts
                    ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/assets/fonts
            )
        endif()
        if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/assets/game.json)
            list(APPEND always_asset_cmds
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${CMAKE_CURRENT_SOURCE_DIR}/assets/game.json
                    ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/assets/game.json
            )
        endif()
        if(always_asset_cmds)
            add_custom_target(VennAssetsCopy ALL
                ${always_asset_cmds}
                COMMENT "Copying readonly assets to binary dir"
            )
        else()
            add_custom_target(VennAssetsCopy ALL
                COMMAND ${CMAKE_COMMAND} -E echo "no readonly assets"
                COMMENT "no readonly assets"
            )
        endif()
    endif()

    add_dependencies(${target} VennShadersCopy VennAssetsCopy)

    # 场景 / 脚本目录（编辑器与运行时都会读写 assets/scenes 与 assets/scripts）
    # ⚠ S6 起 `assets/scripts/` 里是**随仓库提交**的示例玩法脚本
    #   （enemy_patrol / door_timer / pickup / hud_counter），
    #   `assets/scenes/levels/script_demo.json` 用相对资产根的路径引用它们。
    #
    # ⚠⚠ 这里是 `add_custom_target(... ALL)` 而不是 `add_custom_command(TARGET
    #   POST_BUILD)`：POST_BUILD **只在重链之后**才跑，而这两个目录是"改了
    #   资产但没改 C++"时唯一变化的东西 —— 用 POST_BUILD 的话，改完
    #   `script_demo.json` 直接跑测试会拿到**上一份**场景，而且没有任何提示。
    #   （以前它们在 POST_BUILD 里，W3/W4 都靠"反正会重链"侥幸躲过去了。）
    #
    # ⚠⚠ 2026-10-19（启动器 M0）：原来这里是 `${target}_copy_scenes`，**按
    #   target 各建一份**。两个 exe 的输出目录是同一个 `${CMAKE_RUNTIME_OUTPUT_DIRECTORY}`
    #   → ninja 并行执行两条 `copy_directory` 到同一个目标目录 → 实测直接
    #   `Error copying directory ... No such file or directory`（一边正在建目录、
    #   另一边已经在往它里面拷）。与 VennShadersCopy / VennAssetsCopy 统一成
    #   **全局单例 + 两个 exe 都 add_dependencies**，从根上消掉这条竞态。
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/assets/scenes AND NOT TARGET VennScenesCopy)
        add_custom_target(VennScenesCopy ALL
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${CMAKE_CURRENT_SOURCE_DIR}/assets/scenes
                ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/assets/scenes
            COMMENT "Copying scenes to binary dir"
            VERBATIM
        )
    endif()
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/assets/scripts AND NOT TARGET VennScriptsCopy)
        add_custom_target(VennScriptsCopy ALL
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${CMAKE_CURRENT_SOURCE_DIR}/assets/scripts
                ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/assets/scripts
            COMMENT "Copying scripts to binary dir"
            VERBATIM
        )
    endif()
    if(TARGET VennScenesCopy)
        add_dependencies(${target} VennScenesCopy)
    endif()
    if(TARGET VennScriptsCopy)
        add_dependencies(${target} VennScriptsCopy)
    endif()

    # ------------------------------------------------------------
    # 模板（启动器 M0）：`templates/` → `<exe>/templates`
    # ------------------------------------------------------------
    # `project::templatesRoot()` 的候选链是
    #   MYVK_TEMPLATE_DIR > <exe>/templates > <cwd>/templates
    # 所以不拷的话，从 `build/bin` 跑 `Game.exe --selftest project` 会
    # 找不到模板（`templatesRoot()` 会退化成 `<exe>/templates` 这个
    # **不存在**的路径，`templateDir()` 返回空串，创建自检全红）。
    #
    # 与 scenes/scripts 同样用 `ALL` 而不是 POST_BUILD：改一个模板 json
    # 不重链接 C++，POST_BUILD 不会跑，于是"改了模板没生效"。
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/templates AND NOT TARGET VennTemplatesCopy)
        add_custom_target(VennTemplatesCopy ALL
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${CMAKE_CURRENT_SOURCE_DIR}/templates
                ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/templates
            COMMENT "Copying templates to binary dir"
            VERBATIM
        )
    endif()
    if(TARGET VennTemplatesCopy)
        add_dependencies(${target} VennTemplatesCopy)
    endif()
endfunction()

myvk_copy_runtime_assets(VennEditor)
myvk_copy_runtime_assets(VennRuntime)
# 启动器也要 `templates/`（新建工程时复制它）与 `assets/icons/`（窗口图标）。
# 不需要着色器 —— 启动器一个 3D 也不画；但那个函数会一并拷进来，
# 代价只是磁盘上多一份 shader，换来"三个 exe 的目录布局完全一致"，
# 少一类"某个 exe 跑起来找不到 X"的差异。
myvk_copy_runtime_assets(VennLauncher)


# ============================================================
# 离线资产编译器（assetc/ → AssetC.exe）
# ------------------------------------------------------------
# 它**只**链 `VennAssetIO`，故意不链 `VennEngine`：
#
#   · 链了 VennEngine 就会把 Vulkan / GLFW / VMA 一起带进来，
#     于是"烤一个模型"变成"必须先有显卡驱动"；
#   · 它是构建期工具，跟 editor/runtime/launcher 不是同一类东西
#     （那三个是**应用**，这个是**工具**），所以不放进
#     myvk_copy_runtime_assets 那套"三个 exe 目录一致"的约束里。
#
# 输出名 AssetC（与 Editor / Game / Launcher 一致：短、无版本号）。
# ============================================================
file(GLOB_RECURSE ASSETC_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/assetc/*.cpp
)
add_executable(VennAssetC ${ASSETC_SOURCES})
set_target_properties(VennAssetC PROPERTIES OUTPUT_NAME AssetC)
# nlohmann_json：MeshTweak 读侧车补丁用（头文件型 INTERFACE 库，零链接成本）。
target_link_libraries(VennAssetC PRIVATE VennAssetIO)
if(NOT MSVC)
    target_compile_options(VennAssetC PRIVATE -Wall -Wextra)
endif()
