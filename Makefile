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

# ---------------------------------------------------------------- 工具
CMAKE        ?= cmake
MINGW_PREFIX ?= C:/msys64/ucrt64
CC_BIN       ?= $(MINGW_PREFIX)/bin/gcc.exe
CXX_BIN      ?= $(MINGW_PREFIX)/bin/g++.exe
GENERATOR    ?= Ninja

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
CMAKE_CONFIGURE := $(CMAKE) -S . -B $(BUILD_DIR) -G $(GENERATOR) \
	-DCMAKE_BUILD_TYPE=$(BUILD_TYPE) \
	-DCMAKE_C_COMPILER=$(CC_BIN) \
	-DCMAKE_CXX_COMPILER=$(CXX_BIN) \
	"-DCMAKE_POLICY_VERSION_MINIMUM=3.5" \
	$(CMAKE_ARGS)

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

.DEFAULT_GOAL := all

.PHONY: all build configure editor shaders run run-editor \
        smoke clean distclean rebuild package info help install-make-alias

# ============================================================================
#  构建
# ============================================================================

all: build

## 配置 + 编译全部目标
build: $(CONFIG_STAMP)
	@echo [build] type=$(BUILD_TYPE) out=$(BIN_DIR)
	$(CMAKE) --build $(BUILD_DIR) $(PARALLEL)

# 只在配置戳缺失时跑 configure
$(CONFIG_STAMP):
	@echo [configure] type=$(BUILD_TYPE) gen=$(GENERATOR)
	$(CMAKE_CONFIGURE)
	@$(CMAKE) -E touch $(CONFIG_STAMP)

## 强制重新跑一次 CMake 配置（改了 CMAKE_ARGS / 编译器时用）
configure:
	@echo [configure] forced
	$(CMAKE_CONFIGURE)
	@$(CMAKE) -E touch $(CONFIG_STAMP)

## 只编译编辑器（本工程唯一的可执行文件）
editor: $(CONFIG_STAMP)
	@echo [build] target=Editor
	$(CMAKE) --build $(BUILD_DIR) --target Editor $(PARALLEL)

## 只重新编译 GLSL 到 SPIR-V
shaders: $(CONFIG_STAMP)
	@echo [build] target=Shaders
	$(CMAKE) --build $(BUILD_DIR) --target Shaders $(PARALLEL)

# ============================================================================
#  运行
# ============================================================================

## 编译并运行编辑器（Editor）—— 项目主入口
run: build
	@echo [run] Editor   env: $(RUN_ENV)
	$(RUN_EDITOR)

## run 的别名（旧脚本兼容）
run-editor: run

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
	$(CMAKE) --build $(BUILD_DIR) --target clean
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
	@echo   build type   : $(BUILD_TYPE)
	@echo   generator    : $(GENERATOR)
	@echo   C compiler   : $(CC_BIN)
	@echo   C++ compiler : $(CXX_BIN)
	@echo   parallel     : $(if $(strip $(JOBS)),$(JOBS),auto)
	@echo   build dir    : $(BUILD_DIR)
	@echo   binaries     : $(BIN_DIR)/Editor.exe
	@echo   package name : $(PKG_NAME)
	@echo   smoke frames : $(SMOKE_FRAMES)

## 把 mingw32-make 暴露成 make，之后就能直接敲 make build
install-make-alias:
	@echo [alias] copy mingw32-make.exe to make.exe in $(MINGW_PREFIX)/bin
	@echo [alias] note: this creates a new file inside your MinGW install
	$(CMAKE) -E copy $(MINGW_PREFIX)/bin/mingw32-make.exe $(MINGW_PREFIX)/bin/make.exe
	@echo [alias] done - open a new terminal to use plain "make"

## 列出全部目标
help:
	@echo venn build entry point
	@echo   make                  same as make build
	@echo   make build            configure + build all targets
	@echo   make configure        force re-run CMake configure
	@echo   make editor           build the editor only
	@echo   make shaders          rebuild GLSL to SPIR-V only
	@echo   ---
	@echo   make run              build + run the editor (main entry)
	@echo   make smoke            headless smoke test
	@echo   ---
	@echo   make clean            remove build outputs, keep CMake cache
	@echo   make rebuild          clean then build
	@echo   make distclean        remove build/ and dist/ entirely
	@echo   ---
	@echo   make package          build + zip into dist/
	@echo   make info             print current build settings
	@echo   make install-make-alias  install a plain "make" command
	@echo   ---
	@echo   variables: BUILD_TYPE JOBS FRAMES VALIDATION PKG_NAME
	@echo   example  : make BUILD_TYPE=Release package
	@echo   example  : make run FRAMES=300 VALIDATION=0
