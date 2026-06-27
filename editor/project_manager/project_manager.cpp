/**************************************************************************/
/*  project_manager.cpp                                                   */
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

#include "project_manager.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/io/config_file.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/os/keyboard.h"
#include "core/os/os.h"
#include "core/os/time.h"
#include "core/templates/hash_set.h"
#include "core/version.h"
#include "editor/asset_library/asset_library_editor_plugin.h"
#include "editor/doc/editor_help.h"
#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "editor/gui/editor_about.h"
#include "editor/gui/editor_file_dialog.h"
#include "editor/gui/editor_title_bar.h"
#include "editor/gui/editor_version_button.h"
#include "editor/inspector/editor_inspector.h"
#include "editor/project_manager/engine_update_label.h"
#include "editor/project_manager/project_dialog.h"
#include "editor/project_manager/project_list.h"
#include "editor/project_manager/project_tag.h"
#include "editor/project_manager/quick_settings_dialog.h"
#include "editor/project_manager/solers_pm_cards.h"
#include "editor/project_manager/solers_pm_ai_view.h"
#include "editor/project_manager/solers_pm_theme.h"
#include "editor/settings/editor_settings.h"
#include "editor/themes/editor_scale.h"
#include "editor/themes/editor_theme_manager.h"
#include "main/main.h"
#include "scene/gui/button.h"
#include "scene/gui/check_box.h"
#include "scene/gui/flow_container.h"
#include "scene/gui/label.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/margin_container.h"
#include "scene/gui/menu_bar.h"
#include "scene/gui/menu_button.h"
#include "scene/gui/option_button.h"
#include "scene/gui/panel_container.h"
#include "scene/gui/popup.h"
#include "scene/gui/rich_text_label.h"
#include "scene/gui/separator.h"
#include "scene/gui/split_container.h"
#include "scene/gui/tab_container.h"
#include "scene/main/window.h"
#include "scene/resources/style_box_flat.h"
#include "scene/theme/theme_db.h"
#include "servers/display/display_server.h"
#include "servers/navigation_3d/navigation_server_3d.h"

#ifndef PHYSICS_2D_DISABLED
#include "servers/physics_2d/physics_server_2d.h"
#endif // PHYSICS_2D_DISABLED

#ifndef PHYSICS_3D_DISABLED
#include "servers/physics_3d/physics_server_3d.h"
#endif // PHYSICS_3D_DISABLED

#include "modules/modules_enabled.gen.h" // For gdscript, mono. (For editor help highlighter).

#ifdef MODULE_SOLERS_AI_ENABLED
#include "modules/solers_ai/editor/solers_agent_runtime.h"
#include "modules/solers_ai/editor/solers_dock.h"
#endif

constexpr int GODOT4_CONFIG_VERSION = 5;

// Lucide glyph bodies (24x24 viewBox, white stroke applied by the rasterizer;
// ISC license — see modules/solers_ai/UI_ICON_LICENSE.txt).
static const char *SOLERS_LUCIDE_PLUS = "<path d=\"M5 12h14\"/><path d=\"M12 5v14\"/>";
static const char *SOLERS_LUCIDE_MONITOR = "<rect width=\"20\" height=\"14\" x=\"2\" y=\"3\" rx=\"2\"/><path d=\"M8 21h8\"/><path d=\"M12 17v4\"/>";
static const char *SOLERS_LUCIDE_PANELS = "<rect width=\"18\" height=\"18\" x=\"3\" y=\"3\" rx=\"2\"/><path d=\"M3 9h18\"/><path d=\"M9 21V9\"/>";
static const char *SOLERS_LUCIDE_BRIEFCASE = "<path d=\"M16 20V4a2 2 0 0 0-2-2h-4a2 2 0 0 0-2 2v16\"/><rect width=\"20\" height=\"14\" x=\"2\" y=\"6\" rx=\"2\"/>";
static const char *SOLERS_LUCIDE_HEXAGON_DOT = "<path d=\"M21 16V8a2 2 0 0 0-1-1.73l-7-4a2 2 0 0 0-2 0l-7 4A2 2 0 0 0 3 8v8a2 2 0 0 0 1 1.73l7 4a2 2 0 0 0 2 0l7-4A2 2 0 0 0 21 16z\"/><circle cx=\"12\" cy=\"12\" r=\"3\"/>";

ProjectManager *ProjectManager::singleton = nullptr;

struct SolersShellSessionInfo {
	String session_id;
	String title;
	int64_t wall = 0;
	bool has_title = false;
	bool has_user = false;
};

struct SolersShellSessionInfoSort {
	bool operator()(const SolersShellSessionInfo &p_a, const SolersShellSessionInfo &p_b) const {
		return p_a.wall > p_b.wall;
	}
};

static String _solers_shell_title(const String &p_content) {
	return p_content.strip_edges().replace("\r", " ").replace("\n", " ").strip_edges();
}

static String _solers_shell_time_ago(int64_t p_wall) {
	if (p_wall <= 0) {
		return String();
	}
	const int64_t delta = MAX((int64_t)0, (int64_t)Time::get_singleton()->get_unix_time_from_system() - p_wall);
	if (delta < 60) {
		return TTR("just now");
	}
	if (delta < 3600) {
		const int minutes = MAX(1, (int)(delta / 60));
		return vformat(TTRN("%d minute ago", "%d minutes ago", minutes), minutes);
	}
	if (delta < 86400) {
		const int hours = MAX(1, (int)(delta / 3600));
		return vformat(TTRN("%d hour ago", "%d hours ago", hours), hours);
	}
	const int days = MAX(1, (int)(delta / 86400));
	return vformat(TTRN("%d day ago", "%d days ago", days), days);
}

static Vector<SolersShellSessionInfo> _solers_read_shell_sessions(const String &p_project_path) {
	Vector<SolersShellSessionInfo> sessions;
	HashMap<String, int> by_id;

	Ref<FileAccess> file = FileAccess::open("user://solers_ai_transcript.jsonl", FileAccess::READ);
	if (file.is_null()) {
		return sessions;
	}

	while (!file->eof_reached()) {
		const String line = file->get_line().strip_edges();
		if (line.is_empty()) {
			continue;
		}
		const Variant parsed = JSON::parse_string(line);
		if (parsed.get_type() != Variant::DICTIONARY) {
			continue;
		}

		const Dictionary event = parsed;
		if (String(event.get("project_path", String())) != p_project_path) {
			continue;
		}

		const String role = event.get("role", String());
		const String session_id = event.get("session_id", String());
		if (session_id.is_empty()) {
			continue;
		}

		if (!by_id.has(session_id)) {
			SolersShellSessionInfo session;
			session.session_id = session_id;
			session.title = TTR("current chat");
			session.wall = (int64_t)event.get("wall", 0);
			sessions.push_back(session);
			by_id[session_id] = sessions.size() - 1;
		}

		SolersShellSessionInfo session = sessions[by_id[session_id]];
		if (event.has("wall")) {
			session.wall = (int64_t)event.get("wall", 0);
		}
		if (role == "user") {
			session.has_user = true;
		}
		if (role == "user" && !session.has_title) {
			const String title = _solers_shell_title(event.get("content", String()));
			if (!title.is_empty()) {
				session.title = title;
				session.has_title = true;
			}
		}
		sessions.write[by_id[session_id]] = session;
	}

	Vector<SolersShellSessionInfo> visible_sessions;
	for (const SolersShellSessionInfo &session : sessions) {
		if (session.has_user) {
			visible_sessions.push_back(session);
		}
	}
	return visible_sessions;
}

// Notifications.

void ProjectManager::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			if (!EditorNode::get_singleton()) {
				Engine::get_singleton()->set_editor_hint(false);
			}

			Window *main_window = get_window();
			if (main_window) {
				// Handle macOS fullscreen and extend-to-title changes.
				main_window->connect("titlebar_changed", callable_mp(this, &ProjectManager::_titlebar_resized));
			}

			// Theme has already been created in the constructor, so we can skip that step.
			_update_theme(true);
		} break;

		case NOTIFICATION_READY: {
			const bool app_shell = EditorNode::get_singleton();
			const String window_title = app_shell ? TTR("Solers App Shell", "Application") : TTR("Project Manager", "Application");
			SceneTree::get_singleton()->get_root()->set_title(GODOT_VERSION_NAME + String(" - ") + window_title);
			DisplayServer::get_singleton()->screen_set_keep_on(EDITOR_GET("interface/editor/keep_screen_on"));
			const int default_sorting = (int)EDITOR_GET("project_manager/sorting_order");
			filter_option->select(default_sorting);
			project_list->set_order_option(default_sorting, false);

			if (app_shell) {
				_select_main_view(MAIN_VIEW_RUN);
				_show_shell_chat();
			} else {
				if (shell_workspace_panel) {
					shell_workspace_panel->hide();
				}
				_show_shell_global_view(local_projects_vb);
			}
			_update_list_placeholder();
			_titlebar_resized();
		} break;

		case NOTIFICATION_PROCESS: {
#ifdef MODULE_SOLERS_AI_ENABLED
			if (solers_agent_runtime) {
				solers_agent_runtime->poll();
			}
			if (solers_agent_runtime && solers_agent_runtime->is_running() && solers_home_dock) {
				solers_home_dock->queue_redraw();
			}
#endif
		} break;

		case NOTIFICATION_TRANSLATION_CHANGED: {
			const String window_title = EditorNode::get_singleton() ? TTR("Solers App Shell", "Application") : TTR("Project Manager", "Application");
			SceneTree::get_singleton()->get_root()->set_title(GODOT_VERSION_NAME + String(" - ") + window_title);

			const String line1 = TTR("You don't have any projects yet.");
			const String line2 = TTR("Get started by creating a new one,\nimporting one that exists, or by downloading a project template from the Asset Library!");
			empty_list_message->set_text(vformat("[center][b]%s[/b] %s[/center]", line1, line2));

			_titlebar_resized();
		} break;

		case NOTIFICATION_VISIBILITY_CHANGED: {
			set_process_shortcut_input(is_visible_in_tree());
		} break;

		case NOTIFICATION_WM_CLOSE_REQUEST: {
			_dim_window();
		} break;

		case NOTIFICATION_WM_ABOUT: {
			_show_about();
		} break;

		case EditorSettings::NOTIFICATION_EDITOR_SETTINGS_CHANGED: {
			if (EditorThemeManager::is_generated_theme_outdated()) {
				_update_theme();
			}
			_update_list_placeholder();
		} break;
	}
}

// Utility data.

Ref<Texture2D> ProjectManager::_file_dialog_get_icon(const String &p_path) {
	if (p_path.has_extension("godot")) {
		return singleton->icon_type_cache["GodotMonochrome"];
	}

	return singleton->icon_type_cache["Object"];
}

Ref<Texture2D> ProjectManager::_file_dialog_get_thumbnail(const String &p_path) {
	if (p_path.has_extension("godot")) {
		return singleton->icon_type_cache["GodotFile"];
	}

	return Ref<Texture2D>();
}

void ProjectManager::_build_icon_type_cache(Ref<Theme> p_theme) {
	if (p_theme.is_null()) {
		return;
	}
	List<StringName> tl;
	p_theme->get_icon_list(EditorStringName(EditorIcons), &tl);
	for (const StringName &name : tl) {
		icon_type_cache[name] = p_theme->get_icon(name, EditorStringName(EditorIcons));
	}
}

// Main layout.

void ProjectManager::_update_size_limits() {
	const Size2 minimum_size = Size2(720, 450) * EDSCALE;

	// Define a minimum window size to prevent UI elements from overlapping or being cut off.
	Window *w = Object::cast_to<Window>(SceneTree::get_singleton()->get_root());
	if (w) {
		// Calling Window methods this early doesn't sync properties with DS.
		w->set_min_size(minimum_size);
		DisplayServer::get_singleton()->window_set_min_size(minimum_size);
	}
	Size2 real_size = DisplayServer::get_singleton()->window_get_size();

	Rect2i screen_rect = DisplayServer::get_singleton()->screen_get_usable_rect(DisplayServer::get_singleton()->window_get_current_screen());
	if (screen_rect.size != Vector2i()) {
		// Center the window on the screen.
		Vector2i window_position;
		window_position.x = screen_rect.position.x + (screen_rect.size.x - real_size.x) / 2;
		window_position.y = screen_rect.position.y + (screen_rect.size.y - real_size.y) / 2;

		// Limit popup menus to prevent unusably long lists.
		// We try to set it to half the screen resolution, but no smaller than the minimum window size.
		Size2 half_screen_rect = (screen_rect.size * EDSCALE) / 2;
		Size2 maximum_popup_size = MAX(half_screen_rect, minimum_size);
		quick_settings_dialog->update_size_limits(maximum_popup_size);
	}
}

