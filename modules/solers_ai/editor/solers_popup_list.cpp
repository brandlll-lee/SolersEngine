/**************************************************************************/
/*  solers_popup_list.cpp                                                 */
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

#include "solers_popup_list.h"

#include "core/object/callable_mp.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/panel_container.h"
#include "servers/text/text_server.h"

SolersPopupList::SolersPopupList() {
	set_anchors_and_offsets_preset(PRESET_FULL_RECT);
	set_mouse_filter(MOUSE_FILTER_STOP);
	set_as_top_level(true);
	set_z_index(100);
	hide();

	dismiss = memnew(Button);
	dismiss->set_anchors_and_offsets_preset(PRESET_FULL_RECT);
	dismiss->set_flat(true);
	dismiss->set_focus_mode(FOCUS_NONE);
	dismiss->connect(SceneStringName(pressed), callable_mp(this, &SolersPopupList::_dismissed));
	add_child(dismiss);

	panel = memnew(PanelContainer);
	panel->set_theme_type_variation(SNAME("SolersPopupPanel"));
	add_child(panel);

	rows = memnew(VBoxContainer);
	rows->add_theme_constant_override("separation", int(4 * EDSCALE));
	panel->add_child(rows);
}

void SolersPopupList::_dismissed() {
	close();
}

void SolersPopupList::_item_pressed(const String &p_id) {
	const Callable callback = selected_callback;
	close();
	if (callback.is_valid()) {
		callback.call(p_id);
	}
}

void SolersPopupList::popup(Control *p_anchor, const Array &p_items, const String &p_selected_id, const Callable &p_callback, float p_minimum_width) {
	ERR_FAIL_NULL(p_anchor);

	while (rows->get_child_count() > 0) {
		Node *child = rows->get_child(0);
		rows->remove_child(child);
		child->queue_free();
	}
	selected_callback = p_callback;

	for (const Variant &item_variant : p_items) {
		const Dictionary item = item_variant;
		const String id = item.get("id", String());
		String text = item.get("label", id);
		const String description = item.get("description", String());
		if (!description.is_empty()) {
			text += "\n" + description;
		}
		Button *row = memnew(Button);
		row->set_text(text);
		row->set_toggle_mode(true);
		row->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
		row->set_text_alignment(HORIZONTAL_ALIGNMENT_LEFT);
		row->set_custom_minimum_size(Size2(0, (description.is_empty() ? 34 : 48) * EDSCALE));
		row->set_theme_type_variation(item.get("danger", false) ? SNAME("SolersPopupDangerItem") : SNAME("SolersPopupItem"));
		row->set_disabled(item.get("disabled", false));
		if (item.has("icon")) {
			row->set_button_icon(item["icon"]);
		}
		if (id == p_selected_id) {
			row->set_pressed_no_signal(true);
		}
		row->connect(SceneStringName(pressed), callable_mp(this, &SolersPopupList::_item_pressed).bind(id));
		rows->add_child(row);
	}

	const Vector2 anchor_position = p_anchor->get_global_position() - get_global_position();
	const float width = MAX(p_minimum_width, p_anchor->get_size().x);
	panel->set_position(anchor_position + Vector2(0, p_anchor->get_size().y + 4 * EDSCALE));
	panel->set_size(Size2(width, 0));
	panel->set_custom_minimum_size(Size2(width, 0));
	show();
}

void SolersPopupList::close() {
	selected_callback = Callable();
	hide();
}
