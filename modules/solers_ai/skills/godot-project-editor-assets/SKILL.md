---
name: godot-project-editor-assets
description: Configure Godot projects and use the editor, filesystem, import pipeline, resources, plugins, and assets without breaking reimport ownership.
---

# Project, Editor, and Assets

## When to use
Use for project settings, scene/file organization, imports, resources, editor state, plugins, or third-party assets.

## Inspect first
- Read the current project summary, renderer, main scene, enabled plugins, import state, and relevant dependencies.
- Find existing files with `project.search`; inspect exact resources before editing or replacing them.

## Recommended order
1. Preserve standard Godot paths and importer ownership; author changes in source files, inherited scenes, or wrappers as appropriate.
2. Prefer native Resource and scene APIs. Use raw file writes only for non-serialized files, then wait for filesystem/import completion.
3. Inspect and approve executable plugins before installation; pin identity and version where reproducibility matters.
4. Save the owning resource or scene once the complete change is valid.

## Importing library assets
- Before acquiring a 3D catalog asset, compare its triangle count against the project's import budget; prefer the smallest variant that satisfies the visual goal.
- `asset.import_to_project` enforces a source triangle budget (`max_triangles`, defaulting to the asset's remesh target or the project setting). If it returns `TOPOLOGY_BUDGET_EXCEEDED`, acquire a lower-poly variant or remesh with `asset.run_operation` — do not retry with a raised budget unless the user asked for that fidelity.
- Declare `import_profile: "baked_static"` only when the scene actually bakes lightmaps. UV2 unwrapping is expensive on dense meshes; the default `"runtime"` profile imports geometry as-is.
- Imports run through the editor's frame-budgeted incremental pipeline: the tool stays pending while files import one by one and resumes with per-file results. Do not re-issue the same import while one is pending; the coordinator reuses the pending transaction.

## Validate
Confirm every path loads, dependencies resolve, reimport succeeds, the scene reopens, and no unrelated project setting changed.

## Common failures
Editing generated import output, guessing resource paths, duplicating importer logic, or treating a downloaded file as a loadable Godot resource.
