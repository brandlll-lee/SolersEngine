<div align="center">
  <img src="branding/generated/solers02_icon_transparent_1024.png" width="120" alt="Solers Engine"/>

# Solers Engine

**The AI-native game engine.**

Describe a world, a mechanic, or a system. Solers turns your intent into scenes, scripts, and playable games, right inside the editor.

Built on Godot 4.6 · Bring your own model · Your project stays 100% standard Godot

</div>

---

## What is Solers?

Solers is a game engine where AI is a first-class operator inside the editor, not a chat box bolted onto the side.

Most "AI + gamedev" tools are external assistants that blindly write `.gd` and `.tscn` text. Solers is different. The AI sees your real scene tree, edits nodes through native engine APIs, runs your game, reads the errors, and fixes them, and every single action is something you can watch, approve, and undo.

You bring the ideas. Solers turns them into a real, running game.

## Why Solers

- **AI operates the engine, not the filesystem.** Scene edits go through Godot's native APIs and undo/redo, so they are real editor operations, not fragile text patches.
- **Every action is auditable.** A built-in Action Timeline records each step: what the AI did, which nodes and files it touched, and how to roll it back.
- **Bring your own key.** Use OpenAI, Anthropic, Gemini, DeepSeek, Qwen, Ollama, or LM Studio. No vendor lock-in, with a local privacy mode in the design.
- **Stays pure Godot.** Your project remains a standard Godot project. Open it in upstream Godot any time. No proprietary formats, no trap.
- **Safe by design.** Tiered permissions, approval prompts for risky actions, and automatic file checkpoints before writes.

## What works today

Solers is an early preview, but the operator core is real and running:

- A native **Solers AI panel** built into the editor (an RmlUi-based chat dock, not an external window).
- **50+ typed, permission-gated tools** covering projects, scenes, nodes, scripts, running, validation, resources, and export, each one logged.
- **Live editor observation**: read the current scene tree, selection, project settings, and real editor/runtime logs.
- **Scene & script operations** through Godot's `EditorUndoRedoManager`: add / remove / reparent nodes, set properties, attach scripts, connect signals, create / patch / validate scripts.
- **Run & verify loop**: play the current scene, capture logs and screenshots, and check for errors.
- **Action Timeline**, **permission manager**, and **file checkpoints** built in.
- **MCP-compatible** tool adapter plus a local loopback RPC, so external agents can drive Solers too.
- **BYOK provider registry** and an agent orchestrator (planner → executor → verifier → reporter).

> Coming next: wiring live model inference end-to-end. The provider request layer and a working mock loop are already in place, real model calls are the next milestone.

## Quick start (Windows)

Solers builds like Godot, using SCons + MSVC.

```powershell
git clone https://github.com/brandlll-lee/SolersEngine.git
cd SolersEngine
scons platform=windows target=editor dev_build=yes arch=x86_64
bin\solers.windows.editor.dev.x86_64.exe
```

Open or create a project, find the **Solers** panel on the left, and start describing what you want to build.

macOS and Linux follow the standard Godot build steps for the same `target=editor` configuration.

## Compatibility & license

Solers is a fork of [Godot Engine](https://godotengine.org) and is released under the MIT license. See `LICENSE.txt` and `COPYRIGHT.txt`.

Solers is an independent distribution. It is **not** affiliated with or endorsed by the Godot Foundation. It is compatible with Godot projects, but it is its own thing.

## Acknowledgements

Built on the incredible work of the Godot Engine community. Thank you. ❤️