void ProjectManager::_update_theme(bool p_skip_creation) {
	if (!p_skip_creation) {
		theme = EditorThemeManager::generate_theme(theme);
		SolersPMTheme::apply(theme); // Solers: UE-style Project Manager theme overlay.
		DisplayServer::set_early_window_clear_color_override(true, theme->get_color("background", EditorStringName(Editor)));
	}

	Vector<Ref<Theme>> editor_themes;
	editor_themes.push_back(theme);
	editor_themes.push_back(ThemeDB::get_singleton()->get_default_theme());

	ThemeContext *node_tc = ThemeDB::get_singleton()->get_theme_context(this);
	if (node_tc) {
		node_tc->set_themes(editor_themes);
	} else {
		ThemeDB::get_singleton()->create_theme_context(this, editor_themes);
	}

	Window *owner_window = get_window();
	if (owner_window) {
		ThemeContext *window_tc = ThemeDB::get_singleton()->get_theme_context(owner_window);
		if (window_tc) {
			window_tc->set_themes(editor_themes);
		} else {
			ThemeDB::get_singleton()->create_theme_context(owner_window, editor_themes);
		}
	}

	// Update styles.
	{
		const int top_bar_separation = get_theme_constant("top_bar_separation", EditorStringName(Editor));
		root_container->add_theme_constant_override("margin_left", top_bar_separation);
		root_container->add_theme_constant_override("margin_top", top_bar_separation);
		root_container->add_theme_constant_override("margin_bottom", top_bar_separation);
		root_container->add_theme_constant_override("margin_right", top_bar_separation);
		main_vbox->add_theme_constant_override("separation", top_bar_separation);

		background_panel->add_theme_style_override(SceneStringName(panel), get_theme_stylebox("Background", EditorStringName(EditorStyles)));
		main_view_container->add_theme_style_override(SceneStringName(panel), get_theme_stylebox("panel_container", "ProjectManager"));

		// Project list.
		{
			loading_label->add_theme_font_override(SceneStringName(font), get_theme_font("bold", EditorStringName(EditorFonts)));
			project_list_panel->add_theme_style_override(SceneStringName(panel), get_theme_stylebox("project_list", "ProjectManager"));

			empty_list_create_project->set_button_icon(get_editor_theme_icon("Add"));
			empty_list_import_project->set_button_icon(get_editor_theme_icon("Load"));
			empty_list_open_assetlib->set_button_icon(get_editor_theme_icon("AssetLib"));

			empty_list_online_warning->add_theme_font_override(SceneStringName(font), get_theme_font("italic", EditorStringName(EditorFonts)));
			empty_list_online_warning->add_theme_color_override(SceneStringName(font_color), get_theme_color("font_placeholder_color", EditorStringName(Editor)));

			// Top bar.
			search_box->set_right_icon(get_editor_theme_icon("Search"));

			// Bottom command bar — text-only, exactly like Unreal's footer
			// actions (Create/Cancel). Icon-and-text rows are the single
			// loudest Godot tell, so the chrome drops them entirely. Only the
			// split-button chevron survives (it is the affordance itself).
			open_options_btn->set_button_icon(get_editor_theme_icon("Collapse"));
			create_tag_btn->set_button_icon(get_editor_theme_icon("Add"));

			// Solers: view toggle + left navigation icons — strictly monochrome.
			view_list_btn->set_button_icon(SolersPMTheme::mono_icon(get_editor_theme_icon("FileList")));
			view_grid_btn->set_button_icon(SolersPMTheme::mono_icon(get_editor_theme_icon("FileThumbnail")));
			if (library_more_btn) {
				library_more_btn->set_button_icon(SolersPMTheme::mono_icon(get_editor_theme_icon("GuiTabMenuHl")));
			}
			if (selection_more_btn) {
				selection_more_btn->set_button_icon(SolersPMTheme::mono_icon(get_editor_theme_icon("GuiTabMenuHl")));
			}
			// Left rail: Lucide stroke glyphs (white at source, state-tinted by the
			// card) — modern line iconography instead of chunky editor SVGs.
			if (nav_all_card) {
				Ref<Texture2D> monitor_glyph = SolersPMTheme::lucide_icon(SOLERS_LUCIDE_MONITOR);
				nav_all_card->set_icon(monitor_glyph.is_valid() ? monitor_glyph : SolersPMTheme::mono_icon(get_editor_theme_icon(SNAME("GuiTabMenuHl"))));
			}

			tag_error->add_theme_color_override(SceneStringName(font_color), get_theme_color("error_color", EditorStringName(Editor)));
			tag_edit_error->add_theme_color_override(SceneStringName(font_color), get_theme_color("error_color", EditorStringName(Editor)));

			const int h_separation = get_theme_constant("sidebar_button_icon_separation", "ProjectManager");
			create_btn->add_theme_constant_override("h_separation", h_separation);
			import_btn->add_theme_constant_override("h_separation", h_separation);
			scan_btn->add_theme_constant_override("h_separation", h_separation);
			open_btn->add_theme_constant_override("h_separation", h_separation);
			run_btn->add_theme_constant_override("h_separation", h_separation);
			rename_btn->add_theme_constant_override("h_separation", h_separation);
			duplicate_btn->add_theme_constant_override("h_separation", h_separation);
			manage_tags_btn->add_theme_constant_override("h_separation", h_separation);
			erase_btn->add_theme_constant_override("h_separation", h_separation);
			erase_missing_btn->add_theme_constant_override("h_separation", h_separation);

			open_btn_container->add_theme_constant_override("separation", 0);
			// Solers: text-only menu entries (UE menus carry no status icons).
		}

		// Dialogs
		migration_guide_button->set_button_icon(get_editor_theme_icon("ExternalLink"));

		// Asset library popup.
		if (asset_library && EDITOR_GET("interface/theme/style") == "Classic") {
			// Removes extra border margins.
			asset_library->add_theme_style_override(SceneStringName(panel), memnew(StyleBoxEmpty));
		}
	}
#ifdef ANDROID_ENABLED
	DisplayServer::get_singleton()->window_set_color(theme->get_color("background", EditorStringName(Editor)));
#endif
}

void ProjectManager::_add_main_view(MainViewTab p_id, Control *p_view_control) {
	ERR_FAIL_INDEX(p_id, MAIN_VIEW_MAX);
	ERR_FAIL_COND(main_view_map.has(p_id));

	String tab_name;
	switch (p_id) {
		case MAIN_VIEW_EDITOR:
			tab_name = TTR("Editor");
			break;
		case MAIN_VIEW_RUN:
			tab_name = TTR("Run");
			break;
		case MAIN_VIEW_LOGS:
			tab_name = TTR("Logs");
			break;
		default:
			break;
	}

	p_view_control->set_name(tab_name);
	main_view_container->add_child(p_view_control);
	main_view_map[p_id] = p_view_control;
	if (p_id == current_main_view) {
		main_view_container->set_current_tab(main_view_container->get_tab_idx_from_control(p_view_control));
	}
}

void ProjectManager::_select_main_view(int p_id) {
	MainViewTab view_id = (MainViewTab)p_id;

	ERR_FAIL_INDEX(view_id, MAIN_VIEW_MAX);
	ERR_FAIL_COND(!main_view_map.has(view_id));

	if (current_main_view != view_id) {
		current_main_view = view_id;
	}
	if (main_view_container) {
		main_view_container->set_current_tab(main_view_container->get_tab_idx_from_control(main_view_map[current_main_view]));
	}
}

void ProjectManager::_main_view_tab_changed(int p_tab) {
	if (!main_view_container) {
		return;
	}
	Control *tab_control = main_view_container->get_tab_control(p_tab);
	if (!tab_control) {
		return;
	}
	for (const KeyValue<MainViewTab, Control *> &E : main_view_map) {
		if (E.value == tab_control) {
			_select_main_view((int)E.key);
			return;
		}
	}
}

void ProjectManager::_toggle_shell_workspace() {
	shell_workspace_collapsed = !shell_workspace_collapsed;
	if (shell_workspace_panel) {
		shell_workspace_panel->set_visible(!shell_workspace_collapsed);
	}
}

void ProjectManager::_show_shell_chat() {
	if (shell_global_overlay_view) {
		shell_global_overlay_view->hide();
		shell_global_overlay_view = nullptr;
	}
	if (solers_home_dock) {
		solers_home_dock->show();
	}
}

void ProjectManager::_show_shell_global_view(Control *p_view) {
	if (!p_view) {
		return;
	}
	if (!EditorNode::get_singleton() && p_view != local_projects_vb && project_list_panel) {
		if (shell_global_overlay_view && shell_global_overlay_view != p_view && shell_global_overlay_view != local_projects_vb) {
			shell_global_overlay_view->hide();
		}
		if (p_view->get_parent() != project_list_panel) {
			if (p_view->get_parent()) {
				p_view->get_parent()->remove_child(p_view);
			}
			project_list_panel->add_child(p_view);
		}
		if (project_list) {
			project_list->hide();
		}
		if (empty_list_placeholder) {
			empty_list_placeholder->hide();
		}
		shell_global_overlay_view = p_view;
		shell_global_overlay_view->show();
		return;
	}
	if (shell_global_overlay_view == p_view && p_view->is_visible()) {
		_show_shell_chat();
		return;
	}
	if (solers_home_dock) {
		solers_home_dock->hide();
	}
	if (shell_global_overlay_view && shell_global_overlay_view != p_view) {
		shell_global_overlay_view->hide();
	}
	shell_global_overlay_view = p_view;
	shell_global_overlay_view->show();
#ifndef ANDROID_ENABLED
	if (p_view == local_projects_vb && search_box && search_box->is_inside_tree()) {
		callable_mp((Control *)search_box, &Control::grab_focus).call_deferred(true);
	}
#endif
}

void ProjectManager::_show_shell_session_popup(const Rect2 &p_anchor) {
	if (!shell_session_popup || !shell_session_popup_list) {
		return;
	}

	while (shell_session_popup_list->get_child_count() > 0) {
		Node *child = shell_session_popup_list->get_child(0);
		shell_session_popup_list->remove_child(child);
		child->queue_free();
	}

	Button *new_session = memnew(Button);
	new_session->set_theme_type_variation("PMShellAction");
	new_session->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	new_session->set_text_alignment(HORIZONTAL_ALIGNMENT_LEFT);
	new_session->set_text(TTR("New Session"));
	new_session->connect(SceneStringName(pressed), callable_mp(this, &ProjectManager::_shell_new_session_pressed));
	shell_session_popup_list->add_child(new_session);

	Vector<SolersShellSessionInfo> sessions = _solers_read_shell_sessions(shell_project_path);
	sessions.sort_custom<SolersShellSessionInfoSort>();
	for (const SolersShellSessionInfo &session : sessions) {
		if (session.session_id.is_empty()) {
			continue;
		}
		const String time = _solers_shell_time_ago(session.wall);
		Button *row = memnew(Button);
		row->set_theme_type_variation("PMShellAction");
		row->set_toggle_mode(true);
		row->set_pressed(session.session_id == shell_session_id);
		row->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		row->set_text_alignment(HORIZONTAL_ALIGNMENT_LEFT);
		row->set_clip_text(true);
		row->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
		row->set_text(time.is_empty() ? session.title : vformat("%s  %s", session.title, time));
		row->set_tooltip_text(session.title);
		row->connect(SceneStringName(pressed), callable_mp(this, &ProjectManager::_shell_session_pressed).bind(session.session_id));
		shell_session_popup_list->add_child(row);
	}
	if (sessions.is_empty()) {
		Label *empty = memnew(Label);
		empty->set_text(TTR("No sessions yet."));
		empty->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
		empty->add_theme_color_override(SceneStringName(font_color), get_theme_color("font_placeholder_color", EditorStringName(Editor)));
		shell_session_popup_list->add_child(empty);
	}

	shell_session_popup->reset_size();
	Point2 pos = p_anchor.get_end() + Vector2(-shell_session_popup->get_size().x, 4 * EDSCALE);
	Window *win = get_window();
	if (win) {
		const float win_right = win->get_position().x + win->get_size().x;
		const float overflow = (pos.x + shell_session_popup->get_size().x) - (win_right - 8 * EDSCALE);
		if (overflow > 0) {
			pos.x -= overflow;
		}
	}
	shell_session_popup->set_position(pos);
	shell_session_popup->popup();
}

void ProjectManager::_shell_session_pressed(const String &p_session_id) {
	_show_shell_chat();
	_set_shell_session(shell_project_path, p_session_id);
	if (shell_session_popup) {
		shell_session_popup->hide();
	}
}

void ProjectManager::_set_shell_session(const String &p_project_path, const String &p_session_id) {
	String session_id = p_session_id;
	const bool changed = shell_project_path != p_project_path || shell_session_id != session_id;
	shell_project_path = p_project_path;
	shell_session_id = session_id;
#ifdef MODULE_SOLERS_AI_ENABLED
	if (changed && solers_agent_runtime) {
		if (shell_session_id.is_empty()) {
			solers_agent_runtime->set_project_path(shell_project_path);
		} else {
			solers_agent_runtime->set_session(shell_project_path, shell_session_id);
		}
		if (solers_home_dock) {
			solers_home_dock->load_chat_history(shell_session_id.is_empty() ? Array() : solers_agent_runtime->get_messages());
		}
	}
#endif
	_refresh_shell_project_panel();
}

void ProjectManager::_shell_new_session_pressed() {
#ifdef MODULE_SOLERS_AI_ENABLED
	if (solers_home_dock) {
		_show_shell_chat();
		solers_home_dock->start_new_chat();
	}
	if (solers_agent_runtime) {
		const Dictionary status = solers_agent_runtime->get_status();
		shell_session_id = status.get("session_id", String());
	}
	_refresh_shell_project_panel();
	if (shell_session_popup) {
		shell_session_popup->hide();
	}
#else
	_show_shell_chat();
#endif
}

void ProjectManager::_shell_asset_pressed() {
	if (!EditorNode::get_singleton()) {
		_deselect_all_nav_cards();
		if (nav_asset_card) {
			nav_asset_card->set_selected(true);
		}
	}
	_open_asset_library_confirmed();
}

void ProjectManager::_shell_ai_pressed() {
	if (!EditorNode::get_singleton()) {
		_deselect_all_nav_cards();
		if (nav_ai_card) {
			nav_ai_card->set_selected(true);
		}
	}
	_show_shell_global_view(shell_ai_view);
}

bool ProjectManager::_select_shell_project_in_list() {
	if (!project_list || shell_project_path.is_empty()) {
		return false;
	}
	for (int i = 0; i < project_list->_projects.size(); i++) {
		if (project_list->_projects[i].path == shell_project_path) {
			project_list->select_project(i, true);
			return true;
		}
	}
	return false;
}

void ProjectManager::_shell_edit_project_pressed() {
	if (_select_shell_project_in_list()) {
		_open_selected_projects_check_recovery_mode();
	}
}

void ProjectManager::_shell_open_project_pressed() {
	if (_select_shell_project_in_list()) {
		open_classic_editor = true;
		_open_selected_projects_check_recovery_mode();
	}
}

void ProjectManager::_shell_run_project_pressed() {
	if (_select_shell_project_in_list()) {
		_run_project();
	}
}

void ProjectManager::_load_shell_editor(const String &p_project_path) {
	if (!main_view_container || !main_view_map.has(MAIN_VIEW_EDITOR)) {
		return;
	}

	if (shell_editor_node) {
		if (p_project_path != active_editor_project_path) {
			_show_error(TTR("One project editor is already loaded in this workspace. Use Open Classic Editor for another project."));
			return;
		}
		shell_workspace_collapsed = false;
		if (shell_workspace_panel) {
			shell_workspace_panel->show();
		}
		main_view_container->set_tab_hidden(main_view_container->get_tab_idx_from_control(main_view_map[MAIN_VIEW_EDITOR]), false);
		_select_main_view(MAIN_VIEW_EDITOR);
		return;
	}

	if (!FileAccess::exists(p_project_path.path_join("project.godot"))) {
		_show_error(vformat(TTR("Can't load editor for project at '%s'.\nProject file doesn't exist or is inaccessible."), p_project_path));
		return;
	}

	List<String> args;
	for (const String &a : Main::get_forwardable_cli_arguments(Main::CLI_SCOPE_TOOL)) {
		args.push_back(a);
	}
	args.push_back("--path");
	args.push_back(p_project_path);
	args.push_back("--editor");
	args.push_back("--solers-appshell");

	if (open_in_recovery_mode) {
		args.push_back("--recovery-mode");
	}
	if (open_in_verbose_mode) {
		args.push_back("--verbose");
	}

#ifdef MODULE_SOLERS_AI_ENABLED
	if (p_project_path == shell_project_path && !shell_session_id.is_empty()) {
		OS::get_singleton()->set_environment("SOLERS_SESSION_ID", shell_session_id);
	}
#endif

	OS::get_singleton()->set_restart_on_exit(true, args);
	project_list->project_opening_initiated = true;
	_dim_window();
	get_tree()->quit();
}

