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
#include "core/object/class_db.h"
#include "core/os/time.h"
#include "modules/solers_ai/core/solers_action_timeline.h"

void SolersFileCheckpoint::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_action_timeline", "action_timeline"), &SolersFileCheckpoint::set_action_timeline);
	ClassDB::bind_method(D_METHOD("create_checkpoint", "path", "reason"), &SolersFileCheckpoint::create_checkpoint, DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("restore_checkpoint", "checkpoint_path", "target_path"), &SolersFileCheckpoint::restore_checkpoint);
	ClassDB::bind_method(D_METHOD("get_checkpoint_root_info"), &SolersFileCheckpoint::get_checkpoint_root_info);
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

Dictionary SolersFileCheckpoint::create_checkpoint(const String &p_path, const String &p_reason) {
	String res_path;
	String path_error;
	if (!_normalize_project_path(p_path, res_path, path_error)) {
		return _error("INVALID_PATH", path_error);
	}
	if (!FileAccess::exists(res_path)) {
		Dictionary data;
		data["path"] = res_path;
		data["existed"] = false;
		return _ok(data);
	}

	String checkpoint_root = _checkpoint_root();
	if (checkpoint_root.is_empty()) {
		return _error("PROJECT_SETTINGS_UNAVAILABLE", "ProjectSettings is not available.", false);
	}

	Error dir_err = DirAccess::make_dir_recursive_absolute(ProjectSettings::get_singleton()->globalize_path(checkpoint_root));
	if (dir_err != OK) {
		return _error("CHECKPOINT_DIR_FAILED", vformat("Failed to create checkpoint directory, error code %d.", dir_err));
	}

	Error read_err = OK;
	Vector<uint8_t> bytes = FileAccess::get_file_as_bytes(res_path, &read_err);
	if (read_err != OK) {
		return _error("CHECKPOINT_READ_FAILED", vformat("Failed to read source file, error code %d.", read_err));
	}

	const String timestamp = Time::get_singleton()->get_datetime_string_from_system(false, true).replace_char(' ', '_').replace_char(':', '-');
	const String checkpoint_file = vformat("%s_%s_%s", timestamp, res_path.md5_text(), res_path.get_file());
	const String checkpoint_path = checkpoint_root.path_join(checkpoint_file);

	Error write_err = OK;
	Ref<FileAccess> checkpoint_file_access = FileAccess::open(checkpoint_path, FileAccess::WRITE, &write_err);
	if (checkpoint_file_access.is_null() || write_err != OK) {
		return _error("CHECKPOINT_WRITE_FAILED", vformat("Failed to write checkpoint file, error code %d.", write_err));
	}
	if (!bytes.is_empty() && !checkpoint_file_access->store_buffer(bytes.ptr(), bytes.size())) {
		return _error("CHECKPOINT_WRITE_FAILED", "Failed to store checkpoint bytes.");
	}
	checkpoint_file_access.unref();

	Dictionary data;
	data["path"] = res_path;
	data["checkpoint_path"] = checkpoint_path;
	data["existed"] = true;
	data["size_bytes"] = bytes.size();
	data["reason"] = p_reason;
	data["content_sha256"] = FileAccess::get_sha256(res_path);
	data["timestamp_unix"] = Time::get_singleton()->get_unix_time_from_system();

	if (action_timeline) {
		action_timeline->record_event("file_checkpoint_created", data);
	}

	return _ok(data);
}

Dictionary SolersFileCheckpoint::restore_checkpoint(const String &p_checkpoint_path, const String &p_target_path) {
	String target_path;
	String path_error;
	if (!_normalize_project_path(p_target_path, target_path, path_error)) {
		return _error("INVALID_PATH", path_error);
	}

	String checkpoint_path = p_checkpoint_path.strip_edges().replace_char('\\', '/').simplify_path();
	if (!checkpoint_path.begins_with(_checkpoint_root())) {
		return _error("INVALID_CHECKPOINT_PATH", "Checkpoint path must be inside the Solers checkpoint directory.");
	}
	if (!FileAccess::exists(checkpoint_path)) {
		return _error("CHECKPOINT_NOT_FOUND", "Checkpoint file does not exist.");
	}

	Error read_err = OK;
	Vector<uint8_t> bytes = FileAccess::get_file_as_bytes(checkpoint_path, &read_err);
	if (read_err != OK) {
		return _error("CHECKPOINT_READ_FAILED", vformat("Failed to read checkpoint, error code %d.", read_err));
	}

	Error dir_err = DirAccess::make_dir_recursive_absolute(ProjectSettings::get_singleton()->globalize_path(target_path.get_base_dir()));
	if (dir_err != OK) {
		return _error("TARGET_DIR_FAILED", vformat("Failed to create target directory, error code %d.", dir_err));
	}

	Error write_err = OK;
	Ref<FileAccess> file = FileAccess::open(target_path, FileAccess::WRITE, &write_err);
	if (file.is_null() || write_err != OK) {
		return _error("RESTORE_WRITE_FAILED", vformat("Failed to restore file, error code %d.", write_err));
	}
	if (!bytes.is_empty() && !file->store_buffer(bytes.ptr(), bytes.size())) {
		return _error("RESTORE_WRITE_FAILED", "Failed to store restored bytes.");
	}

	Dictionary data;
	data["checkpoint_path"] = checkpoint_path;
	data["target_path"] = target_path;
	data["size_bytes"] = bytes.size();

	if (action_timeline) {
		action_timeline->record_event("file_checkpoint_restored", data);
	}

	return _ok(data);
}

Dictionary SolersFileCheckpoint::get_checkpoint_root_info() const {
	Dictionary data;
	const String root = _checkpoint_root();
	data["checkpoint_root"] = root;
	data["global_checkpoint_root"] = root.is_empty() ? String() : ProjectSettings::get_singleton()->globalize_path(root);
	return _ok(data);
}
