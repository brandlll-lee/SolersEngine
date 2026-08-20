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
