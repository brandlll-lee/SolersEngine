/**************************************************************************/
/*  project_manager.h                                                     */
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

#include "scene/gui/dialogs.h"
#include "scene/gui/scroll_container.h"

class CheckBox;
class EditorAbout;
class EditorAssetLibrary;
class EditorFileDialog;
class EditorNode;
class EditorTitleBar;
class HFlowContainer;
class HBoxContainer;
class HSplitContainer;
class LineEdit;
class MarginContainer;
class MenuButton;
class OptionButton;
class PanelContainer;
class PopupPanel;
class PopupMenu;
class ProjectDialog;
class ProjectList;
class QuickSettingsDialog;
class RichTextLabel;
class SolersAgentRuntime;
class SolersCategoryCard;
class SolersDock;
class Shortcut;
class TabBar;
class TabContainer;
class Texture2D;
class VBoxContainer;

class ProjectManager : public Control {
	GDCLASS(ProjectManager, Control);

	static ProjectManager *singleton;

	// Utility data.

	static Ref<Texture2D> _file_dialog_get_icon(const String &p_path);
	static Ref<Texture2D> _file_dialog_get_thumbnail(const String &p_path);

	HashMap<String, Ref<Texture2D>> icon_type_cache;

	void _build_icon_type_cache(Ref<Theme> p_theme);

	enum PostDuplicateAction {
		POST_DUPLICATE_ACTION_NONE,
		POST_DUPLICATE_ACTION_OPEN,
		POST_DUPLICATE_ACTION_FULL_CONVERSION,
	};

	PostDuplicateAction post_duplicate_action = POST_DUPLICATE_ACTION_NONE;

	// Main layout.

	Ref<Theme> theme;

	void _update_size_limits();
	void _update_theme(bool p_skip_creation = false);
	void _titlebar_resized();

	MarginContainer *root_container = nullptr;
	Panel *background_panel = nullptr;
	VBoxContainer *main_vbox = nullptr;

	EditorTitleBar *title_bar = nullptr;
	Control *left_menu_spacer = nullptr;
	Control *right_menu_spacer = nullptr;
	Button *title_bar_logo = nullptr;
	PanelContainer *shell_chat_panel = nullptr;
	PanelContainer *shell_workspace_panel = nullptr;
	HSplitContainer *shell_work_split = nullptr;
	PopupPanel *shell_session_popup = nullptr;
	VBoxContainer *shell_session_popup_list = nullptr;
	Control *shell_editor_host = nullptr;
	Control *shell_editor_gui = nullptr;
	EditorNode *shell_editor_node = nullptr;
	Control *shell_workspace_home = nullptr;
	VBoxContainer *shell_workspace_tool_list = nullptr;
	String shell_project_path;
	String shell_session_id;
	String active_editor_project_path;
	bool open_classic_editor = false;
	bool shell_workspace_collapsed = false;

	TabBar *shell_workspace_tab_bar = nullptr;
	TabContainer *main_view_container = nullptr;

	void _show_workspace_launcher(bool p_show_tabs);
	void _set_workspace_canvas_mode(bool p_canvas_mode);
	void _clear_workspace_tool_list();
	void _show_workspace_home();
	void _show_workspace_editor();
	void _rebuild_workspace_launcher();
	void _rebuild_workspace_scene_surface();
	void _rebuild_workspace_script_surface();
	void _rebuild_workspace_assets_surface();
	void _rebuild_workspace_game_surface();
	void _rebuild_workspace_studio_launcher();
	HBoxContainer *_rebuild_workspace_canvas_surface(const String &p_mode, const Ref<Texture2D> &p_icon, const String &p_title, const String &p_hint, Control *p_content = nullptr);
	void _add_workspace_section_label(const String &p_text);
	void _add_workspace_canvas_action(HBoxContainer *p_bar, const String &p_tool_id, const String &p_title, const Ref<Texture2D> &p_icon);
	void _add_workspace_tool_button(VBoxContainer *p_list, const String &p_tool_id, const String &p_title, const Ref<Texture2D> &p_icon, const Ref<Shortcut> &p_shortcut);
	void _workspace_tool_pressed(const String &p_tool_id, const String &p_title, const Ref<Texture2D> &p_icon);
	void _workspace_tool_tab_changed(int p_tab);
	void _workspace_tool_tab_close_pressed(int p_tab);
	void _activate_workspace_tool(const String &p_tool_id);
	int _find_workspace_tool_tab(const String &p_tool_id) const;
	void _toggle_shell_workspace();
	void _show_shell_chat();
	void _show_shell_global_view(Control *p_view);
	void _show_shell_session_popup(const Rect2 &p_anchor);
	void _shell_session_pressed(const String &p_session_id);
	void _shell_new_session_pressed();
	void _shell_asset_pressed();
	void _shell_ai_pressed();
	void _set_shell_session(const String &p_project_path, const String &p_session_id);
	void _load_shell_editor(const String &p_project_path);

