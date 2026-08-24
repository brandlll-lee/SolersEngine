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

#include "core/input/input_event.h"
#include "core/object/callable_mp.h"
#include "core/object/object.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/panel_container.h"
#include "scene/gui/scroll_container.h"
#include "scene/resources/style_box.h"
#include "servers/text/text_server.h"

SolersPopupList::SolersPopupList() {
	set_anchors_and_offsets_preset(PRESET_FULL_RECT);
	set_mouse_filter(MOUSE_FILTER_STOP);
	set_as_top_level(true);
	set_z_index(100);
	set_process_unhandled_key_input(true);
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

	scroll = memnew(ScrollContainer);
	scroll->set_horizontal_scroll_mode(ScrollContainer::SCROLL_MODE_SHOW_NEVER);
	scroll->set_vertical_scroll_mode(ScrollContainer::SCROLL_MODE_AUTO);
	scroll->set_follow_focus(true);
	panel->add_child(scroll);

	rows = memnew(VBoxContainer);
	rows->set_h_size_flags(SIZE_EXPAND_FILL);
	rows->add_theme_constant_override("separation", int(4 * EDSCALE));
	scroll->add_child(rows);
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

void SolersPopupList::unhandled_key_input(const Ref<InputEvent> &p_event) {
	const Ref<InputEventKey> key = p_event;
	if (is_visible() && key.is_valid() && key->is_pressed() && !key->is_echo() && key->get_keycode() == Key::ESCAPE) {
		close();
		get_viewport()->set_input_as_handled();
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
	anchor_id = p_anchor->get_instance_id();
	Button *focus_row = nullptr;

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
			focus_row = row;
		}
		if (!focus_row && !row->is_disabled()) {
			focus_row = row;
		}
		row->connect(SceneStringName(pressed), callable_mp(this, &SolersPopupList::_item_pressed).bind(id));
		rows->add_child(row);
	}

	const float gap = 4 * EDSCALE;
	const Control *bounds_control = get_parent_control();
	const Vector2 bounds_origin = bounds_control ? bounds_control->get_global_position() - get_global_position() : Vector2();
	const Vector2 bounds_size = bounds_control ? bounds_control->get_size() : get_size();
	const Vector2 anchor_position = p_anchor->get_global_position() - get_global_position();
	const float width = MIN(MAX(p_minimum_width, p_anchor->get_size().x), MAX(bounds_size.x - gap * 2, 0.0f));
	const float natural_height = rows->get_combined_minimum_size().y + panel->get_theme_stylebox(SceneStringName(panel))->get_minimum_size().y;
	const float available_below = MAX(bounds_origin.y + bounds_size.y - anchor_position.y - p_anchor->get_size().y - gap, 0.0f);
	const float available_above = MAX(anchor_position.y - bounds_origin.y - gap, 0.0f);
	const bool open_below = natural_height <= available_below || available_below >= available_above;
	const float height = MIN(natural_height, open_below ? available_below : available_above);
	const float x = CLAMP(anchor_position.x, bounds_origin.x + gap, MAX(bounds_origin.x + gap, bounds_origin.x + bounds_size.x - width - gap));
	const float y = open_below ? anchor_position.y + p_anchor->get_size().y + gap : anchor_position.y - height - gap;
	panel->set_position(Vector2(x, y));
	panel->set_size(Size2(width, height));
	panel->set_custom_minimum_size(Size2(width, 0));
	show();
	if (focus_row) {
		focus_row->grab_focus();
	}
}

void SolersPopupList::close() {
	selected_callback = Callable();
	hide();
	Control *anchor = Object::cast_to<Control>(ObjectDB::get_instance(anchor_id));
	anchor_id = ObjectID();
	if (anchor && anchor->is_inside_tree()) {
		anchor->grab_focus();
	}
}

void SolersStudioSelect::pressed() {
	if (!popup_list) {
		show_popup();
		return;
	}
	Array items;
	for (int i = 0; i < get_item_count(); i++) {
		Dictionary item;
		item["id"] = itos(i);
		item["label"] = get_item_text(i);
		item["description"] = get_item_tooltip(i);
		item["icon"] = get_item_icon(i);
		item["disabled"] = is_item_disabled(i);
		items.push_back(item);
	}
	popup_list->popup(this, items, get_selected() < 0 ? String() : itos(get_selected()), callable_mp(this, &SolersStudioSelect::_popup_selected));
}

void SolersStudioSelect::_popup_selected(const String &p_index) {
	const int index = p_index.to_int();
	ERR_FAIL_INDEX(index, get_item_count());
	const bool changed = index != get_selected();
	select(index);
	if (changed || get_allow_reselect()) {
		emit_signal(SceneStringName(item_selected), index);
	}
}
