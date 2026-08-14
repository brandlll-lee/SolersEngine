/**************************************************************************/
/*  solers_pm_ai_view.h                                                   */
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

#include "core/variant/dictionary.h"
#include "scene/gui/box_container.h"

class AcceptDialog;
class Button;
class CheckBox;
class EditorSettingsDialog;
class GridContainer;
class Label;
class LineEdit;
class OptionButton;
class ScrollContainer;
class SolersCategoryCard;
class SolersProviderRegistry;
class SolersSettingsService;
class SpinBox;
class Texture2D;
class VBoxContainer;

class SolersPMAIView : public HBoxContainer {
	GDCLASS(SolersPMAIView, HBoxContainer);

	SolersProviderRegistry *registry = nullptr;
	SolersSettingsService *settings_service = nullptr;
	bool owns_services = false;

	// Left rail.
	VBoxContainer *nav_list = nullptr;
	String selected_category;
	String selected_provider;

	// Right pane — providers.
	VBoxContainer *provider_list_view = nullptr;
	VBoxContainer *provider_list = nullptr;
	VBoxContainer *provider_detail_view = nullptr;
	Label *provider_list_title = nullptr;
	Label *provider_list_notes = nullptr;
	VBoxContainer *local_models_only_box = nullptr;
	CheckBox *local_models_only_check = nullptr;
	Button *detail_back_btn = nullptr;
	Label *provider_title = nullptr;
	Label *provider_notes = nullptr;
	GridContainer *connection_grid = nullptr;
	Label *model_label = nullptr;
	LineEdit *model_edit = nullptr;
	Label *base_url_label = nullptr;
	LineEdit *base_url_edit = nullptr;
	SpinBox *context_window_edit = nullptr;
	SpinBox *max_tokens_edit = nullptr;
	Label *api_key_label = nullptr;
	LineEdit *api_key_edit = nullptr;
	Button *api_key_reveal = nullptr;
	Label *env_hint = nullptr;
	VBoxContainer *oauth_box = nullptr;
	Label *oauth_status = nullptr;
	Button *oauth_connect_btn = nullptr;
	Button *oauth_cancel_btn = nullptr;
	Button *oauth_disconnect_btn = nullptr;
	VBoxContainer *status_list = nullptr;
	Button *save_btn = nullptr;
	Button *disconnect_btn = nullptr;
	Label *saved_feedback = nullptr;

	AcceptDialog *view_all_dialog = nullptr;
	LineEdit *view_all_search = nullptr;
	VBoxContainer *view_all_list = nullptr;

	// Right pane — quick EditorSettings (migrated from QuickSettingsDialog).
	VBoxContainer *quick_settings_view = nullptr;
	VBoxContainer *quick_settings_list = nullptr;
#ifndef ANDROID_ENABLED
	Vector<String> editor_languages;
	OptionButton *language_option_button = nullptr;
#endif
	Vector<String> editor_styles;
	Vector<String> editor_themes;
	Vector<String> editor_scales;
	Vector<String> editor_network_modes;
	Vector<String> editor_check_for_updates;
	Vector<String> editor_directory_naming_conventions;
	OptionButton *style_option_button = nullptr;
	OptionButton *theme_option_button = nullptr;
	OptionButton *scale_option_button = nullptr;
	OptionButton *network_mode_option_button = nullptr;
	OptionButton *check_for_update_button = nullptr;
	OptionButton *directory_naming_convention_button = nullptr;
	Label *custom_theme_label = nullptr;
	Label *restart_required_label = nullptr;
	Button *restart_required_button = nullptr;
	EditorSettingsDialog *editor_settings_dialog = nullptr;

	String _asset_setting_path(const String &p_plugin_id, const String &p_key) const;
	String _stored_asset_string(const String &p_plugin_id, const String &p_key, const String &p_default = String()) const;
	Dictionary _asset_plugin_profile(const String &p_id) const;
	bool _is_asset_provider(const String &p_id) const;
	bool _uses_codex_auth(const String &p_id) const;
	Dictionary _provider_status(const String &p_id, bool p_live_form = false) const;
	String _source_label(const String &p_source) const;
	void _add_section_label(const String &p_text);
	void _add_llm_row(const String &p_id, const String &p_title, const String &p_subtitle, bool p_connect_action);

	void _build_nav();
	void _select_category(const String &p_id);
	void _build_provider_list();
	void _add_provider_row(const String &p_id, const String &p_title, const Ref<Texture2D> &p_icon, bool p_preserve_icon_color = false, const String &p_subtitle = String());
	void _show_provider_list();
	void _select_provider(const String &p_id, bool p_load_stored);
	void _refresh_form(bool p_load_stored);
	void _refresh_status();
	void _add_status_row(const String &p_text, const Color &p_dot_color);
	void _on_field_changed(const String &p_ignored = String());
	void _on_local_models_only_toggled(bool p_pressed);
	void _on_reveal_toggled(bool p_pressed);
	void _on_codex_connect();
	void _on_codex_cancel();
	void _on_codex_disconnect();
	void _save();
	void _disconnect_selected_provider();
	void _open_view_all();
	void _rebuild_view_all_list(const String &p_filter = String());
	void _on_view_all_search(const String &p_text);

	void _fetch_quick_setting_values();
	void _update_quick_settings_values();
	void _add_quick_setting_control(const String &p_text, Control *p_control);
#ifndef ANDROID_ENABLED
	void _language_selected(int p_id);
#endif
	void _style_selected(int p_id);
	void _theme_selected(int p_id);
	void _scale_selected(int p_id);
	void _network_mode_selected(int p_id);
	void _check_for_update_selected(int p_id);
	void _directory_naming_convention_selected(int p_id);
	void _set_quick_setting_value(const String &p_setting, const Variant &p_value, bool p_restart_required = false);
	void _show_full_settings();
	void _request_restart();

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void bind_services(SolersSettingsService *p_settings_service);
	void refresh();
	void select_category(const String &p_id);
	void update_quick_popup_size_limits(const Size2 &p_max_popup_size);

	SolersPMAIView();
	~SolersPMAIView();
};
