# Asset Import and Editor Shutdown Design

## Goal

Prevent catalog material packages from importing unrelated authoring formats, prevent Agent tools from opening interactive importer configuration, and make editor quit/restart safe while Solers work is active.

## Authoritative data flow

Catalog manifests keep three existing concepts with distinct meanings:

- `files`: every downloaded Library file.
- `import_files`: the provider-resolved subset that belongs in `res://`.
- `entrypoints`: resources consumers load after import.

`asset.import_to_project` validates that declared import files and entrypoints belong to `files`, copies only `import_files`, and verifies only `entrypoints`. Legacy manifests without `import_files` derive a dependency closure from native resource entrypoints; an empty entrypoint array never means “copy the whole package”.

Before copying, Solers asks `EditorFileSystem` whether the selected payload would activate an import-format support query. If so, the tool returns a structured non-interactive error. The native scan is never allowed to open configuration or restart dialogs on behalf of an Agent tool.

## Shutdown ownership

`SolersAgentRuntime::shutdown()` is the single idempotent shutdown path. `EditorNode` calls it during `NOTIFICATION_EXIT_TREE`, while `EditorLog` is still alive. Shutdown stops polling/RPC admission, detaches the session log audit through a stored `ObjectID`, aborts session work, and destroys the asset service so all asset workers are cancelled and joined before editor children disappear. Destructors call the same method as a fallback and perform only owned-memory cleanup.

## Preview decoding

Preview bytes are decoded once according to their file signature. Decoder failures therefore represent a genuinely malformed provider response instead of deliberate failed probes that pollute the Godot error log.

## Required checks

- A package containing one declared material, its texture dependency, and unrelated valid resources imports only the material closure.
- A synthetic active format-support query rejects an Agent import before any project file is copied or native scan begins.
- Empty legacy entrypoints use native dependency discovery and never expand to all package files.
- Runtime shutdown is idempotent and is called before `EditorLog::deinit()`.
- JPEG and PNG previews select the matching decoder without first invoking another decoder.
- Normal quit, project restart, and quit with active catalog downloads exit without an access violation.

## Non-goals

No provider-specific extension blacklist, no automatic Blender installation, no retry timeout increase, and no new asset framework. The implementation reuses the existing manifest, import coordinator, Godot format-support queries, and runtime ownership.
