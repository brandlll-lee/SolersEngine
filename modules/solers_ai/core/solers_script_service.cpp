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

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/core_bind.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/resource_format_binary.h"
#include "core/io/resource_importer.h"
#include "core/io/resource_loader.h"
#include "core/io/logger.h"
#include "core/object/callable_method_pointer.h"
#include "core/object/class_db.h"
#include "core/object/script_language.h"
#include "core/os/os.h"
#include "editor/editor_node.h"
#include "scene/main/node.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/file_system/editor_file_system.h"
#include "modules/solers_ai/core/solers_action_timeline.h"
#include "scene/resources/resource_format_text.h"
#include "scene/resources/shader.h"
#include "servers/rendering/shader_language.h"
#include "servers/rendering/shader_preprocessor.h"
#include "servers/rendering/shader_types.h"

#include <cstdarg>
#include <cstdio>

void SolersScriptService::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_action_timeline", "action_timeline"), &SolersScriptService::set_action_timeline);
	ClassDB::bind_method(D_METHOD("write_file", "args"), &SolersScriptService::write_file);
	ClassDB::bind_method(D_METHOD("patch_file", "args"), &SolersScriptService::patch_file);
	ClassDB::bind_method(D_METHOD("edit_project", "args"), &SolersScriptService::edit_project);
	ClassDB::bind_method(D_METHOD("edit_script", "args"), &SolersScriptService::edit_script);
	ClassDB::bind_method(D_METHOD("validate_script", "args"), &SolersScriptService::validate_script);
	ClassDB::bind_method(D_METHOD("run_script", "args"), &SolersScriptService::run_script);
	ClassDB::bind_method(D_METHOD("_apply_project_settings", "values", "erase"), &SolersScriptService::_apply_project_settings);
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
	if (!p_allow_project_data && (path == project_data_path || path.begins_with(project_data_path + "/"))) {
		r_error = "Refusing to edit Godot project data directly.";
		return false;
	}

	r_res_path = path;
	return true;
}

