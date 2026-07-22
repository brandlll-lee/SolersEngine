---
name: godot-project-editor-assets
description: Configure Godot projects and use the editor, filesystem, import pipeline, resources, addons, and assets without breaking reimport ownership.
---

# Project, Editor, and Assets

## When to use
Use for project settings, scene/file organization, imports, resources, editor state, addons, or third-party assets.

## Inspect first
- Read the current project summary, renderer, main scene, enabled addons, import state, and relevant dependencies.
- Find existing files with `project.search`; inspect exact resources before editing or replacing them.

## Recommended order
1. Preserve standard Godot paths and importer ownership; author changes in source files, inherited scenes, or wrappers as appropriate.
2. Prefer native Resource and scene APIs. Use raw file writes only for non-serialized files, then wait for filesystem/import completion.
3. Inspect and approve executable addons before installation; pin identity and version where reproducibility matters.
4. Save the owning resource or scene once the complete change is valid.

## Creating project assets
- Use `asset.generate` or `asset.catalog.acquire` with the final `target_dir`, `max_triangles`, `import_profile`, and optional `map_types`. Provider output is staged internally, then imported directly into `res://`; there is no separate local library or import command.
- Before acquiring a 3D catalog asset, compare its triangle count against the project's import budget; prefer the smallest variant that satisfies the visual goal. If the direct asset job returns `TOPOLOGY_BUDGET_EXCEEDED`, follow its capability-derived remediation. Declarations above `solers/import/max_source_triangles` require an explicit project-setting decision.
- Declare `import_profile: "baked_static"` only when the scene actually bakes lightmaps. UV2 unwrapping is expensive on dense meshes; the default `"runtime"` profile imports geometry as-is.
- Call `job.wait` once after queuing the work. Its terminal result means Godot has verified the imported resources and written project-local `.solers.json` provenance; do not re-issue a pending job.

## Validate
Confirm every path loads, dependencies resolve, reimport succeeds, the scene reopens, and no unrelated project setting changed.

## Common failures
Editing generated import output, guessing resource paths, duplicating importer logic, or treating a downloaded file as a loadable Godot resource.
