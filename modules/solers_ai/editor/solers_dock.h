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
#include "core/templates/hash_map.h"
#include "core/templates/vector.h"
#include "core/variant/callable.h"
#include "scene/gui/panel_container.h"

class Button;
class ButtonGroup;
class Control;
class AcceptDialog;
class ConfirmationDialog;
class HBoxContainer;
class HSplitContainer;
class ItemList;
class Label;
class LineEdit;
class MarginContainer;
class PanelContainer;
class ScrollContainer;
class TextEdit;
class Texture2D;
class VBoxContainer;
class SolersActionTimeline;
class SolersAgentSession;
class SolersAssistantCell;
class SolersGlyphButton;
class SolersContextRing;
class SolersPermissionManager;
class SolersPMAIView;
class SolersPlanCapsule;
class SolersObservationService;
class SolersSelectChip;
class SolersSettingsService;
class SolersStatusCell;
class SolersThinkingCell;
class SolersToolCell;
class SolersToolRegistry;
class SolersUserMessageCell;

class SolersDock : public PanelContainer {
	GDCLASS(SolersDock, PanelContainer);

	ScrollContainer *chat_scroll = nullptr;
	MarginContainer *timeline_inset = nullptr;
	VBoxContainer *message_list = nullptr;
	VBoxContainer *history_mount = nullptr;
	Control *empty_state = nullptr;
	VBoxContainer *empty_home = nullptr;
	VBoxContainer *root_box = nullptr;
	HSplitContainer *body_split = nullptr;
	PanelContainer *session_sidebar = nullptr;
	VBoxContainer *chat_column = nullptr;
	ScrollContainer *session_scroll = nullptr;
	VBoxContainer *session_list = nullptr;
	Ref<ButtonGroup> session_button_group;
	MarginContainer *composer_inset = nullptr;
	SolersPlanCapsule *plan_capsule = nullptr;
	TextEdit *chat_input = nullptr;
	SolersGlyphButton *session_button = nullptr;
	SolersGlyphButton *add_context_button = nullptr;
	SolersGlyphButton *send_chat_button = nullptr;
	SolersContextRing *context_ring = nullptr;
	HBoxContainer *attachment_bar = nullptr;
	AcceptDialog *provider_settings_dialog = nullptr;
	ConfirmationDialog *rewind_dialog = nullptr;
	int64_t pending_rewind_event_id = -1;
	String pending_rewind_prompt;
	Array pending_rewind_attachments;
	SolersPMAIView *provider_settings_view = nullptr;
	SolersSelectChip *model_chip = nullptr;
	PanelContainer *plugin_mention_popup = nullptr;
	VBoxContainer *plugin_mention_box = nullptr;
	LineEdit *plugin_mention_search = nullptr;
	ItemList *plugin_mention_list = nullptr;
	int plugin_mention_line = -1;
	int plugin_mention_start_column = -1;
	uint64_t mention_generation = 0;
	bool mention_picker_explicit = false;
	String mention_section; // empty = root; otherwise section id (solers/addons/files/folders/…)
	Control *model_popup_overlay = nullptr;
	// Cascading model menu: a compact root menu (Model / Effort / actions)
	// plus one lazily built submenu that opens beside the hovered row.
	PanelContainer *model_menu = nullptr;
	VBoxContainer *model_menu_box = nullptr;
	Button *model_menu_model_row = nullptr;
	Button *model_menu_effort_row = nullptr;
	PanelContainer *model_provider_menu = nullptr;
	ScrollContainer *model_provider_scroll = nullptr;
	VBoxContainer *model_provider_list = nullptr;
	PanelContainer *model_submenu = nullptr;
	ScrollContainer *model_submenu_scroll = nullptr;
	VBoxContainer *model_submenu_box = nullptr;
	LineEdit *model_submenu_search = nullptr;
	VBoxContainer *model_submenu_list = nullptr;
	int model_submenu_kind = 0; // 0 = closed, 1 = model, 2 = effort.
	uint64_t model_catalog_revision = 0;
	// Cached model rows for search filtering.
	Array model_submenu_entries;
	MarginContainer *permission_prompt_inset = nullptr;
	Label *permission_tool_label = nullptr;
	Label *permission_detail_label = nullptr;

	// Live turn state: cells updated in place as session events stream in.
	SolersThinkingCell *active_thinking_cell = nullptr;
	SolersAssistantCell *active_text_cell = nullptr;
	VBoxContainer *active_assistant_row = nullptr;
	int64_t pending_assistant_event_id = -1;
	SolersStatusCell *status_cell = nullptr;
	HashMap<String, SolersToolCell *> tool_cells_by_id;
	SolersToolCell *last_started_tool_cell = nullptr;
	int composer_margin_px = -1;
	bool scroll_to_bottom_deferred = false;

	Array pending_attachments;
	String session_project_path;
	String session_current_id;
	Array timeline_messages;
	bool timeline_window_updating = false;

	SolersObservationService *observation_service = nullptr;
	SolersToolRegistry *tool_registry = nullptr;
	SolersActionTimeline *action_timeline = nullptr;
	SolersPermissionManager *permission_manager = nullptr;
	SolersAgentSession *agent_session = nullptr;
	SolersSettingsService *settings_service = nullptr;
	Callable session_select_callback;
	Callable new_session_callback;

