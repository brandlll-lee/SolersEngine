# Solers v0.1 实现报告

日期：2026-06-06  
Godot 基线：`4.6.3-stable`  
Fork 路径：`F:\CodeHub\solers\godot-ai-native`

## 本轮结论

Solers v0.1 已经从“品牌化 Godot fork”推进到“可编译、可启动、可审计、可由 AI 以工具方式操控真实 Godot Editor 状态的原生 operator runtime 底座”。

这不是最终商业化 AI 游戏引擎产品态，但已经完成 v0.1 应该具备的内核级基础：品牌化发行版、内置 C++ 模块、Solers Dock、Editor 状态观察、场景/节点/脚本/资源/导出/运行/验证工具、权限审批、Action Timeline、Agent tool loop、Provider Registry、MCP-compatible adapter，以及显式 opt-in 的本地 JSONL RPC loopback server。

## 发行版品牌基础

- 通过 `solers_branding.py` 集中管理 Solers 品牌信息。
- 已完成 Windows/macOS/Linux/Web 构建与打包元数据的 Solers 命名接入。
- 按要求暂时保留 Godot logo/icon 资产。
- 已验证编辑器启动标识为 `Solers Engine`，用户数据目录走 `AppData\Roaming\Solers`。

## 原生 Solers 模块

新增并编译通过内置模块：

```text
modules/solers_ai/
  config.py
  SCsub
  register_types.h
  register_types.cpp
  core/
    solers_action_timeline.*
    solers_agent_runtime.*
    solers_editor_operator.*
    solers_file_checkpoint.*
    solers_observation_service.*
    solers_permission_manager.*
    solers_provider_registry.*
    solers_resource_service.*
    solers_script_service.*
    solers_settings_service.*
    solers_tool_registry.*
  editor/
    solers_dock.*
    solers_editor_plugin.*
  protocol/
    solers_mcp_adapter.*
    solers_rpc_server.*
```

该模块通过 Godot 原生 `EditorPlugins::add_by_type<SolersEditorPlugin>()` 注册，Solers 能力属于引擎发行版内部能力，而不是项目级 addon。

## Godot 内核小幅下沉

为让 Solers 能读取真实编辑器输出，而不是猜测运行错误，本轮给 `EditorLog` 增加了只读快照 API：

- `EditorLog::get_messages(int p_max_messages)`
- `EditorLog::get_message_counts()`

改动集中在：

- `editor/editor_log.h`
- `editor/editor_log.cpp`

这是小而关键的下沉：运行输出、警告、错误、调试器消息现在能成为 Solers Agent 的正式观察源。

## Editor 观察服务

`SolersObservationService` 已实现：

- 项目信息与项目设置摘要。
- `res://` 项目文件列表、搜索和边界受限读取。
- 打开场景列表与当前编辑场景树序列化。
- 当前选择节点序列化。
- 运行状态读取。
- Editor 输出日志读取与错误/警告计数。
- 聚合 editor snapshot。

## Resource 与 Export 服务

新增 `SolersResourceService`：

- `resource.get_info`：读取资源类型、UID、脚本类、import 状态、依赖列表。
- `export.list_presets`：读取当前 export platform 与 export preset。
- `export.validate_presets`：调用 Godot `EditorExportPlatform::can_export()` 验证导出配置，不生成构建产物。

这为后续资源依赖图、素材 license tracking、导出自动化提供了干净入口。

## Provider Registry 与 BYOK 配置

新增 `SolersProviderRegistry`，用于把 v0.1 的 BYOK 配置从“存字符串”升级为“可审计、可验证、可扩展的 provider profile”。

当前内置 profile：

- `openai`
- `anthropic`
- `gemini`
- `deepseek`
- `qwen`
- `ollama`
- `lm_studio`
- `custom_openai_compatible`

每个 profile 记录 provider id、label、kind、默认 base URL、默认 model、是否本地 provider、是否需要 API key、能力族和设计备注。

`SolersSettingsService` 现在会调用 Provider Registry 做离线验证：

- `privacy_mode=true` 时阻止远程 provider。
- 需要 API key 的 provider 会检查 `api_key_configured`。
- OpenAI-compatible provider 会检查 `base_url`。
- 检查 `base_url` 必须以 `http://` 或 `https://` 开头。
- 对远程 `http://` base URL 发出 warning。

