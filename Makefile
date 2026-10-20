# ============================================================================
#  venn —— Makefile
# ----------------------------------------------------------------------------
#  对 CMake 的一层薄包装（thin wrapper），只做三件事：
#    1. 记住本项目在 Windows + MinGW(ucrt64) + Ninja 下那串又长又易错的
#       configure 参数 —— 尤其是必须带引号的 -DCMAKE_POLICY_VERSION_MINIMUM=3.5
#       （CMake 4.x 会拒绝 GLFW 的 cmake_minimum_required(3.4)）
#    2. 把 build / run / clean / package 这些日常动作收敛成一条短命令
#    3. 产出一个可直接拷走运行的发布包 dist/*.zip
#
#  用法：
#    make              # 等价于 make build
#    make build        # 配置（仅首次）+ 编译 Editor
#    make run          # 编译并运行 Editor（编辑器）—— 项目唯一的可执行文件
#    make clean        # 清掉编译产物，保留配置缓存（重建很快）
#    make package      # 编译 + 打包 dist/venn-<版本>-<类型>-win64.zip
#    make help         # 列出全部目标
#
#  关于"怎么运行游戏"：
#    make run 只打开编辑器。在编辑器里点工具条的 Play，游戏就在**原来的
#    视口里**跑起来（同一个窗口、同一条渲染链，没有第二个进程），按 F 或
#    点视口右上角按钮可以让它占满整个窗口。
#    （原来还有一个 Sandbox.exe 独立示例程序，2026-10-09 按用户要求删除。）
#
#  可覆盖的变量（跟在 make 后面即可）：
#    make BUILD_TYPE=Release build       # 换构建类型（会触发重新 configure）
#    make run FRAMES=300 VALIDATION=0    # 跑 300 帧后自动退出、关验证层
#    make package BUILD_TYPE=Release     # 打 Release 发布包
#    make CC_BIN=.../gcc.exe build       # 换编译器
# ----------------------------------------------------------------------------
#  改这个文件前请先读这三条约束：
#
#  [1] 配方（recipe）必须**同时**能在 cmd.exe 和 sh 下跑。GNU Make 在 Windows
#      上选哪个 shell 取决于 PATH 里有没有 sh.exe，别赌。所以：
#        - 不用 rm / mkdir / cp / del 这类外部命令，一律用 cmake -E 内置命令
#        - 路径统一用正斜杠（cmd 与 sh 都接受）
#        - 不写 `set VAR=` / `VAR=value cmd`，环境变量用 cmake -E env 注入
#        - 不写 $(shell ...) 去调 unix 工具（这台机器上 coreutils 不全）
#
#  [2] echo 出去的文本必须是**纯 ASCII**，且不能含 cmd 元字符
#      （> < | & ^ ( ) %）。原因：
#        - 本文件是 UTF-8，而 cmd.exe 默认用 GBK(936) 代码页，中文会变乱码
#        - `echo A -> B` 里的 `>` 会被 cmd 当成重定向，凭空多出一个文件
#      注释里用中文随意，注释不会被回显。
#
#  [3] 每个配方行必须以 TAB 开头，不能是空格。
# ============================================================================

# ---------------------------------------------------------------- 工具链
# 本工程支持两套工具链，用 TOOLCHAIN 选（默认 auto：有 MinGW 就用 MinGW，
# 否则退到 MSVC）：
#
#   TOOLCHAIN=mingw  MinGW(ucrt64) + Ninja            —— 项目原来的路线
#   TOOLCHAIN=msvc   Visual Studio 2022 + MSBuild     —— 本机实际可用的路线
#   TOOLCHAIN=auto   自动探测（默认）
#
# 为什么会有 msvc 这条路：本机的 MinGW 工具链整条缺失（gcc/g++/
# mingw32-make 都不在，C:/Program Files/CodeBlocks 与 C:/msys64 都不存在），
# 但装了完整的 VS2022 Professional。详见 vcvars.cmd 顶部注释。
#
# MSVC 侧有两个必须知道的约束（都写进 vcvars.cmd 了）：
#   * MSBuild 内部用 .NET Hashtable 存环境变量，这张表**大小写不敏感**。
#     环境里同时存在 https_proxy 与 HTTPS_PROXY 会让它抛 ArgumentException，
#     报成 "MSB6001: CL.exe 的命令行开关无效"。vcvars.cmd 会先清掉小写版。
#   * 源文件是 UTF-8 无 BOM + 大量中文注释，必须给 cl 传 /utf-8，
#     否则按 GBK 代码页解析会报 C2001「常量中有换行符」。
#     （已在 CMakeLists.txt 里为所有 target 钉死。）
CMAKE        ?= cmake
TOOLCHAIN    ?= auto

