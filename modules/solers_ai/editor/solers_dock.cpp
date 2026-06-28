/**************************************************************************/
/*  solers_dock.cpp                                                       */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                              */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/
/* Solers: AI-native game engine.                                        */
/*                                                                        */
/* Native Codex-grade chat dock. Layout and text input are stock Godot   */
/* Controls (TextEdit gives OS-grade IME/CJK input and TextServer glyph  */
/* caching); every piece of chat chrome is self-drawn via the widget kit  */
/* in solers_chat_widgets.h. There is no embedded UI runtime: the dock    */
/* renders through the editor canvas like any other dock and costs zero   */
/* CPU at steady state.                                                   */
/**************************************************************************/

#include "solers_dock.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/input/input_event.h"
#include "core/io/json.h"
#include "core/version.h"
#include "editor/settings/editor_settings.h"
#include "editor/themes/editor_scale.h"
#include "modules/solers_ai/core/solers_action_timeline.h"
#include "modules/solers_ai/core/solers_agent_session.h"
#include "modules/solers_ai/core/solers_observation_service.h"
#include "modules/solers_ai/core/solers_permission_manager.h"
#include "modules/solers_ai/core/solers_settings_service.h"
#include "modules/solers_ai/core/solers_tool_registry.h"
#include "modules/solers_ai/editor/solers_chat_cells.h"
#include "modules/solers_ai/editor/solers_chat_widgets.h"
#include "modules/solers_ai/llm/solers_llm_message.h"
#include "modules/solers_ai/protocol/solers_mcp_adapter.h"
#include "modules/solers_ai/protocol/solers_rpc_server.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/label.h"
#include "scene/gui/margin_container.h"
#include "scene/gui/panel_container.h"
#include "scene/gui/scroll_bar.h"
#include "scene/gui/scroll_container.h"
#include "scene/gui/separator.h"
#include "scene/gui/text_edit.h"
#include "scene/resources/style_box.h"
#include "scene/resources/style_box_flat.h"

constexpr float SOLERS_COMPOSER_TEXT_MIN_HEIGHT = 48.0f;
constexpr float SOLERS_COMPOSER_TEXT_MAX_HEIGHT = 220.0f;
constexpr float SOLERS_COMPOSER_TOOLBAR_HEIGHT = 30.0f;
// Top/bottom composer padding. Keep the toolbar visually attached to the prompt.
constexpr float SOLERS_COMPOSER_VERTICAL_CHROME = 20.0f;

// Codex-calibrated surface palette.
static const Color SOLERS_BG = Color(0.070, 0.073, 0.078);
static const Color SOLERS_COMPOSER_BG = Color(0.086, 0.088, 0.092);
// Hairline: ultra-subtle separator between sections.
// Use a slightly warm gray instead of pure white to avoid cold "screen door" look.
static const Color SOLERS_HAIRLINE = Color(0.95, 0.95, 0.97, 0.035);
// Composer edge: a touch more defined than the section hairline so the input
// reads as a discrete card, like Cursor's composer.
static const Color SOLERS_COMPOSER_BORDER = Color(0.95, 0.95, 0.97, 0.16);
static const Color SOLERS_ACCENT_ORANGE = Color(1.00, 0.49, 0.20);
// Primary text: high contrast for readability on dark backgrounds.
static const Color SOLERS_TEXT_PRIMARY = Color(0.961, 0.969, 0.984);
// Body text: comfortable reading with slightly reduced contrast for hierarchy.
static const Color SOLERS_TEXT_BODY = Color(0.918, 0.929, 0.945);
// Dim text: secondary info, status labels, timestamps.
static const Color SOLERS_TEXT_DIM = Color(0.667, 0.690, 0.733);
// Placeholder text: subtle cue in the input field.
static const Color SOLERS_TEXT_PLACEHOLDER = Color(0.345, 0.357, 0.388);

static Ref<StyleBoxFlat> solers_make_stylebox(const Color &p_bg, const Color &p_border, int p_radius, int p_padding, bool p_shadow = false) {
	Ref<StyleBoxFlat> style(memnew(StyleBoxFlat));
	style->set_bg_color(p_bg);
	style->set_border_color(p_border);
	style->set_border_width_all(p_border.a > 0.0 ? 1 : 0);
	style->set_corner_radius_all(p_radius * EDSCALE);
	style->set_content_margin_all(p_padding * EDSCALE);
	if (p_shadow) {
		style->set_shadow_color(Color(0, 0, 0, 0.22));
		style->set_shadow_size(7 * EDSCALE);
		style->set_shadow_offset(Point2(0, 2 * EDSCALE));
	}
	return style;
}

static String solers_compact_model_label(const String &p_model) {
	const String model = p_model.strip_edges();
	if (model.length() <= 28) {
		return model;
	}
	return model.substr(0, 25) + "...";
}

static StringName solers_tool_glyph_for_metadata(const Dictionary &p_tool) {
	const String exposure = String(p_tool.get("exposure", String())).strip_edges();
	if (exposure == "hidden") {
		return SNAME("shield");
	}

	const String permission = String(p_tool.get("permission", "observe"));
	if (permission == "observe") {
		return exposure == "deferred" ? SNAME("sparkle") : SNAME("tool_observe");
	}
	if (permission == "edit_scene") {
		return SNAME("tool_scene");
	}
	if (permission == "edit_files") {
		return SNAME("tool_file");
	}
	if (permission == "run_project") {
		return SNAME("tool_run");
	}
	if (permission == "export_build") {
		return SNAME("tool_export");
	}
	if (permission == "network") {
		return SNAME("tool_network");
	}
	if (permission == "shell") {
		return SNAME("tool_shell");
	}

	const String mutation = String(p_tool.get("mutation_kind", "none"));
	if (mutation != "none") {
		return bool(p_tool.get("undoable", false)) ? SNAME("tool_scene") : SNAME("alert");
	}
	return bool(p_tool.get("requires_approval", false)) ? SNAME("shield") : SNAME("sparkle");
}