void ProjectManager::_refresh_shell_project_panel() {
	if (!shell_project_name_label || !shell_project_path_label || !shell_session_label) {
		return;
	}

	String project_name = shell_project_path.get_file();
	if (project_list) {
		for (const ProjectList::Item &project : project_list->_projects) {
			if (project.path == shell_project_path) {
				project_name = project.project_name.is_empty() ? project.path.get_file() : project.project_name;
				break;
			}
		}
	}
	shell_project_name_label->set_text(project_name.is_empty() ? TTR("No project") : project_name);
	shell_project_path_label->set_text(shell_project_path);
	shell_project_path_label->set_tooltip_text(shell_project_path);
	shell_session_label->set_text(shell_session_id.is_empty() ? TTR("No session") : shell_session_id);
	if (shell_edit_project_button) {
		shell_edit_project_button->set_disabled(shell_project_path.is_empty());
	}
	if (shell_open_project_button) {
		shell_open_project_button->set_disabled(shell_project_path.is_empty());
	}
	if (shell_run_project_button) {
		shell_run_project_button->set_disabled(shell_project_path.is_empty());
	}
	if (shell_run_status) {
		shell_run_status->clear();
		if (shell_project_path.is_empty()) {
			shell_run_status->add_text(TTR("Select a project session to run it."));
		} else if (active_editor_project_path == shell_project_path) {
			shell_run_status->add_text(TTR("This project is selected for the editor workspace."));
		} else {
			shell_run_status->add_text(TTR("Load Editor keeps this window active and selects the right workspace.\nOpen Classic Editor keeps the full old workflow available when needed."));
		}
	}
	_refresh_shell_logs();
}

void ProjectManager::_refresh_shell_logs() {
	if (!shell_logs_view) {
		return;
	}

	shell_logs_view->clear();
	if (shell_project_path.is_empty() || shell_session_id.is_empty()) {
		shell_logs_view->add_text(TTR("Select a session to inspect its transcript."));
		return;
	}

	Ref<FileAccess> file = FileAccess::open("user://solers_ai_transcript.jsonl", FileAccess::READ);
	if (file.is_null()) {
		shell_logs_view->add_text(TTR("No transcript written yet."));
		return;
	}

	String text;
	while (!file->eof_reached()) {
		const String line = file->get_line().strip_edges();
		if (line.is_empty()) {
			continue;
		}
		const Variant parsed = JSON::parse_string(line);
		if (parsed.get_type() != Variant::DICTIONARY) {
			continue;
		}
		const Dictionary event = parsed;
		if (String(event.get("project_path", String())) != shell_project_path || String(event.get("session_id", String())) != shell_session_id) {
			continue;
		}
		const String role = event.get("role", String());
		const String content = event.get("content", String());
		if (role == "tool") {
			text += vformat("%s: %s\n", role, String(event.get("tool", String())));
		} else if (!content.is_empty()) {
			text += vformat("%s: %s\n", role, content);
		} else {
			text += role + "\n";
		}
	}
	shell_logs_view->add_text(text.is_empty() ? TTR("No transcript entries for this session.") : text);
}

void ProjectManager::_show_about() {
	about_dialog->popup_centered(Size2(780, 500) * EDSCALE);
}

void ProjectManager::_open_asset_library_confirmed() {
	const int network_mode = EDITOR_GET("network/connection/network_mode");
	if (network_mode == EditorSettings::NETWORK_OFFLINE) {
		EditorSettings::get_singleton()->set_setting("network/connection/network_mode", EditorSettings::NETWORK_ONLINE);
		EditorSettings::get_singleton()->notify_changes();
		EditorSettings::get_singleton()->save();
	}

	if (asset_library) {
		asset_library->disable_community_support();
	}
	_show_shell_global_view(shell_asset_view);
}

void ProjectManager::_project_list_menu_option(int p_option) {
	switch (p_option) {
		case ProjectList::MENU_EDIT:
			_open_selected_projects();
			break;

		case ProjectList::MENU_EDIT_VERBOSE:
			open_in_verbose_mode = true;
			_open_selected_projects_check_warnings();
			break;

		case ProjectList::MENU_EDIT_RECOVERY:
			_open_recovery_mode_ask(true);
			break;

		case ProjectList::MENU_RUN:
			_run_project_confirm();
			break;

		case ProjectList::MENU_SHOW_IN_FILE_MANAGER:
			_show_project_in_file_manager();
			break;

		case ProjectList::MENU_COPY_PATH: {
			const Vector<ProjectList::Item> &selected_list = project_list->get_selected_projects();
			if (selected_list.is_empty()) {
				return;
			}
			DisplayServer::get_singleton()->clipboard_set(selected_list[0].path);
		} break;

		case ProjectList::MENU_RENAME:
			_rename_project();
			break;

		case ProjectList::MENU_MANAGE_TAGS:
			_manage_project_tags();
			break;

		case ProjectList::MENU_DUPLICATE:
			_duplicate_project();
			break;

		case ProjectList::MENU_REMOVE:
			_erase_project();
			break;
	}
}

void ProjectManager::_show_error(const String &p_message, const Size2 &p_min_size) {
	error_dialog->set_text(p_message);
	error_dialog->popup_centered(p_min_size);
}

void ProjectManager::_dim_window() {
	// This method must be called before calling `get_tree()->quit()`.
	// Otherwise, its effect won't be visible

	// Dim the project manager window while it's quitting to make it clearer that it's busy.
	// No transition is applied, as the effect needs to be visible immediately
	float c = 0.5f;
	Color dim_color = Color(c, c, c);
	set_modulate(dim_color);
}

// Quick settings.

void ProjectManager::_show_quick_settings() {
	if (!EditorNode::get_singleton()) {
		_deselect_all_nav_cards();
		if (nav_settings_card) {
			nav_settings_card->set_selected(true);
		}
	}
	quick_settings_dialog->popup_centered(Size2(640, 200) * EDSCALE);
}

void ProjectManager::_restart_confirmed() {
	List<String> args = OS::get_singleton()->get_cmdline_args();
	Error err = OS::get_singleton()->create_instance(args);
	ERR_FAIL_COND(err);

	_dim_window();
	get_tree()->quit();
}

// Project list.

void ProjectManager::_update_list_placeholder() {
	if (project_list->get_project_count() > 0) {
		empty_list_placeholder->hide();
		return;
	}

	empty_list_open_assetlib->set_visible(asset_library);

	const int network_mode = EDITOR_GET("network/connection/network_mode");
	if (network_mode == EditorSettings::NETWORK_OFFLINE) {
		empty_list_open_assetlib->set_text(TTRC("Go Online and Open Asset Library"));
		empty_list_online_warning->set_visible(true);
	} else {
		empty_list_open_assetlib->set_text(TTRC("Open Asset Library"));
		empty_list_online_warning->set_visible(false);
	}

	empty_list_placeholder->show();
}

void ProjectManager::_scan_projects() {
	scan_dir->popup_file_dialog();
}

void ProjectManager::_run_project() {
	const HashSet<String> &selected_list = project_list->get_selected_project_keys();

	if (selected_list.size() < 1) {
		return;
	}

	if (selected_list.size() > 1) {
		multi_run_ask->set_text(vformat(TTR("Are you sure to run %d projects at once?"), selected_list.size()));
		multi_run_ask->popup_centered();
	} else {
		_run_project_confirm();
	}
}

void ProjectManager::_run_project_confirm() {
	Vector<ProjectList::Item> selected_list = project_list->get_selected_projects();

	for (int i = 0; i < selected_list.size(); ++i) {
		const String &selected_main = selected_list[i].main_scene;
		if (selected_main.is_empty()) {
			_show_error(TTRC("Can't run project: Project has no main scene defined.\nPlease edit the project and set the main scene in the Project Settings under the \"Application\" category."));
			continue;
		}

		const String &path = selected_list[i].path;

		// `.substr(6)` on `ProjectSettings::get_singleton()->get_imported_files_path()` strips away the leading "res://".
		if (!DirAccess::exists(path.path_join(ProjectSettings::get_singleton()->get_imported_files_path().substr(6)))) {
			_show_error(TTRC("Can't run project: Assets need to be imported first.\nPlease edit the project to trigger the initial import."));
			continue;
		}

		print_line("Running project: " + path);

		List<String> args;

		for (const String &a : Main::get_forwardable_cli_arguments(Main::CLI_SCOPE_PROJECT)) {
			args.push_back(a);
		}

		args.push_back("--path");
		args.push_back(path);

		Error err = OS::get_singleton()->create_instance(args);
		ERR_FAIL_COND(err);
	}
}

void ProjectManager::_open_selected_projects() {
	// Show loading text to tell the user that the project manager is busy loading.
	// This is especially important for the Web project manager.
	loading_label->show();

	const HashSet<String> &selected_list = project_list->get_selected_project_keys();
	if (!open_classic_editor && selected_list.size() > 1) {
		loading_label->hide();
		_show_error(TTR("Load Editor supports one active project at a time."));
		return;
	}

	for (const String &path : selected_list) {
		String conf = path.path_join("project.godot");

		if (!FileAccess::exists(conf)) {
			loading_label->hide();
			_show_error(vformat(TTR("Can't open project at '%s'.\nProject file doesn't exist or is inaccessible."), path));
			return;
		}

		print_line(open_classic_editor ? "Opening classic editor: " + path : "Loading editor workspace: " + path);

		if (!open_classic_editor) {
			_load_shell_editor(path);
			loading_label->hide();
			return;
		}

		List<String> args;

		for (const String &a : Main::get_forwardable_cli_arguments(Main::CLI_SCOPE_TOOL)) {
			args.push_back(a);
		}

		args.push_back("--path");
		args.push_back(path);

		args.push_back("--editor");

		if (open_in_recovery_mode) {
			args.push_back("--recovery-mode");
		}

		if (open_in_verbose_mode) {
			args.push_back("--verbose");
		}

#ifdef MODULE_SOLERS_AI_ENABLED
		const bool pass_solers_session = path == shell_project_path && !shell_session_id.is_empty();
		if (open_classic_editor) {
			OS::get_singleton()->set_environment("SOLERS_CLASSIC_EDITOR", "1");
		}
		if (pass_solers_session) {
			OS::get_singleton()->set_environment("SOLERS_SESSION_ID", shell_session_id);
		}
#endif
		Error err = OS::get_singleton()->create_instance(args);
#ifdef MODULE_SOLERS_AI_ENABLED
		if (open_classic_editor) {
			OS::get_singleton()->unset_environment("SOLERS_CLASSIC_EDITOR");
		}
		if (pass_solers_session) {
			OS::get_singleton()->unset_environment("SOLERS_SESSION_ID");
		}
#endif
		if (err != OK) {
			loading_label->hide();
			open_classic_editor = false;
			_show_error(vformat(TTR("Can't open project at '%s'.\nFailed to start the editor."), path));
			ERR_PRINT(vformat("Failed to start an editor instance for the project at '%s', error code %d.", path, err));
			return;
		}
	}

	open_classic_editor = false;
	project_list->project_opening_initiated = true;

	_dim_window();
	get_tree()->quit();
}

void ProjectManager::_open_selected_projects_check_warnings() {
	const HashSet<String> &selected_list = project_list->get_selected_project_keys();
	if (selected_list.size() < 1) {
		return;
	}

	const Size2i popup_min_size = Size2i(400.0 * EDSCALE, 0);

	if (selected_list.size() > 1) {
		multi_open_ask->set_text(vformat(TTR("You requested to open %d projects in parallel. Do you confirm?\nNote that usual checks for engine version compatibility will be bypassed."), selected_list.size()));
		multi_open_ask->popup_centered(popup_min_size);
		return;
	}

	ProjectList::Item project = project_list->get_selected_projects()[0];
	if (project.missing) {
		return;
	}

	// Update the project settings or don't open.
	const int config_version = project.version;
	PackedStringArray unsupported_features = project.unsupported_features;

	ask_update_label->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_LEFT); // Reset in case of previous center align.
	ask_update_backup->set_pressed(false);
	full_convert_button->hide();
	migration_guide_button->hide();
	ask_update_backup->hide();

	ask_update_settings->get_ok_button()->set_text("OK");

	// Check if the config_version property was empty or 0.
	if (config_version == 0) {
		ask_update_label->set_text(vformat(TTR("The selected project \"%s\" does not specify its supported Godot version in its configuration file (\"project.godot\").\n\nProject path: %s\n\nIf you proceed with opening it, it will be converted to Godot's current configuration file format.\n\nWarning: You won't be able to open the project with previous versions of the engine anymore."), project.project_name, project.path));
		ask_update_settings->popup_centered(popup_min_size);
		return;
	}
	// Check if we need to convert project settings from an earlier engine version.
	if (config_version < ProjectSettings::CONFIG_VERSION) {
		if (config_version == GODOT4_CONFIG_VERSION - 1 && ProjectSettings::CONFIG_VERSION == GODOT4_CONFIG_VERSION) { // Conversion from Godot 3 to 4.
			full_convert_button->show();
			ask_update_label->set_text(vformat(TTR("The selected project \"%s\" was generated by Godot 3.x, and needs to be converted for Godot 4.x.\n\nProject path: %s\n\nYou have three options:\n- Convert only the configuration file (\"project.godot\"). Use this to open the project without attempting to convert its scenes, resources and scripts.\n- Convert the entire project including its scenes, resources and scripts (recommended if you are upgrading).\n- Do nothing and go back.\n\nWarning: If you select a conversion option, you won't be able to open the project with previous versions of the engine anymore."), project.project_name, project.path));
			ask_update_settings->get_ok_button()->set_text(TTRC("Convert project.godot Only"));
		} else {
			ask_update_label->set_text(vformat(TTR("The selected project \"%s\" was generated by an older engine version, and needs to be converted for this version.\n\nProject path: %s\n\nDo you want to convert it?\n\nWarning: You won't be able to open the project with previous versions of the engine anymore."), project.project_name, project.path));
			ask_update_settings->get_ok_button()->set_text(TTRC("Convert project.godot"));
		}
		ask_update_backup->show();
		migration_guide_button->show();
		ask_update_settings->popup_centered(popup_min_size);
		ask_update_settings->get_cancel_button()->grab_focus(); // To prevent accidents.
		return;
	}
	// Check if the file was generated by a newer, incompatible engine version.
	if (config_version > ProjectSettings::CONFIG_VERSION) {
		_show_error(vformat(TTR("Can't open project \"%s\" at the following path:\n\n%s\n\nThe project settings were created by a newer engine version, whose settings are not compatible with this version."), project.project_name, project.path), popup_min_size);
		return;
	}
	// Check if the project is using features not supported by this build of Godot.
	if (!unsupported_features.is_empty()) {
		String warning_message = "";
		for (int i = 0; i < unsupported_features.size(); i++) {
			const String &feature = unsupported_features[i];
			if (feature == "Double Precision") {
				ask_update_backup->show();
				warning_message += TTR("Warning: This project uses double precision floats, but this version of\nGodot uses single precision floats. Opening this project may cause data loss.\n\n");
				unsupported_features.remove_at(i);
				i--;
			} else if (feature == "C#") {
				warning_message += TTR("Warning: This project uses C#, but this build of Godot does not have\nthe Mono module. If you proceed you will not be able to use any C# scripts.\n\n");
				unsupported_features.remove_at(i);
				i--;
			} else if (ProjectList::project_feature_looks_like_version(feature)) {
				ask_update_backup->show();
				migration_guide_button->show();
				version_convert_feature = feature;
				warning_message += vformat(TTR("Warning: This project was last edited in Godot %s. Opening will change it to Godot %s.\n\n"), Variant(feature), Variant(GODOT_VERSION_BRANCH));
				unsupported_features.remove_at(i);
				i--;
			}
		}
		if (!unsupported_features.is_empty()) {
			String unsupported_features_str = String(", ").join(unsupported_features);
			warning_message += vformat(TTR("Warning: This project uses the following features not supported by this build of Godot:\n\n%s\n\n"), unsupported_features_str);
		}
		warning_message += TTR("Open anyway? Project will be modified.");
		ask_update_label->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
		ask_update_label->set_text(warning_message);
		ask_update_settings->popup_centered(popup_min_size);
		return;
	}

	// Open if the project is up-to-date.
	_open_selected_projects();
}

