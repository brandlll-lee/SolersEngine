/**************************************************************************/
/*  solers_observation_service.cpp                                        */
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

#include "solers_observation_service.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/object/class_db.h"
#include "core/version.h"
#include "editor/editor_data.h"
#include "editor/editor_interface.h"
#include "editor/editor_log.h"
#include "editor/editor_node.h"

void SolersObservationService::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_project_info"), &SolersObservationService::get_project_info);
	ClassDB::bind_method(D_METHOD("get_project_settings_summary"), &SolersObservationService::get_project_settings_summary);
	ClassDB::bind_method(D_METHOD("list_project_files", "max_files"), &SolersObservationService::list_project_files, DEFVAL(512));
	ClassDB::bind_method(D_METHOD("search_project_files", "query", "max_files"), &SolersObservationService::search_project_files, DEFVAL(128));
	ClassDB::bind_method(D_METHOD("read_project_file", "path", "max_bytes"), &SolersObservationService::read_project_file, DEFVAL(262144));
	ClassDB::bind_method(D_METHOD("get_open_scenes", "max_depth", "max_children_per_node"), &SolersObservationService::get_open_scenes, DEFVAL(1), DEFVAL(16));
	ClassDB::bind_method(D_METHOD("get_selection", "max_depth", "max_children_per_node"), &SolersObservationService::get_selection, DEFVAL(1), DEFVAL(16));
	ClassDB::bind_method(D_METHOD("get_scene_tree", "max_depth", "max_children_per_node"), &SolersObservationService::get_scene_tree, DEFVAL(8), DEFVAL(128));
	ClassDB::bind_method(D_METHOD("get_runtime_status"), &SolersObservationService::get_runtime_status);
	ClassDB::bind_method(D_METHOD("get_editor_logs", "max_messages"), &SolersObservationService::get_editor_logs, DEFVAL(200));
	ClassDB::bind_method(D_METHOD("get_editor_snapshot", "max_scene_depth", "max_children_per_node"), &SolersObservationService::get_editor_snapshot, DEFVAL(4), DEFVAL(64));
}

bool SolersObservationService::_normalize_project_path(const String &p_path, String &r_res_path, String &r_error) const {
	String path = p_path.strip_edges().replace_char('\\', '/');
	if (path.is_empty()) {
		r_error = "Path is empty.";
		return false;
	}

	if (path.is_absolute_path() && !path.begins_with("res://")) {
		r_error = "Only res:// or project-relative paths are allowed.";
		return false;
	}

	if (!path.begins_with("res://")) {
		path = String("res://").path_join(path);
	}

	path = path.simplify_path();
	if (!path.begins_with("res://") || path.contains("..")) {
		r_error = "Path escapes the project root.";
		return false;
	}

	r_res_path = path;
	return true;
}

void SolersObservationService::_collect_project_files(const String &p_dir, const String &p_query, int p_max_files, Array &r_files, int &r_scanned_count, bool &r_truncated) const {
	if (r_files.size() >= p_max_files) {
		r_truncated = true;
		return;
	}

	Error err = OK;
	Ref<DirAccess> dir = DirAccess::open(p_dir, &err);
	if (dir.is_null() || err != OK) {
		return;
	}

	dir->set_include_hidden(false);
	dir->list_dir_begin();
	String entry = dir->get_next();
	while (!entry.is_empty()) {
		const String path = p_dir.path_join(entry).replace_char('\\', '/');
		if (dir->current_is_dir()) {
			if (entry != ".godot" && entry != ".git" && entry != "__pycache__") {
				_collect_project_files(path, p_query, p_max_files, r_files, r_scanned_count, r_truncated);
			}
		} else {
			r_scanned_count++;
			if (p_query.is_empty() || path.findn(p_query) != -1) {
				r_files.push_back(path);
				if (r_files.size() >= p_max_files) {
					r_truncated = true;
					break;
				}
			}
		}
		entry = dir->get_next();
	}
	dir->list_dir_end();
}

