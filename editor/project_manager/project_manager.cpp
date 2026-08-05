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
#include "core/input/input_event.h"
#include "core/os/keyboard.h"
#include "core/os/os.h"
#include "core/os/time.h"
#include "core/templates/hash_set.h"
#include "core/version.h"
#include "editor/asset_library/asset_library_editor_plugin.h"
#include "editor/docks/editor_dock.h"
#include "editor/docks/editor_dock_manager.h"
#include "editor/docks/filesystem_dock.h"
#include "editor/doc/editor_help.h"
#include "editor/editor_main_screen.h"
#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "editor/settings/editor_settings.h"
#include "editor/gui/editor_about.h"
#include "editor/gui/editor_file_dialog.h"
#include "editor/gui/editor_title_bar.h"
#include "editor/gui/editor_version_button.h"
#include "editor/inspector/editor_inspector.h"
#include "editor/project_manager/project_dialog.h"
#include "editor/project_manager/project_list.h"
#include "editor/project_manager/project_tag.h"
#include "editor/project_manager/solers_pm_ai_view.h"
#include "scene/gui/center_container.h"
#include "editor/project_manager/solers_pm_theme.h"
#include "editor/plugins/editor_plugin.h"
#include "editor/run/editor_run_bar.h"
#include "editor/settings/editor_settings.h"
#include "editor/themes/editor_scale.h"
#include "editor/themes/editor_theme_manager.h"
#include "main/app_icon.gen.h"
#include "main/main.h"
#include "scene/3d/node_3d.h"
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
#include "scene/gui/subviewport_container.h"
#include "scene/gui/tab_bar.h"
#include "scene/gui/tab_container.h"
#include "scene/gui/texture_rect.h"
#include "scene/resources/image_texture.h"
#include "scene/main/canvas_item.h"
#include "scene/main/viewport.h"
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
#include "modules/solers_ai/core/solers_trace.h"
#include "modules/solers_ai/editor/solers_agent_runtime.h"
#include "modules/solers_ai/editor/solers_dock.h"
#endif

constexpr int GODOT4_CONFIG_VERSION = 5;

// Lucide glyph bodies (24x24 viewBox, white stroke applied by the rasterizer;
// ISC license — see modules/solers_ai/UI_ICON_LICENSE.txt).
static const char *SOLERS_LUCIDE_PANELS = "<rect width=\"18\" height=\"18\" x=\"3\" y=\"3\" rx=\"2\"/><path d=\"M3 9h18\"/><path d=\"M9 21V9\"/>";
static const char *SOLERS_LUCIDE_FOLDER = "<path d=\"M20 20a2 2 0 0 0 2-2V8a2 2 0 0 0-2-2h-7.9a2 2 0 0 1-1.69-.9L9.6 3.9A2 2 0 0 0 7.93 3H4a2 2 0 0 0-2 2v13a2 2 0 0 0 2 2Z\"/>";
static const char *SOLERS_LUCIDE_SQUARE_PLUS = "<rect width=\"18\" height=\"18\" x=\"3\" y=\"3\" rx=\"2\"/><path d=\"M8 12h8\"/><path d=\"M12 8v8\"/>";
static const char *SOLERS_LUCIDE_SETTINGS = "<path d=\"M12.22 2h-.44a2 2 0 0 0-2 2v.18a2 2 0 0 1-1 1.73l-.43.25a2 2 0 0 1-2 0l-.15-.08a2 2 0 0 0-2.73.73l-.22.38a2 2 0 0 0 .73 2.73l.15.1a2 2 0 0 1 1 1.72v.51a2 2 0 0 1-1 1.74l-.15.09a2 2 0 0 0-.73 2.73l.22.38a2 2 0 0 0 2.73.73l.15-.08a2 2 0 0 1 2 0l.43.25a2 2 0 0 1 1 1.73V20a2 2 0 0 0 2 2h.44a2 2 0 0 0 2-2v-.18a2 2 0 0 1 1-1.73l.43-.25a2 2 0 0 1 2 0l.15.08a2 2 0 0 0 2.73-.73l.22-.39a2 2 0 0 0-.73-2.73l-.15-.08a2 2 0 0 1-1-1.74v-.5a2 2 0 0 1 1-1.74l.15-.09a2 2 0 0 0 .73-2.73l-.22-.38a2 2 0 0 0-2.73-.73l-.15.08a2 2 0 0 1-2 0l-.43-.25a2 2 0 0 1-1-1.73V4a2 2 0 0 0-2-2z\"/><circle cx=\"12\" cy=\"12\" r=\"3\"/>";

static void _solers_disable_preview_processing(Node *p_node) {
	if (!p_node) {
		return;
	}
	p_node->set_process_mode(Node::PROCESS_MODE_DISABLED);
	for (int i = 0; i < p_node->get_child_count(false); i++) {
		_solers_disable_preview_processing(p_node->get_child(i, false));
	}
}

