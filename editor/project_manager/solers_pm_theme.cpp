/**************************************************************************/
/*  solers_pm_theme.cpp                                                   */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                              */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/

#include "solers_pm_theme.h"

#include "core/io/image.h"
#include "editor/editor_string_names.h"
#include "editor/themes/editor_scale.h"
#include "scene/resources/image_texture.h"
#include "scene/resources/style_box_flat.h"

#include "modules/modules_enabled.gen.h"
#ifdef MODULE_SVG_ENABLED
#include "modules/svg/image_loader_svg.h"
#endif

// File-local: tighten a flat stylebox so rounded corners stay crisp. The default
// corner_detail (8) + 1px AA feather pile up brighter pixels at each corner apex,
// which — combined with a light border — shows up as tiny "corner ticks". A higher
// corner_detail (scaled to the radius) plus a sub-pixel AA size eliminates that,
// giving clean Unreal-grade edges. Idempotent.
static void _solers_tune(const Ref<StyleBoxFlat> &p_sb, int p_radius_px) {
	if (p_sb.is_null()) {
		return;
	}
	p_sb->set_anti_aliased(true);
	p_sb->set_aa_size(0.65f); // Tighter feather than the 1.0 default.
	if (p_radius_px > 0) {
		// ~2 segments per pixel of radius keeps the arc smooth at any DPI.
		p_sb->set_corner_detail(CLAMP(p_radius_px * 2, 10, 24));
	}
}

// File-local: build a flat stylebox with the given (already DPI-scaled) metrics.
// A negative margin means "leave content margin untouched".
static Ref<StyleBoxFlat> _solers_flat(const Color &p_bg, int p_radius_px, const Color &p_border, int p_border_px, float p_margin_px) {
	Ref<StyleBoxFlat> sb;
	sb.instantiate();
	sb->set_bg_color(p_bg);
	if (p_radius_px > 0) {
		sb->set_corner_radius_all(p_radius_px);
	}
	if (p_border_px > 0 && p_border.a > 0.0f) {
		sb->set_border_color(p_border);
		sb->set_border_width_all(p_border_px);
		sb->set_border_blend(true); // Blend border→bg so the corner miter never spikes bright.
	}
	if (p_margin_px >= 0.0f) {
		sb->set_content_margin_all(p_margin_px);
	}
	_solers_tune(sb, p_radius_px);
	return sb;
}

// File-local: recolor an existing control state stylebox in place, preserving
// its content margins/geometry (so layouts never shift). Falls back to a fresh
// flat box if the existing one is missing or not a StyleBoxFlat.
static void _solers_restyle_state(const Ref<Theme> &p_theme, const StringName &p_state, const StringName &p_type, const Color &p_bg, const Color &p_border, int p_radius, int p_border_px, bool p_draw_center) {
	Ref<StyleBoxFlat> box;
	if (p_theme->has_stylebox(p_state, p_type)) {
		Ref<StyleBoxFlat> existing = p_theme->get_stylebox(p_state, p_type);
		if (existing.is_valid()) {
			box = existing->duplicate();
		}
	}
	if (box.is_null()) {
		box = _solers_flat(p_bg, p_radius, p_border, p_border_px, 6 * EDSCALE);
	} else {
		box->set_bg_color(p_bg);
		box->set_corner_radius_all(p_radius);
		if (p_border_px > 0 && p_border.a > 0.0f) {
			box->set_border_color(p_border);
			box->set_border_width_all(p_border_px);
			box->set_border_blend(true);
		} else {
			box->set_border_width_all(0);
		}
	}
	box->set_draw_center(p_draw_center);
	_solers_tune(box, p_radius);
	p_theme->set_stylebox(p_state, p_type, box);
}

Ref<Texture2D> SolersPMTheme::lucide_icon(const char *p_svg_body, int p_size_px, float p_stroke_width) {
#ifdef MODULE_SVG_ENABLED
	// White stroke: callers tint via modulate, so one texture serves every state.
	const String svg = vformat(
			"<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"24\" height=\"24\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#FFFFFF\" stroke-width=\"%.2f\" stroke-linecap=\"round\" stroke-linejoin=\"round\">%s</svg>",
			p_stroke_width, String::utf8(p_svg_body));
	Ref<Image> img;
	img.instantiate();
	const float scale = (p_size_px * EDSCALE) / 24.0f;
	if (ImageLoaderSVG::create_image_from_string(img, svg, scale, false, HashMap<Color, Color>()) == OK && img.is_valid() && !img->is_empty()) {
		return ImageTexture::create_from_image(img);
	}
#endif
	return Ref<Texture2D>();
}

