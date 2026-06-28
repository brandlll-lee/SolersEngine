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
#include "core/templates/hash_map.h"
#include "core/variant/callable.h"
#include "scene/gui/panel_container.h"

class Button;
class Control;
class HBoxContainer;
class Label;
class MarginContainer;
class PanelContainer;
class ScrollContainer;
class TextEdit;
class VBoxContainer;
class SolersActionTimeline;
class SolersAgentSession;
class SolersAssistantCell;
class SolersGlyphButton;
class SolersMCPAdapter;
class SolersPermissionManager;
class SolersObservationService;
class SolersRpcServer;
class SolersSelectChip;
class SolersSettingsService;
class SolersStatusCell;
class SolersThinkingCell;
class SolersToolCell;
class SolersToolGroupCell;
class SolersToolRegistry;
class SolersUserBubble;

class SolersDock : public PanelContainer {
	GDCLASS(SolersDock, PanelContainer);

	ScrollContainer *chat_scroll = nullptr;
	VBoxContainer *message_list = nullptr;
	Control *empty_state = nullptr;
	VBoxContainer *empty_home = nullptr;
	VBoxContainer *root_box = nullptr;
	MarginContainer *composer_inset = nullptr;
	TextEdit *chat_input = nullptr;
	SolersGlyphButton *panel_button = nullptr;
	SolersGlyphButton *session_button = nullptr;
	SolersGlyphButton *add_context_button = nullptr;
	SolersGlyphButton *send_chat_button = nullptr;
	SolersSelectChip *model_chip = nullptr;
	SolersSelectChip *context_chip = nullptr;
	SolersSelectChip *approval_mode_chip = nullptr;
	MarginContainer *approval_overlay_inset = nullptr;
	PanelContainer *approval_overlay_card = nullptr;
	Label *approval_tool_label = nullptr;
	Label *approval_summary_label = nullptr;
	Button *approval_once_button = nullptr;
	Button *approval_always_button = nullptr;
	Button *approval_reject_button = nullptr;
	Button *approval_submit_button = nullptr;

	// Live turn state: cells updated in place as session events stream in.
	SolersThinkingCell *active_thinking_cell = nullptr;
	SolersAssistantCell *active_text_cell = nullptr;
	SolersStatusCell *status_cell = nullptr;
	SolersToolGroupCell *active_tool_group = nullptr;
	HashMap<String, SolersToolCell *> tool_cells_by_id;
	SolersToolCell *last_started_tool_cell = nullptr;
	String approval_choice = "once";
	bool approval_always_confirming = false;
	int active_approval_id = 0;
	int composer_margin_px = -1;

	String chat_log;

	SolersObservationService *observation_service = nullptr;
	SolersToolRegistry *tool_registry = nullptr;
	SolersActionTimeline *action_timeline = nullptr;
	SolersPermissionManager *permission_manager = nullptr;
	SolersAgentSession *agent_session = nullptr;
	SolersMCPAdapter *mcp_adapter = nullptr;
	SolersRpcServer *rpc_server = nullptr;
	SolersSettingsService *settings_service = nullptr;
	Callable workspace_toggle_callback;
	Callable session_menu_callback;

	void _refresh_status();
	void _refresh_model_chip();
	void _sync_layout_widths();
	void _on_send_chat_pressed();
	void _on_workspace_toggle_pressed();
	void _on_session_menu_pressed();
	void _on_model_chip_pressed();
	void _submit_chat_prompt(const String &p_prompt);
	void _on_agent_model_request_started();
	void _on_agent_assistant_delta(const String &p_text);
	void _on_agent_reasoning_delta(const String &p_text);
	void _on_agent_assistant_message(const String &p_text);
	void _on_agent_tool_started(const String &p_id, const String &p_name, const String &p_arguments);
	void _on_agent_tool_updated(const String &p_id, const String &p_name, const String &p_arguments);
	void _on_agent_tool_awaiting_approval(const String &p_id, const String &p_name);
	void _on_agent_tool_finished(const String &p_id, const String &p_name, const Dictionary &p_result, int p_duration_msec);
	void _on_agent_turn_completed(const Dictionary &p_result);
	void _on_agent_turn_failed(const Dictionary &p_error);
	void _on_agent_turn_retrying(int p_attempt, const String &p_message);
	void _sync_approval_panel();
	void _set_approval_choice(const String &p_choice);
	void _submit_current_approval();
	void _set_auto_approve_mode(bool p_enabled, bool p_persist);
	void _on_auto_approve_chip_pressed();
	void _on_chat_input_gui_input(const Ref<InputEvent> &p_event);
	void _on_chat_input_text_changed();
	void _update_chat_input_height();
	void _update_send_enabled();
	bool _is_scroll_pinned() const;
	void _on_cell_content_changed();
	void _scroll_chat_to_bottom();
	void _clear_empty_state();
	void _show_empty_state();
	void _append_user_message(const String &p_message);
	void _append_error_row(const String &p_text);
	void _ensure_status_cell(const String &p_status);
	void _remove_status_cell();
	void _settle_thinking_cell();
	SolersAssistantCell *_ensure_text_cell();
	void _settle_tool_group();
	void _finish_turn_cells();
	void _clear_chat_view(bool p_show_empty);
	PanelContainer *_create_panel_card(const Color &p_color, const Color &p_border_color, int p_radius = 12, int p_padding = 12) const;
	Control *_create_empty_state() const;

protected:
	void _notification(int p_what);

public:
	void set_services(SolersObservationService *p_observation_service, SolersToolRegistry *p_tool_registry, SolersActionTimeline *p_action_timeline, SolersPermissionManager *p_permission_manager, SolersMCPAdapter *p_mcp_adapter, SolersRpcServer *p_rpc_server, SolersSettingsService *p_settings_service);
	void make_visible();
	void set_agent_session(SolersAgentSession *p_agent_session);
	void start_new_chat();
	void load_chat_history(const Array &p_messages);
	void set_workspace_toggle_callback(const Callable &p_callback);
	void set_session_menu_callback(const Callable &p_callback);

	SolersDock();
	~SolersDock();
};
