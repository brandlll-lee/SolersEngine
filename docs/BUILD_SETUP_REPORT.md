# Solers Godot 4.6.3 源码环境构建报告

记录时间：2026-06-06  
工作目录：`F:\CodeHub\solers`  
源码目录：`F:\CodeHub\solers\godot-ai-native`  
目标：获得一份干净、可调试、可深度修改的 Godot 4.6.3 Windows Editor 源码环境，为后续 Solers AI 原生引擎改造做准备。

## 1. 官方资料与工具调研

本次构建前使用 Tavily 与 Exa 检索并核对了 Godot 官方文档。当前会话没有可调用的 Context7 工具，因此 Context7 未实际执行；证据链由 Godot 官方文档、源码扫描、CodeGraph 索引和本地构建日志共同组成。

主要依据：

- Godot 官方 Windows 编译文档：<https://docs.godotengine.org/en/stable/engine_details/development/compiling/compiling_for_windows.html>
- Godot latest Windows 编译文档：<https://docs.godotengine.org/en/latest/engine_details/development/compiling/compiling_for_windows.html>
- Godot latest build system 文档：<https://docs.godotengine.org/en/latest/engine_details/development/compiling/introduction_to_the_buildsystem.html>
- Godot 4.6 Visual Studio 配置文档：<https://docs.godotengine.org/en/4.6/engine_details/development/configuring_an_ide/visual_studio.html>

关键结论：

- Godot 使用 SCons 构建；Windows 上推荐 MSVC/Visual Studio Build Tools 进行引擎开发。
- `target=editor` 构建包含 Project Manager 和 Editor 的编辑器二进制。
- `dev_build=yes` 适合引擎开发：启用 `DEV_ENABLED`，关闭优化，生成调试符号，并输出 `.dev` 后缀二进制。
- `debug_symbols=yes` 保证调试器、性能分析器和崩溃栈有符号信息；MSVC 下符号输出为 `.pdb`。
- Windows 默认启用 Direct3D 12；如果不显式 `d3d12=no`，需要安装 D3D12 构建依赖。
- 可通过 `vsproj=yes` 生成 Visual Studio solution，便于后续 C++ 断点调试和源码导航。

## 2. 本机工具链状态

已确认工具链：

- Git：`2.49.0.windows.1`
- Python：`3.11.9`
- SCons：`4.10.1`
- Visual Studio Build Tools：2022，版本 `17.14.2+36121.58`
- MSVC 编译器：`19.44.35207.1`
- MSVC Linker：`14.44.35207.1`
- Windows SDK：`10.0.26100.0`
- CPU：16 线程
- 内存：约 16 GB

SCons 最初不在 PATH 中，已通过以下命令安装到用户 Python Scripts 目录：

```powershell
python -m pip install --user --upgrade scons
```

后续构建命令中临时把 `C:\Users\ASUS\AppData\Roaming\Python\Python311\Scripts` 加入 PATH，避免污染系统级配置。

## 3. Clone 与 Checkout

最初执行完整 clone 时，Git 在 `index-pack` 阶段因为内存不足失败：

```text
fatal: Out of memory, malloc failed
fatal: fetch-pack: invalid index-pack output
```

处理方式：改用 tag 定向浅克隆，拿到完整 4.6.3 工作树，同时避免拉取完整历史造成内存压力。

```powershell
git clone --branch 4.6.3-stable --single-branch --depth 1 https://github.com/godotengine/godot.git godot-ai-native
cd godot-ai-native
git switch -c solers/4.6.3-ai-native-base
```

当前源码状态：

```text
remote: https://github.com/godotengine/godot.git
branch: solers/4.6.3-ai-native-base
tag: 4.6.3-stable
HEAD: 35e80b3a8822a9df9be390814b62f44c0a9c69e8
```

说明：当前是完整源码工作树，但不是完整 Git 历史。后续如果需要长期维护 upstream rebase，可以在内存充足时执行 `git fetch --unshallow` 或重新以更高内存环境做完整 clone。

## 4. 源码结构与 AI 集成切入点

源码文件规模：

