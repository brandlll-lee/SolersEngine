/**************************************************************************/
/*  solers_studio.cpp                                                     */
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

#include "solers_studio.h"

#include "core/input/input_event.h"
#include "core/io/file_access.h"
#include "core/io/image.h"
#include "core/io/image_loader.h"
#include "core/io/resource_loader.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/string/translation_server.h"
#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "editor/inspector/editor_resource_preview.h"
#include "editor/themes/editor_scale.h"
#include "scene/animation/tween.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/center_container.h"
#include "scene/gui/check_button.h"
#include "scene/gui/dialogs.h"
#include "scene/gui/file_dialog.h"
#include "scene/gui/item_list.h"
#include "scene/gui/label.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/option_button.h"
#include "scene/gui/scroll_bar.h"
#include "scene/gui/scroll_container.h"
#include "scene/gui/split_container.h"
#include "scene/gui/tab_container.h"
#include "scene/gui/text_edit.h"
#include "scene/gui/texture_rect.h"
#include "scene/main/window.h"
#include "scene/resources/image_texture.h"
#include "scene/resources/theme.h"
#include "servers/display/display_server.h"

#include "modules/solers_ai/core/solers_asset_service.h"
#include "modules/solers_ai/editor/solers_asset_grid.h"
#include "modules/solers_ai/editor/solers_chat_widgets.h"
#include "modules/solers_ai/editor/solers_dock.h"
#include "modules/solers_ai/editor/solers_model_preview.h"
#include "modules/solers_ai/editor/solers_popup_list.h"
#include "modules/solers_ai/editor/solers_schema_form.h"
#include "modules/solers_ai/editor/solers_ui_theme.h"
#include "modules/solers_ai/plugins/solers_plugin.h"
template <typename T>
static T *_studio_add(Node *p_parent) {
	T *control = memnew(T);
	p_parent->add_child(control);
	return control;
}
static Label *_studio_label(Node *p_parent, const String &p_text, const StringName &p_variation = StringName()) {
	Label *label = memnew(Label);
	label->set_text(p_text);
	if (!p_variation.is_empty()) {
		label->set_theme_type_variation(p_variation);
	}
	p_parent->add_child(label);
	return label;
}
static Control *_studio_empty_state(Node *p_parent, Label **r_label) {
	CenterContainer *empty = _studio_add<CenterContainer>(p_parent);
	empty->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	VBoxContainer *content = _studio_add<VBoxContainer>(empty);
	content->add_theme_constant_override(SNAME("separation"), int(12 * EDSCALE));
	TextureRect *icon = _studio_add<TextureRect>(content);
	icon->set_texture(SolersIcons::get(SNAME("tool_asset"), int(52 * EDSCALE)));
	icon->set_custom_minimum_size(Size2(52, 52) * EDSCALE);
	icon->set_stretch_mode(TextureRect::STRETCH_KEEP_ASPECT_CENTERED);
	*r_label = _studio_label(content, String(), SNAME("SolersSessionMeta"));
	(*r_label)->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	return empty;
}

static Dictionary _studio_schema_subset(const Dictionary &p_schema, const Array &p_include, const Array &p_exclude, const Dictionary &p_constraints, bool p_remaining) {
	const Dictionary properties = p_schema.has("properties") ? Dictionary(p_schema["properties"]) : p_schema;
	Dictionary selected;
	for (const Variant &key : properties.keys()) {
		const bool included = p_include.has(key);
		if (!p_exclude.has(key) && (p_remaining ? !included : included)) {
			Dictionary property = Dictionary(properties[key]).duplicate(true);
			property.merge(p_constraints.get(key, Dictionary()), true);
			selected[key] = property;
		}
	}
	return selected;
}

static int _studio_reference_limit(const Dictionary &p_preset, bool p_multiview) {
	const int maximum = MAX(1, (int)p_preset.get("max_reference_images", 1));
	return p_multiview ? MIN(maximum, 4) : 1;
}

String SolersStudio::_current_route() const {
	return route_list && route_list->get_selected_items().size() > 0 ? String(route_list->get_item_metadata(route_list->get_selected_items()[0])) : String("3d");
}

String SolersStudio::_current_kind() const {
	const String route = _current_route();
	return route == "assets" ? String("3d") : route;
}
String SolersStudio::_selected_provider(const OptionButton *p_options) const {
	return p_options && p_options->get_selected() >= 0 ? String(p_options->get_selected_metadata()) : String();
}
void SolersStudio::_refresh_registry() {
	const String selected_route = _current_route();
	const char *const routes[][3] = {
		{ "assets", "Assets", "layout_grid" }, { "3d", "3D", "tool_export" }, { "image", "Images", "photo_ai" },
		{ "animation", "Animation", "run_sprint" }, { "audio", "Audio", "vinyl" }, { "material", "Materials", "adjustments" }, { "hdri", "HDRI", "cloud" }
	};
	route_list->clear();
	int selected_index = 1;
	for (const auto &route : routes) {
		const int index = route_list->add_item(TTRC(route[1]), SolersIcons::get(StringName(route[2]), int(22 * EDSCALE)));
		route_list->set_item_metadata(index, route[0]);
		if (selected_route == route[0]) {
			selected_index = index;
		}
	}
	route_list->select(selected_index);
	_route_selected(selected_index);
}

void SolersStudio::_refresh_providers() {
	const String kind = _current_kind();
	const String selected_preset_id = selected_preset.get("id", String());
	const String selected_preset_provider = selected_preset.get("provider", String());
	const String selected_catalog_provider = _selected_provider(catalog_provider);
	generation_presets.clear();
	preset_button->clear();
	catalog_provider->clear();
	int selected_preset_index = -1;
	int default_preset = -1;
	for (SolersPlugin *plugin : SolersPluginRegistry::get_plugins()) {
		const Dictionary profile = plugin->get_profile();
		if (!Array(profile.get("kinds", Array())).has(kind)) {
			continue;
		}
		const String id = profile.get("id", String());
		const String label = profile.get("label", id);
		if ((bool)profile.get("supports_generation", false)) {
			Array presets = profile.get("generation_presets", Array());
			if (presets.is_empty()) {
				Dictionary fallback;
				fallback["id"] = id;
				fallback["label"] = label;
				fallback["description"] = profile.get("description", String());
				fallback["kind"] = kind;
				fallback["options"] = Dictionary();
				fallback["max_reference_images"] = 4;
				presets.push_back(fallback);
			}
			for (const Variant &value : presets) {
				Dictionary preset = value;
				if (String(preset.get("kind", kind)).to_lower() != kind) {
					continue;
				}
				preset["provider"] = id;
				const int index = generation_presets.size();
				preset["icon"] = SolersIcons::provider_logo(profile.get("catalog_provider", id), int(18 * EDSCALE));
				generation_presets.push_back(preset);
				preset_button->add_item(preset.get("label", String()));
				preset_button->set_item_icon(index, preset.get("icon", Ref<Texture2D>()));
				preset_button->set_item_tooltip(index, preset.get("description", String()));
				if ((bool)preset.get("default", false)) {
					default_preset = index;
				}
				if (preset.get("id", String()) == selected_preset_id && id == selected_preset_provider) {
					selected_preset_index = index;
				}
			}
		}
		if ((bool)profile.get("supports_catalog", false)) {
			catalog_provider->add_item(label);
			catalog_provider->set_item_metadata(catalog_provider->get_item_count() - 1, id);
			if (id == selected_catalog_provider) {
				catalog_provider->select(catalog_provider->get_item_count() - 1);
			}
		}
	}
	_preset_selected(selected_preset_index >= 0 ? selected_preset_index : (default_preset >= 0 ? default_preset : (generation_presets.is_empty() ? -1 : 0)));
	_catalog_provider_selected(catalog_provider->get_selected());
}