static StringName solers_tool_glyph_for_name(const SolersToolRegistry *p_registry, const String &p_name) {
	if (p_name.is_empty()) {
		return StringName();
	}
	if (!p_registry) {
		return SNAME("sparkle");
	}
	const Array tools = p_registry->list_tools();
	for (int i = 0; i < tools.size(); i++) {
		const Dictionary tool = tools[i];
		if (String(tool.get("name", String())) == p_name) {
			return solers_tool_glyph_for_metadata(tool);
		}
	}
	return SNAME("sparkle");
}

PanelContainer *SolersDock::_create_panel_card(const Color &p_color, const Color &p_border_color, int p_radius, int p_padding) const {
	PanelContainer *panel = memnew(PanelContainer);
	panel->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	panel->add_theme_style_override("panel", solers_make_stylebox(p_color, p_border_color, p_radius, p_padding));
	return panel;
}

Control *SolersDock::_create_empty_state() const {
	VBoxContainer *state = memnew(VBoxContainer);
	state->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	state->set_v_size_flags(Control::SIZE_SHRINK_CENTER);
	state->set_alignment(BoxContainer::ALIGNMENT_CENTER);
	state->add_theme_constant_override("separation", 0);

	Label *title = memnew(Label(TTR("What should we build?")));
	title->set_h_size_flags(Control::SIZE_SHRINK_CENTER);
	title->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	title->add_theme_color_override(SceneStringName(font_color), SOLERS_TEXT_PRIMARY);
	title->add_theme_font_size_override(SceneStringName(font_size), 28 * EDSCALE);
	state->add_child(title);

	return state;
}

void SolersDock::_sync_layout_widths() {
	if (!composer_inset) {
		return;
	}
	const float width = get_size().x;
	float margin = 20 * EDSCALE;
	if (width > 980 * EDSCALE) {
		const float target = MIN(width * 0.52f, 920 * EDSCALE);
		margin = MAX(margin, (width - target) * 0.5f);
	}
	composer_inset->add_theme_constant_override("margin_left", int(margin));
	composer_inset->add_theme_constant_override("margin_right", int(margin));
}

void SolersDock::_refresh_status() {
	// The only live status surfaces are the inline approval prompt and the
	// model chip; everything else here was wiring for the removed diagnostics.
	_sync_approval_panel();
	_refresh_model_chip();
}

void SolersDock::_refresh_model_chip() {
	if (!model_chip) {
		return;
	}

	if (!settings_service) {
		model_chip->set_texts(TTR("Model"), String());
		model_chip->set_tooltip_text(TTR("AI model settings are unavailable."));
		return;
	}

	const Dictionary provider_result = settings_service->get_provider_config();
	const Dictionary provider_data = provider_result.get("data", Dictionary());
	const String provider = String(provider_data.get("provider", String())).strip_edges();
	const String model = String(provider_data.get("model", String())).strip_edges();
	const String base_url = String(provider_data.get("base_url", String())).strip_edges();
	const Dictionary validation = provider_data.get("validation", Dictionary());
	const bool valid = validation.get("valid", false);

	if (model.is_empty()) {
		model_chip->set_texts(TTR("Model"), String());
		model_chip->set_tooltip_text(TTR("Choose a provider and model in AI Models."));
		return;
	}

	model_chip->set_texts(solers_compact_model_label(model), String());

	String tooltip = vformat(TTR("Model: %s\nProvider: %s"), model, provider.is_empty() ? TTR("unknown") : provider);
	if (!base_url.is_empty()) {
		tooltip += "\n" + vformat(TTR("Base URL: %s"), base_url);
	}
	tooltip += "\n" + String(valid ? TTR("Configuration is valid.") : TTR("Configuration needs attention in AI Models."));
	model_chip->set_tooltip_text(tooltip);
}

void SolersDock::_clear_empty_state() {
	if (empty_home) {
		empty_home->hide();
	}
	if (chat_scroll) {
		chat_scroll->show();
	}
	if (composer_inset && root_box && composer_inset->get_parent() != root_box) {
		if (composer_inset->get_parent()) {
			composer_inset->get_parent()->remove_child(composer_inset);
		}
		root_box->add_child(composer_inset);
	}
	_sync_layout_widths();
}

void SolersDock::_show_empty_state() {
	if (!empty_home || !composer_inset) {
		return;
	}
	if (chat_scroll) {
		chat_scroll->hide();
	}
	if (composer_inset->get_parent() != empty_home) {
		if (composer_inset->get_parent()) {
			composer_inset->get_parent()->remove_child(composer_inset);
		}
		empty_home->add_child(composer_inset);
	}
	empty_home->show();
	_sync_layout_widths();
	_update_chat_input_height();
}

void SolersDock::_scroll_chat_to_bottom() {
	if (!chat_scroll) {
		return;
	}
	VScrollBar *bar = chat_scroll->get_v_scroll_bar();
	if (bar) {
		bar->set_value(bar->get_max());
	}
}

bool SolersDock::_is_scroll_pinned() const {
	if (!chat_scroll) {
		return true;
	}
	VScrollBar *bar = chat_scroll->get_v_scroll_bar();
	if (!bar || !bar->is_visible()) {
		return true;
	}
	// Follow the stream only while the user is at (or near) the bottom; a
	// reader who scrolled up keeps their place.
	return bar->get_value() + bar->get_page() >= bar->get_max() - 48.0 * EDSCALE;
}

void SolersDock::_on_cell_content_changed() {
	if (_is_scroll_pinned()) {
		callable_mp(this, &SolersDock::_scroll_chat_to_bottom).call_deferred();
	}
}

