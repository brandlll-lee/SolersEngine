/**************************************************************************/
/*  solers_project_observation.cpp                                        */
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
#include "modules/solers_ai/core/solers_project_observation.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/debugger/debugger_marshalls.h"
#include "core/input/input_map.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/image.h"
#include "core/io/json.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_uid.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/templates/hash_set.h"
#include "core/templates/local_vector.h"
#include "core/version.h"
#include "editor/debugger/editor_debugger_node.h"
#include "editor/debugger/script_editor_debugger.h"
#include "editor/editor_data.h"
#include "editor/editor_interface.h"
#include "editor/editor_main_screen.h"
#include "editor/editor_node.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/file_system/editor_file_system.h"
#include "editor/run/editor_run_bar.h"
#include "editor/run/game_view_plugin.h"
#include "editor/scene/3d/node_3d_editor_plugin.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/light_3d.h"
#include "scene/3d/node_3d.h"
#include "scene/3d/visual_instance_3d.h"
#include "scene/debugger/scene_debugger_object.h"
#include "scene/main/scene_tree.h"
#include "scene/main/viewport.h"
#include "scene/resources/3d/world_3d.h"
#include "scene/resources/camera_attributes.h"
#include "scene/resources/environment.h"
#include "scene/resources/packed_scene.h"
#include "scene/resources/sky.h"
#include "servers/rendering/rendering_server.h"

#include "modules/solers_ai/core/solers_context_manager.h"
#include "modules/solers_ai/core/solers_geometry_facts.h"
#include "modules/solers_ai/core/solers_path_utils.h"
#include "modules/solers_ai/core/solers_resource_service.h"
#include "modules/solers_ai/core/solers_tool.h"
#include "modules/solers_ai/core/solers_trace.h"

void SolersProjectObservation::_refresh_project_files() {
	EditorFileSystem *filesystem = EditorFileSystem::get_singleton();
	if (!filesystem || filesystem->is_scanning() || filesystem->is_importing() || !filesystem->get_filesystem()) {
		return;
	}
	PackedStringArray snapshot;
	LocalVector<EditorFileSystemDirectory *> stack;
	stack.push_back(filesystem->get_filesystem());
	while (!stack.is_empty()) {
		EditorFileSystemDirectory *directory = stack[stack.size() - 1];
		stack.remove_at(stack.size() - 1);
		for (int i = 0; i < directory->get_file_count(); i++) {
			snapshot.push_back(directory->get_file_path(i));
		}
		for (int i = 0; i < directory->get_subdir_count(); i++) {
			stack.push_back(directory->get_subdir(i));
		}
	}
	MutexLock lock(project_files_mutex);
	project_files = snapshot;
	project_files_ready = true;
}

Dictionary SolersProjectObservation::get_project_info() const {
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
	PackedStringArray files;
	bool files_ready = false;
	{
		MutexLock lock(project_files_mutex);
		files = project_files;
		files_ready = project_files_ready;
	}
	info["files_ready"] = files_ready;
	info["file_count"] = files.size();
	Array inventory;
	const int inventory_budget = MAX(1, SolersContextManager::TOOL_RESULT_MAX_TOKENS / 8);
	for (int i = 0; i < files.size(); i++) {
		Array candidate = inventory;
		candidate.push_back(files[i]);
		if (SolersContextManager::estimate_tokens(JSON::stringify(candidate, "", false, true)) > inventory_budget) {
			break;
		}
		inventory = candidate;
	}
	info["files"] = inventory;
	info["files_truncated"] = inventory.size() < files.size();
	return info;
}

Dictionary SolersProjectObservation::get_project_settings_summary() const {
	Dictionary summary;
	ProjectSettings *project_settings = ProjectSettings::get_singleton();
	ERR_FAIL_NULL_V(project_settings, summary);

	summary["application/config/name"] = GLOBAL_GET("application/config/name");
	summary["application/run/main_scene"] = GLOBAL_GET("application/run/main_scene");
	summary["rendering/renderer/rendering_method"] = GLOBAL_GET("rendering/renderer/rendering_method");
	summary["rendering/lights_and_shadows/use_physical_light_units"] = GLOBAL_GET("rendering/lights_and_shadows/use_physical_light_units");
	summary["display/window/size/viewport_width"] = GLOBAL_GET("display/window/size/viewport_width");
	summary["display/window/size/viewport_height"] = GLOBAL_GET("display/window/size/viewport_height");
	summary["project_settings_path"] = project_settings->get_resource_path().path_join("project.godot");
	return summary;
}

