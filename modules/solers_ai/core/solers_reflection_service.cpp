/**************************************************************************/
/*  solers_reflection_service.cpp                                         */
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

#include "solers_reflection_service.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/io/resource.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "core/math/geometry_3d.h"
#include "core/math/random_pcg.h"
#include "core/object/class_db.h"
#include "core/object/script_language.h"
#include "core/os/os.h"
#include "core/templates/hash_set.h"
#include "editor/doc/editor_help.h"
#include "editor/editor_interface.h"
#include "editor/editor_node.h"
#include "editor/editor_undo_redo_manager.h"

#include "modules/modules_enabled.gen.h"
#include "modules/solers_ai/core/solers_action_timeline.h"
#include "modules/solers_ai/core/solers_geometry_facts.h"
#include "modules/solers_ai/core/solers_resource_service.h"
#ifdef MODULE_CSG_ENABLED
#include "modules/csg/csg_shape.h"
#endif
#include "scene/3d/bone_attachment_3d.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/cpu_particles_3d.h"
#include "scene/3d/gpu_particles_3d.h"
#include "scene/3d/light_3d.h"
#include "scene/3d/lightmap_gi.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/multimesh_instance_3d.h"
#include "scene/3d/node_3d.h"
#include "scene/3d/skeleton_3d.h"
#include "scene/3d/visual_instance_3d.h"
#include "scene/3d/world_environment.h"
#include "scene/animation/animation_mixer.h"
#include "scene/animation/animation_node_state_machine.h"
#include "scene/animation/animation_player.h"
#include "scene/animation/animation_tree.h"
#include "scene/main/node.h"
#include "scene/resources/3d/primitive_meshes.h"
#include "scene/resources/3d/sky_material.h"
#include "scene/resources/camera_attributes.h"
#include "scene/resources/environment.h"
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"
#include "scene/resources/packed_scene.h"
#include "scene/resources/sky.h"
#include "servers/rendering/rendering_server.h"

static constexpr int SOLERS_CLASS_SEARCH_DEFAULT_CAP = 40;
static constexpr int SOLERS_CLASS_SEARCH_MAX_CAP = 200;

enum class SolersDocMemberKind {
	METHOD,
	PROPERTY,
	SIGNAL,
	CONSTANT,
};

static const DocData::ClassDoc *_solers_class_doc(const String &p_class_name) {
	return EditorHelp::get_doc_data() ? EditorHelp::get_doc_data()->class_list.getptr(p_class_name) : nullptr;
}

static String _solers_member_doc(const StringName &p_class, const String &p_member, SolersDocMemberKind p_kind, bool p_include_inherited) {
	StringName current = p_class;
	while (current != StringName()) {
		const DocData::ClassDoc *doc = _solers_class_doc(String(current));
		if (doc) {
			switch (p_kind) {
				case SolersDocMemberKind::METHOD:
					for (const DocData::MethodDoc &member : doc->methods) {
						if (member.name == p_member) {
							return member.description;
						}
					}
					break;
				case SolersDocMemberKind::PROPERTY:
					for (const DocData::PropertyDoc &member : doc->properties) {
						if (member.name == p_member) {
							return member.description;
						}
					}
					break;
				case SolersDocMemberKind::SIGNAL:
					for (const DocData::MethodDoc &member : doc->signals) {
						if (member.name == p_member) {
							return member.description;
						}
					}
					break;
				case SolersDocMemberKind::CONSTANT:
					for (const DocData::ConstantDoc &member : doc->constants) {
						if (member.name == p_member) {
							return member.description;
						}
					}
					break;
			}
		}
		if (!p_include_inherited) {
			break;
		}
		current = ClassDB::get_parent_class(current);
	}
	return String();
}

static bool _solers_doc_matches(const String &p_name, const String &p_description, const String &p_query) {
	if (p_query.is_empty()) {
		return true;
	}
	// Whitespace-separated tokens match independently (OR). A single token keeps
	// substring semantics; space-delimited member names from the model must not
	// be treated as one impossible compound needle.
	const String name_l = p_name.to_lower();
	const String desc_l = p_description.to_lower();
	const PackedStringArray tokens = p_query.split(" ", false);
	for (int i = 0; i < tokens.size(); i++) {
		const String token = String(tokens[i]).strip_edges();
		if (token.is_empty()) {
			continue;
		}
		if (name_l.contains(token) || desc_l.contains(token)) {
			return true;
		}
	}
	return false;
}

enum class SolersBatchOperationKind {
	UNKNOWN,
	CREATE_NODE,
	INSTANTIATE_SCENE,
	SET_PROPERTY,
	REPARENT,
	CONNECT_SIGNAL,
	ATTACH_SCRIPT,
	REMOVE_NODE,
};

static SolersBatchOperationKind _solers_batch_operation_kind(const String &p_name) {
	if (p_name == "create_node") {
		return SolersBatchOperationKind::CREATE_NODE;
	}
	if (p_name == "instantiate") {
		return SolersBatchOperationKind::INSTANTIATE_SCENE;
	}
	if (p_name == "set_property") {
		return SolersBatchOperationKind::SET_PROPERTY;
	}
	if (p_name == "reparent") {
		return SolersBatchOperationKind::REPARENT;
	}
	if (p_name == "connect_signal") {
		return SolersBatchOperationKind::CONNECT_SIGNAL;
	}
	if (p_name == "attach_script") {
		return SolersBatchOperationKind::ATTACH_SCRIPT;
	}
	if (p_name == "remove_node") {
		return SolersBatchOperationKind::REMOVE_NODE;
	}
	return SolersBatchOperationKind::UNKNOWN;
}

static Dictionary _solers_normalize_batch_operation(const Dictionary &p_operation, SolersBatchOperationKind p_kind) {
	Dictionary operation = p_operation.duplicate(true);
	switch (p_kind) {
		case SolersBatchOperationKind::SET_PROPERTY:
		case SolersBatchOperationKind::REPARENT:
		case SolersBatchOperationKind::ATTACH_SCRIPT:
		case SolersBatchOperationKind::REMOVE_NODE:
			if (!operation.has("node_path") && operation.has("path")) {
				operation["node_path"] = operation["path"];
			}
			break;
		default:
			break;
	}
	return operation;
}

static void _solers_add_scene_access(Array &r_accesses, const String &p_mode, const String &p_path) {
	Dictionary access;
	access["mode"] = p_mode;
	access["key"] = p_path == "*" ? String("*") : String("scene:") + (p_path.is_empty() ? String(".") : p_path);
	r_accesses.push_back(access);
}

static Array _solers_vector3_array(const Vector3 &p_vector) {
	Array values;
	values.push_back(p_vector.x);
	values.push_back(p_vector.y);
	values.push_back(p_vector.z);
	return values;
}

void SolersReflectionService::_bind_methods() {
	ClassDB::bind_method(D_METHOD("search_classes", "args"), &SolersReflectionService::search_classes);
	ClassDB::bind_method(D_METHOD("introspect_class", "args"), &SolersReflectionService::introspect_class);
	ClassDB::bind_method(D_METHOD("set_property", "args"), &SolersReflectionService::set_property);
	ClassDB::bind_method(D_METHOD("batch", "args"), &SolersReflectionService::batch);
	ClassDB::bind_method(D_METHOD("open_scene", "args"), &SolersReflectionService::open_scene);
}

Dictionary SolersReflectionService::_ok(const Variant &p_data) const {
	Dictionary result;
	result["ok"] = true;
	result["data"] = p_data;
	return result;
}

Dictionary SolersReflectionService::_error(const String &p_code, const String &p_message, bool p_recoverable) const {
	Dictionary error;
	error["code"] = p_code;
	error["message"] = p_message;
	error["recoverable"] = p_recoverable;

	Dictionary result;
	result["ok"] = false;
	result["error"] = error;
	return result;
}

Node *SolersReflectionService::_resolve_node(const String &p_node_path, String &r_error) const {
	EditorInterface *editor_interface = EditorInterface::get_singleton();
	if (!editor_interface) {
		r_error = "EditorInterface is not available.";
		return nullptr;
	}
	if (!EditorNode::get_singleton() || EditorNode::get_editor_data().get_edited_scene_count() <= 0) {
		r_error = "No edited scene root.";
		return nullptr;
	}
	Node *edited_root = EditorNode::get_singleton()->get_edited_scene();
	if (!edited_root) {
		r_error = "No edited scene root.";
		return nullptr;
	}
	String normalized_path = p_node_path.strip_edges();
	if (normalized_path.is_empty() || normalized_path == "." || normalized_path == String(edited_root->get_name())) {
		return edited_root;
	}
	if (!normalized_path.begins_with("/")) {
		normalized_path = normalized_path.trim_prefix("./");
		const String root_prefix = String(edited_root->get_name()) + "/";
		if (normalized_path.begins_with(root_prefix)) {
			normalized_path = normalized_path.trim_prefix(root_prefix);
		}
	}
	Node *node = nullptr;
	const NodePath node_path = NodePath(normalized_path);
	if (normalized_path.begins_with("/")) {
		node = Object::cast_to<Node>(edited_root->get_node_or_null(edited_root->get_path().rel_path_to(node_path)));
	} else {
		node = edited_root->get_node_or_null(node_path);
	}
	if (!node) {
		// Walk edited-root-relative segments. A leading "/" often means
		// SceneTree-absolute style (/KeyLight); if every segment resolves under
		// the edited root, that walk is authoritative — do not report NOT_FOUND.
		Node *deepest = edited_root;
		String missing_segment;
		const Vector<String> segments = normalized_path.trim_prefix("/").split("/", false);
		bool found_all = !segments.is_empty();
		for (const String &segment : segments) {
			Node *next = deepest->get_node_or_null(NodePath(segment));
			if (!next) {
				missing_segment = segment;
				found_all = false;
				break;
			}
			deepest = next;
		}
		if (found_all) {
			return deepest;
		}
		PackedStringArray child_names;
		for (int i = 0; i < deepest->get_child_count() && i < 64; i++) {
			child_names.push_back(String(deepest->get_child(i)->get_name()));
		}
		String detail = vformat(" Deepest existing ancestor: '%s'.", deepest == edited_root ? String(edited_root->get_name()) : String(edited_root->get_path_to(deepest)));
		const PackedStringArray nearest = solers_nearest_names(missing_segment, child_names, 5);
		if (!nearest.is_empty()) {
			detail += vformat(" Closest children: %s.", String(", ").join(nearest));
		} else if (!child_names.is_empty()) {
			detail += vformat(" Its children: %s.", String(", ").join(child_names.size() > 20 ? child_names.slice(0, 20) : child_names));
		} else {
			detail += " It has no children.";
		}
		r_error = vformat("Node not found: %s.%s", p_node_path, detail);
	}
	return node;
}

bool SolersReflectionService::_safe_node_path(Node *p_node, String &r_out) {
	Node *edited_root = (EditorNode::get_singleton() && EditorNode::get_editor_data().get_edited_scene_count() > 0) ? EditorNode::get_singleton()->get_edited_scene() : nullptr;
	if (edited_root && (p_node == edited_root || edited_root->is_ancestor_of(p_node))) {
		r_out = String(edited_root->get_path_to(p_node));
		return true;
	}
	if (p_node->is_inside_tree()) {
		r_out = String(p_node->get_path());
		return true;
	}
	r_out = String(p_node->get_name());
	return false;
}

bool SolersReflectionService::_coerce_value(Node *p_node, const StringName &p_property, const Variant &p_value, Variant &r_out, String &r_error) const {
	return solers_coerce_property_value(p_node, p_property, p_value, r_out, r_error);
}

static Dictionary _reflect_object_handle(Object *p_object) {
	Dictionary data;
	data["kind"] = "godot_object";
	if (!p_object) {
		data["object_id"] = String();
		data["valid"] = false;
		return data;
	}
	data["valid"] = true;
	data["object_id"] = String::num_int64((int64_t)p_object->get_instance_id());
	data["class_name"] = p_object->get_class();

	if (Resource *resource = Object::cast_to<Resource>(p_object)) {
		data["path"] = resource->get_path();
		data["resource_name"] = resource->get_name();
	}
	if (Node *node = Object::cast_to<Node>(p_object)) {
		data["name"] = node->get_name();
		data["inside_tree"] = node->is_inside_tree();
		if (node->is_inside_tree()) {
			data["node_path"] = node->get_path();
		}
	}
	return data;
}

static Variant _reflect_displayable(const Variant &p_value) {
	if (p_value.get_type() == Variant::OBJECT) {
		Object *object = p_value;
		return _reflect_object_handle(object);
	}
	if (p_value.get_type() == Variant::ARRAY) {
		Array in = p_value;
		if (in.size() > 64) {
			return vformat("<Array size=%d>", in.size());
		}
		Array out;
		for (int i = 0; i < in.size(); i++) {
			out.push_back(_reflect_displayable(in[i]));
		}
		return out;
	}
	if (p_value.get_type() == Variant::DICTIONARY) {
		Dictionary in = p_value;
		Array keys = in.keys();
		if (keys.size() > 64) {
			return vformat("<Dictionary size=%d>", keys.size());
		}
		Dictionary out;
		for (int i = 0; i < keys.size(); i++) {
			out[keys[i]] = _reflect_displayable(in[keys[i]]);
		}
		return out;
	}
	// Bulk Variant payloads (packed arrays, oversized strings) summarize by
	// type so one node dump can never displace working context.
	return solers_summarize_display_value(p_value);
}

bool SolersReflectionService::_apply_initial_properties(Node *p_node, const Dictionary &p_properties, Dictionary &r_applied, String &r_error) const {
	ERR_FAIL_NULL_V(p_node, false);
	for (const Variant *key = p_properties.next(nullptr); key; key = p_properties.next(key)) {
		const String property = String(*key).strip_edges();
		if (property.is_empty() || property.contains("/") || property.contains(":")) {
			r_error = vformat("Initial property must be a direct native property name: %s", property);
			return false;
		}
		const StringName property_name(property);
		Variant coerced;
		if (!_coerce_value(p_node, property_name, p_properties[*key], coerced, r_error)) {
			return false;
		}
		bool valid = false;
		p_node->set(property_name, coerced, &valid);
		const Variant actual = p_node->get(property_name);
		if (!valid || (coerced.get_type() == Variant::OBJECT && actual != coerced)) {
			r_error = vformat("Setting initial property '%s' failed on %s.", property, p_node->get_class());
			return false;
		}
		r_applied[property] = _reflect_displayable(actual);
	}
	if (r_applied.has("current") && bool(p_node->get("current"))) {
		if (Camera3D *camera = Object::cast_to<Camera3D>(p_node)) {
			camera->make_current();
		}
	}
	return true;
}

static Vector<StringName> _property_path_subnames(const String &p_property) {
	Vector<StringName> out;
	const Vector<String> parts = p_property.replace(":", "/").split("/");
	for (const String &part : parts) {
		if (!part.is_empty()) {
			out.push_back(StringName(part));
		}
	}
	return out;
}