void SolersDock::_append_user_message(const String &p_message) {
	chat_log += vformat("%sYou\n%s\n", chat_log.is_empty() ? "" : "\n", p_message);
	if (!message_list) {
		return;
	}
	_clear_empty_state();

	SolersUserBubble *bubble = memnew(SolersUserBubble);
	bubble->set_content_changed_callback(callable_mp(this, &SolersDock::_on_cell_content_changed));
	message_list->add_child(bubble);
	bubble->set_message(p_message);

	callable_mp(this, &SolersDock::_scroll_chat_to_bottom).call_deferred();
}

void SolersDock::_append_error_row(const String &p_text) {
	chat_log += vformat("%s\n", p_text);
	if (!message_list) {
		return;
	}
	_clear_empty_state();

	Label *row = memnew(Label(p_text));
	row->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	row->set_focus_mode(Control::FOCUS_ACCESSIBILITY);
	row->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	row->add_theme_color_override("font_color", Color(0.875, 0.478, 0.420));
	row->add_theme_font_size_override(SceneStringName(font_size), 12 * EDSCALE);
	message_list->add_child(row);

	callable_mp(this, &SolersDock::_scroll_chat_to_bottom).call_deferred();
}

void SolersDock::_ensure_status_cell(const String &p_status) {
	if (!message_list) {
		return;
	}
	_clear_empty_state();
	if (!status_cell) {
		status_cell = memnew(SolersStatusCell);
		message_list->add_child(status_cell);
	}
	// The status row always trails the latest content.
	message_list->move_child(status_cell, message_list->get_child_count() - 1);
	status_cell->set_status(p_status);
	_on_cell_content_changed();
}

void SolersDock::_remove_status_cell() {
	if (status_cell) {
		status_cell->queue_free();
		status_cell = nullptr;
	}
}

void SolersDock::_settle_thinking_cell() {
	if (active_thinking_cell && active_thinking_cell->is_active()) {
		active_thinking_cell->set_done();
	}
}

void SolersDock::_settle_tool_group() {
	// Close the current "N actions" batch; the next tool call opens a new one.
	if (active_tool_group) {
		active_tool_group->settle();
		active_tool_group = nullptr;
	}
}

SolersAssistantCell *SolersDock::_ensure_text_cell() {
	if (active_text_cell) {
		return active_text_cell;
	}
	_clear_empty_state();
	active_text_cell = memnew(SolersAssistantCell);
	active_text_cell->set_content_changed_callback(callable_mp(this, &SolersDock::_on_cell_content_changed));
	message_list->add_child(active_text_cell);
	if (status_cell) {
		message_list->move_child(status_cell, message_list->get_child_count() - 1);
	}
	return active_text_cell;
}

void SolersDock::_finish_turn_cells() {
	_settle_thinking_cell();
	_settle_tool_group();
	active_thinking_cell = nullptr;
	active_text_cell = nullptr;
	tool_cells_by_id.clear();
	last_started_tool_cell = nullptr;
	_remove_status_cell();
}

void SolersDock::_clear_chat_view(bool p_show_empty) {
	chat_log = String();
	active_thinking_cell = nullptr;
	active_text_cell = nullptr;
	status_cell = nullptr;
	active_tool_group = nullptr;
	tool_cells_by_id.clear();
	last_started_tool_cell = nullptr;
	if (message_list) {
		while (message_list->get_child_count() > 0) {
			Node *child = message_list->get_child(0);
			message_list->remove_child(child);
			child->queue_free();
		}
	}
	if (p_show_empty) {
		_show_empty_state();
	} else {
		_clear_empty_state();
	}
}

void SolersDock::_on_send_chat_pressed() {
	if (!chat_input) {
		return;
	}

	const String prompt = chat_input->get_text().strip_edges();
	if (prompt.is_empty()) {
		return;
	}
	chat_input->set_text("");
	_update_chat_input_height();
	_update_send_enabled();
	_refresh_model_chip();
	_submit_chat_prompt(prompt);
}

void SolersDock::_on_workspace_toggle_pressed() {
	if (workspace_toggle_callback.is_valid()) {
		workspace_toggle_callback.call();
	}
}

void SolersDock::_on_session_menu_pressed() {
	if (session_menu_callback.is_valid() && session_button) {
		session_menu_callback.call(session_button->get_screen_rect());
	}
}

void SolersDock::_on_model_chip_pressed() {
	_append_error_row(TTR("Open AI Settings from the project manager."));
}

void SolersDock::start_new_chat() {
	if (agent_session) {
		agent_session->reset_conversation();
	}
	_clear_chat_view(true);
	if (chat_input) {
		chat_input->set_text("");
		_update_chat_input_height();
		_update_send_enabled();
		chat_input->grab_focus();
	}
	_refresh_status();
}

void SolersDock::load_chat_history(const Array &p_messages) {
	_clear_chat_view(p_messages.is_empty());
	for (int i = 0; i < p_messages.size(); i++) {
		const Variant item = p_messages[i];
		if (item.get_type() != Variant::DICTIONARY) {
			continue;
		}
		const Dictionary message = item;
		const String role = message.get("role", String());
		const String content = message.get("content", String());
		if (content.is_empty()) {
			continue;
		}
		if (role == SolersLLMRole::USER) {
			_append_user_message(content);
		} else if (role == SolersLLMRole::ASSISTANT) {
			_on_agent_assistant_message(content);
		}
	}
	_finish_turn_cells();
	_refresh_status();
	callable_mp(this, &SolersDock::_scroll_chat_to_bottom).call_deferred();
}

