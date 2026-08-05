# Upstream maintenance

`origin` is the existing SolersEngine repository. Never replace or reinitialize it.
`upstream` is `https://github.com/godotengine/godot.git`.

The current engine baseline is the signed Godot `4.7.1-stable` tag at
`a13da4feb8d8aefc283c3763d33a2f170a18d541`.

`main` contains the complete Solers-authored history and a tested snapshot of
the selected Godot baseline. Official Godot commits remain unchanged on the
`upstream/godot-*` branches. The `solers-full-lineage-godot-*` tags and matching
`archive/full-lineage-godot-*` branches preserve joined histories for audit.

Before an upgrade, fetch the signed official tag, publish its exact commit on a
new `upstream/godot-*` branch, and protect the current Solers tip with a tag.
Create the upgrade branch from `main`, then port the upstream delta, the Solers
module, and only the required native seams. Keep AI ownership under
`modules/solers_ai`; do not restore an editor-wide compatibility shell.

After tests pass, record the upstream tag, commit, and source tree in the
migration commit, then fast-forward `main`. Do not merge official ancestry into
the default branch, rewrite upstream authors, or force-push an engine upgrade.