Ref<Texture2D> SolersPMTheme::mono_icon(const Ref<Texture2D> &p_icon) {
	if (p_icon.is_null()) {
		return p_icon;
	}
	Ref<Image> img = p_icon->get_image();
	if (img.is_null()) {
		return p_icon;
	}
	img = img->duplicate();
	if (img->is_compressed() && img->decompress() != OK) {
		return p_icon;
	}
	img->convert(Image::FORMAT_RGBA8);
	for (int y = 0; y < img->get_height(); y++) {
		for (int x = 0; x < img->get_width(); x++) {
			const Color c = img->get_pixel(x, y);
			// Luma, then lifted toward the chrome text tone so saturated glyphs
			// (red heart ≈ 0.45 luma) land as legible light-gray, not mud.
			const float g = CLAMP(c.get_luminance() * 0.5f + 0.45f, 0.0f, 1.0f);
			img->set_pixel(x, y, Color(g, g, g, c.a));
		}
	}
	return ImageTexture::create_from_image(img);
}

SolersPMTheme::Tokens SolersPMTheme::make_tokens(const Ref<Theme> &p_theme) {
	Tokens t;

	// Palette sampled against the UE5 Project Browser itself (not the Slate JSON,
	// whose values are linear-space and read too bright once treated as sRGB).
	// Four-layer neutral darks, deepest-to-lightest: window backdrop → content
	// panel → tile surface → hover. UE sits everything noticeably *deeper* than
	// stock Godot; the gap between layers is what makes the chrome read "Unreal".
	t.bg = Color(0.055f, 0.055f, 0.062f); // ~#0E0E10 — window backdrop (deepest).
	t.surface = Color(0.082f, 0.082f, 0.090f); // ~#151517 — content panel / grid backdrop.
	t.card = Color(0.118f, 0.118f, 0.128f); // ~#1E1E21 — tile / card surface.
	t.card_hover = Color(0.165f, 0.165f, 0.180f); // ~#2A2A2E — hover.
	// UE's action blue is *deep and slightly desaturated* — sampled off the
	// Create button it lands near #1265BE. Pure saturated blues (#057FFF,
	// #0070E0) read cartoonish against the graphite chrome.
	t.accent = Color(0.071f, 0.396f, 0.745f); // #1265BE — UE action blue (engine-grade).
	t.card_selected = Color(0.028f, 0.165f, 0.318f); // ~#072A51 — muted selection navy.
	// UE separates surfaces with *dark recess* lines, not bright outlines. Dark,
	// low-alpha edges read as depth and — crucially — never produce visible white
	// corner ticks the way light borders do on rounded boxes.
	t.border = Color(0, 0, 0, 0.45f); // Hairline recess separators.
	t.border_strong = Color(0, 0, 0, 0.62f); // Emphasized edges.
	t.text = Color(0.886f, 0.890f, 0.902f); // ~#E2E3E6 — primary text.
	t.text_dim = Color(0.886f, 0.890f, 0.902f, 0.55f); // Muted text.

	// UE template-tile caption band: a slightly lifted strip under the thumbnail
	// that turns solid accent blue when the tile is selected (white label on top).
	t.caption = Color(0.137f, 0.137f, 0.149f); // ~#232326 — idle band.
	t.caption_hover = Color(0.173f, 0.173f, 0.188f); // ~#2C2C30 — hovered band.
	t.caption_selected = t.accent; // Selected band = UE blue.

	// Honor a user's custom accent only if they explicitly diverged from Godot's
	// default blue; otherwise keep the canonical UE blue for an authentic look.
	if (p_theme.is_valid() && p_theme->has_color("accent_color", EditorStringName(Editor))) {
		const Color user_accent = p_theme->get_color("accent_color", EditorStringName(Editor));
		const Color godot_default = Color(0.26f, 0.55f, 0.98f);
		if (!user_accent.is_equal_approx(godot_default)) {
			t.accent = user_accent;
			t.card_selected = user_accent.lerp(Color(0, 0, 0), 0.45f);
			t.caption_selected = user_accent;
		}
	}

	return t;
}

