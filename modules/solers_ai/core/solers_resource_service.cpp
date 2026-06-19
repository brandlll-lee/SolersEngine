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
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/resource.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "core/io/resource_uid.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "editor/file_system/editor_file_system.h"
#include "editor/export/editor_export.h"

void SolersResourceService::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_resource_info", "args"), &SolersResourceService::get_resource_info);
	ClassDB::bind_method(D_METHOD("create_resource", "args"), &SolersResourceService::create_resource);
	ClassDB::bind_method(D_METHOD("get_resource_property", "args"), &SolersResourceService::get_resource_property);
	ClassDB::bind_method(D_METHOD("set_resource_property", "args"), &SolersResourceService::set_resource_property);
	ClassDB::bind_method(D_METHOD("call_resource_method", "args"), &SolersResourceService::call_resource_method);
	ClassDB::bind_method(D_METHOD("list_export_presets", "args"), &SolersResourceService::list_export_presets);
	ClassDB::bind_method(D_METHOD("validate_export_presets", "args"), &SolersResourceService::validate_export_presets);
	ClassDB::bind_method(D_METHOD("run_export_preset", "args"), &SolersResourceService::run_export_preset);
}

static Variant _solers_resource_displayable(const Variant &p_value) {
	if (p_value.get_type() != Variant::OBJECT) {
		return p_value;
	}
	Object *object = p_value;
	Resource *resource = Object::cast_to<Resource>(object);
	if (resource) {
		return vformat("<%s:%s>", resource->get_class(), resource->get_path());
	}
	return object ? Variant(vformat("<%s:%s>", object->get_class(), itos(object->get_instance_id()))) : Variant("<null>");
}

static bool _solers_find_property(Object *p_object, const StringName &p_property, PropertyInfo &r_info) {
	List<PropertyInfo> properties;
	p_object->get_property_list(&properties);
	for (const PropertyInfo &info : properties) {
		if (info.name == p_property) {
			r_info = info;
			return true;
		}
	}
	return false;
}

static bool _solers_construct_variant(Variant::Type p_type, const Array &p_args, Variant &r_out) {
	Vector<Variant> argv;
	for (int i = 0; i < p_args.size(); i++) {
		argv.push_back(p_args[i]);
	}
	Vector<const Variant *> argp;
	for (int i = 0; i < argv.size(); i++) {
		argp.push_back(&argv[i]);
	}
	Callable::CallError call_error;
	Variant::construct(p_type, r_out, argp.ptrw(), argp.size(), call_error);
	return call_error.error == Callable::CallError::CALL_OK;
}

static bool _solers_coerce_property_value(Object *p_object, const StringName &p_property, const Variant &p_value, Variant &r_out, String &r_error) {
	PropertyInfo info;
	if (!_solers_find_property(p_object, p_property, info)) {
		r_error = vformat("Property '%s' is not exposed by %s.", String(p_property), p_object->get_class());
		return false;
	}
	if (p_value.get_type() == info.type) {
		r_out = p_value;
		return true;
	}
	if (info.type == Variant::OBJECT && p_value.get_type() == Variant::STRING) {
		String path = String(p_value).strip_edges().replace_char('\\', '/').simplify_path();
		if (path.begins_with("res://")) {
			Error load_error = OK;
			Ref<Resource> resource = ResourceLoader::load(path, String(info.class_name), ResourceFormatLoader::CACHE_MODE_REUSE, &load_error);
			if (resource.is_null() || load_error != OK) {
				r_error = vformat("Failed to load resource '%s' for property '%s' (error %d).", path, String(p_property), (int)load_error);
				return false;
			}
			if (info.class_name != StringName() && !ClassDB::is_parent_class(resource->get_class_name(), info.class_name)) {
				r_error = vformat("Resource '%s' is %s, expected %s.", path, resource->get_class(), String(info.class_name));
				return false;
			}
			r_out = resource;
			return true;
		}
	}
	if (p_value.get_type() == Variant::ARRAY && _solers_construct_variant(info.type, p_value, r_out)) {
		return true;
	}
	if (info.type == Variant::COLOR && p_value.get_type() == Variant::DICTIONARY) {
		const Dictionary components = p_value;
		if (components.has("r") && components.has("g") && components.has("b")) {
			Array args;
			args.push_back(components["r"]);
			args.push_back(components["g"]);
			args.push_back(components["b"]);
			args.push_back(components.get("a", 1.0));
			if (_solers_construct_variant(info.type, args, r_out)) {
				return true;
			}
		}
		r_error = "Could not construct Color from the provided dictionary; expected r, g, b and optional a.";
		return false;
	}
	r_out = p_value;
	return true;
}

