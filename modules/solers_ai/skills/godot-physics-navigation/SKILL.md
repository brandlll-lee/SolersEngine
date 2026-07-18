---
name: godot-physics-navigation
description: Build and verify Godot 2D/3D bodies, collision, triggers, joints, character movement, navigation meshes, agents, and avoidance.
---

# Physics, Collision, and Navigation

## When to use
Use for movement, contacts, triggers, queries, joints, pathfinding, navigation regions, links, or avoidance.

## Inspect first
- Identify motion ownership, physics backend, body/shape types, layers/masks, actor dimensions, navigation maps, and existing scripts.
- Define required and forbidden contacts plus deterministic movement/path scenarios.

## Recommended order
1. Choose native body semantics matching static, character-driven, simulated, or detection-only ownership.
2. Use simple correctly scaled collision shapes and explicit layer/mask roles.
3. Run movement and queries in the correct physics phase with one velocity authority.
4. Derive navigation bake settings from actor dimensions; synchronize maps before requesting paths.
5. Add avoidance or links only for explicit requirements.

## Validate
Run contact, slope, edge, trigger, reachable, and unreachable cases; observe actual runtime state rather than inferring behavior from nodes.

## Common failures
Moving rigid bodies by transform, non-uniformly scaled shapes, all-to-all masks, stale navigation maps, repeated target resets, and doubled movement ownership.
