/**************************************************************************/
/*  solers_studio.h                                                       */
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
#include "core/os/mutex.h"
#include "core/os/thread.h"
#include "core/templates/hash_map.h"
#include "core/templates/safe_refcount.h"
#include "scene/gui/panel_container.h"

class AcceptDialog;
class Button;
class CheckButton;
class ConfirmationDialog;
class Control;
class FileDialog;
class HBoxContainer;
class ItemList;
class Label;
class LineEdit;
class OptionButton;
class ScrollContainer;
class SolersAssetService;
class SolersAssetGrid;
class SolersActivityIndicator;
class SolersDock;
class SolersModelPreview;
class SolersPopupList;
class SolersStudioSelect;
class SolersSchemaForm;
class SolersSurface;
class TabContainer;
class TextEdit;
class Texture2D;
class TextureRect;
class VBoxContainer;

class SolersStudio : public PanelContainer {
	GDCLASS(SolersStudio, PanelContainer);

	SolersAssetService *assets = nullptr;
	SolersDock *dock = nullptr;
	uint64_t plugin_revision = 0;
	uint64_t asset_revision = 0;
	uint64_t preview_generation = 0;
	ItemList *route_list = nullptr;
	Control *creation_workspace = nullptr;
	Control *animation_workspace = nullptr;
	SolersStudioSelect *preset_button = nullptr;
	SolersStudioSelect *input_mode_button = nullptr;
	Label *preset_description = nullptr;
	TextEdit *prompt_edit = nullptr;
	Button *reference_buttons[4] = {};
	Button *clear_references_button = nullptr;
	HBoxContainer *reference_header = nullptr;
	SolersSurface *reference_surface = nullptr;
	Control *reference_empty_state = nullptr;
	HBoxContainer *reference_aux = nullptr;
	Button *generate_button = nullptr, *options_toggle = nullptr;
	SolersSchemaForm *featured_form = nullptr, *generation_form = nullptr;
	FileDialog *reference_dialog = nullptr;
	Array reference_attachments;
	Array reference_textures;
	int pending_reference_images = 0;
	uint64_t reference_generation = 1;
	SolersModelPreview *model_preview = nullptr;
	TextureRect *preview = nullptr, *empty_icon = nullptr;
	SolersActivityIndicator *empty_activity = nullptr;
	Label *creation_title = nullptr, *asset_title = nullptr, *asset_status = nullptr;
	Label *geometry_stats = nullptr;
	Control *empty_stage = nullptr;
	SolersSurface *asset_actions = nullptr;
	Button *empty_generate_button = nullptr, *animation_button = nullptr, *remesh_button = nullptr, *import_button = nullptr;
	AcceptDialog *remesh_dialog = nullptr;
	SolersStudioSelect *remesh_provider = nullptr;
	SolersSchemaForm *remesh_form = nullptr;
	Dictionary remesh_operation;
	SolersStudioSelect *animation_provider = nullptr, *animation_clip = nullptr;
	VBoxContainer *animation_operation_list = nullptr;
	SolersSchemaForm *animation_form = nullptr;
	Button *animation_run_button = nullptr, *preview_play_button = nullptr, *preview_stop_button = nullptr, *skeleton_button = nullptr;
	HBoxContainer *preview_controls = nullptr;
	Dictionary animation_operation;
	FileDialog *import_dialog = nullptr;
	Button *acquire_button = nullptr;
	SolersStudioSelect *catalog_variant = nullptr;
	SolersStudioSelect *catalog_provider = nullptr;
	LineEdit *catalog_query = nullptr;
	ItemList *catalog_list = nullptr;
	SolersAssetGrid *project_grid = nullptr;
	Control *catalog_empty = nullptr, *project_empty = nullptr;
	Label *catalog_empty_label = nullptr, *project_empty_label = nullptr;
	TabContainer *library_tabs = nullptr;
	Label *attribution_label = nullptr;
	Dictionary selected_manifest;
	String menu_asset_id;
	String import_asset_id;
	String loaded_model_path;
	Array project_manifests;
	HashMap<String, Ref<Texture2D>> project_previews;
	Dictionary selected_catalog;
	Dictionary asset_capabilities;
	Dictionary catalog_capabilities;
	Dictionary selected_preset;
	Array generation_presets;
	SolersPopupList *popup_list = nullptr;
	ConfirmationDialog *delete_dialog = nullptr;
	String pending_delete_asset_id;
	ScrollContainer *creation_scroll = nullptr;
	Thread catalog_thread;
	SafeFlag catalog_cancel;
	SafeFlag catalog_result_ready;
	SafeFlag catalog_done;
	Mutex catalog_previews_mutex;
	Array catalog_previews;
	String catalog_action;
	Dictionary catalog_args;
	Dictionary catalog_result;
	static void _catalog_thread_func(void *p_userdata);
	void _start_catalog_work(const String &p_action, const Dictionary &p_args);
	void _finish_catalog_work();
	void _sync_library_empty_states();
	void _sync_workspace();
	void _refresh_text();
	void _refresh_registry();
	void _refresh_providers();
	void _refresh_generation_schema();
	void _route_selected(int p_index);
	void _preset_selected(int p_index);
	Dictionary _selected_input_mode() const;
	void _input_mode_selected(int p_index);
	void _refresh_project_assets();
	void _reload_project_assets();
	void _show_manifest(const Dictionary &p_manifest);
	Dictionary _project_manifest(const String &p_asset_id) const;
	void _show_result(const Dictionary &p_result, const String &p_success);
	String _current_route() const;
	String _current_kind() const;
	String _selected_provider(const OptionButton *p_options) const;
	void _catalog_provider_selected(int p_index);
	void _reference_files_selected(const PackedStringArray &p_files);
	void _stage_reference_image(const Variant &p_source);
	void _reference_image_staged(const Dictionary &p_result, const Ref<Image> &p_image, uint64_t p_generation);
	void _refresh_reference_slots();
	void _clear_references();
	void _reference_gui_input(const Ref<InputEvent> &p_event, Button *p_button);
	void _external_reference_files_dropped(const PackedStringArray &p_files);
	bool _can_drop_reference(const Point2 &p_point, const Variant &p_data, Control *p_from) const;
	void _drop_reference(const Point2 &p_point, const Variant &p_data, Control *p_from);
	void _options_toggled(bool p_visible);
	void _generate_pressed();
	void _catalog_search_pressed();
	void _catalog_selected(int p_index);
	void _project_selected(const String &p_asset_id);
	void _asset_menu_requested(const String &p_asset_id, Control *p_anchor);
	void _asset_menu_action(const String &p_action);
	void _delete_asset_confirmed();
	void _acquire_pressed();
	void _animation_pressed();
	void _refresh_animation_workspace();
	void _animation_provider_selected(int p_index);
	void _animation_operation_selected(const Dictionary &p_operation);
	void _animation_run_pressed();
	void _refresh_preview_controls();
	void _preview_play_pressed();
	void _preview_stop_pressed();
	void _preview_skeleton_toggled(bool p_visible);
	void _remesh_pressed();
	void _remesh_provider_selected(int p_index);
	void _remesh_confirmed();
	void _import_pressed();
	void _import_directory_selected(const String &p_directory);
	void _preview_ready(const String &p_path, const Ref<Texture2D> &p_preview, const Ref<Texture2D> &p_small_preview, uint64_t p_generation);
	void _creation_scroll_hovered(bool p_hovered);

protected:
	static void _bind_methods() {}
	void _notification(int p_what);

public:
	SolersStudio(SolersAssetService *p_assets, SolersDock *p_dock);
	~SolersStudio();
};
