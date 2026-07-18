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

#include "modules/solers_ai/core/solers_resource_service.h"

#include "core/config/engine.h"
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
#include "modules/solers_ai/core/solers_geometry_facts.h"
#include "scene/main/node.h"
#include "scene/resources/packed_scene.h"

void SolersResourceService::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_resource_info", "args"), &SolersResourceService::get_resource_info);
	ClassDB::bind_method(D_METHOD("inspect_resource", "args"), &SolersResourceService::inspect_resource);
	ClassDB::bind_method(D_METHOD("edit_resource", "args"), &SolersResourceService::edit_resource);
	ClassDB::bind_method(D_METHOD("create_resource", "args"), &SolersResourceService::create_resource);
	ClassDB::bind_method(D_METHOD("get_resource_property", "args"), &SolersResourceService::get_resource_property);
	ClassDB::bind_method(D_METHOD("set_resource_property", "args"), &SolersResourceService::set_resource_property);
	ClassDB::bind_method(D_METHOD("native_instantiate", "args"), &SolersResourceService::native_instantiate);
	ClassDB::bind_method(D_METHOD("native_load", "args"), &SolersResourceService::native_load);
	ClassDB::bind_method(D_METHOD("native_list_properties", "args"), &SolersResourceService::native_list_properties);
	ClassDB::bind_method(D_METHOD("native_get", "args"), &SolersResourceService::native_get);
	ClassDB::bind_method(D_METHOD("native_set", "args"), &SolersResourceService::native_set);
	ClassDB::bind_method(D_METHOD("native_list_methods", "args"), &SolersResourceService::native_list_methods);
	ClassDB::bind_method(D_METHOD("native_call", "args"), &SolersResourceService::native_call);
	ClassDB::bind_method(D_METHOD("native_save", "args"), &SolersResourceService::native_save);
	ClassDB::bind_method(D_METHOD("native_free", "args"), &SolersResourceService::native_free);
	ClassDB::bind_method(D_METHOD("list_export_presets", "args"), &SolersResourceService::list_export_presets);
	ClassDB::bind_method(D_METHOD("validate_export_presets", "args"), &SolersResourceService::validate_export_presets);
	ClassDB::bind_method(D_METHOD("run_export_preset", "args"), &SolersResourceService::run_export_preset);
}

SolersResourceService::~SolersResourceService() {
	for (const KeyValue<ObjectID, Node *> &E : temp_nodes) {
		if (E.value) {
			memdelete(E.value);
		}
	}
}

static bool _solers_resolve_object_handle(const Variant &p_object_id, Object *&r_object, String &r_error) {
	if (p_object_id.get_type() == Variant::DICTIONARY) {
		const Dictionary handle = p_object_id;
		return _solers_resolve_object_handle(handle.get("object_id", Variant()), r_object, r_error);
	}
	int64_t id = 0;
	if (p_object_id.get_type() == Variant::STRING) {
		id = String(p_object_id).strip_edges().to_int();
	} else if (p_object_id.get_type() == Variant::INT) {
		id = p_object_id;
	} else if (p_object_id.get_type() == Variant::FLOAT) {
		id = (int64_t)(double)p_object_id;
	} else {
		r_error = "object_id must be the string or integer returned by a native object tool.";
		return false;
	}
	if (id == 0) {
		r_error = "object_id is empty.";
		return false;
	}
	r_object = ObjectDB::get_instance(ObjectID(id));
	if (!r_object) {
		r_error = vformat("No live Godot object for object_id %s.", String::num_int64(id));
		return false;
	}
	return true;
}

bool SolersResourceService::_resolve_native_object(const Variant &p_object_id, Object *&r_object, String &r_error) const {
	return _solers_resolve_object_handle(p_object_id, r_object, r_error);
}

