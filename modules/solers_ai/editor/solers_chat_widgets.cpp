/**************************************************************************/
/*  solers_chat_widgets.cpp                                               */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "solers_chat_widgets.h"

#include "core/input/input_event.h"
#include "core/io/file_access.h"
#include "core/io/image.h"
#include "core/object/callable_mp.h"
#include "core/os/keyboard.h"
#include "core/os/time.h"
#include "core/string/ustring.h"
#include "core/variant/dictionary.h"
#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "editor/themes/editor_scale.h"
#include "main/app_icon.gen.h"
#include "scene/gui/box_container.h"
#include "scene/gui/label.h"
#include "scene/gui/panel_container.h"
#include "scene/gui/text_edit.h"
#include "scene/resources/font.h"
#include "scene/resources/image_texture.h"
#include "scene/resources/style_box.h"
#include "scene/resources/style_box_flat.h"
#include "scene/theme/theme_db.h"

#include "modules/modules_enabled.gen.h"
#include "modules/solers_ai/core/solers_trace.h"
#include "modules/solers_ai/editor/solers_ui_theme.h"
#include "modules/solers_ai/generated/solers_svg_assets.gen.h"

#ifdef MODULE_SVG_ENABLED
#include "modules/svg/image_loader_svg.h"
#endif

// Highlight blend speed (1/sec). Short and snappy, mirroring SolersCategoryCard.
static constexpr float SOLERS_WIDGET_ANIM_SPEED = 11.0f;

/* ------------------------------------------------------------------ */
/* Glyph rasterizer                                                    */
/* ------------------------------------------------------------------ */

static HashMap<String, Ref<Texture2D>> g_solers_icon_cache;

static const char *_solers_find_svg(const SolersSvgAssetRecord *p_table, int p_count, const String &p_id) {
	for (int i = 0; i < p_count; i++) {
		if (p_id == p_table[i].id) {
			return p_table[i].svg;
		}
	}
	return nullptr;
}

static Ref<Texture2D> _solers_raster_svg(const char *p_svg, const String &p_cache_key, int p_size_px, int p_source_px = 0) {
	if (!p_svg) {
		return Ref<Texture2D>();
	}
	if (const Ref<Texture2D> *found = g_solers_icon_cache.getptr(p_cache_key)) {
		return *found;
	}

	Ref<Texture2D> texture;
#ifdef MODULE_SVG_ENABLED
	const String svg_string = String::utf8(p_svg);
	int source_px = p_source_px;
	if (source_px == 0) {
		Ref<Image> probe;
		probe.instantiate();
		if (ImageLoaderSVG::create_image_from_string(probe, svg_string, 1.0f, false, HashMap<Color, Color>()) == OK && probe.is_valid()) {
			source_px = MAX(probe->get_width(), probe->get_height());
		}
	}
	if (source_px > 0) {
		Ref<Image> image;
		image.instantiate();
		if (ImageLoaderSVG::create_image_from_string(image, svg_string, float(p_size_px) / source_px, false, HashMap<Color, Color>()) == OK && image.is_valid() && !image->is_empty()) {
			texture = ImageTexture::create_from_image(image);
		}
	}
#endif
	g_solers_icon_cache.insert(p_cache_key, texture);
	return texture;
}

Ref<Texture2D> SolersIcons::get(const StringName &p_name, int p_size_px) {
	const int size_px = MAX(2, p_size_px);
	const String id = String(p_name);
	return _solers_raster_svg(_solers_find_svg(SOLERS_UI_ICONS, SOLERS_UI_ICON_COUNT, id), "icon:" + id + "@" + itos(size_px), size_px, 24);
}

Ref<Texture2D> SolersIcons::provider_logo(const String &p_catalog_id, int p_size_px) {
	const int size_px = MAX(2, p_size_px);
	const String id = p_catalog_id.strip_edges().to_lower();

	// Mono track is baked white at build time; callers tint via modulate.
	const char *svg = _solers_find_svg(SOLERS_PROVIDER_LOGOS, SOLERS_PROVIDER_LOGO_COUNT, id);
	String cache_id = id;
	if (!svg) {
		cache_id = "synthetic";
		svg = _solers_find_svg(SOLERS_PROVIDER_LOGOS, SOLERS_PROVIDER_LOGO_COUNT, cache_id);
	}
	return _solers_raster_svg(svg, "logo:" + cache_id + "@" + itos(size_px), size_px);
}

