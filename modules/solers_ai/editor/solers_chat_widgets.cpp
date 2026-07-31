/**************************************************************************/
/*  solers_chat_widgets.cpp                                               */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                              */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/
/* Solers: AI-native game engine.                                        */
/**************************************************************************/

#include "solers_chat_widgets.h"

#include "core/input/input_event.h"
#include "core/os/keyboard.h"
#include "core/string/ustring.h"
#include "core/variant/dictionary.h"
#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "editor/themes/editor_scale.h"
#include "modules/modules_enabled.gen.h"
#include "modules/solers_ai/generated/solers_provider_logos.gen.h"
#include "scene/gui/box_container.h"
#include "scene/gui/label.h"
#include "scene/gui/panel_container.h"
#include "scene/resources/font.h"
#include "scene/resources/image_texture.h"
#include "scene/resources/style_box.h"
#include "scene/resources/style_box_flat.h"
#include "scene/theme/theme_db.h"

#ifdef MODULE_SVG_ENABLED
#include "modules/svg/image_loader_svg.h"
#endif

// Highlight blend speed (1/sec). Short and snappy, mirroring SolersCategoryCard.
static constexpr float SOLERS_WIDGET_ANIM_SPEED = 11.0f;

/* ------------------------------------------------------------------ */
/* Glyph rasterizer                                                    */
/* ------------------------------------------------------------------ */

static HashMap<String, Ref<Texture2D>> g_solers_glyph_cache;

// Lucide (ISC/MIT, https://lucide.dev) + Tabler Icons (MIT, https://tabler.io/icons).
// All bodies are stroke paths in a 24x24 viewBox; size/stroke unified at rasterize.
static String solers_glyph_body(const StringName &p_name) {
	if (p_name == SNAME("panel")) {
		// lucide: panel-left
		return "<rect width=\"18\" height=\"18\" x=\"3\" y=\"3\" rx=\"2\"/><path d=\"M9 3v18\"/>";
	}
	if (p_name == SNAME("new_chat")) {
		// lucide: square-pen
		return "<path d=\"M12 3H5a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h14a2 2 0 0 0 2-2v-7\"/><path d=\"M18.375 2.625a1 1 0 0 1 3 3l-9.013 9.014a2 2 0 0 1-.853.505l-2.873.84a.5.5 0 0 1-.62-.62l.84-2.873a2 2 0 0 1 .506-.852z\"/>";
	}
	if (p_name == SNAME("history")) {
		// lucide: history
		return "<path d=\"M3 12a9 9 0 1 0 3-6.7L3 8\"/><path d=\"M3 3v5h5\"/><path d=\"M12 7v5l4 2\"/>";
	}
	if (p_name == SNAME("more")) {
		// lucide: ellipsis-vertical
		return "<circle cx=\"12\" cy=\"12\" r=\"1\"/><circle cx=\"12\" cy=\"5\" r=\"1\"/><circle cx=\"12\" cy=\"19\" r=\"1\"/>";
	}
	if (p_name == SNAME("plus")) {
		return "<path d=\"M5 12h14\"/><path d=\"M12 5v14\"/>";
	}
	if (p_name == SNAME("shield")) {
		// lucide: shield-check
		return "<path d=\"M20 13c0 5-3.5 7.5-7.66 8.95a1 1 0 0 1-.67-.01C7.5 20.5 4 18 4 13V6a1 1 0 0 1 1-1c2 0 4.5-1.2 6.24-2.72a1.17 1.17 0 0 1 1.52 0C14.51 3.81 17 5 19 5a1 1 0 0 1 1 1z\"/><path d=\"m9 12 2 2 4-4\"/>";
	}
	if (p_name == SNAME("chevron_down")) {
		return "<path d=\"m6 9 6 6 6-6\"/>";
	}
	if (p_name == SNAME("chevron_up")) {
		return "<path d=\"m18 15-6-6-6 6\"/>";
	}
	if (p_name == SNAME("chevron_right")) {
		return "<path d=\"m9 18 6-6-6-6\"/>";
	}
	if (p_name == SNAME("check")) {
		return "<path d=\"M20 6 9 17l-5-5\"/>";
	}
	if (p_name == SNAME("cross")) {
		// lucide: x
		return "<path d=\"M18 6 6 18\"/><path d=\"m6 6 12 12\"/>";
	}
	if (p_name == SNAME("alert")) {
		// lucide: circle-alert
		return "<circle cx=\"12\" cy=\"12\" r=\"10\"/><line x1=\"12\" x2=\"12\" y1=\"8\" y2=\"12\"/><line x1=\"12\" x2=\"12.01\" y1=\"16\" y2=\"16\"/>";
	}
	if (p_name == SNAME("sparkle")) {
		// lucide: sparkle (single 4-point star)
		return "<path d=\"M9.937 15.5A2 2 0 0 0 8.5 14.063l-6.135-1.582a.5.5 0 0 1 0-.962L8.5 9.936A2 2 0 0 0 9.937 8.5l1.582-6.135a.5.5 0 0 1 .963 0L14.063 8.5A2 2 0 0 0 15.5 9.937l6.135 1.581a.5.5 0 0 1 0 .964L15.5 14.063a2 2 0 0 0-1.437 1.437l-1.582 6.135a.5.5 0 0 1-.963 0z\"/>";
	}
	if (p_name == SNAME("send_up")) {
		// lucide: arrow-up
		return "<path d=\"m5 12 7-7 7 7\"/><path d=\"M12 19V5\"/>";
	}
	if (p_name == SNAME("stop")) {
		return "<rect width=\"10\" height=\"10\" x=\"7\" y=\"7\" rx=\"1.5\" fill=\"#FFFFFF\" stroke=\"none\"/>";
	}
	if (p_name == SNAME("tool_observe")) {
		// lucide: eye
		return "<path d=\"M2.062 12.348a1 1 0 0 1 0-.696 10.75 10.75 0 0 1 19.876 0 1 1 0 0 1 0 .696 10.75 10.75 0 0 1-19.876 0\"/><circle cx=\"12\" cy=\"12\" r=\"3\"/>";
	}
	if (p_name == SNAME("tool_search")) {
		// tabler: search
		return "<path d=\"M10 10m-7 0a7 7 0 1 0 14 0a7 7 0 1 0 -14 0\"/><path d=\"M21 21l-6 -6\"/>";
	}
	if (p_name == SNAME("tool_read")) {
		// lucide: file-text
		return "<path d=\"M15 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V7Z\"/><path d=\"M14 2v4a2 2 0 0 0 2 2h4\"/><path d=\"M10 9H8\"/><path d=\"M16 13H8\"/><path d=\"M16 17H8\"/>";
	}
	if (p_name == SNAME("tool_capture")) {
		// tabler: photo
		return "<path d=\"M15 8h.01\"/><path d=\"M3 6a3 3 0 0 1 3 -3h12a3 3 0 0 1 3 3v12a3 3 0 0 1 -3 3h-12a3 3 0 0 1 -3 -3v-12z\"/><path d=\"M3 16l5 -5c.928 -.893 2.072 -.893 3 0l5 5\"/><path d=\"M14 14l1 -1c.928 -.893 2.072 -.893 3 0l3 3\"/>";
	}
	if (p_name == SNAME("tool_scene")) {
		// tabler: box
		return "<path d=\"M12 3l8 4.5l0 9l-8 4.5l-8 -4.5l0 -9l8 -4.5\"/><path d=\"M12 12l8 -4.5\"/><path d=\"M12 12l0 9\"/><path d=\"M12 12l-8 -4.5\"/>";
	}
	if (p_name == SNAME("tool_file")) {
		// tabler: pencil (write)
		return "<path d=\"M4 20h4l10.5 -10.5a2.828 2.828 0 1 0 -4 -4l-10.5 10.5v4\"/><path d=\"M13.5 6.5l4 4\"/>";
	}
	if (p_name == SNAME("tool_run")) {
		// lucide: play
		return "<polygon points=\"6 3 20 12 6 21 6 3\"/>";
	}
	if (p_name == SNAME("tool_asset")) {
		// lucide: upload
		return "<path d=\"M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4\"/><polyline points=\"17 8 12 3 7 8\"/><line x1=\"12\" x2=\"12\" y1=\"3\" y2=\"15\"/>";
	}
	if (p_name == SNAME("tool_export")) {
		// lucide: package
		return "<path d=\"m7.5 4.27 9 5.15\"/><path d=\"M21 8a2 2 0 0 0-1-1.73l-7-4a2 2 0 0 0-2 0l-7 4A2 2 0 0 0 3 8v8a2 2 0 0 0 1 1.73l7 4a2 2 0 0 0 2 0l7-4A2 2 0 0 0 21 16Z\"/><path d=\"m3.3 7 8.7 5 8.7-5\"/><path d=\"M12 22V12\"/>";
	}
	if (p_name == SNAME("tool_network")) {
		// tabler: world
		return "<path d=\"M3 12a9 9 0 1 0 18 0a9 9 0 0 0 -18 0\"/><path d=\"M3.6 9h16.8\"/><path d=\"M3.6 15h16.8\"/><path d=\"M11.5 3a17 17 0 0 0 0 18\"/><path d=\"M12.5 3a17 17 0 0 1 0 18\"/>";
	}
	if (p_name == SNAME("tool_shell")) {
		// lucide: terminal
		return "<polyline points=\"4 17 10 11 4 5\"/><line x1=\"12\" x2=\"20\" y1=\"19\" y2=\"19\"/>";
	}
	return String();
}