void SolersStudio::_route_selected(int) {
	const String route = _current_route();
	const bool is_3d = route == "3d";
	creation_workspace->set_visible(is_3d);
	catalog_query->set_placeholder(is_3d ? TTRC("Search generated assets...") : TTRC("Search catalog..."));
	library_tabs->set_current_tab(is_3d ? 1 : 0);
	library_tabs->set_tab_hidden(0, is_3d);
	library_tabs->set_tabs_visible(!is_3d);
	_refresh_providers();
	_refresh_project_assets();
	_sync_workspace();
}

void SolersStudio::_preset_selected(int p_index) {
	selected_preset = p_index >= 0 && p_index < generation_presets.size() ? Dictionary(generation_presets[p_index]) : Dictionary();
	preset_button->set_disabled(selected_preset.is_empty());
	preset_button->select(selected_preset.is_empty() ? -1 : p_index);
	preset_description->set_text(selected_preset.get("description", String()));
	const bool supports_multiview = (int)selected_preset.get("max_reference_images", 1) > 1;
	multiview_toggle->set_visible(supports_multiview);
	multiview_toggle->set_pressed_no_signal(supports_multiview);
	_refresh_generation_schema();
	_refresh_reference_slots();
}

void SolersStudio::_refresh_generation_schema() {
	SolersPlugin *plugin = SolersPluginRegistry::get_plugin(selected_preset.get("provider", String()));
	const Dictionary schema = plugin ? plugin->get_generation_options_schema(_current_kind()) : Dictionary();
	const Array featured = selected_preset.get("featured_fields", Array());
	const Array hidden = selected_preset.get("hidden_fields", Array());
	const Dictionary constraints = selected_preset.get("option_constraints", Dictionary());
	const Dictionary presentation = selected_preset.get("presentation", Dictionary());
	featured_form->set_schema(_studio_schema_subset(schema, featured, hidden, constraints, false), Dictionary(), presentation);
	generation_form->set_schema(_studio_schema_subset(schema, featured, hidden, constraints, true), Dictionary(), presentation);
	const Dictionary defaults = selected_preset.get("options", Dictionary());
	featured_form->set_values(defaults);
	generation_form->set_values(defaults);
	const bool enabled = plugin != nullptr;
	generate_button->set_disabled(!enabled);
	empty_generate_button->set_disabled(!enabled);
}
void SolersStudio::_catalog_provider_selected(int) {
	SolersPlugin *plugin = SolersPluginRegistry::get_plugin(_selected_provider(catalog_provider));
	const Dictionary profile = plugin ? plugin->get_profile() : Dictionary();
	attribution_label->set_text(profile.get("attribution", String()));
	catalog_query->set_editable(_current_route() == "3d" || plugin != nullptr);
}
void SolersStudio::_reference_files_selected(const PackedStringArray &p_files) {
	for (const String &path : p_files) {
		_stage_reference_image(path);
		if (reference_attachments.size() + pending_reference_images >= _studio_reference_limit(selected_preset, multiview_toggle->is_pressed())) {
			break;
		}
	}
	_refresh_reference_slots();
}

void SolersStudio::_stage_reference_image(const Variant &p_source) {
	if (reference_attachments.size() + pending_reference_images >= _studio_reference_limit(selected_preset, multiview_toggle->is_pressed())) {
		return;
	}
	pending_reference_images++;
	assets->stage_input_image_async(p_source, callable_mp(this, &SolersStudio::_reference_image_staged).bind(reference_generation));
}

void SolersStudio::_reference_image_staged(const Dictionary &p_result, const Ref<Image> &p_image, uint64_t p_generation) {
	if (p_generation != reference_generation) {
		return;
	}
	pending_reference_images = MAX(0, pending_reference_images - 1);
	if (!(bool)p_result.get("ok", false)) {
		_show_result(p_result, String());
		return;
	}
	if (reference_attachments.size() < _studio_reference_limit(selected_preset, multiview_toggle->is_pressed())) {
		reference_attachments.push_back(p_result.get("data", Dictionary()));
		reference_textures.push_back(ImageTexture::create_from_image(p_image));
	}
	_refresh_reference_slots();
}

void SolersStudio::_refresh_reference_slots() {
	const int limit = _studio_reference_limit(selected_preset, multiview_toggle->is_pressed());
	while (reference_attachments.size() > limit) {
		reference_attachments.pop_back();
		reference_textures.pop_back();
	}
	for (int i = 0; i < 4; i++) {
		const bool slot_visible = i < limit;
		reference_buttons[i]->set_visible(slot_visible);
		if (!slot_visible) {
			continue;
		}
		const Ref<Texture2D> image = i < reference_textures.size() ? Ref<Texture2D>(reference_textures[i]) : Ref<Texture2D>();
		const Ref<Texture2D> placeholder = i == 0 ? Ref<Texture2D>() : SolersIcons::get(SNAME("tool_capture"), int(24 * EDSCALE));
		reference_buttons[i]->set_button_icon(image.is_valid() ? image : placeholder);
		reference_buttons[i]->set_expand_icon(image.is_valid());
	}
	reference_empty_state->set_visible(reference_textures.is_empty());
	reference_aux->set_visible(limit > 1);
	clear_references_button->set_visible(!reference_attachments.is_empty());
}

void SolersStudio::_clear_references() {
	reference_attachments.clear();
	reference_textures.clear();
	pending_reference_images = 0;
	reference_generation++;
	_refresh_reference_slots();
}

void SolersStudio::_multiview_toggled(bool) {
	_refresh_reference_slots();
}

void SolersStudio::_reference_gui_input(const Ref<InputEvent> &p_event, Button *p_button) {
	const Ref<InputEventKey> key = p_event;
	if (!p_button->is_hovered() || !key.is_valid() || !key->is_pressed() || key->is_echo() || key->get_keycode() != Key::V || !key->is_command_or_control_pressed()) {
		return;
	}
	DisplayServer *display = DisplayServer::get_singleton();
	if (display && display->clipboard_has_image()) {
		_stage_reference_image(display->clipboard_get_image());
		accept_event();
	}
}

void SolersStudio::_external_reference_files_dropped(const PackedStringArray &p_files) {
	if (is_visible_in_tree() && _current_route() == "3d") {
		_reference_files_selected(p_files);
	}
}

bool SolersStudio::_can_drop_reference(const Point2 &, const Variant &p_data, Control *) const {
	const Dictionary drag_data = p_data;
	if (String(drag_data.get("type", String())) != "files") {
		return false;
	}
	for (const String &path : PackedStringArray(drag_data.get("files", PackedStringArray()))) {
		if (ImageLoader::recognize(path.get_extension()).is_valid()) {
			return true;
		}
	}
	return false;
}

void SolersStudio::_drop_reference(const Point2 &, const Variant &p_data, Control *) {
	_reference_files_selected(Dictionary(p_data).get("files", PackedStringArray()));
}