Dictionary SolersProjectObservation::inspect_project_delivery(const Dictionary &p_args, int p_token_budget) const {
	ProjectSettings *settings = ProjectSettings::get_singleton();
	ERR_FAIL_NULL_V(settings, Dictionary());
	Array blockers;
	Array roots = p_args.get("roots", Array());
	const String main_scene = GLOBAL_GET("application/run/main_scene");
	if (!main_scene.is_empty()) {
		roots.push_back(main_scene);
	}
	List<PropertyInfo> setting_properties;
	settings->get_property_list(&setting_properties);
	for (const PropertyInfo &property : setting_properties) {
		const String name = property.name;
		if (name.begins_with("autoload/")) {
			const String path = String(settings->get_setting(name)).trim_prefix("*");
			if (!path.is_empty()) {
				roots.push_back(path);
			}
		}
	}

	Array normalized_roots;
	Vector<String> queue;
	HashSet<String> reachable;
	for (int i = 0; i < roots.size(); i++) {
		const SolersPath::NormalizedPath normalized_path = SolersPath::normalize_project_path(roots[i]);
		if (!normalized_path.valid) {
			blockers.push_back(Dictionary({ { "code", "INVALID_ROOT" }, { "path", roots[i] }, { "message", normalized_path.error } }));
			continue;
		}
		const String path = normalized_path.value;
		if (!reachable.has(path)) {
			reachable.insert(path);
			queue.push_back(path);
			normalized_roots.push_back(path);
		}
	}
	for (int queue_index = 0; queue_index < queue.size(); queue_index++) {
		const String path = queue[queue_index];
		if (!FileAccess::exists(path)) {
			blockers.push_back(Dictionary({ { "code", "MISSING_DEPENDENCY" }, { "path", path } }));
			continue;
		}
		List<String> dependencies;
		ResourceLoader::get_dependencies(path, &dependencies, true);
		for (const String &raw_dependency : dependencies) {
			const String dependency = ResourceUID::ensure_path(raw_dependency.get_slice("::", 0));
			if (dependency.begins_with("res://") && !reachable.has(dependency)) {
				reachable.insert(dependency);
				queue.push_back(dependency);
			}
		}
	}

	PackedStringArray files;
	{
		MutexLock lock(project_files_mutex);
		files = project_files;
	}
	EditorFileSystem *filesystem = EditorFileSystem::get_singleton();
	if (!project_files_ready || !filesystem || filesystem->is_scanning() || filesystem->is_importing()) {
		blockers.push_back(Dictionary({ { "code", "PROJECT_INDEX_UNAVAILABLE" }, { "message", "EditorFileSystem has not committed a stable project index." } }));
	}
	Dictionary hashes;
	Array unreferenced;
	int report_tokens = 0;
	bool truncated = false;
	for (int i = 0; i < files.size(); i++) {
		const String path = files[i];
		int file_index = -1;
		EditorFileSystemDirectory *directory = filesystem ? filesystem->find_file(path, &file_index) : nullptr;
		if (directory && !directory->get_file_import_is_valid(file_index)) {
			blockers.push_back(Dictionary({ { "code", "INVALID_IMPORT" }, { "path", path } }));
		}
		if (FileAccess::exists(path)) {
			const String sha = FileAccess::get_sha256(path);
			Array group = hashes.get(sha, Array());
			group.push_back(path);
			hashes[sha] = group;
		}
		if (!reachable.has(path)) {
			truncated |= !SolersContextManager::append_bounded(unreferenced, Dictionary({ { "path", path } }), INT32_MAX, p_token_budget, report_tokens);
		}
	}
	Array duplicate_groups;
	for (const Variant *sha = hashes.next(nullptr); sha; sha = hashes.next(sha)) {
		const Array paths = hashes[*sha];
		if (paths.size() > 1) {
			truncated |= !SolersContextManager::append_bounded(duplicate_groups, Dictionary({ { "sha256", *sha }, { "paths", paths } }), INT32_MAX, p_token_budget, report_tokens);
		}
	}

	Array input_actions;
	if (InputMap *input_map = InputMap::get_singleton()) {
		for (const StringName action : input_map->get_actions()) {
			const List<Ref<InputEvent>> *events = input_map->action_get_events(action);
			input_actions.push_back(Dictionary({ { "name", action }, { "event_count", events ? events->size() : 0 } }));
		}
	}
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	EditorNode *editor = EditorNode::get_singleton();
	Node *edited_root = editor ? editor->get_edited_scene() : nullptr;
	if (edited_root && undo_redo) {
		const int history_id = EditorNode::get_editor_data().get_current_edited_scene_history_id();
		if (history_id != EditorUndoRedoManager::INVALID_HISTORY && undo_redo->is_history_unsaved(history_id)) {
			blockers.push_back(Dictionary({ { "code", "UNSAVED_SCENE" }, { "path", edited_root->get_scene_file_path() } }));
		}
	}

	return Dictionary({ { "status", "complete" }, { "roots", normalized_roots }, { "reachable_count", reachable.size() }, { "project_file_count", files.size() }, { "input_actions", input_actions }, { "unreferenced_from_roots", unreferenced }, { "duplicate_content", duplicate_groups }, { "blockers", blockers }, { "truncated", truncated } });
}

