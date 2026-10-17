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
)
if(NOT MSVC)
    target_compile_options(VennEngine PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
# MSVC 侧：/utf-8 /W3 /permissive- 已在文件顶部的全局 add_compile_options 给出。

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

if(NOT MSVC)
    target_link_options(VennEditor PRIVATE -static-libgcc -static-libstdc++)
    target_link_options(VennRuntime PRIVATE -static-libgcc -static-libstdc++)
    if(WIN32)
        target_link_options(VennEditor PRIVATE -static)
        target_link_options(VennRuntime PRIVATE -static)
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
# Shader 变体 / 关键字系统（与 engine/render/ShaderKeywords.h 配套）
# ------------------------------------------------------------
# 关键字**位序**必须与 ShaderKeywords.h 里的 ShaderKeyword 逐位一致：
#   bit0 → VENN_DOUBLE_SIDED   bit1 → VENN_ALPHA_BLEND
#   bit2 → VENN_NORMAL_MAP     bit3 → VENN_ALPHA_MASK
#   bit4 → VENN_SHADING_SKIN   bit5 → VENN_PARALLAX
#
# 凡是源码里出现 #if VENN_* 的着色器，都必须列进 SHADER_VARIANT_SHADERS ——
# 它会为 2^6 = 64 种组合各编一份 <名字>.k<掩码>.<阶段>.spv。
# 文件名里的 k 数字就是掩码本身，由 render::variantShaderFile() 用同一条
# 规则拼出来。位序改了而这里没改，运行时会读不到文件并在 stderr 报
# "找不到变体着色器"（VariantPipelines.cpp），不会静默画错。
set(SHADER_KEYWORDS VENN_DOUBLE_SIDED VENN_ALPHA_BLEND
                    VENN_NORMAL_MAP VENN_ALPHA_MASK VENN_SHADING_SKIN
                    VENN_PARALLAX)
set(SHADER_VARIANT_SHADERS pbr.frag)

# 掩码 0..31 对应的 -D 参数表 —— **生成式**（R4-1 起从手写 8 行换成循环）。
# 位序来自 SHADER_KEYWORDS 的列表顺序：第 i 个关键字对应 bit i。
# 于是"关键字表改了、-D 表忘了改"这类错位从根上不可能发生。
#
# ⚠ 两个 CMake 陷阱（都踩过，写在这里免得后人重踩）：
#   1. `list(APPEND tbl "${EMPTY}")` 会**丢掉空串** —— 展开后所有下标往前
#      挪一位，k0/k2/k4… 全部错位。所以空项存一个哨兵串 "_"，取用时再
#      换回空。
#   2. `${DEFS}` 本身是**列表**，直接 append 会被展平成多个表项。所以
#      先用 list(JOIN) 拍成一个以空格分隔的单串，表项与掩码才一一对应。
list(LENGTH SHADER_KEYWORDS KEYWORD_COUNT)
math(EXPR VARIANT_COUNT "1 << ${KEYWORD_COUNT}")
math(EXPR VARIANT_MAX "${VARIANT_COUNT} - 1")
set(VARIANT_DEFINES_ALL "")
foreach(MASK RANGE ${VARIANT_MAX})
    set(DEFS "")
    set(BIT 0)
    foreach(KW ${SHADER_KEYWORDS})
        math(EXPR BITVAL "1 << ${BIT}")
        math(EXPR HIT "${MASK} & ${BITVAL}")
        if(HIT)
            list(APPEND DEFS -D${KW}=1)
        endif()
        math(EXPR BIT "${BIT} + 1")
    endforeach()
    if(DEFS)
        list(JOIN DEFS " " DEFS_STR)   # 拍平成单串，避免上面的陷阱 2
    else()
        set(DEFS_STR "_")              # 哨兵，避免陷阱 1
    endif()
    list(APPEND VARIANT_DEFINES_ALL "${DEFS_STR}")
endforeach()
# 把哨兵换回空串（-D 位置留空 = 不定义任何关键字）。
list(TRANSFORM VARIANT_DEFINES_ALL REPLACE "^_$" "")

foreach(SRC ${SHADER_GLSL_SOURCES})
    get_filename_component(FNAME ${SRC} NAME)       # 例：bloom_bright.frag
    string(REGEX REPLACE "\\.[^.]*$" "" STEM ${FNAME})   # bloom_bright
    string(REGEX REPLACE "^.*\\." "" EXT ${FNAME})       # frag

    if(FNAME IN_LIST SHADER_VARIANT_SHADERS)
        # 带关键字的着色器：每种组合一份 .spv
        foreach(MASK RANGE ${VARIANT_MAX})
            list(GET VARIANT_DEFINES_ALL ${MASK} DEFS_STR)
            # 与下面 bindless 那一族同理：DEFS_STR 是空格分隔的单串，
            # 必须拆回多个参数（否则多关键字组合会拼成一个畸形 -D）。
            separate_arguments(DEFS NATIVE_COMMAND "${DEFS_STR}")
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
# 同一份 pbr.frag 再编 ${VARIANT_COUNT} 份，额外加 -DVENN_BINDLESS=1：六张贴图槽从
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
foreach(MASK RANGE ${VARIANT_MAX})
    list(GET VARIANT_DEFINES_ALL ${MASK} DEFS_STR)
    # DEFS_STR 是"以空格分隔的单串"（见上面的生成式说明），必须用
    # separate_arguments 拆回多个参数 —— 否则 k3 会变成**一个**带空格的
    # 参数 `"-DVENN_DOUBLE_SIDED=1 -DVENN_ALPHA_BLEND=1"`，glslc 把它当成
    # 一个畸形 -D 而报 "'#if' : unexpected tokens following directive"。
    separate_arguments(DEFS NATIVE_COMMAND "${DEFS_STR}")
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
    fg_dof.frag fg_motionblur.frag fg_taa.frag decal.frag particle.frag)

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
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/assets/scenes)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${CMAKE_CURRENT_SOURCE_DIR}/assets/scenes
                $<TARGET_FILE_DIR:${target}>/assets/scenes
            COMMENT "Copying scenes to ${target} binary dir"
        )
    endif()
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/assets/scripts)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${CMAKE_CURRENT_SOURCE_DIR}/assets/scripts
                $<TARGET_FILE_DIR:${target}>/assets/scripts
            COMMENT "Copying scripts to ${target} binary dir"
        )
    endif()
endfunction()

myvk_copy_runtime_assets(VennEditor)
myvk_copy_runtime_assets(VennRuntime)

