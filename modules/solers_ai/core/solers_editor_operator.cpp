/**************************************************************************/
/*  solers_editor_operator.cpp                                            */
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

#include "solers_editor_operator.h"

#include "core/config/project_settings.h"
#include "core/io/file_access.h"
#include "core/io/image.h"
#include "core/io/resource_loader.h"
#include "core/object/class_db.h"
#include "editor/editor_interface.h"
#include "editor/editor_node.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/run/editor_run.h"
#include "scene/gui/control.h"
#include "scene/main/node.h"
#include "scene/main/viewport.h"

void SolersEditorOperator::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_node_properties", "args"), &SolersEditorOperator::get_node_properties);
	ClassDB::bind_method(D_METHOD("add_node", "args"), &SolersEditorOperator::add_node);
	ClassDB::bind_method(D_METHOD("reparent_node", "args"), &SolersEditorOperator::reparent_node);
	ClassDB::bind_method(D_METHOD("set_node_properties", "args"), &SolersEditorOperator::set_node_properties);
	ClassDB::bind_method(D_METHOD("remove_node", "args"), &SolersEditorOperator::remove_node);
	ClassDB::bind_method(D_METHOD("attach_script", "args"), &SolersEditorOperator::attach_script);
	ClassDB::bind_method(D_METHOD("connect_signal", "args"), &SolersEditorOperator::connect_signal);
	ClassDB::bind_method(D_METHOD("list_signal_connections", "args"), &SolersEditorOperator::list_signal_connections);
	ClassDB::bind_method(D_METHOD("create_scene", "args"), &SolersEditorOperator::create_scene);
	ClassDB::bind_method(D_METHOD("open_scene", "args"), &SolersEditorOperator::open_scene);
	ClassDB::bind_method(D_METHOD("save_current_scene", "args"), &SolersEditorOperator::save_current_scene);
	ClassDB::bind_method(D_METHOD("save_scene_as", "args"), &SolersEditorOperator::save_scene_as);
	ClassDB::bind_method(D_METHOD("play_current_scene", "args"), &SolersEditorOperator::play_current_scene);
	ClassDB::bind_method(D_METHOD("stop_playing_scene", "args"), &SolersEditorOperator::stop_playing_scene);
	ClassDB::bind_method(D_METHOD("capture_editor_screenshot", "args"), &SolersEditorOperator::capture_editor_screenshot);
	ClassDB::bind_method(D_METHOD("rollback_last_editor_action", "args"), &SolersEditorOperator::rollback_last_editor_action);
}

Dictionary SolersEditorOperator::_ok(const Variant &p_data) const {
	Dictionary result;
	result["ok"] = true;
	result["data"] = p_data;
	return result;
}

Dictionary SolersEditorOperator::_error(const String &p_code, const String &p_message, bool p_recoverable) const {
	Dictionary error;
	error["code"] = p_code;
	error["message"] = p_message;
	error["recoverable"] = p_recoverable;

	Dictionary result;
	result["ok"] = false;
	result["error"] = error;
	return result;
}

Node *SolersEditorOperator::_resolve_node(const String &p_node_path, String &r_error) const {
	EditorInterface *editor_interface = EditorInterface::get_singleton();
	if (!editor_interface) {
		r_error = "EditorInterface is not available.";
		return nullptr;
	}

	Node *edited_root = editor_interface->get_edited_scene_root();
	if (!edited_root) {
		r_error = "No edited scene root.";
		return nullptr;
	}

	if (p_node_path.is_empty() || p_node_path == "." || p_node_path == String(edited_root->get_name())) {
		return edited_root;
	}

	Node *node = nullptr;
	const NodePath node_path = NodePath(p_node_path);
	if (p_node_path.begins_with("/")) {
		node = Object::cast_to<Node>(edited_root->get_node_or_null(edited_root->get_path().rel_path_to(node_path)));
	} else {
		node = edited_root->get_node_or_null(node_path);
	}

	if (!node) {
		r_error = vformat("Node not found: %s", p_node_path);
	}
	return node;
}

