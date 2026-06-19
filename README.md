<div align="center">
  <img src="branding/generated/solers02_icon_transparent_1024.png" width="120" alt="Solers Engine"/>

# Solers Engine

**The AI-native game engine built on Godot.**

Solers lets an AI agent work inside the editor: it can inspect the real scene
tree, call native Godot APIs, edit scripts and resources, run the game, read the
errors, and continue from the engine state instead of guessing from files.

Built on Godot 4.6.3 | Standard Godot projects | Bring your own model

</div>

---

## What is Solers?

Solers is an independent Godot-based engine distribution with an AI operator
built into the editor.

The goal is not to wrap Godot in a high-level "AI feature" layer. The goal is to
give the model a transparent harness over the engine: scene state, ClassDB,
Object properties and methods, resources, scripts, runtime control, export, logs
and permissions. The model composes those native primitives the same way a human
developer would use the editor.

Your project remains a normal Godot project. Scenes, scripts, resources, export
presets and project settings stay in standard Godot formats.

## Quick start

Solers builds like Godot. On Windows with MSVC:

```powershell
git clone https://github.com/brandlll-lee/SolersEngine.git
cd SolersEngine
python -m SCons platform=windows target=editor dev_build=yes arch=x86_64 tests=yes
bin\solers.windows.editor.dev.x86_64.exe
```

Open a project, show the **Solers** dock, configure a provider, then describe
the change you want:

```text
Add a small city to this scene with colored buildings, windows and street lights.
Save it so it is still there after reopening the project.
```

The console build is useful while developing Solers itself:

```powershell
bin\solers.windows.editor.dev.x86_64.console.exe --path "C:/path/to/project" --editor
```

macOS and Linux follow the standard Godot SCons build flow with the same
`target=editor` configuration.

## Installing model access

Solers is BYOK: bring your own provider key or use a local OpenAI-compatible
server.

Supported provider profiles include:

- OpenAI
- Anthropic
- Google Gemini
- DeepSeek
- Qwen / DashScope
- Ollama
- LM Studio
- Custom OpenAI-compatible endpoints

Keys are stored through the Solers editor settings path, with environment
variable fallback where available. Privacy mode defaults toward local providers
and blocks remote providers until the user explicitly disables it.

## What Solers can do today

- **Native editor chat dock**: a built-in Solers panel rendered with Godot editor
  controls, not a browser overlay.
- **Primitive-first tool surface**: direct tools for snapshots, file reads and
  writes, ClassDB introspection, object method calls, batched scene operations,
  resources, runtime control and tool discovery.
- **Real scene edits**: node creation, property writes, reparenting, script
  attachment, signal connection and removal go through validated engine paths,
  with undo/redo where Godot supports it.
- **Native resource control**: create, load, inspect, set, call and save Godot
  `Resource` instances through `ClassDB`, `ResourceLoader`, `ResourceSaver` and
  `Object` APIs.
- **Script feedback**: script writes and patches can validate through Godot's
  registered `ScriptLanguage`; raw validation errors are returned to the model.
- **Run loop**: the agent can play the current scene, inspect the editor/runtime
  snapshot, then stop playback.
- **Auditable actions**: tool calls, permissions, arguments, results and
  transcripts are recorded under the Solers user data directory.
- **MCP-compatible adapter**: tools can also be exposed through the local
  Solers protocol surface for external agents.

## Why not just a plugin?

Solers is an engine distribution because the useful AI surface lives below a
project add-on:

- editor lifecycle
- undo/redo
- ClassDB
- scene ownership
- runtime play state
- resource loading and saving
- provider streaming
- permission gates
- trace and transcript storage

Keeping this inside the engine lets the AI operate on facts the editor already
knows, instead of guessing from filenames, text patches or tool-name checklists.

## Build notes

Re-run the same SCons command after edits. SCons rebuilds incrementally:

```powershell
python -m SCons platform=windows target=editor dev_build=yes arch=x86_64 tests=yes
```

Useful generated files:

- `.sconsign5.dblite` - SCons signature database.
- `bin/obj/` - object files for incremental rebuilds.
- `bin/solers.windows.editor.dev.x86_64.exe` - editor build.
- `bin/solers.windows.editor.dev.x86_64.console.exe` - editor with console logs.

Run focused Solers tests, for example:

```powershell
bin\solers.windows.editor.dev.x86_64.console.exe --test --tc="*[SolersToolRegistry] default model surface*"
bin\solers.windows.editor.dev.x86_64.console.exe --test --tc="*tool.search*"
bin\solers.windows.editor.dev.x86_64.console.exe --test --tc="*batch*"
```

## Documentation

- [Architecture notes](docs/SOLERS_ARCHITECTURE.md)
- [v0.1 implementation report](docs/SOLERS_V0_1_IMPLEMENTATION_REPORT.md)
- [v0.1 execution plan](docs/SOLERS_V0_1_EXECUTION_PLAN.md)
- [Build setup report](docs/BUILD_SETUP_REPORT.md)

## Repository layout

```text
modules/solers_ai/
  core/       Agent session, tools, permissions, resources, settings, traces
  editor/     Solers dock, chat cells, markdown view, editor plugin
  llm/        Provider catalog, protocols, streaming client, retry logic
  protocol/   MCP adapter and local RPC server
  tests/      Solers behavior and contract tests
```

Most Solers-specific code lives under `modules/solers_ai`. The rest of the tree
is the Godot engine base plus Solers branding and build metadata.

## Contributing

Prefer small, behavior-backed changes:

- use native Godot APIs before adding wrappers
- expose primitive engine capability before adding feature tools
- keep tool results structured and honest
- delete stale paths instead of preserving empty compatibility shims
- test behavior contracts, not hardcoded name lists

For general engine contribution flow, see [CONTRIBUTING.md](CONTRIBUTING.md).

## License

Solers is a fork of [Godot Engine](https://godotengine.org) and is released
under the MIT license. See [LICENSE.txt](LICENSE.txt) and
[COPYRIGHT.txt](COPYRIGHT.txt).

Solers is an independent distribution. It is not affiliated with, sponsored by
or endorsed by the Godot Foundation.