Dictionary SolersReflectionService::search_classes(const Dictionary &p_args) {
	const String query = String(p_args.get("query", String())).strip_edges();
	const String inherits = String(p_args.get("inherits", String())).strip_edges();
	int max_results = int(p_args.get("max_results", SOLERS_CLASS_SEARCH_DEFAULT_CAP));
	max_results = CLAMP(max_results, 1, SOLERS_CLASS_SEARCH_MAX_CAP);

	if (query.is_empty() && inherits.is_empty()) {
		return _error("INVALID_ARGUMENT", "query or inherits is required.");
	}

	StringName inherits_sn;
	if (!inherits.is_empty()) {
		inherits_sn = StringName(inherits);
		if (!ClassDB::class_exists(inherits_sn)) {
			return _error("UNKNOWN_CLASS", vformat("Inherited engine class does not exist: %s.%s", inherits, solers_class_suggestions(inherits)));
		}
	}

	LocalVector<StringName> class_names;
	ClassDB::get_class_list(class_names);

	Vector<String> sorted;
	for (uint32_t i = 0; i < class_names.size(); i++) {
		sorted.push_back(String(class_names[i]));
	}
	sorted.sort();

	const String query_lower = query.to_lower();
	Array results;
	int matched_count = 0;
	bool truncated = false;

	for (int i = 0; i < sorted.size(); i++) {
		const String class_name = sorted[i];
		const StringName class_sn(class_name);
		if (!inherits.is_empty() && class_sn != inherits_sn && !ClassDB::is_parent_class(class_sn, inherits_sn)) {
			continue;
		}
		const DocData::ClassDoc *doc = _solers_class_doc(class_name);
		const String description = doc ? (doc->brief_description.is_empty() ? doc->description : doc->brief_description) : String();
		if (!_solers_doc_matches(class_name, description, query_lower)) {
			continue;
		}

		matched_count++;
		if (results.size() >= max_results) {
			truncated = true;
			continue;
		}

		Dictionary item;
		item["class_name"] = class_name;
		item["parent_class"] = String(ClassDB::get_parent_class(class_sn));
		item["can_instantiate"] = ClassDB::can_instantiate(class_sn);
		item["is_node"] = class_sn == SNAME("Node") || ClassDB::is_parent_class(class_sn, SNAME("Node"));
		item["is_resource"] = class_sn == SNAME("Resource") || ClassDB::is_parent_class(class_sn, SNAME("Resource"));
		if (!description.is_empty()) {
			item["description"] = description;
		}
		results.push_back(item);
	}

	Dictionary data;
	data["query"] = query;
	data["inherits"] = inherits;
	data["results"] = results;
	data["count"] = results.size();
	data["matched_count"] = matched_count;
	data["truncated"] = truncated;
	return _ok(data);
}

Dictionary SolersReflectionService::introspect_class(const Dictionary &p_args) {
	const String class_name = p_args.get("class_name", String());
	const bool include_inherited = p_args.get("include_inherited", true);
	const String member_query = String(p_args.get("member_query", String())).strip_edges().to_lower();
	const int member_cursor = MAX(0, (int)p_args.get("cursor", 0));
	const int max_members = CLAMP((int)p_args.get("max_members", 64), 1, 256);
	if (class_name.strip_edges().is_empty()) {
		return _error("INVALID_ARGUMENT", "class_name is required.");
	}
	const StringName class_sn = StringName(class_name);
	if (!ClassDB::class_exists(class_sn)) {
		return _error("UNKNOWN_CLASS", vformat("Engine class does not exist: %s.%s", class_name, solers_class_suggestions(class_name)));
	}
	const bool no_inheritance = !include_inherited;

	PackedStringArray all_member_names;
	List<MethodInfo> methods;
	ClassDB::get_method_list(class_sn, &methods, no_inheritance);
	for (const MethodInfo &method : methods) {
		all_member_names.push_back(String(method.name));
	}
	List<PropertyInfo> properties;
	ClassDB::get_property_list(class_sn, &properties, no_inheritance);
	for (const PropertyInfo &property : properties) {
		if (!(property.usage & PROPERTY_USAGE_EDITOR) && !(property.usage & PROPERTY_USAGE_STORAGE)) {
			continue;
		}
		all_member_names.push_back(String(property.name));
	}
	List<MethodInfo> signals;
	ClassDB::get_signal_list(class_sn, &signals, no_inheritance);
	for (const MethodInfo &signal : signals) {
		all_member_names.push_back(String(signal.name));
	}
	List<String> constants;
	ClassDB::get_integer_constant_list(class_sn, &constants, no_inheritance);
	for (const String &constant : constants) {
		all_member_names.push_back(constant);
	}

	Dictionary data;
	data["class_name"] = class_name;
	data["parent_class"] = String(ClassDB::get_parent_class(class_sn));
	data["can_instantiate"] = ClassDB::can_instantiate(class_sn);
	data["is_node"] = ClassDB::is_parent_class(class_sn, SNAME("Node"));
	data["member_query"] = member_query;
	data["member_count"] = all_member_names.size();
	data["cursor"] = member_cursor;

	// Lean default: names only. Typed signatures/docs require member_query.
	if (member_query.is_empty()) {
		PackedStringArray member_names;
		for (int i = member_cursor; i < MIN(member_cursor + max_members, all_member_names.size()); i++) {
			member_names.push_back(all_member_names[i]);
		}
		data["member_names"] = member_names;
		data["truncated"] = member_cursor + member_names.size() < all_member_names.size();
		if ((bool)data["truncated"]) {
			data["next_cursor"] = member_cursor + member_names.size();
		}
		return _ok(data);
	}

	Array methods_out;
	int matched_member_count = 0;
	int emitted_member_count = 0;
	auto take_member = [&]() {
		const bool take = matched_member_count >= member_cursor && emitted_member_count < max_members;
		matched_member_count++;
		if (take) {
			emitted_member_count++;
		}
		return take;
	};
	for (const MethodInfo &method : methods) {
		const String description = _solers_member_doc(class_sn, method.name, SolersDocMemberKind::METHOD, include_inherited);
		if (!_solers_doc_matches(method.name, description, member_query)) {
			continue;
		}
		if (!take_member()) {
			continue;
		}
		Dictionary md;
		md["name"] = method.name;
		md["return_type"] = Variant::get_type_name(method.return_val.type);
		md["return_class_name"] = String(method.return_val.class_name);
		md["return_hint_string"] = method.return_val.hint_string;
		Array args_out;
		for (const PropertyInfo &arg : method.arguments) {
			Dictionary ad;
			ad["name"] = arg.name;
			ad["type"] = Variant::get_type_name(arg.type);
			ad["class_name"] = String(arg.class_name);
			ad["hint_string"] = arg.hint_string;
			args_out.push_back(ad);
		}
		md["arguments"] = args_out;
		if (!description.is_empty()) {
			md["description"] = description;
		}
		methods_out.push_back(md);
	}

	Array properties_out;
	for (const PropertyInfo &property : properties) {
		if (!(property.usage & PROPERTY_USAGE_EDITOR) && !(property.usage & PROPERTY_USAGE_STORAGE)) {
			continue;
		}
		const String description = _solers_member_doc(class_sn, property.name, SolersDocMemberKind::PROPERTY, include_inherited);
		if (!_solers_doc_matches(property.name, description, member_query)) {
			continue;
		}
		if (!take_member()) {
			continue;
		}
		Dictionary pd;
		pd["name"] = property.name;
		pd["type"] = Variant::get_type_name(property.type);
		pd["class_name"] = String(property.class_name);
		pd["hint"] = (int)property.hint;
		pd["hint_string"] = property.hint_string;
		pd["usage"] = (int64_t)property.usage;
		if (!description.is_empty()) {
			pd["description"] = description;
		}
		properties_out.push_back(pd);
	}

	Array signals_out;
	for (const MethodInfo &signal : signals) {
		const String description = _solers_member_doc(class_sn, signal.name, SolersDocMemberKind::SIGNAL, include_inherited);
		if (!_solers_doc_matches(signal.name, description, member_query)) {
			continue;
		}
		if (!take_member()) {
			continue;
		}
		Dictionary sd;
		sd["name"] = signal.name;
		Array args_out;
		for (const PropertyInfo &arg : signal.arguments) {
			Dictionary ad;
			ad["name"] = arg.name;
			ad["type"] = Variant::get_type_name(arg.type);
			ad["class_name"] = String(arg.class_name);
			ad["hint_string"] = arg.hint_string;
			args_out.push_back(ad);
		}
		sd["arguments"] = args_out;
		if (!description.is_empty()) {
			sd["description"] = description;
		}
		signals_out.push_back(sd);
	}

	Dictionary constants_out;
	for (const String &constant : constants) {
		const String description = _solers_member_doc(class_sn, constant, SolersDocMemberKind::CONSTANT, include_inherited);
		if (!_solers_doc_matches(constant, description, member_query)) {
			continue;
		}
		if (!take_member()) {
			continue;
		}
		if (description.is_empty()) {
			constants_out[constant] = ClassDB::get_integer_constant(class_sn, StringName(constant));
		} else {
			Dictionary constant_data;
			constant_data["value"] = ClassDB::get_integer_constant(class_sn, StringName(constant));
			constant_data["description"] = description;
			constants_out[constant] = constant_data;
		}
	}

	const DocData::ClassDoc *doc = _solers_class_doc(class_name);
	if (doc) {
		data["brief_description"] = doc->brief_description;
		data["description"] = doc->description;
	}
	data["methods"] = methods_out;
	data["properties"] = properties_out;
	data["signals"] = signals_out;
	data["constants"] = constants_out;
	data["matched_member_count"] = matched_member_count;
	data["truncated"] = member_cursor + emitted_member_count < matched_member_count;
	if ((bool)data["truncated"]) {
		data["next_cursor"] = member_cursor + emitted_member_count;
	}

	Array unmatched_tokens;
	const PackedStringArray tokens = member_query.split(" ", false);
	for (int i = 0; i < tokens.size(); i++) {
		const String token = String(tokens[i]).strip_edges().to_lower();
		if (token.is_empty()) {
			continue;
		}
		bool hit = false;
		for (int j = 0; j < all_member_names.size(); j++) {
			if (all_member_names[j].to_lower().contains(token)) {
				hit = true;
				break;
			}
		}
		if (hit) {
			continue;
		}
		Dictionary unmatched;
		unmatched["token"] = token;
		unmatched["nearest_members"] = solers_nearest_names(token, all_member_names, 5);
		PackedStringArray classes_with_member;
		const StringName exact_member(token);
		LocalVector<StringName> class_list;
		ClassDB::get_class_list(class_list);
		for (const StringName &other_class : class_list) {
			if (other_class == class_sn) {
				continue;
			}
			if (ClassDB::has_property(other_class, exact_member) || ClassDB::has_method(other_class, exact_member)) {
				classes_with_member.push_back(String(other_class));
				if (classes_with_member.size() >= 8) {
					break;
				}
			}
		}
		unmatched["classes_with_member"] = classes_with_member;
		unmatched_tokens.push_back(unmatched);
	}
	data["unmatched_member_query_tokens"] = unmatched_tokens;

	return _ok(data);
}

Dictionary SolersReflectionService::set_property(const Dictionary &p_args) {
	const String node_path = p_args.get("node_path", ".");
	const String property = p_args.get("property", String());
	if (property.strip_edges().is_empty()) {
		return _error("INVALID_ARGUMENT", "property is required.");
	}
	if (!p_args.has("value")) {
		return _error("INVALID_ARGUMENT", "value is required.");
	}

	String error;
	Node *node = _resolve_node(node_path, error);
	if (!node) {
		return _error("NODE_NOT_FOUND", error);
	}

	if (property.find("/") >= 0 || property.find(":") >= 0) {
		const String normalized = property.replace(":", "/");
		const Vector<StringName> subnames = _property_path_subnames(property);
		bool valid = false;
		const Variant old_value = node->get_indexed(subnames, &valid);
		if (!valid) {
			return _error("INVALID_PROPERTY_PATH", vformat("Property path '%s' is not valid on %s. Use '/' for nested resource properties, e.g. environment/ambient_light_energy.", normalized, node->get_class()));
		}

		EditorInterface *editor_interface = EditorInterface::get_singleton();
		ERR_FAIL_NULL_V(editor_interface, _error("EDITOR_INTERFACE_UNAVAILABLE", "EditorInterface is not available.", false));
		EditorUndoRedoManager *undo_redo = editor_interface->get_editor_undo_redo();
		ERR_FAIL_NULL_V(undo_redo, _error("UNDO_REDO_UNAVAILABLE", "EditorUndoRedoManager is not available.", false));

		const NodePath property_path = NodePath(Vector<StringName>(), subnames, false);
		if (!batch_action_active) {
			undo_redo->create_action(vformat("Solers: Set %s.%s", node->get_class(), normalized), UndoRedo::MERGE_DISABLE, node);
		} else {
			bool set_valid = false;
			node->set_indexed(subnames, p_args["value"], &set_valid);
			if (!set_valid) {
				return _error("PROPERTY_SET_FAILED", vformat("Setting property path '%s' failed on %s.", normalized, node->get_class()));
			}
		}
		undo_redo->add_do_method(node, "set_indexed", property_path, p_args["value"]);
		undo_redo->add_undo_method(node, "set_indexed", property_path, old_value);
		if (!batch_action_active) {
			undo_redo->commit_action();
		}

		String safe_path;
		_safe_node_path(node, safe_path);
		Dictionary data;
		data["node_path"] = safe_path;
		data["property"] = normalized;
		data["value"] = _reflect_displayable(node->get_indexed(subnames));
		return _ok(data);
	}

	const StringName property_sn = StringName(property);
	Variant coerced;
	if (!_coerce_value(node, property_sn, p_args["value"], coerced, error)) {
		return _error("INVALID_PROPERTY_VALUE", error);
	}

	EditorInterface *editor_interface = EditorInterface::get_singleton();
	ERR_FAIL_NULL_V(editor_interface, _error("EDITOR_INTERFACE_UNAVAILABLE", "EditorInterface is not available.", false));
	EditorUndoRedoManager *undo_redo = editor_interface->get_editor_undo_redo();
	ERR_FAIL_NULL_V(undo_redo, _error("UNDO_REDO_UNAVAILABLE", "EditorUndoRedoManager is not available.", false));

	const Variant old_value = node->get(property_sn);
	bool valid = false;
	node->set(property_sn, coerced, &valid);
	if (!valid || (coerced.get_type() == Variant::OBJECT && node->get(property_sn) != coerced)) {
		node->set(property_sn, old_value);
		return _error("PROPERTY_SET_FAILED", vformat("Setting property '%s' failed on %s.", property, node->get_class()));
	}

	if (!batch_action_active) {
		undo_redo->create_action(vformat("Solers: Set %s.%s", node->get_class(), property), UndoRedo::MERGE_DISABLE, node);
	}
	undo_redo->add_do_property(node, property_sn, coerced);
	undo_redo->add_undo_property(node, property_sn, old_value);
	if (!batch_action_active) {
		undo_redo->commit_action(false);
	}

	if (property == "current" && coerced && Object::cast_to<Camera3D>(node)) {
		Camera3D *camera = Object::cast_to<Camera3D>(node);
		camera->make_current();
	}

	String safe_path;
	_safe_node_path(node, safe_path);
	Dictionary data;
	data["node_path"] = safe_path;
	data["property"] = property;
	data["value"] = _reflect_displayable(node->get(property_sn));
	return _ok(data);
}

