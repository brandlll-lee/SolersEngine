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
#include "editor/export/editor_export.h"
#include "editor/file_system/editor_file_system.h"
#include "scene/animation/animation_player.h"
#include "scene/main/node.h"
#include "scene/resources/animation.h"
#include "scene/resources/packed_scene.h"

#include "modules/solers_ai/core/solers_geometry_facts.h"
#include "modules/solers_ai/core/solers_path_utils.h"
#include "modules/solers_ai/core/solers_tool.h"

void SolersResourceService::_bind_methods() {
}

// --- Actionable error candidates -------------------------------------------
// Every "unknown name" error carries the closest real names from the live
// authoritative source (property list, ClassDB, directory listing) so the
// model's next attempt is informed instead of a guess. Ranking is purely
// lexical: case-insensitive exact, prefix/substring containment, then Godot's
// String::similarity.

PackedStringArray solers_nearest_names(const String &p_needle, const PackedStringArray &p_candidates, int p_max) {
	struct ScoredName {
		String name;
		float score = 0.0f;
	};
	struct ScoredNameCompare {
		bool operator()(const ScoredName &p_a, const ScoredName &p_b) const {
			return p_a.score > p_b.score;
		}
	};
	const String needle = p_needle.strip_edges().to_lower();
	if (needle.is_empty()) {
		return PackedStringArray();
	}
	Vector<ScoredName> scored;
	for (const String &candidate : p_candidates) {
		const String lower = candidate.to_lower();
		ScoredName entry;
		entry.name = candidate;
		if (lower == needle) {
			entry.score = 3.0f;
		} else if (lower.begins_with(needle) || needle.begins_with(lower)) {
			entry.score = 2.0f + needle.similarity(lower);
		} else if (lower.contains(needle) || needle.contains(lower)) {
			entry.score = 1.0f + needle.similarity(lower);
		} else {
			entry.score = needle.similarity(lower);
			if (entry.score < 0.4f) {
				continue;
			}
		}
		scored.push_back(entry);
	}
	scored.sort_custom<ScoredNameCompare>();
	PackedStringArray result;
	for (int i = 0; i < scored.size() && i < p_max; i++) {
		result.push_back(scored[i].name);
	}
	return result;
}

String solers_property_suggestions(Object *p_object, const String &p_property) {
	if (!p_object) {
		return String();
	}
	PackedStringArray candidates;
	List<PropertyInfo> properties;
	p_object->get_property_list(&properties);
	for (const PropertyInfo &info : properties) {
		if ((info.usage & PROPERTY_USAGE_EDITOR) || (info.usage & PROPERTY_USAGE_STORAGE)) {
			candidates.push_back(info.name);
		}
	}
	// A missing property often exists as an accessor method (size -> get_size()).
	List<MethodInfo> methods;
	p_object->get_method_list(&methods);
	for (const MethodInfo &info : methods) {
		candidates.push_back(String(info.name) + "()");
	}
	const PackedStringArray nearest = solers_nearest_names(p_property, candidates, 5);
	if (nearest.is_empty()) {
		return String();
	}
	return vformat(" Closest members: %s.", String(", ").join(nearest));
}

String solers_class_suggestions(const String &p_class_name) {
	String hints;
	for (int i = 0; i < (int)Variant::VARIANT_MAX; i++) {
		if (Variant::get_type_name(Variant::Type(i)).to_lower() == p_class_name.strip_edges().to_lower()) {
			hints += vformat(" '%s' is a built-in Variant type, not an engine class; construct it as a value.", Variant::get_type_name(Variant::Type(i)));
			break;
		}
	}
	LocalVector<StringName> class_names;
	ClassDB::get_class_list(class_names);
	PackedStringArray candidates;
	for (const StringName &name : class_names) {
		candidates.push_back(String(name));
	}
	const PackedStringArray nearest = solers_nearest_names(p_class_name, candidates, 5);
	if (!nearest.is_empty()) {
		hints += vformat(" Closest classes: %s.", String(", ").join(nearest));
	}
	return hints;
}

