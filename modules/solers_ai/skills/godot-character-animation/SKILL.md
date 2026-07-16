---
name: godot-character-animation
description: Import, retarget, assemble, drive, and verify Godot 3D character animation with reimport-safe rigs, AnimationPlayer libraries, and AnimationTree graphs.
requires_tools:
  - editor.get_snapshot
  - project.search_files
  - project.read_file
  - resource.get_info
  - class.search
  - class.introspect
  - objects.batch
  - script.validate
  - runtime.control
  - viewport.capture
---

# Godot Character Animation

Use this skill for 3D skeletal import, retargeting, clip organization, blending, state transitions, root motion, or gameplay-driven character animation.

## Workflow

### 1. Define the animation contract

Inspect the current project, then record:

- Character mesh and source skeleton, animation sources, and required clips.
- Required states, transitions, blend parameters, interruption rules, and one-shot actions.
- Whether locomotion is in-place or root-motion-driven.
- Which system owns gameplay velocity, facing, and vertical motion.
- Required loop boundaries, playback speeds, masks, and transition timing.
- Stable runtime scenarios and camera views that expose deformation, blending, drift, and foot contact.

Create one plan around observable outcomes. Do not add a generic animation framework when one character graph is sufficient.

### 2. Inspect import and ownership

- Use `editor.get_snapshot` and project search to locate imported character scenes, source models, animation libraries, wrappers, scripts, and existing graphs.
- Inspect resource metadata before changing import-dependent assets.
- Prefer the existing source pipeline; for new 3D interchange, use glTF unless the project requires another supported format.
- Keep importer-owned scenes reimportable. Put gameplay nodes, overrides, and scripts in inherited scenes or authored wrappers rather than editing generated output.
- Use `class.search` and `class.introspect` before setting unfamiliar `Skeleton3D`, `AnimationPlayer`, `AnimationTree`, or animation resource properties.

Do not infer bone compatibility from matching mesh appearance or filenames.

### 3. Validate skeletons and retargeting

- Confirm skeleton hierarchy, bone orientation, scale, rest pose, skin binding, and the mesh-to-skeleton path.
- For shared humanoid animation, configure a `BoneMap` against the native humanoid profile and resolve required mappings before testing clips.
- Use import rest-pose correction and track filtering deliberately; do not repair retargeting by adding compensating runtime transforms.
- Remove or exclude tracks that would animate unrelated scene nodes.
- Reimport, then verify the resulting skeleton and animation resources before building the playback graph.

If source skeletons are genuinely incompatible, report the incompatibility instead of hiding it behind per-clip offsets.

### 4. Assemble the playback graph

- Let `AnimationPlayer` and its libraries own animation data; let `AnimationTree` own blending and state evaluation.
- Reuse one coherent animation graph per character unless the contract requires independent layers.
- Choose state machines for discrete states, blend spaces for continuous directional or speed variation, and one-shot/additive branches only for independent actions.
- Keep parameter names and transition conditions aligned with gameplay concepts already present in the project.
- Configure loop behavior and transition duration at their authoritative animation or graph resource.
- Add bone filters only where upper/lower-body separation is required and visually verified.

Build nodes and properties through native APIs, then inspect the graph state rather than assuming assignments succeeded.

### 5. Integrate motion once

Choose one locomotion authority:

- For in-place clips, gameplay/physics moves the character and animation parameters follow measured motion.
- For root motion, configure the root-motion track, read the blended motion delta, and apply it through the character's physics movement path.

Never apply authored root displacement and controller displacement simultaneously. Keep vertical gameplay motion under the intended gameplay owner unless the contract explicitly delegates it to animation.

Drive animation parameters from authoritative runtime state such as measured velocity, grounded state, or confirmed actions. Avoid a second shadow state machine in script.

Validate every changed control script before running.

### 6. Verify the animation loop

Run the character in the contract scenarios and check in this order:

1. Bind/rest pose and skin deformation.
2. Availability, duration, loop boundary, and playback of each required clip.
3. State entry, exit, interruption, and one-shot return behavior.
4. Continuous blend response across the requested parameter range.
5. Facing, displacement, collision motion, and root-motion ownership.
6. Foot contact, visible sliding, popping, bone collapse, mesh separation, and accumulated drift.

Capture the same stable runtime views before and after corrections. Use graph state and gameplay motion as objective evidence alongside the image.

## Acceptance gate

Before calling `done`, verify:

- Imported assets remain reimport-safe and no generated scene is the sole owner of authored gameplay changes.
- Skeleton, skin, rest pose, and bone mapping produce valid deformation for every required clip.
- Animation libraries contain the intended clips with correct loop and track ownership.
- One `AnimationTree` graph represents the requested states and blends without duplicated script state.
- Every transition and one-shot path is reachable and returns to the intended state.
- Locomotion has exactly one displacement authority; root motion neither doubles movement nor accumulates unexplained drift.
- Animation parameters follow authoritative gameplay state.
- Changed scripts pass native validation and runtime emits no relevant animation errors.
- Final captures cover deformation, transition quality, and locomotion behavior.
- All plan items are `completed`.

If a source rig, clip, or mapping is missing, pause with the exact asset requirement instead of synthesizing a false completion.
