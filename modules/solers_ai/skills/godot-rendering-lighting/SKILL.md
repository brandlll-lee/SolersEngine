---
name: godot-rendering-lighting
description: Native Godot 4 rendering, PBR material, camera, exposure, GI, shadow, fog, reflection, and final-capture workflow for believable interior and exterior scenes.
---

# Godot Rendering and Lighting

Choose one coherent renderer and one final GI path. Lighting is a relationship between geometry, WorldEnvironment, camera exposure, light units, materials, and capture timing; never repair a dark frame with arbitrary ambient or fill-light loops.

## Correct order

1. Inspect renderer, physical-light-unit setting, environment mode, camera attributes, GI nodes, and material maps.
2. Seal the room before GI. Walls, floor, ceiling, openings, and thickness must be real geometry with no light leaks. CSG is a whitebox tool; convert stable geometry to mesh for final UV2 and baking.
3. Use one physically motivated dominant source (sun/window/area-like arrangement), a coherent sky or world environment, and restrained indirect light. If physical light units are enabled, use lux/lumens with `CameraAttributesPhysical`; otherwise use Godot's non-physical workflow consistently.
4. For a static enclosed interior, use LightmapGI with valid UV2 and current mesh artifacts. Disable SDFGI sky reading for a fully enclosed room and do not leave stale `.lmbake` data attached to replaced geometry. Use SDFGI during layout only when its convergence and Forward+ cost are acceptable.
5. Add SSAO/SSIL, fog, reflection probes, and tone/exposure controls only when the scene needs them. Screen-space effects cannot light hidden geometry and cannot substitute for GI or correct shadows.
6. Capture one editor/camera frame after the authored revision settles, and one runtime frame when runtime verification is required. Read `content_sha256`, luminance percentiles, contrast, saturation, and black/white fractions. An identical hash is unchanged evidence, not a reason to repeat the same tool call.

## Material contract

Use one coherent PBR family per surface intent (wall plaster, ceiling plaster, wood, carpet, metal). Verify albedo, NormalGL, roughness, AO/ORM, color space, and physical repeat scale. Keep roughness and normal response physically plausible; do not use emission as room illumination. World-space triplanar mapping is useful for CSG whitebox, but final assets should use authored UVs.

## Diagnosis

- Black interior: inspect gaps, sky reading, light direction/units, camera exposure, GI mode, and shadow settings in that order.
- Flat gray interior: reduce ambient/sky energy only after adding directional structure; inspect clipping percentiles and material roughness.
- Blurry or unstable GI: wait for the renderer's convergence; a one-frame capture is not valid SDFGI evidence.
- Missing baked light: verify current mesh identity, UV2, LightmapGIData users, and a clean rebake after topology changes.
