# Native Agent Harness Refactor

## Goal

Make Solers a thin, reliable operator for Godot's native capabilities. The model should work directly with ClassDB, nodes, resources, editor/runtime controls, and imported assets. The Harness owns only deterministic infrastructure: permission checks, resource-conflict scheduling, lifecycle state, durable failures, and evidence freshness.

## Root Cause

The current session uses one `state_revision` for authored changes, runtime play/stop, and project imports. Completion evidence is therefore invalidated by the verification workflow itself. The executor also waits for one pending tool to finish before admitting the next call, so the import coordinator never receives a complete batch. Godot marks import as idle before post-import callbacks finish, which allows recursive reimport.

## Design

1. Replace `state_revision` with `authored_revision`. Only successful scene or project-file mutations advance it. Runtime transitions advance a separate `runtime_epoch`; they never invalidate editor or runtime evidence for unchanged authored content.
2. Treat tool resource access declarations as the scheduling authority. Admit independent calls from the same model response, keep pending continuations alive, and deliver terminal results in provider order. Conflicting calls remain ordered.
3. Let all staged project imports join one native scan wave. File hashing and copying run off the editor thread; `EditorFileSystem` scanning, signals, and final resource validation remain on the main thread.
4. Keep Godot's `importing` state true through post-import callbacks and progress teardown. A native import cycle has one start and one terminal boundary.
5. Bind failures to a stable tool call and resource scope. A successful terminal result may close errors from that same operation or an explicit scoped retry. Unattributed editor diagnostics remain visible but cannot be cleared by an unrelated screenshot.
6. Return image hashes and descriptive visual statistics with captures. Do not hard-code scene-specific visual thresholds or wrap the model in a visual recommender.
7. Teach Godot capabilities through compiled, on-demand Skills. Preserve direct ClassDB/resource/node tools instead of adding category-specific wrapper APIs.
8. Keep reusable 3D asset selection in the existing direct tools: inspect project files, inspect the local Library, search/acquire Poly Haven models, and generate with Meshy only when the user authorizes generation. Provider manifests define package files and loadable entrypoints; the importer preserves that package instead of guessing dependencies from filenames.

## Invariants

- `play -> capture -> stop` leaves `authored_revision` unchanged.
- Evidence for authored revision N remains current across runtime transitions.
- Independent pending imports are staged before the native import wave starts.
- A successful import means every copied package file exists, every native resource is indexed and import-valid, and every declared entrypoint is loadable.
- No observation or unrelated success clears a failure.
- Static final rendering uses one authoritative GI path; a valid LightmapGI replaces iterative SDFGI for the final result.
- A catalog model acquisition preserves every provider-declared dependency at its relative path and verifies every advertised checksum.
- An explicit provider or generation instruction from the user takes precedence; absent that instruction, a reusable project, Library, or catalog asset is preferred over generation.

## References

- Browser Use, *The Bitter Lesson for Agent Harnesses* and *The Bitter Lesson for Agent Frameworks*: keep the action space direct and the loop small; solve infrastructure reliability outside model intelligence.
- Godot 4 documentation: renderer feature matrix, physical light units, Environment/post-processing, SDFGI convergence, LightmapGI, and import process.
- Local Pi, OpenAI Codex, OpenCode, and Kimi Code research trees: tool definitions colocate spec and execution; resource access controls parallelism; pending/running/terminal states are explicit; results preserve provider order.
