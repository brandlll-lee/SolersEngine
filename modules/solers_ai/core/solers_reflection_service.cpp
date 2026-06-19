/**************************************************************************/
/*  solers_reflection_service.cpp                                         */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                              */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/
/* Solers: AI-native game engine.                                        */
/**************************************************************************/

#include "solers_reflection_service.h"

#include "core/io/file_access.h"
#include "core/io/resource_loader.h"
#include "core/object/class_db.h"
#include "core/object/script_language.h"
#include "editor/editor_interface.h"
#include "editor/editor_node.h"
#include "editor/editor_undo_redo_manager.h"
#include "modules/solers_ai/core/solers_action_timeline.h"
#include "scene/main/node.h"

static constexpr int SOLERS_INTROSPECT_METHOD_CAP = 400;
static constexpr int SOLERS_INTROSPECT_PROPERTY_CAP = 400;
static constexpr int SOLERS_INTROSPECT_SIGNAL_CAP = 200;
static constexpr int SOLERS_INTROSPECT_CONSTANT_CAP = 400;
static constexpr int SOLERS_BATCH_OP_CAP = 64;

void SolersReflectionService::_bind_methods() {
	ClassDB::bind_method(D_METHOD("introspect_class", "args"), &SolersReflectionService::introspect_class);
	ClassDB::bind_method(D_METHOD("get_property", "args"), &SolersReflectionService::get_property);
	ClassDB::bind_method(D_METHOD("set_property", "args"), &SolersReflectionService::set_property);
	ClassDB::bind_method(D_METHOD("call_method", "args"), &SolersReflectionService::call_method);
	ClassDB::bind_method(D_METHOD("invoke_editor", "args"), &SolersReflectionService::invoke_editor);
	ClassDB::bind_method(D_METHOD("batch", "args"), &SolersReflectionService::batch);
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
	if (p_node_path.is_empty() || p_node_path == "." || p_node_path == String(edited_root->get_name())) {
		return edited_root;
	}
	Node *node = nullptr;
	const NodePath node_path = NodePath(p_node_path);
	if (p_node_path.begins_with("/")) {
		node = Object::cast_to<Node>(edited_root->get_node_or_null(edited_root->get_path().rel_path_to(node_path)));
	} else {
		node = edited_root->get_node_or_null(node_path);
	}
	if (!node) {
		r_error = vformat("Node not found: %s", p_node_path);
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
	Variant::Type expected = Variant::NIL;
	StringName expected_class;
	bool found = false;
	List<PropertyInfo> property_list;
	p_node->get_property_list(&property_list);
	for (const PropertyInfo &property : property_list) {
		if (property.name == p_property) {
			expected = property.type;
			expected_class = property.class_name;
			found = true;
			break;
		}
	}
	if (!found) {
		r_error = vformat("Property '%s' is not exposed by %s. Use class.introspect to list valid properties.", String(p_property), p_node->get_class());
		return false;
	}
	if (p_value.get_type() == expected) {
		r_out = p_value;
		return true;
	}
	if (expected == Variant::OBJECT && p_value.get_type() == Variant::STRING) {
		String path = String(p_value).strip_edges().replace_char('\\', '/').simplify_path();
		if (path.begins_with("res://")) {
			Error load_error = OK;
			Ref<Resource> resource = ResourceLoader::load(path, String(expected_class), ResourceFormatLoader::CACHE_MODE_REUSE, &load_error);
			if (resource.is_null() || load_error != OK) {
				r_error = vformat("Failed to load resource '%s' for property '%s' (error %d).", path, String(p_property), (int)load_error);
				return false;
			}
			if (expected_class != StringName() && !ClassDB::is_parent_class(resource->get_class_name(), expected_class)) {
				r_error = vformat("Resource '%s' is %s, expected %s.", path, resource->get_class(), String(expected_class));
				return false;
			}
			r_out = resource;
			return true;
		}
	}
	// Strings and numbers are coerced by the engine's own setter; pass through.
	switch (expected) {
		case Variant::STRING:
		case Variant::STRING_NAME:
		case Variant::NODE_PATH:
		case Variant::BOOL:
		case Variant::INT:
		case Variant::FLOAT:
		case Variant::NIL: {
			r_out = p_value;
			return true;
		} break;
		default:
			break;
	}
	// Math/packed types accept a JSON array of components, constructed safely
	// via the engine's own constructor (no expression evaluation).
	if (p_value.get_type() == Variant::ARRAY) {
		const Array components = p_value;
		Vector<Variant> argv;
		for (int i = 0; i < components.size(); i++) {
			argv.push_back(components[i]);
		}
		Vector<const Variant *> argp;
		for (int i = 0; i < argv.size(); i++) {
			argp.push_back(&argv[i]);
		}
		Variant constructed;
		Callable::CallError call_error;
		Variant::construct(expected, constructed, argp.ptrw(), argp.size(), call_error);
		if (call_error.error == Callable::CallError::CALL_OK) {
			r_out = constructed;
			return true;
		}
		r_error = vformat("Could not construct %s from the provided array.", Variant::get_type_name(expected));
		return false;
	}
	if (expected == Variant::COLOR && p_value.get_type() == Variant::DICTIONARY) {
		const Dictionary components = p_value;
		if (components.has("r") && components.has("g") && components.has("b")) {
			Vector<Variant> argv;
			argv.push_back(components["r"]);
			argv.push_back(components["g"]);
			argv.push_back(components["b"]);
			argv.push_back(components.get("a", 1.0));
			Vector<const Variant *> argp;
			for (int i = 0; i < argv.size(); i++) {
				argp.push_back(&argv[i]);
			}
			Variant constructed;
			Callable::CallError call_error;
			Variant::construct(expected, constructed, argp.ptrw(), argp.size(), call_error);
			if (call_error.error == Callable::CallError::CALL_OK) {
				r_out = constructed;
				return true;
			}
		}
		r_error = "Could not construct Color from the provided dictionary; expected r, g, b and optional a.";
		return false;
	}
	// Last resort: hand the raw value to the setter and let the engine try.
	r_out = p_value;
	return true;
}

static Variant _reflect_displayable(const Variant &p_value) {
	if (p_value.get_type() != Variant::OBJECT) {
		return p_value;
	}
	Object *object = p_value;
	return object ? Variant(vformat("<%s:%s>", object->get_class(), itos(object->get_instance_id()))) : Variant("<null>");
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

Dictionary SolersReflectionService::_call_method_on_object(Object *p_object, const String &p_owner, const String &p_method, const MethodInfo &p_info, const Array &p_args) const {
	const int max_args = p_info.arguments.size();
	const int min_args = max_args - p_info.default_arguments.size();
	if (p_args.size() < min_args || p_args.size() > max_args) {
		return _error("INVALID_ARGUMENT_COUNT", vformat("%s.%s expects %d-%d args, got %d.", p_owner, p_method, min_args, max_args, p_args.size()));
	}

	Vector<Variant> argv;
	for (int i = 0; i < p_args.size(); i++) {
		Variant value = p_args[i];
		if (i < max_args) {
			const Variant::Type expected = p_info.arguments[i].type;
			if (expected != Variant::NIL && value.get_type() != expected && value.get_type() == Variant::ARRAY) {
				const Array components = value;
				Vector<Variant> ctor_args;
				for (int c = 0; c < components.size(); c++) {
					ctor_args.push_back(components[c]);
				}
				Vector<const Variant *> ctor_argp;
				for (int c = 0; c < ctor_args.size(); c++) {
					ctor_argp.push_back(&ctor_args[c]);
				}
				Variant constructed;
				Callable::CallError ctor_error;
				Variant::construct(expected, constructed, ctor_argp.ptrw(), ctor_argp.size(), ctor_error);
				if (ctor_error.error == Callable::CallError::CALL_OK) {
					value = constructed;
				}
			}
		}
		argv.push_back(value);
	}
	Vector<const Variant *> argp;
	for (int i = 0; i < argv.size(); i++) {
		argp.push_back(&argv[i]);
	}

	Callable::CallError call_error;
	const Variant ret = p_object->callp(StringName(p_method), argp.ptrw(), argp.size(), call_error);
	if (call_error.error != Callable::CallError::CALL_OK) {
		return _error("METHOD_CALL_FAILED", vformat("Calling %s.%s(%d args) failed (error %d). Check arg count/types via class.introspect.", p_owner, p_method, p_args.size(), (int)call_error.error));
	}

	Dictionary data;
	data["method"] = p_method;
	data["arg_count"] = p_args.size();
	data["return_type"] = Variant::get_type_name(p_info.return_val.type);
	data["result"] = _reflect_displayable(ret);
	return _ok(data);
}

Dictionary SolersReflectionService::introspect_class(const Dictionary &p_args) {
	const String class_name = p_args.get("class_name", String());
	const bool include_inherited = p_args.get("include_inherited", true);
	if (class_name.strip_edges().is_empty()) {
		return _error("INVALID_ARGUMENT", "class_name is required.");
	}
	const StringName class_sn = StringName(class_name);
	if (!ClassDB::class_exists(class_sn)) {
		return _error("UNKNOWN_CLASS", vformat("Engine class does not exist: %s", class_name));
	}
	const bool no_inheritance = !include_inherited;

	Array methods_out;
	List<MethodInfo> methods;
	ClassDB::get_method_list(class_sn, &methods, no_inheritance);
	bool methods_truncated = false;
	for (const MethodInfo &method : methods) {
		if (methods_out.size() >= SOLERS_INTROSPECT_METHOD_CAP) {
			methods_truncated = true;
			break;
		}
		Dictionary md;
		md["name"] = method.name;
		md["return_type"] = Variant::get_type_name(method.return_val.type);
		Array args_out;
		for (const PropertyInfo &arg : method.arguments) {
			Dictionary ad;
			ad["name"] = arg.name;
			ad["type"] = Variant::get_type_name(arg.type);
			args_out.push_back(ad);
		}
		md["arguments"] = args_out;
		methods_out.push_back(md);
	}

	Array properties_out;
	List<PropertyInfo> properties;
	ClassDB::get_property_list(class_sn, &properties, no_inheritance);
	bool properties_truncated = false;
	for (const PropertyInfo &property : properties) {
		if (!(property.usage & PROPERTY_USAGE_EDITOR) && !(property.usage & PROPERTY_USAGE_STORAGE)) {
			continue;
		}
		if (properties_out.size() >= SOLERS_INTROSPECT_PROPERTY_CAP) {
			properties_truncated = true;
			break;
		}
		Dictionary pd;
		pd["name"] = property.name;
		pd["type"] = Variant::get_type_name(property.type);
		pd["hint_string"] = property.hint_string;
		properties_out.push_back(pd);
	}

	Array signals_out;
	List<MethodInfo> signals;
	ClassDB::get_signal_list(class_sn, &signals, no_inheritance);
	bool signals_truncated = false;
	for (const MethodInfo &signal : signals) {
		if (signals_out.size() >= SOLERS_INTROSPECT_SIGNAL_CAP) {
			signals_truncated = true;
			break;
		}
		Dictionary sd;
		sd["name"] = signal.name;
		Array args_out;
		for (const PropertyInfo &arg : signal.arguments) {
			Dictionary ad;
			ad["name"] = arg.name;
			ad["type"] = Variant::get_type_name(arg.type);
			args_out.push_back(ad);
		}
		sd["arguments"] = args_out;
		signals_out.push_back(sd);
	}

	Dictionary constants_out;
	List<String> constants;
	ClassDB::get_integer_constant_list(class_sn, &constants, no_inheritance);
	bool constants_truncated = false;
	int constant_count = 0;
	for (const String &constant : constants) {
		if (constant_count >= SOLERS_INTROSPECT_CONSTANT_CAP) {
			constants_truncated = true;
			break;
		}
		constants_out[constant] = ClassDB::get_integer_constant(class_sn, StringName(constant));
		constant_count++;
	}

	Dictionary data;
	data["class_name"] = class_name;
	data["parent_class"] = String(ClassDB::get_parent_class(class_sn));
	data["can_instantiate"] = ClassDB::can_instantiate(class_sn);
	data["is_node"] = ClassDB::is_parent_class(class_sn, SNAME("Node"));
	data["methods"] = methods_out;
	data["properties"] = properties_out;
	data["signals"] = signals_out;
	data["constants"] = constants_out;
	data["truncated"] = methods_truncated || properties_truncated || signals_truncated || constants_truncated;
	return _ok(data);
}

Dictionary SolersReflectionService::get_property(const Dictionary &p_args) {
	const String node_path = p_args.get("node_path", ".");
	const String property = p_args.get("property", String());
	if (property.strip_edges().is_empty()) {
		return _error("INVALID_ARGUMENT", "property is required.");
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
		const Variant value = node->get_indexed(subnames, &valid);
		if (!valid) {
			return _error("INVALID_PROPERTY_PATH", vformat("Property path '%s' is not valid on %s. Use '/' for nested resource properties, e.g. environment/ambient_light_energy.", normalized, node->get_class()));
		}
		String safe_path;
		_safe_node_path(node, safe_path);
		Dictionary data;
		data["node_path"] = safe_path;
		data["property"] = normalized;
		data["type"] = Variant::get_type_name(value.get_type());
		data["value"] = _reflect_displayable(value);
		return _ok(data);
	}

	const StringName property_sn = StringName(property);
	bool valid = false;
	Variant::Type property_type = Variant::NIL;
	List<PropertyInfo> property_list;
	node->get_property_list(&property_list);
	for (const PropertyInfo &info : property_list) {
		if (info.name == property_sn) {
			valid = true;
			property_type = info.type;
			break;
		}
	}
	if (!valid) {
		return _error("UNKNOWN_PROPERTY", vformat("Property '%s' is not exposed by %s. Use class.introspect for valid names.", property, node->get_class()));
	}

	String safe_path;
	_safe_node_path(node, safe_path);
	Dictionary data;
	data["node_path"] = safe_path;
	data["property"] = property;
	data["type"] = Variant::get_type_name(property_type);
	data["value"] = _reflect_displayable(node->get(property_sn));
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
		undo_redo->create_action(vformat("Solers: Set %s.%s", node->get_class(), normalized), UndoRedo::MERGE_DISABLE, node);
		undo_redo->add_do_method(node, "set_indexed", property_path, p_args["value"]);
		undo_redo->add_undo_method(node, "set_indexed", property_path, old_value);
		undo_redo->commit_action();

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

	undo_redo->create_action(vformat("Solers: Set %s.%s", node->get_class(), property), UndoRedo::MERGE_DISABLE, node);
	undo_redo->add_do_property(node, property_sn, coerced);
	undo_redo->add_undo_property(node, property_sn, node->get(property_sn));
	undo_redo->commit_action();

	String safe_path;
	_safe_node_path(node, safe_path);
	Dictionary data;
	data["node_path"] = safe_path;
	data["property"] = property;
	data["value"] = _reflect_displayable(node->get(property_sn));
	return _ok(data);
}

Dictionary SolersReflectionService::call_method(const Dictionary &p_args) {
	const String node_path = p_args.get("node_path", ".");
	const String method = p_args.get("method", String());
	if (method.strip_edges().is_empty()) {
		return _error("INVALID_ARGUMENT", "method is required.");
	}

	String error;
	Node *node = _resolve_node(node_path, error);
	if (!node) {
		return _error("NODE_NOT_FOUND", error);
	}

	const StringName method_sn = StringName(method);
	if (!node->has_method(method_sn)) {
		return _error("UNKNOWN_METHOD", vformat("Method '%s' is not available on %s. Use class.introspect for valid methods.", method, node->get_class()));
	}

	MethodInfo method_info;
	if (!ClassDB::get_method_info(node->get_class_name(), method_sn, &method_info)) {
		method_info.name = method;
	}
	Dictionary result = _call_method_on_object(node, node->get_class(), method, method_info, p_args.get("args", Array()));
	if (!(bool)result.get("ok", false)) {
		return result;
	}
	Dictionary data = result.get("data", Dictionary());

	String safe_path;
	_safe_node_path(node, safe_path);
	data["node_path"] = safe_path;
	return _ok(data);
}

Dictionary SolersReflectionService::invoke_editor(const Dictionary &p_args) {
	EditorInterface *editor_interface = EditorInterface::get_singleton();
	ERR_FAIL_NULL_V(editor_interface, _error("EDITOR_INTERFACE_UNAVAILABLE", "EditorInterface is not available.", false));

	const String method = p_args.get("method", String());
	if (method.strip_edges().is_empty()) {
		return _error("INVALID_ARGUMENT", "method is required.");
	}

	const StringName method_sn = StringName(method);
	MethodInfo method_info;
	if (!ClassDB::get_method_info(SNAME("EditorInterface"), method_sn, &method_info)) {
		return _error("UNKNOWN_METHOD", vformat("EditorInterface.%s is not exposed by ClassDB. Use class.introspect for valid methods.", method));
	}
	if (!editor_interface->has_method(method_sn)) {
		return _error("UNKNOWN_METHOD", vformat("EditorInterface.%s is not callable on this editor instance.", method));
	}

	return _call_method_on_object(editor_interface, "EditorInterface", method, method_info, p_args.get("args", Array()));
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
		return _error("INVALID_NODE_TYPE", vformat("Class is not an instantiable Node type: %s", type));
	}

	String error;
	Node *parent = _resolve_node(parent_path, error);
	if (!parent) {
		return _error("PARENT_NODE_NOT_FOUND", error);
	}

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

	EditorInterface *editor_interface = EditorInterface::get_singleton();
	ERR_FAIL_NULL_V(editor_interface, _error("EDITOR_INTERFACE_UNAVAILABLE", "EditorInterface is not available.", false));
	Node *edited_root = editor_interface->get_edited_scene_root();
	EditorUndoRedoManager *undo_redo = editor_interface->get_editor_undo_redo();
	ERR_FAIL_NULL_V(undo_redo, _error("UNDO_REDO_UNAVAILABLE", "EditorUndoRedoManager is not available.", false));

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
	undo_redo->add_undo_method(node, "set_owner", node->get_owner());
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

Dictionary SolersReflectionService::batch(const Dictionary &p_args) {
	const Array operations = p_args.get("operations", Array());
	if (operations.is_empty()) {
		return _error("INVALID_ARGUMENT", "operations is required and must be a non-empty array.");
	}
	if (operations.size() > SOLERS_BATCH_OP_CAP) {
		return _error("INVALID_ARGUMENT", vformat("Too many operations (%d); the batch cap is %d.", operations.size(), SOLERS_BATCH_OP_CAP));
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
		const Dictionary op = operations[i];
		const String kind = op.get("op", String());
		Dictionary result;
		if (kind == "create_node") {
			result = _create_node(op);
		} else if (kind == "set_property") {
			result = set_property(op);
		} else if (kind == "get_property") {
			result = get_property(op);
		} else if (kind == "call_method") {
			result = call_method(op);
		} else if (kind == "reparent") {
			result = _reparent_node(op);
		} else if (kind == "connect_signal") {
			result = _connect_signal(op);
		} else if (kind == "attach_script") {
			result = _attach_script(op);
		} else if (kind == "remove_node") {
			result = _remove_node(op);
		} else if (kind == "list_properties") {
			result = _list_properties(op);
		} else if (kind == "list_signal_connections") {
			result = _list_signal_connections(op);
		} else {
			result = _error("UNKNOWN_OP", vformat("Unknown batch op '%s'. Supported: create_node, set_property, get_property, call_method, reparent, connect_signal, attach_script, remove_node, list_properties, list_signal_connections.", kind));
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

	Dictionary data;
	data["results"] = results;
	data["count"] = results.size();
	data["ok_count"] = ok_count;
	data["error_count"] = error_count;
	data["completed"] = error_count == 0;
	if (error_count > 0) {
		Dictionary result = _error("BATCH_FAILED", "objects.batch stopped at the first failed operation.");
		result["data"] = data;
		return result;
	}
	return _ok(data);
}

SolersReflectionService::SolersReflectionService() {}