void ProjectManager::_open_selected_projects_check_recovery_mode() {
	Vector<ProjectList::Item> selected_projects = project_list->get_selected_projects();

	if (selected_projects.is_empty()) {
		return;
	}

	const ProjectList::Item &project = selected_projects[0];
	if (project.missing) {
		return;
	}

	open_in_verbose_mode = false;
	open_in_recovery_mode = false;
	// Check if the project failed to load during last startup.
	if (project.recovery_mode) {
		_open_recovery_mode_ask(false);
		return;
	}

	_open_selected_projects_check_warnings();
}

void ProjectManager::_open_selected_projects_with_migration() {
	if (ask_update_backup->is_pressed() && project_list->get_selected_projects().size() == 1) {
		ask_update_settings->hide();
		ask_update_backup->set_pressed(false);

		_duplicate_project_with_action(POST_DUPLICATE_ACTION_OPEN);
		return;
	}

#ifndef DISABLE_DEPRECATED
	if (project_list->get_selected_projects().size() == 1) {
		// Only migrate if a single project is opened.
		_minor_project_migrate();
	}
#endif
	_open_selected_projects();
}

void ProjectManager::_install_project(const String &p_zip_path, const String &p_title) {
	project_dialog->set_mode(ProjectDialog::MODE_INSTALL);
	project_dialog->set_zip_path(p_zip_path);
	project_dialog->set_zip_title(p_title);
	project_dialog->show_dialog();
}

void ProjectManager::_import_project() {
	project_dialog->set_mode(ProjectDialog::MODE_IMPORT);
	project_dialog->ask_for_path_and_show();
}

void ProjectManager::_new_project() {
	project_dialog->set_mode(ProjectDialog::MODE_NEW);
	project_dialog->show_dialog();
}

void ProjectManager::_rename_project() {
	const Vector<ProjectList::Item> &selected_list = project_list->get_selected_projects();

	if (selected_list.is_empty()) {
		return;
	}

	for (const ProjectList::Item &E : selected_list) {
		project_dialog->set_project_name(E.project_name);
		project_dialog->set_project_path(E.path);
		project_dialog->set_mode(ProjectDialog::MODE_RENAME);
		project_dialog->show_dialog();
	}
}

void ProjectManager::_duplicate_project() {
	_duplicate_project_with_action(POST_DUPLICATE_ACTION_NONE);
}

void ProjectManager::_duplicate_project_with_action(PostDuplicateAction p_post_action) {
	Vector<ProjectList::Item> selected_projects = project_list->get_selected_projects();
	if (selected_projects.is_empty()) {
		return;
	}

	post_duplicate_action = p_post_action;

	const ProjectList::Item &project = selected_projects[0];

	project_dialog->set_mode(ProjectDialog::MODE_DUPLICATE);
	project_dialog->set_project_name(vformat("%s (%s)", project.project_name, p_post_action == POST_DUPLICATE_ACTION_NONE ? "Copy" : project.project_version));
	project_dialog->set_original_project_path(project.path);
	project_dialog->set_duplicate_can_edit(p_post_action == POST_DUPLICATE_ACTION_NONE);
	project_dialog->show_dialog(false);
}

void ProjectManager::_show_project_in_file_manager() {
	const Vector<ProjectList::Item> &selected_list = project_list->get_selected_projects();
	if (selected_list.is_empty()) {
		return;
	}

	for (const ProjectList::Item &E : selected_list) {
		OS::get_singleton()->shell_show_in_file_manager(E.path, true);
	}
}

void ProjectManager::_erase_project() {
	const HashSet<String> &selected_list = project_list->get_selected_project_keys();

	if (selected_list.is_empty()) {
		return;
	}

	String confirm_message;
	if (selected_list.size() >= 2) {
		confirm_message = vformat(TTR("Remove %d projects from the list?"), selected_list.size());
	} else {
		confirm_message = TTRC("Remove this project from the list?");
	}

	erase_ask_label->set_text(confirm_message);
	//delete_project_contents->set_pressed(false);
	erase_ask->popup_centered();
}

void ProjectManager::_erase_missing_projects() {
	erase_missing_ask->set_text(TTRC("Remove all missing projects from the list?\nThe project folders' contents won't be modified."));
	erase_missing_ask->popup_centered();
}

void ProjectManager::_erase_project_confirm() {
	project_list->erase_selected_projects(false);
	_update_project_buttons();
	_update_list_placeholder();
}

void ProjectManager::_erase_missing_projects_confirm() {
	project_list->erase_missing_projects();
	_update_project_buttons();
	_update_list_placeholder();
}

void ProjectManager::_update_project_buttons() {
	Vector<ProjectList::Item> selected_projects = project_list->get_selected_projects();
	bool empty_selection = selected_projects.is_empty();

	bool is_missing_project_selected = false;
	for (int i = 0; i < selected_projects.size(); ++i) {
		if (selected_projects[i].missing) {
			is_missing_project_selected = true;
			break;
		}
	}

	if (selected_projects.size() == 1 && !selected_projects[0].missing) {
		const String selected_path = selected_projects[0].path;
		_set_shell_session(selected_path, selected_path == shell_project_path ? shell_session_id : String());
	}

	erase_btn->set_disabled(empty_selection);
	open_btn->set_disabled(empty_selection || is_missing_project_selected);
	open_options_btn->set_disabled(empty_selection || is_missing_project_selected);
	rename_btn->set_disabled(empty_selection || is_missing_project_selected);
	duplicate_btn->set_disabled(empty_selection || is_missing_project_selected);
	manage_tags_btn->set_disabled(empty_selection || is_missing_project_selected || selected_projects.size() > 1);
	run_btn->set_disabled(empty_selection || is_missing_project_selected);

	erase_missing_btn->set_disabled(!project_list->is_any_project_missing());

	// Solers: contextual action bar — the selection group only exists while a
	// selection does (hidden, not grayed: UE-style minimal resting chrome).
	if (selection_bar) {
		selection_bar->set_visible(!empty_selection);
	}
	if (selection_more_btn) {
		PopupMenu *sel_popup = selection_more_btn->get_popup();
		const bool sel_invalid = empty_selection || is_missing_project_selected;
		sel_popup->set_item_disabled(sel_popup->get_item_index(BOTTOM_MENU_RENAME), sel_invalid);
		sel_popup->set_item_disabled(sel_popup->get_item_index(BOTTOM_MENU_DUPLICATE), sel_invalid);
		sel_popup->set_item_disabled(sel_popup->get_item_index(BOTTOM_MENU_MANAGE_TAGS), sel_invalid || selected_projects.size() > 1);
		sel_popup->set_item_disabled(sel_popup->get_item_index(BOTTOM_MENU_ERASE), empty_selection);
	}
	_refresh_library_more_menu();
}

void ProjectManager::_refresh_library_more_menu() {
	if (!library_more_btn) {
		return;
	}
	PopupMenu *popup = library_more_btn->get_popup();
	popup->clear();
	popup->add_item(TTR("Scan Projects"), BOTTOM_MENU_SCAN);
	popup->set_item_shortcut(popup->get_item_index(BOTTOM_MENU_SCAN), ED_GET_SHORTCUT("project_manager/scan_projects"), true);
	// Contextual entry: only offered while broken list entries actually exist.
	if (project_list && project_list->is_any_project_missing()) {
		popup->add_separator();
		popup->add_item(TTR("Remove Missing"), BOTTOM_MENU_ERASE_MISSING);
	}
}

void ProjectManager::_on_library_more_id_pressed(int p_id) {
	switch ((BottomBarMenuOption)p_id) {
		case BOTTOM_MENU_SCAN:
			_scan_projects();
			break;
		case BOTTOM_MENU_ERASE_MISSING:
			_erase_missing_projects();
			break;
		default:
			break;
	}
}

void ProjectManager::_on_selection_more_id_pressed(int p_id) {
	switch ((BottomBarMenuOption)p_id) {
		case BOTTOM_MENU_RENAME:
			_rename_project();
			break;
		case BOTTOM_MENU_DUPLICATE:
			_duplicate_project();
			break;
		case BOTTOM_MENU_MANAGE_TAGS:
			_manage_project_tags();
			break;
		case BOTTOM_MENU_ERASE:
			_erase_project();
			break;
		default:
			break;
	}
}

// Solers: view mode (list/grid) + left navigation rail.

void ProjectManager::_set_project_view(int p_mode) {
	if (!project_list) {
		return;
	}
	project_list->set_display_mode(p_mode);
}

void ProjectManager::_deselect_all_nav_cards() {
	if (nav_new_card) {
		nav_new_card->set_selected(false);
	}
	if (nav_all_card) {
		nav_all_card->set_selected(false);
	}
	if (nav_asset_card) {
		nav_asset_card->set_selected(false);
	}
	if (nav_ai_card) {
		nav_ai_card->set_selected(false);
	}
	if (nav_settings_card) {
		nav_settings_card->set_selected(false);
	}
}

void ProjectManager::_nav_new_pressed() {
	_deselect_all_nav_cards();
	if (nav_new_card) {
		nav_new_card->set_selected(true);
	}
	_new_project();
}

void ProjectManager::_nav_card_pressed(SolersCategoryCard *p_card) {
	if (!EditorNode::get_singleton() && shell_global_overlay_view && shell_global_overlay_view != local_projects_vb) {
		shell_global_overlay_view->hide();
		shell_global_overlay_view = local_projects_vb;
		if (project_list) {
			project_list->show();
		}
		_update_list_placeholder();
	}
	_deselect_all_nav_cards();
	if (p_card) {
		p_card->set_selected(true);
	}
	_nav_all_pressed();
}

void ProjectManager::_nav_all_pressed() {
	search_box->set_text("");
	project_list->set_search_term("");
	project_list->set_favorites_only(false);
	project_list->sort_projects();
	project_list->select_first_visible_project();
	_update_project_buttons();
}

void ProjectManager::_bottom_bar_separator(HBoxContainer *p_bar) {
	VSeparator *sep = memnew(VSeparator);
	p_bar->add_child(sep);
}

void ProjectManager::_open_options_popup() {
	// The bar hugs the window bottom, so the combo menu opens *upwards* —
	// opening down would get clamped by the screen edge back over the button.
	Rect2 rect = open_btn_container->get_screen_rect();
	open_options_popup->set_size(Size2(rect.size.width, 0));
	rect.position.y -= open_options_popup->get_contents_minimum_size().y + 2 * EDSCALE;
	open_options_popup->set_position(rect.position);

	open_options_popup->popup();
}

void ProjectManager::_position_overflow_popup(PopupMenu *p_popup, Control *p_anchor) {
	if (!p_popup || !p_anchor) {
		return;
	}
	p_popup->reset_size();
	const Rect2 anchor = p_anchor->get_screen_rect();
	Point2 pos = anchor.position;
	// Above the anchor (the bar sits at the window bottom).
	pos.y -= p_popup->get_size().y + 2 * EDSCALE;
	// Keep the menu inside the window: right-align when the anchor is near the
	// right edge (the selection "⋯"), left-align otherwise (the library "⋯").
	Window *win = get_window();
	if (win) {
		const float win_right = win->get_position().x + win->get_size().x;
		const float overflow = (pos.x + p_popup->get_size().x) - (win_right - 8 * EDSCALE);
		if (overflow > 0) {
			pos.x = anchor.get_end().x - p_popup->get_size().x;
		}
	}
	p_popup->set_position(pos);
}

void ProjectManager::_open_recovery_mode_ask(bool manual) {
	String recovery_mode_details;

	// Only show the initial crash preamble if this popup wasn't manually triggered.
	if (!manual) {
		recovery_mode_details +=
				TTR("It looks like Godot crashed when opening this project the last time. If you're having problems editing this project, you can try to open it in Recovery Mode.") +
				String::utf8("\n\n");
	}

	recovery_mode_details +=
			TTR("Recovery Mode is a special mode that may help to recover projects that crash the engine during initialization. This mode temporarily disables the following features:") +
			String::utf8("\n\n•  ") + TTR("Tool scripts") +
			String::utf8("\n•  ") + TTR("Editor plugins") +
			String::utf8("\n•  ") + TTR("GDExtension addons") +
			String::utf8("\n•  ") + TTR("Automatic scene restoring") +
			String::utf8("\n\n") + TTR("This mode is intended only for basic editing to troubleshoot such issues, and therefore it will not be possible to run the project during this mode. It is also a good idea to make a backup of your project before proceeding.") +
			String::utf8("\n\n") + TTR("Edit the project in Recovery Mode?");

	open_recovery_mode_ask->set_text(recovery_mode_details);
	open_recovery_mode_ask->popup_centered(Size2(550, 70) * EDSCALE);
}

void ProjectManager::_on_projects_updated() {
	Vector<ProjectList::Item> selected_projects = project_list->get_selected_projects();
	int index = 0;
	for (int i = 0; i < selected_projects.size(); ++i) {
		index = project_list->refresh_project(selected_projects[i].path);
	}
	if (index != -1) {
		project_list->ensure_project_visible(index);
	}

	project_list->update_dock_menu();
}

void ProjectManager::_on_open_options_selected(int p_option) {
	switch (p_option) {
		case 0: // Edit in verbose mode.
			open_in_verbose_mode = true;
			_open_selected_projects_check_warnings();
			break;
		case 1: // Edit in recovery mode.
			_open_recovery_mode_ask(true);
			break;
	}
}

void ProjectManager::_on_recovery_mode_popup_open_normal() {
	open_recovery_mode_ask->hide();
	open_in_recovery_mode = false;
	_open_selected_projects_check_warnings();
}

void ProjectManager::_on_recovery_mode_popup_open_recovery() {
	open_in_recovery_mode = true;
	_open_selected_projects_check_warnings();
}

void ProjectManager::_on_project_created(const String &dir, bool edit) {
	project_list->add_project(dir, false);
	project_list->save_config();
	search_box->clear();

	int i = project_list->refresh_project(dir);
	project_list->ensure_project_visible(i);
	_update_list_placeholder();

	if (edit) {
		_open_selected_projects_check_warnings();
	}

	project_list->update_dock_menu();
}

void ProjectManager::_on_project_duplicated(const String &p_original_path, const String &p_duplicate_path, bool p_edit) {
	if (post_duplicate_action == POST_DUPLICATE_ACTION_NONE) {
		_on_project_created(p_duplicate_path, p_edit);
	} else {
		project_list->add_project(p_duplicate_path, false);
		project_list->save_config();

		if (post_duplicate_action == POST_DUPLICATE_ACTION_OPEN) {
			_open_selected_projects_with_migration();
		} else if (post_duplicate_action == POST_DUPLICATE_ACTION_FULL_CONVERSION) {
			_full_convert_button_pressed();
		}

		project_list->update_dock_menu();
	}

	post_duplicate_action = POST_DUPLICATE_ACTION_NONE;
}

void ProjectManager::_on_order_option_changed(int p_idx) {
	if (is_inside_tree()) {
		project_list->set_order_option(p_idx, true);
	}
}

