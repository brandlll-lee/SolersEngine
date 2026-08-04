<div align="center">
  <img src="branding/generated/solers02_icon_transparent_1024.png" width="128" alt="Solers V2 logo"/>

# Solers

**AI-native game engine (Godot fork) where AI acts as a first-class native
operator for collaborative game development.**

[Build from source](#build-from-source) · [Architecture](docs/SOLERS_ARCHITECTURE.md) · [License](LICENSE.txt)

Godot 4.6.3 · Standard Godot projects · Open model access

</div>

---

## Features

### Engine-native agent

Solers runs inside the Godot editor. It works from live scenes, resources,
scripts, editor state, errors, and the running game.

### Build, run, inspect

Create and edit 2D or 3D worlds, write gameplay code, manage assets, run the
game, inspect failures, capture rendered output, and continue from evidence.

### Native, recoverable changes

Scene edits use Godot's native transactions and save validation. File and
resource changes use state checks and recoverable checkpoints.

### Long-running work

The agent loop has no fixed request-count cutoff. Completed tool traffic is
compacted, active work can be restored, and background jobs can resume.

### Open model access

Sign in to ChatGPT Codex, select a provider from the model catalog, or connect
an OpenAI-compatible local, private, or hosted endpoint.

### Extensible tools

Solers exposes focused engine tools for scenes, scripts, runtime observation,
rendering, assets, addons, search, and reusable skills. External agents can use
the same engine through a local MCP-compatible interface.

## Why a Godot fork?

Solers integrates the agent runtime, permissions, context, and tools directly
into the editor instead of operating as a sidecar. Projects remain standard
Godot projects that can be opened, edited, and shipped with familiar tools.

## Build from source

Solers is under active development. Clone the repository and use the standard
[Godot build toolchain](https://docs.godotengine.org/en/stable/engine_details/development/compiling/index.html).

```bash
git clone https://github.com/brandlll-lee/SolersEngine.git
cd SolersEngine
```

### Windows

```powershell
python -m SCons platform=windows target=editor dev_build=yes -j4
.\bin\solers.windows.editor.dev.x86_64.exe
```

### Linux

```bash
scons platform=linuxbsd target=editor dev_build=yes -j4
./bin/solers.linuxbsd.editor.dev.x86_64
```

See the [official Linux build guide](https://docs.godotengine.org/en/stable/engine_details/development/compiling/compiling_for_linuxbsd.html)
for distribution-specific dependencies. On macOS, use the same SCons flow with
`platform=macos`.

Then open or create a project, connect a model in **AI Setup**, and open the
**Solers** panel.

## Developing

Read the [Solers architecture notes](docs/SOLERS_ARCHITECTURE.md) before changing
the agent runtime or engine tools. Solers follows Godot's
[contribution workflow](CONTRIBUTING.md) for engine development.

To include the Solers behavior tests in a Windows development build:

```powershell
python -m SCons platform=windows target=editor dev_build=yes tests=yes -j4
.\bin\solers.windows.editor.dev.x86_64.tests.console.exe --test '--test-case=*[Solers*'
```

## License

Solers is a fork of [Godot Engine](https://godotengine.org) and is available
under the [MIT license](LICENSE.txt). See [COPYRIGHT.txt](COPYRIGHT.txt) for
attribution.

Solers is an independent distribution and is not affiliated with or endorsed by
the Godot Foundation.