void SolersDock::_submit_chat_prompt(const String &p_prompt) {
	const String prompt = p_prompt.strip_edges();
	if (prompt.is_empty()) {
		return;
	}

	_append_user_message(prompt);

	if (!agent_session) {
		_append_error_row(TTR("Agent session is unavailable."));
		return;
	}

	// Real BYOK end-to-end: hand the prompt to the single agent loop. The
	// session streams assistant text, tool calls and results back through the
	// signals wired in set_agent_session(); no mock, no hardcoded provider.
	Dictionary args;
	args["prompt"] = prompt;
	const Dictionary result = agent_session->start_turn(args);
	if (!(bool)result.get("ok", false)) {
		_remove_status_cell();
		const Dictionary error = result.get("error", Dictionary());
		_append_error_row(String::utf8("\u26a0 ") + String(error.get("message", "Could not start the agent turn.")));
	}
	_refresh_status();
}

void SolersDock::_on_chat_input_gui_input(const Ref<InputEvent> &p_event) {
	Ref<InputEventKey> key = p_event;
	if (key.is_null() || !key->is_pressed() || key->is_echo()) {
		return;
	}

	const Key keycode = key->get_keycode();
	if (keycode != Key::ENTER && keycode != Key::KP_ENTER) {
		return;
	}

	if (key->is_shift_pressed()) {
		chat_input->insert_text_at_caret("\n");
		_update_chat_input_height();
		chat_input->accept_event();
		return;
	}

	if (permission_manager && permission_manager->get_pending_request_count() > 0) {
		_submit_current_approval();
		chat_input->accept_event();
		return;
	}
	_on_send_chat_pressed();
	chat_input->accept_event();
}

void SolersDock::_on_chat_input_text_changed() {
	_update_chat_input_height();
	_update_send_enabled();
}

void SolersDock::_update_send_enabled() {
	if (send_chat_button && chat_input) {
		const bool blocked = permission_manager && permission_manager->get_pending_request_count() > 0;
		chat_input->set_editable(!blocked);
		send_chat_button->set_enabled(!blocked && !chat_input->get_text().strip_edges().is_empty());
	}
}

void SolersDock::_update_chat_input_height() {
	if (!chat_input || !chat_input->is_inside_tree()) {
		return;
	}

	const int line_height = MAX(1, chat_input->get_line_height());
	const int visible_rows = MAX(1, chat_input->get_total_visible_line_count());
	const float text_height = CLAMP(float(visible_rows * line_height) + 20.0f * EDSCALE, SOLERS_COMPOSER_TEXT_MIN_HEIGHT * EDSCALE, SOLERS_COMPOSER_TEXT_MAX_HEIGHT * EDSCALE);
	chat_input->set_custom_minimum_size(Size2(0, text_height));

	Control *composer_card = Object::cast_to<Control>(chat_input->get_parent() ? chat_input->get_parent()->get_parent() : nullptr);
	if (composer_card) {
		composer_card->set_custom_minimum_size(Size2(0, text_height + SOLERS_COMPOSER_TOOLBAR_HEIGHT * EDSCALE + SOLERS_COMPOSER_VERTICAL_CHROME * EDSCALE));
	}

	const int max_visible_rows = MAX(1, int((SOLERS_COMPOSER_TEXT_MAX_HEIGHT * EDSCALE) / line_height));
	if (visible_rows > max_visible_rows) {
		chat_input->set_v_scroll(MAX(0, chat_input->get_total_visible_line_count() - chat_input->get_visible_line_count()));
	}
}

void SolersDock::_sync_approval_panel() {
	if (!approval_overlay_inset) {
		return;
	}
	if (!permission_manager) {
		approval_overlay_inset->set_visible(false);
		_update_send_enabled();
		return;
	}

	Array pending = permission_manager->list_pending_requests();
	if (permission_manager->is_auto_approve_all()) {
		for (int i = 0; i < pending.size(); i++) {
			const Dictionary request = pending[i];
			permission_manager->approve_request(request.get("id", 0));
		}
		pending = permission_manager->list_pending_requests();
	}
	if (pending.is_empty()) {
		active_approval_id = 0;
		approval_overlay_inset->set_visible(false);
		_update_send_enabled();
		return;
	}

	const Dictionary request = pending[0];
	const int request_id = request.get("id", 0);
	if (active_approval_id != request_id) {
		active_approval_id = request_id;
		approval_choice = "once";
		approval_always_confirming = false;
	}
	approval_overlay_inset->set_visible(true);

	const String tool = String(request.get("tool", String()));
	const String permission = String(request.get("permission", String()));
	const Dictionary args = request.get("args", Dictionary());

	if (approval_tool_label) {
		approval_tool_label->set_text(tool);
	}
	if (approval_summary_label) {
		String summary = solers_summarize_tool_args(JSON::stringify(args, "", false, true));
		if (summary.is_empty()) {
			summary = permission;
		} else {
			summary = vformat("%s - %s", permission, summary);
		}
		approval_summary_label->set_text(summary);
	}
	_set_approval_choice(approval_choice);
	_update_send_enabled();
}

void SolersDock::_set_approval_choice(const String &p_choice) {
	approval_choice = p_choice;
	if (p_choice != "always") {
		approval_always_confirming = false;
	}
	if (approval_once_button) {
		approval_once_button->set_text(p_choice == "once" ? TTR("1  Allow once *") : TTR("1  Allow once"));
	}
	if (approval_always_button) {
		approval_always_button->set_text(p_choice == "always" ? TTR("2  Allow always *") : TTR("2  Allow always"));
	}
	if (approval_reject_button) {
		approval_reject_button->set_text(p_choice == "reject" ? TTR("3  Deny *") : TTR("3  Deny"));
	}
	if (approval_submit_button) {
		approval_submit_button->set_text(approval_always_confirming ? TTR("Confirm") : TTR("Submit"));
	}
}