```text
all=13795
editor=1791
modules=3116
core=444
```

核心目录：

- `core`：对象系统、Variant、资源、IO、基础服务。
- `scene`：Node/SceneTree/UI/2D/3D 场景系统。
- `editor`：编辑器主体、插件、Inspector、FileSystem、运行、导出、调试器。
- `modules`：GDScript、Mono、Jolt、WebSocket、JSONRPC、OpenXR 等模块。
- `platform/windows`：Windows 平台检测、MSVC、Direct3D 12 依赖检查。
- `servers`：渲染、物理、音频、导航等底层服务。
- `SConstruct`：顶层构建配置。

CodeGraph 索引状态：

```text
Files indexed: 9623
Total nodes: 273807
Total edges: 913957
Database size: 611.88 MB
```

对 Solers AI 原生操作者最重要的现有扩展点：

- `editor/editor_interface.h`
  - `get_selection`
  - `get_editor_undo_redo`
  - `get_file_system_dock`
  - `get_inspector`
  - `get_editor_viewport_2d`
  - `get_editor_viewport_3d`
  - `open_scene_from_path`
  - `get_open_scenes`
  - `get_open_scene_roots`
  - `get_edited_scene_root`
  - `add_root_node`
  - `save_scene`
  - `save_scene_as`
  - `save_all_scenes`
  - `play_main_scene`
  - `play_current_scene`
  - `play_custom_scene`
  - `stop_playing_scene`
- `editor/plugins/editor_plugin.h`
  - `add_control_to_dock`
  - `add_control_to_bottom_panel`
  - `add_tool_menu_item`
  - `add_custom_type`
  - `add_import_plugin`
  - `add_export_plugin`
  - `add_inspector_plugin`
  - `add_debugger_plugin`
  - `get_editor_interface`
  - `get_undo_redo`
- `editor/editor_undo_redo_manager.cpp`
  - `create_action`
  - `commit_action`
  - `add_do_property`
  - `add_undo_property`
  - `get_object_history_id`
  - `get_history_undo_redo`
  - `clear_history`
- `editor/export/editor_export.h`
  - 后续可作为导出 pipeline hooks 的核心入口。

初步判断：

- Phase 1 的 Solers Dock/MCP bridge 可以优先用 `EditorPlugin + EditorInterface + EditorUndoRedoManager` 实现。
- Phase 2 再把插件碰到的 API 边界下沉到 C++，例如统一命令总线、事务、viewport 截图、运行时观察、资源依赖图、导出 hooks。

## 5. D3D12 依赖安装

第一次构建失败原因：

```text
ERROR: The Direct3D 12 rendering driver requires dependencies to be installed.
You can install them by running `python misc\scripts\install_d3d12_sdk_windows.py`.
Alternatively, disable this driver by compiling with `d3d12=no` explicitly.
```

为保留完整 Windows 渲染能力，本次没有使用 `d3d12=no`，而是安装官方脚本依赖：

```powershell
python misc\scripts\install_d3d12_sdk_windows.py
```

安装位置：

```text
C:\Users\ASUS\AppData\Local\Godot\build_deps
```

安装内容包括：

- Mesa NIR 编译依赖
- DirectX 12 Agility SDK `1.618.5`
- WinPixEventRuntime `1.0.240308001`

日志：

```text
F:\CodeHub\solers\build_logs\godot-4.6.3-install-d3d12-deps.log
```

## 6. Editor 构建

使用 Visual Studio 2022 Build Tools 的 `vcvars64.bat` 初始化 x64 MSVC 环境。

最终构建命令：

```powershell
$env:Path = 'C:\Users\ASUS\AppData\Roaming\Python\Python311\Scripts;' + $env:Path
cmd /c "call ""C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"" && scons platform=windows target=editor dev_build=yes debug_symbols=yes arch=x86_64 -j8 progress=yes verbose=no"
```

参数说明：

