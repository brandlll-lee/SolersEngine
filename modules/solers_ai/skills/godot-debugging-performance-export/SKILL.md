---
name: godot-debugging-performance-export
description: Godot 4 diagnostics, debugger, profiling, performance monitors, command line, and export.
---

# Godot Debugging, Performance, and Export

## Scope
Use when working with Godot diagnostics, debugger sessions, profiling, engine monitors, command-line runs, or exports.

## Native model
- Parser, script, shader, import, engine, and platform diagnostics are produced by different subsystems and retain their
  own source locations, severity, and runtime context.
- The debugger receives errors, breakpoints, stack frames, inspectors, profilers, and monitors from a running debug game.
- The profiler samples frame categories and script functions while enabled. Performance exposes engine counters such as
  frame rate, memory, object counts, draw calls, and physics activity.
- Performance monitors have subsystem-specific update rates and availability; some return no data in release builds.
- ExportPreset defines platform and feature configuration plus resource inclusion. Export templates provide the target
  runtime binaries; `export_presets.cfg` and export credentials have different persistence and secrecy requirements.
- Godot's command line can run projects or scenes, execute tests and scripts, and perform debug or release exports.

## Compatibility and prerequisites
- Profiling, debugging, rendering counters, and export options depend on build type, language, platform, and renderer.
- An exported project uses packaged resources and platform lifecycle rules that differ from an editor debug run.

## Authoritative state
Complete diagnostics, stack frames, profiler captures, Performance monitor values, active export preset, exporter result,
produced artifact, and target-platform runtime output are authoritative.

## Official references
- https://docs.godotengine.org/en/latest/tutorials/scripting/debug/index.html
- https://docs.godotengine.org/en/latest/tutorials/scripting/debug/the_profiler.html
- https://docs.godotengine.org/en/latest/tutorials/performance/index.html
- https://docs.godotengine.org/en/latest/classes/class_performance.html
- https://docs.godotengine.org/en/latest/tutorials/export/exporting_projects.html
- https://docs.godotengine.org/en/latest/tutorials/editor/command_line_tutorial.html
