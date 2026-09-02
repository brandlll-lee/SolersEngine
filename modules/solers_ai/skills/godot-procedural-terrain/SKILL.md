---
name: godot-procedural-terrain
description: Godot 4 procedural geometry, terrain composition, instancing, collision, navigation, and level of detail.
---

# Godot Procedural Terrain

## Scope
Use when working with generated geometry, height fields, terrain systems, foliage instancing, collision, navigation, or LOD.

## Native model
- Godot core provides geometry, rendering, physics, navigation, and streaming primitives rather than one universal 3D
  terrain node or terrain storage format.
- Mesh resources contain surfaces. MeshInstance3D places a Mesh in a scene and can override its materials.
- ArrayMesh constructs surfaces from typed arrays. SurfaceTool builds vertex data sequentially; MeshDataTool exposes
  vertices, edges, and faces; ImmediateMesh is intended for frequently rebuilt simple geometry.
- HeightMapShape3D represents grid height data for physics. Visual mesh density and collision-map resolution are separate.
- MultiMesh batches many instances of one mesh with per-instance transforms, colors, and custom data. The MultiMesh is
  culled as one object rather than per instance.
- Navigation consumes selected source geometry into NavigationMesh data. Visibility ranges and mesh LOD control visual
  detail independently of collision and navigation.
- Terrain addons define their own nodes, resources, storage, and editor workflows on top of these engine primitives.

## Compatibility and prerequisites
- Geometry format, renderer limits, collision backend, navigation baking, floating-point scale, and addon version constrain
  a terrain implementation.
- Generated data needs an explicit owner and persistence path if it must survive scene reload or export.

## Authoritative state
The owning source data, generated Mesh resources, scene transforms, MultiMesh bounds, physics shapes, navigation maps,
saved resources, addon API observed in the project, rendered output, and runtime performance are authoritative.

## Official references
- https://docs.godotengine.org/en/latest/tutorials/3d/procedural_geometry/index.html
- https://docs.godotengine.org/en/latest/tutorials/3d/procedural_geometry/arraymesh.html
- https://docs.godotengine.org/en/latest/tutorials/3d/procedural_geometry/surfacetool.html
- https://docs.godotengine.org/en/latest/classes/class_heightmapshape3d.html
- https://docs.godotengine.org/en/latest/classes/class_multimesh.html
- https://docs.godotengine.org/en/latest/tutorials/3d/visibility_ranges.html
- https://docs.godotengine.org/en/latest/tutorials/navigation/navigation_using_navigationmeshes.html