	VBoxContainer *local_projects_vb = nullptr;
	EditorAssetLibrary *asset_library = nullptr;
	Control *shell_asset_view = nullptr;
	Control *shell_ai_view = nullptr;
	Control *shell_global_overlay_view = nullptr;
	SolersAgentRuntime *solers_agent_runtime = nullptr;
	SolersDock *solers_home_dock = nullptr;

	EditorAbout *about_dialog = nullptr;

	void _show_about();
	void _open_asset_library_confirmed();
	void _project_list_menu_option(int p_option);

	AcceptDialog *error_dialog = nullptr;

	void _show_error(const String &p_message, const Size2 &p_min_size = Size2());
	void _dim_window();

	// Quick settings.

	QuickSettingsDialog *quick_settings_dialog = nullptr;

	void _show_quick_settings();
	void _restart_confirmed();

	// Project list.

	VBoxContainer *empty_list_placeholder = nullptr;
	RichTextLabel *empty_list_message = nullptr;
	Button *empty_list_create_project = nullptr;
	Button *empty_list_import_project = nullptr;
	Button *empty_list_open_assetlib = nullptr;
	Label *empty_list_online_warning = nullptr;

	void _update_list_placeholder();

	ProjectList *project_list = nullptr;
	bool initialized = false;

	LineEdit *search_box = nullptr;
	Label *loading_label = nullptr;
	Label *sort_label = nullptr;
	OptionButton *filter_option = nullptr;
	PanelContainer *project_list_panel = nullptr;

	// Solers: view mode toggle (list / grid).
	Ref<ButtonGroup> view_mode_group;
	Button *view_list_btn = nullptr;
	Button *view_grid_btn = nullptr;
	void _set_project_view(int p_mode);

	// Solers: left navigation rail — custom-drawn UE-style category cards.
	VBoxContainer *nav_panel = nullptr;
	SolersCategoryCard *nav_new_card = nullptr;
	SolersCategoryCard *nav_all_card = nullptr;
	SolersCategoryCard *nav_asset_card = nullptr;
	SolersCategoryCard *nav_ai_card = nullptr;
	SolersCategoryCard *nav_settings_card = nullptr;
	void _nav_new_pressed();
	void _nav_card_pressed(SolersCategoryCard *p_card);
	void _deselect_all_nav_cards();
	void _nav_all_pressed();
	void _bottom_bar_separator(HBoxContainer *p_bar);

	// Solers: progressive-disclosure bottom bar. Low-frequency actions live in
	// two overflow menus; the selection group only exists while a selection does.
	enum BottomBarMenuOption {
		BOTTOM_MENU_SCAN,
		BOTTOM_MENU_ERASE_MISSING,
		BOTTOM_MENU_RENAME,
		BOTTOM_MENU_DUPLICATE,
		BOTTOM_MENU_MANAGE_TAGS,
		BOTTOM_MENU_ERASE,
	};
	void _on_library_more_id_pressed(int p_id);
	void _on_selection_more_id_pressed(int p_id);
	void _refresh_library_more_menu();
	void _position_overflow_popup(PopupMenu *p_popup, Control *p_anchor);

	MenuButton *library_more_btn = nullptr;
	MenuButton *selection_more_btn = nullptr;
	HBoxContainer *selection_bar = nullptr;

	Button *create_btn = nullptr;
	Button *import_btn = nullptr;
	Button *scan_btn = nullptr;
	Button *open_btn = nullptr;
	Button *open_options_btn = nullptr;
	Button *run_btn = nullptr;
	Button *rename_btn = nullptr;
	Button *duplicate_btn = nullptr;
	Button *manage_tags_btn = nullptr;
	Button *erase_btn = nullptr;
	Button *erase_missing_btn = nullptr;
	Button *donate_btn = nullptr;

