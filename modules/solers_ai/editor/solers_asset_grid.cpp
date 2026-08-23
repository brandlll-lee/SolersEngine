/**************************************************************************/
/*  solers_asset_grid.cpp                                                 */
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

#include "solers_asset_grid.h"

#include "solers_chat_widgets.h"

#include "core/object/callable_mp.h"
#include "core/string/translation.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/button.h"
#include "scene/gui/center_container.h"
#include "scene/gui/grid_container.h"
#include "scene/gui/scroll_bar.h"
#include "scene/gui/texture_rect.h"

static void _sync_asset_menu(Button *p_card, Button *p_menu) {
	p_menu->set_visible(p_card->is_hovered() || p_card->has_focus() || p_menu->has_focus());
}

SolersAssetGrid::SolersAssetGrid() {
	set_horizontal_scroll_mode(SCROLL_MODE_DISABLED);
	set_vertical_scroll_mode(SCROLL_MODE_AUTO);
	get_v_scroll_bar()->set_theme_type_variation(SNAME("SolersStudioScroll"));
	grid = memnew(GridContainer);
	grid->set_h_size_flags(SIZE_EXPAND_FILL);
	grid->add_theme_constant_override(SNAME("h_separation"), int(10 * EDSCALE));
	grid->add_theme_constant_override(SNAME("v_separation"), int(10 * EDSCALE));
	add_child(grid);
}

void SolersAssetGrid::_notification(int p_what) {
	if (p_what == NOTIFICATION_RESIZED) {
		_update_columns();
	}
}

void SolersAssetGrid::_update_columns() {
	const float card_width = 108 * EDSCALE;
	const float separation = 10 * EDSCALE;
	grid->set_columns(MAX(1, int((get_size().x + separation) / (card_width + separation))));
}

void SolersAssetGrid::set_callbacks(const Callable &p_selected, const Callable &p_menu) {
	selected_callback = p_selected;
	menu_callback = p_menu;
}

void SolersAssetGrid::clear_assets() {
	while (grid->get_child_count() > 0) {
		Node *child = grid->get_child(0);
		grid->remove_child(child);
		child->queue_free();
	}
	asset_count = 0;
}

void SolersAssetGrid::add_asset(const Dictionary &p_manifest, const Ref<Texture2D> &p_preview) {
	const String asset_id = p_manifest.get("id", String());
	Button *card = memnew(Button);
	card->set_meta(SNAME("asset_id"), asset_id);
	card->set_name("AssetCard");
	card->set_toggle_mode(true);
	card->set_pressed_no_signal(asset_id == selected_id);
	card->set_custom_minimum_size(Size2(108, 108) * EDSCALE);
	card->set_h_size_flags(SIZE_EXPAND_FILL);
	card->set_clip_contents(true);
	card->set_theme_type_variation(SNAME("SolersAssetCard"));
	card->set_tooltip_text(String(p_manifest.get("name", asset_id)) + "\n" + String(p_manifest.get("status", String())).capitalize());
	card->connect(SceneStringName(pressed), callable_mp(this, &SolersAssetGrid::_asset_pressed).bind(asset_id));
	grid->add_child(card);

	const String status = String(p_manifest.get("status", String())).to_lower();
	if (status == "queued" || status == "running") {
		CenterContainer *center = memnew(CenterContainer);
		center->set_anchors_and_offsets_preset(PRESET_FULL_RECT);
		center->set_mouse_filter(MOUSE_FILTER_IGNORE);
		SolersActivityIndicator *activity = memnew(SolersActivityIndicator);
		activity->set_name("ActivityIndicator");
		activity->set_custom_minimum_size(Size2(32, 32) * EDSCALE);
		center->add_child(activity);
		card->add_child(center);
	} else {
		TextureRect *image = memnew(TextureRect);
		image->set_anchors_and_offsets_preset(PRESET_FULL_RECT);
		image->set_offsets_preset(PRESET_FULL_RECT, PRESET_MODE_MINSIZE, 2 * EDSCALE);
		image->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
		image->set_stretch_mode(TextureRect::STRETCH_KEEP_ASPECT_COVERED);
		image->set_texture(p_preview);
		image->set_mouse_filter(MOUSE_FILTER_IGNORE);
		card->add_child(image);
	}

	Button *more = memnew(Button);
	more->set_name("AssetMenuButton");
	more->set_button_icon(SolersIcons::get(SNAME("more"), int(16 * EDSCALE)));
	more->set_custom_minimum_size(Size2(28, 28) * EDSCALE);
	more->set_theme_type_variation(SNAME("SolersStudioActionButton"));
	more->set_tooltip_text(TTRC("Asset actions"));
	more->hide();
	card->add_child(more);
	more->set_anchors_and_offsets_preset(PRESET_BOTTOM_RIGHT, PRESET_MODE_MINSIZE, int(4 * EDSCALE));
	more->set_grow_direction_preset(PRESET_BOTTOM_RIGHT);
	more->connect(SceneStringName(pressed), callable_mp(this, &SolersAssetGrid::_asset_menu_pressed).bind(asset_id, more));
	const Callable sync_menu = callable_mp_static(_sync_asset_menu).bind(card, more);
	for (const StringName &signal : { SceneStringName(mouse_entered), SceneStringName(mouse_exited), SceneStringName(focus_entered), SceneStringName(focus_exited) }) {
		card->connect(signal, sync_menu);
		more->connect(signal, sync_menu);
	}
	asset_count++;
}

void SolersAssetGrid::_asset_pressed(const String &p_asset_id) {
	set_selected_asset(p_asset_id);
	if (selected_callback.is_valid()) {
		selected_callback.call(p_asset_id);
	}
}

void SolersAssetGrid::_asset_menu_pressed(const String &p_asset_id, Control *p_anchor) {
	set_selected_asset(p_asset_id);
	if (menu_callback.is_valid()) {
		menu_callback.call(p_asset_id, p_anchor);
	}
}

void SolersAssetGrid::set_selected_asset(const String &p_asset_id) {
	selected_id = p_asset_id;
	for (int i = 0; i < grid->get_child_count(); i++) {
		Button *card = Object::cast_to<Button>(grid->get_child(i));
		if (card) {
			card->set_pressed_no_signal(card->get_meta(SNAME("asset_id"), String()) == selected_id);
		}
	}
}
