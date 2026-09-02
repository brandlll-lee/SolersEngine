---
name: godot-animation-rigging
description: Godot 4 animation resources, playback, blending, skeletons, retargeting, and root motion.
---

# Godot Animation and Rigging

## Scope
Use when working with animation clips, playback, transitions, blending, skeletons, retargeting, IK, or root motion.

## Native model
- Animation stores typed tracks whose keys target properties, transforms, methods, audio, or nested animations.
- AnimationPlayer owns AnimationLibrary resources and resolves track NodePaths relative to its root node.
- AnimationTree does not store animations. It evaluates an AnimationNode graph using animations supplied by an
  AnimationPlayer and exposes graph parameters at runtime.
- Skeleton3D owns a bone hierarchy, rest transforms, and poses. Skin binds mesh vertices to skeleton bones.
- BoneMap and SkeletonProfile define retargeting correspondence. Imported animation and skeleton settings belong to the
  source asset's import configuration.
- Root motion removes selected root-bone motion from the visible pose and exposes the blended delta through
  AnimationTree for application by gameplay code.

## Compatibility and prerequisites
- Track paths, bone names, animation-library names, and imported resource identities must match the instantiated scene.
- Reimport can replace importer-owned animation and skeleton data; project-owned overrides belong outside generated data.

## Authoritative state
Animation resources, resolved tracks, AnimationPlayer playback, AnimationTree parameters, Skeleton3D rest and pose,
import settings, runtime transforms, and diagnostics are authoritative.

## Official references
- https://docs.godotengine.org/en/latest/tutorials/animation/index.html
- https://docs.godotengine.org/en/latest/tutorials/animation/animation_tree.html
- https://docs.godotengine.org/en/latest/tutorials/animation/retargeting_3d_skeletons.html
- https://docs.godotengine.org/en/latest/classes/class_animationplayer.html
- https://docs.godotengine.org/en/latest/classes/class_skeleton3d.html
