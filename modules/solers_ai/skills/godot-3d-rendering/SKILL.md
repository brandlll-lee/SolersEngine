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
| Decals | Directional box projection, not arc-length-preserving surface wrapping |
| CSG | Whitebox through ClassDB; keep the source editable and use Godot's native editor conversion for final mesh/collision assets |
| UV2 / lightmaps | Use Godot's importer/editor workflow after topology is stable; inspect the resulting native resources |
| Exposure | `CameraAttributesPhysical` / `Practical` on `Camera3D` when using physical units |

## Laws
- One authoritative final GI path; seal leaks before blaming lights.
- Treat the live edited scene as the authority; construct visible editor state through the applicable scene/resource tools, not script-driven editor rebuilds.
- Create or edit scene roots and Resources through the applicable scene/resource tools using ClassDB property metadata and the state/hash returned by the matching query.
- With `use_physical_light_units`: Directional intensity is `light_intensity_lux` (`PARAM_INTENSITY`); `light_energy` is a dimensionless multiplier. Never put lux-scale numbers into `light_energy`.
- Before claiming an image ratio, query the exact `Texture2D` resource's native width and height; never reuse dimensions from another asset.
- A `Decal` can project onto a curved receiver, but a wide projection does not preserve artwork arc length. Use a narrower projection or an explicitly UV-mapped curved mesh when the distinction matters.
- When appearance is part of the task, use a fresh capture when pixel evidence is needed; framing facts prove only where native bounds land in the image, so judge appearance from the pixels.
- `spatial.inspect` measures explicit world-AABB relations. A capture is not a geometry measurement, and node cardinality is not a lighting verdict.
- The agent chooses the next native observation from the current evidence; this guidance is not a fixed action sequence.
- Use fresh captures when visual claims need pixel evidence, and re-query native state when the cause remains uncertain.
- Never invent model size from thumbnails — measure geometry.
- With physical units: raise lux/lumens or camera attributes — not `ambient_light_energy` / emissive fill as “brightness”.
- CSG is not final art; importers own `.import` — do not hand-edit import caches or claim mipmap/import quality without observing their native state.
- Protective checkpoints are not rollbacks; Solers mid-turn persists scene mutations (`persisted` on tool results).

## Verify
1. For visual claims, choose the native state and pixel evidence needed to test the current hypothesis.
2. If causality is unclear, use `engine.describe` to select a native debug draw or another authoritative observation; do not infer renderer state from pixel statistics.
3. Compare the relevant native receipt and image evidence after a change when the claim depends on appearance. Use runtime observation only for gameplay lifecycle, and relations only for geometry.
