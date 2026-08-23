# Solers Asset Connectors

## Lifecycle

Godot `plugin.cfg` owns discovery; register and unregister the same instance:
```gdscript
@tool
extends EditorPlugin
var connector := preload("connector.gd").new()
func _enter_tree() -> void:
    SolersPlugin.register_plugin(connector)
func _exit_tree() -> void:
    SolersPlugin.unregister_plugin(connector)
```

## Contract

Profiles and schemas declare facts; prepare methods validate while AssetService owns queueing and import.
Store BYOK under `solers/plugins/<profile.id>/`; registry revisions drive consumers.

## Built-in Tripo connector

The Tripo connector uses the versioned `https://openapi.tripo3d.ai/v3` contract.
Its profile publishes H3.1 (`v3.1-20260211`) and P1 (`P1-20260311`) as generation presets, so Studio discovers them without provider-specific UI branches. Generated files are downloaded into `user://solers_jobs`; provider URLs are never treated as persistent assets.