ProjectManager *ProjectManager::singleton = nullptr;

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
			project_list->set_order_option((int)EDITOR_GET("project_manager/sorting_order"), false);

			if (app_shell) {
				_show_workspace_home();
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

			if (empty_list_message) {
				empty_list_message->set_text(TTR("You don't have any projects yet."));
			}

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
#ifdef MODULE_SOLERS_AI_ENABLED
		if (solers_home_dock) {
			if (SolersPMAIView *ai_view = solers_home_dock->get_provider_settings_view()) {
				ai_view->update_quick_popup_size_limits(maximum_popup_size);
			}
		}
#endif
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
		// Cursor-flat: panes share bg; spacing between title/shell/footer is the hairline only.
		main_vbox->add_theme_constant_override("separation", 0);

		background_panel->add_theme_style_override(SceneStringName(panel), get_theme_stylebox("Background", EditorStringName(EditorStyles)));
		const Ref<StyleBox> chrome_panel = get_theme_stylebox("Background", EditorStringName(EditorStyles));
		if (shell_chat_panel) {
			shell_chat_panel->add_theme_style_override(SceneStringName(panel), chrome_panel);
		}
		if (shell_workspace_panel) {
			shell_workspace_panel->add_theme_style_override(SceneStringName(panel), chrome_panel);
		}
		main_view_container->add_theme_style_override(SceneStringName(panel), get_theme_stylebox("panel_container", "ProjectManager"));
		if (shell_workspace_home) {
			shell_workspace_home->add_theme_style_override(SceneStringName(panel), get_theme_stylebox("workspace_home", "ProjectManager"));
		}

		// Project list.
		{
			if (loading_label) {
				loading_label->add_theme_font_override(SceneStringName(font), get_theme_font("bold", EditorStringName(EditorFonts)));
			}
			if (create_tag_btn) {
				create_tag_btn->set_button_icon(get_editor_theme_icon("Add"));
			}
			if (tag_error) {
				tag_error->add_theme_color_override(SceneStringName(font_color), get_theme_color("error_color", EditorStringName(Editor)));
			}
			if (tag_edit_error) {
				tag_edit_error->add_theme_color_override(SceneStringName(font_color), get_theme_color("error_color", EditorStringName(Editor)));
			}
		}


		// Dialogs
		migration_guide_button->set_button_icon(get_editor_theme_icon("ExternalLink"));

		// Asset library popup.
		if (asset_library && EDITOR_GET("interface/theme/style") == "Classic") {
			// Removes extra border margins.
			asset_library->add_theme_style_override(SceneStringName(panel), memnew(StyleBoxEmpty));
		}
	}
	DisplayServer::get_singleton()->window_set_color(theme->get_color("background", EditorStringName(Editor)));
}

void ProjectManager::_show_workspace_launcher(bool p_show_tabs) {
	if (!main_view_container || !shell_workspace_home || !shell_editor_host) {
		return;
	}
	_set_workspace_canvas_mode(false);
	if (shell_workspace_tab_bar) {
		shell_workspace_tab_bar->set_visible(p_show_tabs && shell_workspace_tab_bar->get_tab_count() > 0);
	}
	main_view_container->set_tabs_visible(false);
	const int home_idx = main_view_container->get_tab_idx_from_control(shell_workspace_home);
	if (home_idx >= 0) {
		main_view_container->set_tab_hidden(home_idx, false);
		main_view_container->set_current_tab(home_idx);
	}
	const int host_idx = main_view_container->get_tab_idx_from_control(shell_editor_host);
	if (host_idx >= 0) {
		main_view_container->set_tab_hidden(host_idx, true);
	}
}

void ProjectManager::_set_workspace_canvas_mode(bool p_canvas_mode) {
	if (shell_editor_node) {
		shell_editor_node->set_distraction_free_mode(p_canvas_mode);
	}
	if (EditorTitleBar *editor_title_bar = EditorNode::get_title_bar()) {
		editor_title_bar->set_visible(!p_canvas_mode);
	}
}

void ProjectManager::_clear_workspace_tool_list() {
	if (!shell_workspace_tool_list) {
		return;
	}
	while (shell_workspace_tool_list->get_child_count() > 0) {
		Node *child = shell_workspace_tool_list->get_child(0);
		shell_workspace_tool_list->remove_child(child);
		child->queue_free();
	}
}

void ProjectManager::_add_workspace_section_label(const String &p_text) {
	Label *section = memnew(Label(p_text));
	section->set_theme_type_variation("PMWorkspaceSection");
	shell_workspace_tool_list->add_child(section);
}