Ref<Texture2D> SolersIcons::provider_logo_color(const String &p_catalog_id, int p_size_px) {
	const int size_px = MAX(2, p_size_px);
	const String id = p_catalog_id.strip_edges().to_lower();
	const char *svg = _solers_find_svg(SOLERS_PROVIDER_COLOR_LOGOS, SOLERS_PROVIDER_COLOR_LOGO_COUNT, id);
	if (!svg) {
		return Ref<Texture2D>();
	}
	// Color track preserves official fills — do not theme-tint at draw time.
	return _solers_raster_svg(svg, "logo-color:" + id + "@" + itos(size_px), size_px);
}

Ref<Texture2D> SolersIcons::brand_mark() {
	if (const Ref<Texture2D> *found = g_solers_icon_cache.getptr("brand")) {
		return *found;
	}
	const Ref<Texture2D> texture = ImageTexture::create_from_image(memnew(Image(app_icon_png)));
	g_solers_icon_cache.insert("brand", texture);
	return texture;
}

SolersActivityIndicator::SolersActivityIndicator() {
	set_mouse_filter(MOUSE_FILTER_IGNORE);
	set_process_internal(true);
}

void SolersActivityIndicator::_notification(int p_what) {
	if (p_what == NOTIFICATION_INTERNAL_PROCESS) {
		if (is_visible_in_tree()) {
			queue_redraw();
		}
	} else if (p_what == NOTIFICATION_DRAW) {
		const float phase = Math::fposmod(Time::get_singleton()->get_ticks_msec() / 1000.0f, 1.4f);
		const float spacing = MIN(get_size().x, get_size().y) / 5.5f;
		const Vector2 origin = (get_size() - Vector2(spacing * 4.0f, spacing * 4.0f)) * 0.5f;
		for (int y = 0; y < 5; y++) {
			for (int x = 0; x < 5; x++) {
				const float wave = Math::fposmod(phase - x * 0.12f - y * 0.17f, 1.4f);
				const float pulse = Math::pow(MAX(0.0f, 1.0f - wave / 0.35f), 2.0f);
				draw_circle(origin + Vector2(x, y) * spacing, MAX(1.0f, spacing * 0.19f), Color(1, 1, 1, 0.08f + pulse * 0.92f));
			}
		}
	}
}

void SolersIcons::clear_cache() {
	g_solers_icon_cache.clear();
}

/* ------------------------------------------------------------------ */
/* Shared palette                                                      */
/* ------------------------------------------------------------------ */

// Quiet Codex-style palette: low chrome at rest, one strong send action.
static const Color SOLERS_GLYPH_IDLE = Color(0.64, 0.65, 0.69);
static const Color SOLERS_GLYPH_HOVER = Color(0.94, 0.95, 0.97);
static const Color SOLERS_TEXT_STRONG = Color(0.90, 0.91, 0.94);
static const Color SOLERS_TEXT_MUTED = Color(0.57, 0.58, 0.62);
static const SolersUITheme::Tokens SOLERS_UI_TOKENS = SolersUITheme::make_tokens();

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
					fill = pressing ? SOLERS_UI_TOKENS.primary_pressed : SOLERS_UI_TOKENS.primary.lerp(SOLERS_UI_TOKENS.primary_hover, anim);
					glyph_color = SOLERS_UI_TOKENS.on_primary;
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

			Ref<Texture2D> tex = SolersIcons::get(glyph, int(Math::round(glyph_px * ed)));
			if (tex.is_valid()) {
				const Point2 pos = (r.size - Size2(tex->get_size())) * 0.5f;
				draw_texture(tex, pos.floor(), glyph_color);
			}
		} break;
	}
}

