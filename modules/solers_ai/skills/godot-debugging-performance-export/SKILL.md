---
name: godot-debugging-performance-export
description: Diagnose Godot runtime failures, profile measured bottlenecks, optimize native systems, validate export presets, and smoke-test packaged builds.
---

# Debugging, Performance, and Export

## When to use
Use for runtime errors, breakpoints, remote state, profiler work, optimization, export configuration, packaging, or release checks.

## Inspect first
- Define one reproducible scenario, target platform/build, expected result, performance budget if supplied, and required artifact.
- Read incremental runtime observations and project ownership before changing code or settings.

## Recommended order
1. Reproduce through `runtime.control`; use `runtime.observe` for errors, output, remote changes, and requested performance samples.
2. Fix deterministic failures at their owner and rerun the same scenario.
3. For performance, establish a comparable baseline and change only the measured bottleneck with native scaling features.
4. Validate the exact export preset before producing an explicitly requested artifact.
5. Smoke-test the packaged build outside the editor on its target environment.

## Validate
Compare the same scenario before/after; confirm errors are gone, metrics improve without visual/gameplay regressions, preset validation passes, and the artifact launches.

## Common failures
Diagnosing from FPS alone, checklist optimization, changing several bottlenecks at once, treating editor play as export proof, and claiming target readiness without hardware evidence.