String solers_file_suggestions(const String &p_res_path) {
	const String dir_path = p_res_path.get_base_dir();
	Ref<DirAccess> dir = DirAccess::open(dir_path);
	if (dir.is_null()) {
		return String();
	}
	PackedStringArray candidates;
	dir->list_dir_begin();
	for (String entry = dir->get_next(); !entry.is_empty(); entry = dir->get_next()) {
		if (!dir->current_is_dir()) {
			candidates.push_back(entry);
		}
	}
	dir->list_dir_end();
	const PackedStringArray nearest = solers_nearest_names(p_res_path.get_file(), candidates, 5);
	if (nearest.is_empty()) {
		return String();
	}
	return vformat(" Closest files in %s: %s.", dir_path, String(", ").join(nearest));
}

static bool _solers_resolve_object_handle(const Variant &p_object_id, Object *&r_object, String &r_error) {
	if (p_object_id.get_type() == Variant::DICTIONARY) {
		const Dictionary handle = p_object_id;
		return _solers_resolve_object_handle(handle.get("object_id", Variant()), r_object, r_error);
	}
	ObjectID object_id;
	if (!solers_object_id_from_variant(p_object_id, object_id)) {
		r_error = "object_id must be the non-zero decimal string returned by a native object tool.";
		return false;
	}
	r_object = ObjectDB::get_instance(object_id);
	if (!r_object) {
		r_error = vformat("No live Godot object for object_id %s.", solers_object_id_to_string(object_id));
		return false;
	}
	return true;
}

bool SolersResourceService::_resolve_native_object(const Variant &p_object_id, Object *&r_object, String &r_error) const {
	return _solers_resolve_object_handle(p_object_id, r_object, r_error);
}

Dictionary solers_native_object_handle(Object *p_object) {
	Dictionary data;
	data["kind"] = "godot_object";
	if (!p_object) {
		data["object_id"] = String();
		data["valid"] = false;
		return data;
	}
	data["valid"] = true;
	data["object_id"] = solers_object_id_to_string(p_object->get_instance_id());
	data["class_name"] = p_object->get_class();

	Resource *resource = Object::cast_to<Resource>(p_object);
	if (resource) {
		data["path"] = resource->get_path();
		data["resource_name"] = resource->get_name();
	}

	Node *node = Object::cast_to<Node>(p_object);
	if (node) {
		data["name"] = node->get_name();
		data["inside_tree"] = node->is_inside_tree();
		if (node->is_inside_tree()) {
			data["node_path"] = node->get_path();
		}
	}
	return data;
}

