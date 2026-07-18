---
name: godot-xr-mobile-platforms
description: Adapt Godot projects for XR, mobile, web, and platform-specific rendering, input, plugins, permissions, and lifecycle constraints.
---

# XR, Mobile, and Platforms

## When to use
Use for OpenXR/WebXR, headsets, tracked input, Android/iOS/Web, sensors, touch, permissions, native plugins, or platform behavior.

## Inspect first
- Read target platform, renderer, export preset, enabled interfaces/extensions, device capabilities, input map, permissions, and plugin compatibility.
- Treat current ClassDB and loaded platform APIs as authoritative; features vary by build and renderer.

## Recommended order
1. Keep gameplay platform-neutral and isolate platform-specific capabilities behind native interfaces.
2. Choose renderer, resolution, input, UI scale, and lifecycle behavior for target hardware.
3. Query XR/device capabilities before enabling optional features; provide deliberate fallbacks.
4. Configure permissions and native plugins only in the owning export/platform settings.

## Validate
Run an exported build on target hardware; test startup/resume, tracking or touch, orientation, permissions, thermal/frame budget, UI readability, and fallback behavior.

## Common failures
Editor-only proof, desktop renderer assumptions, missing mobile lifecycle handling, unguarded XR extensions, and platform permissions inferred from code alone.
