---
name: godot-vfx-particles
description: Godot 4 GPUParticles/CPUParticles, decals, trails, one-shot bursts, material billboards, and a worked destruction example (cracks → shatter → dust).
---

# VFX and Particles

## When to use
Use for impacts, trails, weather, fire/smoke, magics, decals, debris bursts, or destruction staging. Look/lighting → `godot-3d-rendering`. Custom particle shaders → `godot-shaders`.

## Facts
| Piece | Role |
|-------|------|
| `GPUParticles3D`/`2D` | Default high-count path; `CPUParticles*` when GPU particles unavailable |
| `ParticleProcessMaterial` | Velocity, gravity, color/scale ramps, emission shapes |
| `one_shot` + `explosiveness` | Bursts; `restart()` to retrigger |
| `Decal` | Cheap projected surface detail (cracks/blood) |
| Draw | Particle material billboard modes; soft alpha; keep overdraw in budget |
| Sub-emitters | Cascades (spark → smoke) when supported |

## Laws
- Prefer restarting pooled emitters over spawning unbounded node trees.
- Match local vs global coordinates to the attachment point.
- Compressed textures must be `decompress()` once before CPU `get_pixel` (never per frame).
- Destruction debris: freeze → impulse → sleep/free; watch collision layers.

## Recipes
**One-shot burst:** `one_shot=true`, `explosiveness=1`, amount 32–128, lifetime 1–2.5s; process material direction = hit normal, spread 35–60°, velocity 2–6.
**Looping ambience:** low amount, soft gravity, large emission AABB; disable when off-camera if costly.
**Worked example — destruction:**
1. Surface: stack `Decal` cracks at impact (`size` ~0.6×0.2×0.6, distance fade).
2. Structure: swap to pre-fractured `RigidBody3D` pieces with `freeze=true`.
3. Collapse: unfreeze + `apply_impulse`; dust `GPUParticles3D` `restart()` at point; sleep pieces after ~4s.
4. Optional continuous damage: shader uniform `damage` mixing crack mask into albedo/AO.

## Traps
| Wrong | Correct |
|-------|---------|
| `get_pixel` on compressed import every frame | Decompress once; cache `Image` |
| New particles node every bullet | Pool + `restart()` |
| Debris colliding with camera forever | Dedicated layer/mask; downgrade after impact |
| Unlimited alpha overdraw | Cap amount; shorten lifetime; use soft particles carefully |
| CPU particles “because habit” on desktop | GPU path first |

## Verify
1. `object.query target=scene` for particle amount, material, decals, debris layers.
2. `runtime.control` trigger → `render.capture`; watch settle.
3. `runtime.observe` — zero compressed-texture spam; stable timing during bursts.
