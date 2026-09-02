---
name: godot-scripting-input-gameplay
description: Godot 4 scripts, scene-tree lifecycle, signals, input, frame callbacks, pause, autoloads, and persistence.
---

# Godot Scripting, Input, and Gameplay

## Scope
Use when working with scripts, node lifecycle, signals, input actions, process frames, pause, autoloads, or saved game data.

## Native model
- Script instances extend Godot Objects. Node scripts participate in SceneTree lifecycle and notifications; RefCounted
  instances use reference-counted lifetime instead of scene-tree ownership.
- Nodes enter the tree parent-first, receive ready child-first, and exit child-first. Deferred calls and queued deletion
  execute through the main loop rather than immediately at the source line.
- `_process()` follows rendered frames; `_physics_process()` follows fixed physics ticks. A Node's process mode and the
  SceneTree pause state determine which callbacks run.
- Signals connect an emitter to Callables and remain independent of direct parent-child method calls.
- InputMap defines named actions. Viewport dispatches input through input, GUI, shortcut, unhandled, and physics-picking
  stages until the event is handled.
- Autoloads are Nodes or scripts inserted near the root of the running SceneTree. They provide lifetime and reachability,
  not automatic ownership or synchronization of application state.
- Persistent runtime data is written under `user://`; file format and migration remain part of the game's data contract.

## Compatibility and prerequisites
- GDScript, C#, GDExtension, engine version, and export platform expose different language and deployment constraints.
- Scene paths, signal signatures, InputMap actions, and autoload definitions must match the loaded project state.

## Authoritative state
Parsed scripts, ClassDB, the live SceneTree, connected signals, current process and pause state, InputMap, runtime input
events, script diagnostics, and persisted files are authoritative.

## Official references
- https://docs.godotengine.org/en/latest/tutorials/scripting/gdscript/index.html
- https://docs.godotengine.org/en/latest/tutorials/scripting/scene_tree.html
- https://docs.godotengine.org/en/latest/tutorials/scripting/idle_and_physics_processing.html
- https://docs.godotengine.org/en/latest/tutorials/inputs/inputevent.html
- https://docs.godotengine.org/en/latest/tutorials/scripting/pausing_games.html
- https://docs.godotengine.org/en/latest/tutorials/scripting/singletons_autoload.html
- https://docs.godotengine.org/en/latest/tutorials/io/saving_games.html