Dictionary SolersEditorOperator::get_node_properties(const Dictionary &p_args) {
	const String node_path = p_args.get("node_path", ".");
	const int max_properties = CLAMP((int)p_args.get("max_properties", 128), 0, 512);

	String error;
	Node *node = _resolve_node(node_path, error);
	if (!node) {
		return _error("NODE_NOT_FOUND", error);
	}

	List<PropertyInfo> property_list;
	node->get_property_list(&property_list);

	Array properties;
	int count = 0;
	for (const PropertyInfo &property : property_list) {
		if (count >= max_properties) {
			break;
		}
		if (!(property.usage & PROPERTY_USAGE_EDITOR) && !(property.usage & PROPERTY_USAGE_STORAGE)) {
			continue;
		}

		Dictionary item;
		item["name"] = property.name;
		item["type"] = Variant::get_type_name(property.type);
		item["hint"] = property.hint;
		item["hint_string"] = property.hint_string;
		item["usage"] = property.usage;
		const Variant value = node->get(property.name);
		if (value.get_type() != Variant::OBJECT) {
			item["value"] = value;
		} else {
			Object *object = value;
			item["value"] = object ? vformat("<%s:%s>", object->get_class(), itos(object->get_instance_id())) : "<null>";
		}
		properties.push_back(item);
		count++;
	}

	Dictionary data;
	data["node_path"] = String(node->get_path());
	data["properties"] = properties;
	data["count"] = properties.size();
	data["truncated"] = count >= max_properties;
	return _ok(data);
}

Dictionary SolersEditorOperator::add_node(const Dictionary &p_args) {
	const String parent_path = p_args.get("parent_path", ".");
	const String type = p_args.get("type", "Node");
	const String requested_name = p_args.get("name", String());

	String error;
	Node *parent = _resolve_node(parent_path, error);
	if (!parent) {
		return _error("PARENT_NODE_NOT_FOUND", error);
	}

	if (!ClassDB::class_exists(type) || !ClassDB::can_instantiate(type) || !ClassDB::is_parent_class(type, SNAME("Node"))) {
		return _error("INVALID_NODE_TYPE", vformat("Class is not an instantiable Node type: %s", type));
	}

	Object *object = ClassDB::instantiate(type);
	Node *node = Object::cast_to<Node>(object);
	if (!node) {
		if (object) {
			memdelete(object);
		}
		return _error("NODE_INSTANTIATION_FAILED", vformat("Failed to instantiate node type: %s", type), false);
	}

	if (!requested_name.is_empty()) {
		node->set_name(requested_name);
	}

	EditorInterface *editor_interface = EditorInterface::get_singleton();
	Node *edited_root = editor_interface->get_edited_scene_root();
	EditorUndoRedoManager *undo_redo = editor_interface->get_editor_undo_redo();
	ERR_FAIL_NULL_V(undo_redo, _error("UNDO_REDO_UNAVAILABLE", "EditorUndoRedoManager is not available.", false));

	undo_redo->create_action(vformat("Solers: Add %s", type), UndoRedo::MERGE_DISABLE, parent);
	undo_redo->add_do_method(parent, "add_child", node, true);
	undo_redo->add_do_method(node, "set_owner", edited_root);
	undo_redo->add_do_reference(node);
	undo_redo->add_undo_method(parent, "remove_child", node);
	undo_redo->commit_action();

	Dictionary data;
	data["type"] = type;
	data["name"] = node->get_name();
	data["path"] = String(node->get_path());
	data["relative_path"] = String(edited_root->get_path_to(node));
	return _ok(data);
}

Dictionary SolersEditorOperator::save_current_scene(const Dictionary &p_args) {
	EditorInterface *editor_interface = EditorInterface::get_singleton();
	ERR_FAIL_NULL_V(editor_interface, _error("EDITOR_INTERFACE_UNAVAILABLE", "EditorInterface is not available.", false));

	const Error err = editor_interface->save_scene();
	if (err != OK) {
		return _error("SAVE_SCENE_FAILED", vformat("EditorInterface::save_scene failed with error code %d.", err));
	}

	Dictionary data;
	Node *edited_root = editor_interface->get_edited_scene_root();
	data["scene_path"] = edited_root ? edited_root->get_scene_file_path() : String();
	return _ok(data);
}

