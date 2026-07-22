#!/usr/bin/env python3
"""One-shot: write {id}.color.svg marks from official brand sources.

Not part of the editor runtime. Re-run when refreshing color logos.
"""

from __future__ import annotations

import os
import pathlib
import re
import tempfile

from PIL import Image
import vtracer

ROOT = pathlib.Path(__file__).resolve().parent
TMP = pathlib.Path(os.environ.get("TEMP", tempfile.gettempdir())) / "solers_logos"


def write(name: str, svg: str) -> None:
	path = ROOT / name
	path.write_text(svg.strip() + "\n", encoding="utf-8")
	print(f"wrote {path.name} ({len(svg)} chars)")


def meshy_from_lobe_tsx() -> None:
	tsx = (TMP / "meshy_color.tsx").read_text(encoding="utf-8")
	paths = re.findall(r"<path\b[^/]*/>|<path\b[\s\S]*?</path>", tsx)
	# Drop React-only attrs that thorvg may choke on; keep fill/d/clipRule/fillRule.
	cleaned = []
	for p in paths:
		p = p.replace("clipRule", "clip-rule").replace("fillRule", "fill-rule")
		p = re.sub(r"\s+\{[^}]*\}", "", p)  # drop {...rest} if any
		cleaned.append(p)
	body = "\n".join(cleaned)
	svg = (
		'<svg width="24" height="24" viewBox="0 0 24 24" '
		'xmlns="http://www.w3.org/2000/svg" fill="none">\n'
		f"{body}\n</svg>"
	)
	write("meshy.color.svg", svg)


def color_trace_png(png: pathlib.Path, out_name: str, size: int = 24) -> None:
	raw = TMP / f"{out_name}.raw.svg"
	vtracer.convert_image_to_svg_py(
		str(png),
		str(raw),
		colormode="color",
		hierarchical="stacked",
		mode="spline",
		filter_speckle=4,
		color_precision=6,
		layer_difference=16,
		corner_threshold=60,
		length_threshold=4.0,
		max_iterations=10,
		splice_threshold=45,
		path_precision=2,
	)
	svg = raw.read_text(encoding="utf-8")
	m = re.search(r'viewBox="([^"]+)"', svg)
	vb = m.group(1) if m else f"0 0 {size} {size}"
	svg = re.sub(r"<\?xml[^?]*\?>\s*", "", svg)
	svg = re.sub(r"<!--.*?-->\s*", "", svg, flags=re.S)
	# Keep vtracer's coordinate viewBox; only set display size.
	svg = re.sub(
		r"<svg\b[^>]*>",
		f'<svg width="{size}" height="{size}" viewBox="{vb}" xmlns="http://www.w3.org/2000/svg">',
		svg,
		count=1,
	)
	write(out_name, svg)


def polyhaven_crop() -> pathlib.Path:
	im = Image.open(TMP / "polyhaven_256.png").convert("RGBA")
	px = im.load()
	w, h = im.size
	out = Image.new("RGBA", (w, h), (0, 0, 0, 0))
	op = out.load()
	for y in range(h):
		for x in range(w):
			r, g, b, a = px[x, y]
			if a < 32 or r + g + b < 30:
				continue
			op[x, y] = (r, g, b, a)
	path = TMP / "polyhaven_color_crop.png"
	out.save(path)
	return path


def ambientcg_square() -> pathlib.Path:
	# android-chrome is already a square brand tile.
	return TMP / "acg_android-chrome-512x512.png"


if __name__ == "__main__":
	meshy_from_lobe_tsx()
	color_trace_png(ambientcg_square(), "ambientcg.color.svg")
	color_trace_png(polyhaven_crop(), "polyhaven.color.svg")
	# ElevenLabs brand mark is monochrome bars — no .color.svg; runtime falls back to mono.
	print("skip elevenlabs.color.svg (no multicolor brand mark)")
