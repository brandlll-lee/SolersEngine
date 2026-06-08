/**************************************************************************/
/*  solers_resource_service.cpp                                           */
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

#include "solers_resource_service.h"

#include "core/config/project_settings.h"
#include "core/io/file_access.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_uid.h"
#include "core/object/class_db.h"
#include "editor/export/editor_export.h"

void SolersResourceService::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_resource_info", "args"), &SolersResourceService::get_resource_info);
	ClassDB::bind_method(D_METHOD("list_export_presets", "args"), &SolersResourceService::list_export_presets);
	ClassDB::bind_method(D_METHOD("validate_export_presets", "args"), &SolersResourceService::validate_export_presets);
}

Dictionary SolersResourceService::_ok(const Variant &p_data) const {
	Dictionary result;
	result["ok"] = true;
	result["data"] = p_data;
	return result;
}

Dictionary SolersResourceService::_error(const String &p_code, const String &p_message, bool p_recoverable) const {
	Dictionary error;
	error["code"] = p_code;
	error["message"] = p_message;
	error["recoverable"] = p_recoverable;

	Dictionary result;
	result["ok"] = false;
	result["error"] = error;
	return result;
}

bool SolersResourceService::_normalize_project_path(const String &p_path, String &r_res_path, String &r_error) const {
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

String SolersResourceService::_export_filter_to_string(int p_filter) const {
	switch ((EditorExportPreset::ExportFilter)p_filter) {
		case EditorExportPreset::EXPORT_ALL_RESOURCES:
			return "all_resources";
		case EditorExportPreset::EXPORT_SELECTED_SCENES:
			return "selected_scenes";
		case EditorExportPreset::EXPORT_SELECTED_RESOURCES:
			return "selected_resources";
		case EditorExportPreset::EXCLUDE_SELECTED_RESOURCES:
			return "exclude_selected_resources";
		case EditorExportPreset::EXPORT_CUSTOMIZED:
			return "customized";
	}
	return "unknown";
}

String SolersResourceService::_script_export_mode_to_string(int p_mode) const {
	switch ((EditorExportPreset::ScriptExportMode)p_mode) {
		case EditorExportPreset::MODE_SCRIPT_TEXT:
			return "text";
		case EditorExportPreset::MODE_SCRIPT_BINARY_TOKENS:
			return "binary_tokens";
		case EditorExportPreset::MODE_SCRIPT_BINARY_TOKENS_COMPRESSED:
			return "compressed_binary_tokens";
	}
	return "unknown";
}

String SolersResourceService::_export_message_type_to_string(int p_type) const {
	switch ((EditorExportPlatform::ExportMessageType)p_type) {
		case EditorExportPlatform::EXPORT_MESSAGE_NONE:
			return "none";
		case EditorExportPlatform::EXPORT_MESSAGE_INFO:
			return "info";
		case EditorExportPlatform::EXPORT_MESSAGE_WARNING:
			return "warning";
		case EditorExportPlatform::EXPORT_MESSAGE_ERROR:
			return "error";
	}
	return "unknown";
}

Dictionary SolersResourceService::get_resource_info(const Dictionary &p_args) const {
	const String path_arg = p_args.get("path", String());
	const bool include_dependencies = p_args.get("include_dependencies", true);
	const int max_dependencies = CLAMP((int)p_args.get("max_dependencies", 128), 0, 2048);

	String path;
	String path_error;
	if (!_normalize_project_path(path_arg, path, path_error)) {
		return _error("INVALID_PATH", path_error);
	}

	Dictionary data;
	data["path"] = path;
	data["exists"] = FileAccess::exists(path) || ResourceLoader::exists(path);
	data["resource_exists"] = ResourceLoader::exists(path);
	data["resource_type"] = ResourceLoader::get_resource_type(path);
	data["script_class"] = ResourceLoader::get_resource_script_class(path);

	const ResourceUID::ID uid = ResourceLoader::get_resource_uid(path);
	data["uid"] = uid == ResourceUID::INVALID_ID ? String() : ResourceUID::get_singleton()->id_to_text(uid);
	data["is_imported"] = ResourceLoader::is_imported(path);
	data["import_valid"] = ResourceLoader::is_import_valid(path);
	data["import_group_file"] = ResourceLoader::get_import_group_file(path);

	if (include_dependencies) {
		List<String> dependencies;
		ResourceLoader::get_dependencies(path, &dependencies, true);
		Array dependency_items;
		int count = 0;
		for (const String &dependency : dependencies) {
			if (count >= max_dependencies) {
				break;
			}
			dependency_items.push_back(dependency);
			count++;
		}
		data["dependencies"] = dependency_items;
		data["dependency_count"] = dependencies.size();
		data["dependencies_truncated"] = dependencies.size() > max_dependencies;
	}

	return _ok(data);
}

Dictionary SolersResourceService::list_export_presets(const Dictionary &p_args) const {
	EditorExport *editor_export = EditorExport::get_singleton();
	if (!editor_export) {
		return _error("EDITOR_EXPORT_UNAVAILABLE", "EditorExport singleton is not available.", false);
	}

	const bool include_platforms = p_args.get("include_platforms", true);
	Dictionary data;
	Array presets;
	for (int i = 0; i < editor_export->get_export_preset_count(); i++) {
		Ref<EditorExportPreset> preset = editor_export->get_export_preset(i);
		if (preset.is_null()) {
			continue;
		}
		Ref<EditorExportPlatform> platform = preset->get_platform();

		Dictionary item;
		item["index"] = i;
		item["name"] = preset->get_name();
		item["platform"] = platform.is_valid() ? platform->get_name() : String();
		item["runnable"] = preset->is_runnable();
		item["dedicated_server"] = preset->is_dedicated_server();
		item["export_filter"] = _export_filter_to_string(preset->get_export_filter());
		item["include_filter"] = preset->get_include_filter();
		item["exclude_filter"] = preset->get_exclude_filter();
		item["custom_features"] = preset->get_custom_features();
		item["export_path"] = preset->get_export_path();
		item["script_export_mode"] = _script_export_mode_to_string(preset->get_script_export_mode());
		presets.push_back(item);
	}

	data["presets"] = presets;
	data["preset_count"] = presets.size();

	if (include_platforms) {
		Array platforms;
		for (int i = 0; i < editor_export->get_export_platform_count(); i++) {
			Ref<EditorExportPlatform> platform = editor_export->get_export_platform(i);
			if (platform.is_null()) {
				continue;
			}
			Dictionary item;
			item["index"] = i;
			item["name"] = platform->get_name();
			item["os_name"] = platform->get_os_name();
			platforms.push_back(item);
		}
		data["platforms"] = platforms;
		data["platform_count"] = platforms.size();
	}

	return _ok(data);
}

Dictionary SolersResourceService::validate_export_presets(const Dictionary &p_args) const {
	EditorExport *editor_export = EditorExport::get_singleton();
	if (!editor_export) {
		return _error("EDITOR_EXPORT_UNAVAILABLE", "EditorExport singleton is not available.", false);
	}

	const bool debug = p_args.get("debug", false);
	Array validations;
	int error_count = 0;
	int missing_template_count = 0;

	for (int i = 0; i < editor_export->get_export_preset_count(); i++) {
		Ref<EditorExportPreset> preset = editor_export->get_export_preset(i);
		if (preset.is_null()) {
			continue;
		}
		Ref<EditorExportPlatform> platform = preset->get_platform();

		Dictionary item;
		item["index"] = i;
		item["name"] = preset->get_name();
		item["platform"] = platform.is_valid() ? platform->get_name() : String();
		item["export_path"] = preset->get_export_path();

		if (platform.is_null()) {
			item["valid"] = false;
			item["error"] = "Export platform is missing.";
			item["missing_templates"] = false;
			error_count++;
			validations.push_back(item);
			continue;
		}

		String error;
		bool missing_templates = false;
		const bool valid = platform->can_export(preset, error, missing_templates, debug);
		item["valid"] = valid;
		item["error"] = error;
		item["missing_templates"] = missing_templates;
		item["worst_message_type"] = _export_message_type_to_string(platform->get_worst_message_type());

		Array messages;
		for (int message_index = 0; message_index < platform->get_message_count(); message_index++) {
			EditorExportPlatform::ExportMessage message = platform->get_message(message_index);
			Dictionary message_item;
			message_item["type"] = _export_message_type_to_string(message.msg_type);
			message_item["category"] = message.category;
			message_item["text"] = message.text;
			messages.push_back(message_item);
		}
		item["messages"] = messages;

		if (!valid) {
			error_count++;
		}
		if (missing_templates) {
			missing_template_count++;
		}
		validations.push_back(item);
	}

	Dictionary data;
	data["valid"] = error_count == 0;
	data["preset_count"] = validations.size();
	data["error_count"] = error_count;
	data["missing_template_count"] = missing_template_count;
	data["validations"] = validations;
	return _ok(data);
}