Dictionary SolersEditorOperator::create_scene(const Dictionary &p_args) {
	const String root_type = p_args.get("root_type", "Node3D");
	const String root_name = p_args.get("root_name", "Main");
	const bool close_current_if_empty = p_args.get("close_current_if_empty", true);

	if (!ClassDB::class_exists(root_type) || !ClassDB::can_instantiate(root_type) || !ClassDB::is_parent_class(root_type, SNAME("Node"))) {
		return _error("INVALID_NODE_TYPE", vformat("Class is not an instantiable Node type: %s", root_type));
	}

	EditorInterface *editor_interface = EditorInterface::get_singleton();
	ERR_FAIL_NULL_V(editor_interface, _error("EDITOR_INTERFACE_UNAVAILABLE", "EditorInterface is not available.", false));

	if (editor_interface->get_edited_scene_root()) {
		if (!close_current_if_empty) {
			return _error("EDITED_SCENE_ALREADY_EXISTS", "The current editor tab already has a scene root. Open a new empty scene tab before creating another scene root.");
		}
		EditorNode::get_singleton()->new_scene();
		if (editor_interface->get_edited_scene_root()) {
			return _error("EDITED_SCENE_ALREADY_EXISTS", "Unable to switch to an empty scene tab before creating the scene.");
		}
	}

	Object *object = ClassDB::instantiate(root_type);
	Node *root = Object::cast_to<Node>(object);
	if (!root) {
		if (object) {
			memdelete(object);
		}
		return _error("NODE_INSTANTIATION_FAILED", vformat("Failed to instantiate root node type: %s", root_type), false);
	}

	if (!root_name.strip_edges().is_empty()) {
		root->set_name(root_name.strip_edges());
	}

	editor_interface->add_root_node(root);
	editor_interface->edit_node(root);

	Dictionary data;
	data["root_type"] = root_type;
	data["root_name"] = root->get_name();
	data["root_path"] = String(root->get_path());
	data["scene_path"] = root->get_scene_file_path();
	data["unsaved"] = true;
	return _ok(data);
}

Dictionary SolersEditorOperator::save_scene_as(const Dictionary &p_args) {
	const String path_arg = p_args.get("path", String());
	String scene_path = path_arg.strip_edges().replace_char('\\', '/');
	if (scene_path.is_empty()) {
		return _error("INVALID_ARGUMENT", "Scene path is empty.");
	}
	if (scene_path.is_absolute_path() && !scene_path.begins_with("res://")) {
		return _error("INVALID_PATH", "Only res:// or project-relative paths are allowed.");
	}
	if (!scene_path.begins_with("res://")) {
		scene_path = String("res://").path_join(scene_path);
	}
	scene_path = scene_path.simplify_path();
	if (!scene_path.begins_with("res://") || scene_path.contains("..")) {
		return _error("INVALID_PATH", "Scene path escapes the project root.");
	}
	if (!scene_path.has_extension("tscn") && !scene_path.has_extension("scn")) {
		return _error("INVALID_PATH", "Scene path must end in .tscn or .scn.");
	}

	EditorInterface *editor_interface = EditorInterface::get_singleton();
	ERR_FAIL_NULL_V(editor_interface, _error("EDITOR_INTERFACE_UNAVAILABLE", "EditorInterface is not available.", false));
	if (!editor_interface->get_edited_scene_root()) {
		return _error("NO_EDITED_SCENE", "No edited scene root is available.");
	}

	editor_interface->save_scene_as(scene_path, true);
	Dictionary data;
	data["scene_path"] = scene_path;
	return _ok(data);
}