Ref<Texture2D> SolersChatGlyphs::get(const StringName &p_name, int p_size_px, float p_stroke_width) {
	const int size_px = MAX(2, p_size_px);
	const String key = String(p_name) + "@" + itos(size_px) + "@" + String::num(p_stroke_width, 2);
	if (const Ref<Texture2D> *found = g_solers_glyph_cache.getptr(key)) {
		return *found;
	}

	Ref<Texture2D> texture;
#ifdef MODULE_SVG_ENABLED
	const String body = solers_glyph_body(p_name);
	if (!body.is_empty()) {
		// White strokes; callers tint via draw modulate so one texture serves
		// every color state (idle/hover/accent) with zero re-rasterization.
		const String svg = vformat(
				"<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"24\" height=\"24\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#FFFFFF\" stroke-width=\"%s\" stroke-linecap=\"round\" stroke-linejoin=\"round\">%s</svg>",
				String::num(p_stroke_width, 2), body);
		Ref<Image> image;
		image.instantiate();
		const float scale = float(size_px) / 24.0f;
		if (ImageLoaderSVG::create_image_from_string(image, svg, scale, false, HashMap<Color, Color>()) == OK && image.is_valid() && !image->is_empty()) {
			texture = ImageTexture::create_from_image(image);
		}
	}
#endif
	g_solers_glyph_cache.insert(key, texture);
	return texture;
}

static const char *_solers_find_provider_logo_svg(const SolersProviderLogoRecord *p_table, int p_count, const String &p_id) {
	for (int i = 0; i < p_count; i++) {
		if (p_id == p_table[i].id) {
			return p_table[i].svg;
		}
	}
	return nullptr;
}

