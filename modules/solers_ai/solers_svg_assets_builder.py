"""Build-time compiler for Solers SVG assets.

The filename is the runtime id. There are no code-maintained asset maps.

- `assets/ui_icons/{id}.svg` — pinned Tabler UI glyph.
- `assets/provider_logos/{id}.svg` — monochrome provider mark.
  Build bakes `currentColor` → `#ffffff`; callers tint via modulate.
- `assets/provider_logos/{id}.color.svg` — official multicolor mark. Fills are
  preserved; callers must draw without theme/Button icon tint.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

import methods

_ID_RE = re.compile(r"^[a-z0-9_-]+$")


def _escape_c_string(value: str) -> str:
    return (
        value.replace("\\", "\\\\").replace('"', '\\"').replace("\r", "").replace("\n", "\\n").replace("\t", "\\t")
    )


def _write_c_string_literal(file, value: str, indent: str = "\t\t") -> None:
    # MSVC C2026: a single string literal must stay under ~16KB. Adjacent
    # literals concatenate, so large SVGs are emitted as chunked pieces.
    chunk = 8000
    if not value:
        file.write(f'{indent}"",\n')
        return
    pieces = [_escape_c_string(value[i : i + chunk]) for i in range(0, len(value), chunk)]
    file.write(indent)
    file.write(" ".join(f'"{piece}"' for piece in pieces))
    file.write(",\n")


def _parse_asset_path(svg_path: Path) -> tuple[str, str]:
    """Return (asset_id, track) where track is ui, mono, or color."""
    name = svg_path.name
    if svg_path.parent.name == "ui_icons" and name.endswith(".svg"):
        asset_id = svg_path.stem
        track = "ui"
    elif svg_path.parent.name == "provider_logos" and name.endswith(".color.svg"):
        asset_id = name[: -len(".color.svg")]
        track = "color"
    elif svg_path.parent.name == "provider_logos" and name.endswith(".svg"):
        asset_id = svg_path.stem
        track = "mono"
    else:
        raise ValueError(f"{svg_path}: unsupported Solers SVG asset path")
    if not _ID_RE.match(asset_id):
        raise ValueError(f"{svg_path}: filename must be a lowercase asset id, got '{asset_id}'")
    return asset_id, track


def _load_asset(svg_path: Path) -> dict[str, str]:
    asset_id, track = _parse_asset_path(svg_path)
    content = svg_path.read_text(encoding="utf-8").strip()
    if "<svg" not in content:
        raise ValueError(f"{svg_path}: not an SVG document")
    if track == "ui" and not re.search(r'viewBox=["\']0 0 24 24["\']', content):
        raise ValueError(f"{svg_path}: Tabler UI icons must use a 24x24 viewBox")

    if track != "color":
        # ThorVG has no CSS color context. Bake white once and tint at draw.
        content = content.replace("currentColor", "#ffffff")

    return {"id": asset_id, "track": track, "svg": content}


def _write_table(file, table_name: str, count_name: str, assets: list[dict[str, str]]) -> None:
    file.write(f"static const SolersSvgAssetRecord {table_name}[] = {{\n")
    for asset in assets:
        file.write("\t{\n")
        file.write(f'\t\t"{_escape_c_string(asset["id"])}",\n')
        _write_c_string_literal(file, asset["svg"])
        file.write("\t},\n")
    file.write("};\n\n")
    file.write(f"static const int {count_name} = {len(assets)};\n")


def make_svg_assets_header(target, source, env):
    del env

    assets = [_load_asset(Path(str(src))) for src in source]
    tracks = {}
    for track in ("ui", "mono", "color"):
        tracks[track] = sorted((asset for asset in assets if asset["track"] == track), key=lambda asset: asset["id"])

    for track_name, track_assets in tracks.items():
        seen: set[str] = set()
        for asset in track_assets:
            if asset["id"] in seen:
                raise ValueError(f"duplicate {track_name} asset id: {asset['id']}")
            seen.add(asset["id"])

    with methods.generated_wrapper(str(target[0])) as file:
        file.write(
            "struct SolersSvgAssetRecord {\n"
            "\tconst char *id;\n"
            "\tconst char *svg;\n"
            "};\n\n"
        )
        _write_table(file, "SOLERS_UI_ICONS", "SOLERS_UI_ICON_COUNT", tracks["ui"])
        file.write("\n")
        _write_table(file, "SOLERS_PROVIDER_LOGOS", "SOLERS_PROVIDER_LOGO_COUNT", tracks["mono"])
        file.write("\n")
        _write_table(file, "SOLERS_PROVIDER_COLOR_LOGOS", "SOLERS_PROVIDER_COLOR_LOGO_COUNT", tracks["color"])


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("usage: solers_svg_assets_builder.py <output.gen.h>", file=sys.stderr)
        sys.exit(1)
    assets_root = Path(__file__).resolve().parent / "assets"
    folders = ("ui_icons", "provider_logos")
    sources = sorted(p for d in folders for p in (assets_root / d).glob("*.svg") if not p.name.startswith("_"))
    if not sources:
        print(f"no SVG assets found under {assets_root}", file=sys.stderr)
        sys.exit(1)
    make_svg_assets_header([sys.argv[1]], sources, None)