void ProjectManager::_on_search_term_changed(const String &p_term) {
	project_list->set_search_term(p_term);
	project_list->sort_projects();

	// Select the first visible project in the list.
	// This makes it possible to open a project without ever touching the mouse,
	// as the search field is automatically focused on startup.
	project_list->select_first_visible_project();
	_update_project_buttons();
}

void ProjectManager::_on_search_term_submitted(const String &p_text) {
	if (!local_projects_vb || !local_projects_vb->is_visible_in_tree()) {
		return;
	}

	_open_selected_projects_check_recovery_mode();
}

LineEdit *ProjectManager::get_search_box() {
	return search_box;
}

// Project tag management.

void ProjectManager::_manage_project_tags() {
	for (int i = 0; i < project_tags->get_child_count(); i++) {
		project_tags->get_child(i)->queue_free();
	}

	const ProjectList::Item item = project_list->get_selected_projects()[0];
	current_project_tags = item.tags;
	for (const String &tag : current_project_tags) {
		ProjectTag *tag_control = memnew(ProjectTag(tag, true));
		project_tags->add_child(tag_control);
		tag_control->connect_button_to(callable_mp(this, &ProjectManager::_delete_project_tag).bind(tag));
	}

	tag_edit_error->hide();
	tag_manage_dialog->popup_centered(Vector2i(500, 0) * EDSCALE);
}

void ProjectManager::_add_project_tag(const String &p_tag) {
	if (current_project_tags.has(p_tag)) {
		return;
	}
	current_project_tags.append(p_tag);

	ProjectTag *tag_control = memnew(ProjectTag(p_tag, true));
	project_tags->add_child(tag_control);
	tag_control->connect_button_to(callable_mp(this, &ProjectManager::_delete_project_tag).bind(p_tag));
}

void ProjectManager::_delete_project_tag(const String &p_tag) {
	current_project_tags.erase(p_tag);
	for (int i = 0; i < project_tags->get_child_count(); i++) {
		ProjectTag *tag_control = Object::cast_to<ProjectTag>(project_tags->get_child(i));
		if (tag_control && tag_control->get_tag() == p_tag) {
			memdelete(tag_control);
			break;
		}
	}
}

void ProjectManager::_apply_project_tags() {
	PackedStringArray tags;
	for (int i = 0; i < project_tags->get_child_count(); i++) {
		ProjectTag *tag_control = Object::cast_to<ProjectTag>(project_tags->get_child(i));
		if (tag_control) {
			tags.append(tag_control->get_tag());
		}
	}

	const String project_godot = project_list->get_selected_projects()[0].path.path_join("project.godot");
	ProjectSettings *cfg = memnew(ProjectSettings(project_godot));
	if (!cfg->is_project_loaded()) {
		memdelete(cfg);
		tag_edit_error->set_text(vformat(TTR("Couldn't load project at '%s'. It may be missing or corrupted."), project_godot));
		tag_edit_error->show();
		callable_mp((Window *)tag_manage_dialog, &Window::show).call_deferred(); // Make sure the dialog does not disappear.
		return;
	} else {
		tags.sort();
		cfg->set("application/config/tags", tags);
		Error err = cfg->save_custom(project_godot);
		memdelete(cfg);

		if (err != OK) {
			tag_edit_error->set_text(vformat(TTR("Couldn't save project at '%s' (error %d)."), project_godot, err));
			tag_edit_error->show();
			callable_mp((Window *)tag_manage_dialog, &Window::show).call_deferred();
			return;
		}
	}

	_on_projects_updated();
}

void ProjectManager::_set_new_tag_name(const String p_name) {
	create_tag_dialog->get_ok_button()->set_disabled(true);
	if (p_name.strip_edges().is_empty()) {
		tag_error->set_text(TTRC("Tag name can't be empty."));
		return;
	}

	if (p_name[0] == '_' || p_name[p_name.length() - 1] == '_') {
		tag_error->set_text(TTRC("Tag name can't begin or end with underscore."));
		return;
	}

	bool was_underscore = false;
	for (const char32_t &c : p_name.span()) {
		// Treat spaces as underscores, as we convert spaces to underscores automatically in the tag input field.
		if (c == '_' || c == ' ') {
			if (was_underscore) {
				tag_error->set_text(TTRC("Tag name can't contain consecutive underscores or spaces."));
				return;
			}
			was_underscore = true;
		} else {
			was_underscore = false;
		}
	}

	for (const String &c : forbidden_tag_characters) {
		if (p_name.contains(c)) {
			tag_error->set_text(vformat(TTR("These characters are not allowed in tags: %s."), String(" ").join(forbidden_tag_characters)));
			return;
		}
	}

	tag_error->set_text("");
	create_tag_dialog->get_ok_button()->set_disabled(false);
}

void ProjectManager::_create_new_tag() {
	if (!tag_error->get_text().is_empty()) {
		return;
	}
	create_tag_dialog->hide(); // When using text_submitted, need to hide manually.

	// Enforce a valid tag name (no spaces, lowercase only) automatically.
	// The project manager displays underscores as spaces, and capitalization is performed automatically.
	const String new_tag = new_tag_name->get_text().strip_edges().to_lower().replace_char(' ', '_');
	add_new_tag(new_tag);
	_add_project_tag(new_tag);
}

void ProjectManager::add_new_tag(const String &p_tag) {
	if (!tag_set.has(p_tag)) {
		tag_set.insert(p_tag);
		ProjectTag *tag_control = memnew(ProjectTag(p_tag));
		all_tags->add_child(tag_control);
		all_tags->move_child(tag_control, -2);
		tag_control->connect_button_to(callable_mp(this, &ProjectManager::_add_project_tag).bind(p_tag));
	}
}

// Project converter/migration tool.

#ifndef DISABLE_DEPRECATED
void ProjectManager::_minor_project_migrate() {
	const ProjectList::Item migrated_project = project_list->get_selected_projects()[0];

	if (version_convert_feature.begins_with("4.3")) {
		// Migrate layout after scale changes.
		const float edscale = EDSCALE;
		if (edscale != 1.0) {
			Ref<ConfigFile> layout_file;
			layout_file.instantiate();

			const String layout_path = migrated_project.path.path_join(".godot/editor/editor_layout.cfg");
			Error err = layout_file->load(layout_path);
			if (err == OK) {
				for (int i = 0; i < 4; i++) {
					const String key = "dock_hsplit_" + itos(i + 1);
					int old_value = layout_file->get_value("docks", key, 0);
					if (old_value != 0) {
						layout_file->set_value("docks", key, old_value / edscale);
					}
				}
				layout_file->save(layout_path);
			}
		}
	}
}
#endif

void ProjectManager::_full_convert_button_pressed() {
	ask_update_settings->hide();

	if (ask_update_backup->is_pressed()) {
		ask_update_backup->set_pressed(false);

		_duplicate_project_with_action(POST_DUPLICATE_ACTION_FULL_CONVERSION);
		return;
	}

	ask_full_convert_dialog->popup_centered(Size2i(600.0 * EDSCALE, 0));
	ask_full_convert_dialog->get_cancel_button()->grab_focus();
}

void ProjectManager::_migration_guide_button_pressed() {
	const String url = vformat("%s/tutorials/migrating/index.html", GODOT_VERSION_DOCS_URL);
	OS::get_singleton()->shell_open(url);
}

void ProjectManager::_perform_full_project_conversion() {
	Vector<ProjectList::Item> selected_list = project_list->get_selected_projects();
	if (selected_list.is_empty()) {
		return;
	}

	const String &path = selected_list[0].path;

	print_line("Converting project: " + path);
	List<String> args;
	args.push_back("--path");
	args.push_back(path);
	args.push_back("--convert-3to4");
	args.push_back("--rendering-driver");
	args.push_back(Main::get_rendering_driver_name());

	Error err = OS::get_singleton()->create_instance(args);
	ERR_FAIL_COND(err);

	project_list->set_project_version(path, GODOT4_CONFIG_VERSION);
}

// Input and I/O.

void ProjectManager::shortcut_input(const Ref<InputEvent> &p_ev) {
	ERR_FAIL_COND(p_ev.is_null());

	Ref<InputEventKey> k = p_ev;

	if (k.is_valid()) {
		if (!k->is_pressed()) {
			return;
		}

		// Pressing Command + Q quits the Project Manager
		// This is handled by the platform implementation on macOS,
		// so only define the shortcut on other platforms
#ifndef MACOS_ENABLED
		if (k->get_keycode_with_modifiers() == (KeyModifierMask::META | Key::Q)) {
			_dim_window();
			get_tree()->quit();
		}
#endif

		if (!local_projects_vb || !local_projects_vb->is_visible_in_tree()) {
			return;
		}

		bool keycode_handled = true;

		switch (k->get_keycode()) {
			case Key::ENTER: {
				_open_selected_projects_check_recovery_mode();
			} break;
			case Key::HOME: {
				if (project_list->get_project_count() > 0) {
					project_list->ensure_project_visible(0);
				}

			} break;
			case Key::END: {
				if (project_list->get_project_count() > 0) {
					project_list->ensure_project_visible(project_list->get_project_count() - 1);
				}

			} break;
			case Key::F: {
				if (k->is_command_or_control_pressed()) {
					search_box->grab_focus();
				} else {
					keycode_handled = false;
				}
			} break;
			case Key::A: {
				if (k->is_command_or_control_pressed()) {
					if (k->is_shift_pressed()) {
						project_list->deselect_all_visible_projects();
					} else {
						project_list->select_all_visible_projects();
					}
					_update_project_buttons();
				}
			} break;
			default: {
				keycode_handled = false;
			} break;
		}

		if (keycode_handled) {
			accept_event();
		}
	}
}

void ProjectManager::_files_dropped(PackedStringArray p_files) {
	// TODO: Support installing multiple ZIPs at the same time?
	if (p_files.size() == 1 && p_files[0].ends_with(".zip")) {
		const String &file = p_files[0];
		_install_project(file, file.get_file().get_basename().capitalize());
		return;
	}

	HashSet<String> folders_set;
	Ref<DirAccess> da = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
	for (int i = 0; i < p_files.size(); i++) {
		const String &file = p_files[i];
		folders_set.insert(da->dir_exists(file) ? file : file.get_base_dir());
	}
	ERR_FAIL_COND(folders_set.is_empty()); // This can't really happen, we consume every dropped file path above.

	PackedStringArray folders;
	for (const String &E : folders_set) {
		folders.push_back(E);
	}
	project_list->find_projects_multiple(folders);
}

void ProjectManager::_titlebar_resized() {
	DisplayServer::get_singleton()->window_set_window_buttons_offset(Vector2i(title_bar->get_global_position().y + title_bar->get_size().y / 2, title_bar->get_global_position().y + title_bar->get_size().y / 2), DisplayServer::MAIN_WINDOW_ID);
	const Vector3i &margin = DisplayServer::get_singleton()->window_get_safe_title_margins(DisplayServer::MAIN_WINDOW_ID);
	if (left_menu_spacer) {
		int w = (root_container->is_layout_rtl()) ? margin.y : margin.x;
		left_menu_spacer->set_custom_minimum_size(Size2(w, 0));
	}
	if (right_menu_spacer) {
		int w = (root_container->is_layout_rtl()) ? margin.x : margin.y;
		right_menu_spacer->set_custom_minimum_size(Size2(w, 0));
	}
	if (title_bar) {
		title_bar->set_custom_minimum_size(Size2(0, margin.z - title_bar->get_global_position().y));
	}
}

void ProjectManager::_open_donate_page() {
	OS::get_singleton()->shell_open("https://fund.godotengine.org/?ref=project_manager");
}

// Object methods.