当前仍不做真实模型推理调用；这是有意保守边界。真实 Provider Gateway 应单独实现请求调度、流式解析、工具调用解析、错误归一化、成本估算和隐私审计，而不是塞进 SettingsService。

## 工具注册表

`SolersToolRegistry` 当前注册 52 个 v0.1 工具：

```text
project.get_info
project.get_settings_summary
project.list_files
project.search_files
project.read_file
project.write_file
script.read
script.write
script.patch
script.create
script.validate
script.open_in_editor
scene.get_open_scenes
scene.open
scene.create
scene.get_tree
scene.save
scene.save_as
selection.get_nodes
node.get_properties
node.add
node.set_properties
node.reparent
node.attach_script
node.connect_signal
node.list_signal_connections
node.remove
runtime.get_status
runtime.get_logs
runtime.play_current_scene
runtime.stop
runtime.capture_screenshot
editor.get_snapshot
editor.get_logs
editor.capture_screenshot
timeline.list_actions
timeline.rollback_last
validation.validate_project_scripts
validation.assert_no_errors
validation.read_editor_errors
validation.run_scene_smoke
resource.get_info
export.list_presets
export.validate_presets
provider.get_config
provider.list_profiles
provider.validate_config
provider.set_config
approvals.list_pending
rpc.get_status
rpc.start
rpc.stop
```

所有工具统一返回 `{ ok, data/error }` 结构，统一走权限检查、时间线记录和 MCP adapter 暴露。新增的 provider profile 工具不发起网络请求，只读取和验证本地配置/能力元数据。

## 权限边界

`SolersPermissionManager` 已实现：

- `observe` 默认自动批准。
- `edit_scene`、`edit_files`、`run_project`、`import_assets`、`export_build`、`network`、`shell` 默认需要批准。
- Dock 提供开发期 toggle：场景修改、文件写入、运行控制。
- 未自动批准的工具调用会生成 pending approval request，返回 `approval_id`。
- Dock 支持批准或拒绝最早的 pending request；批准后工具可带 `approval_id` 重试一次。
- 一次性批准现在绑定工具名，避免审批 id 被错用到另一个工具。
- `provider.set_config` 的 `api_key`、`rpc.start` 的 `session_token` 在 timeline 记录前已脱敏。
- `provider.get_config` 永远返回 `api_key=<redacted>` 与 `api_key_configured`，不回传真实 key。

## Action Timeline

`SolersActionTimeline` 已实现：

- 记录工具调用开始、完成、拦截。
- 记录文件 checkpoint、文件写入、Agent turn 等事件。
- 每个事件包含 id、类型、Unix 时间戳、monotonic msec 时间戳和 payload。
- `timeline.rollback_last` 目前映射到 Godot `EditorUndoRedoManager::undo()`，后续会升级为 action-scoped rollback。

## 文件与脚本闭环

`SolersFileCheckpoint`：

- 文件写入前在 `res://.godot/solers/checkpoints/` 创建 checkpoint。
- 支持 checkpoint restore 基础接口。
- checkpoint 不污染标准 Godot 场景/资源文件。

`SolersScriptService`：

- 安全写入项目文本文件。
- 精确文本 patch，支持 `expected_sha256` 防止覆盖用户并发改动。
- 调用 Godot 注册的 `ScriptLanguage::validate()` 做真实语言验证。
- 支持全项目脚本验证。
- 支持打开脚本到 Godot ScriptEditor。

## Editor Operator

`SolersEditorOperator` 已覆盖核心可变更工作流：

- 创建新场景根节点。
- 节点创建、删除、属性修改、重挂父子关系。
- 脚本附加。
- 持久信号连接和连接列表读取。
- 场景打开、保存、另存。
- 运行/停止当前场景。
- 编辑器截图。
- Godot UndoRedo rollback。

场景/节点 mutation 均优先走 Godot editor API 与 `EditorUndoRedoManager`，避免直接写坏 editor 状态。

## Agent Runtime

`SolersAgentRuntime` 已实现 v0.1 同步工具循环：

- turn id 与 agent state。
- `idle/running/waiting_for_approval/completed/failed/aborted` 状态。
- 工具 batch 顺序执行。
- 遇到 `USER_APPROVAL_REQUIRED` 自动进入等待审批状态。
- abort 入口。
- 每轮记录到 Action Timeline。

`SolersMCPAdapter` 额外暴露 Agent 控制方法：