Dictionary SolersReflectionService::inspect_nodes(const Dictionary &p_args) {
	Array paths = p_args.get("node_paths", Array());
	if (paths.is_empty()) {
		paths.push_back(".");
	}
	// Property dumps are the single largest observation payload, so they are
	// opt-in: identity, class, and structure answer most inspect calls.
	const bool include_properties = p_args.get("include_properties", false);
	const bool include_connections = p_args.get("include_connections", false);
	const int max_properties = CLAMP((int)p_args.get("max_properties", 128), 1, 512);
	Array nodes;
	for (const Variant &value : paths) {
		const String path = String(value);
		String resolve_error;
		Node *node = _resolve_node(path, resolve_error);
		if (!node) {
			return _error("NODE_NOT_FOUND", resolve_error);
		}
		Dictionary item;
		String safe_path;
		_safe_node_path(node, safe_path);
		item["node_path"] = safe_path;
		item["object_id"] = String::num_int64((int64_t)node->get_instance_id());
		item["name"] = node->get_name();
		item["class_name"] = node->get_class();
		item["child_count"] = node->get_child_count();
		item["scene_file_path"] = node->get_scene_file_path();
		// Measurable facts by default: where the node actually is, how big it
		// actually is, and whether it is actually drawn. These are what decide
		// a spatial question; an image can only suggest an answer.
		const Dictionary spatial = _spatial_facts(Object::cast_to<Node3D>(node));
		if (!spatial.is_empty()) {
			item.merge(spatial);
		}
		const Dictionary subsystem = _subsystem_facts(node);
		if (!subsystem.is_empty()) {
			item.merge(subsystem);
		}
		const String instance_scene = _instance_scene_path(node);
		if (!instance_scene.is_empty()) {
			item["instance_scene_path"] = instance_scene;
			item["owned_by_instance"] = true;
		}
		if (include_properties) {
			Dictionary property_args;
			property_args["node_path"] = safe_path;
			property_args["max_properties"] = max_properties;
			const Dictionary property_result = _list_properties(property_args);
			if (!(bool)property_result.get("ok", false)) {
				return property_result;
			}
			item["properties"] = Dictionary(property_result.get("data", Dictionary())).get("properties", Array());
		}
		if (include_connections) {
			Dictionary connection_args;
			connection_args["source_path"] = safe_path;
			const Dictionary connection_result = _list_signal_connections(connection_args);
			if (!(bool)connection_result.get("ok", false)) {
				return connection_result;
			}
			item["connections"] = Dictionary(connection_result.get("data", Dictionary())).get("connections", Array());
		}
		nodes.push_back(item);
	}
	Dictionary data;
	data["nodes"] = nodes;
	data["count"] = nodes.size();
	return _ok(data);
}

Dictionary SolersReflectionService::_create_node(const Dictionary &p_args) {
	String parent_path = p_args.get("parent_path", String());
	if (parent_path.is_empty()) {
		parent_path = p_args.get("parent", ".");
	}
	const String type = p_args.get("class_name", p_args.get("type", "Node"));
	const String requested_name = p_args.get("name", String());
	const StringName type_sn = StringName(type);
	if (!ClassDB::class_exists(type_sn) || !ClassDB::can_instantiate(type_sn) || !ClassDB::is_parent_class(type_sn, SNAME("Node"))) {
		return _error("INVALID_NODE_TYPE", vformat("Class is not an instantiable Node type: %s.%s", type, solers_class_suggestions(type)));
	}

	EditorInterface *editor_interface = EditorInterface::get_singleton();
	ERR_FAIL_NULL_V(editor_interface, _error("EDITOR_INTERFACE_UNAVAILABLE", "EditorInterface is not available.", false));
	EditorNode *editor_node = EditorNode::get_singleton();
	if (!editor_node || EditorNode::get_editor_data().get_edited_scene_count() <= 0) {
		return _error("EDITED_SCENE_UNAVAILABLE", "The editor has no scene tab in which to create a node.");
	}
	Node *edited_root = editor_node->get_edited_scene();
	const bool create_root = !edited_root && (parent_path.is_empty() || parent_path == ".");
	String error;
	Node *parent = create_root ? nullptr : _resolve_node(parent_path, error);
	if (!create_root && !parent) {
		return _error("PARENT_NODE_NOT_FOUND", error);
	}
	const Variant properties_value = p_args.get("properties", Dictionary());
	if (properties_value.get_type() != Variant::DICTIONARY) {
		return _error("INVALID_ARGUMENT", "create_node properties must be an object.");
	}
	const Dictionary properties = properties_value;

	Object *object = ClassDB::instantiate(type_sn);
	Node *node = Object::cast_to<Node>(object);
	if (!node) {
		if (object) {
			memdelete(object);
		}
		return _error("NODE_INSTANTIATION_FAILED", vformat("Failed to instantiate node type: %s", type), false);
	}
	if (!requested_name.is_empty()) {
		node->set_name(requested_name);
	}

	Dictionary applied_properties;
	if (!_apply_initial_properties(node, properties, applied_properties, error)) {
		memdelete(node);
		return _error("INVALID_PROPERTY_VALUE", error);
	}

	EditorUndoRedoManager *undo_redo = editor_interface->get_editor_undo_redo();
	ERR_FAIL_NULL_V(undo_redo, _error("UNDO_REDO_UNAVAILABLE", "EditorUndoRedoManager is not available.", false));

	if (create_root) {
		if (!batch_action_active) {
			undo_redo->create_action_for_history("Solers: New Scene Root", EditorNode::get_editor_data().get_current_edited_scene_history_id());
		}
		undo_redo->add_do_method(editor_node, "set_edited_scene", node);
		undo_redo->add_do_reference(node);
		undo_redo->add_undo_method(editor_node, "set_edited_scene", (Object *)nullptr);
		if (batch_action_active) {
			editor_node->set_edited_scene(node);
		} else {
			undo_redo->commit_action();
		}
		if (editor_interface->get_edited_scene_root() != node) {
			return _error("SCENE_ROOT_CREATION_FAILED", "Godot did not accept the new edited scene root.", false);
		}
		Dictionary data;
		data["type"] = type;
		data["name"] = node->get_name();
		data["path"] = ".";
		data["root"] = true;
		data["properties"] = applied_properties;
		data["initialized_property_count"] = applied_properties.size();
		return _ok(data);
	}

	if (!batch_action_active) {
		undo_redo->create_action(vformat("Solers: Add %s", type), UndoRedo::MERGE_DISABLE, parent);
	}
	undo_redo->add_do_method(parent, "add_child", node, true);
	undo_redo->add_do_method(node, "set_owner", edited_root);
	undo_redo->add_do_reference(node);
	undo_redo->add_undo_method(parent, "remove_child", node);
	if (batch_action_active) {
		parent->add_child(node, true);
		node->set_owner(edited_root);
	} else {
		undo_redo->commit_action();
	}

	String safe_path;
	_safe_node_path(node, safe_path);
	Dictionary data;
	data["type"] = type;
	data["name"] = node->get_name();
	data["path"] = safe_path;
	data["properties"] = applied_properties;
	data["initialized_property_count"] = applied_properties.size();
	return _ok(data);
}

static bool _solers_scene_contains_path(Node *p_node, const String &p_scene_path) {
	if (!p_node) {
		return false;
	}
	if (p_node->get_scene_file_path() == p_scene_path) {
		return true;
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		if (_solers_scene_contains_path(p_node->get_child(i), p_scene_path)) {
			return true;
		}
	}
	return false;
}

Dictionary SolersReflectionService::instantiate_scene(const Dictionary &p_args) {
	const String resource_path = String(p_args.get("resource_path", String())).strip_edges().replace_char('\\', '/').simplify_path();
	const String parent_path = String(p_args.get("parent_path", ".")).strip_edges();
	const String requested_name = String(p_args.get("name", String())).strip_edges();
	if (!resource_path.begins_with("res://")) {
		return _error("INVALID_RESOURCE_PATH", "resource_path must be a loadable res:// scene or mesh resource.");
	}
	const Variant properties_value = p_args.get("properties", Dictionary());
	if (properties_value.get_type() != Variant::DICTIONARY) {
		return _error("INVALID_ARGUMENT", "object.transaction scene instantiate properties must be an object.");
	}

	EditorInterface *editor_interface = EditorInterface::get_singleton();
	EditorNode *editor_node = EditorNode::get_singleton();
	if (!editor_interface || !editor_node || !editor_node->get_edited_scene()) {
		return _error("EDITED_SCENE_UNAVAILABLE", "The editor has no active scene in which to instantiate the resource.", false);
	}
	Node *edited_root = editor_node->get_edited_scene();
	String resolve_error;
	Node *parent = _resolve_node(parent_path, resolve_error);
	if (!parent) {
		return _error("PARENT_NODE_NOT_FOUND", resolve_error);
	}

	Error load_error = OK;
	Ref<Resource> resource = ResourceLoader::load(resource_path, String(), ResourceFormatLoader::CACHE_MODE_REUSE, &load_error);
	Ref<PackedScene> scene = resource;
	Ref<Mesh> mesh = resource;
	if (resource.is_null() || load_error != OK || (scene.is_null() && mesh.is_null())) {
		return _error("UNSUPPORTED_INSTANTIABLE_RESOURCE", vformat("Failed to load '%s' as a PackedScene or Mesh (error %d).", resource_path, (int)load_error));
	}
	Node *instance = nullptr;
	if (scene.is_valid()) {
		instance = scene->instantiate(PackedScene::GEN_EDIT_STATE_INSTANCE);
	} else {
		MeshInstance3D *mesh_instance = memnew(MeshInstance3D);
		mesh_instance->set_mesh(mesh);
		instance = mesh_instance;
	}
	if (!instance) {
		return _error("RESOURCE_INSTANTIATION_FAILED", vformat("Resource produced no scene node: %s", resource_path), false);
	}
	const String edited_scene_path = edited_root->get_scene_file_path();
	if (scene.is_valid() && !edited_scene_path.is_empty() && _solers_scene_contains_path(instance, edited_scene_path)) {
		memdelete(instance);
		return _error("CYCLIC_SCENE_DEPENDENCY", vformat("Instantiating '%s' would create a cyclic scene dependency.", resource_path));
	}
	if (!requested_name.is_empty()) {
		instance->set_name(requested_name);
	}
	Dictionary applied_properties;
	String property_error;
	if (!_apply_initial_properties(instance, properties_value, applied_properties, property_error)) {
		memdelete(instance);
		return _error("INVALID_PROPERTY_VALUE", property_error);
	}
	if (scene.is_valid()) {
		instance->set_scene_file_path(ProjectSettings::get_singleton()->localize_path(resource_path));
	}

	EditorUndoRedoManager *undo_redo = editor_interface->get_editor_undo_redo();
	if (!undo_redo) {
		memdelete(instance);
		return _error("UNDO_REDO_UNAVAILABLE", "EditorUndoRedoManager is not available.", false);
	}
	if (!batch_action_active) {
		undo_redo->create_action(vformat("Solers: Instantiate %s", resource_path.get_file()), UndoRedo::MERGE_DISABLE, parent);
	}
	undo_redo->add_do_method(parent, "add_child", instance, true);
	undo_redo->add_do_method(instance, "set_owner", edited_root);
	undo_redo->add_do_reference(instance);
	undo_redo->add_undo_method(parent, "remove_child", instance);
	if (batch_action_active) {
		parent->add_child(instance, true);
		instance->set_owner(edited_root);
	} else {
		undo_redo->commit_action();
	}

	String safe_path;
	_safe_node_path(instance, safe_path);
	Dictionary data;
	data["resource_path"] = resource_path;
	data["path"] = safe_path;
	data["name"] = instance->get_name();
	data["type"] = instance->get_class();
	data["resource_type"] = scene.is_valid() ? String("PackedScene") : String("Mesh");
	data["properties"] = applied_properties;
	data["initialized_property_count"] = applied_properties.size();
	data["geometry"] = solers_describe_geometry(instance, true);
	return _ok(data);
}

Dictionary SolersReflectionService::_reparent_node(const Dictionary &p_args) {
	const String node_path = p_args.get("node_path", String());
	String new_parent_path = p_args.get("new_parent_path", String());
	if (new_parent_path.is_empty()) {
		new_parent_path = p_args.get("new_parent", String());
	}
	const int position = p_args.get("position", -1);
	if (node_path.is_empty() || node_path == ".") {
		return _error("INVALID_ARGUMENT", "Refusing to reparent the edited scene root.");
	}

	String error;
	Node *node = _resolve_node(node_path, error);
	if (!node) {
		return _error("NODE_NOT_FOUND", error);
	}
	Node *new_parent = _resolve_node(new_parent_path, error);
	if (!new_parent) {
		return _error("PARENT_NODE_NOT_FOUND", error);
	}
	Node *old_parent = node->get_parent();
	if (!old_parent) {
		return _error("INVALID_ARGUMENT", "Node has no parent and cannot be reparented.");
	}
	if (node == new_parent || node->is_ancestor_of(new_parent)) {
		return _error("INVALID_ARGUMENT", "Cannot reparent a node to itself or to one of its descendants.");
	}

	EditorInterface *editor_interface = EditorInterface::get_singleton();
	ERR_FAIL_NULL_V(editor_interface, _error("EDITOR_INTERFACE_UNAVAILABLE", "EditorInterface is not available.", false));
	Node *edited_root = editor_interface->get_edited_scene_root();
	EditorUndoRedoManager *undo_redo = editor_interface->get_editor_undo_redo();
	ERR_FAIL_NULL_V(undo_redo, _error("UNDO_REDO_UNAVAILABLE", "EditorUndoRedoManager is not available.", false));

	const int old_index = node->get_index(false);
	const int new_index = position < 0 ? -1 : CLAMP(position, 0, new_parent->get_child_count(false));
	Node *old_owner = node->get_owner();
	if (!batch_action_active) {
		undo_redo->create_action("Solers: Reparent Node", UndoRedo::MERGE_DISABLE, new_parent);
	}
	undo_redo->add_do_method(old_parent, "remove_child", node);
	undo_redo->add_do_method(new_parent, "add_child", node, true);
	if (new_index >= 0) {
		undo_redo->add_do_method(new_parent, "move_child", node, new_index);
	}
	undo_redo->add_do_method(node, "set_owner", edited_root);
	if (batch_action_active) {
		// The batch action reverses operation groups on commit. Register each
		// group's undo methods backwards so their local order stays intact.
		undo_redo->add_undo_method(node, "set_owner", old_owner);
		undo_redo->add_undo_method(old_parent, "move_child", node, old_index);
		undo_redo->add_undo_method(old_parent, "add_child", node, true);
		undo_redo->add_undo_method(new_parent, "remove_child", node);
	} else {
		undo_redo->add_undo_method(new_parent, "remove_child", node);
		undo_redo->add_undo_method(old_parent, "add_child", node, true);
		undo_redo->add_undo_method(old_parent, "move_child", node, old_index);
		undo_redo->add_undo_method(node, "set_owner", old_owner);
	}
	if (batch_action_active) {
		old_parent->remove_child(node);
		new_parent->add_child(node, true);
		if (new_index >= 0) {
			new_parent->move_child(node, new_index);
		}
		node->set_owner(edited_root);
	} else {
		undo_redo->commit_action();
	}

	String node_safe_path;
	String parent_safe_path;
	_safe_node_path(node, node_safe_path);
	_safe_node_path(new_parent, parent_safe_path);
	Dictionary data;
	data["node_path"] = node_safe_path;
	data["new_parent_path"] = parent_safe_path;
	data["old_index"] = old_index;
	data["new_index"] = new_index;
	return _ok(data);
}

Dictionary SolersReflectionService::_connect_signal(const Dictionary &p_args) {
	const String source_path = p_args.get("source_path", ".");
	const String signal_name = p_args.get("signal", String());
	const String target_path = p_args.get("target_path", ".");
	const String method_name = p_args.get("method", String());
	if (signal_name.is_empty() || method_name.is_empty()) {
		return _error("INVALID_ARGUMENT", "signal and method are required.");
	}

	String error;
	Node *source = _resolve_node(source_path, error);
	if (!source) {
		return _error("SOURCE_NODE_NOT_FOUND", error);
	}
	Node *target = _resolve_node(target_path, error);
	if (!target) {
		return _error("TARGET_NODE_NOT_FOUND", error);
	}

	const int flags = p_args.get("flags", (int)Object::CONNECT_PERSIST);
	const StringName signal_sn = StringName(signal_name);
	const Callable callable(target, StringName(method_name));
	if (source->is_connected(signal_sn, callable)) {
		return _error("SIGNAL_ALREADY_CONNECTED", "The requested signal connection already exists.");
	}

	EditorInterface *editor_interface = EditorInterface::get_singleton();
	ERR_FAIL_NULL_V(editor_interface, _error("EDITOR_INTERFACE_UNAVAILABLE", "EditorInterface is not available.", false));
	EditorUndoRedoManager *undo_redo = editor_interface->get_editor_undo_redo();
	ERR_FAIL_NULL_V(undo_redo, _error("UNDO_REDO_UNAVAILABLE", "EditorUndoRedoManager is not available.", false));

	if (batch_action_active) {
		const Error connect_error = source->connect(signal_sn, callable, flags);
		if (connect_error != OK) {
			return _error("SIGNAL_CONNECT_FAILED", vformat("Connecting signal '%s' failed with error %d.", signal_name, (int)connect_error));
		}
	} else {
		undo_redo->create_action(vformat("Solers: Connect %s", signal_name), UndoRedo::MERGE_DISABLE, source);
	}
	undo_redo->add_do_method(source, "connect", signal_sn, callable, flags);
	undo_redo->add_undo_method(source, "disconnect", signal_sn, callable);
	if (!batch_action_active) {
		undo_redo->commit_action();
	}

	String source_safe_path;
	String target_safe_path;
	_safe_node_path(source, source_safe_path);
	_safe_node_path(target, target_safe_path);
	Dictionary data;
	data["source_path"] = source_safe_path;
	data["target_path"] = target_safe_path;
	data["signal"] = signal_name;
	data["method"] = method_name;
	data["flags"] = flags;
	return _ok(data);
}

