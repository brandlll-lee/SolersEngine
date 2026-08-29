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
#include "core/io/json.h"
#include "core/io/resource_format_binary.h"
#include "core/io/resource_importer.h"
#include "core/io/resource_loader.h"
#include "core/object/class_db.h"
#include "core/object/script_language.h"
#include "core/os/os.h"
#include "editor/editor_node.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/file_system/editor_file_system.h"
#include "scene/main/node.h"
#include "scene/resources/resource_format_text.h"
#include "scene/resources/shader.h"
#include "servers/rendering/shader_language.h"
#include "servers/rendering/shader_preprocessor.h"
#include "servers/rendering/shader_types.h"

#include "modules/solers_ai/core/solers_action_timeline.h"
#include "modules/solers_ai/core/solers_resource_service.h"

void SolersScriptService::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_action_timeline", "action_timeline"), &SolersScriptService::set_action_timeline);
	ClassDB::bind_method(D_METHOD("write_file", "args"), &SolersScriptService::write_file);
	ClassDB::bind_method(D_METHOD("patch_file", "args"), &SolersScriptService::patch_file);
	ClassDB::bind_method(D_METHOD("edit_project", "args"), &SolersScriptService::edit_project);
	ClassDB::bind_method(D_METHOD("edit_script", "args"), &SolersScriptService::edit_script);
	ClassDB::bind_method(D_METHOD("validate_script", "args"), &SolersScriptService::validate_script);
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
				compile_info.functions = types->get_functions(RSE::ShaderMode(mode));
				compile_info.render_modes = types->get_modes(RSE::ShaderMode(mode));
				compile_info.stencil_modes = types->get_stencil_modes(RSE::ShaderMode(mode));
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
	const String expected_sha256 = p_args.get("expected_sha256", String());
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
		return _error("EDITOR_OWNED_FILE", "Modify project.godot through project.settings so live ProjectSettings stays synchronized.");
	}
	if (_solers_is_native_serialized_resource_path(res_path)) {
		return _error("NATIVE_RESOURCE_WRITE_BLOCKED", "Godot serialized resources must be edited through resource.create or resource.update, not raw file writes.");
	}

	const bool existed_before = FileAccess::exists(res_path);
	if (existed_before && !overwrite) {
		return _error("FILE_EXISTS", vformat("File already exists and overwrite=false: %s", res_path));
	}
	if (!existed_before && !create) {
		return _error("FILE_NOT_FOUND", vformat("File does not exist and create=false: %s", res_path));
	}
	if (!expected_sha256.is_empty() && (!existed_before || FileAccess::get_sha256(res_path) != expected_sha256)) {
		Dictionary failure = _error("STATE_CONFLICT", vformat("File '%s' changed since it was inspected.", res_path));
		failure["data"] = Dictionary({ { "path", res_path }, { "expected_sha256", expected_sha256 }, { "actual_sha256", existed_before ? FileAccess::get_sha256(res_path) : String() } });
		return failure;
	}
	const bool is_script_text = has_text_content && _solers_is_script_source_path(res_path);
	Dictionary validation_data;
	if (is_script_text) {
		validation_data = _validate_source(res_path, content);
		if (!(bool)validation_data.get("valid", false)) {
			Dictionary failure = _error("SCRIPT_VALIDATION_FAILED", "The proposed source is invalid; the file was not changed.");
			failure["data"] = validation_data;
			return failure;
		}
	}

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

	const String absolute_path = ProjectSettings::get_singleton()->globalize_path(res_path);
	const String temporary_path = absolute_path + ".solers-" + String::num_uint64(OS::get_singleton()->get_ticks_usec()) + ".tmp";
	Error write_err = OK;
	Ref<FileAccess> file = FileAccess::open(temporary_path, FileAccess::WRITE, &write_err);
	if (file.is_null() || write_err != OK) {
		return _error("FILE_WRITE_FAILED", vformat("Failed to open temporary file for writing, error code %d.", write_err));
	}
	const bool stored = has_binary_content ? file->store_buffer(bytes) : file->store_string(content);
	if (!stored) {
		file.unref();
		DirAccess::remove_absolute(temporary_path);
		return _error("FILE_WRITE_FAILED", "Failed to store file content.");
	}
	file.unref();
	if (!expected_sha256.is_empty() && FileAccess::get_sha256(res_path) != expected_sha256) {
		DirAccess::remove_absolute(temporary_path);
		Dictionary failure = _error("STATE_CONFLICT", vformat("File '%s' changed while the edit was being validated.", res_path));
		failure["data"] = Dictionary({ { "path", res_path }, { "expected_sha256", expected_sha256 }, { "actual_sha256", FileAccess::get_sha256(res_path) } });
		return failure;
	}
	const Error replace_err = DirAccess::rename_absolute(temporary_path, absolute_path);
	if (replace_err != OK) {
		DirAccess::remove_absolute(temporary_path);
		return _error("FILE_WRITE_FAILED", vformat("Failed to replace the target file, error code %d.", replace_err));
	}

	EditorFileSystem *filesystem = Engine::get_singleton()->is_editor_hint() ? EditorFileSystem::get_singleton() : nullptr;
	if (filesystem && filesystem->get_filesystem()) {
		filesystem->update_file(res_path);
	}

	Dictionary data;
	data["path"] = res_path;
	data["created"] = !existed_before;
	data["overwritten"] = existed_before;
	data["size_bytes"] = has_binary_content ? bytes.size() : content.utf8().length();
	data["sha256"] = FileAccess::get_sha256(res_path);
	data["binary"] = has_binary_content;
	data["filesystem_synced"] = filesystem && filesystem->get_filesystem();
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

	if (old_text.is_empty() || expected_sha256.is_empty() || !p_args.has("new_text")) {
		return _error("INVALID_ARGUMENT", "old_text, new_text, and expected_sha256 are required for script.edit replace.");
	}

	String res_path;
	String path_error;
	if (!_normalize_project_path(path_arg, res_path, path_error)) {
		return _error("INVALID_PATH", path_error);
	}
	if (_solers_is_project_settings_path(res_path)) {
		return _error("EDITOR_OWNED_FILE", "Modify project.godot through project.settings so live ProjectSettings stays synchronized.");
	}
	if (!FileAccess::exists(res_path)) {
		return _error("FILE_NOT_FOUND", vformat("File does not exist: %s.%s", res_path, solers_file_suggestions(res_path)));
	}
	if (_solers_is_native_serialized_resource_path(res_path)) {
		return _error("NATIVE_RESOURCE_PATCH_BLOCKED", "Godot serialized resources must be edited through resource.create or resource.update, not text replacement.");
	}

	Error read_err = OK;
	String content = FileAccess::get_file_as_string(res_path, &read_err);
	if (read_err != OK) {
		return _error("FILE_READ_FAILED", vformat("Failed to read file, error code %d.", read_err));
	}
	const String actual_sha256 = FileAccess::get_sha256(res_path);
	if (actual_sha256 != expected_sha256) {
		Dictionary failure = _error("STATE_CONFLICT", vformat("File '%s' changed since it was inspected.", res_path));
		failure["data"] = Dictionary({ { "path", res_path }, { "expected_sha256", expected_sha256 }, { "actual_sha256", actual_sha256 } });
		return failure;
	}
	const int offset = content.find(old_text);
	if (offset < 0) {
		return _error("PATCH_TEXT_NOT_FOUND", "old_text was not found byte-for-byte in the authoritative file. Re-read the file and copy the exact current text.");
	}
	if (content.find(old_text, offset + old_text.length()) >= 0) {
		return _error("PATCH_TEXT_AMBIGUOUS", "old_text occurs more than once in the authoritative file. Include enough exact surrounding text to identify one block.");
	}
	const String patched = content.substr(0, offset) + new_text + content.substr(offset + old_text.length());
	Dictionary write_args;
	write_args["path"] = res_path;
	write_args["content"] = patched;
	write_args["create"] = false;
	write_args["overwrite"] = true;
	write_args["expected_sha256"] = expected_sha256;
	Dictionary write_result = write_file(write_args);
	if (!(bool)write_result.get("ok", false)) {
		return write_result;
	}

	Dictionary data = write_result.get("data", Dictionary());
	data["patched_offset"] = offset;
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
		const Dictionary wire_values = p_args.get("values", Dictionary());
		const PackedStringArray erase = p_args.get("erase", PackedStringArray());
		if (wire_values.is_empty() && erase.is_empty()) {
			return _error("INVALID_ARGUMENT", "project.settings requires values or erase.");
		}
		ProjectSettings *settings = ProjectSettings::get_singleton();
		EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
		if (!settings || !undo_redo) {
			return _error("PROJECT_SETTINGS_UNAVAILABLE", "ProjectSettings or EditorUndoRedoManager is unavailable.", false);
		}
		Dictionary values;
		Dictionary previous_values;
		PackedStringArray previous_missing;
		for (const Variant *key = wire_values.next(nullptr); key; key = wire_values.next(key)) {
			const String setting = String(*key).strip_edges();
			if (setting.is_empty()) {
				return _error("INVALID_SETTING", "Project setting names cannot be empty.");
			}
			Variant value;
			String decode_error;
			if (!solers_decode_wire_variant(wire_values[*key], value, decode_error)) {
				return _error("INVALID_SETTING_VALUE", vformat("Project setting '%s' is invalid: %s", setting, decode_error));
			}
			values[setting] = value;
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
		return _error("INVALID_ARGUMENT", "Unsupported project path operation.");
	}
	const String path = p_args.get("path", String());
	if (_solers_is_project_settings_path(path)) {
		return _error("EDITOR_OWNED_FILE", "Modify project.godot through project.settings.");
	}
	// Ownership boundaries come from the engine's own registries — script
	// languages, resource loaders, and the import pipeline — never from an
	// extension whitelist. Everything they do not claim is ordinary project
	// data (.gdignore, .editorconfig, custom text formats, ...).
	if (_solers_is_script_source_path(path)) {
		return _error("PROJECT_FILE_TYPE_BLOCKED", "Script sources are edited through script.edit so parser diagnostics stay attached to the write.");
	}
	if (_solers_is_native_serialized_resource_path(path)) {
		return _error("PROJECT_FILE_TYPE_BLOCKED", "Godot serialized scenes and resources are edited through the applicable scene or resource tool, not raw file writes.");
	}
	ResourceFormatImporter *format_importer = ResourceFormatImporter::get_singleton();
	if (format_importer && format_importer->get_importer_by_file(path).is_valid()) {
		return _error("PROJECT_FILE_TYPE_BLOCKED", "This file format is owned by Godot's import pipeline; bring media into the project through the asset tools.");
	}
	const bool exists = FileAccess::exists(path);
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
			return _error("FILE_NOT_FOUND", vformat("Script file does not exist: %s.%s", res_path, solers_file_suggestions(res_path)));
		}
		Error read_err = OK;
		source = FileAccess::get_file_as_string(res_path, &read_err);
		if (read_err != OK) {
			return _error("FILE_READ_FAILED", vformat("Failed to read script, error code %d.", read_err));
		}
	}

	return _ok(_validate_source(res_path, source));
}