ProjectManager::ProjectManager() {
	singleton = this;

	// Turn off some servers we aren't going to be using in the Project Manager.
	if (!EditorNode::get_singleton()) {
		NavigationServer3D::get_singleton()->set_active(false);
		PhysicsServer3D::get_singleton()->set_active(false);
		PhysicsServer2D::get_singleton()->set_active(false);
	}

	// Initialize settings.
	{
		if (!EditorSettings::get_singleton()) {
			EditorSettings::create();
		}
		EditorSettings::get_singleton()->set_optimize_save(false); // Just write settings as they come.

		{
			bool agile_input_event_flushing = EDITOR_GET("input/buffering/agile_event_flushing");
			bool use_accumulated_input = EDITOR_GET("input/buffering/use_accumulated_input");

			Input::get_singleton()->set_agile_input_event_flushing(agile_input_event_flushing);
			Input::get_singleton()->set_use_accumulated_input(use_accumulated_input);
		}

		int display_scale = EDITOR_GET("interface/editor/display_scale");

		switch (display_scale) {
			case 0:
				// Try applying a suitable display scale automatically.
				EditorScale::set_scale(EditorSettings::get_auto_display_scale());
				break;
			case 1:
				EditorScale::set_scale(0.75);
				break;
			case 2:
				EditorScale::set_scale(1.0);
				break;
			case 3:
				EditorScale::set_scale(1.25);
				break;
			case 4:
				EditorScale::set_scale(1.5);
				break;
			case 5:
				EditorScale::set_scale(1.75);
				break;
			case 6:
				EditorScale::set_scale(2.0);
				break;
			default:
				EditorScale::set_scale(EDITOR_GET("interface/editor/custom_display_scale"));
				break;
		}
		FileDialog::set_get_icon_callback(callable_mp_static(ProjectManager::_file_dialog_get_icon));
		FileDialog::set_get_thumbnail_callback(callable_mp_static(ProjectManager::_file_dialog_get_thumbnail));

		FileDialog::set_default_show_hidden_files(EDITOR_GET("filesystem/file_dialog/show_hidden_files"));
		FileDialog::set_default_display_mode((FileDialog::DisplayMode)EDITOR_GET("filesystem/file_dialog/display_mode").operator int());

		int swap_cancel_ok = EDITOR_GET("interface/editor/accept_dialog_cancel_ok_buttons");
		if (swap_cancel_ok != 0) { // 0 is auto, set in register_scene based on DisplayServer.
			// Swap on means OK first.
			AcceptDialog::set_swap_cancel_ok(swap_cancel_ok == 2);
		}

		OS::get_singleton()->set_low_processor_usage_mode(true);
	}

#if defined(MODULE_GDSCRIPT_ENABLED) || defined(MODULE_MONO_ENABLED)
	if (!EditorNode::get_singleton()) {
		EditorHelpHighlighter::create_singleton();
	}
#endif

	SceneTree::get_singleton()->get_root()->connect("files_dropped", callable_mp(this, &ProjectManager::_files_dropped));

	// Initialize UI.
	{
		int pm_root_dir = EDITOR_GET("interface/editor/ui_layout_direction");
		Control::set_root_layout_direction(pm_root_dir);
		Window::set_root_layout_direction(pm_root_dir);

		EditorThemeManager::initialize();
		theme = EditorThemeManager::generate_theme();
		SolersPMTheme::apply(theme); // Solers: UE-style Project Manager theme overlay.
		DisplayServer::set_early_window_clear_color_override(true, theme->get_color(SNAME("background"), EditorStringName(Editor)));

		set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);

		_build_icon_type_cache(theme);
	}

	// Project manager layout.

	background_panel = memnew(Panel);
	add_child(background_panel);
	background_panel->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);

	root_container = memnew(MarginContainer);
	add_child(root_container);
	root_container->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);

	main_vbox = memnew(VBoxContainer);
	root_container->add_child(main_vbox);

	// Title bar.
	bool can_expand = bool(EDITOR_GET("interface/editor/expand_to_title")) && DisplayServer::get_singleton()->has_feature(DisplayServer::FEATURE_EXTEND_TO_TITLE);

	{
		title_bar = memnew(EditorTitleBar);
		main_vbox->add_child(title_bar);

		if (can_expand) {
			// Add spacer to avoid other controls under window minimize/maximize/close buttons (left side).
			left_menu_spacer = memnew(Control);
			left_menu_spacer->set_mouse_filter(Control::MOUSE_FILTER_PASS);
			title_bar->add_child(left_menu_spacer);
		}

		HBoxContainer *left_hbox = memnew(HBoxContainer);
		left_hbox->set_alignment(BoxContainer::ALIGNMENT_BEGIN);
		left_hbox->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		left_hbox->set_stretch_ratio(1.0);
		title_bar->add_child(left_hbox);

		// Solers: the Godot wordmark/logo is intentionally removed from the
		// top-left of the shell. Primary navigation lives in the left rail.

		bool global_menu = !bool(EDITOR_GET("interface/editor/use_embedded_menu")) && NativeMenu::get_singleton()->has_feature(NativeMenu::FEATURE_GLOBAL_MENU);
		if (global_menu) {
			MenuBar *main_menu_bar = memnew(MenuBar);
			main_menu_bar->set_start_index(0); // Main menu, add to the start of global menu.
			main_menu_bar->set_prefer_global_menu(true);
			left_hbox->add_child(main_menu_bar);

			if (NativeMenu::get_singleton()->has_system_menu(NativeMenu::WINDOW_MENU_ID)) {
				PopupMenu *window_menu = memnew(PopupMenu);
				window_menu->set_system_menu(NativeMenu::WINDOW_MENU_ID);
				window_menu->set_name(TTRC("Window"));
				main_menu_bar->add_child(window_menu);
			}
			if (NativeMenu::get_singleton()->has_system_menu(NativeMenu::HELP_MENU_ID)) {
				PopupMenu *help_menu = memnew(PopupMenu);
				help_menu->set_system_menu(NativeMenu::HELP_MENU_ID);
				help_menu->set_name(TTRC("Help"));
				main_menu_bar->add_child(help_menu);
			}
		}
		HBoxContainer *right_hbox = memnew(HBoxContainer);
		right_hbox->set_alignment(BoxContainer::ALIGNMENT_END);
		right_hbox->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		right_hbox->set_stretch_ratio(1.0);
		title_bar->add_child(right_hbox);

		if (can_expand) {
			// Add spacer to avoid other controls under the window minimize/maximize/close buttons (right side).
			right_menu_spacer = memnew(Control);
			right_menu_spacer->set_mouse_filter(Control::MOUSE_FILTER_PASS);
			title_bar->add_child(right_menu_spacer);
		}
	}

	HBoxContainer *shell = memnew(HBoxContainer);
	shell->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	shell->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	shell->add_theme_constant_override("separation", 8 * EDSCALE);
	main_vbox->add_child(shell);

	shell_work_split = memnew(HSplitContainer);
	shell_work_split->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	shell_work_split->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	shell->add_child(shell_work_split);

	shell_chat_panel = memnew(PanelContainer);
	shell_chat_panel->set_name("SolersChatPanel");
	shell_chat_panel->set_custom_minimum_size(Size2(420, 0) * EDSCALE);
	shell_chat_panel->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	shell_chat_panel->set_stretch_ratio(0.42);
	shell_chat_panel->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	shell_work_split->add_child(shell_chat_panel);

#ifdef MODULE_SOLERS_AI_ENABLED
	{
		solers_agent_runtime = memnew(SolersAgentRuntime);
		solers_home_dock = memnew(SolersDock);
		solers_home_dock->set_name("SolersChat");
		solers_home_dock->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		solers_home_dock->set_v_size_flags(Control::SIZE_EXPAND_FILL);
		solers_home_dock->set_workspace_toggle_callback(callable_mp(this, &ProjectManager::_toggle_shell_workspace));
		solers_home_dock->set_session_menu_callback(callable_mp(this, &ProjectManager::_show_shell_session_popup));
		solers_agent_runtime->bind_dock(solers_home_dock);
		shell_chat_panel->add_child(solers_home_dock);

		shell_session_popup = memnew(PopupPanel);
		shell_session_popup->add_theme_style_override(SceneStringName(panel), memnew(StyleBoxEmpty));

		PanelContainer *session_popup_surface = memnew(PanelContainer);
		Ref<StyleBoxFlat> session_popup_style;
		session_popup_style.instantiate();
		session_popup_style->set_bg_color(Color(0.075, 0.078, 0.086));
		session_popup_style->set_corner_radius_all(int(12 * EDSCALE));
		session_popup_style->set_shadow_color(Color(0, 0, 0, 0.34));
		session_popup_style->set_shadow_size(int(18 * EDSCALE));
		session_popup_surface->add_theme_style_override(SceneStringName(panel), session_popup_style);
		add_child(shell_session_popup);

		MarginContainer *session_popup_margin = memnew(MarginContainer);
		session_popup_margin->set_custom_minimum_size(Size2(360, 0) * EDSCALE);
		session_popup_margin->add_theme_constant_override("margin_left", 8 * EDSCALE);
		session_popup_margin->add_theme_constant_override("margin_right", 8 * EDSCALE);
		session_popup_margin->add_theme_constant_override("margin_top", 8 * EDSCALE);
		session_popup_margin->add_theme_constant_override("margin_bottom", 8 * EDSCALE);
		shell_session_popup->add_child(session_popup_surface);
		session_popup_surface->add_child(session_popup_margin);

		shell_session_popup_list = memnew(VBoxContainer);
		shell_session_popup_list->add_theme_constant_override("separation", 4 * EDSCALE);
		session_popup_margin->add_child(shell_session_popup_list);

		set_process(true);
	}