Dictionary SolersReflectionService::_attach_script(const Dictionary &p_args) {
	const String node_path = p_args.get("node_path", ".");
	const String script_path = p_args.get("script_path", String());
	if (script_path.is_empty()) {
		return _error("INVALID_ARGUMENT", "script_path is required.");
	}

	String error;
	Node *node = _resolve_node(node_path, error);
	if (!node) {
		return _error("NODE_NOT_FOUND", error);
	}

	String normalized_script_path = script_path.strip_edges().replace_char('\\', '/');
	if (!normalized_script_path.begins_with("res://")) {
		normalized_script_path = String("res://").path_join(normalized_script_path);
	}
	normalized_script_path = normalized_script_path.simplify_path();
	if (!normalized_script_path.begins_with("res://") || normalized_script_path.contains("..")) {
		return _error("INVALID_PATH", "Script path escapes the project root.");
	}
	if (!FileAccess::exists(normalized_script_path)) {
		return _error("SCRIPT_NOT_FOUND", vformat("Script does not exist: %s", normalized_script_path));
	}

	Ref<Script> script = ResourceLoader::load(normalized_script_path, "Script");
	if (script.is_null()) {
		return _error("SCRIPT_LOAD_FAILED", vformat("Unable to load script: %s", normalized_script_path));
	}

	EditorInterface *editor_interface = EditorInterface::get_singleton();
	ERR_FAIL_NULL_V(editor_interface, _error("EDITOR_INTERFACE_UNAVAILABLE", "EditorInterface is not available.", false));
	EditorUndoRedoManager *undo_redo = editor_interface->get_editor_undo_redo();
	ERR_FAIL_NULL_V(undo_redo, _error("UNDO_REDO_UNAVAILABLE", "EditorUndoRedoManager is not available.", false));

	Ref<Script> previous_script = node->get_script();
	if (!batch_action_active) {
		undo_redo->create_action("Solers: Attach Script", UndoRedo::MERGE_DISABLE, node);
	}
	undo_redo->add_do_method(node, "set_script", script);
	undo_redo->add_undo_method(node, "set_script", previous_script);
	if (batch_action_active) {
		node->set_script(script);
	} else {
		undo_redo->commit_action();
	}

	String safe_path;
	_safe_node_path(node, safe_path);
	Dictionary data;
	data["node_path"] = safe_path;
	data["script_path"] = normalized_script_path;
	data["had_previous_script"] = previous_script.is_valid();
	return _ok(data);
}

Dictionary SolersReflectionService::_remove_node(const Dictionary &p_args) {
	const String node_path = p_args.get("node_path", String());
	if (node_path.is_empty() || node_path == ".") {
		return _error("INVALID_ARGUMENT", "Refusing to remove the edited scene root.");
	}

	String error;
	Node *node = _resolve_node(node_path, error);
	if (!node) {
		return _error("NODE_NOT_FOUND", error);
	}
	Node *parent = node->get_parent();
	if (!parent) {
		return _error("INVALID_ARGUMENT", "Node has no parent and cannot be removed safely.");
	}

	EditorInterface *editor_interface = EditorInterface::get_singleton();
	ERR_FAIL_NULL_V(editor_interface, _error("EDITOR_INTERFACE_UNAVAILABLE", "EditorInterface is not available.", false));
	EditorUndoRedoManager *undo_redo = editor_interface->get_editor_undo_redo();
	ERR_FAIL_NULL_V(undo_redo, _error("UNDO_REDO_UNAVAILABLE", "EditorUndoRedoManager is not available.", false));

	const int original_index = node->get_index(false);
	const Dictionary removed_object = _reflect_object_handle(node);
	if (!batch_action_active) {
		undo_redo->create_action("Solers: Remove Node", UndoRedo::MERGE_DISABLE, parent);
	}
	undo_redo->add_do_method(parent, "remove_child", node);
	if (batch_action_active) {
		undo_redo->add_undo_method(parent, "move_child", node, original_index);
		undo_redo->add_undo_method(parent, "add_child", node, true);
		undo_redo->add_undo_reference(node);
	} else {
		undo_redo->add_undo_method(parent, "add_child", node, true);
		undo_redo->add_undo_method(parent, "move_child", node, original_index);
		undo_redo->add_undo_reference(node);
	}
	if (batch_action_active) {
		parent->remove_child(node);
	} else {
		undo_redo->commit_action();
	}

	String parent_safe_path;
	_safe_node_path(parent, parent_safe_path);
	Dictionary data;
	data["removed_node"] = node_path;
	data["object"] = removed_object;
	data["parent_path"] = parent_safe_path;
	data["original_index"] = original_index;
	return _ok(data);
}

Dictionary SolersReflectionService::_list_properties(const Dictionary &p_args) {
	const String node_path = p_args.get("node_path", ".");
	const int max_properties = CLAMP((int)p_args.get("max_properties", 128), 0, 512);

	String error;
	Node *node = _resolve_node(node_path, error);
	if (!node) {
		return _error("NODE_NOT_FOUND", error);
	}

	List<PropertyInfo> property_list;
	node->get_property_list(&property_list);
	Array properties;
	int count = 0;
	for (const PropertyInfo &property : property_list) {
		if (count >= max_properties) {
			break;
		}
		if (!(property.usage & PROPERTY_USAGE_EDITOR) && !(property.usage & PROPERTY_USAGE_STORAGE)) {
			continue;
		}
		Dictionary item;
		item["name"] = property.name;
		item["type"] = Variant::get_type_name(property.type);
		item["hint"] = property.hint;
		item["hint_string"] = property.hint_string;
		item["usage"] = property.usage;
		item["value"] = _reflect_displayable(node->get(property.name));
		properties.push_back(item);
		count++;
	}

	String safe_path;
	_safe_node_path(node, safe_path);
	Dictionary data;
	data["node_path"] = safe_path;
	data["properties"] = properties;
	data["count"] = properties.size();
	data["truncated"] = count >= max_properties;
	return _ok(data);
}

Dictionary SolersReflectionService::_list_signal_connections(const Dictionary &p_args) {
	const String source_path = p_args.get("source_path", ".");
	const String signal_name = p_args.get("signal", String());

	String error;
	Node *source = _resolve_node(source_path, error);
	if (!source) {
		return _error("SOURCE_NODE_NOT_FOUND", error);
	}

	Array connection_items;
	if (signal_name.is_empty()) {
		List<MethodInfo> signal_list;
		source->get_signal_list(&signal_list);
		for (const MethodInfo &signal : signal_list) {
			List<Object::Connection> connection_list;
			source->get_signal_connection_list(signal.name, &connection_list);
			for (const Object::Connection &connection : connection_list) {
				Dictionary item;
				item["signal"] = connection.signal.get_name();
				item["callable"] = String(connection.callable);
				item["flags"] = connection.flags;
				connection_items.push_back(item);
			}
		}
	} else {
		List<Object::Connection> connection_list;
		source->get_signal_connection_list(StringName(signal_name), &connection_list);
		for (const Object::Connection &connection : connection_list) {
			Dictionary item;
			item["signal"] = connection.signal.get_name();
			item["callable"] = String(connection.callable);
			item["flags"] = connection.flags;
			connection_items.push_back(item);
		}
	}

	String safe_path;
	_safe_node_path(source, safe_path);
	Dictionary data;
	data["source_path"] = safe_path;
	data["signal"] = signal_name;
	data["connections"] = connection_items;
	data["count"] = connection_items.size();
	return _ok(data);
}

static bool _solers_batch_operation_mutates(SolersBatchOperationKind p_kind) {
	switch (p_kind) {
		case SolersBatchOperationKind::CREATE_NODE:
		case SolersBatchOperationKind::INSTANTIATE_SCENE:
		case SolersBatchOperationKind::SET_PROPERTY:
		case SolersBatchOperationKind::REPARENT:
		case SolersBatchOperationKind::CONNECT_SIGNAL:
		case SolersBatchOperationKind::ATTACH_SCRIPT:
		case SolersBatchOperationKind::REMOVE_NODE:
			return true;
		default:
			return false;
	}
}

Dictionary SolersReflectionService::batch(const Dictionary &p_args) {
	const Array operations = p_args.get("operations", Array());
	if (operations.is_empty()) {
		return _error("INVALID_ARGUMENT", "operations is required and must be a non-empty array.");
	}
	EditorNode *editor_node = EditorNode::get_singleton();
	const bool has_scene_slot = editor_node && EditorNode::get_editor_data().get_edited_scene_count() > 0;
	bool has_mutation = false;
	for (int i = 0; i < operations.size(); i++) {
		if (operations[i].get_type() == Variant::DICTIONARY) {
			has_mutation = has_mutation || _solers_batch_operation_mutates(_solers_batch_operation_kind(Dictionary(operations[i]).get("op", String())));
		}
	}
	EditorUndoRedoManager *undo_redo = has_mutation ? EditorUndoRedoManager::get_singleton() : nullptr;
	const int history_id = has_scene_slot ? EditorNode::get_editor_data().get_current_edited_scene_history_id() : EditorUndoRedoManager::INVALID_HISTORY;
	if (has_mutation && (!undo_redo || history_id == EditorUndoRedoManager::INVALID_HISTORY)) {
		return _error("UNDO_REDO_UNAVAILABLE", "The current edited scene has no UndoRedo history.", false);
	}
	if (has_mutation) {
		undo_redo->create_action_for_history("Solers: Batch", history_id, UndoRedo::MERGE_DISABLE, true);
		undo_redo->force_fixed_history();
		batch_action_active = true;
	}

	Array results;
	int ok_count = 0;
	int error_count = 0;
	for (int i = 0; i < operations.size(); i++) {
		if (operations[i].get_type() != Variant::DICTIONARY) {
			Dictionary entry;
			entry["index"] = i;
			entry["op"] = String();
			entry["result"] = _error("INVALID_OP", "Each operation must be an object with an 'op' field.");
			results.push_back(entry);
			error_count++;
			break;
		}
		const Dictionary raw_op = operations[i];
		const String kind = raw_op.get("op", String());
		const SolersBatchOperationKind operation_kind = _solers_batch_operation_kind(kind);
		const Dictionary op = _solers_normalize_batch_operation(raw_op, operation_kind);
		Dictionary result;
		switch (operation_kind) {
			case SolersBatchOperationKind::CREATE_NODE:
				result = _create_node(op);
				break;
			case SolersBatchOperationKind::INSTANTIATE_SCENE:
				result = instantiate_scene(op);
				break;
			case SolersBatchOperationKind::SET_PROPERTY:
				result = set_property(op);
				break;
			case SolersBatchOperationKind::REPARENT:
				result = _reparent_node(op);
				break;
			case SolersBatchOperationKind::CONNECT_SIGNAL:
				result = _connect_signal(op);
				break;
			case SolersBatchOperationKind::ATTACH_SCRIPT:
				result = _attach_script(op);
				break;
			case SolersBatchOperationKind::REMOVE_NODE:
				result = _remove_node(op);
				break;
			default:
				result = _error("UNKNOWN_OP", vformat("Unknown scene edit op '%s'. Supported: create_node, instantiate, set_property, reparent, connect_signal, attach_script, remove_node.", kind));
				break;
		}
		Dictionary entry;
		entry["index"] = i;
		entry["op"] = kind;
		entry["result"] = result;
		if (!(bool)result.get("ok", false) && op.has("property")) {
			Dictionary error = result.get("error", Dictionary());
			error["hint"] = "For nested resource properties, use '/' paths such as environment/ambient_light_energy.";
			result["error"] = error;
			entry["result"] = result;
		}
		results.push_back(entry);
		if ((bool)result.get("ok", false)) {
			ok_count++;
		} else {
			error_count++;
			// Stop at the first failure so the model can correct before the
			// later ops compound on a bad state.
			break;
		}
	}

	bool rolled_back = false;
	if (has_mutation) {
		batch_action_active = false;
		undo_redo->commit_action(false);
		if (error_count > 0) {
			rolled_back = undo_redo->undo_history(history_id);
			EditorUndoRedoManager::History &history = undo_redo->get_or_create_history(history_id);
			history.redo_stack.clear();
			history.undo_redo->discard_redo();
		}
	}

	Dictionary data;
	data["count"] = results.size();
	data["ok_count"] = ok_count;
	data["error_count"] = error_count;
	data["completed"] = error_count == 0;
	data["rolled_back"] = rolled_back;
	data["authored_state_changed"] = has_mutation && error_count == 0;
	if (error_count == 0) {
		const Array affected = _spatial_digest_for_results(results);
		if (!affected.is_empty()) {
			data["affected_nodes"] = affected;
		}
		// Success: path receipts only — not a full per-op node dump.
		Array path_receipts;
		for (int i = 0; i < results.size(); i++) {
			const Dictionary entry = results[i];
			Dictionary receipt;
			receipt["index"] = entry.get("index", i);
			receipt["op"] = entry.get("op", String());
			const Dictionary result_data = Dictionary(entry.get("result", Dictionary())).get("data", Dictionary());
			const String path = result_data.get("node_path", result_data.get("path", String()));
			if (!path.is_empty()) {
				receipt["node_path"] = path;
			}
			path_receipts.push_back(receipt);
		}
		data["results"] = path_receipts;
	} else {
		data["results"] = results;
	}
	if (has_mutation && error_count == 0) {
		Node *root = has_scene_slot ? editor_node->get_edited_scene() : nullptr;
		data["target_scene_path"] = root ? root->get_scene_file_path() : String();
		const Array nested = _nested_instance_scenes(results);
		if (!nested.is_empty()) {
			data["nested_instance_scenes"] = nested;
			data["nested_instance_warning"] = "These operations changed nodes owned by instanced sub-scenes. Godot records that as an instance override inside target_scene_path, and the listed sub-scene files stay untouched. Open the sub-scene and edit it directly if the change has to live in that file.";
		}
	}
	if (error_count > 0) {
		Dictionary result = _error("SCENE_EDIT_FAILED", "The scene transaction stopped at the first failed operation and rolled back.");
		result["data"] = data;
		return result;
	}
	return _ok(data);
}

static real_t _solers_axis_min(const AABB &p_bounds, int p_axis) {
	return p_bounds.position[p_axis];
}

static real_t _solers_axis_max(const AABB &p_bounds, int p_axis) {
	return p_bounds.position[p_axis] + p_bounds.size[p_axis];
}

static int _solers_axis_index(const String &p_axis) {
	if (p_axis == "x") {
		return Vector3::AXIS_X;
	}
	if (p_axis == "y") {
		return Vector3::AXIS_Y;
	}
	if (p_axis == "z") {
		return Vector3::AXIS_Z;
	}
	return -1;
}

static real_t _solers_axis_anchor(const AABB &p_bounds, int p_axis, const String &p_anchor) {
	if (p_anchor == "min") {
		return _solers_axis_min(p_bounds, p_axis);
	}
	if (p_anchor == "max") {
		return _solers_axis_max(p_bounds, p_axis);
	}
	return (_solers_axis_min(p_bounds, p_axis) + _solers_axis_max(p_bounds, p_axis)) * 0.5;
}

