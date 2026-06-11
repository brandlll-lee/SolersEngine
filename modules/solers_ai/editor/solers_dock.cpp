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
#include "core/io/file_access.h"
#include "core/io/image.h"
#include "core/io/json.h"
#include "core/os/os.h"
#include "core/version.h"
#include "editor/settings/editor_settings.h"
#include "editor/themes/editor_scale.h"
#include "modules/solers_ai/core/solers_action_timeline.h"
#include "modules/solers_ai/core/solers_agent_runtime.h"
#include "modules/solers_ai/core/solers_agent_session.h"
#include "modules/solers_ai/core/solers_observation_service.h"
#include "modules/solers_ai/core/solers_permission_manager.h"
#include "modules/solers_ai/core/solers_settings_service.h"
#include "modules/solers_ai/core/solers_tool_registry.h"
#include "modules/solers_ai/editor/solers_chat_widgets.h"
#include "modules/solers_ai/protocol/solers_mcp_adapter.h"
#include "modules/solers_ai/protocol/solers_rpc_server.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/check_button.h"
#include "scene/gui/label.h"
#include "scene/gui/margin_container.h"
#include "scene/gui/panel_container.h"
#include "scene/gui/rich_text_label.h"
#include "scene/gui/scroll_bar.h"
#include "scene/gui/scroll_container.h"
#include "scene/gui/separator.h"
#include "scene/gui/text_edit.h"
#include "scene/gui/texture_rect.h"
#include "scene/resources/image_texture.h"
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
static const Color SOLERS_ACCENT_ORANGE = Color(1.00, 0.49, 0.20);
// Alert tint for the access control.
static const Color SOLERS_ACCENT_AMBER = Color(1.00, 0.49, 0.20);
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

static Ref<Texture2D> solers_load_logo_texture() {
	Vector<String> candidates;
	const String cwd = OS::get_singleton()->get_cwd();
	const String exe_dir = OS::get_singleton()->get_executable_path().get_base_dir();
	const String source_root = exe_dir.get_base_dir();
	const String repo_root = source_root.get_base_dir();

	candidates.push_back(repo_root.path_join("branding/generated/solers02_icon_transparent_1024.png"));
	candidates.push_back(repo_root.path_join("branding/generated/solers_splash_icon_transparent_800x600.png"));
	candidates.push_back(cwd.path_join("icon.png"));
	candidates.push_back(cwd.path_join("main/app_icon.png"));
	candidates.push_back(source_root.path_join("icon.png"));
	candidates.push_back(source_root.path_join("main/app_icon.png"));
	candidates.push_back(exe_dir.path_join("icon.png"));

	for (const String &path : candidates) {
		if (!FileAccess::exists(path)) {
			continue;
		}
		Ref<Image> image = Image::load_from_file(path);
		if (image.is_valid() && !image->is_empty()) {
			return ImageTexture::create_from_image(image);
		}
	}
	return Ref<Texture2D>();
}

static float solers_longest_line_width(const String &p_text, const Ref<Font> &p_font, int p_font_size) {
	if (p_font.is_null()) {
		return 0.0f;
	}
	float width = 0.0f;
	for (const String &line : p_text.split("\n")) {
		width = MAX(width, p_font->get_string_size(line, HORIZONTAL_ALIGNMENT_LEFT, -1, p_font_size).x);
	}
	return width;
}

static String solers_compact_model_label(const String &p_model) {
	const String model = p_model.strip_edges();
	if (model.length() <= 28) {
		return model;
	}
	return model.substr(0, 25) + "...";
}

Label *SolersDock::_create_section_label(const String &p_text) {
	Label *label = memnew(Label(p_text));
	label->set_theme_type_variation("HeaderSmall");
	label->set_focus_mode(Control::FOCUS_ACCESSIBILITY);
	return label;
}

PanelContainer *SolersDock::_create_panel_card(const Color &p_color, const Color &p_border_color, int p_radius, int p_padding) const {
	PanelContainer *panel = memnew(PanelContainer);
	panel->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	panel->add_theme_style_override("panel", solers_make_stylebox(p_color, p_border_color, p_radius, p_padding));
	return panel;
}

Label *SolersDock::_create_body_label(const String &p_text, bool p_bold) const {
	Label *label = memnew(Label(p_text));
	label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	label->set_focus_mode(Control::FOCUS_ACCESSIBILITY);
	label->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	if (p_bold) {
		label->set_theme_type_variation("HeaderSmall");
	}
	return label;
}

Control *SolersDock::_create_empty_state() const {
	// Codex-minimal: a single, deliberately faded brand glyph centered in the
	// canvas. No headline, no subtitle — the composer placeholder carries the
	// only call to action, so the empty state stays calm and uncluttered.
	VBoxContainer *state = memnew(VBoxContainer);
	state->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	state->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	state->set_alignment(BoxContainer::ALIGNMENT_CENTER);
	state->add_theme_constant_override("separation", 0);

	Ref<Texture2D> logo = solers_load_logo_texture();
	if (logo.is_valid()) {
		TextureRect *logo_rect = memnew(TextureRect);
		logo_rect->set_texture(logo);
		logo_rect->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
		logo_rect->set_stretch_mode(TextureRect::STRETCH_KEEP_ASPECT_CENTERED);
		logo_rect->set_custom_minimum_size(Size2(60 * EDSCALE, 60 * EDSCALE));
		logo_rect->set_h_size_flags(Control::SIZE_SHRINK_CENTER);
		// Ghosted: a faint watermark, like the Codex empty canvas.
		logo_rect->set_self_modulate(Color(1, 1, 1, 0.13f));
		state->add_child(logo_rect);
	}

	return state;
}