void SolersPMTheme::apply(const Ref<Theme> &p_theme) {
	ERR_FAIL_COND(p_theme.is_null());

	const Tokens t = make_tokens(p_theme);

	const int rp = MAX(0, (int)(t.radius_panel * EDSCALE));
	const int rc = MAX(0, (int)(t.radius_card * EDSCALE));
	const int rr = MAX(0, (int)(t.radius_control * EDSCALE));
	const int hair = MAX(1, (int)(EDSCALE));

	// 1) Window backdrop — deep, neutral charcoal (drawn by background_panel).
	{
		Ref<StyleBoxFlat> bg = _solers_flat(t.bg, 0, Color(), 0, 0);
		p_theme->set_stylebox("Background", EditorStringName(EditorStyles), bg);
		p_theme->set_color("background", EditorStringName(Editor), t.bg);
	}

	// 2) Project Manager content surfaces (outer panel + inner list area).
	//    The list/grid backdrop sits one layer *below* the tiles (UE keeps the
	//    grid area darker than the template tiles so each tile reads raised).
	{
		Ref<StyleBoxFlat> panel = _solers_flat(t.surface, rp, t.border, hair, 4 * EDSCALE);
		p_theme->set_stylebox("panel_container", "ProjectManager", panel);

		Ref<StyleBoxFlat> list = _solers_flat(t.surface, rc, t.border_strong, hair, 6 * EDSCALE);
		p_theme->set_stylebox("project_list", "ProjectManager", list);

		Ref<StyleBoxFlat> qs = _solers_flat(t.card, rc, t.border, hair, 6 * EDSCALE);
		p_theme->set_stylebox("quick_settings_panel", "ProjectManager", qs);
	}

	// 3) Project list row states (hover / selected / focus).
	{
		Ref<StyleBoxFlat> hovered = _solers_flat(t.card_hover, rr, Color(), 0, 0);

		Ref<StyleBoxFlat> selected = _solers_flat(t.card_selected, rr, Color(), 0, 0);
		selected->set_border_color(t.accent);
		selected->set_border_width(SIDE_LEFT, MAX(2, (int)(3 * EDSCALE)));

		Ref<StyleBoxFlat> hover_pressed = selected->duplicate();
		hover_pressed->set_bg_color(t.card_selected.lerp(t.accent, 0.10f));

		Ref<StyleBoxFlat> focus = _solers_flat(Color(0, 0, 0, 0), rr, Color(t.accent.r, t.accent.g, t.accent.b, 0.75), hair, 0);
		focus->set_draw_center(false);

		p_theme->set_stylebox("hovered", "ProjectList", hovered);
		p_theme->set_stylebox("selected", "ProjectList", selected);
		p_theme->set_stylebox("hover_pressed", "ProjectList", hover_pressed);
		p_theme->set_stylebox("focus", "ProjectList", focus);

		p_theme->set_color(SNAME("font_color"), "ProjectList", t.text);
		// Faint row guide for an ordered, textured list (UE-like separators).
		p_theme->set_color("guide_color", "ProjectList", Color(t.border.r, t.border.g, t.border.b, t.border.a * 0.8f));
	}

	// 4) Top view toggles (MainScreenButton variation) — UE tab treatment: the
	//    active view is marked by a crisp accent *underline* on a faintly lifted
	//    fill (Unreal never fills its main tabs with solid selection blue).
	{
		Ref<StyleBoxFlat> normal = _solers_flat(Color(0, 0, 0, 0), 0, Color(), 0, -1.0f);
		normal->set_content_margin(SIDE_LEFT, 14 * EDSCALE);
		normal->set_content_margin(SIDE_RIGHT, 14 * EDSCALE);
		normal->set_content_margin(SIDE_TOP, 7 * EDSCALE);
		normal->set_content_margin(SIDE_BOTTOM, 7 * EDSCALE);

		Ref<StyleBoxFlat> hover = normal->duplicate();
		hover->set_bg_color(Color(1, 1, 1, 0.05f));

		Ref<StyleBoxFlat> pressed = normal->duplicate();
		pressed->set_bg_color(Color(1, 1, 1, 0.06f));
		pressed->set_border_color(t.accent);
		pressed->set_border_width(SIDE_BOTTOM, MAX(2, (int)(2 * EDSCALE)));
		// Keep text optically centered despite the underline's added height.
		pressed->set_content_margin(SIDE_BOTTOM, 7 * EDSCALE - pressed->get_border_width(SIDE_BOTTOM));

		Ref<StyleBoxFlat> focus = normal->duplicate();
		focus->set_draw_center(false);
		focus->set_border_color(Color(t.accent.r, t.accent.g, t.accent.b, 0.6));
		focus->set_border_width_all(hair);

		p_theme->set_stylebox(SNAME("normal"), "MainScreenButton", normal);
		p_theme->set_stylebox(SNAME("hover"), "MainScreenButton", hover);
		p_theme->set_stylebox(SNAME("pressed"), "MainScreenButton", pressed);
		p_theme->set_stylebox(SNAME("hover_pressed"), "MainScreenButton", pressed->duplicate());
		p_theme->set_stylebox(SNAME("disabled"), "MainScreenButton", normal->duplicate());
		p_theme->set_stylebox(SNAME("focus"), "MainScreenButton", focus);

		p_theme->set_color(SNAME("font_color"), "MainScreenButton", t.text_dim);
		p_theme->set_color(SNAME("font_hover_color"), "MainScreenButton", t.text);
		p_theme->set_color(SNAME("font_pressed_color"), "MainScreenButton", t.text);
		p_theme->set_color(SNAME("font_hover_pressed_color"), "MainScreenButton", t.text);
		p_theme->set_color(SNAME("font_focus_color"), "MainScreenButton", t.text);
	}

	// 5) Left navigation rail items (PMNavButton) + section headers (PMNavHeader).
	{
		p_theme->set_type_variation("PMNavButton", "Button");

		Ref<StyleBoxFlat> nav_normal = _solers_flat(Color(0, 0, 0, 0), rr, Color(), 0, -1.0f);
		nav_normal->set_content_margin(SIDE_LEFT, 12 * EDSCALE);
		nav_normal->set_content_margin(SIDE_RIGHT, 12 * EDSCALE);
		nav_normal->set_content_margin(SIDE_TOP, 6 * EDSCALE);
		nav_normal->set_content_margin(SIDE_BOTTOM, 6 * EDSCALE);

		Ref<StyleBoxFlat> nav_hover = nav_normal->duplicate();
		nav_hover->set_bg_color(t.card_hover);

		Ref<StyleBoxFlat> nav_pressed = nav_normal->duplicate();
		nav_pressed->set_bg_color(t.card_selected);
		nav_pressed->set_border_color(t.accent);
		nav_pressed->set_border_width(SIDE_LEFT, MAX(2, (int)(2 * EDSCALE)));

		Ref<StyleBoxFlat> nav_focus = nav_normal->duplicate();
		nav_focus->set_draw_center(false);
		nav_focus->set_border_color(Color(t.accent.r, t.accent.g, t.accent.b, 0.5));
		nav_focus->set_border_width_all(hair);

		p_theme->set_stylebox(SNAME("normal"), "PMNavButton", nav_normal);
		p_theme->set_stylebox(SNAME("hover"), "PMNavButton", nav_hover);
		p_theme->set_stylebox(SNAME("pressed"), "PMNavButton", nav_pressed);
		p_theme->set_stylebox(SNAME("hover_pressed"), "PMNavButton", nav_pressed->duplicate());
		p_theme->set_stylebox(SNAME("disabled"), "PMNavButton", nav_normal->duplicate());
		p_theme->set_stylebox(SNAME("focus"), "PMNavButton", nav_focus);

		p_theme->set_color(SNAME("font_color"), "PMNavButton", t.text_dim);
		p_theme->set_color(SNAME("font_hover_color"), "PMNavButton", t.text);
		p_theme->set_color(SNAME("font_pressed_color"), "PMNavButton", t.text);
		p_theme->set_color(SNAME("font_hover_pressed_color"), "PMNavButton", t.text);
		p_theme->set_color(SNAME("font_focus_color"), "PMNavButton", t.text);

		p_theme->set_type_variation("PMNavHeader", "Label");
		p_theme->set_color(SNAME("font_color"), "PMNavHeader", Color(t.text_dim.r, t.text_dim.g, t.text_dim.b, 0.45));
		p_theme->set_font_size(SNAME("font_size"), "PMNavHeader", MAX(1, (int)(11 * EDSCALE)));
		// Align section headers with the rail rows' 12px text inset and give each
		// section UE's breathing room above.
		Ref<StyleBoxFlat> header_pad = _solers_flat(Color(0, 0, 0, 0), 0, Color(), 0, 0);
		header_pad->set_content_margin(SIDE_LEFT, 12 * EDSCALE);
		header_pad->set_content_margin(SIDE_TOP, 10 * EDSCALE);
		header_pad->set_content_margin(SIDE_BOTTOM, 2 * EDSCALE);
		p_theme->set_stylebox(SNAME("normal"), "PMNavHeader", header_pad);
	}

	// 6) Card thumbnail recess (PMCardThumb) — the tile's image well. Near-black
	//    like UE's template-art area, hard-cornered, borderless (the tile outline
	//    is drawn by the card itself, so the image runs edge-to-edge).
	{
		p_theme->set_type_variation("PMCardThumb", "PanelContainer");
		Ref<StyleBoxFlat> thumb = _solers_flat(t.bg.lerp(Color(0, 0, 0), 0.25f), 0, Color(), 0, 0);
		p_theme->set_stylebox(SNAME("panel"), "PMCardThumb", thumb);
	}

	// 7) P4 — cohesive control styling for the bottom bar + dialogs.
	//     Geometry/margins are preserved (states are recolored in place), so no
	//     layout shifts; only the palette is unified with the deep PM theme.
	{
		// UE inputs are *recessed*: visibly darker than every surrounding panel,
		// nearly black, with a dark hairline edge. Focus adds only a 1px accent
		// line — Unreal never glows a 2px ring around its search fields.
		const Color input_bg = Color(0.043f, 0.043f, 0.049f); // ~#0B0B0C.

		// Buttons (bottom bar, dialog buttons, etc.) — UE-flat: a solid-reading
		// translucent-white *fill* (fills never tick at corners) over a dark recess
		// edge. Alpha is tuned so the button reads as a discrete gray slab (like
		// Unreal's secondary buttons), not a faint outline. Hover/pressed only shift
		// the fill, matching UE's restrained button feedback.
		const Color btn_n = Color(1, 1, 1, 0.10f);
		const Color btn_h = Color(1, 1, 1, 0.16f);
		const Color btn_p = Color(0, 0, 0, 0.30f);
		_solers_restyle_state(p_theme, SNAME("normal"), "Button", btn_n, t.border, rr, hair, true);
		_solers_restyle_state(p_theme, SNAME("hover"), "Button", btn_h, t.border_strong, rr, hair, true);
		_solers_restyle_state(p_theme, SNAME("pressed"), "Button", btn_p, t.border_strong, rr, hair, true);
		_solers_restyle_state(p_theme, SNAME("disabled"), "Button", Color(1, 1, 1, 0.02f), t.border, rr, hair, true);
		_solers_restyle_state(p_theme, SNAME("focus"), "Button", Color(0, 0, 0, 0), Color(t.accent.r, t.accent.g, t.accent.b, 0.6), rr, hair, false);

		// Text-only buttons need UE's unhurried horizontal padding to read as
		// deliberate slabs instead of cramped labels.
		const StringName btn_states[4] = { SNAME("normal"), SNAME("hover"), SNAME("pressed"), SNAME("disabled") };
		for (const StringName &state : btn_states) {
			Ref<StyleBoxFlat> sb = p_theme->get_stylebox(state, "Button");
			if (sb.is_valid()) {
				sb->set_content_margin(SIDE_LEFT, 14 * EDSCALE);
				sb->set_content_margin(SIDE_RIGHT, 14 * EDSCALE);
				sb->set_content_margin(SIDE_TOP, 6 * EDSCALE);
				sb->set_content_margin(SIDE_BOTTOM, 6 * EDSCALE);
			}
		}

		// Dropdowns share the button look.
		_solers_restyle_state(p_theme, SNAME("normal"), "OptionButton", btn_n, t.border, rr, hair, true);
		_solers_restyle_state(p_theme, SNAME("hover"), "OptionButton", btn_h, t.border_strong, rr, hair, true);
		_solers_restyle_state(p_theme, SNAME("pressed"), "OptionButton", btn_p, t.border_strong, rr, hair, true);
		_solers_restyle_state(p_theme, SNAME("disabled"), "OptionButton", Color(1, 1, 1, 0.02f), t.border, rr, hair, true);
		_solers_restyle_state(p_theme, SNAME("focus"), "OptionButton", Color(0, 0, 0, 0), Color(t.accent.r, t.accent.g, t.accent.b, 0.6), rr, hair, false);

		// Overflow menu buttons (the bottom-bar "⋯" entries) must read as the
		// same slab family as plain Buttons — the stock MenuButton type is bare.
		_solers_restyle_state(p_theme, SNAME("normal"), "MenuButton", btn_n, t.border, rr, hair, true);
		_solers_restyle_state(p_theme, SNAME("hover"), "MenuButton", btn_h, t.border_strong, rr, hair, true);
		_solers_restyle_state(p_theme, SNAME("pressed"), "MenuButton", btn_p, t.border_strong, rr, hair, true);
		_solers_restyle_state(p_theme, SNAME("disabled"), "MenuButton", Color(1, 1, 1, 0.02f), t.border, rr, hair, true);
		_solers_restyle_state(p_theme, SNAME("focus"), "MenuButton", Color(0, 0, 0, 0), Color(t.accent.r, t.accent.g, t.accent.b, 0.6), rr, hair, false);
		const StringName menu_btn_states[4] = { SNAME("normal"), SNAME("hover"), SNAME("pressed"), SNAME("disabled") };
		for (const StringName &state : menu_btn_states) {
			Ref<StyleBoxFlat> sb = p_theme->get_stylebox(state, "MenuButton");
			if (sb.is_valid()) {
				sb->set_content_margin(SIDE_LEFT, 10 * EDSCALE);
				sb->set_content_margin(SIDE_RIGHT, 10 * EDSCALE);
				sb->set_content_margin(SIDE_TOP, 6 * EDSCALE);
				sb->set_content_margin(SIDE_BOTTOM, 6 * EDSCALE);
			}
		}

		// Text inputs (search box, dialog fields) — recessed, hard-edged, with a
		// restrained 1px accent line on focus.
		_solers_restyle_state(p_theme, SNAME("normal"), "LineEdit", input_bg, t.border_strong, 0, hair, true);
		// Soft focus tell — the PM search box grabs focus on launch, so a loud
		// ring would be the first thing users see. UE keeps this nearly silent.
		_solers_restyle_state(p_theme, SNAME("focus"), "LineEdit", input_bg, Color(t.accent.r, t.accent.g, t.accent.b, 0.45f), 0, hair, true);
		_solers_restyle_state(p_theme, SNAME("read_only"), "LineEdit", t.surface, t.border, 0, hair, true);

		// Popup menus (context menu, dropdown lists).
		_solers_restyle_state(p_theme, SNAME("panel"), "PopupMenu", t.surface, t.border_strong, rp, hair, true);
		_solers_restyle_state(p_theme, SNAME("hover"), "PopupMenu", t.card_selected, Color(), rr, 0, true);

		// Typography density — Unreal's chrome text sits a step below the editor
		// default size; this fine-grained type is a large part of the "premium"
		// read. Titles/tabs keep their own sizes, so hierarchy is preserved.
		const int dense_fs = MAX(10, p_theme->get_default_font_size() - 2 * (int)MAX(1.0f, EDSCALE));
		p_theme->set_font_size(SNAME("font_size"), "Button", dense_fs);
		p_theme->set_font_size(SNAME("font_size"), "OptionButton", dense_fs);
		p_theme->set_font_size(SNAME("font_size"), "LineEdit", dense_fs);
		p_theme->set_font_size(SNAME("font_size"), "Label", dense_fs);
		p_theme->set_font_size(SNAME("font_size"), "PopupMenu", dense_fs);
	}

	// 8) Project CARD tiles (grid view) — faithful UE template tiles: a hard-edged
	//    slab with a 1px outline that goes light on hover and accent on selection.
	//    The selection *color* lives in the caption band (drawn by the item), not
	//    in the tile fill — exactly how UE marks its selected template.
	{
		// Normal: dark recess outline so each tile reads as a discrete slab.
		Ref<StyleBoxFlat> card = _solers_flat(t.card, 0, t.border_strong, hair, 0);

		// Hover: UE brightens the *edge* (thin light-gray outline), not the fill.
		Ref<StyleBoxFlat> card_hover = _solers_flat(t.card.lerp(Color(1, 1, 1), 0.03f), 0, Color(1, 1, 1, 0.30f), hair, 0);

		// Selected: crisp 1px accent ring; the blue caption band does the talking.
		Ref<StyleBoxFlat> card_selected = _solers_flat(t.card, 0, t.accent, hair, 0);

		p_theme->set_stylebox("solers_card", "ProjectList", card);
		p_theme->set_stylebox("solers_card_hover", "ProjectList", card_hover);
		p_theme->set_stylebox("solers_card_selected", "ProjectList", card_selected);

		// Caption band palette consumed by ProjectListItemControl's draw pass.
		p_theme->set_color("solers_caption", "ProjectList", t.caption);
		p_theme->set_color("solers_caption_hover", "ProjectList", t.caption_hover);
		p_theme->set_color("solers_caption_selected", "ProjectList", t.caption_selected);
	}

	// 9) Primary action button (PMPrimaryButton) — the UE blue filled CTA used by
	//    the bottom-bar "Edit" action, replicating Unreal's "Open" button.
	{
		p_theme->set_type_variation("PMPrimaryButton", "Button");

		Ref<StyleBoxFlat> n = _solers_flat(t.accent, rr, Color(), 0, -1.0f);
		n->set_content_margin(SIDE_LEFT, 16 * EDSCALE);
		n->set_content_margin(SIDE_RIGHT, 16 * EDSCALE);
		n->set_content_margin(SIDE_TOP, 7 * EDSCALE);
		n->set_content_margin(SIDE_BOTTOM, 7 * EDSCALE);

		Ref<StyleBoxFlat> h = n->duplicate();
		h->set_bg_color(t.accent.lerp(Color(1, 1, 1), 0.14f));

		Ref<StyleBoxFlat> pr = n->duplicate();
		pr->set_bg_color(t.accent.lerp(Color(0, 0, 0), 0.14f));

		Ref<StyleBoxFlat> dis = n->duplicate();
		dis->set_bg_color(Color(t.accent.r, t.accent.g, t.accent.b, 0.40f));

		Ref<StyleBoxFlat> foc = n->duplicate();
		foc->set_border_color(Color(1, 1, 1, 0.5));
		foc->set_border_width_all(hair);
		foc->set_border_blend(true);

		p_theme->set_stylebox(SNAME("normal"), "PMPrimaryButton", n);
		p_theme->set_stylebox(SNAME("hover"), "PMPrimaryButton", h);
		p_theme->set_stylebox(SNAME("pressed"), "PMPrimaryButton", pr);
		p_theme->set_stylebox(SNAME("hover_pressed"), "PMPrimaryButton", h->duplicate());
		p_theme->set_stylebox(SNAME("disabled"), "PMPrimaryButton", dis);
		p_theme->set_stylebox(SNAME("focus"), "PMPrimaryButton", foc);

		const Color on_accent = Color(1, 1, 1);
		p_theme->set_color(SNAME("font_color"), "PMPrimaryButton", on_accent);
		p_theme->set_color(SNAME("font_hover_color"), "PMPrimaryButton", on_accent);
		p_theme->set_color(SNAME("font_pressed_color"), "PMPrimaryButton", on_accent);
		p_theme->set_color(SNAME("font_hover_pressed_color"), "PMPrimaryButton", on_accent);
		p_theme->set_color(SNAME("font_focus_color"), "PMPrimaryButton", on_accent);
		p_theme->set_color(SNAME("icon_normal_color"), "PMPrimaryButton", on_accent);
		p_theme->set_color(SNAME("icon_hover_color"), "PMPrimaryButton", on_accent);
		p_theme->set_color(SNAME("icon_pressed_color"), "PMPrimaryButton", on_accent);

		// Segmented variants — Edit▾ must read as ONE combo control (UE-style):
		// the label segment keeps only its outer-left corners, the chevron only
		// its outer-right ones, and a dark hairline divides them internally.
		p_theme->set_type_variation("PMPrimaryButtonLeft", "Button");
		p_theme->set_type_variation("PMPrimaryButtonRight", "Button");

		auto make_segment = [&](const Ref<StyleBoxFlat> &p_src, bool p_left) -> Ref<StyleBoxFlat> {
			Ref<StyleBoxFlat> sb = p_src->duplicate();
			if (p_left) {
				sb->set_corner_radius(CORNER_TOP_RIGHT, 0);
				sb->set_corner_radius(CORNER_BOTTOM_RIGHT, 0);
				sb->set_content_margin(SIDE_LEFT, 16 * EDSCALE);
				sb->set_content_margin(SIDE_RIGHT, 12 * EDSCALE);
			} else {
				sb->set_corner_radius(CORNER_TOP_LEFT, 0);
				sb->set_corner_radius(CORNER_BOTTOM_LEFT, 0);
				// Internal divider: a single dark recess line, not a gap.
				sb->set_border_color(Color(0, 0, 0, 0.35f));
				sb->set_border_width(SIDE_LEFT, hair);
				sb->set_border_blend(true);
				sb->set_content_margin(SIDE_LEFT, 8 * EDSCALE);
				sb->set_content_margin(SIDE_RIGHT, 8 * EDSCALE);
			}
			return sb;
		};

		const StringName seg_states[6] = { SNAME("normal"), SNAME("hover"), SNAME("pressed"), SNAME("hover_pressed"), SNAME("disabled"), SNAME("focus") };
		for (const StringName &state : seg_states) {
			Ref<StyleBoxFlat> src = p_theme->get_stylebox(state, "PMPrimaryButton");
			if (src.is_valid()) {
				p_theme->set_stylebox(state, "PMPrimaryButtonLeft", make_segment(src, true));
				p_theme->set_stylebox(state, "PMPrimaryButtonRight", make_segment(src, false));
			}
		}
		const StringName seg_colors[8] = { SNAME("font_color"), SNAME("font_hover_color"), SNAME("font_pressed_color"), SNAME("font_hover_pressed_color"), SNAME("font_focus_color"), SNAME("icon_normal_color"), SNAME("icon_hover_color"), SNAME("icon_pressed_color") };
		for (const StringName &cname : seg_colors) {
			p_theme->set_color(cname, "PMPrimaryButtonLeft", on_accent);
			p_theme->set_color(cname, "PMPrimaryButtonRight", on_accent);
		}
	}

	// 10) Bottom action bar (PMBottomBarPanel) — Unreal's command bar reads as a
	//     distinct recessed strip separated from the content by a single top
	//     hairline, not a floating row of buttons. We draw only the top edge (a
	//     dark recess line) over a faint deeper fill, with generous padding so the
	//     buttons breathe like UE's footer. PanelContainer draws this behind the
	//     button HBox; geometry stays in the constructor so nothing shifts.
	{
		p_theme->set_type_variation("PMBottomBarPanel", "PanelContainer");
		Ref<StyleBoxFlat> bar = _solers_flat(t.bg, 0, Color(), 0, -1.0f);
		bar->set_border_color(t.border_strong);
		bar->set_border_width(SIDE_TOP, hair);
		bar->set_border_blend(true);
		bar->set_content_margin(SIDE_LEFT, 10 * EDSCALE);
		bar->set_content_margin(SIDE_RIGHT, 10 * EDSCALE);
		bar->set_content_margin(SIDE_TOP, 9 * EDSCALE);
		bar->set_content_margin(SIDE_BOTTOM, 9 * EDSCALE);
		p_theme->set_stylebox(SNAME("panel"), "PMBottomBarPanel", bar);
	}
}