real_t SolersReflectionService::get_aabb_max_gap(const AABB &p_a, const AABB &p_b) {
	real_t max_gap = 0.0;
	for (int axis = 0; axis < 3; axis++) {
		max_gap = MAX(max_gap, MAX(MAX(_solers_axis_min(p_a, axis) - _solers_axis_max(p_b, axis), _solers_axis_min(p_b, axis) - _solers_axis_max(p_a, axis)), (real_t)0.0));
	}
	return max_gap;
}

static Dictionary _solers_aabb_data(const AABB &p_bounds) {
	Dictionary data;
	data["position"] = _solers_vector3_array(p_bounds.position);
	data["size"] = _solers_vector3_array(p_bounds.size);
	return data;
}

Dictionary SolersReflectionService::validate_spatial_relations(const Dictionary &p_args) const {
	const Array relations = p_args.get("relations", Array());
	if (relations.is_empty()) {
		return _error("INVALID_ARGUMENT", "relations must contain at least one spatial contract.");
	}

	Array results;
	int failure_count = 0;
	for (int i = 0; i < relations.size(); i++) {
		if (relations[i].get_type() != Variant::DICTIONARY) {
			return _error("INVALID_ARGUMENT", vformat("relations[%d] must be an object.", i));
		}
		const Dictionary relation = relations[i];
		const String a_path = String(relation.get("a", String())).strip_edges();
		const String b_path = String(relation.get("b", String())).strip_edges();
		const String kind = String(relation.get("kind", String())).strip_edges();
		if (a_path.is_empty() || b_path.is_empty() || (kind != "max_gap" && kind != "contains" && kind != "align" && kind != "no_overlap")) {
			return _error("INVALID_ARGUMENT", vformat("relations[%d] requires a, b, and kind=max_gap|contains|align|no_overlap.", i));
		}
		if (!relation.has("tolerance") || (relation["tolerance"].get_type() != Variant::FLOAT && relation["tolerance"].get_type() != Variant::INT)) {
			return _error("INVALID_ARGUMENT", vformat("relations[%d].tolerance must be an explicit non-negative project-unit value.", i));
		}
		const real_t tolerance = relation["tolerance"];
		if (tolerance < 0.0) {
			return _error("INVALID_ARGUMENT", vformat("relations[%d].tolerance cannot be negative.", i));
		}

		String resolve_error;
		GeometryInstance3D *a = Object::cast_to<GeometryInstance3D>(_resolve_node(a_path, resolve_error));
		if (!a) {
			return _error("GEOMETRY_NODE_NOT_FOUND", resolve_error.is_empty() ? vformat("Node is not GeometryInstance3D: %s", a_path) : resolve_error);
		}
		GeometryInstance3D *b = Object::cast_to<GeometryInstance3D>(_resolve_node(b_path, resolve_error));
		if (!b) {
			return _error("GEOMETRY_NODE_NOT_FOUND", resolve_error.is_empty() ? vformat("Node is not GeometryInstance3D: %s", b_path) : resolve_error);
		}
		const AABB a_bounds = a->get_global_transform().xform(a->get_aabb());
		const AABB b_bounds = b->get_global_transform().xform(b->get_aabb());

		bool checked_axes[3] = { true, true, true };
		const Array axes = relation.get("axes", Array());
		if (!axes.is_empty()) {
			checked_axes[0] = checked_axes[1] = checked_axes[2] = false;
			for (int axis_index = 0; axis_index < axes.size(); axis_index++) {
				const String axis = String(axes[axis_index]).to_lower();
				if (axis == "x") {
					checked_axes[0] = true;
				} else if (axis == "y") {
					checked_axes[1] = true;
				} else if (axis == "z") {
					checked_axes[2] = true;
				} else {
					return _error("INVALID_ARGUMENT", vformat("relations[%d].axes contains '%s'; expected x, y, or z.", i, axis));
				}
			}
		}
		String a_anchor;
		String b_anchor;
		if (kind == "align") {
			const String axis_name = String(relation.get("axis", String())).to_lower();
			const int axis = _solers_axis_index(axis_name);
			a_anchor = String(relation.get("a_anchor", "center")).to_lower();
			b_anchor = String(relation.get("b_anchor", "center")).to_lower();
			const bool valid_a_anchor = a_anchor == "min" || a_anchor == "center" || a_anchor == "max";
			const bool valid_b_anchor = b_anchor == "min" || b_anchor == "center" || b_anchor == "max";
			if (axis < 0 || !axes.is_empty() || !valid_a_anchor || !valid_b_anchor) {
				return _error("INVALID_ARGUMENT", vformat("relations[%d] align requires axis=x|y|z, optional min|center|max anchors, and no axes array.", i));
			}
			checked_axes[0] = checked_axes[1] = checked_axes[2] = false;
			checked_axes[axis] = true;
		}

		bool passed = kind == "no_overlap" ? false : true;
		Array measurements;
		for (int axis = 0; axis < 3; axis++) {
			if (!checked_axes[axis]) {
				continue;
			}
			Dictionary measurement;
			measurement["axis"] = axis == 0 ? "x" : (axis == 1 ? "y" : "z");
			bool axis_passed = false;
			if (kind == "max_gap") {
				const real_t gap = MAX(MAX(_solers_axis_min(a_bounds, axis) - _solers_axis_max(b_bounds, axis), _solers_axis_min(b_bounds, axis) - _solers_axis_max(a_bounds, axis)), (real_t)0.0);
				measurement["gap"] = gap;
				axis_passed = gap <= tolerance;
			} else if (kind == "contains") {
				const real_t underflow = MAX(_solers_axis_min(a_bounds, axis) - _solers_axis_min(b_bounds, axis), (real_t)0.0);
				const real_t overflow = MAX(_solers_axis_max(b_bounds, axis) - _solers_axis_max(a_bounds, axis), (real_t)0.0);
				measurement["underflow"] = underflow;
				measurement["overflow"] = overflow;
				axis_passed = underflow <= tolerance && overflow <= tolerance;
			} else if (kind == "align") {
				const real_t a_value = _solers_axis_anchor(a_bounds, axis, a_anchor);
				const real_t b_value = _solers_axis_anchor(b_bounds, axis, b_anchor);
				const real_t delta = Math::abs(a_value - b_value);
				measurement["a_anchor"] = a_anchor;
				measurement["b_anchor"] = b_anchor;
				measurement["a_value"] = a_value;
				measurement["b_value"] = b_value;
				measurement["delta"] = delta;
				axis_passed = delta <= tolerance;
			} else {
				const real_t overlap = MIN(_solers_axis_max(a_bounds, axis), _solers_axis_max(b_bounds, axis)) - MAX(_solers_axis_min(a_bounds, axis), _solers_axis_min(b_bounds, axis));
				measurement["overlap"] = overlap;
				axis_passed = overlap <= tolerance;
			}
			measurement["passed"] = axis_passed;
			measurements.push_back(measurement);
			passed = kind == "no_overlap" ? passed || axis_passed : passed && axis_passed;
		}

		Dictionary result;
		result["index"] = i;
		result["kind"] = kind;
		result["a"] = a_path;
		result["b"] = b_path;
		result["a_world_aabb"] = _solers_aabb_data(a_bounds);
		result["b_world_aabb"] = _solers_aabb_data(b_bounds);
		if (kind == "max_gap") {
			result["max_gap"] = get_aabb_max_gap(a_bounds, b_bounds);
		}
		result["measurements"] = measurements;
		result["passed"] = passed;
		results.push_back(result);
		if (!passed) {
			failure_count++;
		}
	}

	Dictionary data;
	data["relations"] = results;
	data["checked_relation_count"] = results.size();
	data["failure_count"] = failure_count;
	if (failure_count > 0) {
		Dictionary failure = _error("SPATIAL_VALIDATION_FAILED", vformat("%d of %d declared spatial relations failed.", failure_count, results.size()));
		failure["data"] = data;
		return failure;
	}
	return _ok(data);
}

static void _solers_collect_geometry_nodes(Node *p_node, Vector<GeometryInstance3D *> &r_nodes) {
	if (!p_node) {
		return;
	}
	if (GeometryInstance3D *geometry = Object::cast_to<GeometryInstance3D>(p_node)) {
		r_nodes.push_back(geometry);
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		_solers_collect_geometry_nodes(p_node->get_child(i), r_nodes);
	}
}

uint64_t SolersReflectionService::get_spatial_geometry_digest() const {
	Node *root = EditorInterface::get_singleton() ? EditorInterface::get_singleton()->get_edited_scene_root() : nullptr;
	Vector<GeometryInstance3D *> nodes;
	_solers_collect_geometry_nodes(root, nodes);
	Array entries;
	for (GeometryInstance3D *geometry : nodes) {
		Dictionary entry;
		String path;
		_safe_node_path(geometry, path);
		entry["path"] = path;
		entry["transform"] = geometry->get_global_transform();
		entry["aabb"] = geometry->get_aabb();
		entry["visible"] = geometry->is_visible();
		entries.push_back(entry);
	}
	return entries.hash();
}

static Variant _solers_stable_resource_value(const Variant &p_value, HashSet<ObjectID> &r_visiting) {
	if (p_value.get_type() == Variant::ARRAY) {
		const Array source = p_value;
		Array normalized;
		for (int i = 0; i < source.size(); i++) {
			normalized.push_back(_solers_stable_resource_value(source[i], r_visiting));
		}
		return normalized;
	}
	if (p_value.get_type() == Variant::DICTIONARY) {
		const Dictionary source = p_value;
		Dictionary normalized;
		for (const Variant *key = source.next(nullptr); key; key = source.next(key)) {
			normalized[*key] = _solers_stable_resource_value(source[*key], r_visiting);
		}
		return normalized;
	}
	if (p_value.get_type() != Variant::OBJECT) {
		return p_value;
	}
	Object *object = p_value;
	Resource *resource = Object::cast_to<Resource>(object);
	if (!resource) {
		return object ? Variant(object->get_class()) : Variant();
	}
	Dictionary state;
	state["class"] = resource->get_class();
	const String path = resource->get_path();
	if (!path.is_empty()) {
		state["path"] = path;
	}
	const String file_path = path.get_slice("::", 0);
	if (!path.contains("::") && FileAccess::exists(file_path)) {
		state["modified_time"] = (int64_t)FileAccess::get_modified_time(file_path);
		state["size"] = (int64_t)FileAccess::get_size(file_path);
		return state;
	}
	const ObjectID object_id = resource->get_instance_id();
	if (r_visiting.has(object_id)) {
		state["cycle"] = true;
		return state;
	}
	r_visiting.insert(object_id);
	Dictionary properties;
	List<PropertyInfo> property_list;
	resource->get_property_list(&property_list);
	for (const PropertyInfo &property : property_list) {
		if (!(property.usage & PROPERTY_USAGE_STORAGE) || String(property.name).begins_with("metadata/")) {
			continue;
		}
		bool valid = false;
		const Variant value = resource->get(property.name, &valid);
		if (valid) {
			properties[property.name] = _solers_stable_resource_value(value, r_visiting);
		}
	}
	r_visiting.erase(object_id);
	state["properties"] = properties;
	return state;
}

static Dictionary _solers_stable_object_properties(Object *p_object) {
	Dictionary properties;
	if (!p_object) {
		return properties;
	}
	HashSet<ObjectID> visiting;
	List<PropertyInfo> property_list;
	p_object->get_property_list(&property_list);
	for (const PropertyInfo &property : property_list) {
		if (!(property.usage & PROPERTY_USAGE_STORAGE) || String(property.name).begins_with("metadata/")) {
			continue;
		}
		bool valid = false;
		const Variant value = p_object->get(property.name, &valid);
		if (valid) {
			properties[property.name] = _solers_stable_resource_value(value, visiting);
		}
	}
	return properties;
}

static void _solers_collect_lightmap_inputs(Node *p_node, Array &r_entries) {
	if (!p_node) {
		return;
	}
	Dictionary entry;
	if (MeshInstance3D *mesh_instance = Object::cast_to<MeshInstance3D>(p_node)) {
		if (mesh_instance->get_gi_mode() == GeometryInstance3D::GI_MODE_STATIC && mesh_instance->is_visible_in_tree()) {
			entry["kind"] = "mesh";
			entry["path"] = String(mesh_instance->get_path());
			entry["transform"] = mesh_instance->get_global_transform();
			HashSet<ObjectID> visiting;
			const Variant material_override = mesh_instance->get_material_override();
			const Variant material_overlay = mesh_instance->get_material_overlay();
			entry["material_override"] = _solers_stable_resource_value(material_override, visiting);
			entry["material_overlay"] = _solers_stable_resource_value(material_overlay, visiting);
			Ref<Mesh> mesh = mesh_instance->get_mesh();
			Array surfaces;
			if (mesh.is_valid()) {
				for (int surface = 0; surface < mesh->get_surface_count(); surface++) {
					Dictionary surface_state;
					surface_state["format"] = (int64_t)mesh->surface_get_format(surface);
					surface_state["arrays_hash"] = (int64_t)mesh->surface_get_arrays(surface).hash();
					const Variant material = mesh_instance->get_active_material(surface);
					surface_state["material"] = _solers_stable_resource_value(material, visiting);
					surfaces.push_back(surface_state);
				}
			}
			entry["surfaces"] = surfaces;
		}
	} else if (Light3D *light = Object::cast_to<Light3D>(p_node)) {
		entry["kind"] = "light";
		entry["path"] = String(light->get_path());
		entry["transform"] = light->get_global_transform();
		entry["properties"] = _solers_stable_object_properties(light);
	} else if (LightmapGI *lightmap = Object::cast_to<LightmapGI>(p_node)) {
		Dictionary properties = _solers_stable_object_properties(lightmap);
		properties.erase(SNAME("light_data"));
		entry["kind"] = "lightmap_settings";
		entry["path"] = String(lightmap->get_path());
		entry["properties"] = properties;
	} else if (WorldEnvironment *world_environment = Object::cast_to<WorldEnvironment>(p_node)) {
		entry["kind"] = "environment";
		entry["path"] = String(world_environment->get_path());
		entry["properties"] = _solers_stable_object_properties(world_environment);
	}
	if (!entry.is_empty()) {
		r_entries.push_back(entry);
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		_solers_collect_lightmap_inputs(p_node->get_child(i), r_entries);
	}
}

uint64_t SolersReflectionService::get_lightmap_input_digest() const {
	Node *root = EditorInterface::get_singleton() ? EditorInterface::get_singleton()->get_edited_scene_root() : nullptr;
	Array entries;
	_solers_collect_lightmap_inputs(root, entries);
	return entries.hash();
}

