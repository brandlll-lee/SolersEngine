/**************************************************************************/
/*  solers_pm_cards.cpp                                                   */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                              */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/

#include "solers_pm_cards.h"

#include "core/input/input_event.h"
#include "core/math/math_funcs.h"
#include "editor/themes/editor_scale.h"
#include "scene/resources/font.h"
#include "scene/resources/style_box_flat.h"
#include "scene/resources/texture.h"

static const Color SOLERS_TEXT = Color(0.886f, 0.890f, 0.902f); // Primary text.

SolersCategoryCard::SolersCategoryCard() {
	set_custom_minimum_size(Size2(0, 48) * EDSCALE);
	set_mouse_filter(MOUSE_FILTER_STOP);
	set_focus_mode(FOCUS_NONE);
	set_default_cursor_shape(CURSOR_POINTING_HAND);
}

void SolersCategoryCard::_update_anim_target() {
	anim_target = (selected || hovering) ? 1.0f : 0.0f;
	if (!Math::is_equal_approx(anim, anim_target)) {
		set_process_internal(true);
	}
}

void SolersCategoryCard::configure(const String &p_title, const Ref<Texture2D> &p_icon, const Color &p_hue) {
	title = p_title;
	icon = p_icon;
	hue = p_hue;
	queue_redraw();
}

void SolersCategoryCard::set_icon(const Ref<Texture2D> &p_icon) {
	icon = p_icon;
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
				set_process_internal(false); // Bounded: stop once settled.
			}
			queue_redraw();
		} break;

		case NOTIFICATION_DRAW: {
			const Rect2 r(Point2(), get_size());
			const float ed = EDSCALE;

			if (filled || selected || anim > 0.005f) {
				Ref<StyleBoxFlat> pill;
				pill.instantiate();
				pill->set_bg_color(Color(1, 1, 1, (filled || selected) ? 0.070f + 0.020f * anim : 0.045f * anim));
				pill->set_corner_radius_all((int)(14 * ed));
				draw_style_box(pill, Rect2(6 * ed, 3 * ed, r.size.x - 12 * ed, r.size.y - 6 * ed));
			}

			const float pad_l = 18 * ed;
			const Vector2 isz = Vector2(17, 17) * ed;
			const float icon_x = pad_l;
			const float text_x = icon.is_valid() ? (icon_x + isz.x + 14 * ed) : pad_l;

			if (icon.is_valid()) {
				const Vector2 ipos = Vector2(icon_x, Math::round((r.size.y - isz.y) * 0.5f));
				const Color idle_tint = Color(0.66f, 0.69f, 0.74f, 0.95f);
				const Color icon_col = (filled || selected) ? Color(1, 1, 1) : idle_tint.lerp(Color(1, 1, 1), 0.35f * anim);
				draw_texture_rect(icon, Rect2(ipos, isz), false, icon_col);
			}

			const Ref<Font> font = get_theme_font(SNAME("font"), SNAME("Label"));
			const int fs = MAX(10, (int)(14 * ed));
			if (font.is_valid()) {
				const Color idle_tc = Color(SOLERS_TEXT.r, SOLERS_TEXT.g, SOLERS_TEXT.b, 0.62f);
				const Color tc = (filled || selected) ? Color(1, 1, 1) : idle_tc.lerp(Color(1, 1, 1), 0.25f * anim);
				const float baseline = Math::round((r.size.y + font->get_ascent(fs) - font->get_descent(fs)) * 0.5f);
				draw_string(font, Vector2(text_x, baseline), title, HORIZONTAL_ALIGNMENT_LEFT, r.size.x - text_x - 10 * ed, fs, tc);
			}
		} break;
	}
}
