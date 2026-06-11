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
#include "editor/themes/editor_scale.h"
#include "modules/modules_enabled.gen.h"
#include "scene/resources/image_texture.h"
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

// Official Lucide path data (MIT, https://lucide.dev), 24x24 viewBox.
static String solers_glyph_body(const StringName &p_name) {
	if (p_name == SNAME("panel")) {
		// lucide: panel-left
		return "<rect width=\"18\" height=\"18\" x=\"3\" y=\"3\" rx=\"2\"/><path d=\"M9 3v18\"/>";
	}
	if (p_name == SNAME("new_chat")) {
		// lucide: square-pen
		return "<path d=\"M12 3H5a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h14a2 2 0 0 0 2-2v-7\"/><path d=\"M18.375 2.625a1 1 0 0 1 3 3l-9.013 9.014a2 2 0 0 1-.853.505l-2.873.84a.5.5 0 0 1-.62-.62l.84-2.873a2 2 0 0 1 .506-.852z\"/>";
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
static const Color SOLERS_PRIMARY_FILL = Color(0.64, 0.65, 0.68);
static const Color SOLERS_PRIMARY_FILL_HOVER = Color(0.78, 0.79, 0.82);
static const Color SOLERS_PRIMARY_FILL_PRESS = Color(0.52, 0.53, 0.56);
static const Color SOLERS_PRIMARY_GLYPH = Color(0.090, 0.092, 0.102);

static void solers_draw_wash(Control *p_control, const Rect2 &p_rect, float p_alpha, float p_radius) {
	if (p_alpha <= 0.001f) {
		return;
	}
	// One shared scratch stylebox: rounded rects via StyleBoxFlat without
	// allocating per widget or per frame.
	static Ref<StyleBoxFlat> wash;
	if (wash.is_null()) {
		wash.instantiate();
		wash->set_border_width_all(0);
	}
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
					fill = Color(1, 1, 1, 0.085f);
					glyph_color = Color(1, 1, 1, 0.32f);
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

void SolersSelectChip::set_show_chevron(bool p_show) {
	if (show_chevron == p_show) {
		return;
	}
	show_chevron = p_show;
	update_minimum_size();
	queue_redraw();
}

Size2 SolersSelectChip::get_minimum_size() const {
	const float ed = EDSCALE;
	const Ref<Font> font = get_theme_font(SceneStringName(font), SNAME("Label"));
	const int font_size = int(12 * ed);

	float width = 5.0f * ed; // Leading pad.
	if (glyph != StringName()) {
		width += 13.0f * ed + 5.0f * ed;
	}
	if (font.is_valid()) {
		if (!strong_text.is_empty()) {
			width += font->get_string_size(strong_text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size).x;
		}
		if (!muted_text.is_empty()) {
			width += 5.0f * ed + font->get_string_size(muted_text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size).x;
		}
	}
	if (show_chevron) {
		width += 6.0f * ed + 9.0f * ed; // Chevron gap + chevron.
	}
	width += 5.0f * ed; // Trailing pad.
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

			solers_draw_wash(this, r, 0.055f * anim + (pressing ? 0.04f : 0.0f), 7.0f * ed);

			const bool accented = solers_has_accent(accent);
			const Color strong_idle = accented ? accent : SOLERS_TEXT_STRONG;
			const Color strong_lit = accented ? accent.lerp(Color(1, 1, 1, accent.a), 0.22f) : Color(1.0f, 1.0f, 1.0f);
			const Color strong_color = strong_idle.lerp(strong_lit, anim);
			const Color muted_color = (accented ? accent.darkened(0.08f) : SOLERS_TEXT_MUTED).lerp(strong_lit, 0.42f * anim);
			const Color chevron_color = (accented ? accent : Color(0.50f, 0.51f, 0.55f)).lerp(strong_lit, 0.45f * anim);

			const Ref<Font> font = get_theme_font(SceneStringName(font), SNAME("Label"));
			const int font_size = int(12 * ed);
			float x = 5.0f * ed;

			if (glyph != StringName()) {
				Ref<Texture2D> icon = SolersChatGlyphs::get(glyph, int(Math::round(13.0f * ed)), 1.9f);
				if (icon.is_valid()) {
					const Point2 pos(x, (r.size.y - icon->get_height()) * 0.5f);
					draw_texture(icon, pos.floor(), strong_color);
				}
				x += 13.0f * ed + 5.0f * ed;
			}

			if (font.is_valid()) {
				const float ascent = font->get_ascent(font_size);
				const float text_h = font->get_height(font_size);
				const float baseline = (r.size.y - text_h) * 0.5f + ascent;
				if (!strong_text.is_empty()) {
					draw_string(font, Point2(x, baseline).floor(), strong_text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, strong_color);
					x += font->get_string_size(strong_text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size).x;
				}
				if (!muted_text.is_empty()) {
					x += 5.0f * ed;
					draw_string(font, Point2(x, baseline).floor(), muted_text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, muted_color);
					x += font->get_string_size(muted_text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size).x;
				}
			}

			x += 6.0f * ed;
			if (show_chevron) {
				Ref<Texture2D> chevron = SolersChatGlyphs::get(SNAME("chevron_down"), int(Math::round(9.0f * ed)), 2.2f);
				if (chevron.is_valid()) {
					const Point2 pos(x, (r.size.y - chevron->get_height()) * 0.5f + 0.5f * ed);
					draw_texture(chevron, pos.floor(), chevron_color);
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

			// One shared scratch box: every pass below draws a pure FILL (zero
			// border width), so the rounded corners are smooth with no AA bloom.
			static Ref<StyleBoxFlat> sb;
			if (sb.is_null()) {
				sb.instantiate();
				sb->set_border_width_all(0);
				sb->set_anti_aliased(true);
			}

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