static bool _solers_text_search_extension(const String &p_extension) {
	return p_extension == "gd" || p_extension == "cs" || p_extension == "shader" || p_extension == "gdshader" ||
			p_extension == "tscn" || p_extension == "tres" || p_extension == "cfg" || p_extension == "json" ||
			p_extension == "xml" || p_extension == "md" || p_extension == "txt" || p_extension == "cpp" ||
			p_extension == "h" || p_extension == "py";
}

static bool _solers_identifier_character(char32_t p_character) {
	return p_character == '_' || (p_character >= '0' && p_character <= '9') || (p_character >= 'A' && p_character <= 'Z') ||
			(p_character >= 'a' && p_character <= 'z') || p_character >= 128;
}

static int _solers_find_text(const String &p_line, const String &p_query, bool p_symbol) {
	const String line = p_line.to_lower();
	const String query = p_query.to_lower();
	int from = 0;
	while (from <= line.length() - query.length()) {
		const int position = line.find(query, from);
		if (position < 0 || !p_symbol) {
			return position;
		}
		const bool left_boundary = position == 0 || !_solers_identifier_character(line[position - 1]);
		const int end = position + query.length();
		const bool right_boundary = end == line.length() || !_solers_identifier_character(line[end]);
		if (left_boundary && right_boundary) {
			return position;
		}
		from = position + 1;
	}
	return -1;
}

static Dictionary _solers_project_result(const String &p_path) {
	Dictionary result;
	result["path"] = p_path;
	return result;
}

static bool _solers_is_packed_scene_type(const String &p_resource_type) {
	return p_resource_type == "PackedScene";
}