- `platform=windows`：Windows 平台。
- `target=editor`：构建编辑器。
- `dev_build=yes`：开发构建，适合后续改 C++ 和断点调试。
- `debug_symbols=yes`：显式保留调试符号。
- `arch=x86_64`：64 位构建。
- `-j8`：本机 16 线程，但考虑 16 GB 内存和 Godot 编译峰值，使用 8 并发更稳。

构建结果：

```text
scons: done building targets.
INFO: Time elapsed: 00:09:54.100
exit code: 0
```

构建日志：

```text
F:\CodeHub\solers\build_logs\godot-4.6.3-editor-dev-msvc-build.log
```

主要输出：

```text
F:\CodeHub\solers\godot-ai-native\bin\godot.windows.editor.dev.x86_64.exe
F:\CodeHub\solers\godot-ai-native\bin\godot.windows.editor.dev.x86_64.pdb
F:\CodeHub\solers\godot-ai-native\bin\godot.windows.editor.dev.x86_64.console.exe
F:\CodeHub\solers\godot-ai-native\bin\godot.windows.editor.dev.x86_64.console.pdb
```

文件大小：

```text
godot.windows.editor.dev.x86_64.exe         240,228,352 bytes
godot.windows.editor.dev.x86_64.pdb         557,273,088 bytes
godot.windows.editor.dev.x86_64.console.exe     640,512 bytes
godot.windows.editor.dev.x86_64.console.pdb   6,172,672 bytes
```

版本验证：

```text
4.6.3.stable.custom_build.35e80b3a8
```

## 7. Visual Studio 工程生成

为后续 C++ 调试生成了 Solers 命名的 Visual Studio solution：

```powershell
scons platform=windows target=editor dev_build=yes debug_symbols=yes arch=x86_64 vsproj=yes vsproj_name=solers-godot progress=no
```

生成物：

```text
F:\CodeHub\solers\godot-ai-native\solers-godot.sln
F:\CodeHub\solers\godot-ai-native\solers-godot.vcxproj
F:\CodeHub\solers\godot-ai-native\solers-godot.vcxproj.filters
F:\CodeHub\solers\godot-ai-native\solers-godot.windows.editor.x86_64.generated.props
```

这些文件被 Godot `.gitignore` 规则忽略，不污染源码提交。

## 8. 自包含 Editor 设置

已创建：

```text
F:\CodeHub\solers\godot-ai-native\bin\._sc_
```

用途：让这份自编译 Godot 使用 `bin` 下的自包含编辑器配置，避免混用本机官方 Godot 的 editor settings。该文件位于已忽略的 `bin/` 目录中，不污染源码提交。

## 9. Smoke 验证

验证项目：

```text
F:\CodeHub\solers\validation\godot-smoke-project
```

### 9.1 运行时脚本验证

命令：

```powershell
F:\CodeHub\solers\godot-ai-native\bin\godot.windows.editor.dev.x86_64.console.exe --headless --path F:\CodeHub\solers\validation\godot-smoke-project
```

结果：

```text
Godot Engine v4.6.3.stable.custom_build.35e80b3a8 (2026-05-20 13:47:53 UTC) - https://godotengine.org
SOLERS_SMOKE_OK version=4.6.3-stable (custom_build) status=stable hash=35e80b3a8822a9df9be390814b62f44c0a9c69e8
exit code: 0
```

日志：

```text
F:\CodeHub\solers\build_logs\godot-4.6.3-runtime-smoke.log
```

### 9.2 Headless Editor 启动验证

命令：

```powershell
F:\CodeHub\solers\godot-ai-native\bin\godot.windows.editor.dev.x86_64.console.exe --headless --editor --path F:\CodeHub\solers\validation\godot-smoke-project --quit-after 5
```

结果：

```text
[ DONE ] first_scan_filesystem
[ DONE ] loading_editor_layout
exit code: 0
```

日志：

```text
F:\CodeHub\solers\build_logs\godot-4.6.3-headless-editor-smoke-5s.log
```

### 9.3 可视化渲染截图验证

命令运行自编译 Godot，用 OpenGL3 打开 smoke 项目并保存一张 PNG：

