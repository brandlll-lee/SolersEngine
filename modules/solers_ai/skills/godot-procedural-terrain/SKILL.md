---
name: godot-procedural-terrain
description: Procedural heightfields, Noise/Mesh/MultiMesh terrain, collision and navigation bake, LOD, and third-party terrain addons only through addon.inspect Contracts.
---

# Procedural Terrain and Large Landscapes

## When to use
Use for heightmaps, procedural ground meshes, vegetation instancing, terrain collision/navigation, LOD, or installing a terrain **addon**. Do not invent engine `terrain.*` tools.

## Facts
| Piece | Role |
|-------|------|
| Built-in path | `Noise`/`FastNoiseLite` → `Image` height → `ArrayMesh`/`SurfaceTool` or `HeightMapShape3D` |
| Instancing | `MultiMeshInstance3D` for rocks/grass; density by slope/height masks |
| Collision | Separate simplified collision from visual mesh when needed |
| Navigation | Bake `NavigationRegion3D` from walkable surfaces after geometry stable |
| LOD | Distance swap / clipmap-style sections; measure far views |
| Addons | Any terrain plugin: `addon.inspect` → Contract → ClassDB — pin version |

## Laws
- Prefer Godot primitives until an addon Contract is required and trusted.
- Never guess addon method names; never invent Solers `terrain.*` APIs.
- Establish world scale (1 unit ≈ 1 m) and height range before sculpting density.
- Visual foliage is not collision — author collision deliberately.
- Persist whatever the Contract says is authoritative (regions/data_directory/etc.).

## Recipes
**Native height mesh:** noise image → displace plane grid → create collision shape → sample material by slope.
**Vegetation:** mask steep slopes out → MultiMesh with random yaw/scale → LOD cull far.
**Addon path:** `addon.inspect` → follow Contract workflow/validation steps → save owning resources → reopen scene.

## Traps
| Wrong | Correct |
|-------|---------|
| Hardcoding a vendor terrain class in generic plans | Contract from `addon.inspect` |
| Inventing `terrain.sculpt` tools | `scene.edit` / addon ClassDB / native mesh |
| Height scale mismatch (cm vs m) | Measure extents; fix noise amplitude |
| Navmesh before holes/collision final | Bake after walkable surface settles |
| No far-camera perf check | Capture near + far; watch draw cost |

## Verify
1. `scene.inspect` / `resource.inspect` bounds, materials, collision, nav.
2. `runtime.control` traversal; near/far `viewport.capture`.
3. `runtime.observe` for shader/addon errors after reload.
4. If addon: Contract validation checklist must pass (regions present, data reloads).
