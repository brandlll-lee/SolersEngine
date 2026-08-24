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

struct SolersStudioRoute {
	const char *id;
	const char *label;
	const char *icon;
	const char *asset_kind;
	const char *workspace;
	const char *library;
	const char *bottom_action;
};

static constexpr SolersStudioRoute studio_routes[] = {
	{ "assets", "Assets", "layout_grid", "3d", "unavailable", "catalog", "acquire" },
	{ "3d", "3D", "tool_export", "3d", "generation", "project", "model" },
	{ "image", "Images", "photo_ai", "image", "unavailable", "catalog", "acquire" },
	{ "animation", "Animation", "run_sprint", "3d", "animation", "project", "import" },
	{ "audio", "Audio", "vinyl", "audio", "unavailable", "catalog", "acquire" },
	{ "material", "Materials", "adjustments", "material", "unavailable", "catalog", "acquire" },
	{ "hdri", "HDRI", "cloud", "hdri", "unavailable", "catalog", "acquire" },
};

static const SolersStudioRoute &_studio_route(const String &p_id) {
	for (const SolersStudioRoute &route : studio_routes) {
		if (p_id == route.id) {
			return route;
		}
	}
	return studio_routes[1];
}

static bool _studio_operation_less(const Variant &p_left, const Variant &p_right) {
	return int(Dictionary(p_left).get("presentation_order", 0)) < int(Dictionary(p_right).get("presentation_order", 0));
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

static int _studio_reference_limit(const Dictionary &p_mode) {
	return CLAMP((int)p_mode.get("max_reference_images", 0), 0, 4);
}

static String _studio_manifest_status(const Dictionary &p_manifest) {
	const Dictionary error = p_manifest.get("error", Dictionary());
	if (!error.is_empty()) {
		String message = error.get("message", TTRC("The operation failed."));
		const String code = error.get("code", String());
		return code.is_empty() ? message : message + "\n" + code;
	}
	String stage = TTR(String(p_manifest.get("stage", p_manifest.get("status", String()))).capitalize());
	if (p_manifest.has("progress")) {
		stage += "  " + itos(CLAMP((int)p_manifest["progress"], 0, 100)) + "%";
	}
	return stage;
}

String SolersStudio::_current_route() const {
	return route_list && route_list->get_selected_items().size() > 0 ? String(route_list->get_item_metadata(route_list->get_selected_items()[0])) : String("3d");
}

String SolersStudio::_current_kind() const {
	return _studio_route(_current_route()).asset_kind;
}
String SolersStudio::_selected_provider(const OptionButton *p_options) const {
	return p_options && p_options->get_selected() >= 0 ? String(p_options->get_selected_metadata()) : String();
}
void SolersStudio::_refresh_registry() {
	const String selected_route = _current_route();
	route_list->clear();
	int selected_index = 1;
	for (const SolersStudioRoute &route : studio_routes) {
		const int index = route_list->add_item(TTR(route.label), SolersIcons::get(StringName(route.icon), int(22 * EDSCALE)));
		route_list->set_item_metadata(index, route.id);
		if (selected_route == route.id) {
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
				Dictionary input_mode;
				input_mode["id"] = "text";
				input_mode["label"] = "Text prompt";
				input_mode["default"] = true;
				Array input_modes;
				input_modes.push_back(input_mode);
				fallback["input_modes"] = input_modes;
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
	const SolersStudioRoute &route = _studio_route(_current_route());
	const bool project_library = String(route.library) == "project";
	creation_workspace->set_visible(String(route.workspace) == "generation");
	animation_workspace->set_visible(String(route.workspace) == "animation");
	catalog_query->set_placeholder(project_library ? TTRC("Search generated assets...") : TTRC("Search catalog..."));
	library_tabs->set_current_tab(project_library ? 1 : 0);
	library_tabs->set_tab_hidden(0, project_library);
	library_tabs->set_tabs_visible(!project_library);
	_refresh_providers();
	_refresh_project_assets();
	_sync_workspace();
}

void SolersStudio::_preset_selected(int p_index) {
	selected_preset = p_index >= 0 && p_index < generation_presets.size() ? Dictionary(generation_presets[p_index]) : Dictionary();
	preset_button->set_disabled(selected_preset.is_empty());
	preset_button->select(selected_preset.is_empty() ? -1 : p_index);
	preset_description->set_text(selected_preset.get("description", String()));
	input_mode_button->clear();
	int default_mode = -1;
	for (const Variant &value : Array(selected_preset.get("input_modes", Array()))) {
		const Dictionary mode = value;
		input_mode_button->add_item(TTR(mode.get("label", String())));
		const int index = input_mode_button->get_item_count() - 1;
		input_mode_button->set_item_metadata(index, mode.get("id", String()));
		if ((bool)mode.get("default", false)) {
			default_mode = index;
		}
	}
	input_mode_button->set_disabled(input_mode_button->get_item_count() < 2);
	_input_mode_selected(default_mode >= 0 ? default_mode : (input_mode_button->get_item_count() > 0 ? 0 : -1));
}

Dictionary SolersStudio::_selected_input_mode() const {
	const String selected = _selected_provider(input_mode_button);
	for (const Variant &value : Array(selected_preset.get("input_modes", Array()))) {
		const Dictionary mode = value;
		if (mode.get("id", String()) == selected) {
			return mode;
		}
	}
	return Dictionary();
}

void SolersStudio::_input_mode_selected(int p_index) {
	input_mode_button->select(p_index);
	const Dictionary mode = _selected_input_mode();
	const bool text_mode = mode.get("id", String()) == "text";
	prompt_edit->set_visible(text_mode);
	const bool references_visible = _studio_reference_limit(mode) > 0;
	reference_header->set_visible(references_visible);
	reference_surface->set_visible(references_visible);
	_refresh_generation_schema();
	_refresh_reference_slots();
}

void SolersStudio::_refresh_generation_schema() {
	SolersPlugin *plugin = SolersPluginRegistry::get_plugin(selected_preset.get("provider", String()));
	const Dictionary schema = plugin ? plugin->get_generation_options_schema(_current_kind()) : Dictionary();
	const Dictionary mode = _selected_input_mode();
	Array featured = Array(selected_preset.get("featured_fields", Array())).duplicate();
	featured.append_array(mode.get("featured_fields", Array()));
	Array hidden = Array(selected_preset.get("hidden_fields", Array())).duplicate();
	hidden.append_array(mode.get("hidden_fields", Array()));
	Dictionary constraints = Dictionary(selected_preset.get("option_constraints", Dictionary())).duplicate(true);
	constraints.merge(mode.get("option_constraints", Dictionary()), true);
	Dictionary presentation = Dictionary(selected_preset.get("presentation", Dictionary())).duplicate(true);
	presentation.merge(mode.get("presentation", Dictionary()), true);
	featured_form->set_schema(_studio_schema_subset(schema, featured, hidden, constraints, false), Dictionary(), presentation);
	generation_form->set_schema(_studio_schema_subset(schema, featured, hidden, constraints, true), Dictionary(), presentation);
	Dictionary defaults = Dictionary(selected_preset.get("options", Dictionary())).duplicate(true);
	defaults.merge(mode.get("options", Dictionary()), true);
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
	catalog_query->set_editable(String(_studio_route(_current_route()).library) == "project" || plugin != nullptr);
}
void SolersStudio::_reference_files_selected(const PackedStringArray &p_files) {
	for (const String &path : p_files) {
		_stage_reference_image(path);
		if (reference_attachments.size() + pending_reference_images >= _studio_reference_limit(_selected_input_mode())) {
			break;
		}
	}
	_refresh_reference_slots();
}

void SolersStudio::_stage_reference_image(const Variant &p_source) {
	if (reference_attachments.size() + pending_reference_images >= _studio_reference_limit(_selected_input_mode())) {
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
	if (reference_attachments.size() < _studio_reference_limit(_selected_input_mode())) {
		reference_attachments.push_back(p_result.get("data", Dictionary()));
		reference_textures.push_back(ImageTexture::create_from_image(p_image));
	}
	_refresh_reference_slots();
}

void SolersStudio::_refresh_reference_slots() {
	const int limit = _studio_reference_limit(_selected_input_mode());
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
	reference_empty_state->set_visible(limit > 0 && reference_textures.is_empty());
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
	if (_studio_reference_limit(_selected_input_mode()) == 0 || String(drag_data.get("type", String())) != "files") {
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
	const Dictionary mode = _selected_input_mode();
	if (reference_attachments.size() < (int)mode.get("min_reference_images", 0)) {
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
	args["input_mode"] = mode.get("id", String());
	if (mode.get("id", String()) == "text") {
		args["prompt"] = prompt_edit->get_text();
	}
	Dictionary provider_options = selected_preset.get("options", Dictionary()).duplicate(true);
	provider_options.merge(mode.get("options", Dictionary()), true);
	provider_options.merge(featured_form->get_values(), true);
	provider_options.merge(generation_form->get_values(), true);
	for (const Variant &field : Array(mode.get("hidden_fields", Array()))) {
		provider_options.erase(field);
	}
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
	if (String(_studio_route(_current_route()).library) == "project") {
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
	const bool owns_catalog = String(_studio_route(_current_route()).library) == "catalog";
	if (catalog_result_ready.is_set()) {
		catalog_result_ready.clear();
		const Dictionary result = catalog_result;
		if (owns_catalog && !(bool)result.get("ok", false)) {
			_show_result(result, String());
		} else if (owns_catalog) {
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
				catalog_capabilities = result_data;
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
	for (const Variant &value : owns_catalog ? previews : Array()) {
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
	args["source_version"] = catalog_capabilities.get("source_version", String());
	_show_result(assets->catalog_acquire(args, String()), TTRC("Asset acquisition queued."));
}
void SolersStudio::_refresh_project_assets() {
	const String selected_id = selected_manifest.get("id", String());
	const String kind = _current_kind();
	const bool project_library = String(_studio_route(_current_route()).library) == "project";
	const String query = catalog_query ? catalog_query->get_text().strip_edges().to_lower() : String();
	project_grid->clear_assets();
	for (const Variant &value : project_manifests) {
		const Dictionary manifest = value;
		if (project_library && String(manifest.get("kind", String())).to_lower() != kind) {
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
	const Dictionary manifest = _project_manifest(p_asset_id);
	if (!manifest.is_empty()) {
		_show_manifest(manifest);
	}
}

Dictionary SolersStudio::_project_manifest(const String &p_asset_id) const {
	for (const Variant &value : project_manifests) {
		const Dictionary manifest = value;
		if (manifest.get("id", String()) == p_asset_id) {
			return manifest;
		}
	}
	return Dictionary();
}

void SolersStudio::_asset_menu_requested(const String &p_asset_id, Control *p_anchor) {
	menu_asset_id = p_asset_id;
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
	const Dictionary manifest = _project_manifest(menu_asset_id);
	if (manifest.is_empty()) {
		return;
	}
	if (p_action == "import") {
		import_asset_id = menu_asset_id;
		import_dialog->popup_file_dialog();
	} else if (p_action == "delete") {
		pending_delete_asset_id = menu_asset_id;
		delete_dialog->set_text(vformat(TTRC("Delete '%s' from the global Studio library? This cannot be undone."), manifest.get("name", pending_delete_asset_id)));
		delete_dialog->popup_centered();
	}
}

void SolersStudio::_delete_asset_confirmed() {
	if (pending_delete_asset_id.is_empty()) {
		return;
	}
	const String deleted_asset_id = pending_delete_asset_id;
	const Dictionary result = assets->delete_asset(deleted_asset_id);
	pending_delete_asset_id.clear();
	if (!result.get("ok", false)) {
		_show_result(result, String());
		return;
	}
	if (selected_manifest.get("id", String()) == deleted_asset_id) {
		selected_manifest.clear();
		loaded_model_path.clear();
		model_preview->clear_model();
		_refresh_preview_controls();
	}
	_reload_project_assets();
	_sync_workspace();
}
void SolersStudio::_show_manifest(const Dictionary &p_manifest) {
	selected_manifest = p_manifest;
	selected_catalog.clear();
	asset_title->set_text(p_manifest.get("name", p_manifest.get("id", TTRC("Untitled asset"))));
	asset_status->set_text(_studio_manifest_status(p_manifest));
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
	const Array project_entrypoints = p_manifest.get("project_entrypoints", Array());
	const String project_path = project_entrypoints.is_empty() ? String() : String(project_entrypoints[0]);
	if (!project_path.is_empty() && EditorResourcePreview::get_singleton()) {
		preview_generation++;
		EditorResourcePreview::get_singleton()->queue_resource_preview(project_path, callable_mp(this, &SolersStudio::_preview_ready).bind(preview_generation));
	}
	Dictionary capability_args;
	capability_args["asset_id"] = p_manifest.get("id", String());
	const Dictionary result = assets->capabilities(capability_args);
	asset_capabilities = result.get("data", Dictionary());
	_refresh_animation_workspace();
	_refresh_preview_controls();
	_sync_workspace();
}

void SolersStudio::_sync_workspace() {
	const String route = _current_route();
	const SolersStudioRoute &route_info = _studio_route(route);
	const bool model_workspace = String(route_info.workspace) == "generation" || String(route_info.workspace) == "animation";
	const bool has_manifest = model_workspace && !selected_manifest.is_empty();
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
		asset_title->set_text(String(route_info.workspace) == "animation" ? TTRC("Choose a model to animate") : (model_workspace ? TTRC("What will you create today?") : TTRC("This Studio workspace is not available yet.")));
		if (asset_status->get_text().is_empty()) {
			asset_status->set_text(model_workspace ? TTRC("Generate a model or choose one from your library.") : TTRC("This workspace is not available yet."));
		}
	}
	const bool has_model = has_manifest && !busy && model_preview->has_model();
	model_preview->set_visible(has_model);
	preview->set_visible(!model_workspace && !empty && !busy && !has_model && preview->get_texture().is_valid());
	const int64_t polycount = selected_manifest.get("polycount", 0);
	const int64_t vertex_count = selected_manifest.get("vertex_count", 0);
	geometry_stats->set_text(polycount > 0 || vertex_count > 0 ? vformat(TTRC("%s polygons | %s vertices"), String::num_int64(polycount), String::num_int64(vertex_count)) : String());
	geometry_stats->set_visible(!geometry_stats->get_text().is_empty());
	const bool ready = has_manifest && status == "ready";
	const bool preview_failed = ready && model_workspace && !has_model;
	if (preview_failed) {
		asset_status->set_text(TTRC("3D preview unavailable."));
	}
	asset_actions->set_visible(ready && model_workspace);
	asset_status->set_visible(empty || !ready || preview_failed);
	bool has_remesh = false;
	for (const Variant &value : Array(asset_capabilities.get("available_operations", Array()))) {
		has_remesh |= Dictionary(value).get("intent", String()) == "geometry.remesh";
	}
	const bool model_actions = String(route_info.bottom_action) == "model";
	animation_button->set_visible(model_actions);
	remesh_button->set_visible(model_actions);
	remesh_button->set_disabled(!has_remesh);
	import_button->set_visible(model_actions || String(route_info.bottom_action) == "import");
	import_button->set_text(String(route_info.bottom_action) == "import" ? TTRC("Import to current project") : String());
	preview_controls->set_visible(route == "animation" && has_model);
	const bool has_variant = has_catalog && catalog_variant->get_item_count() > 0;
	catalog_variant->set_visible(has_variant);
	acquire_button->set_visible(has_variant);
	empty_generate_button->set_visible(empty && String(route_info.workspace) == "generation");
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
	preview_play_button->set_tooltip_text(TTRC("Play or pause animation"));
	preview_stop_button->set_tooltip_text(TTRC("Stop animation"));
	skeleton_button->set_tooltip_text(TTRC("Show skeleton"));
	acquire_button->set_text(TTRC("Add to project"));
	catalog_empty_label->set_text(TTRC("No catalog assets found."));
	project_empty_label->set_text(TTRC("Your generated assets will appear here."));
	catalog_query->set_placeholder(String(_studio_route(_current_route()).library) == "project" ? TTRC("Search generated assets...") : TTRC("Search catalog..."));
	library_tabs->set_tab_title(0, TTRC("Catalog"));
	library_tabs->set_tab_title(1, TTRC("My generations"));
	if (!selected_manifest.is_empty()) {
		asset_status->set_text(_studio_manifest_status(selected_manifest));
	}
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

void SolersStudio::_refresh_animation_workspace() {
	if (!animation_provider) {
		return;
	}
	const String selected_provider = _selected_provider(animation_provider);
	Array providers;
	const String workspace = _studio_route(_current_route()).workspace;
	for (const Variant &value : Array(asset_capabilities.get("available_operations", Array()))) {
		const Dictionary operation = value;
		const String provider = operation.get("provider", String());
		if (operation.get("workspace", String()) == workspace && !providers.has(provider)) {
			providers.push_back(provider);
		}
	}
	animation_provider->clear();
	int selected = -1;
	const Dictionary contexts = asset_capabilities.get("provider_contexts", Dictionary());
	for (const Variant &value : providers) {
		const String provider = value;
		const Dictionary context = contexts.get(provider, Dictionary());
		animation_provider->add_item(context.get("label", provider));
		const int index = animation_provider->get_item_count() - 1;
		animation_provider->set_item_metadata(index, provider);
		if (provider == selected_provider) {
			selected = index;
		}
	}
	if (selected < 0 && !providers.is_empty()) {
		const int source_index = providers.find(selected_manifest.get("provider", String()));
		selected = source_index >= 0 ? source_index : 0;
	}
	_animation_provider_selected(selected);
}

void SolersStudio::_animation_provider_selected(int p_index) {
	animation_provider->select(p_index);
	animation_operation.clear();
	while (animation_operation_list->get_child_count() > 0) {
		Node *child = animation_operation_list->get_child(0);
		animation_operation_list->remove_child(child);
		child->queue_free();
	}
	const String provider = _selected_provider(animation_provider);
	Array operations;
	for (const Variant &value : Array(asset_capabilities.get("available_operations", Array()))) {
		const Dictionary operation = value;
		if (operation.get("provider", String()) == provider && operation.get("workspace", String()) == _studio_route(_current_route()).workspace) {
			operations.push_back(operation);
		}
	}
	operations.sort_custom(callable_mp_static(_studio_operation_less));
	String group;
	for (const Variant &value : operations) {
		const Dictionary operation = value;
		const String next_group = operation.get("presentation_group", operation.get("category", String()));
		if (!next_group.is_empty() && next_group != group) {
			group = next_group;
			_studio_label(animation_operation_list, TTR(group), SNAME("SolersSessionMeta"));
		}
		Button *button = _studio_add<Button>(animation_operation_list);
		button->set_text(TTR(operation.get("label", String())));
		button->set_text_alignment(HORIZONTAL_ALIGNMENT_LEFT);
		button->set_theme_type_variation(SNAME("SolersStudioActionButton"));
		button->connect(SceneStringName(pressed), callable_mp(this, &SolersStudio::_animation_operation_selected).bind(operation));
		if (animation_operation.is_empty()) {
			_animation_operation_selected(operation);
		}
	}
	animation_provider->set_disabled(animation_provider->get_item_count() == 0);
	if (animation_operation.is_empty()) {
		animation_form->set_schema(Dictionary());
		animation_run_button->set_text(TTRC("Run operation"));
	}
	animation_run_button->set_disabled(animation_operation.is_empty());
}

void SolersStudio::_animation_operation_selected(const Dictionary &p_operation) {
	animation_operation = p_operation;
	const String provider = p_operation.get("provider", String());
	const Dictionary context = Dictionary(asset_capabilities.get("provider_contexts", Dictionary())).get(provider, Dictionary());
	animation_form->set_schema(p_operation.get("options_schema", Dictionary()), context, p_operation.get("presentation", Dictionary()));
	animation_run_button->set_text(TTR(p_operation.get("label", TTRC("Run operation"))));
	animation_run_button->set_disabled(p_operation.is_empty());
}

void SolersStudio::_animation_run_pressed() {
	if (selected_manifest.is_empty() || animation_operation.is_empty()) {
		return;
	}
	const String provider = animation_operation.get("provider", String());
	if (!assets->is_provider_configured("3d", provider)) {
		dock->open_provider_settings("plugins");
		return;
	}
	Dictionary args;
	args["asset_id"] = selected_manifest.get("id", String());
	args["provider"] = provider;
	args["operation_id"] = animation_operation.get("operation_id", String());
	args["options"] = animation_form->get_values();
	_show_result(assets->run_operation(args), TTRC("Asset operation queued."));
}

void SolersStudio::_refresh_preview_controls() {
	if (!animation_clip || !preview_play_button || !preview_stop_button || !skeleton_button || !model_preview) {
		return;
	}
	const String selected = _selected_provider(animation_clip);
	animation_clip->clear();
	for (const String &name : model_preview->get_animation_names()) {
		animation_clip->add_item(name);
		const int index = animation_clip->get_item_count() - 1;
		animation_clip->set_item_metadata(index, name);
		if (name == selected) {
			animation_clip->select(index);
		}
	}
	if (animation_clip->get_selected() < 0 && animation_clip->get_item_count() > 0) {
		animation_clip->select(0);
	}
	const bool has_animation = animation_clip->get_item_count() > 0;
	animation_clip->set_disabled(!has_animation);
	preview_play_button->set_disabled(!has_animation);
	preview_stop_button->set_disabled(!has_animation);
	skeleton_button->set_disabled(!model_preview->has_skeleton());
	skeleton_button->set_pressed_no_signal(false);
	model_preview->set_skeleton_visible(false);
	preview_play_button->set_button_icon(get_editor_theme_icon(SNAME("MainPlay")));
	preview_stop_button->set_button_icon(get_editor_theme_icon(SNAME("Stop")));
	skeleton_button->set_button_icon(get_editor_theme_icon(SNAME("Skeleton3D")));
}

void SolersStudio::_preview_play_pressed() {
	if (model_preview->is_animation_playing()) {
		model_preview->pause_animation();
		preview_play_button->set_button_icon(get_editor_theme_icon(SNAME("MainPlay")));
	} else if (model_preview->play_animation(_selected_provider(animation_clip))) {
		preview_play_button->set_button_icon(get_editor_theme_icon(SNAME("Pause")));
	}
}

void SolersStudio::_preview_stop_pressed() {
	model_preview->stop_animation();
	preview_play_button->set_button_icon(get_editor_theme_icon(SNAME("MainPlay")));
}

void SolersStudio::_preview_skeleton_toggled(bool p_visible) {
	model_preview->set_skeleton_visible(p_visible);
}

void SolersStudio::_remesh_pressed() {
	const String selected_provider = _selected_provider(remesh_provider);
	remesh_provider->clear();
	const Dictionary contexts = asset_capabilities.get("provider_contexts", Dictionary());
	for (const Variant &value : Array(asset_capabilities.get("available_operations", Array()))) {
		const Dictionary operation = value;
		if (operation.get("intent", String()) != "geometry.remesh") {
			continue;
		}
		const String provider = operation.get("provider", String());
		remesh_provider->add_item(Dictionary(contexts.get(provider, Dictionary())).get("label", provider));
		remesh_provider->set_item_metadata(remesh_provider->get_item_count() - 1, provider);
	}
	if (remesh_provider->get_item_count() == 0) {
		return;
	}
	int selected = -1;
	for (int i = 0; i < remesh_provider->get_item_count(); i++) {
		const String provider = remesh_provider->get_item_metadata(i);
		if (provider == selected_provider || (selected < 0 && provider == String(selected_manifest.get("provider", String())))) {
			selected = i;
		}
	}
	_remesh_provider_selected(selected >= 0 ? selected : 0);
	remesh_dialog->popup_centered(Size2i(420, 360) * EDSCALE);
}

void SolersStudio::_remesh_provider_selected(int p_index) {
	remesh_provider->select(p_index);
	remesh_operation.clear();
	const String provider = _selected_provider(remesh_provider);
	for (const Variant &value : Array(asset_capabilities.get("available_operations", Array()))) {
		const Dictionary operation = value;
		if (operation.get("provider", String()) == provider && operation.get("intent", String()) == "geometry.remesh") {
			remesh_operation = operation;
			break;
		}
	}
	const Dictionary context = Dictionary(asset_capabilities.get("provider_contexts", Dictionary())).get(provider, Dictionary());
	remesh_form->set_schema(remesh_operation.get("options_schema", Dictionary()), context, remesh_operation.get("presentation", Dictionary()));
}

void SolersStudio::_remesh_confirmed() {
	if (selected_manifest.is_empty() || remesh_operation.is_empty()) {
		return;
	}
	const String provider = remesh_operation.get("provider", String());
	if (!assets->is_provider_configured("3d", provider)) {
		dock->open_provider_settings("plugins");
		return;
	}
	Dictionary args;
	args["asset_id"] = selected_manifest.get("id", String());
	args["provider"] = provider;
	args["operation_id"] = remesh_operation.get("operation_id", String());
	args["options"] = remesh_form->get_values();
	_show_result(assets->run_operation(args), TTRC("Asset operation queued."));
}

void SolersStudio::_import_pressed() {
	if (selected_manifest.is_empty()) {
		return;
	}
	import_asset_id = selected_manifest.get("id", String());
	import_dialog->popup_file_dialog();
}

void SolersStudio::_import_directory_selected(const String &p_directory) {
	Dictionary args;
	args["asset_id"] = import_asset_id;
	args["target_dir"] = p_directory;
	import_asset_id.clear();
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
		_refresh_preview_controls();
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
	} else if (p_what == NOTIFICATION_THEME_CHANGED) {
		_refresh_preview_controls();
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
	creation_surface->set_h_size_flags(SIZE_EXPAND_FILL);
	creation_surface->set_v_size_flags(SIZE_EXPAND_FILL);
	creation_surface->configure(tokens.surface, tokens.border, tokens.radius_home_tile, 12, false);
	VBoxContainer *creation_frame = _studio_add<VBoxContainer>(creation_surface);
	creation_workspace = creation_frame;
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
	input_mode_button = _studio_add<SolersStudioSelect>(creation_column);
	input_mode_button->set_text_alignment(HORIZONTAL_ALIGNMENT_LEFT);
	reference_header = _studio_add<HBoxContainer>(creation_column);
	Label *reference_title = _studio_label(reference_header, TTRC("Reference images"), SNAME("SolersSessionTitle"));
	reference_title->set_h_size_flags(SIZE_EXPAND_FILL);
	clear_references_button = _studio_add<Button>(reference_header);
	clear_references_button->set_flat(true);
	clear_references_button->set_button_icon(SolersIcons::get(SNAME("cross"), int(14 * EDSCALE)));
	reference_surface = _studio_add<SolersSurface>(creation_column);
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
	VBoxContainer *animation_column = _studio_add<VBoxContainer>(creation_surface);
	animation_workspace = animation_column;
	animation_column->set_h_size_flags(SIZE_EXPAND_FILL);
	animation_column->set_v_size_flags(SIZE_EXPAND_FILL);
	animation_column->add_theme_constant_override(SNAME("separation"), int(12 * EDSCALE));
	_studio_label(animation_column, TTRC("Animate 3D"), SNAME("SolersSessionTitle"));
	animation_provider = _studio_add<SolersStudioSelect>(animation_column);
	animation_provider->set_text_alignment(HORIZONTAL_ALIGNMENT_LEFT);
	ScrollContainer *animation_scroll = _studio_add<ScrollContainer>(animation_column);
	animation_scroll->set_h_size_flags(SIZE_EXPAND_FILL);
	animation_scroll->set_v_size_flags(SIZE_EXPAND_FILL);
	animation_scroll->set_horizontal_scroll_mode(ScrollContainer::SCROLL_MODE_DISABLED);
	VBoxContainer *animation_body = _studio_add<VBoxContainer>(animation_scroll);
	animation_body->set_h_size_flags(SIZE_EXPAND_FILL);
	animation_body->add_theme_constant_override(SNAME("separation"), int(8 * EDSCALE));
	animation_operation_list = _studio_add<VBoxContainer>(animation_body);
	animation_operation_list->add_theme_constant_override(SNAME("separation"), int(6 * EDSCALE));
	animation_form = _studio_add<SolersSchemaForm>(animation_body);
	animation_form->set_image_stager(callable_mp(assets, &SolersAssetService::stage_input_image_async));
	animation_run_button = _studio_add<Button>(animation_column);
	animation_run_button->set_theme_type_variation(SNAME("SolersPrimaryButton"));
	animation_run_button->set_custom_minimum_size(Size2(0, 44 * EDSCALE));

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
	preview_controls = _studio_add<HBoxContainer>(center);
	preview_controls->set_h_size_flags(SIZE_SHRINK_CENTER);
	preview_controls->add_theme_constant_override(SNAME("separation"), int(4 * EDSCALE));
	animation_clip = _studio_add<SolersStudioSelect>(preview_controls);
	animation_clip->set_custom_minimum_size(Size2(180, 34) * EDSCALE);
	preview_play_button = _studio_add<Button>(preview_controls);
	preview_stop_button = _studio_add<Button>(preview_controls);
	skeleton_button = _studio_add<Button>(preview_controls);
	skeleton_button->set_toggle_mode(true);
	for (Button *button : { preview_play_button, preview_stop_button, skeleton_button }) {
		button->set_theme_type_variation(SNAME("SolersStudioActionButton"));
		button->set_custom_minimum_size(Size2(36, 34) * EDSCALE);
	}
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
	VBoxContainer *remesh_column = _studio_add<VBoxContainer>(remesh_dialog);
	remesh_column->set_custom_minimum_size(Size2(380, 280) * EDSCALE);
	remesh_provider = _studio_add<SolersStudioSelect>(remesh_column);
	remesh_form = _studio_add<SolersSchemaForm>(remesh_column);
	remesh_form->set_image_stager(callable_mp(assets, &SolersAssetService::stage_input_image_async));
	delete_dialog = _studio_add<ConfirmationDialog>(this);
	delete_dialog->set_title(TTRC("Delete asset"));
	delete_dialog->set_ok_button_text(TTRC("Delete"));
	popup_list = _studio_add<SolersPopupList>(this);
	for (SolersStudioSelect *selector : { preset_button, input_mode_button, catalog_variant, catalog_provider, remesh_provider, animation_provider, animation_clip }) {
		selector->set_popup_list(popup_list);
		selector->set_fit_to_longest_item(false);
		selector->set_clip_text(true);
	}
	for (SolersSchemaForm *form : { featured_form, generation_form, remesh_form, animation_form }) {
		form->set_popup_list(popup_list);
	}
	route_list->connect(SceneStringName(item_selected), callable_mp(this, &SolersStudio::_route_selected));
	preset_button->connect(SceneStringName(item_selected), callable_mp(this, &SolersStudio::_preset_selected));
	input_mode_button->connect(SceneStringName(item_selected), callable_mp(this, &SolersStudio::_input_mode_selected));
	catalog_provider->connect(SceneStringName(item_selected), callable_mp(this, &SolersStudio::_catalog_provider_selected));
	remesh_provider->connect(SceneStringName(item_selected), callable_mp(this, &SolersStudio::_remesh_provider_selected));
	animation_provider->connect(SceneStringName(item_selected), callable_mp(this, &SolersStudio::_animation_provider_selected));
	for (Button *button : reference_buttons) {
		button->connect(SceneStringName(pressed), callable_mp(reference_dialog, &FileDialog::popup_file_dialog));
		button->connect(SceneStringName(mouse_entered), callable_mp((Control *)button, &Control::grab_focus).bind(true));
		button->connect(SceneStringName(gui_input), callable_mp(this, &SolersStudio::_reference_gui_input).bind(button));
		button->set_drag_forwarding(Callable(), callable_mp(this, &SolersStudio::_can_drop_reference).bind(button), callable_mp(this, &SolersStudio::_drop_reference).bind(button));
	}
	reference_dialog->connect(SNAME("files_selected"), callable_mp(this, &SolersStudio::_reference_files_selected));
	clear_references_button->connect(SceneStringName(pressed), callable_mp(this, &SolersStudio::_clear_references));
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
	animation_run_button->connect(SceneStringName(pressed), callable_mp(this, &SolersStudio::_animation_run_pressed));
	preview_play_button->connect(SceneStringName(pressed), callable_mp(this, &SolersStudio::_preview_play_pressed));
	preview_stop_button->connect(SceneStringName(pressed), callable_mp(this, &SolersStudio::_preview_stop_pressed));
	skeleton_button->connect(SceneStringName(toggled), callable_mp(this, &SolersStudio::_preview_skeleton_toggled));
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
