---
name: godot-3d-rendering
description: Godot 4 3D layout, PBR, lighting, single GI path, Environment tonemap (ACES/AgX), physical light units, fog, lightmaps, and photoreal verification. Prefer this over a separate photorealism skill.
---

# 3D Scenes, Materials, Lighting, and Photoreal Look

## When to use
Use for 3D layout, imported models, PBR materials, lights, Environment/GI, shadows, fog, baking, or “photorealistic / cinematic / like a photo” look. Camera **motion** → `godot-camera-cinematography`. Custom `.gdshader` → `godot-shaders`. Terrain meshes → `godot-procedural-terrain` / addons via Contract.

## Facts
| Piece | Role |
|-------|------|
| Renderer | Project setting: Forward+ / Mobile / Compatibility — features differ (SDFGI/volumetrics mainly Forward+) |
| Scale | 1 unit ≈ 1 meter; measure bounds before placing |
| `WorldEnvironment` + `Environment` | Tonemap, background/sky, ambient, SDFGI/SSAO/SSIL/SSR, fog, glow |
| Lights | `DirectionalLight3D` / `OmniLight3D` / `SpotLight3D`; physical lux/lumen when `physical_light_units` on |
| GI (pick **one** final) | SDFGI (dynamic/large) **or** `LightmapGI` (static) **or** `VoxelGI` — not two finals |
| Materials | One PBR map family (albedo/normal/roughness); `StandardMaterial3D` / `ORMMaterial3D` |
| CSG | Whitebox through ClassDB; bake mesh or collision artifacts inside the same `object.transaction` |
| UV2 / lightmaps | Discover the applicable native capability with `engine.describe`; transact only after topology is stable |
| Exposure | `CameraAttributesPhysical` / `Practical` on `Camera3D` when using physical units |

## Laws
- One authoritative final GI path; seal leaks before blaming lights.
- Treat the live edited scene as the authority; construct visible editor state through `object.transaction`, not script-driven editor rebuilds.
- Create or edit scene roots and Resources through `object.transaction` using ClassDB property metadata and the state/hash returned by the matching query.
- With `use_physical_light_units`: Directional intensity is `light_intensity_lux` (`PARAM_INTENSITY`); `light_energy` is a dimensionless multiplier. Never put lux-scale numbers into `light_energy`.
- After any appearance change, capture the intended viewport. Its receipt binds pixels to the exact World3D RenderState; unchanged pixels and changed RenderState are different facts.
- `object.query target=relations` measures explicit world-AABB relations. A capture is not a geometry measurement, and node cardinality is not a lighting verdict.
- Never invent model size from thumbnails — measure geometry.
- With physical units: raise lux/lumens or camera attributes — not `ambient_light_energy` / emissive fill as “brightness”.
- CSG is not final art; importers own `.import` — do not hand-edit import caches.
- Protective checkpoints are not rollbacks; Solers mid-turn persists scene mutations (`persisted` on tool results).

## Verify
1. Static appearance: `render.capture target=camera`; use its Environment, CameraAttributes, lights, effective shader uniforms, and RenderState fingerprint as the authority.
2. If causality is unclear, capture once with a native `Viewport.debug_draw` value obtained from `engine.describe`; do not infer renderer state from pixel statistics.
3. Fix the measured source, re-capture once, and compare RenderState plus image hash. Use runtime observation only for gameplay lifecycle, and relations only for geometry.
4. After the native UV2 or lightmap transaction, capture again.
