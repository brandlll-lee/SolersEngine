# Upstream maintenance

## Repository contract

`origin` is the existing SolersEngine repository. Never replace or reinitialize
it. `upstream` is `https://github.com/godotengine/godot.git`.

The current engine baseline is Godot `4.7.1-stable` at
`a13da4feb8d8aefc283c3763d33a2f170a18d541`. This is a lightweight tag, so the
commit SHA, not a tag signature, is authoritative.

The branch roles are fixed:

- `main` contains Solers-authored product history and is the only release line.
- `upstream/godot-*` preserves exact, immutable Godot history for each baseline.
- `upgrade/godot-*` is temporary integration state and is never a release line.
- `solers-full-lineage-godot-*` tags preserve tested joined histories for audit.

Keep AI ownership under `modules/solers_ai` and keep native changes to the
smallest required Godot seams. The default branch must not contain official
Godot commits as ancestors; otherwise GitHub attributes upstream authors to the
Solers product history.

## Upgrade protocol

An upgrade has exactly three commit inputs and one version label:

```text
OLD_BASE     current recorded Godot commit
SOLERS_HEAD  current origin/main commit
NEW_BASE     exact commit behind the selected new official Godot tag
NEW_TAG      selected official version tag
```

Start from a clean worktree. Fetch `upstream`, resolve the new tag to a commit,
and verify that `OLD_BASE` is its ancestor. Publish `NEW_BASE` unchanged as
`upstream/godot-<version>` before integration.

Preflight the upgrade with Git's ORT merge engine and an explicit base:

```bash
git merge-tree --write-tree \
  --merge-base="$OLD_BASE" "$SOLERS_HEAD" "$NEW_BASE"
```

Exit status `0` means the merge is clean. Exit status `1` means there are real
content, rename, delete, mode, binary, or directory conflicts. Never replace
this step with a whole-tree copy, a plain two-way diff, or a merge-base guessed
from the disconnected default-branch history.

Materialize the same merge through a temporary full-lineage anchor so normal
Git conflict tooling remains available:

```bash
SOLERS_TREE=$(git rev-parse "$SOLERS_HEAD^{tree}")
ANCHOR=$(git commit-tree "$SOLERS_TREE" \
  -p "$SOLERS_HEAD" -p "$OLD_BASE" \
  -m "chore(upstream): anchor Solers on the recorded Godot baseline")
git switch -c "upgrade/godot-$NEW_TAG" "$ANCHOR"
git merge --no-ff "$NEW_BASE"
```

Resolve only the conflicts reported by Git. Do not restore deleted compatibility
layers or duplicate upstream APIs. Run formatting, static checks, the J8 editor
and test builds, all Solers behavior tests, and headless editor initialization.

## Publication contract

The tested integration Tree is projected onto `main` without copying files and
without attaching upstream ancestry to the default branch:

```bash
INTEGRATION=$(git rev-parse HEAD)
RESULT_TREE=$(git rev-parse "$INTEGRATION^{tree}")
PUBLISH=$(git commit-tree "$RESULT_TREE" -p "$SOLERS_HEAD" \
  -m "refactor(engine): migrate Solers to Godot $NEW_TAG" \
  -m "Godot-Upstream-From: $OLD_BASE
Godot-Upstream-To: $NEW_BASE
Integration-Commit: $INTEGRATION
Source-Tree: $RESULT_TREE")
git tag -a "solers-full-lineage-godot-$NEW_TAG" "$INTEGRATION" \
  -m "Preserve the tested Solers and Godot $NEW_TAG lineage"
git switch main
git merge --ff-only "$PUBLISH"
```

Before pushing, require all of the following:

- The integration and publication commits have identical Tree IDs.
- The previous `main` is an ancestor of the new `main`.
- `NEW_BASE` is not an ancestor of `main`.
- The recorded upstream branch and audit tag resolve to the declared commits.
- The worktree is clean and every build and behavior gate passes.

Push `main`, the immutable upstream branch, and the audit tag atomically. Never
merge an `upstream/*` or full-lineage ref into `main`, rewrite upstream authors,
manually copy an engine tree, or force-push an engine upgrade.
