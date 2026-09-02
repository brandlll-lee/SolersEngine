---
name: godot-physics-navigation
description: Godot 4 physics bodies, collision, space queries, navigation maps, pathfinding, and avoidance.
---

# Godot Physics and Navigation

## Scope
Use when working with 2D or 3D bodies, collision, physics queries, character motion, navigation, pathfinding, or avoidance.

## Native model
- Physics advances at fixed ticks. PhysicsBody, Area, CollisionObject, CollisionShape, and PhysicsServer state define the
  simulated world independently of visual geometry.
- Collision layers describe membership; collision masks select layers considered by a body, area, or query.
- CharacterBody movement is user-controlled through motion methods such as `move_and_slide()`. RigidBody motion is owned
  by the physics simulation and is influenced through forces, impulses, or integration callbacks.
- World direct-space state exposes ray, point, shape, and motion queries against the synchronized physics space.
- NavigationServer owns maps identified by RID. Maps contain regions, links, and avoidance agents; queued changes become
  effective when the navigation server synchronizes.
- NavigationAgent computes path-following information but does not move its parent. Avoidance computes a safe velocity
  and is separate from pathfinding and physics collision.

## Compatibility and prerequisites
- Physics and navigation have separate 2D and 3D servers, resources, layers, and world membership.
- Navigation results depend on map assignment, synchronized region data, compatible navigation layers, and baked or
  generated navigation geometry.

## Authoritative state
PhysicsServer and NavigationServer state, body and shape transforms, layers and masks, direct query results, contacts,
map RIDs and synchronization, path data, safe velocity, and runtime physics frames are authoritative.

## Official references
- https://docs.godotengine.org/en/latest/tutorials/physics/physics_introduction.html
- https://docs.godotengine.org/en/latest/tutorials/physics/ray-casting.html
- https://docs.godotengine.org/en/latest/classes/class_characterbody3d.html
- https://docs.godotengine.org/en/latest/classes/class_rigidbody3d.html
- https://docs.godotengine.org/en/latest/tutorials/navigation/navigation_introduction_3d.html
- https://docs.godotengine.org/en/latest/tutorials/navigation/navigation_using_navigationmaps.html
- https://docs.godotengine.org/en/latest/tutorials/navigation/navigation_using_navigationagents.html