#ifdef MODULE_CSG_ENABLED
Dictionary SolersReflectionService::bake_csg(const Dictionary &p_args) {
	const Array node_paths = p_args.get("node_paths", Array());
	if (node_paths.is_empty()) {
		return _error("INVALID_ARGUMENT", "node_paths must contain at least one CSGShape3D root.");
	}
	const bool hide_sources = p_args.get("hide_sources", true);
	Vector<CSGShape3D *> sources;
	Vector<Ref<ArrayMesh>> meshes;
	for (int i = 0; i < node_paths.size(); i++) {
		String resolve_error;
		CSGShape3D *source = Object::cast_to<CSGShape3D>(_resolve_node(String(node_paths[i]), resolve_error));
		if (!source) {
			return _error("CSG_NODE_NOT_FOUND", resolve_error.is_empty() ? vformat("node_paths[%d] is not a CSGShape3D.", i) : resolve_error);
		}
		if (!source->is_root_shape() || !source->get_parent()) {
			return _error("INVALID_CSG_ROOT", vformat("%s must be a non-scene-root CSG root shape.", String(node_paths[i])));
		}
		source->call(SNAME("_update_shape"));
		Ref<ArrayMesh> mesh = source->bake_static_mesh();
		if (mesh.is_null() || mesh->get_surface_count() == 0) {
			return _error("CSG_BAKE_FAILED", vformat("CSG produced no mesh: %s", String(node_paths[i])));
		}
		const AABB source_bounds = source->get_global_transform().xform(source->get_aabb());
		const AABB baked_bounds = source->get_global_transform().xform(mesh->get_aabb());
		if (!source_bounds.is_equal_approx(baked_bounds)) {
			return _error("CSG_BAKE_MISMATCH", vformat("Baked mesh bounds do not match the source CSG: %s", String(node_paths[i])));
		}
		sources.push_back(source);
		meshes.push_back(mesh);
	}

	const uint64_t input_digest = get_spatial_geometry_digest();
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	EditorInterface *editor_interface = EditorInterface::get_singleton();
	if (!undo_redo || !editor_interface || !editor_interface->get_edited_scene_root()) {
		return _error("EDITOR_CONTEXT_UNAVAILABLE", "CSG baking requires an edited scene and EditorUndoRedoManager.", false);
	}
	Node *owner = editor_interface->get_edited_scene_root();
	undo_redo->create_action("Bake CSG meshes");
	Vector<MeshInstance3D *> outputs;
	for (int i = 0; i < sources.size(); i++) {
		MeshInstance3D *output = memnew(MeshInstance3D);
		output->set_name(String(sources[i]->get_name()) + "BakedMesh");
		output->set_mesh(meshes[i]);
		output->set_transform(sources[i]->get_transform());
		output->set_gi_mode(GeometryInstance3D::GI_MODE_STATIC);
		String source_path;
		_safe_node_path(sources[i], source_path);
		output->set_meta(SNAME("_solers_baked_from"), source_path);
		undo_redo->add_do_method(sources[i], "add_sibling", output, true);
		undo_redo->add_do_method(output, "set_owner", owner);
		undo_redo->add_do_reference(output);
		undo_redo->add_undo_method(sources[i]->get_parent(), "remove_child", output);
		if (hide_sources) {
			undo_redo->add_do_method(sources[i], "set_visible", false);
			undo_redo->add_undo_method(sources[i], "set_visible", sources[i]->is_visible());
		}
		outputs.push_back(output);
	}
	undo_redo->commit_action();

	Array output_paths;
	for (MeshInstance3D *output : outputs) {
		String path;
		_safe_node_path(output, path);
		output_paths.push_back(path);
	}
	Dictionary artifact;
	artifact["kind"] = "baked_mesh";
	artifact["input_geometry_digest"] = String::num_uint64(input_digest);
	artifact["output_geometry_digest"] = String::num_uint64(get_spatial_geometry_digest());
	artifact["source_paths"] = node_paths;
	artifact["output_paths"] = output_paths;
	Dictionary data;
	data["artifact"] = artifact;
	data["output_paths"] = output_paths;
	return _ok(data);
}
#else
Dictionary SolersReflectionService::bake_csg(const Dictionary &) {
	return _error("CSG_MODULE_UNAVAILABLE", "This build does not include the CSG module.", false);
}
#endif

static bool _solers_mesh_has_uv2(const Ref<Mesh> &p_mesh) {
	if (p_mesh.is_null() || p_mesh->get_surface_count() == 0) {
		return false;
	}
	for (int surface = 0; surface < p_mesh->get_surface_count(); surface++) {
		if (!(p_mesh->surface_get_format(surface) & Mesh::ARRAY_FORMAT_TEX_UV2)) {
			return false;
		}
	}
	return true;
}

void SolersReflectionService::_uv2_unwrap_thread(void *p_userdata) {
	UV2UnwrapTask *task = static_cast<UV2UnwrapTask *>(p_userdata);
	if (!task->cancelled.is_set()) {
		task->worker_error = task->current_mesh->lightmap_unwrap(task->current_transform);
	}
	task->worker_done.set();
}

Dictionary SolersReflectionService::_pending_uv2_unwrap(const UV2UnwrapTask *p_task, const String &p_stage) const {
	Dictionary poll_args;
	poll_args["_uv2_id"] = p_task->operation_id;
	Dictionary data;
	data["status"] = "pending";
	data["stage"] = p_stage;
	data["processed_count"] = p_task->next_index;
	data["total_count"] = p_task->node_paths.size();
	data["already_valid_count"] = p_task->already_valid_count;
	if (p_task->current_index >= 0 && p_task->current_index < p_task->node_paths.size()) {
		data["current_path"] = p_task->node_paths[p_task->current_index];
	}
	data["poll_args"] = poll_args;
	return _ok(data);
}

void SolersReflectionService::_free_uv2_unwrap(UV2UnwrapTask *p_task, bool p_wait) {
	if (!p_task) {
		return;
	}
	p_task->cancelled.set();
	if (p_task->worker.is_started() && (p_wait || p_task->worker_done.is_set())) {
		p_task->worker.wait_to_finish();
		p_task->worker_active = false;
	}
	if (p_task->worker.is_started()) {
		return;
	}
	uv2_unwrap_tasks.erase(p_task->operation_id);
	memdelete(p_task);
}

void SolersReflectionService::_sweep_uv2_unwrap_tasks() {
	Vector<UV2UnwrapTask *> finished;
	for (const KeyValue<String, UV2UnwrapTask *> &entry : uv2_unwrap_tasks) {
		UV2UnwrapTask *task = entry.value;
		if (task->cancelled.is_set() && (!task->worker_active || task->worker_done.is_set())) {
			finished.push_back(task);
		}
	}
	for (UV2UnwrapTask *task : finished) {
		_free_uv2_unwrap(task, true);
	}
}

Dictionary SolersReflectionService::_fail_uv2_unwrap(UV2UnwrapTask *p_task, const String &p_code, const String &p_message) {
	Dictionary result = _error(p_code, p_message);
	Dictionary data;
	data["processed_count"] = p_task ? p_task->next_index : 0;
	data["total_count"] = p_task ? p_task->node_paths.size() : 0;
	result["data"] = data;
	_free_uv2_unwrap(p_task, true);
	return result;
}

Dictionary SolersReflectionService::_advance_uv2_unwrap(UV2UnwrapTask *p_task) {
	while (p_task->next_index < p_task->node_paths.size()) {
		const int index = p_task->next_index;
		const String path = p_task->node_paths[index];
		String resolve_error;
		MeshInstance3D *node = Object::cast_to<MeshInstance3D>(_resolve_node(path, resolve_error));
		if (!node || node->get_instance_id() != p_task->node_ids[index] || node->get_mesh() != p_task->original_meshes[index]) {
			return _fail_uv2_unwrap(p_task, "MESH_CHANGED_DURING_UNWRAP", vformat("The mesh changed while UV2 was being prepared: %s", path));
		}
		const Ref<Mesh> original = p_task->original_meshes[index];
		if (_solers_mesh_has_uv2(original)) {
			p_task->replacement_meshes.write[index] = original;
			p_task->already_valid_count++;
			p_task->next_index++;
			continue;
		}

		Ref<PrimitiveMesh> primitive = original;
		if (primitive.is_valid()) {
			Ref<PrimitiveMesh> copy = primitive->duplicate(false);
			copy->set_add_uv2(true);
			if (!_solers_mesh_has_uv2(copy)) {
				return _fail_uv2_unwrap(p_task, "UV2_UNWRAP_INVALID", vformat("Godot returned a primitive mesh without UV2 for %s.", path));
			}
			p_task->replacement_meshes.write[index] = copy;
			p_task->next_index++;
			return _pending_uv2_unwrap(p_task, "prepared");
		}

		Ref<ArrayMesh> array_mesh = original;
		if (array_mesh.is_null()) {
			return _fail_uv2_unwrap(p_task, "UNSUPPORTED_MESH", vformat("Only PrimitiveMesh and ArrayMesh can be unwrapped: %s", path));
		}
		p_task->current_index = index;
		p_task->current_mesh = array_mesh->duplicate(false);
		p_task->current_transform = node->get_global_transform();
		p_task->worker_error = OK;
		p_task->worker_done.clear();
		p_task->worker_active = true;
		p_task->worker.start(&SolersReflectionService::_uv2_unwrap_thread, p_task);
		if (!p_task->worker.is_started()) {
			p_task->worker_active = false;
			return _fail_uv2_unwrap(p_task, "UV2_WORKER_UNAVAILABLE", "Godot could not start the UV2 unwrap worker.");
		}
		return _pending_uv2_unwrap(p_task, "unwrapping");
	}

	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	if (!undo_redo) {
		return _fail_uv2_unwrap(p_task, "EDITOR_CONTEXT_UNAVAILABLE", "UV2 unwrapping requires EditorUndoRedoManager.");
	}
	Vector<MeshInstance3D *> nodes;
	int changed_count = 0;
	for (int i = 0; i < p_task->node_paths.size(); i++) {
		String resolve_error;
		MeshInstance3D *node = Object::cast_to<MeshInstance3D>(_resolve_node(String(p_task->node_paths[i]), resolve_error));
		if (!node || node->get_instance_id() != p_task->node_ids[i] || node->get_mesh() != p_task->original_meshes[i]) {
			return _fail_uv2_unwrap(p_task, "MESH_CHANGED_DURING_UNWRAP", vformat("The mesh changed before the atomic UV2 commit: %s", String(p_task->node_paths[i])));
		}
		if (!_solers_mesh_has_uv2(p_task->replacement_meshes[i])) {
			return _fail_uv2_unwrap(p_task, "UV2_UNWRAP_INVALID", vformat("The prepared mesh has no UV2: %s", String(p_task->node_paths[i])));
		}
		nodes.push_back(node);
		changed_count += p_task->replacement_meshes[i] != p_task->original_meshes[i] ? 1 : 0;
	}
	if (changed_count > 0) {
		undo_redo->create_action("Unwrap mesh UV2");
		for (int i = 0; i < nodes.size(); i++) {
			if (p_task->replacement_meshes[i] == p_task->original_meshes[i]) {
				continue;
			}
			undo_redo->add_do_method(nodes[i], "set_mesh", p_task->replacement_meshes[i]);
			undo_redo->add_do_reference(p_task->replacement_meshes[i].ptr());
			undo_redo->add_undo_method(nodes[i], "set_mesh", p_task->original_meshes[i]);
		}
		undo_redo->commit_action();
	}

	Dictionary artifact;
	artifact["kind"] = "uv2";
	artifact["mesh_paths"] = p_task->node_paths;
	artifact["lightmap_input_digest"] = String::num_uint64(get_lightmap_input_digest());
	artifact["uv2_verified"] = true;
	Dictionary data;
	data["artifact"] = artifact;
	data["mesh_paths"] = p_task->node_paths;
	data["processed_count"] = p_task->node_paths.size();
	data["already_valid_count"] = p_task->already_valid_count;
	data["unwrapped_count"] = changed_count;
	data["mesh_data_changed"] = changed_count > 0;
	_free_uv2_unwrap(p_task, true);
	return _ok(data);
}

static Dictionary _solers_scene_state_receipt(const String &p_path) {
	Dictionary data;
	data["path"] = p_path;
	data["authored_state_changed"] = true;
	const int history_id = EditorNode::get_editor_data().get_current_edited_scene_history_id();
	data["history_id"] = history_id;
	if (Node *root = EditorNode::get_singleton() ? EditorNode::get_singleton()->get_edited_scene() : nullptr) {
		data["root_object_id"] = (int64_t)root->get_instance_id();
		data["scene_path"] = root->get_scene_file_path();
	}
	EditorUndoRedoManager *manager = EditorUndoRedoManager::get_singleton();
	if (manager && history_id != EditorUndoRedoManager::INVALID_HISTORY) {
		data["version"] = (int64_t)manager->get_or_create_history(history_id).undo_redo->get_version();
	}
	return data;
}

Dictionary SolersReflectionService::open_scene(const Dictionary &p_args) {
	EditorNode *editor = EditorNode::get_singleton();
	EditorInterface *editor_interface = EditorInterface::get_singleton();
	if (!editor || !editor_interface) {
		return _error("EDITOR_UNAVAILABLE", "Editor is not available.", false);
	}
	String path = ProjectSettings::get_singleton()->localize_path(String(p_args.get("path", String())).strip_edges());
	if (path.is_empty() || !path.begins_with("res://")) {
		return _error("SCENE_PATH_REQUIRED", "scene.open requires a res:// path.", true);
	}
	if (!FileAccess::exists(path)) {
		return _error("SCENE_NOT_FOUND", vformat("Scene file not found: %s", path), true);
	}
	if (editor->is_changing_scene()) {
		return _error("SCENE_BUSY", "Editor is already changing scenes; retry shortly.", true);
	}
	editor_interface->open_scene_from_path(path, p_args.get("set_inherited", false));
	return _ok(_solers_scene_state_receipt(path));
}

Dictionary SolersReflectionService::unwrap_uv2(const Dictionary &p_args, const String &p_operation_id) {
	_sweep_uv2_unwrap_tasks();
	if (!uv2_unwrap_tasks.is_empty()) {
		return _error("UV2_WORKER_BUSY", "A cancelled UV2 worker is still finishing its current native unwrap. Retry this call after it stops.");
	}
	const Array node_paths = p_args.get("node_paths", Array());
	if (node_paths.is_empty()) {
		return _error("INVALID_ARGUMENT", "node_paths must contain at least one MeshInstance3D.");
	}
	UV2UnwrapTask *task = memnew(UV2UnwrapTask);
	task->operation_id = p_operation_id.is_empty() ? (String::num_uint64(OS::get_singleton()->get_ticks_usec()) + JSON::stringify(node_paths)).md5_text() : p_operation_id;
	task->node_paths = node_paths.duplicate();
	task->replacement_meshes.resize(node_paths.size());
	HashSet<String> seen_paths;
	for (int i = 0; i < node_paths.size(); i++) {
		const String path = String(node_paths[i]).strip_edges();
		if (path.is_empty() || seen_paths.has(path)) {
			memdelete(task);
			return _error("INVALID_ARGUMENT", vformat("node_paths[%d] is empty or duplicated.", i));
		}
		seen_paths.insert(path);
		task->node_paths[i] = path;
		String resolve_error;
		MeshInstance3D *node = Object::cast_to<MeshInstance3D>(_resolve_node(path, resolve_error));
		if (!node || node->get_mesh().is_null()) {
			memdelete(task);
			return _error("MESH_NODE_NOT_FOUND", resolve_error.is_empty() ? vformat("node_paths[%d] is not a MeshInstance3D with a mesh.", i) : resolve_error);
		}
		const Ref<Mesh> original = node->get_mesh();
		Ref<ArrayMesh> array_mesh = original;
		Ref<PrimitiveMesh> primitive = original;
		if (!_solers_mesh_has_uv2(original) && array_mesh.is_null() && primitive.is_null()) {
			memdelete(task);
			return _error("UNSUPPORTED_MESH", vformat("Only PrimitiveMesh and ArrayMesh can be unwrapped: %s", path));
		}
		if (!_solers_mesh_has_uv2(original) && array_mesh.is_valid()) {
			if (array_mesh->get_blend_shape_count() > 0) {
				memdelete(task);
				return _error("MESH_NOT_UNWRAPPABLE", vformat("Meshes with blend shapes cannot be lightmap unwrapped: %s", path));
			}
			for (int surface = 0; surface < array_mesh->get_surface_count(); surface++) {
				if (array_mesh->surface_get_primitive_type(surface) != Mesh::PRIMITIVE_TRIANGLES || !(array_mesh->surface_get_format(surface) & Mesh::ARRAY_FORMAT_NORMAL)) {
					memdelete(task);
					return _error("MESH_NOT_UNWRAPPABLE", vformat("Mesh surfaces must be triangles with normals: %s", path));
				}
			}
		}
		task->node_ids.push_back(node->get_instance_id());
		task->original_meshes.push_back(original);
	}
	uv2_unwrap_tasks[task->operation_id] = task;
	return _advance_uv2_unwrap(task);
}

