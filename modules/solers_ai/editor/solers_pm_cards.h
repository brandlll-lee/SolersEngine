/**************************************************************************/
/*  solers_pm_cards.h                                                     */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                              */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/
/* Solers: AI-native game engine.                                        */
/*                                                                        */
/* Self-drawn nav / list row for Project Manager and Provider Settings.  */
/* Quiet ink wash on hover/selection (Cursor-flat), optional subtitle.   */
/**************************************************************************/

#pragma once

#include "core/variant/callable.h"
#include "scene/gui/control.h"

class Texture2D;

class SolersCategoryCard : public Control {
	GDCLASS(SolersCategoryCard, Control);

	String title;
	String subtitle;
	Ref<Texture2D> icon;
	Color status_dot = Color(0, 0, 0, 0); // Trailing readiness light; transparent = none.
	bool preserve_icon_color = false; // Official multicolor marks — no grey chrome tint.

	bool selected = false;
	bool filled = false;
	bool hovering = false;
	bool pressing = false;

	float anim = 0.0f; // Highlight blend [0..1] (hover/selection).
	float anim_target = 0.0f;

	Callable pressed_callback;

	void _update_anim_target();
	void _sync_min_height();

protected:
	void _notification(int p_what);
	static void _bind_methods() {}

public:
	virtual void gui_input(const Ref<InputEvent> &p_event) override;

	void configure(const String &p_title, const Ref<Texture2D> &p_icon, const String &p_subtitle = String(), const Color &p_status_dot = Color(0, 0, 0, 0));
	void set_icon(const Ref<Texture2D> &p_icon);
	void set_preserve_icon_color(bool p_preserve);
	void set_filled(bool p_filled);
	void set_selected(bool p_selected);
	bool is_selected() const { return selected; }
	void set_pressed_callback(const Callable &p_cb) { pressed_callback = p_cb; }

	SolersCategoryCard();
};
