---
name: godot-animation-rigging
description: Import, retarget, rig, blend, and verify Godot 2D/3D animation with AnimationPlayer, AnimationTree, skeletons, IK, and root motion.
---

# Animation, Rigging, and State Machines

## When to use
Use for keyframes, skeletal animation, retargeting, blend spaces, state machines, IK, one-shots, or root motion.

## Inspect first
- Identify importer ownership, skeleton/rest pose, clips, libraries, graph parameters, gameplay motion owner, and required transitions.
- Keep imported scenes reimport-safe by authoring wrappers or inherited scenes.

## Recommended order
1. Verify hierarchy, scale, rest pose, skin, bone mapping, tracks, and clips before building a graph.
2. Let AnimationPlayer own animation data and AnimationTree own blending/state evaluation.
3. Choose state machines, blend spaces, filters, or IK only where the behavior contract needs them.
4. Choose exactly one locomotion authority: gameplay motion or root motion.

## Validate
Run each clip and transition; inspect deformation, loop boundaries, interruption, foot contact, drift, graph state, and runtime errors.

## Common failures
Editing importer output, compensating bad retargeting at runtime, duplicate script/AnimationTree state, doubled root motion, and unverified bone maps.