void SolersDock::_submit_current_approval() {
	if (!permission_manager) {
		return;
	}
	Array pending = permission_manager->list_pending_requests();
	if (pending.is_empty()) {
		_refresh_status();
		return;
	}
	Dictionary request = pending[0];
	const int request_id = request.get("id", 0);
	if (approval_choice == "reject") {
		permission_manager->reject_request(request_id);
	} else if (approval_choice == "always") {
		if (!approval_always_confirming) {
			approval_always_confirming = true;
			_set_approval_choice("always");
			return;
		}
		const int permission_id = request.get("permission_id", (int)SolersPermissionManager::PERMISSION_OBSERVE);
		permission_manager->set_auto_approve_permission((SolersPermissionManager::Permission)permission_id, true);
		permission_manager->approve_request(request_id);
	} else {
		permission_manager->approve_request(request_id);
	}
	approval_always_confirming = false;
	_refresh_status();
	_sync_approval_panel();
}

void SolersDock::_set_auto_approve_mode(bool p_enabled, bool p_persist) {
	if (!permission_manager) {
		return;
	}
	permission_manager->set_auto_approve_all(p_enabled);
	if (approval_mode_chip) {
		approval_mode_chip->set_texts(p_enabled ? TTR("Auto") : TTR("Manual"), String());
		approval_mode_chip->set_tooltip_text(p_enabled ? TTR("Auto-approve each pending tool call once.") : TTR("Ask before mutating tool calls."));
	}
	if (p_persist && EditorSettings::get_singleton()) {
		EditorSettings::get_singleton()->set_project_metadata("solers", "auto_approve_mode", p_enabled);
	}
}

void SolersDock::_on_auto_approve_chip_pressed() {
	const bool enabled = permission_manager && permission_manager->is_auto_approve_all();
	_set_auto_approve_mode(!enabled, true);
	_sync_approval_panel();
}

void SolersDock::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			_sync_layout_widths();
			_update_chat_input_height();
			_update_send_enabled();
			_refresh_status();
		} break;
		case NOTIFICATION_RESIZED: {
			_sync_layout_widths();
		} break;
		case NOTIFICATION_THEME_CHANGED: {
			_update_chat_input_height();
			_refresh_model_chip();
		} break;
		case EditorSettings::NOTIFICATION_EDITOR_SETTINGS_CHANGED: {
			_refresh_status();
		} break;
		case NOTIFICATION_TRANSLATION_CHANGED: {
			_refresh_status();
		} break;
	}
}

void SolersDock::set_services(SolersObservationService *p_observation_service, SolersToolRegistry *p_tool_registry, SolersActionTimeline *p_action_timeline, SolersPermissionManager *p_permission_manager, SolersMCPAdapter *p_mcp_adapter, SolersRpcServer *p_rpc_server, SolersSettingsService *p_settings_service) {
	observation_service = p_observation_service;
	tool_registry = p_tool_registry;
	action_timeline = p_action_timeline;
	permission_manager = p_permission_manager;
	mcp_adapter = p_mcp_adapter;
	rpc_server = p_rpc_server;
	settings_service = p_settings_service;
	const bool auto_mode = EditorSettings::get_singleton() ? (bool)EditorSettings::get_singleton()->get_project_metadata("solers", "auto_approve_mode", false) : false;
	_set_auto_approve_mode(auto_mode, false);
	_refresh_status();
	_sync_approval_panel();
}

void SolersDock::make_visible() {
	_refresh_status();
}

void SolersDock::set_workspace_toggle_callback(const Callable &p_callback) {
	workspace_toggle_callback = p_callback;
	if (panel_button) {
		panel_button->set_visible(workspace_toggle_callback.is_valid());
	}
}

void SolersDock::set_session_menu_callback(const Callable &p_callback) {
	session_menu_callback = p_callback;
}

