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
#include "scene/main/node.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/file_system/editor_file_system.h"
#include "modules/solers_ai/core/solers_action_timeline.h"
#include "modules/solers_ai/core/solers_resource_service.h"
#include "scene/resources/resource_format_text.h"
#include "scene/resources/shader.h"
#include "servers/rendering/shader_language.h"
#include "servers/rendering/shader_preprocessor.h"
#include "servers/rendering/shader_types.h"

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
		return _error("NATIVE_RESOURCE_WRITE_BLOCKED", "Godot serialized resources must be edited through object.transaction, not raw file writes.");
	}

	const bool existed_before = FileAccess::exists(res_path);
	if (existed_before && !overwrite) {
		return _error("FILE_EXISTS", vformat("File already exists and overwrite=false: %s", res_path));
	}
	if (!existed_before && !create) {
		return _error("FILE_NOT_FOUND", vformat("File does not exist and create=false: %s", res_path));
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

// --- Tolerant old_text location ----------------------------------------------
// The current file content is the only authoritative match target. The model's
// old_text routinely drifts from it in ways that never change meaning for
// source code: trailing whitespace, indentation depth, collapsed spacing, and
// typographic lookalike characters. Strategies run strictest-first over the
// ORIGINAL content and the first one that matches uniquely decides; ambiguity
// is an error, never a guess. This mirrors opencode's replacer cascade and
// codex's seek_sequence.

static char32_t _solers_ascii_fold(char32_t p_char) {
	switch (p_char) {
		case 0x2018: // ' variants
		case 0x2019:
		case 0x201A:
		case 0x201B:
		case 0x2032:
			return '\'';
		case 0x201C: // " variants
		case 0x201D:
		case 0x201E:
		case 0x201F:
		case 0x2033:
			return '"';
		case 0x2010: // dash variants
		case 0x2011:
		case 0x2012:
		case 0x2013:
		case 0x2014:
		case 0x2015:
		case 0x2212:
			return '-';
		case 0x00A0: // space variants
		case 0x2002:
		case 0x2003:
		case 0x2004:
		case 0x2005:
		case 0x2006:
		case 0x2007:
		case 0x2008:
		case 0x2009:
		case 0x200A:
		case 0x202F:
		case 0x3000:
			return ' ';
		default:
			return p_char;
	}
}

static String _solers_fold_typography(const String &p_text) {
	String out = p_text;
	char32_t *ptr = out.ptrw();
	for (int i = 0; i < out.length(); i++) {
		ptr[i] = _solers_ascii_fold(ptr[i]);
	}
	return out;
}

// Edge trim + typography fold; interior whitespace runs optionally collapse.
static String _solers_line_key(const String &p_line, bool p_collapse_interior) {
	const String folded = _solers_fold_typography(p_line.strip_edges());
	if (!p_collapse_interior) {
		return folded;
	}
	String out;
	bool in_whitespace = false;
	for (int i = 0; i < folded.length(); i++) {
		const char32_t c = folded[i];
		if (c == ' ' || c == '\t') {
			in_whitespace = true;
			continue;
		}
		if (in_whitespace && !out.is_empty()) {
			out += ' ';
		}
		in_whitespace = false;
		out += c;
	}
	return out;
}

struct SolersSourceLine {
	int start = 0; // first character offset in content
	int end = 0; // offset past this line's newline (== next line's start)
	int text_end = 0; // offset past the last character before the newline
	String text; // without the trailing newline, without trailing '\r'
};

static Vector<SolersSourceLine> _solers_split_lines(const String &p_text) {
	Vector<SolersSourceLine> lines;
	int start = 0;
	while (start <= p_text.length()) {
		const int newline = p_text.find_char('\n', start);
		SolersSourceLine line;
		line.start = start;
		if (newline == -1) {
			line.text_end = p_text.length();
			line.end = p_text.length();
		} else {
			line.text_end = newline;
			line.end = newline + 1;
		}
		line.text = p_text.substr(line.start, line.text_end - line.start).trim_suffix("\r");
		lines.push_back(line);
		if (newline == -1) {
			break;
		}
		start = newline + 1;
	}
	return lines;
}

static String _solers_leading_whitespace(const String &p_line) {
	int indent = 0;
	while (indent < p_line.length() && (p_line[indent] == ' ' || p_line[indent] == '\t')) {
		indent++;
	}
	return p_line.substr(0, indent);
}

struct SolersPatchMatch {
	int start = -1;
	int end = -1;
	int match_count = 0;
	String strategy;
	// First matched content line's indentation, for re-indenting new_text
	// when the anchor tolerated an indentation shift.
	String matched_indent;
	String needle_indent;
};

static SolersPatchMatch _solers_locate_old_text(const String &p_content, const String &p_old_text, int p_occurrence) {
	SolersPatchMatch match;

	// Strategy 1: exact substring; the only strategy where an explicit
	// occurrence index is meaningful.
	{
		int found = -1;
		int count = 0;
		int pos = p_content.find(p_old_text);
		while (pos != -1) {
			count++;
			if (count == p_occurrence) {
				found = pos;
			}
			pos = p_content.find(p_old_text, pos + MAX(1, (int)p_old_text.length()));
		}
		if (found != -1) {
			match.start = found;
			match.end = found + p_old_text.length();
			match.match_count = count;
			match.strategy = "exact";
			return match;
		}
		if (count > 0) {
			// The text exists but the requested occurrence does not; tolerant
			// tiers must not silently retarget a different occurrence.
			match.match_count = count;
			match.strategy = "exact";
			return match;
		}
	}
	if (p_occurrence != 1) {
		return match;
	}

	const Vector<SolersSourceLine> content_lines = _solers_split_lines(p_content);
	const bool needle_ends_with_newline = p_old_text.ends_with("\n");
	const Vector<SolersSourceLine> needle_lines = _solers_split_lines(needle_ends_with_newline ? p_old_text.substr(0, p_old_text.length() - 1) : p_old_text);
	if (needle_lines.is_empty() || content_lines.size() < needle_lines.size()) {
		return match;
	}

	// Strategy 2: per-line edge trim absorbs indentation and edge-whitespace
	// drift. Strategy 3 additionally collapses interior whitespace runs.
	for (int tier = 0; tier < 2; tier++) {
		const bool collapse_interior = tier == 1;
		Vector<String> needle_keys;
		for (const SolersSourceLine &line : needle_lines) {
			needle_keys.push_back(_solers_line_key(line.text, collapse_interior));
		}
		int found_window = -1;
		int window_count = 0;
		for (int i = 0; i + needle_keys.size() <= content_lines.size(); i++) {
			bool matched = true;
			for (int j = 0; j < needle_keys.size(); j++) {
				if (_solers_line_key(content_lines[i + j].text, collapse_interior) != needle_keys[j]) {
					matched = false;
					break;
				}
			}
			if (matched) {
				window_count++;
				if (found_window == -1) {
					found_window = i;
				}
			}
		}
		if (window_count == 0) {
			continue;
		}
		match.match_count = window_count;
		match.strategy = collapse_interior ? "whitespace_normalized" : "line_trimmed";
		if (window_count == 1) {
			const SolersSourceLine &last = content_lines[found_window + needle_keys.size() - 1];
			match.start = content_lines[found_window].start;
			match.end = needle_ends_with_newline ? last.end : last.text_end;
			match.matched_indent = _solers_leading_whitespace(content_lines[found_window].text);
			match.needle_indent = _solers_leading_whitespace(needle_lines[0].text);
		}
		return match;
	}
	return match;
}

// When a tolerant tier anchored the block at a different indentation depth,
// shift new_text by the same offset so the replacement sits at the file's
// real depth (GDScript is indentation-sensitive).
static String _solers_reindent_replacement(const String &p_new_text, const String &p_needle_indent, const String &p_matched_indent) {
	if (p_needle_indent == p_matched_indent) {
		return p_new_text;
	}
	const Vector<SolersSourceLine> lines = _solers_split_lines(p_new_text);
	String out;
	for (int i = 0; i < lines.size(); i++) {
		String text = p_new_text.substr(lines[i].start, lines[i].text_end - lines[i].start);
		if (text.begins_with(p_needle_indent)) {
			text = p_matched_indent + text.substr(p_needle_indent.length());
		}
		out += text;
		if (lines[i].end != lines[i].text_end) {
			out += "\n";
		}
	}
	return out;
}

// The lines of p_content around the first line whose trimmed text equals the
// needle's first non-empty trimmed line; gives the model real bytes to anchor
// its next attempt instead of guessing.
static String _solers_closest_context(const String &p_content, const String &p_old_text, int p_context_lines = 8) {
	String anchor;
	for (const SolersSourceLine &line : _solers_split_lines(p_old_text)) {
		anchor = _solers_fold_typography(line.text.strip_edges());
		if (!anchor.is_empty()) {
			break;
		}
	}
	if (anchor.is_empty()) {
		return String();
	}
	const Vector<SolersSourceLine> lines = _solers_split_lines(p_content);
	for (int i = 0; i < lines.size(); i++) {
		if (_solers_fold_typography(lines[i].text.strip_edges()) == anchor) {
			String out;
			for (int j = i; j < MIN((int)lines.size(), i + p_context_lines); j++) {
				out += lines[j].text + "\n";
			}
			return out;
		}
	}
	return String();
}

Dictionary SolersScriptService::patch_file(const Dictionary &p_args) {
	const String path_arg = p_args.get("path", String());
	const String old_text = p_args.get("old_text", String());
	const String new_text = p_args.get("new_text", String());
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
		return _error("FILE_NOT_FOUND", vformat("File does not exist: %s.%s", res_path, solers_file_suggestions(res_path)));
	}
	if (_solers_is_native_serialized_resource_path(res_path)) {
		return _error("NATIVE_RESOURCE_PATCH_BLOCKED", "Godot serialized resources must be edited through object.transaction, not text replacement.");
	}

	Error read_err = OK;
	String content = FileAccess::get_file_as_string(res_path, &read_err);
	if (read_err != OK) {
		return _error("FILE_READ_FAILED", vformat("Failed to read file, error code %d.", read_err));
	}

	const SolersPatchMatch match = _solers_locate_old_text(content, old_text, occurrence);
	if (match.start == -1) {
		if (match.strategy == "exact" && match.match_count >= 1) {
			return _error("PATCH_TEXT_NOT_FOUND", vformat("old_text occurrence %d was requested but only %d exact occurrence(s) exist in %s.", occurrence, match.match_count, res_path));
		}
		if (match.match_count > 1) {
			return _error("PATCH_TEXT_AMBIGUOUS", vformat("old_text matches %d places in %s (via %s matching). Include more surrounding lines so the target is unique.", match.match_count, res_path, match.strategy));
		}
		Dictionary failure = _error("PATCH_TEXT_NOT_FOUND", "old_text was not found in the current file content, even with whitespace-tolerant matching. The file's current bytes are authoritative; re-read the region and copy it exactly.");
		const String closest = _solers_closest_context(content, old_text);
		if (!closest.is_empty()) {
			Dictionary failure_data;
			failure_data["closest_context"] = closest;
			failure["data"] = failure_data;
		}
		return failure;
	}

	const String replacement = match.strategy == "exact" ? new_text : _solers_reindent_replacement(new_text, match.needle_indent, match.matched_indent);
	const String patched = content.substr(0, match.start) + replacement + content.substr(match.end);
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
	data["match_strategy"] = match.strategy;
	data["patched_offset"] = match.start;
	// The patched region as it now exists on disk (plus three lines each
	// side): the model anchors follow-up edits on real bytes instead of its
	// memory of what it sent.
	{
		const Vector<SolersSourceLine> patched_lines = _solers_split_lines(patched);
		int first_line = 0;
		int last_line = 0;
		const int new_end = match.start + replacement.length();
		for (int i = 0; i < patched_lines.size(); i++) {
			if (patched_lines[i].start <= match.start) {
				first_line = i;
			}
			if (patched_lines[i].start <= new_end) {
				last_line = i;
			}
		}
		String context_after;
		for (int i = MAX(0, first_line - 3); i <= MIN((int)patched_lines.size() - 1, last_line + 3); i++) {
			context_after += patched_lines[i].text + "\n";
		}
		data["context_after"] = context_after;
	}
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
		return _error("PROJECT_FILE_TYPE_BLOCKED", "Godot serialized scenes and resources are edited through object.transaction, not raw file writes.");
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
		patch_args["occurrence"] = p_args.get("occurrence", 1);
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

void SolersScriptService::_cleanup_compute(PendingCompute &r_compute) {
	if (r_compute.process_id && OS::get_singleton()->is_process_running(r_compute.process_id)) {
		OS::get_singleton()->kill(r_compute.process_id);
	}
	Ref<DirAccess> directory = DirAccess::open(r_compute.directory);
	if (directory.is_valid()) {
		directory->erase_contents_recursive();
	}
	DirAccess::remove_absolute(r_compute.directory);
	r_compute.process_id = 0;
}

SolersScriptService::~SolersScriptService() {
	for (KeyValue<String, PendingCompute> &entry : pending_computes) {
		_cleanup_compute(entry.value);
	}
}

Dictionary SolersScriptService::compute_script(const String &p_call_id, const Dictionary &p_args) {
	const String source = p_args.get("source", String());
	const Array requested_outputs = p_args.get("outputs", Array());
	if (p_call_id.is_empty() || source.strip_edges().is_empty() || requested_outputs.is_empty()) {
		return _error("INVALID_ARGUMENT", "script.compute requires source and at least one declared output.");
	}
	if (pending_computes.has(p_call_id)) {
		return _error("COMPUTE_ALREADY_RUNNING", "This script.compute call is already running.", false);
	}

	const Dictionary validation = _validate_source("res://compute.gd", source);
	if (!(bool)validation.get("valid", false)) {
		Dictionary failure = _error("SCRIPT_PARSE_FAILED", "The compute script does not parse.");
		failure["data"] = validation;
		return failure;
	}

	Array outputs;
	for (int i = 0; i < requested_outputs.size(); i++) {
		const Dictionary requested = requested_outputs[i];
		const String from = String(requested.get("from", String())).replace_char('\\', '/').simplify_path();
		String to;
		String path_error;
		if (from.is_empty() || from.is_absolute_path() || from.begins_with("res://") || from.contains("..")) {
			return _error("INVALID_OUTPUT_SOURCE", "Compute output sources must be relative paths inside the isolated project.");
		}
		if (!_normalize_project_path(requested.get("to", String()), to, path_error) || _solers_is_project_settings_path(to)) {
			return _error("INVALID_OUTPUT_TARGET", path_error.is_empty() ? "Use project.edit for project.godot." : path_error);
		}
		Dictionary output;
		output["from"] = from;
		output["to"] = to;
		output["resource_type"] = String(requested.get("resource_type", String())).strip_edges();
		outputs.push_back(output);
	}

	PendingCompute compute;
	compute.directory = OS::get_singleton()->get_temp_path().path_join("solers-compute-" + p_call_id.sha256_text().left(16));
	compute.outputs = outputs;
	Ref<DirAccess> stale = DirAccess::open(compute.directory);
	if (stale.is_valid()) {
		stale->erase_contents_recursive();
	}
	DirAccess::remove_absolute(compute.directory);
	if (DirAccess::make_dir_recursive_absolute(compute.directory) != OK) {
		return _error("COMPUTE_DIRECTORY_FAILED", "Unable to create the isolated compute project.", false);
	}

	auto write_text = [&](const String &p_path, const String &p_text) -> Error {
		Error error = OK;
		Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE, &error);
		if (file.is_null() || error != OK || !file->store_string(p_text)) {
			return error == OK ? ERR_FILE_CANT_WRITE : error;
		}
		return OK;
	};
	if (write_text(compute.directory.path_join("project.godot"), "[application]\nconfig/name=\"Solers Compute\"\n") != OK ||
			write_text(compute.directory.path_join("compute.gd"), source) != OK) {
		_cleanup_compute(compute);
		return _error("COMPUTE_SETUP_FAILED", "Unable to write the isolated compute project.", false);
	}

	List<String> arguments;
	arguments.push_back("--headless");
	arguments.push_back("--path");
	arguments.push_back(compute.directory);
	arguments.push_back("--script");
	arguments.push_back("res://compute.gd");
	const Error start_error = OS::get_singleton()->create_process(OS::get_singleton()->get_executable_path(), arguments, &compute.process_id);
	if (start_error != OK) {
		_cleanup_compute(compute);
		return _error("COMPUTE_START_FAILED", vformat("Unable to start isolated Godot (error %d).", start_error), false);
	}
	pending_computes[p_call_id] = compute;

	Dictionary data;
	data["status"] = "pending";
	data["poll_args"] = Dictionary();
	return _ok(data);
}