Dictionary SolersEditorOperator::open_scene(const Dictionary &p_args) {
	const String path_arg = p_args.get("path", String());
	String scene_path = path_arg.strip_edges().replace_char('\\', '/');
	if (scene_path.is_empty()) {
		return _error("INVALID_ARGUMENT", "Scene path is empty.");
	}
	if (scene_path.is_absolute_path() && !scene_path.begins_with("res://")) {
		return _error("INVALID_PATH", "Only res:// or project-relative scene paths are allowed.");
	}
	if (!scene_path.begins_with("res://")) {
		scene_path = String("res://").path_join(scene_path);
	}
	scene_path = scene_path.simplify_path();
	if (!scene_path.begins_with("res://") || scene_path.contains("..")) {
		return _error("INVALID_PATH", "Scene path escapes the project root.");
	}
	if (!FileAccess::exists(scene_path)) {
		return _error("SCENE_NOT_FOUND", vformat("Scene file does not exist: %s", scene_path));
	}

	EditorInterface *editor_interface = EditorInterface::get_singleton();
	ERR_FAIL_NULL_V(editor_interface, _error("EDITOR_INTERFACE_UNAVAILABLE", "EditorInterface is not available.", false));
	editor_interface->open_scene_from_path(scene_path, p_args.get("set_inherited", false));

	Dictionary data;
	data["scene_path"] = scene_path;
	return _ok(data);
}

Dictionary SolersEditorOperator::play_current_scene(const Dictionary &p_args) {
	EditorInterface *editor_interface = EditorInterface::get_singleton();
	ERR_FAIL_NULL_V(editor_interface, _error("EDITOR_INTERFACE_UNAVAILABLE", "EditorInterface is not available.", false));

	editor_interface->play_current_scene();
	Dictionary data;
	data["is_playing"] = editor_interface->is_playing_scene();
	data["playing_scene"] = editor_interface->get_playing_scene();
	return _ok(data);
}

Dictionary SolersEditorOperator::stop_playing_scene(const Dictionary &p_args) {
	EditorInterface *editor_interface = EditorInterface::get_singleton();
	ERR_FAIL_NULL_V(editor_interface, _error("EDITOR_INTERFACE_UNAVAILABLE", "EditorInterface is not available.", false));

	editor_interface->stop_playing_scene();
	Dictionary data;
	data["is_playing"] = editor_interface->is_playing_scene();
	return _ok(data);
}

Dictionary SolersEditorOperator::capture_editor_screenshot(const Dictionary &p_args) {
	String path = p_args.get("path", String());
	if (path.is_empty()) {
		path = "user://solers_editor_screenshot.png";
	}
	path = path.strip_edges().replace_char('\\', '/');
	if (!path.begins_with("user://") && !path.begins_with("res://")) {
		return _error("INVALID_PATH", "Screenshot path must be user:// or res://.");
	}

	EditorInterface *editor_interface = EditorInterface::get_singleton();
	ERR_FAIL_NULL_V(editor_interface, _error("EDITOR_INTERFACE_UNAVAILABLE", "EditorInterface is not available.", false));
	Control *base_control = editor_interface->get_base_control();
	if (!base_control || !base_control->get_viewport()) {
		return _error("VIEWPORT_UNAVAILABLE", "Editor viewport is not available.");
	}

	Ref<ViewportTexture> texture = base_control->get_viewport()->get_texture();
	if (texture.is_null()) {
		return _error("VIEWPORT_TEXTURE_UNAVAILABLE", "Editor viewport texture is not available.");
	}
	Ref<Image> image = texture->get_image();
	if (image.is_null()) {
		return _error("IMAGE_CAPTURE_FAILED", "Unable to capture editor viewport image.");
	}
	const Error err = image->save_png(path);
	if (err != OK) {
		return _error("SCREENSHOT_SAVE_FAILED", vformat("Failed to save screenshot, error code %d.", err));
	}

	Dictionary data;
	data["path"] = path;
	data["global_path"] = ProjectSettings::get_singleton()->globalize_path(path);
	data["width"] = image->get_width();
	data["height"] = image->get_height();
	return _ok(data);
}