HBoxContainer *ProjectManager::_rebuild_workspace_canvas_surface(const String &p_mode, const Ref<Texture2D> &p_icon, const String &p_title, const String &p_hint, Control *p_content) {
	if (!shell_workspace_tool_list) {
		return nullptr;
	}

	_clear_workspace_tool_list();

	PanelContainer *canvas = memnew(PanelContainer);
	canvas->set_theme_type_variation("PMWorkspaceCanvas");
	canvas->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	canvas->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	shell_workspace_tool_list->add_child(canvas);

	MarginContainer *canvas_margin = memnew(MarginContainer);
	canvas_margin->add_theme_constant_override("margin_left", 18 * EDSCALE);
	canvas_margin->add_theme_constant_override("margin_right", 18 * EDSCALE);
	canvas_margin->add_theme_constant_override("margin_top", 16 * EDSCALE);
	canvas_margin->add_theme_constant_override("margin_bottom", 14 * EDSCALE);
	canvas->add_child(canvas_margin);

	VBoxContainer *canvas_root = memnew(VBoxContainer);
	canvas_root->add_theme_constant_override("separation", 0);
	canvas_root->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	canvas_root->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	canvas_margin->add_child(canvas_root);

	HBoxContainer *header = memnew(HBoxContainer);
	header->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	header->set_custom_minimum_size(Size2(0, 34) * EDSCALE);
	canvas_root->add_child(header);

	PanelContainer *pill = memnew(PanelContainer);
	pill->set_theme_type_variation("PMWorkspaceModePill");
	header->add_child(pill);

	MarginContainer *pill_margin = memnew(MarginContainer);
	pill_margin->add_theme_constant_override("margin_left", 10 * EDSCALE);
	pill_margin->add_theme_constant_override("margin_right", 11 * EDSCALE);
	pill_margin->add_theme_constant_override("margin_top", 5 * EDSCALE);
	pill_margin->add_theme_constant_override("margin_bottom", 5 * EDSCALE);
	pill->add_child(pill_margin);

	HBoxContainer *pill_row = memnew(HBoxContainer);
	pill_row->add_theme_constant_override("separation", 7 * EDSCALE);
	pill_margin->add_child(pill_row);

	TextureRect *pill_icon = memnew(TextureRect);
	pill_icon->set_texture(p_icon);
	pill_icon->set_custom_minimum_size(Size2(16, 16) * EDSCALE);
	pill_icon->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
	pill_icon->set_stretch_mode(TextureRect::STRETCH_KEEP_ASPECT_CENTERED);
	pill_row->add_child(pill_icon);

	Label *pill_label = memnew(Label(p_mode));
	pill_label->set_theme_type_variation("PMWorkspaceModePillLabel");
	pill_label->set_vertical_alignment(VERTICAL_ALIGNMENT_CENTER);
	pill_row->add_child(pill_label);

	Control *header_spacer = memnew(Control);
	header_spacer->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	header->add_child(header_spacer);

	VBoxContainer *center = memnew(VBoxContainer);
	center->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	center->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	canvas_root->add_child(center);

	if (p_content) {
		p_content->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		p_content->set_v_size_flags(Control::SIZE_EXPAND_FILL);
		center->add_child(p_content);
	} else {
		Control *top_spacer = memnew(Control);
		top_spacer->set_v_size_flags(Control::SIZE_EXPAND_FILL);
		center->add_child(top_spacer);

		VBoxContainer *copy = memnew(VBoxContainer);
		copy->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		copy->add_theme_constant_override("separation", 8 * EDSCALE);
		center->add_child(copy);

		TextureRect *hero_icon = memnew(TextureRect);
		hero_icon->set_texture(p_icon);
		hero_icon->set_custom_minimum_size(Size2(54, 54) * EDSCALE);
		hero_icon->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
		hero_icon->set_stretch_mode(TextureRect::STRETCH_KEEP_ASPECT_CENTERED);
		hero_icon->set_h_size_flags(Control::SIZE_SHRINK_CENTER);
		hero_icon->set_modulate(Color(1, 1, 1, 0.22f));
		copy->add_child(hero_icon);

		Label *title = memnew(Label(p_title));
		title->set_theme_type_variation("PMWorkspaceCanvasTitle");
		title->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
		title->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		copy->add_child(title);

		Label *hint = memnew(Label(p_hint));
		hint->set_theme_type_variation("PMWorkspaceCanvasHint");
		hint->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
		hint->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
		hint->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		copy->add_child(hint);

		Control *bottom_spacer = memnew(Control);
		bottom_spacer->set_v_size_flags(Control::SIZE_EXPAND_FILL);
		center->add_child(bottom_spacer);
	}

	HBoxContainer *toolbar_wrap = memnew(HBoxContainer);
	toolbar_wrap->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	toolbar_wrap->set_alignment(BoxContainer::ALIGNMENT_CENTER);
	canvas_root->add_child(toolbar_wrap);

	PanelContainer *toolbar = memnew(PanelContainer);
	toolbar->set_theme_type_variation("PMWorkspaceCanvasToolbar");
	toolbar_wrap->add_child(toolbar);

	MarginContainer *toolbar_margin = memnew(MarginContainer);
	toolbar_margin->add_theme_constant_override("margin_left", 6 * EDSCALE);
	toolbar_margin->add_theme_constant_override("margin_right", 6 * EDSCALE);
	toolbar_margin->add_theme_constant_override("margin_top", 5 * EDSCALE);
	toolbar_margin->add_theme_constant_override("margin_bottom", 5 * EDSCALE);
	toolbar->add_child(toolbar_margin);

	HBoxContainer *toolbar_actions = memnew(HBoxContainer);
	toolbar_actions->add_theme_constant_override("separation", 6 * EDSCALE);
	toolbar_margin->add_child(toolbar_actions);
	return toolbar_actions;
}

void ProjectManager::_add_workspace_canvas_action(HBoxContainer *p_bar, const String &p_tool_id, const String &p_title, const Ref<Texture2D> &p_icon) {
	ERR_FAIL_NULL(p_bar);

	Button *button = memnew(Button(p_title));
	button->set_theme_type_variation("PMWorkspaceCanvasAction");
	button->set_button_icon(p_icon);
	button->set_icon_alignment(HORIZONTAL_ALIGNMENT_LEFT);
	button->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	button->set_clip_text(true);
	button->set_custom_minimum_size(Size2(0, 30) * EDSCALE);
	button->set_tooltip_text(p_title);
	button->connect(SceneStringName(pressed), callable_mp(this, &ProjectManager::_workspace_tool_pressed).bind(p_tool_id, p_title, p_icon));
	p_bar->add_child(button);
}

void ProjectManager::_show_workspace_home() {
	_show_workspace_launcher(false);
	_rebuild_workspace_launcher();
}

void ProjectManager::_show_workspace_editor() {
	if (!main_view_container || !shell_editor_host) {
		return;
	}
	if (shell_workspace_tab_bar && shell_workspace_tab_bar->get_tab_count() > 0) {
		shell_workspace_tab_bar->show();
	}
	main_view_container->set_tabs_visible(false);
	const int home_idx = main_view_container->get_tab_idx_from_control(shell_workspace_home);
	if (home_idx >= 0) {
		main_view_container->set_tab_hidden(home_idx, true);
	}
	const int host_idx = main_view_container->get_tab_idx_from_control(shell_editor_host);
	if (host_idx >= 0) {
		main_view_container->set_tab_hidden(host_idx, false);
		main_view_container->set_current_tab(host_idx);
	}
}

void ProjectManager::_rebuild_workspace_launcher() {
	if (!shell_workspace_tool_list) {
		return;
	}

	_clear_workspace_tool_list();

	_add_workspace_tool_button(shell_workspace_tool_list, "scene", TTR("Scene"), SolersPMTheme::mono_icon(get_editor_theme_icon(SNAME("PackedScene"))), Ref<Shortcut>());
	_add_workspace_tool_button(shell_workspace_tool_list, "script", TTR("Script"), SolersPMTheme::mono_icon(get_editor_theme_icon(SNAME("Script"))), Ref<Shortcut>());
	_add_workspace_tool_button(shell_workspace_tool_list, "assets", TTR("Assets"), SolersPMTheme::mono_icon(get_editor_theme_icon(SNAME("Folder"))), Ref<Shortcut>());
	_add_workspace_tool_button(shell_workspace_tool_list, "game", TTR("Game"), SolersPMTheme::mono_icon(get_editor_theme_icon(SNAME("Play"))), Ref<Shortcut>());
	_add_workspace_tool_button(shell_workspace_tool_list, "studio", TTR("Studio"), SolersPMTheme::lucide_icon(SOLERS_LUCIDE_PANELS), Ref<Shortcut>());
}

