---
name: godot-networking
description: Design and verify Godot multiplayer authority, RPCs, replication, transport, serialization, reconnect, and hostile-network boundaries.
---

# Networking and Multiplayer

## When to use
Use for RPC, synchronized scenes, authority, ENet, WebSocket, WebRTC, HTTP, sockets, lobbies, or multiplayer state.

## Inspect first
- Identify topology, authority, transports, peers, replicated state, trust boundaries, serialization, and existing network tests.
- Separate local gameplay state from authoritative network state before editing.

## Recommended order
1. Define who may mutate each state and which events are reliable, ordered, or transient.
2. Reuse Godot's high-level multiplayer/scene replication unless custom transport semantics are required.
3. Validate incoming data and keep credentials/secrets out of scenes and logs.
4. Handle join, leave, timeout, reconnect, late state, and ownership transfer explicitly.

## Validate
Run at least host/client processes with latency/loss scenarios; verify authority, convergence, duplicate handling, disconnect recovery, and security errors.

## Common failures
Client-authoritative trust, RPCs without ownership checks, frame-by-frame reliable traffic, divergent local state, and tests performed in one peer only.