Dictionary SolersResourceService::_native_object_handle(Object *p_object) const {
	Dictionary data;
	data["kind"] = "godot_object";
	if (!p_object) {
		data["object_id"] = String();
		data["valid"] = false;
		return data;
	}
	const ObjectID id = p_object->get_instance_id();
	data["valid"] = true;
	data["object_id"] = String::num_int64((int64_t)id);
	data["class_name"] = p_object->get_class();
	if (RefCounted *ref_counted = Object::cast_to<RefCounted>(p_object)) {
		retained_refcounted[id] = Ref<RefCounted>(ref_counted);
	}

	Resource *resource = Object::cast_to<Resource>(p_object);
	if (resource) {
		data["path"] = resource->get_path();
		data["resource_name"] = resource->get_name();
	}

	Node *node = Object::cast_to<Node>(p_object);
	if (node) {
		if (!node->is_inside_tree() && !node->get_parent()) {
			temp_nodes[id] = node;
		}
		data["name"] = node->get_name();
		data["inside_tree"] = node->is_inside_tree();
		data["temporary"] = temp_nodes.has(id);
		if (node->is_inside_tree()) {
			data["node_path"] = node->get_path();
		}
	}
	return data;
}

Variant SolersResourceService::_displayable(const Variant &p_value) const {
	if (p_value.get_type() == Variant::OBJECT) {
		Object *object = p_value;
		return _native_object_handle(object);
	}
	if (p_value.get_type() == Variant::ARRAY) {
		Array in = p_value;
		Array out;
		for (int i = 0; i < in.size(); i++) {
			out.push_back(_displayable(in[i]));
		}
		return out;
	}
	if (p_value.get_type() == Variant::DICTIONARY) {
		Dictionary in = p_value;
		Dictionary out;
		Array keys = in.keys();
		for (int i = 0; i < keys.size(); i++) {
			out[keys[i]] = _displayable(in[keys[i]]);
		}
		return out;
	}
	return p_value;
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

static bool _solers_matches_allowed_class(Object *p_object, const StringName &p_allowed_classes) {
	if (!p_object || p_allowed_classes == StringName()) {
		return true;
	}

	bool matched = false;
	const Vector<String> classes = String(p_allowed_classes).split(",");
	for (const String &entry : classes) {
		const String class_name = entry.strip_edges();
		if (class_name.begins_with("-")) {
			if (p_object->is_class(StringName(class_name.trim_prefix("-")))) {
				return false;
			}
		} else if (p_object->is_class(StringName(class_name))) {
			matched = true;
		}
	}
	return matched;
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

static Variant::Type _solers_packed_array_element_type(Variant::Type p_type) {
	switch (p_type) {
		case Variant::PACKED_BYTE_ARRAY:
		case Variant::PACKED_INT32_ARRAY:
		case Variant::PACKED_INT64_ARRAY:
			return Variant::INT;
		case Variant::PACKED_FLOAT32_ARRAY:
		case Variant::PACKED_FLOAT64_ARRAY:
			return Variant::FLOAT;
		case Variant::PACKED_STRING_ARRAY:
			return Variant::STRING;
		case Variant::PACKED_VECTOR2_ARRAY:
			return Variant::VECTOR2;
		case Variant::PACKED_VECTOR3_ARRAY:
			return Variant::VECTOR3;
		case Variant::PACKED_COLOR_ARRAY:
			return Variant::COLOR;
		case Variant::PACKED_VECTOR4_ARRAY:
			return Variant::VECTOR4;
		default:
			return Variant::NIL;
	}
}

static bool _solers_construct_variant_value(Variant::Type p_type, const Variant &p_value, Variant &r_out) {
	if (p_value.get_type() == p_type) {
		r_out = p_value;
		return true;
	}
	if (p_value.get_type() == Variant::ARRAY) {
		const Variant::Type element_type = _solers_packed_array_element_type(p_type);
		if (element_type != Variant::NIL) {
			const Array values = p_value;
			Array converted;
			for (int i = 0; i < values.size(); i++) {
				Variant element;
				if (!_solers_construct_variant_value(element_type, values[i], element)) {
					return false;
				}
				converted.push_back(element);
			}
			Array one_arg;
			one_arg.push_back(converted);
			return _solers_construct_variant(p_type, one_arg, r_out);
		}
		if (_solers_construct_variant(p_type, Array(p_value), r_out)) {
			return true;
		}
		Array one_arg;
		one_arg.push_back(p_value);
		return _solers_construct_variant(p_type, one_arg, r_out);
	}
	if (p_value.get_type() != Variant::DICTIONARY) {
		Array one_arg;
		one_arg.push_back(p_value);
		return _solers_construct_variant(p_type, one_arg, r_out);
	}

	Variant constructed;
	Array no_args;
	if (!_solers_construct_variant(p_type, no_args, constructed)) {
		return false;
	}
	const Dictionary components = p_value;
	for (const Variant *key = components.next(nullptr); key; key = components.next(key)) {
		if (key->get_type() != Variant::STRING && key->get_type() != Variant::STRING_NAME) {
			return false;
		}
		const StringName component_name = StringName(String(*key));
		bool valid = false;
		const Variant current = constructed.get_named(component_name, valid);
		if (!valid) {
			return false;
		}
		Variant component = components[*key];
		if (component.get_type() != current.get_type()) {
			Variant coerced;
			if (!_solers_construct_variant_value(current.get_type(), component, coerced)) {
				return false;
			}
			component = coerced;
		}
		constructed.set_named(component_name, component, valid);
		if (!valid) {
			return false;
		}
	}
	r_out = constructed;
	return true;
}

static bool _solers_coerce_value(const PropertyInfo &p_info, const Variant &p_value, Variant &r_out, String &r_error) {
	if (p_info.type == Variant::NIL || (p_info.type != Variant::OBJECT && p_value.get_type() == p_info.type)) {
		r_out = p_value;
		return true;
	}
	if (p_info.type == Variant::OBJECT) {
		Object *object = nullptr;
		Variant object_value;
		if (p_value.get_type() == Variant::NIL) {
			r_out = Variant();
			return true;
		}
		if (p_value.get_type() == Variant::OBJECT) {
			object = p_value;
			object_value = p_value;
		} else if (p_value.get_type() == Variant::DICTIONARY) {
			if (!_solers_resolve_object_handle(p_value, object, r_error)) {
				return false;
			}
			object_value = object;
		} else if (p_value.get_type() == Variant::STRING) {
			const String path = String(p_value).strip_edges().replace_char('\\', '/').simplify_path();
			if (path.begins_with("res://")) {
				Error load_error = OK;
				const Ref<Resource> resource = ResourceLoader::load(path, String(), ResourceFormatLoader::CACHE_MODE_REUSE, &load_error);
				if (resource.is_null() || load_error != OK) {
					r_error = vformat("Failed to load resource '%s' (error %d).", path, (int)load_error);
					return false;
				}
				object = resource.ptr();
				object_value = resource;
			}
		}
		if (!object) {
			r_error = vformat("Expected %s or a res:// resource path, not %s.", String(p_info.class_name), Variant::get_type_name(p_value.get_type()));
			return false;
		}
		if (!_solers_matches_allowed_class(object, p_info.class_name)) {
			r_error = vformat("Object is %s, expected %s.", object->get_class(), String(p_info.class_name));
			return false;
		}
		r_out = object_value;
		return true;
	}
	if (_solers_construct_variant_value(p_info.type, p_value, r_out)) {
		return true;
	}
	r_error = vformat("Could not construct %s from %s.", Variant::get_type_name(p_info.type), Variant::get_type_name(p_value.get_type()));
	return false;
}

static bool _solers_property_matches(const Variant &p_actual, const Variant &p_expected) {
	if ((p_actual.get_type() == Variant::FLOAT || p_actual.get_type() == Variant::INT) &&
			(p_expected.get_type() == Variant::FLOAT || p_expected.get_type() == Variant::INT)) {
		return Math::is_equal_approx((double)p_actual, (double)p_expected);
	}
	return p_actual == p_expected;
}

bool solers_coerce_property_value(Object *p_object, const StringName &p_property, const Variant &p_value, Variant &r_out, String &r_error) {
	PropertyInfo info;
	if (!_solers_find_property(p_object, p_property, info)) {
		r_error = vformat("Property '%s' is not exposed by %s.", String(p_property), p_object->get_class());
		return false;
	}
	if (_solers_coerce_value(info, p_value, r_out, r_error)) {
		return true;
	}
	r_error = vformat("Property '%s': %s", String(p_property), r_error);
	return false;
}

bool solers_call_method(Object *p_object, const MethodInfo &p_method, const Array &p_args, Variant &r_result, String &r_error_code, String &r_error) {
	const int declared_args = p_method.arguments.size();
	const int required_args = declared_args - p_method.default_arguments.size();
	const bool vararg = (p_method.flags & METHOD_FLAG_VARARG) != 0;
	if (p_args.size() < required_args || (!vararg && p_args.size() > declared_args)) {
		r_error_code = "INVALID_ARGUMENT_COUNT";
		r_error = vformat("%s.%s expects %d-%s arguments, got %d.", p_object->get_class(), String(p_method.name), required_args, vararg ? String("many") : String::num_int64(declared_args), p_args.size());
		return false;
	}

	Vector<Variant> argv;
	for (int i = 0; i < p_args.size(); i++) {
		Variant value = p_args[i];
		if (i < declared_args && !_solers_coerce_value(p_method.arguments[i], value, value, r_error)) {
			r_error_code = "INVALID_ARGUMENT_VALUE";
			r_error = vformat("Argument %d ('%s') for %s.%s: %s", i, String(p_method.arguments[i].name), p_object->get_class(), String(p_method.name), r_error);
			return false;
		}
		argv.push_back(value);
	}
	Vector<const Variant *> argp;
	for (int i = 0; i < argv.size(); i++) {
		argp.push_back(&argv[i]);
	}
	Callable::CallError call_error;
	r_result = p_object->callp(p_method.name, argp.ptrw(), argp.size(), call_error);
	if (call_error.error != Callable::CallError::CALL_OK) {
		r_error_code = "METHOD_CALL_FAILED";
		r_error = vformat("Calling %s.%s(%d args) failed (error %d).", p_object->get_class(), String(p_method.name), p_args.size(), (int)call_error.error);
		return false;
	}
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
	if ((bool)data["resource_exists"]) {
		Error load_error = OK;
		const Ref<Resource> resource = ResourceLoader::load(path, String(), ResourceFormatLoader::CACHE_MODE_REUSE, &load_error);
		const Ref<Mesh> mesh = resource;
		const Ref<PackedScene> scene = resource;
		if (load_error == OK && mesh.is_valid()) {
			data["geometry"] = solers_describe_mesh(mesh);
		} else if (load_error == OK && scene.is_valid()) {
			Node *root = scene->instantiate(PackedScene::GEN_EDIT_STATE_DISABLED);
			if (root) {
				data["geometry"] = solers_describe_geometry(root);
				memdelete(root);
			}
		}
	}

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

Dictionary SolersResourceService::inspect_resource(const Dictionary &p_args) const {
	Dictionary info_result = get_resource_info(p_args);
	if (!(bool)info_result.get("ok", false)) {
		return info_result;
	}
	Dictionary data = info_result.get("data", Dictionary());
	const Array requested = p_args.get("properties", Array());
	if (!requested.is_empty()) {
		Dictionary values;
		for (const Variant &value : requested) {
			Dictionary property_args;
			property_args["path"] = p_args.get("path", String());
			property_args["property"] = String(value);
			property_args["type_hint"] = p_args.get("type_hint", String());
			const Dictionary property_result = get_resource_property(property_args);
			if (!(bool)property_result.get("ok", false)) {
				return property_result;
			}
			const Dictionary property_data = property_result.get("data", Dictionary());
			Dictionary item;
			item["type"] = property_data.get("type", String());
			item["value"] = property_data.get("value", Variant());
			values[String(value)] = item;
		}
		data["properties"] = values;
	}
	return _ok(data);
}

Dictionary SolersResourceService::edit_resource(const Dictionary &p_args) const {
	const String action = String(p_args.get("action", String())).strip_edges();
	String path;
	String path_error;
	if (!_normalize_project_path(p_args.get("path", String()), path, path_error)) {
		return _error("INVALID_PATH", path_error);
	}

	Dictionary result;
	if (action == "create") {
		if (FileAccess::exists(path) || ResourceLoader::exists(path)) {
			return _error("RESOURCE_EXISTS", "resource.edit create never overwrites an existing resource.");
		}
		Dictionary create_args = p_args.duplicate(true);
		create_args["path"] = path;
		create_args.erase("action");
		create_args.erase("expected_sha256");
		result = create_resource(create_args);
	} else if (action == "update") {
		const String expected_sha256 = p_args.get("expected_sha256", String());
		if (expected_sha256.is_empty() || !FileAccess::exists(path) || FileAccess::get_sha256(path) != expected_sha256) {
			return _error("RESOURCE_CHANGED", "resource.edit update requires the current resource expected_sha256.");
		}
		Dictionary update_args;
		update_args["path"] = path;
		update_args["properties"] = p_args.get("properties", Dictionary());
		update_args["type_hint"] = p_args.get("type_hint", String());
		result = set_resource_property(update_args);
	} else {
		return _error("INVALID_ARGUMENT", "resource.edit action must be create or update.");
	}
	if (!(bool)result.get("ok", false)) {
		return result;
	}

	Error reload_error = OK;
	const Ref<Resource> reloaded = ResourceLoader::load(path, p_args.get("type_hint", String()), ResourceFormatLoader::CACHE_MODE_REPLACE, &reload_error);
	if (reloaded.is_null() || reload_error != OK) {
		return _error("RESOURCE_RELOAD_FAILED", vformat("Saved resource could not be reloaded (error %d).", reload_error));
	}
	Dictionary data = result.get("data", Dictionary());
	data["action"] = action;
	data["path"] = path;
	data["class_name"] = reloaded->get_class();
	data["sha256"] = FileAccess::get_sha256(path);
	data["reload_verified"] = true;
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

	const Variant initial_properties = p_args.get("properties", Dictionary());
	if (initial_properties.get_type() != Variant::DICTIONARY) {
		return _error("INVALID_ARGUMENT", "properties must be an object.");
	}
	const Dictionary properties = initial_properties;
	const Array property_names = properties.keys();
	for (int i = 0; i < property_names.size(); i++) {
		const StringName property = StringName(property_names[i]);
		Variant value;
		String error;
		if (!solers_coerce_property_value(resource.ptr(), property, properties[property_names[i]], value, error)) {
			return _error("INVALID_PROPERTY_VALUE", error);
		}
		bool valid = false;
		resource->set(property, value, &valid);
		if (!valid) {
			return _error("PROPERTY_SET_FAILED", vformat("Setting property '%s' failed on %s.", String(property), resource->get_class()));
		}
	}

	Error dir_err = DirAccess::make_dir_recursive_absolute(ProjectSettings::get_singleton()->globalize_path(path.get_base_dir()));
	if (dir_err != OK) {
		return _error("DIRECTORY_CREATE_FAILED", vformat("Failed to create parent directory, error code %d.", dir_err));
	}
	Error save_err = ResourceSaver::save(resource, path);
	if (save_err != OK) {
		return _error("RESOURCE_SAVE_FAILED", vformat("Failed to save resource, error code %d.", save_err));
	}
	EditorFileSystem *filesystem = Engine::get_singleton()->is_editor_hint() ? EditorFileSystem::get_singleton() : nullptr;
	if (filesystem && filesystem->get_filesystem() && !filesystem->is_scanning()) {
		filesystem->update_file(path);
	}
	Dictionary data = _solers_resource_data(resource, path);
	data["initialized_property_count"] = property_names.size();
	return _ok(data);
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
	data["value"] = _displayable(resource->get(property_sn));
	return _ok(data);
}

Dictionary SolersResourceService::set_resource_property(const Dictionary &p_args) const {
	const String path_arg = p_args.get("path", String());
	const String type_hint = p_args.get("type_hint", String());
	Dictionary properties;
	if (p_args.has("properties")) {
		if (p_args["properties"].get_type() != Variant::DICTIONARY || Dictionary(p_args["properties"]).is_empty()) {
			return _error("INVALID_ARGUMENT", "properties must be a non-empty object.");
		}
		if (p_args.has("property") || p_args.has("value")) {
			return _error("INVALID_ARGUMENT", "Use properties or property/value, not both.");
		}
		properties = p_args["properties"];
	} else {
		const String property = String(p_args.get("property", String())).strip_edges();
		if (property.is_empty() || !p_args.has("value")) {
			return _error("INVALID_ARGUMENT", "Provide a non-empty properties object or property and value.");
		}
		properties[property] = p_args["value"];
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

	const Array names = properties.keys();
	Array values;
	Array old_values;
	for (int i = 0; i < names.size(); i++) {
		const StringName property = StringName(names[i]);
		Variant value;
		String error;
		if (!solers_coerce_property_value(resource.ptr(), property, properties[names[i]], value, error)) {
			return _error("INVALID_PROPERTY_VALUE", error);
		}
		values.push_back(value);
		old_values.push_back(resource->get(property));
	}
	for (int i = 0; i < names.size(); i++) {
		const StringName property = StringName(names[i]);
		bool valid = false;
		resource->set(property, values[i], &valid);
		if (!valid || !_solers_property_matches(resource->get(property), values[i])) {
			for (int restore = 0; restore <= i; restore++) {
				resource->set(StringName(names[restore]), old_values[restore]);
			}
			return _error("PROPERTY_SET_FAILED", vformat("Setting property '%s' failed on %s.", String(property), resource->get_class()));
		}
	}
	Error save_err = ResourceSaver::save(resource, path);
	if (save_err != OK) {
		for (int i = 0; i < names.size(); i++) {
			resource->set(StringName(names[i]), old_values[i]);
		}
		return _error("RESOURCE_SAVE_FAILED", vformat("Failed to save resource, error code %d.", save_err));
	}
	EditorFileSystem *filesystem = Engine::get_singleton()->is_editor_hint() ? EditorFileSystem::get_singleton() : nullptr;
	if (filesystem && filesystem->get_filesystem() && !filesystem->is_scanning()) {
		filesystem->update_file(path);
	}

	Dictionary data = _solers_resource_data(resource, path);
	Dictionary updated;
	for (int i = 0; i < names.size(); i++) {
		updated[names[i]] = _displayable(resource->get(StringName(names[i])));
	}
	data["properties"] = updated;
	data["updated_property_count"] = names.size();
	if (names.size() == 1) {
		data["property"] = names[0];
		data["value"] = updated[names[0]];
	}
	return _ok(data);
}

Dictionary SolersResourceService::native_instantiate(const Dictionary &p_args) const {
	const String class_name = String(p_args.get("class_name", String())).strip_edges();
	if (class_name.is_empty()) {
		return _error("INVALID_ARGUMENT", "class_name is required.");
	}
	const StringName class_sn = StringName(class_name);
	if (!ClassDB::class_exists(class_sn) || !ClassDB::can_instantiate(class_sn) || !ClassDB::is_parent_class(class_sn, SNAME("RefCounted"))) {
		return _error("INVALID_REFCOUNTED_TYPE", vformat("Class is not an instantiable RefCounted type: %s", class_name));
	}
	RefCounted *object = Object::cast_to<RefCounted>(ClassDB::instantiate(class_sn));
	if (!object) {
		return _error("NATIVE_INSTANTIATION_FAILED", vformat("Failed to instantiate RefCounted type: %s", class_name), false);
	}
	retained_refcounted[object->get_instance_id()] = Ref<RefCounted>(object);
	return _ok(_native_object_handle(object));
}

Dictionary SolersResourceService::native_load(const Dictionary &p_args) const {
	const String path_arg = p_args.get("path", String());
	const String type_hint = p_args.get("type_hint", String());

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
	retained_refcounted[resource->get_instance_id()] = resource;

	Dictionary data = _native_object_handle(resource.ptr());
	data["path"] = path;
	return _ok(data);
}

Dictionary SolersResourceService::native_list_properties(const Dictionary &p_args) const {
	Object *object = nullptr;
	String error;
	if (!_resolve_native_object(p_args.get("object_id", Variant()), object, error)) {
		return _error("INVALID_OBJECT", error);
	}

	Array out;
	List<PropertyInfo> properties;
	object->get_property_list(&properties);
	for (const PropertyInfo &property : properties) {
		Dictionary item;
		item["name"] = property.name;
		item["type"] = Variant::get_type_name(property.type);
		item["class_name"] = property.class_name;
		item["usage"] = property.usage;
		out.push_back(item);
	}
	Dictionary data;
	data["object"] = _native_object_handle(object);
	data["properties"] = out;
	return _ok(data);
}

Dictionary SolersResourceService::native_get(const Dictionary &p_args) const {
	Object *object = nullptr;
	String error;
	if (!_resolve_native_object(p_args.get("object_id", Variant()), object, error)) {
		return _error("INVALID_OBJECT", error);
	}
	const String property = String(p_args.get("property", String())).strip_edges();
	if (property.is_empty()) {
		return _error("INVALID_ARGUMENT", "property is required.");
	}
	const StringName property_sn = StringName(property);
	PropertyInfo info;
	if (!_solers_find_property(object, property_sn, info)) {
		return _error("UNKNOWN_PROPERTY", vformat("Property '%s' is not exposed by %s.", property, object->get_class()));
	}

	Dictionary data;
	data["object"] = _native_object_handle(object);
	data["property"] = property;
	data["type"] = Variant::get_type_name(info.type);
	data["value"] = _displayable(object->get(property_sn));
	return _ok(data);
}

Dictionary SolersResourceService::native_set(const Dictionary &p_args) const {
	Object *object = nullptr;
	String error;
	if (!_resolve_native_object(p_args.get("object_id", Variant()), object, error)) {
		return _error("INVALID_OBJECT", error);
	}
	const String property = String(p_args.get("property", String())).strip_edges();
	if (property.is_empty()) {
		return _error("INVALID_ARGUMENT", "property is required.");
	}
	if (!p_args.has("value")) {
		return _error("INVALID_ARGUMENT", "value is required.");
	}

	const StringName property_sn = StringName(property);
	Variant value;
	if (!solers_coerce_property_value(object, property_sn, p_args["value"], value, error)) {
		return _error("INVALID_PROPERTY_VALUE", error);
	}
	bool valid = false;
	object->set(property_sn, value, &valid);
	if (!valid) {
		return _error("PROPERTY_SET_FAILED", vformat("Setting property '%s' failed on %s.", property, object->get_class()));
	}
	Dictionary data;
	data["object"] = _native_object_handle(object);
	data["property"] = property;
	data["value"] = _displayable(object->get(property_sn));
	return _ok(data);
}

Dictionary SolersResourceService::native_list_methods(const Dictionary &p_args) const {
	Object *object = nullptr;
	String error;
	if (!_resolve_native_object(p_args.get("object_id", Variant()), object, error)) {
		return _error("INVALID_OBJECT", error);
	}

	Array out;
	List<MethodInfo> methods;
	object->get_method_list(&methods);
	for (const MethodInfo &method : methods) {
		Dictionary item;
		item["name"] = method.name;
		item["return_type"] = Variant::get_type_name(method.return_val.type);
		item["arg_count"] = method.arguments.size();
		item["default_arg_count"] = method.default_arguments.size();
		out.push_back(item);
	}
	Dictionary data;
	data["object"] = _native_object_handle(object);
	data["methods"] = out;
	return _ok(data);
}

Dictionary SolersResourceService::native_call(const Dictionary &p_args) const {
	Object *object = nullptr;
	String error;
	if (!_resolve_native_object(p_args.get("object_id", Variant()), object, error)) {
		return _error("INVALID_OBJECT", error);
	}
	const String method = String(p_args.get("method", String())).strip_edges();
	const Array args = p_args.get("args", Array());
	if (method.is_empty()) {
		return _error("INVALID_ARGUMENT", "method is required.");
	}
	const StringName method_sn = StringName(method);
	MethodInfo method_info;
	bool found_method = false;
	List<MethodInfo> methods;
	object->get_method_list(&methods);
	for (const MethodInfo &candidate : methods) {
		if (candidate.name == method_sn) {
			method_info = candidate;
			found_method = true;
			break;
		}
	}
	if (!found_method) {
		return _error("UNKNOWN_METHOD", vformat("Method '%s' is not available on %s.", method, object->get_class()));
	}
	Variant ret;
	String error_code;
	if (!solers_call_method(object, method_info, args, ret, error_code, error)) {
		return _error(error_code, error);
	}

	Dictionary data;
	data["object"] = _native_object_handle(object);
	data["method"] = method;
	data["arg_count"] = args.size();
	data["result"] = _displayable(ret);
	return _ok(data);
}

Dictionary SolersResourceService::native_save(const Dictionary &p_args) const {
	Object *object = nullptr;
	String error;
	if (!_resolve_native_object(p_args.get("object_id", Variant()), object, error)) {
		return _error("INVALID_OBJECT", error);
	}
	Resource *resource = Object::cast_to<Resource>(object);
	if (!resource) {
		return _error("INVALID_RESOURCE", vformat("%s cannot be saved with ResourceSaver.", object->get_class()));
	}
	String path;
	if (!_normalize_project_path(p_args.get("path", String()), path, error)) {
		return _error("INVALID_PATH", error);
	}
	const Error dir_error = DirAccess::make_dir_recursive_absolute(ProjectSettings::get_singleton()->globalize_path(path.get_base_dir()));
	if (dir_error != OK) {
		return _error("DIRECTORY_CREATE_FAILED", vformat("Failed to create parent directory, error code %d.", dir_error));
	}
	const Error save_error = ResourceSaver::save(resource, path);
	if (save_error != OK) {
		return _error("RESOURCE_SAVE_FAILED", vformat("Failed to save resource '%s' (error %d).", path, (int)save_error));
	}
	EditorFileSystem *filesystem = Engine::get_singleton()->is_editor_hint() ? EditorFileSystem::get_singleton() : nullptr;
	if (filesystem && filesystem->get_filesystem() && !filesystem->is_scanning()) {
		filesystem->update_file(path);
	}
	Dictionary data = _native_object_handle(resource);
	data["path"] = path;
	return _ok(data);
}

Dictionary SolersResourceService::native_free(const Dictionary &p_args) const {
	Object *object = nullptr;
	String error;
	if (!_resolve_native_object(p_args.get("object_id", Variant()), object, error)) {
		return _error("INVALID_OBJECT", error);
	}
	const ObjectID id = object->get_instance_id();
	const bool had_refcounted = retained_refcounted.has(id);
	const bool had_temp = temp_nodes.has(id);
	if (had_refcounted) {
		retained_refcounted.erase(id);
	}
	if (had_temp) {
		Node *node = temp_nodes[id];
		temp_nodes.erase(id);
		memdelete(node);
	}

	Dictionary data;
	data["object_id"] = String::num_int64((int64_t)id);
	data["freed"] = had_refcounted || had_temp;
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
