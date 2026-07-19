---
name: destruction-vfx
description: Concrete Godot 4 recipes for damage and destruction - decal cracks, staged pre-fractured meshes, RigidBody3D debris, GPUParticles3D dust, damage shaders, and reading compressed textures correctly.
---

# Destruction and Damage VFX

## When to use
Use for breakable objects, progressive damage (cracks then shatter), impact effects, debris, dust, or any "hit/destroy/explode" gameplay visual.

## Progressive damage pattern (three stages)
1. **Surface damage**: project crack `Decal` nodes at impact points (cheap, unlimited).
2. **Structural damage**: swap the intact mesh for a pre-fractured version whose pieces are still frozen (`RigidBody3D.freeze` = `true`).
3. **Collapse**: unfreeze pieces and apply impulses; despawn or freeze them again after they settle.
Track accumulated damage as a float on the object's script; stage thresholds (for example 0.4 / 1.0) are gameplay data, not magic - expose them as `@export`.

## Stage 1 - crack decals
- `Decal` node: `texture_albedo` = crack texture with alpha, `texture_normal` for depth illusion, `size` ~ `(0.6, 0.2, 0.6)`, `albedo_mix` = `1.0`, small random rotation around the surface normal per hit.
- Orient the decal's -Y along the surface normal (`look_at` from hit point + normal, then rotate).
- `cull_mask` limits which geometry receives it; `distance_fade_enabled` = `true` avoids popping.
- Stack 2-3 decals of growing `size` at the same point to sell progressive cracking before stage 2.

## Stage 2/3 - pre-fractured mesh
- Author fracture pieces ahead of time: either import a fractured model variant, or generate shards in `script.run` (Voronoi-ish splits of the AABB into 8-20 convex chunks is enough for walls/crates).
- Each piece: `RigidBody3D` + `CollisionShape3D` (convex - `create_convex_shape()` or simplified box), `freeze` = `true`, `freeze_mode` = `0` (static).
- On shatter: hide/queue_free the intact mesh, set `freeze` = `false` on all pieces, then per piece `apply_impulse(direction_from_impact * strength, impact_point - piece_center)`; randomize strength 2-6 m/s for a 1 m wall.
- Settle handling: after ~4 s put pieces to sleep (`sleeping = true`) or free the small ones; keep at most the largest chunks for permanence.

## Dust and debris burst (GPUParticles3D)
- `one_shot` = `true`, `explosiveness` = `1.0`, `amount` = `48`-`128`, `lifetime` = `1.2`-`2.5`.
- `ParticleProcessMaterial`: `direction` = surface normal, `spread` = `35`-`60`, `initial_velocity_min/max` = `2/6`, `gravity` = `(0, -9.8, 0)` for chips or `(0, -0.5, 0)` for dust, `scale_min/max` = `0.05/0.25`, `damping_min/max` = `1/3`.
- Dust material: `StandardMaterial3D` with `billboard_mode` = `3` (particle), soft smoke albedo texture, `transparency` = alpha, color ramp fading alpha to 0 over lifetime (`color_ramp` on the process material).
- Reuse one particles node per effect type; call `restart()` at each impact position instead of instantiating new nodes.

## Damage shader pattern (cheap, no mesh swap)
Fragment shader on the damaged material: sample a crack mask in a second UV channel or triplanar, then
`ALBEDO = mix(base_albedo, crack_albedo, crack_mask * damage);` and darken `AO` by the mask. Drive `damage` (0-1 uniform) from the script via `set_shader_parameter("damage", value)`. This gives continuous damage feedback between decal hits and shatter.

## Reading pixels from imported textures (the get_pixel trap)
Imported textures are GPU-compressed; `Image.get_pixel()` fails on them and spams errors every call.
Correct pattern, once, outside any loop:

```gdscript
var img: Image = texture.get_image()
if img.is_compressed():
    img.decompress()
# now img.get_pixel(x, y) is valid
```

Never call `get_pixel` on a compressed image inside `_process` - decompress once and cache the `Image`.

## Physics hygiene
- Debris pieces belong on their own collision layer that does not collide with the player camera or with each other beyond the first second (`collision_mask` downgrade after impact) to avoid jitter explosions.
- Contact monitoring only where needed: `contact_monitor` = `true` and `max_contacts_reported` = `2` on the projectile/fist body, not on every shard.

## Verify
Play the scene (`runtime.control`), trigger the impact, capture with `viewport.capture target=runtime`, and check runtime errors (`runtime.observe`): zero physics warnings, stable framerate during the burst, shards settling instead of vibrating.
