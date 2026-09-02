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

#include "modules/solers_ai/core/solers_reflection_service.h"

#include "core/config/project_settings.h"
#include "core/io/file_access.h"
#include "core/io/resource.h"
#include "core/io/resource_loader.h"
#include "core/math/geometry_3d.h"
#include "core/math/random_pcg.h"
#include "core/object/class_db.h"
#include "core/object/script_language.h"
#include "editor/doc/editor_help.h"
#include "editor/editor_interface.h"
#include "editor/editor_node.h"
#include "editor/editor_undo_redo_manager.h"

#include "modules/modules_enabled.gen.h"
#include "modules/solers_ai/core/solers_action_timeline.h"
#include "modules/solers_ai/core/solers_geometry_facts.h"
#include "modules/solers_ai/core/solers_path_utils.h"
#include "modules/solers_ai/core/solers_resource_service.h"
#include "modules/solers_ai/core/solers_tool.h"
#ifdef MODULE_CSG_ENABLED
#include "modules/csg/csg_shape.h"
#endif
#include "scene/3d/bone_attachment_3d.h"
#include "scene/3d/cpu_particles_3d.h"
#include "scene/3d/gpu_particles_3d.h"
#include "scene/3d/light_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/multimesh_instance_3d.h"
#include "scene/3d/node_3d.h"
#include "scene/3d/skeleton_3d.h"
#include "scene/3d/visual_instance_3d.h"
#include "scene/animation/animation_mixer.h"
#include "scene/animation/animation_node_state_machine.h"
#include "scene/animation/animation_player.h"
#include "scene/animation/animation_tree.h"
#include "scene/main/node.h"
#include "scene/main/scene_tree.h"
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"
#include "scene/resources/packed_scene.h"
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
	Node *edited_root = SceneTree::get_singleton()->get_edited_scene_root();
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
	Node *edited_root = SceneTree::get_singleton()->get_edited_scene_root();
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

bool SolersReflectionService::_prepare_property_change(Node *p_node, const String &p_property, const Variant &p_value, Variant &r_old, Variant &r_value, NodePath &r_indexed_path, String &r_error) const {
	if (p_property.is_empty()) {
		r_error = "Property names must not be empty.";
		return false;
	}
	if (!p_property.contains("/") && !p_property.contains(":")) {
		r_old = p_node->get(StringName(p_property));
		return _coerce_value(p_node, StringName(p_property), p_value, r_value, r_error);
	}
	const Vector<StringName> subnames = _property_path_subnames(p_property);
	bool valid = false;
	r_old = p_node->get_indexed(subnames, &valid);
	if (!valid) {
		r_error = vformat("Property path '%s' is not valid on %s.", p_property.replace(":", "/"), p_node->get_class());
		return false;
	}
	PropertyInfo info(r_old.get_type(), subnames[subnames.size() - 1]);
	if (!solers_coerce_variant_value(info, p_value, r_value, r_error)) {
		r_error = vformat("Property path '%s': %s", p_property.replace(":", "/"), r_error);
		return false;
	}
	r_indexed_path = NodePath(Vector<StringName>(), subnames, false);
	return true;
}