void SolersStudio::_options_toggled(bool p_visible) {
	generation_form->set_visible(p_visible);
	options_toggle->set_button_icon(SolersIcons::get(p_visible ? SNAME("chevron_down") : SNAME("chevron_right"), int(14 * EDSCALE)));
}

void SolersStudio::_show_result(const Dictionary &p_result, const String &p_success) {
	if ((bool)p_result.get("ok", false)) {
		asset_status->set_text(p_success);
		const Dictionary result_data = p_result.get("data", Dictionary());
		if (!String(result_data.get("id", String())).is_empty()) {
			_show_manifest(result_data);
		}
		return;
	}
	const Dictionary error = p_result.get("error", Dictionary());
	asset_status->set_text(error.get("message", TTRC("The operation failed.")));
	_sync_workspace();
}
void SolersStudio::_generate_pressed() {
	const String kind = _current_kind();
	const String provider = selected_preset.get("provider", String());
	if (reference_attachments.size() < (int)selected_preset.get("min_reference_images", 0)) {
		asset_status->set_text(TTRC("Add reference images"));
		return;
	}
	if (!assets->is_provider_configured(kind, provider)) {
		dock->open_provider_settings("plugins");
		return;
	}
	Dictionary args;
	args["kind"] = kind;
	args["provider"] = provider;
	args["prompt"] = prompt_edit->get_text();
	Dictionary provider_options = selected_preset.get("options", Dictionary()).duplicate(true);
	provider_options.merge(featured_form->get_values(), true);
	provider_options.merge(generation_form->get_values(), true);
	args["provider_options"] = provider_options;
	if (!reference_attachments.is_empty()) {
		Array ids;
		for (const Variant &value : reference_attachments) {
			ids.push_back(Dictionary(value).get("id", String()));
		}
		args["input_attachments"] = ids;
		args["_attachments"] = reference_attachments;
	}
	_show_result(assets->generate(args), TTRC("Generation queued."));
}
void SolersStudio::_catalog_thread_func(void *p_userdata) {
	SolersStudio *studio = static_cast<SolersStudio *>(p_userdata);
	Dictionary result;
	if (studio->catalog_action == "search") {
		result = studio->assets->catalog_search(studio->catalog_args, &studio->catalog_cancel);
	} else {
		result = studio->assets->catalog_inspect(studio->catalog_args, &studio->catalog_cancel);
	}
	studio->catalog_result = result;
	studio->catalog_result_ready.set();
	if (studio->catalog_action == "search" && (bool)result.get("ok", false)) {
		const Array items = Dictionary(result.get("data", Dictionary())).get("assets", Array());
		const String provider = studio->catalog_args.get("provider", String());
		for (int i = 0; i < items.size() && !studio->catalog_cancel.is_set(); i++) {
			const String url = Dictionary(items[i]).get("preview_url", String());
			if (url.is_empty()) {
				continue;
			}
			const Dictionary preview_result = studio->assets->fetch_provider_preview(provider, url, &studio->catalog_cancel);
			if ((bool)preview_result.get("ok", false)) {
				Dictionary item_preview;
				item_preview["index"] = i;
				item_preview["image"] = preview_result.get("data", Ref<Image>());
				MutexLock lock(studio->catalog_previews_mutex);
				studio->catalog_previews.push_back(item_preview);
			}
		}
	}
	studio->catalog_done.set();
}
void SolersStudio::_start_catalog_work(const String &p_action, const Dictionary &p_args) {
	if (catalog_thread.is_started()) {
		catalog_cancel.set();
		catalog_thread.wait_to_finish();
	}
	catalog_cancel.clear();
	catalog_result_ready.clear();
	catalog_done.clear();
	{
		MutexLock lock(catalog_previews_mutex);
		catalog_previews.clear();
	}
	catalog_action = p_action;
	catalog_args = p_args.duplicate(true);
	asset_status->set_text(p_action == "search" ? TTRC("Searching catalog...") : TTRC("Inspecting asset..."));
	catalog_thread.start(&SolersStudio::_catalog_thread_func, this);
}
void SolersStudio::_catalog_search_pressed() {
	if (_current_route() == "3d") {
		_refresh_project_assets();
		return;
	}
	Dictionary args;
	args["kind"] = _current_kind();
	args["provider"] = _selected_provider(catalog_provider);
	args["query"] = catalog_query->get_text();
	args["limit"] = 30;
	_start_catalog_work("search", args);
}
void SolersStudio::_finish_catalog_work() {
	if (catalog_result_ready.is_set()) {
		catalog_result_ready.clear();
		const Dictionary result = catalog_result;
		if (!(bool)result.get("ok", false)) {
			_show_result(result, String());
		} else {
			const Dictionary result_data = result.get("data", Dictionary());
			if (catalog_action == "search") {
				catalog_list->clear();
				const Ref<Texture2D> placeholder = SolersIcons::get(SNAME("tool_asset"), int(54 * EDSCALE));
				for (const Variant &value : Array(result_data.get("assets", Array()))) {
					const Dictionary item = value;
					const String title = item.get("display_name", item.get("name", item.get("asset_id", String())));
					const int index = catalog_list->add_item(title, placeholder);
					catalog_list->set_item_metadata(index, item);
				}
				_sync_library_empty_states();
				asset_status->set_text(vformat(TTRN("%d catalog asset", "%d catalog assets", catalog_list->get_item_count()), catalog_list->get_item_count()));
			} else {
				capability_data = result_data;
				catalog_variant->clear();
				for (const Variant &value : Array(result_data.get("variants", Array()))) {
					const Dictionary variant = value;
					const String id = variant.get("id", String());
					catalog_variant->add_item(variant.get("label", id));
					catalog_variant->set_item_metadata(catalog_variant->get_item_count() - 1, id);
				}
				asset_status->set_text(TTRC("Choose a variant to add to the project."));
			}
			_sync_workspace();
		}
	}
	Array previews;
	{
		MutexLock lock(catalog_previews_mutex);
		previews = catalog_previews;
		catalog_previews.clear();
	}
	for (const Variant &value : previews) {
		const Dictionary item_preview = value;
		const int index = item_preview.get("index", -1);
		const Ref<Image> image = item_preview.get("image", Ref<Image>());
		if (index >= 0 && index < catalog_list->get_item_count() && image.is_valid() && !image->is_empty()) {
			const Ref<ImageTexture> item_texture = ImageTexture::create_from_image(image);
			catalog_list->set_item_icon(index, item_texture);
			if (catalog_list->is_selected(index)) {
				preview->set_texture(item_texture);
				_sync_workspace();
			}
		}
	}
	if (catalog_done.is_set()) {
		catalog_thread.wait_to_finish();
		catalog_done.clear();
	}
}
void SolersStudio::_sync_library_empty_states() {
	catalog_empty->set_visible(catalog_list->get_item_count() == 0);
	project_empty->set_visible(project_grid->get_asset_count() == 0);
}
void SolersStudio::_catalog_selected(int p_index) {
	if (p_index < 0 || p_index >= catalog_list->get_item_count()) {
		return;
	}
	selected_catalog = catalog_list->get_item_metadata(p_index);
	selected_manifest.clear();
	asset_title->set_text(selected_catalog.get("display_name", selected_catalog.get("name", TTRC("Catalog asset"))));
	asset_status->set_text(selected_catalog.get("description", String()));
	preview->set_texture(catalog_list->get_item_icon(p_index));
	catalog_variant->clear();
	_sync_workspace();
	Dictionary args;
	args["kind"] = _current_kind();
	args["provider"] = _selected_provider(catalog_provider);
	args["asset_id"] = selected_catalog.get("asset_id", selected_catalog.get("id", String()));
	_start_catalog_work("inspect", args);
}
void SolersStudio::_acquire_pressed() {
	if (selected_catalog.is_empty() || catalog_variant->get_selected() < 0) {
		return;
	}
	Dictionary args;
	args["kind"] = _current_kind();
	args["provider"] = _selected_provider(catalog_provider);
	args["asset_id"] = selected_catalog.get("asset_id", selected_catalog.get("id", String()));
	args["variant"] = catalog_variant->get_selected_metadata();
	args["source_version"] = capability_data.get("source_version", String());
	_show_result(assets->catalog_acquire(args, String()), TTRC("Asset acquisition queued."));
}
void SolersStudio::_refresh_project_assets() {
	const String selected_id = selected_manifest.get("id", String());
	const String route = _current_route();
	const String query = catalog_query ? catalog_query->get_text().strip_edges().to_lower() : String();
	project_grid->clear_assets();
	for (const Variant &value : project_manifests) {
		const Dictionary manifest = value;
		if (route != "assets" && String(manifest.get("kind", String())).to_lower() != route) {
			continue;
		}
		const String title = manifest.get("name", manifest.get("id", TTRC("Untitled asset")));
		if (!query.is_empty() && !title.to_lower().contains(query)) {
			continue;
		}
		const String status = String(manifest.get("status", "unknown")).to_lower();
		const bool busy = status == "queued" || status == "running";
		Ref<Texture2D> item_preview = SolersIcons::get(SNAME("tool_asset"), int(54 * EDSCALE));
		const String preview_file = manifest.get("preview_file", String());
		if (!busy && !preview_file.is_empty()) {
			const Ref<Texture2D> *cached = project_previews.getptr(preview_file);
			if (cached) {
				item_preview = *cached;
			} else {
				const Ref<Image> image = Image::load_from_file(preview_file);
				if (image.is_valid() && !image->is_empty()) {
					item_preview = ImageTexture::create_from_image(image);
					project_previews.insert(preview_file, item_preview);
				}
			}
		}
		project_grid->add_asset(manifest, item_preview);
		if (manifest.get("id", String()) == selected_id) {
			project_grid->set_selected_asset(selected_id);
			_show_manifest(manifest);
		}
	}
	_sync_library_empty_states();
}

