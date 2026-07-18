---
name: godot-audio
description: Build and verify Godot music, sound effects, positional audio, buses, effects, streaming, and animation-linked playback.
---

# Audio and Mixing

## When to use
Use for music, effects, voice, 2D/3D sound, buses, DSP effects, attenuation, reverb, recording, or playback timing.

## Inspect first
- Locate streams, import settings, players, bus layout, ownership, trigger signals, and target loudness/space requirements.
- Check whether audio is positional, streamed, looped, synchronized, or platform constrained.

## Recommended order
1. Route each sound category through named buses and reusable bus effects.
2. Use the correct player type and explicit attenuation/spatial settings.
3. Trigger discrete sounds from authoritative signals; synchronize animation/audio through native tracks where appropriate.
4. Stream long assets and keep duplicate decodes/resources out of memory.

## Validate
Run representative listeners and devices; check routing, clipping, loops, spatial falloff, pause/state behavior, latency, and missing-resource errors.

## Common failures
Everything on Master, duplicated players, positional audio without listener tests, abrupt loops, and loading long streams as short samples.
