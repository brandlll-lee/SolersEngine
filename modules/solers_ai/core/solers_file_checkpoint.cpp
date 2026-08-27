/**************************************************************************/
/*  solers_file_checkpoint.cpp                                            */
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

#include "solers_file_checkpoint.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/io/resource_loader.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/os/time.h"
#include "editor/file_system/editor_file_system.h"
#include "editor/settings/editor_settings.h"

#include "modules/solers_ai/core/solers_action_timeline.h"

void SolersFileCheckpoint::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_action_timeline", "action_timeline"), &SolersFileCheckpoint::set_action_timeline);
	ClassDB::bind_method(D_METHOD("create_checkpoint", "path", "reason"), &SolersFileCheckpoint::create_checkpoint, DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("remove_project_path", "path"), &SolersFileCheckpoint::remove_project_path);
	ClassDB::bind_method(D_METHOD("restore_checkpoint_state", "checkpoint"), &SolersFileCheckpoint::restore_checkpoint_state);
	ClassDB::bind_method(D_METHOD("get_path_state", "path"), &SolersFileCheckpoint::get_path_state);
}

Dictionary SolersFileCheckpoint::_ok(const Variant &p_data) const {
	Dictionary result;
	result["ok"] = true;
	result["data"] = p_data;
	return result;
}

Dictionary SolersFileCheckpoint::_error(const String &p_code, const String &p_message, bool p_recoverable) const {
	Dictionary error;
	error["code"] = p_code;
	error["message"] = p_message;
	error["recoverable"] = p_recoverable;

	Dictionary result;
	result["ok"] = false;
	result["error"] = error;
	return result;
}

bool SolersFileCheckpoint::_normalize_project_path(const String &p_path, String &r_res_path, String &r_error) const {
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
	if (path.begins_with("res://.git/") || path == "res://.git") {
		r_error = "Refusing to operate on .git metadata.";
		return false;
	}

	r_res_path = path;
	return true;
}

String SolersFileCheckpoint::_checkpoint_root() const {
	ProjectSettings *project_settings = ProjectSettings::get_singleton();
	ERR_FAIL_NULL_V(project_settings, String());
	return project_settings->get_project_data_path().path_join("solers/checkpoints");
}

void SolersFileCheckpoint::set_action_timeline(SolersActionTimeline *p_action_timeline) {
	action_timeline = p_action_timeline;
}

static void _collect_directory_state(const String &p_path, const String &p_base, PackedStringArray &r_entries) {
	Ref<DirAccess> directory = DirAccess::open(p_path);
	if (directory.is_null() || directory->list_dir_begin() != OK) {
		return;
	}
	for (String name = directory->get_next(); !name.is_empty(); name = directory->get_next()) {
		if (name == "." || name == "..") {
			continue;
		}
		const String child = p_path.path_join(name);
		if (directory->current_is_dir()) {
			_collect_directory_state(child, p_base, r_entries);
		} else {
			r_entries.push_back(child.trim_prefix(p_base) + ":" + FileAccess::get_sha256(child));
		}
	}
	directory->list_dir_end();
}

static void _collect_indexed_files(EditorFileSystemDirectory *p_directory, HashSet<String> &r_files) {
	if (!p_directory) {
		return;
	}
	for (int i = 0; i < p_directory->get_subdir_count(); i++) {
		_collect_indexed_files(p_directory->get_subdir(i), r_files);
	}
	for (int i = 0; i < p_directory->get_file_count(); i++) {
		r_files.insert(p_directory->get_file_path(i));
	}
}

static void _collect_external_owners(EditorFileSystemDirectory *p_directory, const HashSet<String> &p_removed, Array &r_owners) {
	if (!p_directory) {
		return;
	}
	for (int i = 0; i < p_directory->get_subdir_count(); i++) {
		_collect_external_owners(p_directory->get_subdir(i), p_removed, r_owners);
	}
	for (int i = 0; i < p_directory->get_file_count(); i++) {
		const String owner = p_directory->get_file_path(i);
		if (p_removed.has(owner)) {
			continue;
		}
		for (const String &dependency : p_directory->get_file_deps(i)) {
			if (p_removed.has(dependency)) {
				r_owners.push_back(Dictionary({ { "path", owner }, { "type", p_directory->get_file_type(i) }, { "dependency", dependency } }));
			}
		}
	}
}