Dictionary SolersReflectionService::poll_uv2_unwrap(const Dictionary &p_args) {
	const String operation_id = String(p_args.get("_uv2_id", String())).strip_edges();
	UV2UnwrapTask *const *stored = uv2_unwrap_tasks.getptr(operation_id);
	if (!stored || !*stored) {
		return _error("UV2_OPERATION_NOT_FOUND", "The pending UV2 operation no longer exists.", false);
	}
	UV2UnwrapTask *task = *stored;
	if (task->cancelled.is_set()) {
		return _fail_uv2_unwrap(task, "UV2_UNWRAP_CANCELLED", "The UV2 operation was cancelled.");
	}
	if (task->worker_active) {
		if (!task->worker_done.is_set()) {
			return _pending_uv2_unwrap(task, "unwrapping");
		}
		task->worker.wait_to_finish();
		task->worker_active = false;
		if (task->worker_error != OK) {
			return _fail_uv2_unwrap(task, "UV2_UNWRAP_FAILED", vformat("Godot lightmap_unwrap failed for %s (error %d).", String(task->node_paths[task->current_index]), task->worker_error));
		}
		if (!_solers_mesh_has_uv2(task->current_mesh)) {
			return _fail_uv2_unwrap(task, "UV2_UNWRAP_INVALID", vformat("Godot returned a mesh without UV2 for %s.", String(task->node_paths[task->current_index])));
		}
		task->replacement_meshes.write[task->current_index] = task->current_mesh;
		task->current_mesh.unref();
		task->next_index = task->current_index + 1;
		task->current_index = -1;
	}
	return _advance_uv2_unwrap(task);
}

bool SolersReflectionService::is_uv2_unwrap_ready(const Dictionary &p_args) const {
	const String operation_id = String(p_args.get("_uv2_id", String())).strip_edges();
	UV2UnwrapTask *const *stored = uv2_unwrap_tasks.getptr(operation_id);
	if (!stored || !*stored || (*stored)->cancelled.is_set()) {
		return true;
	}
	return !(*stored)->worker_active || (*stored)->worker_done.is_set();
}

void SolersReflectionService::cancel_uv2_unwrap(const String &p_operation_id) {
	UV2UnwrapTask *const *stored = uv2_unwrap_tasks.getptr(p_operation_id);
	if (stored && *stored) {
		(*stored)->cancelled.set();
	}
}

static String _solers_lightmap_bake_error(LightmapGI::BakeError p_error) {
	switch (p_error) {
		case LightmapGI::BAKE_ERROR_OK: {
			return "OK";
		}
		case LightmapGI::BAKE_ERROR_NO_SCENE_ROOT: {
			return "NO_SCENE_ROOT";
		}
		case LightmapGI::BAKE_ERROR_FOREIGN_DATA: {
			return "FOREIGN_DATA";
		}
		case LightmapGI::BAKE_ERROR_NO_LIGHTMAPPER: {
			return "NO_LIGHTMAPPER";
		}
		case LightmapGI::BAKE_ERROR_NO_SAVE_PATH: {
			return "NO_SAVE_PATH";
		}
		case LightmapGI::BAKE_ERROR_NO_MESHES: {
			return "NO_MESHES";
		}
		case LightmapGI::BAKE_ERROR_MESHES_INVALID: {
			return "MESHES_INVALID";
		}
		case LightmapGI::BAKE_ERROR_CANT_CREATE_IMAGE: {
			return "CANT_CREATE_IMAGE";
		}
		case LightmapGI::BAKE_ERROR_USER_ABORTED: {
			return "USER_ABORTED";
		}
		case LightmapGI::BAKE_ERROR_TEXTURE_SIZE_TOO_SMALL: {
			return "TEXTURE_SIZE_TOO_SMALL";
		}
		case LightmapGI::BAKE_ERROR_LIGHTMAP_TOO_SMALL: {
			return "LIGHTMAP_TOO_SMALL";
		}
		case LightmapGI::BAKE_ERROR_ATLAS_TOO_SMALL: {
			return "ATLAS_TOO_SMALL";
		}
	}
	return "UNKNOWN";
}

static bool _solers_lightmap_bake_step(float p_progress, const String &p_description, void *p_userdata, bool p_refresh) {
	EditorProgress *progress = static_cast<EditorProgress *>(p_userdata);
	return progress && progress->step(p_description, Math::round(p_progress * 1000.0f), p_refresh);
}

static void _solers_collect_lightmap_scope(Node *p_node, Node *p_bake_root, Array &r_eligible, Array &r_excluded) {
	if (!p_node) {
		return;
	}
	if (MeshInstance3D *mesh_instance = Object::cast_to<MeshInstance3D>(p_node)) {
		Array reasons;
		const Ref<Mesh> mesh = mesh_instance->get_mesh();
		if (!mesh_instance->is_visible_in_tree()) {
			reasons.push_back("not_visible_in_tree");
		}
		if (mesh_instance->get_gi_mode() != GeometryInstance3D::GI_MODE_STATIC) {
			reasons.push_back("gi_mode_not_static");
		}
		if (mesh.is_null()) {
			reasons.push_back("missing_mesh");
		} else if (mesh->get_surface_count() == 0) {
			reasons.push_back("no_surfaces");
		} else {
			bool has_normals = true;
			bool has_uv2 = true;
			bool triangles_only = true;
			for (int surface = 0; surface < mesh->get_surface_count(); surface++) {
				const uint64_t format = mesh->surface_get_format(surface);
				has_normals = has_normals && (bool)(format & Mesh::ARRAY_FORMAT_NORMAL);
				has_uv2 = has_uv2 && (bool)(format & Mesh::ARRAY_FORMAT_TEX_UV2);
				triangles_only = triangles_only && mesh->surface_get_primitive_type(surface) == Mesh::PRIMITIVE_TRIANGLES;
			}
			if (!triangles_only) {
				reasons.push_back("non_triangle_surface");
			}
			if (!has_normals) {
				reasons.push_back("missing_normals");
			}
			if (!has_uv2) {
				reasons.push_back("missing_uv2");
			}
		}
		Dictionary item;
		item["path"] = String(p_bake_root->get_path_to(mesh_instance));
		if (reasons.is_empty()) {
			r_eligible.push_back(item);
		} else {
			item["reasons"] = reasons;
			r_excluded.push_back(item);
		}
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		_solers_collect_lightmap_scope(p_node->get_child(i), p_bake_root, r_eligible, r_excluded);
	}
}

Dictionary SolersReflectionService::bake_lightmap(const Dictionary &p_args) {
	EditorInterface *editor_interface = EditorInterface::get_singleton();
	if (!editor_interface || !editor_interface->get_edited_scene_root()) {
		return _error("EDITOR_CONTEXT_UNAVAILABLE", "Lightmap baking requires an edited scene.", false);
	}
	const String node_path = String(p_args.get("node_path", String())).strip_edges();
	String resolve_error;
	LightmapGI *lightmap = Object::cast_to<LightmapGI>(_resolve_node(node_path, resolve_error));
	if (!lightmap) {
		return _error("LIGHTMAP_NODE_NOT_FOUND", resolve_error.is_empty() ? "node_path must reference a LightmapGI." : resolve_error);
	}
	String data_path = String(p_args.get("data_path", String())).strip_edges();
	if (data_path.is_empty()) {
		Node *root = editor_interface->get_edited_scene_root();
		const String scene_path = root ? root->get_scene_file_path() : String();
		if (scene_path.is_empty()) {
			return _error("UNSAVED_SCENE", "Save the scene before baking LightmapGI.");
		}
		data_path = scene_path.get_basename() + ".lmbake";
	}
	if (!data_path.begins_with("res://") || data_path.get_extension().to_lower() != "lmbake") {
		return _error("INVALID_TARGET", "data_path must be a res:// path ending in .lmbake.");
	}
	Node *bake_root = lightmap == editor_interface->get_edited_scene_root() ? static_cast<Node *>(lightmap) : lightmap->get_parent();
	Array eligible_meshes;
	Array excluded_meshes;
	_solers_collect_lightmap_scope(bake_root, bake_root, eligible_meshes, excluded_meshes);
	Dictionary scope;
	scope["bake_root"] = String(editor_interface->get_edited_scene_root()->get_path_to(bake_root));
	scope["eligible_mesh_count"] = eligible_meshes.size();
	scope["eligible_meshes"] = eligible_meshes;
	scope["excluded_meshes"] = excluded_meshes;
	if (eligible_meshes.is_empty()) {
		Dictionary result = _error("LIGHTMAP_SCOPE_EMPTY", "The LightmapGI parent subtree contains no visible GI_MODE_STATIC MeshInstance3D with triangle surfaces, normals, and UV2. Place the LightmapGI so its native bake root contains the intended meshes or fix the reported mesh facts.");
		result["data"] = scope;
		return result;
	}
	const uint64_t input_digest = get_lightmap_input_digest();
	EditorProgress progress("solers_bake_lightmaps", TTR("Bake Lightmaps"), 1000, true);
	const LightmapGI::BakeError bake_error = lightmap->bake(bake_root, data_path, _solers_lightmap_bake_step, &progress);
	if (bake_error != LightmapGI::BAKE_ERROR_OK) {
		return _error("LIGHTMAP_BAKE_FAILED", vformat("Godot LightmapGI bake failed: %s.", _solers_lightmap_bake_error(bake_error)));
	}
	Ref<LightmapGIData> light_data = lightmap->get_light_data();
	if (light_data.is_null() || light_data->get_user_count() == 0) {
		return _error("LIGHTMAP_BAKE_INVALID", "Godot reported success but produced no LightmapGI users.");
	}
	if (!FileAccess::exists(data_path) || light_data->get_path().simplify_path() != data_path.simplify_path()) {
		return _error("LIGHTMAP_BAKE_INVALID", "Godot reported success but the current LightmapGIData is not bound to the requested .lmbake resource.");
	}
	Array user_paths;
	for (int i = 0; i < light_data->get_user_count(); i++) {
		const NodePath user_path = light_data->get_user_path(i);
		if (!lightmap->get_node_or_null(user_path)) {
			return _error("LIGHTMAP_BAKE_INVALID", vformat("Baked LightmapGI user does not resolve in the current scene: %s", String(user_path)));
		}
		user_paths.push_back(String(user_path));
	}
	Dictionary artifact;
	artifact["kind"] = "lightmap";
	artifact["input_geometry_digest"] = String::num_uint64(input_digest);
	artifact["lightmap_node_path"] = node_path;
	artifact["data_path"] = light_data->get_path();
	artifact["user_paths"] = user_paths;
	artifact["lightmap_input_digest"] = String::num_uint64(input_digest);
	lightmap->set_meta(SNAME("_solers_lightmap_input_digest"), String::num_uint64(input_digest));
	lightmap->set_meta(SNAME("_solers_lightmap_data_path"), light_data->get_path());
	Dictionary data;
	data["artifact"] = artifact;
	data["data_path"] = light_data->get_path();
	data["user_count"] = light_data->get_user_count();
	data["scope"] = scope;
	return _ok(data);
}

Array SolersReflectionService::resolve_batch_resource_access(const Dictionary &p_args) const {
	Array accesses;
	const Variant operations_value = p_args.get("operations", Variant());
	if (operations_value.get_type() != Variant::ARRAY) {
		_solers_add_scene_access(accesses, "write", "*");
		return accesses;
	}
	const Array operations = operations_value;
	for (int i = 0; i < operations.size(); i++) {
		if (operations[i].get_type() != Variant::DICTIONARY) {
			_solers_add_scene_access(accesses, "write", "*");
			continue;
		}
		const Dictionary raw_op = operations[i];
		const SolersBatchOperationKind kind = _solers_batch_operation_kind(raw_op.get("op", String()));
		const Dictionary op = _solers_normalize_batch_operation(raw_op, kind);
		switch (kind) {
			case SolersBatchOperationKind::CREATE_NODE:
				_solers_add_scene_access(accesses, "write", op.get("parent_path", op.get("parent", ".")));
				break;
			case SolersBatchOperationKind::INSTANTIATE_SCENE: {
				Dictionary resource;
				resource["mode"] = "read";
				resource["key"] = "project:" + String(op.get("resource_path", String())).replace_char('\\', '/').simplify_path();
				accesses.push_back(resource);
				_solers_add_scene_access(accesses, "write", op.get("parent_path", "."));
			} break;
			case SolersBatchOperationKind::SET_PROPERTY:
			case SolersBatchOperationKind::ATTACH_SCRIPT:
			case SolersBatchOperationKind::REMOVE_NODE:
				_solers_add_scene_access(accesses, "write", op.get("node_path", "."));
				break;
			case SolersBatchOperationKind::REPARENT:
				_solers_add_scene_access(accesses, "write", op.get("node_path", "."));
				_solers_add_scene_access(accesses, "write", op.get("new_parent_path", op.get("new_parent", ".")));
				break;
			case SolersBatchOperationKind::CONNECT_SIGNAL:
				_solers_add_scene_access(accesses, "write", op.get("source_path", "."));
				_solers_add_scene_access(accesses, "write", op.get("target_path", "."));
				break;
			default:
				_solers_add_scene_access(accesses, "write", "*");
				break;
		}
	}
	return accesses;
}

// Compact world-space digest of every 3D node a successful batch touched, so
// the model sees the resulting global transforms and bounds without a
// follow-up snapshot round-trip (local-vs-global confusion was the top source
// of misplaced geometry).
// Where a node is, how big it is, and whether it is drawn: the engine's own
// answers, shared by every read that needs them so no caller has to rebuild
// (and drift on) the shape.
Dictionary SolersReflectionService::_spatial_facts(Node3D *p_node) const {
	Dictionary facts;
	if (!p_node || !p_node->is_inside_tree()) {
		return facts;
	}
	facts["global_position"] = _solers_vector3_array(p_node->get_global_position());
	facts["global_rotation_degrees"] = _solers_vector3_array(p_node->get_global_rotation_degrees());
	facts["global_scale"] = _solers_vector3_array(p_node->get_global_basis().get_scale());
	facts["visible_in_tree"] = p_node->is_visible_in_tree();
	const Dictionary geometry = solers_describe_geometry(p_node, true);
	if (geometry.has("aabb")) {
		facts["world_aabb"] = geometry["aabb"];
		facts["geometry"] = geometry;
	}
	return facts;
}

static Array _solers_color_array(const Color &p_color) {
	Array values;
	values.push_back(p_color.r);
	values.push_back(p_color.g);
	values.push_back(p_color.b);
	values.push_back(p_color.a);
	return values;
}

static String _solers_resource_id(const Ref<Resource> &p_resource) {
	if (p_resource.is_null()) {
		return String();
	}
	const String path = p_resource->get_path();
	return path.is_empty() ? p_resource->get_class() : vformat("%s (%s)", path, p_resource->get_class());
}

static Dictionary _solers_skeleton_facts(Skeleton3D *p_skeleton) {
	Dictionary facts;
	const Transform3D skeleton_to_world = p_skeleton->get_global_transform();
	Array bones;
	for (int i = 0; i < p_skeleton->get_bone_count(); i++) {
		Dictionary bone;
		bone["index"] = i;
		bone["name"] = p_skeleton->get_bone_name(i);
		bone["parent"] = p_skeleton->get_bone_parent(i);
		// The posed world position is what decides where an attachment lands;
		// the rest pose alone answers nothing once an animation is applied.
		const Transform3D pose = skeleton_to_world * p_skeleton->get_bone_global_pose(i);
		const Vector3 euler = pose.basis.get_euler();
		bone["global_position"] = _solers_vector3_array(pose.origin);
		bone["global_rotation_degrees"] = _solers_vector3_array(Vector3(Math::rad_to_deg(euler.x), Math::rad_to_deg(euler.y), Math::rad_to_deg(euler.z)));
		bone["posed"] = !p_skeleton->get_bone_pose(i).is_equal_approx(p_skeleton->get_bone_rest(i));
		bones.push_back(bone);
	}
	facts["bone_count"] = p_skeleton->get_bone_count();
	facts["bones"] = bones;
	return facts;
}

