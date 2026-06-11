/**************************************************************************/
/*  solers_dock.h                                                         */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                              */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/
/* Solers: AI-native game engine.                                        */
/*                                                                        */
/* The Solers AI chat dock. Pure native implementation: Godot container   */
/* Controls for layout and text (TextEdit keeps OS-grade IME/CJK input),  */
/* plus the self-drawn Codex-style widget kit in solers_chat_widgets.h    */
/* for every piece of chat chrome (icon buttons, select chips, send pill).*/
/* No embedded UI runtime; rendering goes straight through the editor's   */
/* canvas like every other dock, so steady state costs zero CPU.          */
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
class SolersAgentSession;
class SolersGlyphButton;
class SolersMCPAdapter;
class SolersPermissionManager;
class SolersObservationService;
class SolersRpcServer;
class SolersSelectChip;
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
	Label *approval_status_label = nullptr;
	Label *timeline_status_label = nullptr;
	ScrollContainer *chat_scroll = nullptr;
	VBoxContainer *message_list = nullptr;
	Control *empty_state = nullptr;
	TextEdit *chat_input = nullptr;
	SolersGlyphButton *panel_button = nullptr;
	SolersGlyphButton *new_chat_button = nullptr;
	SolersGlyphButton *more_button = nullptr;
	SolersGlyphButton *add_context_button = nullptr;
	SolersGlyphButton *send_chat_button = nullptr;
	SolersSelectChip *access_chip = nullptr;
	SolersSelectChip *model_chip = nullptr;
	SolersSelectChip *context_chip = nullptr;
	RichTextLabel *snapshot_preview = nullptr;
	Label *active_reasoning_label = nullptr;
	String active_reasoning_text;
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
	SolersAgentSession *agent_session = nullptr;
	SolersMCPAdapter *mcp_adapter = nullptr;
	SolersRpcServer *rpc_server = nullptr;
	SolersSettingsService *settings_service = nullptr;

	void _refresh_status();
	void _refresh_model_chip();
	void _on_refresh_pressed();
	void _on_run_loopback_probe_pressed();
	void _on_send_chat_pressed();
	void _on_model_chip_pressed();
	void _on_new_chat_pressed();
	void _submit_chat_prompt(const String &p_prompt);
	void _on_agent_reasoning_delta(const String &p_text);
	void _on_agent_assistant_message(const String &p_text);
	void _on_agent_tool_started(const String &p_id, const String &p_name, const String &p_arguments);
	void _on_agent_tool_finished(const String &p_id, const String &p_name, const Dictionary &p_result);
	void _on_agent_turn_completed(const Dictionary &p_result);
	void _on_agent_turn_failed(const Dictionary &p_error);
	void _on_abort_agent_pressed();
	void _on_approve_next_pressed();
	void _on_reject_next_pressed();
	void _on_allow_scene_mutations_toggled(bool p_enabled);
	void _on_allow_file_saves_toggled(bool p_enabled);
	void _on_allow_run_project_toggled(bool p_enabled);
	void _on_chat_input_gui_input(const Ref<InputEvent> &p_event);
	void _on_chat_input_text_changed();
	void _update_chat_input_height();
	void _update_send_enabled();
	void _debug_dump_settled();
	void _scroll_chat_to_bottom();
	void _clear_empty_state();
	void _append_chat_message(const String &p_speaker, const String &p_message);
	void _append_tool_row(const String &p_text, bool p_ok);
	void _clear_active_reasoning();
	PanelContainer *_create_panel_card(const Color &p_color, const Color &p_border_color, int p_radius = 12, int p_padding = 12) const;
	Label *_create_body_label(const String &p_text, bool p_bold = false) const;
	Label *_create_section_label(const String &p_text);
	Control *_create_empty_state() const;

protected:
	void _notification(int p_what);

public:
	void set_services(SolersObservationService *p_observation_service, SolersToolRegistry *p_tool_registry, SolersActionTimeline *p_action_timeline, SolersPermissionManager *p_permission_manager, SolersAgentRuntime *p_agent_runtime, SolersMCPAdapter *p_mcp_adapter, SolersRpcServer *p_rpc_server, SolersSettingsService *p_settings_service);
	void make_visible();
	void set_agent_session(SolersAgentSession *p_agent_session);

	SolersDock();
	~SolersDock();
};