void SolersStudio::_reload_project_assets() {
	project_manifests = assets->list_assets();
	_refresh_project_assets();
}
void SolersStudio::_creation_scroll_hovered(bool p_hovered) {
	if (!creation_scroll) {
		return;
	}
	VScrollBar *bar = creation_scroll->get_v_scroll_bar();
	if (!bar) {
		return;
	}
	Ref<Tween> tween = Ref<Tween>(create_tween());
	tween->set_trans(Tween::TRANS_QUAD);
	tween->set_ease(Tween::EASE_OUT);
	tween->tween_property(bar, NodePath("self_modulate"), Color(1, 1, 1, p_hovered ? 1.0f : 0.0f), 0.12);
}
void SolersStudio::_project_selected(const String &p_asset_id) {
	for (const Variant &value : project_manifests) {
		const Dictionary manifest = value;
		if (manifest.get("id", String()) == p_asset_id) {
			_show_manifest(manifest);
			return;
		}
	}
}

void SolersStudio::_asset_menu_requested(const String &p_asset_id, Control *p_anchor) {
	_project_selected(p_asset_id);
	Array items;
	Dictionary import_item;
	import_item["id"] = "import";
	import_item["label"] = TTRC("Import to project");
	import_item["icon"] = SolersIcons::get(SNAME("tool_export"), int(16 * EDSCALE));
	items.push_back(import_item);
	Dictionary delete_item;
	delete_item["id"] = "delete";
	delete_item["label"] = TTRC("Delete");
	delete_item["icon"] = SolersIcons::get(SNAME("cross"), int(16 * EDSCALE));
	delete_item["danger"] = true;
	items.push_back(delete_item);
	popup_list->popup(p_anchor, items, String(), callable_mp(this, &SolersStudio::_asset_menu_action), 180 * EDSCALE);
}

void SolersStudio::_asset_menu_action(const String &p_action) {
	if (p_action == "import") {
		_import_pressed();
	} else if (p_action == "delete" && !selected_manifest.is_empty()) {
		pending_delete_asset_id = selected_manifest.get("id", String());
		delete_dialog->set_text(vformat(TTRC("Delete '%s' from the global Studio library? This cannot be undone."), selected_manifest.get("name", pending_delete_asset_id)));
		delete_dialog->popup_centered();
	}
}

void SolersStudio::_delete_asset_confirmed() {
	if (pending_delete_asset_id.is_empty()) {
		return;
	}
	const Dictionary result = assets->delete_asset(pending_delete_asset_id);
	pending_delete_asset_id.clear();
	if (!result.get("ok", false)) {
		_show_result(result, String());
		return;
	}
	selected_manifest.clear();
	loaded_model_path.clear();
	model_preview->clear_model();
	_reload_project_assets();
	_sync_workspace();
}
void SolersStudio::_show_manifest(const Dictionary &p_manifest) {
	selected_manifest = p_manifest;
	selected_catalog.clear();
	asset_title->set_text(p_manifest.get("name", p_manifest.get("id", TTRC("Untitled asset"))));
	const Dictionary error = p_manifest.get("error", Dictionary());
	if (!error.is_empty()) {
		String message = error.get("message", TTRC("The operation failed."));
		const String code = error.get("code", String());
		if (!code.is_empty()) {
			message += "\n" + code;
		}
		asset_status->set_text(message);
	} else {
		String stage = String(p_manifest.get("stage", p_manifest.get("status", String()))).capitalize();
		if (p_manifest.has("progress")) {
			stage += "  " + itos(CLAMP((int)p_manifest["progress"], 0, 100)) + "%";
		}
		asset_status->set_text(stage);
	}
	preview->set_texture(Ref<Texture2D>());
	const String preview_file = p_manifest.get("preview_file", String());
	if (!preview_file.is_empty()) {
		const Ref<Texture2D> *cached = project_previews.getptr(preview_file);
		if (cached) {
			preview->set_texture(*cached);
		}
	}
	const String model_path = assets->resolve_model_file(p_manifest);
	if (model_path != loaded_model_path || (!model_path.is_empty() && !model_preview->has_model())) {
		loaded_model_path.clear();
		model_preview->clear_model();
		if (!model_path.is_empty() && model_preview->load_model(model_path) == OK) {
			loaded_model_path = model_path;
		}
	}
	String project_path;
	for (const Variant &value : Array(p_manifest.get("project_entrypoints", Array()))) {
		project_path = value;
		break;
	}
	if (!project_path.is_empty() && EditorResourcePreview::get_singleton()) {
		preview_generation++;
		EditorResourcePreview::get_singleton()->queue_resource_preview(project_path, callable_mp(this, &SolersStudio::_preview_ready).bind(preview_generation));
	}
	Dictionary capability_args;
	capability_args["asset_id"] = p_manifest.get("id", String());
	const Dictionary result = assets->capabilities(capability_args);
	capability_data = result.get("data", Dictionary());
	remesh_operation.clear();
	for (const Variant &value : Array(capability_data.get("available_operations", Array()))) {
		const Dictionary operation = value;
		if (Array(operation.get("remediates", Array())).has("triangle_budget")) {
			remesh_operation = operation;
			break;
		}
	}
	_sync_workspace();
}