Dictionary SolersObservationService::_serialize_node(Node *p_node, Node *p_edited_root, int p_depth, int p_max_depth, int p_max_children_per_node) const {
	Dictionary node_data;
	if (!p_node) {
		node_data["valid"] = false;
		return node_data;
	}

	node_data["valid"] = true;
	node_data["name"] = p_node->get_name();
	node_data["type"] = p_node->get_class();
	node_data["path"] = String(p_node->get_path());
	node_data["scene_file_path"] = p_node->get_scene_file_path();
	node_data["child_count"] = p_node->get_child_count();

	if (p_edited_root) {
		if (p_node == p_edited_root) {
			node_data["relative_path"] = ".";
		} else if (p_edited_root->is_ancestor_of(p_node)) {
			node_data["relative_path"] = String(p_edited_root->get_path_to(p_node));
		}
	}

	Node *owner = p_node->get_owner();
	if (owner) {
		node_data["owner_path"] = String(owner->get_path());
	}

	if (p_depth >= p_max_depth) {
		node_data["children_truncated_by_depth"] = p_node->get_child_count() > 0;
		return node_data;
	}

	Array children;
	const int child_count = p_node->get_child_count();
	const int max_children = MAX(0, p_max_children_per_node);
	const int child_limit = MIN(child_count, max_children);
	for (int i = 0; i < child_limit; i++) {
		children.push_back(_serialize_node(p_node->get_child(i), p_edited_root, p_depth + 1, p_max_depth, p_max_children_per_node));
	}
	node_data["children"] = children;
	if (child_count > child_limit) {
		node_data["children_truncated_count"] = child_count - child_limit;
	}

	return node_data;
}

Array SolersObservationService::_serialize_node_array(const TypedArray<Node> &p_nodes, Node *p_edited_root, int p_max_depth, int p_max_children_per_node) const {
	Array serialized;
	for (int i = 0; i < p_nodes.size(); i++) {
		Node *node = Object::cast_to<Node>(p_nodes[i]);
		if (node) {
			serialized.push_back(_serialize_node(node, p_edited_root, 0, p_max_depth, p_max_children_per_node));
		}
	}
	return serialized;
}

Dictionary SolersObservationService::get_project_info() const {
	Dictionary info;
	ProjectSettings *project_settings = ProjectSettings::get_singleton();
	ERR_FAIL_NULL_V(project_settings, info);

	const String project_name = GLOBAL_GET("application/config/name");
	const String main_scene = GLOBAL_GET("application/run/main_scene");

	info["name"] = project_name;
	info["resource_path"] = project_settings->get_resource_path();
	info["main_scene"] = main_scene;
	info["godot_version"] = VERSION_FULL_NAME;
	info["engine_distribution"] = "Solers Engine";
	info["project_settings_path"] = project_settings->get_resource_path().path_join("project.godot");
	return info;
}

Dictionary SolersObservationService::get_project_settings_summary() const {
	Dictionary summary;
	ProjectSettings *project_settings = ProjectSettings::get_singleton();
	ERR_FAIL_NULL_V(project_settings, summary);

	summary["application/config/name"] = GLOBAL_GET("application/config/name");
	summary["application/run/main_scene"] = GLOBAL_GET("application/run/main_scene");
	summary["rendering/renderer/rendering_method"] = GLOBAL_GET("rendering/renderer/rendering_method");
	summary["display/window/size/viewport_width"] = GLOBAL_GET("display/window/size/viewport_width");
	summary["display/window/size/viewport_height"] = GLOBAL_GET("display/window/size/viewport_height");
	summary["project_settings_path"] = project_settings->get_resource_path().path_join("project.godot");
	return summary;
}

Dictionary SolersObservationService::list_project_files(int p_max_files) const {
	Dictionary result;
	Array files;
	int scanned_count = 0;
	bool truncated = false;
	_collect_project_files("res://", String(), CLAMP(p_max_files, 0, 10000), files, scanned_count, truncated);
	result["files"] = files;
	result["count"] = files.size();
	result["scanned_count"] = scanned_count;
	result["truncated"] = truncated;
	return result;
}

Dictionary SolersObservationService::search_project_files(const String &p_query, int p_max_files) const {
	Dictionary result;
	const String query = p_query.strip_edges();
	if (query.is_empty()) {
		result["ok"] = false;
		result["error"] = "Query is empty.";
		return result;
	}

	Array files;
	int scanned_count = 0;
	bool truncated = false;
	_collect_project_files("res://", query, CLAMP(p_max_files, 0, 10000), files, scanned_count, truncated);
	result["ok"] = true;
	result["query"] = query;
	result["files"] = files;
	result["count"] = files.size();
	result["scanned_count"] = scanned_count;
	result["truncated"] = truncated;
	return result;
}