Dictionary SolersEditorOperator::rollback_last_editor_action(const Dictionary &p_args) {
	EditorInterface *editor_interface = EditorInterface::get_singleton();
	ERR_FAIL_NULL_V(editor_interface, _error("EDITOR_INTERFACE_UNAVAILABLE", "EditorInterface is not available.", false));
	EditorUndoRedoManager *undo_redo = editor_interface->get_editor_undo_redo();
	ERR_FAIL_NULL_V(undo_redo, _error("UNDO_REDO_UNAVAILABLE", "EditorUndoRedoManager is not available.", false));
	if (!undo_redo->has_undo()) {
		return _error("NO_UNDO_AVAILABLE", "EditorUndoRedoManager has no undo action available.");
	}

	const String action_name = undo_redo->get_current_action_name();
	const bool ok = undo_redo->undo();
	if (!ok) {
		return _error("UNDO_FAILED", "EditorUndoRedoManager::undo returned false.");
	}

	Dictionary data;
	data["rolled_back"] = true;
	data["action_name"] = action_name;
	return _ok(data);
}

Dictionary SolersEditorOperator::reparent_node(const Dictionary &p_args) {
	const String node_path = p_args.get("node_path", String());
	const String new_parent_path = p_args.get("new_parent_path", String());
	const int position = p_args.get("position", -1);
	if (node_path.is_empty() || node_path == ".") {
		return _error("INVALID_ARGUMENT", "Refusing to reparent the edited scene root.");
	}

	String error;
	Node *node = _resolve_node(node_path, error);
	if (!node) {
		return _error("NODE_NOT_FOUND", error);
	}
	Node *new_parent = _resolve_node(new_parent_path, error);
	if (!new_parent) {
		return _error("PARENT_NODE_NOT_FOUND", error);
	}
	Node *old_parent = node->get_parent();
	if (!old_parent) {
		return _error("INVALID_ARGUMENT", "Node has no parent and cannot be reparented.");
	}
	if (node == new_parent || node->is_ancestor_of(new_parent)) {
		return _error("INVALID_ARGUMENT", "Cannot reparent a node to itself or to one of its descendants.");
	}

	EditorInterface *editor_interface = EditorInterface::get_singleton();
	Node *edited_root = editor_interface->get_edited_scene_root();
	EditorUndoRedoManager *undo_redo = editor_interface->get_editor_undo_redo();
	ERR_FAIL_NULL_V(undo_redo, _error("UNDO_REDO_UNAVAILABLE", "EditorUndoRedoManager is not available.", false));

	const int old_index = node->get_index(false);
	const int new_index = position < 0 ? -1 : CLAMP(position, 0, new_parent->get_child_count(false));
	undo_redo->create_action("Solers: Reparent Node", UndoRedo::MERGE_DISABLE, node);
	undo_redo->add_do_method(old_parent, "remove_child", node);
	undo_redo->add_do_method(new_parent, "add_child", node, true);
	if (new_index >= 0) {
		undo_redo->add_do_method(new_parent, "move_child", node, new_index);
	}
	undo_redo->add_do_method(node, "set_owner", edited_root);
	undo_redo->add_undo_method(new_parent, "remove_child", node);
	undo_redo->add_undo_method(old_parent, "add_child", node, true);
	undo_redo->add_undo_method(old_parent, "move_child", node, old_index);
	undo_redo->add_undo_method(node, "set_owner", node->get_owner());
	undo_redo->commit_action();

	Dictionary data;
	data["node_path"] = String(node->get_path());
	data["new_parent_path"] = String(new_parent->get_path());
	data["old_parent_path"] = String(old_parent->get_path());
	data["old_index"] = old_index;
	data["new_index"] = new_index;
	return _ok(data);
}

