---
name: godot-plugins-terrain
description: Inspect and use Godot plugins, Terrain3D, procedural height data, terrain materials, vegetation, collision, navigation, and LOD through native APIs.
---

# Plugins, Terrain3D, and Procedural Content

## When to use
Use for editor plugins, GDExtensions, large editable terrain, heightmaps, procedural placement, vegetation, or terrain traversal.

## Inspect first
- List installed plugins; inspect exact version, license, executable files, docs, registered ClassDB types, compatibility, and project terrain resources.
- Establish terrain extent, elevation range, region size, renderer, source data, and traversal/performance targets.

## Recommended order
1. Use Godot's Mesh/Image/Noise/HeightMap/MultiMesh primitives when sufficient; otherwise ensure the pinned trusted Terrain3D package.
2. Inspect real plugin classes and methods before native calls; never invent a `terrain.*` API.
3. Generate/import bounded height data, verify scale, then add sculpting, materials, holes, vegetation, collision, navigation, and LOD.
4. Save every owning resource and scene; stop truthfully if plugin loading requires restart.

## Validate
Reopen and run near/far/traversal views; check bounds, materials, LOD transitions, vegetation density, holes, collision, navigation, logs, and frame cost.

## Common failures
Unapproved plugin code, guessed API names, mismatched height scale, visual foliage treated as collision, unsaved Terrain3D data, and no long-distance performance test.
