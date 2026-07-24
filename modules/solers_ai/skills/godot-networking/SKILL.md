---
name: godot-networking
description: Godot 4 multiplayer authority, MultiplayerSpawner/Synchronizer, RPC modes, ENet/WebSocket/WebRTC choice, serialization, and hostile-network validation.
---

# Networking and Multiplayer

## When to use
Use for RPC, scene replication, peer authority, ENet/WebSocket/WebRTC, lobbies, reconnect, or trust boundaries. HTTP-only backends are fine without this skill if no peer sync is required.

## Facts
| Piece | Role |
|-------|------|
| `MultiplayerAPI` / `SceneMultiplayer` | Peer graph + RPC dispatch |
| Authority | `set_multiplayer_authority` — who may mutate which node |
| `rpc` / `@rpc` | Modes: `any_peer`/`authority`, `call_remote`/`call_local`, `reliable`/`unreliable`/`unreliable_ordered` |
| `MultiplayerSpawner` | Spawn/despawn replicated scenes |
| `MultiplayerSynchronizer` | Property replication configs |
| Transport | `ENetMultiplayerPeer` (games), `WebSocketMultiplayerPeer`, `WebRTCMultiplayerPeer` |
| Serialization | Prefer typed packets / Variant with validation — never trust client floats as truth |

## Laws
- Server (or explicit authority) owns mutable gameplay facts; clients propose intents.
- Every mutating RPC checks authority / peer id before applying.
- Do not spam `reliable` every frame — use sync properties or unreliable for continuous state.
- Credentials and secrets never live in scenes, RPCs, or logs.

## Recipes
**Authority intent:** client `@rpc("any_peer", "reliable")` sends input intent → authority validates → mutates → sync/RPC result.
**Spawner:** configure spawn path + scenes on `MultiplayerSpawner`; spawn only on authority.
**Join flow:** create peer → `multiplayer.multiplayer_peer = peer` → wait `peer_connected` → request late-join state snapshot once.

## Traps
| Wrong | Correct |
|-------|---------|
| Client sets HP / inventory as truth | Authority applies; client predicts optionally |
| `@rpc` without call mode / transfer mode thought through | Match reliability to event type |
| Testing only in one process | At least host+client (two instances or `--server`/`--client` paths) |
| Frame-by-frame reliable position | Synchronizer / unreliable / delta compress |
| Godot 3 `rpc_id` habits without 4 `@rpc` annotations | Use current `@rpc` + ClassDB |

## Verify
1. `scene.inspect` / `script.validate` authority and RPC annotations.
2. Two peers via `runtime.control` (or documented multi-instance); force latency if available.
3. `runtime.observe` for RPC errors, desync, disconnect/reconnect.
4. Attempt illegal client mutation — must be rejected.