# 一个空格（给路径转义用）。空 = 空 = 一个空格，这是 GNU Make 里
# 表达"字面空格"的标准写法（空变量与只含空格的变量展开后都是那个空格）。
empty :=
space := $(empty) $(empty)

# ---- MinGW 侧（TOOLCHAIN=mingw / auto 探测命中时使用）----
MINGW_PREFIX ?= C:/msys64/ucrt64
CC_BIN       ?= $(MINGW_PREFIX)/bin/gcc.exe
CXX_BIN      ?= $(MINGW_PREFIX)/bin/g++.exe
GENERATOR    ?= Ninja

# ---- MSVC 侧（TOOLCHAIN=msvc 时使用）----
# VS 安装根目录。vcvars.cmd 用同一个默认值；换机器只需覆盖这一个变量。
VENN_VS_DIR  ?= D:/Microsoft Visual Studio/2022/Professional
# MSVC 用 Ninja 生成器同样可以（cl.exe 由 vcvars.cmd 带进 PATH），
# 这样 build/ 的目录布局与 MinGW 侧完全一致，构建后处理命令不用改。
MSVC_GENERATOR ?= Ninja

# ---- 自动探测 ----
# 探测依据：本机是否存在可用的编译器。
#
# ⚠ 必须用 $(wildcard) 而**不能**用 $(exists)：GNU Make 没有 exists 函数。
# ⚠ 路径里的**空格必须用反斜杠转义**，否则 wildcard 直接返回空 ——
#   "D:/Microsoft Visual Studio/..." 这种路径不转义会让探测永远失败，
#   然后静默地退回 mingw（= 本机没有的编译器）。这个坑实测过：
#     $(wildcard D:/Microsoft Visual Studio/.../vcvars64.bat)      -> 空
#     $(wildcard D:/Microsoft\ Visual\ Studio/.../vcvars64.bat)    -> 命中
#   （没空格的路径转不转义都行。）
# ⚠ 也不能用 $(shell if exist ...)：Make 在 Windows 上若 PATH 里有 sh.exe
#   就会用 sh 跑 $(shell)，cmd 的 if exist 语法在那里直接语法错误。
VENN_VS_DIR_MK := $(subst $(space),\$(space),$(VENN_VS_DIR))
ifeq ($(TOOLCHAIN),auto)
  ifneq ($(wildcard $(subst $(space),\$(space),$(MINGW_PREFIX))/bin/g++.exe),)
    TOOLCHAIN := mingw
  else ifneq ($(wildcard $(subst $(space),\$(space),$(VENN_VS_DIR))/VC/Auxiliary/Build/vcvars64.bat),)
    TOOLCHAIN := msvc
  else
    TOOLCHAIN := mingw
  endif
endif

ifeq ($(TOOLCHAIN),msvc)
  # MSVC：configure / build 都交给 msvc-build.cmd —— 它内部先 call vcvars.cmd
  # 搭好环境（含清掉大小写重复的代理变量），再调 cmake。
  # Makefile 这边只负责把参数用环境变量递过去，保持配方是"一条干净命令"。
  GENERATOR  := $(MSVC_GENERATOR)
  MSVC_BUILD  = cmd /c msvc-build.cmd
else
  MSVC_BUILD  =
endif

