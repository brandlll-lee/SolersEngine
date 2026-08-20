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

#include "core/io/file_access.h"
#include "core/io/image.h"
#include "core/io/image_loader.h"
#include "core/io/resource_loader.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/string/translation_server.h"
#include "editor/editor_node.h"
#include "editor/inspector/editor_resource_preview.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/center_container.h"
#include "scene/gui/file_dialog.h"
#include "scene/gui/item_list.h"
#include "scene/gui/label.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/option_button.h"
#include "scene/gui/progress_bar.h"
#include "scene/gui/scroll_container.h"
#include "scene/gui/split_container.h"
#include "scene/gui/tab_container.h"
#include "scene/gui/text_edit.h"
#include "scene/gui/texture_rect.h"
#include "scene/resources/image_texture.h"

#include "modules/solers_ai/core/solers_asset_service.h"
#include "modules/solers_ai/editor/solers_chat_widgets.h"
#include "modules/solers_ai/editor/solers_dock.h"
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
String SolersStudio::_current_kind() const {
	return kind_option && kind_option->get_selected() >= 0 ? String(kind_option->get_selected_metadata()) : String();
}
String SolersStudio::_selected_provider(const OptionButton *p_options) const {
	return p_options && p_options->get_selected() >= 0 ? String(p_options->get_selected_metadata()) : String();
}
void SolersStudio::_refresh_registry() {
	const String selected_kind = _current_kind();
	HashSet<String> kinds;
	for (SolersPlugin *plugin : SolersPluginRegistry::get_plugins()) {
		for (const Variant &value : Array(plugin->get_profile().get("kinds", Array()))) {
			kinds.insert(String(value).to_lower());
		}
	}
	Vector<String> ordered;
	for (const String &kind : kinds) {
		ordered.push_back(kind);
	}
	ordered.sort();
	kind_option->clear();
	for (const String &kind : ordered) {
		kind_option->add_item(kind.capitalize());
		kind_option->set_item_metadata(kind_option->get_item_count() - 1, kind);
		if (kind == selected_kind) {
			kind_option->select(kind_option->get_item_count() - 1);
		}
	}
	_refresh_providers();
}
void SolersStudio::_refresh_providers() {
	const String kind = _current_kind();
	const String selected_generation = _selected_provider(generation_provider);
	const String selected_catalog_provider = _selected_provider(catalog_provider);
	generation_provider->clear();
	catalog_provider->clear();
	for (SolersPlugin *plugin : SolersPluginRegistry::get_plugins()) {
		const Dictionary profile = plugin->get_profile();
		if (!Array(profile.get("kinds", Array())).has(kind)) {
			continue;
		}
		const String id = profile.get("id", String());
		const String label = profile.get("label", id);
		if ((bool)profile.get("supports_generation", false)) {
			generation_provider->add_item(label);
			generation_provider->set_item_metadata(generation_provider->get_item_count() - 1, id);
			if (id == selected_generation) {
				generation_provider->select(generation_provider->get_item_count() - 1);
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
	_refresh_generation_schema();
	_catalog_provider_selected(catalog_provider->get_selected());
}
void SolersStudio::_refresh_generation_schema() {
	SolersPlugin *plugin = SolersPluginRegistry::get_plugin(_selected_provider(generation_provider));
	generation_form->set_schema(plugin ? plugin->get_generation_options_schema(_current_kind()) : Dictionary());
	generate_button->set_disabled(!plugin);
}
void SolersStudio::_catalog_provider_selected(int) {
	SolersPlugin *plugin = SolersPluginRegistry::get_plugin(_selected_provider(catalog_provider));
	const Dictionary profile = plugin ? plugin->get_profile() : Dictionary();
	attribution_label->set_text(profile.get("attribution", String()));
	catalog_query->set_editable(plugin != nullptr);
}
void SolersStudio::_reference_files_selected(const PackedStringArray &p_files) {
	reference_paths.clear();
	for (const String &path : p_files) {
		Ref<Image> image;
		image.instantiate();
		if (ImageLoader::load_image(path, image) == OK && !image->is_empty()) {
			reference_paths.push_back(path);
			if (reference_paths.size() == 4) {
				break;
			}
		}
	}
	reference_button->set_text(reference_paths.is_empty() ? TTRC("Add reference images") : vformat(TTRN("%d reference image", "%d reference images", reference_paths.size()), reference_paths.size()));
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
	generation_options_surface->set_visible(p_visible);
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
	const String provider = _selected_provider(generation_provider);
	if (!assets->is_provider_configured(kind, provider)) {
		dock->open_provider_settings("plugins");
		return;
	}
	Dictionary args;
	args["kind"] = kind;
	args["provider"] = provider;
	args["prompt"] = prompt_edit->get_text();
	args["provider_options"] = generation_form->get_values();
	if (!reference_paths.is_empty()) {
		Array ids;
		Array attachments;
		for (const String &path : reference_paths) {
			const String id = FileAccess::get_sha256(path);
			Dictionary attachment;
			attachment["id"] = id;
			attachment["type"] = "image";
			const String extension = path.get_extension().to_lower();
			attachment["mime_type"] = "image/" + (extension == "png" || extension == "webp" ? extension : String("jpeg"));
			attachment["filename"] = path.get_file();
			attachment["local_path"] = path;
			attachments.push_back(attachment);
			ids.push_back(id);
		}
		args["input_attachments"] = ids;
		args["_attachments"] = attachments;
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
void SolersStudio::_catalog_selected(int p_index) {
	if (p_index < 0 || p_index >= catalog_list->get_item_count()) {
		return;
	}
	selected_catalog = catalog_list->get_item_metadata(p_index);
	selected_manifest.clear();
	asset_title->set_text(selected_catalog.get("display_name", selected_catalog.get("name", TTRC("Catalog asset"))));
	asset_status->set_text(selected_catalog.get("description", String()));
	preview->set_texture(catalog_list->get_item_icon(p_index));
	operation_option->clear();
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
String SolersStudio::_manifest_resource_path(const Dictionary &p_manifest) const {
	for (const char *field : { "project_entrypoints", "project_files" }) {
		for (const Variant &value : Array(p_manifest.get(field, Array()))) {
			const String path = value;
			const StringName type = ResourceLoader::get_resource_type(path);
			if (type == SNAME("PackedScene") || (ClassDB::class_exists(type) && ClassDB::is_parent_class(type, SNAME("PackedScene")))) {
				return path;
			}
		}
	}
	return String();
}
void SolersStudio::_refresh_project_assets() {
	const String selected_id = selected_manifest.get("id", String());
	project_list->clear();
	const Array manifests = assets->list_assets();
	for (const Variant &value : manifests) {
		const Dictionary manifest = value;
		const String title = manifest.get("name", manifest.get("id", TTRC("Untitled asset")));
		Ref<Texture2D> item_preview = SolersIcons::get(SNAME("tool_asset"), int(54 * EDSCALE));
		const String preview_file = manifest.get("preview_file", String());
		if (!preview_file.is_empty()) {
			const Ref<Image> image = Image::load_from_file(preview_file);
			if (image.is_valid() && !image->is_empty()) {
				item_preview = ImageTexture::create_from_image(image);
			}
		}
		const int index = project_list->add_item(title + "\n" + String(manifest.get("status", "unknown")).capitalize(), item_preview);
		project_list->set_item_metadata(index, manifest);
		if (manifest.get("id", String()) == selected_id) {
			project_list->select(index);
			_show_manifest(manifest);
		}
	}
}
void SolersStudio::_project_selected(int p_index) {
	if (p_index >= 0 && p_index < project_list->get_item_count()) {
		_show_manifest(project_list->get_item_metadata(p_index));
	}
}
void SolersStudio::_show_manifest(const Dictionary &p_manifest) {
	selected_manifest = p_manifest;
	selected_catalog.clear();
	asset_title->set_text(p_manifest.get("name", p_manifest.get("id", TTRC("Untitled asset"))));
	asset_status->set_text(String(p_manifest.get("stage", p_manifest.get("status", String()))).capitalize());
	const double progress_value = p_manifest.get("progress", 0.0);
	asset_progress->set_value(progress_value <= 1.0 ? progress_value * 100.0 : progress_value);
	preview->set_texture(Ref<Texture2D>());
	const String preview_file = p_manifest.get("preview_file", String());
	if (!preview_file.is_empty()) {
		Ref<Image> image = Image::load_from_file(preview_file);
		if (image.is_valid() && !image->is_empty()) {
			preview->set_texture(ImageTexture::create_from_image(image));
		}
	}
	const String path = _manifest_resource_path(p_manifest);
	if (!path.is_empty() && EditorResourcePreview::get_singleton()) {
		preview_generation++;
		EditorResourcePreview::get_singleton()->queue_resource_preview(path, callable_mp(this, &SolersStudio::_preview_ready).bind(preview_generation));
	}
	Dictionary capability_args;
	capability_args["asset_id"] = p_manifest.get("id", String());
	const Dictionary result = assets->capabilities(capability_args);
	capability_data = result.get("data", Dictionary());
	operation_option->clear();
	for (const Variant &value : Array(capability_data.get("available_operations", Array()))) {
		const Dictionary operation = value;
		operation_option->add_item(operation.get("label", operation.get("operation_id", String())));
		operation_option->set_item_metadata(operation_option->get_item_count() - 1, operation);
	}
	const bool has_operations = operation_option->get_item_count() > 0;
	if (has_operations) {
		_operation_selected(operation_option->get_selected());
	}
	_sync_workspace();
}

void SolersStudio::_sync_workspace() {
	const bool has_manifest = !selected_manifest.is_empty();
	const bool has_catalog = !selected_catalog.is_empty();
	const bool empty = !has_manifest && !has_catalog;
	empty_icon->set_visible(empty);
	if (empty) {
		asset_title->set_text(TTRC("What will you create today?"));
		if (asset_status->get_text().is_empty()) {
			asset_status->set_text(TTRC("Generate an asset or choose one from your library."));
		}
	}
	preview->set_visible(!empty && preview->get_texture().is_valid());
	const String status = String(selected_manifest.get("status", String())).to_lower();
	asset_progress->set_visible(has_manifest && !SolersAssetService::is_project_import_terminal_status(status));
	operation_panel->set_visible(has_manifest && operation_option->get_item_count() > 0);
	const bool has_variant = has_catalog && catalog_variant->get_item_count() > 0;
	catalog_variant->set_visible(has_variant);
	acquire_button->set_visible(has_variant);
	place_button->set_visible(has_manifest && !_manifest_resource_path(selected_manifest).is_empty());
}

void SolersStudio::_refresh_text() {
	creation_title->set_text(TTRC("Create"));
	prompt_edit->set_placeholder(TTRC("Describe the asset to create..."));
	options_toggle->set_text(TTRC("Options"));
	generate_button->set_text(TTRC("Generate"));
	operation_button->set_text(TTRC("Run operation"));
	acquire_button->set_text(TTRC("Add to project"));
	place_button->set_text(TTRC("Place"));
	catalog_query->set_placeholder(TTRC("Search catalog..."));
	library_tabs->set_tab_title(0, TTRC("Catalog"));
	library_tabs->set_tab_title(1, TTRC("Project"));
	const PackedStringArray current_reference_paths = reference_paths;
	_reference_files_selected(current_reference_paths);
	_sync_workspace();
}
void SolersStudio::_operation_selected(int p_index) {
	if (p_index < 0 || p_index >= operation_option->get_item_count()) {
		operation_form->set_schema(Dictionary());
		return;
	}
	const Dictionary operation = operation_option->get_item_metadata(p_index);
	operation_form->set_schema(operation.get("options_schema", Dictionary()), capability_data);
}
void SolersStudio::_operation_pressed() {
	if (selected_manifest.is_empty() || operation_option->get_selected() < 0) {
		return;
	}
	const Dictionary operation = operation_option->get_selected_metadata();
	Dictionary args;
	args["asset_id"] = selected_manifest.get("id", String());
	args["operation_id"] = operation.get("operation_id", String());
	args["options"] = operation_form->get_values();
	_show_result(assets->run_operation(args), TTRC("Asset operation queued."));
}
void SolersStudio::_place_pressed() {
	const String path = _manifest_resource_path(selected_manifest);
	if (path.is_empty()) {
		return;
	}
	Vector<String> paths;
	paths.push_back(path);
	EditorNode::get_singleton()->request_instantiate_scenes(paths);
}
void SolersStudio::_preview_ready(const String &, const Ref<Texture2D> &p_preview, const Ref<Texture2D> &p_small_preview, uint64_t p_generation) {
	if (p_generation == preview_generation && (p_preview.is_valid() || p_small_preview.is_valid())) {
		preview->set_texture(p_preview.is_valid() ? p_preview : p_small_preview);
		_sync_workspace();
	}
}
void SolersStudio::_notification(int p_what) {
	if (p_what == NOTIFICATION_PROCESS) {
		if (plugin_revision != SolersPluginRegistry::get_revision()) {
			plugin_revision = SolersPluginRegistry::get_revision();
			_refresh_registry();
		}
		if (asset_revision != assets->get_revision()) {
			asset_revision = assets->get_revision();
			_refresh_project_assets();
		}
		_finish_catalog_work();
	} else if (p_what == NOTIFICATION_TRANSLATION_CHANGED) {
		_refresh_text();
	}
}
SolersStudio::SolersStudio(SolersAssetService *p_assets, SolersDock *p_dock) :
		assets(p_assets), dock(p_dock) {
	set_name("SolersStudio");
	set_process(true);
	const SolersUITheme::Tokens tokens = SolersUITheme::make_tokens();
	HSplitContainer *columns = _studio_add<HSplitContainer>(this);
	columns->set_v_size_flags(SIZE_EXPAND_FILL);
	VBoxContainer *creation_column = _studio_add<VBoxContainer>(columns);
	creation_column->set_custom_minimum_size(Size2(300 * EDSCALE, 0));
	creation_column->set_v_size_flags(SIZE_EXPAND_FILL);
	creation_column->add_theme_constant_override(SNAME("separation"), int(12 * EDSCALE));
	ScrollContainer *creation_scroll = _studio_add<ScrollContainer>(creation_column);
	creation_scroll->set_v_size_flags(SIZE_EXPAND_FILL);
	creation_scroll->set_horizontal_scroll_mode(ScrollContainer::SCROLL_MODE_DISABLED);
	VBoxContainer *creation = _studio_add<VBoxContainer>(creation_scroll);
	creation->set_h_size_flags(SIZE_EXPAND_FILL);
	creation->add_theme_constant_override(SNAME("separation"), int(10 * EDSCALE));
	creation_title = _studio_label(creation, String(), SNAME("SolersSessionTitle"));
	kind_option = _studio_add<OptionButton>(creation);
	kind_option->set_custom_minimum_size(Size2(0, 38 * EDSCALE));
	generation_provider = _studio_add<OptionButton>(creation);
	generation_provider->set_custom_minimum_size(Size2(0, 38 * EDSCALE));
	SolersSurface *reference_surface = _studio_add<SolersSurface>(creation);
	reference_surface->configure(tokens.card, tokens.border, tokens.radius_list_thumb, 8, false);
	reference_button = _studio_add<Button>(reference_surface);
	reference_button->set_flat(true);
	reference_button->set_button_icon(SolersIcons::get(SNAME("tool_capture"), int(24 * EDSCALE)));
	reference_button->set_custom_minimum_size(Size2(0, 104 * EDSCALE));
	SolersSurface *prompt_surface = _studio_add<SolersSurface>(creation);
	solers_configure_prompt_surface(prompt_surface);
	prompt_edit = _studio_add<TextEdit>(prompt_surface);
	solers_configure_prompt_text_edit(prompt_edit);
	prompt_edit->set_custom_minimum_size(Size2(0, 92 * EDSCALE));
	options_toggle = _studio_add<Button>(creation);
	options_toggle->set_flat(true);
	options_toggle->set_toggle_mode(true);
	options_toggle->set_text_alignment(HORIZONTAL_ALIGNMENT_LEFT);
	generation_options_surface = _studio_add<SolersSurface>(creation);
	generation_options_surface->configure(tokens.card, tokens.border, tokens.radius_list_thumb, 10, false);
	generation_form = _studio_add<SolersSchemaForm>(generation_options_surface);
	generate_button = _studio_add<Button>(creation_column);
	generate_button->set_theme_type_variation(SNAME("SolersPrimaryButton"));
	generate_button->set_custom_minimum_size(Size2(0, 44 * EDSCALE));

	VBoxContainer *center = _studio_add<VBoxContainer>(columns);
	center->set_custom_minimum_size(Size2(420 * EDSCALE, 0));
	center->set_h_size_flags(SIZE_EXPAND_FILL);
	center->set_v_size_flags(SIZE_EXPAND_FILL);
	center->add_theme_constant_override(SNAME("separation"), int(12 * EDSCALE));
	CenterContainer *stage = _studio_add<CenterContainer>(center);
	stage->set_v_size_flags(SIZE_EXPAND_FILL);
	VBoxContainer *stage_content = _studio_add<VBoxContainer>(stage);
	stage_content->set_custom_minimum_size(Size2(420 * EDSCALE, 0));
	stage_content->add_theme_constant_override(SNAME("separation"), int(12 * EDSCALE));
	empty_icon = _studio_add<TextureRect>(stage_content);
	empty_icon->set_texture(SolersIcons::get(SNAME("sparkle"), int(58 * EDSCALE)));
	empty_icon->set_custom_minimum_size(Size2(58, 58) * EDSCALE);
	empty_icon->set_stretch_mode(TextureRect::STRETCH_KEEP_ASPECT_CENTERED);
	asset_title = _studio_label(stage_content, String(), SNAME("SolersSessionTitle"));
	asset_title->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	preview = _studio_add<TextureRect>(stage_content);
	preview->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
	preview->set_stretch_mode(TextureRect::STRETCH_KEEP_ASPECT_CENTERED);
	preview->set_custom_minimum_size(Size2(420, 330) * EDSCALE);
	asset_status = _studio_label(stage_content, String(), SNAME("SolersSessionMeta"));
	asset_status->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	asset_status->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	asset_progress = _studio_add<ProgressBar>(stage_content);
	asset_progress->set_show_percentage(false);
	asset_progress->set_custom_minimum_size(Size2(0, 6 * EDSCALE));
	operation_panel = _studio_add<VBoxContainer>(center);
	operation_panel->add_theme_constant_override(SNAME("separation"), int(8 * EDSCALE));
	operation_option = _studio_add<OptionButton>(operation_panel);
	operation_form = _studio_add<SolersSchemaForm>(operation_panel);
	operation_button = _studio_add<Button>(operation_panel);
	operation_button->set_theme_type_variation(SNAME("SolersPrimaryButton"));
	catalog_variant = _studio_add<OptionButton>(center);
	acquire_button = _studio_add<Button>(center);
	acquire_button->set_theme_type_variation(SNAME("SolersPrimaryButton"));
	place_button = _studio_add<Button>(center);
	place_button->set_theme_type_variation(SNAME("SolersPrimaryButton"));

	library_tabs = _studio_add<TabContainer>(columns);
	library_tabs->set_custom_minimum_size(Size2(330 * EDSCALE, 0));
	library_tabs->set_v_size_flags(SIZE_EXPAND_FILL);
	VBoxContainer *catalog = _studio_add<VBoxContainer>(library_tabs);
	catalog->set_name("Catalog");
	catalog->add_theme_constant_override(SNAME("separation"), int(8 * EDSCALE));
	catalog_provider = _studio_add<OptionButton>(catalog);
	SolersSurface *search_surface = _studio_add<SolersSurface>(catalog);
	search_surface->configure(tokens.card, tokens.border, tokens.radius_list_thumb, 4, false);
	catalog_query = _studio_add<LineEdit>(search_surface);
	solers_style_bare_search_line_edit(catalog_query);
	catalog_list = _studio_add<ItemList>(catalog);
	catalog_list->set_icon_mode(ItemList::ICON_MODE_TOP);
	catalog_list->set_fixed_icon_size(Size2i(112, 112) * EDSCALE);
	catalog_list->set_fixed_column_width(int(124 * EDSCALE));
	catalog_list->set_same_column_width(true);
	catalog_list->set_max_text_lines(2);
	catalog_list->set_v_size_flags(SIZE_EXPAND_FILL);
	attribution_label = _studio_label(catalog, String(), SNAME("SolersSessionMeta"));
	attribution_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	project_list = _studio_add<ItemList>(library_tabs);
	project_list->set_name("Project");
	project_list->set_icon_mode(ItemList::ICON_MODE_TOP);
	project_list->set_fixed_icon_size(Size2i(112, 112) * EDSCALE);
	project_list->set_fixed_column_width(int(124 * EDSCALE));
	project_list->set_same_column_width(true);
	project_list->set_max_text_lines(2);
	reference_dialog = _studio_add<FileDialog>(this);
	reference_dialog->set_access(FileDialog::ACCESS_FILESYSTEM);
	reference_dialog->set_file_mode(FileDialog::FILE_MODE_OPEN_FILES);
	List<String> extensions;
	ImageLoader::get_recognized_extensions(&extensions);
	PackedStringArray patterns;
	for (const String &extension : extensions) {
		patterns.push_back("*." + extension);
	}
	reference_dialog->add_filter(String(", ").join(patterns), TTRC("Images"));
	kind_option->connect(SceneStringName(item_selected), callable_mp(this, &SolersStudio::_refresh_providers).unbind(1));
	generation_provider->connect(SceneStringName(item_selected), callable_mp(this, &SolersStudio::_refresh_generation_schema).unbind(1));
	catalog_provider->connect(SceneStringName(item_selected), callable_mp(this, &SolersStudio::_catalog_provider_selected));
	reference_button->connect(SceneStringName(pressed), callable_mp(reference_dialog, &FileDialog::popup_file_dialog));
	reference_button->set_drag_forwarding(Callable(), callable_mp(this, &SolersStudio::_can_drop_reference).bind(reference_button), callable_mp(this, &SolersStudio::_drop_reference).bind(reference_button));
	reference_dialog->connect(SNAME("files_selected"), callable_mp(this, &SolersStudio::_reference_files_selected));
	options_toggle->connect(SceneStringName(toggled), callable_mp(this, &SolersStudio::_options_toggled));
	generate_button->connect(SceneStringName(pressed), callable_mp(this, &SolersStudio::_generate_pressed));
	catalog_query->connect(SceneStringName(text_submitted), callable_mp(this, &SolersStudio::_catalog_search_pressed).unbind(1));
	catalog_list->connect(SceneStringName(item_selected), callable_mp(this, &SolersStudio::_catalog_selected));
	project_list->connect(SceneStringName(item_selected), callable_mp(this, &SolersStudio::_project_selected));
	operation_option->connect(SceneStringName(item_selected), callable_mp(this, &SolersStudio::_operation_selected));
	operation_button->connect(SceneStringName(pressed), callable_mp(this, &SolersStudio::_operation_pressed));
	acquire_button->connect(SceneStringName(pressed), callable_mp(this, &SolersStudio::_acquire_pressed));
	place_button->connect(SceneStringName(pressed), callable_mp(this, &SolersStudio::_place_pressed));
	_options_toggled(false);
	_refresh_registry();
	_refresh_project_assets();
	_refresh_text();
}
SolersStudio::~SolersStudio() {
	if (catalog_thread.is_started()) {
		catalog_cancel.set();
		catalog_thread.wait_to_finish();
	}
}
