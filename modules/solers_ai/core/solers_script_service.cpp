/**************************************************************************/
/*  solers_script_service.cpp                                             */
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

#include "solers_script_service.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/resource_loader.h"
#include "core/object/class_db.h"
#include "core/object/script_language.h"
#include "editor/editor_interface.h"
#include "editor/file_system/editor_file_system.h"
#include "modules/solers_ai/core/solers_action_timeline.h"
#include "modules/solers_ai/core/solers_file_checkpoint.h"

void SolersScriptService::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_action_timeline", "action_timeline"), &SolersScriptService::set_action_timeline);
	ClassDB::bind_method(D_METHOD("set_file_checkpoint", "file_checkpoint"), &SolersScriptService::set_file_checkpoint);
	ClassDB::bind_method(D_METHOD("write_file", "args"), &SolersScriptService::write_file);
	ClassDB::bind_method(D_METHOD("patch_file", "args"), &SolersScriptService::patch_file);
	ClassDB::bind_method(D_METHOD("validate_script", "args"), &SolersScriptService::validate_script);
	ClassDB::bind_method(D_METHOD("validate_project_scripts", "args"), &SolersScriptService::validate_project_scripts);
	ClassDB::bind_method(D_METHOD("open_script", "args"), &SolersScriptService::open_script);
}

Dictionary SolersScriptService::_ok(const Variant &p_data) const {
	Dictionary result;
	result["ok"] = true;
	result["data"] = p_data;
	return result;
}

Dictionary SolersScriptService::_error(const String &p_code, const String &p_message, bool p_recoverable) const {
	Dictionary error;
	error["code"] = p_code;
	error["message"] = p_message;
	error["recoverable"] = p_recoverable;

	Dictionary result;
	result["ok"] = false;
	result["error"] = error;
	return result;
}

bool SolersScriptService::_normalize_project_path(const String &p_path, String &r_res_path, String &r_error, bool p_allow_project_data) const {
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
	const String project_data_path = ProjectSettings::get_singleton() ? ProjectSettings::get_singleton()->get_project_data_path() : "res://.godot";
	if (!p_allow_project_data && (path == project_data_path || path.begins_with(project_data_path.path_join("")))) {
		r_error = "Refusing to edit Godot project data directly.";
		return false;
	}

	r_res_path = path;
	return true;
}

