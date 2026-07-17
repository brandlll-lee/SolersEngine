---
name: godot-terrain
description: Build, edit, and verify game terrain with Godot primitives or the bundled Terrain3D extension through Solers' generic plugin and native object capabilities.
---

# Godot Terrain

Use this skill when a project needs editable large-scale terrain, terrain materials, foliage, collision, or navigation.

## Rules

- Godot provides low-level terrain building blocks, not a complete built-in terrain authoring system. Use native Mesh, Image, FastNoiseLite, HeightMapShape3D, Navigation and MultiMesh directly when they are sufficient.
- For a full terrain workflow, inspect and ensure the bundled, pinned Terrain3D package. Do not invent a `terrain.*` API or install an unverified version.
- Treat `plugin.inspect`, the loaded ClassDB documentation, and the current project's resources as authoritative. Never guess a Terrain3D class, property, method, enum, or file path.
- Keep source height data, Terrain3D resources, scene nodes, materials, collision, navigation and runtime verification as separate facts. A successful tool call is not proof that the terrain looks or plays correctly.
- Work in meters. Establish the terrain extent, height range and region size before generating data. Preserve user-authored regions and assets unless replacement is explicitly requested.

## Workflow

1. Call `plugin.list`. If the pinned Terrain3D package is not installed, call `plugin.inspect` with `source=bundled` and `plugin_id=terrain3d`, review its version, hash, files and compatibility, then call `plugin.ensure` with the exact inspected identity. If loading requires an editor restart, stop terrain mutation and report that state truthfully.
2. Read the plugin's registered classes and documentation from `plugin.inspect`. Use `class.search` and focused `class.introspect` calls for the exact Terrain3D types and members needed by this task.
3. Inspect the scene and existing terrain resources. Build the minimal node/resource graph with `objects.batch`; create RefCounted resources with `native.instantiate`, invoke documented methods with `native.call`, and persist Resources with `native.save`.
4. Generate a bounded base height image with documented Godot Image/FastNoiseLite APIs, or import an explicit R16, EXR or DEM source. Write it through the loaded Terrain3D version's real API. Apply further sculpting, holes, materials, assets and instancing only after the base extent and elevation are verified.
5. Use project materials and vegetation assets. Keep visual foliage separate from navigation and collision. Configure collision and navigation from the final terrain data, then save every owning resource and scene.
6. Run the project and capture representative near, far and traversal views. Check editor/runtime logs, terrain bounds, LOD transitions, holes, material scale, collision, navigation and performance. Correct the underlying data or configuration rather than hiding defects with extra geometry.
