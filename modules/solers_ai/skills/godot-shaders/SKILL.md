---
name: godot-shaders
description: Godot 4 shading language, shader types, stages, render modes, uniforms, materials, and renderer support.
---

# Godot Shaders

## Scope
Use when working with Godot shader source, ShaderMaterial, visual shaders, custom rendering, or shader-driven simulation.

## Native model
- Godot's shading language is GPU-oriented. Every shader declares a type; supported processor functions, built-ins, and
  render modes are defined by that type.
- Vertex, fragment, and light processors handle different stages of spatial and canvas rendering. Particle shaders update
  particle state before another material draws the particle geometry.
- Render modes alter pipeline behavior such as blending, depth, culling, lighting, transforms, and world coordinates.
- Uniforms are material inputs. Global uniforms are project-wide; instance uniforms vary selected GeometryInstance3D
  values without duplicating the material.
- ShaderMaterial binds a Shader and its uniform values. Mesh surfaces and CanvasItems determine where that material is used.
- Reading or writing `ALPHA` in a spatial shader selects the transparent pipeline, which has different depth, shadow,
  sorting, and screen-texture behavior from opaque rendering.
- Screen, depth, and normal-roughness textures use explicit sampler hints and have renderer-specific availability.

## Compatibility and prerequisites
- Shader built-ins, render modes, texture hints, precision, compute support, and limits depend on shader type, renderer,
  graphics API, and target hardware.
- Generated normals, tangents, UV channels, material ownership, and draw geometry are inputs to shader execution.

## Authoritative state
The active renderer, compiled Shader resource, compiler diagnostics, material and instance uniforms, mesh attributes,
render modes, RenderingServer state, and rendered output are authoritative.

## Official references
- https://docs.godotengine.org/en/latest/tutorials/shaders/introduction_to_shaders.html
- https://docs.godotengine.org/en/latest/tutorials/shaders/shader_reference/shading_language.html
- https://docs.godotengine.org/en/latest/tutorials/shaders/shader_reference/spatial_shader.html
- https://docs.godotengine.org/en/latest/tutorials/shaders/shader_reference/canvas_item_shader.html
- https://docs.godotengine.org/en/latest/tutorials/shaders/shader_reference/particle_shader.html
- https://docs.godotengine.org/en/latest/tutorials/shaders/screen-reading_shaders.html
