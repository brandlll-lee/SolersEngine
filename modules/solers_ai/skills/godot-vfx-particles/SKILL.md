---
name: godot-vfx-particles
description: Godot 4 particle simulation, process materials, particle shaders, draw passes, trails, decals, and bounds.
---

# Godot VFX and Particles

## Scope
Use when working with 2D or 3D particles, particle shaders, process materials, trails, decals, or effect rendering.

## Native model
- GPU particle nodes simulate on the GPU; CPU particle nodes simulate on the CPU while their visual material is still
  rendered by the graphics pipeline.
- ParticleProcessMaterial provides standard emission shape, velocity, acceleration, damping, scale, color, animation, and
  collision behavior. A particle ShaderMaterial provides lower-level `start()` and `process()` control.
- A particle emitter's amount, lifetime, one-shot state, explosiveness, randomness, fixed FPS, interpolation, preprocess,
  local coordinates, and seed define simulation timing and reproducibility.
- 3D draw passes provide the Mesh resources rendered for particles; their materials determine spatial shading,
  transparency, and trail support.
- Visibility AABB defines the 3D GPU particle simulation and rendering bounds used for culling and particle interaction.
- Particle trails require trail simulation plus a supported trail mesh and material. Particle collision uses configured
  collision bases and renderer-supported collision nodes.
- Decal projects material channels onto eligible GeometryInstance3D layers without changing the underlying mesh material.

## Compatibility and prerequisites
- GPU particle features, emission APIs, collision, decals, trails, and limits depend on renderer and target hardware.
- Transparency, overdraw, light interaction, simulation rate, particle count, and bounds contribute independently to cost.

## Authoritative state
Emitter and process properties, active ParticleProcessMaterial or shader, draw meshes and materials, visibility bounds,
renderer support, runtime simulation state, render diagnostics, profiler data, and captured frames are authoritative.

## Official references
- https://docs.godotengine.org/en/latest/tutorials/3d/particles/index.html
- https://docs.godotengine.org/en/latest/tutorials/3d/particles/properties.html
- https://docs.godotengine.org/en/latest/classes/class_gpuparticles3d.html
- https://docs.godotengine.org/en/latest/classes/class_cpuparticles3d.html
- https://docs.godotengine.org/en/latest/tutorials/shaders/shader_reference/particle_shader.html
- https://docs.godotengine.org/en/latest/classes/class_decal.html