# ---------------------------------------------------------------- 构建配置
# 注意：这几个变量必须在 CONFIG_STAMP / CMAKE_CONFIGURE 之前定义。
# 那两个用的是 `:=` 立即展开，写反了会拿到空值（踩过一次）。
BUILD_TYPE ?= Debug
JOBS       ?= $(NUMBER_OF_PROCESSORS)
CMAKE_ARGS ?=

# 并行度：NUMBER_OF_PROCESSORS 在 Windows 上总是存在，其它平台兜底为空
ifeq ($(strip $(JOBS)),)
PARALLEL :=
else
PARALLEL := --parallel $(JOBS)
endif

# ---------------------------------------------------------------- 目录
BUILD_DIR ?= build
DIST_DIR  ?= dist
BIN_DIR    = $(BUILD_DIR)/bin
EDITOR     = $(BIN_DIR)/Editor.exe
GAME       = $(BIN_DIR)/Game.exe
LAUNCHER   = $(BIN_DIR)/Launcher.exe
SHADER_DIR = $(BIN_DIR)/shaders
ASSET_DIR  = $(BIN_DIR)/assets

# 配置戳：文件名里带上构建类型，于是 `make BUILD_TYPE=Release` 会自动重新
# configure，而反复 `make build` 不会再跑一遍 cmake 配置。
CONFIG_STAMP := $(BUILD_DIR)/.configured-$(BUILD_TYPE)

# ---------------------------------------------------------------- 打包
VERSION  ?= 0.2.0
PKG_NAME ?= venn-$(VERSION)-$(BUILD_TYPE)-win64

# 那一行长命令只写一次，$(CONFIG_STAMP) 与 configure 共用。
# 注意 "..." 这对引号是必须的：cmd/PowerShell 会把 -DXXX=3.5 拆成
# -DXXX=3 加 .5，加引号才能原样传给 CMake。
#
# 两套工具链的 configure 参数不同：
#   mingw  —— 显式指定 gcc/g++（Ninja 不会自动找）
#   msvc   —— 完全交给 msvc-build.cmd（内部不指定编译器，让 Ninja 从 PATH
#             里找到 vcvars.cmd 带进来的 cl.exe）
ifeq ($(TOOLCHAIN),msvc)
CMAKE_CONFIGURE =
else
CMAKE_CONFIGURE = $(CMAKE) -S . -B $(BUILD_DIR) -G $(GENERATOR) \
	-DCMAKE_BUILD_TYPE=$(BUILD_TYPE) \
	"-DCMAKE_C_COMPILER=$(CC_BIN)" \
	"-DCMAKE_CXX_COMPILER=$(CXX_BIN)" \
	"-DCMAKE_POLICY_VERSION_MINIMUM=3.5" \
	$(CMAKE_ARGS)
endif

# 编译命令：两条路都归一到"能跑 cmake --build"这一条。
# MSVC 侧不能直接用 cmake --build（那样没有 vcvars 环境，cl.exe 找不到），
# 必须过 msvc-build.cmd —— 并行度由脚本自己从 NUMBER_OF_PROCESSORS 取，
# **不要**在这里再拼 $(PARALLEL)：那会被脚本当成 target 名，
# 报 "CMake Error: Invalid value used with --target"。
ifeq ($(TOOLCHAIN),msvc)
CMAKE_BUILD = $(MSVC_BUILD) build
else
CMAKE_BUILD = $(CMAKE) --build $(BUILD_DIR) $(PARALLEL)
endif

# ---------------------------------------------------------------- 运行参数
FRAMES        ?=
VALIDATION    ?=
RUN_ENV_EXTRA ?=
ARGS          ?=
SMOKE_FRAMES  ?= 120

# 只在变量非空时才注入对应环境变量，避免给进程塞一堆空值。
# 注意 MYVK_VALIDATION=0 才表示关闭；空串会被当成"没设置"（默认开启）。
RUN_ENV :=
ifneq ($(strip $(FRAMES)),)
RUN_ENV += MYVK_FRAMES=$(FRAMES)
endif
ifneq ($(strip $(VALIDATION)),)
RUN_ENV += MYVK_VALIDATION=$(VALIDATION)
endif
ifneq ($(strip $(RUN_ENV_EXTRA)),)
RUN_ENV += $(RUN_ENV_EXTRA)
endif

