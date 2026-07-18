---
name: godot-3d-rendering
description: Build and render Godot 3D scenes with real scale, native assets, PBR materials, cameras, lighting, GI, shadows, fog, and visual verification.
---

# 3D Scenes, Materials, and Rendering

## When to use
Use for 3D layout, reference matching, imported models, materials, shaders, lighting, environment, GI, baking, or cameras.

## Inspect first
- Read renderer, physical-light-unit setting, scene scale, environment, camera, GI, asset geometry, UV2, and material/import ownership.
- Use ClassDB only for an unknown current-engine API; never infer model dimensions from a thumbnail.

## Recommended order
1. Lock scale, structure, traversal, openings, and camera composition; CSG is for whiteboxing, not final asset modeling.
2. Place real project/catalog assets from measured bounds and keep collision simpler than render geometry.
3. Build coherent PBR materials with calibrated texture scale and one map family per surface.
4. Configure one renderer and one authoritative final GI path; seal geometry before diagnosing light leaks.
5. Bake UV2/lightmaps only after topology is stable, then add reflections, fog, and post effects where evidence requires them.

## Validate
Run and capture stable camera views; check geometry, scale, material response, luminance, shadows, GI convergence, reflections, runtime logs, and performance.

## Common failures
Exposure loops hiding bad lighting, emission used as fill, mixed PBR map families, stale bake data, duplicate GI paths, and unmeasured asset placement.