static Ref<Texture2D> _solers_raster_provider_logo(const char *p_svg, const String &p_cache_key, int p_size_px) {
	if (!p_svg) {
		return Ref<Texture2D>();
	}
	if (const Ref<Texture2D> *found = g_solers_glyph_cache.getptr(p_cache_key)) {
		return *found;
	}

	Ref<Texture2D> texture;
#ifdef MODULE_SVG_ENABLED
	// Intrinsic document size varies per mark; probe at 1x, then rasterize.
	const String svg_string = String::utf8(p_svg);
	Ref<Image> probe;
	probe.instantiate();
	if (ImageLoaderSVG::create_image_from_string(probe, svg_string, 1.0f, false, HashMap<Color, Color>()) == OK && probe.is_valid() && probe->get_width() > 0) {
		const float scale = float(p_size_px) / float(MAX(probe->get_width(), probe->get_height()));
		Ref<Image> image;
		image.instantiate();
		if (ImageLoaderSVG::create_image_from_string(image, svg_string, scale, false, HashMap<Color, Color>()) == OK && image.is_valid() && !image->is_empty()) {
			texture = ImageTexture::create_from_image(image);
		}
	}
#endif
	g_solers_glyph_cache.insert(p_cache_key, texture);
	return texture;
}

Ref<Texture2D> SolersChatGlyphs::provider_logo(const String &p_catalog_id, int p_size_px) {
	const int size_px = MAX(2, p_size_px);
	const String id = p_catalog_id.strip_edges().to_lower();

	// Mono track is baked white at build time; callers tint via modulate.
	const char *svg = _solers_find_provider_logo_svg(SOLERS_PROVIDER_LOGOS, SOLERS_PROVIDER_LOGO_COUNT, id);
	String cache_id = id;
	if (!svg) {
		cache_id = "synthetic";
		svg = _solers_find_provider_logo_svg(SOLERS_PROVIDER_LOGOS, SOLERS_PROVIDER_LOGO_COUNT, cache_id);
	}
	return _solers_raster_provider_logo(svg, "logo:" + cache_id + "@" + itos(size_px), size_px);
}

Ref<Texture2D> SolersChatGlyphs::provider_logo_color(const String &p_catalog_id, int p_size_px) {
	const int size_px = MAX(2, p_size_px);
	const String id = p_catalog_id.strip_edges().to_lower();
	const char *svg = _solers_find_provider_logo_svg(SOLERS_PROVIDER_COLOR_LOGOS, SOLERS_PROVIDER_COLOR_LOGO_COUNT, id);
	if (!svg) {
		return Ref<Texture2D>();
	}
	// Color track preserves official fills — do not theme-tint at draw time.
	return _solers_raster_provider_logo(svg, "logo-color:" + id + "@" + itos(size_px), size_px);
}

void SolersChatGlyphs::clear_cache() {
	g_solers_glyph_cache.clear();
}

/* ------------------------------------------------------------------ */
/* Shared palette                                                      */
/* ------------------------------------------------------------------ */

// Quiet Codex-style palette: low chrome at rest, one strong send action.
static const Color SOLERS_GLYPH_IDLE = Color(0.64, 0.65, 0.69);
static const Color SOLERS_GLYPH_HOVER = Color(0.94, 0.95, 0.97);
static const Color SOLERS_TEXT_STRONG = Color(0.90, 0.91, 0.94);
static const Color SOLERS_TEXT_MUTED = Color(0.57, 0.58, 0.62);
static const Color SOLERS_PRIMARY_FILL = Color(0.94, 0.94, 0.92);
static const Color SOLERS_PRIMARY_FILL_HOVER = Color(1.0, 1.0, 0.98);
static const Color SOLERS_PRIMARY_FILL_PRESS = Color(0.82, 0.82, 0.80);
static const Color SOLERS_PRIMARY_GLYPH = Color(0.055, 0.055, 0.052);

static void solers_draw_wash(Control *p_control, const Rect2 &p_rect, float p_alpha, float p_radius) {
	if (p_alpha <= 0.001f) {
		return;
	}
	Ref<StyleBoxFlat> wash;
	wash.instantiate();
	wash->set_border_width_all(0);
	wash->set_corner_radius_all(int(p_radius));
	wash->set_bg_color(Color(1, 1, 1, p_alpha));
	p_control->draw_style_box(wash, p_rect);
}

static bool solers_has_accent(const Color &p_color) {
	return p_color.a > 0.001f && (p_color.r > 0.001f || p_color.g > 0.001f || p_color.b > 0.001f);
}

/* ------------------------------------------------------------------ */
/* SolersGlyphButton                                                   */
/* ------------------------------------------------------------------ */

SolersGlyphButton::SolersGlyphButton() {
	set_focus_mode(FOCUS_NONE);
	set_default_cursor_shape(CURSOR_POINTING_HAND);
}

void SolersGlyphButton::configure(const StringName &p_glyph, Skin p_skin, const String &p_tooltip, int p_glyph_px) {
	glyph = p_glyph;
	skin = p_skin;
	glyph_px = p_glyph_px;
	set_tooltip_text(p_tooltip);
	const float base = (skin == SKIN_PRIMARY) ? 34.0f : 28.0f;
	set_custom_minimum_size(Size2(base, base) * EDSCALE);
	queue_redraw();
}

void SolersGlyphButton::set_accent(const Color &p_accent) {
	accent = p_accent;
	queue_redraw();
}

void SolersGlyphButton::set_enabled(bool p_enabled) {
	if (enabled_state == p_enabled) {
		return;
	}
	enabled_state = p_enabled;
	set_default_cursor_shape(enabled_state ? CURSOR_POINTING_HAND : CURSOR_ARROW);
	if (!enabled_state) {
		pressing = false;
		anim = 0.0f;
		anim_target = 0.0f;
		set_process_internal(false);
	}
	queue_redraw();
}

void SolersGlyphButton::_update_anim_target() {
	const float target = (hovering && enabled_state) ? 1.0f : 0.0f;
	if (Math::is_equal_approx(target, anim_target) && !is_processing_internal()) {
		if (!Math::is_equal_approx(anim, anim_target)) {
			set_process_internal(true);
		}
		return;
	}
	anim_target = target;
	set_process_internal(true);
}