	HBoxContainer *open_btn_container = nullptr;
	PopupMenu *open_options_popup = nullptr;

	EditorFileDialog *scan_dir = nullptr;

	ConfirmationDialog *erase_ask = nullptr;
	Label *erase_ask_label = nullptr;
	// Comment out for now until we have a better warning system to
	// ensure users delete their project only.
	//CheckBox *delete_project_contents = nullptr;
	ConfirmationDialog *erase_missing_ask = nullptr;
	ConfirmationDialog *multi_open_ask = nullptr;
	ConfirmationDialog *multi_run_ask = nullptr;
	ConfirmationDialog *open_recovery_mode_ask = nullptr;

	ProjectDialog *project_dialog = nullptr;

	void _scan_projects();
	void _run_project();
	void _run_project_confirm();
	void _open_selected_projects();
	void _open_selected_projects_with_migration();
	void _open_selected_projects_check_warnings();
	void _open_selected_projects_check_recovery_mode();

	void _install_project(const String &p_zip_path, const String &p_title);
	void _import_project();
	void _new_project();
	void _rename_project();
	void _duplicate_project();
	void _duplicate_project_with_action(PostDuplicateAction p_action);
	void _show_project_in_file_manager();
	void _erase_project();
	void _erase_missing_projects();
	void _erase_project_confirm();
	void _erase_missing_projects_confirm();
	void _update_project_buttons();
	void _open_options_popup();
	void _open_recovery_mode_ask(bool manual = false);
	void _open_donate_page();

	void _on_project_created(const String &dir, bool edit);
	void _on_project_duplicated(const String &p_original_path, const String &p_duplicate_path, bool p_edit);
	void _on_projects_updated();
	void _on_open_options_selected(int p_option);
	void _on_recovery_mode_popup_open_normal();
	void _on_recovery_mode_popup_open_recovery();

	void _on_order_option_changed(int p_idx);
	void _on_search_term_changed(const String &p_term);
	void _on_search_term_submitted(const String &p_text);

	// Project tag management.

	HashSet<String> tag_set;
	PackedStringArray current_project_tags;
	PackedStringArray forbidden_tag_characters{ "/", "\\", "-" };

	ConfirmationDialog *tag_manage_dialog = nullptr;
	HFlowContainer *project_tags = nullptr;
	HFlowContainer *all_tags = nullptr;
	Label *tag_edit_error = nullptr;

	Button *create_tag_btn = nullptr;
	ConfirmationDialog *create_tag_dialog = nullptr;
	LineEdit *new_tag_name = nullptr;
	Label *tag_error = nullptr;

	void _manage_project_tags();
	void _add_project_tag(const String &p_tag);
	void _delete_project_tag(const String &p_tag);
	void _apply_project_tags();
	void _set_new_tag_name(const String p_name);
	void _create_new_tag();

	// Project converter/migration tool.

	ConfirmationDialog *ask_full_convert_dialog = nullptr;
	ConfirmationDialog *ask_update_settings = nullptr;
	VBoxContainer *ask_update_vb = nullptr;
	Label *ask_update_label = nullptr;
	CheckBox *ask_update_backup = nullptr;
	Button *full_convert_button = nullptr;
	Button *migration_guide_button = nullptr;

	String version_convert_feature;
	bool open_in_recovery_mode = false;
	bool open_in_verbose_mode = false;

#ifndef DISABLE_DEPRECATED
	void _minor_project_migrate();
#endif
	void _full_convert_button_pressed();
	void _migration_guide_button_pressed();
	void _perform_full_project_conversion();

	// Input and I/O.

	virtual void shortcut_input(const Ref<InputEvent> &p_ev) override;

	void _files_dropped(PackedStringArray p_files);

protected:
	void _notification(int p_what);

public:
	static ProjectManager *get_singleton() { return singleton; }

	static constexpr int DEFAULT_WINDOW_WIDTH = 1152;
	static constexpr int DEFAULT_WINDOW_HEIGHT = 800;

	// Project list.

	bool is_initialized() const { return initialized; }
	LineEdit *get_search_box();

	// Project tag management.

	void add_new_tag(const String &p_tag);
	void mount_shell_editor(EditorNode *p_editor_node);

	ProjectManager();
	~ProjectManager();
};
