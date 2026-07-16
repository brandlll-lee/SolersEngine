---
name: godot-performance-release
description: Measure, optimize, and release Godot projects using profiler evidence, native scalability features, export preset validation, and packaged-build acceptance checks.
requires_tools:
  - editor.get_snapshot
  - project.search_files
  - project.read_file
  - class.search
  - class.introspect
  - objects.batch
  - script.validate
  - runtime.control
  - viewport.capture
  - export.list_presets
  - export.validate_presets
  - export.run_preset
---

# Godot Performance and Release

Use this skill when the user asks to diagnose performance, optimize a representative workload, validate export configuration, or prepare a release artifact.

## Workflow

### 1. Define the performance and release contract

Inspect the project and record:

- Target platforms, renderers, hardware classes, display modes, and release presets.
- Representative scenes and deterministic interactions to measure.
- User-provided or project-defined frame-time, memory, loading, and package constraints.
- Visual and gameplay invariants that optimization must preserve.
- Required release artifacts and the environments in which they must launch.

Do not invent pass/fail thresholds. If the project has no budget, establish a measured baseline and report relative change without labeling it acceptable.

### 2. Establish an authoritative baseline

- Validate that the chosen scenario can be repeated with the same camera, content, simulation state, and duration.
- Measure a release-like build on target hardware whenever possible; identify editor-only measurements explicitly.
- Use Godot's Profiler for script/physics activity and the Visual Profiler for render CPU/GPU work.
- Record the dominant frame-time owner, memory pressure, loading behavior, draw/object counts, and relevant warnings.
- Separate CPU, render-thread, GPU, physics, memory, and loading evidence. Do not diagnose a bottleneck from FPS alone.

If current tools cannot access the required profiler capture or target device, request that evidence or pause. Never replace missing measurements with a node-name or scene-size heuristic.

### 3. Inspect only the measured bottleneck

Use scene snapshots, project search, resource metadata, and ClassDB inspection to trace the dominant cost to its authoritative owner.

Choose the smallest native correction that matches the evidence:

- Disable processing for inactive nodes instead of adding a global polling manager.
- Share resources and use `MultiMeshInstance3D` for genuinely repeated mesh/material draws.
- Use mesh LOD and visibility ranges/HLOD where distance reduces required detail.
- Use occlusion culling where stable occluders and scene structure make hidden geometry the measured problem.
- Reduce expensive lights, shadows, transparency, post-processing, or resolution only when GPU evidence points there.
- Simplify collision and reduce unnecessary active physics work only when physics measurements justify it.
- Fix loading or memory pressure through import/resource ownership and reuse rather than duplicated runtime caches.

Do not apply every optimization technique as a checklist. Change one coherent bottleneck group at a time.

### 4. Re-measure and guard quality

Repeat the exact baseline scenario after each coherent change:

1. Compare the same profiler categories and target build conditions.
2. Confirm the dominant cost changed in the intended direction.
3. Capture the same editor/runtime view and compare visual invariants.
4. Re-run affected gameplay, physics, navigation, and animation checks.
5. Revert changes that do not produce measurable value or that shift cost without improving the contract.

Validate any changed scripts before measurement. Keep the shortest diff that produces evidence-backed improvement.

### 5. Validate and export

- List export presets and select the exact preset requested by the user.
- Validate presets before exporting; treat missing templates, credentials, platform tooling, and invalid options as authoritative blockers.
- Preserve platform-specific options and feature tags already owned by the preset.
- Run `export.run_preset` only for an explicitly requested artifact and report its exact output path.
- Test the packaged release outside the editor on the required environment. Exercise startup, initial scene load, input, save/config paths, shutdown, and the representative performance scenario.

An editor run is not proof that an exported artifact launches. A successful export command is not proof that the packaged game works.

## Acceptance gate

Before calling `done`, verify:

- Baseline and final measurements use the same scenario, build type, settings, and hardware.
- The reported bottleneck is supported by profiler or platform evidence.
- Each retained optimization maps directly to that evidence and improves the declared metric.
- Visual and gameplay invariants remain intact in stable comparison captures and runtime checks.
- No speculative manager, duplicate cache, or parallel implementation was added.
- Changed scripts pass native validation and the final run has no relevant errors.
- The selected export preset validates and produces the requested artifact.
- The packaged artifact launches and completes the required smoke scenario on its target environment.
- All plan items are `completed`.

If target-hardware measurement or packaged-build execution is unavailable, report the verified subset and pause with that exact external requirement. Do not claim release readiness.
