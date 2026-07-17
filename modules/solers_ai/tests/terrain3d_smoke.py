import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

ARCHIVE = "Terrain3D_v1.0.2-stable.zip"
PROJECT = """config_version=5

[application]
config/name="Solers Terrain3D Smoke Test"
"""
CHECK = """extends SceneTree

func _initialize() -> void:
\tvar required := [&"Terrain3D", &"Terrain3DData", &"Terrain3DMaterial", &"Terrain3DAssets"]
\tfor type_name in required:
\t\tif not ClassDB.class_exists(type_name):
\t\t\tpush_error("Missing Terrain3D class: %s" % type_name)
\t\t\tquit(1)
\t\t\treturn
\tprint("SOLERS_TERRAIN3D_SMOKE_OK classes=", required)
\tquit(0)
"""


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: terrain3d_smoke.py <editor-binary>")
    editor = Path(sys.argv[1]).resolve()
    archive = editor.parent / "solers_bundles" / ARCHIVE
    if not editor.is_file() or not archive.is_file():
        raise SystemExit(f"missing editor or Terrain3D bundle: {editor}, {archive}")

    with tempfile.TemporaryDirectory(prefix="solers-terrain3d-") as temporary:
        project = Path(temporary)
        with zipfile.ZipFile(archive) as package:
            package.extractall(project, [name for name in package.namelist() if name.startswith("addons/terrain_3d/")])
        (project / "project.godot").write_text(PROJECT, encoding="utf-8")
        (project / "check.gd").write_text(CHECK, encoding="utf-8")
        (project / ".godot").mkdir()
        (project / ".godot" / "extension_list.cfg").write_text(
            "res://addons/terrain_3d/terrain.gdextension\n", encoding="utf-8"
        )
        subprocess.run([str(editor), "--headless", "--path", str(project), "--script", "res://check.gd"], check=True)


if __name__ == "__main__":
    main()
