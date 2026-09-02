---
name: godot-project-editor-assets
description: Godot 4 project settings, paths, resources, scenes, imports, UIDs, editor filesystem, and addons.
---

# Godot Project, Editor, and Assets

## Scope
Use when working with project structure, settings, resource paths, scenes, imports, UIDs, editor state, addons, or exports.

## Native model
- `res://` identifies files in the project resource root. `user://` identifies the writable per-user data directory.
- ProjectSettings loads project-wide configuration from `project.godot`; InputMap and enabled plugins are part of that
  configuration.
- ResourceLoader and ResourceSaver use registered format loaders and savers. A saved scene is a PackedScene resource that
  instantiates a node hierarchy.
- Scene ownership determines which nodes are serialized into a PackedScene. Referenced resources may be built in,
  external, shared by several owners, or local to a scene.
- Imported source assets are transformed by a ResourceImporter. Import settings and source identity determine generated
  data under `.godot/imported`; generated import output is not the editable source asset.
- Resource UIDs provide stable resource identity independent of path where supported. EditorFileSystem exposes the
  editor's current scan, type, and import view of project files.
- An editor addon is project code loaded by EditorPlugin and may register docks, importers, inspectors, and other editor
  behavior.

## Compatibility and prerequisites
- Resource formats, import options, class names, UIDs, and addon APIs depend on the current engine and addon versions.
- Reimport and filesystem scans are asynchronous editor operations; export includes resources according to its preset.

## Authoritative state
ProjectSettings, native resource type and UID, ResourceLoader results, PackedScene state, importer configuration,
EditorFileSystem scan/import state, editor diagnostics, and exported resource contents are authoritative.

## Official references
- https://docs.godotengine.org/en/latest/tutorials/io/data_paths.html
- https://docs.godotengine.org/en/latest/tutorials/scripting/resources.html
- https://docs.godotengine.org/en/latest/tutorials/assets_pipeline/import_process.html
- https://docs.godotengine.org/en/latest/tutorials/assets_pipeline/importing_3d_scenes/index.html
- https://docs.godotengine.org/en/latest/classes/class_resourceuid.html
- https://docs.godotengine.org/en/latest/classes/class_editorfilesystem.html
- https://docs.godotengine.org/en/latest/tutorials/plugins/editor/making_plugins.html