static String _project_setting_path(const PropertyInfo &p_property, const Variant &p_value) {
	if (p_property.type != Variant::STRING || (p_property.hint != PROPERTY_HINT_FILE && p_property.hint != PROPERTY_HINT_DIR && p_property.hint != PROPERTY_HINT_FILE_PATH)) {
		return String();
	}
	return ResourceUID::ensure_path(p_value);
}

Dictionary SolersFileCheckpoint::get_path_state(const String &p_path) const {
	String path;
	String error;
	if (!_normalize_project_path(p_path, path, error)) {
		return _error("INVALID_PATH", error);
	}
	Dictionary data;
	data["path"] = path;
	data["directory"] = DirAccess::exists(path);
	data["existed"] = (bool)data["directory"] || FileAccess::exists(path);
	if ((bool)data["directory"]) {
		PackedStringArray entries;
		_collect_directory_state(path, path, entries);
		entries.sort();
		data["content_sha256"] = String("\n").join(entries).sha256_text();
	} else if ((bool)data["existed"]) {
		data["content_sha256"] = FileAccess::get_sha256(path);
	}
	return _ok(data);
}

Dictionary SolersFileCheckpoint::create_checkpoint(const String &p_path, const String &p_reason) {
	String res_path;
	String path_error;
	if (!_normalize_project_path(p_path, res_path, path_error)) {
		return _error("INVALID_PATH", path_error);
	}
	Dictionary data = get_path_state(res_path).get("data", Dictionary());
	EditorSettings *editor_settings = EditorSettings::get_singleton();
	const String prefix = res_path.ends_with("/") ? res_path : res_path + "/";
	PackedStringArray favorites;
	if (editor_settings) {
		for (const String &favorite : editor_settings->get_favorites()) {
			if (favorite == res_path || ((bool)data.get("directory", false) && favorite.begins_with(prefix))) {
				favorites.push_back(favorite);
			}
		}
	}
	data["favorites"] = favorites;
	Dictionary project_settings;
	List<PropertyInfo> properties;
	ProjectSettings::get_singleton()->get_property_list(&properties);
	for (const PropertyInfo &property : properties) {
		const Variant value = ProjectSettings::get_singleton()->get(property.name);
		const String path = _project_setting_path(property, value);
		if (path == res_path || ((bool)data.get("directory", false) && path.begins_with(prefix))) {
			project_settings[property.name] = value;
		}
	}
	if (!project_settings.is_empty()) {
		data["project_settings"] = project_settings;
	}
	if (!(bool)data.get("existed", false)) {
		return _ok(data);
	}
	if (ResourceCache::has(res_path)) {
		data["resource_object_id"] = (int64_t)ResourceCache::get_ref(res_path)->get_instance_id();
	}

	String checkpoint_root = _checkpoint_root();
	if (checkpoint_root.is_empty()) {
		return _error("PROJECT_SETTINGS_UNAVAILABLE", "ProjectSettings is not available.", false);
	}

	Error dir_err = DirAccess::make_dir_recursive_absolute(ProjectSettings::get_singleton()->globalize_path(checkpoint_root));
	if (dir_err != OK) {
		return _error("CHECKPOINT_DIR_FAILED", vformat("Failed to create checkpoint directory, error code %d.", dir_err));
	}

	const String timestamp = Time::get_singleton()->get_datetime_string_from_system(false, true).replace_char(' ', '_').replace_char(':', '-');
	const String checkpoint_file = vformat("%s_%s_%s_%s", timestamp, String::num_uint64(Time::get_singleton()->get_ticks_usec()), res_path.md5_text(), res_path.get_file());
	const String checkpoint_path = checkpoint_root.path_join(checkpoint_file);
	Error copy_error = OK;
	if ((bool)data.get("directory", false)) {
		Ref<DirAccess> source = DirAccess::open(res_path);
		copy_error = source.is_valid() ? source->copy_dir(source->get_current_dir(), ProjectSettings::get_singleton()->globalize_path(checkpoint_path)) : ERR_CANT_OPEN;
	} else {
		copy_error = DirAccess::copy_absolute(res_path, ProjectSettings::get_singleton()->globalize_path(checkpoint_path));
	}
	if (copy_error != OK) {
		return _error("CHECKPOINT_WRITE_FAILED", vformat("Failed to copy checkpoint state, error code %d.", copy_error));
	}
	data["checkpoint_path"] = checkpoint_path;
	data["reason"] = p_reason;
	data["timestamp_unix"] = Time::get_singleton()->get_unix_time_from_system();

	if (action_timeline) {
		action_timeline->record_event("file_checkpoint_created", data);
	}

	return _ok(data);
}