# cmake -E chdir 先切到 bin 目录再启动程序：ImGui 把布局写成
# <当前工作目录>/imgui.ini，从项目根目录直接跑会把 imgui.ini 甩到仓库根，
# 所以这里显式 chdir 到 build/bin。
RUN_EDITOR  = $(CMAKE) -E chdir $(BIN_DIR) $(CMAKE) -E env $(RUN_ENV) Editor.exe  $(ARGS)
# G01：运行时（Game.exe）。与编辑器完全平行 —— 同一个引擎库，另一个 main。
RUN_GAME    = $(CMAKE) -E chdir $(BIN_DIR) $(CMAKE) -E env $(RUN_ENV) Game.exe    $(ARGS)
# 启动器（Launcher.exe）。同理也是平行进程，**不**是 Editor 的外壳：
# 它在工程还不存在时就要能跑，然后自己 CreateProcess 拉起 Editor / Game。
RUN_LAUNCHER = $(CMAKE) -E chdir $(BIN_DIR) $(CMAKE) -E env $(RUN_ENV) Launcher.exe $(ARGS)

.DEFAULT_GOAL := all

.PHONY: all build configure editor game shaders run run-editor run-game \
        run-launcher toolchain \
        smoke clean distclean rebuild package info help install-make-alias

# ============================================================================
#  构建
# ============================================================================

all: build

## 配置 + 编译全部目标
build: $(CONFIG_STAMP)
	@echo [build] tools=$(TOOLCHAIN) type=$(BUILD_TYPE) out=$(BIN_DIR)
	$(CMAKE_BUILD)

# 只在配置戳缺失时跑 configure
$(CONFIG_STAMP):
	@echo [configure] tools=$(TOOLCHAIN) type=$(BUILD_TYPE) gen=$(GENERATOR)
ifeq ($(TOOLCHAIN),msvc)
	$(MSVC_BUILD) configure
else
	$(CMAKE_CONFIGURE)
	@$(CMAKE) -E touch $(CONFIG_STAMP)
endif

## 强制重新跑一次 CMake 配置（改了 CMAKE_ARGS / 编译器时用）
configure:
	@echo [configure] forced  tools=$(TOOLCHAIN)
ifeq ($(TOOLCHAIN),msvc)
	$(MSVC_BUILD) configure
else
	$(CMAKE_CONFIGURE)
	@$(CMAKE) -E touch $(CONFIG_STAMP)
endif

## 只编译编辑器（editor/ → Editor.exe）
editor: $(CONFIG_STAMP)
	@echo [build] target=VennEditor
ifeq ($(TOOLCHAIN),msvc)
	$(MSVC_BUILD) build VennEditor
else
	$(CMAKE) --build $(BUILD_DIR) --target VennEditor $(PARALLEL)
endif

## 只编译运行时（runtime/ → Game.exe）
game: $(CONFIG_STAMP)
	@echo [build] target=VennRuntime
ifeq ($(TOOLCHAIN),msvc)
	$(MSVC_BUILD) build VennRuntime
else
	$(CMAKE) --build $(BUILD_DIR) --target VennRuntime $(PARALLEL)
endif

## 只重新编译 GLSL 到 SPIR-V
shaders: $(CONFIG_STAMP)
	@echo [build] target=Shaders
ifeq ($(TOOLCHAIN),msvc)
	$(MSVC_BUILD) build Shaders
else
	$(CMAKE) --build $(BUILD_DIR) --target Shaders $(PARALLEL)
endif

# ============================================================================
#  运行
# ============================================================================