SolersDock::SolersDock() {
	set_name(TTRC("Solers"));
	set_custom_minimum_size(Size2(520 * EDSCALE, 0));
	set_h_size_flags(Control::SIZE_FILL);
	set_v_size_flags(Control::SIZE_EXPAND_FILL);
	add_theme_style_override("panel", solers_make_stylebox(SOLERS_BG, Color(0.16, 0.16, 0.17, 1), 0, 0));

	root_box = memnew(VBoxContainer);
	root_box->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	root_box->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	root_box->add_theme_constant_override("separation", 0);
	add_child(root_box);

	/* Topbar — chat actions left, workspace controls right. */

	MarginContainer *topbar_inset = memnew(MarginContainer);
	topbar_inset->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	topbar_inset->set_custom_minimum_size(Size2(0, 40 * EDSCALE));
	topbar_inset->add_theme_constant_override("margin_left", 10 * EDSCALE);
	topbar_inset->add_theme_constant_override("margin_right", 10 * EDSCALE);
	topbar_inset->add_theme_constant_override("margin_top", 5 * EDSCALE);
	topbar_inset->add_theme_constant_override("margin_bottom", 5 * EDSCALE);
	root_box->add_child(topbar_inset);

	HBoxContainer *topbar_content = memnew(HBoxContainer);
	topbar_content->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	topbar_content->add_theme_constant_override("separation", 4 * EDSCALE);
	topbar_inset->add_child(topbar_content);

	Control *topbar_spacer = memnew(Control);
	topbar_spacer->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	topbar_content->add_child(topbar_spacer);

	panel_button = memnew(SolersGlyphButton);
	panel_button->configure(SNAME("panel"), SolersGlyphButton::SKIN_GHOST, TTR("Toggle workspace"), 15);
	panel_button->set_pressed_callback(callable_mp(this, &SolersDock::_on_workspace_toggle_pressed));
	panel_button->hide();
	topbar_content->add_child(panel_button);

	session_button = memnew(SolersGlyphButton);
	session_button->configure(SNAME("history"), SolersGlyphButton::SKIN_GHOST, TTR("Sessions"), 15);
	session_button->set_pressed_callback(callable_mp(this, &SolersDock::_on_session_menu_pressed));
	topbar_content->add_child(session_button);

	/* Hidden diagnostics labels (kept for _refresh_status plumbing). */

	empty_home = memnew(VBoxContainer);
	empty_home->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	empty_home->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	empty_home->set_alignment(BoxContainer::ALIGNMENT_CENTER);
	empty_home->add_theme_constant_override("separation", 28 * EDSCALE);
	root_box->add_child(empty_home);

	empty_state = _create_empty_state();
	empty_home->add_child(empty_state);

	/* Conversation timeline. */

	chat_scroll = memnew(ScrollContainer);
	chat_scroll->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	chat_scroll->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	chat_scroll->set_horizontal_scroll_mode(ScrollContainer::SCROLL_MODE_DISABLED);
	chat_scroll->hide();
	root_box->add_child(chat_scroll);

	message_list = memnew(VBoxContainer);
	message_list->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	message_list->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	message_list->add_theme_constant_override("separation", 14 * EDSCALE);

	MarginContainer *timeline_inset = memnew(MarginContainer);
	timeline_inset->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	timeline_inset->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	timeline_inset->add_theme_constant_override("margin_left", 20 * EDSCALE);
	timeline_inset->add_theme_constant_override("margin_right", 20 * EDSCALE);
	timeline_inset->add_theme_constant_override("margin_top", 10 * EDSCALE);
	timeline_inset->add_theme_constant_override("margin_bottom", 12 * EDSCALE);
	timeline_inset->add_child(message_list);
	chat_scroll->add_child(timeline_inset);

	/* Approval prompt — shown inline above the composer when a tool is blocked. */

	approval_overlay_inset = memnew(MarginContainer);
	approval_overlay_inset->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	approval_overlay_inset->add_theme_constant_override("margin_left", 20 * EDSCALE);
	approval_overlay_inset->add_theme_constant_override("margin_right", 20 * EDSCALE);
	approval_overlay_inset->add_theme_constant_override("margin_top", 0);
	approval_overlay_inset->add_theme_constant_override("margin_bottom", 8 * EDSCALE);
	approval_overlay_inset->set_visible(false);
	root_box->add_child(approval_overlay_inset);

	approval_overlay_card = _create_panel_card(Color(0.104, 0.106, 0.112), Color(1.0, 0.49, 0.20, 0.34), 14, 12);
	approval_overlay_card->set_custom_minimum_size(Size2(0, 118 * EDSCALE));
	approval_overlay_inset->add_child(approval_overlay_card);

	VBoxContainer *approval_box = memnew(VBoxContainer);
	approval_box->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	approval_box->add_theme_constant_override("separation", 4 * EDSCALE);
	approval_overlay_card->add_child(approval_box);

	HBoxContainer *approval_header = memnew(HBoxContainer);
	approval_header->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	approval_box->add_child(approval_header);

	Label *approval_title = memnew(Label(TTR("Allow using this tool?")));
	approval_header->add_child(approval_title);

	approval_tool_label = memnew(Label);
	approval_tool_label->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	approval_tool_label->set_clip_text(true);
	approval_tool_label->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	approval_tool_label->add_theme_color_override("font_color", SOLERS_TEXT_BODY);
	approval_header->add_child(approval_tool_label);

	approval_summary_label = memnew(Label);
	approval_summary_label->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	approval_summary_label->set_clip_text(true);
	approval_summary_label->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	approval_summary_label->add_theme_color_override("font_color", SOLERS_TEXT_DIM);
	approval_box->add_child(approval_summary_label);

	HBoxContainer *approval_actions = memnew(HBoxContainer);
	approval_actions->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	approval_box->add_child(approval_actions);

	approval_once_button = memnew(Button(TTR("Allow once")));
	approval_once_button->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	approval_once_button->connect(SceneStringName(pressed), callable_mp(this, &SolersDock::_set_approval_choice).bind("once"));
	approval_actions->add_child(approval_once_button);

	approval_always_button = memnew(Button(TTR("Allow always")));
	approval_always_button->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	approval_always_button->connect(SceneStringName(pressed), callable_mp(this, &SolersDock::_set_approval_choice).bind("always"));
	approval_actions->add_child(approval_always_button);

	approval_reject_button = memnew(Button(TTR("Deny")));
	approval_reject_button->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	approval_reject_button->connect(SceneStringName(pressed), callable_mp(this, &SolersDock::_set_approval_choice).bind("reject"));
	approval_actions->add_child(approval_reject_button);

	approval_submit_button = memnew(Button(TTR("Submit")));
	approval_submit_button->connect(SceneStringName(pressed), callable_mp(this, &SolersDock::_submit_current_approval));
	approval_box->add_child(approval_submit_button);
	/* Composer — one floating rounded card owns text entry and actions. */

	composer_inset = memnew(MarginContainer);
	composer_inset->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	composer_inset->add_theme_constant_override("margin_left", 20 * EDSCALE);
	composer_inset->add_theme_constant_override("margin_right", 20 * EDSCALE);
	composer_inset->add_theme_constant_override("margin_top", 4 * EDSCALE);
	composer_inset->add_theme_constant_override("margin_bottom", 13 * EDSCALE);
	empty_home->add_child(composer_inset);

	SolersSurface *composer_card = memnew(SolersSurface);
	composer_card->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	composer_card->configure(SOLERS_COMPOSER_BG, SOLERS_COMPOSER_BORDER, 19, 14, true);
	composer_card->set_custom_minimum_size(Size2(0, (SOLERS_COMPOSER_TEXT_MIN_HEIGHT + SOLERS_COMPOSER_TOOLBAR_HEIGHT + SOLERS_COMPOSER_VERTICAL_CHROME) * EDSCALE));
	composer_inset->add_child(composer_card);

	VBoxContainer *composer = memnew(VBoxContainer);
	composer->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	composer->add_theme_constant_override("separation", 0);
	composer_card->add_child(composer);

	chat_input = memnew(TextEdit);
	chat_input->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	chat_input->set_custom_minimum_size(Size2(0, SOLERS_COMPOSER_TEXT_MIN_HEIGHT * EDSCALE));
	chat_input->set_line_wrapping_mode(TextEdit::LINE_WRAPPING_BOUNDARY);
	chat_input->set_placeholder(TTR("Make, test, iterate..."));
	chat_input->set_smooth_scroll_enabled(true);
	chat_input->set_scroll_past_end_of_file_enabled(false);
	chat_input->set_fit_content_height_enabled(false);
	chat_input->set_indent_wrapped_lines(false);
	chat_input->set_highlight_current_line(false);
	chat_input->set_draw_minimap(false);
	chat_input->set_caret_blink_enabled(true);
	chat_input->add_theme_style_override("normal", memnew(StyleBoxEmpty));
	chat_input->add_theme_style_override("focus", memnew(StyleBoxEmpty));
	chat_input->add_theme_style_override("read_only", memnew(StyleBoxEmpty));
	chat_input->add_theme_color_override("font_color", SOLERS_TEXT_PRIMARY);
	chat_input->add_theme_color_override("font_placeholder_color", SOLERS_TEXT_PLACEHOLDER);
	chat_input->add_theme_color_override("background_color", Color(0, 0, 0, 0));
	chat_input->add_theme_color_override("caret_color", Color(0.86, 0.91, 0.98, 1));
	chat_input->add_theme_color_override("selection_color", Color(0.10, 0.42, 0.62, 0.56));
	chat_input->add_theme_constant_override("line_spacing", 4 * EDSCALE);
	chat_input->add_theme_font_size_override(SceneStringName(font_size), 14 * EDSCALE);
	chat_input->connect(SceneStringName(gui_input), callable_mp(this, &SolersDock::_on_chat_input_gui_input));
	chat_input->connect(SceneStringName(text_changed), callable_mp(this, &SolersDock::_on_chat_input_text_changed));
	composer->add_child(chat_input);

	HBoxContainer *composer_toolbar = memnew(HBoxContainer);
	composer_toolbar->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	composer_toolbar->set_custom_minimum_size(Size2(0, SOLERS_COMPOSER_TOOLBAR_HEIGHT * EDSCALE));
	composer_toolbar->set_alignment(BoxContainer::ALIGNMENT_BEGIN);
	composer_toolbar->add_theme_constant_override("separation", 6 * EDSCALE);
	composer->add_child(composer_toolbar);

	add_context_button = memnew(SolersGlyphButton);
	add_context_button->configure(SNAME("plus"), SolersGlyphButton::SKIN_GHOST, TTR("Attach context"), 15);
	add_context_button->set_v_size_flags(Control::SIZE_SHRINK_CENTER);
	composer_toolbar->add_child(add_context_button);

	approval_mode_chip = memnew(SolersSelectChip);
	approval_mode_chip->configure(SNAME("shield"), TTR("Manual"), String(), TTR("Ask before mutating tool calls."));
	approval_mode_chip->set_pressed_callback(callable_mp(this, &SolersDock::_on_auto_approve_chip_pressed));
	composer_toolbar->add_child(approval_mode_chip);

	Control *toolbar_spacer = memnew(Control);
	toolbar_spacer->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	composer_toolbar->add_child(toolbar_spacer);

	model_chip = memnew(SolersSelectChip);
	model_chip->configure(StringName(), TTR("Model"), String(), TTR("Model and provider"));
	model_chip->set_pressed_callback(callable_mp(this, &SolersDock::_on_model_chip_pressed));
	composer_toolbar->add_child(model_chip);

	send_chat_button = memnew(SolersGlyphButton);
	send_chat_button->configure(SNAME("send_up"), SolersGlyphButton::SKIN_PRIMARY, TTR("Send"), 16);
	send_chat_button->set_v_size_flags(Control::SIZE_SHRINK_CENTER);
	send_chat_button->set_pressed_callback(callable_mp(this, &SolersDock::_on_send_chat_pressed));
	send_chat_button->set_enabled(false);
	composer_toolbar->add_child(send_chat_button);

	_update_chat_input_height();
}