Dictionary SolersProjectObservation::observe_path(const String &p_path) const {
	Dictionary result;
	const SolersPath::NormalizedPath normalized_path = SolersPath::normalize_project_path(p_path);
	if (!normalized_path.valid) {
		result["ok"] = false;
		result["error"] = normalized_path.error;
		return result;
	}
	String res_path = normalized_path.value;
	if (res_path != "res://" && res_path.ends_with("/")) {
		res_path = res_path.substr(0, res_path.length() - 1);
	}

	EditorFileSystem *efs = EditorFileSystem::get_singleton();
	EditorFileSystemDirectory *efs_dir = (efs && !efs->is_scanning()) ? efs->get_filesystem_path(res_path) : nullptr;
	const bool is_directory = efs_dir != nullptr || DirAccess::open(res_path).is_valid();
	if (is_directory) {
		const int max_children = 48;
		Array children;
		Dictionary type_counts;
		int file_count = 0;
		int subdir_count = 0;
		bool truncated = false;

		if (efs_dir) {
			subdir_count = efs_dir->get_subdir_count();
			file_count = efs_dir->get_file_count();
			for (int i = 0; i < efs_dir->get_subdir_count(); i++) {
				EditorFileSystemDirectory *sub = efs_dir->get_subdir(i);
				if (!sub) {
					continue;
				}
				type_counts["directory"] = (int)type_counts.get("directory", 0) + 1;
				if (children.size() >= max_children) {
					truncated = true;
					continue;
				}
				Dictionary child;
				child["path"] = sub->get_path();
				child["kind"] = "directory";
				children.push_back(child);
			}
			for (int i = 0; i < efs_dir->get_file_count(); i++) {
				const String child_type = String(efs_dir->get_file_type(i));
				const String count_key = child_type.is_empty() ? String("file") : child_type;
				type_counts[count_key] = (int)type_counts.get(count_key, 0) + 1;
				if (children.size() >= max_children) {
					truncated = true;
					continue;
				}
				Dictionary child;
				const String child_path = efs_dir->get_file_path(i);
				child["path"] = child_path;
				child["kind"] = count_key;
				child["import_valid"] = efs_dir->get_file_import_is_valid(i);
				const ResourceUID::ID child_uid = ResourceLoader::get_resource_uid(child_path);
				if (child_uid != ResourceUID::INVALID_ID) {
					child["uid"] = ResourceUID::get_singleton()->id_to_text(child_uid);
				}
				child["dependency_count"] = efs_dir->get_file_deps(i).size();
				children.push_back(child);
			}
		} else {
			Ref<DirAccess> dir = DirAccess::open(res_path);
			if (dir.is_valid()) {
				dir->list_dir_begin();
				for (String name = dir->get_next(); !name.is_empty(); name = dir->get_next()) {
					if (name == "." || name == "..") {
						continue;
					}
					const String child_path = res_path.path_join(name);
					if (dir->current_is_dir()) {
						subdir_count++;
						if (children.size() < max_children) {
							Dictionary child;
							child["path"] = child_path;
							child["kind"] = "directory";
							children.push_back(child);
						} else {
							truncated = true;
						}
						type_counts["directory"] = (int)type_counts.get("directory", 0) + 1;
					} else {
						file_count++;
						const String child_type = ResourceLoader::get_resource_type(child_path);
						const String kind = child_type.is_empty() ? String("file") : child_type;
						if (children.size() < max_children) {
							Dictionary child;
							child["path"] = child_path;
							child["kind"] = kind;
							children.push_back(child);
						} else {
							truncated = true;
						}
						type_counts[kind] = (int)type_counts.get(kind, 0) + 1;
					}
				}
			}
		}

		Dictionary digest;
		digest["kind"] = "directory";
		digest["path"] = res_path;
		digest["file_count"] = file_count;
		digest["subdir_count"] = subdir_count;
		digest["children"] = children;
		digest["type_counts"] = type_counts;
		digest["truncated"] = truncated || (file_count + subdir_count) > children.size();
		digest["summary"] = vformat("Directory with %d files and %d subdirectories.", file_count, subdir_count);
		result["ok"] = true;
		result["path"] = res_path;
		result["digest"] = digest;
		return result;
	}

	if (!FileAccess::exists(res_path)) {
		result["ok"] = false;
		result["error"] = vformat("Path does not exist.%s", solers_file_suggestions(res_path));
		result["path"] = res_path;
		return result;
	}

	const String resource_type = ResourceLoader::get_resource_type(res_path);
	if (_solers_is_packed_scene_type(resource_type)) {
		return digest_packed_scene(res_path, 96);
	}

	Dictionary digest;
	digest["kind"] = resource_type.is_empty() ? String("file") : resource_type;
	digest["path"] = res_path;
	if (!resource_type.is_empty()) {
		digest["resource_type"] = resource_type;
	}
	const ResourceUID::ID uid = ResourceLoader::get_resource_uid(res_path);
	if (uid != ResourceUID::INVALID_ID) {
		digest["uid"] = ResourceUID::get_singleton()->id_to_text(uid);
	}
	digest["size_bytes"] = FileAccess::get_size(res_path);

	Array dependencies;
	List<String> dep_list;
	ResourceLoader::get_dependencies(res_path, &dep_list, true);
	const int max_deps = 32;
	int dep_i = 0;
	for (const String &raw_dependency : dep_list) {
		if (dep_i >= max_deps) {
			break;
		}
		dependencies.push_back(ResourceUID::ensure_path(raw_dependency.get_slice("::", 0)));
		dep_i++;
	}
	digest["dependencies"] = dependencies;
	digest["dependencies_truncated"] = dep_list.size() > dependencies.size();
	digest["summary"] = resource_type.is_empty()
			? vformat("File (%d bytes).", (int64_t)digest.get("size_bytes", 0))
			: vformat("%s resource (%d bytes, %d deps).", resource_type, (int64_t)digest.get("size_bytes", 0), dependencies.size());

	result["ok"] = true;
	result["path"] = res_path;
	result["digest"] = digest;
	return result;
}

