---
name: godot-2d-scenes
description: Build and verify Godot 2D scenes, TileMap layers, sprites, lighting, particles, animation, and canvas rendering.
---

# 2D Scenes, TileMaps, and Particles

## When to use
Use for 2D worlds, sprites, TileMaps, parallax, CanvasItem drawing, 2D lights, or particles.

## Inspect first
- Check project pixel scale, viewport/stretch settings, texture filtering, current scene ownership, and existing TileSet resources.
- Inspect the exact current-engine classes before using unfamiliar TileMap, particle, or canvas properties.

## Recommended order
1. Establish world scale, layers, camera bounds, and collision/navigation ownership.
2. Build reusable scenes and TileSet data before painting repeated layout.
3. Add animation, light, particles, and shaders only after geometry and draw order are stable.
4. Batch coherent node/property edits and keep authored assets reusable.

## Validate
Run at target resolutions; verify draw order, filtering, animation, collisions, navigation, light masks, particle bounds, and frame cost.

## Common failures
Hard-coded screen coordinates, mixed pixel scales, stale TileSet data, incorrect canvas layers, and particles whose visibility bounds clip.
