---
name: godot-xr-mobile-platforms
description: Godot 4 OpenXR, mobile and web platforms, rendering constraints, lifecycle, input, permissions, and export.
---

# Godot XR, Mobile, and Platforms

## Scope
Use when working with OpenXR, Android, iOS, Web, touch and sensor input, platform lifecycle, permissions, or device export.

## Native model
- XRServer owns XR interfaces and trackers. OpenXRInterface connects Godot to an OpenXR runtime and must be initialized
  with project and startup configuration compatible with the selected graphics backend.
- XROrigin3D maps tracking space into the game world. XRCamera3D and XRController3D consume tracked poses and are children
  of that origin.
- The OpenXR action map binds logical actions and poses to interaction profiles. Runtime trackers provide current pose,
  confidence, and input state for supported devices.
- Reference space defines how the tracking origin relates to the user and how recentering is interpreted.
- Forward+, Mobile, and Compatibility have different platform, XR, post-processing, and rendering costs. XR uses
  stereoscopic Viewports and runtime-controlled frame timing.
- Mobile and Web exports have platform-specific application lifecycle, window, storage, permission, input, graphics API,
  and packaging rules.
- ExportPreset, enabled plugins or extensions, architecture, renderer, and permissions define the deployed application.

## Compatibility and prerequisites
- Runtime, headset or device, graphics backend, OpenXR extensions, vendor plugin, browser, and export template determine
  available capabilities.
- Editor execution does not reproduce every permission, lifecycle, performance, or packaging property of a deployed build.

## Authoritative state
Project and export settings, initialized XRInterface, runtime trackers and poses, action map, platform capability reports,
permission results, lifecycle events, device diagnostics, frame timing, and deployed-build output are authoritative.

## Official references
- https://docs.godotengine.org/en/latest/tutorials/xr/setting_up_xr.html
- https://docs.godotengine.org/en/latest/tutorials/xr/openxr_settings.html
- https://docs.godotengine.org/en/latest/tutorials/platform/android/index.html
- https://docs.godotengine.org/en/latest/tutorials/platform/ios/index.html
- https://docs.godotengine.org/en/latest/tutorials/platform/web/index.html
- https://docs.godotengine.org/en/latest/tutorials/rendering/renderers.html
- https://docs.godotengine.org/en/latest/tutorials/export/index.html