SolersContextRing::SolersContextRing() {
	set_custom_minimum_size(Size2(28, 28) * EDSCALE);
	set_v_size_flags(SIZE_SHRINK_CENTER);
	set_focus_mode(FOCUS_NONE);
	set_usage(Dictionary());
}

String SolersContextRing::_format_tokens(int64_t p_tokens) {
	const int64_t tokens = MAX((int64_t)0, p_tokens);
	if (tokens >= 1000000) {
		return String::num_int64((tokens + 500000) / 1000000) + "M";
	}
	if (tokens >= 1000) {
		return String::num_int64((tokens + 500) / 1000) + "K";
	}
	return String::num_int64(tokens);
}

void SolersContextRing::set_usage(const Dictionary &p_usage) {
	used_tokens = MAX((int64_t)0, (int64_t)p_usage.get("used_tokens", 0));
	total_tokens = MAX((int64_t)0, (int64_t)p_usage.get("context_window", 0));
	const bool known = p_usage.get("known", false);
	usage_ratio = known && total_tokens > 0 ? CLAMP(float(used_tokens) / float(total_tokens), 0.0f, 1.0f) : -1.0f;
	const int64_t last_input = MAX((int64_t)0, (int64_t)p_usage.get("last_input_tokens", 0));
	const int64_t last_cache_read = MAX((int64_t)0, (int64_t)p_usage.get("last_cache_read_tokens", 0));
	const int64_t last_cache_write = MAX((int64_t)0, (int64_t)p_usage.get("last_cache_write_tokens", 0));
	const double cache_hit = last_input + last_cache_read + last_cache_write > 0 ? 100.0 * double(last_cache_read) / double(last_input + last_cache_read + last_cache_write) : 0.0;
	String details = vformat(String::utf8("\u2191%s \u2193%s R%s W%s CH%s%% $%s"), _format_tokens(p_usage.get("input_tokens", 0)), _format_tokens(p_usage.get("output_tokens", 0)), _format_tokens(p_usage.get("cache_read_tokens", 0)), _format_tokens(p_usage.get("cache_write_tokens", 0)), String::num(cache_hit, 1), String::num((double)p_usage.get("cost", 0.0), 3));
	details += known && total_tokens > 0 ? vformat("\n%s%%/%s (auto)", String::num(usage_ratio * 100.0, 1), _format_tokens(total_tokens)) : "\n?/" + (total_tokens > 0 ? _format_tokens(total_tokens) : String("?")) + " (auto)";
	set_tooltip_text(details);
	queue_redraw();
}

