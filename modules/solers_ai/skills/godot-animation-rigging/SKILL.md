---
name: godot-animation-rigging
description: Godot 4 AnimationPlayer/AnimationTree, Skeleton3D retarget, bone maps, blend spaces, IK, root motion authority, and import-safe animation wrappers.
---

# Animation, Rigging, and State Machines

## When to use
Use for clips, skeletons, retargeting, AnimationTree graphs, IK, one-shots, or root motion. Mesh/material look → `godot-3d-rendering`. Camera blends → `godot-camera-cinematography`.

## Facts
| Piece | Role |
|-------|------|
| `AnimationPlayer` | Owns animation libraries/tracks |
| `AnimationTree` | Blends / state machines / blend spaces on top of a player |
| `Skeleton3D` / `Skeleton2D` | Bones, rest pose, modifiers |
| `BoneMap` / retarget | Profile mapping for imported humanoids |
| Importer | Owns GLB/FBX animation — edit wrappers / inherited scenes, not raw import dumps |
| Root motion | `AnimationTree` / player root motion → **or** gameplay `velocity`, never both as authority |
| IK | `SkeletonIK3D` / modifiers — secondary to authored poses |

## Laws
- One locomotion authority: root motion **or** CharacterBody velocity.
- Do not hand-edit importer output as the long-term source of truth.
- Verify rest pose + bone map before blaming blend graphs.
- AnimationTree advances with the player process mode — match physics/render intent.

## Recipes
**Clip only:** `AnimationPlayer.play("run")` for simple props/UI.
**Locomotion graph:** Player + Tree; `AnimationNodeStateMachine` with Idle/Walk/Run; blend space 1D on speed.
**Retarget:** import with bone map → confirm rest → then build Tree.
**One-shot:** `AnimationNodeOneShot` for attack/emote over base locomotion.

## Traps
| Wrong | Correct |
|-------|---------|
| Editing `.glb` import internals for tweaks | Inherited scene / extra library |
| Script `position` + root motion together | Pick one authority |
| Broken elbows from bad retarget “fixed” in Tree | Fix BoneMap / rest first |
| Godot 3 `AnimationTreePlayer` mental model | Current `AnimationTree` node graph |
| Ignoring loop end pops | Check track interpolation and loop wrap |

## Verify
1. `object.query target=resource|scene` for skeleton, libraries, and Tree parameters.
2. `runtime.control` — each clip, interrupt, blend; watch feet/hands.
3. For locomotion transitions (Idle/Walk/Run): drive state through the gameplay API or a project test script, then `render.capture` while moving — do not claim transitions from a static Idle frame alone.
4. `runtime.observe` digest for missing tracks / errors.
5. Confirm no double translation when root motion enabled.