Dictionary SolersProjectObservation::digest_packed_scene(const String &p_path, int p_max_nodes) const {
	Dictionary result;
	const SolersPath::NormalizedPath normalized_path = SolersPath::normalize_project_path(p_path);
	if (!normalized_path.valid) {
		result["ok"] = false;
		result["error"] = normalized_path.error;
		return result;
	}
	const String res_path = normalized_path.value;
	const String resource_type = ResourceLoader::get_resource_type(res_path);
	if (!_solers_is_packed_scene_type(resource_type)) {
		result["ok"] = false;
		result["error"] = vformat("Not a PackedScene (resource_type=%s).", resource_type.is_empty() ? String("unknown") : resource_type);
		result["path"] = res_path;
		return result;
	}

	const Ref<PackedScene> scene = ResourceLoader::load(res_path, "PackedScene", ResourceFormatLoader::CACHE_MODE_REUSE);
	if (scene.is_null()) {
		result["ok"] = false;
		result["error"] = "Unable to load PackedScene.";
		result["path"] = res_path;
		return result;
	}
	const Ref<SceneState> state = scene->get_state();
	if (state.is_null()) {
		result["ok"] = false;
		result["error"] = "PackedScene has no SceneState.";
		result["path"] = res_path;
		return result;
	}

	const int max_nodes = CLAMP(p_max_nodes, 1, 256);
	const int node_count = state->get_node_count();
	Array nodes;
	Array scripts;
	Array external_scenes;
	HashSet<String> seen_scripts;
	HashSet<String> seen_scenes;

	for (int i = 0; i < node_count; i++) {
		const String node_path = String(state->get_node_path(i));
		const String name = String(state->get_node_name(i));
		const String node_type = String(state->get_node_type(i));
		if (nodes.size() < max_nodes) {
			Dictionary node;
			node["path"] = node_path;
			node["name"] = name;
			node["type"] = node_type;
			if (state->is_node_instance_placeholder(i)) {
				const String placeholder = state->get_node_instance_placeholder(i);
				node["instance"] = placeholder;
				if (!placeholder.is_empty() && !seen_scenes.has(placeholder)) {
					seen_scenes.insert(placeholder);
					external_scenes.push_back(placeholder);
				}
			} else {
				const Ref<PackedScene> inst = state->get_node_instance(i);
				if (inst.is_valid()) {
					const String inst_path = inst->get_path();
					if (!inst_path.is_empty()) {
						node["instance"] = inst_path;
						if (!seen_scenes.has(inst_path)) {
							seen_scenes.insert(inst_path);
							external_scenes.push_back(inst_path);
						}
					}
				}
			}
			nodes.push_back(node);
		}
		for (int p = 0; p < state->get_node_property_count(i); p++) {
			if (state->get_node_property_name(i, p) != StringName("script")) {
				continue;
			}
			const Variant value = state->get_node_property_value(i, p);
			String script_path;
			if (value.get_type() == Variant::OBJECT) {
				const Ref<Resource> res = value;
				if (res.is_valid()) {
					script_path = res->get_path();
				}
			} else if (value.get_type() == Variant::STRING) {
				script_path = String(value);
			}
			if (!script_path.is_empty() && !seen_scripts.has(script_path)) {
				seen_scripts.insert(script_path);
				scripts.push_back(script_path);
			}
		}
	}

	Array dependencies;
	List<String> dep_list;
	ResourceLoader::get_dependencies(res_path, &dep_list, true);
	const int max_deps = 32;
	int dep_i = 0;
	for (const String &raw_dependency : dep_list) {
		if (dep_i >= max_deps) {
			break;
		}
		dependencies.push_back(ResourceUID::ensure_path(raw_dependency.get_slice("::", 0)));
		dep_i++;
	}

	Dictionary digest;
	digest["kind"] = "PackedScene";
	digest["path"] = res_path;
	digest["resource_type"] = resource_type;
	const ResourceUID::ID uid = ResourceLoader::get_resource_uid(res_path);
	if (uid != ResourceUID::INVALID_ID) {
		digest["uid"] = ResourceUID::get_singleton()->id_to_text(uid);
	}
	if (node_count > 0) {
		digest["root_name"] = String(state->get_node_name(0));
		digest["root_type"] = String(state->get_node_type(0));
	}
	digest["node_count"] = node_count;
	digest["nodes_reported"] = nodes.size();
	digest["nodes_truncated"] = node_count > nodes.size();
	digest["nodes"] = nodes;
	digest["scripts"] = scripts;
	digest["external_scenes"] = external_scenes;
	digest["dependencies"] = dependencies;
	digest["dependencies_truncated"] = dep_list.size() > dependencies.size();
	digest["summary"] = vformat("PackedScene root=%s (%s), %d nodes, %d scripts, %d external scenes.",
			String(digest.get("root_name", String())), String(digest.get("root_type", String())), node_count, scripts.size(), external_scenes.size());

	result["ok"] = true;
	result["path"] = res_path;
	result["digest"] = digest;
	return result;
}