Dictionary SolersEditorOperator::set_node_properties(const Dictionary &p_args) {
	const String node_path = p_args.get("node_path", ".");
	const Dictionary properties = p_args.get("properties", Dictionary());
	if (properties.is_empty()) {
		return _error("INVALID_ARGUMENT", "No properties were provided.");
	}

	String error;
	Node *node = _resolve_node(node_path, error);
	if (!node) {
		return _error("NODE_NOT_FOUND", error);
	}

	EditorInterface *editor_interface = EditorInterface::get_singleton();
	EditorUndoRedoManager *undo_redo = editor_interface->get_editor_undo_redo();
	ERR_FAIL_NULL_V(undo_redo, _error("UNDO_REDO_UNAVAILABLE", "EditorUndoRedoManager is not available.", false));

	undo_redo->create_action("Solers: Set Node Properties", UndoRedo::MERGE_DISABLE, node);
	Array changed_properties;
	const Variant *key = nullptr;
	while ((key = properties.next(key))) {
		const StringName property_name = StringName(*key);
		const Variant new_value = properties[*key];
		undo_redo->add_do_property(node, property_name, new_value);
		undo_redo->add_undo_property(node, property_name, node->get(property_name));
		changed_properties.push_back(String(property_name));
	}
	undo_redo->commit_action();

	Dictionary data;
	data["node_path"] = String(node->get_path());
	data["changed_properties"] = changed_properties;
	data["changed_count"] = changed_properties.size();
	return _ok(data);
}

Dictionary SolersEditorOperator::attach_script(const Dictionary &p_args) {
	const String node_path = p_args.get("node_path", ".");
	const String script_path = p_args.get("script_path", String());
	if (script_path.is_empty()) {
		return _error("INVALID_ARGUMENT", "script_path is required.");
	}

	String error;
	Node *node = _resolve_node(node_path, error);
	if (!node) {
		return _error("NODE_NOT_FOUND", error);
	}

	String normalized_script_path = script_path.strip_edges().replace_char('\\', '/');
	if (!normalized_script_path.begins_with("res://")) {
		normalized_script_path = String("res://").path_join(normalized_script_path);
	}
	normalized_script_path = normalized_script_path.simplify_path();
	if (!normalized_script_path.begins_with("res://") || normalized_script_path.contains("..")) {
		return _error("INVALID_PATH", "Script path escapes the project root.");
	}
	if (!FileAccess::exists(normalized_script_path)) {
		return _error("SCRIPT_NOT_FOUND", vformat("Script does not exist: %s", normalized_script_path));
	}

	Ref<Script> script = ResourceLoader::load(normalized_script_path, "Script");
	if (script.is_null()) {
		return _error("SCRIPT_LOAD_FAILED", vformat("Unable to load script: %s", normalized_script_path));
	}

	EditorInterface *editor_interface = EditorInterface::get_singleton();
	EditorUndoRedoManager *undo_redo = editor_interface->get_editor_undo_redo();
	ERR_FAIL_NULL_V(undo_redo, _error("UNDO_REDO_UNAVAILABLE", "EditorUndoRedoManager is not available.", false));

	Ref<Script> previous_script = node->get_script();
	undo_redo->create_action("Solers: Attach Script", UndoRedo::MERGE_DISABLE, node);
	undo_redo->add_do_method(node, "set_script", script);
	undo_redo->add_undo_method(node, "set_script", previous_script);
	undo_redo->commit_action();

	Dictionary data;
	data["node_path"] = String(node->get_path());
	data["script_path"] = normalized_script_path;
	data["had_previous_script"] = previous_script.is_valid();
	return _ok(data);
}

