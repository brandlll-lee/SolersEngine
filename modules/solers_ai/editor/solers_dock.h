/**************************************************************************/
/*  solers_dock.h                                                         */
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

#include "core/input/input_event.h"
#include "scene/gui/panel_container.h"

class Button;
class CheckButton;
class Control;
class HBoxContainer;
class Label;
class PanelContainer;
class RichTextLabel;
class ScrollContainer;
class TextEdit;
class VBoxContainer;
class SolersActionTimeline;
class SolersAgentRuntime;
class SolersMCPAdapter;
class SolersPermissionManager;
class SolersObservationService;
class SolersRpcServer;
class SolersRmlChatSurface;
class SolersSettingsService;
class SolersToolRegistry;

class SolersDock : public PanelContainer {
	GDCLASS(SolersDock, PanelContainer);

	Label *project_status_label = nullptr;
	Label *runtime_status_label = nullptr;
	Label *scene_status_label = nullptr;
	Label *selection_status_label = nullptr;
	Label *tool_status_label = nullptr;
	Label *agent_status_label = nullptr;
	Label *protocol_status_label = nullptr;
	Label *provider_status_label = nullptr;
	Label *rmlui_status_label = nullptr;
	Label *approval_status_label = nullptr;
	Label *timeline_status_label = nullptr;
	ScrollContainer *chat_scroll = nullptr;
	VBoxContainer *message_list = nullptr;
	Control *empty_state = nullptr;
	TextEdit *chat_input = nullptr;
	Button *send_chat_button = nullptr;
	Button *add_context_button = nullptr;
	Button *access_status_button = nullptr;
	Button *model_select_button = nullptr;
	Button *effort_select_button = nullptr;
	Button *new_chat_button = nullptr;
	Button *more_button = nullptr;
	SolersRmlChatSurface *rml_chat_surface = nullptr;
	RichTextLabel *snapshot_preview = nullptr;
	VBoxContainer *debug_panel = nullptr;
	CheckButton *allow_scene_mutation_toggle = nullptr;
	CheckButton *allow_file_save_toggle = nullptr;
	CheckButton *allow_run_project_toggle = nullptr;
	String chat_log;

	SolersObservationService *observation_service = nullptr;
	SolersToolRegistry *tool_registry = nullptr;
	SolersActionTimeline *action_timeline = nullptr;
	SolersPermissionManager *permission_manager = nullptr;
	SolersAgentRuntime *agent_runtime = nullptr;
	SolersMCPAdapter *mcp_adapter = nullptr;
	SolersRpcServer *rpc_server = nullptr;
	SolersSettingsService *settings_service = nullptr;

	void _refresh_status();
	void _on_refresh_pressed();
	void _on_run_loopback_probe_pressed();
	void _on_send_chat_pressed();
	void _on_rml_prompt_submitted(const String &p_prompt);
	void _submit_chat_prompt(const String &p_prompt);
	void _on_abort_agent_pressed();
	void _on_approve_next_pressed();
	void _on_reject_next_pressed();
	void _on_allow_scene_mutations_toggled(bool p_enabled);
	void _on_allow_file_saves_toggled(bool p_enabled);
	void _on_allow_run_project_toggled(bool p_enabled);
	void _on_chat_input_gui_input(const Ref<InputEvent> &p_event);
	void _on_chat_input_text_changed();
	void _update_chat_input_height();
	void _refresh_icon_buttons();
	void _append_chat_message(const String &p_speaker, const String &p_message);
	void _append_orchestrator_result(const Dictionary &p_result);
	void _append_timeline_event(const Dictionary &p_event);
	void _populate_initial_timeline();
	PanelContainer *_create_panel_card(const Color &p_color, const Color &p_border_color, int p_radius = 12, int p_padding = 12) const;
	Label *_create_body_label(const String &p_text, bool p_bold = false) const;
	Button *_create_chip_button(const String &p_text) const;
	Button *_create_composer_select(const String &p_text, const String &p_tooltip) const;
	Button *_create_icon_button(const StringName &p_icon, const String &p_tooltip, bool p_primary = false) const;
	Label *_create_section_label(const String &p_text);
	Control *_create_empty_state() const;
	Control *_create_brand_mark() const;
	Control *_create_icon_badge(const String &p_text, const Color &p_color, const Color &p_border_color) const;
	HBoxContainer *_create_tool_dots(int p_count, const String &p_label) const;

protected:
	void _notification(int p_what);

public:
	void set_services(SolersObservationService *p_observation_service, SolersToolRegistry *p_tool_registry, SolersActionTimeline *p_action_timeline, SolersPermissionManager *p_permission_manager, SolersAgentRuntime *p_agent_runtime, SolersMCPAdapter *p_mcp_adapter, SolersRpcServer *p_rpc_server, SolersSettingsService *p_settings_service);
	void make_visible();

	SolersDock();
};