void SolersDock::_refresh_status() {
	if (!project_status_label) {
		return;
	}

	if (!observation_service) {
		const String project_name = GLOBAL_GET("application/config/name");
		project_status_label->set_text(project_name.is_empty() ? TTR("No named project loaded") : vformat(TTR("Project: %s"), project_name));
		runtime_status_label->set_text(TTR("Runtime: idle"));
		return;
	}

	Dictionary snapshot = observation_service->get_editor_snapshot(2, 16);
	Dictionary project = snapshot.get("project", Dictionary());
	Dictionary open_scenes = snapshot.get("open_scenes", Dictionary());
	Dictionary scene_tree = snapshot.get("scene_tree", Dictionary());
	Dictionary selection = snapshot.get("selection", Dictionary());
	Dictionary runtime = snapshot.get("runtime", Dictionary());

	const String project_name = project.get("name", String());
	const String resource_path = project.get("resource_path", String());
	project_status_label->set_text(project_name.is_empty() ? vformat(TTR("Project path: %s"), resource_path) : vformat(TTR("Project: %s"), project_name));

	const bool is_playing = runtime.get("is_playing", false);
	const String playing_scene = runtime.get("playing_scene", String());
	runtime_status_label->set_text(is_playing ? vformat(TTR("Runtime: playing %s"), playing_scene) : TTR("Runtime: idle"));

	const bool has_scene = scene_tree.get("has_edited_scene", false);
	const String current_scene_path = open_scenes.get("current_scene_path", String());
	const int open_scene_count = open_scenes.get("count", 0);
	scene_status_label->set_text(has_scene ? vformat(TTR("Scene: %s (%d open)"), current_scene_path.is_empty() ? TTR("unsaved") : current_scene_path, open_scene_count) : TTR("Scene: no edited scene"));

	const int selected_count = selection.get("count", 0);
	selection_status_label->set_text(vformat(TTR("Selection: %d node(s)"), selected_count));

	const bool scene_mutations_enabled = permission_manager ? permission_manager->get_auto_approve_permission(SolersPermissionManager::PERMISSION_EDIT_SCENE) : false;
	const bool file_saves_enabled = permission_manager ? permission_manager->get_auto_approve_permission(SolersPermissionManager::PERMISSION_EDIT_FILES) : false;
	const bool run_project_enabled = permission_manager ? permission_manager->get_auto_approve_permission(SolersPermissionManager::PERMISSION_RUN_PROJECT) : false;
	tool_status_label->set_text(tool_registry ? vformat(TTR("Tools: %d registered, scene edits %s, file writes %s, run %s"), tool_registry->get_tool_count(), scene_mutations_enabled ? TTR("on") : TTR("locked"), file_saves_enabled ? TTR("on") : TTR("locked"), run_project_enabled ? TTR("on") : TTR("locked")) : TTR("Tools: unavailable"));

	Dictionary agent_status = agent_runtime ? agent_runtime->get_status() : Dictionary();
	agent_status_label->set_text(agent_runtime ? vformat(TTR("Agent: %s (turn %d)"), String(agent_status.get("state", "unknown")), (int)agent_status.get("turn_id", 0)) : TTR("Agent: unavailable"));

	Dictionary protocol_status = mcp_adapter ? mcp_adapter->get_status() : Dictionary();
	Dictionary rpc_status = rpc_server ? rpc_server->get_status() : Dictionary();
	if (mcp_adapter && rpc_server) {
		const bool rpc_running = rpc_status.get("running", false);
		protocol_status_label->set_text(rpc_running ? vformat(TTR("Protocol: MCP adapter ready, RPC 127.0.0.1:%d (%d client(s)), %d tool(s)"), (int)rpc_status.get("port", 0), (int)rpc_status.get("clients", 0), (int)protocol_status.get("tools_available", 0)) : vformat(TTR("Protocol: MCP adapter ready, RPC stopped, %d tool(s)"), (int)protocol_status.get("tools_available", 0)));
	} else {
		protocol_status_label->set_text(TTR("Protocol: unavailable"));
	}

	if (settings_service && provider_status_label) {
		Dictionary provider_result = settings_service->get_provider_config();
		Dictionary provider_data = provider_result.get("data", Dictionary());
		Dictionary validation = provider_data.get("validation", Dictionary());
		const String provider = provider_data.get("provider", String("unknown"));
		const bool privacy_mode = provider_data.get("privacy_mode", true);
		const bool api_key_configured = provider_data.get("api_key_configured", false);
		const bool valid = validation.get("valid", false);
		Array warnings = validation.get("warnings", Array());
		Array blockers = validation.get("blockers", Array());
		provider_status_label->set_text(vformat(TTR("Provider: %s, privacy %s, key %s, config %s (%d warning(s), %d blocker(s))"), provider, privacy_mode ? TTR("local-only") : TTR("network-enabled"), api_key_configured ? TTR("set") : TTR("not set"), valid ? TTR("valid") : TTR("blocked"), warnings.size(), blockers.size()));
	} else if (provider_status_label) {
		provider_status_label->set_text(TTR("Provider: unavailable"));
	}

	const int pending_approval_count = permission_manager ? permission_manager->get_pending_request_count() : 0;
	approval_status_label->set_text(permission_manager ? vformat(TTR("Approvals: %d pending"), pending_approval_count) : TTR("Approvals: unavailable"));

	timeline_status_label->set_text(action_timeline ? vformat(TTR("Timeline: %d event(s)"), action_timeline->get_action_count()) : TTR("Timeline: unavailable"));

	if (snapshot_preview) {
		Array tools = tool_registry ? tool_registry->list_tools() : Array();
		String preview = vformat(TTR("Snapshot ready. Open scenes: %d, selected nodes: %d, tools: %d."), open_scene_count, selected_count, tools.size());
		snapshot_preview->set_text(preview);
	}

	_refresh_model_chip();
}