void SolersGlyphButton::_activate() {
	if (enabled_state && pressed_callback.is_valid()) {
		pressed_callback.call();
	}
}

void SolersGlyphButton::gui_input(const Ref<InputEvent> &p_event) {
	if (!enabled_state) {
		return;
	}

	Ref<InputEventMouseButton> mb = p_event;
	if (mb.is_valid() && mb->get_button_index() == MouseButton::LEFT) {
		if (mb->is_pressed()) {
			pressing = true;
			queue_redraw();
		} else {
			const bool inside = Rect2(Point2(), get_size()).has_point(mb->get_position());
			pressing = false;
			queue_redraw();
			if (inside) {
				_activate();
			}
		}
		accept_event();
		return;
	}

	Ref<InputEventKey> key = p_event;
	if (key.is_valid() && key->is_pressed() && !key->is_echo()) {
		const Key code = key->get_keycode();
		if (code == Key::ENTER || code == Key::KP_ENTER || code == Key::SPACE) {
			_activate();
			accept_event();
		}
	}
}

void SolersGlyphButton::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_MOUSE_ENTER: {
			hovering = true;
			_update_anim_target();
		} break;
		case NOTIFICATION_MOUSE_EXIT: {
			hovering = false;
			pressing = false;
			_update_anim_target();
		} break;
		case NOTIFICATION_INTERNAL_PROCESS: {
			const float step = SOLERS_WIDGET_ANIM_SPEED * float(get_process_delta_time());
			anim = Math::move_toward(anim, anim_target, step);
			if (Math::is_equal_approx(anim, anim_target)) {
				set_process_internal(false);
			}
			queue_redraw();
		} break;
		case NOTIFICATION_DRAW: {
			const Rect2 r(Point2(), get_size());
			const float ed = EDSCALE;

			Color glyph_color;
			if (skin == SKIN_PRIMARY) {
				Color fill;
				if (!enabled_state) {
					fill = Color(0.245, 0.245, 0.252);
					glyph_color = Color(0.70, 0.70, 0.70, 0.62f);
				} else {
					fill = pressing ? SOLERS_PRIMARY_FILL_PRESS : SOLERS_PRIMARY_FILL.lerp(SOLERS_PRIMARY_FILL_HOVER, anim);
					glyph_color = SOLERS_PRIMARY_GLYPH;
				}
				const float radius = MIN(r.size.x, r.size.y) * 0.5f;
				draw_circle(r.get_center(), radius, fill, true, -1.0f, true);
			} else {
				const float wash_alpha = 0.075f * anim + (pressing ? 0.045f : 0.0f);
				solers_draw_wash(this, r, wash_alpha, 8.0f * ed);
				const bool accented = solers_has_accent(accent);
				const Color base = accented ? accent : SOLERS_GLYPH_IDLE;
				const Color lit = accented ? accent.lerp(Color(1, 1, 1, accent.a), 0.22f) : SOLERS_GLYPH_HOVER;
				glyph_color = enabled_state ? base.lerp(lit, anim) : Color(1, 1, 1, 0.25f);
			}

			Ref<Texture2D> tex = SolersChatGlyphs::get(glyph, int(Math::round(glyph_px * ed)));
			if (tex.is_valid()) {
				const Point2 pos = (r.size - Size2(tex->get_size())) * 0.5f;
				draw_texture(tex, pos.floor(), glyph_color);
			}
		} break;
	}
}

/* ------------------------------------------------------------------ */
/* SolersSelectChip                                                    */
/* ------------------------------------------------------------------ */

SolersSelectChip::SolersSelectChip() {
	set_focus_mode(FOCUS_NONE);
	set_default_cursor_shape(CURSOR_POINTING_HAND);
	set_v_size_flags(SIZE_SHRINK_CENTER);
}

void SolersSelectChip::configure(const StringName &p_glyph, const String &p_strong, const String &p_muted, const String &p_tooltip) {
	glyph = p_glyph;
	strong_text = p_strong;
	muted_text = p_muted;
	set_tooltip_text(p_tooltip);
	update_minimum_size();
	queue_redraw();
}

void SolersSelectChip::set_accent(const Color &p_accent) {
	accent = p_accent;
	queue_redraw();
}

void SolersSelectChip::set_texts(const String &p_strong, const String &p_muted) {
	strong_text = p_strong;
	muted_text = p_muted;
	update_minimum_size();
	queue_redraw();
}

void SolersSelectChip::set_leading_texture(const Ref<Texture2D> &p_texture) {
	if (leading_texture == p_texture) {
		return;
	}
	leading_texture = p_texture;
	update_minimum_size();
	queue_redraw();
}

void SolersSelectChip::set_show_chevron(bool p_show) {
	if (show_chevron == p_show) {
		return;
	}
	show_chevron = p_show;
	update_minimum_size();
	queue_redraw();
}

void SolersSelectChip::set_filled(bool p_filled) {
	if (filled == p_filled) {
		return;
	}
	filled = p_filled;
	queue_redraw();
}

Size2 SolersSelectChip::get_minimum_size() const {
	const float ed = EDSCALE;
	const Ref<Font> font = get_theme_font(SceneStringName(font), SNAME("Label"));
	const int font_size = int(12 * ed);
	const float icon_slot = 12.0f * ed;
	const float gap_icon_txt = 4.0f * ed;
	const float gap_txt_chev = 4.0f * ed;
	const float chevron_px = 9.0f * ed;
	const float pad_x = 6.0f * ed;

	float width = pad_x;
	if (leading_texture.is_valid() || glyph != StringName()) {
		width += icon_slot + gap_icon_txt;
	}
	if (font.is_valid()) {
		if (!strong_text.is_empty()) {
			width += font->get_string_size(strong_text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size).x;
		}
		if (!muted_text.is_empty()) {
			width += gap_icon_txt + font->get_string_size(muted_text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size).x;
		}
	}
	if (show_chevron) {
		width += gap_txt_chev + chevron_px;
	}
	width += pad_x;
	return Size2(width, 24.0f * ed);
}