void SolersContextRing::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_MOUSE_ENTER:
			hovering = true;
			queue_redraw();
			break;
		case NOTIFICATION_MOUSE_EXIT:
			hovering = false;
			queue_redraw();
			break;
		case NOTIFICATION_DRAW: {
			const Point2 center = get_size() * 0.5f;
			const float radius = 7.5f * EDSCALE;
			const float width = 1.7f * EDSCALE;
			Color track = SOLERS_GLYPH_IDLE;
			track.a = 0.24f;
			draw_arc(center, radius, -Math::PI * 0.5f, Math::PI * 1.5f, 40, track, width, true);
			if (usage_ratio > 0.0f) {
				const Color progress = hovering ? SOLERS_GLYPH_HOVER : SOLERS_GLYPH_IDLE;
				draw_arc(center, radius, -Math::PI * 0.5f, -Math::PI * 0.5f + Math::TAU * usage_ratio, 40, progress, width, true);
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

Size2 SolersSelectChip::get_minimum_size() const {
	const float ed = EDSCALE;
	const Ref<Font> font = get_theme_default_font();
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

			const Color strong_color = SOLERS_TEXT_STRONG.lerp(Color(1, 1, 1), anim);
			const Color muted_color = SOLERS_TEXT_MUTED.lerp(Color(1, 1, 1), 0.42f * anim);
			const Color chevron_color = Color(0.50f, 0.51f, 0.55f).lerp(Color(1, 1, 1), 0.45f * anim);

			const Ref<Font> font = get_theme_default_font();
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
				Ref<Texture2D> icon = SolersIcons::get(glyph, int(Math::round(icon_slot)));
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
				Ref<Texture2D> chevron = SolersIcons::get(SNAME("chevron_down"), int(Math::round(9.0f * ed)));
				if (chevron.is_valid()) {
					const Point2 pos(x, (r.size.y - chevron->get_height()) * 0.5f);
					draw_texture(chevron, pos.round(), chevron_color);
				}
			}
		} break;
	}
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
		case NOTIFICATION_MOUSE_ENTER:
		case NOTIFICATION_MOUSE_EXIT: {
			hovered = p_what == NOTIFICATION_MOUSE_ENTER;
			if (hover_accent) {
				queue_redraw();
			}
		} break;
		case NOTIFICATION_DRAW: {
			const float ed = EDSCALE;
			const Rect2 r(Point2(), get_size());
			const float rad = radius * ed;
			const bool accent = hover_accent && hovered && EditorNode::get_singleton();
			const Color draw_border = accent ? EditorNode::get_singleton()->get_editor_theme()->get_color(SNAME("accent_color"), EditorStringName(Editor)) : border_opaque;
			const float bw = MAX(1.0f, border_w * ed * (accent ? 2.0f : 1.0f));

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

			if (dashed_border && has_border) {
				sb->set_bg_color(bg);
				sb->set_corner_radius_all(int(rad));
				draw_style_box(sb, r);
				const float inset = bw * 0.5f;
				const float dash = 5.0f * ed;
				draw_dashed_line(Point2(rad, inset), Point2(r.size.x - rad, inset), draw_border, bw, dash);
				draw_dashed_line(Point2(rad, r.size.y - inset), Point2(r.size.x - rad, r.size.y - inset), draw_border, bw, dash);
				draw_dashed_line(Point2(inset, rad), Point2(inset, r.size.y - rad), draw_border, bw, dash);
				draw_dashed_line(Point2(r.size.x - inset, rad), Point2(r.size.x - inset, r.size.y - rad), draw_border, bw, dash);
			} else if (has_border) {
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

void solers_configure_prompt_surface(SolersSurface *p_surface) {
	ERR_FAIL_NULL(p_surface);
	p_surface->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	p_surface->configure(solers_composer_bg(), solers_composer_border(), 19, 14, true);
	p_surface->add_theme_constant_override("margin_bottom", 7 * EDSCALE);
}

void solers_configure_prompt_text_edit(TextEdit *p_edit) {
	ERR_FAIL_NULL(p_edit);
	p_edit->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	p_edit->set_custom_maximum_size(Size2(-1, 220.0f * EDSCALE));
	p_edit->set_line_wrapping_mode(TextEdit::LINE_WRAPPING_BOUNDARY);
	p_edit->set_smooth_scroll_enabled(true);
	p_edit->set_scroll_past_end_of_file_enabled(false);
	p_edit->set_fit_content_height_enabled(true);
	p_edit->set_indent_wrapped_lines(false);
	p_edit->set_highlight_current_line(false);
	p_edit->set_draw_minimap(false);
	p_edit->set_caret_blink_enabled(true);
	p_edit->add_theme_style_override("normal", memnew(StyleBoxEmpty));
	p_edit->add_theme_style_override("focus", memnew(StyleBoxEmpty));
	p_edit->add_theme_style_override("read_only", memnew(StyleBoxEmpty));
	p_edit->add_theme_color_override("font_color", Color(0.961, 0.969, 0.984));
	p_edit->add_theme_color_override("font_placeholder_color", Color(0.56, 0.57, 0.60));
	p_edit->add_theme_color_override("background_color", Color(0, 0, 0, 0));
	p_edit->add_theme_color_override("caret_color", Color(0.86, 0.91, 0.98, 1));
	p_edit->add_theme_color_override("selection_color", Color(0.10, 0.42, 0.62, 0.56));
	p_edit->add_theme_constant_override("line_spacing", 4 * EDSCALE);
	p_edit->add_theme_font_size_override(SceneStringName(font_size), 14 * EDSCALE);
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

void solers_draw_mention_chip(RID p_ci, const Rect2 &p_pill, const String &p_label, const Ref<Font> &p_font, int p_font_size, const Ref<Texture2D> &p_icon, const Color &p_text_color) {
	if (p_label.is_empty()) {
		return;
	}
	const float pad_x = 5.0f * EDSCALE;
	const float gap = 4.0f * EDSCALE;
	const int icon_px = int(Math::round(13.0f * EDSCALE));

	float x = p_pill.position.x + pad_x;
	const float mid_y = p_pill.position.y + p_pill.size.y * 0.5f;
	if (p_icon.is_valid()) {
		const Rect2 icon_rect(x, mid_y - icon_px * 0.5f, icon_px, icon_px);
		p_icon->draw_rect(p_ci, icon_rect, false);
		x += icon_px + gap;
	}
	if (p_font.is_valid()) {
		const float text_y = p_pill.position.y + (p_pill.size.y - p_font->get_height(p_font_size)) * 0.5f + p_font->get_ascent(p_font_size);
		p_font->draw_string(p_ci, Point2(x, text_y), p_label, HORIZONTAL_ALIGNMENT_LEFT, -1, p_font_size, p_text_color);
	}
}

Ref<Texture2D> solers_mention_chip_icon(const Dictionary &p_mention, int p_px) {
	const String source = String(p_mention.get("source", "plugin")).strip_edges().to_lower();
	if (source == "plugin") {
		const String id = String(p_mention.get("id", String())).strip_edges().to_lower();
		const Ref<Texture2D> color = SolersIcons::provider_logo_color(id, p_px);
		return color.is_valid() ? color : SolersIcons::provider_logo(id, p_px);
	}
	if (source == "addon") {
		return SolersIcons::get(SNAME("plugin"), p_px);
	}
	if (source == "folder") {
		return SolersIcons::get(SNAME("folder"), p_px);
	}
	if (source == "node") {
		return SolersIcons::get(SNAME("node"), p_px);
	}
	return SolersIcons::get(SNAME("file"), p_px);
}

Ref<Texture2D> solers_attachment_texture(const Dictionary &p_attachment) {
	const String sha256 = String(p_attachment.get("content_sha256", String())).strip_edges();
	if (sha256.length() != 64 || !sha256.is_valid_hex_number(false)) {
		return Ref<Texture2D>();
	}
	const String path = solers_session_dir().path_join("attachments").path_join(sha256 + ".png");
	if (!FileAccess::exists(path)) {
		return Ref<Texture2D>();
	}
	Ref<Image> image = Image::load_from_file(path);
	if (image.is_null() || image->is_empty()) {
		return Ref<Texture2D>();
	}
	const int max_dimension = MAX(image->get_width(), image->get_height());
	if (max_dimension > 112) {
		const float scale = 112.0f / max_dimension;
		image->resize(MAX(1, int(image->get_width() * scale)), MAX(1, int(image->get_height() * scale)), Image::INTERPOLATE_LANCZOS);
	}
	return ImageTexture::create_from_image(image);
}

void solers_style_bare_search_line_edit(LineEdit *p_edit) {
	ERR_FAIL_NULL(p_edit);
	Ref<StyleBoxEmpty> empty;
	empty.instantiate();
	empty->set_content_margin_individual(4 * EDSCALE, 4 * EDSCALE, 4 * EDSCALE, 4 * EDSCALE);
	p_edit->add_theme_style_override("normal", empty);
	p_edit->add_theme_style_override("focus", empty);
	p_edit->add_theme_style_override("read_only", empty);
	p_edit->add_theme_color_override(SceneStringName(font_color), SOLERS_UI_TOKENS.text);
	p_edit->add_theme_color_override("font_placeholder_color", SOLERS_UI_TOKENS.text_dim);
	p_edit->add_theme_font_size_override(SceneStringName(font_size), int(12 * EDSCALE));
	p_edit->set_custom_minimum_size(Size2(0, 26 * EDSCALE));
}
