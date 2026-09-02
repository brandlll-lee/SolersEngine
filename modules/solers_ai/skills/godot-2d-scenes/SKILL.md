---
name: godot-2d-scenes
description: Godot 4 2D scenes with TileMapLayer (not legacy TileMap API), sprites, Parallax2D, CanvasItem order, 2D lights/occluders, and resolution verification.
---

# 2D Scenes, TileMaps, and Canvas

## When to use
Use for 2D worlds, sprites, tile maps, parallax, CanvasItem draw order, 2D lights/particles, or pixel-art filtering. 3D → `godot-3d-rendering`. Particles VFX detail → `godot-vfx-particles` when present.

## Facts
| Piece | Role |
|-------|------|
| `TileMapLayer` | **Authoritative** tile painting node in Godot 4.3+ (physics/nav/terrain per layer data) |
| `TileMap` | Legacy container — prefer per-layer `TileMapLayer` nodes; do not treat old multi-arg `set_cell` as the only API |
| `TileSet` / `TileSetAtlasSource` | Atlas, collision, navigation, terrain peering data |
| `Sprite2D` / `AnimatedSprite2D` | Billboards of texture frames |
| `Parallax2D` | Modern parallax (prefer over old `ParallaxBackground`+`ParallaxLayer` stacks when starting new) |
| `CanvasLayer` / `CanvasGroup` | Draw stack isolation / grouping |
| `Camera2D` | 2D view; limits, drag margins, position smoothing |
| `PointLight2D` / `LightOccluder2D` | 2D lighting |
| Project stretch | `display/window/stretch/mode` + `aspect` — world units ≠ screen pixels unless designed so |

## Laws
- Establish world scale and stretch mode before hard-coding positions.
- Paint from a shared `TileSet`; do not duplicate atlases per room without reason.
- Filtering is often **per CanvasItem** (`texture_filter`) — do not assume one global nearest/linear forever.
- Y-sort belongs on the node that owns draw order (layer / parent), not “somewhere random”.

## Recipes
**Pixel art starter:** window stretch `canvas_items` or `viewport`, aspect `keep`; sprites/TileMapLayer `texture_filter` = nearest.
**Tile workflow:** create `TileSet` → atlas source → configure physics/nav layers on tiles → add `TileMapLayer`(s) → paint.
**Draw order:** background `CanvasLayer` (low) → world → HUD `CanvasLayer` (high). Use `z_index` within a layer sparingly.
**Camera bounds:** `Camera2D.limit_*` from map rect; enable position smoothing only if desired.

## Traps
| Wrong | Correct |
|-------|---------|
| Assuming Godot 3 `TileMap.set_cell(x,y,tile)` is the 4.x way | Use `TileMapLayer` APIs / current ClassDB (`engine.describe`) |
| Mixing pixel sizes (16px tiles + 1080p free camera) | One PPU / stretch contract |
| Hard-coded screen coordinates for world gameplay | World space + Camera2D / UI on separate CanvasLayer |
| Stale TileSet after atlas resize | Reimport / fix atlas; re-assign sources |
| Particles clipped | Expand visibility rect / check parent clip |
| Expecting Container-less Control layout rules in 2D world | World = Node2D tree; UI = Control tree |

## Verify
1. `scene.inspect` for layers, TileSet paths, Camera2D limits, and `project.settings` for stretch settings.
2. `runtime.control` at target resolutions.
3. `render.capture` — draw order, filtering, parallax, light masks.
4. `runtime.observe` for missing textures / tile errors.