void SolersScriptService::_collect_script_files(const String &p_dir, int p_max_files, Array &r_files, bool &r_truncated) const {
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
				_collect_script_files(path, p_max_files, r_files, r_truncated);
			}
		} else {
			const String extension = path.get_extension().to_lower();
			if (ScriptServer::get_language_for_extension(extension)) {
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

Dictionary SolersScriptService::_validate_source(const String &p_path, const String &p_source) const {
	Dictionary data;
	Array errors;
	Array warnings;
	Array functions;

	const String extension = p_path.get_extension().to_lower();
	ScriptLanguage *language = ScriptServer::get_language_for_extension(extension);
	if (!language) {
		data["path"] = p_path;
		data["language"] = String();
		data["supported"] = false;
		data["valid"] = true;
		data["errors"] = errors;
		data["warnings"] = warnings;
		data["functions"] = functions;
		return data;
	}

	List<String> function_list;
	List<ScriptLanguage::ScriptError> error_list;
	List<ScriptLanguage::Warning> warning_list;
	HashSet<int> safe_lines;
	const bool valid = language->validate(p_source, p_path, &function_list, &error_list, &warning_list, &safe_lines);

	for (const String &function_name : function_list) {
		functions.push_back(function_name);
	}

	for (const ScriptLanguage::ScriptError &E : error_list) {
		Dictionary item;
		item["path"] = E.path;
		item["line"] = E.line;
		item["column"] = E.column;
		item["message"] = E.message;
		errors.push_back(item);
	}

	for (const ScriptLanguage::Warning &W : warning_list) {
		Dictionary item;
		item["start_line"] = W.start_line;
		item["end_line"] = W.end_line;
		item["code"] = W.code;
		item["string_code"] = W.string_code;
		item["message"] = W.message;
		warnings.push_back(item);
	}

	data["path"] = p_path;
	data["language"] = language->get_name();
	data["supported"] = true;
	data["valid"] = valid;
	data["errors"] = errors;
	data["warnings"] = warnings;
	data["functions"] = functions;
	data["error_count"] = errors.size();
	data["warning_count"] = warnings.size();
	return data;
}

void SolersScriptService::set_action_timeline(SolersActionTimeline *p_action_timeline) {
	action_timeline = p_action_timeline;
}

void SolersScriptService::set_file_checkpoint(SolersFileCheckpoint *p_file_checkpoint) {
	file_checkpoint = p_file_checkpoint;
}

Dictionary SolersScriptService::write_file(const Dictionary &p_args) {
	const String path_arg = p_args.get("path", String());
	const String content = p_args.get("content", String());
	const bool create = p_args.get("create", true);
	const bool overwrite = p_args.get("overwrite", true);
	const bool validate_if_script = p_args.get("validate_if_script", true);

	String res_path;
	String path_error;
	if (!_normalize_project_path(path_arg, res_path, path_error)) {
		return _error("INVALID_PATH", path_error);
	}

	const bool existed_before = FileAccess::exists(res_path);
	if (existed_before && !overwrite) {
		return _error("FILE_EXISTS", vformat("File already exists and overwrite=false: %s", res_path));
	}
	if (!existed_before && !create) {
		return _error("FILE_NOT_FOUND", vformat("File does not exist and create=false: %s", res_path));
	}

	Dictionary checkpoint_result;
	if (file_checkpoint && existed_before) {
		checkpoint_result = file_checkpoint->create_checkpoint(res_path, "Solers file write");
		if (!checkpoint_result.get("ok", false)) {
			return checkpoint_result;
		}
	}

	Dictionary validation_data;
	if (validate_if_script) {
		validation_data = _validate_source(res_path, content);
		if ((bool)validation_data.get("supported", false) && !(bool)validation_data.get("valid", true)) {
			Dictionary data;
			data["path"] = res_path;
			data["validation"] = validation_data;
			return _error("SCRIPT_VALIDATE_FAILED", "Refusing to write script because validation failed.");
		}
	}

	Error dir_err = DirAccess::make_dir_recursive_absolute(ProjectSettings::get_singleton()->globalize_path(res_path.get_base_dir()));
	if (dir_err != OK) {
		return _error("DIRECTORY_CREATE_FAILED", vformat("Failed to create parent directory, error code %d.", dir_err));
	}

	Error write_err = OK;
	Ref<FileAccess> file = FileAccess::open(res_path, FileAccess::WRITE, &write_err);
	if (file.is_null() || write_err != OK) {
		return _error("FILE_WRITE_FAILED", vformat("Failed to open file for writing, error code %d.", write_err));
	}
	if (!file->store_string(content)) {
		return _error("FILE_WRITE_FAILED", "Failed to store file content.");
	}
	file.unref();

	if (EditorFileSystem::get_singleton()) {
		EditorFileSystem::get_singleton()->update_file(res_path);
	}

	Dictionary data;
	data["path"] = res_path;
	data["created"] = !existed_before;
	data["overwritten"] = existed_before;
	data["size_bytes"] = content.utf8().length();
	data["checkpoint"] = checkpoint_result;
	data["validation"] = validation_data;

	if (action_timeline) {
		action_timeline->record_event("file_written", data);
	}

	return _ok(data);
}

Dictionary SolersScriptService::patch_file(const Dictionary &p_args) {
	const String path_arg = p_args.get("path", String());
	const String old_text = p_args.get("old_text", String());
	const String new_text = p_args.get("new_text", String());
	const String expected_sha256 = p_args.get("expected_sha256", String());
	const bool validate_if_script = p_args.get("validate_if_script", true);
	const int occurrence = p_args.get("occurrence", 1);

	if (old_text.is_empty()) {
		return _error("INVALID_ARGUMENT", "old_text is required for script.patch.");
	}
	if (occurrence < 1) {
		return _error("INVALID_ARGUMENT", "occurrence must be 1 or greater.");
	}

	String res_path;
	String path_error;
	if (!_normalize_project_path(path_arg, res_path, path_error)) {
		return _error("INVALID_PATH", path_error);
	}
	if (!FileAccess::exists(res_path)) {
		return _error("FILE_NOT_FOUND", vformat("File does not exist: %s", res_path));
	}

	if (!expected_sha256.is_empty()) {
		const String current_sha256 = FileAccess::get_sha256(res_path);
		if (current_sha256 != expected_sha256) {
			return _error("FILE_CHANGED", "File sha256 does not match expected_sha256.");
		}
	}

	Error read_err = OK;
	String content = FileAccess::get_file_as_string(res_path, &read_err);
	if (read_err != OK) {
		return _error("FILE_READ_FAILED", vformat("Failed to read file, error code %d.", read_err));
	}

	int found_pos = -1;
	int search_from = 0;
	for (int i = 0; i < occurrence; i++) {
		found_pos = content.find(old_text, search_from);
		if (found_pos == -1) {
			return _error("PATCH_TEXT_NOT_FOUND", vformat("old_text occurrence %d was not found.", occurrence));
		}
		search_from = found_pos + old_text.length();
	}

	const String patched = content.substr(0, found_pos) + new_text + content.substr(found_pos + old_text.length());
	Dictionary write_args;
	write_args["path"] = res_path;
	write_args["content"] = patched;
	write_args["create"] = false;
	write_args["overwrite"] = true;
	write_args["validate_if_script"] = validate_if_script;
	Dictionary write_result = write_file(write_args);
	if (!(bool)write_result.get("ok", false)) {
		return write_result;
	}

	Dictionary data = write_result.get("data", Dictionary());
	data["old_text_length"] = old_text.length();
	data["new_text_length"] = new_text.length();
	data["patched_offset"] = found_pos;
	data["occurrence"] = occurrence;
	return _ok(data);
}

Dictionary SolersScriptService::validate_script(const Dictionary &p_args) const {
	const String path_arg = p_args.get("path", String());
	const String source_override = p_args.get("source", String());

	String res_path;
	String path_error;
	if (!_normalize_project_path(path_arg, res_path, path_error)) {
		return _error("INVALID_PATH", path_error);
	}

	String source = source_override;
	if (source.is_empty()) {
		if (!FileAccess::exists(res_path)) {
			return _error("FILE_NOT_FOUND", vformat("Script file does not exist: %s", res_path));
		}
		Error read_err = OK;
		source = FileAccess::get_file_as_string(res_path, &read_err);
		if (read_err != OK) {
			return _error("FILE_READ_FAILED", vformat("Failed to read script, error code %d.", read_err));
		}
	}

	return _ok(_validate_source(res_path, source));
}

Dictionary SolersScriptService::validate_project_scripts(const Dictionary &p_args) const {
	const int max_files = CLAMP((int)p_args.get("max_files", 256), 0, 5000);
	Array script_files;
	bool truncated = false;
	_collect_script_files("res://", max_files, script_files, truncated);

	Array validations;
	int invalid_count = 0;
	int error_count = 0;
	int warning_count = 0;
	for (int i = 0; i < script_files.size(); i++) {
		const String path = script_files[i];
		Error read_err = OK;
		const String source = FileAccess::get_file_as_string(path, &read_err);
		Dictionary validation;
		if (read_err != OK) {
			validation["path"] = path;
			validation["valid"] = false;
			validation["read_error"] = read_err;
		} else {
			validation = _validate_source(path, source);
		}
		if (!(bool)validation.get("valid", true)) {
			invalid_count++;
		}
		error_count += (int)validation.get("error_count", 0);
		warning_count += (int)validation.get("warning_count", 0);
		validations.push_back(validation);
	}

	Dictionary data;
	data["passed"] = invalid_count == 0;
	data["script_files"] = script_files;
	data["validations"] = validations;
	data["count"] = script_files.size();
	data["invalid_count"] = invalid_count;
	data["error_count"] = error_count;
	data["warning_count"] = warning_count;
	data["truncated"] = truncated;
	return _ok(data);
}

Dictionary SolersScriptService::open_script(const Dictionary &p_args) const {
	const String path_arg = p_args.get("path", String());
	const int line = p_args.get("line", 1);
	const int column = p_args.get("column", 1);
	const bool grab_focus = p_args.get("grab_focus", true);

	String res_path;
	String path_error;
	if (!_normalize_project_path(path_arg, res_path, path_error)) {
		return _error("INVALID_PATH", path_error);
	}
	if (!FileAccess::exists(res_path)) {
		return _error("FILE_NOT_FOUND", vformat("Script file does not exist: %s", res_path));
	}

	Ref<Script> script = ResourceLoader::load(res_path);
	if (script.is_null()) {
		return _error("SCRIPT_LOAD_FAILED", vformat("Unable to load script resource: %s", res_path));
	}

	EditorInterface *editor_interface = EditorInterface::get_singleton();
	ERR_FAIL_NULL_V(editor_interface, _error("EDITOR_INTERFACE_UNAVAILABLE", "EditorInterface is not available.", false));
	editor_interface->edit_script(script, MAX(1, line), MAX(1, column), grab_focus);

	Dictionary data;
	data["path"] = res_path;
	data["line"] = MAX(1, line);
	data["column"] = MAX(1, column);
	return _ok(data);
}
