import hashlib
import os
import shutil
import sys
import urllib.request
from pathlib import Path

VERSION = "1.0.2-stable"
ARCHIVE_NAME = f"Terrain3D_v{VERSION}.zip"
URL = f"https://github.com/TokisanGames/Terrain3D/releases/download/v{VERSION}/{ARCHIVE_NAME}"
SHA256 = "a071850250ec5e596aa54da61c01d75768774eb379ee997584d426a45f4884a2"


def _hash(path):
    digest = hashlib.sha256()
    with open(path, "rb") as archive:
        for chunk in iter(lambda: archive.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _cache_path():
    if sys.platform == "win32" and os.environ.get("LOCALAPPDATA"):
        root = Path(os.environ["LOCALAPPDATA"]) / "Solers" / "cache"
    elif sys.platform == "darwin":
        root = Path.home() / "Library" / "Caches" / "Solers"
    else:
        root = Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache")) / "solers"
    return root / "plugins" / ARCHIVE_NAME


def bundle_terrain3d(target, source, env):
    destination = Path(str(target[0]))
    override = os.environ.get("SOLERS_TERRAIN3D_ARCHIVE")
    archive = Path(override) if override else _cache_path()
    if override and not archive.is_file():
        raise RuntimeError(f"SOLERS_TERRAIN3D_ARCHIVE does not exist: {archive}")

    if not archive.is_file() or _hash(archive) != SHA256:
        if override:
            raise RuntimeError(f"Terrain3D archive SHA-256 mismatch: {archive}")
        archive.parent.mkdir(parents=True, exist_ok=True)
        temporary = archive.with_suffix(".download")
        try:
            request = urllib.request.Request(URL, headers={"User-Agent": "SolersEngine-build"})
            with urllib.request.urlopen(request, timeout=180) as response, open(temporary, "wb") as output:
                shutil.copyfileobj(response, output)
            if _hash(temporary) != SHA256:
                raise RuntimeError("Downloaded Terrain3D archive failed SHA-256 verification")
            os.replace(temporary, archive)
        except Exception as error:
            temporary.unlink(missing_ok=True)
            raise RuntimeError(
                "Terrain3D bundle is unavailable. Restore network access, populate the Solers cache, "
                "or set SOLERS_TERRAIN3D_ARCHIVE to the official v1.0.2-stable archive."
            ) from error

    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(archive, destination)
    return 0