void ProjectManager::_rebuild_workspace_scene_surface() {
	const Ref<Texture2D> icon = SolersPMTheme::mono_icon(get_editor_theme_icon(SNAME("PackedScene")));
	Control *preview = nullptr;
	Node *edited_scene = shell_editor_node ? shell_editor_node->get_edited_scene() : nullptr;
	Node *preview_root = edited_scene ? edited_scene->duplicate(0) : nullptr;
	if (preview_root) {
		_solers_disable_preview_processing(preview_root);

		SubViewportContainer *preview_container = memnew(SubViewportContainer);
		preview_container->set_stretch(true);
		preview_container->set_mouse_target(false);

		SubViewport *preview_viewport = memnew(SubViewport);
		preview_viewport->set_size(Size2i(1280, 720));
		preview_viewport->set_update_mode(SubViewport::UPDATE_WHEN_VISIBLE);
		preview_container->add_child(preview_viewport);
		preview_viewport->add_child(preview_root);
		preview = preview_container;
	}

	HBoxContainer *actions = _rebuild_workspace_canvas_surface(TTR("Scene"), icon, TTR("Scene canvas"), TTR("Open a scene to preview it here."), preview);
	_add_workspace_canvas_action(actions, "run:main", TTR("Run"), SolersPMTheme::mono_icon(get_editor_theme_icon(SNAME("Play"))));
}

void ProjectManager::_rebuild_workspace_script_surface() {
	HBoxContainer *actions = _rebuild_workspace_canvas_surface(TTR("Script"), SolersPMTheme::mono_icon(get_editor_theme_icon(SNAME("Script"))), TTR("Script canvas"), TTR("Describe the gameplay logic you want, or open Studio for manual code."));
	_add_workspace_canvas_action(actions, "assets", TTR("Assets"), SolersPMTheme::mono_icon(get_editor_theme_icon(SNAME("Folder"))));
}

void ProjectManager::_rebuild_workspace_assets_surface() {
	HBoxContainer *actions = _rebuild_workspace_canvas_surface(TTR("Assets"), SolersPMTheme::mono_icon(get_editor_theme_icon(SNAME("Folder"))), TTR("Asset canvas"), TTR("Keep project files quiet here; open Studio only for detailed asset work."));
	_add_workspace_canvas_action(actions, "studio:assetlib", TTR("Asset Library"), SolersPMTheme::mono_icon(get_editor_theme_icon(SNAME("AssetLib"))));
	_add_workspace_canvas_action(actions, "studio:filesystem", TTR("Project Files"), SolersPMTheme::mono_icon(get_editor_theme_icon(SNAME("Folder"))));
}

void ProjectManager::_rebuild_workspace_game_surface() {
	HBoxContainer *actions = _rebuild_workspace_canvas_surface(TTR("Game"), SolersPMTheme::mono_icon(get_editor_theme_icon(SNAME("Play"))), TTR("Game preview"), TTR("Run the project to test the current build."));
	_add_workspace_canvas_action(actions, "run:main", TTR("Run"), SolersPMTheme::mono_icon(get_editor_theme_icon(SNAME("Play"))));
	_add_workspace_canvas_action(actions, "run:stop", TTR("Stop"), SolersPMTheme::mono_icon(get_editor_theme_icon(SNAME("Stop"))));
}

void ProjectManager::_rebuild_workspace_studio_launcher() {
	if (!shell_workspace_tool_list) {
		return;
	}

	_clear_workspace_tool_list();

	EditorMainScreen *main_screen = EditorNode::get_editor_main_screen();
	if (main_screen) {
		_add_workspace_section_label(TTR("Main Screens"));

		for (int i = 0; i < main_screen->get_plugin_count(); i++) {
			if (!main_screen->is_button_enabled(i)) {
				continue;
			}
			EditorPlugin *plugin = main_screen->get_plugin(i);
			if (!plugin) {
				continue;
			}
			Ref<Texture2D> icon = plugin->get_plugin_icon();
			if (icon.is_null() && has_theme_icon(plugin->get_plugin_name(), EditorStringName(EditorIcons))) {
				icon = get_editor_theme_icon(plugin->get_plugin_name());
			}
			_add_workspace_tool_button(shell_workspace_tool_list, "studio:main:" + itos(i), plugin->get_plugin_name(), SolersPMTheme::mono_icon(icon), Ref<Shortcut>());
		}
	}

	EditorDockManager *dock_manager = EditorDockManager::get_singleton();
	if (dock_manager) {
		_add_workspace_section_label(TTR("Docks"));

		for (int i = 0; i < dock_manager->get_dock_count(); i++) {
			EditorDock *dock = dock_manager->get_dock(i);
			if (!dock || !dock->is_enabled() || (!dock->is_global() && dock->get_default_slot() != EditorDock::DOCK_SLOT_BOTTOM)) {
				continue;
			}
			Ref<Texture2D> icon = dock->get_dock_icon();
			if (icon.is_null() && !dock->get_icon_name().is_empty() && has_theme_icon(dock->get_icon_name(), EditorStringName(EditorIcons))) {
				icon = get_editor_theme_icon(dock->get_icon_name());
			}
			_add_workspace_tool_button(shell_workspace_tool_list, "studio:dock:" + uitos(dock->get_instance_id()), dock->get_display_title(), SolersPMTheme::mono_icon(icon), dock->get_dock_shortcut());
		}
	}
}