void SolersDock::_refresh_model_chip() {
	if (!model_chip) {
		return;
	}

	if (!settings_service) {
		model_chip->set_texts(TTR("Model"), TTR("Unavailable"));
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
		model_chip->set_texts(TTR("Model"), TTR("Not set"));
		model_chip->set_tooltip_text(TTR("Choose a provider and model in AI Models."));
		return;
	}

	model_chip->set_texts(solers_compact_model_label(model), provider);

	String tooltip = vformat(TTR("Model: %s\nProvider: %s"), model, provider.is_empty() ? TTR("unknown") : provider);
	if (!base_url.is_empty()) {
		tooltip += "\n" + vformat(TTR("Base URL: %s"), base_url);
	}
	tooltip += "\n" + String(valid ? TTR("Configuration is valid.") : TTR("Configuration needs attention in AI Models."));
	model_chip->set_tooltip_text(tooltip);
}

void SolersDock::_on_refresh_pressed() {
	if (tool_registry) {
		Dictionary args;
		args["max_scene_depth"] = 2;
		args["max_children_per_node"] = 16;
		tool_registry->call_tool("editor.get_snapshot", args);
	}
	_refresh_status();
}

void SolersDock::_on_run_loopback_probe_pressed() {
	if (!agent_runtime) {
		return;
	}

	Array tool_calls;
	Dictionary snapshot_call;
	snapshot_call["name"] = "editor.get_snapshot";
	Dictionary snapshot_args;
	snapshot_args["max_scene_depth"] = 2;
	snapshot_args["max_children_per_node"] = 16;
	snapshot_call["arguments"] = snapshot_args;
	tool_calls.push_back(snapshot_call);

	Dictionary timeline_call;
	timeline_call["name"] = "timeline.list_actions";
	Dictionary timeline_args;
	timeline_args["limit"] = 20;
	timeline_call["arguments"] = timeline_args;
	tool_calls.push_back(timeline_call);

	Dictionary turn;
	turn["objective"] = "Solers v0.1 internal loopback probe";
	turn["mode"] = "tool_batch";
	turn["tool_calls"] = tool_calls;
	Dictionary result = agent_runtime->start_turn(turn);
	if (snapshot_preview) {
		snapshot_preview->set_text(JSON::stringify(result, "\t", false, true));
	}
	_refresh_status();
}

