# Agent Evidence, Instancing, and Placement Design

## Goal

Remove the failure classes exposed by A004 without scene-specific rules: transcript self-locking, unreferenceable tool evidence, imported models that cannot enter the scene, incomplete placement validation, recursive native imports, and successful tool results that hide attributable Godot errors.

## Existing boundaries to reuse

- `SolersAgentSession` already owns live tool results, the plan, stable failure roots, and completion state.
- `solers_transcript_write()` is the one transcript append entrypoint.
- `SolersAssetService` already owns one project import coordinator.
- `SolersReflectionService` already owns native scene mutation, UndoRedo, spatial measurement, and structural validation.
- Godot `PackedScene`, `ResourceLoader`, `EditorUndoRedoManager`, and `EditorFileSystem` remain the execution authorities.

No second ledger, job system, asset workflow, or scene-specific validator is introduced.

## Design

### Runtime state and transcript

The session keeps successful canonical tool calls in its existing live state and restores that index once from the transcript in `set_session()`, before the editor log audit is attached. Completion reads the live index and never rescans the transcript.

The transcript remains append-only audit data. Restore skips blank records and parses malformed records as recoverable input errors without emitting another audited editor error. `solers_transcript_write()` serializes append access; persistence errors do not recursively write to the same transcript.

### Operation and failure identity

The usable provider tool-call ID is the canonical operation ID. If a Chat-compatible gateway leaks a Responses `fc_*` item ID instead, the protocol seam deterministically normalizes that provider-unique value once; it never generates an ID from array position. A call with no identity is rejected. Every terminal tool result exposes the resulting exact ID in its JSON payload so the model can cite it, and the transcript and completion index use the same value.

A failure ID has a separate meaning: it identifies one logical unresolved failure. Retrying it appends an attempt to the same root. Only a successful operation with the same tool and resource scope resolves that root.

### Imported scene placement

Add one direct native tool, `scene.instantiate`, to `SolersReflectionService`. It accepts a loadable `PackedScene` resource path, parent node, optional name, and native initial properties. It instantiates with Godot edit state, applies properties before insertion, records the scene source path, and commits the insertion through the existing UndoRedo pattern. Its result returns the exact resource path and created node path.

An asset is therefore only "used in the scene" after a successful import has produced a loadable entrypoint and `scene.instantiate` has returned a live node path. Downloaded or imported files alone are not placement evidence.

### Complete physical placement contract

Extend `scene.validate_structure` with optional placement roots and support relations. The engine derives the complete logical member set from those roots:

- an instantiated `PackedScene` root is one authored asset member;
- native geometry is validated as individual logical members;
- generated bake artifacts remain excluded.

Every logical placement member must have exactly one declared `supported_by` relation. The relation names the support node, support axis and direction, and an explicit project-unit tolerance. Validation uses transformed bounds only as a broad phase, then confirms contact against transformed Mesh/CSG triangles. Missing members, self-support, support inside the same logical member, duplicate relations, gaps, AABB-only false contacts, and unsupported native geometry fail the contract.

Structural contacts keep their existing exact graph rules. Placement tolerance is explicit data supplied for the project scale, not a scene name, asset name, or built-in magic threshold.

### Native import lifetime

`EditorFileSystem::importing` remains true through post-import progress teardown and `filesystem_changed` / `resources_reimported` delivery. Listeners therefore cannot start a nested `reimport_files()` while the original native cycle still owns the `reimport` progress task. All Solers transactions continue to join the existing coordinator wave.

### Attributable Godot diagnostics

Main-thread diagnostics are attributed to a tool only while its native handler or poll is actively on the stack; worker diagnostics continue to use the registered worker operation. Waiting time in a pending operation is not an attribution window.

If an attributable Godot error occurs, an otherwise successful handler result becomes a structured tool failure before delivery. Historical, unrelated, warning-only, and unscoped editor messages remain visible diagnostics but do not fail the operation.

## Required checks

- Completion does not open or parse the transcript during a running turn.
- Blank or malformed restore records do not emit audited JSON errors or create temporary transcript files.
- The operation ID exposed to the model is the ID accepted by plan evidence.
- A retry failure adds an attempt to one stable root; unrelated success cannot clear it.
- A synthetic PackedScene is instantiated under the requested parent with UndoRedo ownership and source path preserved.
- A new, unnamed native placement member without support fails; a supported member passes; a positive gap and self-support fail.
- `resources_reimported` observers see `EditorFileSystem::is_importing() == true`, and a second import joins or waits instead of creating another `reimport` task.
- A scoped Godot error changes that operation's terminal result to failure; an error outside active execution does not.
- Existing material import, Lightmap, capture, runtime, and normal scene-authoring contracts remain green.

## Non-goals

No popup suppression, timeout increase, provider or extension blacklist, A004 coordinate, asset-name rule, automatic aesthetic scoring, exact arbitrary mesh-to-mesh collision solver, or new background-job framework.
