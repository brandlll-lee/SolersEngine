---
name: godot-physics-navigation
description: Godot 4 body types, CharacterBody move_and_slide, layers/masks, RigidBody rules, Area triggers, NavigationServer sync, and NavigationAgent wiring.
---

# Physics, Collision, and Navigation

## When to use
Use for movement, contacts, triggers, shape queries, joints, navmesh bake, agents, links, or avoidance.

## Facts
| Type | Use when |
|------|----------|
| `StaticBody2D/3D` | Immovable world |
| `AnimatableBody2D/3D` | Kinematic moving platforms (engine-moved) |
| `CharacterBody2D/3D` | Player/AI directly controlled motion |
| `RigidBody2D/3D` | Simulated forces/impulses |
| `Area2D/3D` | Detection / triggers / gravity wells — not solid blocking |
| Shapes | Prefer simple `Box`/`Sphere`/`Capsule` scaled uniformly; convex for complex static |
| Layers | **layer** = what I am; **mask** = what I collide/detect with |
| Navigation | `NavigationRegion3D/2D` bake → map on `NavigationServer*` → `NavigationAgent*` |

## Laws
- Do **not** move `RigidBody*` by writing `global_transform` each frame — use forces/velocity/`integrate_forces`.
- Character motion: one velocity authority; usually `move_and_slide` in `_physics_process`.
- Direct space queries belong in the physics frame (not arbitrary `_process` guessing).
- Synchronize / wait for the navigation map before the first `get_next_path_position` (first-frame empty path is common).
- Bake navmesh from **agent radius/height**, not eyeballed margins alone.

## Recipes
**CharacterBody3D floor move:**
```gdscript
func _physics_process(delta: float) -> void:
    if not is_on_floor():
        velocity.y -= gravity * delta
    var input := Input.get_vector("move_left", "move_right", "move_forward", "move_back")
    var dir := (transform.basis * Vector3(input.x, 0, input.y)).normalized()
    velocity.x = dir.x * speed
    velocity.z = dir.z * speed
    move_and_slide()
```
**Rigid impulse:** `apply_impulse(direction * strength)` at contact; enable `contact_monitor` / `max_contacts_reported` only where needed.
**Agent loop (outline):** bake region → `await get_tree().physics_frame` (or map-changed) → `agent.target_position = goal` → each physics frame steer using `get_next_path_position()`.

**move_and_slide vs move_and_collide:** slide for characters walking on floors; collide when you need precise hit info / custom response.

## Traps
| Wrong | Correct |
|-------|---------|
| `RigidBody.global_position = …` every frame | Forces / `linear_velocity` / joints |
| Non-uniform scale on collision shapes | Uniform scale or fix mesh; re-bake shape |
| Mask/layer all bits on | Explicit roles (world/player/projectile…) |
| Query path same frame as bake | Wait for map sync |
| Resetting `target_position` every frame unnecessarily | Set when goal changes |
| Two scripts both writing `velocity` | Single locomotion owner |
| Godot 3 `KinematicBody` / `move_and_slide()` old signature | `CharacterBody3D` + velocity property API |

## Verify
1. `scene.inspect` for bodies, shapes, layers/masks, region/agent setup.
2. `runtime.control` → slopes, edges, triggers, reachable/unreachable goals.
3. `runtime.observe` for physics errors; confirm no transform-fighting rigid bodies.
4. Optional: `render.capture` for contact/debug shapes if using debug draw.
