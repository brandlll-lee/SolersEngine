---
name: godot-scripting-input-gameplay
description: GDScript 4 lifecycle, InputMap, signals, process_mode/pause, Autoloads, save paths, and Godot 3→4 syntax traps for gameplay scripts.
---

# Scripting, Input, and Gameplay

## When to use
Use for game rules, interaction, input, lifecycle, state ownership, Autoloads, or save/load. Camera motion → `godot-camera-cinematography`. Shader syntax → `godot-shaders` (when present).

## Facts
| Concern | Authority |
|---------|-----------|
| Script language | GDScript 2.0 (Godot 4.x). Unknown API → `engine.describe` |
| Input | `InputMap` actions + `Input.get_vector` / `is_action_*` — not raw `KEY_*` as the primary path |
| InputMap persistence | Running games read **project** InputMap (`project.godot` / `ProjectSettings`). Editor-only `InputMap.action_add_event` does **not** make Play see actions |
| Events | `_input` → `_shortcut_input` → `_gui_input` (Controls) → `_unhandled_input` / `_unhandled_key_input` |
| Frame work | `_physics_process` for movement/physics; `_process` for visuals/UI; match the phase |
| Mutable state | One owner node/script per fact; others read signals or call into the owner |
| Scene ready | Children `_ready` before parent; `@onready` runs before `_ready` |
| Pause | `Node.process_mode` (`PROCESS_MODE_INHERIT/PAUSABLE/WHEN_PAUSED/ALWAYS/DISABLED`) + `get_tree().paused` |
| Persist | Prefer `user://` for saves; `res://` is read-only in exported games |

## Laws
- One owner per mutable gameplay fact; no duplicate state machines.
- Prefer signals over polling; prefer InputMap over hard-coded keys.
- Do not invent ClassDB methods — `engine.describe` first.
- Do not put physics motion in `_process` unless intentionally frame-rate coupled.
- Build visible editor state with `object.transaction`; keep scripts responsible for runtime behavior.

## Recipes
**InputMap move vector (4.x):**
```gdscript
var v := Input.get_vector("move_left", "move_right", "move_forward", "move_back")
velocity = (transform.basis * Vector3(v.x, 0, v.y)).normalized() * speed
```
**Persist InputMap before first Play:** write actions via `project.edit` / `project.godot` `[input]` (or a script that both registers and documents the same actions). Optional runtime `_ensure_*` is only a safety net — not a substitute for project persistence.
**Awaitable signal once:**
```gdscript
await $Button.pressed
# or: await get_tree().create_timer(0.5).timeout
```
**Minimal save:**
```gdscript
var cfg := ConfigFile.new()
cfg.set_value("player", "hp", hp)
cfg.save("user://save.cfg")
```

## Traps
| Wrong (often Godot 3 prior) | Correct (Godot 4) |
|-----------------------------|-------------------|
| `onready var x = $Y` | `@onready var x = $Y` |
| `yield(x, "sig")` / `yield(get_tree(), "idle_frame")` | `await x.sig` / `await get_tree().process_frame` |
| `export var speed` | `@export var speed: float` |
| `connect("pressed", self, "_on")` | `pressed.connect(_on)` or `pressed.connect(Callable(self, "_on"))` |
| Untyped `Array` / `Dictionary` everywhere | Prefer typed `Array[Node]`, `Dictionary` with documented keys |
| `pause_mode` | `process_mode` |
| Listening only in `_input` for gameplay that UI might consume | Use `_unhandled_input` so Controls can mark handled |
| Editing Autoload scripts as if they were scene-unique | Autoloads are singletons — one instance, careful with scene-specific state |
| Registering InputMap only in the editor session, then Play | Persist with `project.edit` first; Play uses the project map |

## Verify
1. `project.search` / `script.edit` / `script.validate`.
2. Confirm InputMap actions exist in project settings before `runtime.control` play.
3. `runtime.control` play → exercise InputMap paths through the gameplay API or a project test script.
4. `runtime.observe` digest for errors; confirm pause/`process_mode` behavior if used.
5. Re-open the project if Autoload or InputMap changed.