bool SolersReflectionService::_apply_initial_properties(Node *p_node, const Dictionary &p_properties, Dictionary &r_applied, String &r_error) const {
	ERR_FAIL_NULL_V(p_node, false);
	for (const Variant *key = p_properties.next(nullptr); key; key = p_properties.next(key)) {
		const String property = String(*key).strip_edges().replace(":", "/");
		Variant old_value;
		Variant value;
		NodePath indexed_path;
		if (!_prepare_property_change(p_node, property, p_properties[*key], old_value, value, indexed_path, r_error)) {
			return false;
		}
		bool valid = false;
		if (indexed_path.is_empty()) {
			p_node->set(StringName(property), value, &valid);
		} else {
			p_node->set_indexed(indexed_path.get_subnames(), value, &valid);
		}
		const Variant actual = indexed_path.is_empty() ? p_node->get(StringName(property)) : p_node->get_indexed(indexed_path.get_subnames());
		if (!valid || (value.get_type() == Variant::OBJECT && actual != value)) {
			r_error = vformat("Setting initial property '%s' failed on %s.", property, p_node->get_class());
			return false;
		}
		r_applied[property] = solers_summarize_display_value(actual);
	}
	return true;
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
		const int required_argument_count = method.arguments.size() - method.default_arguments.size();
		Dictionary md;
		md["name"] = method.name;
		md["return_type"] = Variant::get_type_name(method.return_val.type);
		md["return_class_name"] = String(method.return_val.class_name);
		md["return_hint_string"] = method.return_val.hint_string;
		md["is_const"] = (bool)(method.flags & METHOD_FLAG_CONST);
		md["is_vararg"] = (bool)(method.flags & METHOD_FLAG_VARARG);
		md["required_argument_count"] = required_argument_count;
		Array args_out;
		for (int i = 0; i < method.arguments.size(); i++) {
			const PropertyInfo &arg = method.arguments[i];
			Dictionary ad;
			ad["name"] = arg.name;
			ad["type"] = Variant::get_type_name(arg.type);
			ad["class_name"] = String(arg.class_name);
			ad["hint_string"] = arg.hint_string;
			if (i >= required_argument_count) {
				ad["default_value"] = solers_summarize_display_value(method.default_arguments[i - required_argument_count]);
			}
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
		const Dictionary wire_shape = solers_variant_wire_shape(property.type);
		if (!wire_shape.is_empty()) {
			pd["wire_shape"] = wire_shape;
		}
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

Dictionary SolersReflectionService::update_node(const Dictionary &p_args) {
	const String node_path = p_args.get("node_path", ".");
	const Variant properties_value = p_args.get("properties", Variant());
	if (properties_value.get_type() != Variant::DICTIONARY || Dictionary(properties_value).is_empty()) {
		return _error("INVALID_ARGUMENT", "properties must be a non-empty object.");
	}
	String error;
	Node *node = _resolve_node(node_path, error);
	if (!node) {
		Dictionary result = _error("NODE_NOT_FOUND", error);
		Dictionary data;
		data["requested_node_path"] = node_path;
		if (Node *root = SceneTree::get_singleton()->get_edited_scene_root()) {
			data["edited_root"] = solers_native_object_handle(root);
			data["query"] = Dictionary({ { "target", "scene" }, { "node_paths", Array({ ".", node_path }) } });
		}
		result["data"] = data;
		return result;
	}
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	ERR_FAIL_NULL_V(undo_redo, _error("UNDO_REDO_UNAVAILABLE", "EditorUndoRedoManager is not available.", false));
	const Dictionary properties = properties_value;
	Array changes;
	for (const Variant *key = properties.next(nullptr); key; key = properties.next(key)) {
		const String property = String(*key).strip_edges().replace(":", "/");
		Variant old_value;
		Variant value;
		NodePath indexed_path;
		if (!_prepare_property_change(node, property, properties[*key], old_value, value, indexed_path, error)) {
			return _error("INVALID_PROPERTY_VALUE", error);
		}
		if (old_value == value) {
			continue;
		}
		changes.push_back(Dictionary({ { "property", property }, { "old_value", old_value }, { "value", value }, { "indexed_path", indexed_path } }));
	}
	if (changes.is_empty()) {
		return _error("STATE_ALREADY_SATISFIED", "All requested properties already have the authoritative values.");
	}

	undo_redo->create_action(vformat("Solers: Update %s", node->get_name()), UndoRedo::MERGE_DISABLE, node);
	for (const Variant &item : changes) {
		const Dictionary change = item;
		const String property = change.get("property", String());
		const Variant value = change.get("value", Variant());
		const Variant old_value = change.get("old_value", Variant());
		const NodePath indexed_path = change.get("indexed_path", NodePath());
		if (indexed_path.is_empty()) {
			undo_redo->add_do_property(node, StringName(property), value);
			undo_redo->add_undo_property(node, StringName(property), old_value);
		} else {
			undo_redo->add_do_method(node, "set_indexed", indexed_path, value);
			undo_redo->add_undo_method(node, "set_indexed", indexed_path, old_value);
		}
	}
	undo_redo->commit_action();

	Dictionary applied;
	for (const Variant &item : changes) {
		const Dictionary change = item;
		const String property = change.get("property", String());
		const NodePath indexed_path = change.get("indexed_path", NodePath());
		const Variant actual = indexed_path.is_empty() ? node->get(StringName(property)) : node->get_indexed(indexed_path.get_subnames());
		applied[property] = solers_summarize_display_value(actual);
	}
	String safe_path;
	_safe_node_path(node, safe_path);
	Dictionary data;
	data["node_path"] = safe_path;
	data["properties"] = applied;
	Node *edited_root = SceneTree::get_singleton()->get_edited_scene_root();
	data["scene_file_path"] = edited_root ? edited_root->get_scene_file_path() : String();
	data["owner_path"] = node->get_owner() ? String(node->get_owner()->get_path()) : String();
	data["owner_object_id"] = node->get_owner() ? solers_object_id_to_string(node->get_owner()->get_instance_id()) : String();
	data["instance_scene_path"] = _instance_scene_path(node);
	data["node_object_id"] = solers_object_id_to_string(node->get_instance_id());
	data["inside_tree"] = node->is_inside_tree();
	data["edited_scene_root_object_id"] = edited_root ? solers_object_id_to_string(edited_root->get_instance_id()) : String();
	return _ok(data);
}

Dictionary SolersReflectionService::inspect_nodes(const Dictionary &p_args) {
	Array paths = p_args.get("node_paths", Array());
	if (paths.is_empty()) {
		paths.push_back(".");
	}
	const Array requested_properties = p_args.get("properties", Array());
	const bool include_connections = p_args.get("include_connections", false);
	Array nodes;
	Array errors;
	for (int i = 0; i < paths.size(); i++) {
		const String path = String(paths[i]);
		String resolve_error;
		Node *node = _resolve_node(path, resolve_error);
		if (!node) {
			Dictionary failure;
			failure["index"] = i;
			failure["requested_path"] = path;
			failure["error"] = _error("NODE_NOT_FOUND", resolve_error).get("error", Dictionary());
			errors.push_back(failure);
			continue;
		}
		Dictionary item;
		String safe_path;
		_safe_node_path(node, safe_path);
		item["node_path"] = safe_path;
		item["object_id"] = solers_object_id_to_string(node->get_instance_id());
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
		if (!requested_properties.is_empty()) {
			Dictionary values;
			Dictionary property_errors;
			for (int property_index = 0; property_index < requested_properties.size(); property_index++) {
				const String property = requested_properties[property_index];
				bool valid = false;
				const Variant value = node->get_indexed(_property_path_subnames(property), &valid);
				if (valid) {
					values[property] = solers_summarize_display_value(value);
					continue;
				}
				property_errors[property] = _error("INVALID_PROPERTY_PATH", vformat("Property path '%s' is not valid on %s.", property, node->get_class())).get("error", Dictionary());
			}
			item["properties"] = values;
			if (!property_errors.is_empty()) {
				item["property_errors"] = property_errors;
			}
		}
		if (include_connections) {
			Dictionary connection_args;
			connection_args["source_path"] = safe_path;
			const Dictionary connection_result = _list_signal_connections(connection_args);
			if (!(bool)connection_result.get("ok", false)) {
				Dictionary failure;
				failure["index"] = i;
				failure["node_path"] = safe_path;
				failure["error"] = connection_result.get("error", Dictionary());
				errors.push_back(failure);
			} else {
				item["connections"] = Dictionary(connection_result.get("data", Dictionary())).get("connections", Array());
			}
		}
		nodes.push_back(item);
	}
	Dictionary data;
	data["nodes"] = nodes;
	data["count"] = nodes.size();
	data["requested_count"] = paths.size();
	data["errors"] = errors;
	data["error_count"] = errors.size();
	if (nodes.is_empty() && !errors.is_empty()) {
		Dictionary result = _error("NODE_QUERY_FAILED", "None of the requested nodes exist in the live edited scene.");
		result["data"] = data;
		return result;
	}
	return _ok(data);
}

Dictionary SolersReflectionService::create_node(const Dictionary &p_args) {
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
		undo_redo->create_action_for_history("Solers: New Scene Root", EditorNode::get_editor_data().get_current_edited_scene_history_id());
		undo_redo->add_do_method(editor_node, "set_edited_scene", node);
		undo_redo->add_do_reference(node);
		undo_redo->add_undo_method(editor_node, "set_edited_scene", (Object *)nullptr);
		undo_redo->commit_action();
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

	undo_redo->create_action(vformat("Solers: Add %s", type), UndoRedo::MERGE_DISABLE, parent);
	undo_redo->add_do_method(parent, "add_child", node, true);
	undo_redo->add_do_method(node, "set_owner", edited_root);
	undo_redo->add_do_reference(node);
	undo_redo->add_undo_method(parent, "remove_child", node);
	undo_redo->commit_action();

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
	const SolersPath::NormalizedPath normalized_source = SolersPath::normalize_project_path(p_args.get("source_path", String()));
	if (!normalized_source.valid) {
		return _error("INVALID_RESOURCE_PATH", normalized_source.error);
	}
	const String source_path = normalized_source.value;
	const String parent_path = String(p_args.get("parent_path", ".")).strip_edges();
	const String requested_name = String(p_args.get("name", String())).strip_edges();
	const Variant properties_value = p_args.get("properties", Dictionary());
	if (properties_value.get_type() != Variant::DICTIONARY) {
		return _error("INVALID_ARGUMENT", "scene.instance.instantiate properties must be an object.");
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
	Ref<Resource> resource = ResourceLoader::load(source_path, String(), ResourceFormatLoader::CACHE_MODE_REUSE, &load_error);
	Ref<PackedScene> scene = resource;
	Ref<Mesh> mesh = resource;
	if (resource.is_null() || load_error != OK || (scene.is_null() && mesh.is_null())) {
		return _error("UNSUPPORTED_INSTANTIABLE_RESOURCE", vformat("Failed to load '%s' as a PackedScene or Mesh (error %d).", source_path, (int)load_error));
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
		return _error("RESOURCE_INSTANTIATION_FAILED", vformat("Resource produced no scene node: %s", source_path), false);
	}
	const String edited_scene_path = edited_root->get_scene_file_path();
	if (scene.is_valid() && !edited_scene_path.is_empty() && _solers_scene_contains_path(instance, edited_scene_path)) {
		memdelete(instance);
		return _error("CYCLIC_SCENE_DEPENDENCY", vformat("Instantiating '%s' would create a cyclic scene dependency.", source_path));
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
		instance->set_scene_file_path(ProjectSettings::get_singleton()->localize_path(source_path));
	}

	EditorUndoRedoManager *undo_redo = editor_interface->get_editor_undo_redo();
	if (!undo_redo) {
		memdelete(instance);
		return _error("UNDO_REDO_UNAVAILABLE", "EditorUndoRedoManager is not available.", false);
	}
	undo_redo->create_action(vformat("Solers: Instantiate %s", source_path.get_file()), UndoRedo::MERGE_DISABLE, parent);
	undo_redo->add_do_method(parent, "add_child", instance, true);
	undo_redo->add_do_method(instance, "set_owner", edited_root);
	undo_redo->add_do_reference(instance);
	undo_redo->add_undo_method(parent, "remove_child", instance);
	undo_redo->commit_action();

	String safe_path;
	_safe_node_path(instance, safe_path);
	Dictionary data;
	data["source_path"] = source_path;
	data["path"] = safe_path;
	data["name"] = instance->get_name();
	data["type"] = instance->get_class();
	data["resource_type"] = scene.is_valid() ? String("PackedScene") : String("Mesh");
	data["properties"] = applied_properties;
	data["initialized_property_count"] = applied_properties.size();
	data["geometry"] = solers_describe_geometry(instance, true);
	return _ok(data);
}

Dictionary SolersReflectionService::reparent_node(const Dictionary &p_args) {
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
	undo_redo->create_action("Solers: Reparent Node", UndoRedo::MERGE_DISABLE, new_parent);
	undo_redo->add_do_method(old_parent, "remove_child", node);
	undo_redo->add_do_method(new_parent, "add_child", node, true);
	if (new_index >= 0) {
		undo_redo->add_do_method(new_parent, "move_child", node, new_index);
	}
	undo_redo->add_do_method(node, "set_owner", edited_root);
	undo_redo->add_undo_method(new_parent, "remove_child", node);
	undo_redo->add_undo_method(old_parent, "add_child", node, true);
	undo_redo->add_undo_method(old_parent, "move_child", node, old_index);
	undo_redo->add_undo_method(node, "set_owner", old_owner);
	undo_redo->commit_action();

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

Dictionary SolersReflectionService::connect_signal(const Dictionary &p_args) {
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

	undo_redo->create_action(vformat("Solers: Connect %s", signal_name), UndoRedo::MERGE_DISABLE, source);
	undo_redo->add_do_method(source, "connect", signal_sn, callable, flags);
	undo_redo->add_undo_method(source, "disconnect", signal_sn, callable);
	undo_redo->commit_action();

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

Dictionary SolersReflectionService::attach_script(const Dictionary &p_args) {
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

	const SolersPath::NormalizedPath normalized_script = SolersPath::normalize_project_path(script_path);
	if (!normalized_script.valid) {
		return _error("INVALID_PATH", normalized_script.error);
	}
	const String normalized_script_path = normalized_script.value;
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
	undo_redo->create_action("Solers: Attach Script", UndoRedo::MERGE_DISABLE, node);
	undo_redo->add_do_method(node, "set_script", script);
	undo_redo->add_undo_method(node, "set_script", previous_script);
	undo_redo->commit_action();

	String safe_path;
	_safe_node_path(node, safe_path);
	Dictionary data;
	data["node_path"] = safe_path;
	data["script_path"] = normalized_script_path;
	data["had_previous_script"] = previous_script.is_valid();
	return _ok(data);
}

Dictionary SolersReflectionService::remove_node(const Dictionary &p_args) {
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
	const Dictionary removed_object = solers_native_object_handle(node);
	undo_redo->create_action("Solers: Remove Node", UndoRedo::MERGE_DISABLE, parent);
	undo_redo->add_do_method(parent, "remove_child", node);
	undo_redo->add_undo_method(parent, "add_child", node, true);
	undo_redo->add_undo_method(parent, "move_child", node, original_index);
	undo_redo->add_undo_reference(node);
	undo_redo->commit_action();

	String parent_safe_path;
	_safe_node_path(parent, parent_safe_path);
	Dictionary data;
	data["removed_node"] = node_path;
	data["object"] = removed_object;
	data["parent_path"] = parent_safe_path;
	data["original_index"] = original_index;
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

static real_t _solers_axis_min(const AABB &p_bounds, int p_axis) {
	return p_bounds.position[p_axis];
}

static real_t _solers_axis_max(const AABB &p_bounds, int p_axis) {
	return p_bounds.position[p_axis] + p_bounds.size[p_axis];
}

static Dictionary _solers_aabb_data(const AABB &p_bounds) {
	Dictionary data;
	data["position"] = _solers_vector3_array(p_bounds.position);
	data["size"] = _solers_vector3_array(p_bounds.size);
	return data;
}

Dictionary SolersReflectionService::measure_spatial_relations(const Dictionary &p_args) const {
	const Array relations = p_args.get("relations", Array());
	if (relations.is_empty()) {
		return _error("INVALID_ARGUMENT", "relations must contain at least one node pair.");
	}

	Array results;
	int error_count = 0;
	for (int i = 0; i < relations.size(); i++) {
		const Dictionary relation = relations[i].get_type() == Variant::DICTIONARY ? Dictionary(relations[i]) : Dictionary();
		const String a_path = String(relation.get("a", String())).strip_edges();
		const String b_path = String(relation.get("b", String())).strip_edges();
		String resolve_error;
		Dictionary result;
		result["index"] = i;
		result["a"] = a_path;
		result["b"] = b_path;
		Node3D *a = Object::cast_to<Node3D>(_resolve_node(a_path, resolve_error));
		Node3D *b = Object::cast_to<Node3D>(_resolve_node(b_path, resolve_error));
		if (!a || !b) {
			result["error"] = _error("NODE_NOT_FOUND", resolve_error.is_empty() ? "Both relation endpoints must resolve to Node3D." : resolve_error).get("error", Dictionary());
			error_count++;
			results.push_back(result);
			continue;
		}
		for (const KeyValue<String, Node3D *> &endpoint : { KeyValue<String, Node3D *>("a_node", a), KeyValue<String, Node3D *>("b_node", b) }) {
			Dictionary identity = _spatial_facts(endpoint.value);
			identity["node_path"] = endpoint.value->get_path();
			identity["object_id"] = solers_object_id_to_string(endpoint.value->get_instance_id());
			identity["class_name"] = endpoint.value->get_class();
			result[endpoint.key] = identity;
		}
		result["position_delta"] = _solers_vector3_array(b->get_global_position() - a->get_global_position());
		result["position_distance"] = a->get_global_position().distance_to(b->get_global_position());
		GeometryInstance3D *a_geometry = Object::cast_to<GeometryInstance3D>(a);
		GeometryInstance3D *b_geometry = Object::cast_to<GeometryInstance3D>(b);
		if (a_geometry && b_geometry) {
			const AABB a_bounds = a_geometry->get_global_transform().xform(a_geometry->get_aabb());
			const AABB b_bounds = b_geometry->get_global_transform().xform(b_geometry->get_aabb());
			Array axes;
			for (int axis = 0; axis < 3; axis++) {
				Dictionary measure;
				measure["axis"] = axis == 0 ? "x" : (axis == 1 ? "y" : "z");
				measure["gap"] = MAX(MAX(_solers_axis_min(a_bounds, axis) - _solers_axis_max(b_bounds, axis), _solers_axis_min(b_bounds, axis) - _solers_axis_max(a_bounds, axis)), (real_t)0.0);
				measure["overlap"] = MAX(MIN(_solers_axis_max(a_bounds, axis), _solers_axis_max(b_bounds, axis)) - MAX(_solers_axis_min(a_bounds, axis), _solers_axis_min(b_bounds, axis)), (real_t)0.0);
				axes.push_back(measure);
			}
			result["a_world_aabb"] = _solers_aabb_data(a_bounds);
			result["b_world_aabb"] = _solers_aabb_data(b_bounds);
			result["axes"] = axes;
		}
		results.push_back(result);
	}

	Dictionary data;
	data["relations"] = results;
	data["checked_relation_count"] = results.size();
	data["error_count"] = error_count;
	return _ok(data);
}

static Ref<Material> _solers_resolved_material(MeshInstance3D *p_mesh_instance, int p_surface, String *r_source = nullptr) {
	Ref<Material> material = p_mesh_instance->get_material_override();
	String source = "material_override";
	if (!material.is_valid() && p_surface < p_mesh_instance->get_surface_override_material_count()) {
		material = p_mesh_instance->get_surface_override_material(p_surface);
		source = "surface_override";
	}
	if (!material.is_valid()) {
		const Ref<Mesh> mesh = p_mesh_instance->get_mesh();
		material = mesh.is_valid() && p_surface < mesh->get_surface_count() ? mesh->surface_get_material(p_surface) : Ref<Material>();
		source = "mesh";
	}
	if (r_source) {
		*r_source = source;
	}
	return material;
}

static Dictionary _solers_scene_state_receipt(const String &p_path) {
	Dictionary data;
	data["path"] = p_path;
	data["authored_state_changed"] = true;
	const int history_id = EditorNode::get_editor_data().get_current_edited_scene_history_id();
	data["history_id"] = history_id;
	if (Node *root = EditorNode::get_singleton() ? EditorNode::get_singleton()->get_edited_scene() : nullptr) {
		data["root_object_id"] = solers_object_id_to_string(root->get_instance_id());
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
	if (!editor) {
		return _error("EDITOR_UNAVAILABLE", "Editor is not available.", false);
	}
	String path = ProjectSettings::get_singleton()->localize_path(String(p_args.get("path", String())).strip_edges());
	if (path.is_empty() || !path.begins_with("res://")) {
		return _error("SCENE_PATH_REQUIRED", "scene.open requires a res:// path.", true);
	}
	if (!ResourceLoader::exists(path)) {
		return _error("SCENE_NOT_FOUND", vformat("Scene file not found: %s", path), true);
	}
	if (ResourceLoader::is_imported(path)) {
		return _error("IMPORTED_SCENE_READ_ONLY", "Imported scenes are read-only editor resources; inspect or instantiate this resource instead.", true);
	}
	if (ResourceLoader::get_resource_type(path) != "PackedScene") {
		return _error("SCENE_NOT_EDITABLE", vformat("Resource is not an editable PackedScene: %s", path), true);
	}
	if (editor->is_changing_scene()) {
		return _error("SCENE_BUSY", "Editor is already changing scenes; retry shortly.", true);
	}
	const Error err = editor->open_scene(path, false, false, false);
	if (err != OK) {
		return _error("SCENE_OPEN_FAILED", vformat("Godot could not open %s (Error %d).", path, err), true);
	}
	Node *root = editor->get_edited_scene();
	const bool opened = root && root->get_scene_file_path() == path;
	if (!opened) {
		return _error("SCENE_OPEN_POSTCONDITION_FAILED", "Godot did not make the requested scene state current.", true);
	}
	return _ok(_solers_scene_state_receipt(path));
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
		facts["tree_root"] = solers_summarize_display_value(tree->get_root_animation_node());
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
		facts["process_material"] = solers_summarize_display_value(gpu->get_process_material());
		facts["draw_pass_mesh"] = gpu->get_draw_passes() > 0 ? solers_summarize_display_value(gpu->get_draw_pass_mesh(0)) : solers_summarize_display_value(Ref<Mesh>());
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
		facts["draw_pass_mesh"] = solers_summarize_display_value(cpu->get_mesh());
	}
	return facts;
}

static Dictionary _solers_base_material_facts(const Ref<Material> &p_material) {
	Dictionary facts;
	facts["active_material"] = solers_summarize_display_value(p_material);
	BaseMaterial3D *material = Object::cast_to<BaseMaterial3D>(p_material.ptr());
	if (!material) {
		return facts;
	}
	facts["shading_mode"] = (int)material->get_shading_mode();
	facts["transparency"] = (int)material->get_transparency();
	facts["texture_filter"] = (int)material->get_texture_filter();
	facts["texture_repeat"] = material->get_flag(BaseMaterial3D::FLAG_USE_TEXTURE_REPEAT);
	Dictionary channels;
	channels["albedo"] = Dictionary({ { "color", _solers_color_array(material->get_albedo()) }, { "texture", solers_summarize_display_value(material->get_texture(BaseMaterial3D::TEXTURE_ALBEDO)) } });
	channels["metallic"] = Dictionary({ { "value", material->get_metallic() }, { "texture", solers_summarize_display_value(material->get_texture(BaseMaterial3D::TEXTURE_METALLIC)) } });
	channels["roughness"] = Dictionary({ { "value", material->get_roughness() }, { "texture", solers_summarize_display_value(material->get_texture(BaseMaterial3D::TEXTURE_ROUGHNESS)) } });
	channels["normal"] = Dictionary({ { "enabled", material->get_feature(BaseMaterial3D::FEATURE_NORMAL_MAPPING) }, { "scale", material->get_normal_scale() }, { "texture", solers_summarize_display_value(material->get_texture(BaseMaterial3D::TEXTURE_NORMAL)) } });
	channels["ambient_occlusion"] = Dictionary({ { "enabled", material->get_feature(BaseMaterial3D::FEATURE_AMBIENT_OCCLUSION) }, { "light_affect", material->get_ao_light_affect() }, { "texture", solers_summarize_display_value(material->get_texture(BaseMaterial3D::TEXTURE_AMBIENT_OCCLUSION)) } });
	channels["height"] = Dictionary({ { "enabled", material->get_feature(BaseMaterial3D::FEATURE_HEIGHT_MAPPING) }, { "scale", material->get_heightmap_scale() }, { "texture", solers_summarize_display_value(material->get_texture(BaseMaterial3D::TEXTURE_HEIGHTMAP)) } });
	facts["channels"] = channels;
	facts["uv1"] = Dictionary({ { "scale", _solers_vector3_array(material->get_uv1_scale()) }, { "offset", _solers_vector3_array(material->get_uv1_offset()) }, { "triplanar", material->get_flag(BaseMaterial3D::FLAG_UV1_USE_TRIPLANAR) }, { "world_triplanar", material->get_flag(BaseMaterial3D::FLAG_UV1_USE_WORLD_TRIPLANAR) }, { "triplanar_sharpness", material->get_uv1_triplanar_blend_sharpness() } });
	return facts;
}

static Dictionary _solers_material_facts(MeshInstance3D *p_mesh_instance) {
	Dictionary facts;
	const Ref<Mesh> mesh = p_mesh_instance->get_mesh();
	facts["mesh"] = solers_summarize_display_value(mesh);
	if (mesh.is_null()) {
		return facts;
	}
	Array surfaces;
	for (int i = 0; i < mesh->get_surface_count(); i++) {
		Dictionary surface;
		surface["index"] = i;
		String source;
		const Ref<Material> active = _solers_resolved_material(p_mesh_instance, i, &source);
		surface["source"] = source;
		surface.merge(_solers_base_material_facts(active), true);
		surfaces.push_back(surface);
	}
	facts["surfaces"] = surfaces;
	facts["material_overlay"] = solers_summarize_display_value(p_mesh_instance->get_material_overlay());
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
	} else if (CSGShape3D *csg = Object::cast_to<CSGShape3D>(p_node)) {
		Dictionary csg_facts;
		csg_facts["is_root_shape"] = csg->is_root_shape();
		csg_facts["operation"] = (int)csg->get_operation();
		csg_facts["autosmooth"] = csg->is_autosmooth();
#ifndef PHYSICS_3D_DISABLED
		csg_facts["collision_enabled"] = csg->is_using_collision();
#endif
		facts["csg"] = csg_facts;
		Ref<Material> shape_material;
		const Variant csg_material = ClassDB::has_property(p_node->get_class(), SNAME("material")) ? p_node->get("material") : Variant();
		if (csg_material.get_type() == Variant::OBJECT) {
			shape_material = csg_material;
		}
		Ref<Material> material_override;
		if (GeometryInstance3D *geometry = Object::cast_to<GeometryInstance3D>(p_node)) {
			material_override = geometry->get_material_override();
		}
		const Ref<Material> active = material_override.is_valid() ? material_override : shape_material;
		Dictionary material = _solers_base_material_facts(active);
		material["csg_material"] = solers_summarize_display_value(shape_material);
		material["material_override"] = solers_summarize_display_value(material_override);
		material["source"] = material_override.is_valid() ? "material_override" : (shape_material.is_valid() ? "csg_material" : "none");
		facts["material"] = material;
#endif
	}
	return facts;
}

// The nearest ancestor carrying a scene_file_path is the authority on which
// file a change did *not* enter: Godot writes edits below an instance root as
// overrides in the editing scene, never back into the instanced file.
String SolersReflectionService::_instance_scene_path(Node *p_node) const {
	Node *edited_root = SceneTree::get_singleton()->get_edited_scene_root();
	for (Node *ancestor = p_node; ancestor && ancestor != edited_root; ancestor = ancestor->get_parent()) {
		const String scene_path = ancestor->get_scene_file_path();
		if (!scene_path.is_empty()) {
			return scene_path;
		}
	}
	return String();
}