Dictionary SolersProjectObservation::search_project(const Dictionary &p_args, int p_token_budget) const {
	const String type = String(p_args.get("type", "path")).to_lower();
	const String query = String(p_args.get("query", String())).strip_edges();
	const int max_results = p_args.has("max_results") ? MAX(1, (int)p_args["max_results"]) : INT32_MAX;
	const int cursor = MAX(0, (int)p_args.get("cursor", 0));
	Array results;
	int result_tokens = 0;
	int scanned = 0;
	bool truncated = false;
	bool available = true;

	if (type == "path") {
		PackedStringArray files;
		{
			MutexLock lock(project_files_mutex);
			files = project_files;
			available = project_files_ready;
		}
		int match_index = 0;
		for (int i = 0; i < files.size(); i++) {
			scanned++;
			if (String(files[i]).findn(query) < 0 || match_index++ < cursor) {
				continue;
			}
			if (!SolersContextManager::append_bounded(results, _solers_project_result(files[i]), max_results, p_token_budget, result_tokens)) {
				truncated = true;
				break;
			}
		}
	} else {
		PackedStringArray files;
		{
			MutexLock lock(project_files_mutex);
			files = project_files;
			available = project_files_ready;
		}
		if (type == "text" || type == "symbol") {
			int match_index = 0;
			for (int i = 0; i < files.size(); i++) {
				const String path = files[i];
				if (!_solers_text_search_extension(path.get_extension().to_lower())) {
					continue;
				}
				scanned++;
				Error open_error = OK;
				Ref<FileAccess> file = FileAccess::open(path, FileAccess::READ, &open_error);
				if (file.is_null() || open_error != OK) {
					continue;
				}
				int line_index = 0;
				while (!file->eof_reached()) {
					const String line = file->get_line();
					const int column = _solers_find_text(line, query, type == "symbol");
					if (column < 0) {
						line_index++;
						continue;
					}
					if (match_index++ < cursor) {
						line_index++;
						continue;
					}
					Dictionary match = _solers_project_result(path);
					match["line"] = line_index + 1;
					match["column"] = column + 1;
					match["preview"] = line.strip_edges().left(240);
					if (!SolersContextManager::append_bounded(results, match, max_results, p_token_budget, result_tokens)) {
						truncated = true;
						break;
					}
					line_index++;
				}
				if (truncated) {
					break;
				}
			}
		}
	}

	Dictionary result;
	result["available"] = available;
	if (!available) {
		result["code"] = "EDITOR_INDEX_UPDATING";
	}
	result["type"] = type;
	result["query"] = query;
	result["results"] = results;
	result["count"] = results.size();
	result["cursor"] = cursor;
	if (truncated) {
		result["next_cursor"] = cursor + results.size();
	}
	result["scanned_count"] = scanned;
	result["truncated"] = truncated;
	return result;
}

