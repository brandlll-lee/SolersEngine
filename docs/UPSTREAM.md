# Upstream maintenance

`origin` is the existing SolersEngine repository. Never replace or reinitialize it.
`upstream` is `https://github.com/godotengine/godot.git`.

The current engine baseline is the signed Godot `4.7.1-stable` tag at
`a13da4feb8d8aefc283c3763d33a2f170a18d541`.

Before an upgrade, fetch official history and create a protected Solers tag.
Create the upgrade branch from the chosen official tag, then port the Solers
module and the smallest required native seams. Keep AI ownership under
`modules/solers_ai`; do not restore an editor-wide compatibility shell.

After tests pass, join the old Solers tip with a tree-preserving merge commit.
This keeps both official Godot ancestry and all existing Solers commits, so
`main` advances by normal fast-forward and GitHub history, Stars, issues, and
repository identity remain unchanged. Never force-push an engine upgrade.
