---
name: godot-camera-cinematography
description: Godot 4 cameras, viewports, projections, culling, camera attributes, and cinematic control.
---

# Godot Cameras and Cinematography

## Scope
Use when working with Camera2D, Camera3D, Viewports, projection, framing, camera attributes, transitions, or cutscenes.

## Native model
- Each Viewport has one current 2D camera and one current 3D camera. A camera affects its nearest parent Viewport.
- Camera2D changes the 2D canvas transform and provides position, rotation, zoom, limits, drag, smoothing, and process
  timing controls.
- Camera3D defines perspective, orthogonal, or frustum projection, near and far clipping, cull mask, environment override,
  and optional CameraAttributes.
- Camera3D cull masks select VisualInstance3D render layers. They do not replace light, physics, or navigation masks.
- CameraAttributesPractical and CameraAttributesPhysical define exposure and depth of field. Camera attributes assigned to
  the current Camera3D override corresponding WorldEnvironment defaults.
- Camera motion may be authored by animation, gameplay scripts, physics interpolation, or a parent rig; the final global
  transform is the value consumed by the Viewport.

## Compatibility and prerequisites
- Depth of field, environment effects, and projection behavior depend on the renderer and active camera resource type.
- SubViewport size and update mode determine the image available to camera consumers and textures.

## Authoritative state
The current camera reported by the Viewport, camera global transform and projection, cull layers, effective environment
and CameraAttributes, runtime ownership, and rendered viewport are authoritative.

## Official references
- https://docs.godotengine.org/en/latest/tutorials/rendering/viewports.html
- https://docs.godotengine.org/en/latest/classes/class_camera2d.html
- https://docs.godotengine.org/en/latest/classes/class_camera3d.html
- https://docs.godotengine.org/en/latest/classes/class_cameraattributes.html
- https://docs.godotengine.org/en/latest/tutorials/3d/environment_and_post_processing.html