void SolersSelectChip::_update_anim_target() {
	const float target = hovering ? 1.0f : 0.0f;
	anim_target = target;
	set_process_internal(true);
}

void SolersSelectChip::_activate() {
	if (pressed_callback.is_valid()) {
		pressed_callback.call();
	}
}

void SolersSelectChip::gui_input(const Ref<InputEvent> &p_event) {
	Ref<InputEventMouseButton> mb = p_event;
	if (mb.is_valid() && mb->get_button_index() == MouseButton::LEFT) {
		if (mb->is_pressed()) {
			pressing = true;
			queue_redraw();
		} else {
			const bool inside = Rect2(Point2(), get_size()).has_point(mb->get_position());
			pressing = false;
			queue_redraw();
			if (inside) {
				_activate();
			}
		}
		accept_event();
		return;
	}

	Ref<InputEventKey> key = p_event;
	if (key.is_valid() && key->is_pressed() && !key->is_echo()) {
		const Key code = key->get_keycode();
		if (code == Key::ENTER || code == Key::KP_ENTER || code == Key::SPACE) {
			_activate();
			accept_event();
		}
	}
}

void SolersSelectChip::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_MOUSE_ENTER: {
			hovering = true;
			_update_anim_target();
		} break;
		case NOTIFICATION_MOUSE_EXIT: {
			hovering = false;
			pressing = false;
			_update_anim_target();
		} break;
		case NOTIFICATION_THEME_CHANGED: {
			update_minimum_size();
		} break;
		case NOTIFICATION_INTERNAL_PROCESS: {
			const float step = SOLERS_WIDGET_ANIM_SPEED * float(get_process_delta_time());
			anim = Math::move_toward(anim, anim_target, step);
			if (Math::is_equal_approx(anim, anim_target)) {
				set_process_internal(false);
			}
			queue_redraw();
		} break;
		case NOTIFICATION_DRAW: {
			const Rect2 r(Point2(), get_size());
			const float ed = EDSCALE;

			const float wash = filled ? (0.085f + 0.04f * anim + (pressing ? 0.03f : 0.0f)) : (0.055f * anim + (pressing ? 0.04f : 0.0f));
			solers_draw_wash(this, r, wash, 12.0f * ed);

			const bool accented = solers_has_accent(accent);
			const Color strong_idle = accented ? accent : SOLERS_TEXT_STRONG;
			const Color strong_lit = accented ? accent.lerp(Color(1, 1, 1, accent.a), 0.22f) : Color(1.0f, 1.0f, 1.0f);
			const Color strong_color = strong_idle.lerp(strong_lit, anim);
			const Color muted_color = (accented ? accent.darkened(0.08f) : SOLERS_TEXT_MUTED).lerp(strong_lit, 0.42f * anim);
			const Color chevron_color = (accented ? accent : Color(0.50f, 0.51f, 0.55f)).lerp(strong_lit, 0.45f * anim);

			const Ref<Font> font = get_theme_font(SceneStringName(font), SNAME("Label"));
			const int font_size = int(12 * ed);
			const float icon_slot = 12.0f * ed;
			const float gap_icon_txt = 4.0f * ed;
			const float gap_txt_chev = 4.0f * ed;
			float x = 6.0f * ed;

			if (leading_texture.is_valid()) {
				const float tw = leading_texture->get_width();
				const float th = leading_texture->get_height();
				const Point2 pos(x + (icon_slot - tw) * 0.5f, (r.size.y - th) * 0.5f);
				draw_texture(leading_texture, pos.round(), Color(1, 1, 1, 0.80f + 0.20f * anim));
				x += icon_slot + gap_icon_txt;
			} else if (glyph != StringName()) {
				Ref<Texture2D> icon = SolersChatGlyphs::get(glyph, int(Math::round(icon_slot)), 1.9f);
				if (icon.is_valid()) {
					const float tw = icon->get_width();
					const float th = icon->get_height();
					const Point2 pos(x + (icon_slot - tw) * 0.5f, (r.size.y - th) * 0.5f);
					draw_texture(icon, pos.round(), strong_color);
				}
				x += icon_slot + gap_icon_txt;
			}

			if (font.is_valid()) {
				const float ascent = font->get_ascent(font_size);
				const float text_h = font->get_height(font_size);
				const float baseline = (r.size.y - text_h) * 0.5f + ascent;
				if (!strong_text.is_empty()) {
					draw_string(font, Point2(x, baseline).round(), strong_text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, strong_color);
					x += font->get_string_size(strong_text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size).x;
				}
				if (!muted_text.is_empty()) {
					x += gap_icon_txt;
					draw_string(font, Point2(x, baseline).round(), muted_text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, muted_color);
					x += font->get_string_size(muted_text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size).x;
				}
			}

			x += gap_txt_chev;
			if (show_chevron) {
				Ref<Texture2D> chevron = SolersChatGlyphs::get(SNAME("chevron_down"), int(Math::round(9.0f * ed)), 2.2f);
				if (chevron.is_valid()) {
					const Point2 pos(x, (r.size.y - chevron->get_height()) * 0.5f);
					draw_texture(chevron, pos.round(), chevron_color);
				}
			}
		} break;
	}
}

/* ------------------------------------------------------------------ */
/* SolersToolbarDivider                                                */
/* ------------------------------------------------------------------ */

SolersToolbarDivider::SolersToolbarDivider() {
	set_mouse_filter(MOUSE_FILTER_IGNORE);
	set_v_size_flags(SIZE_SHRINK_CENTER);
}

Size2 SolersToolbarDivider::get_minimum_size() const {
	const float ed = EDSCALE;
	return Size2(11.0f * ed, 16.0f * ed);
}