static Dictionary _solers_animation_facts(AnimationMixer *p_mixer) {
	Dictionary facts;
	facts["animation_active"] = p_mixer->is_active();
	facts["animation_root_node"] = String(p_mixer->get_root_node());
	LocalVector<StringName> animation_names;
	p_mixer->get_animation_list(&animation_names);
	Array animations;
	for (const StringName &name : animation_names) {
		animations.push_back(String(name));
	}
	facts["animations"] = animations;
	if (AnimationPlayer *player = Object::cast_to<AnimationPlayer>(p_mixer)) {
		facts["playing"] = player->is_playing();
		facts["current_animation"] = String(player->get_current_animation());
		facts["assigned_animation"] = String(player->get_assigned_animation());
		facts["autoplay"] = String(player->get_autoplay());
		facts["position"] = player->get_current_animation_position();
		facts["length"] = player->get_current_animation_length();
		facts["speed_scale"] = player->get_speed_scale();
	}
	if (AnimationTree *tree = Object::cast_to<AnimationTree>(p_mixer)) {
		// An invalid tree silently drives nothing; the engine already knows why.
		facts["tree_state_invalid"] = tree->is_state_invalid();
		if (tree->is_state_invalid()) {
			facts["tree_invalid_reason"] = tree->get_editor_error_message();
		}
		facts["tree_root"] = _solers_resource_id(tree->get_root_animation_node());
		const Ref<AnimationNodeStateMachinePlayback> state_machine = tree->get(SNAME("parameters/playback"));
		if (state_machine.is_valid()) {
			facts["state_machine_playing"] = state_machine->is_playing();
			facts["state_machine_current_node"] = String(state_machine->get_current_node());
		}
	}
	return facts;
}

static Dictionary _solers_particles_facts(GeometryInstance3D *p_particles) {
	Dictionary facts;
	if (GPUParticles3D *gpu = Object::cast_to<GPUParticles3D>(p_particles)) {
		facts["emitting"] = gpu->is_emitting();
		facts["one_shot"] = gpu->get_one_shot();
		facts["amount"] = gpu->get_amount();
		facts["lifetime"] = gpu->get_lifetime();
		facts["local_coords"] = gpu->get_use_local_coordinates();
		facts["process_material"] = _solers_resource_id(gpu->get_process_material());
		facts["draw_pass_mesh"] = gpu->get_draw_passes() > 0 ? _solers_resource_id(gpu->get_draw_pass_mesh(0)) : String();
		facts["visibility_aabb"] = _solers_aabb_data(gpu->get_visibility_aabb());
		// The server holds the only live answer to "did anything actually
		// spawn, and where": a configured emitter can still draw nothing.
		const RID particles = gpu->get_base();
		facts["simulation_inactive"] = RenderingServer::get_singleton()->particles_is_inactive(particles);
		facts["live_world_aabb"] = _solers_aabb_data(gpu->get_global_transform().xform(RenderingServer::get_singleton()->particles_get_current_aabb(particles)));
	} else if (CPUParticles3D *cpu = Object::cast_to<CPUParticles3D>(p_particles)) {
		facts["emitting"] = cpu->is_emitting();
		facts["one_shot"] = cpu->get_one_shot();
		facts["amount"] = cpu->get_amount();
		facts["lifetime"] = cpu->get_lifetime();
		facts["local_coords"] = cpu->get_use_local_coordinates();
		facts["draw_pass_mesh"] = _solers_resource_id(cpu->get_mesh());
	}
	return facts;
}

static Dictionary _solers_material_facts(MeshInstance3D *p_mesh_instance) {
	Dictionary facts;
	const Ref<Mesh> mesh = p_mesh_instance->get_mesh();
	facts["mesh"] = _solers_resource_id(mesh);
	if (mesh.is_null()) {
		return facts;
	}
	Array surfaces;
	for (int i = 0; i < mesh->get_surface_count(); i++) {
		Dictionary surface;
		surface["index"] = i;
		// get_active_material resolves Godot's own precedence, so the answer is
		// what will actually be drawn rather than what a property dump lists.
		const Ref<Material> active = p_mesh_instance->get_active_material(i);
		surface["active_material"] = _solers_resource_id(active);
		const Ref<Material> override_material = p_mesh_instance->get_material_override();
		if (override_material.is_valid()) {
			surface["source"] = "material_override";
		} else if (i < p_mesh_instance->get_surface_override_material_count() && p_mesh_instance->get_surface_override_material(i).is_valid()) {
			surface["source"] = "surface_override";
		} else {
			surface["source"] = "mesh";
		}
		if (BaseMaterial3D *base_material = Object::cast_to<BaseMaterial3D>(active.ptr())) {
			surface["shading_mode"] = (int)base_material->get_shading_mode();
			surface["albedo"] = _solers_color_array(base_material->get_albedo());
			surface["albedo_texture"] = _solers_resource_id(base_material->get_texture(BaseMaterial3D::TEXTURE_ALBEDO));
			surface["metallic"] = base_material->get_metallic();
			surface["roughness"] = base_material->get_roughness();
			surface["transparency"] = (int)base_material->get_transparency();
		}
		surfaces.push_back(surface);
	}
	facts["surfaces"] = surfaces;
	facts["material_overlay"] = _solers_resource_id(p_mesh_instance->get_material_overlay());
	return facts;
}

static Dictionary _solers_camera_attributes_facts(const Ref<CameraAttributes> &p_attributes) {
	Dictionary facts;
	if (p_attributes.is_null()) {
		return facts;
	}
	facts["class_name"] = p_attributes->get_class();
	facts["exposure_multiplier"] = p_attributes->get_exposure_multiplier();
	facts["exposure_sensitivity"] = p_attributes->get_exposure_sensitivity();
	facts["auto_exposure_enabled"] = p_attributes->is_auto_exposure_enabled();
	if (const CameraAttributesPhysical *physical = Object::cast_to<CameraAttributesPhysical>(p_attributes.ptr())) {
		facts["exposure_aperture"] = physical->get_aperture();
		facts["exposure_shutter_speed"] = physical->get_shutter_speed();
	}
	return facts;
}

static Dictionary _solers_light_facts(Light3D *p_light) {
	Dictionary facts;
	const bool physical_units = GLOBAL_GET("rendering/lights_and_shadows/use_physical_light_units");
	facts["use_physical_light_units"] = physical_units;
	facts["light_color"] = _solers_color_array(p_light->get_color());
	// PARAM_ENERGY is the dimensionless multiplier; PARAM_INTENSITY is lux/lumens
	// when physical units are on. Report both ClassDB axes — never conflate them.
	facts["light_energy"] = p_light->get_param(Light3D::PARAM_ENERGY);
	facts["light_intensity"] = p_light->get_param(Light3D::PARAM_INTENSITY);
	if (physical_units) {
		if (p_light->get_light_type() == RSE::LIGHT_DIRECTIONAL) {
			facts["light_intensity_lux"] = p_light->get_param(Light3D::PARAM_INTENSITY);
		} else {
			facts["light_intensity_lumens"] = p_light->get_param(Light3D::PARAM_INTENSITY);
		}
		facts["light_temperature"] = p_light->get_temperature();
	}
	facts["light_indirect_energy"] = p_light->get_param(Light3D::PARAM_INDIRECT_ENERGY);
	facts["light_specular"] = p_light->get_param(Light3D::PARAM_SPECULAR);
	facts["shadow_enabled"] = p_light->has_shadow();
	facts["editor_only"] = p_light->is_editor_only();
	facts["cull_mask"] = (int64_t)p_light->get_cull_mask();
	facts["bake_mode"] = (int)p_light->get_bake_mode();
	if (DirectionalLight3D *directional = Object::cast_to<DirectionalLight3D>(p_light)) {
		facts["sky_mode"] = (int)directional->get_sky_mode();
		facts["light_angular_distance"] = directional->get_param(Light3D::PARAM_SIZE);
	}
	return facts;
}

static Dictionary _solers_environment_facts(const Ref<Environment> &p_environment) {
	Dictionary facts;
	facts["background"] = (int)p_environment->get_background();
	facts["background_color"] = _solers_color_array(p_environment->get_bg_color());
	facts["background_energy"] = p_environment->get_bg_energy_multiplier();
	facts["background_intensity"] = p_environment->get_bg_intensity();
	facts["sky"] = _solers_resource_id(p_environment->get_sky());
	facts["ambient_source"] = (int)p_environment->get_ambient_source();
	facts["ambient_color"] = _solers_color_array(p_environment->get_ambient_light_color());
	facts["ambient_energy"] = p_environment->get_ambient_light_energy();
	facts["ambient_sky_contribution"] = p_environment->get_ambient_light_sky_contribution();
	facts["reflection_source"] = (int)p_environment->get_reflection_source();
	facts["tonemap_mode"] = (int)p_environment->get_tonemapper();
	facts["tonemap_exposure"] = p_environment->get_tonemap_exposure();
	facts["tonemap_white"] = p_environment->get_tonemap_white();
	facts["glow_enabled"] = p_environment->is_glow_enabled();
	facts["ssao_enabled"] = p_environment->is_ssao_enabled();
	facts["ssil_enabled"] = p_environment->is_ssil_enabled();
	facts["sdfgi_enabled"] = p_environment->is_sdfgi_enabled();
	facts["sdfgi_read_sky_light"] = p_environment->is_sdfgi_reading_sky_light();
	facts["fog_enabled"] = p_environment->is_fog_enabled();
	return facts;
}

// Each branch is keyed on the engine's own class, which is the authoritative
// statement of which subsystem owns this node; there is no generic way to read
// a bone pose or a resolved material out of a property list.
Dictionary SolersReflectionService::_subsystem_facts(Node *p_node) const {
	Dictionary facts;
	if (Skeleton3D *skeleton = Object::cast_to<Skeleton3D>(p_node)) {
		facts["skeleton"] = _solers_skeleton_facts(skeleton);
	} else if (BoneAttachment3D *attachment = Object::cast_to<BoneAttachment3D>(p_node)) {
		Dictionary bone_attachment;
		bone_attachment["bone_name"] = attachment->get_bone_name();
		bone_attachment["bone_index"] = attachment->get_bone_idx();
		bone_attachment["external_skeleton"] = attachment->get_use_external_skeleton();
		facts["bone_attachment"] = bone_attachment;
	} else if (AnimationMixer *mixer = Object::cast_to<AnimationMixer>(p_node)) {
		facts["animation"] = _solers_animation_facts(mixer);
	} else if (Object::cast_to<GPUParticles3D>(p_node) || Object::cast_to<CPUParticles3D>(p_node)) {
		facts["particles"] = _solers_particles_facts(Object::cast_to<GeometryInstance3D>(p_node));
	} else if (MeshInstance3D *mesh_instance = Object::cast_to<MeshInstance3D>(p_node)) {
		facts["material"] = _solers_material_facts(mesh_instance);
#ifdef MODULE_CSG_ENABLED
	} else if (Object::cast_to<CSGShape3D>(p_node)) {
		Dictionary material;
		const Variant csg_material = p_node->get("material");
		if (csg_material.get_type() == Variant::OBJECT) {
			material["csg_material"] = _solers_resource_id(csg_material);
		}
		if (GeometryInstance3D *geometry = Object::cast_to<GeometryInstance3D>(p_node)) {
			material["material_override"] = _solers_resource_id(geometry->get_material_override());
			const bool has_override = geometry->get_material_override().is_valid();
			const bool has_csg = csg_material.get_type() == Variant::OBJECT && ((Ref<Resource>)csg_material).is_valid();
			material["resolved"] = has_override ? "material_override" : (has_csg ? "csg_material" : "none");
		}
		facts["material"] = material;
#endif
	} else if (Light3D *light = Object::cast_to<Light3D>(p_node)) {
		facts["light"] = _solers_light_facts(light);
	} else if (Camera3D *camera = Object::cast_to<Camera3D>(p_node)) {
		Dictionary camera_facts;
		camera_facts["current"] = camera->is_current();
		camera_facts["attributes"] = _solers_camera_attributes_facts(camera->get_attributes());
		facts["camera"] = camera_facts;
	} else if (WorldEnvironment *world_environment = Object::cast_to<WorldEnvironment>(p_node)) {
		const Ref<Environment> environment = world_environment->get_environment();
		if (environment.is_valid()) {
			facts["environment"] = _solers_environment_facts(environment);
		} else {
			// An empty WorldEnvironment is the usual reason a scene renders
			// against the fallback gray instead of the authored sky.
			facts["environment_missing"] = true;
		}
		facts["camera_attributes"] = _solers_camera_attributes_facts(world_environment->get_camera_attributes());
	}
	return facts;
}

// The nearest ancestor carrying a scene_file_path is the authority on which
// file a change did *not* enter: Godot writes edits below an instance root as
// overrides in the editing scene, never back into the instanced file.
String SolersReflectionService::_instance_scene_path(Node *p_node) const {
	EditorInterface *editor_interface = EditorInterface::get_singleton();
	Node *edited_root = editor_interface ? editor_interface->get_edited_scene_root() : nullptr;
	for (Node *ancestor = p_node; ancestor && ancestor != edited_root; ancestor = ancestor->get_parent()) {
		const String scene_path = ancestor->get_scene_file_path();
		if (!scene_path.is_empty()) {
			return scene_path;
		}
	}
	return String();
}

Array SolersReflectionService::_nested_instance_scenes(const Array &p_results) const {
	Array scenes;
	HashSet<String> seen;
	for (int i = 0; i < p_results.size(); i++) {
		const Dictionary result = Dictionary(p_results[i]).get("result", Dictionary());
		if (!(bool)result.get("ok", false)) {
			continue;
		}
		const Dictionary result_data = result.get("data", Dictionary());
		const String path = result_data.has("path") ? String(result_data.get("path", String())) : String(result_data.get("node_path", String()));
		if (path.is_empty()) {
			continue;
		}
		String resolve_error;
		const String scene_path = _instance_scene_path(_resolve_node(path, resolve_error));
		if (!scene_path.is_empty() && !seen.has(scene_path)) {
			seen.insert(scene_path);
			scenes.push_back(scene_path);
		}
	}
	return scenes;
}

Array SolersReflectionService::_spatial_digest_for_results(const Array &p_results) const {
	Array digest;
	HashSet<String> seen;
	for (int i = 0; i < p_results.size(); i++) {
		const Dictionary result = Dictionary(p_results[i]).get("result", Dictionary());
		if (!(bool)result.get("ok", false)) {
			continue;
		}
		const Dictionary result_data = result.get("data", Dictionary());
		const String path = result_data.has("path") ? String(result_data.get("path", String())) : String(result_data.get("node_path", String()));
		if (path.is_empty() || seen.has(path)) {
			continue;
		}
		seen.insert(path);
		String resolve_error;
		Node *node = _resolve_node(path, resolve_error);
		Node3D *node_3d = node ? Object::cast_to<Node3D>(node) : nullptr;
		const Dictionary facts = _spatial_facts(node_3d);
		if (facts.is_empty()) {
			continue;
		}
		Dictionary entry = facts;
		entry["path"] = path;
		entry["type"] = node_3d->get_class();
		digest.push_back(entry);
	}
	return digest;
}

SolersReflectionService::SolersReflectionService() {}

SolersReflectionService::~SolersReflectionService() {
	Vector<UV2UnwrapTask *> tasks;
	for (const KeyValue<String, UV2UnwrapTask *> &entry : uv2_unwrap_tasks) {
		tasks.push_back(entry.value);
	}
	for (UV2UnwrapTask *task : tasks) {
		_free_uv2_unwrap(task, true);
	}
}