## 编译并运行**启动器**（Launcher.exe）—— 项目主入口
## ⚠ 为什么默认是它而不是编辑器：引擎**没有单一的 main()**。三个 exe 平级
##   （launcher/ editor/ runtime/ 各一个 main，只共享 VennEngine 静态库），
##   Windows 这边"启动引擎"这个动作原本只被接到了 Editor.exe 上 —— 于是
##   启动器虽然编得出来，却没有任何入口去跑它。工程是启动器里选/建的，
##   所以"启动引擎"= 启动启动器；想跳过它请显式用 run-editor。
run: build
	@echo [run] Launcher env: $(RUN_ENV)
	$(RUN_LAUNCHER)

## 直接进编辑器（**跳过**启动器）。此时没有工程上下文，资产根按
## MYVK_ASSET_DIR → exe/assets → cwd/assets 的候选链退化，相当于"默认工程"。
run-editor: build
	@echo [run] Editor   env: $(RUN_ENV)
	$(RUN_EDITOR)

## run 的别名（旧脚本 / 肌肉记忆兼容）
run-launcher: run

## 编译并运行**运行时**（Game.exe）—— 出货的那个程序
run-game: build
	@echo [run] Game     env: $(RUN_ENV)
	$(RUN_GAME)

## 无人值守冒烟测试：编辑器跑 SMOKE_FRAMES 帧后自动退出。
## 默认自动进入 Play —— 于是"复制运行态 + 每帧执行脚本 + 渲染到视口"
## 这条链路也一并被验证。
smoke: build
	@echo [smoke] Editor  frames=$(SMOKE_FRAMES) auto-play in viewport
	$(CMAKE) -E chdir $(BIN_DIR) $(CMAKE) -E env MYVK_FRAMES=$(SMOKE_FRAMES) MYVK_EDITOR_PLAY=1 Editor.exe

# ============================================================================
#  清理
# ============================================================================

ifeq ($(wildcard $(BUILD_DIR)/CMakeCache.txt),)
clean:
	@echo [clean] nothing to do: no $(BUILD_DIR)/CMakeCache.txt
else
## 清掉编译产物，保留配置缓存（下次 make build 直接重编，很快）
clean:
	@echo [clean] removing build outputs, keeping $(BUILD_DIR) cache
ifeq ($(TOOLCHAIN),msvc)
	@echo [clean] note: msvc+ninja 没有 clean 目标，改删 bin/ 与中间产物
	$(CMAKE) -E remove_directory $(BUILD_DIR)/bin
else
	$(CMAKE) --build $(BUILD_DIR) --target clean
endif
endif

## 彻底删除 build/ 与 dist/（回到"干净检出"状态）
distclean:
	@echo [distclean] removing $(BUILD_DIR)/ and $(DIST_DIR)/
	$(CMAKE) -E remove_directory $(BUILD_DIR)
	$(CMAKE) -E remove_directory $(DIST_DIR)

## 重新编译一遍（保留配置缓存）
rebuild: clean build

# ============================================================================
#  打包
#  产物目录结构与运行目录一致，双击 exe 即可跑（资源按 exe 同目录解析）：
#    venn-<版本>-<类型>-win64/
#      Editor.exe
#      shaders/     assets/
# ============================================================================

## 编译 + 打包成 zip（发布用；建议配 BUILD_TYPE=Release）
package: build
	@echo [package] staging to $(DIST_DIR)/$(PKG_NAME)
	$(CMAKE) -E remove_directory $(DIST_DIR)/$(PKG_NAME)
	$(CMAKE) -E make_directory $(DIST_DIR)/$(PKG_NAME)
	$(CMAKE) -E copy $(EDITOR) $(DIST_DIR)/$(PKG_NAME)
	$(CMAKE) -E copy $(GAME) $(DIST_DIR)/$(PKG_NAME)
	$(CMAKE) -E copy_directory $(SHADER_DIR) $(DIST_DIR)/$(PKG_NAME)/shaders
	$(CMAKE) -E copy_directory $(ASSET_DIR) $(DIST_DIR)/$(PKG_NAME)/assets
	@echo [package] compressing
	$(CMAKE) -E rm -f $(DIST_DIR)/$(PKG_NAME).zip
	$(CMAKE) -E chdir $(DIST_DIR) $(CMAKE) -E tar cf $(PKG_NAME).zip --format=zip $(PKG_NAME)
	@echo [package] done: $(DIST_DIR)/$(PKG_NAME).zip

