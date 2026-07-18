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

## Validate
Confirm every path loads, dependencies resolve, reimport succeeds, the scene reopens, and no unrelated project setting changed.

## Common failures
Editing generated import output, guessing resource paths, duplicating importer logic, or treating a downloaded file as a loadable Godot resource.
