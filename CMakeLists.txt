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

# ---- nlohmann/json（单头文件；场景序列化用）----
# 头文件放在 third_party/nlohmann/json.hpp，include 目录指到 third_party，
# 这样代码里统一写 #include <nlohmann/json.hpp>
add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

add_library(vma INTERFACE)
target_include_directories(vma INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/VulkanMemoryAllocator/include)

# ============================================================
# 引擎静态库（target 名 = venn）
# ============================================================
file(GLOB_RECURSE ENGINE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/engine/*.cpp)
add_library(venn STATIC ${ENGINE_SOURCES})
target_include_directories(venn PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
)
target_link_libraries(venn PUBLIC
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
if(MSVC)
    target_compile_options(venn PRIVATE /W3)
else()
    target_compile_options(venn PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# ============================================================
# 编辑器（唯一的可执行文件）
# ------------------------------------------------------------
# editor/ 里是只读依赖 engine/，反过来 engine/ 不认识 editor/。
# （曾经还有一个 Sandbox 独立示例程序，2026-10-09 按用户要求整体删除。）
# ============================================================
file(GLOB_RECURSE EDITOR_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/editor/*.cpp
)
add_executable(Editor ${EDITOR_SOURCES})
if(DEFINED VENN_ICON_RC)
    target_sources(Editor PRIVATE ${VENN_ICON_RC})
    # venn.rc 里是 `1 ICON "venn.ico"`，windres 在编译 rc 时才去读那个 ico。
    # 但 ninja 只把 rc 本身当作输入，不看 rc 里 `ICON` 语句引用的文件——
    # 于是单独重做 venn.ico（tools/make_icon.py 生成的 17 档高清图标）时，
    # rc.obj 被判定为最新，Editor.exe 里仍然是老图标。2026-10-11 就是这么
    # 踩的：ico 换成 17 档后 exe 内嵌资源仍是旧的 7 档。
    # 用 OBJECT_DEPENDS 把 ico 显式挂成 rc 的编译期依赖。
    set_source_files_properties(${VENN_ICON_RC} PROPERTIES
        OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/venn.ico")
endif()
target_link_libraries(Editor PRIVATE venn)

if(NOT MSVC)
    target_link_options(Editor PRIVATE -static-libgcc -static-libstdc++)
    if(WIN32)
        target_link_options(Editor PRIVATE -static)
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
#   bit0 → VENN_DOUBLE_SIDED   bit1 → VENN_ALPHA_BLEND   bit2 → VENN_NORMAL_MAP
#
# 凡是源码里出现 #if VENN_* 的着色器，都必须列进 SHADER_VARIANT_SHADERS ——
# 它会为 2^3 = 8 种组合各编一份 <名字>.k<掩码>.<阶段>.spv。
# 文件名里的 k 数字就是掩码本身，由 render::variantShaderFile() 用同一条
# 规则拼出来。位序改了而这里没改，运行时会读不到文件并在 stderr 报
# "找不到变体着色器"（VariantPipelines.cpp），不会静默画错。
set(SHADER_KEYWORDS VENN_DOUBLE_SIDED VENN_ALPHA_BLEND VENN_NORMAL_MAP)
set(SHADER_VARIANT_SHADERS pbr.frag)

# 掩码 0..7 对应的 -D 参数表。
# 手写展开而不用 math(EXPR) 的位运算：这份表肉眼就能逐个核对，也不依赖
# CMake 版本对 & | << 的支持。加第 4 个关键字时这里会变成 16 行 —— 那时
# 再换生成式也不迟。
set(VARIANT_DEFINES_0)
set(VARIANT_DEFINES_1 -DVENN_DOUBLE_SIDED=1)
set(VARIANT_DEFINES_2 -DVENN_ALPHA_BLEND=1)
set(VARIANT_DEFINES_3 -DVENN_DOUBLE_SIDED=1 -DVENN_ALPHA_BLEND=1)
set(VARIANT_DEFINES_4 -DVENN_NORMAL_MAP=1)
set(VARIANT_DEFINES_5 -DVENN_DOUBLE_SIDED=1 -DVENN_NORMAL_MAP=1)
set(VARIANT_DEFINES_6 -DVENN_ALPHA_BLEND=1 -DVENN_NORMAL_MAP=1)
set(VARIANT_DEFINES_7 -DVENN_DOUBLE_SIDED=1 -DVENN_ALPHA_BLEND=1 -DVENN_NORMAL_MAP=1)

foreach(SRC ${SHADER_GLSL_SOURCES})
    get_filename_component(FNAME ${SRC} NAME)       # 例：bloom_bright.frag
    string(REGEX REPLACE "\\.[^.]*$" "" STEM ${FNAME})   # bloom_bright
    string(REGEX REPLACE "^.*\\." "" EXT ${FNAME})       # frag

    if(FNAME IN_LIST SHADER_VARIANT_SHADERS)
        # 带关键字的着色器：每种组合一份 .spv
        foreach(MASK RANGE 7)
            set(DEFS ${VARIANT_DEFINES_${MASK}})
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
    ssao.frag ssao_blur.frag fg_ssr.frag fg_ssgi.frag fg_godrays.frag)

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

add_custom_target(Shaders ALL DEPENDS ${SHADER_TARGETS})
add_dependencies(Editor Shaders)

# ============================================================
# 运行期资源拷贝（Editor 用）
# ------------------------------------------------------------
# 运行时统一按 <exe_dir>/shaders 与 <exe_dir>/assets/... 查找资源
# （见 assets/AssetPath.cpp 的解析顺序）。
# ============================================================
function(myvk_copy_runtime_assets target)
    # 拷贝 .spv 与 GLSL 源码到可执行目录
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${SHADER_OUT_DIR} $<TARGET_FILE_DIR:${target}>/shaders
        COMMAND ${CMAKE_COMMAND} -E copy
            ${SHADER_GLSL_SOURCES} $<TARGET_FILE_DIR:${target}>/shaders
        COMMENT "Copying shaders to ${target} binary dir"
    )

    # 模型等资源也拷到可执行目录
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/assets/models)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${CMAKE_CURRENT_SOURCE_DIR}/assets/models
                $<TARGET_FILE_DIR:${target}>/assets/models
            COMMENT "Copying models to ${target} binary dir"
        )
    endif()

    # 纹理资源（DDS 压缩纹理测试资产等）
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/assets/textures)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${CMAKE_CURRENT_SOURCE_DIR}/assets/textures
                $<TARGET_FILE_DIR:${target}>/assets/textures
            COMMENT "Copying textures to ${target} binary dir"
        )
    endif()

    # 场景 / 脚本目录（编辑器会读写 assets/scenes 与 assets/scripts）
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

    # 编辑器 UI 字体（ImGui 用 TTF，运行时按 assets/fonts 查找）
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/misc/fonts)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                $<TARGET_FILE_DIR:${target}>/assets/fonts
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/misc/fonts
                $<TARGET_FILE_DIR:${target}>/assets/fonts
            COMMENT "Copying UI fonts to ${target} binary dir"
        )
    endif()

    # venn 的图标（PNG：运行时 glfwSetWindowIcon 用；ICO/RES 只用于 exe 资源）
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/assets/icons)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                $<TARGET_FILE_DIR:${target}>/assets/icons
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${CMAKE_CURRENT_SOURCE_DIR}/assets/icons
                $<TARGET_FILE_DIR:${target}>/assets/icons
            COMMENT "Copying venn icons to ${target} binary dir"
        )
    endif()
endfunction()

myvk_copy_runtime_assets(Editor)