void ProjectManager::_add_workspace_tool_button(VBoxContainer *p_list, const String &p_tool_id, const String &p_title, const Ref<Texture2D> &p_icon, const Ref<Shortcut> &p_shortcut) {
	Button *button = memnew(Button);
	button->set_theme_type_variation("PMWorkspaceTool");
	button->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	button->set_custom_minimum_size(Size2(0, 48) * EDSCALE);
	button->set_tooltip_text(p_title);

	MarginContainer *margin = memnew(MarginContainer);
	margin->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
	margin->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	margin->add_theme_constant_override("margin_left", 13 * EDSCALE);
	margin->add_theme_constant_override("margin_right", 12 * EDSCALE);
	button->add_child(margin);

	HBoxContainer *row = memnew(HBoxContainer);
	row->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	row->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	row->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	row->add_theme_constant_override("separation", 10 * EDSCALE);
	margin->add_child(row);

	TextureRect *icon = memnew(TextureRect);
	icon->set_texture(p_icon);
	icon->set_custom_minimum_size(Size2(18, 18) * EDSCALE);
	icon->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
	icon->set_stretch_mode(TextureRect::STRETCH_KEEP_ASPECT_CENTERED);
	icon->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	row->add_child(icon);

	Label *title = memnew(Label(p_title));
	title->set_theme_type_variation("PMWorkspaceToolTitle");
	title->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	title->set_vertical_alignment(VERTICAL_ALIGNMENT_CENTER);
	title->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	title->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	row->add_child(title);

	if (p_shortcut.is_valid()) {
		const String shortcut_text = p_shortcut->get_as_text();
		if (!shortcut_text.is_empty()) {
			button->set_tooltip_text(p_title + "\n" + shortcut_text);
			Label *shortcut = memnew(Label(shortcut_text));
			shortcut->set_theme_type_variation("PMWorkspaceShortcut");
			shortcut->set_vertical_alignment(VERTICAL_ALIGNMENT_CENTER);
			shortcut->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
			row->add_child(shortcut);
		}
	}

	button->connect(SceneStringName(pressed), callable_mp(this, &ProjectManager::_workspace_tool_pressed).bind(p_tool_id, p_title, p_icon));
	p_list->add_child(button);
}

void ProjectManager::_workspace_tool_pressed(const String &p_tool_id, const String &p_title, const Ref<Texture2D> &p_icon) {
	ERR_FAIL_NULL(shell_workspace_tab_bar);
	if (p_tool_id == "home" || p_tool_id == "scene" || p_tool_id == "script" || p_tool_id == "assets" || p_tool_id == "game" || p_tool_id == "studio" || p_tool_id.begins_with("run:")) {
		_activate_workspace_tool(p_tool_id);
		return;
	}

	int tab_idx = _find_workspace_tool_tab(p_tool_id);
	if (tab_idx < 0) {
		shell_workspace_tab_bar->add_tab(p_title, p_icon);
		tab_idx = shell_workspace_tab_bar->get_tab_count() - 1;
		shell_workspace_tab_bar->set_tab_metadata(tab_idx, p_tool_id);
		shell_workspace_tab_bar->set_tab_tooltip(tab_idx, p_title);
		shell_workspace_tab_bar->set_tab_icon_max_width(tab_idx, 18 * EDSCALE);
	}
	if (shell_workspace_tab_bar->get_current_tab() != tab_idx) {
		shell_workspace_tab_bar->set_current_tab(tab_idx);
	} else {
		_workspace_tool_tab_changed(tab_idx);
	}
}

void ProjectManager::_workspace_tool_tab_changed(int p_tab) {
	if (!shell_workspace_tab_bar) {
		return;
	}
	if (p_tab < 0 || p_tab >= shell_workspace_tab_bar->get_tab_count()) {
		return;
	}
	_activate_workspace_tool(String(shell_workspace_tab_bar->get_tab_metadata(p_tab)));
}

void ProjectManager::_workspace_tool_tab_close_pressed(int p_tab) {
	if (!shell_workspace_tab_bar) {
		return;
	}
	if (p_tab < 0 || p_tab >= shell_workspace_tab_bar->get_tab_count()) {
		return;
	}
	shell_workspace_tab_bar->remove_tab(p_tab);
	if (shell_workspace_tab_bar->get_tab_count() == 0) {
		_show_workspace_home();
		return;
	}
	const int next_tab = shell_workspace_tab_bar->get_current_tab();
	if (next_tab >= 0) {
		_workspace_tool_tab_changed(next_tab);
	}
}

void ProjectManager::_activate_workspace_tool(const String &p_tool_id) {
	if (p_tool_id == "home") {
		_show_workspace_home();
		return;
	}
	if (p_tool_id == "scene") {
		_show_workspace_launcher(false);
		_rebuild_workspace_scene_surface();
		return;
	}
	if (p_tool_id == "script") {
		_show_workspace_launcher(false);
		_rebuild_workspace_script_surface();
		return;
	}
	if (p_tool_id == "assets") {
		_show_workspace_launcher(false);
		_rebuild_workspace_assets_surface();
		return;
	}
	if (p_tool_id == "game") {
		_show_workspace_launcher(false);
		_rebuild_workspace_game_surface();
		return;
	}
	if (p_tool_id == "studio") {
		_show_workspace_launcher(true);
		_rebuild_workspace_studio_launcher();
		return;
	}
	if (p_tool_id == "run:main") {
		if (EditorRunBar::get_singleton()) {
			EditorRunBar::get_singleton()->play_main_scene(false, Vector<String>());
		}
		return;
	}
	if (p_tool_id == "run:stop") {
		if (EditorRunBar::get_singleton()) {
			EditorRunBar::get_singleton()->stop_playing();
		}
		return;
	}
	if (p_tool_id == "studio:scene") {
		_show_workspace_editor();
		EditorMainScreen *main_screen = EditorNode::get_editor_main_screen();
		if (main_screen) {
			Node *root = shell_editor_node ? shell_editor_node->get_edited_scene() : nullptr;
			int target_screen = -1;
			if (Object::cast_to<Node3D>(root)) {
				target_screen = EditorMainScreen::EDITOR_3D;
			} else if (Object::cast_to<CanvasItem>(root)) {
				target_screen = EditorMainScreen::EDITOR_2D;
			}
			if (target_screen >= 0 && main_screen->is_button_enabled(target_screen)) {
				main_screen->select(target_screen);
			}
		}
		_set_workspace_canvas_mode(false);
		return;
	}
	if (p_tool_id == "studio:script" || p_tool_id == "studio:game" || p_tool_id == "studio:assetlib") {
		_show_workspace_editor();
		EditorMainScreen *main_screen = EditorNode::get_editor_main_screen();
		const int target_screen = p_tool_id == "studio:script" ? EditorMainScreen::EDITOR_SCRIPT : (p_tool_id == "studio:game" ? EditorMainScreen::EDITOR_GAME : EditorMainScreen::EDITOR_ASSETLIB);
		if (main_screen && main_screen->is_button_enabled(target_screen)) {
			main_screen->select(target_screen);
		}
		_set_workspace_canvas_mode(false);
		return;
	}
	if (p_tool_id.begins_with("studio:main:")) {
		_show_workspace_editor();
		EditorMainScreen *main_screen = EditorNode::get_editor_main_screen();
		const int target_screen = p_tool_id.substr(12).to_int();
		if (main_screen && target_screen >= 0 && target_screen < main_screen->get_plugin_count() && main_screen->is_button_enabled(target_screen)) {
			main_screen->select(target_screen);
		}
		_set_workspace_canvas_mode(false);
		return;
	}
	if (p_tool_id == "studio:filesystem") {
		_show_workspace_editor();
		_set_workspace_canvas_mode(false);
		if (FileSystemDock *filesystem_dock = FileSystemDock::get_singleton()) {
			if (EditorDockManager::get_singleton()) {
				EditorDockManager::get_singleton()->focus_dock(filesystem_dock);
			}
		}
		return;
	}
	if (p_tool_id.begins_with("studio:file:")) {
		_show_workspace_editor();
		_set_workspace_canvas_mode(false);
		if (FileSystemDock *filesystem_dock = FileSystemDock::get_singleton()) {
			if (EditorDockManager::get_singleton()) {
				EditorDockManager::get_singleton()->focus_dock(filesystem_dock);
			}
			filesystem_dock->navigate_to_path(p_tool_id.substr(12));
		}
		return;
	}
	if (p_tool_id.begins_with("studio:dock:")) {
		_show_workspace_editor();
		_set_workspace_canvas_mode(false);
		Object *object = ObjectDB::get_instance(ObjectID((uint64_t)p_tool_id.substr(12).to_int()));
		EditorDock *dock = Object::cast_to<EditorDock>(object);
		if (dock && EditorDockManager::get_singleton()) {
			EditorDockManager::get_singleton()->focus_dock(dock);
		}
	}
}

