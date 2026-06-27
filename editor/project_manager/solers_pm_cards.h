/**************************************************************************/
/*  solers_pm_cards.h                                                     */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                              */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/
/* Solers: AI-native game engine.                                        */
/*                                                                        */
/* Custom-drawn navigation/category card for the Project Manager's left   */
/* rail. This is a fully self-drawn Control (no stock Button/Panel        */
/* skeleton), so it escapes the recognizable Godot look and matches       */
/* Unreal's refined, minimal left-rail tiles: a neutral graphite surface  */
/* that lifts on hover, a left-aligned accent icon glyph, a vertically    */
/* centered title, and a crisp accent fill + edge + bar on selection.     */
/* No saturated color fills (those read as cheap), only restrained tints. */
/*                                                                        */
/* It is event-driven: hover/selection drive a short, bounded internal    */
/* process for a subtle highlight animation, then processing stops — so    */
/* it never adds steady CPU/redraw load to the low-power Project Manager.  */
/**************************************************************************/

#pragma once

#include "core/variant/callable.h"
#include "scene/gui/control.h"

class Texture2D;

class SolersCategoryCard : public Control {
	GDCLASS(SolersCategoryCard, Control);

	String title;
	Ref<Texture2D> icon;
	Color hue = Color(0.10f, 0.45f, 0.95f); // Per-category accent tint (icon only).

	bool selected = false;
	bool filled = false;
	bool hovering = false;
	bool pressing = false;

	float anim = 0.0f; // Highlight blend [0..1] (hover/selection).
	float anim_target = 0.0f;

	Callable pressed_callback;

	void _update_anim_target();

protected:
	void _notification(int p_what);
	static void _bind_methods() {}

public:
	virtual void gui_input(const Ref<InputEvent> &p_event) override;

	void configure(const String &p_title, const Ref<Texture2D> &p_icon, const Color &p_hue);
	void set_icon(const Ref<Texture2D> &p_icon);
	void set_filled(bool p_filled);
	void set_selected(bool p_selected);
	bool is_selected() const { return selected; }
	void set_pressed_callback(const Callable &p_cb) { pressed_callback = p_cb; }

	SolersCategoryCard();
};
