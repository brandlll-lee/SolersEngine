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
/* first-class Project Manager tab. Left rail lists provider profiles     */
/* (SolersCategoryCard rows, Lucide glyphs); the right pane is a calm,    */
/* hard-edged UE-style form: model / base URL / API key, privacy mode,    */
/* and a live validation readout. Secrets are written through            */
/* SolersSecretStore (DPAPI / machine-bound AES) and are never echoed     */
/* back into the UI.                                                      */
/**************************************************************************/

#pragma once

#include "scene/gui/box_container.h"

class Button;
class CheckBox;
class GridContainer;
class Label;
class LineEdit;
class ScrollContainer;
class SolersCategoryCard;
class SolersProviderRegistry;
class VBoxContainer;

class SolersPMAIView : public HBoxContainer {
	GDCLASS(SolersPMAIView, HBoxContainer);

	SolersProviderRegistry *registry = nullptr; // Owned.

	// Left rail.
	VBoxContainer *nav_list = nullptr;
	String selected_provider;

	// Right pane.
	Label *provider_title = nullptr;
	Label *provider_notes = nullptr;
	LineEdit *model_edit = nullptr;
	LineEdit *base_url_edit = nullptr;
	LineEdit *api_key_edit = nullptr;
	Button *api_key_reveal = nullptr;
	Label *env_hint = nullptr;
	CheckBox *privacy_check = nullptr;
	VBoxContainer *status_list = nullptr;
	Button *save_btn = nullptr;
	Label *saved_feedback = nullptr;

	String _setting_path(const String &p_key) const;
	String _stored_string(const String &p_key, const String &p_default = String()) const;
	bool _stored_bool(const String &p_key, bool p_default) const;

	void _build_nav();
	void _select_provider(const String &p_id, bool p_load_stored);
	void _refresh_form(bool p_load_stored);
	void _refresh_status();
	void _add_status_row(const String &p_text, const Color &p_dot_color);
	void _on_field_changed(const String &p_ignored = String());
	void _on_privacy_toggled(bool p_pressed);
	void _on_reveal_toggled(bool p_pressed);
	void _save();

protected:
	void _notification(int p_what);
	static void _bind_methods() {}

public:
	SolersPMAIView();
	~SolersPMAIView();
};
