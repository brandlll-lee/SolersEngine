---
name: godot-shaders
description: Godot 4 shading language for Spatial/CanvasItem/Particles/Sky, uniforms, varyings, render modes, light/fragment hooks, and verification without inventing built-ins.
---

# Shaders

## When to use
Use for `.gdshader` / ShaderMaterial work: custom lighting response, dissolve, outlines, water, toon, fullscreen post via backbuffer, particle shaders, or sky. Prefer `StandardMaterial3D` until a shader is actually required.

## Facts
| Piece | Role |
|-------|------|
| Types | `shader_type spatial;` / `canvas_item;` / `particles;` / `sky;` |
| Pipeline | `vertex()` → `fragment()`; optional `light()` in spatial |
| Uniforms | `uniform` with hints (`source_color`, `hint_range`, `hint_normal`, …) |
| Varyings | Pass vertex → fragment; precision matters on mobile |
| Render modes | `unshaded`, `cull_disabled`, `depth_draw_*`, `blend_*`, `diffuse_*`, `specular_*` |
| Built-ins | `VERTEX`, `NORMAL`, `UV`, `COLOR`, `ALBEDO`, `ROUGHNESS`, `METALLIC`, `ALPHA`, `TIME`, … — **ClassDB/docs only**, never invent |
| Convert | `StandardMaterial3D` → convert to shader code as a starting point when stuck |

## Laws
- Do not invent shader built-ins or render_mode tokens — verify against engine docs/`engine.describe` materials.
- Keep one concern per shader; stack materials sparingly.
- Mobile/XR: minimize dependent texture reads and overdraw; test on target renderer.
- Drive gameplay knobs via `set_shader_parameter` — not by rewriting shader text each frame.

## Recipes
**Dissolve:** noise sample → `ALPHA` cut with uniform threshold; optional edge emission near cut.
**Fresnel highlight:** `fresnel = pow(1.0 - dot(NORMAL, VIEW), exp)` → mix into ALBEDO/EMISSION.
**Canvas flash:** `canvas_item` fragment modulates `COLOR` with uniform pulse on `TIME`.
**Damage blend (3D):** mix base albedo with crack albedo by uniform `damage` and mask texture.

## Traps
| Wrong | Correct |
|-------|---------|
| Copying Unity/Unreal HLSL as-is | Godot shading language + Godot built-ins |
| Inventing `TEXTURE_PIXEL_SIZE`-like names | Check current built-in list for the shader_type |
| Ignoring `render_mode` cull/depth | Match transparency and sorting needs |
| Huge shader when ORM material suffices | Stay on Standard/ORM until forced |
| Editing imported material internals | ShaderMaterial on a wrapper mesh/surface |

## Verify
1. Assign ShaderMaterial; `object.query target=resource` for uniforms.
2. Static look: `render.capture target=camera|editor` + `source_state`; use runtime only for gameplay lighting.
3. `runtime.observe` only when Play is needed for compile/runtime errors.
4. Toggle parameters via script once to prove the contract.