	void _refresh_status();
	void _refresh_model_chip();
	void _sync_layout_widths();
	void _sync_session_button();
	void _on_send_chat_pressed();
	void _on_stop_chat_pressed();
	void _toggle_session_sidebar();
	void _refresh_session_list();
	void _request_session_list_refresh();
	void _sync_session_selection();
	void _on_session_row_pressed(const String &p_session_id);
	void _append_history_message(const Dictionary &p_message);
	void _render_timeline();
	// A fit_content RichTextLabel reshapes all of its text inside
	// get_minimum_size(), so every live row costs a full re-wrap on each width
	// change. Rows stay in the tree 1:1 with the transcript, but only those near
	// the viewport carry cells; the rest pin their measured height.
	void _update_timeline_window();
	void _hydrate_timeline_row(Control *p_row, const Variant &p_entry);
	void _park_timeline_row(Control *p_row);
	VBoxContainer *_chat_mount() const;
	void _on_new_chat_pressed();
	void _on_model_chip_pressed();
	void _position_model_menu();
	void _open_model_submenu(int p_kind);
	void _open_provider_models(const Dictionary &p_config, Button *p_anchor_row);
	void _position_model_provider_menu(Button *p_anchor_row);
	void _rebuild_model_submenu_list(const String &p_filter = String());
	void _on_model_submenu_search(const String &p_text);
	void _position_model_submenu(Button *p_anchor_row);
	void _close_model_submenu();
	void _hide_model_popup();
	void _on_model_popup_overlay_gui_input(const Ref<InputEvent> &p_event);
	void _set_model_provider_from_popup(const String &p_provider, const String &p_model);
	void _set_reasoning_effort_from_popup(const String &p_effort);
	void _reset_model_defaults_from_popup();
	void _open_model_settings_from_popup();
	void _submit_chat_prompt(const String &p_prompt, const Array &p_attachments = Array());
	void _submit_steering(const String &p_prompt, const Array &p_attachments);
	void _on_agent_model_request_started();
	void _on_agent_timeline_entry_committed(int64_t p_event_id, const String &p_role);
	void _on_agent_assistant_delta(const String &p_text);
	void _on_agent_reasoning_delta(const String &p_text);
	void _on_agent_assistant_message(const String &p_text);
	void _on_agent_tool_started(const String &p_id, const String &p_name, const String &p_arguments);
	void _on_agent_tool_awaiting_approval(const String &p_id, const String &p_name);
	void _on_agent_tool_finished(const String &p_id, const String &p_name, const Dictionary &p_result, int p_duration_msec);
	void _on_agent_turn_completed(const Dictionary &p_result);
	void _on_agent_turn_failed(const Dictionary &p_error);
	void _on_agent_turn_retrying(int p_attempt, const String &p_message);
	void _on_agent_turn_waiting(const Dictionary &p_waiting);
	void _on_agent_plan_updated(const String &p_explanation, const Array &p_plan);
	void _sync_approval_panel();
	void _resolve_current_approval(bool p_approve, bool p_remember);
	void _on_chat_input_gui_input(const Ref<InputEvent> &p_event);
	void _on_chat_input_text_changed();
	Array _mention_inline_parse(const String &p_line_text);
	void _mention_inline_draw(const Dictionary &p_info, const Rect2 &p_rect);
	void _mention_inline_click(const Dictionary &p_info, const Rect2 &p_rect);
	bool _try_delete_mention_span(int p_direction);
	void _refresh_mention_popup();
	void _on_mention_search_changed(const String &p_text);
	void _on_mention_item_clicked(int p_index, const Vector2 &p_position, MouseButton p_button);
	void _on_mention_preview_ready(const String &p_path, const Ref<Texture2D> &p_preview, const Ref<Texture2D> &p_small_preview, uint64_t p_generation, int p_index);
	void _hide_mention_popup();
	void _select_mention(const Dictionary &p_mention);
	void _activate_mention_selection();
	void _move_mention_selection(int p_delta);
	void _on_add_context_pressed();
	bool _add_image_attachment_from_clipboard();
	void _refresh_attachment_bar();
	void _remove_attachment(int p_index);
	void _clear_attachments();
	void _update_send_enabled();
	bool _is_scroll_pinned() const;
	void _on_cell_content_changed();
	void _on_history_edit_requested(int64_t p_event_id, const String &p_prompt, const Array &p_attachments);
	void _on_history_edit_confirmed();
	void _scroll_chat_to_bottom();
	void _clear_empty_state();
	void _show_empty_state();
	SolersUserMessageCell *_append_user_message(const String &p_message, const Array &p_attachments = Array(), int64_t p_event_id = -1, int64_t p_wall = 0);
	void _append_error_row(const String &p_text);
	void _ensure_status_cell(const String &p_status);
	void _remove_status_cell();
	void _settle_thinking_cell();
	VBoxContainer *_ensure_assistant_row();
	SolersAssistantCell *_ensure_text_cell();
	void _finish_turn_cells();
	void _clear_chat_view(bool p_show_empty);
	Control *_create_empty_state() const;

protected:
	void _notification(int p_what);

public:
	void set_services(SolersObservationService *p_observation_service, SolersToolRegistry *p_tool_registry, SolersActionTimeline *p_action_timeline, SolersPermissionManager *p_permission_manager, SolersSettingsService *p_settings_service);
	void make_visible();
	void set_agent_session(SolersAgentSession *p_agent_session);
	void start_new_chat();
	void load_chat_history(const Array &p_messages);
	void set_session_select_callback(const Callable &p_callback);
	void set_new_session_callback(const Callable &p_callback);
	void set_session_context(const String &p_project_path, const String &p_session_id);
	void notify_sessions_changed();
	// Sole settings host (Editor + Project Manager). Category: plugins|llm|quick.
	void open_provider_settings(const String &p_category = "plugins");
	SolersPMAIView *get_provider_settings_view() const;

	SolersDock();
	~SolersDock();
};