void SolersToolbarDivider::_notification(int p_what) {
	if (p_what != NOTIFICATION_DRAW) {
		return;
	}
	const float ed = EDSCALE;
	const Rect2 r(Point2(), get_size());
	const float line_h = 15.0f * ed;
	const float x = Math::floor(r.size.x * 0.5f);
	const float y0 = (r.size.y - line_h) * 0.5f;
	draw_line(Point2(x, y0), Point2(x, y0 + line_h), Color(1, 1, 1, 0.12f), MAX(1.0f, ed), true);
}

/* ------------------------------------------------------------------ */
/* SolersSurface                                                       */
/* ------------------------------------------------------------------ */

SolersSurface::SolersSurface() {
	// Background only; let pointer events fall through to the child controls.
	set_mouse_filter(MOUSE_FILTER_PASS);
}

void SolersSurface::set_surface_colors(const Color &p_bg, const Color &p_border) {
	bg = p_bg;
	// Check if border is translucent white (the common case for hairline separators).
	// If so, blend it with the background to get an opaque color that renders cleanly
	// without white AA bloom at the corners.
	const bool is_translucent_white = (p_border.r > 0.9f && p_border.g > 0.9f && p_border.b > 0.9f && p_border.a < 1.0f);
	if (is_translucent_white) {
		// Blend with background: keep only a barely visible edge.
		const float blend = p_border.a * 0.28f;
		has_border = blend > 0.01f;
		border_opaque = bg.lerp(p_border, blend);
		// Force fully opaque.
		border_opaque.a = 1.0f;
	} else {
		has_border = p_border.a > 0.0001f;
		border_opaque = Color(p_border.r, p_border.g, p_border.b, 1.0f);
	}
	queue_redraw();
}

void SolersSurface::configure(const Color &p_bg, const Color &p_border, float p_radius, int p_padding, bool p_shadow, float p_border_width) {
	radius = p_radius;
	border_w = p_border_width;
	shadow = p_shadow;
	set_surface_colors(p_bg, p_border);

	const int pad = int(p_padding * EDSCALE);
	add_theme_constant_override("margin_left", pad);
	add_theme_constant_override("margin_right", pad);
	add_theme_constant_override("margin_top", pad);
	add_theme_constant_override("margin_bottom", pad);
	queue_redraw();
}

void SolersSurface::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_DRAW: {
			const float ed = EDSCALE;
			const Rect2 r(Point2(), get_size());
			const float rad = radius * ed;
			const float bw = MAX(1.0f, border_w * ed);

			Ref<StyleBoxFlat> sb;
			sb.instantiate();
			sb->set_border_width_all(0);
			sb->set_anti_aliased(true);

			// Very light separation; the composer should feel embedded, not card-heavy.
			if (shadow) {
				sb->set_bg_color(Color(0, 0, 0, 0));
				sb->set_corner_radius_all(int(rad));
				sb->set_shadow_color(Color(0, 0, 0, 0.14f));
				sb->set_shadow_size(int(9.0f * ed));
				sb->set_shadow_offset(Point2(0, 2.0f * ed));
				draw_style_box(sb, r);
				sb->set_shadow_size(0);
				sb->set_shadow_offset(Point2());
				sb->set_shadow_color(Color(0, 0, 0, 0));
			}

			if (has_border) {
				// Outer fill = the (now opaque) hairline color.
				sb->set_bg_color(border_opaque);
				sb->set_corner_radius_all(int(rad));
				draw_style_box(sb, r);
				// Inner fill = the real background, inset by one border width.
				sb->set_bg_color(bg);
				sb->set_corner_radius_all(MAX(0, int(rad - bw)));
				draw_style_box(sb, r.grow(-bw));
			} else {
				sb->set_bg_color(bg);
				sb->set_corner_radius_all(int(rad));
				draw_style_box(sb, r);
			}
		} break;
	}
}

/* ------------------------------------------------------------------ */
/* Shared mention chip paint                                           */
/* ------------------------------------------------------------------ */

String solers_mention_chip_label(const Dictionary &p_mention) {
	return String(p_mention.get("label", p_mention.get("id", String()))).strip_edges();
}

float solers_mention_chip_width(const String &p_label, const Ref<Font> &p_font, int p_font_size, bool p_has_icon) {
	const float pad_x = 6.0f * EDSCALE;
	const float gap = 4.0f * EDSCALE;
	const int icon_px = int(Math::round(13.0f * EDSCALE));
	const float text_w = p_font.is_valid() ? p_font->get_string_size(p_label, HORIZONTAL_ALIGNMENT_LEFT, -1, p_font_size).x : float(p_label.length() * p_font_size * 0.55f);
	return pad_x + (p_has_icon ? icon_px + gap : 0.0f) + text_w + pad_x;
}

void solers_draw_mention_chip(RID p_ci, const Rect2 &p_pill, const String &p_label, const Ref<Font> &p_font, int p_font_size, const Ref<Texture2D> &p_icon) {
	if (p_label.is_empty()) {
		return;
	}
	const float pad_x = 5.0f * EDSCALE;
	const float gap = 4.0f * EDSCALE;
	const int icon_px = int(Math::round(13.0f * EDSCALE));

	Ref<StyleBoxFlat> style;
	style.instantiate();
	style->set_bg_color(solers_chip_bg());
	style->set_corner_radius_all(int(Math::round(6.0f * EDSCALE)));
	style->draw(p_ci, p_pill);

	float x = p_pill.position.x + pad_x;
	const float mid_y = p_pill.position.y + p_pill.size.y * 0.5f;
	if (p_icon.is_valid()) {
		const Rect2 icon_rect(x, mid_y - icon_px * 0.5f, icon_px, icon_px);
		p_icon->draw_rect(p_ci, icon_rect, false);
		x += icon_px + gap;
	}
	if (p_font.is_valid()) {
		const float text_y = p_pill.position.y + (p_pill.size.y - p_font->get_height(p_font_size)) * 0.5f + p_font->get_ascent(p_font_size);
		p_font->draw_string(p_ci, Point2(x, text_y), p_label, HORIZONTAL_ALIGNMENT_LEFT, -1, p_font_size, solers_chip_text());
	}
}