Variant solers_summarize_display_value(const Variant &p_value) {
	switch (p_value.get_type()) {
		case Variant::NIL:
		case Variant::BOOL:
		case Variant::INT:
		case Variant::FLOAT:
			return p_value;
		case Variant::STRING_NAME:
		case Variant::NODE_PATH:
			return String(p_value);
		case Variant::OBJECT: {
			Object *object = p_value;
			return solers_native_object_handle(object);
		}
		case Variant::ARRAY: {
			const Array values = p_value;
			if (values.size() > 64) {
				return vformat("<Array size=%d>", values.size());
			}
			Array out;
			for (int i = 0; i < values.size(); i++) {
				out.push_back(solers_summarize_display_value(values[i]));
			}
			return out;
		}
		case Variant::DICTIONARY: {
			const Dictionary values = p_value;
			if (values.size() > 64) {
				return vformat("<Dictionary size=%d>", values.size());
			}
			Dictionary out;
			for (const Variant *key = values.next(nullptr); key; key = values.next(key)) {
				out[String(*key)] = solers_summarize_display_value(values[*key]);
			}
			return out;
		}
		case Variant::PACKED_BYTE_ARRAY:
		case Variant::PACKED_INT32_ARRAY:
		case Variant::PACKED_INT64_ARRAY:
		case Variant::PACKED_FLOAT32_ARRAY:
		case Variant::PACKED_FLOAT64_ARRAY:
		case Variant::PACKED_STRING_ARRAY:
		case Variant::PACKED_VECTOR2_ARRAY:
		case Variant::PACKED_VECTOR3_ARRAY:
		case Variant::PACKED_VECTOR4_ARRAY:
		case Variant::PACKED_COLOR_ARRAY: {
			const int element_count = ((Array)p_value).size();
			if (element_count > 16) {
				return vformat("<%s size=%d>", Variant::get_type_name(p_value.get_type()), element_count);
			}
			Array out;
			const Array values = p_value;
			for (int i = 0; i < values.size(); i++) {
				out.push_back(solers_summarize_display_value(values[i]));
			}
			return out;
		}
		case Variant::STRING: {
			const String text = p_value;
			if (text.length() > 2048) {
				return text.left(2048) + vformat("... <truncated, %d chars total>", text.length());
			}
			return p_value;
		}
		default: {
			List<StringName> members;
			Variant::get_member_list(p_value.get_type(), &members);
			if (members.is_empty()) {
				return String(p_value);
			}
			Dictionary out;
			for (const StringName &member : members) {
				bool valid = false;
				const Variant value = p_value.get_named(member, valid);
				if (valid) {
					out[String(member)] = solers_summarize_display_value(value);
				}
			}
			return out;
		}
	}
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

Dictionary solers_variant_wire_shape(Variant::Type p_type) {
	List<StringName> member_names;
	Variant::get_member_list(p_type, &member_names);
	Dictionary members;
	for (const StringName &member_name : member_names) {
		if (Variant::get_member_validated_setter(p_type, member_name)) {
			members[member_name] = Variant::get_type_name(Variant::get_member_type(p_type, member_name));
		}
	}
	if (members.is_empty()) {
		return Dictionary();
	}
	return Dictionary({ { "encoding", "named_members" }, { "members", members } });
}

bool solers_decode_wire_variant(const Variant &p_value, Variant &r_out, String &r_error) {
	if (p_value.get_type() == Variant::ARRAY) {
		const Array source = p_value;
		Array decoded;
		decoded.resize(source.size());
		for (int i = 0; i < source.size(); i++) {
			if (!solers_decode_wire_variant(source[i], decoded[i], r_error)) {
				return false;
			}
		}
		r_out = decoded;
		return true;
	}
	if (p_value.get_type() != Variant::DICTIONARY) {
		r_out = p_value;
		return true;
	}

	const Dictionary source = p_value;
	if (source.has("class_name") && source.has("properties") && !source.has("object_id") && !source.has("path")) {
		const PropertyInfo resource_info(Variant::OBJECT, SNAME("value"), PROPERTY_HINT_RESOURCE_TYPE, String(), PROPERTY_USAGE_DEFAULT, Resource::get_class_static());
		return solers_coerce_variant_value(resource_info, source, r_out, r_error);
	}
	Dictionary decoded;
	for (const Variant *key = source.next(nullptr); key; key = source.next(key)) {
		Variant value = source[*key];
		if (!solers_decode_wire_variant(value, value, r_error)) {
			return false;
		}
		decoded[*key] = value;
	}
	r_out = decoded;
	return true;
}

static bool _solers_coerce_value(const PropertyInfo &p_info, const Variant &p_value, Variant &r_out, String &r_error) {
	if (p_info.type == Variant::NIL || ((p_info.type == Variant::ARRAY || p_info.type == Variant::DICTIONARY) && p_value.get_type() == p_info.type)) {
		return solers_decode_wire_variant(p_value, r_out, r_error);
	}
	if (p_info.type != Variant::OBJECT && p_value.get_type() == p_info.type) {
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
			const Dictionary handle = p_value;
			const String handle_path = String(handle.get("path", String())).strip_edges().replace_char('\\', '/').simplify_path();
			if (!handle.has("object_id") && handle.has("class_name")) {
				const StringName class_name = StringName(String(handle.get("class_name", String())).strip_edges());
				if (!ClassDB::class_exists(class_name) || !ClassDB::can_instantiate(class_name) || !ClassDB::is_parent_class(class_name, Resource::get_class_static())) {
					r_error = vformat("Class '%s' is not an instantiable Resource.", class_name);
					return false;
				}
				Ref<Resource> resource = Object::cast_to<Resource>(ClassDB::instantiate(class_name));
				const Variant properties_value = handle.get("properties", Dictionary());
				if (resource.is_null() || properties_value.get_type() != Variant::DICTIONARY) {
					r_error = vformat("Inline Resource '%s' requires a properties dictionary.", class_name);
					return false;
				}
				const Dictionary properties = properties_value;
				const Array names = properties.keys();
				for (int i = 0; i < names.size(); i++) {
					const StringName property = StringName(String(names[i]));
					Variant value;
					if (!solers_coerce_property_value(resource.ptr(), property, properties[names[i]], value, r_error)) {
						return false;
					}
					bool valid = false;
					resource->set(property, value, &valid);
					if (!valid) {
						r_error = vformat("Failed to set inline Resource property '%s.%s'.", class_name, property);
						return false;
					}
				}
				object = resource.ptr();
				object_value = resource;
			} else if (!handle.has("object_id") && handle_path.begins_with("res://")) {
				Error load_error = OK;
				const Ref<Resource> resource = ResourceLoader::load(handle_path, String(), ResourceFormatLoader::CACHE_MODE_REUSE, &load_error);
				if (resource.is_null() || load_error != OK) {
					r_error = vformat("Failed to load resource '%s' (error %d).", handle_path, (int)load_error);
					return false;
				}
				object = resource.ptr();
				object_value = resource;
			} else if (!_solers_resolve_object_handle(p_value, object, r_error)) {
				r_error += " Object-typed properties also accept a res:// path string.";
				return false;
			} else {
				object_value = object;
			}
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
	if (p_actual.is_null() && p_expected.is_null()) {
		return true;
	}
	if ((p_actual.get_type() == Variant::FLOAT || p_actual.get_type() == Variant::INT) &&
			(p_expected.get_type() == Variant::FLOAT || p_expected.get_type() == Variant::INT)) {
		return Math::is_equal_approx((double)p_actual, (double)p_expected);
	}
	return p_actual == p_expected;
}

bool solers_coerce_property_value(Object *p_object, const StringName &p_property, const Variant &p_value, Variant &r_out, String &r_error) {
	PropertyInfo info;
	if (!_solers_find_property(p_object, p_property, info)) {
		r_error = vformat("Property '%s' is not exposed by %s.%s", String(p_property), p_object->get_class(), solers_property_suggestions(p_object, String(p_property)));
		return false;
	}
	if (_solers_coerce_value(info, p_value, r_out, r_error)) {
		return true;
	}
	r_error = vformat("Property '%s': %s", String(p_property), r_error);
	return false;
}

bool solers_coerce_variant_value(const PropertyInfo &p_info, const Variant &p_value, Variant &r_out, String &r_error) {
	return _solers_coerce_value(p_info, p_value, r_out, r_error);
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

static void _solers_collect_animation_inventory(Node *p_node, Node *p_root, Array &r_players) {
	if (AnimationPlayer *player = Object::cast_to<AnimationPlayer>(p_node)) {
		Dictionary player_info;
		player_info["node_path"] = String(p_root->get_path_to(player));
		LocalVector<StringName> libraries;
		player->get_animation_library_list(&libraries);
		Array library_names;
		for (const StringName &name : libraries) {
			library_names.push_back(String(name));
		}
		player_info["libraries"] = library_names;
		LocalVector<StringName> names;
		player->get_animation_list(&names);
		Array clips;
		for (const StringName &name : names) {
			const Ref<Animation> animation = player->get_animation(name);
			if (animation.is_null()) {
				continue;
			}
			Dictionary clip;
			clip["name"] = String(name);
			clip["length"] = animation->get_length();
			clip["loop_mode"] = (int)animation->get_loop_mode();
			clip["track_count"] = animation->get_track_count();
			clips.push_back(clip);
		}
		player_info["clips"] = clips;
		r_players.push_back(player_info);
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		_solers_collect_animation_inventory(p_node->get_child(i), p_root, r_players);
	}
}

Dictionary SolersResourceService::get_resource_info(const Dictionary &p_args) const {
	const String path_arg = p_args.get("path", String());
	const bool include_dependencies = p_args.get("include_dependencies", true);
	const int max_dependencies = CLAMP((int)p_args.get("max_dependencies", 128), 0, 2048);

	const SolersPath::NormalizedPath normalized_path = SolersPath::normalize_project_path(path_arg);
	if (!normalized_path.valid) {
		return _error("INVALID_PATH", normalized_path.error);
	}
	const String path = normalized_path.value;

	Dictionary data;
	data["path"] = path;
	data["exists"] = FileAccess::exists(path) || ResourceLoader::exists(path);
	if (FileAccess::exists(path)) {
		data["sha256"] = FileAccess::get_sha256(path);
	}
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
				Array animation_players;
				_solers_collect_animation_inventory(root, root, animation_players);
				data["animation_players"] = animation_players;
				memdelete(root);
			}
		}
	}

	if (include_dependencies && (bool)data["exists"]) {
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
	const Array method_calls = p_args.get("method_calls", Array());
	if (!requested.is_empty() || !method_calls.is_empty()) {
		Error load_error = OK;
		const Ref<Resource> resource = ResourceLoader::load(data.get("path", String()), p_args.get("type_hint", String()), ResourceFormatLoader::CACHE_MODE_REUSE, &load_error);
		if (resource.is_null() || load_error != OK) {
			return _error("RESOURCE_LOAD_FAILED", vformat("Failed to load resource '%s' (error %d).", String(data.get("path", String())), (int)load_error));
		}
		if (!requested.is_empty()) {
			Dictionary values;
			Dictionary errors;
			for (const Variant &value : requested) {
				const StringName property = StringName(String(value));
				PropertyInfo property_info;
				if (!_solers_find_property(resource.ptr(), property, property_info)) {
					errors[String(value)] = _error("UNKNOWN_PROPERTY", vformat("Property '%s' is not exposed by %s.%s", String(value), resource->get_class(), solers_property_suggestions(resource.ptr(), String(value)))).get("error", Dictionary());
					continue;
				}
				Dictionary item;
				item["type"] = Variant::get_type_name(property_info.type);
				item["value"] = solers_summarize_display_value(resource->get(property));
				values[String(value)] = item;
			}
			data["properties"] = values;
			if (!errors.is_empty()) {
				data["property_errors"] = errors;
			}
		}
		Array method_results;
		for (const Variant &call_value : method_calls) {
			const Dictionary call = call_value;
			const StringName method = StringName(String(call.get("name", String())));
			const Array arguments = call.get("arguments", Array());
			const Dictionary result = _read_method(resource.ptr(), method, arguments);
			Dictionary item = result.get("data", Dictionary());
			if (!(bool)result.get("ok", false)) {
				item["method"] = method;
				item["arguments"] = arguments;
				item["error"] = result.get("error", Dictionary());
			}
			method_results.push_back(item);
		}
		if (!method_results.is_empty()) {
			data["method_results"] = method_results;
		}
	}
	return _ok(data);
}

Dictionary SolersResourceService::_read_method(Object *p_object, const StringName &p_method, const Array &p_arguments) const {
	MethodInfo info;
	if (!ClassDB::get_method_info(p_object->get_class_name(), p_method, &info)) {
		return _error("UNKNOWN_METHOD", vformat("Method is not exposed by ClassDB: %s.%s", p_object->get_class(), p_method));
	}
	if (!(info.flags & METHOD_FLAG_CONST) || (info.flags & METHOD_FLAG_VARARG)) {
		return _error("METHOD_NOT_READABLE", vformat("Method %s.%s must be const and non-vararg.", p_object->get_class(), p_method));
	}
	const int argument_count = info.arguments.size();
	const int required_argument_count = argument_count - info.default_arguments.size();
	if (p_arguments.size() < required_argument_count || p_arguments.size() > argument_count) {
		return _error("METHOD_ARGUMENT_COUNT", vformat("Method %s.%s requires %d to %d arguments; received %d.", p_object->get_class(), p_method, required_argument_count, argument_count, p_arguments.size()));
	}

	Vector<Variant> arguments;
	arguments.resize(argument_count);
	for (int i = 0; i < p_arguments.size(); i++) {
		String error;
		if (!solers_coerce_variant_value(info.arguments[i], p_arguments[i], arguments.write[i], error)) {
			return _error("METHOD_ARGUMENT_INVALID", vformat("Argument %d for %s.%s is invalid: %s", i, p_object->get_class(), p_method, error));
		}
	}
	for (int i = p_arguments.size(); i < argument_count; i++) {
		arguments.write[i] = info.default_arguments[i - required_argument_count];
	}
	Vector<const Variant *> argument_ptrs;
	argument_ptrs.resize(argument_count);
	for (int i = 0; i < argument_count; i++) {
		argument_ptrs.write[i] = &arguments[i];
	}

	Callable::CallError call_error;
	const Variant value = p_object->callp(p_method, (const Variant **)argument_ptrs.ptr(), argument_ptrs.size(), call_error);
	if (call_error.error != Callable::CallError::CALL_OK) {
		return _error("METHOD_CALL_FAILED", vformat("Godot rejected %s.%s with CallError %d.", p_object->get_class(), p_method, call_error.error));
	}
	return _ok(Dictionary({ { "method", p_method }, { "arguments", p_arguments }, { "type", Variant::get_type_name(value.get_type()) }, { "value", solers_summarize_display_value(value) } }));
}

Dictionary SolersResourceService::_create_resource(const Dictionary &p_args, const SolersToolContext *p_context) const {
	const String class_name = String(p_args.get("class_name", String())).strip_edges();
	if (class_name.is_empty()) {
		return _error("INVALID_ARGUMENT", "class_name is required.");
	}
	const String path = p_args.get("path", String());

	const StringName class_sn = StringName(class_name);
	if (!ClassDB::class_exists(class_sn) || !ClassDB::can_instantiate(class_sn) || !ClassDB::is_parent_class(class_sn, SNAME("Resource"))) {
		return _error("INVALID_RESOURCE_TYPE", vformat("Class is not an instantiable Resource type: %s.%s", class_name, solers_class_suggestions(class_name)));
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
	if (p_context) {
		const Dictionary denied = p_context->require_permission(SolersPermissionManager::PERMISSION_EDIT_FILES, p_args);
		if (!denied.is_empty()) {
			return denied;
		}
		const Dictionary current = get_resource_info(Dictionary({ { "path", path }, { "include_dependencies", false } }));
		if (!(bool)current.get("ok", false) || (bool)Dictionary(current.get("data", Dictionary())).get("exists", false)) {
			return _error("RESOURCE_STATE_CONFLICT", "The resource appeared while the edit was waiting for approval.");
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

Dictionary SolersResourceService::_set_resource_properties(const Dictionary &p_args, const SolersToolContext *p_context) const {
	const String path = p_args.get("path", String());
	const String type_hint = p_args.get("type_hint", String());
	if (p_args.get("properties", Variant()).get_type() != Variant::DICTIONARY || Dictionary(p_args["properties"]).is_empty()) {
		return _error("INVALID_ARGUMENT", "properties must be a non-empty object.");
	}
	const Dictionary properties = p_args["properties"];
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
	if (p_context) {
		const Dictionary denied = p_context->require_permission(SolersPermissionManager::PERMISSION_EDIT_FILES, p_args);
		if (!denied.is_empty()) {
			return denied;
		}
		const Dictionary current = get_resource_info(Dictionary({ { "path", path }, { "include_dependencies", false } }));
		const Dictionary expected = p_args.get("expected_state", Dictionary());
		const Dictionary actual = current.get("data", Dictionary());
		for (const char *field : { "exists", "sha256", "uid" }) {
			if (expected.has(field) && expected[field] != actual.get(field, Variant())) {
				return _error("RESOURCE_STATE_CONFLICT", vformat("The resource %s changed while the edit was waiting for approval.", field));
			}
		}
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
		updated[names[i]] = solers_summarize_display_value(resource->get(StringName(names[i])));
	}
	data["properties"] = updated;
	data["updated_property_count"] = names.size();
	return _ok(data);
}

Dictionary SolersResourceService::edit_resource(const Dictionary &p_args, const SolersToolContext *p_context) const {
	const SolersPath::NormalizedPath path = SolersPath::normalize_project_path(p_args.get("path", String()));
	if (!path.valid) {
		return _error("INVALID_PATH", path.error);
	}
	Dictionary args = p_args;
	args["path"] = path.value;
	const Dictionary expected = p_args.get("expected_state", Dictionary());
	const Dictionary current_result = get_resource_info(Dictionary({ { "path", path.value }, { "include_dependencies", false } }));
	if (!(bool)current_result.get("ok", false)) {
		return current_result;
	}
	const Dictionary current = current_result.get("data", Dictionary());
	for (const char *field : { "exists", "sha256", "uid" }) {
		if (expected.has(field) && expected[field] != current.get(field, Variant())) {
			Dictionary result = _error("STALE_RESOURCE_STATE", vformat("The resource %s changed; inspect it again before editing.", field));
			result["data"] = Dictionary({ { "expected", expected }, { "actual", current } });
			return result;
		}
	}

	Dictionary result = (bool)current.get("exists", false) ? _set_resource_properties(args, p_context) : _create_resource(args, p_context);
	if (!(bool)result.get("ok", false)) {
		return result;
	}
	Dictionary data = result.get("data", Dictionary());
	const Dictionary receipt = get_resource_info(Dictionary({ { "path", path.value }, { "include_dependencies", false } }));
	if ((bool)receipt.get("ok", false)) {
		data["state"] = receipt.get("data", Dictionary());
	}
	result["data"] = data;
	return result;
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
	data["object"] = solers_native_object_handle(object);
	data["properties"] = out;
	return _ok(data);
}

Dictionary SolersResourceService::native_get(const Dictionary &p_args) const {
	Object *object = nullptr;
	String error;
	if (!_resolve_native_object(p_args.get("object_id", Variant()), object, error)) {
		return _error("INVALID_OBJECT", error);
	}
	const StringName method = StringName(String(p_args.get("method", String())).strip_edges());
	if (!method.is_empty()) {
		Dictionary result = _read_method(object, method, p_args.get("arguments", Array()));
		if ((bool)result.get("ok", false)) {
			Dictionary data = result.get("data", Dictionary());
			data["object"] = solers_native_object_handle(object);
			result["data"] = data;
		}
		return result;
	}
	const String property = String(p_args.get("property", String())).strip_edges();
	if (property.is_empty()) {
		return _error("INVALID_ARGUMENT", "property is required.");
	}
	const StringName property_sn = StringName(property);
	PropertyInfo info;
	if (!_solers_find_property(object, property_sn, info)) {
		return _error("UNKNOWN_PROPERTY", vformat("Property '%s' is not exposed by %s.%s", property, object->get_class(), solers_property_suggestions(object, property)));
	}

	Dictionary data;
	data["object"] = solers_native_object_handle(object);
	data["property"] = property;
	data["type"] = Variant::get_type_name(info.type);
	data["value"] = solers_summarize_display_value(object->get(property_sn));
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
