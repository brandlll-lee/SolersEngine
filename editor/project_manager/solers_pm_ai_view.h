/**************************************************************************/
/*  solers_pm_ai_view.h                                                   */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                              */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/
/* Solers: AI-native game engine.                                        */
/*                                                                        */
/* BYOK (bring-your-own-key) AI model configuration view, hosted as a     */
/* first-class shared settings view. Left rail lists provider profiles   */
/* (SolersCategoryCard rows, Lucide glyphs); the right pane is a calm,    */
/* hard-edged UE-style form: model / base URL / API key, local policy,    */
/* and a live validation readout. Secrets are written through            */
/* SolersSecretStore (DPAPI / machine-bound AES) and are never echoed     */
/* back into the UI.                                                      */
/**************************************************************************/

#pragma once

#include "core/variant/dictionary.h"
#include "scene/gui/box_container.h"

class Button;
class CheckBox;
class GridContainer;
class Label;
class LineEdit;
class ScrollContainer;
class SolersCategoryCard;
class SolersProviderRegistry;
class SolersSettingsService;
class Texture2D;
class VBoxContainer;

class SolersPMAIView : public HBoxContainer {
	GDCLASS(SolersPMAIView, HBoxContainer);

	SolersProviderRegistry *registry = nullptr; // Owned.
	SolersSettingsService *settings_service = nullptr; // Owned.

	// Left rail.
	VBoxContainer *nav_list = nullptr;
	String selected_category;
	String selected_provider;

	// Right pane.
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

	String _asset_setting_path(const String &p_plugin_id, const String &p_key) const;
	String _stored_asset_string(const String &p_plugin_id, const String &p_key, const String &p_default = String()) const;
	Dictionary _asset_plugin_profile(const String &p_id) const;
	bool _is_asset_provider(const String &p_id) const;
	bool _uses_codex_auth(const String &p_id) const;

	void _build_nav();
	void _select_category(const String &p_id);
	void _build_provider_list();
	void _add_provider_row(const String &p_id, const String &p_title, const Ref<Texture2D> &p_icon, bool p_preserve_icon_color = false);
	void _open_provider(const String &p_id);
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

protected:
	void _notification(int p_what);
	static void _bind_methods() {}

public:
	void refresh();
	SolersPMAIView();
	~SolersPMAIView();
};
