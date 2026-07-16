---
name: godot-native-capabilities
description: On-demand map of Godot's native editor, rendering, scene, scripting, UI, audio, physics, navigation, XR, networking, and asset APIs. Use it to choose a native class or service before inventing code.
requires_tools:
  - class.search
  - class.introspect
  - tool.search
  - skill.read
  - project.search_files
  - resource.get_info
  - editor.get_snapshot
---

# Godot Native Capabilities

Solers exposes Godot's real ClassDB and editor services. Use the smallest native composition that satisfies the request. Search and introspect an unfamiliar class once, then call the native tool directly; do not guess property names or create a Solers wrapper for an existing Godot API.

## Platform and editor

Godot runs the editor and exported projects on Windows, macOS, Linux, and Android (editor support is experimental). Projects can export to iOS and Web, and may target consoles through supported platform partners. The editor provides the scene tree, script editor, debugger, profiler, performance monitors, remote inspector, live scene reload, camera preview, offline class reference, localization, and editor plugins. Use `editor.get_snapshot` for editor state and `viewport.capture` for rendered evidence; a tree snapshot is not a rendered frame.

## Native domains

- **Rendering:** choose Forward+ for desktop features such as SDFGI, VoxelGI, SSIL, and LightmapGI; Mobile for constrained devices; Compatibility for OpenGL/Web/older hardware. Configure HDR, Environment, CameraAttributes, lights, shadows, GI, reflection probes, fog, particles, and post effects through their native nodes/resources.
- **2D:** use `Node2D`, `CanvasItem`, `Sprite2D`, `Polygon2D`, `Line2D`, `GPUParticles2D`, `Light2D`, `TileMap`, parallax nodes, `AnimationPlayer`, and CanvasItem shaders. Keep pixel scale and filtering explicit.
- **3D:** use `Node3D` plus native meshes, CSG for whitebox only, imported glTF/Blender assets, PBR materials, cameras, lights, worlds, particles, terrain, and physics bodies. Convert stable CSG to mesh before UV2/lightmap baking.
- **Animation:** use `AnimationPlayer`, `AnimationTree`, skeleton/skin resources, blend spaces, and property tracks. Almost every node property is animatable; inspect the class rather than adding a parallel timeline system.
- **Physics:** use GodotPhysics or Jolt backends, `RigidBody*`, `CharacterBody*`, `Area*`, shapes, joints, ray/shape queries, interpolation, and collision layers/masks. Keep collision geometry simpler than render geometry.
- **Scripting:** use GDScript or C# for normal project code, GDExtension for native performance without rebuilding the engine, and signals for decoupled node composition. Validate edited scripts through the registered language.
- **UI:** use `Control`, containers, themes, anchors, focus, accessibility, and input events. Let containers solve responsive layout instead of hard-coded viewport coordinates.
- **Audio:** use `AudioStreamPlayer`/`AudioStreamPlayer3D`, buses, effects, attenuation, and reverb. Keep bus routing in project resources so it is inspectable and reusable.
- **Navigation/AI:** use `NavigationRegion3D/2D`, nav meshes, `NavigationAgent*`, path queries, avoidance, and regions/links. Bake navigation after walkable geometry is stable.
- **XR:** use OpenXR/WebXR and XR tools for headsets, tracked hands/bodies, spatial anchors, and passthrough extensions. Query the active interface before assuming a device feature.
- **Networking:** use high-level multiplayer for replicated scenes/RPC and low-level TCP/UDP/HTTP/WebSocket APIs for custom protocols. Declare authority, ownership, and serialization explicitly.
- **Assets:** let `EditorFileSystem` and `ResourceLoader` handle imported resources. Use `FileAccess` only for raw/non-imported bytes, then wait for native filesystem/import signals and verify indexed, import-valid, and loadable resources.

## Operating rule

Use `class.search` -> `class.introspect` -> native scene/resource operation. Inspect the live scene and project settings before editing. When native composition cannot express behavior, write the smallest project script and run the native validator. Keep renderer-specific features behind the selected renderer's actual support matrix.
