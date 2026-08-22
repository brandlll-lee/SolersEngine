@tool
extends EditorPlugin

const IMPORT_PATH := "res://.solers_editor_import_contract.png"


func _exit_tree() -> void:
	DirAccess.remove_absolute(ProjectSettings.globalize_path(IMPORT_PATH))
	DirAccess.remove_absolute(ProjectSettings.globalize_path(IMPORT_PATH + ".import"))


func _check(condition: bool, message: String) -> bool:
	if condition:
		return true
	push_error(message)
	get_tree().quit(1)
	return false


func _contains_button_text(node: Node, text: String) -> bool:
	if node is Button and node.text == text:
		return true
	for child in node.get_children():
		if _contains_button_text(child, text):
			return true
	return false


func _find_item(list: ItemList, text: String) -> int:
	for index in list.item_count:
		if list.get_item_text(index) == text:
			return index
	return -1


func _check_editor_authority(base: Control, solers: Control) -> bool:
	var editor_files := get_editor_interface().get_resource_filesystem()
	if not _check(Image.create_empty(1, 1, false, Image.FORMAT_RGBA8).save_png(IMPORT_PATH) == OK, "Could not create the native import contract"):
		return false
	editor_files.update_file(IMPORT_PATH)
	editor_files.reimport_files(PackedStringArray([IMPORT_PATH]))
	if not _check(ResourceLoader.exists(IMPORT_PATH), "EditorFileSystem did not register the imported resource"):
		return false

	var filesystem_docks := base.find_children("*", "FileSystemDock", true, false)
	if not _check(not filesystem_docks.is_empty(), "The native FileSystemDock is unavailable"):
		return false
	filesystem_docks[0].navigate_to_path("res://addons")
	var input := solers.find_child("ComposerInput", true, false) as TextEdit
	var popup := solers.find_child("MentionPopup", true, false) as Control
	var list := solers.find_child("MentionList", true, false) as ItemList
	if not _check(input != null and popup != null and list != null, "The context picker is unavailable"):
		return false
	input.text = "@"
	input.set_caret_column(1)
	input.emit_signal("text_changed")
	await get_tree().process_frame
	var files_index := _find_item(list, "Files")
	if not _check(popup.visible and files_index >= 0, "The context picker did not expose project files"):
		return false
	list.emit_signal("item_clicked", files_index, Vector2.ZERO, MOUSE_BUTTON_LEFT)
	await get_tree().process_frame
	var found_root_import := false
	for index in list.item_count:
		var mention: Dictionary = list.get_item_metadata(index).get("mention", {})
		found_root_import = found_root_import or mention.get("path", "") == IMPORT_PATH
	if not _check(found_root_import, "The context picker followed FileSystemDock browsing state instead of the project index"):
		return false

	return true


func _enter_tree() -> void:
	print("SOLERS_LAYOUT_TEST_START")
	_check_layout.call_deferred()


func _check_layout() -> void:
	await get_tree().process_frame
	await get_tree().process_frame

	var base := get_editor_interface().get_base_control()
	var solers := base.find_child("SolersChat", true, false) as Control
	if not _check(solers != null, "Solers chat is not mounted"):
		return
	if not _check(solers.get_parent().name == "EditorSidePanel", "Solers chat is not in the fixed left host"):
		return

	var main_screen := base.find_child("MainScreen", true, false)
	if not _check(main_screen != null and not main_screen.is_ancestor_of(solers), "Solers still occupies EditorMainScreen"):
		return
	if not _check(not _contains_button_text(base, "Solers"), "Solers still exposes a main-screen button"):
		return
	get_editor_interface().set_main_screen_editor("Studio")
	await get_tree().process_frame
	await get_tree().process_frame
	var studio := base.find_child("SolersStudio", true, false) as Control
	var creation_scroll: ScrollContainer = studio.find_child("CreationScroll", true, false) if studio else null
	var rail: ItemList = studio.find_child("StudioRail", true, false) if studio else null
	var library_search: Control = studio.find_child("StudioLibrarySearch", true, false) if studio else null
	var generate: Button = studio.find_child("GenerateButton", true, false) if studio else null
	var composer := solers.find_child("ComposerInput", true, false) as TextEdit
	if not _check(studio != null and creation_scroll != null and rail != null and library_search != null and generate != null and composer != null, "The Studio layout contract is unavailable"):
		return
	if not _check(creation_scroll.vertical_scroll_mode == ScrollContainer.SCROLL_MODE_AUTO, "The creation form does not own vertical overflow"):
		return
	var viewport_bottom := base.get_global_rect().end.y
	if not _check(studio.get_combined_minimum_size().y <= studio.size.y, "Studio minimum height exceeds its native host"):
		return
	if not _check(generate.get_global_rect().end.y <= viewport_bottom and composer.get_global_rect().end.y <= viewport_bottom, "Studio pushed an editor composer below the visible window"):
		return

	var left_split := base.find_child("DockVSplitLeftL", true, false)
	if not _check(left_split != null and left_split.get_parent() is SplitContainer, "Left host is not resizable by the native split"):
		return

	var right_tabs := base.find_child("DockSlotRightUL", true, false) as TabContainer
	if not _check(right_tabs != null and right_tabs.get_tab_count() > 0, "No native docks remain open on the right"):
		return
	var dock_palette := right_tabs.get_popup() as PopupMenu
	if not _check(dock_palette != null and dock_palette.item_count > right_tabs.get_tab_count(), "The right-side add menu does not expose closed docks"):
		return
	if not _check(not dock_palette.get_parent() is PopupMenu, "The dock palette still reuses the Settings submenu"):
		return

	dock_palette.popup()
	await get_tree().process_frame
	if not _check(dock_palette.visible and dock_palette.size.y > 0, "The dock palette opens without visible content"):
		return
	dock_palette.hide()

	var secondary_tabs := base.find_child("DockSlotRightBL", true, false) as TabContainer
	if not _check(secondary_tabs != null and secondary_tabs.get_popup() is PopupPanel, "Native dock context popup was replaced outside the palette slot"):
		return
	if not await _check_editor_authority(base, solers):
		return

	print("SOLERS_LAYOUT_TEST_PASS")
	get_tree().quit()
