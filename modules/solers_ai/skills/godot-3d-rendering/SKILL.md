---
name: godot-3d-rendering
description: Godot 4 3D layout, PBR, lighting, single GI path, Environment tonemap (ACES/AgX), physical light units, fog, lightmaps, and photoreal verification. Prefer this over a separate photorealism skill.
tools: scene.csg.bake, scene.lightmap.bake, mesh.unwrap_uv2
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
| Decals | Directional box projection, not arc-length-preserving surface wrapping |
| CSG | Whitebox through ClassDB; bake mesh or collision artifacts with `scene.csg.bake` |
| UV2 / lightmaps | Discover the applicable native capability with `engine.describe`; transact only after topology is stable |
| Exposure | `CameraAttributesPhysical` / `Practical` on `Camera3D` when using physical units |

## Laws
- One authoritative final GI path; seal leaks before blaming lights.
- Treat the live edited scene as the authority; construct visible editor state through the applicable scene/resource tools, not script-driven editor rebuilds.
- Create or edit scene roots and Resources through the applicable scene/resource tools using ClassDB property metadata and the state/hash returned by the matching query.
- With `use_physical_light_units`: Directional intensity is `light_intensity_lux` (`PARAM_INTENSITY`); `light_energy` is a dimensionless multiplier. Never put lux-scale numbers into `light_energy`.
- Before claiming an image ratio, query the exact `Texture2D` resource's native width and height; never reuse dimensions from another asset.
- A `Decal` can project onto a curved receiver, but a wide projection does not preserve artwork arc length. Use a narrower projection or an explicitly UV-mapped curved mesh when the distinction matters.
- After any appearance change, capture the intended viewport with `target=focus`. Framing facts prove only where native bounds land in the image; judge appearance from the pixels.
- `object.query target=relations` measures explicit world-AABB relations. A capture is not a geometry measurement, and node cardinality is not a lighting verdict.
- Never invent model size from thumbnails — measure geometry.
- With physical units: raise lux/lumens or camera attributes — not `ambient_light_energy` / emissive fill as “brightness”.
- CSG is not final art; importers own `.import` — do not hand-edit import caches or claim mipmap/import quality without observing their native state.
- Protective checkpoints are not rollbacks; Solers mid-turn persists scene mutations (`persisted` on tool results).

## Verify
1. Static appearance: `render.capture target=focus`; request `include_render_state=true` only when Environment, CameraAttributes, lights, or effective shader facts are relevant.
2. If causality is unclear, capture once with a native `Viewport.debug_draw` value obtained from `engine.describe`; do not infer renderer state from pixel statistics.
3. Fix the measured source, re-capture once, and compare RenderState plus image hash. Use runtime observation only for gameplay lifecycle, and relations only for geometry.
4. After the native UV2 or lightmap transaction, capture again.
