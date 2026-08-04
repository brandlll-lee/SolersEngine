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
| CSG | Whitebox only — bake to MeshInstance3D for shipping (`scene.bake_csg`) |
| UV2 / lightmaps | `mesh.unwrap_uv2` then `lightmap.bake` after topology stable |
| Exposure | `CameraAttributesPhysical` / `Practical` on `Camera3D` when using physical units |

## Laws
- One authoritative final GI path; seal leaks before blaming lights.
- Treat the edited scene as the authority: inspect the actual environment, lights, and active camera. If `@tool` `_ready` builders changed on disk, `scene.reload` before `render.capture target=editor|camera`; runtime proves Play only.
- Create or edit scene roots and Resources through `object.transaction` using ClassDB property metadata and the state/hash returned by the matching query.
- With `use_physical_light_units`: Directional intensity is `light_intensity_lux` (`PARAM_INTENSITY`); `light_energy` is a dimensionless multiplier. Never put lux-scale numbers into `light_energy`.
- After any appearance change, capture the intended viewport and require a render receipt sourced from the new scene state. An identical image hash is evidence of identical pixels, not evidence that a mutation rendered.
- `object.query target=relations` measures explicit world-AABB relations. A capture is not a geometry measurement, and node cardinality is not a lighting verdict.
- Never invent model size from thumbnails — measure geometry.
- With physical units: raise lux/lumens or camera attributes — not `ambient_light_energy` / emissive fill as “brightness”.
- CSG is not final art; importers own `.import` — do not hand-edit import caches.
- Protective checkpoints are not rollbacks; Solers mid-turn persists scene mutations (`persisted` on tool results).

## ClassDB levers (not Solers recipes)
`engine.describe` without `member_query` returns member names only; pass `member_query` (whitespace-separated tokens) for typed signatures/docs. When physical light units are enabled, set `Environment`, `DirectionalLight3D.light_intensity_lux`, and `CameraAttributesPhysical` through `object.transaction`; values and units come from engine docs and measured render facts, never tool defaults.

## Traps
| Wrong | Correct |
|-------|---------|
| Write noon lux into `light_energy` | `light_intensity_lux` when physical units on |
| SDFGI **and** LightmapGI both as final | One final GI path |
| Brighten with ambient/emission | Raise lux/lumens or exposure attributes |
| Mixed albedo/normal from unrelated sets | One map family + matched UV scale |
| Guessing meters from preview images | Measure AABB / importer stats |
| Hand-editing `.import` | Change importer settings / reimport |
| Compatibility renderer + expecting SDFGI | Check renderer feature matrix via project settings / docs |
| Property name `tonemapper` | `tonemap_mode` on `Environment` |
| Treat high `near_black` alone as “add light” | Also read intensity/energy/CameraAttributes and bipolar bands |

## Verify
1. `object.query target=scene|resource` for the actual Environment, lights, CameraAttributes, and materials; retain returned state/hash preconditions.
2. Static appearance: `render.capture target=camera` with the intended `Camera3D`; require its source receipt to match the edited state.
3. Gameplay or script lifecycle only: `runtime.control` play → `render.capture target=runtime` → `runtime.observe` as needed.
4. Support, gaps, containment, coaxial alignment: `object.query target=relations` (world AABB). A capture does not measure these.
5. Fix the measured source, re-capture once, and compare receipt plus image hash before another mutation.
6. After UV2/lightmap work: `mesh.unwrap_uv2` / `lightmap.bake` then capture again.
