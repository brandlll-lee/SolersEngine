---
name: godot-project-editor-assets
description: Godot project settings, res:// ownership, .import/.uid sidecar hygiene, addon Contract via addon.inspect, and asset.generate/acquire without vendor skill dumps.
---

# Project, Editor, and Assets

## When to use
Use for project settings, folder layout, imports, resources, editor state, addons, or generated/catalog assets.

## Facts
| Piece | Role |
|-------|------|
| `res://` | Project resources; exported games treat as packed/read-only |
| `user://` | Writable saves/cache |
| `.import` | Importer sidecar — engine-owned; change via importer settings / reimport |
| `.uid` | Stable resource identity files — keep with the resource; do not invent UIDs |
| `plugin.cfg` / addons | Enable only after inspect; executable code is a trust boundary |
| `asset.generate` / `asset.catalog.acquire` | Provider → stage → import into `target_dir`; options come from **plugin Contract** (`asset.capabilities`), not from this skill |
| Provenance | Project-local `.solers.json` after verified import |

## Laws
- Author in source scenes/scripts/resources; never treat `.import` caches as editable art.
- Addon APIs come from `addon.inspect` / ClassDB — never invent `terrain.*` / vendor tool families.
- Provider quality knobs live in the plugin's generation_options Contract — read capabilities before setting `provider_options`.
- Triangle budgets: honor `solers/import/max_source_triangles` and remediation from job errors.

## Recipes
**Import four-pack (mental model):** source file + `.import` + `.uid` + (optional) imported companion — move/rename with Godot/`project` tools so sidecars stay coherent.
**Generate/acquire:** set final `target_dir`, `import_profile` (`runtime` default; `baked_static` only if lightmaps), optional `max_triangles` / `map_types` → call generate/acquire → let the job reach terminal import (do not busy-poll status).
**Addon:** `addon.inspect` → read Contract/entry classes → install/enable only if pinned and trusted → restart if the Contract says so.
**CSG:** whitebox from ClassDB → bake native mesh/collision artifacts in `object.transaction` after topology is verified.

## Traps
| Wrong | Correct |
|-------|---------|
| Hand-editing `.import` | Importer UI / reimport |
| Hardcoding vendor model tier names in generic advice | `asset.capabilities` → `provider_options` schema |
| Guessing addon method names | `addon.inspect` + `engine.describe` |
| `import_profile: baked_static` “just in case” | Only when UV2/lightmap bake is real |
| Treating download bytes as a Godot resource without import | Wait for verified import paths |

## Verify
1. `project.search` / `object.query target=resource` paths resolve; sidecars present after moves.
2. After generate/acquire: imported `res://` paths load; provenance exists.
3. Reopen scene/project; `runtime.observe` for import errors.
4. Addon path: inspect Contract, then validate ClassDB types exist post-enable.
