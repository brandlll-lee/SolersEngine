<p align="center">
  <img src="branding/generated/solers02_icon_transparent_1024.png" width="96" alt="Solers V2 logo" />
</p>

<h1 align="center">Solers</h1>

<p align="center">
  <strong>AI-native game engine (Godot fork) where AI acts as a first-class native operator for collaborative game development.</strong>
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
| **AI-assisted design** | Human/AI workflow, design theory, verification, safety, and delivery roadmap. | [`docs/AI_ASSISTED_GAME_DESIGN.md`](docs/AI_ASSISTED_GAME_DESIGN.md) |
| **Upstream** | Deterministic protocol for tracking and validating new Godot releases. | [`docs/UPSTREAM.md`](docs/UPSTREAM.md) |
| **Tests** | Unit contracts and real editor behavior projects for Solers. | [`modules/solers_ai/tests/`](modules/solers_ai/tests/) |

## AI Is Part of the Engine

Solers is not a chat window attached to a game engine. The agent runs inside the
editor and operates on the same live state as the person building the game. It
can inspect the edited scene, understand Godot classes and resources, modify the
project, run it, observe the result, and continue from evidence without moving
the workflow to a separate tool.

Solers defines the AI panel's visual language while Godot continues to provide
the native editor lifecycle underneath it: project state, docking, input, text,
rendering, UndoRedo, importing, debugging, and persistence.

## Builds and Changes Real Worlds

Solers works across 2D and 3D scenes, scripts, resources, materials, assets, and
project files. Scene changes are applied against authoritative live editor state
instead of a second in-memory copy. Resource and file writes carry state
preconditions so stale observations do not silently overwrite newer work.

The result remains a standard Godot project. You can keep editing it manually,
open it with familiar Godot workflows, and ship it with the engine toolchain.

## Runs, Looks, and Keeps Going

The agent can start or stop the running project, read debugger state, inspect
errors, query object relationships, and capture rendered output bound to a known
scene revision. Visual evidence is part of the work loop, not a final decorative
screenshot: Solers can look at what the engine produced and decide what to do
next.

Background asset jobs use an explicit wait-and-resume contract. The agent parks
when engine-owned work is still running and resumes from the terminal result
instead of polling the editor or asking the user to repeat the task.

## Native, Recoverable Changes

Scene mutations use Godot's native transaction and UndoRedo path. Resource and
file changes use validated state, atomic replacement, and recoverable
checkpoints. Tool calls pass through a permission boundary before mutating the
project, while receipts record the state that was observed or changed.

This keeps one authoritative writer for engine state and avoids the editor,
filesystem, and agent fighting over different versions of the same scene.

## Long Tasks Stay Long

Solers does not end an agent turn because it crossed a fixed tool-call count.
The model or the user owns the end of the turn. As context fills, completed work
is compacted into a typed continuation while active observations stay attached
to the request that needs them. Session journals preserve the human timeline
without replaying completed tool payloads back into every later model request.

## Bring Your Model

Solers is not locked to one inference provider. The provider catalog and
protocol layer keep model access separate from the engine tool surface.

| Connection | How it works |
|------------|--------------|
| **ChatGPT Codex** | Sign in with ChatGPT and use the native OAuth/Responses integration. |
| **Provider catalog** | Choose a catalog provider using its declared endpoint, protocol, and model metadata. |
| **OpenAI-compatible** | Connect a private, hosted, or gateway endpoint with an explicit base URL and model. |
| **Local models** | Use a compatible local endpoint without sending project prompts to a hosted provider. |

## Engine Tool Surface

The model sees focused engine capabilities rather than a second framework built
around the model.

| Domain | Native capabilities |
|--------|---------------------|
| **World state** | Query live scenes and resources, open scenes, and apply state-checked object transactions. |
| **Project** | Search and read project files, edit scripts, and inspect Godot class or property contracts. |
| **Runtime evidence** | Control playback, read errors, inspect relationships, and capture version-bound renders. |
| **Assets** | Acquire, import, inspect, and resume asynchronous asset work through Godot's importer. |
| **Extensions** | Load built-in skills, install inspected addons, and expose the same engine through a local MCP-compatible interface. |

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