Dictionary SolersFileCheckpoint::remove_project_path(const String &p_path) {
	String path;
	String path_error;
	if (!_normalize_project_path(p_path, path, path_error)) {
		return _error("INVALID_PATH", path_error);
	}
	const bool directory = DirAccess::exists(path);
	if ((!directory && !FileAccess::exists(path)) || path == "res://") {
		return _error("PATH_NOT_FOUND", vformat("Project path does not exist or cannot be removed: %s", path));
	}
	EditorFileSystem *filesystem = EditorFileSystem::get_singleton();
	if (!filesystem || !filesystem->get_filesystem() || filesystem->is_scanning()) {
		return _error("EDITOR_FILESYSTEM_UNAVAILABLE", "EditorFileSystem is not ready.");
	}
	HashSet<String> removed_files;
	const String prefix = path.ends_with("/") ? path : path + "/";
	if (directory) {
		_collect_indexed_files(filesystem->get_filesystem_path(path), removed_files);
	} else {
		removed_files.insert(path);
	}
	Array owners;
	_collect_external_owners(filesystem->get_filesystem(), removed_files, owners);
	if (!owners.is_empty()) {
		Dictionary result = _error("PATH_HAS_OWNERS", vformat("Project path is referenced by %d resource(s) outside the removal set.", owners.size()));
		result["data"] = Dictionary({ { "path", path }, { "owners", owners } });
		return result;
	}
	const Error remove_error = OS::get_singleton()->move_to_trash(ProjectSettings::get_singleton()->globalize_path(path));
	if (remove_error != OK || FileAccess::exists(path) || DirAccess::exists(path)) {
		Dictionary result = _error("PATH_REMOVE_FAILED", vformat("Godot could not move '%s' to the system trash (error %d).", path, remove_error));
		result["data"] = Dictionary({ { "path", path }, { "native_error", remove_error } });
		return result;
	}
	for (const String &file : removed_files) {
		if (ResourceCache::has(file)) {
			ResourceCache::get_ref(file)->set_path("");
		}
	}
	bool settings_changed = false;
	List<PropertyInfo> properties;
	ProjectSettings::get_singleton()->get_property_list(&properties);
	for (const PropertyInfo &property : properties) {
		const String value = _project_setting_path(property, ProjectSettings::get_singleton()->get(property.name));
		if (!value.is_empty() && (removed_files.has(value) || (directory && value.begins_with(prefix)))) {
			ProjectSettings::get_singleton()->set(property.name, "");
			settings_changed = true;
		}
	}
	if (settings_changed) {
		ProjectSettings::get_singleton()->save();
	}
	EditorSettings *editor_settings = EditorSettings::get_singleton();
	if (editor_settings) {
		const Vector<String> previous = editor_settings->get_favorites();
		Vector<String> favorites;
		for (const String &favorite : previous) {
			if (favorite != path && (!directory || !favorite.begins_with(prefix))) {
				favorites.push_back(favorite);
			}
		}
		if (favorites.size() != previous.size()) {
			editor_settings->set_favorites(favorites);
		}
	}
	if (directory) {
		filesystem->scan_changes();
	} else {
		filesystem->update_file(path);
	}
	return _ok(Dictionary({ { "path", path }, { "directory", directory }, { "removed_files", removed_files.size() }, { "owners", owners }, { "native_error", remove_error }, { "authored_state_changed", true } }));
}