SolersDock::~SolersDock() {
	// The dock is the sole consumer of the glyph cache; release the textures
	// with it so nothing lives past renderer teardown.
	SolersChatGlyphs::clear_cache();
}

void SolersDock::set_agent_session(SolersAgentSession *p_agent_session) {
	agent_session = p_agent_session;
	if (!agent_session) {
		return;
	}
	agent_session->connect(SNAME("model_request_started"), callable_mp(this, &SolersDock::_on_agent_model_request_started));
	agent_session->connect(SNAME("assistant_delta"), callable_mp(this, &SolersDock::_on_agent_assistant_delta));
	agent_session->connect(SNAME("reasoning_delta"), callable_mp(this, &SolersDock::_on_agent_reasoning_delta));
	agent_session->connect(SNAME("assistant_message"), callable_mp(this, &SolersDock::_on_agent_assistant_message));
	agent_session->connect(SNAME("tool_call_started"), callable_mp(this, &SolersDock::_on_agent_tool_started));
	agent_session->connect(SNAME("tool_call_updated"), callable_mp(this, &SolersDock::_on_agent_tool_updated));
	agent_session->connect(SNAME("tool_call_awaiting_approval"), callable_mp(this, &SolersDock::_on_agent_tool_awaiting_approval));
	agent_session->connect(SNAME("tool_call_finished"), callable_mp(this, &SolersDock::_on_agent_tool_finished));
	agent_session->connect(SNAME("turn_completed"), callable_mp(this, &SolersDock::_on_agent_turn_completed));
	agent_session->connect(SNAME("turn_failed"), callable_mp(this, &SolersDock::_on_agent_turn_failed));
	agent_session->connect(SNAME("turn_retrying"), callable_mp(this, &SolersDock::_on_agent_turn_retrying));
}

void SolersDock::_on_agent_model_request_started() {
	// Covers both the first request and every follow-up after a tool batch.
	_ensure_status_cell(TTR("Waiting for model"));
}

