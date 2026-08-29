---
name: godot-xr-mobile-platforms
description: Godot 4 OpenXR/mobile/web constraints - renderer choice, lifecycle, permissions, XR interfaces, touch/tracked input, and on-device verification.
tools: project.settings, runtime.observe, runtime.control, export.list_presets, export.validate_presets, export.run_preset
---

# XR, Mobile, and Platforms

## When to use
Use for OpenXR/WebXR, headsets, tracked controllers, Android/iOS/Web export, sensors, touch, permissions, or platform plugins.

## Facts
| Piece | Role |
|-------|------|
| Renderer | Mobile/Compatibility often required; Forward+ features may be unavailable |
| XR | `XRServer` / OpenXR interface / `XROrigin3D` / `XRCamera3D` / controllers |
| Lifecycle | `NOTIFICATION_APPLICATION_FOCUS_*` / pause — mobile and XR both interrupt |
| Input | Touch, joypad, XR poses — map through InputMap where possible |
| Permissions | Export preset / platform config owns them — not scene scripts alone |
| UI scale | DPI / stretch; headset overlay vs flat mobile differ |

## Laws
- Query capabilities before enabling optional XR extensions; provide fallbacks.
- Keep core gameplay platform-neutral; isolate platform code behind interfaces.
- Editor play is not device proof — validate exported builds on hardware.
- Never invent platform plugin APIs — inspect ClassDB / addon Contract.

## Recipes
**XR minimum:** enable OpenXR in project → `XROrigin3D` + camera + controllers → start interface → test tracking lost.
**Mobile:** Mobile renderer, touch UI targets ≥ ~48dp, handle focus loss pausing games.
**Web:** Compatibility renderer expectations; async load; user-gesture audio.

## Traps
| Wrong | Correct |
|-------|---------|
| Desktop-only lighting features on mobile XR | Match renderer feature matrix |
| Unguarded OpenXR calls | Check interface init / session state |
| Permissions assumed from GDScript | Export preset + OS permission APIs |
| Tiny UI for finger/controller | Platform-sized hit targets |
| Skipping resume after interrupt | Test background/resume explicitly |

## Verify
1. Inspect project renderer, XR settings, export preset, permissions.
2. Prefer exported build on target device/headset.
3. `runtime.observe` (where available) for interface/init errors.
4. Exercise pause/resume, tracking loss, and input fallbacks.
