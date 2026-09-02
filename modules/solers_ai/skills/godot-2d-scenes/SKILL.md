---
name: godot-2d-scenes
description: Godot 4 2D scenes, CanvasItem rendering, TileMapLayer, cameras, lights, and physics.
---

# Godot 2D Scenes

## Scope
Use when working with Godot's 2D scene composition, rendering, tilemaps, cameras, lights, or physics.

## Native model
- A Godot scene is a saved tree of nodes. Node2D provides a 2D transform; CanvasItem provides visibility, ordering,
  material, and canvas drawing state.
- CanvasItem ordering is controlled by canvas layer, z index, scene-tree order, and optional Y sorting.
- CanvasLayer places its descendants on a separate 2D rendering layer. Camera2D changes the canvas transform of its
  nearest parent Viewport.
- PointLight2D and DirectionalLight2D affect CanvasItems where item cull masks and CanvasItem light masks overlap.
  LightOccluder2D provides 2D shadow occlusion geometry.
- TileMapLayer renders cells from a TileSet. TileSet data can also define collision, navigation, terrain, and custom data.
- 2D collision membership and queries use physics layers and masks; these are independent of rendering and light layers.

## Compatibility and prerequisites
- Confirm the project's Godot version and renderer before using classes or properties documented only in `latest`.
- TileSet source data, texture imports, scene ownership, and Viewport configuration are part of the resulting 2D state.

## Authoritative state
The saved scene tree, live CanvasItem and Viewport properties, TileSet resources, physics-server state, diagnostics, and
rendered viewport are authoritative. Filenames and node names alone do not establish rendered or physical behavior.

## Official references
- https://docs.godotengine.org/en/latest/tutorials/2d/index.html
- https://docs.godotengine.org/en/latest/tutorials/2d/canvas_layers.html
- https://docs.godotengine.org/en/latest/tutorials/2d/using_tilemaps.html
- https://docs.godotengine.org/en/latest/tutorials/2d/2d_lights_and_shadows.html
- https://docs.godotengine.org/en/latest/tutorials/physics/physics_introduction.html