void SolersDock::_on_agent_reasoning_delta(const String &p_text) {
	if (p_text.is_empty() || !message_list) {
		return;
	}
	_clear_empty_state();
	_remove_status_cell();
	_settle_tool_group();
	if (!active_thinking_cell || !active_thinking_cell->is_active()) {
		active_thinking_cell = memnew(SolersThinkingCell);
		active_thinking_cell->set_content_changed_callback(callable_mp(this, &SolersDock::_on_cell_content_changed));
		message_list->add_child(active_thinking_cell);
	}
	active_thinking_cell->append_reasoning(p_text);
	_on_cell_content_changed();
}

void SolersDock::_on_agent_assistant_delta(const String &p_text) {
	if (p_text.is_empty() || !message_list) {
		return;
	}
	// The model moved from thinking to answering.
	_settle_thinking_cell();
	_settle_tool_group();
	_remove_status_cell();
	_ensure_text_cell()->append_delta(p_text);
}

void SolersDock::_on_agent_assistant_message(const String &p_text) {
	const String text = p_text.strip_edges();
	chat_log += vformat("Solers\n%s\n", text);
	_settle_tool_group();
	if (active_text_cell) {
		// Authoritative final text for this model step; unchanged streams only
		// drop the caret, avoiding a second full markdown layout.
		active_text_cell->finalize(p_text);
		active_text_cell = nullptr;
		return;
	}
	if (text.is_empty() || !message_list) {
		return;
	}
	// Provider without streaming: materialize the step in one piece.
	_settle_thinking_cell();
	_remove_status_cell();
	_clear_empty_state();
	SolersAssistantCell *cell = memnew(SolersAssistantCell);
	cell->set_content_changed_callback(callable_mp(this, &SolersDock::_on_cell_content_changed));
	message_list->add_child(cell);
	cell->set_full_text_immediate(p_text);
	_on_cell_content_changed();
}

void SolersDock::_on_agent_tool_started(const String &p_id, const String &p_name, const String &p_arguments) {
	if (!message_list) {
		return;
	}
	if (!p_id.is_empty()) {
		SolersToolCell **found = tool_cells_by_id.getptr(p_id);
		if (found && *found) {
			(*found)->update(p_name, p_arguments, solers_tool_glyph_for_name(tool_registry, p_name));
			last_started_tool_cell = *found;
			_on_cell_content_changed();
			return;
		}
	}
	_settle_thinking_cell();
	_remove_status_cell();
	_clear_empty_state();

	if (!active_tool_group) {
		active_tool_group = memnew(SolersToolGroupCell);
		active_tool_group->set_content_changed_callback(callable_mp(this, &SolersDock::_on_cell_content_changed));
		message_list->add_child(active_tool_group);
	}
	SolersToolCell *cell = active_tool_group->add_tool();
	cell->start(p_name, p_arguments, solers_tool_glyph_for_name(tool_registry, p_name));
	if (!p_id.is_empty()) {
		tool_cells_by_id.insert(p_id, cell);
	}
	last_started_tool_cell = cell;
	_on_cell_content_changed();
}

void SolersDock::_on_agent_tool_updated(const String &p_id, const String &p_name, const String &p_arguments) {
	if (!p_id.is_empty()) {
		SolersToolCell **found = tool_cells_by_id.getptr(p_id);
		if (found && *found) {
			(*found)->update(p_name, p_arguments, solers_tool_glyph_for_name(tool_registry, p_name));
			_on_cell_content_changed();
			return;
		}
	}
	_on_agent_tool_started(p_id, p_name, p_arguments);
}

void SolersDock::_on_agent_tool_awaiting_approval(const String &p_id, const String &p_name) {
	// The call is parked on the permission gate. Its tool cell keeps spinning
	// (the call really is in progress) while we surface the approval prompt;
	// the session resolves the same call in place the moment the user decides.
	_ensure_status_cell(TTR("Awaiting approval"));
	_refresh_status();
	_on_cell_content_changed();
}

void SolersDock::_on_agent_tool_finished(const String &p_id, const String &p_name, const Dictionary &p_result, int p_duration_msec) {
	SolersToolCell *cell = nullptr;
	if (!p_id.is_empty()) {
		SolersToolCell **found = tool_cells_by_id.getptr(p_id);
		if (found) {
			cell = *found;
			tool_cells_by_id.erase(p_id);
		}
	}
	if (!cell) {
		cell = last_started_tool_cell;
	}

	const bool ok = p_result.get("ok", false);
	String error_message;
	if (!ok) {
		const Dictionary error = p_result.get("error", Dictionary());
		error_message = error.get("message", String());
	}
	chat_log += vformat("%s %s%s\n", ok ? "[ok]" : "[error]", p_name, error_message.is_empty() ? String() : " - " + error_message);

	if (cell) {
		cell->finish(ok, error_message, p_duration_msec);
	}
	_remove_status_cell();
	if (active_tool_group) {
		active_tool_group->note_finished(ok);
	}
	if (cell == last_started_tool_cell) {
		last_started_tool_cell = nullptr;
	}
	_refresh_status();
	_on_cell_content_changed();
}

void SolersDock::_on_agent_turn_completed(const Dictionary &p_result) {
	_finish_turn_cells();
	_refresh_status();
}

void SolersDock::_on_agent_turn_failed(const Dictionary &p_error) {
	_finish_turn_cells();
	_append_error_row(String::utf8("\u26a0 ") + String(p_error.get("message", "Agent turn failed.")));
	_refresh_status();
}

void SolersDock::_on_agent_turn_retrying(int p_attempt, const String &p_message) {
	// A transient provider/connection failure is being retried with backoff;
	// keep the turn alive and show a shimmer status instead of an error row.
	_settle_thinking_cell();
	_ensure_status_cell(vformat(TTR("Reconnecting (attempt %d)..."), p_attempt));
}
