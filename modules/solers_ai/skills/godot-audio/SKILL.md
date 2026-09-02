---
name: godot-audio
description: Godot 4 audio streams, players, buses, effects, listeners, positional audio, and imports.
---

# Godot Audio

## Scope
Use when working with audio streams, playback, buses, effects, recording, positional sound, listeners, or audio imports.

## Native model
- AudioStream is audio data or a generator. AudioStreamPlayer, AudioStreamPlayer2D, and AudioStreamPlayer3D create
  playback and route it to an AudioServer bus.
- Audio buses form an ordered routing graph with volume, mute, solo, send targets, and effect chains. Bus layouts are
  project resources loaded by AudioServer.
- AudioStreamPlayer2D and AudioStreamPlayer3D derive panning and attenuation from the active listener and world state.
  AudioListener nodes can override the listener supplied by the current camera.
- AudioStreamPlayer3D additionally defines distance model, emission direction, Doppler tracking, area mask, and bus
  redirection through Area3D.
- Import options determine compression, looping, sample rate, and storage behavior of imported audio resources.

## Compatibility and prerequisites
- Stream format support, playback type, channel layout, and platform audio backend vary by source and target platform.
- A bus name that is unavailable when assigned falls back to `Master`; the live AudioServer bus layout is authoritative.

## Authoritative state
Imported AudioStream resources, AudioServer buses and effects, player playback state, active listener, world transforms,
runtime diagnostics, and audible output are authoritative.

## Official references
- https://docs.godotengine.org/en/latest/tutorials/audio/audio_streams.html
- https://docs.godotengine.org/en/latest/tutorials/audio/audio_buses.html
- https://docs.godotengine.org/en/latest/tutorials/assets_pipeline/importing_audio_samples.html
- https://docs.godotengine.org/en/latest/classes/class_audiostreamplayer3d.html
- https://docs.godotengine.org/en/latest/classes/class_audiolistener3d.html