Dictionary SolersObservationService::read_project_file(const String &p_path, int p_max_bytes) const {
	Dictionary result;
	String res_path;
	String path_error;
	if (!_normalize_project_path(p_path, res_path, path_error)) {
		result["ok"] = false;
		result["error"] = path_error;
		return result;
	}

	if (!FileAccess::exists(res_path)) {
		result["ok"] = false;
		result["error"] = "File does not exist.";
		result["path"] = res_path;
		return result;
	}

	Error err = OK;
	Ref<FileAccess> file = FileAccess::open(res_path, FileAccess::READ, &err);
	if (file.is_null() || err != OK) {
		result["ok"] = false;
		result["error"] = "Unable to open file for reading.";
		result["path"] = res_path;
		return result;
	}

	const int64_t length = file->get_length();
	const int max_bytes = CLAMP(p_max_bytes, 0, 1048576);
	result["ok"] = true;
	result["path"] = res_path;
	result["size_bytes"] = length;
	result["truncated"] = length > max_bytes;
	Vector<uint8_t> bytes = file->get_buffer(MIN((int64_t)max_bytes, length));
	result["content"] = bytes.is_empty() ? String() : String::utf8((const char *)bytes.ptr(), bytes.size());
	return result;
}

Dictionary SolersObservationService::get_open_scenes(int p_max_depth, int p_max_children_per_node) const {
	Dictionary result;
	EditorInterface *editor_interface = EditorInterface::get_singleton();
	ERR_FAIL_NULL_V(editor_interface, result);

	PackedStringArray paths = editor_interface->get_open_scenes();
	TypedArray<Node> roots = editor_interface->get_open_scene_roots();
	Node *edited_root = editor_interface->get_edited_scene_root();

	Array path_array;
	for (int i = 0; i < paths.size(); i++) {
		path_array.push_back(paths[i]);
	}

	result["count"] = roots.size();
	result["paths"] = path_array;
	result["roots"] = _serialize_node_array(roots, edited_root, p_max_depth, p_max_children_per_node);
	result["current_scene_path"] = edited_root ? edited_root->get_scene_file_path() : String();
	return result;
}

Dictionary SolersObservationService::get_selection(int p_max_depth, int p_max_children_per_node) const {
	Dictionary result;
	EditorInterface *editor_interface = EditorInterface::get_singleton();
	ERR_FAIL_NULL_V(editor_interface, result);

	EditorSelection *selection = editor_interface->get_selection();
	if (!selection) {
		result["count"] = 0;
		result["nodes"] = Array();
		return result;
	}

	TypedArray<Node> selected_nodes = selection->get_selected_nodes();
	Node *edited_root = editor_interface->get_edited_scene_root();
	result["count"] = selected_nodes.size();
	result["nodes"] = _serialize_node_array(selected_nodes, edited_root, p_max_depth, p_max_children_per_node);
	return result;
}

Dictionary SolersObservationService::get_scene_tree(int p_max_depth, int p_max_children_per_node) const {
	Dictionary result;
	EditorInterface *editor_interface = EditorInterface::get_singleton();
	ERR_FAIL_NULL_V(editor_interface, result);

	Node *edited_root = editor_interface->get_edited_scene_root();
	result["has_edited_scene"] = edited_root != nullptr;
	if (!edited_root) {
		result["root"] = Dictionary();
		return result;
	}

	result["root"] = _serialize_node(edited_root, edited_root, 0, p_max_depth, p_max_children_per_node);
	return result;
}

Dictionary SolersObservationService::get_runtime_status() const {
	Dictionary result;
	EditorInterface *editor_interface = EditorInterface::get_singleton();
	ERR_FAIL_NULL_V(editor_interface, result);

	const bool playing = editor_interface->is_playing_scene();
	result["is_playing"] = playing;
	result["playing_scene"] = playing ? editor_interface->get_playing_scene() : String();
	return result;
}

Dictionary SolersObservationService::get_editor_logs(int p_max_messages) const {
	Dictionary result;
	EditorLog *log = EditorNode::get_log();
	if (!log) {
		result["available"] = false;
		result["messages"] = Array();
		result["counts"] = Dictionary();
		return result;
	}

	result["available"] = true;
	result["messages"] = log->get_messages(p_max_messages);
	result["counts"] = log->get_message_counts();
	return result;
}

Dictionary SolersObservationService::get_editor_snapshot(int p_max_scene_depth, int p_max_children_per_node) const {
	Dictionary snapshot;
	snapshot["project"] = get_project_info();
	snapshot["project_settings"] = get_project_settings_summary();
	snapshot["open_scenes"] = get_open_scenes(1, p_max_children_per_node);
	snapshot["scene_tree"] = get_scene_tree(p_max_scene_depth, p_max_children_per_node);
	snapshot["selection"] = get_selection(1, p_max_children_per_node);
	snapshot["runtime"] = get_runtime_status();
	snapshot["editor_log"] = get_editor_logs(40);
	return snapshot;
}
