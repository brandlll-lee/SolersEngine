---
name: godot-scripting-input-gameplay
description: Implement Godot gameplay with minimal scripts, native lifecycle and signals, InputMap actions, deterministic state ownership, and runtime checks.
---

# Scripting, Input, and Gameplay

## When to use
Use for game rules, interaction logic, input, lifecycle behavior, state, save data, or reusable project scripts.

## Inspect first
- Locate the owning scene/script, related signals, InputMap, Autoloads, tests, and callers with `project.search`.
- Read before patching and inspect native classes before replacing built-in behavior with code.

## Recommended order
1. Define observable behavior and one owner for each mutable state.
2. Reuse nodes, resources, signals, and existing scripts; write the smallest typed script needed.
3. Use `_process`, `_physics_process`, and input callbacks only for work matching their lifecycle.
4. Patch with revision/hash guards, validate, then run deterministic scenarios.

## Validate
Check syntax, startup, input paths, state transitions, errors, reload/save behavior, and the requested gameplay outcome.

## Common failures
Polling instead of signals, duplicate state machines, raw key checks instead of InputMap, frame-rate-dependent logic, and editing the wrong scene owner.
