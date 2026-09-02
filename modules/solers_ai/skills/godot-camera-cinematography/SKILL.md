---
name: godot-camera-cinematography
description: Generic Godot 4 camera building blocks - Camera3D authority, SpringArm3D third-person, Marker3D shots, seamless Tween interpolate_with transitions, PathFollow3D moves, CameraAttributes, input freeze, and runtime visual verification. Not a Black-Eye clone.
tools: runtime.observe, runtime.control
---

# Camera Cinematography (Generic)

## When to use
Use for gameplay cameras, film-like moves into dialog/shop/cut-ins, over-the-shoulder (OTS) framing, scripted fly-throughs, FOV/DOF changes, and returning to player control. Use whenever the Agent must **drive or blend cameras with Godot nodes**, not only compose a still for `render.capture`.

For photoreal exposure/DOF look recipes, also read `godot-3d-rendering`. This skill owns **who moves the camera and how**.

## Authority model (non-negotiable)
- **One driver at a time**: either the gameplay rig (for example `SpringArm3D` → `Camera3D`), **or** a cinematic `Camera3D`/`Tween`/`AnimationPlayer`/`PathFollow3D`. Never let two systems write the same camera transform in the same frame.
- **Viewport fact**: each Viewport has exactly one current `Camera3D`. Switching is `make_current()` / `current = true` (immediate). There is **no** built-in 3D camera cross-fade in Godot.
- **Seamless cut into a blend**: copy the live gameplay camera's `global_transform` (+ FOV / attributes) onto the cinematic camera **first**, then `make_current()`, **then** Tween. Doing `make_current` before alignment causes a visible pop.
- **Triggers are declared**, never guessed: `Area3D` signals, InputMap actions, or `play(shot_id, target_path, …)` with explicit `NodePath` / shot id. Do **not** invent “walked into shop” heuristics, approach-angle solvers, priority stacks, or corridor geometry as Solers defaults.

## Engine primitives (use the full set)
| Primitive | Role |
|-----------|------|
| `Camera3D` | Projection, FOV, near/far, cull mask, `current`, environment override, `attributes` |
| `CameraAttributesPractical` / `CameraAttributesPhysical` | Exposure, DOF, physical focal length (Physical owns FOV when attached) |
| `SpringArm3D` | Third-person length + collision retract; places **direct child** Node3D each physics tick |
| `Marker3D` | Named shot pose (OTS, wide, insert). Pure `Node3D` with gizmo |
| `Tween` | Runtime blends: transform, FOV, attributes fields; dynamic end pose |
| `Transform3D.interpolate_with` | Correct pose blend (lerp origin + quaternion slerp). Prefer this over Euler |
| `AnimationPlayer` / `AnimationMixer` | Authored 3D position/rotation/scale tracks on a camera |
| `Path3D` + `PathFollow3D` | Designer-drawn fly path; Tween `progress` / `progress_ratio` |
| `RemoteTransform3D` | One-way bind only. **Do not** use during an active Tween/SpringArm on the same node |
| `Area3D` | Enter/exit shot triggers (`body_entered` / `body_exited`) |
| `SubViewport` + extra `Camera3D` | Picture-in-picture / render-to-texture; separate Viewport current-camera authority |

Instance property values: `scene.inspect` and `resource.inspect`. Use `engine.describe` only when a ClassDB class or method name is unknown.

## Agent tool sequence
1. `skill.read` this skill (and `godot-scripting-input-gameplay` if input ownership is unclear).
2. `scene.inspect` for the live camera (`current`, `fov`, `global_transform`, parent rig).
3. Use the applicable scene tools with the inspected state receipt to create Marker shots, Area triggers, cinematic `Camera3D`, connections, and hierarchy layout.
4. `script.edit` for the transition controller.
5. `runtime.control` → play → trigger → `render.capture target=runtime` (and `target=camera` with `node_path` when needed) → `runtime.observe`. Spatial placement still requires `spatial.inspect`, not a capture.

There is **no** `camera.*` tool. Capabilities are native nodes + scripts.

## Building block A — third-person gameplay rig
```
Player (CharacterBody3D)
 └─ Pivot (Node3D)          # yaw
     └─ SpringArm3D         # pitch on the arm or a Pitch node
         └─ Camera3D        # current during gameplay
```
- Drive look with InputMap actions in `_physics_process` or `_unhandled_input`; keep one script as look authority.
- While a cinematic transition owns the view, **disable** look input and optionally set `SpringArm3D.spring_length` unchanged (do not fight the arm by Tweening the child camera position).
- Collision: set SpringArm `collision_mask` so it retracts from world, not from the player capsule.