# ============================================================================
#  杂项
# ============================================================================

## 打印当前生效的构建参数
info:
	@echo   toolchain    : $(TOOLCHAIN)
	@echo   build type   : $(BUILD_TYPE)
	@echo   generator    : $(GENERATOR)
ifeq ($(TOOLCHAIN),msvc)
	@echo   C compiler   : MSVC cl.exe via vcvars.cmd
	@echo   C++ compiler : MSVC cl.exe via vcvars.cmd
	@echo   VS root      : $(VENN_VS_DIR)
else
	@echo   C compiler   : $(CC_BIN)
	@echo   C++ compiler : $(CXX_BIN)
endif
	@echo   parallel     : $(if $(strip $(JOBS)),$(JOBS),auto)
	@echo   build dir    : $(BUILD_DIR)
	@echo   binaries     : $(BIN_DIR)/Launcher.exe  $(BIN_DIR)/Editor.exe  $(BIN_DIR)/Game.exe
	@echo   package name : $(PKG_NAME)
	@echo   smoke frames : $(SMOKE_FRAMES)

## 把 make 暴露成 PATH 上的裸命令（仅 MinGW 侧有意义）
## MSVC 侧请改用根目录的 make.cmd（它会自动找一份可用的 GNU Make）。
install-make-alias:
ifeq ($(TOOLCHAIN),msvc)
	@echo [alias] toolchain=msvc - nothing to do here.
	@echo [alias] use .\make.cmd ^(or add this dir to PATH^) to run make.
else
	@echo [alias] copy mingw32-make.exe to make.exe in $(MINGW_PREFIX)/bin
	@echo [alias] note: this creates a new file inside your MinGW install
	$(CMAKE) -E copy $(MINGW_PREFIX)/bin/mingw32-make.exe $(MINGW_PREFIX)/bin/make.exe
	@echo [alias] done - open a new terminal to use plain "make"
endif

## 诊断：报告本机可用的构建工具链
toolchain:
	@echo   detected     : $(TOOLCHAIN)
	@echo   make         : this GNU Make process
	@echo   cmake        : $(CMAKE)
ifeq ($(TOOLCHAIN),msvc)
	@echo   VS root      : $(VENN_VS_DIR)
	@echo   vcvars       : $(VENN_VS_DIR)/VC/Auxiliary/Build/vcvars64.bat
else
	@echo   mingw prefix : $(MINGW_PREFIX)
	@echo   g++          : $(CXX_BIN)
endif
	@echo   override     : make TOOLCHAIN=msvc  or  make TOOLCHAIN=mingw

## 列出全部目标
help:
	@echo venn build entry point
	@echo   make                  same as make build
	@echo   make build            configure + build all targets
	@echo   make configure        force re-run CMake configure
	@echo   make editor           build the editor only
	@echo   make game             build the runtime (Game.exe) only
	@echo   make shaders          rebuild GLSL to SPIR-V only
	@echo   ---
	@echo   make run              build + run the launcher (main entry: pick/create a project)
	@echo   make run-editor       build + run the editor directly (skips the launcher)
	@echo   make run-game         build + run the runtime (Game.exe)
	@echo   make smoke            headless smoke test
	@echo   ---
	@echo   make clean            remove build outputs, keep CMake cache
	@echo   make rebuild          clean then build
	@echo   make distclean        remove build/ and dist/ entirely
	@echo   ---
	@echo   make package          build + zip into dist/
	@echo   make info             print current build settings
	@echo   make toolchain        report the detected toolchain
	@echo   make install-make-alias  install a plain "make" command
	@echo   ---
	@echo   variables: TOOLCHAIN BUILD_TYPE JOBS FRAMES VALIDATION PKG_NAME
	@echo   example  : make TOOLCHAIN=msvc build
	@echo   example  : make TOOLCHAIN=mingw build
	@echo   example  : make BUILD_TYPE=Release package
	@echo   example  : make run FRAMES=300 VALIDATION=0