- `solers/agent/start_turn`
- `solers/agent/status`
- `solers/agent/abort`

这是后续 Planner/Executor/Verifier/Reporter 组合的底座；当前不绑定外部模型 provider。

## MCP / JSON-RPC / RPC Loopback

`SolersMCPAdapter` 已实现 MCP-compatible 内存 adapter：

- `initialize`
- `tools/list`
- `tools/call`
- `resources/list`
- `resources/read`
- `prompts/list`
- `ping`
- `solers/status`
- `solers/agent/*`

新增 `SolersRpcServer`：

- JSONL over TCP，每行一个 JSON-RPC request。
- 默认不启动，必须显式调用 `rpc.start`。
- 仅允许绑定 `127.0.0.1` 或 `::1`。
- 默认 session token 认证。
- `rpc.start` 返回一次性可见 token，后续 status 中脱敏。
- `rpc.stop` 可关闭 server 并断开 client。

这为外部 MCP client、Codex/OpenCode 类 agent、Python gateway 提供了安全 loopback 基础，同时避免 v0.1 默认暴露网络面。

## Solers Dock

Dock 已展示：

- 项目、运行、场景、选择状态。
- 工具数量与权限状态。
- Agent Runtime 状态。
- Pending approval 数量。
- MCP adapter 与 RPC loopback 状态。
- Timeline 事件数。
- Provider 配置状态：provider、隐私模式、key 是否配置、验证结果、warning/blocker 数。
- Provider 配置通过工具可读、可列出 profile、可离线验证。
- Snapshot 刷新。
- 内部 runtime probe：在不调用任何模型的情况下执行一批真实 Solers 工具。
- Abort Agent Turn。
- Approve Next / Reject Next。

## 验证

构建命令：

```powershell
python -m SCons platform=windows target=editor dev_build=yes debug_symbols=yes arch=x86_64 progress=no
```

结果：

```text
Linking Program bin\solers.windows.editor.dev.x86_64.exe
Linking Program bin\solers.windows.editor.dev.x86_64.console.exe
scons: done building targets.
INFO: Time elapsed: 00:00:47.78
```

Headless editor smoke：

```powershell
bin\solers.windows.editor.dev.x86_64.console.exe --headless --editor --quit --path F:\CodeHub\solers\validation\godot-smoke-project
```

结果：

```text
Solers Engine v4.6.3.stable.custom_build.35e80b3a8 - https://solers.ai
[ DONE ] first_scan_filesystem
[ DONE ] loading_editor_layout
```

清洁检查：

- 已清理 `modules/solers_ai/__pycache__`。
- `.codegraph/` 是本地索引状态，不应提交。
- 新增 Solers 代码集中在 `modules/solers_ai`。
- 当前工具注册数复核为 `52`。

## 仍未完成的 v0.1 深水区

当前版本已经完成 Solers v0.1 的原生 operator runtime 底座和可推进闭环，但还不是“AI 自动完成完整商业游戏”的产品态。

下一批硬任务：

- 外部模型 Provider Gateway：OpenAI/Anthropic/Gemini/DeepSeek/Qwen/Ollama/LM Studio 的 BYOK 实际调用。当前已完成 Provider Registry、配置存储、profile 列表与离线验证。
- 更完整的审批 UI：每个待批准工具调用显示参数、影响范围、diff/checkpoint 和允许/拒绝按钮；当前已支持 pending queue 与一次性批准/拒绝。
- 真正 game-view runtime bridge：接入 Godot debugger game-view screenshot、运行时日志、错误树、节点查询；当前 `runtime.capture_screenshot` 是 editor-visible viewport capture。
- Visual Verification Loop：运行场景、截图、读取错误、回传 verifier，并自动修复。
- 更细粒度 rollback：把 Timeline event 与 UndoRedo/file checkpoint 映射成可选择回滚。
- 自动化 editor integration tests：节点 mutation、保存重开、UndoRedo、script patch、provider key 脱敏、RPC auth。

## 工程结论

Solers v0.1 已建立 AI-native game engine 最重要的内核地基：AI 不再只是外部聊天助手，而是可以通过 Solers 原生工具观察、修改、运行、验证真实 Godot Editor 状态的受控操作者。

下一阶段的重点不是盲目堆更多工具，而是把 provider、审批 UX、运行时验证和自动化测试做扎实，让 Solers 的每一次 AI 操作都能被用户理解、验证、停止、回滚。
