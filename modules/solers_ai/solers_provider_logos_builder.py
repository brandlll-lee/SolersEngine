"""Build-time compiler for Solers provider logos.

Dual-track contract under assets/provider_logos/:

- `{id}.svg` — monochrome chrome mark (models.dev / Lucide-style).
  Build bakes `currentColor` → `#ffffff`; callers tint via modulate.
- `{id}.color.svg` — official multicolor mark. Fills are preserved;
  callers must draw without theme/Button icon tint.

The FILENAME is the authority: mono id equals the provider/plugin id;
color id is the stem before `.color.svg`. No per-brand code tables.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

import methods

_ID_RE = re.compile(r"^[a-z0-9_\-]+$")


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


def _parse_logo_path(svg_path: Path) -> tuple[str, str]:
    """Return (logo_id, track) where track is 'mono' or 'color'."""
    name = svg_path.name
    if name.endswith(".color.svg"):
        logo_id = name[: -len(".color.svg")]
        track = "color"
    elif name.endswith(".svg"):
        logo_id = svg_path.stem
        track = "mono"
    else:
        raise ValueError(f"{svg_path}: expected .svg or .color.svg")
    if not _ID_RE.match(logo_id):
        raise ValueError(f"{svg_path}: filename must be a lowercase provider id, got '{logo_id}'")
    return logo_id, track


def _load_logo(svg_path: Path) -> dict[str, str]:
    logo_id, track = _parse_logo_path(svg_path)
    content = svg_path.read_text(encoding="utf-8").strip()
    if "<svg" not in content:
        raise ValueError(f"{svg_path}: not an SVG document")

    if track == "mono":
        # models.dev logos paint with CSS `currentColor`. thorvg has no CSS
        # color context, so bake white; callers tint via draw modulate.
        content = content.replace("currentColor", "#ffffff")

    return {"id": logo_id, "track": track, "svg": content}


def _write_table(file, table_name: str, count_name: str, logos: list[dict[str, str]]) -> None:
    file.write(f"static const SolersProviderLogoRecord {table_name}[] = {{\n")
    for logo in logos:
        file.write("\t{\n")
        file.write(f'\t\t"{_escape_c_string(logo["id"])}",\n')
        _write_c_string_literal(file, logo["svg"])
        file.write("\t},\n")
    file.write("};\n\n")
    file.write(f"static const int {count_name} = {len(logos)};\n")


def make_provider_logos_header(target, source, env):
    del env

    logos = [_load_logo(Path(str(src))) for src in source]
    mono = sorted((logo for logo in logos if logo["track"] == "mono"), key=lambda logo: logo["id"])
    color = sorted((logo for logo in logos if logo["track"] == "color"), key=lambda logo: logo["id"])

    for track_name, track_logos in (("mono", mono), ("color", color)):
        seen: set[str] = set()
        for logo in track_logos:
            if logo["id"] in seen:
                raise ValueError(f"duplicate provider {track_name} logo id: {logo['id']}")
            seen.add(logo["id"])

    with methods.generated_wrapper(str(target[0])) as file:
        file.write(
            "struct SolersProviderLogoRecord {\n"
            "\tconst char *id;\n"
            "\tconst char *svg;\n"
            "};\n\n"
        )
        _write_table(file, "SOLERS_PROVIDER_LOGOS", "SOLERS_PROVIDER_LOGO_COUNT", mono)
        file.write("\n")
        _write_table(file, "SOLERS_PROVIDER_COLOR_LOGOS", "SOLERS_PROVIDER_COLOR_LOGO_COUNT", color)


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("usage: solers_provider_logos_builder.py <output.gen.h>", file=sys.stderr)
        sys.exit(1)
    logos_root = Path(__file__).resolve().parent / "assets" / "provider_logos"
    # Skip underscore helper scripts' accidental .svg matches; only real marks.
    sources = sorted(p for p in logos_root.glob("*.svg") if not p.name.startswith("_"))
    if not sources:
        print(f"no provider logos found under {logos_root}", file=sys.stderr)
        sys.exit(1)
    make_provider_logos_header([sys.argv[1]], sources, None)
