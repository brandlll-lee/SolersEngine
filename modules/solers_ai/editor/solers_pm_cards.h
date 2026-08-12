/**************************************************************************/
/*  solers_pm_cards.h                                                     */
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
	bool preserve_icon_color = false; // Official multicolor marks — no gray chrome tint.

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
