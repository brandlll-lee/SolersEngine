<p align="center">
  <img src="branding/generated/solers02_icon_transparent_1024.png" width="96" alt="Solers V2 标志" />
</p>

<h1 align="center">Solers</h1>

<p align="center">
  <strong>基于 Godot 的 AI 原生游戏引擎。</strong>
</p>

<p align="center">
  <a href="README.md">English</a> | <strong>简体中文</strong>
</p>

<div align="center">
<table>
<tbody>
<tr>
<td align="center"><a href="#从源码构建"><strong>构建</strong></a></td>
<td align="center"><a href="docs/SOLERS_ARCHITECTURE.md"><strong>架构</strong></a></td>
<td align="center"><a href="docs/UPSTREAM.md"><strong>上游同步</strong></a></td>
<td align="center"><a href="CONTRIBUTING.md"><strong>参与贡献</strong></a></td>
<td align="center"><a href="LICENSE.txt"><strong>许可证</strong></a></td>
</tr>
</tbody>
</table>
</div>

<p align="center">
  Godot 4.7.1 &nbsp; | &nbsp; 标准 Godot 项目 &nbsp; | &nbsp; 开放模型接入
</p>

---

## 项目索引

| 领域 | 内容 | 位置 |
|------|------|------|
| **Solers AI** | Agent 循环、上下文、工具、模型提供商、权限、编辑器 UI 与 MCP 兼容接口。 | [`modules/solers_ai/`](modules/solers_ai/) |
| **引擎** | 作为 Solers 分支持续维护的 Godot 4.7.1 编辑器与运行时。 | [`/`](.) |
| **架构** | 运行时边界、原生写入契约与开发规范。 | [`docs/SOLERS_ARCHITECTURE.md`](docs/SOLERS_ARCHITECTURE.md) |
| **上游同步** | 跟踪、验证和迁移 Godot 新版本的确定性流程。 | [`docs/UPSTREAM.md`](docs/UPSTREAM.md) |
| **测试** | Solers 单元契约与真实编辑器行为测试项目。 | [`modules/solers_ai/tests/`](modules/solers_ai/tests/) |

## AI 原生引擎架构

Solers 是围绕引擎内 AI Agent 构建的 Godot 分支。Agent 是编辑器内的一等操作者，与开发者共享同一个实时项目、场景、资源系统、导入器、调试器、渲染器和运行时生命周期。

这形成了一条统一的原生开发闭环：

**理解 → 修改 → 运行 → 观察 → 验证 → 继续**

人类与 AI 操作同一个权威 Godot 项目。不存在与编辑器竞争的影子场景模型，Agent 与引擎之间也不需要额外的外部自动化层。

## 一个引擎，一个事实来源

Solers 通过 Godot 原生系统应用修改，包括实时场景状态、UndoRedo、资源加载、导入、调试与持久化。状态前置条件防止过期观察覆盖较新的工作，操作回执与检查点则让每次修改都可追踪、可恢复。

项目始终是标准 Godot 项目，可以继续手动编辑、沿用熟悉的工作流，并通过原生引擎工具链发布。

## 原生 Agent Runtime

Solers 的 Agent Runtime 直接建立在 Godot 的编辑器状态、调试器与运行时生命周期之上。Agent 不是执行一次命令就离开的外部控制器，而是能够在引擎状态持续变化时保持任务、证据与行动连续性的开发参与者。

观察、修改、运行与验证构成同一个原生闭环。每一步都以引擎的权威状态为依据，经过权限与状态校验，并留下可追踪、可恢复的操作回执；经过验证的运行时结论可以继续沉淀为场景、资源或脚本修改。

任务可以跨越多轮对话、异步引擎工作与上下文边界持续推进。Solers 保留已经确认的事实和当前意图，而不是因为工具次数或上下文长度中断工作。

## 接入你的模型

模型接入与引擎工具面彼此独立。

| 连接方式 | 集成方式 |
|----------|----------|
| **ChatGPT Codex** | 原生 OAuth 与 Responses 集成。 |
| **模型提供商目录** | 通过声明式提供商、协议、端点和模型元数据接入。 |
| **OpenAI 兼容接口** | 接入私有、托管或网关端点。 |
| **本地模型** | 使用兼容的本地推理端点，无需将项目提示发送到托管服务。 |

## 原生引擎能力面

| 领域 | 能力 |
|------|------|
| **世界** | 检查和修改实时 2D/3D 场景、节点、资源、材质与项目设置。 |
| **运行时** | 运行、暂停、单步，并检查远程场景树、对象、调用栈、错误、性能和渲染结果。 |
| **项目** | 搜索文件、编辑脚本，并检查 Godot 原生类与属性契约。 |
| **资产** | 获取、导入、检查并恢复由引擎管理的异步资产任务。 |
| **安全** | 权限、状态前置条件、操作回执、UndoRedo、检查点与会话日志。 |
| **扩展** | 内置 Skill、经过检查的插件与本地 MCP 兼容引擎接口。 |

## 从源码构建

Solers 正在积极开发，目前通过源码分发。请使用对应平台的标准 [Godot 4.7 构建工具链](https://docs.godotengine.org/zh-cn/4.7/engine_details/development/compiling/index.html)。

```bash
git clone https://github.com/brandlll-lee/SolersEngine.git
cd SolersEngine
```

### Windows

```powershell
python -m SCons platform=windows target=editor dev_build=yes -j4
.\bin\solers.windows.editor.dev.x86_64.exe
```

### Linux/BSD

```bash
scons platform=linuxbsd target=editor dev_build=yes -j4
./bin/solers.linuxbsd.editor.dev.x86_64
```

发行版相关依赖请参阅 Godot 官方 [Linux/BSD 构建指南](https://docs.godotengine.org/zh-cn/4.7/engine_details/development/compiling/compiling_for_linuxbsd.html)。

### macOS

```bash
scons platform=macos target=editor dev_build=yes -j4
```

构建完成后，打开或创建项目，并在编辑器左侧的 Solers 设置视图中连接模型。

## 测试 Solers 模块

启用测试构建编辑器，然后运行 Solers 测试：

```powershell
python -m SCons platform=windows target=editor dev_build=yes tests=yes -j4
.\bin\solers.windows.editor.dev.x86_64.console.exe --test --test-case="*Solers*" --no-colors --minimal
```

编辑器布局行为测试项目位于 [`modules/solers_ai/tests/editor_layout_project/`](modules/solers_ai/tests/editor_layout_project/)。

## 跟踪 Godot 上游

Solers 将分支差异保持为小型、明确的集合，同时把 AI 行为限制在 Solers 模块内部。上游版本通过仓库内的确定性升级协议完成导入、验证与晋升。修改引擎边界或升级 Godot 前，请先阅读 [`docs/UPSTREAM.md`](docs/UPSTREAM.md)。

## 参与贡献

请从[架构说明](docs/SOLERS_ARCHITECTURE.md)与[贡献指南](CONTRIBUTING.md)开始。保持改动聚焦，为编辑器或 Agent 契约补充行为测试，并维持 Solers 自有视觉与 Godot 原生引擎行为之间的边界。

## 许可证

Solers 是 [Godot Engine](https://godotengine.org) 的分支，采用 [MIT 许可证](LICENSE.txt)。归属信息见 [COPYRIGHT.txt](COPYRIGHT.txt)。

Solers 是独立发行版，与 Godot Foundation 不存在隶属或背书关系。
