---
name: godot-3d-rendering
description: Godot 4 renderers, lights, materials, environments, global illumination, shadows, and post-processing.
---

# Godot 3D Rendering

## Scope
Use when working with Godot's 3D rendering pipeline, lighting, materials, environments, global illumination, shadows,
reflections, fog, exposure, or post-processing.

## Native model
- Forward+, Mobile, and Compatibility are distinct rendering methods with different feature and hardware support.
- A rendered surface combines geometry and normals, material/shader state, direct lights, environment and sky, reflection
  data, global illumination, camera attributes, and post-processing.
- DirectionalLight3D, OmniLight3D, and SpotLight3D provide direct light. Their cull masks select VisualInstance3D layers;
  shadow casting remains a separate GeometryInstance3D property.
- WorldEnvironment supplies the default Environment and CameraAttributes. Camera3D resources override corresponding
  world defaults for that camera.
- LightmapGI stores editor-baked static indirect light. Contributing meshes need static GI mode and valid UV2 data;
  dynamic geometry can receive indirect light from probes.
- VoxelGI is a baked-volume, real-time GI technique for Forward+ and small or medium scenes. SDFGI is a Forward+,
  camera-following GI technique for larger or procedurally generated worlds; it supports dynamic lights but not dynamic
  occluders or dynamic emissive surfaces.
- Physical light units are enabled project-wide and are interpreted together with CameraAttributes exposure.

## Compatibility and prerequisites
- Renderer support is defined by the current renderer feature table. Compatibility can display LightmapGI, but default
  lightmap baking requires RenderingDevice support; VoxelGI and SDFGI require Forward+.
- GI output depends on mesh GI mode, imported light-baking settings, UV2 availability, light bake mode, and current baked
  resources. Changes to bake inputs require new baked data where the selected technique is baked.

## Authoritative state
The project renderer, live World3D, active camera and overrides, native mesh/light/material properties, GI resources,
render diagnostics, and captured pixels are authoritative. A scene file or screenshot alone is not the complete state.

## Official references
- https://docs.godotengine.org/en/latest/tutorials/rendering/renderers.html
- https://docs.godotengine.org/en/latest/tutorials/3d/lights_and_shadows.html
- https://docs.godotengine.org/en/latest/tutorials/3d/environment_and_post_processing.html
- https://docs.godotengine.org/en/latest/tutorials/3d/global_illumination/introduction_to_global_illumination.html
- https://docs.godotengine.org/en/latest/tutorials/3d/global_illumination/using_lightmap_gi.html
- https://docs.godotengine.org/en/latest/tutorials/3d/global_illumination/using_voxel_gi.html
- https://docs.godotengine.org/en/latest/tutorials/3d/global_illumination/using_sdfgi.html