int ProjectManager::_find_workspace_tool_tab(const String &p_tool_id) const {
	if (!shell_workspace_tab_bar) {
		return -1;
	}
	for (int i = 0; i < shell_workspace_tab_bar->get_tab_count(); i++) {
		if (String(shell_workspace_tab_bar->get_tab_metadata(i)) == p_tool_id) {
			return i;
		}
	}
	return -1;
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
	if (!EditorNode::get_singleton() && p_view != local_projects_vb && local_projects_vb) {
		if (shell_global_overlay_view && shell_global_overlay_view != p_view && shell_global_overlay_view != local_projects_vb) {
			shell_global_overlay_view->hide();
		}
		if (p_view->get_parent() != local_projects_vb) {
			if (p_view->get_parent()) {
				p_view->get_parent()->remove_child(p_view);
			}
			local_projects_vb->add_child(p_view);
		}
		if (project_list) {
			project_list->hide();
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
	if (p_view == local_projects_vb && project_list) {
		project_list->show();
	}
#ifndef ANDROID_ENABLED
#endif
}

void ProjectManager::_shell_session_pressed(const String &p_session_id) {
	_show_shell_chat();
	_set_shell_session(shell_project_path, p_session_id);
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
			solers_home_dock->load_chat_history(shell_session_id.is_empty() ? Array() : solers_agent_runtime->get_timeline_entries());
		}
	}
	if (solers_home_dock) {
		solers_home_dock->set_session_context(shell_project_path, shell_session_id);
	}
#endif
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
	if (solers_home_dock) {
		solers_home_dock->set_session_context(shell_project_path, shell_session_id);
	}
#else
	_show_shell_chat();
#endif
}

void ProjectManager::_shell_asset_pressed() {
	_open_asset_library_confirmed();
}