```powershell
$env:SOLERS_SMOKE_SCREENSHOT='F:\CodeHub\solers\build_logs\godot-4.6.3-visual-smoke.png'
F:\CodeHub\solers\godot-ai-native\bin\godot.windows.editor.dev.x86_64.console.exe --path F:\CodeHub\solers\validation\godot-smoke-project --resolution 640x360 --windowed --rendering-driver opengl3 --quit-after 120
```

结果：

```text
OpenGL API 3.3.0 NVIDIA 591.86 - Compatibility - Using Device: NVIDIA - NVIDIA GeForce RTX 4050 Laptop GPU
SOLERS_SCREENSHOT path=F:\CodeHub\solers\build_logs\godot-4.6.3-visual-smoke.png error=0
SOLERS_SMOKE_OK version=4.6.3-stable (custom_build) status=stable hash=35e80b3a8822a9df9be390814b62f44c0a9c69e8
exit code: 0
```

截图：

```text
F:\CodeHub\solers\build_logs\godot-4.6.3-visual-smoke.png
```

注意：这张图是运行时窗口渲染验证，不是 Godot Editor UI 截图；Editor UI 已通过 headless editor 初始化日志验证。

## 10. Git 与生成物状态

Godot 源码仓库当前分支：

```text
## solers/4.6.3-ai-native-base
?? .codegraph/
```

说明：

- Godot 源码本身没有被修改。
- `.codegraph/` 是本地源码分析索引，未跟踪。
- 构建输出、SCons 中间文件、VS solution、`bin/._sc_` 均被 `.gitignore` 忽略。

当前没有残留的 `scons`、`cl`、`link`、`godot`、`codegraph`、`git` 构建进程。

## 11. 可复现构建步骤

从当前状态重新构建：

```powershell
cd F:\CodeHub\solers\godot-ai-native
$env:Path = 'C:\Users\ASUS\AppData\Roaming\Python\Python311\Scripts;' + $env:Path
cmd /c "call ""C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"" && scons platform=windows target=editor dev_build=yes debug_symbols=yes arch=x86_64 -j8 progress=yes verbose=no"
```

如果 D3D12 依赖丢失：

```powershell
python misc\scripts\install_d3d12_sdk_windows.py
```

生成 Visual Studio solution：

```powershell
scons platform=windows target=editor dev_build=yes debug_symbols=yes arch=x86_64 vsproj=yes vsproj_name=solers-godot progress=no
```

验证二进制：

```powershell
bin\godot.windows.editor.dev.x86_64.console.exe --version
bin\godot.windows.editor.dev.x86_64.console.exe --headless --editor --path F:\CodeHub\solers\validation\godot-smoke-project --quit-after 5
bin\godot.windows.editor.dev.x86_64.console.exe --headless --path F:\CodeHub\solers\validation\godot-smoke-project
```

## 12. 后续建议

下一步建议：

1. 把远程从 `godotengine/godot` 切换为你的 Solers fork，并把 upstream 保留为官方 Godot：

```powershell
git remote rename origin upstream
git remote add origin <your-solers-fork-url>
```

2. 建立 Phase 0 分支：

```powershell
git switch -c solers/phase0-branding-and-ai-dock
```

3. 优先实现插件层 Solers Dock/MCP bridge，不急着改核心：

- BYOK provider 配置。
- 内部 WebSocket/MCP command bridge。
- scene tree inspect。
- add/remove/reparent node。
- property get/set。
- script read/write。
- editor log read。
- run current scene。
- screenshot capture。
- save scene。
- undo/redo。

4. 只有当插件层 API 不够用时，再进入 C++ 核心 fork：

- `editor/solers/` 或 `modules/solers_ai/` 放置 Solers 原生服务。
- 扩展 `EditorInterface` 暴露缺失能力。
- 封装 AI Action Timeline 和统一事务。
- 加入权限、审计、回滚和验证闭环。

当前结论：Godot 4.6.3 源码环境、Windows MSVC dev editor 构建、调试符号、Visual Studio 工程、CodeGraph 索引、运行时 smoke、headless editor smoke、可视化渲染 smoke 均已完成。该环境可以作为 Solers AI 原生深度集成的干净工程基线。