void SolersDock::_clear_empty_state() {
	if (empty_state) {
		empty_state->queue_free();
		empty_state = nullptr;
	}
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

void SolersDock::_append_chat_message(const String &p_speaker, const String &p_message) {
	_clear_active_reasoning();
	chat_log += vformat("%s%s\n%s\n", chat_log.is_empty() ? "" : "\n", p_speaker, p_message);
	if (!message_list) {
		return;
	}
	_clear_empty_state();

	const bool is_user = p_speaker == "You";
	if (is_user) {
		// Right-aligned bubble that hugs its content, capped for readability.
		HBoxContainer *row = memnew(HBoxContainer);
		row->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		message_list->add_child(row);

		Control *spacer = memnew(Control);
		spacer->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		row->add_child(spacer);

		PanelContainer *bubble = memnew(PanelContainer);
		bubble->set_h_size_flags(Control::SIZE_SHRINK_END);
		bubble->add_theme_style_override("panel", solers_make_stylebox(Color(1, 1, 1, 0.075), Color(1, 1, 1, 0.0), 14, 10));
		row->add_child(bubble);

		Label *body = _create_body_label(p_message);
		const int font_size = 14 * EDSCALE;
		body->add_theme_color_override("font_color", SOLERS_TEXT_PRIMARY);
		body->add_theme_font_size_override(SceneStringName(font_size), font_size);
		const Ref<Font> font = body->get_theme_font(SceneStringName(font));
		const float text_width = solers_longest_line_width(p_message, font, font_size) + 4 * EDSCALE;
		body->set_custom_minimum_size(Size2(MIN(text_width, 340 * EDSCALE), 0));
		bubble->add_child(body);
	} else {
		// Assistant: calm full-width prose, Codex style (no bubble chrome).
		Label *body = _create_body_label(p_message);
		body->add_theme_color_override("font_color", SOLERS_TEXT_BODY);
		body->add_theme_font_size_override(SceneStringName(font_size), 14 * EDSCALE);
		message_list->add_child(body);
	}

	callable_mp(this, &SolersDock::_scroll_chat_to_bottom).call_deferred();
}

void SolersDock::_append_tool_row(const String &p_text, bool p_ok) {
	_clear_active_reasoning();
	chat_log += vformat("%s\n", p_text);
	if (!message_list) {
		return;
	}
	_clear_empty_state();

	Label *row = memnew(Label(p_text));
	row->set_autowrap_mode(TextServer::AUTOWRAP_OFF);
	row->set_clip_text(true);
	row->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	row->set_focus_mode(Control::FOCUS_ACCESSIBILITY);
	row->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	row->add_theme_color_override("font_color", p_ok ? SOLERS_TEXT_DIM : Color(0.85, 0.46, 0.40));
	row->add_theme_font_size_override(SceneStringName(font_size), 12 * EDSCALE);
	message_list->add_child(row);

	callable_mp(this, &SolersDock::_scroll_chat_to_bottom).call_deferred();
}

void SolersDock::_clear_active_reasoning() {
	active_reasoning_label = nullptr;
	active_reasoning_text = String();
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

void SolersDock::_on_model_chip_pressed() {
	_append_tool_row(TTR("Model settings live in Project Manager -> AI Models."), true);
}

void SolersDock::_on_new_chat_pressed() {
	chat_log = String();
	if (agent_session) {
		agent_session->reset_conversation();
	}
	if (message_list) {
		while (message_list->get_child_count() > 0) {
			Node *child = message_list->get_child(0);
			message_list->remove_child(child);
			child->queue_free();
		}
		empty_state = _create_empty_state();
		message_list->add_child(empty_state);
	}
	if (chat_input) {
		chat_input->set_text("");
		_update_chat_input_height();
		_update_send_enabled();
		chat_input->grab_focus();
	}
	_refresh_status();
}

void SolersDock::_submit_chat_prompt(const String &p_prompt) {
	const String prompt = p_prompt.strip_edges();
	if (prompt.is_empty()) {
		return;
	}

	_append_chat_message("You", prompt);

	if (!agent_session) {
		_append_chat_message("Solers", "Agent session is unavailable.");
		return;
	}

	// Real BYOK end-to-end: hand the prompt to the single agent loop. The
	// session streams assistant text, tool calls and results back through the
	// signals wired in set_agent_session(); no mock, no hardcoded provider.
	Dictionary args;
	args["prompt"] = prompt;
	const Dictionary result = agent_session->start_turn(args);
	if (!(bool)result.get("ok", false)) {
		const Dictionary error = result.get("error", Dictionary());
		_append_chat_message("Solers", String::utf8("\u26a0 ") + String(error.get("message", "Could not start the agent turn.")));
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

	_on_send_chat_pressed();
	chat_input->accept_event();
}

void SolersDock::_on_chat_input_text_changed() {
	_update_chat_input_height();
	_update_send_enabled();
}

void SolersDock::_update_send_enabled() {
	if (send_chat_button && chat_input) {
		send_chat_button->set_enabled(!chat_input->get_text().strip_edges().is_empty());
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

void SolersDock::_on_abort_agent_pressed() {
	if (agent_runtime) {
		agent_runtime->abort_current_turn();
	}
	_refresh_status();
}

void SolersDock::_on_approve_next_pressed() {
	if (!permission_manager) {
		return;
	}
	Array pending = permission_manager->list_pending_requests();
	if (pending.is_empty()) {
		_refresh_status();
		return;
	}
	Dictionary request = pending[0];
	permission_manager->approve_request(request.get("id", 0));
	if (snapshot_preview) {
		snapshot_preview->set_text(vformat(TTR("Approved Solers request %d for one retry."), (int)request.get("id", 0)));
	}
	_refresh_status();
}

void SolersDock::_on_reject_next_pressed() {
	if (!permission_manager) {
		return;
	}
	Array pending = permission_manager->list_pending_requests();
	if (pending.is_empty()) {
		_refresh_status();
		return;
	}
	Dictionary request = pending[0];
	permission_manager->reject_request(request.get("id", 0));
	if (snapshot_preview) {
		snapshot_preview->set_text(vformat(TTR("Rejected Solers request %d."), (int)request.get("id", 0)));
	}
	_refresh_status();
}

void SolersDock::_on_allow_scene_mutations_toggled(bool p_enabled) {
	if (permission_manager) {
		permission_manager->set_auto_approve_permission(SolersPermissionManager::PERMISSION_EDIT_SCENE, p_enabled);
	}
	_refresh_status();
}

void SolersDock::_on_allow_file_saves_toggled(bool p_enabled) {
	if (permission_manager) {
		permission_manager->set_auto_approve_permission(SolersPermissionManager::PERMISSION_EDIT_FILES, p_enabled);
	}
	_refresh_status();
}

void SolersDock::_on_allow_run_project_toggled(bool p_enabled) {
	if (permission_manager) {
		permission_manager->set_auto_approve_permission(SolersPermissionManager::PERMISSION_RUN_PROJECT, p_enabled);
	}
	_refresh_status();
}

void SolersDock::_debug_dump_settled() {
	print_line(vformat("[solers-settled] dock global=%s size=%s", get_global_rect(), get_size()));
	if (chat_input) {
		Control *composer = Object::cast_to<Control>(chat_input->get_parent());
		Control *card = composer ? Object::cast_to<Control>(composer->get_parent()) : nullptr;
		Control *inset = card ? Object::cast_to<Control>(card->get_parent()) : nullptr;
		Control *rootv = inset ? Object::cast_to<Control>(inset->get_parent()) : nullptr;
		if (rootv) {
			print_line(vformat("[solers-settled] root global=%s", rootv->get_global_rect()));
		}
		if (inset) {
			print_line(vformat("[solers-settled] composer_inset global=%s visible=%s", inset->get_global_rect(), inset->is_visible_in_tree() ? "yes" : "no"));
		}
		if (card) {
			print_line(vformat("[solers-settled] composer_card global=%s", card->get_global_rect()));
		}
	}
	if (chat_scroll) {
		print_line(vformat("[solers-settled] scroll global=%s", chat_scroll->get_global_rect()));
	}
	if (empty_state) {
		print_line(vformat("[solers-settled] empty_state global=%s", empty_state->get_global_rect()));
	}
}

void SolersDock::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			_update_chat_input_height();
			_update_send_enabled();
			_refresh_status();
			// TEMP DEBUG: settled-state dump.
			if (is_inside_tree()) {
				SceneTree *tree = get_tree();
				if (tree) {
					Ref<SceneTreeTimer> timer = tree->create_timer(3.0);
					timer->connect("timeout", callable_mp(this, &SolersDock::_debug_dump_settled));
				}
			}
		} break;
		case NOTIFICATION_RESIZED: {
			// TEMP DEBUG: dump geometry chain.
			print_line(vformat("[solers-dbg] dock size=%s min=%s", get_size(), get_combined_minimum_size()));
			if (chat_input && chat_input->is_inside_tree()) {
				Control *composer = Object::cast_to<Control>(chat_input->get_parent());
				Control *card = composer ? Object::cast_to<Control>(composer->get_parent()) : nullptr;
				Control *inset = card ? Object::cast_to<Control>(card->get_parent()) : nullptr;
				Control *rootv = inset ? Object::cast_to<Control>(inset->get_parent()) : nullptr;
				print_line(vformat("[solers-dbg] root size=%s min=%s pos=%s", rootv ? rootv->get_size() : Size2(), rootv ? rootv->get_combined_minimum_size() : Size2(), rootv ? rootv->get_position() : Point2()));
				print_line(vformat("[solers-dbg] inset rect=%s min=%s", inset ? Rect2(inset->get_position(), inset->get_size()) : Rect2(), inset ? inset->get_combined_minimum_size() : Size2()));
				print_line(vformat("[solers-dbg] card rect=%s min=%s", card ? Rect2(card->get_position(), card->get_size()) : Rect2(), card ? card->get_combined_minimum_size() : Size2()));
				print_line(vformat("[solers-dbg] scroll rect=%s min=%s", chat_scroll ? Rect2(chat_scroll->get_position(), chat_scroll->get_size()) : Rect2(), chat_scroll ? chat_scroll->get_combined_minimum_size() : Size2()));
				print_line(vformat("[solers-dbg] EDSCALE=%f", EDSCALE));
				Control *up = this;
				for (int i = 0; i < 4 && up; i++) {
					print_line(vformat("[solers-dbg] up%d %s rect=%s min=%s", i, up->get_name(), Rect2(up->get_position(), up->get_size()), up->get_combined_minimum_size()));
					up = Object::cast_to<Control>(up->get_parent());
				}
			}
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

void SolersDock::set_services(SolersObservationService *p_observation_service, SolersToolRegistry *p_tool_registry, SolersActionTimeline *p_action_timeline, SolersPermissionManager *p_permission_manager, SolersAgentRuntime *p_agent_runtime, SolersMCPAdapter *p_mcp_adapter, SolersRpcServer *p_rpc_server, SolersSettingsService *p_settings_service) {
	observation_service = p_observation_service;
	tool_registry = p_tool_registry;
	action_timeline = p_action_timeline;
	permission_manager = p_permission_manager;
	agent_runtime = p_agent_runtime;
	mcp_adapter = p_mcp_adapter;
	rpc_server = p_rpc_server;
	settings_service = p_settings_service;
	_refresh_status();
}

void SolersDock::make_visible() {
	_refresh_status();
}

SolersDock::SolersDock() {
	set_name(TTRC("Solers"));
	set_custom_minimum_size(Size2(520 * EDSCALE, 0));
	set_h_size_flags(Control::SIZE_FILL);
	set_v_size_flags(Control::SIZE_EXPAND_FILL);
	add_theme_style_override("panel", solers_make_stylebox(SOLERS_BG, Color(0.16, 0.16, 0.17, 1), 0, 0));

	VBoxContainer *root = memnew(VBoxContainer);
	root->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	root->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	root->add_theme_constant_override("separation", 0);
	add_child(root);

	/* Topbar — panel toggle left, chat actions right. */

	MarginContainer *topbar_inset = memnew(MarginContainer);
	topbar_inset->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	topbar_inset->set_custom_minimum_size(Size2(0, 40 * EDSCALE));
	topbar_inset->add_theme_constant_override("margin_left", 10 * EDSCALE);
	topbar_inset->add_theme_constant_override("margin_right", 10 * EDSCALE);
	topbar_inset->add_theme_constant_override("margin_top", 5 * EDSCALE);
	topbar_inset->add_theme_constant_override("margin_bottom", 5 * EDSCALE);
	root->add_child(topbar_inset);

	HBoxContainer *topbar_content = memnew(HBoxContainer);
	topbar_content->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	topbar_content->add_theme_constant_override("separation", 4 * EDSCALE);
	topbar_inset->add_child(topbar_content);

	panel_button = memnew(SolersGlyphButton);
	panel_button->configure(SNAME("panel"), SolersGlyphButton::SKIN_GHOST, TTR("Solers panel"), 15);
	topbar_content->add_child(panel_button);

	Control *topbar_spacer = memnew(Control);
	topbar_spacer->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	topbar_content->add_child(topbar_spacer);

	new_chat_button = memnew(SolersGlyphButton);
	new_chat_button->configure(SNAME("new_chat"), SolersGlyphButton::SKIN_GHOST, TTR("New chat"), 15);
	new_chat_button->set_pressed_callback(callable_mp(this, &SolersDock::_on_new_chat_pressed));
	topbar_content->add_child(new_chat_button);

	more_button = memnew(SolersGlyphButton);
	more_button->configure(SNAME("more"), SolersGlyphButton::SKIN_GHOST, TTR("More"), 15);
	topbar_content->add_child(more_button);

	/* Hidden diagnostics labels (kept for _refresh_status plumbing). */

	project_status_label = memnew(Label);
	project_status_label->set_autowrap_mode(TextServer::AUTOWRAP_OFF);
	project_status_label->set_focus_mode(Control::FOCUS_ACCESSIBILITY);
	project_status_label->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	project_status_label->set_visible(false);
	root->add_child(project_status_label);

	runtime_status_label = memnew(Label);
	runtime_status_label->set_visible(false);
	runtime_status_label->set_focus_mode(Control::FOCUS_ACCESSIBILITY);
	root->add_child(runtime_status_label);

	agent_status_label = memnew(Label);
	agent_status_label->set_autowrap_mode(TextServer::AUTOWRAP_OFF);
	agent_status_label->set_focus_mode(Control::FOCUS_ACCESSIBILITY);
	agent_status_label->set_visible(false);
	root->add_child(agent_status_label);

	provider_status_label = memnew(Label);
	provider_status_label->set_visible(false);
	provider_status_label->set_focus_mode(Control::FOCUS_ACCESSIBILITY);
	root->add_child(provider_status_label);

	/* Conversation timeline. */

	chat_scroll = memnew(ScrollContainer);
	chat_scroll->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	chat_scroll->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	chat_scroll->set_horizontal_scroll_mode(ScrollContainer::SCROLL_MODE_DISABLED);
	root->add_child(chat_scroll);

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

	empty_state = _create_empty_state();
	message_list->add_child(empty_state);

	/* Composer — one floating rounded card owns text entry and actions. */

	MarginContainer *composer_inset = memnew(MarginContainer);
	composer_inset->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	composer_inset->add_theme_constant_override("margin_left", 20 * EDSCALE);
	composer_inset->add_theme_constant_override("margin_right", 20 * EDSCALE);
	composer_inset->add_theme_constant_override("margin_top", 4 * EDSCALE);
	composer_inset->add_theme_constant_override("margin_bottom", 13 * EDSCALE);
	root->add_child(composer_inset);

	SolersSurface *composer_card = memnew(SolersSurface);
	composer_card->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	composer_card->configure(SOLERS_COMPOSER_BG, SOLERS_HAIRLINE, 19, 14, true);
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
	chat_input->set_placeholder(TTR("Ask for follow-up changes"));
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

	access_chip = memnew(SolersSelectChip);
	access_chip->configure(SNAME("alert"), TTR("Full access"), String(), TTR("Agent access"));
	access_chip->set_accent(SOLERS_ACCENT_AMBER);
	composer_toolbar->add_child(access_chip);

	Control *toolbar_spacer = memnew(Control);
	toolbar_spacer->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	composer_toolbar->add_child(toolbar_spacer);

	model_chip = memnew(SolersSelectChip);
	model_chip->configure(StringName(), TTR("Model"), TTR("Not set"), TTR("Model and provider"));
	model_chip->set_pressed_callback(callable_mp(this, &SolersDock::_on_model_chip_pressed));
	composer_toolbar->add_child(model_chip);

	send_chat_button = memnew(SolersGlyphButton);
	send_chat_button->configure(SNAME("send_up"), SolersGlyphButton::SKIN_PRIMARY, TTR("Send"), 16);
	send_chat_button->set_v_size_flags(Control::SIZE_SHRINK_CENTER);
	send_chat_button->set_pressed_callback(callable_mp(this, &SolersDock::_on_send_chat_pressed));
	send_chat_button->set_enabled(false);
	composer_toolbar->add_child(send_chat_button);

	_update_chat_input_height();

	/* Hidden diagnostics panel (status plumbing + manual probes). */

	HSeparator *debug_separator = memnew(HSeparator);
	debug_separator->set_visible(false);
	root->add_child(debug_separator);

	Label *debug_title = _create_section_label(TTR("Tools & approvals"));
	debug_title->set_visible(false);
	root->add_child(debug_title);

	ScrollContainer *debug_scroll = memnew(ScrollContainer);
	debug_scroll->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	debug_scroll->set_custom_minimum_size(Size2(0, 220 * EDSCALE));
	debug_scroll->set_visible(false);
	root->add_child(debug_scroll);

	debug_panel = memnew(VBoxContainer);
	debug_panel->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	debug_panel->add_theme_constant_override("separation", 6 * EDSCALE);
	debug_scroll->add_child(debug_panel);

	scene_status_label = memnew(Label);
	scene_status_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	scene_status_label->set_focus_mode(Control::FOCUS_ACCESSIBILITY);
	debug_panel->add_child(scene_status_label);

	selection_status_label = memnew(Label);
	selection_status_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	selection_status_label->set_focus_mode(Control::FOCUS_ACCESSIBILITY);
	debug_panel->add_child(selection_status_label);

	tool_status_label = memnew(Label);
	tool_status_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	tool_status_label->set_focus_mode(Control::FOCUS_ACCESSIBILITY);
	debug_panel->add_child(tool_status_label);

	timeline_status_label = memnew(Label);
	timeline_status_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	timeline_status_label->set_focus_mode(Control::FOCUS_ACCESSIBILITY);
	debug_panel->add_child(timeline_status_label);

	protocol_status_label = memnew(Label);
	protocol_status_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	protocol_status_label->set_focus_mode(Control::FOCUS_ACCESSIBILITY);
	debug_panel->add_child(protocol_status_label);

	approval_status_label = memnew(Label);
	approval_status_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	approval_status_label->set_focus_mode(Control::FOCUS_ACCESSIBILITY);
	debug_panel->add_child(approval_status_label);

	Button *refresh_button = memnew(Button(TTR("Refresh Snapshot")));
	refresh_button->set_focus_mode(Control::FOCUS_ALL);
	refresh_button->connect(SceneStringName(pressed), callable_mp(this, &SolersDock::_on_refresh_pressed));
	debug_panel->add_child(refresh_button);

	Button *loopback_probe_button = memnew(Button(TTR("Run Runtime Probe")));
	loopback_probe_button->set_focus_mode(Control::FOCUS_ALL);
	loopback_probe_button->set_tooltip_text(TTR("Run a local Solers Agent Runtime tool batch without contacting any model provider."));
	loopback_probe_button->connect(SceneStringName(pressed), callable_mp(this, &SolersDock::_on_run_loopback_probe_pressed));
	debug_panel->add_child(loopback_probe_button);

	Button *abort_button = memnew(Button(TTR("Abort Agent Turn")));
	abort_button->set_focus_mode(Control::FOCUS_ALL);
	abort_button->connect(SceneStringName(pressed), callable_mp(this, &SolersDock::_on_abort_agent_pressed));
	debug_panel->add_child(abort_button);

	HBoxContainer *approval_buttons = memnew(HBoxContainer);
	approval_buttons->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	debug_panel->add_child(approval_buttons);

	Button *approve_next_button = memnew(Button(TTR("Approve Next")));
	approve_next_button->set_focus_mode(Control::FOCUS_ALL);
	approve_next_button->set_tooltip_text(TTR("Approve the oldest pending Solers tool request for one retry."));
	approve_next_button->connect(SceneStringName(pressed), callable_mp(this, &SolersDock::_on_approve_next_pressed));
	approval_buttons->add_child(approve_next_button);

	Button *reject_next_button = memnew(Button(TTR("Reject Next")));
	reject_next_button->set_focus_mode(Control::FOCUS_ALL);
	reject_next_button->set_tooltip_text(TTR("Reject the oldest pending Solers tool request."));
	reject_next_button->connect(SceneStringName(pressed), callable_mp(this, &SolersDock::_on_reject_next_pressed));
	approval_buttons->add_child(reject_next_button);

	allow_scene_mutation_toggle = memnew(CheckButton(TTR("Allow Scene Mutations")));
	allow_scene_mutation_toggle->set_pressed(false);
	allow_scene_mutation_toggle->set_tooltip_text(TTR("Temporarily allow Solers edit_scene tools such as node.add and node.set_properties."));
	allow_scene_mutation_toggle->connect(SceneStringName(toggled), callable_mp(this, &SolersDock::_on_allow_scene_mutations_toggled));
	debug_panel->add_child(allow_scene_mutation_toggle);

	allow_file_save_toggle = memnew(CheckButton(TTR("Allow File Writes")));
	allow_file_save_toggle->set_pressed(false);
	allow_file_save_toggle->set_tooltip_text(TTR("Temporarily allow Solers edit_files tools such as scene.save and script.write."));
	allow_file_save_toggle->connect(SceneStringName(toggled), callable_mp(this, &SolersDock::_on_allow_file_saves_toggled));
	debug_panel->add_child(allow_file_save_toggle);

	allow_run_project_toggle = memnew(CheckButton(TTR("Allow Run Controls")));
	allow_run_project_toggle->set_pressed(false);
	allow_run_project_toggle->set_tooltip_text(TTR("Temporarily allow Solers run_project tools such as runtime.play_current_scene and runtime.stop."));
	allow_run_project_toggle->connect(SceneStringName(toggled), callable_mp(this, &SolersDock::_on_allow_run_project_toggled));
	debug_panel->add_child(allow_run_project_toggle);

	debug_panel->add_child(_create_section_label(TTR("v0.1 Runtime Spine")));

	RichTextLabel *spine = memnew(RichTextLabel);
	spine->set_fit_content(true);
	spine->set_scroll_active(false);
	spine->set_focus_mode(Control::FOCUS_ACCESSIBILITY);
	spine->set_text(TTR("- Observation service\n- Tool registry\n- Permission policy\n- File checkpoints\n- Script validation\n- MCP adapter\n- JSONL RPC loopback server\n- Agent tool loop\n- Action timeline"));
	debug_panel->add_child(spine);

	debug_panel->add_child(_create_section_label(TTR("Observation Preview")));

	snapshot_preview = memnew(RichTextLabel);
	snapshot_preview->set_fit_content(true);
	snapshot_preview->set_scroll_active(false);
	snapshot_preview->set_focus_mode(Control::FOCUS_ACCESSIBILITY);
	snapshot_preview->set_text(TTR("Snapshot not captured yet."));
	debug_panel->add_child(snapshot_preview);
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
	agent_session->connect(SNAME("reasoning_delta"), callable_mp(this, &SolersDock::_on_agent_reasoning_delta));
	agent_session->connect(SNAME("assistant_message"), callable_mp(this, &SolersDock::_on_agent_assistant_message));
	agent_session->connect(SNAME("tool_call_started"), callable_mp(this, &SolersDock::_on_agent_tool_started));
	agent_session->connect(SNAME("tool_call_finished"), callable_mp(this, &SolersDock::_on_agent_tool_finished));
	agent_session->connect(SNAME("turn_completed"), callable_mp(this, &SolersDock::_on_agent_turn_completed));
	agent_session->connect(SNAME("turn_failed"), callable_mp(this, &SolersDock::_on_agent_turn_failed));
}

void SolersDock::_on_agent_reasoning_delta(const String &p_text) {
	const String text = p_text.strip_edges();
	if (text.is_empty()) {
		return;
	}
	active_reasoning_text += text;
	if (!message_list) {
		return;
	}
	_clear_empty_state();
	if (!active_reasoning_label) {
		active_reasoning_label = memnew(Label);
		active_reasoning_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
		active_reasoning_label->set_focus_mode(Control::FOCUS_ACCESSIBILITY);
		active_reasoning_label->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		active_reasoning_label->add_theme_color_override("font_color", Color(SOLERS_TEXT_DIM, 0.72));
		active_reasoning_label->add_theme_font_size_override(SceneStringName(font_size), 12 * EDSCALE);
		message_list->add_child(active_reasoning_label);
	}
	active_reasoning_label->set_text(String::utf8("\u2026 Thinking: ") + active_reasoning_text.strip_edges());
	callable_mp(this, &SolersDock::_scroll_chat_to_bottom).call_deferred();
}

void SolersDock::_on_agent_assistant_message(const String &p_text) {
	if (!p_text.strip_edges().is_empty()) {
		_append_chat_message("Solers", p_text);
	}
}

void SolersDock::_on_agent_tool_started(const String &p_id, const String &p_name, const String &p_arguments) {
	String label = String::utf8("\u2192 ") + p_name;
	if (!p_id.is_empty()) {
		label += vformat("  #%s", p_id);
	}
	_append_tool_row(label, true);
}

void SolersDock::_on_agent_tool_finished(const String &p_id, const String &p_name, const Dictionary &p_result) {
	const bool ok = p_result.get("ok", false);
	String label = String::utf8(ok ? "\u2713 " : "\u2717 ") + p_name;
	if (!ok) {
		const Dictionary error = p_result.get("error", Dictionary());
		const String message = error.get("message", String());
		if (!message.is_empty()) {
			label += " - " + message;
		}
	}
	_refresh_status();
	_append_tool_row(label, ok);
}

void SolersDock::_on_agent_turn_completed(const Dictionary &p_result) {
	_refresh_status();
}

void SolersDock::_on_agent_turn_failed(const Dictionary &p_error) {
	_append_chat_message("Solers", String::utf8("\u26a0 ") + String(p_error.get("message", "Agent turn failed.")));
	_refresh_status();
}