#endif

	shell_workspace_panel = memnew(PanelContainer);
	shell_workspace_panel->set_name("SolersWorkspacePanel");
	shell_workspace_panel->set_custom_minimum_size(Size2(320, 0) * EDSCALE);
	shell_workspace_panel->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	shell_workspace_panel->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	shell_work_split->add_child(shell_workspace_panel);

	main_view_container = memnew(TabContainer);
	main_view_container->set_name("SolersWorkspaceTabs");
	main_view_container->set_theme_type_variation("TabContainerInner");
	main_view_container->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	main_view_container->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	main_view_container->connect("tab_changed", callable_mp(this, &ProjectManager::_main_view_tab_changed));
	shell_workspace_panel->add_child(main_view_container);

	// Project list view.
	{
		local_projects_vb = memnew(VBoxContainer);
		local_projects_vb->set_name("LocalProjectsOverlay");
		local_projects_vb->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		local_projects_vb->set_v_size_flags(Control::SIZE_EXPAND_FILL);
		local_projects_vb->hide();
		shell_chat_panel->add_child(local_projects_vb);

		// Project list's top bar: search, sort and the list/grid view toggle.
		// (Create/Import/Scan now live in the bottom action bar.)
		{
			HBoxContainer *hb = memnew(HBoxContainer);
			hb->set_h_size_flags(Control::SIZE_EXPAND_FILL);
			local_projects_vb->add_child(hb);

			loading_label = memnew(Label(TTRC("Loading, please wait...")));
			loading_label->set_accessibility_live(DisplayServer::AccessibilityLiveMode::LIVE_ASSERTIVE);
			loading_label->set_h_size_flags(Control::SIZE_EXPAND_FILL);
			loading_label->hide();
			hb->add_child(loading_label);

			search_box = memnew(LineEdit);
			search_box->set_placeholder(TTRC("Filter Projects"));
			search_box->set_accessibility_name(TTRC("Filter Projects"));
			search_box->set_tooltip_text(TTRC("This field filters projects by name and last path component.\nTo filter projects by name and full path, the query must contain at least one `/` character."));
			search_box->set_clear_button_enabled(true);
			search_box->connect(SceneStringName(text_changed), callable_mp(this, &ProjectManager::_on_search_term_changed));
			search_box->connect(SceneStringName(text_submitted), callable_mp(this, &ProjectManager::_on_search_term_submitted));
			search_box->set_h_size_flags(Control::SIZE_EXPAND_FILL);
			hb->add_child(search_box);

			sort_label = memnew(Label);
			sort_label->set_text(TTRC("Sort:"));
			hb->add_child(sort_label);

			filter_option = memnew(OptionButton);
			filter_option->set_clip_text(true);
			filter_option->set_h_size_flags(Control::SIZE_EXPAND_FILL);
			filter_option->set_stretch_ratio(0.3);
			filter_option->set_accessibility_name(TTRC("Sort:"));
			filter_option->connect(SceneStringName(item_selected), callable_mp(this, &ProjectManager::_on_order_option_changed));
			hb->add_child(filter_option);

			filter_option->add_item(TTRC("Last Edited"));
			filter_option->add_item(TTRC("Name"));
			filter_option->add_item(TTRC("Path"));
			filter_option->add_item(TTRC("Tags"));

			hb->add_child(memnew(VSeparator));

			// View mode toggle (list / grid).
			view_mode_group.instantiate();

			view_list_btn = memnew(Button);
			view_list_btn->set_toggle_mode(true);
			view_list_btn->set_button_group(view_mode_group);
			view_list_btn->set_tooltip_text(TTRC("List View"));
			view_list_btn->set_accessibility_name(TTRC("List View"));
			view_list_btn->connect(SceneStringName(pressed), callable_mp(this, &ProjectManager::_set_project_view).bind((int)ProjectList::DISPLAY_LIST));
			hb->add_child(view_list_btn);

			view_grid_btn = memnew(Button);
			view_grid_btn->set_toggle_mode(true);
			view_grid_btn->set_button_group(view_mode_group);
			view_grid_btn->set_tooltip_text(TTRC("Grid View"));
			view_grid_btn->set_accessibility_name(TTRC("Grid View"));
			view_grid_btn->connect(SceneStringName(pressed), callable_mp(this, &ProjectManager::_set_project_view).bind((int)ProjectList::DISPLAY_GRID));
			hb->add_child(view_grid_btn);
		}

		// Project list body: left navigation rail + the project list/grid panel.
		{
			HBoxContainer *project_list_hbox = memnew(HBoxContainer);
			local_projects_vb->add_child(project_list_hbox);
			project_list_hbox->set_v_size_flags(Control::SIZE_EXPAND_FILL);

			nav_panel = memnew(VBoxContainer);
			nav_panel->set_custom_minimum_size(Size2(240, 0) * EDSCALE);
			nav_panel->add_theme_constant_override("separation", 0);
			project_list_hbox->add_child(nav_panel);

			auto make_nav_card = [&](const String &p_title, const char *p_glyph, const Callable &p_callback) -> SolersCategoryCard * {
				SolersCategoryCard *card = memnew(SolersCategoryCard);
				card->configure(p_title, SolersPMTheme::lucide_icon(p_glyph), Color());
				card->set_pressed_callback(p_callback);
				nav_panel->add_child(card);
				return card;
			};

			nav_new_card = make_nav_card(TTRC("New Project"), SOLERS_LUCIDE_PLUS, callable_mp(this, &ProjectManager::_nav_new_pressed));
			nav_new_card->set_filled(true);

			nav_all_card = make_nav_card(TTRC("Projects"), SOLERS_LUCIDE_MONITOR, Callable());
			nav_all_card->set_pressed_callback(callable_mp(this, &ProjectManager::_nav_card_pressed).bind(nav_all_card));
			nav_asset_card = make_nav_card(TTRC("Asset Library"), SOLERS_LUCIDE_PANELS, callable_mp(this, &ProjectManager::_shell_asset_pressed));
			nav_ai_card = make_nav_card(TTRC("AI Settings"), SOLERS_LUCIDE_BRIEFCASE, callable_mp(this, &ProjectManager::_shell_ai_pressed));
			nav_settings_card = make_nav_card(TTRC("Settings"), SOLERS_LUCIDE_HEXAGON_DOT, callable_mp(this, &ProjectManager::_show_quick_settings));

			project_list_panel = memnew(PanelContainer);
			project_list_panel->set_h_size_flags(Control::SIZE_EXPAND_FILL);
			project_list_hbox->add_child(project_list_panel);

			project_list = memnew(ProjectList);
			project_list->set_horizontal_scroll_mode(ScrollContainer::SCROLL_MODE_DISABLED);
			project_list_panel->add_child(project_list);
			project_list->connect(ProjectList::SIGNAL_LIST_CHANGED, callable_mp(this, &ProjectManager::_update_project_buttons));
			project_list->connect(ProjectList::SIGNAL_LIST_CHANGED, callable_mp(this, &ProjectManager::_update_list_placeholder));
			project_list->connect(ProjectList::SIGNAL_SELECTION_CHANGED, callable_mp(this, &ProjectManager::_update_project_buttons));
			project_list->connect(ProjectList::SIGNAL_PROJECT_ASK_OPEN, callable_mp(this, &ProjectManager::_open_selected_projects_check_recovery_mode));
			project_list->connect(ProjectList::SIGNAL_MENU_OPTION_SELECTED, callable_mp(this, &ProjectManager::_project_list_menu_option));

			// Empty project list placeholder.
			{
				empty_list_placeholder = memnew(VBoxContainer);
				empty_list_placeholder->set_v_size_flags(Control::SIZE_SHRINK_CENTER);
				empty_list_placeholder->add_theme_constant_override("separation", 16 * EDSCALE);
				empty_list_placeholder->hide();
				project_list_panel->add_child(empty_list_placeholder);

				empty_list_message = memnew(RichTextLabel);
				empty_list_message->set_auto_translate_mode(AUTO_TRANSLATE_MODE_DISABLED);
				empty_list_message->set_use_bbcode(true);
				empty_list_message->set_fit_content(true);
				empty_list_message->set_h_size_flags(SIZE_EXPAND_FILL);
				empty_list_message->add_theme_style_override(CoreStringName(normal), memnew(StyleBoxEmpty));

				empty_list_placeholder->add_child(empty_list_message);

				FlowContainer *empty_list_actions = memnew(FlowContainer);
				empty_list_actions->set_alignment(FlowContainer::ALIGNMENT_CENTER);
				empty_list_placeholder->add_child(empty_list_actions);

				empty_list_create_project = memnew(Button);
				empty_list_create_project->set_text(TTRC("Create New Project"));
				empty_list_create_project->set_theme_type_variation("PanelBackgroundButton");
				empty_list_actions->add_child(empty_list_create_project);
				empty_list_create_project->connect(SceneStringName(pressed), callable_mp(this, &ProjectManager::_new_project));

				empty_list_import_project = memnew(Button);
				empty_list_import_project->set_text(TTRC("Import Existing Project"));
				empty_list_import_project->set_theme_type_variation("PanelBackgroundButton");
				empty_list_actions->add_child(empty_list_import_project);
				empty_list_import_project->connect(SceneStringName(pressed), callable_mp(this, &ProjectManager::_import_project));

				empty_list_open_assetlib = memnew(Button);
				empty_list_open_assetlib->set_text(TTRC("Open Asset Library"));
				empty_list_open_assetlib->set_theme_type_variation("PanelBackgroundButton");
				empty_list_actions->add_child(empty_list_open_assetlib);
				empty_list_open_assetlib->connect(SceneStringName(pressed), callable_mp(this, &ProjectManager::_open_asset_library_confirmed));

				empty_list_online_warning = memnew(Label);
				empty_list_online_warning->set_focus_mode(FOCUS_ACCESSIBILITY);
				empty_list_online_warning->set_horizontal_alignment(HorizontalAlignment::HORIZONTAL_ALIGNMENT_CENTER);
				empty_list_online_warning->set_custom_minimum_size(Size2(220, 0) * EDSCALE);
				empty_list_online_warning->set_autowrap_mode(TextServer::AUTOWRAP_WORD);
				empty_list_online_warning->set_h_size_flags(Control::SIZE_EXPAND_FILL);
				empty_list_online_warning->set_text(TTRC("Note: The Asset Library requires an online connection and involves sending data over the internet."));
				empty_list_placeholder->add_child(empty_list_online_warning);
			}

		} // Project list body.

		// Bottom action bar — UE-grade progressive disclosure. Resting state shows
		// only [Create] [Import] [⋯]; the selection group [⋯][Run][Edit▾] exists
		// only while a project is selected. Low-frequency actions collapse into
		// the two overflow menus (and the per-project right-click context menu),
		// with all keyboard shortcuts preserved through global popup shortcuts.
		{
			PanelContainer *bottom_bar_panel = memnew(PanelContainer);
			bottom_bar_panel->set_theme_type_variation("PMBottomBarPanel");
			local_projects_vb->add_child(bottom_bar_panel);

			HBoxContainer *bottom_bar = memnew(HBoxContainer);
			bottom_bar->set_theme_type_variation("PMBottomBar");
			bottom_bar_panel->add_child(bottom_bar);

			// --- Library actions (always visible) ---
			create_btn = memnew(Button);
			create_btn->set_text(TTRC("Create"));
			create_btn->set_shortcut(ED_SHORTCUT("project_manager/new_project", TTRC("New Project"), KeyModifierMask::CMD_OR_CTRL | Key::N));
			create_btn->connect(SceneStringName(pressed), callable_mp(this, &ProjectManager::_new_project));
			bottom_bar->add_child(create_btn);

			import_btn = memnew(Button);
			import_btn->set_text(TTRC("Import"));
			import_btn->set_shortcut(ED_SHORTCUT("project_manager/import_project", TTRC("Import Project"), KeyModifierMask::CMD_OR_CTRL | Key::I));
			import_btn->connect(SceneStringName(pressed), callable_mp(this, &ProjectManager::_import_project));
			bottom_bar->add_child(import_btn);

			// Hidden logic anchors: dialogs and update paths still reference these,
			// but the visible entry points are the overflow menus below.
			scan_btn = memnew(Button);
			scan_btn->set_text(TTRC("Scan"));
			scan_btn->set_shortcut(ED_SHORTCUT("project_manager/scan_projects", TTRC("Scan Projects"), KeyModifierMask::CMD_OR_CTRL | Key::S));
			scan_btn->connect(SceneStringName(pressed), callable_mp(this, &ProjectManager::_scan_projects));
			bottom_bar->add_child(scan_btn);
			scan_btn->hide();

			erase_missing_btn = memnew(Button);
			erase_missing_btn->set_text(TTRC("Remove Missing"));
			erase_missing_btn->connect(SceneStringName(pressed), callable_mp(this, &ProjectManager::_erase_missing_projects));
			bottom_bar->add_child(erase_missing_btn);
			erase_missing_btn->hide();

			donate_btn = memnew(Button);
			donate_btn->set_text(TTRC("Donate"));
			donate_btn->connect(SceneStringName(pressed), callable_mp(this, &ProjectManager::_open_donate_page));
			bottom_bar->add_child(donate_btn);
			// Solers: hidden — Unreal's command bar carries no donate entry.
			donate_btn->hide();

			// Library overflow (⋯): scanning and cleanup; "Remove Missing" only
			// materializes when missing projects actually exist (contextual UI).
			library_more_btn = memnew(MenuButton);
			library_more_btn->set_flat(false);
			library_more_btn->set_accessibility_name(TTRC("Library Options"));
			library_more_btn->set_tooltip_text(TTRC("Library maintenance"));
			bottom_bar->add_child(library_more_btn);
			library_more_btn->get_popup()->connect(SceneStringName(id_pressed), callable_mp(this, &ProjectManager::_on_library_more_id_pressed));
			library_more_btn->get_popup()->connect("about_to_popup", callable_mp(this, &ProjectManager::_position_overflow_popup).bind(library_more_btn->get_popup(), (Control *)library_more_btn));
			_refresh_library_more_menu();

			// Spacer pushes the selection actions to the right edge.
			Control *bottom_spacer = memnew(Control);
			bottom_spacer->set_h_size_flags(Control::SIZE_EXPAND_FILL);
			bottom_bar->add_child(bottom_spacer);

			// --- Selection actions (visible only while a selection exists) ---
			selection_bar = memnew(HBoxContainer);
			bottom_bar->add_child(selection_bar);

			// Hidden logic anchors (dialog wiring + button-state updates).
			manage_tags_btn = memnew(Button);
			manage_tags_btn->set_text(TTRC("Manage Tags"));
			manage_tags_btn->set_shortcut(ED_SHORTCUT("project_manager/project_tags", TTRC("Manage Tags"), KeyModifierMask::CMD_OR_CTRL | Key::T));
			selection_bar->add_child(manage_tags_btn);
			manage_tags_btn->hide();

			rename_btn = memnew(Button);
			rename_btn->set_text(TTRC("Rename"));
			// The F2 shortcut isn't overridden with Enter on macOS as Enter is already used to edit a project.
			rename_btn->set_shortcut(ED_SHORTCUT("project_manager/rename_project", TTRC("Rename Project"), Key::F2));
			rename_btn->connect(SceneStringName(pressed), callable_mp(this, &ProjectManager::_rename_project));
			selection_bar->add_child(rename_btn);
			rename_btn->hide();

			duplicate_btn = memnew(Button);
			duplicate_btn->set_text(TTRC("Duplicate"));
			duplicate_btn->connect(SceneStringName(pressed), callable_mp(this, &ProjectManager::_duplicate_project));
			selection_bar->add_child(duplicate_btn);
			duplicate_btn->hide();

			erase_btn = memnew(Button);
			erase_btn->set_text(TTRC("Remove"));
			erase_btn->set_shortcut(ED_SHORTCUT("project_manager/remove_project", TTRC("Remove Project"), Key::KEY_DELETE));
			erase_btn->connect(SceneStringName(pressed), callable_mp(this, &ProjectManager::_erase_project));
			selection_bar->add_child(erase_btn);
			erase_btn->hide();

			// Selection overflow (⋯): occasional per-project operations. The
			// destructive Remove sits last, behind a separator — physically
			// isolated from Run/Edit (also mirrored in the right-click menu).
			selection_more_btn = memnew(MenuButton);
			selection_more_btn->set_flat(false);
			selection_more_btn->set_accessibility_name(TTRC("Project Options"));
			selection_more_btn->set_tooltip_text(TTRC("More project actions"));
			selection_bar->add_child(selection_more_btn);
			{
				PopupMenu *sel_popup = selection_more_btn->get_popup();
				sel_popup->add_item(TTRC("Rename"), BOTTOM_MENU_RENAME);
				sel_popup->set_item_shortcut(sel_popup->get_item_index(BOTTOM_MENU_RENAME), ED_GET_SHORTCUT("project_manager/rename_project"), true);
				sel_popup->add_item(TTRC("Duplicate"), BOTTOM_MENU_DUPLICATE);
				sel_popup->add_item(TTRC("Manage Tags"), BOTTOM_MENU_MANAGE_TAGS);
				sel_popup->set_item_shortcut(sel_popup->get_item_index(BOTTOM_MENU_MANAGE_TAGS), ED_GET_SHORTCUT("project_manager/project_tags"), true);
				sel_popup->add_separator();
				sel_popup->add_item(TTRC("Remove from List"), BOTTOM_MENU_ERASE);
				sel_popup->set_item_shortcut(sel_popup->get_item_index(BOTTOM_MENU_ERASE), ED_GET_SHORTCUT("project_manager/remove_project"), true);
				sel_popup->connect(SceneStringName(id_pressed), callable_mp(this, &ProjectManager::_on_selection_more_id_pressed));
				sel_popup->connect("about_to_popup", callable_mp(this, &ProjectManager::_position_overflow_popup).bind(sel_popup, (Control *)selection_more_btn));
			}

			run_btn = memnew(Button);
			run_btn->set_text(TTRC("Run"));
			run_btn->set_shortcut(ED_SHORTCUT("project_manager/run_project", TTRC("Run Project"), KeyModifierMask::CMD_OR_CTRL | Key::R));
			run_btn->connect(SceneStringName(pressed), callable_mp(this, &ProjectManager::_run_project));
			selection_bar->add_child(run_btn);

			// Primary action: Edit (+ options popup) as a segmented button.
			open_btn_container = memnew(HBoxContainer);
			open_btn_container->add_theme_constant_override("separation", 0);
			selection_bar->add_child(open_btn_container);

			open_btn = memnew(Button);
			open_btn->set_text(TTRC("Load Editor"));
			open_btn->set_theme_type_variation("PMPrimaryButtonLeft"); // UE blue combo CTA, label segment.
			open_btn->set_shortcut(ED_SHORTCUT("project_manager/edit_project", TTRC("Load Editor"), KeyModifierMask::CMD_OR_CTRL | Key::E));
			open_btn->connect(SceneStringName(pressed), callable_mp(this, &ProjectManager::_open_selected_projects_check_recovery_mode));
			open_btn_container->add_child(open_btn);

			open_options_btn = memnew(Button);
			open_options_btn->set_accessibility_name(TTRC("Options"));
			open_options_btn->set_theme_type_variation("PMPrimaryButtonRight"); // Chevron segment.
			open_options_btn->set_icon_alignment(HorizontalAlignment::HORIZONTAL_ALIGNMENT_CENTER);
			open_options_btn->connect(SceneStringName(pressed), callable_mp(this, &ProjectManager::_open_options_popup));
			open_btn_container->add_child(open_options_btn);

			open_options_popup = memnew(PopupMenu);
			open_options_popup->add_item(TTRC("Load in verbose mode"));
			open_options_popup->add_item(TTRC("Load in recovery mode"));
			open_options_popup->connect(SceneStringName(id_pressed), callable_mp(this, &ProjectManager::_on_open_options_selected));
			open_options_btn->add_child(open_options_popup);
		}
	}

	{
		shell_editor_host = memnew(Control);
		shell_editor_host->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		shell_editor_host->set_v_size_flags(Control::SIZE_EXPAND_FILL);
		_add_main_view(MAIN_VIEW_EDITOR, shell_editor_host);
		main_view_container->set_tab_hidden(main_view_container->get_tab_idx_from_control(shell_editor_host), true);
	}

	{
		VBoxContainer *shell_project_vb = memnew(VBoxContainer);
		shell_project_vb->add_theme_constant_override("separation", 8 * EDSCALE);
		_add_main_view(MAIN_VIEW_RUN, shell_project_vb);

		Label *shell_project_header = memnew(Label);
		shell_project_header->set_theme_type_variation("PMNavHeader");
		shell_project_header->set_text(TTRC("PROJECT"));
		shell_project_vb->add_child(shell_project_header);

		shell_project_name_label = memnew(Label);
		shell_project_name_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
		shell_project_vb->add_child(shell_project_name_label);

		shell_project_path_label = memnew(Label);
		shell_project_path_label->set_clip_text(true);
		shell_project_path_label->add_theme_color_override(SceneStringName(font_color), Color(1, 1, 1, 0.62));
		shell_project_vb->add_child(shell_project_path_label);

		Label *shell_session_header = memnew(Label);
		shell_session_header->set_theme_type_variation("PMNavHeader");
		shell_session_header->set_text(TTRC("SESSION"));
		shell_project_vb->add_child(shell_session_header);

		shell_session_label = memnew(Label);
		shell_session_label->set_clip_text(true);
		shell_project_vb->add_child(shell_session_label);

		shell_run_status = memnew(RichTextLabel);
		shell_run_status->set_fit_content(true);
		shell_run_status->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		shell_project_vb->add_child(shell_run_status);

		Control *shell_project_spacer = memnew(Control);
		shell_project_spacer->set_v_size_flags(Control::SIZE_EXPAND_FILL);
		shell_project_vb->add_child(shell_project_spacer);

		shell_edit_project_button = memnew(Button);
		shell_edit_project_button->set_text(TTRC("Load Editor"));
		shell_edit_project_button->connect(SceneStringName(pressed), callable_mp(this, &ProjectManager::_shell_edit_project_pressed));
		shell_project_vb->add_child(shell_edit_project_button);

		shell_run_project_button = memnew(Button);
		shell_run_project_button->set_text(TTRC("Run Project"));
		shell_run_project_button->connect(SceneStringName(pressed), callable_mp(this, &ProjectManager::_shell_run_project_pressed));
		shell_project_vb->add_child(shell_run_project_button);

		shell_open_project_button = memnew(Button);
		shell_open_project_button->set_text(TTRC("Open Classic Editor"));
		shell_open_project_button->connect(SceneStringName(pressed), callable_mp(this, &ProjectManager::_shell_open_project_pressed));
		shell_project_vb->add_child(shell_open_project_button);
	}

	{
		shell_logs_view = memnew(RichTextLabel);
		shell_logs_view->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		shell_logs_view->set_v_size_flags(Control::SIZE_EXPAND_FILL);
		_add_main_view(MAIN_VIEW_LOGS, shell_logs_view);
	}

	// Asset library view.
	if (AssetLibraryEditorPlugin::is_available()) {
		asset_library = memnew(EditorAssetLibrary(true));
		asset_library->set_name("AssetLibraryOverlay");
		asset_library->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		asset_library->set_v_size_flags(Control::SIZE_EXPAND_FILL);
		asset_library->hide();
		shell_chat_panel->add_child(asset_library);
		shell_asset_view = asset_library;
		asset_library->connect("install_asset", callable_mp(this, &ProjectManager::_install_project));
	} else {
		VBoxContainer *asset_library_filler = memnew(VBoxContainer);
		asset_library_filler->set_name("AssetLibraryOverlay");
		asset_library_filler->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		asset_library_filler->set_v_size_flags(Control::SIZE_EXPAND_FILL);
		asset_library_filler->hide();
		shell_chat_panel->add_child(asset_library_filler);
		shell_asset_view = asset_library_filler;
	}

	// Solers: BYOK AI model configuration view.
	{
		SolersPMAIView *ai_view = memnew(SolersPMAIView);
		ai_view->set_name("AIModelsOverlay");
		ai_view->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		ai_view->set_v_size_flags(Control::SIZE_EXPAND_FILL);
		ai_view->hide();
		shell_chat_panel->add_child(ai_view);
		shell_ai_view = ai_view;
	}

	// Footer bar.
	{
		HBoxContainer *footer_bar = memnew(HBoxContainer);
		footer_bar->set_alignment(BoxContainer::ALIGNMENT_END);
		footer_bar->add_theme_constant_override("separation", 20 * EDSCALE);
		main_vbox->add_child(footer_bar);

#ifdef ENGINE_UPDATE_CHECK_ENABLED
		EngineUpdateLabel *update_label = memnew(EngineUpdateLabel);
		footer_bar->add_child(update_label);
		update_label->connect("offline_clicked", callable_mp(this, &ProjectManager::_show_quick_settings));
#endif

		EditorVersionButton *version_btn = memnew(EditorVersionButton(EditorVersionButton::FORMAT_WITH_BUILD));
		// Fade the version label to be less prominent, but still readable.
		version_btn->set_self_modulate(Color(1, 1, 1, 0.6));
		footer_bar->add_child(version_btn);
	}

	// Dialogs.
	{
		quick_settings_dialog = memnew(QuickSettingsDialog);
		add_child(quick_settings_dialog);
		quick_settings_dialog->connect("restart_required", callable_mp(this, &ProjectManager::_restart_confirmed));

		scan_dir = memnew(EditorFileDialog);
		scan_dir->set_access(EditorFileDialog::ACCESS_FILESYSTEM);
		scan_dir->set_file_mode(EditorFileDialog::FILE_MODE_OPEN_DIR);
		scan_dir->set_title(TTRC("Select a Folder to Scan")); // Must be after mode or it's overridden.
		scan_dir->set_current_dir(EDITOR_GET("filesystem/directories/default_project_path"));
		add_child(scan_dir);
		scan_dir->connect("dir_selected", callable_mp(project_list, &ProjectList::find_projects));

		erase_missing_ask = memnew(ConfirmationDialog);
		erase_missing_ask->set_ok_button_text(TTRC("Remove All"));
		erase_missing_ask->get_ok_button()->connect(SceneStringName(pressed), callable_mp(this, &ProjectManager::_erase_missing_projects_confirm));
		add_child(erase_missing_ask);

		erase_ask = memnew(ConfirmationDialog);
		erase_ask->set_ok_button_text(TTRC("Remove"));
		erase_ask->get_ok_button()->connect(SceneStringName(pressed), callable_mp(this, &ProjectManager::_erase_project_confirm));
		add_child(erase_ask);

		VBoxContainer *erase_ask_vb = memnew(VBoxContainer);
		erase_ask->add_child(erase_ask_vb);

		erase_ask_label = memnew(Label);
		erase_ask_label->set_focus_mode(FOCUS_ACCESSIBILITY);
		erase_ask_vb->add_child(erase_ask_label);

		// Comment out for now until we have a better warning system to
		// ensure users delete their project only.
		//delete_project_contents = memnew(CheckBox);
		//delete_project_contents->set_text(TTRC("Also delete project contents (no undo!)"));
		//erase_ask_vb->add_child(delete_project_contents);

		multi_open_ask = memnew(ConfirmationDialog);
		multi_open_ask->set_ok_button_text(TTRC("Load Editor"));
		multi_open_ask->get_ok_button()->connect(SceneStringName(pressed), callable_mp(this, &ProjectManager::_open_selected_projects));
		add_child(multi_open_ask);

		multi_run_ask = memnew(ConfirmationDialog);
		multi_run_ask->set_ok_button_text(TTRC("Run"));
		multi_run_ask->get_ok_button()->connect(SceneStringName(pressed), callable_mp(this, &ProjectManager::_run_project_confirm));
		add_child(multi_run_ask);

		open_recovery_mode_ask = memnew(ConfirmationDialog);
		open_recovery_mode_ask->set_min_size(Size2(550, 70) * EDSCALE);
		open_recovery_mode_ask->set_autowrap(true);
		open_recovery_mode_ask->add_button(TTRC("Edit normally"))->connect(SceneStringName(pressed), callable_mp(this, &ProjectManager::_on_recovery_mode_popup_open_normal));
		open_recovery_mode_ask->set_ok_button_text(TTRC("Edit in Recovery Mode"));
		open_recovery_mode_ask->get_ok_button()->connect(SceneStringName(pressed), callable_mp(this, &ProjectManager::_on_recovery_mode_popup_open_recovery));
		add_child(open_recovery_mode_ask);

		ask_update_settings = memnew(ConfirmationDialog);
		add_child(ask_update_settings);
		ask_update_vb = memnew(VBoxContainer);
		ask_update_settings->add_child(ask_update_vb);
		ask_update_label = memnew(Label);
		ask_update_label->set_focus_mode(FOCUS_ACCESSIBILITY);
		ask_update_label->set_custom_minimum_size(Size2(300 * EDSCALE, 1));
		ask_update_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD);
		ask_update_label->set_v_size_flags(SIZE_EXPAND_FILL);
		ask_update_vb->add_child(ask_update_label);
		ask_update_backup = memnew(CheckBox);
		ask_update_backup->set_text(TTRC("Backup project first"));
		ask_update_backup->set_h_size_flags(SIZE_SHRINK_CENTER);
		ask_update_vb->add_child(ask_update_backup);
		ask_update_settings->get_ok_button()->connect(SceneStringName(pressed), callable_mp(this, &ProjectManager::_open_selected_projects_with_migration));
		int ed_swap_cancel_ok = EDITOR_GET("interface/editor/accept_dialog_cancel_ok_buttons");
		if (ed_swap_cancel_ok == 0) {
			ed_swap_cancel_ok = DisplayServer::get_singleton()->get_swap_cancel_ok() ? 2 : 1;
		}
		full_convert_button = ask_update_settings->add_button(TTRC("Convert Full Project"), ed_swap_cancel_ok != 2);
		full_convert_button->connect(SceneStringName(pressed), callable_mp(this, &ProjectManager::_full_convert_button_pressed));
		migration_guide_button = ask_update_settings->add_button(TTRC("See Migration Guide"), ed_swap_cancel_ok != 2);
		migration_guide_button->connect(SceneStringName(pressed), callable_mp(this, &ProjectManager::_migration_guide_button_pressed));

		ask_full_convert_dialog = memnew(ConfirmationDialog);
		ask_full_convert_dialog->set_autowrap(true);
		ask_full_convert_dialog->set_text(TTRC("This option will perform full project conversion, updating scenes, resources and scripts from Godot 3 to work in Godot 4.\n\nNote that this is a best-effort conversion, i.e. it makes upgrading the project easier, but it will not open out-of-the-box and will still require manual adjustments.\n\nIMPORTANT: Make sure to backup your project before converting, as this operation makes it impossible to open it in older versions of Godot."));
		ask_full_convert_dialog->connect(SceneStringName(confirmed), callable_mp(this, &ProjectManager::_perform_full_project_conversion));
		add_child(ask_full_convert_dialog);

		project_dialog = memnew(ProjectDialog);
		project_dialog->connect("projects_updated", callable_mp(this, &ProjectManager::_on_projects_updated));
		project_dialog->connect("project_created", callable_mp(this, &ProjectManager::_on_project_created));
		project_dialog->connect("project_duplicated", callable_mp(this, &ProjectManager::_on_project_duplicated));
		add_child(project_dialog);

		error_dialog = memnew(AcceptDialog);
		error_dialog->set_title(TTRC("Error"));
		add_child(error_dialog);

		about_dialog = memnew(EditorAbout);
		add_child(about_dialog);
	}

	// Tag management.
	{
		tag_manage_dialog = memnew(ConfirmationDialog);
		add_child(tag_manage_dialog);
		tag_manage_dialog->set_title(TTRC("Manage Project Tags"));
		tag_manage_dialog->get_ok_button()->connect(SceneStringName(pressed), callable_mp(this, &ProjectManager::_apply_project_tags));
		manage_tags_btn->connect(SceneStringName(pressed), callable_mp(this, &ProjectManager::_manage_project_tags));

		VBoxContainer *tag_vb = memnew(VBoxContainer);
		tag_manage_dialog->add_child(tag_vb);

		Label *label = memnew(Label(TTRC("Project Tags")));
		tag_vb->add_child(label);
		label->set_theme_type_variation("HeaderMedium");
		label->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);

		label = memnew(Label(TTRC("Click tag to remove it from the project.")));
		tag_vb->add_child(label);
		label->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);

		project_tags = memnew(HFlowContainer);
		tag_vb->add_child(project_tags);
		project_tags->set_custom_minimum_size(Vector2(0, 100) * EDSCALE);

		tag_vb->add_child(memnew(HSeparator));

		label = memnew(Label(TTRC("All Tags")));
		tag_vb->add_child(label);
		label->set_theme_type_variation("HeaderMedium");
		label->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);

		label = memnew(Label(TTRC("Click tag to add it to the project.")));
		tag_vb->add_child(label);
		label->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);

		all_tags = memnew(HFlowContainer);
		tag_vb->add_child(all_tags);
		all_tags->set_custom_minimum_size(Vector2(0, 100) * EDSCALE);

		tag_edit_error = memnew(Label);
		tag_vb->add_child(tag_edit_error);
		tag_edit_error->set_autowrap_mode(TextServer::AUTOWRAP_WORD);

		create_tag_dialog = memnew(ConfirmationDialog);
		tag_manage_dialog->add_child(create_tag_dialog);
		create_tag_dialog->set_title(TTRC("Create New Tag"));
		create_tag_dialog->get_ok_button()->connect(SceneStringName(pressed), callable_mp(this, &ProjectManager::_create_new_tag));

		tag_vb = memnew(VBoxContainer);
		create_tag_dialog->add_child(tag_vb);

		Label *info = memnew(Label(TTRC("Tags are capitalized automatically when displayed.")));
		tag_vb->add_child(info);

		new_tag_name = memnew(LineEdit);
		tag_vb->add_child(new_tag_name);
		new_tag_name->set_accessibility_name(TTRC("New Tag Name"));
		new_tag_name->set_placeholder(TTRC("example_tag (will display as Example Tag)"));
		new_tag_name->connect(SceneStringName(text_changed), callable_mp(this, &ProjectManager::_set_new_tag_name));
		new_tag_name->connect(SceneStringName(text_submitted), callable_mp(this, &ProjectManager::_create_new_tag).unbind(1));
		create_tag_dialog->connect("about_to_popup", callable_mp(new_tag_name, &LineEdit::clear));
		create_tag_dialog->connect("about_to_popup", callable_mp((Control *)new_tag_name, &Control::grab_focus).bind(false), CONNECT_DEFERRED);

		tag_error = memnew(Label);
		tag_error->set_focus_mode(FOCUS_ACCESSIBILITY);
		tag_vb->add_child(tag_error);

		create_tag_btn = memnew(Button);
		create_tag_btn->set_accessibility_name(TTRC("Create Tag"));
		all_tags->add_child(create_tag_btn);
		create_tag_btn->connect(SceneStringName(pressed), callable_mp((Window *)create_tag_dialog, &Window::popup_centered).bind(Vector2i(500, 0) * EDSCALE));

		_set_new_tag_name("");
	}

	// Initialize project list.
	{
		project_list->load_project_list();

		Ref<DirAccess> dir_access = DirAccess::create(DirAccess::AccessType::ACCESS_FILESYSTEM);

		String default_project_path = EDITOR_GET("filesystem/directories/default_project_path");
		if (!default_project_path.is_empty() && !dir_access->dir_exists(default_project_path)) {
			Error error = dir_access->make_dir_recursive(default_project_path);
			if (error != OK) {
				ERR_PRINT("Could not create default project directory at: " + default_project_path);
			}
		}

		String autoscan_path = EDITOR_GET("filesystem/directories/autoscan_project_path");
		if (!autoscan_path.is_empty()) {
			if (dir_access->dir_exists(autoscan_path)) {
				project_list->find_projects(autoscan_path);
			} else {
				Error error = dir_access->make_dir_recursive(autoscan_path);
				if (error != OK) {
					ERR_PRINT("Could not create project autoscan directory at: " + autoscan_path);
				}
			}
		}
		// Solers: start in the UE-style grid before first population so items
		// build into the card layout even if older settings saved list mode.
		project_list->set_display_mode(ProjectList::DISPLAY_GRID);
		view_grid_btn->set_pressed_no_signal(true);

		project_list->update_project_list();
		initialized = true;
	}

	// Extend menu bar to window title.
	if (can_expand) {
		DisplayServer::get_singleton()->process_events();
		DisplayServer::get_singleton()->window_set_flag(DisplayServer::WINDOW_FLAG_EXTEND_TO_TITLE, true, DisplayServer::MAIN_WINDOW_ID);
		title_bar->set_can_move_window(true);
		title_bar->connect(SceneStringName(item_rect_changed), callable_mp(this, &ProjectManager::_titlebar_resized));
	}

	_update_size_limits();
}

