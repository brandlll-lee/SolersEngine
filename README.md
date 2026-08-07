<p align="center">
  <img src="branding/generated/solers02_icon_transparent_1024.png" width="96" alt="Solers V2 logo" />
</p>

<h1 align="center">Solers</h1>

<p align="center">
  <strong>The AI-native game engine built on Godot.</strong>
</p>

<p align="center">
  <strong>English</strong> | <a href="README.zh-CN.md">简体中文</a>
</p>

<div align="center">
<table>
<tbody>
<tr>
<td align="center"><a href="#build-from-source"><strong>Build</strong></a></td>
<td align="center"><a href="docs/SOLERS_ARCHITECTURE.md"><strong>Architecture</strong></a></td>
<td align="center"><a href="docs/UPSTREAM.md"><strong>Upstream</strong></a></td>
<td align="center"><a href="CONTRIBUTING.md"><strong>Contributing</strong></a></td>
<td align="center"><a href="LICENSE.txt"><strong>License</strong></a></td>
</tr>
</tbody>
</table>
</div>

<p align="center">
  Godot 4.7.1 &nbsp; | &nbsp; Standard Godot projects &nbsp; | &nbsp; Open model access
</p>

---

## Project Index

| Area | What it contains | Location |
|------|------------------|----------|
| **Solers AI** | Agent loop, context, tools, providers, permissions, editor UI, and MCP-compatible surface. | [`modules/solers_ai/`](modules/solers_ai/) |
| **Engine** | The Godot 4.7.1 editor and runtime maintained as the Solers fork. | [`/`](.) |
| **Architecture** | Runtime boundaries, native write contracts, and development guidance. | [`docs/SOLERS_ARCHITECTURE.md`](docs/SOLERS_ARCHITECTURE.md) |
| **Upstream** | Deterministic protocol for tracking and validating new Godot releases. | [`docs/UPSTREAM.md`](docs/UPSTREAM.md) |
| **Tests** | Unit contracts and real editor behavior projects for Solers. | [`modules/solers_ai/tests/`](modules/solers_ai/tests/) |

## AI-Native Engine Architecture

Solers is a Godot fork built around an in-engine AI agent. The agent is a
first-class editor operator with access to the same live project, scene,
resource, importer, debugger, renderer, and runtime lifecycle as the developer.

This creates one native development loop:

**Understand → change → run → observe → verify → continue**

Human and AI work on the same authoritative Godot project. There is no shadow
scene model competing with the editor and no external automation layer between
the agent and the engine.

## One Engine, One Source of Truth

Solers applies changes through Godot-native systems, including live scene state,
UndoRedo, resource loading, importing, debugging, and persistence. State
preconditions prevent stale observations from overwriting newer work, while
receipts and checkpoints make mutations traceable and recoverable.

Projects remain standard Godot projects that can be edited manually, opened with
familiar workflows, and shipped through the engine toolchain.

## Native Agent Runtime

Solers Agent Runtime is built directly on Godot's editor state, debugger, and
runtime lifecycle. The agent is not an external controller that leaves after
issuing a command, but a development participant that maintains continuity of
task, evidence, and action as engine state changes.

Observation, modification, execution, and verification form one native loop.
Each step is grounded in authoritative engine state, checked against permissions
and state preconditions, and recorded in traceable, recoverable receipts.
Verified runtime findings can continue into scene, resource, or script changes.

Tasks can continue across turns, asynchronous engine work, and context
boundaries. Solers preserves confirmed facts and current intent instead of
interrupting work at a tool-count or context limit.

## Bring Your Model

Model access is independent from the engine tool surface.

| Connection | Integration |
|------------|-------------|
| **ChatGPT Codex** | Native OAuth and Responses integration. |
| **Provider catalog** | Declared providers, protocols, endpoints, and model metadata. |
| **OpenAI-compatible** | Private, hosted, or gateway endpoints. |
| **Local models** | Compatible local inference without hosted project prompts. |

## Native Engine Surface

| Domain | Capabilities |
|--------|--------------|
| **World** | Inspect and modify live 2D/3D scenes, nodes, resources, materials, and project settings. |
| **Runtime** | Run, pause, step, inspect the remote tree, objects, stacks, errors, performance, and rendered output. |
| **Project** | Search files, edit scripts, and inspect native Godot class and property contracts. |
| **Assets** | Acquire, import, inspect, and resume engine-owned asset jobs. |
| **Safety** | Permissions, state preconditions, receipts, UndoRedo, checkpoints, and session journals. |
| **Extensions** | Built-in skills, inspected addons, and a local MCP-compatible engine interface. |

## Build from Source

Solers is under active development and is currently distributed from source.
Use the standard
[Godot 4.7 build toolchain](https://docs.godotengine.org/en/4.7/engine_details/development/compiling/index.html)
for your platform.

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

See the official
[Linux/BSD build guide](https://docs.godotengine.org/en/4.7/engine_details/development/compiling/compiling_for_linuxbsd.html)
for distribution-specific dependencies.

### macOS

```bash
scons platform=macos target=editor dev_build=yes -j4
```

After the build, open or create a project and connect a model from the Solers
setup view in the editor's left panel.

## Test the Solers Module

Build the editor with tests enabled, then run the Solers test cases:

```powershell
python -m SCons platform=windows target=editor dev_build=yes tests=yes -j4
.\bin\solers.windows.editor.dev.x86_64.console.exe --test --test-case="*Solers*" --no-colors --minimal
```

The editor layout behavior project lives at
[`modules/solers_ai/tests/editor_layout_project/`](modules/solers_ai/tests/editor_layout_project/).

## Tracking Godot

Solers preserves a small, explicit fork delta while keeping AI behavior inside
the Solers module. Upstream releases are imported, verified, and promoted using
the repository's deterministic upgrade protocol. Read
[`docs/UPSTREAM.md`](docs/UPSTREAM.md) before changing the engine boundary or
upgrading Godot.

## Contributing

Start with the [architecture notes](docs/SOLERS_ARCHITECTURE.md) and the
[contribution guide](CONTRIBUTING.md). Keep changes focused, include behavior
tests for editor or agent contracts, and preserve the boundary between Solers
visual ownership and Godot-native engine behavior.

## License

Solers is a fork of [Godot Engine](https://godotengine.org) and is available
under the [MIT license](LICENSE.txt). See [COPYRIGHT.txt](COPYRIGHT.txt) for
attribution.

Solers is an independent distribution and is not affiliated with or endorsed by
the Godot Foundation.
