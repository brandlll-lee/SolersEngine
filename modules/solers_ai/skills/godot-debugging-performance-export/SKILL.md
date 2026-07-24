---
name: godot-debugging-performance-export
description: Reproduce Godot failures with runtime.observe, profile one bottleneck, apply native fixes, validate export presets, and smoke-test packaged builds.
---

# Debugging, Performance, and Export

## When to use
Use for runtime errors, profiling, optimization, export presets, packaging, or release smoke tests.

## Facts
| Piece | Role |
|-------|------|
| Reproduce | Same `runtime.control` scenario every time |
| Observe | `runtime.observe` errors, output, optional performance samples |
| Profiler | Godot profiler / Debugger — measure before changing |
| Bottleneck classes | CPU script, draw calls, GPU fill, physics, loading spikes |
| Export | Editor export presets + platform features; editor play ≠ device proof |
| Native levers | LOD, occlusion, pooling, baked lighting, fewer materials, thread imports |

## Laws
- One reproducible scenario; fix the owner of the failure first.
- Performance: baseline → change **one** measured bottleneck → remeasure.
- Do not claim platform readiness from editor FPS alone.
- Export only with an explicit preset that matches the target renderer/features.

## Recipes
**Crash/error loop:** reproduce → read observe stack → fix owner → same scenario green.
**Perf loop:** capture time/draw stats → identify top cost → apply one native mitigation → compare.
**Export loop:** validate preset → export → run artifact on target → smoke input/render/audio.

## Traps
| Wrong | Correct |
|-------|---------|
| Optimizing from FPS folklore | Instrument + single change |
| Changing five systems at once | Isolate variables |
| “Works in editor” as shipping proof | Packaged build on target |
| Ignoring first-load stutters | Measure load/import/compile separately |
| Disabling features randomly | Tie change to measured cost |

## Verify
1. Same scenario before/after via `runtime.control` + `runtime.observe`.
2. Optional `viewport.capture` if visual regression risk.
3. Export preset validation; artifact launch checklist.
4. Stop when scenario passes and metrics meet the stated budget (or report the remaining gap truthfully).