Dictionary SolersEditorOperator::connect_signal(const Dictionary &p_args) {
	const String source_path = p_args.get("source_path", ".");
	const String signal_name = p_args.get("signal", String());
	const String target_path = p_args.get("target_path", ".");
	const String method_name = p_args.get("method", String());
	if (signal_name.is_empty() || method_name.is_empty()) {
		return _error("INVALID_ARGUMENT", "signal and method are required.");
	}

	String error;
	Node *source = _resolve_node(source_path, error);
	if (!source) {
		return _error("SOURCE_NODE_NOT_FOUND", error);
	}
	Node *target = _resolve_node(target_path, error);
	if (!target) {
		return _error("TARGET_NODE_NOT_FOUND", error);
	}

	const int flags = p_args.get("flags", (int)Object::CONNECT_PERSIST);
	const Callable callable(target, StringName(method_name));
	if (source->is_connected(StringName(signal_name), callable)) {
		return _error("SIGNAL_ALREADY_CONNECTED", "The requested signal connection already exists.");
	}

	EditorInterface *editor_interface = EditorInterface::get_singleton();
	EditorUndoRedoManager *undo_redo = editor_interface->get_editor_undo_redo();
	ERR_FAIL_NULL_V(undo_redo, _error("UNDO_REDO_UNAVAILABLE", "EditorUndoRedoManager is not available.", false));

	undo_redo->create_action(vformat("Solers: Connect %s", signal_name), UndoRedo::MERGE_DISABLE, source);
	undo_redo->add_do_method(source, "connect", StringName(signal_name), callable, flags);
	undo_redo->add_undo_method(source, "disconnect", StringName(signal_name), callable);
	undo_redo->commit_action();

	Dictionary data;
	data["source_path"] = String(source->get_path());
	data["target_path"] = String(target->get_path());
	data["signal"] = signal_name;
	data["method"] = method_name;
	data["flags"] = flags;
	return _ok(data);
}

Dictionary SolersEditorOperator::list_signal_connections(const Dictionary &p_args) {
	const String source_path = p_args.get("source_path", ".");
	const String signal_name = p_args.get("signal", String());

	String error;
	Node *source = _resolve_node(source_path, error);
	if (!source) {
		return _error("SOURCE_NODE_NOT_FOUND", error);
	}

	Array connection_items;
	if (signal_name.is_empty()) {
		List<MethodInfo> signal_list;
		source->get_signal_list(&signal_list);
		for (const MethodInfo &signal : signal_list) {
			List<Object::Connection> connection_list;
			source->get_signal_connection_list(signal.name, &connection_list);
			for (const Object::Connection &connection : connection_list) {
				Dictionary item;
				item["signal"] = connection.signal.get_name();
				item["callable"] = String(connection.callable);
				item["flags"] = connection.flags;
				connection_items.push_back(item);
			}
		}
	} else {
		List<Object::Connection> connection_list;
		source->get_signal_connection_list(StringName(signal_name), &connection_list);
		for (const Object::Connection &connection : connection_list) {
			Dictionary item;
			item["signal"] = connection.signal.get_name();
			item["callable"] = String(connection.callable);
			item["flags"] = connection.flags;
			connection_items.push_back(item);
		}
	}

	Dictionary data;
	data["source_path"] = String(source->get_path());
	data["signal"] = signal_name;
	data["connections"] = connection_items;
	data["count"] = connection_items.size();
	return _ok(data);
}

Dictionary SolersEditorOperator::remove_node(const Dictionary &p_args) {
	const String node_path = p_args.get("node_path", String());
	if (node_path.is_empty() || node_path == ".") {
		return _error("INVALID_ARGUMENT", "Refusing to remove the edited scene root.");
	}

	String error;
	Node *node = _resolve_node(node_path, error);
	if (!node) {
		return _error("NODE_NOT_FOUND", error);
	}

	Node *parent = node->get_parent();
	if (!parent) {
		return _error("INVALID_ARGUMENT", "Node has no parent and cannot be removed safely.");
	}

	EditorInterface *editor_interface = EditorInterface::get_singleton();
	EditorUndoRedoManager *undo_redo = editor_interface->get_editor_undo_redo();
	ERR_FAIL_NULL_V(undo_redo, _error("UNDO_REDO_UNAVAILABLE", "EditorUndoRedoManager is not available.", false));

	const int original_index = node->get_index(false);
	undo_redo->create_action("Solers: Remove Node", UndoRedo::MERGE_DISABLE, parent);
	undo_redo->add_do_method(parent, "remove_child", node);
	undo_redo->add_undo_method(parent, "add_child", node, true);
	undo_redo->add_undo_method(parent, "move_child", node, original_index);
	undo_redo->add_undo_reference(node);
	undo_redo->commit_action();

	Dictionary data;
	data["removed_node"] = node_path;
	data["parent_path"] = String(parent->get_path());
	data["original_index"] = original_index;
	return _ok(data);
}
