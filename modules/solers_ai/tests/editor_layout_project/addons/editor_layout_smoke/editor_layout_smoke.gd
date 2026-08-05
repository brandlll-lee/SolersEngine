@tool
extends EditorPlugin


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


func _enter_tree() -> void:
	print("SOLERS_LAYOUT_TEST_START")
	_check_layout.call_deferred()


func _check_layout() -> void:
	await get_tree().process_frame
	await get_tree().process_frame

	var base := get_editor_interface().get_base_control()
	var solers := base.find_child("SolersChat", true, false)
	if not _check(solers != null, "Solers chat is not mounted"):
		return
	if not _check(solers.get_parent().name == "EditorSidePanel", "Solers chat is not in the fixed left host"):
		return

	var main_screen := base.find_child("MainScreen", true, false)
	if not _check(main_screen != null and not main_screen.is_ancestor_of(solers), "Solers still occupies EditorMainScreen"):
		return
	if not _check(not _contains_button_text(base, "Solers"), "Solers still exposes a main-screen button"):
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

	print("SOLERS_LAYOUT_TEST_PASS")
	get_tree().quit()