Ref<Texture2D> solers_mention_chip_icon(const Dictionary &p_mention, int p_px) {
	const String source = String(p_mention.get("source", "plugin")).strip_edges().to_lower();
	if (source == "plugin") {
		const String id = String(p_mention.get("id", String())).strip_edges().to_lower();
		const Ref<Texture2D> color = SolersChatGlyphs::provider_logo_color(id, p_px);
		return color.is_valid() ? color : SolersChatGlyphs::provider_logo(id, p_px);
	}
	EditorNode *editor = EditorNode::get_singleton();
	if (!editor) {
		return Ref<Texture2D>();
	}
	if (source == "addon") {
		return editor->get_editor_theme()->get_icon(SNAME("PluginScript"), EditorStringName(EditorIcons));
	}
	if (source == "folder") {
		return editor->get_editor_theme()->get_icon(SNAME("Folder"), EditorStringName(EditorIcons));
	}
	if (source == "node") {
		const String type = String(p_mention.get("type", "Node")).strip_edges();
		return editor->get_class_icon(type.is_empty() ? String("Node") : type, "Node");
	}
	return editor->get_class_icon("File");
}

/* ------------------------------------------------------------------ */
/* SolersPlanCapsule                                                   */
/* ------------------------------------------------------------------ */

SolersPlanCapsule::SolersPlanCapsule() {
	set_mouse_filter(MOUSE_FILTER_STOP);
	set_h_size_flags(SIZE_SHRINK_CENTER);
	set_v_size_flags(SIZE_SHRINK_CENTER);
	hide();

	detail_panel = memnew(PanelContainer);
	detail_panel->set_mouse_filter(MOUSE_FILTER_STOP);
	detail_panel->set_as_top_level(true);
	detail_panel->hide();
	add_child(detail_panel);

	Ref<StyleBoxFlat> detail_style;
	detail_style.instantiate();
	detail_style->set_bg_color(Color(0.11f, 0.115f, 0.125f, 0.96f));
	detail_style->set_border_width_all(0);
	detail_style->set_corner_radius_all(10);
	detail_style->set_content_margin_all(10);
	detail_panel->add_theme_style_override("panel", detail_style);

	detail_list = memnew(VBoxContainer);
	detail_list->add_theme_constant_override("separation", 6);
	detail_panel->add_child(detail_list);
	detail_panel->connect(SceneStringName(mouse_entered), callable_mp(this, &SolersPlanCapsule::_on_detail_mouse).bind(true));
	detail_panel->connect(SceneStringName(mouse_exited), callable_mp(this, &SolersPlanCapsule::_on_detail_mouse).bind(false));
}

int SolersPlanCapsule::_current_step_index() const {
	int first_open = -1;
	for (int i = 0; i < plan.size(); i++) {
		const String status = Dictionary(plan[i]).get("status", "pending");
		if (status == "in_progress") {
			return i + 1;
		}
		if (first_open < 0 && status != "completed") {
			first_open = i + 1;
		}
	}
	if (first_open > 0) {
		return first_open;
	}
	return plan.is_empty() ? 0 : plan.size();
}

bool SolersPlanCapsule::_has_open_work() const {
	if (plan.is_empty()) {
		return false;
	}
	for (int i = 0; i < plan.size(); i++) {
		if (String(Dictionary(plan[i]).get("status", "pending")) != "completed") {
			return true;
		}
	}
	return false;
}

Size2 SolersPlanCapsule::_chip_size() const {
	const float ed = EDSCALE;
	if (!_has_open_work()) {
		return Size2();
	}
	const Ref<Font> font = get_theme_font(SceneStringName(font), SNAME("Label"));
	const int font_size = int(12 * ed);
	const String label = vformat("Step %d / %d", _current_step_index(), plan.size());
	float width = 28.0f * ed;
	if (font.is_valid()) {
		width += font->get_string_size(label, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size).x;
	} else {
		width += 72.0f * ed;
	}
	return Size2(width, 28.0f * ed);
}

void SolersPlanCapsule::_rebuild_detail() {
	if (!detail_list) {
		return;
	}
	while (detail_list->get_child_count() > 0) {
		Node *child = detail_list->get_child(0);
		detail_list->remove_child(child);
		child->queue_free();
	}
	for (int i = 0; i < plan.size(); i++) {
		const Dictionary item = plan[i];
		const String status = item.get("status", "pending");
		const String marker = status == "completed" ? String::utf8("✓ ") :
				status == "in_progress" ? String::utf8("→ ") :
										  String::utf8("○ ");
		Label *row = memnew(Label(marker + String(item.get("step", String()))));
		row->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
		row->set_custom_minimum_size(Size2(220 * EDSCALE, 0));
		row->add_theme_color_override("font_color", status == "completed" ? Color(0.55f, 0.58f, 0.62f) : Color(0.82f, 0.84f, 0.88f));
		detail_list->add_child(row);
	}
}

void SolersPlanCapsule::_on_detail_mouse(bool p_entered) {
	detail_hovering = p_entered;
	_sync_detail_visibility();
}

void SolersPlanCapsule::_sync_detail_visibility() {
	if (!detail_panel) {
		return;
	}
	const bool show = (hovering || detail_hovering) && _has_open_work() && !plan.is_empty();
	if (!show) {
		detail_panel->hide();
		return;
	}
	const Size2 popup_size = detail_panel->get_combined_minimum_size();
	const Vector2 origin = get_global_transform_with_canvas().get_origin();
	const Size2 chip = get_size();
	detail_panel->set_size(popup_size);
	detail_panel->set_position(origin + Vector2((chip.x - popup_size.x) * 0.5f, -popup_size.y - 8.0f * EDSCALE));
	detail_panel->show();
}