bool SolersScriptService::compute_script_ready(const String &p_call_id) const {
	const PendingCompute *compute = pending_computes.getptr(p_call_id);
	return !compute || !OS::get_singleton()->is_process_running(compute->process_id);
}

Dictionary SolersScriptService::compute_script_finalize(const String &p_call_id) {
	PendingCompute *stored = pending_computes.getptr(p_call_id);
	if (!stored) {
		return _error("COMPUTE_STATE_MISSING", "The isolated compute state is gone.", false);
	}
	if (OS::get_singleton()->is_process_running(stored->process_id)) {
		Dictionary data;
		data["status"] = "pending";
		data["poll_args"] = Dictionary();
		return _ok(data);
	}

	PendingCompute compute = *stored;
	pending_computes.erase(p_call_id);
	const int exit_code = OS::get_singleton()->get_process_exit_code(compute.process_id);
	if (exit_code != 0) {
		_cleanup_compute(compute);
		return _error("COMPUTE_FAILED", vformat("Isolated Godot exited with code %d.", exit_code));
	}

	EditorUndoRedoManager *undo_manager = EditorUndoRedoManager::get_singleton();
	Node *edited_root = EditorNode::get_singleton() ? EditorNode::get_editor_data().get_edited_scene_root() : nullptr;
	const int history_id = edited_root ? EditorNode::get_editor_data().get_current_edited_scene_history_id() : EditorUndoRedoManager::INVALID_HISTORY;
	Array committed;
	PackedStringArray changed_paths;
	for (int i = 0; i < compute.outputs.size(); i++) {
		const Dictionary output = compute.outputs[i];
		const String source_path = compute.directory.path_join(output.get("from", String()));
		const String target_path = output.get("to", String());
		if (!FileAccess::exists(source_path)) {
			_cleanup_compute(compute);
			return _error("COMPUTE_OUTPUT_MISSING", vformat("Declared output was not produced: %s.", output.get("from", String())));
		}
		if (edited_root && edited_root->get_scene_file_path() == target_path && undo_manager && undo_manager->is_history_unsaved(history_id)) {
			_cleanup_compute(compute);
			return _error("SCENE_UNSAVED", "Refusing to replace an open scene while its native UndoRedo history is unsaved.");
		}
		const String target_absolute = ProjectSettings::get_singleton()->globalize_path(target_path);
		if (DirAccess::make_dir_recursive_absolute(target_absolute.get_base_dir()) != OK) {
			_cleanup_compute(compute);
			return _error("OUTPUT_DIRECTORY_FAILED", vformat("Unable to create the parent directory for %s.", target_path), false);
		}
		const String temporary = target_absolute + ".solers-" + String::num_uint64(OS::get_singleton()->get_ticks_usec()) + ".tmp";
		const Error copy_error = DirAccess::copy_absolute(source_path, temporary);
		const Error replace_error = copy_error == OK ? DirAccess::rename_absolute(temporary, target_absolute) : copy_error;
		if (replace_error != OK) {
			DirAccess::remove_absolute(temporary);
			_cleanup_compute(compute);
			return _error("OUTPUT_COMMIT_FAILED", vformat("Unable to atomically commit %s (error %d).", target_path, replace_error), false);
		}
		const String source_sha = FileAccess::get_sha256(source_path);
		const String saved_sha = FileAccess::get_sha256(target_path);
		const String resource_type = output.get("resource_type", String());
		Error load_error = OK;
		const Ref<Resource> loaded = resource_type.is_empty() ? Ref<Resource>() : ResourceLoader::load(target_path, resource_type, ResourceFormatLoader::CACHE_MODE_IGNORE_DEEP, &load_error);
		if (source_sha != saved_sha || (!resource_type.is_empty() && (loaded.is_null() || load_error != OK || !loaded->is_class(resource_type)))) {
			DirAccess::remove_absolute(target_absolute);
			_cleanup_compute(compute);
			return _error("OUTPUT_VERIFY_FAILED", vformat("Committed output did not verify as %s.", resource_type.is_empty() ? String("bytes") : resource_type), false);
		}
		Dictionary receipt;
		receipt["path"] = target_path;
		receipt["sha256"] = saved_sha;
		committed.push_back(receipt);
		changed_paths.push_back(target_path);
	}

	Dictionary data;
	data["outputs"] = committed;
	const String result_path = compute.directory.path_join("result.json");
	if (FileAccess::exists(result_path)) {
		const Variant parsed = JSON::parse_string(FileAccess::get_file_as_string(result_path));
		if (parsed.get_type() != Variant::NIL) {
			data["result"] = parsed;
		}
	}
	EditorFileSystem *filesystem = EditorFileSystem::get_singleton();
	if (filesystem && filesystem->get_filesystem()) {
		filesystem->update_files(changed_paths);
		data["filesystem_synced"] = true;
	}
	if (edited_root && changed_paths.has(edited_root->get_scene_file_path())) {
		data["requires_scene_reload"] = true;
	}
	data["authored_state_changed"] = true;
	if (action_timeline) {
		action_timeline->record_event("script_compute", data);
	}
	_cleanup_compute(compute);
	return _ok(data);
}

void SolersScriptService::compute_script_complete(const String &p_call_id) {
	PendingCompute *compute = pending_computes.getptr(p_call_id);
	if (!compute) {
		return;
	}
	_cleanup_compute(*compute);
	pending_computes.erase(p_call_id);
}