Dictionary SolersFileCheckpoint::restore_checkpoint_state(const Dictionary &p_checkpoint) {
	const String target_path = p_checkpoint.get("path", String());
	String normalized_path;
	String path_error;
	if (!_normalize_project_path(target_path, normalized_path, path_error)) {
		return _error("INVALID_PATH", path_error);
	}

	Dictionary result;
	if ((bool)p_checkpoint.get("directory", false)) {
		const String global_target = ProjectSettings::get_singleton()->globalize_path(normalized_path);
		if (DirAccess::exists(normalized_path)) {
			Ref<DirAccess> target = DirAccess::open(normalized_path);
			if (target.is_null() || target->erase_contents_recursive() != OK) {
				return _error("RESTORE_DELETE_FAILED", "Failed to clear the changed checkpoint directory.");
			}
		}
		if ((bool)p_checkpoint.get("existed", false)) {
			DirAccess::make_dir_recursive_absolute(global_target);
			const String checkpoint_path = p_checkpoint.get("checkpoint_path", String());
			Ref<DirAccess> source = DirAccess::open(checkpoint_path);
			if (source.is_null() || source->copy_dir(source->get_current_dir(), global_target) != OK) {
				return _error("RESTORE_WRITE_FAILED", "Failed to restore the checkpoint directory.");
			}
		} else if (DirAccess::exists(normalized_path) && DirAccess::remove_absolute(global_target) != OK) {
			return _error("RESTORE_DELETE_FAILED", "Failed to remove the newly created directory.");
		}
		result = _ok(Dictionary({ { "target_path", normalized_path } }));
	} else if ((bool)p_checkpoint.get("existed", false)) {
		const String checkpoint_path = p_checkpoint.get("checkpoint_path", String());
		if (!checkpoint_path.simplify_path().begins_with(_checkpoint_root()) || !FileAccess::exists(checkpoint_path)) {
			return _error("CHECKPOINT_NOT_FOUND", "The structured checkpoint file is unavailable.");
		}
		const Error error = DirAccess::copy_absolute(checkpoint_path, ProjectSettings::get_singleton()->globalize_path(normalized_path));
		if (error != OK) {
			return _error("RESTORE_WRITE_FAILED", vformat("Failed to restore checkpoint state, error code %d.", error));
		}
		result = _ok(Dictionary({ { "checkpoint_path", checkpoint_path }, { "target_path", normalized_path } }));
	} else {
		if (FileAccess::exists(normalized_path)) {
			const Error error = DirAccess::remove_absolute(ProjectSettings::get_singleton()->globalize_path(normalized_path));
			if (error != OK) {
				return _error("RESTORE_DELETE_FAILED", vformat("Failed to remove newly created file, error code %d.", error));
			}
		}
		if (ResourceCache::has(normalized_path)) {
			ResourceCache::get_ref(normalized_path)->set_path("");
		}
		Dictionary data;
		data["target_path"] = normalized_path;
		data["removed_created_file"] = true;
		result = _ok(data);
	}
	if (!(bool)result.get("ok", false)) {
		return result;
	}
	const Dictionary project_settings = p_checkpoint.get("project_settings", Dictionary());
	for (const Variant *setting = project_settings.next(nullptr); setting; setting = project_settings.next(setting)) {
		ProjectSettings::get_singleton()->set(*setting, project_settings[*setting]);
	}
	if (!project_settings.is_empty()) {
		ProjectSettings::get_singleton()->save();
	}
	Resource *resource = Object::cast_to<Resource>(ObjectDB::get_instance(ObjectID((uint64_t)(int64_t)p_checkpoint.get("resource_object_id", 0))));
	if (resource && resource->get_path().is_empty()) {
		resource->set_path(normalized_path);
	}
	EditorSettings *editor_settings = EditorSettings::get_singleton();
	if (editor_settings) {
		Vector<String> favorites = editor_settings->get_favorites();
		for (const String &favorite : PackedStringArray(p_checkpoint.get("favorites", PackedStringArray()))) {
			if (!favorites.has(favorite)) {
				favorites.push_back(favorite);
			}
		}
		editor_settings->set_favorites(favorites);
	}

	EditorFileSystem *filesystem = EditorFileSystem::get_singleton();
	if (filesystem && filesystem->get_filesystem() && !filesystem->is_scanning()) {
		if ((bool)p_checkpoint.get("directory", false)) {
			filesystem->scan_changes();
		} else {
			filesystem->update_file(normalized_path);
		}
	}
	if (FileAccess::exists(normalized_path)) {
		ResourceLoader::load(normalized_path, String(), ResourceFormatLoader::CACHE_MODE_REPLACE);
	}
	if (action_timeline) {
		action_timeline->record_event("file_checkpoint_restored", result.get("data", Dictionary()));
	}
	return result;
}

void SolersFileCheckpoint::discard_checkpoint_state(const Dictionary &p_checkpoint) {
	const String checkpoint_path = String(p_checkpoint.get("checkpoint_path", String())).simplify_path();
	if (!checkpoint_path.is_empty() && checkpoint_path.begins_with(_checkpoint_root())) {
		if (DirAccess::exists(checkpoint_path)) {
			Ref<DirAccess> directory = DirAccess::open(checkpoint_path);
			directory->erase_contents_recursive();
		}
		DirAccess::remove_absolute(ProjectSettings::get_singleton()->globalize_path(checkpoint_path));
	}
}