void SolersStudio::_sync_workspace() {
	const String route = _current_route();
	const bool has_manifest = (route == "3d" || route == "assets") && !selected_manifest.is_empty();
	const bool has_catalog = route == "assets" && !selected_catalog.is_empty();
	const bool empty = !has_manifest && !has_catalog;
	const String status = String(selected_manifest.get("status", String())).to_lower();
	const bool busy = has_manifest && (status == "queued" || status == "running");
	empty_stage->set_visible(empty || busy);
	empty_icon->set_visible(!busy);
	empty_activity->set_visible(busy);
	if (empty) {
		empty_icon->set_texture(SolersIcons::get(SNAME("cube_plus"), int(84 * EDSCALE)));
		empty_icon->set_self_modulate(SolersUITheme::make_tokens().text);
		asset_title->set_text(route == "3d" ? TTRC("What will you create today?") : TTRC("This Studio workspace is not available yet."));
		if (asset_status->get_text().is_empty()) {
			asset_status->set_text(route == "3d" ? TTRC("Generate a model or choose one from your library.") : TTRC("Only the 3D workspace is enabled in this release."));
		}
	}
	const bool has_model = has_manifest && !busy && model_preview->has_model();
	model_preview->set_visible(has_model);
	preview->set_visible(route != "3d" && !empty && !busy && !has_model && preview->get_texture().is_valid());
	const int64_t polycount = selected_manifest.get("polycount", 0);
	const int64_t vertex_count = selected_manifest.get("vertex_count", 0);
	geometry_stats->set_text(polycount > 0 || vertex_count > 0 ? vformat(TTRC("%s polygons | %s vertices"), String::num_int64(polycount), String::num_int64(vertex_count)) : String());
	geometry_stats->set_visible(!geometry_stats->get_text().is_empty());
	const bool ready = has_manifest && status == "ready";
	const bool preview_failed = ready && route == "3d" && !has_model;
	if (preview_failed) {
		asset_status->set_text(TTRC("3D preview unavailable."));
	}
	asset_actions->set_visible(ready);
	asset_status->set_visible(empty || !ready || preview_failed);
	remesh_button->set_disabled(remesh_operation.is_empty());
	const bool has_variant = has_catalog && catalog_variant->get_item_count() > 0;
	catalog_variant->set_visible(has_variant);
	acquire_button->set_visible(has_variant);
	empty_generate_button->set_visible(empty && route == "3d");
}

void SolersStudio::_refresh_text() {
	creation_title->set_text(TTRC("Create 3D"));
	prompt_edit->set_placeholder(TTRC("Describe the model to create..."));
	options_toggle->set_text(TTRC("Options"));
	generate_button->set_text(TTRC("Generate model"));
	empty_generate_button->set_text(TTRC("Generate model"));
	animation_button->set_tooltip_text(TTRC("Animation"));
	remesh_button->set_tooltip_text(TTRC("Remesh"));
	import_button->set_tooltip_text(TTRC("Import to project"));
	acquire_button->set_text(TTRC("Add to project"));
	catalog_empty_label->set_text(TTRC("No catalog assets found."));
	project_empty_label->set_text(TTRC("Your generated assets will appear here."));
	catalog_query->set_placeholder(_current_route() == "3d" ? TTRC("Search generated assets...") : TTRC("Search catalog..."));
	library_tabs->set_tab_title(0, TTRC("Catalog"));
	library_tabs->set_tab_title(1, TTRC("My generations"));
	_refresh_reference_slots();
	_sync_workspace();
}

void SolersStudio::_animation_pressed() {
	for (int i = 0; i < route_list->get_item_count(); i++) {
		if (route_list->get_item_metadata(i) == "animation") {
			route_list->select(i);
			_route_selected(i);
			return;
		}
	}
}

void SolersStudio::_remesh_pressed() {
	if (remesh_operation.is_empty()) {
		return;
	}
	remesh_form->set_schema(remesh_operation.get("options_schema", Dictionary()), capability_data, remesh_operation.get("presentation", Dictionary()));
	remesh_dialog->popup_centered(Size2i(420, 360) * EDSCALE);
}

void SolersStudio::_remesh_confirmed() {
	if (selected_manifest.is_empty() || remesh_operation.is_empty()) {
		return;
	}
	Dictionary args;
	args["asset_id"] = selected_manifest.get("id", String());
	args["operation_id"] = remesh_operation.get("operation_id", String());
	args["options"] = remesh_form->get_values();
	_show_result(assets->run_operation(args), TTRC("Asset operation queued."));
}

void SolersStudio::_import_pressed() {
	if (selected_manifest.is_empty()) {
		return;
	}
	import_dialog->popup_file_dialog();
}