Dictionary SolersScriptService::_validate_source(const String &p_path, const String &p_source) const {
	Dictionary data;
	Array errors;
	Array warnings;
	Array functions;

	const String extension = p_path.get_extension().to_lower();
	if (extension == "gdshader" || extension == "gdshaderinc") {
		String preprocessed;
		String preprocess_error;
		List<ShaderPreprocessor::FilePosition> preprocess_positions;
		ShaderPreprocessor preprocessor;
		Error validation_error = preprocessor.preprocess(p_source, p_path, preprocessed, &preprocess_error, &preprocess_positions);
		ShaderLanguage language;
		if (validation_error == OK) {
			ShaderLanguage::ShaderCompileInfo compile_info;
			if (extension == "gdshaderinc") {
				compile_info.is_include = true;
			} else {
				Shader::Mode mode = Shader::MODE_SPATIAL;
				const String shader_type = ShaderLanguage::get_shader_type(preprocessed);
				if (shader_type == "canvas_item") {
					mode = Shader::MODE_CANVAS_ITEM;
				} else if (shader_type == "particles") {
					mode = Shader::MODE_PARTICLES;
				} else if (shader_type == "sky") {
					mode = Shader::MODE_SKY;
				} else if (shader_type == "fog") {
					mode = Shader::MODE_FOG;
				}
				ShaderTypes *types = ShaderTypes::get_singleton();
				if (!types) {
					return _error("SHADER_VALIDATOR_UNAVAILABLE", "Godot's shader type registry is unavailable.", false);
				}
				compile_info.functions = types->get_functions(RenderingServer::ShaderMode(mode));
				compile_info.render_modes = types->get_modes(RenderingServer::ShaderMode(mode));
				compile_info.stencil_modes = types->get_stencil_modes(RenderingServer::ShaderMode(mode));
				compile_info.shader_types = types->get_types();
			}
			validation_error = language.compile(preprocessed, compile_info);
		}
		if (validation_error != OK) {
			Dictionary item;
			item["path"] = p_path;
			item["line"] = preprocess_positions.is_empty() ? language.get_error_line() : preprocess_positions.front()->get().line;
			item["message"] = preprocess_error.is_empty() ? language.get_error_text() : preprocess_error;
			errors.push_back(item);
		}
		data["path"] = p_path;
		data["language"] = "Godot Shader";
		data["supported"] = true;
		data["valid"] = validation_error == OK;
		data["errors"] = errors;
		data["warnings"] = warnings;
		data["functions"] = functions;
		data["error_count"] = errors.size();
		data["warning_count"] = 0;
		return data;
	}
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

static bool _solers_is_native_serialized_resource_path(const String &p_path) {
	List<String> extensions;
	if (ResourceFormatLoaderText::singleton) {
		ResourceFormatLoaderText::singleton->get_recognized_extensions(&extensions);
	}
	ResourceFormatLoaderBinary binary_loader;
	binary_loader.get_recognized_extensions(&extensions);
	const String extension = p_path.get_extension().to_lower();
	for (const String &recognized : extensions) {
		if (extension == recognized.to_lower()) {
			return true;
		}
	}
	return false;
}

static bool _solers_is_script_source_path(const String &p_path) {
	const String extension = p_path.get_extension().to_lower();
	return extension == "gd" || extension == "cs" || extension == "gdshader" || extension == "gdshaderinc";
}

static bool _solers_is_project_settings_path(const String &p_path) {
	String path = p_path.strip_edges().replace_char('\\', '/');
	if (!path.begins_with("res://")) {
		path = String("res://").path_join(path);
	}
	return path.simplify_path() == "res://project.godot";
}

Dictionary SolersScriptService::write_file(const Dictionary &p_args) {
	const String path_arg = p_args.get("path", String());
	const String content = p_args.get("content", String());
	const String content_base64 = p_args.get("content_base64", String());
	const bool has_text_content = p_args.has("content");
	const bool has_binary_content = !content_base64.strip_edges().is_empty();
	if (has_text_content == has_binary_content) {
		return _error("INVALID_ARGUMENT", "Provide content OR content_base64 (non-empty), not both. Omit the unused key entirely.");
	}
	const bool create = p_args.get("create", true);
	const bool overwrite = p_args.get("overwrite", true);

	String res_path;
	String path_error;
	if (!_normalize_project_path(path_arg, res_path, path_error)) {
		return _error("INVALID_PATH", path_error);
	}
	if (_solers_is_project_settings_path(res_path)) {
		return _error("EDITOR_OWNED_FILE", "Modify project.godot through project.edit settings so the live ProjectSettings state stays synchronized.");
	}
	if (_solers_is_native_serialized_resource_path(res_path)) {
		return _error("NATIVE_RESOURCE_WRITE_BLOCKED", "Godot serialized resources must be edited through scene.edit or resource.edit, not raw file writes.");
	}

	const bool existed_before = FileAccess::exists(res_path);
	if (existed_before && !overwrite) {
		return _error("FILE_EXISTS", vformat("File already exists and overwrite=false: %s", res_path));
	}
	if (!existed_before && !create) {
		return _error("FILE_NOT_FOUND", vformat("File does not exist and create=false: %s", res_path));
	}
	const bool is_script_text = has_text_content && _solers_is_script_source_path(res_path);

	Vector<uint8_t> bytes;
	if (has_binary_content) {
		CoreBind::Marshalls *marshalls = CoreBind::Marshalls::get_singleton();
		ERR_FAIL_NULL_V(marshalls, _error("MARSHALLS_UNAVAILABLE", "Base64 decoder is not available.", false));
		bytes = marshalls->base64_to_raw(content_base64);
		if (!content_base64.is_empty() && bytes.is_empty()) {
			return _error("INVALID_BASE64", "content_base64 is not valid base64.");
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
	const bool stored = has_binary_content ? file->store_buffer(bytes) : file->store_string(content);
	if (!stored) {
		return _error("FILE_WRITE_FAILED", "Failed to store file content.");
	}
	file.unref();

	EditorFileSystem *filesystem = Engine::get_singleton()->is_editor_hint() ? EditorFileSystem::get_singleton() : nullptr;
	if (filesystem && filesystem->get_filesystem() && !filesystem->is_scanning()) {
		filesystem->update_file(res_path);
	}

	// Post-write diagnostics: the file is committed either way, and the model
	// reads the exact parser output to self-correct. Reversal stays available
	// through the file checkpoint (history.revert).
	Dictionary validation_data;
	if (is_script_text) {
		validation_data = _validate_source(res_path, content);
	}

	Dictionary data;
	data["path"] = res_path;
	data["created"] = !existed_before;
	data["overwritten"] = existed_before;
	data["size_bytes"] = has_binary_content ? bytes.size() : content.utf8().length();
	data["sha256"] = FileAccess::get_sha256(res_path);
	data["binary"] = has_binary_content;
	data["import_valid"] = ResourceLoader::is_import_valid(res_path);
	if (is_script_text) {
		data["valid"] = validation_data.get("valid", true);
		data["validation"] = validation_data;
	}

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
	const int occurrence = p_args.get("occurrence", 1);

	if (old_text.is_empty()) {
		return _error("INVALID_ARGUMENT", "old_text is required for script.edit replace.");
	}
	if (occurrence < 1) {
		return _error("INVALID_ARGUMENT", "occurrence must be 1 or greater.");
	}

	String res_path;
	String path_error;
	if (!_normalize_project_path(path_arg, res_path, path_error)) {
		return _error("INVALID_PATH", path_error);
	}
	if (_solers_is_project_settings_path(res_path)) {
		return _error("EDITOR_OWNED_FILE", "Modify project.godot through project.edit settings so the live ProjectSettings state stays synchronized.");
	}
	if (!FileAccess::exists(res_path)) {
		return _error("FILE_NOT_FOUND", vformat("File does not exist: %s", res_path));
	}
	if (_solers_is_native_serialized_resource_path(res_path)) {
		return _error("NATIVE_RESOURCE_PATCH_BLOCKED", "Godot serialized resources must be edited through scene.edit or resource.edit, not text replacement.");
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

void SolersScriptService::_apply_project_settings(const Dictionary &p_values, const PackedStringArray &p_erase) {
	ProjectSettings *settings = ProjectSettings::get_singleton();
	if (!settings) {
		project_settings_save_error = ERR_UNAVAILABLE;
		return;
	}
	for (const String &key : p_erase) {
		settings->clear(key);
	}
	for (const Variant *key = p_values.next(nullptr); key; key = p_values.next(key)) {
		settings->set_setting(String(*key), p_values[*key]);
	}
	project_settings_save_error = settings->save();
}

Dictionary SolersScriptService::edit_project(const Dictionary &p_args) {
	const String operation = String(p_args.get("operation", String())).strip_edges();
	if (operation == "settings") {
		const Dictionary values = p_args.get("values", Dictionary());
		const PackedStringArray erase = p_args.get("erase", PackedStringArray());
		if (values.is_empty() && erase.is_empty()) {
			return _error("INVALID_ARGUMENT", "project.edit settings requires values or erase.");
		}
		ProjectSettings *settings = ProjectSettings::get_singleton();
		EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
		if (!settings || !undo_redo) {
			return _error("PROJECT_SETTINGS_UNAVAILABLE", "ProjectSettings or EditorUndoRedoManager is unavailable.", false);
		}
		Dictionary previous_values;
		PackedStringArray previous_missing;
		for (const Variant *key = values.next(nullptr); key; key = values.next(key)) {
			const String setting = String(*key).strip_edges();
			if (setting.is_empty()) {
				return _error("INVALID_SETTING", "Project setting names cannot be empty.");
			}
			if (settings->has_setting(setting)) {
				previous_values[setting] = settings->get(setting);
			} else {
				previous_missing.push_back(setting);
			}
		}
		for (const String &setting : erase) {
			if (setting.strip_edges().is_empty()) {
				return _error("INVALID_SETTING", "Project setting names cannot be empty.");
			}
			if (settings->has_setting(setting) && !previous_values.has(setting)) {
				previous_values[setting] = settings->get(setting);
			}
		}

		const int history_id = EditorNode::get_singleton() && EditorNode::get_editor_data().get_edited_scene_count() > 0 ? EditorNode::get_editor_data().get_current_edited_scene_history_id() : EditorUndoRedoManager::GLOBAL_HISTORY;
		undo_redo->create_action_for_history("Solers: Edit Project Settings", history_id, UndoRedo::MERGE_DISABLE, true);
		undo_redo->add_do_method(this, "_apply_project_settings", values, erase);
		undo_redo->add_undo_method(this, "_apply_project_settings", previous_values, previous_missing);
		project_settings_save_error = OK;
		undo_redo->commit_action();
		if (project_settings_save_error != OK) {
			return _error("PROJECT_SETTINGS_SAVE_FAILED", vformat("ProjectSettings::save failed with error %d.", project_settings_save_error));
		}
		Dictionary data;
		data["operation"] = operation;
		data["updated"] = values.keys();
		data["erased"] = erase;
		data["path"] = "res://project.godot";
		return _ok(data);
	}

	if (operation == "create_directory") {
		const String dir_arg = p_args.get("path", String());
		String res_dir;
		String dir_error;
		if (!_normalize_project_path(dir_arg, res_dir, dir_error)) {
			return _error("INVALID_PATH", dir_error);
		}
		const String global_dir = ProjectSettings::get_singleton()->globalize_path(res_dir);
		const bool existed = DirAccess::dir_exists_absolute(global_dir);
		if (!existed) {
			const Error dir_err = DirAccess::make_dir_recursive_absolute(global_dir);
			if (dir_err != OK) {
				return _error("DIRECTORY_CREATE_FAILED", vformat("Failed to create directory, error code %d.", dir_err));
			}
			EditorFileSystem *filesystem = Engine::get_singleton()->is_editor_hint() ? EditorFileSystem::get_singleton() : nullptr;
			if (filesystem && filesystem->get_filesystem() && !filesystem->is_scanning()) {
				filesystem->scan_changes();
			}
		}
		Dictionary data;
		data["operation"] = "create_directory";
		data["path"] = res_dir;
		data["created"] = !existed;
		data["existed"] = existed;
		if (action_timeline && !existed) {
			action_timeline->record_event("directory_created", data);
		}
		return _ok(data);
	}
	if (operation != "write_file") {
		return _error("INVALID_ARGUMENT", "project.edit operation must be settings, write_file, or create_directory.");
	}
	const String path = p_args.get("path", String());
	if (_solers_is_project_settings_path(path)) {
		return _error("EDITOR_OWNED_FILE", "Modify project.godot through project.edit settings.");
	}
	// Ownership boundaries come from the engine's own registries — script
	// languages, resource loaders, and the import pipeline — never from an
	// extension whitelist. Everything they do not claim is ordinary project
	// data (.gdignore, .editorconfig, custom text formats, ...).
	if (_solers_is_script_source_path(path)) {
		return _error("PROJECT_FILE_TYPE_BLOCKED", "Script sources are edited through script.edit so parser diagnostics stay attached to the write.");
	}
	if (_solers_is_native_serialized_resource_path(path)) {
		return _error("PROJECT_FILE_TYPE_BLOCKED", "Godot serialized scenes and resources are edited through scene.edit or resource.edit, not raw file writes.");
	}
	ResourceFormatImporter *format_importer = ResourceFormatImporter::get_singleton();
	if (format_importer && format_importer->get_importer_by_file(path).is_valid()) {
		return _error("PROJECT_FILE_TYPE_BLOCKED", "This file format is owned by Godot's import pipeline; bring media into the project through the asset tools.");
	}
	const bool exists = FileAccess::exists(path);
	const String expected_sha256 = p_args.get("expected_sha256", String());
	if (exists && !expected_sha256.is_empty() && FileAccess::get_sha256(path) != expected_sha256) {
		return _error("FILE_CHANGED", "File sha256 does not match expected_sha256.");
	}
	Dictionary write_args;
	write_args["path"] = path;
	write_args["content"] = p_args.get("content", String());
	write_args["create"] = true;
	write_args["overwrite"] = exists;
	return write_file(write_args);
}

Dictionary SolersScriptService::edit_script(const Dictionary &p_args) {
	const String operation = String(p_args.get("operation", String())).strip_edges();
	const String path = p_args.get("path", String());
	if (!_solers_is_script_source_path(path)) {
		return _error("SCRIPT_FILE_TYPE_REQUIRED", "script.edit accepts .gd, .cs, .gdshader, or .gdshaderinc files.");
	}
	if (operation == "create") {
		if (FileAccess::exists(path)) {
			return _error("FILE_EXISTS", "script.edit create never overwrites an existing script.");
		}
		Dictionary write_args;
		write_args["path"] = path;
		write_args["content"] = p_args.get("content", String());
		write_args["create"] = true;
		write_args["overwrite"] = false;
		return write_file(write_args);
	}
	if (operation == "replace") {
		Dictionary patch_args;
		patch_args["path"] = path;
		patch_args["old_text"] = p_args.get("old_text", String());
		patch_args["new_text"] = p_args.get("new_text", String());
		patch_args["occurrence"] = p_args.get("occurrence", 1);
		patch_args["expected_sha256"] = p_args.get("expected_sha256", String());
		return patch_file(patch_args);
	}
	return _error("INVALID_ARGUMENT", "script.edit operation must be create or replace.");
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

// Captures engine log output while a script.run entry point executes so the
// caller receives the same prints and errors a developer would see in the
// editor log. Registered once; the composite logger owns and frees it.
class SolersRunCaptureLogger : public ::Logger {
	static constexpr int MAX_CAPTURE_CHARS = 32768;

	void _append(String &r_target, const String &p_text) {
		if (r_target.length() >= MAX_CAPTURE_CHARS) {
			return;
		}
		r_target += p_text;
		if (r_target.length() > MAX_CAPTURE_CHARS) {
			r_target = r_target.substr(0, MAX_CAPTURE_CHARS) + "\n[capture truncated]";
		}
	}

public:
	bool active = false;
	String output;
	String errors;

	void begin() {
		output = String();
		errors = String();
		active = true;
	}

	void end() {
		active = false;
	}

	virtual void logv(const char *p_format, va_list p_list, bool p_err) override {
		if (!active) {
			return;
		}
		va_list list_copy;
		va_copy(list_copy, p_list);
		const int length = vsnprintf(nullptr, 0, p_format, list_copy);
		va_end(list_copy);
		if (length <= 0) {
			return;
		}
		Vector<char> buffer;
		buffer.resize(length + 1);
		vsnprintf(buffer.ptrw(), buffer.size(), p_format, p_list);
		_append(p_err ? errors : output, String::utf8(buffer.ptr(), length));
	}

	virtual void log_error(const char *p_function, const char *p_file, int p_line, const char *p_code, const char *p_rationale, bool p_editor_notify, ErrorType p_type, const Vector<Ref<ScriptBacktrace>> &p_script_backtraces) override {
		if (!active) {
			return;
		}
		const char *details = (p_rationale && p_rationale[0]) ? p_rationale : p_code;
		_append(errors, vformat("%s: %s (%s:%d)\n", String(error_type_string(p_type)), String::utf8(details), String::utf8(p_file), p_line));
	}
};

static SolersRunCaptureLogger *solers_run_capture_logger = nullptr;

// Converts a run() return value into data every provider can serialize:
// JSON-native types pass through, containers recurse with bounded depth and
// size, and engine types fall back to their human-readable string form.
static Variant _solers_run_jsonable(const Variant &p_value, int p_depth = 0) {
	switch (p_value.get_type()) {
		case Variant::NIL:
		case Variant::BOOL:
		case Variant::INT:
		case Variant::FLOAT:
		case Variant::STRING:
		case Variant::STRING_NAME:
			return p_value;
		case Variant::ARRAY: {
			if (p_depth >= 8) {
				return String(p_value);
			}
			const Array source_array = p_value;
			Array converted;
			const int limit = MIN(source_array.size(), 256);
			for (int i = 0; i < limit; i++) {
				converted.push_back(_solers_run_jsonable(source_array[i], p_depth + 1));
			}
			if (source_array.size() > limit) {
				converted.push_back(vformat("[%d more items omitted]", source_array.size() - limit));
			}
			return converted;
		}
		case Variant::DICTIONARY: {
			if (p_depth >= 8) {
				return String(p_value);
			}
			const Dictionary source_dict = p_value;
			Dictionary converted;
			for (const Variant *key = source_dict.next(nullptr); key; key = source_dict.next(key)) {
				converted[String(*key)] = _solers_run_jsonable(source_dict[*key], p_depth + 1);
			}
			return converted;
		}
		default:
			return String(p_value);
	}
}

Dictionary SolersScriptService::run_script(const Dictionary &p_args) {
	const String source = p_args.get("source", String());
	if (source.strip_edges().is_empty()) {
		return _error("INVALID_ARGUMENT", "source is required.");
	}

	const Dictionary validation = _validate_source("res://.solers/script_run.gd", source);
	if (validation.has("ok") && !(bool)validation.get("ok", true)) {
		return validation;
	}
	if (!(bool)validation.get("valid", true)) {
		Dictionary result = _error("SCRIPT_PARSE_FAILED", "The script does not parse; fix the reported diagnostics.");
		result["data"] = validation;
		return result;
	}

	Ref<Script> script = Object::cast_to<Script>(ClassDB::instantiate(SNAME("GDScript")));
	if (script.is_null()) {
		return _error("GDSCRIPT_UNAVAILABLE", "The GDScript language module is unavailable in this build.", false);
	}
	script->set_source_code(source);
	const Error compile_error = script->reload();
	if (compile_error != OK) {
		return _error("SCRIPT_COMPILE_FAILED", vformat("GDScript compilation failed (error %d).", (int)compile_error));
	}
	if (!script->is_tool()) {
		return _error("TOOL_ANNOTATION_REQUIRED", "Start the source with @tool so it can run inside the editor process.");
	}
	const StringName base_type = script->get_instance_base_type();
	if (!ClassDB::is_parent_class(base_type, SNAME("RefCounted"))) {
		return _error("REFCOUNTED_BASE_REQUIRED", vformat("script.run sources must extend a RefCounted type such as the default base or EditorScript; got %s.", String(base_type)));
	}
	Ref<RefCounted> instance = Object::cast_to<RefCounted>(ClassDB::instantiate(base_type));
	if (instance.is_null()) {
		return _error("SCRIPT_INSTANTIATION_FAILED", vformat("Could not instantiate script base type %s.", String(base_type)), false);
	}
	instance->set_script(script);
	const StringName entry = instance->has_method(SNAME("run")) ? SNAME("run") : (instance->has_method(SNAME("_run")) ? SNAME("_run") : StringName());
	if (entry == StringName()) {
		return _error("RUN_ENTRY_MISSING", "Define func run() (or EditorScript-style _run()) as the entry point.");
	}

	// The host is a temporary Node already inside the editor scene tree, so
	// scripts have a legitimate place for nodes that only initialize in-tree
	// (GDExtension terrain, physics helpers). Its lifetime is bound to this
	// tool call: whatever the script parents under it is freed with it, so a
	// failed script can never leak nodes into the editor UI tree.
	Node *host = nullptr;
	if (EditorNode::get_singleton()) {
		host = memnew(Node);
		host->set_name("SolersScriptRunHost");
		EditorNode::get_singleton()->add_child(host);
	}
	const ObjectID host_id = host ? host->get_instance_id() : ObjectID();
	const int64_t source_bytes = source.utf8().length();

	if (!solers_run_capture_logger) {
		solers_run_capture_logger = memnew(SolersRunCaptureLogger);
		OS::get_singleton()->add_logger(solers_run_capture_logger);
	}
	solers_run_capture_logger->begin();
	Callable::CallError call_error;
	Variant return_value;
	bool argument_count_valid = false;
	const int argument_count = instance->get_method_argument_count(entry, &argument_count_valid);
	if (host && argument_count_valid && argument_count >= 1) {
		Variant host_variant = host;
		const Variant *argptrs[1] = { &host_variant };
		return_value = instance->callp(entry, argptrs, 1, call_error);
	} else {
		return_value = instance->callp(entry, nullptr, 0, call_error);
	}

	if (call_error.error != Callable::CallError::CALL_OK) {
		solers_run_capture_logger->end();
		Dictionary data;
		data["entry"] = String(entry);
		data["output"] = solers_run_capture_logger->output;
		data["error_output"] = solers_run_capture_logger->errors;
		if (host) {
			host->queue_free();
		}
		Dictionary result = _error("SCRIPT_RUN_FAILED", vformat("Calling %s() failed: %s", String(entry), Variant::get_call_error_text(instance.ptr(), entry, nullptr, 0, call_error)));
		result["data"] = data;
		return result;
	}

	// An awaiting run() hands back a suspended coroutine instead of a result.
	// Keep the coroutine (and the script instance) alive and complete through
	// the registry's ready/poll continuation once its "completed" signal
	// fires, so awaited work is a first-class citizen rather than a leak.
	Object *state_object = return_value.get_type() == Variant::OBJECT ? (Object *)return_value : nullptr;
	if (state_object && state_object->get_class() == SNAME("GDScriptFunctionState")) {
		const int64_t run_id = next_run_id++;
		PendingRun pending;
		pending.instance = instance;
		pending.state = Ref<RefCounted>(Object::cast_to<RefCounted>(state_object));
		pending.host_id = host_id;
		pending.entry = String(entry);
		pending.source_bytes = source_bytes;
		pending_runs.insert(run_id, pending);
		state_object->connect(SNAME("completed"), callable_mp(this, &SolersScriptService::_on_run_completed).bind(run_id), Object::CONNECT_ONE_SHOT);
		Dictionary poll_args;
		poll_args["run_id"] = run_id;
		poll_args["deadline_msec"] = (int64_t)(OS::get_singleton()->get_ticks_msec() + 30000);
		Dictionary data;
		data["status"] = "pending";
		data["poll_args"] = poll_args;
		return _ok(data); // The capture logger stays active until finalize.
	}

	solers_run_capture_logger->end();
	return _finish_run(String(entry), source_bytes, return_value, host_id);
}

void SolersScriptService::_on_run_completed(const Variant &p_result, int64_t p_run_id) {
	PendingRun *pending = pending_runs.getptr(p_run_id);
	if (pending) {
		pending->completed = true;
		pending->result = p_result;
	}
}

bool SolersScriptService::run_script_ready(const Dictionary &p_args) const {
	const int64_t run_id = p_args.get("run_id", 0);
	const PendingRun *pending = pending_runs.getptr(run_id);
	if (!pending || pending->completed) {
		return true;
	}
	return OS::get_singleton()->get_ticks_msec() >= (uint64_t)(int64_t)p_args.get("deadline_msec", 0);
}

Dictionary SolersScriptService::run_script_finalize(const Dictionary &p_args) {
	const int64_t run_id = p_args.get("run_id", 0);
	if (!pending_runs.has(run_id)) {
		return _error("RUN_STATE_MISSING", "The awaited script state is gone; run the script again.", false);
	}
	PendingRun pending = pending_runs[run_id];
	pending_runs.erase(run_id);
	if (solers_run_capture_logger) {
		solers_run_capture_logger->end();
	}
	if (!pending.completed) {
		// Dropping the coroutine and instance references abandons the
		// suspended run(); freeing the host clears its temporary nodes.
		Dictionary finished = _finish_run(pending.entry, pending.source_bytes, Variant(), pending.host_id);
		Dictionary result = _error("SCRIPT_RUN_TIMEOUT", "run() was still awaiting after 30 seconds; the coroutine was abandoned and its host node freed.");
		result["data"] = finished.get("data", Dictionary());
		return result;
	}
	return _finish_run(pending.entry, pending.source_bytes, pending.result, pending.host_id);
}

Dictionary SolersScriptService::_finish_run(const String &p_entry, int64_t p_source_bytes, const Variant &p_return_value, ObjectID p_host_id) {
	Dictionary data;
	data["entry"] = p_entry;
	data["output"] = solers_run_capture_logger ? solers_run_capture_logger->output : String();
	data["error_output"] = solers_run_capture_logger ? solers_run_capture_logger->errors : String();
	data["result"] = _solers_run_jsonable(p_return_value);
	Node *host = Object::cast_to<Node>(ObjectDB::get_instance(p_host_id));
	if (host) {
		data["host_children_freed"] = host->get_child_count();
		host->queue_free();
	}
	if (action_timeline) {
		Dictionary event;
		event["entry"] = p_entry;
		event["source_bytes"] = p_source_bytes;
		action_timeline->record_event("script_run", event);
	}
	return _ok(data);
}
