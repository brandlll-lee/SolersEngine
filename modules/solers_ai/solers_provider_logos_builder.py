"""Build-time compiler for Solers provider logos.

Each SVG under assets/provider_logos/ is a monochrome provider mark fetched
from models.dev/logos/{id}.svg (the same source opencode's icon spritesheet
uses). The FILENAME is the contract: it must equal the provider profile's
`catalog_provider` id. Adding a new provider logo is a pure data change --
drop `{catalog_id}.svg` in the assets directory, no code edits.
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


def _load_logo(svg_path: Path) -> dict[str, str]:
    logo_id = svg_path.stem
    if not _ID_RE.match(logo_id):
        raise ValueError(f"{svg_path}: filename must be a lowercase provider id, got '{logo_id}'")

    content = svg_path.read_text(encoding="utf-8").strip()
    if "<svg" not in content:
        raise ValueError(f"{svg_path}: not an SVG document")

    # models.dev logos paint with the CSS `currentColor` keyword. The runtime
    # rasterizer (thorvg) has no CSS color context, so bake the marks white
    # here; callers tint via draw modulate exactly like the Lucide glyph cache.
    content = content.replace("currentColor", "#ffffff")

    return {"id": logo_id, "svg": content}


def make_provider_logos_header(target, source, env):
    del env

    logos = [_load_logo(Path(str(src))) for src in source]
    logos.sort(key=lambda logo: logo["id"])

    seen: set[str] = set()
    for logo in logos:
        if logo["id"] in seen:
            raise ValueError(f"duplicate provider logo id: {logo['id']}")
        seen.add(logo["id"])

    with methods.generated_wrapper(str(target[0])) as file:
        file.write(
            "struct SolersProviderLogoRecord {\n"
            "\tconst char *id;\n"
            "\tconst char *svg;\n"
            "};\n\n"
        )

        file.write("static const SolersProviderLogoRecord SOLERS_PROVIDER_LOGOS[] = {\n")
        for logo in logos:
            file.write("\t{\n")
            file.write(f'\t\t"{_escape_c_string(logo["id"])}",\n')
            file.write(f'\t\t"{_escape_c_string(logo["svg"])}",\n')
            file.write("\t},\n")
        file.write("};\n\n")
        file.write(f"static const int SOLERS_PROVIDER_LOGO_COUNT = {len(logos)};\n")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("usage: solers_provider_logos_builder.py <output.gen.h>", file=sys.stderr)
        sys.exit(1)
    logos_root = Path(__file__).resolve().parent / "assets" / "provider_logos"
    sources = sorted(logos_root.glob("*.svg"))
    if not sources:
        print(f"no provider logos found under {logos_root}", file=sys.stderr)
        sys.exit(1)
    make_provider_logos_header([sys.argv[1]], sources, None)
