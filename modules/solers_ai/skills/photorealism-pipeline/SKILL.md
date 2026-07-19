---
name: photorealism-pipeline
description: Concrete Godot 4 recipe for photorealistic rendering - ACES tonemapping, physical light units, SDFGI/SSAO/SSIL/SSR settings, sun/sky/fog values, camera attributes, and a verification loop.
---

# Photorealism Pipeline

## When to use
Use when the goal is "photorealistic", "cinematic", "like a photo/film", or when a flat, game-like render must be pushed toward reference quality.

## The five pillars (order matters)
1. Correct exposure model (physical light units + tonemap) - without it every other tweak lies.
2. Real-world light intensities and one sun.
3. One global illumination path with ambient occlusion support.
4. Atmosphere (fog, haze, volumetrics) for depth cues.
5. Assets with real PBR maps at correct physical scale - lighting cannot rescue untextured primitives.

## Project settings (project.edit settings)
- `rendering/lights_and_shadows/physical_light_units` = `true` - enables lux/lumen intensities on all lights and exposure on cameras.
- `rendering/lights_and_shadows/directional_shadow/size` = `4096`, `soft_shadow_filter_quality` = `3` or higher.
- `rendering/anti_aliasing/quality/msaa_3d` = `2` (4x) plus `screen_space_aa` = `1` (FXAA) or use TAA (`use_taa` = `true`) when there is animated foliage.

## WorldEnvironment recipe (resource.edit an Environment)
- `tonemap_mode` = `3` (ACES Fitted), `tonemap_white` = `6.0`.
- `background_mode` = `2` (Sky) with a `PhysicalSkyMaterial` sky, or an HDRI panorama from the asset catalog for reference-grade ambient.
- SDFGI (exterior/large scenes): `sdfgi_enabled` = `true`, `sdfgi_use_occlusion` = `true`, `sdfgi_cascades` = `4`-`6`, `sdfgi_min_cell_size` ~ `0.15` for human-scale scenes. Interiors that never move can use LightmapGI (bake) instead - never both as the final path.
- SSAO: `ssao_enabled` = `true`, `ssao_radius` = `1.0`, `ssao_intensity` = `2.0`, `ssao_power` = `1.5`.
- SSIL: `ssil_enabled` = `true`, `ssil_radius` = `5.0`, `ssil_intensity` = `1.0` - fills the meter-scale bounce SDFGI misses.
- SSR (wet ground, water, glossy floors): `ssr_enabled` = `true`, `ssr_max_steps` = `64`.
- Glow, subtle: `glow_enabled` = `true`, `glow_intensity` = `0.4`, `glow_bloom` = `0.05`, `glow_hdr_threshold` = `1.2`. Glow above these values reads as "video game" immediately.
- Depth fog for distance haze: `fog_enabled` = `true`, `fog_light_energy` = `1.0`, `fog_density` ~ `0.001`-`0.01`, `fog_aerial_perspective` = `0.5`, `fog_sky_affect` ~ `0.25`.
- Volumetric fog only when light shafts are wanted: `volumetric_fog_enabled` = `true`, `volumetric_fog_density` ~ `0.01`-`0.05`, `volumetric_fog_length` = `96`.

## Lights (physical units active)
- Sun: one `DirectionalLight3D`, `light_intensity_lux` = `100000` (noon) / `35000` (overcast) / `400` (sunset), `light_temperature` = `5500`-`6500` K, `light_angular_distance` = `0.53` (real sun; drives correct soft-shadow penumbra), `shadow_enabled` = `true`, `directional_shadow_mode` = `2` (4 splits), `directional_shadow_blend_splits` = `true`, `directional_shadow_max_distance` sized to the visible range (150-300 m).
- Interior practicals: `OmniLight3D`/`SpotLight3D` with `light_intensity_lumens` = `800`-`3000` (bulb range), warm `light_temperature` = `2700`-`3200` K.
- Never brighten a scene with `ambient_light_energy` or emissive fill - raise the real sources or the camera exposure.

## Camera (Camera3D + CameraAttributesPhysical)
- Attach `CameraAttributesPhysical`: daylight exterior `exposure_aperture` = `16`, `exposure_shutter_speed` = `1/100`, `exposure_sensitivity` = `100` ISO; interiors ~ aperture `2.8`, ISO `400`-`800`.
- Depth of field: `frustum_focal_length` = `35`-`85` mm and focus distance on the subject; enable DOF blur only for close-up composition shots.
- Eye height 1.6-1.8 m for human-scale believability; avoid perfectly horizontal or axis-aligned camera angles.

## Materials and assets
- Every visible surface needs albedo + normal + roughness from one texture family; assign `detail` normal or a second UV-scaled sample for close-range breakup on large surfaces.
- Texture scale must match physical size: a 2 m brick texture on a 2 m wall (uv1_scale from measured bounds, not eyeballed).
- Roughness variation sells realism more than resolution: prefer materials whose roughness map has visible contrast.
- Vegetation/cloth need `cull_disabled` + `alpha_scissor` and subsurface scattering (`subsurf_scatter_enabled`, strength ~ `0.15`).

## Verify (the loop that actually makes it photoreal)
1. `viewport.capture` from the final composition camera - not top-down.
2. Check: is the darkest shadow still readable? Is anything clipped white that is not the sun/sky? Do shadows have soft edges growing with distance?
3. Fix at the source (light intensity, GI, material), re-capture, compare. Two or three loops minimum; photorealism is convergence, not a preset.
