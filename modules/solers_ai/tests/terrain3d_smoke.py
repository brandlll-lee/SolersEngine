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

func fail(message: String) -> void:
\tpush_error(message)
\tquit(1)


func _initialize() -> void:
\t_run.call_deferred()


func _run() -> void:
\tvar required := [&"Terrain3D", &"Terrain3DData", &"Terrain3DMaterial", &"Terrain3DAssets"]
\tfor type_name in required:
\t\tif not ClassDB.class_exists(type_name):
\t\t\tfail("Missing Terrain3D class: %s" % type_name)
\t\t\treturn

\tif DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path("res://terrain_data")) != OK:
\t\tfail("Terrain data directory could not be created")
\t\treturn

\tvar scene := Node3D.new()
	print("SOLERS_TERRAIN3D_STAGE scene")
\tscene.name = "TerrainSmoke"
\troot.add_child(scene)
\tvar camera := Camera3D.new()
\tcamera.name = "Camera3D"
\tscene.add_child(camera)
\tcamera.owner = scene
\tcamera.make_current()
\tvar terrain := Terrain3D.new()
\tterrain.name = "Terrain3D"
\tscene.add_child(terrain)
\tterrain.owner = scene
\tterrain.region_size = 64
\tterrain.data_directory = "res://terrain_data"

\tvar height := Image.create_empty(64, 64, false, Image.FORMAT_RF)
	print("SOLERS_TERRAIN3D_STAGE height")
\tfor x in height.get_width():
\t\tfor y in height.get_height():
\t\t\theight.set_pixel(x, y, Color(float(x + y) / 126.0, 0.0, 0.0, 1.0))
\tterrain.data.import_images([height, null, null], Vector3.ZERO, 0.0, 32.0)
	print("SOLERS_TERRAIN3D_STAGE imported")
\tif terrain.data.get_region_count() < 1:
\t\tfail("Terrain3DData did not create a region")
\t\treturn
\tterrain.data.save_directory(terrain.data_directory)

\tvar packed := PackedScene.new()
	print("SOLERS_TERRAIN3D_STAGE save")
\tif packed.pack(scene) != OK or ResourceSaver.save(packed, "res://terrain_smoke.tscn") != OK:
\t\tfail("Terrain scene could not be saved")
\t\treturn
\tif ResourceLoader.load("res://terrain_smoke.tscn", "PackedScene", ResourceLoader.CACHE_MODE_REPLACE) == null:
\t\tfail("Terrain scene could not be reloaded")
\t\treturn
\tvar region_files := DirAccess.get_files_at("res://terrain_data")
\tif region_files.is_empty():
\t\tfail("Terrain3D did not save per-region data")
\t\treturn

\tprint("SOLERS_TERRAIN3D_SMOKE_OK classes=", required, " regions=", terrain.data.get_region_count())
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
        completed = subprocess.run(
            [str(editor), "--headless", "--path", str(project), "--script", "res://check.gd", "--quit-after", "300"],
            check=False,
            capture_output=True,
            text=True,
            timeout=20,
        )
        print(completed.stdout, end="")
        print(completed.stderr, end="", file=sys.stderr)
        output = completed.stdout + completed.stderr
        if completed.returncode != 0 or "SOLERS_TERRAIN3D_SMOKE_OK" not in output or "ERROR:" in output:
            raise SystemExit("Terrain3D smoke test did not reach a clean verified state")


if __name__ == "__main__":
    main()