void ProjectManager::mount_shell_editor(EditorNode *p_editor_node) {
	ERR_FAIL_NULL(p_editor_node);
	ERR_FAIL_NULL(shell_editor_host);

	if (shell_editor_node == p_editor_node) {
		_select_main_view(MAIN_VIEW_EDITOR);
		return;
	}

	if (shell_editor_node) {
		_show_error(TTR("One project editor is already loaded in this workspace."));
		return;
	}

	shell_editor_node = p_editor_node;
	if (!shell_editor_node->get_parent()) {
		add_child(shell_editor_node);
	}

	shell_editor_gui = shell_editor_node->get_gui_base();
	ERR_FAIL_NULL(shell_editor_gui);

	if (shell_editor_gui->get_parent()) {
		shell_editor_gui->get_parent()->remove_child(shell_editor_gui);
	}
	shell_editor_host->add_child(shell_editor_gui);
	shell_editor_gui->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);

	active_editor_project_path = ProjectSettings::get_singleton()->get_resource_path();
	const String solers_session_id = OS::get_singleton()->get_environment("SOLERS_SESSION_ID");
	_set_shell_session(active_editor_project_path, solers_session_id);
	if (!solers_session_id.is_empty()) {
		OS::get_singleton()->unset_environment("SOLERS_SESSION_ID");
	}

	shell_workspace_collapsed = false;
	if (shell_workspace_panel) {
		shell_workspace_panel->show();
	}
	main_view_container->set_tab_hidden(main_view_container->get_tab_idx_from_control(main_view_map[MAIN_VIEW_EDITOR]), false);
	_select_main_view(MAIN_VIEW_EDITOR);
	_refresh_shell_project_panel();
}

ProjectManager::~ProjectManager() {
	singleton = nullptr;
#ifdef MODULE_SOLERS_AI_ENABLED
	if (solers_agent_runtime) {
		memdelete(solers_agent_runtime);
		solers_agent_runtime = nullptr;
	}
	solers_home_dock = nullptr;
#endif
	shell_editor_host = nullptr;
	shell_editor_gui = nullptr;
	if (!shell_editor_node && !EditorNode::get_singleton()) {
		EditorInspector::cleanup_plugins();

#if defined(MODULE_GDSCRIPT_ENABLED) || defined(MODULE_MONO_ENABLED)
		EditorHelpHighlighter::free_singleton();
#endif

		if (EditorSettings::get_singleton()) {
			EditorSettings::destroy();
		}

		EditorThemeManager::finalize();
	}
	shell_editor_node = nullptr;
}
