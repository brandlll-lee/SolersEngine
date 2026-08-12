/**************************************************************************/
/*  solers_pm_cards.cpp                                                   */
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

#include "solers_pm_cards.h"

#include "core/input/input_event.h"
#include "core/math/math_funcs.h"
#include "editor/themes/editor_scale.h"
#include "scene/resources/font.h"
#include "scene/resources/style_box_flat.h"
#include "scene/resources/texture.h"

static const Color SOLERS_TEXT = Color(0.886f, 0.890f, 0.902f);

SolersCategoryCard::SolersCategoryCard() {
	_sync_min_height();
	set_mouse_filter(MOUSE_FILTER_STOP);
	set_focus_mode(FOCUS_NONE);
	set_default_cursor_shape(CURSOR_POINTING_HAND);
}

void SolersCategoryCard::_sync_min_height() {
	set_custom_minimum_size(Size2(0, subtitle.is_empty() ? 36 : 48) * EDSCALE);
}

void SolersCategoryCard::_update_anim_target() {
	anim_target = (selected || hovering) ? 1.0f : 0.0f;
	if (!Math::is_equal_approx(anim, anim_target)) {
		set_process_internal(true);
	}
}

void SolersCategoryCard::configure(const String &p_title, const Ref<Texture2D> &p_icon, const String &p_subtitle, const Color &p_status_dot) {
	title = p_title;
	subtitle = p_subtitle;
	icon = p_icon;
	status_dot = p_status_dot;
	_sync_min_height();
	queue_redraw();
}

void SolersCategoryCard::set_icon(const Ref<Texture2D> &p_icon) {
	icon = p_icon;
	queue_redraw();
}

void SolersCategoryCard::set_preserve_icon_color(bool p_preserve) {
	if (preserve_icon_color == p_preserve) {
		return;
	}
	preserve_icon_color = p_preserve;
	queue_redraw();
}

void SolersCategoryCard::set_filled(bool p_filled) {
	filled = p_filled;
	queue_redraw();
}

void SolersCategoryCard::set_selected(bool p_selected) {
	if (selected == p_selected) {
		return;
	}
	selected = p_selected;
	_update_anim_target();
	queue_redraw();
}

void SolersCategoryCard::gui_input(const Ref<InputEvent> &p_event) {
	const Ref<InputEventMouseButton> mb = p_event;
	if (mb.is_valid() && mb->get_button_index() == MouseButton::LEFT) {
		if (mb->is_pressed()) {
			pressing = true;
		} else if (pressing) {
			pressing = false;
			if (pressed_callback.is_valid()) {
				pressed_callback.call();
			}
		}
		accept_event();
	}
}

void SolersCategoryCard::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_MOUSE_ENTER: {
			hovering = true;
			_update_anim_target();
			queue_redraw();
		} break;

		case NOTIFICATION_MOUSE_EXIT: {
			hovering = false;
			pressing = false;
			_update_anim_target();
			queue_redraw();
		} break;

		case NOTIFICATION_THEME_CHANGED: {
			queue_redraw();
		} break;

		case NOTIFICATION_INTERNAL_PROCESS: {
			const float dt = get_process_delta_time();
			anim = Math::lerp(anim, anim_target, MIN(1.0f, dt * 14.0f));
			if (Math::abs(anim - anim_target) < 0.01f) {
				anim = anim_target;
				set_process_internal(false);
			}
			queue_redraw();
		} break;

		case NOTIFICATION_DRAW: {
			const Rect2 r(Point2(), get_size());
			const float ed = EDSCALE;

			// Cursor-flat: quiet ink wash, tight radius — not a loud pill.
			if (filled || selected || anim > 0.005f) {
				Ref<StyleBoxFlat> wash;
				wash.instantiate();
				const float a = (filled || selected) ? (0.055f + 0.020f * anim) : (0.035f * anim);
				wash->set_bg_color(Color(1, 1, 1, a));
				wash->set_corner_radius_all((int)(6 * ed));
				draw_style_box(wash, Rect2(4 * ed, 2 * ed, r.size.x - 8 * ed, r.size.y - 4 * ed));
			}

			const float pad_l = 12 * ed;
			const float pad_r = 12 * ed;
			const bool show_dot = status_dot.a > 0.01f;
			const float dot_d = 6.0f * ed;
			const float dot_gap = show_dot ? (dot_d + 10.0f * ed) : 0.0f;
			const Vector2 isz = Vector2(16, 16) * ed;
			const float icon_x = pad_l;
			const float text_x = icon.is_valid() ? (icon_x + isz.x + 10 * ed) : pad_l;
			const float text_w = r.size.x - text_x - pad_r - dot_gap;

			const Ref<Font> font = get_theme_default_font();
			const int fs = MAX(10, (int)(13 * ed));
			const int sub_fs = MAX(9, (int)(11 * ed));
			const bool dual = !subtitle.is_empty() && font.is_valid();

			float content_h = isz.y;
			if (font.is_valid()) {
				content_h = dual ? (font->get_height(fs) + 2.0f * ed + font->get_height(sub_fs)) : font->get_height(fs);
			}
			const float content_top = Math::round((r.size.y - content_h) * 0.5f);

			if (icon.is_valid()) {
				const Vector2 ipos = Vector2(icon_x, Math::round((r.size.y - isz.y) * 0.5f));
				Color icon_col = Color(1, 1, 1);
				if (!preserve_icon_color) {
					const Color idle_tint = Color(0.66f, 0.69f, 0.74f, 0.95f);
					icon_col = (filled || selected) ? Color(1, 1, 1) : idle_tint.lerp(Color(1, 1, 1), 0.35f * anim);
				} else if (!(filled || selected)) {
					icon_col = Color(1, 1, 1, 0.92f + 0.08f * anim);
				}
				draw_texture_rect(icon, Rect2(ipos, isz), false, icon_col);
			}

			if (font.is_valid()) {
				const Color idle_tc = Color(SOLERS_TEXT.r, SOLERS_TEXT.g, SOLERS_TEXT.b, 0.62f);
				const Color tc = (filled || selected) ? Color(1, 1, 1) : idle_tc.lerp(Color(1, 1, 1), 0.25f * anim);
				if (dual) {
					const float title_y = content_top + font->get_ascent(fs);
					draw_string(font, Vector2(text_x, title_y), title, HORIZONTAL_ALIGNMENT_LEFT, text_w, fs, tc);
					const Color sub_tc = Color(SOLERS_TEXT.r, SOLERS_TEXT.g, SOLERS_TEXT.b, 0.42f + 0.08f * anim);
					const float sub_y = content_top + font->get_height(fs) + 2.0f * ed + font->get_ascent(sub_fs);
					draw_string(font, Vector2(text_x, sub_y), subtitle, HORIZONTAL_ALIGNMENT_LEFT, text_w, sub_fs, sub_tc);
				} else {
					const float baseline = Math::round((r.size.y + font->get_ascent(fs) - font->get_descent(fs)) * 0.5f);
					draw_string(font, Vector2(text_x, baseline), title, HORIZONTAL_ALIGNMENT_LEFT, text_w, fs, tc);
				}
			}

			if (show_dot) {
				const Vector2 center(r.size.x - pad_r - dot_d * 0.5f, r.size.y * 0.5f);
				draw_circle(center, dot_d * 0.5f, status_dot);
			}
		} break;
	}
}