## Building block B — shot anchors
- Place `Marker3D` nodes for each intentional frame: e.g. `Shots/OTS_Shopkeeper`, `Shots/Wide_Interior`, `Shots/Return_Player`.
- OTS recipe: slightly behind/beside the listener, looking at the speaker (`look_at` speaker head/chest Marker). Parent the OTS Marker to the player **or** keep it world-space and re-`look_at` when the shot starts if the player can move.
- Store shot parameters as `@export` on a small Resource or on the controller: `target_path`, `duration`, `trans`/`ease`, optional `fov`, optional `attributes`, optional `path_path`. **Data-driven** — new shots do not require new engine code.

## Building block C — seamless transition (core recipe)
```gdscript
func play_to_marker(shot: Marker3D, duration := 1.2, fov_to := -1.0) -> void:
    var vp := get_viewport()
    var from_cam := vp.get_camera_3d()
    if from_cam == null:
        return
    # 1) Align cinematic camera to the live camera (no pop).
    cine.global_transform = from_cam.global_transform
    cine.fov = from_cam.fov
    cine.attributes = from_cam.attributes
    cine.make_current()
    # 2) Freeze gameplay look / movement as needed (caller-owned flags).
    set_gameplay_camera_input(false)
    # 3) Blend with interpolate_with — never Euler-lerp basis.
    var start := cine.global_transform
    var end := shot.global_transform
    var start_fov := cine.fov
    var end_fov := fov_to if fov_to > 0.0 else cine.fov
    if _active_tween:
        _active_tween.kill()
    _active_tween = create_tween().set_trans(Tween.TRANS_CUBIC).set_ease(Tween.EASE_IN_OUT)
    _active_tween.tween_method(
        func(t: float):
            cine.global_transform = start.interpolate_with(end, t)
            cine.fov = lerpf(start_fov, end_fov, t),
        0.0, 1.0, duration)
    await _active_tween.finished
```
Return to gameplay: reverse the same pattern — Tween toward the **current** gameplay camera pose (read live SpringArm result), then `gameplay_cam.make_current()`, re-enable input.

Cancel / re-enter: always `Tween.kill()` the previous tween before starting another; keep a single `_active_tween` reference.

## Building block D — PathFollow3D fly-through
When the move must avoid geometry or follow a beat board:
1. Author `Path3D` curve in the editor.
2. `PathFollow3D` (rotation mode as needed) + child `Camera3D` **or** Tween a free camera along sampled transforms.
3. Tween `progress_ratio` 0→1; do **not** also run SpringArm on that same camera.

## Building block E — AnimationPlayer
Use when timing is authored on a timeline (cuts, holds, multi-beat dialog). Animate the cinematic camera's `position`/`rotation`/`fov` tracks. Still align + `make_current` before playing if blending out of gameplay.

## Building block F — feel (FOV / attributes)
- Practical DOF: `CameraAttributesPractical` near/far blur for close-ups after the move settles (or Tween blur amount).
- Physical: attach `CameraAttributesPhysical`; drive `frustum_focal_length` / focus distance instead of fighting `Camera3D.fov` (Physical owns projection when active).
- Parallel Tween FOV with transform for “push-in” emphasis; keep changes modest for dialog (often 5–15°).

## Building block G — multi-shot without a priority stack
Expose `play(shot_id: StringName)` that looks up a Dictionary/`Shot` Resource. The **caller** chooses which shot runs (signal, dialog line, quest state). That is generic and open for extension.
Do **not** bake Unreal-style priority stacks, blend corridors, or approach-angle auto-framing into Solers or a mandatory Autoload — those are optional game-specific policies on top of `play(shot_id)`.

## Optional addon
If the project already wants a virtual-camera plugin: `addon.search` → `addon.inspect` → `addon.ensure` using the returned `entry_classes` / Contract. Prefer native nodes first; addons are optional, not the Solers camera center.

## Shop / dialog example (composition only)
1. TPS rig = Building block A.
2. `Area3D` on shop entrance → `body_entered` → `play(&"ots_keeper")` with Marker path.
3. Building block C moves to OTS looking at the keeper.
4. On exit / cancel → reverse blend to gameplay cam.
All shot ids and paths are **authored**; the Agent must not infer them from mesh names.

## Verify
1. `runtime.control` play the scene.
2. Trigger the shot (walk into Area or call `play`).
3. `render.capture target=runtime` at start, mid-blend, and end — no pop at `make_current`, continuous motion, intended framing.
4. `runtime.observe` — no spam from competing camera writers; SpringArm not fighting Tween.
5. Confirm input disabled during blend and restored after return.

## Common failures
- `make_current` before aligning transforms (pop).
- Tweening a camera that is still a SpringArm child while the arm writes position every physics frame.
- Euler/`basis` lerp instead of `interpolate_with`.
- Two Tweens or gameplay look + cinematic both active.
- Using `RemoteTransform3D` during a blend.
- Guessing shot targets from node names instead of explicit paths/ids.
- Building a mandatory priority-stack “director” framework when `play(shot_id)` + Markers suffice.
