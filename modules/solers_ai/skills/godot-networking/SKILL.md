---
name: godot-networking
description: Godot 4 MultiplayerAPI, peers, authority, RPC, replication, transports, and dedicated servers.
---

# Godot Networking

## Scope
Use when working with peers, authority, RPC, replication, network transports, lobbies, or dedicated servers.

## Native model
- SceneTree exposes a MultiplayerAPI. A MultiplayerPeer supplies the transport and connection state used by that API.
- Every peer has an ID; the server has ID 1 by default. Multiplayer authority is assigned per Node and defaults to the
  server unless changed.
- `@rpc` defines caller policy, local or remote execution, transfer mode, and channel. RPC compatibility includes the
  declaration, function signature, and the NodePath that owns it on each peer.
- Reliable, unreliable, and unreliable-ordered transfer modes provide different delivery and ordering guarantees.
- MultiplayerSpawner replicates configured scene or custom spawns from authority. MultiplayerSynchronizer replicates
  configured properties using SceneReplicationConfig.
- High-level multiplayer does not make application logic secure. Server authority and validation remain application
  responsibilities.

## Compatibility and prerequisites
- Transport, browser, mobile, NAT, firewall, TLS, and dedicated-server support depend on deployment platform and peer type.
- Replication requires compatible scenes, scripts, node paths, authority assignment, and configuration on participating
  peers.

## Authoritative state
MultiplayerAPI and peer connection state, peer IDs, node authority, RPC configuration, replication configuration,
transport events, runtime errors, and the state observed by each real peer are authoritative.

## Official references
- https://docs.godotengine.org/en/latest/tutorials/networking/high_level_multiplayer.html
- https://docs.godotengine.org/en/latest/classes/class_multiplayerapi.html
- https://docs.godotengine.org/en/latest/classes/class_multiplayerpeer.html
- https://docs.godotengine.org/en/latest/classes/class_multiplayerspawner.html
- https://docs.godotengine.org/en/latest/classes/class_multiplayersynchronizer.html
- https://docs.godotengine.org/en/latest/tutorials/export/exporting_for_dedicated_servers.html