void ProjectManager::_load_shell_editor(const String &p_project_path) {
	if (!main_view_container) {
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
		_show_workspace_home();
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

// Quick settings — same Settings host as Manage providers (SolersDock).

void ProjectManager::_show_quick_settings() {
#ifdef MODULE_SOLERS_AI_ENABLED
	if (solers_home_dock) {
		solers_home_dock->open_provider_settings("quick");
	}
#endif
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
	if (!empty_list_message || !project_list) {
		return;
	}
	empty_list_message->set_visible(project_list->get_project_count() <= 0);
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
	if (!project_list) {
		return;
	}
	Vector<ProjectList::Item> selected_projects = project_list->get_selected_projects();
	if (selected_projects.size() == 1 && !selected_projects[0].missing) {
		const String selected_path = selected_projects[0].path;
		_set_shell_session(selected_path, selected_path == shell_project_path ? shell_session_id : String());
	}
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

LineEdit *ProjectManager::get_search_box() {
	return nullptr;
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

		if (ED_IS_SHORTCUT("project_manager/new_project", k)) {
			_new_project();
		} else if (ED_IS_SHORTCUT("project_manager/import_project", k)) {
			_import_project();
		} else if (ED_IS_SHORTCUT("project_manager/scan_projects", k)) {
			_scan_projects();
		} else if (ED_IS_SHORTCUT("project_manager/edit_project", k)) {
			_open_selected_projects_check_recovery_mode();
		} else if (ED_IS_SHORTCUT("project_manager/run_project", k)) {
			_run_project();
		} else if (ED_IS_SHORTCUT("project_manager/rename_project", k)) {
			_rename_project();
		} else if (ED_IS_SHORTCUT("project_manager/project_tags", k)) {
			_manage_project_tags();
		} else if (ED_IS_SHORTCUT("project_manager/remove_project", k)) {
			_erase_project();
		} else {
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
				case Key::A: {
					if (k->is_command_or_control_pressed()) {
						if (k->is_shift_pressed()) {
							project_list->deselect_all_visible_projects();
						} else {
							project_list->select_all_visible_projects();
						}
						_update_project_buttons();
					} else {
						keycode_handled = false;
					}
				} break;
				default: {
					keycode_handled = false;
				} break;
			}
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
	shell->add_theme_constant_override("separation", 0);
	main_vbox->add_child(shell);

	shell_work_split = memnew(HSplitContainer);
	shell_work_split->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	shell_work_split->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	shell_work_split->set_dragger_visibility(SplitContainer::DRAGGER_HIDDEN);
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
		solers_home_dock->set_session_select_callback(callable_mp(this, &ProjectManager::_shell_session_pressed));
		solers_home_dock->set_new_session_callback(callable_mp(this, &ProjectManager::_shell_new_session_pressed));
		solers_agent_runtime->bind_dock(solers_home_dock);
		shell_chat_panel->add_child(solers_home_dock);
		if (SolersPMAIView *settings_view = solers_home_dock->get_provider_settings_view()) {
			settings_view->connect("restart_required", callable_mp(this, &ProjectManager::_restart_confirmed));
		}

		set_process(true);
	}
#endif

	shell_workspace_panel = memnew(PanelContainer);
	shell_workspace_panel->set_name("SolersWorkspacePanel");
	shell_workspace_panel->set_custom_minimum_size(Size2(320, 0) * EDSCALE);
	shell_workspace_panel->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	shell_workspace_panel->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	shell_work_split->add_child(shell_workspace_panel);

	VBoxContainer *workspace_root = memnew(VBoxContainer);
	workspace_root->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	workspace_root->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	workspace_root->add_theme_constant_override("separation", 0);
	shell_workspace_panel->add_child(workspace_root);

	shell_workspace_tab_bar = memnew(TabBar);
	shell_workspace_tab_bar->set_name("SolersWorkspaceTabBar");
	shell_workspace_tab_bar->set_theme_type_variation("PMWorkspaceTabBar");
	shell_workspace_tab_bar->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	shell_workspace_tab_bar->set_tab_close_display_policy(TabBar::CLOSE_BUTTON_SHOW_ACTIVE_ONLY);
	shell_workspace_tab_bar->set_max_tab_width(220 * EDSCALE);
	shell_workspace_tab_bar->connect("tab_changed", callable_mp(this, &ProjectManager::_workspace_tool_tab_changed));
	shell_workspace_tab_bar->connect("tab_close_pressed", callable_mp(this, &ProjectManager::_workspace_tool_tab_close_pressed));
	shell_workspace_tab_bar->hide();
	workspace_root->add_child(shell_workspace_tab_bar);

	main_view_container = memnew(TabContainer);
	main_view_container->set_name("SolersWorkspaceTabs");
	main_view_container->set_theme_type_variation("TabContainerInner");
	main_view_container->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	main_view_container->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	main_view_container->set_tabs_visible(false);
	workspace_root->add_child(main_view_container);

	shell_workspace_home = memnew(PanelContainer);
	shell_workspace_home->set_name("Workspace");
	shell_workspace_home->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	shell_workspace_home->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	main_view_container->add_child(shell_workspace_home);

	ScrollContainer *workspace_scroll = memnew(ScrollContainer);
	workspace_scroll->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	workspace_scroll->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	shell_workspace_home->add_child(workspace_scroll);

	MarginContainer *workspace_margin = memnew(MarginContainer);
	workspace_margin->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	workspace_margin->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	workspace_margin->add_theme_constant_override("margin_left", 32 * EDSCALE);
	workspace_margin->add_theme_constant_override("margin_right", 32 * EDSCALE);
	workspace_margin->add_theme_constant_override("margin_top", 32 * EDSCALE);
	workspace_margin->add_theme_constant_override("margin_bottom", 32 * EDSCALE);
	workspace_scroll->add_child(workspace_margin);

	VBoxContainer *workspace_body = memnew(VBoxContainer);
	workspace_body->add_theme_constant_override("separation", 7 * EDSCALE);
	workspace_body->set_custom_minimum_size(Size2(668, 0) * EDSCALE);
	workspace_body->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	workspace_body->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	workspace_margin->add_child(workspace_body);

	shell_workspace_tool_list = memnew(VBoxContainer);
	shell_workspace_tool_list->add_theme_constant_override("separation", 7 * EDSCALE);
	shell_workspace_tool_list->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	shell_workspace_tool_list->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	workspace_body->add_child(shell_workspace_tool_list);

	shell_editor_host = memnew(Control);
	shell_editor_host->set_name("EditorHost");
	shell_editor_host->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	shell_editor_host->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	main_view_container->add_child(shell_editor_host);
	main_view_container->set_tab_hidden(main_view_container->get_tab_idx_from_control(shell_editor_host), true);

	// Project list view — Cursor-style centered home.
	{
		local_projects_vb = memnew(VBoxContainer);
		local_projects_vb->set_name("LocalProjectsOverlay");
		local_projects_vb->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		local_projects_vb->set_v_size_flags(Control::SIZE_EXPAND_FILL);
		local_projects_vb->add_theme_constant_override("separation", 0);
		local_projects_vb->hide();
		shell_chat_panel->add_child(local_projects_vb);

		Control *top_spacer = memnew(Control);
		top_spacer->set_v_size_flags(Control::SIZE_EXPAND_FILL);
		top_spacer->set_stretch_ratio(1.2);
		local_projects_vb->add_child(top_spacer);

		CenterContainer *home_center = memnew(CenterContainer);
		home_center->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		local_projects_vb->add_child(home_center);

		VBoxContainer *home_column = memnew(VBoxContainer);
		home_column->set_custom_minimum_size(Size2(580, 0) * EDSCALE);
		home_column->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		home_column->add_theme_constant_override("separation", 20 * EDSCALE);
		home_center->add_child(home_column);

		home_logo = memnew(TextureRect);
		home_logo->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
		home_logo->set_stretch_mode(TextureRect::STRETCH_KEEP_ASPECT_CENTERED);
		home_logo->set_custom_minimum_size(Size2(72, 72) * EDSCALE);
		home_logo->set_h_size_flags(Control::SIZE_SHRINK_CENTER);
		{
			Ref<Image> icon_img = memnew(Image(app_icon_png));
			home_logo->set_texture(ImageTexture::create_from_image(icon_img));
		}
		home_column->add_child(home_logo);

		HBoxContainer *tiles = memnew(HBoxContainer);
		tiles->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		tiles->add_theme_constant_override("separation", 12 * EDSCALE);
		home_column->add_child(tiles);

		auto make_tile = [&](const String &p_text, const char *p_lucide, const Callable &p_cb) {
			Button *btn = memnew(Button);
			btn->set_text(p_text);
			btn->set_button_icon(SolersPMTheme::lucide_icon(p_lucide, 18, 1.75f));
			btn->set_icon_alignment(HORIZONTAL_ALIGNMENT_LEFT);
			btn->set_text_alignment(HORIZONTAL_ALIGNMENT_LEFT);
			btn->set_h_size_flags(Control::SIZE_EXPAND_FILL);
			btn->set_custom_minimum_size(Size2(0, 44) * EDSCALE);
			btn->set_theme_type_variation("PMHomeTile");
			btn->connect(SceneStringName(pressed), p_cb);
			tiles->add_child(btn);
		};
		make_tile(TTRC("Import Project"), SOLERS_LUCIDE_FOLDER, callable_mp(this, &ProjectManager::_import_project));
		make_tile(TTRC("New Project"), SOLERS_LUCIDE_SQUARE_PLUS, callable_mp(this, &ProjectManager::_new_project));
		make_tile(TTRC("Solers Settings"), SOLERS_LUCIDE_SETTINGS, callable_mp(this, &ProjectManager::_show_quick_settings));

		Label *all_projects = memnew(Label(TTRC("All Projects")));
		all_projects->add_theme_color_override(SceneStringName(font_color), SolersPMTheme::make_tokens(Ref<Theme>()).text_dim);
		all_projects->add_theme_font_size_override(SceneStringName(font_size), MAX(11, (int)(12 * EDSCALE)));
		home_column->add_child(all_projects);

		loading_label = memnew(Label(TTRC("Loading, please wait...")));
		loading_label->set_accessibility_live(DisplayServer::AccessibilityLiveMode::LIVE_ASSERTIVE);
		loading_label->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
		loading_label->hide();
		home_column->add_child(loading_label);

		project_list = memnew(ProjectList);
		project_list->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		project_list->set_custom_minimum_size(Size2(0, ProjectList::ROW_MIN_HEIGHT_PX * 4) * EDSCALE);
		project_list->set_v_size_flags(Control::SIZE_EXPAND_FILL);
		project_list->connect(ProjectList::SIGNAL_LIST_CHANGED, callable_mp(this, &ProjectManager::_update_project_buttons));
		project_list->connect(ProjectList::SIGNAL_LIST_CHANGED, callable_mp(this, &ProjectManager::_update_list_placeholder));
		project_list->connect(ProjectList::SIGNAL_SELECTION_CHANGED, callable_mp(this, &ProjectManager::_update_project_buttons));
		project_list->connect(ProjectList::SIGNAL_PROJECT_ASK_OPEN, callable_mp(this, &ProjectManager::_open_selected_projects_check_recovery_mode));
		project_list->connect(ProjectList::SIGNAL_MENU_OPTION_SELECTED, callable_mp(this, &ProjectManager::_project_list_menu_option));
		home_column->add_child(project_list);

		empty_list_message = memnew(Label(TTRC("You don't have any projects yet.")));
		empty_list_message->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
		empty_list_message->add_theme_color_override(SceneStringName(font_color), SolersPMTheme::make_tokens(Ref<Theme>()).text_dim);
		empty_list_message->hide();
		home_column->add_child(empty_list_message);

		Control *bottom_spacer = memnew(Control);
		bottom_spacer->set_v_size_flags(Control::SIZE_EXPAND_FILL);
		bottom_spacer->set_stretch_ratio(1.0);
		local_projects_vb->add_child(bottom_spacer);

		// Register shortcuts without visible button hosts.
		ED_SHORTCUT("project_manager/new_project", TTRC("New Project"), KeyModifierMask::CMD_OR_CTRL | Key::N);
		ED_SHORTCUT("project_manager/import_project", TTRC("Import Project"), KeyModifierMask::CMD_OR_CTRL | Key::I);
		ED_SHORTCUT("project_manager/scan_projects", TTRC("Scan Projects"), KeyModifierMask::CMD_OR_CTRL | Key::S);
		ED_SHORTCUT("project_manager/project_tags", TTRC("Manage Tags"), KeyModifierMask::CMD_OR_CTRL | Key::T);
		ED_SHORTCUT("project_manager/rename_project", TTRC("Rename Project"), Key::F2);
		ED_SHORTCUT("project_manager/remove_project", TTRC("Remove Project"), Key::KEY_DELETE);
		ED_SHORTCUT("project_manager/run_project", TTRC("Run Project"), KeyModifierMask::CMD_OR_CTRL | Key::R);
		ED_SHORTCUT("project_manager/edit_project", TTRC("Load Editor"), KeyModifierMask::CMD_OR_CTRL | Key::E);
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

	// Footer bar — version only.
	{
		HBoxContainer *footer_bar = memnew(HBoxContainer);
		footer_bar->set_alignment(BoxContainer::ALIGNMENT_END);
		main_vbox->add_child(footer_bar);

		EditorVersionButton *version_btn = memnew(EditorVersionButton(EditorVersionButton::FORMAT_WITH_BUILD));
		version_btn->set_self_modulate(Color(1, 1, 1, 0.6));
		footer_bar->add_child(version_btn);
	}

	// Dialogs.
	{
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
	ERR_FAIL_NULL(main_view_container);
	ERR_FAIL_NULL(shell_editor_host);

	if (shell_workspace_tab_bar) {
		shell_workspace_tab_bar->clear_tabs();
	}

	if (shell_editor_node == p_editor_node) {
		_show_workspace_home();
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

	if (shell_editor_gui->get_parent() != shell_editor_host) {
		if (shell_editor_gui->get_parent()) {
			shell_editor_gui->get_parent()->remove_child(shell_editor_gui);
		}
		shell_editor_host->add_child(shell_editor_gui);
		shell_editor_gui->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
	}

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
	_rebuild_workspace_launcher();
	_show_workspace_home();
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
