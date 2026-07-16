<div align="center">
  <img src="branding/generated/solers02_icon_transparent_1024.png" width="112" alt="Solers Engine"/>

# Solers

### Build playable worlds by talking to the engine.

**Solers is an open-source, AI-native game engine built on Godot.**

Describe a world, a mechanic, or a problem. Solers works inside the real editor:
it can understand your project, make the change, run the game, look at the
result, read the errors, and keep going.

[Getting started](#getting-started) · [Documentation](docs/SOLERS_ARCHITECTURE.md) · [Contributing](CONTRIBUTING.md)

Godot 4.6.3 · Standard Godot projects · Bring your own model

</div>

---

## What Solers controls

- **Understand the entire game** — read the project, scenes, resources, code,
  imported assets, editor state, logs, and the running game.
- **Build the world** — create and refine 2D or 3D scenes, geometry, materials,
  physics, navigation, animation, cameras, and reusable resources.
- **Program the experience** — write and validate scripts, connect systems, and
  implement interactions, player controls, and game rules.
- **Direct the visual pipeline** — control lighting, environments, shaders, GI,
  shadows, reflections, UVs, lightmaps, rendering, and baking.
- **Manage the content pipeline** — find, generate, process, organize, and import
  models, materials, HDRIs, music, and sound effects.
- **Run, debug, and ship** — play the game, inspect its output, capture visual
  evidence, fix failures, validate the result, and export playable builds.
- **Work autonomously, under your control** — plan long tasks, continue across
  background jobs or restarts, request approval, preserve history, and undo
  supported edits.

Your work is never trapped in a private format. A Solers project is a normal
Godot project that you can open, edit, and ship with familiar Godot tools.

## Getting started

Solers is currently an active-development build. Clone the source first:

```bash
git clone https://github.com/brandlll-lee/SolersEngine.git
cd SolersEngine
```

### Windows

Install the standard
[Godot build toolchain](https://docs.godotengine.org/en/stable/engine_details/development/compiling/index.html),
then build and run:

```powershell
python -m SCons platform=windows target=editor dev_build=yes tests=yes -j4
.\bin\solers.windows.editor.dev.x86_64.exe
```

### Linux (Ubuntu/Debian)

Install the native dependencies, then build and run:

```bash
sudo apt-get update
sudo apt-get install -y build-essential scons pkg-config \
  libx11-dev libxcursor-dev libxinerama-dev libxi-dev libxrandr-dev \
  libgl1-mesa-dev libglu1-mesa-dev libasound2-dev libpulse-dev \
  libudev-dev libwayland-dev libwayland-bin

scons platform=linuxbsd target=editor dev_build=yes tests=yes -j4
./bin/solers.linuxbsd.editor.dev.x86_64
```

For other distributions, see the
[official Linux build guide](https://docs.godotengine.org/en/stable/engine_details/development/compiling/compiling_for_linuxbsd.html).

Then:

1. Open or create a Godot project.
2. Open **AI Setup** and connect a model provider.
3. Open the **Solers** panel and describe what you want.

```text
Use these reference images to build a warm, lived-in bedroom.
Run the scene, inspect the result and errors, fix any visible problems, then save it.
```

macOS follows the same SCons flow with `platform=macos`.

## Models

Use the model access you already have:

- sign in with ChatGPT to use **Codex**
- connect **OpenAI, Anthropic, Gemini, DeepSeek, or Qwen**
- run locally with **Ollama or LM Studio**
- use any compatible private or hosted OpenAI-style endpoint

Solers only shows connected providers. Image input and reasoning effort appear
when the selected model supports them.

## How Solers works

Solers is built into the editor, so its AI acts on live engine state instead of
a text-only copy of the project. It uses composable native operations, keeps
long tasks recoverable, and verifies the result before declaring success.
External agents can use the same capabilities through a local MCP-compatible
interface.

## Privacy

Projects and session history stay on your machine. When you choose a remote
model or asset service, the information needed for that request is sent directly
to that provider under its own terms. Local models can be used without a remote
LLM provider, and remote access is blocked while Privacy Mode is enabled.

## Contributing

Solers is built in public. Contributions that make the engine simpler, more
capable, or more trustworthy are welcome. Start with the
[contribution guide](CONTRIBUTING.md) and [architecture notes](docs/SOLERS_ARCHITECTURE.md).

## License

Solers is a fork of [Godot Engine](https://godotengine.org) and is available
under the [MIT license](LICENSE.txt). See [COPYRIGHT.txt](COPYRIGHT.txt) for
attribution.

Solers is an independent distribution and is not affiliated with or endorsed by
the Godot Foundation.