static Dictionary _solers_resource_data(const Ref<Resource> &p_resource, const String &p_path = String()) {
	Dictionary data;
	data["path"] = p_path.is_empty() ? p_resource->get_path() : p_path;
	data["class_name"] = p_resource->get_class();
	data["resource_name"] = p_resource->get_name();
	return data;
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

Dictionary SolersResourceService::create_resource(const Dictionary &p_args) const {
	const String class_name = String(p_args.get("class_name", String())).strip_edges();
	const String path_arg = p_args.get("path", String());
	if (class_name.is_empty()) {
		return _error("INVALID_ARGUMENT", "class_name is required.");
	}

	String path;
	String path_error;
	if (!_normalize_project_path(path_arg, path, path_error)) {
		return _error("INVALID_PATH", path_error);
	}

	const StringName class_sn = StringName(class_name);
	if (!ClassDB::class_exists(class_sn) || !ClassDB::can_instantiate(class_sn) || !ClassDB::is_parent_class(class_sn, SNAME("Resource"))) {
		return _error("INVALID_RESOURCE_TYPE", vformat("Class is not an instantiable Resource type: %s", class_name));
	}

	Ref<Resource> resource = Object::cast_to<Resource>(ClassDB::instantiate(class_sn));
	if (resource.is_null()) {
		return _error("RESOURCE_INSTANTIATION_FAILED", vformat("Failed to instantiate resource type: %s", class_name), false);
	}

	Error dir_err = DirAccess::make_dir_recursive_absolute(ProjectSettings::get_singleton()->globalize_path(path.get_base_dir()));
	if (dir_err != OK) {
		return _error("DIRECTORY_CREATE_FAILED", vformat("Failed to create parent directory, error code %d.", dir_err));
	}
	Error save_err = ResourceSaver::save(resource, path);
	if (save_err != OK) {
		return _error("RESOURCE_SAVE_FAILED", vformat("Failed to save resource, error code %d.", save_err));
	}
	if (EditorFileSystem::get_singleton()) {
		EditorFileSystem::get_singleton()->update_file(path);
	}
	return _ok(_solers_resource_data(resource, path));
}

Dictionary SolersResourceService::get_resource_property(const Dictionary &p_args) const {
	const String path_arg = p_args.get("path", String());
	const String property = String(p_args.get("property", String())).strip_edges();
	const String type_hint = p_args.get("type_hint", String());
	if (property.is_empty()) {
		return _error("INVALID_ARGUMENT", "property is required.");
	}

	String path;
	String path_error;
	if (!_normalize_project_path(path_arg, path, path_error)) {
		return _error("INVALID_PATH", path_error);
	}
	Error load_error = OK;
	Ref<Resource> resource = ResourceLoader::load(path, type_hint, ResourceFormatLoader::CACHE_MODE_REUSE, &load_error);
	if (resource.is_null() || load_error != OK) {
		return _error("RESOURCE_LOAD_FAILED", vformat("Failed to load resource '%s' (error %d).", path, (int)load_error));
	}

	const StringName property_sn = StringName(property);
	PropertyInfo info;
	if (!_solers_find_property(resource.ptr(), property_sn, info)) {
		return _error("UNKNOWN_PROPERTY", vformat("Property '%s' is not exposed by %s.", property, resource->get_class()));
	}

	Dictionary data = _solers_resource_data(resource, path);
	data["property"] = property;
	data["type"] = Variant::get_type_name(info.type);
	data["value"] = _solers_resource_displayable(resource->get(property_sn));
	return _ok(data);
}

Dictionary SolersResourceService::set_resource_property(const Dictionary &p_args) const {
	const String path_arg = p_args.get("path", String());
	const String property = String(p_args.get("property", String())).strip_edges();
	const String type_hint = p_args.get("type_hint", String());
	if (property.is_empty()) {
		return _error("INVALID_ARGUMENT", "property is required.");
	}
	if (!p_args.has("value")) {
		return _error("INVALID_ARGUMENT", "value is required.");
	}

	String path;
	String path_error;
	if (!_normalize_project_path(path_arg, path, path_error)) {
		return _error("INVALID_PATH", path_error);
	}
	Error load_error = OK;
	Ref<Resource> resource = ResourceLoader::load(path, type_hint, ResourceFormatLoader::CACHE_MODE_REUSE, &load_error);
	if (resource.is_null() || load_error != OK) {
		return _error("RESOURCE_LOAD_FAILED", vformat("Failed to load resource '%s' (error %d).", path, (int)load_error));
	}

	const StringName property_sn = StringName(property);
	Variant value;
	String error;
	if (!_solers_coerce_property_value(resource.ptr(), property_sn, p_args["value"], value, error)) {
		return _error("INVALID_PROPERTY_VALUE", error);
	}
	bool valid = false;
	resource->set(property_sn, value, &valid);
	if (!valid) {
		return _error("PROPERTY_SET_FAILED", vformat("Setting property '%s' failed on %s.", property, resource->get_class()));
	}
	Error save_err = ResourceSaver::save(resource, path);
	if (save_err != OK) {
		return _error("RESOURCE_SAVE_FAILED", vformat("Failed to save resource, error code %d.", save_err));
	}
	if (EditorFileSystem::get_singleton()) {
		EditorFileSystem::get_singleton()->update_file(path);
	}

	Dictionary data = _solers_resource_data(resource, path);
	data["property"] = property;
	data["value"] = _solers_resource_displayable(resource->get(property_sn));
	return _ok(data);
}

Dictionary SolersResourceService::call_resource_method(const Dictionary &p_args) const {
	const String path_arg = p_args.get("path", String());
	const String method = String(p_args.get("method", String())).strip_edges();
	const String type_hint = p_args.get("type_hint", String());
	const Array args = p_args.get("args", Array());
	const bool save = p_args.get("save", false);
	if (method.is_empty()) {
		return _error("INVALID_ARGUMENT", "method is required.");
	}

	String path;
	String path_error;
	if (!_normalize_project_path(path_arg, path, path_error)) {
		return _error("INVALID_PATH", path_error);
	}
	Error load_error = OK;
	Ref<Resource> resource = ResourceLoader::load(path, type_hint, ResourceFormatLoader::CACHE_MODE_REUSE, &load_error);
	if (resource.is_null() || load_error != OK) {
		return _error("RESOURCE_LOAD_FAILED", vformat("Failed to load resource '%s' (error %d).", path, (int)load_error));
	}

	const StringName method_sn = StringName(method);
	if (!resource->has_method(method_sn)) {
		return _error("UNKNOWN_METHOD", vformat("Method '%s' is not available on %s.", method, resource->get_class()));
	}
	Vector<Variant> argv;
	for (int i = 0; i < args.size(); i++) {
		argv.push_back(args[i]);
	}
	Vector<const Variant *> argp;
	for (int i = 0; i < argv.size(); i++) {
		argp.push_back(&argv[i]);
	}
	Callable::CallError call_error;
	const Variant ret = resource->callp(method_sn, argp.ptrw(), argp.size(), call_error);
	if (call_error.error != Callable::CallError::CALL_OK) {
		return _error("METHOD_CALL_FAILED", vformat("Calling %s.%s(%d args) failed (error %d).", resource->get_class(), method, args.size(), (int)call_error.error));
	}
	if (save) {
		Error save_err = ResourceSaver::save(resource, path);
		if (save_err != OK) {
			return _error("RESOURCE_SAVE_FAILED", vformat("Failed to save resource, error code %d.", save_err));
		}
		if (EditorFileSystem::get_singleton()) {
			EditorFileSystem::get_singleton()->update_file(path);
		}
	}

	Dictionary data = _solers_resource_data(resource, path);
	data["method"] = method;
	data["arg_count"] = args.size();
	data["saved"] = save;
	data["result"] = _solers_resource_displayable(ret);
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

Dictionary SolersResourceService::run_export_preset(const Dictionary &p_args) const {
	EditorExport *editor_export = EditorExport::get_singleton();
	if (!editor_export) {
		return _error("EDITOR_EXPORT_UNAVAILABLE", "EditorExport singleton is not available.", false);
	}

	Ref<EditorExportPreset> preset;
	int preset_index = -1;
	if (p_args.has("preset_index")) {
		preset_index = (int)p_args["preset_index"];
		if (preset_index < 0 || preset_index >= editor_export->get_export_preset_count()) {
			return _error("INVALID_PRESET", vformat("Export preset index %d is out of range.", preset_index));
		}
		preset = editor_export->get_export_preset(preset_index);
	} else {
		const String preset_name = String(p_args.get("preset_name", String())).strip_edges();
		if (preset_name.is_empty()) {
			return _error("INVALID_ARGUMENT", "preset_index or preset_name is required.");
		}
		for (int i = 0; i < editor_export->get_export_preset_count(); i++) {
			Ref<EditorExportPreset> candidate = editor_export->get_export_preset(i);
			if (candidate.is_valid() && candidate->get_name() == preset_name) {
				preset = candidate;
				preset_index = i;
				break;
			}
		}
		if (preset.is_null()) {
			return _error("INVALID_PRESET", vformat("Export preset '%s' was not found.", preset_name));
		}
	}
	if (preset.is_null()) {
		return _error("INVALID_PRESET", "Export preset is unavailable.");
	}

	Ref<EditorExportPlatform> platform = preset->get_platform();
	if (platform.is_null()) {
		return _error("EXPORT_PLATFORM_UNAVAILABLE", "Export platform is missing.", false);
	}

	const bool debug = p_args.get("debug", false);
	String validation_error;
	bool missing_templates = false;
	if (!platform->can_export(preset, validation_error, missing_templates, debug)) {
		return _error("EXPORT_NOT_READY", validation_error.is_empty() ? "Export preset cannot be exported." : validation_error);
	}

	String export_path = p_args.has("export_path") ? String(p_args["export_path"]).strip_edges() : preset->get_export_path();
	if (export_path.is_empty()) {
		return _error("INVALID_EXPORT_PATH", "Export path is empty.");
	}
	if (export_path.begins_with("res://") || export_path.begins_with("user://")) {
		export_path = ProjectSettings::get_singleton()->globalize_path(export_path);
	}

	const uint64_t started = OS::get_singleton()->get_ticks_msec();
	const Error err = platform->export_project(preset, debug, export_path);
	const uint64_t duration_msec = OS::get_singleton()->get_ticks_msec() - started;

	Array messages;
	for (int message_index = 0; message_index < MIN(platform->get_message_count(), 20); message_index++) {
		EditorExportPlatform::ExportMessage message = platform->get_message(message_index);
		Dictionary message_item;
		message_item["type"] = _export_message_type_to_string(message.msg_type);
		message_item["category"] = message.category;
		message_item["text"] = message.text;
		messages.push_back(message_item);
	}

	Dictionary data;
	data["preset_index"] = preset_index;
	data["preset_name"] = preset->get_name();
	data["platform"] = platform->get_name();
	data["export_path"] = export_path;
	data["debug"] = debug;
	data["blocking"] = true;
	data["error_code"] = (int)err;
	data["duration_msec"] = (int)duration_msec;
	data["worst_message_type"] = _export_message_type_to_string(platform->get_worst_message_type());
	data["messages"] = messages;
	data["message_count"] = platform->get_message_count();
	if (err != OK) {
		Dictionary error;
		error["code"] = "EXPORT_FAILED";
		error["message"] = vformat("Export failed with Error %d.", (int)err);
		error["recoverable"] = true;
		Dictionary result;
		result["ok"] = false;
		result["error"] = error;
		result["data"] = data;
		return result;
	}
	return _ok(data);
}
