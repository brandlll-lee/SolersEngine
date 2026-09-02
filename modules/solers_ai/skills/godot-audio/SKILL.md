---
name: godot-audio
description: Godot 4 AudioServer buses, players (2D/3D/stream), attenuation, polyphonic one-shots, AnimationPlayer audio tracks, and loudness verification.
---

# Audio and Mixing

## When to use
Use for music, SFX, voice, 2D/3D positional audio, buses, effects, streaming, or animation-synced playback.

## Facts
| Piece | Role |
|-------|------|
| `AudioServer` buses | Route categories (Music/SFX/UI/Voice); effects live on buses |
| `AudioStreamPlayer` | Non-positional |
| `AudioStreamPlayer2D` / `3D` | Positional; needs listener (`Camera`/`AudioListener*`) |
| Streams | WAV/Ogg/MP3 import settings — loop, BPM, trim |
| Polyphony | `max_polyphony` / dedicated players — avoid spawning unbounded players |
| Sync | Prefer AnimationPlayer audio tracks or explicit signal owners |

## Laws
- Named buses per category; do not dump everything on Master with ad-hoc volume hacks.
- Long music streams; short SFX as samples — match importer expectations.
- One owner triggers a one-shot; do not double-fire from UI + gameplay blindly.
- Positional audio must be tested at listener positions — editor ear ≠ in-game ear.

## Recipes
**Bus layout:** Master ← Music / SFX / UI; optional Compressor/Reverb on SFX bus.
**3D shot:** `AudioStreamPlayer3D` child of emitter; `attenuation_model` + max distance set; `play()` on signal.
**UI click:** shared `AudioStreamPlayer` on Autoload or UI root with `max_polyphony`.

## Traps
| Wrong | Correct |
|-------|---------|
| New player node per footstep forever | Pool / polyphony / restart |
| Looping music as huge WAV uncompressed carelessly | Stream format + loop flags |
| 3D SFX without listener / wrong bus mute | Check listener + bus mute/solo |
| Volume only via `volume_db` spam on Master | Bus send levels + composition |
| Ignoring pause (`process_mode` / tree pause) | Decide which buses continue when paused |

## Verify
1. `scene.inspect` for players, buses, stream paths.
2. `runtime.control` — trigger SFX/music; move listener for 3D falloff.
3. `runtime.observe` for missing streams / decode errors.
4. Confirm pause and scene-change do not leak looping players.