Dictionary SolersProjectObservation::read_project_file(const String &p_path, int p_line_start, int p_line_count, bool p_raw, int p_token_budget) const {
	Dictionary result;
	const SolersPath::NormalizedPath normalized_path = SolersPath::normalize_project_path(p_path);
	if (!normalized_path.valid) {
		result["ok"] = false;
		result["error"] = normalized_path.error;
		return result;
	}
	const String res_path = normalized_path.value;

	if (!FileAccess::exists(res_path)) {
		result["ok"] = false;
		result["error"] = vformat("File does not exist.%s", solers_file_suggestions(res_path));
		result["path"] = res_path;
		return result;
	}

	const String resource_type = p_raw ? String() : ResourceLoader::get_resource_type(res_path);
	if (_solers_is_packed_scene_type(resource_type)) {
		const Dictionary observed = observe_path(res_path);
		result["ok"] = false;
		result["code"] = "SCENE_TEXT_DENIED";
		result["error"] = "PackedScene source text is not the scene fact model. The digest field is the complete answer for structure/identity — do not retry with raw=true. Use scene.inspect for live details. Pass raw=true only when editing .tscn/.scn source text.";
		result["path"] = res_path;
		result["resource_type"] = resource_type;
		result["recovery"] = "Use the included digest or scene.inspect. Do not call project.read_file with raw=true for observation.";
		if ((bool)observed.get("ok", false) && observed.has("digest")) {
			result["digest"] = observed["digest"];
		}
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
	const int line_start = MAX(1, p_line_start);
	const int line_count = MAX(1, p_line_count);
	int line = 1;
	while (line < line_start && !file->eof_reached()) {
		static_cast<void>(file->get_line());
		line++;
	}
	String content;
	int returned = 0;
	while (returned < line_count && !file->eof_reached()) {
		const String next = file->get_line();
		const String candidate = content + (content.is_empty() ? String() : "\n") + next;
		if (!content.is_empty() && SolersContextManager::estimate_tokens(candidate) > MAX(1, p_token_budget)) {
			break;
		}
		content = candidate;
		returned++;
		line++;
	}
	result["ok"] = true;
	result["path"] = res_path;
	result["size_bytes"] = length;
	result["sha256"] = FileAccess::get_sha256(res_path);
	result["line_start"] = line_start;
	result["line_end"] = line - 1;
	result["content"] = content;
	result["truncated"] = !file->eof_reached();
	if (!file->eof_reached()) {
		result["next_line"] = line;
	}
	return result;
}

void SolersProjectObservation::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_project_info"), &SolersProjectObservation::get_project_info);
	ClassDB::bind_method(D_METHOD("get_project_settings_summary"), &SolersProjectObservation::get_project_settings_summary);
	ClassDB::bind_method(D_METHOD("search_project", "args", "token_budget"), &SolersProjectObservation::search_project, DEFVAL(INT32_MAX));
	ClassDB::bind_method(D_METHOD("observe_path", "path"), &SolersProjectObservation::observe_path);
	ClassDB::bind_method(D_METHOD("digest_packed_scene", "path", "max_nodes"), &SolersProjectObservation::digest_packed_scene, DEFVAL(96));
	ClassDB::bind_method(D_METHOD("read_project_file", "path", "line_start", "line_count", "raw", "token_budget"), &SolersProjectObservation::read_project_file, DEFVAL(1), DEFVAL(200), DEFVAL(false), DEFVAL(INT32_MAX));
}

SolersProjectObservation::SolersProjectObservation() {
	if (EditorFileSystem *filesystem = EditorFileSystem::get_singleton()) {
		filesystem->connect(SNAME("filesystem_changed"), callable_mp(this, &SolersProjectObservation::_refresh_project_files));
		_refresh_project_files();
	}
}