void SolersPlanCapsule::set_plan(const String &p_explanation, const Array &p_plan) {
	explanation = p_explanation.strip_edges();
	plan = p_plan.duplicate(true);
	_rebuild_detail();
	set_visible(_has_open_work());
	update_minimum_size();
	queue_redraw();
	_sync_detail_visibility();
}

void SolersPlanCapsule::clear_plan() {
	explanation = String();
	plan.clear();
	hovering = false;
	detail_hovering = false;
	_rebuild_detail();
	hide();
	if (detail_panel) {
		detail_panel->hide();
	}
	update_minimum_size();
	queue_redraw();
}

Size2 SolersPlanCapsule::get_minimum_size() const {
	return _chip_size();
}

void SolersPlanCapsule::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_MOUSE_ENTER: {
			hovering = true;
			_sync_detail_visibility();
			queue_redraw();
		} break;
		case NOTIFICATION_MOUSE_EXIT: {
			hovering = false;
			_sync_detail_visibility();
			queue_redraw();
		} break;
		case NOTIFICATION_THEME_CHANGED:
		case NOTIFICATION_RESIZED: {
			update_minimum_size();
			queue_redraw();
		} break;
		case NOTIFICATION_VISIBILITY_CHANGED: {
			if (!is_visible_in_tree() && detail_panel) {
				detail_panel->hide();
			}
		} break;
		case NOTIFICATION_DRAW: {
			if (!_has_open_work()) {
				break;
			}
			const float ed = EDSCALE;
			const Rect2 r(Point2(), get_size());
			solers_draw_wash(this, r, 0.09f, 14.0f * ed);

			const Ref<Font> font = get_theme_font(SceneStringName(font), SNAME("Label"));
			if (font.is_null()) {
				break;
			}
			const int font_size = int(12 * ed);
			const String label = vformat("Step %d / %d", _current_step_index(), plan.size());
			const Color text = Color(0.86f, 0.88f, 0.92f);
			const float ring = 8.0f * ed;
			const Point2 ring_c(10.0f * ed + ring * 0.5f, r.size.y * 0.5f);
			draw_arc(ring_c, ring * 0.5f, -Math::PI * 0.5f, Math::PI * 1.2f, 24, Color(0.45f, 0.72f, 0.95f, 0.9f), 1.5f * ed, true);
			const float ascent = font->get_ascent(font_size);
			const float baseline = (r.size.y - font->get_height(font_size)) * 0.5f + ascent;
			draw_string(font, Point2(10.0f * ed + ring + 6.0f * ed, baseline).floor(), label, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, text);
		} break;
	}
}

/* ------------------------------------------------------------------ */
/* Tool ui_kind → glyph + verb (single table)                          */
/* ------------------------------------------------------------------ */

namespace {
struct SolersToolUiChromeRow {
	const char *kind;
	const char *glyph; // StringName literal
	const char *verb; // English source for TTR
};

static const SolersToolUiChromeRow SOLERS_TOOL_UI_CHROME[] = {
	{ "observe", "tool_observe", "Inspect" },
	{ "read", "tool_read", "Read" },
	{ "search", "tool_search", "Search" },
	{ "write", "tool_file", "Edit" },
	{ "scene", "tool_scene", "Scene" },
	{ "shell", "tool_shell", "Terminal" },
	{ "run", "tool_run", "Run" },
	{ "network", "tool_network", "Network" },
	{ "asset", "tool_asset", "Asset" },
	{ "capture", "tool_capture", "Capture" },
	{ "think", "sparkle", "Wait" },
	{ "shield", "shield", "Permission" },
};

static const SolersToolUiChromeRow *_solers_tool_ui_row_for_kind(const String &p_kind) {
	for (const SolersToolUiChromeRow &row : SOLERS_TOOL_UI_CHROME) {
		if (p_kind == row.kind) {
			return &row;
		}
	}
	return nullptr;
}
} // namespace

StringName solers_tool_glyph_for_ui_kind(const String &p_ui_kind) {
	const SolersToolUiChromeRow *row = _solers_tool_ui_row_for_kind(p_ui_kind.strip_edges());
	return row ? StringName(row->glyph) : StringName();
}

String solers_tool_verb_for_ui_kind(const String &p_ui_kind) {
	const SolersToolUiChromeRow *row = _solers_tool_ui_row_for_kind(p_ui_kind.strip_edges());
	return row ? TTR(row->verb) : TTR("Tool");
}

String solers_tool_verb_for_glyph(const StringName &p_glyph) {
	if (p_glyph == SNAME("tool_export")) {
		return TTR("Asset");
	}
	for (const SolersToolUiChromeRow &row : SOLERS_TOOL_UI_CHROME) {
		if (p_glyph == StringName(row.glyph)) {
			return TTR(row.verb);
		}
	}
	return TTR("Tool");
}

void solers_style_bare_search_line_edit(LineEdit *p_edit) {
	ERR_FAIL_NULL(p_edit);
	Ref<StyleBoxEmpty> empty;
	empty.instantiate();
	empty->set_content_margin_individual(4 * EDSCALE, 4 * EDSCALE, 4 * EDSCALE, 4 * EDSCALE);
	p_edit->add_theme_style_override("normal", empty);
	p_edit->add_theme_style_override("focus", empty);
	p_edit->add_theme_style_override("read_only", empty);
	p_edit->add_theme_color_override(SceneStringName(font_color), Color(0.918f, 0.929f, 0.945f));
	p_edit->add_theme_color_override("font_placeholder_color", Color(0.345f, 0.357f, 0.388f));
	p_edit->add_theme_font_size_override(SceneStringName(font_size), int(12 * EDSCALE));
	p_edit->set_custom_minimum_size(Size2(0, 26 * EDSCALE));
}
