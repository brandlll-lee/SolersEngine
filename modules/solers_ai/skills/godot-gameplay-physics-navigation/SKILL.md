---
name: godot-gameplay-physics-navigation
description: Build and verify Godot gameplay movement, collision, interaction volumes, and 3D navigation using native physics and navigation semantics.
---

# Godot Gameplay, Physics, and Navigation

Use this skill for controllable actors, physical interactions, collision rules, triggers, pathfinding, or navigation behavior. Do not invoke it for a visual-only scene edit.

## Workflow

### 1. Define the behavior contract

Inspect the current scene and project before editing. Record:

- The actors, who owns their motion, and which bodies are player-controlled, scripted, simulated, static, or detection-only.
- Required movement states and constraints: grounded, airborne, slope, step, jump, push, knockback, moving platform, or other requested behavior.
- A collision matrix expressed as roles and intended contacts, not unexplained layer numbers.
- Navigation start/goal cases, agent dimensions, traversable regions, links, dynamic obstacles, and unreachable-target behavior.
- The smallest runtime scenarios that can prove success and expose regressions.

Create one outcome-oriented plan. Do not add physics or navigation systems that the contract does not require.

### 2. Inspect the existing implementation

- Use `editor.get_snapshot` to inspect body, shape, area, navigation, and script ownership.
- Search and read existing scenes and movement scripts before creating replacements.
- Use `class.search` and `class.introspect` for unfamiliar Godot classes, properties, methods, and signals.
- Preserve established input actions, collision conventions, and scene composition when they are valid.
- Treat the live scene, imported resources, ClassDB, and runtime state as authoritative. Do not infer behavior from node names.

### 3. Choose native semantics

Use the body type whose engine contract matches the requested ownership:

- Static geometry remains static.
- Script-driven characters move through `CharacterBody3D` collision APIs during physics ticks.
- Physics-driven objects remain under `RigidBody3D` control; do not overwrite their transforms every frame.
- Detection and overlap behavior uses `Area3D` and signals rather than polling unrelated scene state.
- Moving collision geometry uses the native body semantics appropriate to animation-driven motion.

Keep collision geometry simpler than visible geometry. Prefer primitive or convex shapes for moving bodies; reserve concave triangle collision for static level geometry. Set shape dimensions on the resource and avoid non-uniform collision scaling.

Define layers and masks from the contract so each contact is intentional. Do not fix missed collisions by enabling every layer.

### 4. Implement movement and interaction

- Run movement and physics queries in `_physics_process` unless the relevant Godot API explicitly requires another phase.
- Keep one source of truth for velocity and movement ownership.
- Apply gravity, floor/slope rules, acceleration, and stopping behavior explicitly from the requested design.
- Connect native signals for discrete overlap or contact events.
- Add scripts only where native nodes/resources cannot express behavior; patch existing scripts when ownership already exists.
- Validate every changed script with `script.validate` before runtime testing.

Build coherent node/property batches with `objects.batch`, then inspect the resulting scene instead of assuming mutation success.

### 5. Build navigation from actor constraints

- Derive navigation mesh agent radius, height, climb, and slope constraints from the controlled actor and level contract.
- Ensure regions share the intended navigation map and navigation layers.
- Use links only for explicit traversals such as jumps, ladders, or doors.
- Wait for the navigation server to synchronize before treating path results as evidence.
- For `NavigationAgent3D`, set a target when the target changes, query the next path position during physics updates, and stop when navigation is finished.
- Enable avoidance only when local dynamic avoidance is required. Keep pathfinding and avoidance failures diagnostically separate.
- Handle empty maps, partial/unreachable routes, and target cancellation explicitly.

Do not continuously rebake or reset targets to mask an incorrect navigation setup.

### 6. Verify in runtime

Run the current scene and execute the contract scenarios:

1. Exercise movement at rest, acceleration, stopping, slopes/edges, and requested transitions.
2. Test every intended and forbidden collision pair.
3. Trigger each interaction volume from valid and invalid actors.
4. Request representative navigation routes, including an unreachable or blocked case when relevant.
5. Observe rerouting and avoidance only if requested.
6. Capture stable runtime views that show body placement, contacts, and route outcome.

Use runtime state, collision/path results, and errors as primary evidence. Screenshots verify visible outcome but do not prove physics correctness.

## Acceptance gate

Before calling `done`, verify:

- Every actor uses the intended native motion owner and physics phase.
- Required contacts occur; forbidden contacts do not.
- Collision shapes align with visible and traversable space without unsupported scaling.
- Movement remains stable across the requested floor, slope, edge, platform, and airborne cases.
- Navigation is synchronized, starts and ends in valid regions, and handles unreachable targets without loops or stale motion.
- Navigation settings match actor dimensions; avoidance is present only when required.
- Changed scripts pass native validation and runtime produces no relevant errors.
- Final runtime capture and objective checks cover every contract scenario.
- All plan items are `completed`.

If a required runtime scenario cannot be exercised, pause with the exact missing precondition instead of claiming completion.
