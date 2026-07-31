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
- Never invent model size from thumbnails — measure geometry.
- With physical units: raise real lights or camera exposure — not `ambient_light_energy` / emissive fill as “brightness”.
- CSG is not final art; importers own `.import` — do not hand-edit import caches.

## Recipes
**Project (physical look):**
- `rendering/lights_and_shadows/physical_light_units` = `true`
- Directional shadow size `4096`, soft filter quality ≥ `3`
- MSAA 3D `2` (4×) and/or TAA for foliage

**Environment (photoreal starting point):**
- `tonemap_mode` = `3` (ACES) or `4` (AgX where available); `tonemap_white` ≈ `6`
- Sky: `PhysicalSkyMaterial` or HDRI; SDFGI for exteriors (`cascades` 4–6, `min_cell_size` ~0.15 human scale)
- Interiors that never move: prefer baked `LightmapGI` instead of SDFGI as the final path
- SSAO/SSIL/SSR as needed; glow keep low (`intensity` ~0.4, `hdr_threshold` ~1.2) — high glow reads “gamey”
- Depth fog subtle (`density` ~0.001–0.01)

**Sun (physical):**
- Noon lux ~`100000`, overcast ~`35000`, sunset ~`400`; temperature 5500–6500 K; `light_angular_distance` ~`0.53`; 4-split shadows

**Camera exposure:**
- Exterior: aperture ~16, shutter 1/100, ISO 100; interior: aperture ~2.8, ISO 400–800
- DOF via focal length / focus distance on close-ups only

## Traps
| Wrong | Correct |
|-------|---------|
| SDFGI **and** LightmapGI both as final | One final GI path |
| Brighten with ambient/emission | Raise lux/lumens or exposure |
| Mixed albedo/normal from unrelated sets | One map family + matched UV scale |
| Guessing meters from preview images | Measure AABB / importer stats |
| Hand-editing `.import` | Change importer settings / reimport |
| Compatibility renderer + expecting SDFGI | Check renderer feature matrix via project settings / docs |
| Property name `tonemapper` | `tonemap_mode` on `Environment` |

## Verify
1. `scene.inspect` / `resource.inspect` Environment, lights, materials.
2. Static look-dev (materials, lights, still composition): `viewport.capture target=camera` with the product `Camera3D` — shares the edited World3D; do **not** `runtime.control` play only to screenshot.
3. Gameplay or script lifecycle only: `runtime.control` play → `viewport.capture target=runtime` → `runtime.observe` as needed.
4. Support, gaps, containment, coaxial alignment: `scene.validate` (world AABB). A capture does not measure these.
5. Fix sources (light/GI/material/transform), then re-capture or re-validate; stop when the facts pass — no fixed loop count.
6. After UV2/lightmap work: `mesh.unwrap_uv2` / `lightmap.bake` then capture again.