void SolersStudio::_import_directory_selected(const String &p_directory) {
	Dictionary args;
	args["asset_id"] = selected_manifest.get("id", String());
	args["target_dir"] = p_directory;
	_show_result(assets->start_project_import(args), TTRC("Project import started."));
}
void SolersStudio::_preview_ready(const String &, const Ref<Texture2D> &p_preview, const Ref<Texture2D> &p_small_preview, uint64_t p_generation) {
	if (p_generation == preview_generation && (p_preview.is_valid() || p_small_preview.is_valid())) {
		preview->set_texture(p_preview.is_valid() ? p_preview : p_small_preview);
		_sync_workspace();
	}
}
void SolersStudio::_notification(int p_what) {
	if (p_what == NOTIFICATION_READY) {
		Window *host_window = get_window();
		if (host_window && !host_window->is_connected(SNAME("files_dropped"), callable_mp(this, &SolersStudio::_external_reference_files_dropped))) {
			host_window->connect(SNAME("files_dropped"), callable_mp(this, &SolersStudio::_external_reference_files_dropped));
		}
	} else if (p_what == NOTIFICATION_PROCESS) {
		if (plugin_revision != SolersPluginRegistry::get_revision()) {
			plugin_revision = SolersPluginRegistry::get_revision();
			_refresh_registry();
		}
		if (asset_revision != assets->get_revision()) {
			asset_revision = assets->get_revision();
			_reload_project_assets();
		}
		_finish_catalog_work();
	} else if (p_what == NOTIFICATION_TRANSLATION_CHANGED) {
		_refresh_registry();
		_refresh_text();
	}
}
SolersStudio::SolersStudio(SolersAssetService *p_assets, SolersDock *p_dock) :
		assets(p_assets), dock(p_dock) {
	set_name("SolersStudio");
	set_process(true);
	const SolersUITheme::Tokens tokens = SolersUITheme::make_tokens();
	List<String> image_extensions;
	ImageLoader::get_recognized_extensions(&image_extensions);
	HBoxContainer *shell = _studio_add<HBoxContainer>(this);
	shell->set_h_size_flags(SIZE_EXPAND_FILL);
	shell->set_v_size_flags(SIZE_EXPAND_FILL);
	shell->add_theme_constant_override(SNAME("separation"), int(10 * EDSCALE));
	SolersSurface *rail_surface = _studio_add<SolersSurface>(shell);
	rail_surface->set_custom_minimum_size(Size2(78 * EDSCALE, 0));
	rail_surface->set_v_size_flags(SIZE_EXPAND_FILL);
	rail_surface->configure(tokens.surface, tokens.border, tokens.radius_home_tile, 5, false);
	route_list = _studio_add<ItemList>(rail_surface);
	route_list->set_name("StudioRail");
	route_list->set_theme_type_variation(SNAME("SolersStudioRail"));
	route_list->set_icon_mode(ItemList::ICON_MODE_TOP);
	route_list->set_fixed_icon_size(Size2i(24, 24) * EDSCALE);
	route_list->set_fixed_column_width(int(68 * EDSCALE));
	route_list->set_max_columns(1);
	route_list->set_max_text_lines(1);
	route_list->set_same_column_width(true);
	route_list->set_v_size_flags(SIZE_EXPAND_FILL);
	HSplitContainer *columns = _studio_add<HSplitContainer>(shell);
	columns->set_h_size_flags(SIZE_EXPAND_FILL);
	columns->set_v_size_flags(SIZE_EXPAND_FILL);
	columns->add_theme_constant_override(SNAME("separation"), int(10 * EDSCALE));
	SolersSurface *creation_surface = _studio_add<SolersSurface>(columns);
	creation_workspace = creation_surface;
	creation_surface->set_h_size_flags(SIZE_EXPAND_FILL);
	creation_surface->set_v_size_flags(SIZE_EXPAND_FILL);
	creation_surface->configure(tokens.surface, tokens.border, tokens.radius_home_tile, 12, false);
	VBoxContainer *creation_frame = _studio_add<VBoxContainer>(creation_surface);
	creation_frame->set_h_size_flags(SIZE_EXPAND_FILL);
	creation_frame->set_v_size_flags(SIZE_EXPAND_FILL);
	creation_frame->add_theme_constant_override(SNAME("separation"), int(12 * EDSCALE));
	creation_scroll = _studio_add<ScrollContainer>(creation_frame);
	creation_scroll->set_name("CreationScroll");
	creation_scroll->set_h_size_flags(SIZE_EXPAND_FILL);
	creation_scroll->set_v_size_flags(SIZE_EXPAND_FILL);
	creation_scroll->set_horizontal_scroll_mode(ScrollContainer::SCROLL_MODE_DISABLED);
	creation_scroll->set_vertical_scroll_mode(ScrollContainer::SCROLL_MODE_AUTO);
	creation_scroll->get_v_scroll_bar()->set_theme_type_variation(SNAME("SolersStudioScroll"));
	creation_scroll->get_v_scroll_bar()->set_self_modulate(Color(1, 1, 1, 0));
	VBoxContainer *creation_column = _studio_add<VBoxContainer>(creation_scroll);
	creation_column->set_h_size_flags(SIZE_EXPAND_FILL);
	creation_column->add_theme_constant_override(SNAME("separation"), int(12 * EDSCALE));
	creation_title = _studio_label(creation_column, String(), SNAME("SolersSessionTitle"));
	preset_button = _studio_add<SolersStudioSelect>(creation_column);
	preset_button->set_custom_minimum_size(Size2(0, 52 * EDSCALE));
	preset_button->set_text_alignment(HORIZONTAL_ALIGNMENT_LEFT);
	preset_button->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	preset_description = _studio_label(creation_column, String(), SNAME("SolersSessionMeta"));
	preset_description->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	HBoxContainer *reference_header = _studio_add<HBoxContainer>(creation_column);
	Label *reference_title = _studio_label(reference_header, TTRC("Reference images"), SNAME("SolersSessionTitle"));
	reference_title->set_h_size_flags(SIZE_EXPAND_FILL);
	clear_references_button = _studio_add<Button>(reference_header);
	clear_references_button->set_flat(true);
	clear_references_button->set_button_icon(SolersIcons::get(SNAME("cross"), int(14 * EDSCALE)));
	SolersSurface *reference_surface = _studio_add<SolersSurface>(creation_column);
	reference_surface->set_custom_minimum_size(Size2(0, 210 * EDSCALE));
	reference_surface->configure(tokens.card, tokens.border, tokens.radius_home_tile, 8, false);
	VBoxContainer *reference_column = _studio_add<VBoxContainer>(reference_surface);
	reference_column->set_h_size_flags(SIZE_EXPAND_FILL);
	reference_column->set_v_size_flags(SIZE_EXPAND_FILL);
	reference_column->add_theme_constant_override(SNAME("separation"), int(6 * EDSCALE));
	SolersSurface *reference_well = _studio_add<SolersSurface>(reference_column);
	reference_well->set_v_size_flags(SIZE_EXPAND_FILL);
	reference_well->configure(tokens.card, tokens.border, tokens.radius_list_thumb, 0, false);
	reference_well->set_dashed_border(true);
	reference_well->set_hover_accent(true);
	reference_buttons[0] = _studio_add<Button>(reference_well);
	reference_buttons[0]->set_h_size_flags(SIZE_EXPAND_FILL);
	reference_buttons[0]->set_v_size_flags(SIZE_EXPAND_FILL);
	reference_empty_state = _studio_add<CenterContainer>(reference_buttons[0]);
	reference_empty_state->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
	reference_empty_state->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	VBoxContainer *reference_empty_content = _studio_add<VBoxContainer>(reference_empty_state);
	reference_empty_content->add_theme_constant_override(SNAME("separation"), int(5 * EDSCALE));
	TextureRect *reference_empty_icon = _studio_add<TextureRect>(reference_empty_content);
	reference_empty_icon->set_texture(SolersIcons::get(SNAME("tool_capture"), int(32 * EDSCALE)));
	reference_empty_icon->set_custom_minimum_size(Size2(32, 32) * EDSCALE);
	reference_empty_icon->set_stretch_mode(TextureRect::STRETCH_KEEP_ASPECT_CENTERED);
	Label *reference_empty_title = _studio_label(reference_empty_content, TTRC("Click, drop, or paste an image"), SNAME("SolersSessionTitle"));
	reference_empty_title->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	PackedStringArray image_extension_labels;
	for (const String &extension : image_extensions) {
		image_extension_labels.push_back("." + extension);
	}
	Label *reference_formats = _studio_label(reference_empty_content, String(", ").join(image_extension_labels), SNAME("SolersSessionMeta"));
	reference_formats->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	reference_formats->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	reference_aux = _studio_add<HBoxContainer>(reference_column);
	reference_aux->add_theme_constant_override(SNAME("separation"), int(6 * EDSCALE));
	for (int i = 1; i < 4; i++) {
		SolersSurface *slot = _studio_add<SolersSurface>(reference_aux);
		slot->set_h_size_flags(SIZE_EXPAND_FILL);
		slot->set_custom_minimum_size(Size2(0, 64 * EDSCALE));
		slot->configure(tokens.card, tokens.border, tokens.radius_list_thumb, 0, false);
		slot->set_dashed_border(true);
		slot->set_hover_accent(true);
		reference_buttons[i] = _studio_add<Button>(slot);
		reference_buttons[i]->set_h_size_flags(SIZE_EXPAND_FILL);
		reference_buttons[i]->set_v_size_flags(SIZE_EXPAND_FILL);
		reference_buttons[i]->set_icon_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	}
	for (Button *button : reference_buttons) {
		button->set_flat(true);
		button->set_focus_mode(FOCUS_ALL);
		button->set_mouse_filter(MOUSE_FILTER_PASS);
	}
	multiview_toggle = _studio_add<CheckButton>(creation_column);
	multiview_toggle->set_text(TTRC("Multi-view"));
	prompt_edit = _studio_add<TextEdit>(creation_column);
	prompt_edit->set_theme_type_variation(SNAME("SolersStudioPrompt"));
	prompt_edit->set_h_size_flags(SIZE_EXPAND_FILL);
	prompt_edit->set_custom_maximum_size(Size2(-1, 180 * EDSCALE));
	prompt_edit->set_line_wrapping_mode(TextEdit::LINE_WRAPPING_BOUNDARY);
	prompt_edit->set_smooth_scroll_enabled(true);
	prompt_edit->set_scroll_past_end_of_file_enabled(false);
	prompt_edit->set_fit_content_height_enabled(true);
	prompt_edit->set_indent_wrapped_lines(false);
	prompt_edit->set_highlight_current_line(false);
	prompt_edit->set_draw_minimap(false);
	prompt_edit->set_caret_blink_enabled(true);
	prompt_edit->set_custom_minimum_size(Size2(0, 92 * EDSCALE));
	featured_form = _studio_add<SolersSchemaForm>(creation_column);
	featured_form->set_image_stager(callable_mp(assets, &SolersAssetService::stage_input_image_async));
	options_toggle = _studio_add<Button>(creation_column);
	options_toggle->set_flat(true);
	options_toggle->set_toggle_mode(true);
	options_toggle->set_text_alignment(HORIZONTAL_ALIGNMENT_LEFT);
	generation_form = _studio_add<SolersSchemaForm>(creation_column);
	generation_form->set_image_stager(callable_mp(assets, &SolersAssetService::stage_input_image_async));
	generate_button = _studio_add<Button>(creation_frame);
	generate_button->set_name("GenerateButton");
	generate_button->set_theme_type_variation(SNAME("SolersPrimaryButton"));
	generate_button->set_custom_minimum_size(Size2(0, 44 * EDSCALE));

	SolersSurface *center_surface = _studio_add<SolersSurface>(columns);
	center_surface->set_h_size_flags(SIZE_EXPAND_FILL);
	center_surface->set_v_size_flags(SIZE_EXPAND_FILL);
	center_surface->set_stretch_ratio(2.0f);
	center_surface->configure(tokens.surface, tokens.border, tokens.radius_home_tile, 18, false);
	VBoxContainer *center = _studio_add<VBoxContainer>(center_surface);
	center->set_h_size_flags(SIZE_EXPAND_FILL);
	center->set_v_size_flags(SIZE_EXPAND_FILL);
	center->add_theme_constant_override(SNAME("separation"), int(12 * EDSCALE));
	geometry_stats = _studio_label(center, String(), SNAME("SolersSessionMeta"));
	PanelContainer *stage = _studio_add<PanelContainer>(center);
	stage->set_h_size_flags(SIZE_EXPAND_FILL);
	stage->set_v_size_flags(SIZE_EXPAND_FILL);
	preview = _studio_add<TextureRect>(stage);
	preview->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
	preview->set_stretch_mode(TextureRect::STRETCH_KEEP_ASPECT_CENTERED);
	preview->set_mouse_filter(MOUSE_FILTER_IGNORE);
	model_preview = _studio_add<SolersModelPreview>(stage);
	model_preview->set_h_size_flags(SIZE_EXPAND_FILL);
	model_preview->set_v_size_flags(SIZE_EXPAND_FILL);
	empty_stage = _studio_add<CenterContainer>(stage);
	VBoxContainer *empty_content = _studio_add<VBoxContainer>(empty_stage);
	empty_content->add_theme_constant_override(SNAME("separation"), int(14 * EDSCALE));
	empty_icon = _studio_add<TextureRect>(empty_content);
	empty_icon->set_texture(SolersIcons::get(SNAME("cube_plus"), int(84 * EDSCALE)));
	empty_icon->set_custom_minimum_size(Size2(84, 84) * EDSCALE);
	empty_icon->set_stretch_mode(TextureRect::STRETCH_KEEP_ASPECT_CENTERED);
	empty_activity = _studio_add<SolersActivityIndicator>(empty_content);
	empty_activity->set_custom_minimum_size(Size2(64, 64) * EDSCALE);
	empty_activity->set_self_modulate(tokens.primary);
	asset_title = _studio_label(empty_content, String(), SNAME("SolersHeroTitle"));
	asset_title->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	empty_generate_button = _studio_add<Button>(empty_content);
	empty_generate_button->set_theme_type_variation(SNAME("SolersPrimaryButton"));
	empty_generate_button->set_custom_minimum_size(Size2(180 * EDSCALE, 42 * EDSCALE));
	asset_status = _studio_label(center, String(), SNAME("SolersSessionMeta"));
	asset_status->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	asset_status->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	asset_actions = _studio_add<SolersSurface>(center);
	asset_actions->set_h_size_flags(SIZE_SHRINK_CENTER);
	asset_actions->configure(tokens.card, tokens.border, tokens.radius_home_tile, 4, false);
	HBoxContainer *action_row = _studio_add<HBoxContainer>(asset_actions);
	action_row->add_theme_constant_override(SNAME("separation"), int(4 * EDSCALE));
	import_button = _studio_add<Button>(action_row);
	import_button->set_button_icon(SolersIcons::get(SNAME("tool_export"), int(16 * EDSCALE)));
	animation_button = _studio_add<Button>(action_row);
	animation_button->set_button_icon(SolersIcons::get(SNAME("run_sprint"), int(16 * EDSCALE)));
	remesh_button = _studio_add<Button>(action_row);
	remesh_button->set_button_icon(SolersIcons::get(SNAME("adjustments"), int(16 * EDSCALE)));
	for (Button *button : { animation_button, remesh_button, import_button }) {
		button->set_theme_type_variation(SNAME("SolersStudioActionButton"));
		button->set_custom_minimum_size(Size2(38, 34) * EDSCALE);
	}
	catalog_variant = _studio_add<SolersStudioSelect>(center);
	acquire_button = _studio_add<Button>(center);
	acquire_button->set_theme_type_variation(SNAME("SolersPrimaryButton"));

	SolersSurface *library_surface = _studio_add<SolersSurface>(columns);
	library_surface->set_h_size_flags(SIZE_EXPAND_FILL);
	library_surface->set_v_size_flags(SIZE_EXPAND_FILL);
	library_surface->configure(tokens.surface, tokens.border, tokens.radius_home_tile, 12, false);
	VBoxContainer *library_column = _studio_add<VBoxContainer>(library_surface);
	library_column->set_h_size_flags(SIZE_EXPAND_FILL);
	library_column->set_v_size_flags(SIZE_EXPAND_FILL);
	library_column->add_theme_constant_override(SNAME("separation"), int(8 * EDSCALE));
	SolersSurface *search_surface = _studio_add<SolersSurface>(library_column);
	search_surface->set_name("StudioLibrarySearch");
	search_surface->configure(tokens.card, tokens.border, tokens.radius_list_thumb, 4, false);
	HBoxContainer *search_row = _studio_add<HBoxContainer>(search_surface);
	search_row->add_theme_constant_override(SNAME("separation"), int(6 * EDSCALE));
	TextureRect *search_icon = _studio_add<TextureRect>(search_row);
	search_icon->set_texture(SolersIcons::get(SNAME("tool_search"), int(15 * EDSCALE)));
	search_icon->set_custom_minimum_size(Size2(18, 18) * EDSCALE);
	search_icon->set_stretch_mode(TextureRect::STRETCH_KEEP_ASPECT_CENTERED);
	search_icon->set_mouse_filter(MOUSE_FILTER_IGNORE);
	catalog_query = _studio_add<LineEdit>(search_row);
	catalog_query->set_h_size_flags(SIZE_EXPAND_FILL);
	solers_style_bare_search_line_edit(catalog_query);
	library_tabs = _studio_add<TabContainer>(library_column);
	library_tabs->set_name("StudioLibraryTabs");
	library_tabs->set_h_size_flags(SIZE_EXPAND_FILL);
	library_tabs->set_v_size_flags(SIZE_EXPAND_FILL);
	VBoxContainer *catalog = _studio_add<VBoxContainer>(library_tabs);
	catalog->set_name("Catalog");
	catalog->add_theme_constant_override(SNAME("separation"), int(8 * EDSCALE));
	catalog_provider = _studio_add<SolersStudioSelect>(catalog);
	PanelContainer *catalog_stack = _studio_add<PanelContainer>(catalog);
	catalog_stack->set_v_size_flags(SIZE_EXPAND_FILL);
	catalog_list = _studio_add<ItemList>(catalog_stack);
	catalog_list->set_icon_mode(ItemList::ICON_MODE_TOP);
	catalog_list->set_fixed_icon_size(Size2i(92, 92) * EDSCALE);
	catalog_list->set_fixed_column_width(int(100 * EDSCALE));
	catalog_list->set_same_column_width(true);
	catalog_list->set_max_columns(0);
	catalog_list->set_max_text_lines(2);
	catalog_list->set_v_size_flags(SIZE_EXPAND_FILL);
	catalog_list->get_v_scroll_bar()->set_theme_type_variation(SNAME("SolersStudioScroll"));
	catalog_empty = _studio_empty_state(catalog_stack, &catalog_empty_label);
	attribution_label = _studio_label(catalog, String(), SNAME("SolersSessionMeta"));
	attribution_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	PanelContainer *project = _studio_add<PanelContainer>(library_tabs);
	project->set_name("Project");
	project_grid = _studio_add<SolersAssetGrid>(project);
	project_grid->set_v_size_flags(SIZE_EXPAND_FILL);
	project_empty = _studio_empty_state(project, &project_empty_label);
	reference_dialog = _studio_add<FileDialog>(this);
	reference_dialog->set_access(FileDialog::ACCESS_FILESYSTEM);
	reference_dialog->set_file_mode(FileDialog::FILE_MODE_OPEN_FILES);
	PackedStringArray patterns;
	for (const String &extension : image_extensions) {
		patterns.push_back("*." + extension);
	}
	reference_dialog->add_filter(String(", ").join(patterns), TTRC("Images"));
	import_dialog = _studio_add<FileDialog>(this);
	import_dialog->set_access(FileDialog::ACCESS_RESOURCES);
	import_dialog->set_file_mode(FileDialog::FILE_MODE_OPEN_DIR);
	remesh_dialog = _studio_add<AcceptDialog>(this);
	remesh_dialog->set_title(TTRC("Remesh model"));
	remesh_dialog->set_ok_button_text(TTRC("Remesh"));
	remesh_form = _studio_add<SolersSchemaForm>(remesh_dialog);
	delete_dialog = _studio_add<ConfirmationDialog>(this);
	delete_dialog->set_title(TTRC("Delete asset"));
	delete_dialog->set_ok_button_text(TTRC("Delete"));
	popup_list = _studio_add<SolersPopupList>(this);
	for (SolersStudioSelect *selector : { preset_button, catalog_variant, catalog_provider }) {
		selector->set_popup_list(popup_list);
		selector->set_fit_to_longest_item(false);
		selector->set_clip_text(true);
	}
	for (SolersSchemaForm *form : { featured_form, generation_form, remesh_form }) {
		form->set_popup_list(popup_list);
	}
	route_list->connect(SceneStringName(item_selected), callable_mp(this, &SolersStudio::_route_selected));
	preset_button->connect(SceneStringName(item_selected), callable_mp(this, &SolersStudio::_preset_selected));
	catalog_provider->connect(SceneStringName(item_selected), callable_mp(this, &SolersStudio::_catalog_provider_selected));
	for (Button *button : reference_buttons) {
		button->connect(SceneStringName(pressed), callable_mp(reference_dialog, &FileDialog::popup_file_dialog));
		button->connect(SceneStringName(mouse_entered), callable_mp((Control *)button, &Control::grab_focus).bind(true));
		button->connect(SceneStringName(gui_input), callable_mp(this, &SolersStudio::_reference_gui_input).bind(button));
		button->set_drag_forwarding(Callable(), callable_mp(this, &SolersStudio::_can_drop_reference).bind(button), callable_mp(this, &SolersStudio::_drop_reference).bind(button));
	}
	reference_dialog->connect(SNAME("files_selected"), callable_mp(this, &SolersStudio::_reference_files_selected));
	clear_references_button->connect(SceneStringName(pressed), callable_mp(this, &SolersStudio::_clear_references));
	multiview_toggle->connect(SceneStringName(toggled), callable_mp(this, &SolersStudio::_multiview_toggled));
	options_toggle->connect(SceneStringName(toggled), callable_mp(this, &SolersStudio::_options_toggled));
	generate_button->connect(SceneStringName(pressed), callable_mp(this, &SolersStudio::_generate_pressed));
	empty_generate_button->connect(SceneStringName(pressed), callable_mp(this, &SolersStudio::_generate_pressed));
	catalog_query->connect(SceneStringName(text_submitted), callable_mp(this, &SolersStudio::_catalog_search_pressed).unbind(1));
	catalog_query->connect(SceneStringName(text_changed), callable_mp(this, &SolersStudio::_refresh_project_assets).unbind(1));
	creation_scroll->connect(SceneStringName(mouse_entered), callable_mp(this, &SolersStudio::_creation_scroll_hovered).bind(true));
	creation_scroll->connect(SceneStringName(mouse_exited), callable_mp(this, &SolersStudio::_creation_scroll_hovered).bind(false));
	catalog_list->connect(SceneStringName(item_selected), callable_mp(this, &SolersStudio::_catalog_selected));
	project_grid->set_callbacks(callable_mp(this, &SolersStudio::_project_selected), callable_mp(this, &SolersStudio::_asset_menu_requested));
	acquire_button->connect(SceneStringName(pressed), callable_mp(this, &SolersStudio::_acquire_pressed));
	animation_button->connect(SceneStringName(pressed), callable_mp(this, &SolersStudio::_animation_pressed));
	remesh_button->connect(SceneStringName(pressed), callable_mp(this, &SolersStudio::_remesh_pressed));
	remesh_dialog->connect(SceneStringName(confirmed), callable_mp(this, &SolersStudio::_remesh_confirmed));
	delete_dialog->connect(SceneStringName(confirmed), callable_mp(this, &SolersStudio::_delete_asset_confirmed));
	import_button->connect(SceneStringName(pressed), callable_mp(this, &SolersStudio::_import_pressed));
	import_dialog->connect(SNAME("dir_selected"), callable_mp(this, &SolersStudio::_import_directory_selected));
	_options_toggled(false);
	_refresh_registry();
	_reload_project_assets();
	_refresh_text();
}
SolersStudio::~SolersStudio() {
	if (catalog_thread.is_started()) {
		catalog_cancel.set();
		catalog_thread.wait_to_finish();
	}
}
